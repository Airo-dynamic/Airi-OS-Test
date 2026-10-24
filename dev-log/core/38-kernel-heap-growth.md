# K38 · 给内核对象堆接入新页面，并区分缓存与活对象

从 K37 继续。用户已有 4 MiB 数据容量，但内核对象仍只能从 K17 的 64 KiB 固定范围中取得。较大的服务缓冲区会让 `airi_kmalloc` 失败。本节保留旧对象的地址与内容，空间不足时向页池申请新页面，映射在堆尾，再把新范围交给对象分配器。

初始和每批新增范围均为 64 KiB，最多十六批，即 1 MiB。对象 free 后空间回到堆，映射页面保留作为缓存。启动演示会实际增长到上限，所以本节启动后的 mem 显示 1 MiB；扩展机制本身只在申请找不到足够空闲块时增加页面。单核、内核不可抢占与 IRQ 不分配的约定保持，不引入并发分配器、自动收缩、slab 或完整 malloc ABI。

<a id="heap-extend-chain"></a>
## 尾部空闲块可以延长，活对象不能被扩容改变

K17 已将堆分成两层：memory.c 持有物理页和内核映射，heap.c 只在已映射范围中维护块头与 payload。现在仍由 memory.c 建立新页面，新增 `airi_heap_extend(heap, tail, bytes)` 只负责接入这些字节。调用者必须保证新范围可写且在整个借用期有效。

例如旧 backing 覆盖起点后的 65536 字节，tail 必须精确等于起点加 65536。若有地址洞，对象分配器把两侧误当连续后可能在洞里写块头；若 tail 落入旧范围，则会覆盖活对象。因此 extend 先检查旧右端、tail 相等、长度与16字节对齐和新右端溢出，全部成立后才读取并修改末块。

末块的两种状态产生不同结果：

- 末块空闲：直接增加它的 payload 容量，原来的块头继续有效，新页面不再占一个额外块头。
- 末块仍被借用：在新范围首部完整写好一个32字节空闲块头，再让旧末块指向它。旧对象的容量与返回地址保持，不能让调用者意外拥有新页面。

下面更新 `include/airi/heap.h`。新增的 used 查询只统计仍借出的 payload 容量，包含对齐后实际占有的字节，不含空闲空间与块头。它用来区分对象借用与已经映射但尚无对象使用的缓存。

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
bool airi_heap_extend(struct airi_heap *heap, void *tail, size_t bytes); // 调用者已映射紧邻尾部的新范围，失败不改变旧块链。
size_t airi_heap_used_bytes(const struct airi_heap *heap); // 只统计仍借出的payload容量，不含空闲缓存与块头。
void *airi_heap_alloc(struct airi_heap *heap, size_t bytes); // 返回 16 字节对齐的未初始化小对象，失败或零长度返回 NULL。
bool airi_heap_free(struct airi_heap *heap, void *pointer); // NULL 作为空操作，其他指针必须是当前借出的对象起点。
size_t airi_heap_free_bytes(const struct airi_heap *heap); // 空闲 payload 字节和，不含块头，不保证可以一次分配这么大。
#endif // 结束包含保护。
```

更新 `kernel/mm/heap.c`。在 init 与 alloc 之间加入 extend 和 used 查询，其余分割、首次适配、精确起点归还与合并逻辑保持 K17。所有扩展检查失败都不修改旧链，也不会访问传入的新范围；成功后，新范围已经属于块链，调用者不能再单独解除它的映射。

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
bool airi_heap_extend(struct airi_heap *heap, void *tail, size_t bytes) { // 只接入已经拥有且连续的新backing，不执行映射。
    if (!heap || !heap->first || !tail || bytes < sizeof(struct airi_heap_block) + 16 || bytes % 16 || heap->bytes > UINTPTR_MAX - (uintptr_t)heap->base) // 先核对对象与旧右端，再读取块链。
        return false; // 失败不修改已存在的对象或块头。
    uintptr_t end = (uintptr_t)heap->base + heap->bytes; // 上面的减法检查保证旧右端不回绕。
    if ((uintptr_t)tail != end || bytes > UINTPTR_MAX - end) // 新范围必须精确紧邻，不能有洞、覆盖旧对象或右端溢出。
        return false; // 此时尚未触碰调用者的新范围。
    struct airi_heap_block *last = heap->first; // 找到唯一一个没有后继的块。
    while (last->next) // 只沿已有受管理块，不读取新页面上的旧字节。
        last = last->next; // 链按地址连续排列，末块到达旧右端。
    if (last->free) // 空末块可直接延长，避免再付一个块头。
        last->bytes += bytes; // 原payload地址与所有活对象位置均不改变。
    else { // 旧末块仍被对象借用，不能把新增容量偷偷并入该对象。
        struct airi_heap_block *added = tail; // 新块头恰在旧backing右端。
        *added = (struct airi_heap_block){bytes - sizeof(*added), NULL, true}; // 完整初始化后才接入旧链。
        last->next = added; // 发布新的空闲末块，旧对象容量保持。
    }
    heap->bytes += bytes; // 块链准备好以后才发布完整backing容量。
    return true; // 新范围此后归堆管理，调用者不能单独解除映射。
}
size_t airi_heap_used_bytes(const struct airi_heap *heap) { // 资源核对只观察活payload，不把空闲缓存算成对象泄漏。
    size_t bytes = 0; // 空堆与无活对象都为零。
    if (heap) // 尚未初始化的查询同样安全。
        for (const struct airi_heap_block *block = heap->first; block; block = block->next) // 不改变块状态。
            if (!block->free) // 已归还的payload不再有借用者。
                bytes += block->bytes; // 包含对象实际取得的对齐容量，不是原请求长度。
    return bytes; // 元数据块头与物理backing另有所有者。
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

<a id="heap-cache-usage"></a>
## 页面从页池转入堆，并不表示多了一个活对象

旧的会话检查要求前后 `pages_free` 和 `heap_free` 都完全相等。现在一次临时大对象可能让堆从64 KiB增长到128 KiB：即使对象已经free，页池仍少十六页，而堆的空闲payload增加。这符合保留缓存的策略，却会被旧检查误报为泄漏。

快照新增 heap_used，单独保存活payload。新的 `airi_memory_same_usage` 在当前固定布局下同时核对两项：

1. `pages_free + heap_bytes / 4096` 保持，表示堆增长所占的数据页恰好从页池转入；其他地方丢失的页不能被忽略。
2. heap_used 保持，表示会话没有留下额外活对象；堆变大不能抵消对象未归还。

这个核对有明确范围。堆基址按2 MiB对齐，1 MiB上限始终落在第一批已经建立的同一张叶子页表内，扩展不会再增加页表页。若以后将堆扩大到别的叶子表，新增页表本身也需要单独统计，不能继续只加数据页。它也只检查净资源数量，不识别对象引用是否指向正确身份。

更新 `include/airi/memory.h`。`static inline` 让各使用者取得同一小函数的定义，不需要额外链接一个实现符号；这里的inline是编译提示，函数语义仍是普通C调用。

<!-- write: include/airi/memory.h -->
```c
#ifndef AIRI_MEMORY_H // 通用内存服务与只读数据快照。
#define AIRI_MEMORY_H // 包含保护。
#include <airi/boot.h> // 启动合同和固定宽整数。
#include <stddef.h> // 小对象请求按字节计数。
struct airi_memory_stats { // 消费者只得到数值，不持有分配器内部块指针。
    uint32_t pages_total, pages_free; // 实际受管理物理页与当前未借出页数。
    size_t heap_bytes, heap_free, heap_used; // 已映射backing、空闲payload与活对象payload分别统计。
};
void airi_memory_init(const AiriBootInfo *info); // 顺序初始化页池、页表和可扩展内核堆。
void airi_memory_snapshot(struct airi_memory_stats *stats); // 同步复制当前数值到调用者对象。
static inline bool airi_memory_same_usage(const struct airi_memory_stats *before, const struct airi_memory_stats *after) { // 每个包含者使用这段小函数，无独立链接符号。
    uint64_t old_available = before->pages_free + before->heap_bytes / 4096; // 堆的缓存数据页从页池转出，但没有外部借用。
    uint64_t new_available = after->pages_free + after->heap_bytes / 4096; // 当前1MiB堆始终使用初始已建的同一张叶子表。
    return old_available == new_available && before->heap_used == after->heap_used; // 新增活对象或堆以外的页泄漏都不能被缓存抵消。
}
void *airi_kmalloc(size_t bytes); // 对象空间不足时按64KiB批次扩展，最大1MiB，不承诺完整libc malloc ABI。
bool airi_kfree(void *pointer); // 结束对象借用，backing 页仍由内核堆持有。
#define AIRI_REGION_PAGES 16u // 当前有界内核区域最多持有十六个数据页。
struct airi_kernel_region { // 所有权句柄必须在使用前清零，不能按值复制后重复释放。
    uint64_t virtual_address; // 所有数据页连续映射的内核虚拟起点。
    uint64_t physical[AIRI_REGION_PAGES]; // 逐页记录实际借出的物理页，便于精确回滚。
    unsigned count; // 只有已经成功映射的页才计入所有权。
};
bool airi_memory_region_create(struct airi_kernel_region *region, uint64_t address, unsigned count); // 建立新的有界RW/NX内核区域，不覆盖已有映射。
bool airi_memory_region_free(struct airi_kernel_region *region); // 所有借用结束后先解除映射，再归还数据页。
struct airi_address_space; // 内核内部可持有地址空间，不向UI交出页表。
bool airi_memory_user_space(struct airi_address_space *space); // 从长期内核根借用高半区。
void airi_memory_use_kernel(void); // 返回启动内核地址空间后才能销毁用户空间。
uint64_t airi_memory_take_page(void); // 内核对象明确持有一个物理数据页。
bool airi_memory_return_page(uint64_t physical); // 映射和借用结束后归还。
void *airi_memory_direct(uint64_t physical); // 当前受支持RAM的HHDM别名，不创建新映射。
#endif // 结束包含保护。
```

<a id="heap-grow-mapping"></a>
## 完整映射新批次以后，才接入块链

memory.c 使用已经存在的 `airi_kernel_region` 保存每批十六个物理页。十六个区域句柄放在静态BSS中，heap_chunks只记录已经完成接入的批次；为堆申请元数据时不反过来调用尚在扩展的堆。新批次的地址由固定基址加已提交字节数算出，正好接在旧尾部。

`heap_grow` 先调用区域接口。该接口预检范围，再逐页申请、映射、登记；中途失败会先解除映射再归还数据页，句柄清零。整批成功后范围已经RW/NX映射并清零，此时才能调用 heap_extend。若对象层仍拒绝，新批尚未借给任何对象，heap_grow可解除该区域，旧堆保持。只有两层都成功才增加heap_chunks并发布HEAP_GROW。

`airi_kmalloc` 首先尝试旧空闲块。找不到时增长一批再试，最多十六批；上限、页池耗尽或映射失败返回NULL，旧对象仍有效。零长度、SIZE_MAX和大于整个堆最大payload的请求在增长前拒绝。用户数据页及其记录页仍按K37的独立所有权管理，不从这个对象堆取得。

更新 `kernel/mm/memory.c`：替换固定堆构造与kmalloc，快照增加heap_used；页池、自有页表与区域接口的原逻辑保留。小对象演示的“本批容不下”检查直接调用底层heap_alloc，因为它只验证旧64 KiB范围，不能触发自动扩展。随后增加完整的大对象演示。

<!-- write: kernel/mm/memory.c -->
```c
#include <airi/memory.h> // 内存服务对消费者只交出快照和对象。
#include <airi/heap.h> // 页面映射与对象块链分别维护，扩展后保留活对象。
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
#define HEAP_STEP_PAGES 16u // 初始与每次扩展都为64KiB。
#define HEAP_MAX_CHUNKS 16u // 总backing最多1MiB，仍位于同一2MiB叶子表覆盖内。
static struct airi_heap heap; // backing持有者和对象块链在整个内核期存在。
static struct airi_kernel_region heap_regions[HEAP_MAX_CHUNKS]; // 静态句柄记录每批物理页，不向尚未建立的堆递归申请元数据。
static unsigned heap_chunks; // 只有完整映射并接入块链的批次才计入。
static bool heap_grow(void) { // 单核不可抢占普通上下文调用，IRQ不进入分配器。
    if (heap_chunks == HEAP_MAX_CHUNKS) // 明确容量上限，不能越界写句柄或扩展到别的页表。
        return false; // 当前活对象与空闲块完全保持。
    struct airi_kernel_region *region = &heap_regions[heap_chunks]; // 未发布槽必须仍是空句柄。
    uint64_t address = HEAP_ADDRESS + heap_chunks * HEAP_STEP_PAGES * 4096ull; // 精确紧邻旧backing右端。
    if (!airi_memory_region_create(region, address, HEAP_STEP_PAGES)) // 整批映射失败由区域接口回滚，不提供半个新范围。
        return false; // heap的块链与字节容量还没有改变。
    void *tail = (void *)(uintptr_t)address; // 完整RW/NX映射已建立并清零，才能构造块头。
    bool accepted = heap_chunks ? airi_heap_extend(&heap, tail, HEAP_STEP_PAGES * 4096u) : airi_heap_init(&heap, tail, HEAP_STEP_PAGES * 4096u); // 第一批初始化，其余批只接在尾部。
    if (!accepted) { // 对象分配器拒绝接入时，它保证旧链未修改。
        if (!airi_memory_region_free(region)) // 新批还没交给任何对象，可完整解除和归还。
            airi_panic("cannot roll back kernel heap extension"); // 内部所有权错误不能吞掉。
        return false; // 原堆仍可继续分配和归还。
    }
    ++heap_chunks; // 从这里起句柄与块链共同拥有此批，直到内核生命周期结束。
    airi_log("[HEAP_GROW] mapped_pages=%u bytes=%zu", heap_chunks * HEAP_STEP_PAGES, heap.bytes); // 只有完整提交才报告容量。
    return true; // backing缓存不会随单个对象free自动缩小。
}
static void heap_init(void) { // 所需页与页表都由当前内核拥有。
    if (!heap_grow()) // 第一批也遵守映射成功后才接入的顺序。
        airi_panic("cannot initialize kernel heap"); // 启动必须有最小对象容量。
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
    if (joined != a || c[31] != 0x5a || airi_kmalloc(SIZE_MAX) || airi_kmalloc(0) || airi_heap_alloc(&heap, HEAP_STEP_PAGES * AIRI_PAGE_SIZE)) // 检查合并、不覆盖活对象、溢出、零长度和耗尽。
        airi_panic("kernel heap split/coalesce failed"); // 不把分配失败伪装成有效指针。
    if (!airi_kfree(joined) || !airi_kfree(c) || airi_heap_free_bytes(&heap) != before) // 所有对象归还后应恢复单个空闲块容量。
        airi_panic("kernel heap accounting did not recover"); // backing仍持有，只有对象借用结束。
    airi_log("[HEAP_DEMO_OK] mapped_pages=16 bytes=%zu free_payload=%zu aligned=16 coalesced=1", heap.bytes, before); // 实际分配、归还和失败演示结束。
    struct airi_memory_stats baseline, grown; // 允许缓存增长，但结束时活payload与堆外页面必须恢复。
    airi_memory_snapshot(&baseline); // 初始堆仅64KiB且所有演示对象已归还。
    uint8_t *guard = airi_kmalloc(32); // 跨扩展保留一个活对象，不能移动或覆盖。
    if (!guard) // 尚未取得活对象时不能写入。
        airi_panic("cannot allocate heap growth guard"); // 普通耗尽保持明确。
    airi_memset(guard, 0x5a, 32); // 扩展前留下旧对象内容，随后仍须保持。
    uint8_t *large = airi_kmalloc(1024u * 1024 - 256); // 迫使按批扩展直到1MiB，而非返回首批外的无映射指针。
    if (!guard || !large) // 已有页资源不足就明确停止演示。
        airi_panic("cannot grow bounded kernel heap"); // 不访问失败指针。
    airi_memset(large, 0xa6, 1024u * 1024 - 256); // 实际写遍新映射，证明它们可访问。
    void *last = airi_kmalloc(128); // 吃掉最后空闲payload，堆达到明确边界。
    if (!last || airi_kmalloc(16) || airi_kmalloc(SIZE_MAX) || guard[31] != 0x5a || large[1024u * 1024 - 257] != 0xa6) // 超限不扩展到相邻未拥有范围，活内容保持。
        airi_panic("kernel heap growth limit or live data failed"); // 不把NULL以外的越界结果当成功。
    if (!airi_kfree(last) || !airi_kfree(large) || !airi_kfree(guard) || airi_heap_used_bytes(&heap)) // 所有临时对象归还，backing仍缓存1MiB。
        airi_panic("kernel heap growth leaked live payload"); // 不把持有页面与活对象混算。
    airi_memory_snapshot(&grown); // 新堆空闲payload比原来大，但外部借用没有增加。
    if (!airi_memory_same_usage(&baseline, &grown) || grown.heap_bytes != 1024u * 1024 || grown.heap_free != grown.heap_bytes - 32) // 缓存增长精确对应新增数据页，尾块完全合并。
        airi_panic("kernel heap cache accounting failed"); // 页或对象泄漏仍需报告。
    airi_log("[HEAP_CAPACITY_OK] initial=65536 bytes=%zu live=%zu chunks=%u preserved=1 limit=null", grown.heap_bytes, grown.heap_used, heap_chunks); // 真实映射、写入、耗尽和归还完成。

}
void *airi_kmalloc(size_t bytes) { // 内核小对象入口，不引入宿主libc。
    if (!heap.first || !bytes || bytes > HEAP_MAX_CHUNKS * HEAP_STEP_PAGES * 4096u - 32) // 未初始化、空请求或绝不可能容纳的对象不触发增长。
        return NULL; // 也在向上对齐前排除SIZE_MAX等溢出输入。
    for (;;) { // 每次失败最多增加一个64KiB批次，总共只有十六批。
        void *object = airi_heap_alloc(&heap, bytes); // 先利用已有空闲块，不为可重用空间增加页面。
        if (object) // 成功返回的对象可能来自旧空洞或新尾部。
            return object; // 已有对象从未移动。
        if (!heap_grow()) // 上限或物理/映射失败都只影响本次新请求。
            return NULL; // 调用者保留所有旧借用及其内容。
    }
}
bool airi_kfree(void *pointer) { // 对象所有者结束最后一个借用后调用。
    return airi_heap_free(&heap, pointer); // 对象空间回堆，物理页不因此归还。
}
void airi_memory_snapshot(struct airi_memory_stats *stats) { // 目前单核不可抢占，可同步复制一致数值。
    stats->pages_total = pages.total; // 固定受管理物理页容量。
    stats->pages_free = pages.free; // 减去页表和堆backing等当前所有者。
    stats->heap_bytes = heap.bytes; // 已映射的总backing字节。
    stats->heap_used = airi_heap_used_bytes(&heap); // 真实活payload与缓存页面分别报告。
    stats->heap_free = airi_heap_free_bytes(&heap); // 空闲对象payload，不包含块头。
}

