# K17 · 在持有的页面上分配小对象

从 K16 继续。页池一次交出 4096 字节，后续内核任务或文件对象往往只需要几十字节；为每个小对象独占一页会浪费空间。我们先借出十六个物理页，用自己的页表把它们接成连续的 64 KiB 虚拟范围，再在这个范围里划分和回收小对象。页面不要求物理连续，小对象也不直接拥有整页。

本节新建 heap.h、heap.c、memory.h 和主机观察，更新页池头中的内存初始化声明归属，再替换 memory.c、console.h/console.c、main.c、Makefile 和两个观察脚本。K16 的页表与异常实现保持。全部完成后构建，不需要图形源码。

<a id="heap-backing"></a>
## 页面和对象分别由谁持有

内核堆持有固定的 backing 范围：页池记录这十六页已经借出，页表把它们映射到 0xffffd00000000000 开始的连续虚拟地址，权限为 supervisor 读写、NX。堆分配器只在这片已可访问的范围中写块头和 payload，不再申请页，也不执行特权指令。以后归还一个对象，空间回到堆中，backing 页仍归堆持有，页池 free 不会因此增加。

每块的 32 字节块头记录 payload 容量、后继块指针和空闲标记。所有 payload 按 16 字节对齐，足以满足当前整数、指针和普通内核结构的对齐要求；更高对齐或 DMA 请求以后需要独立接口。第一次只有一个空闲块，因此初始 payload 是 65536−32=65504 字节。空闲 payload 可能分散在多个块里，其总和不表示能够一次分配同样大的连续对象。

**新建 `include/airi/heap.h`。** 不完全类型 `struct airi_heap_block` 允许接口保存指针，把布局留在实现里。

<!-- write: include/airi/heap.h -->
```c
#ifndef AIRI_HEAP_H // 字节级分配接口只展开一次。
#define AIRI_HEAP_H // 包含保护。
#include <airi/base.h> // size_t、NULL 与 SIZE_MAX。
#include <stdbool.h> // 分配和归还操作的明确成功状态。
struct airi_heap_block; // 块元数据布局只由 heap.c 维护。
struct airi_heap { // 调用者持有 backing 范围和分配状态。
    uint8_t *base; // 可读写且至少 16 字节对齐的虚拟起点。
    size_t bytes; // backing 的总字节数，包含内部元数据。
    struct airi_heap_block *first; // 第一个块，后续块按地址顺序链接。
};
bool airi_heap_init(struct airi_heap *heap, void *base, size_t bytes); // 不申请页，只在调用者已经拥有的连续范围建堆。
void *airi_heap_alloc(struct airi_heap *heap, size_t bytes); // 返回 16 字节对齐的未初始化小对象，失败或零长度返回 NULL。
bool airi_heap_free(struct airi_heap *heap, void *pointer); // NULL 作为空操作，其他指针必须是当前借出的对象起点。
size_t airi_heap_free_bytes(const struct airi_heap *heap); // 空闲 payload 字节和，不含块头，不保证可以一次分配这么大。
#endif // 结束包含保护。
```

<a id="heap-split"></a>
## 分割时先接住原来的后继

alloc 把请求向上补齐到 16，先检查 bytes+15 不会溢出，再按地址顺序选第一个空闲且足够大的块。只有余下空间能容纳新块头和至少 16 字节 payload，才分出尾块。tail 先保存旧后继，再让当前块指向 tail；若反过来覆盖 b->next，会失去后面的块链。余量过小时整块交给对象，避免创建没有可用内容的块。

C 的 `b+1` 不是加一个字节，而是跨过一个完整块头，得到 payload 起点；转换成 uint8_t 指针后再加 needed，才按字节定位尾块头。块头大小与 needed 都是 16 的倍数，后续对象因此继续对齐。返回前先把 free 改为 false，保证空间不再被下一次分配选中。

free 不从传入指针向前猜块头，而是沿已知块链比较 payload 起点，拒绝内部指针和重复释放。找到活对象后才标为空闲，再把相邻空闲块合并；原后继块头也回到 payload 容量中。调用者必须已经结束其他借用，归还后不能继续使用旧指针。接口不能识别一个地址被重新借出后的过期引用，也无法防止调用者越界破坏块头；当前需要内核调用者遵守所有权约定。

**新建 `kernel/mm/heap.c`。** 不引入宿主 malloc；输入空间必须已经由调用者持有并映射。

<!-- write: kernel/mm/heap.c -->
```c
#include <airi/heap.h> // 独立于页表和设备的字节分配器。
struct airi_heap_block { // 块头紧靠 payload 之前，整个块按地址顺序排列。
    size_t bytes; // payload 容量，包含为 16 字节对齐增加的空间。
    struct airi_heap_block *next; // 紧邻的下一个块，NULL 表示 backing 末尾。
    bool free; // true 表示当前没有对象借用这块 payload。
} __attribute__((aligned(16))); // 块头大小也必须是 16 的倍数，才能保持后续对象对齐。
_Static_assert(sizeof(struct airi_heap_block) == 32, "heap header layout"); // 本项目固定 64 位 C ABI。
bool airi_heap_init(struct airi_heap *heap, void *base, size_t bytes) { // 调用者保证整个 backing 持续可读写。
    if (!heap || !base || (uintptr_t)base % 16 || bytes < sizeof(struct airi_heap_block) + 16 || bytes > UINTPTR_MAX - (uintptr_t)base) // 拒绝未对齐、过小和右端回绕。
        return false; // 不构造不完整的初始块。
    bytes &= ~(size_t)15; // 尾部不足 16 字节的空间不参与对象划分。
    heap->base = base; // 保存当前 backing 虚拟起点。
    heap->bytes = bytes; // 保存包含块头的总容量。
    heap->first = base; // 第一个块头就在 backing 起点。
    *heap->first = (struct airi_heap_block){bytes - sizeof(*heap->first), NULL, true}; // 初始只有一个完整空闲块。
    return true; // 仍未向任何对象借出空间。
}
void *airi_heap_alloc(struct airi_heap *heap, size_t bytes) { // 首次适配按地址顺序寻找能放下对象的空闲块。
    if (!heap || !heap->first || !bytes || bytes > SIZE_MAX - 15) // 拒绝零长度和对齐加法溢出。
        return NULL; // 失败不改变块链。
    size_t needed = (bytes + 15) & ~(size_t)15; // 向上补齐到 16 字节。
    for (struct airi_heap_block *b = heap->first; b; b = b->next) { // 遍历仍属于这个 backing 的块。
        if (!b->free || b->bytes < needed) // 已借出或容量不足的块不能使用。
            continue; // 检查后面的块。
        if (b->bytes - needed >= sizeof(*b) + 16) { // 剩余空间必须能容纳新块头和最小 payload。
            struct airi_heap_block *tail = (void *)((uint8_t *)(b + 1) + needed); // 新块头位于本次对象容量之后。
            *tail = (struct airi_heap_block){b->bytes - needed - sizeof(*tail), b->next, true}; // 尾块接回原来的后继。
            b->bytes = needed; // 当前块缩到本次实际占用容量。
            b->next = tail; // 再把新尾块接入链，避免丢失原后继。
        }
        b->free = false; // 在交出 payload 指针前记录已借出。
        return b + 1; // C 的结构指针加一恰好跨过一个完整块头。
    }
    return NULL; // 碎片或容量耗尽，不越过 backing 尝试写入。
}
bool airi_heap_free(struct airi_heap *heap, void *pointer) { // 只比较已知块的 payload，不解引用传入的任意指针。
    if (!pointer) // 空借用没有对象需要归还。
        return true; // free(NULL) 作为明确空操作。
    if (!heap || !heap->first) // 没有初始化过的堆不接受对象。
        return false; // 不访问未知块链。
    struct airi_heap_block *found = NULL; // 尚未找到匹配的对象起点。
    for (struct airi_heap_block *b = heap->first; b; b = b->next) // 沿受堆管理的真实块查找。
        if ((void *)(b + 1) == pointer) // 只接受精确 payload 起点，不接受内部地址。
            found = b; // 找到对应的当前块元数据。
    if (!found || found->free) // 未找到或已经归还，不能再次改变容量。
        return false; // 过期块头被合并后也不会作为有效对象找到。
    found->free = true; // 对象从已借出变为空闲，调用者从此不能再访问。
    for (struct airi_heap_block *b = heap->first; b && b->next;) { // 按地址顺序合并相邻空闲块。
        if (b->free && b->next->free) { // 两个相邻 payload 都没有所有者。
            b->bytes += sizeof(*b) + b->next->bytes; // 原后继块头也回到可用 payload。
            b->next = b->next->next; // 跳过已被合并的后继，继续检查新的邻居。
        } else // 中间还有借出的对象时不能跨越它合并。
            b = b->next; // 向后检查下一对块。
    }
    return true; // 只归还对象空间，不触碰 backing 页所有权。
}
size_t airi_heap_free_bytes(const struct airi_heap *heap) { // 统计可供小对象使用的 payload。
    size_t bytes = 0; // 空堆或没有空闲块时为零。
    if (heap) // 允许消费者查询尚未初始化的空状态。
        for (const struct airi_heap_block *b = heap->first; b; b = b->next) // 不修改分配状态。
            if (b->free) // 已借出块不能计入空闲。
                bytes += b->bytes; // 和可能分散在多个块中，不代表最大连续块。
    return bytes; // 不包含任何仍存在的块头。
}
```

<a id="memory-service"></a>
## 查询者只取得数值

接下来给后续内核代码提供 kmalloc/kfree，并把分配状态复制成一个小快照。快照中的 pages_free 是物理页池状态，heap_free 是堆内部空闲 payload，它们不能混成一个“剩余内存”数字。文本控制台只接收这些数值，不拿位图、块头或页表内部指针；未来图形或宿主网关也可以消费同样的只读数据。

**新建 `include/airi/memory.h`。** 内存服务初始化从页池算法头移到这里，kmalloc 的 NULL 和 kfree 的布尔失败语义明确保留。

<!-- write: include/airi/memory.h -->
```c
#ifndef AIRI_MEMORY_H // 通用内存服务与只读数据快照。
#define AIRI_MEMORY_H // 包含保护。
#include <airi/boot.h> // 启动合同和固定宽整数。
#include <stddef.h> // 小对象请求按字节计数。
struct airi_memory_stats { // 消费者只得到数值，不持有分配器内部块指针。
    uint32_t pages_total, pages_free; // 实际受管理物理页与当前未借出页数。
    size_t heap_bytes, heap_free; // 固定堆 backing 字节与空闲 payload 字节。
};
void airi_memory_init(const AiriBootInfo *info); // 顺序初始化页池、页表和固定内核堆。
void airi_memory_snapshot(struct airi_memory_stats *stats); // 同步复制当前数值到调用者对象。
void *airi_kmalloc(size_t bytes); // 小对象来自已映射内核堆，不承诺完整 libc malloc ABI。
bool airi_kfree(void *pointer); // 结束对象借用，backing 页仍由内核堆持有。
#endif // 结束包含保护。
```

**完整替换 `include/airi/pages.h`。** 这里只保留物理页池自身接口，原 memory_init 声明移入 memory.h。

<!-- write: include/airi/pages.h -->
```c
#ifndef AIRI_PAGES_H // 物理页接口只声明一次。
#define AIRI_PAGES_H // 包含保护标记。
#include <airi/boot.h> // 分配器只消费稳定启动地图，不包含 Limine 协议。
#define AIRI_PAGE_SIZE 4096ull // 一个页帧的字节数。
#define AIRI_PHYSICAL_LIMIT (1024ull * 1024 * 1024) // 首版只管理低 1 GiB，固定目标内存为 256 MiB。
#define AIRI_PAGE_COUNT (AIRI_PHYSICAL_LIMIT / AIRI_PAGE_SIZE) // 位图可表示的页帧总数。
struct airi_page_pool { // 两张位图区分“允许管理”和“本分配器当前已借出”。
    uint8_t managed[AIRI_PAGE_COUNT / 8]; // 一位对应一个完整可用页帧。
    uint8_t allocated[AIRI_PAGE_COUNT / 8]; // 只对 managed 中的页面记借出状态。
    uint32_t total, free, hint; // 管理总页数、空闲页数和下一次扫描起点。
};
bool airi_pages_init(struct airi_page_pool *pool, const AiriBootInfo *info); // 初始化只接收有效地图，不回收加载器区域。
uint64_t airi_page_alloc(struct airi_page_pool *pool); // 返回物理页起点，耗尽返回 0；物理页 0 永不借出。
bool airi_page_free(struct airi_page_pool *pool, uint64_t physical); // 对齐、归属和借出状态均有效才释放。
#endif // 结束包含保护。
```

堆的十六个 backing 页逐页借出和映射，成功后才登记到数组。若中途失败，先逆序解除已成功映射的范围，使旧翻译失效，再归还对应数据页；不能把“分配成功但映射失败”的本次数据页漏掉。只有十六页全部建立后，才清零并创建初始块。

启动演示先分 80、128、32 字节三块，保留第三块尾字节。归还前两块后申请 208 字节，应该复用第一个地址，证明相邻空间已合并；第三块仍活着，它的尾字节不能被改变。零请求、SIZE_MAX、超过整个 backing 的请求均返回 NULL，内部地址与重复释放均被拒绝。最后全部对象归还，payload 恢复65504字节，backing页面继续持有。

**完整替换 `kernel/mm/memory.c`。** 先页池，再页表，再堆，避免让较底层初始化反过来依赖 kmalloc。

<!-- write: kernel/mm/memory.c -->
```c
#include <airi/memory.h> // 内存服务对消费者只交出快照和对象。
#include <airi/heap.h> // 固定backing上的字节分配器。
#include <airi/vm.h> // 使用自有四级页表建立内核映射。
#include <airi/trap.h> // 真实页权限故障由异常入口报告。
#include <airi/pages.h> // 物理页分配与启动信息。
#include <airi/kernel.h> // 日志和具体失败出口。
static struct airi_page_pool pages; // 页池元数据放在内核 BSS，不能自己被分配。
static void heap_init(void); // 自有页表建立后才映射堆backing。
static void virtual_memory_init(const AiriBootInfo *info); // 页池就绪后才建立并激活页表。
void airi_memory_init(const AiriBootInfo *info) { // 在异常设施安装后调用，仍使用加载器页表。
    if (!airi_pages_init(&pages, info)) // 没有可管理页就不能继续建立内存服务。
        airi_panic("physical page pool has no usable pages"); // 停止，而不是假装分配成功。
    airi_log("[PAGES_READY] page_bytes=4096 capacity_bytes=%llu managed=%u free=%u", // 明确固定上限和实际管理数量。
             (unsigned long long)AIRI_PHYSICAL_LIMIT, pages.total, pages.free); // 内存地图中的非 usable 页面不在统计中。
    uint32_t before = pages.free; // 保存演示前空闲数量，最后必须恢复。
    uint64_t first = airi_page_alloc(&pages); // 取得第一个物理页，0 是失败。
    uint64_t second = airi_page_alloc(&pages); // 第二个页面必须独立借出。
    if (!first || !second || first == second) // 耗尽和重复借出都不允许继续写入。
        airi_panic("physical page allocation failed"); // 停止潜在的内存破坏。
    volatile uint64_t *mapped = (volatile uint64_t *)(uintptr_t)(info->direct_map_offset + first); // HHDM 将物理页转换为当前可访问的虚拟别名。
    *mapped = 0x4149524950414745ull; // 真正写入刚借出的 RAM，volatile 保留设备级观察所需的访问。
    if (*mapped != 0x4149524950414745ull || pages.free != before - 2) // 验证内容和所有权统计是同一次分配结果。
        airi_panic("physical page write/accounting failed"); // 不能只凭非零地址宣称可用。
    if (!airi_page_free(&pages, first) || airi_page_free(&pages, first) || airi_page_free(&pages, second + 1)) // 验证正常归还、重复释放拒绝和未对齐释放拒绝。
        airi_panic("physical page release validation failed"); // 失败释放不应改变 second 的所有权。
    uint64_t again = airi_page_alloc(&pages); // 应复用刚归还的低地址页。
    if (again != first || !airi_page_free(&pages, again) || !airi_page_free(&pages, second) || pages.free != before) // 所有演示借用在结束前归还。
        airi_panic("physical page reuse/accounting failed"); // 任何泄漏都阻止后续阶段。
    airi_log("[PAGES_DEMO_OK] first=0x%llx second=0x%llx reused=0x%llx free=%u", // 日志给出本轮真实分配地址。
             (unsigned long long)first, (unsigned long long)second, (unsigned long long)again, pages.free); // 记录数值，不在归还后解引用旧指针。
    virtual_memory_init(info); // 页表页也从同一所有权池取得。
    heap_init(); // 小对象分配不能反过来成为页表构建的前置。
}

static struct airi_address_space kernel_space; // 当前内核持有根页，所有映射在切换后持续存在。
extern char __kernel_start[], __text_start[], __text_end[], __data_start[], __kernel_end[]; // 链接器给出页对齐权限边界。
static unsigned kernel_flags(uint64_t address) { // 同一高地址页只取必要权限。
    if (address >= (uintptr_t)__text_start && address < (uintptr_t)__text_end) // text 页只读且允许执行。
        return AIRI_VM_EXEC; // 不同时打开写权限。
    return address >= (uintptr_t)__data_start ? AIRI_VM_WRITE : 0; // data/BSS 可写不可执行，requests/rodata 只读不可执行。
}
static void require_map(uint64_t virtual_address, uint64_t physical, unsigned flags) { // 启动映射失败不能带着半成品切 CR3。
    uint64_t existing; // 处理相邻非页对齐 RAM 区域可能共享的边缘页。
    unsigned old_flags; // 重复页面必须具有完全相同的权限。
    if (airi_vm_lookup(&kernel_space, virtual_address, &existing, &old_flags)) { // 已映射时只能复用同一事实。
        if (existing != physical || old_flags != flags) // 冲突不能被后来的映射静默覆盖。
            airi_panic("conflicting bootstrap virtual mapping"); // 报告构图错误并保留旧根。
    } else if (!airi_vm_map(&kernel_space, virtual_address, physical, flags)) // 缺页表资源或无效范围。
        airi_panic("cannot construct kernel page tables"); // 没有完整映射就绝不装 CR3。
}
static void virtual_memory_init(const AiriBootInfo *info) { // 借用加载器直接映射来填写新的页表页。
    if (!airi_vm_create(&kernel_space, &pages, info->direct_map_offset)) // 根页也可能分配失败。
        airi_panic("cannot allocate page table root"); // 没有根时停止。
    uint64_t kernel_end = info->kernel_physical_base + info->kernel_size; // 用于限制物理别名的写权限。
    for (uint32_t i = 0; i < info->region_count; ++i) { // 重建仍需要的 RAM 直接映射，不复制加载器页表。
        const struct airi_memory_region *r = &info->regions[i]; // 范围已经通过启动校验。
        if (r->kind == AIRI_MEMORY_RESERVED || r->kind == AIRI_MEMORY_BAD || r->kind == AIRI_MEMORY_FRAMEBUFFER || r->base >= AIRI_PHYSICAL_LIMIT) // 不把任意保留/MMIO范围当普通缓存 RAM。
            continue; // 帧缓冲另外按设备属性建立。
        uint64_t end = r->base + r->length; // 启动校验已经排除溢出。
        if (end > AIRI_PHYSICAL_LIMIT) // 本阶段直接映射和页池使用同一上限。
            end = AIRI_PHYSICAL_LIMIT; // 高于上限的 RAM 暂不使用。
        end = (end + AIRI_PAGE_SIZE - 1) & ~(AIRI_PAGE_SIZE - 1); // 映射覆盖区域边缘，分配则仍只取完整 usable 页。
        for (uint64_t p = r->base & ~(AIRI_PAGE_SIZE - 1); p < end; p += AIRI_PAGE_SIZE) { // 每页各自决定别名权限。
            unsigned flags = AIRI_VM_WRITE; // 普通 RAM 别名可写不可执行。
            if (p >= info->kernel_physical_base && p < kernel_end) // 内核的物理别名也不能绕过 text/rodata 保护。
                flags = kernel_flags(info->kernel_virtual_base + p - info->kernel_physical_base) & AIRI_VM_WRITE; // HHDM 中永不执行，代码和常量别名也只读。
            require_map(info->direct_map_offset + p, p, flags); // 使用同一 HHDM 虚拟布局，现有指针继续有效。
        }
    }
    for (uint64_t v = (uintptr_t)__kernel_start; v < (uintptr_t)__kernel_end; v += AIRI_PAGE_SIZE) // 覆盖代码、静态表、BSS和当前内核栈。
        require_map(v, info->kernel_physical_base + v - info->kernel_virtual_base, kernel_flags(v)); // 按链接器边界赋予权限。
    if (airi_framebuffer_valid(&info->framebuffer)) { // 可选显示存在时保留其真实设备映射。
        const struct airi_framebuffer *fb = &info->framebuffer; // 指针仍借用静态 BootInfo。
        uint64_t virtual_address = fb->virtual_address; // 保持原显示接口中已有地址不变。
        uint64_t physical = fb->physical_address; // 物理设备地址不能从普通 RAM 页池取得。
        uint64_t bytes = fb->pitch * fb->height; // boot framebuffer 校验已检查乘法上界。
        if ((virtual_address & 4095) != (physical & 4095) || bytes > UINT64_MAX - physical || physical + bytes > UINT64_MAX - 4095) // 拒绝不能用整页保持偏移或右端回绕的设备范围。
            airi_panic("framebuffer mapping range is invalid"); // 不安装错误 MMIO 映射。
        uint64_t end = (physical + bytes + 4095) & ~4095ull; // 包含最后一个实际像素字节的页。
        uint64_t v = virtual_address & ~4095ull; // VA 和 PA 保留相同页内偏移。
        for (uint64_t p = physical & ~4095ull; p < end; p += 4096, v += 4096) // 映射整个有效显示范围。
            require_map(v, p, AIRI_VM_WRITE | AIRI_VM_DEVICE); // 帧缓冲可写不可执行，并采用设备缓存属性。
    }
    if (!airi_vm_activate(&kernel_space)) // 当前指令和栈必须在新根中都可访问。
        airi_panic("x86 paging requires four levels, NX and no PCID"); // 不在未支持的启动配置上假装成功。
    airi_log("[VM_READY] cr3=0x%llx levels=4 page_bytes=4096 wp=1 nx=1 free=%u", // 只有真正切根后才输出。
             (unsigned long long)kernel_space.root, pages.free); // 页表持有的页面计入已借出数量。
    uint32_t before = pages.free; // 演示只临时创建一个独立分支，最后应全部回收。
    uint64_t data = airi_page_alloc(&pages); // 调用者拥有这个数据页，页表不会替它归还。
    if (!data) // 演示也必须处理耗尽。
        airi_panic("cannot allocate paging demonstration page"); // 不写失败哨兵。
    uint64_t *alias = (uint64_t *)(uintptr_t)(info->direct_map_offset + data); // 新根已保留全部受管理 RAM 的 HHDM。
    *alias = 0xc3; // 第一字节是 ret，若 NX 缺失，调用也能返回并被演示检查拒绝。
    if (!airi_vm_map(&kernel_space, AIRI_VM_DEMO_ADDRESS, data, 0)) // 建立 supervisor 只读、不可执行的别名。
        airi_panic("cannot map paging demonstration page"); // 失败不能访问目标 VA。
    volatile uint64_t *probe = (volatile uint64_t *)(uintptr_t)AIRI_VM_DEMO_ADDRESS; // 这次 C 访问依赖我们自己的四级表。
    if (*probe != 0xc3) // 与 HHDM 物理别名应读到同一内容。
        airi_panic("virtual alias does not reach owned page"); // 报告翻译错误。
    airi_trap_memory_probe(AIRI_VM_DEMO_ADDRESS, AIRI_FAULT_WRITE); // 真实 supervisor 写只读页，应得到 error=3。
    airi_trap_memory_probe(AIRI_VM_DEMO_ADDRESS, AIRI_FAULT_EXEC); // 真实取指访问 NX 页，应得到 error=17。
    if (!airi_vm_unmap(&kernel_space, AIRI_VM_DEMO_ADDRESS) || airi_vm_lookup(&kernel_space, AIRI_VM_DEMO_ADDRESS, NULL, NULL)) // 解除后不能再查询到旧叶子。
        airi_panic("virtual unmap failed"); // 保留数据页，不冒险提前归还。
    airi_trap_memory_probe(AIRI_VM_DEMO_ADDRESS, AIRI_FAULT_READ); // 未映射读取应得到 error=0 和精确 CR2。
    if (!airi_page_free(&pages, data) || pages.free != before) // 空表已经回收，再归还数据页，应恢复演示前数量。
        airi_panic("paging demonstration leaked owned pages"); // 不掩盖页表回收泄漏。
    airi_log("[VM_DEMO_OK] alias=0x%llx faults=write-ro,nx,unmapped tables_reclaimed=1 free=%u", // 三次硬件故障恢复后输出。
             (unsigned long long)AIRI_VM_DEMO_ADDRESS, pages.free); // 不再访问已解除的 probe 指针。
}

#define HEAP_ADDRESS 0xffffd00000000000ull // 固定堆虚拟范围不与内核、HHDM或演示页重叠。
#define HEAP_PAGES 16u // 首版堆固定 64 KiB，不自动扩容。
static struct airi_heap heap; // backing持有者和对象块链在整个内核期存在。
static void heap_init(void) { // 所需页与页表都由当前内核拥有。
    uint64_t backing[HEAP_PAGES]; // 保存每个已借出数据页，失败时精确回滚。
    unsigned count = 0; // 只统计已经成功映射的backing页。
    for (; count < HEAP_PAGES; ++count) { // 虚拟连续不要求物理连续。
        uint64_t physical = airi_page_alloc(&pages); // 每页独立取得所有权。
        if (!physical) // 页池耗尽时还没有本次页面可归还。
            break; // 转到统一回滚路径。
        if (!airi_vm_map(&kernel_space, HEAP_ADDRESS + count * AIRI_PAGE_SIZE, physical, AIRI_VM_WRITE)) { // 只开放supervisor读写，不执行。
            airi_page_free(&pages, physical); // 未接入的本次数据页立即归还。
            break; // 之前成功映射的页面稍后拆除。
        }
        backing[count] = physical; // 在映射成功后登记当前拥有的数据页。
    }
    if (count != HEAP_PAGES) { // 不允许半个堆被提供给调用者。
        while (count) { // 逆序拆除已成功映射的所有backing。
            --count; // 退到最后一个真实映射。
            airi_vm_unmap(&kernel_space, HEAP_ADDRESS + count * AIRI_PAGE_SIZE); // 先断开映射并使TLB失效。
            airi_page_free(&pages, backing[count]); // 然后才能归还数据页。
        }
        airi_panic("cannot map fixed kernel heap"); // 失败保持明确，不回退到未拥有的地址。
    }
    void *base = (void *)(uintptr_t)HEAP_ADDRESS; // 完整映射之后才形成可供堆访问的指针。
    airi_memset(base, 0, HEAP_PAGES * AIRI_PAGE_SIZE); // 清除页面旧内容，块链不会泄漏旧所有者数据。
    if (!airi_heap_init(&heap, base, HEAP_PAGES * AIRI_PAGE_SIZE)) // 把已持有范围交给小对象分配器。
        airi_panic("cannot initialize kernel heap"); // 参数不符合对齐或容量约定就停止。
    size_t before = airi_heap_free_bytes(&heap); // 结束后对象级空闲应恢复。
    uint8_t *a = airi_kmalloc(80); // 第一个对象拥有独立payload。
    uint8_t *b = airi_kmalloc(128); // 中间对象用于演示相邻归还合并。
    uint8_t *c = airi_kmalloc(32); // 后面仍有活对象时不能越过它合并。
    if (!a || !b || !c || (uintptr_t)a % 16) // 不能把NULL作为可写对象。
        airi_panic("kernel heap allocation failed"); // 明确拒绝后再写payload。
    a[79] = 0xa5; // 写在第一个对象的最后一个请求字节。
    c[31] = 0x5a; // 写在第三个对象的最后一个请求字节。
    if (airi_kfree(b + 1) || !airi_kfree(b) || !airi_kfree(a) || airi_kfree(a)) // 内部指针和重复释放必须被拒绝。
        airi_panic("kernel heap release validation failed"); // 不允许错误释放破坏空闲块。
    uint8_t *joined = airi_kmalloc(208); // 原80和128两块合并后才能在a的位置容纳208字节。
    if (joined != a || c[31] != 0x5a || airi_kmalloc(SIZE_MAX) || airi_kmalloc(0) || airi_kmalloc(HEAP_PAGES * AIRI_PAGE_SIZE)) // 检查合并、不覆盖活对象、溢出、零长度和耗尽。
        airi_panic("kernel heap split/coalesce failed"); // 不把分配失败伪装成有效指针。
    if (!airi_kfree(joined) || !airi_kfree(c) || airi_heap_free_bytes(&heap) != before) // 所有对象归还后应恢复单个空闲块容量。
        airi_panic("kernel heap accounting did not recover"); // backing仍持有，只有对象借用结束。
    airi_log("[HEAP_DEMO_OK] mapped_pages=16 bytes=%zu free_payload=%zu aligned=16 coalesced=1", heap.bytes, before); // 实际分配、归还和失败演示结束。
}
void *airi_kmalloc(size_t bytes) { // 内核小对象入口，不引入宿主libc。
    return airi_heap_alloc(&heap, bytes); // 保留有界堆的NULL失败语义。
}
bool airi_kfree(void *pointer) { // 对象所有者结束最后一个借用后调用。
    return airi_heap_free(&heap, pointer); // 对象空间回堆，物理页不因此归还。
}
void airi_memory_snapshot(struct airi_memory_stats *stats) { // 目前单核不可抢占，可同步复制一致数值。
    stats->pages_total = pages.total; // 固定受管理物理页容量。
    stats->pages_free = pages.free; // 减去页表和堆backing等当前所有者。
    stats->heap_bytes = heap.bytes; // 已映射的总backing字节。
    stats->heap_free = airi_heap_free_bytes(&heap); // 空闲对象payload，不包含块头。
}
```