bool airi_memory_region_create(struct airi_kernel_region *region, uint64_t address, unsigned count) { // 调用者清零句柄并保证没有并发操作这个对象。
    if (!region || region->count || !count || count > AIRI_REGION_PAGES || address % AIRI_PAGE_SIZE || address < 0xffff800000000000ull || address > UINT64_MAX - count * AIRI_PAGE_SIZE) // 只接受新的有界内核范围。
        return false; // 参数错误不改变任何页或映射。
    for (unsigned i = 0; i < count; ++i) // 先验证整个范围没有被其他服务映射。
        if (airi_vm_lookup(&kernel_space, address + i * AIRI_PAGE_SIZE, NULL, NULL)) // 覆盖已有对象会破坏所有权。
            return false; // 尚未借出页面，可以原样拒绝。
    region->virtual_address = address; // 保存本次区域的虚拟位置。
    while (region->count < count) { // 每成功一页才增加持有数量。
        uint64_t physical = airi_page_alloc(&pages); // 数据页的所有权先归本次创建操作。
        if (!physical) // 页池耗尽，没有本次页可归还。
            break; // 统一回滚已经成功的页面。
        if (!airi_vm_map(&kernel_space, address + region->count * AIRI_PAGE_SIZE, physical, AIRI_VM_WRITE)) { // 映射为supervisor读写NX。
            airi_page_free(&pages, physical); // 本次数据页未接入，立即归还。
            break; // 之前已经接入的页面由统一出口解除。
        }
        region->physical[region->count++] = physical; // 记录物理地址后才增加有效数量。
    }
    if (region->count != count) { // 不提供只建成一部分的区域。
        if (region->count && !airi_memory_region_free(region)) // 只有实际持有页时才回滚；内部映射必须仍匹配句柄。
            airi_panic("cannot roll back partial kernel region"); // 不清掉无法归还的所有权记录后假装无泄漏。
        airi_memset(region, 0, sizeof(*region)); // 失败句柄恢复无所有权状态。
        return false; // 调用者没有取得可访问区域。
    }
    airi_memset((void *)(uintptr_t)address, 0, count * AIRI_PAGE_SIZE); // 完整映射后清除旧页面内容。
    return true; // 句柄拥有数据页及对应映射，不能复制出第二个释放者。
}
bool airi_memory_region_free(struct airi_kernel_region *region) { // 调用前必须离开该区域，包括不能还在它的任务栈上执行。
    if (!region || !region->count || region->count > AIRI_REGION_PAGES) // 空句柄和损坏容量不能参与回收。
        return false; // 不猜测曾经持有的页面。
    for (unsigned i = 0; i < region->count; ++i) { // 先完整验证所有映射仍对应句柄记录。
        uint64_t physical; // 保存软件翻译结果。
        if (!airi_vm_lookup(&kernel_space, region->virtual_address + i * AIRI_PAGE_SIZE, &physical, NULL) || physical != region->physical[i]) // 不归还已被别处替换的范围。
            return false; // 验证失败不拆任何映射。
    }
    while (region->count) { // 由末尾逆序解除映射。
        --region->count; // 当前槽位仍有真实页面所有权。
        if (!airi_vm_unmap(&kernel_space, region->virtual_address + region->count * AIRI_PAGE_SIZE) || !airi_page_free(&pages, region->physical[region->count])) // 先断链接和失效，之后才归还数据页。
            airi_panic("kernel region ownership changed during release"); // 内部不变量破坏时不继续双重释放。
    }
    airi_memset(region, 0, sizeof(*region)); // 成功后句柄不能再次释放同一组页面。
    return true; // 当前没有遗留的数据页借用。
}