<a id="memory-consumer"></a>
## 同一个 mem 命令接入真实状态

console 新增一个可选的同步函数指针 read_memory，形参指向调用者栈上的快照。main 在内存初始化和 console_init 之后绑定它，防止初始化清零掉刚写入的回调。mem 在回调返回后只格式化复制出的数字，不保留局部快照指针。宿主纯命令模型和早期检查点未绑定时，继续明确报告尚无分配统计。

**完整替换 `include/airi/console.h`。** 引入的是快照的数据声明，文本模型不直接调用或链接分配器实现。

<!-- write: include/airi/console.h -->
```c
#ifndef AIRI_CONSOLE_H  // 第一次包含控制台接口时保留以下声明，重复包含时跳过
#define AIRI_CONSOLE_H  // 标记本编译单元已经读取控制台接口
#include <airi/memory.h> // 消费稳定内存数值快照，不链接分配器实现。
#include <airi/boot.h>  // 引入启动结构、固定宽整数和内存范围接口
#define AIRI_CONSOLE_LINES 256  // 输出环最多保存256条文字记录
#define AIRI_CONSOLE_WIDTH 128  // 一条输出的数组容量为128字节，含终止零
#define AIRI_COMMAND_SIZE 64    // 命令与历史项各用64字节，最多63个内容字节
#define AIRI_HISTORY_SIZE 8     // 最多记住8条已提交的命令
enum airi_view { AIRI_VIEW_TERMINAL, AIRI_VIEW_MEMORY, AIRI_VIEW_DEVICES, AIRI_VIEW_JOURNAL };  // 按顺序编号当前airi_view可接受的状态或事件
enum airi_key {  // 按顺序编号当前airi_key可接受的状态或事件
    AIRI_KEY_UP = 256,  // 向上浏览命令历史或移动选择
    AIRI_KEY_DOWN,      // 向下浏览命令历史或移动选择
    AIRI_KEY_PAGE_UP,   // 向上滚动显示或翻内存页
    AIRI_KEY_PAGE_DOWN,  // 向下滚动显示或翻内存页
    AIRI_KEY_F1,    // 切到Terminal的逻辑事件
    AIRI_KEY_F2,    // 切到Memory的逻辑事件
    AIRI_KEY_F3,    // 切到Devices的逻辑事件
    AIRI_KEY_F4,    // 切到Journal的逻辑事件
    AIRI_KEY_CLEAR  // 清控制台可见历史的逻辑事件
};
/* Fixed storage owned by the kernel. No heap, process or filesystem dependency. */
struct airi_console {  // 独立保存文本输入、历史与输出环。
    bool (*extension)(struct airi_console *, const char *, void *);  // 可选命令消费者；true 表示已经处理，NULL 表示没有扩展。
    void (*read_memory)(struct airi_memory_stats *); // 可选同步查询；宿主纯文本观察可保持NULL。
    void *extension_context;   // 借用扩展拥有者的数据，必须活过回调调用。
    const AiriBootInfo *boot;  // 借用启动信息，描述与内存地图须持续有效
    enum airi_view view;  // 当前命令视图编号
    char lines[AIRI_CONSOLE_LINES][AIRI_CONSOLE_WIDTH];  // 256行文字环，每行容量128字节
    uint64_t total_lines;  // 累计逻辑输出行号，覆盖旧槽也继续增加
    unsigned line_count;   // 当前环中仍保存的有效行数
    unsigned scroll;    // 显示滚动量，单位由当前视图决定
    unsigned map_page;  // 内存地图每12条一页的页编号
    unsigned commands;  // 已提交非空命令次数
    char input[AIRI_COMMAND_SIZE];  // 尚未提交的NUL命令字符串
    char history[AIRI_HISTORY_SIZE][AIRI_COMMAND_SIZE];  // 最多8条已提交命令的独立文字副本
    unsigned input_length;   // 当前输入内容字节数，不含NUL
    unsigned history_count;  // 有效命令历史条数
    unsigned history_back;   // 离当前草稿有多远，0表示草稿而非历史
    char draft[AIRI_COMMAND_SIZE];  // 首次浏览历史前保存的未提交输入
};
void airi_console_print(struct airi_console *c, const char *fmt, ...);  // 把扩展结果复制到有界文本环，不借用格式化实参。
void airi_console_init(struct airi_console *c, const AiriBootInfo *boot);  // 清命令状态并绑定只读启动结构，形成初始文字
bool airi_console_key(struct airi_console *c, int key);  // 消费一个逻辑键并报告显示状态是否变化
const char *airi_console_line(const struct airi_console *c, uint64_t sequence);  // 按逻辑行号借用仍保留的日志文字
unsigned airi_journal_count(void);  // 返回当前固定启动记录数量
void airi_journal_line(const struct airi_console *c, unsigned index, char *out, size_t size);  // 把指定启动事件格式化到调用者输出缓冲
void airi_device_line(const struct airi_console *c, unsigned index, char *out, size_t size);  // 把指定设备描述格式化到调用者输出缓冲
void airi_map_line(const struct airi_console *c, unsigned index, char *out, size_t size);  // 把指定物理内存范围格式化成一行文字
bool airi_console_draw(struct airi_console *c);  // 把当前会话和启动描述同步绘到真实帧缓冲，返回此帧是否完成
int airi_input_poll(void);  // 每轮从键盘或UART取一个事件，无事件返回0
int airi_keyboard_decode(uint8_t code);  // 保留修饰键状态并把一个扫描字节变成逻辑键
#endif  // 结束当前条件编译范围
```

**完整替换 `kernel/console/console.c`。** 原启动地图仍可查询，新页池与堆统计另行输出。

<!-- write: kernel/console/console.c -->
```c
#include <airi/console.h>  // 引入控制台状态与逻辑键编号
#include <airi/base.h>     // 引入自有字节操作和有界格式化函数

void airi_console_print(struct airi_console *c, const char *fmt, ...) {  // 向console输出环新增一条有界格式化记录
    char *line = c->lines[c->total_lines % AIRI_CONSOLE_LINES];  // 逻辑行号取余得到可复用槽位，line借用该行的128字节数组
    va_list args;  // 准备遍历fmt之后实际传入的格式参数
    va_start(args, fmt);  // 从最后一个具名参数fmt之后开始读取格式实参
    airi_vsnprintf(line, AIRI_CONSOLE_WIDTH, fmt, args);  // 把本条记录格式化到输出环，容量包含终止零
    va_end(args);      // 结束本轮可变参数读取
    ++c->total_lines;  // 累计逻辑行号前进一行，不因环绕覆盖归零
    if (c->line_count < AIRI_CONSOLE_LINES)  // 环尚未装满时才增加仍保留的行数
        ++c->line_count;  // 记录环中又多了一条仍可读取的输出
}
const char *airi_console_line(const struct airi_console *c, uint64_t sequence) {  // 按逻辑行号借用仍保留的日志文字
    if (sequence >= c->total_lines || c->total_lines - sequence > c->line_count)  // 拒绝未来行号和已经被覆盖的历史行号
        return "";  // 无可读取记录时返回有效的空字符串
    return c->lines[sequence % AIRI_CONSOLE_LINES];  // 按逻辑行号定位槽位，返回借用指针，后续覆盖可能改变内容
}
static bool equal(const char *a, const char *b) {  // 按长度和内容判断两个NUL字符串相同
    size_t n = airi_strlen(a);  // 取得第一个待比较字符串的内容字节数，稍后要求第二个长度也相同
    return n == airi_strlen(b) && !airi_memcmp(a, b, n);  // 先确认长度相同，再比较全部内容字节；memcmp返回0才相等
}
unsigned airi_journal_count(void) {  // 返回当前固定启动记录数量
    return 8;  // 启动记录固定8项，调用者据此限制查询下标
}
void airi_journal_line(const struct airi_console *c, unsigned i, char *s, size_t n) {  // 把指定启动事件格式化到调用者输出缓冲
    const AiriBootInfo *b = c->boot;  // 借用控制台绑定的启动结构，后续记录从这里取真实字段
    switch (i) {  // 按调用者请求的记录下标选择当前文字内容
    case 0:  // 选择加载器身份记录
        airi_snprintf(s, n, "0000 boot     loader=%s", b->loader);  // 把启动信息中的加载器名称写成第0条记录，调用者随后保存或绘制它
        break;  // 本项文字已形成，结束switch而不继续生成下一项
    case 1:  // 选择交接合同版本与内存地图条数记录
        airi_snprintf(s, n, "0001 handoff  ABI v%u validated; %u memory regions", b->version,  // 记录本次启动合同版本与有效内存区域条数，两项都来自已校验的启动结构
                      b->region_count);  // 把内存地图有效区域条数作为本条记录的%u实参
        break;  // 本项文字已形成，结束switch而不继续生成下一项
    case 2:  // 选择内核物理装载范围记录
        airi_snprintf(s, n, "0002 image    physical=0x%llx bytes=%llu",  // 记录内核物理装载起点与镜像长度，地址按十六进制、长度按字节显示
                      (unsigned long long)b->kernel_physical_base,  // 内核物理起点用64位无符号格式打印，指向实际装载范围
                      (unsigned long long)b->kernel_size);  // 内核镜像字节长度按%llu要求的类型传入
        break;  // 本项文字已形成，结束switch而不继续生成下一项
    case 3:  // 选择直接映射偏移记录
        airi_snprintf(s, n, "0003 mapping  direct-map=0x%llx",  // 记录物理地址转直接映射虚拟地址时需加的固定偏移
                      (unsigned long long)b->direct_map_offset);  // 将直接映射固定偏移交给64位十六进制格式
        break;  // 本项文字已形成，结束switch而不继续生成下一项
    case 4:  // 选择可用物理内存量记录
        airi_snprintf(s, n, "0004 memory   usable=%llu KiB; reclaim regions held",  // 记录当前可用物理范围总量，转为KiB，并明确引导可回收范围仍未释放
                      (unsigned long long)(airi_memory_bytes(b, AIRI_MEMORY_USABLE) / 1024));  // 字节除1024转KiB，再按%llu约定提供unsigned long long
        break;  // 本项文字已形成，结束switch而不继续生成下一项
    case 5:  // 选择COM1串口访问方式记录
        airi_snprintf(s, n, "0005 serial   COM1 115200 8N1; polled I/O");  // 记录已配置的COM1速率、8位数据无校验1停止位及轮询访问方式
        break;  // 本项文字已形成，结束switch而不继续生成下一项
    case 6:  // 选择实际图形可用性记录
        airi_snprintf(s, n, "0006 display  %s",  // 按实际帧缓冲校验结果选择图形可用或仅串口仍活跃的说明
                      airi_framebuffer_valid(&b->framebuffer)  // 先用尺寸、位域与地址约束判断当前帧缓冲描述能否绘图
                          ? "mapped framebuffer available"  // 校验通过时报告有可访问的帧缓冲
                          : "framebuffer absent; serial console active");  // 校验失败时说明图形缺失，现有串口通路仍可输出
        break;  // 本项文字已形成，结束switch而不继续生成下一项
    default:  // 固定8项记录中的最后一项为本地命令输入状态
        airi_snprintf(s, n, "0007 console  local command interpreter; logical-key input");  // 最后一项说明本地命令解释器接收设备轮询转换后的逻辑键
        break;  // 本项文字已形成，结束switch而不继续生成下一项
    }
}
void airi_device_line(const struct airi_console *c, unsigned i, char *s, size_t n) {  // 把指定设备描述格式化到调用者输出缓冲
    const struct airi_framebuffer *f = &c->boot->framebuffer;  // 取启动信息内嵌帧缓冲描述的地址，未复制像素内存
    switch (i) {  // 按调用者请求的记录下标选择当前文字内容
    case 0:  // 选择已配置COM1串口设备
        airi_snprintf(s, n, "ttyS0   serial    COM1 0x03f8 / 115200 / 8N1 / polling");  // 形成串口设备记录，包含实际端口基址、速率与轮询方式
        break;  // 此设备说明已形成，结束switch分派
    case 1:  // 选择固定i8042键盘设备
        airi_snprintf(s, n, "kbd0    input     i8042 / ports 0x60,0x64 / US layout");  // 形成固定键盘控制器记录，说明数据与状态端口以及当前US键位范围
        break;  // 此设备说明已形成，结束switch分派
    default:  // 第三个固定设备项读取实际帧缓冲描述
        if (airi_framebuffer_valid(f))  // 描述满足绘图前提时才显示实际分辨率和布局
            airi_snprintf(s, n, "fb0     display   %llux%llu / %u bpp / pitch %llu",  // 形成实际帧缓冲设备记录：宽高为像素，bpp为每像素位数，pitch为每行字节数
                          (unsigned long long)f->width, (unsigned long long)f->height,  // 把软件画布像素宽度、软件画布像素高度转成格式要求的unsigned long long
                          (unsigned)f->bpp, (unsigned long long)f->pitch);  // 按本次格式或坐标计算的整数类型传入当前字段值
        else  // 前一分支未成立时执行下方替代操作
            airi_snprintf(s, n, "fb0     display   unavailable");  // 图形描述不满足绘图前提时明确写不可用，避免读取无效像素地址
    }
}
void airi_map_line(const struct airi_console *c, unsigned i, char *s, size_t n) {  // 把指定物理内存范围格式化成一行文字
    if (i >= c->boot->region_count) {  // 查询下标超过有效内存区域时生成空输出
        if (n)  // 输出容量非零时才允许写入首字节
            s[0] = 0;  // 把调用者输出标成空的NUL字符串
        return;  // 行号超出地图有效项，保留空输出并返回，不能读取数组越界项
    }
    const struct airi_memory_region *r = &c->boot->regions[i];  // 借用第i条实际内存范围，读取起点、长度和类别
    airi_snprintf(s, n, "%03u %016llx %016llx %s", i, (unsigned long long)r->base,  // 记录区域下标与物理半开区间左右端点，地址固定十六进制宽度方便逐列比较
                  (unsigned long long)(r->base + r->length), airi_memory_name(r->kind));  // 把当前64位字段转成格式要求的unsigned long long
}
static void system_info(struct airi_console *c) {  // 将启动身份和内核尺寸加入命令输出
    airi_console_print(c, "AiriOS 0.1 / x86_64 / bootstrap kernel");  // 将AiriOS 0.1 / x86_64 / bootstrap kernel保存为一条命令说明或身份记录
    airi_console_print(c, "Console: local    CPU: single    Boot: UEFI / %s", c->boot->loader);  // 把本次查询字段按Console: local    CPU: single    Boot: UEFI / %s的格式保存到输出环
    airi_console_print(c, "Kernel image: %llu KiB    Boot ABI: v%u",  // 把本次查询字段按Kernel image: %llu KiB    Boot ABI: v%u的格式保存到输出环
         (unsigned long long)(c->boot->kernel_size / 1024), c->boot->version);  // 字节除1024转KiB，再按%llu约定提供unsigned long long
}
void airi_console_init(struct airi_console *c, const AiriBootInfo *b) {  // 清命令状态并绑定只读启动结构，形成初始文字
    airi_memset(c, 0, sizeof(*c));  // 清零整个当前状态或快照，使计数和标记从确定初值开始
    c->boot = b;     // 将借用启动信息，描述与内存地图须持续有效置为b，后续读取使用新状态
    system_info(c);  // 从同一启动结构生成系统身份与内核镜像尺寸
    airi_console_print(c, "");  // 在输出环中保存一条空行，为后续显示保留间隔
    airi_console_print(c, "System console attached.");  // 将System console attached.保存为一条命令说明或身份记录
    airi_console_print(c, "help       Command reference");  // 将help       Command reference保存为一条命令说明或身份记录
    airi_console_print(c, "mem        Physical memory inventory");  // 将mem        Physical memory inventory保存为一条命令说明或身份记录
    airi_console_print(c, "devices    Console, input and display endpoints");  // 将devices    Console, input and display endpoints保存为一条命令说明或身份记录
    airi_console_print(c, "log        Boot journal");  // 将log        Boot journal保存为一条命令说明或身份记录
    airi_console_print(c, "");  // 在输出环中保存一条空行，为后续显示保留间隔
}
static void execute(struct airi_console *c) {  // 读取完整提交命令，更新历史并产生结果
    char command[AIRI_COMMAND_SIZE];  // 本次提交使用独立命令副本，避免清输入时丢失分派文字
    airi_memcpy(command, c->input, sizeof(command));  // 连同终止零复制完整输入数组到本次提交副本
    char *start = command;  // 先让命令起点指向副本首字节
    while (*start == ' ')   // 起点仍为空格时继续向命令正文前进
        ++start;  // 跳过一个前导空格，原数组保留但起点向后移动
    size_t length = airi_strlen(start);  // 测量去掉前导空格后的内容长度，不计终止零
    while (length && start[length - 1] == ' ')  // 有内容且末字节为空格时缩短有效命令
        start[--length] = 0;  // 先缩短长度，再在被移除的尾部空格处写NUL
    if (!length)  // 首尾去空格后为空命令时不提交
        return;  // 输入只有空白，返回而不增加命令计数或记录历史
    airi_console_print(c, "airi> %s", start);  // 将本次真正提交的命令保存为输出环的一行
    ++c->commands;  // 非空命令的累计提交次数增加一次
    if (c->history_count == AIRI_HISTORY_SIZE) {  // 8条历史已满时腾出最后一个槽
        airi_memmove(c->history, c->history + 1, (AIRI_HISTORY_SIZE - 1) * AIRI_COMMAND_SIZE);  // 后7条完整历史前移一槽，重叠区域用memmove保住待读字节
        --c->history_count;  // 覆盖最旧历史后暂记7条，为本条提交留位置
    }
    airi_snprintf(c->history[c->history_count++], AIRI_COMMAND_SIZE, "%s", start);  // 先在旧计数下标保存命令，再把历史计数加一
    c->view = AIRI_VIEW_TERMINAL;  // 切回Terminal显示当前输入或命令结果
    c->scroll = 0;  // 回到输出尾部，便于看到最新命令
    char line[AIRI_CONSOLE_WIDTH];  // 命令查询先在128字节局部数组形成一行，再由airi_console_print复制进输出环
    if (equal(start, "help")) {     // 提交文字完整等于help时生成命令列表
        airi_console_print(c, "COMMAND    OPERATION");  // 将COMMAND    OPERATION保存为一条命令说明或身份记录
        airi_console_print(c, "sys        Kernel and boot identity");  // 将sys        Kernel and boot identity保存为一条命令说明或身份记录
        airi_console_print(c, "mem        Usable / held physical memory summary");  // 将mem        Usable / held physical memory summary保存为一条命令说明或身份记录
        airi_console_print(c, "mem map    Complete physical address inventory");  // 将mem map    Complete physical address inventory保存为一条命令说明或身份记录
        airi_console_print(c, "devices    Registered console hardware endpoints");  // 将devices    Registered console hardware endpoints保存为一条命令说明或身份记录
        airi_console_print(c, "log        Boot journal (ordered events, no wall clock)");  // 将log        Boot journal (ordered events, no wall clock)保存为一条命令说明或身份记录
        airi_console_print(c, "echo TEXT  Write text to this console");  // 将echo TEXT  Write text to this console保存为一条命令说明或身份记录
        airi_console_print(c, "clear      Clear terminal scrollback");  // 将clear      Clear terminal scrollback保存为一条命令说明或身份记录
        airi_console_print(c, "F1-F4      Terminal / Memory / Devices / Journal");  // 将F1-F4      Terminal / Memory / Devices / Journal保存为一条命令说明或身份记录
        airi_console_print(c, "PgUp/PgDn  Scroll terminal or memory inventory");  // 将PgUp/PgDn  Scroll terminal or memory inventory保存为一条命令说明或身份记录
        airi_console_print(c, "Up/Down    Command history    Ctrl-L: clear");  // 将Up/Down    Command history    Ctrl-L: clear保存为一条命令说明或身份记录
    } else if (equal(start, "sys"))  // 完整匹配sys时查询同一启动身份
        system_info(c);  // 从同一启动结构生成系统身份与内核镜像尺寸
    else if (equal(start, "mem")) {  // 完整匹配mem时按启动地图类别统计字节数
        airi_console_print(c, "TYPE                  KiB");  // 将TYPE                  KiB保存为一条命令说明或身份记录
        airi_console_print(c, "usable                %llu",  // 把本次查询字段按usable                %llu的格式保存到输出环
             (unsigned long long)(airi_memory_bytes(c->boot, AIRI_MEMORY_USABLE) / 1024));  // 字节除1024转KiB，再按%llu约定提供unsigned long long
        airi_console_print(c, "boot reclaim (held)   %llu",  // 把本次查询字段按boot reclaim (held)   %llu的格式保存到输出环
             (unsigned long long)(airi_memory_bytes(c->boot, AIRI_MEMORY_BOOT_RECLAIM) / 1024));  // 字节除1024转KiB，再按%llu约定提供unsigned long long
        airi_console_print(c, "kernel / modules      %llu",  // 把本次查询字段按kernel / modules      %llu的格式保存到输出环
             (unsigned long long)(airi_memory_bytes(c->boot, AIRI_MEMORY_KERNEL) / 1024));  // 字节除1024转KiB，再按%llu约定提供unsigned long long
        if (c->read_memory) { // guest 内存服务初始化后提供真实分配状态。
            struct airi_memory_stats stats; // 本次查询只借用同步填好的数值。
            c->read_memory(&stats); // 回调返回前复制，不借出页池或堆块指针。
            airi_console_print(c, "page pool: total=%u free=%u held=%u", stats.pages_total, stats.pages_free, stats.pages_total - stats.pages_free); // 页表和堆backing都计入held。
            airi_console_print(c, "kernel heap: bytes=%zu free_payload=%zu", stats.heap_bytes, stats.heap_free); // 区分页级backing与对象级空闲。
        } else // 较早检查点与宿主纯模型尚未绑定分配器。
            airi_console_print(c, "%u firmware regions. No allocator accounting yet.", c->boot->region_count); // 未接入就明确不提供伪造统计。
    } else if (equal(start, "mem map")) {  // 完整匹配mem map时逐条输出实际物理范围
        airi_console_print(c, "ID  PHYSICAL START   PHYSICAL END     TYPE");  // 将ID  PHYSICAL START   PHYSICAL END     TYPE保存为一条命令说明或身份记录
        for (unsigned i = 0; i < c->boot->region_count; ++i) {  // 只遍历启动地图有效前缀，每项形成一行范围说明
            airi_map_line(c, i, line, sizeof(line));  // 把第i条地图范围写入128字节临时行，下一步复制进命令输出环
            airi_console_print(c, "%s", line);  // 把本次查询字段按%s的格式保存到输出环
        }
    } else if (equal(start, "devices")) {  // 完整匹配devices时显示当前固定设备端点
        airi_console_print(c, "NAME    CLASS     CONFIGURATION");  // 将NAME    CLASS     CONFIGURATION保存为一条命令说明或身份记录
        for (unsigned i = 0; i < 3; ++i) {  // 逐项输出当前约定的3个设备说明
            airi_device_line(c, i, line, sizeof(line));  // 把第i个固定设备描述写入临时行，随后airi_console_print复制进输出环
            airi_console_print(c, "%s", line);  // 把本次查询字段按%s的格式保存到输出环
        }
    } else if (equal(start, "log")) {  // 完整匹配log时逐条显示有序启动记录
        for (unsigned i = 0; i < airi_journal_count(); ++i) {  // 按8条启动记录的有效下标查询并显示
            airi_journal_line(c, i, line, sizeof(line));  // 把第i条启动事件格式化到临时行，再追加到输出环
            airi_console_print(c, "%s", line);  // 把本次查询字段按%s的格式保存到输出环
        }
    } else if (equal(start, "clear"))  // 完整匹配clear时清可见输出环而不重置累计逻辑号
        c->line_count = 0;  // 清除可见历史行数，累计逻辑行号仍保留
    else if (!airi_memcmp(start, "echo ", length < 5 ? length : 5) && length >= 5)  // 最多比较当前长度与5的较小值，再要求至少5字节，确认echo加空格前缀
        airi_console_print(c, "%s", start + 5);  // 略过echo和空格这5字节，将余部作为本条输出
    else if (c->extension && c->extension(c, start, c->extension_context)) {  // 同步调用可选消费者；它自己形成输出，不能保留局部命令指针。
        /* 扩展已处理本条命令，不再生成未知命令错误。 */
    } else  // 没有扩展或扩展不认识本命令时，保留原来的具体错误。
        airi_console_print(c, "%s: unknown command. Type help.", start);  // 把本次查询字段按%s: unknown command. Type help.的格式保存到输出环
    airi_console_print(c, "");  // 在输出环中保存一条空行，为后续显示保留间隔
}
bool airi_console_key(struct airi_console *c, int key) {  // 消费一个逻辑键并报告显示状态是否变化
    if (key >= AIRI_KEY_F1 && key <= AIRI_KEY_F4) {  // 连续功能键范围F1到F4用于选择四种命令视图
        c->view = (enum airi_view)(key - AIRI_KEY_F1);  // F1到F4连续编号减去起点，得到0到3的视图编号
        return true;  // 本逻辑事件已处理，调用者可重画当前状态
    }
    if (key == AIRI_KEY_PAGE_UP || key == AIRI_KEY_PAGE_DOWN) {  // 两个翻页键先按当前视图区分内存分页和终端滚动
        if (c->view == AIRI_VIEW_MEMORY) {  // Memory视图以每12条记录的一页为移动单位
            unsigned pages = (c->boot->region_count + 11) / 12;  // 每页12条，先加11实现整数向上取整
            if (key == AIRI_KEY_PAGE_UP && c->map_page)  // 向上翻页且当前不是第一页时才减页号，避免无符号下溢
                --c->map_page;  // 内存地图回到前一页
            if (key == AIRI_KEY_PAGE_DOWN && c->map_page + 1 < pages)  // 向下翻页且还有下一有效页时才加页号
                ++c->map_page;  // 内存地图前进一页
        } else if (c->view == AIRI_VIEW_TERMINAL) {  // Terminal视图改变文字滚动量，内存页号保持原值
            if (key == AIRI_KEY_PAGE_UP) {  // 终端向上翻时从最新输出回看更早文字
                c->scroll += 10;  // 终端向较早文字滚动10条显示行
                if (c->scroll > c->line_count * 4)  // 滚动量最多保留每条日志拆成4显示行的预留范围
                    c->scroll = c->line_count * 4;  // 把滚动量限制在为拆行预留的最大范围
            } else  // 前一分支条件未成立时转入随后的单条替代操作
                c->scroll = c->scroll > 10 ? c->scroll - 10 : 0;  // 向输出尾部回滚10行，剩余不足10时归零
        }
        return true;  // 本逻辑事件已处理，调用者可重画当前状态
    }
    if (key == AIRI_KEY_UP || key == AIRI_KEY_DOWN) {  // 上下键进入命令历史浏览，不提交新命令
        if (!c->history_count)  // 尚未有已提交历史时没有可浏览项
            return false;  // 当前事件没有可执行操作，调用者无需因此重画
        if (!c->history_back)  // 第一次离开当前输入时先保存草稿，后续历史浏览不反复覆盖它
            airi_memcpy(c->draft, c->input, sizeof(c->draft));  // 首次看旧命令前保存未提交输入，返回时可恢复
        if (key == AIRI_KEY_UP && c->history_back < c->history_count)  // 向上且还未到最旧历史时增加回看距离
            ++c->history_back;  // 向更早的一条提交命令移动
        if (key == AIRI_KEY_DOWN && c->history_back)  // 向下且仍在历史中时减少回看距离，最终0恢复草稿
            --c->history_back;  // 向更新历史或当前草稿移动
        airi_snprintf(c->input, sizeof(c->input), "%s",  // 将选定历史或草稿复制回64字节输入数组，随后重测输入长度
                      c->history_back ? c->history[c->history_count - c->history_back] : c->draft);  // 浏览历史时按从尾部反数下标取旧命令，回到0时恢复未提交草稿
        c->input_length = (unsigned)airi_strlen(c->input);  // 历史文本复制后重新计算当前输入字节数
        c->view = AIRI_VIEW_TERMINAL;  // 切回Terminal显示当前输入或命令结果
        return true;  // 本逻辑事件已处理，调用者可重画当前状态
    }
    if (key == AIRI_KEY_CLEAR) {  // 清屏功能键移除可见历史并回到Terminal，输入文字仍保留
        c->line_count = 0;  // 清除可见历史行数，累计逻辑行号仍保留
        c->scroll = 0;      // 回到输出尾部，便于看到最新命令
        c->view = AIRI_VIEW_TERMINAL;  // 切回Terminal显示当前输入或命令结果
        return true;  // 本逻辑事件已处理，调用者可重画当前状态
    }
    if (key == '\n' || key == '\r') {  // 换行或回车提交控制台正在积累的命令
        execute(c);  // 提交当前输入副本，更新历史并在同一console生成命令结果
        c->input_length = 0;  // 本次提交结束后输入长度归零
        c->input[0] = 0;      // 在输入首字节写NUL，为下一条命令准备空串
        c->history_back = 0;  // 退出历史浏览，回到当前输入
        c->draft[0] = 0;      // 提交后丢弃浏览历史之前的旧草稿
        return true;  // 本逻辑事件已处理，调用者可重画当前状态
    }
    if (key == 8 || key == 127) {  // 两种退格逻辑字节都删除当前输入尾部一个字符
        if (c->input_length)  // 输入非空才允许删除尾字符，避免长度减1下溢
            c->input[--c->input_length] = 0;  // 先退一字节再写NUL，删除最后一个输入字符
        c->view = AIRI_VIEW_TERMINAL;  // 切回Terminal显示当前输入或命令结果
        return true;  // 本逻辑事件已处理，调用者可重画当前状态
    }
    if (key >= 32 && key <= 126 && c->input_length + 1 < sizeof(c->input)) {  // 只加入可打印ASCII；新内容后必须仍留一字节给NUL
        c->input[c->input_length++] = (char)key;  // 在旧末尾写入ASCII字符，然后推进有效长度
        c->input[c->input_length] = 0;  // 给新输入尾部补NUL供字符串函数读取
        c->view = AIRI_VIEW_TERMINAL;   // 切回Terminal显示当前输入或命令结果
        c->scroll = 0;  // 回到输出尾部，便于看到最新命令
        return true;    // 本逻辑事件已处理，调用者可重画当前状态
    }
    return false;  // 当前事件没有可执行操作，调用者无需因此重画
}
```