bool airi_memory_user_space(struct airi_address_space *space) { // 调用时所需内核栈映射已在长期根中存在。
    return airi_vm_clone_kernel(space, &kernel_space); // 用户空间只拥有新根和低半区表。
}
void airi_memory_use_kernel(void) { // 用户退出后先切回长期根。
    if (!airi_vm_activate(&kernel_space)) // 同一固定CPU应支持先前已经启用的配置。
        airi_panic("cannot restore kernel address space"); // 未切离不能继续回收用户根。
}
uint64_t airi_memory_take_page(void) { // 物理页生命周期由内核调用者显式管理。
    return airi_page_alloc(&pages); // 失败保留0哨兵。
}
bool airi_memory_return_page(uint64_t physical) { // 不代替调用者解除仍存活映射。
    return airi_page_free(&pages, physical); // 保留对齐和重复归还检查。
}
void *airi_memory_direct(uint64_t physical) { // 仅供已拥有低RAM页的内部调用者使用。
    if (physical >= AIRI_PHYSICAL_LIMIT) // 不把任意MMIO或超上限地址当普通RAM。
        airi_panic("physical address outside direct RAM policy"); // 具体错误而非生成无效指针。
    return (void *)(uintptr_t)(kernel_space.direct_offset + physical); // 页内偏移保留不变。
}
```

大对象演示先在旧堆保留一个32字节guard并写0x5a，再申请1048320字节，即1 MiB减256字节。分配器必须真实映射新页，才能逐字节写入0xa6。此时已有两个活块头及一个末尾空闲块头，最后128字节payload仍可申请；再申请16字节必须失败。旧guard与大对象末字节继续保持。

三个对象归还后，相邻块跨批次合并，只剩一个32字节初始块头，空闲payload为1048544字节，heap_used为零。新的资源核对允许缓存从十六页增长到256页，但不允许丢失其他页面。这里没有人为制造整机物理页耗尽；部分映射失败的回滚沿用已有区域接口，实际演示的失败是堆容量边界。

<a id="heap-grow-callers"></a>
## 会话收尾采用同一套资源规则

在 `kernel/task/task.c` 收尾处，将两项旧字段比较替换为 `!airi_memory_same_usage(&before, &after)`。任务仍必须离开目标栈后再归还栈与页表；新核对不放宽这些页面的释放条件。

在 `kernel/process/user.c` 的 launch 中也有三处需要替换：初始映像失败、异步owner创建失败，以及完整用户会话收尾。每处保留原有FD计数条件。它们共用同一规则，避免某条错误路径仍把缓存增长当作泄漏。除此以外，K37的页记录、ELF、fork/exec/wait和输入监督保持。下面是这两个文件的完整终点，已有文件只修改上述条件。

<details>
<summary>kernel/task/task.c 的完整终点</summary>

<!-- write: kernel/task/task.c -->
```c
#include <airi/task.h> // 主动切换和完整演示入口。
#include <airi/memory.h> // 每个任务栈拥有自己的映射区域。
#include <airi/kernel.h> // 日志与停止接口。
#define TASKS 3u // 一个启动任务，加两个演示任务。
#define STACK_BASE 0xffffe00000000000ull // 任务栈使用独立高地址分支。
#define STACK_PAGES 4u // 每个任务16KiB，前面额外留一页不映射。
enum task_state { UNUSED, RUNNABLE, RUNNING, DONE }; // 每个槽的生命周期状态。
struct task { // 全部状态在内核静态数组，任务不会持有局部控制块。
    uint64_t rsp; // 普通C调用被暂停时保存的栈指针。
    struct airi_kernel_region stack; // 只由此任务槽持有的backing与映射。
    void (*entry)(void); // 第一次被调度时执行的普通C函数。
    enum task_state state; // 只有RUNNABLE槽可以被调度器选中。
};
static struct task tasks[TASKS]; // 槽0借用原启动栈，不创建或释放它。
static unsigned current, trace_count, trace[6]; // 当前任务编号和实际交错顺序。
extern void airi_context_switch(uint64_t *old_rsp, uint64_t new_rsp); // 汇编只保存普通C ABI规定的被调用者寄存器。
extern _Noreturn void airi_stack_guard_fault(uint64_t guard_top); // 作者故障入口不能返回正常栈。
void airi_task_yield(void) { // 只有正常内核调用主动进入，不从时钟IRQ调用。
    unsigned old = current; // 这个局部值随旧任务自己的栈/保留寄存器一起暂停。
    if (tasks[old].state == RUNNING) // 正在结束的DONE任务不能再成为可运行。
        tasks[old].state = RUNNABLE; // 普通yield仍允许未来被选中。
    unsigned next = old; // 暂未找到其他候选。
    for (unsigned n = 1; n <= TASKS; ++n) { // 最多查一圈，包含必要时选择原任务。
        unsigned candidate = (old + n) % TASKS; // 轮流选择下一个槽。
        if (tasks[candidate].state == RUNNABLE) { // UNUSED和DONE不能被错误唤醒。
            next = candidate; // 保存找到的可运行任务。
            break; // 首个候选实现本节round-robin顺序。
        }
    }
    if (tasks[next].state != RUNNABLE) // 当前已DONE且其他任务也不可运行时，不存在合法返回栈。
        airi_panic("task scheduler has no runnable continuation"); // 不用已结束的调用链继续执行。
    tasks[next].state = RUNNING; // 在转交栈前记录目标状态。
    current = next; // 目标trampoline或恢复点会读取这个编号。
    if (next != old) // 没有其他任务时无需保存并恢复同一个栈。
        airi_context_switch(&tasks[old].rsp, tasks[next].rsp); // ret将在目标栈的调用链中继续。
    if (current != old) // 只有旧任务真正被重新选中才可能从它的switch调用返回。
        airi_panic("task resumed with mismatched owner"); // 明确拒绝混用任务栈与控制块。
}
static _Noreturn void trampoline(void) { // 首次任务不是从普通call进入，而由预制栈的ret进入。
    tasks[current].entry(); // 运行这个槽持有的普通C函数。
    tasks[current].state = DONE; // 函数返回表示任务结束，不能再参与候选。
    airi_task_yield(); // 切到另一个仍存活的栈，启动任务稍后负责回收。
    airi_panic("completed task unexpectedly resumed"); // DONE任务若再次被选中，明确失败。
}
static bool spawn(unsigned id, void (*entry)(void)) { // 由启动任务在首次调度前建立全部状态。
    uint64_t address = STACK_BASE + id * (STACK_PAGES + 1) * 4096ull + 4096; // 每个槽前面一页保持未映射保护。
    if (id == 0 || id >= TASKS || !entry || tasks[id].state != UNUSED) // 不覆盖启动任务或仍被占用的槽。
        return false; // 不创建任何新栈。
    if (!airi_memory_region_create(&tasks[id].stack, address, STACK_PAGES)) // 四页虚拟连续，物理可以分散。
        return false; // 区域接口已经回滚半成品。
    uint64_t *sp = (uint64_t *)(uintptr_t)(address + STACK_PAGES * 4096ull); // 栈从已映射范围的高端向下生长。
    *--sp = 0; // 模拟C函数的调用者返回槽；trampoline承诺不返回。
    *--sp = (uintptr_t)trampoline; // 六次pop之后的ret会取出这个首次入口。
    for (unsigned i = 0; i < 6; ++i) // RBP、RBX、R12–R15首次没有旧值。
        *--sp = 0; // 预制出与switch保存部分相同的六个槽。
    tasks[id].rsp = (uintptr_t)sp; // 指向第一个即将pop的R15槽。
    tasks[id].entry = entry; // 控制块拥有函数身份，参数本节由current编号区分。
    tasks[id].state = RUNNABLE; // 全部栈内容完成后才允许被调度器发现。
    return true; // 此时尚未运行任务任何一条指令。
}
static void worker(void) { // 两个槽使用同一段代码，但各自有独立调用链和局部变量。
    unsigned id = current; // 本函数暂停后应仍保持这个身份。
    volatile uint64_t local = id * 100 + 7; // 强制局部值留在当前任务栈，证明不是共享一个局部对象。
    for (unsigned round = 0; round < 3; ++round) { // 每个任务输出三次并主动让出。
        if (trace_count >= 6) // 防止错误调度使轨迹数组越界。
            airi_panic("task trace overflow"); // 具体错误停止而不是覆盖控制块。
        trace[trace_count++] = id; // 保存实际执行身份后增加有效轨迹数。
        airi_log("[TASK_STEP] id=%u round=%u stack_local=%llu", id, round, (unsigned long long)local); // 输出当前独立栈的局部值。
        airi_task_yield(); // 保存当前调用链，再从别的任务栈返回。
        if (current != id || local != id * 100 + 7) // 恢复必须保持身份与栈上内容。
            airi_panic("task stack local changed across yield"); // 不能只凭日志交错宣布切换成功。
    }
}
void airi_task_demo(void) { // 启动任务收拢运行和回收，后续仍回串口循环。
    struct airi_memory_stats before, after; // 用页池与堆快照检查任务资源是否回收。
    airi_memory_snapshot(&before); // 创建任务栈之前的基准。
    tasks[0].state = RUNNING; // 当前C调用链属于槽0的原始启动栈。
    if (!spawn(1, worker) || !spawn(2, worker)) // 两个任务都必须拥有完整独立栈。
        airi_panic("cannot create kernel task stacks"); // 不运行部分准备的演示。
#ifdef AIRI_TEST_STACK_GUARD // 独立故障镜像完整演示坏栈上的DF接管。
    airi_log("[STACK_GUARD_PROBE] top=0x%llx ist=1", (unsigned long long)tasks[1].stack.virtual_address); // 保护页上边界即栈映射起点。
    airi_stack_guard_fault(tasks[1].stack.virtual_address); // 正常栈被故意丢弃，必须由DF的独立栈报告。
#endif // 普通镜像继续真正的任务交错。
    while (tasks[1].state != DONE || tasks[2].state != DONE) // 有任务尚未结束就继续主动交接。
        airi_task_yield(); // 启动任务本身也参与轮转，为最终回收保留活调用链。
    for (unsigned i = 0; i < 6; ++i) // 检查实际轨迹，不只要求输出六行。
        if (trace_count != 6 || trace[i] != 1 + i % 2) // 期望两个独立栈按1、2、1、2、1、2继续。
            airi_panic("kernel tasks did not alternate"); // 状态机或上下文错位时拒绝完成。
    for (unsigned id = 1; id < TASKS; ++id) // 当前已回槽0，两个目标栈均不再执行。
        if (!airi_memory_region_free(&tasks[id].stack)) // 先解除映射和空页表，再归还数据页。
            airi_panic("cannot reclaim completed task stack"); // 不保留隐藏泄漏。
    airi_memory_snapshot(&after); // 回收后的当前实际统计。
    if (!airi_memory_same_usage(&before, &after)) // 任务栈和临时页表必须全部恢复。
        airi_panic("completed kernel tasks leaked memory"); // 内核演示不应吃掉后续主线的容量。
    airi_log("[TASK_DEMO_OK] order=121212 separate_stacks=2 reclaimed=1"); // 只有切换、局部值、顺序与回收全部成立才输出。
}
```

</details>

<details>
<summary>kernel/process/user.c 的完整终点</summary>

<!-- write: kernel/process/user.c -->
```c
#include <airi/async.h> // 进程轮次与固定缓冲资源的生命周期。
#include <airi/arch.h> // CPU空闲与用户调度门留在架构适配。
#include <airi/storage.h> // 用户存储请求经过有界副本进入日志服务。
#include <airi/file.h> // 每进程FD表共享打开文件对象。
#include <airi/console.h> // 内核命令消费服务结果，不依赖主题和绘图。
#include <airi/elf.h> // 独立用户ELF检查器。
#include <airi/user.h> // 完整陷入与安全用户返回。
#include <airi/memory.h> // 页面、内核栈与长期根的所有权。
#include <airi/vm.h> // 每个进程有独立低半区。
#include <airi/timer.h> // 用户时间片依赖真实IRQ0。
#include <airi/kernel.h> // 有界串口输出与故障报告。
#define USER_CODE 0x400000ull // 旧受控映像的用户入口。
#define USER_STACK 0x70000000ull // 初始栈最高一页保持原地址，新增页向下展开。
#define STACK_BASE 0xffffe10000000000ull // 所有进程陷入栈位于同一长期内核分支。
#define USER_HEAP 0x10000000ull // ELF静态范围之外的用户页分配起点。
#define USER_HEAP_END 0x20000000ull // 动态用户页只占这段独立范围，不接近初始用户栈。
#define PROCESS_COUNT 8u // 有界进程表，不做动态扩容。
enum user_state { USER_UNUSED, USER_RUNNABLE, USER_RUNNING, USER_WAITING, USER_IO_WAIT, USER_ZOMBIE, USER_REAP_PENDING }; // 退出与回收是两个不同状态。
enum io_operation { IO_NONE, IO_READ, IO_WRITE, IO_READY, IO_ASYNC, IO_SLEEP }; // 等待保存操作条件，不保留内核局部buffer。
struct user_page { uint64_t address, physical; unsigned flags; }; // fork需要把同一VA映射到另一个物理页。
#define PAGE_RECORDS (4096u / sizeof(struct user_page)) // 一物理页放170项，余下16字节不用。
#define RECORD_CHUNKS ((AIRI_USER_PAGES + PAGE_RECORDS - 1) / PAGE_RECORDS) // 1024项最多借七个记录页。
struct user_process { // 所有权和调度状态由内核持有，用户只能看到PID。
    bool async_owned; // 本控制块是否持有当前PID的异步owner，局部旧exec副本必须清除此所有权。
    struct airi_fd_table files; // exec转移表项，fork增加同一打开对象的引用。
    struct airi_address_space space; // 根和低半区表自有，高半区借用。
    struct airi_kernel_region stack; // 不能在仍使用它的C调用中释放。
    uint64_t records[RECORD_CHUNKS]; // 元数据页的物理地址；各页经HHDM访问，不映射给用户。
    unsigned count, pid, parent, user_ticks; // 资源数量、身份、父PID和本进程实际IRQ数。
    uint64_t fault_error; // 用户故障证据；动态页位置由现有映射寻找，不再使用单向游标。
    int exit_code; // 自有wait接口返回普通退出码或负故障编码。
    enum user_state state; // 只有RUNNABLE可被调度器选中。
    int64_t wait_target; // 正PID表示指定孩子，-1表示任意孩子。
    uint64_t wait_status; // 已验证的父进程用户输出地址，阻塞期间父进程不会修改映射。
    enum io_operation io; // 只有USER_IO_WAIT消费下面的请求副本。
    unsigned io_fd, io_events; // FD属于本进程表，events只用于就绪等待。
    uint64_t io_address, io_bytes, io_deadline; // 用户页在阻塞期间仍由进程拥有，deadline按真实tick。
    struct airi_trap_frame initial; // 尚未运行时的寄存器值，不依赖临时C对象。
    struct airi_trap_frame *frame; // 运行后指向本进程内核栈的实际现场。
};
_Static_assert(sizeof(struct user_process) <= 1024, "exec control block must stay small"); // exec的新旧局部副本不能随用户容量膨胀。
static struct user_process processes[PROCESS_COUNT]; // 槽可以复用，PID单调增长，不等于槽下标。
static struct user_process *current; // 只在完整陷入边界改变当前身份。
static unsigned next_pid = 1, root_pid, timer_switches, wait_blocks; // 每次运行会话的根身份和实际行为计数。
static unsigned io_block_count, io_wake_count, idle_halt_count; // 会话真实I/O阻塞与唤醒次数。
static bool session_idle, service_from_timer; // 是否暂回启动栈服务，和整个会话结束分开。
static unsigned supervisor_pid, input_pid; // 可选init身份与当前唯一UART输入身份，不是进程槽。
static bool session_active; // 只有用户会话活动时才接受用户IRQ或系统调用。
static int session_result; // 根用户退出结果，不用最后一个孩子的结果覆盖。
static struct airi_trap_frame *kernel_return; // 启动栈一直保留到整个用户会话结束。
extern const uint8_t airi_user_probe_start[], airi_user_probe_end[]; // 受控正常映像。
extern const uint8_t airi_user_bad_start[], airi_user_bad_end[]; // 用户cli故障。
extern const uint8_t airi_user_load_bad_start[], airi_user_load_bad_end[]; // 用户读supervisor页故障。
extern const uint8_t airi_user_rsp_bad_start[], airi_user_rsp_bad_end[]; // 不规范用户返回栈。
extern const uint8_t airi_hello_elf_start[], airi_hello_elf_end[]; // 既有独立用户C程序。
extern const uint8_t airi_process_elf_start[], airi_process_elf_end[]; // 新的fork/wait和抢占工作负载。
extern const uint8_t airi_init_elf_start[], airi_init_elf_end[]; // 用户态常驻监督程序。
extern const uint8_t airi_capacity_elf_start[], airi_capacity_elf_end[]; // 大BSS、深用户栈与跨记录页的fork/exec。
extern const uint8_t airi_memory_elf_start[], airi_memory_elf_end[]; // 用户页面归还与空洞复用工作负载。
extern const uint8_t airi_orphans_elf_start[], airi_orphans_elf_end[]; // 持续会话中的孤儿回收与槽复用演示。
extern const uint8_t airi_shell_elf_start[], airi_shell_elf_end[]; // 真正用户shell，不是内核命令别名。
extern const uint8_t airi_demo_elf_start[], airi_demo_elf_end[]; // 用户页分配与异步存储贯通演示。
extern const uint8_t airi_async_elf_start[], airi_async_elf_end[]; // 用户注册、CQ等待与exec资源轮次。
extern const uint8_t airi_waits_elf_start[], airi_waits_elf_end[]; // 真正阻塞与超时工作负载。
extern const uint8_t airi_pipes_elf_start[], airi_pipes_elf_end[]; // 非阻塞管道用户程序。
extern const uint8_t airi_storage_elf_start[], airi_storage_elf_end[]; // 写入和恢复共用一份明确分支的用户ELF。
extern const uint8_t airi_files_elf_start[], airi_files_elf_end[]; // 内存文件用户工作负载。
extern const uint8_t airi_worker_elf_start[], airi_worker_elf_end[]; // exec目标是另一个独立静态ELF。
extern const uint8_t airi_broken_elf_start[], airi_broken_elf_end[]; // 受控损坏输入，验证旧进程保持。
struct executable { const char *name; const uint8_t *start, *end; }; // 初始映像目录尚不是通用文件系统。
static const struct executable executables[] = { // 每份映像字节由内核只读段长期持有。
    {"sh", airi_shell_elf_start, airi_shell_elf_end}, // 独占串口输入的可选用户程序。
    {"init", airi_init_elf_start, airi_init_elf_end}, // 管理前台shell的常驻用户进程。
    {"capacity", airi_capacity_elf_start, airi_capacity_elf_end}, // 预算耗尽后回收并继续运行。
    {"memory", airi_memory_elf_start, airi_memory_elf_end}, // 同一地址空间内释放与重新申请。
    {"orphans", airi_orphans_elf_start, airi_orphans_elf_end}, // 不结束根会话也能回收退出的孤儿。
    {"demo", airi_demo_elf_start, airi_demo_elf_end}, // 页、任务和设备完成串联。
    {"hello", airi_hello_elf_start, airi_hello_elf_end}, // 独立C运行库与BSS观察。
    {"processes", airi_process_elf_start, airi_process_elf_end}, // fork/exec/wait实际工作负载。
    {"worker", airi_worker_elf_start, airi_worker_elf_end},
    {"async", airi_async_elf_start, airi_async_elf_end}, // 完整用户提交/完成路径。
    {"async-reset", airi_async_elf_start, airi_async_elf_end}, // 同一PID新epoch的验证入口。
    {"waits", airi_waits_elf_start, airi_waits_elf_end}, // 阻塞流和就绪条件等待。
    {"pipes", airi_pipes_elf_start, airi_pipes_elf_end}, // 有界FIFO与关闭语义。
    {"storage", airi_storage_elf_start, airi_storage_elf_end}, // 显式提交记录。
    {"recover", airi_storage_elf_start, airi_storage_elf_end}, // 只读取已恢复记录。
    {"files", airi_files_elf_start, airi_files_elf_end}, // 真实用户FD演示。
    {"broken", airi_broken_elf_start, airi_broken_elf_end}, // 用于完整的坏ELF失败演示。
};
static const struct executable *find_program(const char *name) { // 输入是内核已复制的短NUL字符串。
    size_t bytes = airi_strlen(name); // 只读当前有效字符串，不保存其指针。
    for (unsigned i = 0; i < sizeof(executables) / sizeof(executables[0]); ++i) // 遍历固定目录。
        if (airi_strlen(executables[i].name) == bytes && !airi_memcmp(executables[i].name, name, bytes)) // 长度与全部名称字节都相同才匹配。
            return &executables[i]; // 只借长期静态目录项。
    return NULL; // 未知名称不触碰任何用户地址空间。
}
static unsigned slot(const struct user_process *p) { // 只接受processes中的真实槽，不接受临时复制对象。
    return (unsigned)(p - processes); // 同一数组内指针差得到稳定槽号。
}
static bool create_stack(struct user_process *p) { // 每槽前面留一页保护页，所有栈共享已存在高半区分支。
    uint64_t address = STACK_BASE + slot(p) * 5 * 4096ull + 4096; // 四页数据与一页保护，PID增长不扩散地址。
    return airi_memory_region_create(&p->stack, address, 4); // 完整成功后才借给用户根。
}
static struct user_page *owned_at(const struct user_process *p, unsigned index) { // 下标可等于count供新增项写入，但必须已有记录页。
    unsigned chunk = index / PAGE_RECORDS; // 整数除法选出记录页，不要求各物理页连续。
    if (index >= AIRI_USER_PAGES || !p->records[chunk]) // 所有调用均来自内核已经验证的有效项。
        airi_panic("user page record is missing"); // 内部越界不能返回任意HHDM指针。
    struct user_page *entries = airi_memory_direct(p->records[chunk]); // 别名只借用，物理所有权仍在records中。
    return &entries[index % PAGE_RECORDS]; // 余数选页内项；单项不会跨物理页。
}
static void trim_records(struct user_process *p) { // count已缩小，释放不再容纳任何活记录的尾部元数据页。
    unsigned needed = (p->count + PAGE_RECORDS - 1) / PAGE_RECORDS; // 零项需要零页，其余向上取整。
    for (unsigned i = needed; i < RECORD_CHUNKS; ++i) { // 固定七槽，只检查完全位于有效项之外的页。
        if (!p->records[i]) // 尚未分配的槽没有物理所有权。
            continue; // 不把0哨兵交给页池。
        if (!airi_memory_return_page(p->records[i])) // 没有独立VA映射，HHDM随内核始终存在。
            airi_panic("cannot reclaim user page records"); // 归属错误不能静默复用页面。
        p->records[i] = 0; // 归还后立即撤掉唯一所有权记录。
    }
}
static bool user_access(const struct user_process *p, uint64_t address, size_t bytes, bool write) { // 检查完整用户范围，不直接解引用它。
    if (address >= AIRI_USER_LIMIT || bytes > AIRI_USER_LIMIT - address) // 排除内核地址与右端溢出。
        return false; // 零长度也保留本项目地址策略。
    for (size_t done = 0; done < bytes;) { // 跨越的每一页都必须满足权限。
        unsigned flags; // 当前叶子权限。
        if (!airi_vm_lookup(&p->space, address + done, NULL, &flags) || !(flags & AIRI_VM_USER) || (write && !(flags & AIRI_VM_WRITE))) // 输出指针还要求用户可写。
            return false; // 不先复制有效前缀。
        size_t part = 4096 - ((address + done) & 4095); // 当前页内剩余跨度。
        if (part > bytes - done) // 最后一段只到请求右端。
            part = bytes - done; // 不额外检查其他页。
        done += part; // 检查下一段。
    }
    return true; // 当前不可抢占内核中没有并发映射修改者。
}
static bool copy_user(struct user_process *p, void *buffer, uint64_t address, size_t bytes, bool to_user) { // 允许从非活动父根复制wait状态，但始终按其所有权查页。
    if (!user_access(p, address, bytes, to_user)) // 写用户缓冲必须检查WRITE，读只需要USER映射。
        return false; // 任何页失败都不复制。
    for (size_t done = 0; done < bytes;) { // 每次用HHDM别名处理一页中的有效段。
        uint64_t physical; // 翻译保留页内偏移。
        airi_vm_lookup(&p->space, address + done, &physical, NULL); // 完整检查后页表在本次内核操作中保持不变。
        size_t part = 4096 - ((address + done) & 4095); // 当前页边界。
        if (part > bytes - done) // 不超出剩余内容。
            part = bytes - done; // 缩到最后一段。
        if (to_user) // wait结果由内核写入父进程自己的页面。
            airi_memcpy(airi_memory_direct(physical), (uint8_t *)buffer + done, part); // 不切到父用户根也能访问其已拥有物理页。
        else // write系统调用把用户数据复制进内核副本。
            airi_memcpy((uint8_t *)buffer + done, airi_memory_direct(physical), part); // 不信任原始VA作为内核指针。
        done += part; // 推进下一段。
    }
    return true; // 同步结束，不借出buffer指针。
}
static long file_transfer(struct user_process *p, uint64_t fd, uint64_t address, size_t bytes, bool write, bool positioned, uint64_t offset) { // 允许在另一根的阻塞进程上重试，同步副本属于当前内核栈。
    char buffer[256]; // 一次有界副本，不跨调度保留。
    if (fd >= AIRI_FD_COUNT) // 完整64位编号先检查再转换。
        return -9; // -EBADF。
    if (bytes > sizeof(buffer)) // 不让用户长度覆盖内核栈。
        return -22; // -EINVAL。
    if (!user_access(p, address, bytes, !write)) // 输出在消费流或偏移之前完整验证。
        return -14; // -EFAULT。
    if (write) { // 用户内容在本次尝试内复制。
        copy_user(p, buffer, address, bytes, false); // 通过目标自己的页表，不解引用原始用户VA。
        if (!positioned && airi_fd_console(&p->files, (unsigned)fd)) { // 固定终端同步发送。
            airi_serial_write_bytes(buffer, bytes); // 不保存局部buffer给设备。
            return (long)bytes; // 返回逻辑进展。
        }
        return airi_fd_write(&p->files, (unsigned)fd, buffer, bytes, positioned, offset); // 满管道可返回EAGAIN。
    }
    if (airi_fd_input(&p->files, (unsigned)fd)) { // 固定输入对象的引用与当前前台读取资格分别检查。
        if (p->pid != input_pid) // fork复制的FD引用本身不转移UART消费权。
            return -13; // -EACCES；不消费设备字节。
        if (positioned) // 字节流没有文件偏移。
            return -29; // -ESPIPE，不消费字节。
        if (!bytes) // 零长度读取不能登记永久等待，也不消费UART。
            return 0; // 有效输入FD的空操作。
        size_t received = 0; // 返回当前可取得的前缀，不等待填满用户请求。
        while (received < bytes) { // bytes与整个用户输出范围已在上方验证。
            int c = airi_arch_serial_read(); // 设备寄存器只在架构层。
            if (c < 0) // 暂无字节和字符NUL不同。
                break; // 让已有前缀先交给用户。
            buffer[received++] = (uint8_t)c; // 固定内核副本不会越过256字节。
        }
        if (!received) // 空输入走同一个IO_READ持久等待机制。
            return -11; // 由陷入分派登记等待，不让shell忙轮询。
        if (!copy_user(p, buffer, address, received, true)) // 消费UART前已经完整验证用户范围。
            airi_panic("validated UART output mapping changed"); // 内部错误不能静默丢字节。
        return (long)received; // 实际取得的字节数。
    }
    long result = airi_fd_read(&p->files, (unsigned)fd, buffer, bytes, positioned, offset); // EOF和暂不可读分开。
    if (result > 0 && !copy_user(p, buffer, address, (size_t)result, true)) // 目标阻塞且单线程，已检查映射不应变化。
        airi_panic("validated file output mapping changed"); // 内部错误不丢字节后假装成功。
    return result; // 完成前缀或明确负错误。
}
static void poll_waiters(void) { // 仅普通内核上下文运行，IRQ本身不复制、分配或打印。
    for (unsigned i = 0; i < PROCESS_COUNT; ++i) { // 本阶段有界等待队列由进程槽直接表示。
        struct user_process *p = &processes[i]; // 控制块在等待期间不会回收。
        if (p->state != USER_IO_WAIT) // 不碰正在运行、wait孩子或已退出者。
            continue; // 检查下一个等待槽。
        long result; // 恢复到用户同一次调用的结果。
        if (p->io == IO_SLEEP) { // 只有截止tick，没有用户缓冲或设备FD。
            if (airi_timer_ticks() < p->io_deadline) // 时钟尚未到期，保留原现场等待。
                continue; // 不恢复用户忙轮询。
            result = 0; // 到期后同一次sleep系统调用成功返回。
        } else if (p->io == IO_ASYNC) { // 原CQ等待仍保留同一个用户输出地址。
            struct airi_io_completion completion; // 先取得内核数值副本，不借用户指针给引擎。
            result = airi_async_pop(p->pid, &completion); // 空CQ为-11，不代表I/O结果。
            if (result == -11) // 尚未形成可收集结果。
                continue; // 设备由启动服务栈推进，不在这里忙等。
            if (!result && !copy_user(p, &completion, p->io_address, sizeof(completion), true)) // 等待前已验证，单线程阻塞期间映射不能改变。
                airi_panic("validated completion output mapping changed"); // 不能消费CQ后静默丢结果。
        } else if (p->io == IO_READY) { // 订阅的是条件，不是一次成功read的承诺。
            result = airi_fd_ready(&p->files, p->io_fd); // 当前条件可能被其他进程消费或改变。
            if (result >= 0) // 负错误必须原样唤醒，不能按位筛掉。
                result &= p->io_events | AIRI_READY_HUP | AIRI_READY_ERROR; // 对端关闭即使未显式请求也应可见。
            if (!result && airi_timer_ticks() < p->io_deadline) // 无条件成立且尚未超时。
                continue; // 保持等待，之后由状态变化或PIT服务重查。
        } else { // 阻塞read/write重新尝试同一个有界请求。
            result = file_transfer(p, p->io_fd, p->io_address, (size_t)p->io_bytes, p->io == IO_WRITE, false, 0); // 用户buffer仍由p持有。
            if (result == -11) // 暂时依旧没有进展。
                continue; // 不把原系统调用提前返回给用户忙等。
        }
        p->frame->rax = (uint64_t)result; // 用户返回位置仍在原调用之后。
        p->io = IO_NONE; // 这一次等待不再持有请求地址。
        p->io_address = p->io_bytes = p->io_deadline = 0; // 清过期的用户对象与截止值。
        p->state = USER_RUNNABLE; // 结果就绪之后才发布运行资格。
        ++io_wake_count; // 实际唤醒，不把查询次数计成完成。
        airi_log("[WAIT_WAKE] pid=%u result=%ld", p->pid, result); // 普通内核服务栈输出，不在IRQ里打印。
    }
}
static void cleanup(struct user_process *p) { // 目标根必须不活动，而且当前不能使用目标内核栈。
    if (p->async_owned) { // 失败构造或最终收拢都只关闭真正拥有的owner。
        airi_async_close(p->pid); // ACTIVE副本归内核，取消不释放其后端目标。
        p->async_owned = false; // 局部副本与重复清理不能再关同一轮次。
    }
    airi_fd_close_all(&p->files, false); // 回滚或已退出空表均安全，不能遗留全局引用。
    if (p->space.root && !airi_vm_destroy(&p->space)) // 先解除并归还自有页表，不动借用的内核表。
        airi_panic("cannot destroy inactive user tables"); // 不能归还仍被活动根引用的数据页。
    while (p->count) // 用户代码与用户栈数据页分别归还。
        if (!airi_memory_return_page(owned_at(p, --p->count)->physical)) // 先退到最后一个真正拥有的页。
            airi_panic("cannot return owned user page"); // 归属错误不继续复用内存。
    trim_records(p); // 所有数据页已归还，最后释放保存其VA/PA的记录页。
    if (p->stack.count && !airi_memory_region_free(&p->stack)) // 调用者已经离开目标内核栈，才可销毁它。
        airi_panic("cannot reclaim user kernel stack"); // 不能在finish中提前做这一步。
    airi_memset(p, 0, sizeof(*p)); // 不留下可重复释放的根或句柄。
}
static void initial_frame(struct user_process *p, uint64_t entry, const char *name) { // 输入页已完整映射并清零，最后才发布首次返回现场。
    uint64_t stack_pointer = USER_STACK + 4096 - 16; // 旧受控汇编映像不读取argc，保持原布局。
    if (name) { // C程序使用本项目最小argc/argv栈协议。
        size_t bytes = airi_strlen(name) + 1; // 包含用户字符串的终止零。
        if (bytes > 64) // 当前程序名是固定短字面量，不支持无界参数。
            airi_panic("initial program name exceeds stack budget"); // 不覆盖用户启动布局。
        uint64_t physical; // 当前根未激活，通过软件翻译找到用户栈数据页。
        if (!airi_vm_lookup(&p->space, USER_STACK, &physical, NULL)) // 栈必须已建立。
            airi_panic("initial user stack mapping is missing"); // 不猜测owned数组中的位置。
        uint8_t *alias = airi_memory_direct(physical); // 通过内核HHDM填写尚未运行的用户栈。
        size_t name_offset = 4096 - bytes; // 字符串放在栈页高端。
        airi_memcpy(alias + name_offset, name, bytes); // 用户只得到自己的字符串副本。
        size_t words_offset = (name_offset & ~(size_t)15) - 4 * sizeof(uint64_t); // 四个64位槽，入口RSP仍16字节对齐。
        uint64_t *words = (uint64_t *)(alias + words_offset); // 已对齐的栈内数组。
        words[0] = 1; // argc目前固定一个程序名。
        words[1] = USER_STACK + name_offset; // argv[0]必须是用户VA，不是HHDM指针。
        words[2] = 0; // argv[1]结束指针数组。
        words[3] = 0; // 空环境，当前没有完整ELF辅助向量。
        stack_pointer = USER_STACK + words_offset; // CPU初始RSP指向argc槽。
    }
    p->frame = &p->initial; // 先保存独立寄存器值，place_frame稍后复制进实际内核栈。
    airi_memset(p->frame, 0, sizeof(*p->frame)); // 首次没有旧GPR。
    p->frame->rip = entry; // ELF入口或已知汇编入口。
    p->frame->cs = 27; // 用户代码段与RPL3。
    p->frame->ss = 35; // 用户数据段与RPL3。
    p->frame->rsp = stack_pointer; // 使用刚准备的用户布局。
    p->frame->rflags = 0x202; // 用户IF开启，IOPL为零。
}
static bool owned_page(struct user_process *p, uint64_t address, unsigned flags) { // 一次只建立一页，失败页面仍可由统一cleanup找到。
    if (p->count == AIRI_USER_PAGES) // 包括ELF、动态页和用户栈，不包括记录页。
        return false; // 不越过固定记录容量。
    unsigned chunk = p->count / PAGE_RECORDS; // 下一项应写入哪一页元数据。
    if (!p->records[chunk]) { // 仅跨过170项边界时才增加一个记录页。
        p->records[chunk] = airi_memory_take_page(); // 记录页与用户数据页分别拥有、分别归还。
        if (!p->records[chunk]) // 元数据耗尽不能继续分配没有记录位置的数据页。
            return false; // 旧有效项和旧映射完全保持。
        airi_memset(airi_memory_direct(p->records[chunk]), 0, 4096); // 清除前一个所有者的地址记录。
    }
    uint64_t physical = airi_memory_take_page(); // 取得真正归本上下文持有的物理页。
    if (!physical) { // 可能刚借出记录页，却还未增加有效数据项。
        trim_records(p); // 收回本次没有承载活项的新记录页。
        return false; // 调用方再回滚本批此前成功的数据页。
    }
    airi_memset(airi_memory_direct(physical), 0, 4096); // 文件尾部与BSS默认都从零开始。
    if (!airi_vm_map(&p->space, address, physical, flags | AIRI_VM_USER)) { // 单页映射失败还没有发布到所有权数组。
        airi_memory_return_page(physical); // 明确归还本次未接入数据页。
        trim_records(p); // 映射失败也不能遗留空元数据页。
        return false; // 调用者回滚此前成功的部分。
    }
    *owned_at(p, p->count) = (struct user_page){address, physical, flags | AIRI_VM_USER}; // 保存VA/PA/权限供fork复制与精确回收。
    ++p->count; // 记录完整后才发布有效项；指针不跨记录页释放保留。
    return true; // 每个已登记数据页都有对应映射。
}
static int prepare_elf(struct user_process *p, const uint8_t *image, size_t bytes, const char *name, bool make_stack) { // 空目标只拥有新用户资源；exec不另建正在使用的内核栈。
    struct airi_elf_view view; // 只借输入ELF直到复制结束。
    const char *error = airi_elf_inspect(image, bytes, &view); // 在任何资源分配前校验整个输入。
    if (error) { // 损坏格式不能触发后续内存写入。
        airi_log("[ELF_REJECTED] %s", error); // 给出精确范围/权限原因。
        return -8; // -ENOEXEC，旧输出和调用者原上下文都未改变。
    }
    if (make_stack && !create_stack(p)) // 内核陷入栈先完成，再借高半区。
        return -12; // -ENOMEM，区域接口已回滚失败。
    if (!airi_memory_user_space(&p->space)) // 创建独立低半区。
        goto failed; // 后续失败均收回所有已持有资源。
    for (unsigned i = 0; i < view.count; ++i) { // 每段独立赋予对应权限。
        const struct airi_elf_segment *segment = &view.segments[i]; // 元数据已经过统一校验。
        unsigned flags = ((segment->flags & 2) ? AIRI_VM_WRITE : 0) | ((segment->flags & 1) ? AIRI_VM_EXEC : 0); // ELF PF标志转换成本内核权限枚举。
        uint64_t first = segment->address & ~4095ull; // 覆盖段首所在页。
        uint64_t end = (segment->address + segment->memory_bytes + 4095) & ~4095ull; // 包含完整BSS跨度。
        for (uint64_t address = first; address < end; address += 4096) // 所有页面先清零再接入。
            if (!owned_page(p, address, flags)) // 物理耗尽或映射失败都终止本次装载。
                goto failed; // 清理根/表后再归还数据页。
        for (uint64_t done = 0; done < segment->file_bytes;) { // 只复制filesz字节，memsz剩余继续保持零。
            uint64_t physical; // lookup结果包含当前页内偏移。
            if (!airi_vm_lookup(&p->space, segment->address + done, &physical, NULL)) // 已映射跨度必须可查询。
                goto failed; // 内部装载状态不一致也不运行。
            uint64_t part = 4096 - ((segment->address + done) & 4095); // 当前页内可复制长度。
            if (part > segment->file_bytes - done) // 不从文件右端外复制字节。
                part = segment->file_bytes - done; // 最后一次缩到实际剩余。
            airi_memcpy(airi_memory_direct(physical), view.image + segment->offset + done, (size_t)part); // 内核控制的物理别名可初始化最终只读/可执行页。
            done += part; // 下一次继续剩余文件内容。
        }
    }
    for (unsigned i = 0; i < AIRI_USER_STACK_PAGES; ++i) // 最高页地址不变，向低地址增加十五页。
        if (!owned_page(p, USER_STACK - i * 4096ull, AIRI_VM_WRITE)) // 栈RW/NX，最低页之下仍无映射。
            goto failed; // 任意一页失败均回收整份新映像。
    initial_frame(p, view.entry, name); // 用户_start接收最小argc/argv协议。
    airi_log("[ELF_LOAD] entry=0x%llx segments=%u data_pages=%u argc=1", (unsigned long long)view.entry, view.count, p->count); // 不含页表与内核栈的用户数据页数量。
    return 0; // 只准备新空间，尚未切CR3或覆盖旧现场。
failed: // 长期根仍活动，用户空间从未开始执行。
    cleanup(p); // 页表、用户数据和内核栈全部回收。
    return -12; // -ENOMEM，不提供部分映像。
}
static void place_frame(struct user_process *p) { // 初始寄存器值完成后复制到自己拥有的内核栈。
    uint64_t top = p->stack.virtual_address + p->stack.count * 4096ull; // TSS以后从同一个栈顶压入真实现场。
    p->frame = (void *)(uintptr_t)(top - sizeof(*p->frame)); // 与公共入口的176字节布局一致。
    *p->frame = p->initial; // 不借用临时C结构或其他进程的内核栈。
}
static bool prepare_flat(struct user_process *p, const uint8_t *image, size_t bytes) { // 保留受控指令故障回归，不作为ELF加载接口。
    if (!bytes || bytes > 4096 || !create_stack(p)) // 本节旧映像最多一页。
        return false; // 栈创建失败已回滚。
    if (!airi_memory_user_space(&p->space) || !owned_page(p, USER_CODE, AIRI_VM_EXEC) || !owned_page(p, USER_STACK, AIRI_VM_WRITE)) // 分别拥有根、代码和栈。
        goto failed; // 所有权列表可清理全部已成功部分。
    airi_memcpy(airi_memory_direct(owned_at(p, 0)->physical), image, bytes); // 映像内引用保持旧固定布局。
    initial_frame(p, USER_CODE, NULL); // 受控汇编不读取argc/argv。
    return true; // 尚未设置可运行状态。
failed: // 目标从未被调度，当前根和栈均不属于它。
    cleanup(p); // 完整回滚。
    return false; // 不运行半个上下文。
}
static struct user_process *find_pid(unsigned pid) { // PID与数组槽分开，退出回收后不会匹配旧身份。
    for (unsigned i = 0; i < PROCESS_COUNT; ++i) // 遍历有界进程表。
        if (processes[i].state != USER_UNUSED && processes[i].pid == pid) // 只查仍存活或待回收的身份。
            return &processes[i]; // 内核内部借用，不向用户交指针。
    return NULL; // 未找到活身份。
}
static struct user_process *unused(void) { // 准备中的槽不会被发布为RUNNABLE。
    for (unsigned i = 0; i < PROCESS_COUNT; ++i) // 固定容量避免无界内核分配。
        if (processes[i].state == USER_UNUSED) // 调用者在不可抢占内核中独占准备。
            return &processes[i]; // 后续失败必须清掉全部所有权。
    return NULL; // 表满返回明确失败。
}
static void collect_pending(void) { // 只在新的用户陷入入口调用，不在旧进程退出的C调用中释放其栈。
    for (unsigned i = 0; i < PROCESS_COUNT; ++i) // 回收父进程已取得状态的孩子。
        if (&processes[i] != current && processes[i].state == USER_REAP_PENDING) { // 当前真正运行的内核栈不能在此回收。
            unsigned pid = processes[i].pid; // cleanup清零控制块前保存诊断身份。
            bool orphan = processes[i].parent == 0; // 用户wait回收与内核托管回收分开观察。
            cleanup(&processes[i]); // 非活动根和已离开的内核栈可安全解除。
            if (orphan) // 不把普通父wait冒充自动托管。
                airi_log("[ORPHAN_REAP] pid=%u released=1", pid); // 只在内存和FD资源实际释放后发布。
        }
}
enum schedule_reason { REASON_TIMER, REASON_YIELD, REASON_WAIT, REASON_EXIT, REASON_IO }; // 原因用于验证真实抢占，不混成主动yield。
static struct airi_trap_frame *select_user(struct user_process *p) { // 只在完整陷入现场边界切根和TSS。
    current = p; // 之后的IRQ和系统调用归属这个身份。
    p->state = USER_RUNNING; // 新用户即将获得CPU。
    airi_trap_set_stack(p->stack.virtual_address + p->stack.count * 4096ull); // 陷入时使用目标自己的内核栈。
    if (!airi_vm_activate(&p->space)) // 所有高半区内核映射在准备时已存在。
        airi_panic("cannot activate selected user root"); // 不返回到缺映射的调用链。
    return airi_user_resume(p->frame); // 用户返回地址与标志必须仍符合边界。
}
static struct airi_trap_frame *pause_for_service(bool from_timer) { // 完整用户frame已保存，暂回长期启动栈做可延后工作。
    session_idle = true; // 会话仍active，不是用户程序已经退出。
    service_from_timer = from_timer; // 真正IRQ导致的换身份计数另记。
    airi_memory_use_kernel(); // 启动服务不依赖某个用户根。
    kernel_return->rax = 0; // 暂停结果不当成应用最终退出值。
    return kernel_return; // 汇编离开当前进程栈，之后才能安全服务其他等待者。
}
static struct airi_trap_frame *schedule(enum schedule_reason reason) { // 当前完整frame边界可切换，但IRQ不直接执行复杂I/O。
    struct user_process *old = current; // 仍暂用old的内核栈。
    if (old->state == USER_RUNNING) // 等待或退出不能误改回可运行。
        old->state = USER_RUNNABLE; // 时间片结束仍可运行。
    if (reason != REASON_TIMER) // 普通内核边界可重试等待条件。
        poll_waiters(); // 已有字节、关闭或错误形成后再唤醒。
    else { // IRQ只检查状态，并把设备推进交给启动服务栈。
        bool service = airi_async_work(); // 即使无人等待，已接收请求也不能被纯计算用户饿死。
        for (unsigned i = 0; i < PROCESS_COUNT; ++i) // 管道、有限条件与CQ等待都需要普通服务。
            service |= processes[i].state == USER_IO_WAIT; // 不在IRQ里复制、分配或打印。
        if (service) // 完整用户frame已经保存。
            return pause_for_service(true); // 由启动栈pump与重查，再进入下一个用户。
    }
    for (unsigned n = 1; n <= PROCESS_COUNT; ++n) { // 从old之后轮转一圈。
        struct user_process *next = &processes[(slot(old) + n) % PROCESS_COUNT]; // PID与槽仍分开。
        if (next->state == USER_RUNNABLE) { // 仅可运行者有可返回的用户现场。
            if (next != old && reason == REASON_TIMER) // 真IRQ切不同身份。
                ++timer_switches; // 主动阻塞不冒充抢占。
            return select_user(next); // 汇编恢复目标frame。
        }
    }
    for (unsigned i = 0; i < PROCESS_COUNT; ++i) // 没有可运行者时检查仍在等待的活进程。
        if (processes[i].state == USER_WAITING || processes[i].state == USER_IO_WAIT) // 可能正在等设备、超时或未来对端。
            return pause_for_service(false); // 允许启动栈hlt等待真实IRQ，不PANIC用户死锁。
    session_active = false; // 所有用户均结束，此时才算完整会话退出。
    session_idle = false; // 不再保留暂停身份。
    airi_memory_use_kernel(); // 最后一个用户根也变成不活动。
    kernel_return->rax = (uint64_t)(int64_t)session_result; // 根用户的真实退出结果。
    return kernel_return; // 回启动栈后统一回收残留资源。
}
static struct airi_trap_frame *exit_current(int code) { // CPU还在当前进程内核栈上，先结束身份而不释放栈。
    struct user_process *p = current; // 当前退出者的稳定控制块。
    if (p->async_owned) { // 用户身份结束，不能把旧CQ交给之后复用的进程槽。
        airi_async_close(p->pid); // QUEUED撤销，ACTIVE内核副本保持到收集。
        p->async_owned = false; // cleanup不重复关闭未来的新epoch。
    }
    if (input_pid == p->pid) // 当前前台shell停止后，init的新孩子才能取得UART。
        input_pid = 0; // 只释放本进程的消费权，不替其他身份撤销。
    airi_fd_close_all(&p->files, false); // exit立即释放文件，僵尸只保留等待状态与待回收内存。
    p->exit_code = code; // 保存给父进程wait读取。
    p->state = USER_ZOMBIE; // 不再运行，但资源和状态尚存在。
    if (p->pid == root_pid) // 整个会话的结果以最初根用户为准。
        session_result = code; // 孩子退出不改变根结果。
    for (unsigned i = 0; i < PROCESS_COUNT; ++i) { // 父退出时交接全部直接孩子，不等待整次会话收拢。
        struct user_process *child = &processes[i]; // 活孩子和未收的僵尸都可能仍引用此父PID。
        if (child->state == USER_UNUSED || child->parent != p->pid) // 只接管本次退出者真正拥有的孩子。
            continue; // 不改其他父子关系。
        child->parent = 0; // PID0表示内核托管者，不是可运行的用户进程。
        if (child->state == USER_ZOMBIE) // 父已无法取得这份退出状态。
            child->state = USER_REAP_PENDING; // 下一条安全内核栈回收，不让僵尸永久占槽。
        airi_log("[ORPHAN_ADOPT] pid=%u former_parent=%u finished=%u", child->pid, p->pid, child->state == USER_REAP_PENDING); // 活孩子仍正常调度或等待。
    }
    struct user_process *parent = find_pid(p->parent); // 父身份可能已退出或不存在。
    if (p->pid != root_pid && (!parent || parent->state == USER_ZOMBIE || parent->state == USER_REAP_PENDING)) { // 会话根的统计保留到launch结束；普通孤儿无需用户wait。
        p->parent = 0; // 不再保留已失效的用户父PID。
        p->state = USER_REAP_PENDING; // 当前栈仍在使用，只登记延后回收资格。
        airi_log("[ORPHAN_EXIT] pid=%u code=%d deferred=1", p->pid, code); // 真正释放发生在离开此栈后。
    }
    if (parent && parent->state == USER_WAITING && (parent->wait_target == -1 || (uint64_t)parent->wait_target == p->pid)) { // 只有真正等待此孩子的父进程可以被唤醒。
        bool copied = !parent->wait_status || copy_user(parent, &code, parent->wait_status, sizeof(code), true); // 通过父进程自己的页表写结果。
        parent->frame->rax = copied ? p->pid : (uint64_t)-14ll; // 返回子PID或明确地址错误。
        parent->state = USER_RUNNABLE; // 结果形成后才公开可运行状态。
        parent->wait_target = 0; // 这一次等待已经结束。
        parent->wait_status = 0; // 不保留用户局部对象的过期地址。
        if (copied) // 地址错误时保留僵尸，让父进程能用有效指针重试。
            p->state = USER_REAP_PENDING; // 下一次别的用户陷入后才能释放本次仍在用的内核栈。
    }
    airi_log("[USER_EXIT] pid=%u code=%d user_ticks=%u", p->pid, code, p->user_ticks); // 正常系统调用上下文记录身份结果。
    return schedule(REASON_EXIT); // 子进程或父进程可继续，未必立刻回启动栈。
}
static long fork_current(struct airi_trap_frame *parent_frame) { // 首版逐页复制，不实现COW。
    if (next_pid == UINT32_MAX) // 固定32位正PID不允许回绕命中旧身份。
        return -75; // -EOVERFLOW。
    struct user_process *parent = current; // 父的用户数据与完整返回现场。
    struct user_process *child = unused(); // 未发布的新槽。
    if (!child) // 表满没有可分配身份。
        return -11; // -EAGAIN。
    if (!create_stack(child) || !airi_memory_user_space(&child->space)) // 先创建独立内核栈和空低半区。
        goto failed; // 父状态从未改变。
    for (unsigned i = 0; i < parent->count; ++i) { // 包括用户栈和此前申请的堆页。
        const struct user_page *page = owned_at(parent, i); // 每页原VA、权限和物理地址。
        if (!owned_page(child, page->address, page->flags)) // 子页在相同VA，但物理所有权独立。
            goto failed; // 回滚全部子资源，不改变父页面。
        airi_memcpy(airi_memory_direct(owned_at(child, child->count - 1)->physical), airi_memory_direct(page->physical), 4096); // 复制完整页内容，父子以后写入互不影响。
    }
    airi_fd_clone(&child->files, &parent->files); // 页复制全部成功后再取得FD引用，失败cleanup仍能撤销。
    child->initial = *parent_frame; // 用户RIP/RSP和寄存器从同一次fork返回点继续。
    child->initial.rax = 0; // 子分支看到fork返回0。
    child->pid = next_pid++; // 全部内存成功后才分配PID。
    child->parent = parent->pid; // 关系用身份而不是可能复用的槽指针表示。
    if (airi_async_open(child->pid)) // fork创建空上下文，用户内存里的父handle整数并不授权子访问。
        goto failed; // 其余子资源统一回滚，父不变。
    child->async_owned = true; // 成功后才记资源所有权。
    place_frame(child); // 只把用户返回现场放入子内核栈，不复制父的内核C调用链。
    child->state = USER_RUNNABLE; // 准备完成后才让IRQ调度器看见它。
    airi_log("[FORK] parent=%u child=%u copied_pages=%u", parent->pid, child->pid, child->count); // 实际复制数量含用户栈与堆。
    return child->pid; // 父分支得到正子PID。
failed: // 还没有向调度器发布孩子，当前仍在父根和父内核栈上。
    cleanup(child); // 删除不活动子根和其独立栈，父继续原系统调用。
    return -12; // -ENOMEM，父的地址空间和返回点不变。
}
static struct airi_trap_frame *wait_current(struct airi_trap_frame *f) { // 自有wait ABI返回PID，status写普通int而非Linux编码。
    int64_t target = (int64_t)f->rdi; // -1表示任意孩子，其他必须为正PID。
    uint64_t status = f->rsi; // NULL允许只回收不读取退出码。
    if ((target < 1 && target != -1) || (status && !user_access(current, status, sizeof(int), true))) { // 输出范围在阻塞之前就核对。
        f->rax = (target < 1 && target != -1) ? (uint64_t)-22ll : (uint64_t)-14ll; // 参数错误不改变调度状态。
        return airi_user_resume(f); // 当前进程仍运行，可换参数重试。
    }
    bool found = false; // 区分没有孩子与孩子仍在运行。
    for (unsigned i = 0; i < PROCESS_COUNT; ++i) { // 只匹配当前PID的子关系。
        struct user_process *child = &processes[i]; // 槽可能已经回收或属于其他父进程。
        if (child->state == USER_UNUSED || child->state == USER_REAP_PENDING || child->parent != current->pid || (target != -1 && (uint64_t)target != child->pid)) // 不允许等待别人的子进程。
            continue; // 查找下一个可能孩子。
        found = true; // 至少有一个真实孩子。
        if (child->state == USER_ZOMBIE) { // 已结束孩子可以在父内核栈上立即回收。
            unsigned pid = child->pid; // 清理前保存返回身份。
            int code = child->exit_code; // 清理前保存退出状态。
            if (status && !copy_user(current, &code, status, sizeof(code), true)) // 即使前面检查过也保留明确复制结果。
                airi_panic("wait output mapping changed in nonpreemptible kernel"); // 内部不变量被破坏，不静默错写。
            cleanup(child); // 当前确实不在孩子栈上，其根也不活动。
            f->rax = pid; // 回收完成后返回已取得状态的孩子PID。
            return airi_user_resume(f); // 普通返回到父用户调用方。
        }
    }
    if (!found) { // 没有匹配孩子，不应永远等待。
        f->rax = (uint64_t)-10ll; // -ECHILD。
        return airi_user_resume(f); // 父可继续其他工作。
    }
    current->wait_target = target; // 保存要等待的身份条件。
    current->wait_status = status; // 父阻塞期间单线程不会修改这些用户映射。
    current->state = USER_WAITING; // 先记录条件与结果地址，再让调度器跳过父。
    ++wait_blocks; // 真实阻塞，而非轮询返回成功。
    return schedule(REASON_WAIT); // 孩子结束时填写此f->rax并唤醒父。
}
static long allocate_user(size_t bytes) { // 按页提供有界用户内存，不是完整malloc。
    if (!bytes || bytes > AIRI_USER_REQUEST_PAGES * 4096ull) // 一次最多六十四页，取整加法保持有界。
        return -22; // -EINVAL。
    unsigned count = (unsigned)((bytes + 4095) / 4096); // 请求向上取整为完整页。
    if (count > AIRI_USER_PAGES - current->count) // 先检查整批有效项容量。
        return -12; // -ENOMEM。
    uint64_t address = USER_HEAP; // 从低端找能容纳本次整段的空洞，不依赖旧分配顺序。
    for (;;) { // 每次冲突越过至少一个已拥有页，搜索被1024项预算限制。
        bool conflict = false; // 本候选范围是否撞上现有用户数据页。
        for (unsigned i = 0; i < current->count; ++i) { // owned可能因释放而换序，不要求按VA排序。
            uint64_t used = owned_at(current, i)->address; // 当前页仍由此进程拥有。
            if (used >= address && used < address + count * 4096ull) { // 页对齐后，页起点落入候选即发生重叠。
                address = used + 4096; // 此页之前的起点都不能容纳本次整段，跳到该页右端。
                conflict = true; // 新候选须重新从整个owned数组检查。
                break; // 不沿用旧范围检查结果。
            }
        }
        if (address > USER_HEAP_END - count * 4096ull) // 先减有界长度，不允许候选越过动态范围。
            return -12; // -ENOMEM；尚未分配页面，也没有修改原映射。
        if (!conflict) // 所有现有页都不与此候选相交。
            break; // 后续只为这段空洞创建映射。
    }
    unsigned before = current->count; // 失败只回滚本次新增映射。
    for (unsigned i = 0; i < count; ++i) // 页物理可以不连续，用户VA连续。
        if (!owned_page(current, address + i * 4096ull, AIRI_VM_WRITE)) // 成功页面已在owned中登记。
            goto rollback; // 游标尚未前移。
    airi_log("[USER_ALLOC] pid=%u address=0x%llx pages=%u zeroed=1", current->pid, (unsigned long long)address, count); // 只记录已完整提交的用户映射。
    return (long)address; // 用户可显式归还完整页，未归还页仍在exit统一清理。
rollback: // 已有用户页面不受本次耗尽影响。
    while (current->count > before) { // 只处理新增的成功项。
        const struct user_page *page = owned_at(current, --current->count); // 先退到最后一项。
        if (!airi_vm_unmap(&current->space, page->address) || !airi_memory_return_page(page->physical)) // 活动根必须先断映射并失效，再归还。
            airi_panic("cannot roll back user page allocation"); // 内部所有权错误不能被吞掉。
    }
    trim_records(current); // 本批回滚后，尾部没有活项的记录页也一起归还。
    return -12; // 不返回半段用户范围。
}
static unsigned owned_index(uint64_t address) { // 只在当前单线程进程的不可抢占系统调用中查询。
    for (unsigned i = 0; i < current->count; ++i) // 有效项之外可能有旧字节，不参与查找。
        if (owned_at(current, i)->address == address) // 精确页起点，不接受页内指针。
            return i; // 下标只借给本次操作，不跨状态修改保存。
    return current->count; // count本身是未找到哨兵，不能作为数组下标解引用。
}
static long release_user(uint64_t address, uint64_t bytes) { // 按页解除动态用户映射，不是C malloc对象free。
    if (!bytes || bytes > AIRI_USER_REQUEST_PAGES * 4096ull || (bytes & 4095) || (address & 4095) || address < USER_HEAP || address >= USER_HEAP_END || bytes > USER_HEAP_END - address) // 短路先限地址再做右端减法。
        return -22; // -EINVAL；不舍入无效长度，不动任何页。
    unsigned count = (unsigned)(bytes / 4096); // 参数已限制为一到六十四个完整页。
    for (unsigned i = 0; i < count; ++i) { // 第一遍先验证整段，避免中途坏页留下部分解除。
        unsigned index = owned_index(address + i * 4096ull); // 当前目标VA属于动态页范围。
        if (index == current->count || owned_at(current, index)->flags != (AIRI_VM_USER | AIRI_VM_WRITE)) // 既需真实所有权，也只接受RW/NX用户动态页。
            return -22; // 任何一页不存在，整段保持原样。
    }
    for (unsigned i = 0; i < count; ++i) { // 第二遍提交，当前没有其他线程改变本地址空间。
        unsigned index = owned_index(address + i * 4096ull); // 每次重新查，上一轮可能移动了owned末项。
        struct user_page page = *owned_at(current, index); // 在压缩数组前保留当前VA与PA数值。
        if (!airi_vm_unmap(&current->space, page.address) || !airi_memory_return_page(page.physical)) // 先断开活动根映射并invlpg，才归还物理页。
            airi_panic("cannot release owned dynamic user page"); // 内部不变量失败不能返回假成功。
        *owned_at(current, index) = *owned_at(current, --current->count); // 末项可位于另一记录页，复制后才允许回收尾页。
    }
    trim_records(current); // 全段压缩结束，任何有效项都不再借用空尾页。
    airi_log("[USER_RELEASE] pid=%u address=0x%llx pages=%u", current->pid, (unsigned long long)address, count); // 只记录整段真正归还后的结果。
    return 0; // 后续分配可复用空洞，新物理页按原owned_page逻辑清零。
}
static long exec_current(struct airi_trap_frame *f) { // 成功不返回旧用户调用，失败必须保持旧PID、空间和现场。
    char name[64]; // 先把用户名称复制到当前内核栈，后续不再借旧用户页。
    bool terminated = false; // 不能把未终止输入交给strlen。
    for (unsigned i = 0; i < sizeof(name); ++i) { // 有界逐字节读取，允许跨用户页但不越过上限。
        if (f->rdi > UINT64_MAX - i || !copy_user(current, &name[i], f->rdi + i, 1, false)) // 名称本身也可能是坏指针。
            return -14; // -EFAULT，旧用户继续。
        if (!name[i]) { // 取得完整NUL字符串后才查目录。
            terminated = true; // 后续strlen有明确终止界限。
            break; // 不读取字符串后无关字节。
        }
    }
    if (!terminated) // 超过本项目名称预算。
        return -36; // -ENAMETOOLONG。
    const struct executable *image = find_program(name); // 不存在目标不用分配新根。
    if (!image) // 和损坏格式是两种不同失败。
        return -2; // -ENOENT。
    bool acquire_input = supervisor_pid && current->parent == supervisor_pid && airi_strlen(name) == 2 && !airi_memcmp(name, "sh", 2); // 只有init的直接shell孩子可获得新前台输入。
    if (acquire_input && input_pid && input_pid != current->pid) // 同一个init也不能同时授权两个读者。
        return -16; // -EBUSY；尚未触碰旧映像或FD。
    struct user_process next = {0}; // 临时所有者不在进程表中，尚不能被调度。
    int error = prepare_elf(&next, image->start, (size_t)((uintptr_t)image->end - (uintptr_t)image->start), name, false); // 不创建第二条内核栈，先完整准备新用户空间。
    if (error) // 构造器已回滚next，current所有字段和旧f不变。
        return error; // -ENOEXEC或-ENOMEM。
    int async_error = airi_async_reset(current->pid); // 新ELF完整后，提交前确认可以进入新资源epoch。
    if (async_error) { // reset失败发生在撤销前，旧映像与旧I/O资源仍有效。
        cleanup(&next); // next尚未拥有当前PID的owner。
        return async_error; // 原用户调用可继续处理失败。
    }
    struct user_process old = *current; // 暂存旧用户资源，切离旧根之后归还。
    old.async_owned = false; // reset已撤销旧轮次，cleanup旧映像不能关闭新的owner。
    next.async_owned = true; // 同一PID的新epoch由提交后的控制块拥有。
    unsigned old_pages = old.count; // 归还前保留实际替换规模用于记录。
    next.pid = old.pid; // exec保持进程身份。
    next.parent = old.parent; // 父仍等待同一个孩子。
    next.user_ticks = old.user_ticks; // 同一身份的已运行计数不丢失。
    next.stack = old.stack; // 明确转移正在使用的内核栈所有权，VA和物理内容都不变。
    airi_memset(&old.stack, 0, sizeof(old.stack)); // old清理者不再拥有这组栈，不能重复释放。
    next.frame = f; // 真实返回现场仍在同一内核栈上，不能留下指向next.initial局部对象的指针。
    *f = next.initial; // 事务提交时才覆盖旧用户RIP/RSP和寄存器。
    next.files = old.files; // 文件表所有权从旧映像转到新映像，偏移和普通FD继续存在。
    airi_memset(&old.files, 0, sizeof(old.files)); // cleanup旧映像不能重复关闭已转移引用。
    airi_fd_close_all(&next.files, true); // 只有成功提交才关闭CLOEXEC，失败exec不影响原表。
    if (input_pid == next.pid) // 若前台进程自行exec，旧输入权限不能漏到其他映像。
        input_pid = 0; // 成功提交时才撤销，失败exec仍保留旧权限。
    if (acquire_input) { // 本次新映像确实是init管理的shell。
        airi_fd_close(&next.files, 0); // 无表项时-9可忽略；已有普通FD0也按此启动策略关闭。
        if (!airi_fd_attach_input(&next.files)) // 新FD0必须为空且输入引用成功接入。
            airi_panic("cannot attach supervised shell input"); // 事务提交内部不变量错误。
        input_pid = next.pid; // 文件引用完成后才发布唯一消费身份。
    }
    next.state = USER_RUNNING; // 此身份继续运行，但从新_start开始。
    *current = next; // 把新所有权发布到原进程槽，PID不变。
    if (!airi_vm_activate(&current->space)) // 新根已借到当前内核栈及内核映射。
        airi_panic("cannot activate committed exec image"); // 硬件不变量失败不能回旧用户继续。
    cleanup(&old); // 旧根已经不活动，旧数据可回收；当前内核栈已转移所以不释放。
    airi_log("[EXEC_COMMIT] pid=%u program=%s old_pages=%u new_pages=%u", current->pid, name, old_pages, current->count); // 名称是内核副本，不引用刚释放用户页。
    return 0; // 调用者直接恢复新f，不把这个0写回旧用户调用。
}
struct airi_trap_frame *airi_user_resume(struct airi_trap_frame *f) { // 所有用户iretq路径使用同一规范地址和标志策略。
    if (f->cs != 27 || f->ss != 35 || f->rip >= AIRI_USER_LIMIT || f->rsp >= AIRI_USER_LIMIT) { // 坏返回不能变成内核GP。
        airi_log("[USER_RETURN_REJECTED] pid=%u rip=0x%llx rsp=0x%llx", current->pid, (unsigned long long)f->rip, (unsigned long long)f->rsp); // 不解引用错误地址。
        return exit_current(-22); // 只结束拥有这个现场的用户。
    }
    f->rflags &= ~((3ull << 12) | (1ull << 14) | (1ull << 17)); // 清IOPL、NT和VM。
    f->rflags |= 0x202; // 保留必需bit1并保持用户IF。
    return f; // 返回选中进程的完整现场。
}
struct airi_trap_frame *airi_user_timer(struct airi_trap_frame *f) { // IRQ路径不分配、不回收、不打印，只有状态与根选择。
    if (!session_active || !current || current->state != USER_RUNNING) // CS3的IRQ必须有明确用户所有者。
        airi_panic("user IRQ without a running process"); // 不把未知内核栈交给调度器。
    current->frame = f; // 保存真正被打断的位置，不只保存C ABI寄存器。
    ++current->user_ticks; // 实际CPU用户时间片事件数。
    if (current->user_ticks % 2 == 0) // 每两个实际IRQ尝试一次round-robin。
        return schedule(REASON_TIMER); // 没有其他可运行者时恢复同一个进程。
    return airi_user_resume(f); // 也不能跳过坏返回检查。
}
struct airi_trap_frame *airi_user_trap(struct airi_trap_frame *f) { // 内核仍不可抢占，只在完整用户陷入边界处理进程状态。
    if (f->vector == 129 && f->cs == 8 && (!session_active || session_idle) && current && current->state == USER_RUNNABLE) { // 内核启动一组已经准备好的用户。
        kernel_return = f; // 启动栈的继续位置一直保留。
        session_idle = false; // 当前int129重新登记了仍在启动栈上的继续位置。
        session_active = true; // 之后的CS3陷入有会话归属。
        return select_user(current); // 按目标根和TSS进入首个用户。
    }
    if (!session_active || !current || current->state != USER_RUNNING || (f->cs & 3) != 3) // 内核错误不能伪装为用户故障。
        airi_panic("user trap without a running process"); // 维持故障边界。
    collect_pending(); // 现在确实在另一条内核栈上，回收已经离开的孩子。
    current->frame = f; // 系统调用与故障都以当前实际现场为继续点。
    if (f->vector != 128) { // 用户CPU故障只结束拥有者。
        current->fault_error = f->error; // 保存权限故障证据。
        airi_log("[USER_FAULT] pid=%u vector=%llu error=0x%llx rip=0x%llx cr2=0x%llx", current->pid, (unsigned long long)f->vector, (unsigned long long)f->error, (unsigned long long)f->rip, (unsigned long long)(f->vector == 14 ? airi_trap_fault_address() : 0)); // PF才读取本次CR2。
        return exit_current(-(128 + (int)f->vector)); // 自有故障编码，不宣称Linux信号ABI。
    }
    if (f->rax == 1 || f->rax == 10 || f->rax == 15 || f->rax == 16) { // 同步文件与流的完整副本边界。
        bool write = f->rax == 1 || f->rax == 16; // 输出到流或文件。
        bool positioned = f->rax == 15 || f->rax == 16; // 显式偏移不支持阻塞流。
        long result = file_transfer(current, f->rdi, f->rsi, (size_t)f->rdx, write, positioned, f->r10); // 先实际尝试一次。
        if (result == -11 && !positioned && !airi_fd_nonblocking(&current->files, (unsigned)f->rdi)) { // 阻塞流暂不可完成，用户不能提前看到EAGAIN。
            current->io = write ? IO_WRITE : IO_READ; // 保存需要重试的操作种类。
            current->io_fd = (unsigned)f->rdi; // 此时FD已由file_transfer完整验证。
            current->io_address = f->rsi; // 用户页继续归此进程所有，不借内核局部buffer。
            current->io_bytes = f->rdx; // 一次最多256字节，已经验证跨度。
            current->state = USER_IO_WAIT; // 条件和数据先登记，再移出可运行集合。
            ++io_block_count; // 实际阻塞计数。
            airi_log("[WAIT_REGISTER] pid=%u operation=%s fd=%u", current->pid, write ? "write" : "read", current->io_fd); // 普通系统调用上下文记录。
            return schedule(REASON_IO); // 选择别的用户或进入真正内核空闲等待。
        }
        f->rax = (uint64_t)result; // 非阻塞仍把EAGAIN交给用户，其他结果直接返回。
    } else if (f->rax == 2) // 退出后不再恢复此用户指令。
        return exit_current((int)(f->rdi & 255)); // 普通退出码0–255。
    else if (f->rax == 3) // getpid查询当前身份。
        f->rax = current->pid; // 槽复用不改变已运行PID语义。
    else if (f->rax == 4) // 显式yield让出一次时间片。
        return schedule(REASON_YIELD); // 和真实timer抢占分开计数。
    else if (f->rax == 5) // 单调tick，不是墙钟。
        f->rax = airi_timer_ticks(); // IRQ在用户执行期间更新。
    else if (f->rax == 6) // fork复制所有用户页与返回现场。
        f->rax = (uint64_t)fork_current(f); // 父得到子PID，孩子自己的frame已经设为0。
    else if (f->rax == 7) { // exec失败保留旧现场，成功已把f替换成新入口。
        long result = exec_current(f); // 名称复制与新空间事务在不可抢占内核完成。
        if (result < 0) // 只有失败才向旧用户调用返回错误。
            f->rax = (uint64_t)result; // PID和旧地址空间仍保持。
        return airi_user_resume(f); // 成功恢复新_start，失败恢复旧调用下一条指令。
    } else if (f->rax == 8) // wait可直接回收或真的阻塞。
        return wait_current(f); // 结果恢复到父同一次调用之后。
    else if (f->rax == 9) // 申请零填充用户页。
        f->rax = (uint64_t)allocate_user((size_t)f->rdi); // 不成功不前移堆游标。
    else if (f->rax == 11) { // open只消费内核持有的短路径副本。
        char path[32]; // 与平坦目录名容量一致。
        bool copied = false; // 必须看到终止零才可交给文件层。
        for (size_t i = 0; i < sizeof(path); ++i) { // 逐字节检查，不能strlen用户指针。
            if (!copy_user(current, &path[i], f->rdi + i, 1, false)) // 未映射或内核地址拒绝。
                break; // 不发生创建或截断。
            if (!path[i]) { // 有界NUL字符串完成。
                copied = true; // 副本可在当前内核调用中使用。
                break; // 不额外读取终止零后面的用户页。
            }
        }
        f->rax = copied && f->rsi <= 31 ? (uint64_t)airi_fd_open(&current->files, path, (unsigned)f->rsi) : (uint64_t)-14ll; // 错误路径不变更文件。
    } else if (f->rax == 12) // close撤销当前表项引用。
        f->rax = (f->rdi < AIRI_FD_COUNT ? (uint64_t)airi_fd_close(&current->files, (unsigned)f->rdi) : (uint64_t)-9ll); // 不删除文件内容。
    else if (f->rax == 13) // dup共享打开对象，标志放在新表项。
        f->rax = (f->rdi < AIRI_FD_COUNT ? (uint64_t)airi_fd_dup(&current->files, (unsigned)f->rdi, f->rsi != 0) : (uint64_t)-9ll); // 单CPU内核中同步加引用。
    else if (f->rax == 14) // 仅绝对定位，不实现Linux全部whence。
        f->rax = (f->rdi < AIRI_FD_COUNT ? (uint64_t)airi_fd_seek(&current->files, (unsigned)f->rdi, f->rsi) : (uint64_t)-9ll); // 共享打开对象的偏移随之变化。
    else if (f->rax == 17 || f->rax == 18) { // 日志append与read不是通用文件ABI。
        uint8_t buffer[480]; // payload上限与磁盘格式一致，设备不直接访问用户页。
        long result; // 实际长度、sequence或负错误。
        if (f->rax == 17) { // RDI输入地址，RSI长度。
            if (!f->rsi || f->rsi > sizeof(buffer)) // 不创建空记录或无界内核副本。
                result = -22; // -EINVAL。
            else if (!copy_user(current, buffer, f->rdi, (size_t)f->rsi, false)) // 完整复制后才可能产生磁盘副作用。
                result = -14; // -EFAULT。
            else { // 设备同步等待临时开IRQ，但不会在CS0切用户。
                airi_log("[STORAGE_REQUEST] pid=%u operation=append bytes=%llu wait=kernel", current->pid, (unsigned long long)f->rsi); // 诚实区分同步内核等待与后续用户阻塞。
                result = airi_storage_append(buffer, (size_t)f->rsi); // 成功已完成数据和提交同步。
            }
        } else { // RDI索引，RSI用户输出，RDX容量。
            if (f->rdi >= 64 || f->rdx > sizeof(buffer)) // 完整64位索引不能截断成其他记录。
                result = -22; // -EINVAL。
            else if (!user_access(current, f->rsi, (size_t)f->rdx, true)) // 先验证完整用户输出范围。
                result = -14; // -EFAULT，不进行设备I/O。
            else { // 仅将校验通过的实际长度复制到用户。
                result = airi_storage_read((unsigned)f->rdi, buffer, (size_t)f->rdx); // 设备数据和commit重新核对。
                if (result > 0 && !copy_user(current, buffer, f->rsi, (size_t)result, true)) // 当前映射不应变化。
                    airi_panic("validated storage output mapping changed"); // 内部不变量破坏明确停止。
            }
        }
        f->rax = (uint64_t)result; // 同一次用户调用取得精确结果。
    } else if (f->rax == 19) // 已恢复前缀数值查询。
        f->rax = (uint64_t)airi_storage_count(); // 缺设备或不确定状态返回负数。
    else if (f->rax == 20) { // pipe原子创建并返回两个整数，不暴露内部环。
        unsigned ends[2]; // 内核副本，发布前用户输出已验证。
        long result; // 0或负错误。
        if (f->rsi > 1) // 明确接受阻塞0与非阻塞1，其他参数拒绝。
            result = -22; // -EINVAL。
        else if (!user_access(current, f->rdi, sizeof(ends), true)) // 坏输出不能创建无法交给用户的FD。
            result = -14; // -EFAULT。
        else { // 没有半次FD分配的用户可见状态。
            result = airi_fd_pipe(&current->files, ends, f->rsi != 0); // 两端和缓冲都齐后才成功。
            if (!result && !copy_user(current, ends, f->rdi, sizeof(ends), true)) // 映射在本次调用中不应变化。
                airi_panic("validated pipe output mapping changed"); // 明确内核不变量。
        }
        f->rax = (uint64_t)result; // 失败输出保持原值。
    } else if (f->rax == 21) // 当前就绪条件查询，不保存通知事件。
        f->rax = f->rdi < AIRI_FD_COUNT ? (uint64_t)airi_fd_ready(&current->files, (unsigned)f->rdi) : (uint64_t)-9ll; // 完整64位编号先限制。
    else if (f->rax == 22) { // 等待管道条件，有限tick超时，0表示只查询。
        long result; // 事件掩码、0超时或负错误。
        if (f->rdi >= AIRI_FD_COUNT || !f->rsi || (f->rsi & ~3ull) || f->rdx > 500) // 只接受READ/WRITE请求，最多五秒。
            result = -22; // -EINVAL。
        else { // 先看当前条件，避免已经满足还去睡眠。
            result = airi_fd_ready(&current->files, (unsigned)f->rdi); // FD错误原样返回。
            if (result >= 0) // 负错误不参与位运算。
                result &= (long)f->rsi | AIRI_READY_HUP | AIRI_READY_ERROR; // 关闭与错误始终报告。
            if (!result && f->rdx) { // 条件未满足且愿意有限等待。
                current->io = IO_READY; // 不读写任何字节，只重查条件。
                current->io_fd = (unsigned)f->rdi; // 属于此进程的稳定表项。
                current->io_events = (unsigned)f->rsi; // 保存订阅条件。
                current->io_deadline = airi_timer_ticks() + f->rdx; // 真实时钟截止，不靠循环次数。
                current->state = USER_IO_WAIT; // 原frame已保存在进程中。
                ++io_block_count; // 这次调用确实阻塞。
                airi_log("[WAIT_REGISTER] pid=%u operation=ready fd=%u", current->pid, current->io_fd); // 由普通内核上下文记录。
                return schedule(REASON_IO); // 返回到同一次wait调用之后。
            }
        }
        f->rax = (uint64_t)result; // 立即就绪、纯查询、错误统一返回。
    }
    else if (f->rax == 23) // 固定注册缓冲申请，不返回内核地址。
        f->rax = (uint64_t)airi_async_buffer_alloc(current->pid); // 正handle或负错误。
    else if (f->rax == 24 || f->rax == 25) { // 注册缓冲与用户范围之间同步复制。
        uint8_t buffer[512]; // 设备不访问这个内核栈副本，之后另由请求单元持有。
        bool put = f->rax == 24; // put用户输入，get用户输出。
        long result; // 0或明确错误，字节数由已验证请求长度决定。
        if (f->rdx > sizeof(buffer)) // 完整64位长度先限制。
            result = -22; // -EINVAL。
        else if (!user_access(current, f->rsi, (size_t)f->rdx, !put)) // 输出必须可写，输入只需可读。
            result = -14; // -EFAULT，没有引擎副作用。
        else if (put) { // 输入先复制完再可能覆盖注册对象。
            copy_user(current, buffer, f->rsi, (size_t)f->rdx, false); // 不把用户VA交给资源引擎。
            result = airi_async_buffer_put(current->pid, f->rdi, buffer, (size_t)f->rdx); // BUSY拒绝，尾部清零。
        } else { // get先检查owner、handle和IDLE。
            result = airi_async_buffer_get(current->pid, f->rdi, buffer, (size_t)f->rdx); // 不收集CQ不能读取BUSY。
            if (!result && !copy_user(current, buffer, f->rsi, (size_t)f->rdx, true)) // 映射已完整验证且当前不可抢占。
                airi_panic("validated registered-buffer output changed"); // 内部错误不能静默复制。
        }
        f->rax = (uint64_t)result; // 用户自己判断资源状态。
    } else if (f->rax == 26) // 注册buffer释放不等同用户堆free。
        f->rax = (uint64_t)airi_async_buffer_free(current->pid, f->rdi); // 完整代数句柄不截断。
    else if (f->rax == 27) { // 提交描述全部复制后才接受进SQ。
        struct airi_io_submission submission; // 固定16字节无指针记录。
        if (f->rdi != AIRI_IO_READ && f->rdi != AIRI_IO_WRITE && f->rdi != AIRI_IO_FLUSH) // 完整64位操作先验证，不能截断成合法编号。
            f->rax = (uint64_t)-22ll; // 格式错误无CQ。
        else if (!copy_user(current, &submission, f->rdx, sizeof(submission), false)) // sector和cookie都由当前用户副本给出。
            f->rax = (uint64_t)-14ll; // 未接受，没有缓冲所有权变化。
        else // 已复制的数值交给资源引擎，不借用户提交结构。
            f->rax = (uint64_t)airi_async_submit(current->pid, (unsigned)f->rdi, f->rsi, &submission); // 0仅接收，I/O结果以后由CQ给。
    } else if (f->rax == 28) { // CQ收集可以非阻塞或保持原调用等待。
        struct airi_io_completion completion; // 先到内核数值副本。
        long result; // 0交出一条CQ，-11空，其他负错误。
        if (f->rsi > 1) // 只接受0不等、1阻塞。
            result = -22; // -EINVAL。
        else if (!user_access(current, f->rdi, sizeof(completion), true)) // 必须在消费CQ之前验证完整用户输出。
            result = -14; // 不丢完成、信用与buffer状态。
        else { // 当前owner的CQ与user输出身份明确。
            result = airi_async_pop(current->pid, &completion); // 可能设备很快，已有CQ直接返回。
            if (!result && !copy_user(current, &completion, f->rdi, sizeof(completion), true)) // 单线程映射稳定。
                airi_panic("validated completion output changed"); // 不能消费后隐瞒复制失败。
            if (result == -11 && f->rsi) { // 空CQ且选择阻塞，保留这同一次调用。
                current->io = IO_ASYNC; // poll_waiters只收集，设备推进在启动栈。
                current->io_address = f->rdi; // 用户输出页仍由进程拥有。
                current->io_bytes = sizeof(completion); // 固定ABI长度，不借C局部对象。
                current->state = USER_IO_WAIT; // 先保存条件和输出，再撤销运行资格。
                ++io_block_count; // 记录真实CQ等待，不强迫快设备也假装阻塞。
                airi_log("[WAIT_REGISTER] pid=%u operation=cq", current->pid); // 等设备完成后原调用继续。
                return schedule(REASON_IO); // 可切其他用户或进入启动服务空闲。
            }
        }
        f->rax = (uint64_t)result; // 具体I/O错误在completion.result，不是这层0。
    } else if (f->rax == 29) { // 有界只读状态供用户与未来呈现消费者观察。
        struct airi_io_state state; // 不含内核地址或用户buffer指针。
        long result; // 当前轮次状态或负错误。
        if (!user_access(current, f->rdi, sizeof(state), true)) // 输出先验证，快照不会消耗资源。
            result = -14; // -EFAULT。
        else { // 当前PID由内核决定，用户不能查询或修改另一个owner。
            result = airi_async_state(current->pid, &state); // 包括同PID旧轮次待收集的ACTIVE数量。
            if (!result && !copy_user(current, &state, f->rdi, sizeof(state), true)) // 当前映射应保持稳定。
                airi_panic("validated I/O state output changed"); // 明确内部失败。
        }
        f->rax = (uint64_t)result; // 用户看到的只是本次时刻数值。
    }
    else if (f->rax == 30) { // 目录只返回有界文本，不暴露ELF或内核对象地址。
        char names[256]; size_t used = 0; // 当前固定目录完整小于此上限。
        for (unsigned i=0; i<sizeof(executables)/sizeof(executables[0]); ++i) { // 结果与exec使用同一真实注册表。
            size_t n=airi_strlen(executables[i].name); // 名称包含内部演示目标，用户仍可选择尝试。
            if (used+n+2>sizeof(names)) // 防止以后目录增长造成栈覆盖。
                airi_panic("program catalog exceeds bounded ABI"); // 维护者必须扩大显式上限。
            airi_memcpy(names+used,executables[i].name,n); used+=n; // 复制有效名称前缀。
            names[used++]='\n'; // 每行一个名字，界面自己呈现。
        }
        names[used]=0; // 查询返回含NUL的完整文本，不返回截断目录。
        if (f->rsi < used+1) // 小容量不能写一半。
            f->rax=(uint64_t)-28ll; // -ENOSPC。
        else if (!copy_user(current,names,f->rdi,used+1,true)) // 验证实际输出跨度。
            f->rax=(uint64_t)-14ll; // -EFAULT。
        else // 完整复制后才发布有效长度。
            f->rax=used; // 不含末尾NUL，与strlen一致。
    }
    else if (f->rax == 31) // 只归还调用者自己的动态页面，不能释放ELF或初始栈。
        f->rax = (uint64_t)release_user(f->rdi, f->rsi); // 负参数错误保持全部原映射。
    else if (f->rax == 32) { // 单调tick睡眠，复用持久等待现场而非用户忙等。
        uint64_t now = airi_timer_ticks(); // 截止以真实PIT事件计数为基准。
        if (f->rdi > 6000) // 一次最多6000tick（名义六十秒），完整64位参数先验证。
            f->rax = (uint64_t)-22ll; // -EINVAL，不改变运行状态。
        else if (!f->rdi) // 零长度是立即成功，不登记等待。
            f->rax = 0; // 和主动yield分开。
        else if (now > UINT64_MAX - f->rdi) // 不让截止计数回绕成已经过期。
            f->rax = (uint64_t)-75ll; // -EOVERFLOW。
        else { // 请求完整有效，保存条件后再退出可运行集合。
            current->io = IO_SLEEP; // poll_waiters不再把它当FD操作。
            current->io_fd = current->io_events = 0; // 清与此次请求无关的旧数值。
            current->io_address = current->io_bytes = 0; // sleep不借用用户指针。
            current->io_deadline = now + f->rdi; // 保留绝对截止，不在每次轮询延长。
            current->state = USER_IO_WAIT; // 此前已经保存实际系统调用frame。
            ++io_block_count; // 记录真正阻塞，而非返回0的空操作。
            airi_log("[WAIT_REGISTER] pid=%u operation=sleep ticks=%llu", current->pid, (unsigned long long)f->rdi); // 普通调用栈记录，不在IRQ打印。
            return schedule(REASON_IO); // 其他用户继续；全阻塞时启动栈hlt等PIT。
        }
    }
    else // 尚未提供的接口都明确拒绝。
        f->rax = (uint64_t)-38ll; // -ENOSYS。
    poll_waiters(); // 文件/流修改完成后立即重查其他等待者，不等用户轮询。
    return airi_user_resume(f); // 普通调用继续同一用户，仍检查RSP与标志。
}
struct user_result { int code; unsigned ticks, switches, blocks; uint64_t fault_error; unsigned io_blocks, io_wakes, idle_halts; }; // 启动任务只取得数值，不借已回收控制块。
static int launch(const uint8_t *image, size_t bytes, const char *name, bool elf, struct user_result *result) { // 可用于启动演示或串口命令，程序失败不等于内核失败。
    if (session_active) // 当前只有一个由启动栈收拢的用户会话。
        return -16; // -EBUSY，不能覆盖已有kernel_return。
    if (next_pid == UINT32_MAX) // 不能把回绕后的0当合法新owner。
        return -75; // -EOVERFLOW，尚未分配任何资源。
    struct airi_memory_stats before, after; // 包含根、所有孩子和内核栈。
    airi_memory_snapshot(&before); // 在任何资源分配前取基准。
    struct user_process *root = unused(); // 上次会话已回收的空槽。
    if (!root) // 仍有未回收身份就明确失败。
        return -11; // -EAGAIN。
    int error = elf ? prepare_elf(root, image, bytes, name, true) : (prepare_flat(root, image, bytes) ? 0 : -12); // ELF保留格式与资源错误之别。
    if (error) { // 失败构造也必须完整恢复资源。
        airi_memory_snapshot(&after); // 不能只看错误码忽略泄漏。
        if (!airi_memory_same_usage(&before, &after) || airi_files_open_count()) // 半份新映像不应改变空闲数量。
            airi_panic("failed user launch leaked resources"); // 内部错误与普通坏文件分开。
        return error; // 调用者可以继续命令，不停机。
    }
    airi_fd_init(&root->files); // 初始stdout/stderr归本次根进程持有。
    bool direct_shell = name && airi_strlen(name)==2 && !airi_memcmp(name,"sh",2); // init自身不读UART，只监督自己的shell孩子。
    if (direct_shell && !airi_fd_attach_input(&root->files)) // 根的新FD0必须为空。
        airi_panic("cannot attach direct shell input"); // 初始化不变量不成立。
    root->pid = next_pid++; // 完整准备后才取得运行身份。
    int async_error = airi_async_open(root->pid); // 完整映像准备后才登记新进程资源上下文。
    if (async_error) { // 未向用户发布，此时根与栈不活动。
        cleanup(root); // 回滚页面、FD与内核栈，尚未拥有异步owner。
        airi_memory_snapshot(&after); // 失败不能留下普通内存泄漏。
        if (!airi_memory_same_usage(&before, &after)) // 静态池外的资源同样核对。
            airi_panic("failed async owner launch leaked memory"); // 内部不变量明确停止。
        return async_error; // 应用可见普通资源失败。
    }
    root->async_owned = true; // cleanup和exit之后负责释放。
    place_frame(root); // 寄存器值进入本进程真实内核栈。
    root->state = USER_RUNNABLE; // 发布首次运行资格。
    root_pid = root->pid; // 会话根身份不会被孩子exit覆盖。
    supervisor_pid = name && airi_strlen(name)==4 && !airi_memcmp(name,"init",4) ? root_pid : 0; // 只在明确run init的模式授予前台重启资格。
    input_pid = direct_shell ? root_pid : 0; // 直接shell已接入；init的孩子在成功exec时取得资格。
    if (supervisor_pid) // 普通演示的启动路径和观察标记保持。
        airi_log("[INIT_SESSION] pid=%u uart=exclusive", supervisor_pid); // PID不固定为1，身份不等于槽号。
    session_result = 0; // 不继承上次结果。
    timer_switches = 0; // 这次会话实际抢占计数。
    wait_blocks = 0; // 这次会话实际阻塞计数。
    current = root; // 内核启动门选择此根。
    io_block_count = io_wake_count = idle_halt_count = 0; // 本次I/O阻塞统计独立于wait孩子。
    session_idle = false; // 首次int129创建新活动会话。
    long code; // 根结束结果由启动现场RAX交回。
    for (;;) { // 每次返回启动栈，先区分暂时服务和整个会话结束。
        code = airi_arch_user_gate(); // 当前架构登记继续现场，返回可能暂服务或整个会话结束。
        if (!session_active) // 只有所有用户结束才离开launch。
            break; // code此时才是根用户真实退出值。
        collect_pending(); // 当前确在启动栈，仍保守跳过current槽。
        for (;;) { // 暂无可运行者时在长期内核根上服务等待条件。
            if (airi_async_work()) // 硬件完成只在长期普通服务栈推进。
                airi_async_pump(); // 一次有界服务，不在IRQ C链内运行。
            poll_waiters(); // 可复制用户结果和记录日志，不在IRQ C链里做。
            struct user_process *next = NULL; // 每次重查后重新选择。
            for (unsigned n = 1; n <= PROCESS_COUNT; ++n) // 轮转保持公平，不总挑槽0。
                if (processes[(slot(current) + n) % PROCESS_COUNT].state == USER_RUNNABLE) { // 等待者可能刚被唤醒。
                    next = &processes[(slot(current) + n) % PROCESS_COUNT]; // 保存控制块，不提前切根。
                    break; // 首个可运行者获得下一次int129。
                }
            if (next) { // 服务后已有用户可继续。
                if (service_from_timer && next != current) // 只有来自真实IRQ的身份改变才计抢占。
                    ++timer_switches; // 不把用户主动阻塞算成IRQ切换。
                current = next; // int129随后正式设置TSS与目标根。
                break; // 回外层重新登记新的启动继续现场。
            }
            ++idle_halt_count; // 统计真正没有可运行用户后执行的空闲等待。
            airi_arch_idle(); // CPU真正空闲等PIT，醒后仍在启动服务栈。
        }
    }
    *result = (struct user_result){(int)code, root->user_ticks, timer_switches, wait_blocks, root->fault_error, io_block_count, io_wake_count, idle_halt_count}; // 回收前复制必要数值。
    for (unsigned i = 0; i < PROCESS_COUNT; ++i) // 当前不在任何用户内核栈上。
        if (processes[i].state != USER_UNUSED) // wait已回收槽不重复处理。
            cleanup(&processes[i]); // 收拢剩余僵尸和未被父wait的资源。
    current = NULL; // 不保留已清掉的身份指针。
    supervisor_pid = input_pid = 0; // 有限会话结束后不能把授权留给下次运行。
    kernel_return = NULL; // 下次会话重新登记。
    airi_async_drain(); // 所有owner已关闭，内核仍须结束无主ACTIVE并核对静态池完全回收。
    airi_memory_snapshot(&after); // 全部页和栈回收后验证。
    if (!airi_memory_same_usage(&before, &after) || airi_files_open_count()) // 用户程序退出码可以任意，但内核资源必须恢复。
        airi_panic("user process session leaked memory"); // 不把泄漏当成应用错误跳过。
    return 0; // 表示已完整启动与收拢，程序结果另在result.code。
}
static void run_one(const uint8_t *image, size_t bytes, const char *name, int expected, bool elf, bool require_ticks, bool require_scheduler) { // 作者启动演示使用具体预期，不控制普通命令的合法退出码。
    struct user_result result; // 会话结束只消费数值副本。
    if (launch(image, bytes, name, elf, &result) || result.code != expected || (require_ticks && result.ticks < 2) || (expected == -142 && result.fault_error != 5) || (require_scheduler && (!result.switches || !result.blocks))) // 同时验证真正执行、故障、抢占和等待。
        airi_panic("user session ended without expected semantics"); // 固定演示有误才拒绝发布READY。
    if (require_scheduler) // 实际多用户会话单独报告行为证据。
        airi_log("[PROCESS_DEMO_OK] fork=copy exec=transaction wait=blocked timer_switches=%u wait_blocks=%u reclaimed=1", result.switches, result.blocks); // 计数来自真实IRQ与状态转换。
}
void airi_user_demo(void) { // 串口主线的真实用户边界与多进程回归。
    airi_files_init(); // 用户会话开始前创建只读seed与空RAM目录，仅启动一次。
    run_one(airi_user_probe_start, (size_t)((uintptr_t)airi_user_probe_end - (uintptr_t)airi_user_probe_start), NULL, 0, false, true, false); // 原有非法指针与用户IRQ验证。
    run_one(airi_user_bad_start, (size_t)((uintptr_t)airi_user_bad_end - (uintptr_t)airi_user_bad_start), NULL, -141, false, false, false); // 用户cli只杀当前身份。
    run_one(airi_user_load_bad_start, (size_t)((uintptr_t)airi_user_load_bad_end - (uintptr_t)airi_user_load_bad_start), NULL, -142, false, false, false); // USER保护PF。
    run_one(airi_user_rsp_bad_start, (size_t)((uintptr_t)airi_user_rsp_bad_end - (uintptr_t)airi_user_rsp_bad_start), NULL, -22, false, false, false); // 坏返回栈不击穿内核。
    run_one(airi_hello_elf_start, (size_t)((uintptr_t)airi_hello_elf_end - (uintptr_t)airi_hello_elf_start), "hello", 0, true, false, false); // 独立C和自有用户库。
    run_one(airi_process_elf_start, (size_t)((uintptr_t)airi_process_elf_end - (uintptr_t)airi_process_elf_start), "processes", 0, true, false, true); // 真实多进程复制、抢占、阻塞与回收。
    run_one(airi_files_elf_start, (size_t)((uintptr_t)airi_files_elf_end - (uintptr_t)airi_files_elf_start), "files", 0, true, false, false); // 文件内容、共享偏移与exec引用回收。
    run_one(airi_pipes_elf_start, (size_t)((uintptr_t)airi_pipes_elf_end - (uintptr_t)airi_pipes_elf_start), "pipes", 0, true, false, false); // 真实FIFO、背压、关闭和fork共享。
    struct user_result waits_result; // 数值证据不借已回收控制块。
    if (launch(airi_waits_elf_start, (size_t)((uintptr_t)airi_waits_elf_end - (uintptr_t)airi_waits_elf_start), "waits", true, &waits_result) || waits_result.code || waits_result.io_blocks < 4 || waits_result.io_wakes < 4 || !waits_result.idle_halts) // 读、写、空闲与纯计算并行超时四种实际等待均须发生。
        airi_panic("blocking I/O did not preserve waiting semantics"); // 不把用户循环重试当内核阻塞。
    airi_log("[WAIT_DEMO_OK] blocked=%u woken=%u idle_halts=%u", waits_result.io_blocks, waits_result.io_wakes, waits_result.idle_halts); // 统计来自真实状态变化。
    struct user_result async_result; // 快设备可能立即有CQ，真实阻塞次数按观察记录。
    if (launch(airi_async_elf_start, (size_t)((uintptr_t)airi_async_elf_end - (uintptr_t)airi_async_elf_start), "async", true, &async_result) || async_result.code) // 正常和受限设备由用户程序明确分支。
        airi_panic("user asynchronous lifecycle demo failed"); // 不拿submit0冒充I/O成功。
    airi_log("[ASYNC_USER_DEMO_OK] blocked=%u woken=%u switches=%u pools=reclaimed", async_result.io_blocks, async_result.io_wakes, async_result.switches); // 静态池已由launch排空并核对。
    airi_log("[USER_DEMO_OK] ring=3 syscall=int80 bad_pointers=2 user_gp=13 user_pf=14 bad_rsp=rejected reclaimed=1"); // 完整会话结束，仍回串口命令。
}