**完整替换 `kernel/main.c`。** 绑定快照并在所有初始化之后发布 AIRI_P2_READY；P1 标记保留给既有输入工具。

<!-- write: kernel/main.c -->
```c
#include <airi/memory.h> // 通过内存服务建立页池、页表、堆并查询快照。
#include <airi/trap.h> // 安装异常表并演示处理器现场。
#include <airi/arch.h>     // 通过架构边界使用忙等提示。
#include <airi/kernel.h>   // 取得启动信息、基础库与串口日志接口。
#include <airi/console.h>  // 取得命令环、按键与视图类型。

static struct airi_console console;  // 静态保存终端命令与日志，不在每次重画时重建。

#ifndef AIRI_DEMO_MESSAGE  // 允许修改实验在编译时替换输出文本。
#define AIRI_DEMO_MESSAGE "Hello from AiriOS P1"  // 普通配置默认输出的学习标记文字。
#endif  // 结束默认演示文字的条件定义
_Noreturn void airi_kernel_main(const AiriBootInfo *info) {  // 引导适配交入稳定启动描述；_Noreturn 声明内核不返回加载器。
    const char *error = airi_boot_validate(info);  // 校验必需启动字段，NULL 表示通过，字符串给出失败原因。
    if (error)  // 有具体原因则不能继续读取未经验证的数据。
        airi_panic(error);  // 输出失败原因并停止当前 CPU 执行。
    airi_trap_init(); // 通过启动校验后先接管异常，不启用设备中断。
    airi_trap_demo(); // 正常启动验证带与不带硬件错误码的返回路径。
#ifdef AIRI_TEST_FATAL_TRAP // 只在作者故障镜像中执行未处理异常。
    airi_trap_fatal_demo(); // 必须报告并停止，不能发布 READY。
#endif // 普通镜像继续初始化。
    airi_log("[BOOT_INFO] version=%u regions=%u loader=%s", info->version, info->region_count,  // 打印协议版本和有效内存区域数，%u 与无符号参数匹配。
             info->loader);  // 输出适配层复制且有 NUL 终止的加载器名称。
    airi_log("[KERNEL] physical=0x%llx virtual=0x%llx size=%llu",  // 串口分别记录内核物理位置、虚拟地址和字节长度。
             (unsigned long long)info->kernel_physical_base,  // 转换到 %llx 要求的无符号 long long。
             (unsigned long long)info->kernel_virtual_base, (unsigned long long)info->kernel_size);  // 虚拟地址采用十六进制，内核大小按字节十进制输出。
    airi_log("[HHDM] offset=0x%llx", (unsigned long long)info->direct_map_offset);  // 记录把物理地址换成直接映射虚拟地址的偏移量。
    for (uint32_t i = 0; i < info->region_count; ++i) {  // 只遍历启动描述声明的有效区域，不读取固定数组剩余项。
        const struct airi_memory_region *r = &info->regions[i];  // 借用第 i 条区域描述。
        airi_log("[MEMORY] index=%u base=0x%llx length=0x%llx kind=%s", i,  // 逐项输出下标、物理起点、长度及用途。
                 (unsigned long long)r->base, (unsigned long long)r->length,  // 地址和长度都是64位范围，按 long long 适配格式。
                 airi_memory_name(r->kind));  // 将区域种类转成可读名称，不改变区域数据。
    }
    uint64_t usable = airi_memory_bytes(info, AIRI_MEMORY_USABLE);  // 按实际种类累加可用区域字节数，不等同虚拟机总配置内存。
    airi_log("[MEMORY_SUMMARY] usable_bytes=%llu boot_reclaim_bytes=%llu",  // 输出可用内存与暂由加载器占用的可回收内存两个值。
             (unsigned long long)usable,  // 可用字节数转换匹配 %llu。
             (unsigned long long)airi_memory_bytes(info, AIRI_MEMORY_BOOT_RECLAIM));  // 另行累计加载器可回收区域，不混入 usable。
    airi_log("[LESSON] %s", AIRI_DEMO_MESSAGE);  // 发送当前配置的学习文本，修改实验可据此核对版本。
    airi_memory_init(info); // 只借出完整 usable 页，并在真实 RAM 演示写入与归还。
    airi_console_init(&console, info);  // 初始化内核静态控制台，借用整个启动期间有效的 info。
    console.read_memory = airi_memory_snapshot; // 文本查询只读取数值，不持有分配器内部对象。
    bool graphics = airi_console_draw(&console);  // 实际完成首帧后取得布尔结果，不能仅凭描述存在认定成功。
    if (graphics) {  // 有效图形路径才输出图形参数。
        const struct airi_framebuffer *fb = &info->framebuffer;  // 借用刚用于绘图的同一份帧缓冲描述。
        airi_log("[FRAMEBUFFER] width=%llu height=%llu pitch=%llu bpp=%u",  // 记录尺寸、字节行距和每像素位数，供截图与日志比对。
                 (unsigned long long)fb->width, (unsigned long long)fb->height,  // 宽高转为与 %llu 相同的参数类型。
                 (unsigned long long)fb->pitch, (unsigned)fb->bpp);  // pitch 按字节输出，bpp 按位数输出。
    } else  // 未完成图形时仍发送串口原因并继续启动。
        airi_log("[DISPLAY_SERIAL] no frame produced; using serial output");  // 图形不适用时记录原因路径，后面仍进入串口命令循环。
    /* Written AFTER all drawing, so tests can take a completed screenshot. */
    airi_log("[AIRI_P1_READY] graphics=%u usable_bytes=%llu", graphics ? 1u : 0u,  // 首帧调用结束后才发布完成标记，布尔转换为明确的0/1无符号值。
             (unsigned long long)usable);  // 完成记录附同一实际可用字节数。
    airi_log("[AIRI_P2_READY] exceptions=1 pages=1 own_cr3=1 heap=1"); // 所有内存演示和控制台初始化结束后才发布阶段完成。
    airi_serial_write("airi> ");  // 输出首个串口提示符。
    for (;;) {  // 没有退出条件，内核持续轮询输入。
        int key = airi_input_poll();  // 从虚拟键盘或 UART 取得一个共享键值，0 表示暂无输入。
        if (!key) {  // 没有新键时不处理命令和重画。
            airi_arch_relax();  // 提示当前架构正在忙等；这不是睡眠或调度。
            continue;  // 跳过当前不适用项，继续检查下一项。
        }
        uint64_t before = console.total_lines;  // 记录输入前日志尾，回车后只回显本次产生的新行。
        bool changed = airi_console_key(&console, key);  // 按键改变命令或页面状态，返回是否需要刷新。
        if (!changed)  // 被忽略的按键不产生新帧。
            continue;  // 跳过当前不适用项，继续检查下一项。
        if (key == '\n' || key == '\r') {  // 回车完成命令，才把新增输出回显到 UART。
            uint64_t first = console.total_lines - console.line_count;  // 日志环只保留 line_count 行，first 是尚可读取的最早逻辑行。
            if (before < first)  // 若本次命令挤掉旧行，先把读取起点限到仍存在的区域。
                before = first;  // 不访问已经被覆盖的逻辑行。
            for (uint64_t i = before; i < console.total_lines; ++i) {  // 逐行回显本次命令产生且仍保留的内容。
                airi_serial_write(airi_console_line(&console, i));  // 逻辑行号由环接口转换为数组下标。
                airi_serial_write("\n");  // 为该行补串口行结束。
            }
            airi_serial_write("airi> ");  // 输出首个串口提示符。
        }
        if (graphics)  // 只在启动时建立了图形路径的情况下重画。
            airi_console_draw(&console);  // 把变更后的命令与页面状态写成完整画面。
        /* Published after rendering: interaction tests observe completed frames. */
        if (key == '\n' || key == '\r' || key >= AIRI_KEY_PAGE_UP)  // 命令提交与功能键在绘图后发布可观察完成记录。
            airi_log("[CONSOLE_UPDATE] view=%u commands=%u page=%u", (unsigned)console.view,  // 输出当前页面、累计命令和内存页号，观察程序据此等待具体状态。
                     console.commands, console.map_page);  // 这两个计数来自刚处理并重画的同一命令对象。
    }
}
```

<a id="heap-lifetimes"></a>
## 交错分配时检查仍然活着的对象

主机观察先运行和 guest 相同的可控合并情形，再用固定种子的两千步交错操作维护十六个对象槽。每次操作后检查所有活对象的每一个请求字节，既能发现重复借出，也能发现分割或合并覆盖了邻居。最后归还所有槽，空闲 payload 必须恢复。固定种子让出现问题时能重复相同操作顺序；这组作者验证没有要求读者另写测试。

**新建 `tests/unit/heap_test.c`。** arena 是宿主已拥有的独立空间，不把内核高地址交给宿主。

<!-- write: tests/unit/heap_test.c -->
```c
#include <assert.h> // 失败直接返回非零，不依赖文字判断。
#include <stdio.h> // 宿主输出测试结果。
#include <airi/heap.h> // 同一小对象算法不依赖宿主页表。
static uint8_t arena[4096] __attribute__((aligned(16))); // 宿主只提供一个受控的已拥有范围。
struct item { uint8_t *pointer; size_t bytes; uint8_t pattern; }; // 记录每个活对象的内容预期。
int main(void) { // 独立验证对象生命周期和碎片回收。
    struct airi_heap heap = {0}; // 初始没有backing。
    assert(!airi_heap_init(&heap, arena + 1, sizeof(arena) - 1)); // 未对齐起点不能偷偷接受。
    assert(!airi_heap_init(&heap, arena, 32)); // 只有块头没有payload时必须失败。
    assert(airi_heap_init(&heap, arena, sizeof(arena))); // 正常创建单块空闲堆。
    size_t initial = airi_heap_free_bytes(&heap); // 记录元数据之外的真实payload容量。
    assert(initial == sizeof(arena) - 32); // 当前块头32字节。
    assert(!airi_heap_alloc(&heap, 0) && !airi_heap_alloc(&heap, SIZE_MAX)); // 零长度和对齐溢出均失败。
    assert(!airi_heap_alloc(&heap, sizeof(arena)) && airi_heap_free_bytes(&heap) == initial); // 耗尽不修改空闲状态。
    void *a = airi_heap_alloc(&heap, 80); // 创建相邻的三块，以控制碎片位置。
    void *b = airi_heap_alloc(&heap, 128); // 后续先归还中间块。
    uint8_t *c = airi_heap_alloc(&heap, 32); // 活对象阻止跨越它合并。
    assert(a && b && c && (uintptr_t)a % 16 == 0); // 所有对象非空且对齐。
    c[31] = 0x5a; // 保留活对象尾字节，确认合并不覆盖它。
    assert(!airi_heap_free(&heap, c + 1)); // 内部指针不能作为对象起点。
    assert(airi_heap_free(&heap, b) && airi_heap_free(&heap, a)); // 两个相邻空闲块应合并。
    assert(!airi_heap_free(&heap, a)); // 重复释放不得改变状态。
    void *joined = airi_heap_alloc(&heap, 208); // 请求大于80和128，必须利用合并结果。
    assert(joined == a && c[31] == 0x5a); // first-fit复用合并块，第三对象内容不变。
    assert(airi_heap_free(&heap, joined) && airi_heap_free(&heap, c)); // 全部归还会继续与尾块合并。
    assert(airi_heap_free_bytes(&heap) == initial && airi_heap_free(&heap, NULL)); // 合并恢复最初容量，NULL为空操作。
    struct item items[16] = {0}; // 十六个同时存活对象，空槽pointer为NULL。
    uint32_t random = 0x12345678; // 固定种子使失败可以逐次复现。
    for (unsigned step = 0; step < 2000; ++step) { // 完整演示交错大小、归还和碎片重用。
        random = random * 1664525u + 1013904223u; // 无符号运算按32位回绕，生成确定序列。
        unsigned slot = (random >> 16) % 16; // 从高位选择对象槽，避免只重复低位周期。
        struct item *item = &items[slot]; // 本次只改变一个活对象或空槽。
        if (item->pointer) { // 活槽先验证再归还。
            for (size_t i = 0; i < item->bytes; ++i) // 检查整个请求范围的内容。
                assert(item->pointer[i] == item->pattern); // 其他分配和归还不能覆盖活对象。
            assert(airi_heap_free(&heap, item->pointer)); // 结束该对象生命周期。
            item->pointer = NULL; // 槽不再持有过期指针。
        } else { // 空槽尝试不同大小分配。
            item->bytes = 1 + ((random >> 8) % 240); // 覆盖不足16字节和多个对齐跨度。
            item->pointer = airi_heap_alloc(&heap, item->bytes); // 容量不足允许返回NULL。
            item->pattern = (uint8_t)(slot + 1); // 各槽使用不同内容以发现重叠。
            if (item->pointer) { // 只有取得所有权后才写入。
                assert((uintptr_t)item->pointer % 16 == 0); // 每种大小都保持16字节对齐。
                airi_memset(item->pointer, item->pattern, item->bytes); // 填满请求范围，不写对齐填充区。
            }
        }
        for (unsigned n = 0; n < 16; ++n) // 每次操作后检查所有仍活着的对象。
            if (items[n].pointer) // 空槽没有可访问内容。
                for (size_t i = 0; i < items[n].bytes; ++i) // 不只检查被选槽。
                    assert(items[n].pointer[i] == items[n].pattern); // 检测分割、合并或重复借出导致的破坏。
    }
    for (unsigned n = 0; n < 16; ++n) // 释放最后留下的活对象。
        assert(airi_heap_free(&heap, items[n].pointer)); // NULL槽也明确允许。
    assert(airi_heap_free_bytes(&heap) == initial); // 不留下隐藏块头碎片或借用泄漏。
    puts("PASS heap: alignment, overflow, exhaustion, invalid free, coalescing, 2000 lifetime steps"); // 全部实际断言通过。
    return 0; // 正常退出宿主。
}
```

**完整替换 `Makefile`。** guest 加入纯字节堆实现，host-test 增加 ASan/UBSan 运行。

<!-- write: Makefile -->
```make
# 用既有make_command取得交叉工具参数；统一airios入口在K13加入。
# 内核C编译器由调用参数覆盖；此处默认clang，不借宿主libc。
CC := clang
# ELF链接器默认LLD，调用参数可传入真实找到的路径。
LD := ld.lld
# 主机实验用宿主编译器，可以链接宿主libc。
HOST_CC := cc
# ?=仅在调用者未赋值时取normal，各测试配置独立构建。
PROFILE ?= normal
# $(PROFILE)展开当前配置名称，OUT指向这一配置的产物目录。
OUT := build/$(PROFILE)
# 基础目标只编译文本会话与设备；不读取 frontend 的目录或字体。
SOURCES := kernel/main.c kernel/boot/limine.c kernel/boot/info.c \
    kernel/lib/base.c kernel/lib/runtime.c kernel/arch/x86_64/serial.c \
    kernel/console/console.c kernel/arch/x86_64/input.c kernel/arch/x86_64/cpu.c kernel/arch/x86_64/trap.c kernel/mm/pages.c kernel/mm/memory.c kernel/arch/x86_64/paging.c kernel/mm/heap.c
# 只有 desktop- 前缀目标才读取图形支线自己的构建片段。
ifneq ($(filter desktop-%,$(PROFILE)),)
include frontend/sources.mk # 本分支取得图形自己的源码清单。
# guest 图形适配与纯 C 前端进入同一内核，参考服务和 Cocoa 不进入。
SOURCES += kernel/console/display.c $(UI_SOURCES)
else # 没有desktop-前缀时只选择串口实现。
# 串口实现没有字体、绘图与桌面状态。
SOURCES += kernel/console/serial_display.c
endif # 结束当前可选图形分支。
# 去掉图形前缀后，两种呈现方式使用同一故障配置含义。
CASE := $(patsubst desktop-%,%,$(PROFILE))
# patsubst把每个.c相对路径改为当前OUT下的.o，汇编入口对象另加入。
OBJECTS := $(patsubst %.c,$(OUT)/%.o,$(SOURCES)) $(OUT)/kernel/arch/x86_64/entry.o $(OUT)/kernel/arch/x86_64/traps.o
# 交叉目标为无宿主ELF；C11/O2/调试信息/警告作为错误。freestanding与fno-builtin关闭宿主假设；禁栈保护和PIC/PIE；不使用red zone与浮点寄存器；kernel地址模型；MMD/MP生成头文件依赖。
CFLAGS := --target=x86_64-unknown-none-elf -std=c11 -O2 -g -Wall -Wextra -Werror \
          -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-pie \
          -mno-red-zone -mgeneral-regs-only -mcmodel=kernel -MMD -MP
# 预处理器从自有include、固定协议头third_party和前端include取得声明。
CPPFLAGS := -Iinclude -Ithird_party
# 只有显式图形配置需要前端头文件。
ifneq ($(filter desktop-%,$(PROFILE)),)
CPPFLAGS += -Ifrontend/include # 为已选择的图形实现提供头文件搜索位置。
endif # 结束当前可选图形分支。
# Make条件只在no-framebuffer配置加入下方测试宏。
ifeq ($(CASE),no-framebuffer)
# +=追加预处理选项，让启动适配模拟缺少图形响应。
CPPFLAGS += -DAIRI_TEST_NO_FRAMEBUFFER
endif # 结束 no-framebuffer 的编译条件
# 缺内存响应配置采用独立构建目录和下面宏。
ifeq ($(CASE),missing-memory)
# 模拟必需内存地图缺失，内核应打印具体PANIC。
CPPFLAGS += -DAIRI_TEST_NO_MEMMAP
endif # 结束 missing-memory 的编译条件
# 作者故障配置不登记恢复，应报告真实异常后停止。
ifeq ($(CASE),trap-fatal)
CPPFLAGS += -DAIRI_TEST_FATAL_TRAP # 仅此构建目录触发未处理 UD2。
endif # 结束故障配置。
# 修改实验仅改变标题与输出文本，普通配置不继承这两个宏。
ifeq ($(CASE),edit-experiment)
# 单引号由shell保护内部双引号，编译器取得C字符串宏值；下一续行同时设置学习日志文字。
CPPFLAGS += -DAIRI_PANEL_TITLE='"LOCAL / MAINTENANCE CONSOLE"' \
            -DAIRI_DEMO_MESSAGE='"I changed the output and rebooted"'
endif # 结束 edit-experiment 的文字覆盖条件

# 这些名称是动作目标，即使同名文件存在也执行其规则。
.PHONY: all kernel image host-test clean
# 默认动作依赖镜像，触发内核编译、链接和镜像制作。
all: image
# 只构建独立内核ELF，不制作启动镜像。
kernel: $(OUT)/airios.elf
# 模式规则将相同相对路径的C源文件编成当前配置对象；%匹配路径主体。
$(OUT)/%.o: %.c
# @隐藏命令回显；$@是目标路径，dir取得其目录并确保存在。
	@mkdir -p $(dir $@)
# $<取第一项依赖即当前源文件，$@取输出对象；-c只编译不链接。
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
# 同一对象布局下编译带预处理的汇编入口。
$(OUT)/%.o: %.S
# @隐藏命令回显；$@是目标路径，dir取得其目录并确保存在。
	@mkdir -p $(dir $@)
# $<取第一项依赖即当前源文件，$@取输出对象；-c只编译不链接。
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
# ELF依赖全部对象与链接脚本，任一变化会重新链接。
$(OUT)/airios.elf: $(OBJECTS) kernel/linker.ld
# 这一完整续行命令用ELF x86-64、无标准库、静态链接、4KiB页对齐和不可执行栈；-T指定自有布局，最后-o写目标。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 \
	    -z noexecstack -T kernel/linker.ld $(OBJECTS) -o $@
# 镜像先要求独立ELF成功。
image: $(OUT)/airios.elf
# 把同一配置的ELF装入UEFI启动镜像。
	python3 scripts/image.py $(PROFILE)
# 主机字节算法、启动合同与命令状态检查，保留assert并启用ASan/UBSan。
host-test:
# 先建立主机检查产物目录。
	@mkdir -p build/tests
# 以下完整续行命令编译主机检查；fno-builtin避免优化绕过被测实现，sanitizer检查内存和未定义行为，AIRI_HOST_TEST避开真实端口指令。
	$(HOST_CC) -std=c11 -g -O1 -Wall -Wextra -Werror -fno-builtin \
	    -fsanitize=address,undefined -DAIRI_HOST_TEST -Iinclude tests/unit/base_test.c \
	    kernel/lib/base.c kernel/boot/info.c \
	    kernel/console/console.c kernel/arch/x86_64/input.c \
		    -o build/tests/base_test
# 只有全部assert成立才打印PASS，失败状态返回Make。
	./build/tests/base_test
# 独立编译同一物理页算法，主机不解引用 guest 地址。
	$(HOST_CC) -std=c11 -g -O1 -Wall -Wextra -Werror -fno-builtin \
	    -fsanitize=address,undefined -Iinclude tests/unit/pages_test.c \
	    kernel/mm/pages.c kernel/boot/info.c kernel/lib/base.c -o build/tests/pages_test
# 验证耗尽、保留范围、重复释放和舍入边界。
	./build/tests/pages_test
# 同一四级页表算法在受控arena里验证，绝不安装宿主页表。
	$(HOST_CC) -std=c11 -g -O1 -Wall -Wextra -Werror -fno-builtin \
	    -fsanitize=address,undefined -DAIRI_HOST_TEST -Iinclude tests/unit/vm_test.c \
	    kernel/arch/x86_64/paging.c kernel/mm/pages.c kernel/boot/info.c kernel/lib/base.c -o build/tests/vm_test
# 覆盖翻译、共享分支、空表回收和中途耗尽的所有权回滚。
	./build/tests/vm_test
# 在宿主受控backing上验证同一小对象实现，不借用guest地址。
	$(HOST_CC) -std=c11 -g -O1 -Wall -Wextra -Werror -fno-builtin \
	    -fsanitize=address,undefined -Iinclude tests/unit/heap_test.c \
	    kernel/mm/heap.c kernel/lib/base.c -o build/tests/heap_test
# 交错分配与归还时检查所有活对象内容和最终容量。
	./build/tests/heap_test
# 清理仅删除可重建产物，保留运行证据artifacts。
clean:
# 调用shutil删除build；ignore_errors允许尚不存在目录。
	python3 -c 'import shutil; shutil.rmtree("build", ignore_errors=True)'
# 后缀替换得到自动依赖文件；前缀-允许首次构建尚无.d。
-include $(OBJECTS:.o=.d)


# 手动选择图形动作时才读取支线目标；默认构建不检查该目录。
ifneq ($(filter ui-preview ui-test lab-editor,$(MAKECMDGOALS)),)
include frontend/targets.mk # 仅显式图形动作读取相应规则。
endif # 结束当前可选图形分支。

# 三个观察名称均是动作，即使有同名文件也运行。
.PHONY: lab-memmove lab-boot-info
# 重新构建重叠移动与截断输出观察。
lab-memmove:
# 建立观察程序目录，保持所有可重建产物在build。
	@mkdir -p build/labs
# 下面续行按O0保留便于调试的步骤，链接当前观察所需的同一自有实现。
	$(HOST_CC) -std=c11 -O0 -g -Wall -Wextra -Werror -Iinclude \
	    labs/host/base/memmove_trace.c kernel/lib/base.c -o build/labs/memmove
# 打印左/右重叠及截断结果。
	./build/labs/memmove
# 重新构建合法/损坏启动数据观察。
lab-boot-info:
# 建立观察程序目录，保持所有可重建产物在build。
	@mkdir -p build/labs
# 下面续行按O0保留便于调试的步骤，链接当前观察所需的同一自有实现。
	$(HOST_CC) -std=c11 -O0 -g -Wall -Wextra -Werror -Iinclude \
	    labs/host/boot/boot_info_probe.c kernel/boot/info.c -o build/labs/boot-info
# 打印启动合同的正常与独立失败原因。
	./build/labs/boot-info
```

启动观察改为等完整的 AIRI_P2_READY，不提前看到 P1 标记就检查尚未写完的 P2 行。UART 的真实输入回归增加 mem 查询，PS/2 的旧 mem 预期同步换成页池输出。其他超时、进程清理、无帧缓冲与必需信息缺失路径保持。

**完整替换 `scripts/boot.py`。** 正常镜像等待本轮 P2 完成；故障镜像仍等待对应 PANIC。