bool airi_user_command(struct airi_console *console, const char *command) { // 系统命令消费者独立于图形主题扩展。
    size_t bytes = airi_strlen(command); // 先取得有效长度，固定前缀比较不能越过短字符串。
    if (bytes == 8 && !airi_memcmp(command, "programs", 8)) { // 显示当前初始ELF目录。
        airi_console_print(console,"programs: registered ELF images"); // 表头不硬编码另一套程序名。
        for (unsigned i=0; i<sizeof(executables)/sizeof(executables[0]); ++i) // 与exec共享真实目录，不维护第二份静态名称。
            airi_console_print(console, "%s", executables[i].name); // broken是明确损坏的演示输入。
        return true; // 已处理，不再交给图形扩展。
    }
    if (bytes < 4 || airi_memcmp(command, "run ", 4)) // 只接受明确的run空格前缀。
        return false; // 其他命令继续原分派链。
    const char *name = command + 4; // 只在本次调用期借控制台的命令副本。
    const struct executable *image = find_program(name); // 目前目录名不是通用文件系统路径。
    if (!image) { // 错别字不影响内核继续输入。
        airi_console_print(console, "program not found: %s", name); // 给出当前名称。
        return true; // 请求已得到明确结果。
    }
#ifdef AIRI_GRAPHICAL_CONSOLE // 当前图形终端尚未接入用户TTY，不能让窗口命令陷入不可见输入等待。
    if ((bytes==6 && !airi_memcmp(name,"sh",2)) || (bytes==8 && !airi_memcmp(name,"init",4))) { // shell及其常驻监督模式都需要实际UART输入。
        airi_console_print(console,"User shell uses UART: start ./airios run, then run sh."); // 给出实际可操作入口。
        return true; // 图形桌面立即继续接收输入。
    }
#endif // 纯串口主线没有此限制。
    struct user_result result; // 子进程都结束后取得数值退出状态。
    int error = launch(image->start, (size_t)((uintptr_t)image->end - (uintptr_t)image->start), name, true, &result); // 同步执行并完整回收，不借回调局部对象给用户。
    if (error) // 例如坏ELF返回-8，不是PANIC。
        airi_console_print(console, "cannot load %s: error=%d", name, error); // 串口详细日志还给出解析原因。
    else // 程序可以合法返回非零，例如worker的23。
        airi_console_print(console, "program %s exited %d", name, result.code); // 不把应用结果当成内核停止。
    return true; // GUI是否存在都不影响服务执行。
}
```

</details>

<a id="heap-grow-tests"></a>
## 在已有对象还活着时验证两种扩展

主机检查继续用同一份heap.c，arena模拟已经映射的连续内存。原先2000步交错分配/归还保留；它们结束并归还全部对象后，检查从arena前512字节重新开始：占满480字节payload，扩展第二段，再从新段申请400字节。原对象与新对象都写入不同字节，以核对拒绝有洞、过短、未对齐和溢出范围后内容未变。

两对象归还后扩展空末块，应该只有一个块头；再次占满并扩展，则应出现独立新末块。随后归还，整个arena仍能合并成一块。资源快照夹具还分别演示正常缓存增长、堆外页面未归还、活对象未归还三种情况。更新 `tests/unit/heap_test.c`。这些宿主检查验证对象算法，真实映射与访问另由guest演示完成。

<!-- write: tests/unit/heap_test.c -->
```c
#include <assert.h> // 失败直接返回非零，不依赖文字判断。
#include <stdio.h> // 宿主输出测试结果。
#include <airi/memory.h> // 对象借用与backing缓存使用不同的资源核对。
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
    assert(airi_heap_init(&heap, arena, 512)); // 所有旧对象已归还，重新用同一arena前512字节建立小堆。
    uint8_t *held = airi_heap_alloc(&heap, 480); // 占满末块，使扩展走新增块头分支。
    assert(held && airi_heap_used_bytes(&heap) == 480); // 活payload不含32字节块头。
    airi_memset(held, 0x6b, 480); // 新backing接入不能移动或覆盖此对象。
    assert(airi_heap_extend(&heap, arena + 512, 512)); // 第二段虚拟上精确紧邻旧右端。
    uint8_t *added = airi_heap_alloc(&heap, 400); // 从新增空块取得另一个对象。
    assert(added && added != held && airi_heap_used_bytes(&heap) == 880); // 两个对象各自拥有payload。
    airi_memset(added, 0xa3, 400); // 后续失败与合并不能改变活内容。
    assert(!airi_heap_extend(&heap, arena + 1536, 512)); // 有洞的新范围不能进入块链。
    assert(!airi_heap_extend(&heap, arena + 1024, 32)); // 容不下块头加最小payload。
    assert(!airi_heap_extend(&heap, arena + 1024, 511)); // 长度必须保持16字节对齐。
    assert(!airi_heap_extend(&heap, arena + 1024, SIZE_MAX & ~(size_t)15)); // 巨大右端不能回绕。
    assert(heap.bytes == 1024 && airi_heap_used_bytes(&heap) == 880); // 拒绝没有悄悄发布容量。
    for (unsigned i = 0; i < 480; ++i) // 扩展前的对象所有字节仍在原地址。
        assert(held[i] == 0x6b); // 不只检查边界字节。
    for (unsigned i = 0; i < 400; ++i) // 新对象在失败扩展后也继续有效。
        assert(added[i] == 0xa3); // 不能部分覆盖已有链。
    assert(airi_heap_free(&heap, held) && airi_heap_free(&heap, added)); // 跨旧新backing边界的块可合并。
    assert(airi_heap_extend(&heap, arena + 1024, 1024)); // 空末块直接延长，不增加另一个块头。
    assert(airi_heap_free_bytes(&heap) == 2048 - 32 && !airi_heap_used_bytes(&heap)); // 只有一个初始块头，活对象为零。
    held = airi_heap_alloc(&heap, 2016); // 用跨扩展边界的大对象占满当前范围。
    assert(held && airi_heap_extend(&heap, arena + 2048, 2048)); // 已借出尾块仍走独立新块分支。
    assert(airi_heap_used_bytes(&heap) == 2016 && airi_heap_free_bytes(&heap) == 2016); // 旧对象容量不会随backing增加。
    assert(airi_heap_free(&heap, held) && airi_heap_free_bytes(&heap) == sizeof(arena) - 32); // 最后恢复一个完整空闲块。
    struct airi_memory_stats before = {.pages_free=100, .heap_bytes=65536, .heap_used=16}; // 已有一个活对象的资源快照。
    struct airi_memory_stats after = {.pages_free=84, .heap_bytes=131072, .heap_used=16}; // 十六个页仅从页池转入堆缓存。
    assert(airi_memory_same_usage(&before, &after)); // 空闲缓存增长不会冒充对象泄漏。
    --after.pages_free; // 另有一个不属于堆缓存的页面没有归还。
    assert(!airi_memory_same_usage(&before, &after)); // 真实页泄漏仍须被拒绝。
    ++after.pages_free; // 还原页面数，单独改变活对象容量。
    after.heap_used = 32; // 新活payload未归还，不能被缓存容量掩盖。
    assert(!airi_memory_same_usage(&before, &after)); // 对象泄漏与backing缓存分开判断。
    puts("PASS heap: alignment, overflow, exhaustion, invalid free, coalescing, 2000 lifetime steps, contiguous extension, cache vs live usage"); // 全部实际断言通过。
    return 0; // 正常退出宿主。
}
```

启动检查需在当前READY之前看见新的HEAP_CAPACITY_OK，保证不是只链接了扩展函数。更新 `scripts/boot.py` 的正常启动分支，故障配置仍按原有停止位置核对；缺内存、未处理异常等早期失败不要求经过堆扩展。

<details>
<summary>scripts/boot.py 的完整终点</summary>

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
    disk = work / 'disk.raw' # 每个运行目录只使用项目自己的独立实验盘。
    if disk.is_symlink(): # 拒绝把实验写入重定向到外部文件。
        raise RuntimeError('Experiment disk cannot be a symlink') # 不跟随未知目标。
    if not disk.exists(): # 保留已有实验盘用于后续重启恢复观察。
        with disk.open('xb') as stream: # 排他创建，不覆盖竞争产生的文件。
            stream.truncate(8 * 1024 * 1024) # 稀疏零文件，固定8MiB。
    if not disk.is_file() or disk.stat().st_size != 8 * 1024 * 1024: # 当前设备容量固定，未知文件不交给guest写。
        raise RuntimeError('Expected an 8 MiB project experiment disk') # 要求显式处理错误，不自动truncate旧文件。
    drive = f'if=none,id=airi-disk,format=raw,file={disk},cache=writeback' # QEMU后端可缓存，持久化需客体FLUSH。
    if profile.removeprefix('desktop-') == 'block-readonly': # 故障演示使用真实只读设备特征。
        drive += ',readonly=on' # 不修改宿主文件的权限设置。
    if profile.removeprefix('desktop-') != 'block-missing': # 缺设备配置故意不接实验盘。
        cmd += ['-drive', drive, '-device', 'virtio-blk-pci,drive=airi-disk,disable-modern=on,vectors=0'] # 固定legacy与无MSI-X布局，现代接口另行学习。
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
    expected='[AIRI_PANIC] required Limine memory map is missing' if case=='missing-memory' else '[AIRI_P3_READY]' if case == 'block-timeout' else '[AIRI_P4_READY]' if case == 'block-readonly' else '[AIRI_P5_READY]' # 正常等完整异步阶段，受限设备只发布实际可用能力。
    if case == 'block-missing': # 缺盘是明确设备初始化失败，不能继续宣称存储可用。
        expected = '[AIRI_PANIC] virtio block initialization failed' # 错误码另外核对。
    if case in ('trap-fatal', 'stack-guard'): # 未处理异常镜像必须进入具体停止路径。
        expected = '[AIRI_PANIC] unhandled CPU exception' # 不以 QEMU 存活推断故障已处理。
    started=time.monotonic()  # 单调秒数作为经过时间起点，不受系统时钟调整影响。
    with (work/'qemu.log').open('w') as errors:  # 主机 QEMU 输出单独存档，with 保证文件最终关闭。
        process=subprocess.Popen(cmd,stdout=errors,stderr=errors)  # 启动后立即取得进程句柄，轮询 guest 标记而非等待进程退出。
        try:  # 受保护区域失败也必须进入后面的资源回收。
            while time.monotonic()-started<timeout:  # 按实际经过秒数限制整次启动等待。
                text=serial.read_text(errors='replace') if serial.exists() else ''  # 日志尚未创建时取空串；暂时不完整的字节以替代字符读取。
                complete_lines = text.splitlines(keepends=True)  # 保留行结束符，完整记录与写到一半的标记可区分。
                if any(line.endswith('\n') and line.startswith(expected) for line in complete_lines):  # 生成器逐行检查，至少一个完整且以预期标记开头才成立。
                    if case in ('missing-memory', 'trap-fatal', 'stack-guard', 'block-missing'):  # 失败配置只允许具体拒绝记录，不能同时宣布启动完成。
                        if case == 'block-missing': # 必须确认真实枚举结果为缺设备。
                            assert '[BLOCK_INIT_ERROR] error=-19' in text # 不能用其他初始化失败充数。
                        if case == 'stack-guard': # 坏栈必须真的转到双重故障的独立栈。
                            assert '[STACK_GUARD_PROBE]' in text and '[EXCEPTION] vector=8 error=0x0 ' in text # 不能把任意PANIC冒充保护页演示。
                        assert '[AIRI_P1_READY]' not in text  # 错误路径不得出现成功标记。
                    else:  # 按当前profile的另一条完整验证路径处理。
                        assert '[EXEC_FAILURE_OK]' in text and '[EXEC_WORKER_OK]' in text and '[EXEC_COMMIT]' in text # 坏映像保持与成功替换都须实际运行。
                        block_marker = '[BLOCK_TIMEOUT_OK]' if case == 'block-timeout' else '[BLOCK_READONLY_OK]' if case == 'block-readonly' else '[BLOCK_DEMO_OK]' # 不同设备条件分别核对真实结果。
                        assert '[BLOCK_READY]' in text and block_marker in text # 完成、拒绝或超时必须符合本配置。
                        if case != 'block-timeout': # 隔离盘不再允许新提交。
                            assert '[BLOCK_NONBLOCK_OK]' in text # begin接受、重叠拒绝和poll收集已运行。
                        journal_marker = '[JOURNAL_SKIPPED]' if case in ('block-timeout', 'block-readonly') else '[JOURNAL_DEMO_OK]' # 没有可写设备时不冒充日志完成。
                        assert journal_marker in text # 正常配置必须真实恢复并替换坏尾。
                        async_marker = '[ASYNC_QUEUE_SKIPPED]' if case in ('block-timeout', 'block-readonly') else '[ASYNC_QUEUE_DEMO_OK]' # 受限设备不冒充可写队列演示。
                        assert async_marker in text # 接收、忙与完成身份须符合当前设备。
                        storage_marker = '[STORAGE_SKIPPED]' if case == 'block-timeout' else '[STORAGE_RECOVERED]' # 只读仍可恢复，隔离盘不能提供空日志。
                        assert storage_marker in text # 正式区必须实际扫描或明确未提供。
                        user_async_marker = '[ASYNC_UNAVAILABLE_OK]' if case == 'block-timeout' else '[ASYNC_READONLY_OK]' if case == 'block-readonly' else '[ASYNC_USER_OK]' # 真正用户程序按设备状态分支。
                        assert user_async_marker in text and '[ASYNC_USER_DEMO_OK]' in text # 资源回收由launch核对。
                        if case not in ('block-timeout', 'block-readonly'): # 可写正常设备还须通过新epoch验证。
                            assert '[ASYNC_EXEC_RESET_OK]' in text # 不以旧映像输出冒充exec。
                        assert '[WAIT_USER_OK]' in text and '[WAIT_IDLE_OK]' in text and '[WAIT_DEMO_OK]' in text # 真阻塞、超时与现场继续均已完成。
                        assert text.index('[WAIT_TIMEOUT_OK]') < text.index('[WAIT_BUSY_DONE]') # 纯计算用户不能饿死等待超时。
                        assert '[PIPE_USER_OK]' in text # 有界流、背压、关闭与fork共享均已实际运行。
                        assert '[FILES_USER_OK]' in text and '[FD_EXEC_OK]' in text # RAM文件和成功exec继承均完成。
                        assert '[PROCESS_FORK_WAIT_OK]' in text and '[PROCESS_DEMO_OK]' in text # 复制、真实阻塞、timer换身份和回收都须完成。
                        assert '[ELF_LOAD]' in text and '[ELF_USER_OK]' in text # 独立ELF段、BSS与用户libc必须真正运行。
                        assert '[USER_DEMO_OK]' in text # 用户输出、指针拒绝和用户故障后内核必须继续。
                        assert '[HEAP_CAPACITY_OK] initial=65536 bytes=1048576 live=0 chunks=16 preserved=1 limit=null' in text # 新映射、活对象保持、上限和缓存归属均实际演示。
                        assert '[TASK_DEMO_OK]' in text # 两个独立栈必须交错并全部回收。
                        assert '[TIMER_DEMO_OK]' in text # 时钟必须在真实IRQ唤醒后完成。
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

</details>

串口mem查询现在要看到实际扩展后的1048576字节。更新 `scripts/console_test.py` 中这一期望，输入数量、后续程序与错误恢复保持。可选图形读取同一数值快照；这里不添加前端路径或新的图形前置。

<details>
<summary>scripts/console_test.py 的完整终点</summary>

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
                        if marker in ('[AIRI_P1_READY]', '[AIRI_P3_READY]', '[AIRI_P4_READY]', '[AIRI_P5_READY]'):  # 阶段READY后还有机制字段，按标记后的空格确定边界。
                            matches=record.startswith(marker+' ')  # 接受带字段的完整READY行，不接受相似标记名称。
                        if matches:  # 本次输出中已取得完整且匹配的状态记录。
                            return text[after:]  # 把本次动作的新结果交回调用者继续核对正文。
                    if process.poll() is not None:  # 运行进程提前退出，即使旧日志有文字也不能当成本次输入成功。
                        raise RuntimeError('Guest exited during console test')  # 报告真实guest生命周期失败。
                    time.sleep(.05)  # 暂停50毫秒给设备或日志处理时间，再重复本次检查。
                raise RuntimeError(f'Console timeout waiting for {marker}; see {serial}')  # 明确给出缺少的标记和已有日志路径。
            wait('[AIRI_P5_READY]',timeout=35)  # 输入前先等内核初始化与首帧结束，最多35秒。
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
                assert 'page pool: total=' in tail and 'kernel heap: bytes=1048576' in tail # 不能只返回启动地图。
                for number, text, expected in [(5, 'programs', 'programs: registered ELF images'), (6, 'run hello', 'program hello exited 0'), (7, 'run broken', 'cannot load broken: error=-8'), (8, 'run worker', 'program worker exited 23'), (9, 'run processes', 'program processes exited 0'), (10, 'run missing', 'program not found: missing'), (11, 'echo after-programs', 'after-programs')]: # 正常、坏ELF、非零退出和未知名称都不破坏后续会话。
                    offset=len(output()) # 当前命令只消费新日志。
                    wire.sendall((text + '\n').encode()) # 发送真正UART输入，而不是直接调用内核函数。
                    tail=wait(update(0,number),offset) # 程序会话全部收拢后才完成控制台命令。
                    assert expected in tail # 核对当前请求结果，不能复用旧标记。
                    assert '[AIRI_PANIC]' not in tail # 应用格式或退出错误不能停掉内核。
                cases=['UART receive / command dispatch' ,'serial-only device report']  # 结果只记录此路径实际运行的两项观察。
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
                assert 'learning kernel' in wait(update(0,commands),offset)  # 修复后的sys命令应报告当前内核身份。
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

</details>

<a id="heap-grow-observe"></a>
## 构建、查询，再运行原有用户程序

从K37完成上述修改后，在同一学习目录运行：

<!-- run -->
```sh
./airios build # 用新的对象算法、页映射与快照核对重建当前镜像。
./airios test # 实际运行主机检查、正常与故障启动、串口、用户和存储回归。
```

Mac默认python3若仍指向旧系统版本，在命令前使用`PATH=/opt/homebrew/bin:$PATH`。交互入口与guest输入如下：

```sh
./airios run # 观察本轮启动日志，Ctrl-A后X由宿主结束QEMU。
```

```text
mem
run sh
capacity
exit
```

启动先发布旧小对象演示，再出现逐批HEAP_GROW；最后一批与完成记录为：

```text
[HEAP_GROW] mapped_pages=256 bytes=1048576
[HEAP_CAPACITY_OK] initial=65536 bytes=1048576 live=0 chunks=16 preserved=1 limit=null
```

mem查询应显示`kernel heap: bytes=1048576 free_payload=1048544`。页池空闲页少于K37同环境启动时，是因为堆缓存增加了240个数据页，内核BSS与映像本身也可能影响启动可用页数，不能只拿两个版本的总数相减推断泄漏。同一会话内的前后核对使用当前真实基准。随后capacity仍完成四轮满1024页的用户fork/exec，shell退出返回监控台。

超出堆上限的申请只返回NULL，已经借出的guard与large不会移动。指针不是当前活对象的起点、对象已经归还或输入长度无效时，接口仍按K17拒绝。内核调用者仍负责结束全部借用后再free；本节没有为旧指针增加代数或引用计数。

Linux对照继续使用L02的映射、RSS和触页观察。AiriOS这里按批立即映射并保留backing，用户层的Linux malloc、线程缓存与内核slab也各有不同策略；不能把这套计数直接当作Linux的RSS含义。个人Linux实验仍未验证。下一步继续把正式启动策略与设备服务从当前教学启动路径中分开。