<!-- write: scripts/boot.py -->
```python
"""QEMU launch and bounded boot checks; success requires a kernel marker."""  # 固定虚拟机参数并按本轮内核标记判断启动结果
import json  # 在 Python 对象与 JSON 文本之间转换，不执行其中内容。
from pathlib import Path  # Path 表示主机路径对象，/ 在这里拼接路径片段。
import shutil      # 取得目录复制、文件复制与临时结果清理接口。
import socket      # 取得主机通信连接接口，QMP/调试器通过独立连接交接。
import subprocess  # 以参数列表启动已有外部程序，并读取输出或退出码。
import tempfile    # 创建互不冲突的临时路径，用完后显式回收。
import time  # 单调时钟计算截止时间，sleep 给设备与进程处理时间。
from tools import ROOT, tool  # ROOT 确定项目位置，tool 按已建立优先级寻找工具。

def command(profile, work, gui=False, debug=False, debug_port=1234):  # 返回 QEMU 参数列表；profile 选镜像，work 保存本轮可写固件与日志。
    work.mkdir(parents=True,exist_ok=True)  # 建立本轮输出目录，已有目录允许复用。
    firmware=ROOT/'third_party/firmware'    # 定位固定固件目录。
    variables=work/'ovmf-vars.fd'  # 本轮固件变量单独存放，避免改动锁定的原始模板。
    shutil.copyfile(firmware/'edk2-i386-vars.fd',variables)  # 复制变量模板，QEMU 可写副本而保持外部依赖原件。
    cmd=[tool('qemu-system-x86_64','qemu'),'-machine','q35','-accel','tcg',  # 先选 QEMU 路径与 q35/TCG，参数列表按元素边界传递。
         '-cpu','qemu64','-smp','1','-m','256M','-vga','std',  # 固定CPU型号、单核、256MiB及标准VGA。
         '-drive',f'if=pflash,format=raw,unit=0,readonly=on,file={firmware/"edk2-x86_64-code.fd"}',  # 只读代码固件是第0个 pflash 单元，f 字符串填入实际路径。
         '-drive',f'if=pflash,format=raw,unit=1,file={variables}',  # 第1个 pflash 单元使用本轮可写变量副本。
         '-monitor','none','-cdrom',str(ROOT/'build'/profile/'airios.iso'),'-boot','d','-no-reboot',  # 挂载对应配置镜像，从光盘启动，故障时不循环重启。
         '-serial',f'file:{work/"serial.log"}']  # 默认 UART 输出保存到本轮 serial.log。
    if not gui: cmd+=['-display','none']  # 无人操作时隐藏显示窗口，guest 仍可绘图并经 QMP 截帧。
    if debug: cmd+=['-S','-gdb',f'tcp:127.0.0.1:{debug_port}']  # -S 暂停CPU，GDB只监听回环地址指定端口。
    return cmd  # 仅返回参数列表，调用者决定何时启动与回收进程。
def qmp_execute(sockpath, execute, arguments=None):  # 本机连接只执行本次指定的控制请求。
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:  # 外层拥有底层连接，最后关闭。
        sock.settimeout(5)  # 连接、发送和读取最多阻塞五秒。
        sock.connect(str(sockpath))  # 路径定位QEMU控制端点，不是普通输出文件。
        with sock.makefile('rb') as stream:  # 内层包装按行读取字节，先关闭包装再关闭连接。
            json.loads(stream.readline())  # 消费服务器问候，JSON库直接接受bytes。
            result = None  # 尚未收到动作结果，不与空字典混淆。
            request = {'execute': execute}  # 当前控制动作保存为请求字段。
            if arguments is not None:  # 空字典也是显式参数，只有None表示不提供。
                request['arguments'] = arguments  # 将本次参数加到同一请求。
            for req in ({'execute': 'qmp_capabilities'}, request):  # 先协商能力再发实际动作。
                sock.sendall((json.dumps(req) + '\n').encode())  # 直到全部消息字节交出或失败，不假设一次write写完。
                while True:  # 跳过异步事件，等待当前请求的结果。
                    reply = json.loads(stream.readline())  # 按消息换行取得完整JSON字节。
                    if 'error' in reply:  # QEMU拒绝了当前请求。
                        raise RuntimeError(str(reply['error']))  # 保留服务器的具体错误。
                    if 'return' in reply:  # event不满足完成条件。
                        result = reply['return']  # 取得当前请求结果。
                        break  # 此请求已完成，进入下一个或返回。
            return result  # 返回实际动作结果，两个with仍按由内到外关闭资源。

def qmp_screenshot(sockpath, output):  # 把本轮截图目标路径交给QEMU控制端点。
    qmp_execute(sockpath, 'screendump', {'filename': str(output)})  # 控制显示设备保存PPM，不向UART发送命令。
def check(profile='normal',timeout=35):  # 每次启动最多35秒，成功条件由配置对应完成标记决定。
    work=ROOT/'artifacts'/('boot-'+profile)  # 不同配置分开保存启动证据。
    work.mkdir(parents=True,exist_ok=True)   # 建立本轮输出目录，已有目录允许复用。
    (work/'result.json').unlink(missing_ok=True)  # 删除上次汇总，失败不能留下旧成功身份。
    serial=work/'serial.log'  # 当前检查只读取这次启动的串口文件。
    serial.unlink(missing_ok=True)  # 删去旧日志；不存在不报错，旧 READY 不会使新启动提前通过。
    # AF_UNIX paths are capped at 104 bytes on macOS. Source exports can be deep.
    endpoints=tempfile.TemporaryDirectory(prefix='airios-qmp-',dir='/tmp')  # 在短/tmp路径建立控制端点，避免 macOS UNIX socket 路径过长。
    sockpath=Path(endpoints.name)/'qmp.sock'  # 临时目录中的 qmp.sock 命名本次控制连接。
    cmd=command(profile,work)+['-qmp',f'unix:{sockpath},server=on,wait=off']  # 在固定启动参数后追加控制端点，QEMU不等待客户端才启动。
    (work/'command.json').write_text(json.dumps(cmd,indent=2))  # 保存实际参数列表供本轮复现。
    case = profile.removeprefix('desktop-')  # 图形前缀不改变故障注入含义。
    graphics = profile.startswith('desktop-') and case != 'no-framebuffer'  # 有绘图实现且设备存在才应生成画面。
    expected='[AIRI_PANIC] required Limine memory map is missing' if case=='missing-memory' else '[AIRI_P2_READY]'  # 正常配置等完整内存阶段，P1标记仍供早期输入观察。
    if case == 'trap-fatal': # 未处理异常镜像必须进入具体停止路径。
        expected = '[AIRI_PANIC] unhandled CPU exception' # 不以 QEMU 存活推断故障已处理。
    started=time.monotonic()  # 单调秒数作为经过时间起点，不受系统时钟调整影响。
    with (work/'qemu.log').open('w') as errors:  # 主机 QEMU 输出单独存档，with 保证文件最终关闭。
        process=subprocess.Popen(cmd,stdout=errors,stderr=errors)  # 启动后立即取得进程句柄，轮询 guest 标记而非等待进程退出。
        try:  # 受保护区域失败也必须进入后面的资源回收。
            while time.monotonic()-started<timeout:  # 按实际经过秒数限制整次启动等待。
                text=serial.read_text(errors='replace') if serial.exists() else ''  # 日志尚未创建时取空串；暂时不完整的字节以替代字符读取。
                complete_lines = text.splitlines(keepends=True)  # 保留行结束符，完整记录与写到一半的标记可区分。
                if any(line.endswith('\n') and line.startswith(expected) for line in complete_lines):  # 生成器逐行检查，至少一个完整且以预期标记开头才成立。
                    if case in ('missing-memory', 'trap-fatal'):  # 失败配置只允许具体拒绝记录，不能同时宣布启动完成。
                        assert '[AIRI_P1_READY]' not in text  # 错误路径不得出现成功标记。
                    else:  # 按当前profile的另一条完整验证路径处理。
                        assert '[AIRI_P2_READY]' in text # 当前主线还必须完成页池、自有页表和堆。
                        assert '[TRAP_DEMO_OK] ud=6 gp=13 resumed=2' in text # 正常启动必须真的从两种异常栈返回。
                        assert '[AIRI_PANIC]' not in text  # 正常完成路径不得混入内核停止记录。
                        assert f'graphics={int(graphics)}' in text  # 报告值须与本次选择的呈现实现一致。。
                        assert '[MEMORY_SUMMARY]' in text  # 完成之前应已记录实际内存总计。
                        if case=='edit-experiment':  # 修改实验额外核对替换后的文字。
                            assert 'I changed the output and rebooted' in text  # 确认运行的是实验配置的输出行为。
                        if graphics:  # 有图形的完成路径才请求显示面截图。
                            qmp_screenshot(sockpath,work/'screen.ppm')  # 在完整 READY 之后读取对应已完成画面。
                    result={'profile':profile,'passed':True,'expected_marker':expected,  # 记录配置、成功与本次实际等待的标记。
                            'seconds':round(time.monotonic()-started,2)}  # 记录经过秒数，round(...,2) 保留两位小数。
                    (work/'result.json').write_text(json.dumps(result,indent=2)+'\n')  # 结果编码为便于阅读的 JSON，并补文件末尾换行。
                    print(f'PASS boot/{profile} ({result["seconds"]}s)')  # 打印本次配置和耗时，而非以QEMU仍运行判定通过。
                    return result  # 当前配置标记与截图核对成功，返回这次启动记录。
                if process.poll() is not None:  # poll 非 None 表示主机 QEMU 已退出，不能继续等 guest 标记。
                    raise RuntimeError(f'QEMU exited before marker; see {work}')  # 提前退出时报告证据目录。
                time.sleep(0.1)  # 给虚拟CPU和日志写入100毫秒处理时间，再检查。
            raise RuntimeError(f'Boot timeout after {timeout}s; see {work}')  # 耗尽截止时间后明确失败，并保留该配置的日志。
        finally:  # 成功返回与异常离开都进入收尾。
            process.terminate()  # 先请求主机 QEMU 进程结束。
            try:  # terminate只是结束请求，随后仍需收回子进程。
                process.wait(timeout=5)  # 最多等待五秒，防止清理本身无限卡住。
            except subprocess.TimeoutExpired:  # QEMU没有在正常终止期限内退出。
                process.kill()  # 强制结束本轮仍存活的QEMU。
                process.wait()  # 收回已结束子进程的退出状态和资源。
            sockpath.unlink(missing_ok=True)  # 删除本轮控制端点，端点已消失时允许继续。
            endpoints.cleanup()  # 回收临时目录，不删除保留在 artifacts 的证据。
```

**完整替换 `scripts/console_test.py`。** 查询结果必须属于当前命令后的新日志。

<!-- write: scripts/console_test.py -->
```python
"""Exercise the running guest through QEMU keys and real UART receive bytes."""  # 两种设备输入均进入真正运行的内核控制台
import json    # 在 Python 对象与 JSON 文本之间转换，不执行其中内容。
import socket  # 取得主机通信连接接口，QMP/调试器通过独立连接交接。
import subprocess  # 以参数列表启动已有外部程序，并读取输出或退出码。
import tempfile    # 创建互不冲突的临时路径，用完后显式回收。
from pathlib import Path  # Path 表示主机路径对象，/ 在这里拼接路径片段。
import time  # 单调时钟计算截止时间，sleep 给设备与进程处理时间。
from boot import command, qmp_execute, qmp_screenshot  # 复用启动、QMP 请求与截图接口，输入检查继续等待 guest 完成记录。
from tools import ROOT  # 共享 ROOT 指向项目根目录，后续路径不依赖调用位置。

def check_console(profile='normal', uart_input=None):  # 每次启动独立guest，按uart_input选择UART字节或PS/2按键。
    graphics = profile.startswith('desktop-') and not profile.endswith('no-framebuffer')  # 根据实际构建选择有无图像断言。
    if uart_input is None:  # 没有显式选择设备时，串口目标使用 UART。
        uart_input = not graphics  # 默认串口目标走UART，桌面目标走PS/2；调用者可显式改选。
    work=ROOT/'artifacts'/('console-'+profile+('-ps2' if not uart_input else '-uart'))  # 不同输入路径保存独立本轮证据。
    work.mkdir(parents=True,exist_ok=True)  # 建立本轮输出目录，已有目录允许复用。
    (work/'result.json').unlink(missing_ok=True)  # 删除上次汇总，失败不能留下旧成功身份。
    serial=work/'serial.log'  # 只读取这次guest对应的日志。
    serial.unlink(missing_ok=True)  # 删除旧完成标记，不存在时允许继续。
    endpoints=tempfile.TemporaryDirectory(prefix='airios-console-',dir='/tmp')  # 短临时目录保存本次控制与UART端点，规避socket路径长度限制。
    qmp=Path(endpoints.name)/'qmp.sock'    # QMP端点只传主机控制请求，不是内核串口。
    uart=Path(endpoints.name)/'uart.sock'  # UART端点传设备收发字节，无图形时也能交互。
    cmd=command(profile,work)+['-qmp',f'unix:{qmp},server=on,wait=off']  # 复用启动参数，再添加本轮QMP服务端点。
    wire=None  # 尚未建立UART输入连接，PS/2路径保持None。
    received=bytearray()  # 可增长数组累积本次设备字节。
    if uart_input:  # 所选输入通路为UART时才建立或读取字节连接。
        cmd[cmd.index('-serial')+1]=f'unix:{uart},server=on,wait=off'  # 找到-serial参数后替换其值，把原文件输出改成设备socket。
        cmd+=['-S']  # 先暂停CPU，UART接收连接建好后再执行，以免丢早期输出。
    started=time.monotonic()  # 单调秒数作为经过时间起点，不受系统时钟调整影响。
    with (work/'qemu.log').open('w') as errors:  # 主机 QEMU 输出单独存档，with 保证文件最终关闭。
        process=subprocess.Popen(cmd,stdout=errors,stderr=errors)  # 启动后立即取得进程句柄，轮询 guest 标记而非等待进程退出。
        try:  # 受保护区域失败也必须进入后面的资源回收。
            if uart_input:  # 所选输入通路为UART时才建立或读取字节连接。
                deadline=time.monotonic()+10  # 等待UART端点创建最多十秒。
                while not uart.exists():      # 端点尚未建立时短暂重试，不能立刻connect。
                    if process.poll() is not None or time.monotonic()>deadline:  # CPU暂停仍要求QEMU存活并及时创建端点。
                        raise RuntimeError('UART socket unavailable')  # 端点未建立时拒绝继续连接。
                    time.sleep(.05)  # 暂停50毫秒给设备或日志处理时间，再重复本次检查。
                wire=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM)  # 本机流式UART端点。
                wire.connect(str(uart))  # 接收连接在CPU继续之前建好。
                wire.setblocking(False)  # 暂无字节时立即抛BlockingIOError。
                qmp_execute(qmp,'cont')  # 接收端准备好后通过控制连接让虚拟CPU继续。
            def output():  # 取得同一次guest的累计串口文本，UART字节同时保存文件。
                if wire:  # 有UART连接时从该连接读取；否则读取QEMU写出的日志文件。
                    while True:  # 尽量取出当前已有UART字节，无更多数据或连接结束时停止。
                        try:  # 单次非阻塞读取可能暂时无字节。
                            chunk=wire.recv(4096)  # 最多接收4096设备字节。
                        except BlockingIOError:  # 暂无字节不表示设备故障。
                            break  # 停止本轮读取，wait稍后会再调用output。
                        if not chunk:  # 空bytes说明设备连接结束。
                            break  # 结束这一轮字节收集。
                        received.extend(chunk)  # 可变bytearray追加本次真实接收字节。
                    serial.write_bytes(received)  # 把累计设备输出保存在本轮日志，失败时仍可查看。
                    return received.decode(errors='replace')  # 字节解码为文本，异常编码以替代字符呈现而不丢整份记录。
                return serial.read_text(errors='replace') if serial.exists() else ''  # 文件尚未创建时返回空串，等待循环继续。
            def wait(marker,after=0,timeout=10):  # after是旧日志字符偏移，只在之后寻找这次完成标记。
                deadline=time.monotonic()+timeout  # 每次等待按单调时间设置独立截止秒数。
                while time.monotonic()<deadline:   # 剩余时间内读取新的实际guest输出。
                    text=output()  # 获取累计文本，长度可能随guest运行增加。
                    for line in text[after:].splitlines(keepends=True):  # 只检查本次动作之后的行，保留结束符以识别尚未写完的记录。
                        if not line.endswith('\n'):  # 最后一行可能只有半条状态，不能据此通过。
                            continue  # 等后续读取取得完整换行再检查这条记录。
                        record=line.removeprefix('airi> ').removesuffix('\n').removesuffix('\r')  # 只移除已知提示符与CRLF，保留字段本身。
                        matches=record==marker  # CONSOLE_UPDATE要求完整等值，page=1不会匹配page=10。
                        if marker=='[AIRI_P1_READY]':  # READY后还有graphics与内存字段，按标记后的空格确定边界。
                            matches=record.startswith(marker+' ')  # 接受带字段的完整READY行，不接受相似标记名称。
                        if matches:  # 本次输出中已取得完整且匹配的状态记录。
                            return text[after:]  # 把本次动作的新结果交回调用者继续核对正文。
                    if process.poll() is not None:  # 运行进程提前退出，即使旧日志有文字也不能当成本次输入成功。
                        raise RuntimeError('Guest exited during console test')  # 报告真实guest生命周期失败。
                    time.sleep(.05)  # 暂停50毫秒给设备或日志处理时间，再重复本次检查。
                raise RuntimeError(f'Console timeout waiting for {marker}; see {serial}')  # 明确给出缺少的标记和已有日志路径。
            wait('[AIRI_P1_READY]',timeout=35)  # 输入前先等内核初始化与首帧结束，最多35秒。
            def key(*codes):  # 形参*把多个键名收集成元组，可同时发送ctrl或shift组合。
                qmp_execute(qmp,'send-key',{'keys':[{'type':'qcode','data':c} for c in codes],'hold-time':20})  # 每个键名转qcode字典，列表作为keys，按住时间单位毫秒。
                time.sleep(.12)  # 两次虚拟键事件间隔120毫秒，轮询内核有时间逐次消费。
            def update(view,commands,page=0):  # 组装与内核输出字段顺序一致的具体完成标记。
                return f'[CONSOLE_UPDATE] view={view} commands={commands} page={page}'  # f字符串把预期页面、累计命令数与内存页号代入文本。
            def capture(path):  # 只有实际图形目标才读取显示设备。
                if graphics:  # 串口或缺帧缓冲目标不产生截图。
                    qmp_screenshot(qmp, path)  # 在命令完成后保存本轮画面。
            commands=0  # 本次guest已提交命令数，嵌套run每次递增。
            def run(text,expect):  # 把整条终端命令送虚拟键盘，等待这次回车对应记录并核对输出。
                nonlocal commands     # 修改check_console作用域变量，不在run内另建未初始化的局部计数。
                offset=len(output())  # 先记录旧文本长度，后续只消费它之后的完成与结果。
                for c in text:  # 字符串逐个字符迭代。
                    key('spc' if c==' ' else 'minus' if c=='-' else c)  # 空格/横线用QEMU键名，其余直接送字符名。
                key('ret')   # 回车使guest处理当前完整命令。
                commands+=1  # 预期累计提交数增加一。
                tail=wait(update(0,commands),offset)  # 终端view=0且命令数匹配后才得到本次输出尾部。
                assert expect in tail, f'Missing output: {expect}'  # 缺少当前命令预期文字时用assert明确中止。
            if wire:  # 有UART连接时从该连接读取；否则读取QEMU写出的日志文件。
                offset=len(output())  # 记录第一条UART命令之前的日志末尾。
                wire.sendall(b'echo uart-receive-ok\r')  # bytes中的回车提交这条实际设备命令。
                tail=wait(update(0,1),offset)  # 第一条UART命令完成后取得新增输出。
                assert 'uart-receive-ok' in tail and 'graphics=0' in output()  # 无图形启动仍须实际处理UART输入与echo。
                offset=len(output())  # 第二条命令不复用第一条完成标记。
                wire.sendall(b'devices\n')     # 实际UART字节末尾换行提交。
                tail=wait(update(0,2),offset)  # 等第二条UART命令的完成记录。
                if profile.endswith('no-framebuffer'):  # 缺设备与未启用绘图是两个独立事实。
                    assert 'fb0     display   unavailable' in tail  # 设备查询明确图形不可用。
                offset=len(output())  # 新命令只观察之后的完成记录。
                wire.sendall(b'theme day\n')  # 串口目标没有主题扩展，应明确拒绝。
                wait(update(0,3),offset)      # 等待第三次真实提交。
                assert 'unknown command' in output()[offset:]  # 不把无消费者的主题设置伪装成功。
                offset=len(output()) # 内存查询使用本次命令之后的新记录。
                wire.sendall(b'mem\n') # 同一串口主线查询真实页池与堆。
                tail=wait(update(0,4),offset) # 等第四次提交的完整状态。
                assert 'page pool: total=' in tail and 'kernel heap: bytes=65536' in tail # 不能只返回启动地图。
                cases=['UART receive / command dispatch','serial-only device report']  # 结果只记录此路径实际运行的两项观察。
            else:  # 按当前profile的另一条完整验证路径处理。
                capture(work/'terminal.ppm')  # READY已到达后保存初始终端画面。
                run('help','mem map    Complete physical address inventory')  # 验证帮助输出包含完整物理地图命令说明。
                run('mem','page pool: total=')     # 验证实际内存描述，不假装已有分配器占用统计。
                run('unknown','unknown command. Type help.')  # 未知命令必须得到具体错误文本。
                run('echo ready','ready')  # 检查文字参数到输出的正常命令通路。
                for code,view,name in [('f2',1,'memory'),('f3',2,'devices'),('f4',3,'journal'),('f1',0,'terminal-active')]:  # 每个三元组同时指定按键、预期视图和截图名称，循环解包三项。
                    offset=len(output())  # 记录切页前日志尾。
                    key(code)  # 按本三元组指定的功能键。
                    wait(update(view,commands),offset)  # 等对应视图新完成记录，命令数不变。
                    capture(work/(name+'.ppm'))  # 对应视图完成后才保存画面。
                    if code=='f2':  # 只有内存页面具有物理区域分页操作。
                        offset=len(output())  # 记录翻页之前的日志尾。
                        key('pgdn')  # 请求向后翻内存地图页。
                        wait(update(1,commands,1),offset)  # 等内存view=1/page=1完成。
                        capture(work/'memory-page2.ppm')   # 保存第二页画面，与刚等待的page=1对应。
                        offset=len(output())  # 此次向前翻页排除刚才page=1记录。
                        key('pgup')  # 请求返回首个内存页。
                        wait(update(1,commands,0),offset)  # 等page=0新完成。
                if graphics:  # 日夜命令属于图形扩展。
                    for mode in ('day', 'night'):  # 两个真实命令都走 PS/2 输入路径。
                        run('theme ' + mode, 'Appearance: ' + mode.title())  # 等待命令完成标记，不以进程仍在运行代替成功。
                        capture( work / ('theme-' + mode + '.ppm'))  # 保存真实 guest 帧缓冲，供与主机样板比较。
                run('mem map','PHYSICAL END')  # 完整地图命令输出应包含区域结束地址列。
                offset=len(output())  # 清屏前排除已有完成记录。
                key('ctrl','l')  # 同时发送Ctrl-L功能组合。
                wait(update(0,commands),offset)  # 等清屏完成，累计命令数保持。
                capture(work/'cleared.ppm')      # 清屏完成后再截帧。
                offset=len(output())  # 记录历史操作开始位置。
                key('up')    # 取回最近的mem map历史文本。
                key('ret')   # 回车形成新的命令提交。
                commands+=1  # 等待新的累计命令数。
                wait(update(0,commands),offset)  # 等待新增命令计数的完成标记。
                assert 'airi> mem map' in output()[offset:]  # 历史回取必须实际提交前一条地图命令。
                offset=len(output())  # 先记录旧文本长度，后续只消费它之后的完成与结果。
                for c in 'syx':  # 先输入错误命令草稿。
                    key(c)  # 将当前字符送到虚拟键盘。
                key('backspace')  # 删除草稿末尾x。
                key('s')     # 补成sys。
                key('ret')   # 提交修复后的命令。
                commands+=1  # 本次实际提交也计入预期数。
                assert 'bootstrap kernel' in wait(update(0,commands),offset)  # 修复后的sys命令应报告当前内核身份。
                offset=len(output())  # 先记录旧文本长度，后续只消费它之后的完成与结果。
                for c in 'echo ':     # 逐个字符准备echo前缀。
                    key('spc' if c==' ' else c)  # 空格转spc键名。
                key('shift','a')  # 同时按Shift和a得到大写A。
                key('9')  # 普通数字9。
                key('shift','1')  # Shift-1转换为标点!。
                key('ret')   # 提交echo A9!。
                commands+=1  # 与guest累计提交计数同步。
                assert 'A9!' in wait(update(0,commands),offset)  # 等待本次完成后核对大写与标点扫描码转换。
                capture(work/'terminal-session.ppm')  # 保存全部命令序列结束时的实际终端画面。
                cases=['PS/2 command dispatch','help/mem/map/echo/errors','F1-F4 guest views','memory paging',  # 记录本次PS/2通路已执行的命令、切页和分页。
                       'Ctrl-L clear','history recall','backspace','shift letters/punctuation']  # 继续记录清屏、历史、退格和Shift观察。
            result={'profile':profile,'passed':True,'cases':cases,'seconds':round(time.monotonic()-started,2)}  # 各观察成立后才形成结果，耗时来自本次单调时钟。
            (work/'result.json').write_text(json.dumps(result,indent=2)+'\n')  # 结果编码为便于阅读的 JSON，并补文件末尾换行。
            print(f'PASS console/{profile} ({result["seconds"]}s)')  # 按实际路径与耗时报告本轮输入通过。
            return result  # 当前输入路径全部观察成功，返回实际case记录。
        finally:  # 成功返回与异常离开都进入收尾。
            if wire:  # 只有UART通路建立了设备socket。
                wire.close()  # 正常和失败都关闭本轮UART连接。
            process.terminate()  # 先请求主机 QEMU 进程结束。
            try:  # terminate只是请求结束，仍需收回进程。
                process.wait(timeout=5)  # 最多五秒等待正常退出。
            except subprocess.TimeoutExpired:  # QEMU没有在截止时间前退出。
                process.kill()  # 强制结束仍存活的本轮进程。
                process.wait()  # 收回已结束进程，不留下僵尸状态。
            qmp.unlink(missing_ok=True)   # 清理本轮QMP端点。
            uart.unlink(missing_ok=True)  # 清理UART端点，PS/2通路未创建也允许。
            endpoints.cleanup()  # 回收临时目录，不删除保留在 artifacts 的证据。
```

<!-- run -->
```sh
./airios test  # 真正构建全部配置，运行主机边界、五次启动和三种设备输入。
```

正常串口新增 `[HEAP_DEMO_OK] mapped_pages=16 bytes=65536 free_payload=65504 aligned=16 coalesced=1` 和 `[AIRI_P2_READY] exceptions=1 pages=1 own_cr3=1 heap=1`。运行 `./airios run` 后输入 mem，应看到 page pool 的 total/free/held 和 kernel heap 的 bytes/free_payload。held 包含长期页表与堆 backing，不等于用户对象使用量。

这里完整的修改演示已经在 guest 的三对象序列中完成：80与128分别不够208，相邻归还并合并后，同一地址能容纳208，仍活着的32字节对象不变。主机还实际执行内部地址、重复释放、溢出和耗尽等失败情况。调试可在 airi_heap_alloc 的分割处分步看 b、tail 与 next，在 airi_heap_free 合并后看块头数量减少；实际指针必须指向当前 guest 映射，不能拿宿主 arena 地址混用。

对应 Linux，malloc 返回对象与 mmap 建立映射同样处在不同层；free 一个对象也不保证 RSS 立即下降。本节没有实现完整 libc malloc、按需扩大/收缩堆、用户堆、并发分配器或物理页回收策略。P2 的核心内存链已经连通，Linux 对照接着观察“地址范围存在”和“实际触碰后驻留”之间的区别，再进入定时器、任务与用户态。
