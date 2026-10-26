# K19 · 两个任务沿各自的栈暂停和继续

K18 的时钟已经能进入内核，IRQ 目前只计数，不会替正在执行的代码选择下一个任务。本章先让任务自己调用 `airi_task_yield()` 交出执行权：任务 1 输出一次，暂时停下；任务 2 输出一次，再让其他任务继续。以后回来时，两者都应从自己上次暂停的位置接着执行。

重新调用一遍 `worker()` 不能产生这样的继续：它会从入口重新开始，而我们要留下当前轮数、局部值和中间函数的返回位置。两个任务因此各有自己的栈。任务 1 的局部值 107 和任务 2 的 207 分别留在各自现场中，三轮输出应形成 `121212`。启动任务也保留原来的栈，最后从那里回收两个工作任务的栈，使空闲页数恢复。

全部任务仍共享当前内核 CR3，没有引入用户态、不同进程地址空间或 ELF 装载。图形不是前置。先扩展内存区域接口，再写切换汇编和任务控制，最后接入构建、启动与观察脚本；这些新调用到章末才闭合，中途先不构建。

<a id="stack-region-owner"></a>
## 暂停的调用链需要一直保留自己的栈

一个工作任务使用四页，即 16 KiB 栈。它的虚拟地址要连续，push、pop 和局部变量才能按同一个连续范围访问；底下的四个物理页可以分别向页池借出。映射允许内核写入、禁止执行（NX），前面额外留一页不映射，用来暴露越过栈边界的写入。

现在需要记住的不只是栈顶数值，还有这组映射与物理页归谁回收。`airi_kernel_region` 保存虚拟起点、物理页数组和实际持有的 count，任务槽持有这个句柄。初始必须清零；不能按值复制后让两份句柄都释放同一组页面。

创建时一页一页借出并映射，成功才登记到 count。例如要求四页、只完成两页便耗尽，句柄中的两项正好说明需要拆掉哪些映射、归还哪些页。本次借到却未能映射的页先单独归还；已经成功的部分按句柄回滚。四页全部就绪以后才清零整个范围，交给任务使用，不把半成品栈发布出去。

回收之前，调用者必须已经离开该区域。任务仍在自己的栈上执行释放函数时，也还在借用这份内存；所以工作任务结束后先换到其他栈，由启动任务回收。释放函数先核对所有映射是否仍指向句柄记录的物理页，全部匹配才逆序解除映射、失效翻译，再归还数据页。当前单 CPU、IRQ 不操作这些映射，验证与回收之间没有其他修改者；匹配失败保留原状态，内部不变量被破坏则停止，不能假装已经归还。

**完整替换 `include/airi/memory.h`。** 统计快照继续只提供数值；新增 region 句柄供内核自己管理资源。

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
#define AIRI_REGION_PAGES 16u // 当前有界内核区域最多持有十六个数据页。
struct airi_kernel_region { // 所有权句柄必须在使用前清零，不能按值复制后重复释放。
    uint64_t virtual_address; // 所有数据页连续映射的内核虚拟起点。
    uint64_t physical[AIRI_REGION_PAGES]; // 逐页记录实际借出的物理页，便于精确回滚。
    unsigned count; // 只有已经成功映射的页才计入所有权。
};
bool airi_memory_region_create(struct airi_kernel_region *region, uint64_t address, unsigned count); // 建立新的有界RW/NX内核区域，不覆盖已有映射。
bool airi_memory_region_free(struct airi_kernel_region *region); // 所有借用结束后先解除映射，再归还数据页。
#endif // 结束包含保护。
```

`memory.c` 仍持有页池、页表与固定堆。原先的初始化和观察保留，新增的区域函数复用这些对象，不再建立一套分配器。创建前先检查整个目标虚拟范围尚未映射；有冲突就原样拒绝，避免覆盖其他服务的内存。

**完整替换 `kernel/mm/memory.c`。** 重点新增末尾的 `airi_memory_region_create/free`，原页池、页表与堆实现保持。

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
```

<a id="switch-call-abi"></a>
## 保存旧栈位置，再从另一份调用链返回

先看两个都已经运行过的任务。假设任务 2 上次调用 yield 时暂停在 `airi_context_switch()` 中，那次 call 的返回地址和需保留的寄存器仍在任务 2 的栈里。现在任务 1 也调用 yield，调度器选择任务 2，切换函数就需要留下任务 1 的位置，再找回任务 2 的位置。

第一个实参 `&tasks[1].rsp` 指向旧任务控制块里的保存位置；第二个实参 `tasks[2].rsp` 则是目标任务上次留下的栈顶值。普通 call 已把返回地址压到任务 1 的栈上，汇编再压入 RBP、RBX、R12–R15，把此时的 RSP 写进第一个实参指向的字段。此时只是保存好了旧位置，CPU 仍在执行任务 1 的切换函数。

把第二个实参装入处理器 RSP，才真正改变接下来读哪一份栈。后面的六次 pop 和 ret 都读取任务 2 的现场；ret 取得任务 2 那次 call 的返回地址，继续的是任务 2 以前尚未返回的调用。任务 1 本次调用仍留在自己的旧栈中，直到未来重新选中任务 1、换回它的 RSP 才返回。暂停期间的局部值和中间返回地址不能回收。

这里只保存六个通用寄存器，是因为 yield 主动通过普通 C 调用进入。编译器知道调用后 RAX、RCX 等调用者保存寄存器可能改变，需要跨调用存活的值会安排在栈或被调用者保存寄存器中。汇编保存 RBP、RBX、R12–R15 并记录 RSP，就继续履行这份约定；返回地址已经由 call 保存。K14 的异常可以发生在任意指令处，编译器没有提前安排一次普通调用的机会，因此必须保存全部通用寄存器，两个范围不能互换。

**新建 `kernel/arch/x86_64/switch.S`。** 后半段坏栈入口只用于独立故障镜像，正常任务交接不执行它。

<!-- write: kernel/arch/x86_64/switch.S -->
```asm
.section .text /* 任务交接代码属于内核可执行段。 */
.global airi_context_switch /* C用普通调用约定进入。 */
.type airi_context_switch,@function /* 标记函数便于反汇编。 */
airi_context_switch: /* rdi指向旧任务的RSP槽，rsi是新任务已保存的RSP。 */
    pushq %rbp /* 普通C调用需要保留的寄存器之一。 */
    pushq %rbx /* 保存被调用者负责保留的RBX。 */
    pushq %r12 /* 保存R12。 */
    pushq %r13 /* 保存R13。 */
    pushq %r14 /* 保存R14。 */
    pushq %r15 /* 最后保存的R15位于最低地址。 */
    movq %rsp, (%rdi) /* 保存的是暂停的调用链，不是刚执行完函数后的栈。 */
    movq %rsi, %rsp /* 此后所有pop和ret都从另一个任务栈取值。 */
    popq %r15 /* 恢复目标任务的被调用者保存寄存器。 */
    popq %r14 /* 恢复R14。 */
    popq %r13 /* 恢复R13。 */
    popq %r12 /* 恢复R12。 */
    popq %rbx /* 恢复RBX。 */
    popq %rbp /* 恢复目标调用链的帧指针。 */
    ret /* 弹出目标任务之前的返回地址，首次则进入trampoline。 */
.size airi_context_switch,.-airi_context_switch /* 记录符号长度。 */
.global airi_stack_guard_fault /* 作者故障镜像用坏栈触发双重故障。 */
.type airi_stack_guard_fault,@function /* 这是不返回的特定演示函数。 */
airi_stack_guard_fault: /* rdi为映射栈下方保护页的上边界。 */
    movq %rdi, %rsp /* 把栈顶置于边界，下一次压栈会写进未映射页。 */
    pushq $0 /* PF也无法在坏栈上压现场，CPU改用DF的IST1。 */
    ud2 /* 若错误地没有触发保护，仍明确失败而不继续用坏栈运行。 */
.size airi_stack_guard_fault,.-airi_stack_guard_fault /* 记录演示范围。 */
.section .note.GNU-stack,"",@progbits /* 任务栈不需要执行权限。 */
```

已有调用链能够恢复，但新任务尚未运行，栈里还没有那次 call 的返回地址。下一步需要让同一组六次 pop 和 ret 也能进入第一次执行的任务。

<a id="task-first-stack"></a>
## 给首次任务准备恢复步骤能够使用的现场

`spawn()` 从栈顶预制六个初始寄存器槽和一个 `trampoline` 入口。第一次切入时，pop 取得六个初始值，ret 取得 trampoline 地址，执行便进入一个新的 C 函数。初始寄存器值设零，因为此前没有旧值需要恢复；trampoline 再根据当前任务槽调用真正的 entry。

C 入口还必须满足 SysV 的 `RSP % 16 == 8`。这里初始栈顶按 16 字节对齐，额外预留一个模拟调用者返回槽，再放 trampoline 地址与六个寄存器槽，总计 64 字节。恢复消耗六次 pop 的 48 字节和 ret 的 8 字节，进入 trampoline 时还留下八字节，正好满足入口对齐。额外槽设零，trampoline 承诺不返回，所以它不会作为正常返回地址使用。

首次进入与再次恢复共用切换代码，任务控制还要决定哪一份现场可以运行。接口只提供本章主动交接与演示，不向其他模块承诺完整的进程或线程创建 API。

**新建 `include/airi/task.h`：**

<!-- write: include/airi/task.h -->
```c
#ifndef AIRI_TASK_H // 教学任务接口只声明一次。
#define AIRI_TASK_H // 包含保护。
void airi_task_yield(void); // 当前内核任务主动交接CPU，尚不是时钟抢占。
void airi_task_demo(void); // 两个独立栈任务轮流执行并回收其映射。
#endif // 结束包含保护。
```

## 沿 0→1→2→0 看全局编号与各自的局部值

演示有三个静态任务槽。0 是当前启动任务，借用原启动栈；1、2 是工作任务，各自持有新建区域和相同的 worker 入口。创建前工作槽为 UNUSED，栈与 entry 全部准备好以后才设 RUNNABLE，调度器才能选它。

0 第一次调用 yield，局部 `old` 保存 0，自己的 RUNNING 改回 RUNNABLE。调度器从下一个编号开始查一圈，只选 RUNNABLE；找到 1 后，先把目标状态改成 RUNNING、将全局 `current` 写成 1，再切换栈。任务 1 第一次经预制现场进入 trampoline，任务 2 之后也按同样方式开始。

任务 1 让出给 2，任务 2 再让出给 0，最早属于 0 的 switch 调用才返回。这时 current 已由后来的调度重新写成 0，而 old 随 0 自己的栈或保留寄存器一直保存，因此返回处应满足 `current == old`。不是改完 current 后旧调用立即运行下一行；切栈已经把它暂停了。每个任务恢复时都检查这项对应，避免把某任务的栈当成另一槽的现场。

`worker` 的 id、round 和 local 也随各自调用链保留。local 用 volatile 放在真实栈上，任务 1 的 107 不应被任务 2 的 207 覆盖。全局 trace 只记录工作任务输出，不记录启动任务的内部轮转，所以最终仍是六项 `121212`。

entry 返回时，trampoline 先把本槽设为 DONE，再 yield 离开这份栈。DONE 不会重新变成 RUNNABLE，也不会被再次选中；启动任务等两个工作槽都 DONE，才在自己的原栈上回收它们。状态生命周期因而是 UNUSED→RUNNABLE→RUNNING，普通 yield 回到 RUNNABLE，结束则转 DONE。回收后控制块里残留的旧 RSP 已过期，不能再恢复这些调用链。

**新建 `kernel/task/task.c`。** 任务槽保存现场和资源，启动任务统一核对轨迹与回收数量。

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
    if (after.pages_free != before.pages_free || after.heap_free != before.heap_free) // 任务栈和临时页表必须全部恢复。
        airi_panic("completed kernel tasks leaked memory"); // 内核演示不应吃掉后续主线的容量。
    airi_log("[TASK_DEMO_OK] order=121212 separate_stacks=2 reclaimed=1"); // 只有切换、局部值、顺序与回收全部成立才输出。
}
```

<a id="guard-double-fault"></a>
## 栈已越界时，故障报告为什么也要换栈

正常任务交接已经有了独立栈。保护页用于发现超出栈范围的写入，但发现故障本身也需要栈：普通页故障 PF 到来时，CPU 还要压入入口现场。若 RSP 已指向未映射处，这一步也失败，就会形成双重故障 DF。

K14 已为向量 8 配置 IST1。DF 通过这项配置换到内核静态独立栈，才能进入 C、报告并停止。栈保护并不保证原调用还能继续，本章故障演示就是验证失去原栈后报告通路仍可使用。

`stack-guard` 镜像在两组栈已建立后，把 RSP 设到栈映射起点，再 push 八字节；栈向低地址增长，写入落到前面的未映射保护页。它故意放弃旧调用栈，不尝试恢复；串口必须出现向量 8 与 PANIC，不能继续 READY。正常镜像不执行故障入口，故障宏只用于独立产物目录。

**完整替换 `Makefile`。** guest 加入任务 C 文件和 switch 汇编，stack-guard 配置定义故障宏。

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
    kernel/console/console.c kernel/arch/x86_64/input.c kernel/arch/x86_64/cpu.c kernel/arch/x86_64/trap.c kernel/mm/pages.c kernel/mm/memory.c kernel/arch/x86_64/paging.c kernel/mm/heap.c kernel/arch/x86_64/timer.c kernel/task/task.c
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
OBJECTS := $(patsubst %.c,$(OUT)/%.o,$(SOURCES)) $(OUT)/kernel/arch/x86_64/entry.o $(OUT)/kernel/arch/x86_64/traps.o $(OUT)/kernel/arch/x86_64/switch.o
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
# 栈保护故障镜像让未映射栈触发DF，由IST1报告。
ifeq ($(CASE),stack-guard)
CPPFLAGS += -DAIRI_TEST_STACK_GUARD # 此镜像不能发布READY。
endif # 结束栈故障配置。
# 时钟故障镜像仅供K18作者演示超时，不进入正常启动配置集合。
ifeq ($(CASE),timer-masked)
CPPFLAGS += -DAIRI_TEST_TIMER_MASKED # PIT仍计数，但PIC阻止IRQ0到达CPU。
endif # 结束时钟故障条件。
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

## 把任务接在内存与时钟就绪之后

任务需要现成的页池和映射，保护故障也需要已安装的异常入口。main 保持已有初始化，在时钟就绪后执行任务演示。演示返回表示已经重新回到启动栈；随后才进入 console，而不是在某个工作栈上继续整个内核。

**完整替换 `kernel/main.c`：**

<!-- write: kernel/main.c -->
```c
#include <airi/task.h> // 两个独立栈内核任务主动交接并回收。
#include <airi/timer.h> // 内存建立后开始真实设备中断。
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
    airi_timer_demo(); // 从PIT/PIC收到真实IRQ0，hlt后恢复到正常上下文。
    airi_task_demo(); // 当前只在正常内核上下文主动切换，IRQ不抢占内核任务。
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

启动观察分别处理两种结果。正常镜像必须核对任务交错与回收标记，再等待 READY；stack-guard 是预期停止的独立镜像，必须看到真实 DF 记录。不能把“进程仍运行”或只有预先打印的探测文字当作故障已经正确交付。

**完整替换 `scripts/boot.py`。** 正常路径核对 TASK_DEMO_OK，保护页路径核对向量 8 和 PANIC。

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
                    if case in ('missing-memory', 'trap-fatal', 'stack-guard'):  # 失败配置只允许具体拒绝记录，不能同时宣布启动完成。
                        if case == 'stack-guard': # 坏栈必须真的转到双重故障的独立栈。
                            assert '[STACK_GUARD_PROBE]' in text and '[EXCEPTION] vector=8 error=0x0 ' in text # 不能把任意PANIC冒充保护页演示。
                        assert '[AIRI_P1_READY]' not in text  # 错误路径不得出现成功标记。
                    else:  # 按当前profile的另一条完整验证路径处理。
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

统一入口继续使用各配置自己的构建与启动检查。日常完整回归现在有六个配置，新增 stack-guard 的预期故障；它的停止不会被当成正常镜像成功进入交互循环。

**完整替换 `scripts/manage.py`：**

<!-- write: scripts/manage.py -->
```python
import argparse  # 命令行解析器把字符串选项转换为带名称的属性。
import json      # 在 Python 对象与 JSON 文本之间转换，不执行其中内容。
import shlex     # 按 Shell 引用规则拆分或引用文本，区别于实际执行。
import subprocess  # 以参数列表启动已有外部程序，并读取输出或退出码。
import sys  # 取得当前解释器路径、入口参数及标准错误流。
from tools import ROOT, tool, make_command  # 复用路径、工具查找与构建参数，不另建工具选择规则。
from deps import prepare  # 复用外部依赖下载与校验，统一入口先调用它。
from boot import check, command  # check 等待启动完成，command 只组装交互QEMU参数。
from console_test import check_console  # 复用真实键盘和UART输入验证。

PROFILES=('normal','no-framebuffer','missing-memory','edit-experiment','trap-fatal','stack-guard')  # 元组固定六种构建配置，CLI choices 与 test 遍历共用。
def run(cmd):  # 所有统一入口外部命令都从同一根目录执行。
    subprocess.run(cmd,cwd=ROOT,check=True)  # 非零状态转成异常传至入口。
def compilation_database():  # 从 Make 干跑输出收集每个C/汇编文件的实际编译参数。
    # GNU Make dry-run is the single source of compiler flags, including headers.
    command=make_command('normal','kernel')  # 局部 command 是 Make 参数列表，不是导入的QEMU command函数。
    command.insert(1,'-Bn')  # 在可执行路径之后插入 -B强制列规则、-n只打印的选项。
    lines=subprocess.check_output(command,cwd=ROOT,text=True).splitlines()  # 读取命令文本并按行拆分，不执行打印出的编译器命令。
    entries=[]  # 保存编译数据库条目，每项对应一份源文件及参数。
    for line in lines:  # 逐条检查 Make 打印的命令。
        if ' -c ' not in line or ' -o ' not in line: continue  # 过滤目录创建与链接命令，只考虑单文件编译。
        tokens=shlex.split(line)  # 按shell引号解析参数边界，列表值随后直接保存。
        if '-c' not in tokens or '-o' not in tokens: continue  # 分词后再确认编译选项，排除仅字符串碰巧匹配的行。
        source=tokens[tokens.index('-c')+1]  # index 找到-c位置，其后一个元素是被编译的源文件路径。
        entries.append({'directory':str(ROOT),'file':str(ROOT/source),'arguments':tokens})  # 保存工作目录、绝对源路径和完整参数列表供 clangd 消费。
    target=ROOT/'build/normal/compile_commands.json'  # 数据库放在普通配置目录，.clangd 从此处读取。
    target.parent.mkdir(parents=True,exist_ok=True)   # 数据库父目录缺失时创建，已有目录保持。
    target.write_text(json.dumps(entries,indent=2)+'\n')  # 列表编码为JSON数组，输出本次真实构建参数。

def build(profile):  # 给指定配置调用同一Make规则，普通配置额外更新编辑器数据库。
    run(make_command(profile))  # 组装配置及工具覆盖，执行完整内核与镜像构建。
    if profile=='normal':  # 测试配置不替换日常编辑参数。
        compilation_database()  # 普通配置才导出clangd数据库。
def inspect_elf(profile='normal'):  # 核对独立内核目标，不接受宿主解释器、动态依赖或未解析符号。
    binary=ROOT/'build'/profile/'airios.elf'  # 定位本次配置的ELF，避免检查另一目录的旧产物。
    readelf=tool('llvm-readelf','llvm')  # 按既定规则找到 LLVM ELF 检查器。
    report=subprocess.check_output([readelf,'-h','-l','-d','-s',str(binary)],text=True)  # 合并ELF头、程序段、动态信息和符号表的文本报告。
    nm=subprocess.check_output([tool('llvm-nm','llvm'),'-u',str(binary)],text=True)  # -u 只列未定义符号，独立内核应没有内容。
    if nm.strip() or 'INTERP' in report or '(NEEDED)' in report:  # 非空未定义项、解释器段或动态依赖任一存在就拒绝。
        raise RuntimeError('Kernel is not a fully resolved freestanding executable')  # 报告不满足完全解析的独立目标约定。
    if 'X86-64' not in report and 'x86-64' not in report:  # 兼顾工具文字大小写，必须出现x86-64机器类型。
        raise RuntimeError('Wrong ELF target')  # 错误机器目标不能作为此次内核成功结果。
    if 'EXEC (Executable file)' not in report:  # 要求固定地址静态ET_EXEC，与链接脚本装载方式一致。
        raise RuntimeError('Expected a static ET_EXEC ELF')  # PIE或其他ELF种类不满足目前启动路径。
    (ROOT/'artifacts').mkdir(exist_ok=True)  # 建立检查报告目录。
    (ROOT/'artifacts/kernel-elf.txt').write_text(report)  # 保存实际ELF检查输出，便于后续调试查看。
    print('PASS ELF: x86-64 ET_EXEC, no interpreter/dependencies/undefined symbols')  # 只在机器、类型、依赖和符号全部符合后打印通过。

def doctor():  # 查工具与版本，再实际准备和构建内核以验证环境。
    record={}  # 字典以工具名称为键，值保存路径和首行版本。
    for name,formula in [('clang','llvm'),('ld.lld','lld'),('qemu-system-x86_64','qemu'),  # 每项二元组给可执行名称与对应软件包标识。
                         ('xorriso','xorriso'),('gmake','make'),('llvm-readelf','llvm')]:  # 继续列镜像制作、GNU Make和ELF检查器；循环解包到name/formula。
        executable=make_command()[0] if name=='gmake' else tool(name,formula)  # GNU Make沿用gmake再make的选择，其余工具按既有规则查找。
        flag='-version' if name=='xorriso' else '--version'  # xorriso 使用单横线，其余工具使用双横线版本参数。
        result=subprocess.run([executable,flag],capture_output=True,text=True)  # 启动版本命令，分别收集标准输出与错误文本。
        version=(result.stdout+result.stderr).splitlines()[0]  # 版本可能写在任一流，取合并后的第一行。
        record[name]={'path':executable,'version':version}     # 工具键映射到实际路径和版本，不只有安装包名称。
        print(f'OK {name}: {version}')  # 显示本轮查到的工具身份和版本。
    prepare()  # 下载并校验当前锁定引导器、协议头与固件。
    run(make_command(target='kernel'))  # 通过真实内核编译和链接验证环境，不仅看工具是否存在。
    compilation_database()  # 使用同一规则生成编辑器参数。
    inspect_elf()  # 检查刚构建出的普通配置ELF。
    (ROOT/'artifacts/toolchain.json').write_text(json.dumps(record,indent=2)+'\n')  # 保存本次实际工具路径和版本。
def main():  # 解析统一入口动作，并把它交给先前实现的脚本。
    if len(sys.argv)>1 and sys.argv[1]=='learn':  # 先检查有动作参数，再识别采用独立子解析器的learn。
        from learning import main as learn  # 只进入learn时导入后续提供的学习模块，别名避免与当前main混淆。
        learn(sys.argv[2:])  # 切片丢掉程序名和learn，把余下参数交给子入口。
        return  # 当前动作完成，不继续后面的构建或分派。
    parser=argparse.ArgumentParser(description='AiriOS build and learning tools')  # 建立动作解析器，不合法参数由argparse给出用法。
    parser.add_argument('action',choices=['doctor','build','run','test','debug','kernel-debug','linux-lab','clean','ui-preview','ui-test','docs-check','snapshots'])  # 位置参数限定动作集合，拼错动作不能落到默认运行QEMU。
    parser.add_argument('--app',choices=('terminal','files','editor','tasks','system','settings','viewer'),default='editor')  # 主机窗口默认Editor，只接受已知应用名。
    parser.add_argument('--state',choices=['normal','empty','loading','error','offline'],default='normal')  # 状态名限定到共享视图已有的五种状态。
    parser.add_argument('--profile',choices=PROFILES,default='normal')  # 镜像默认普通配置，只接受固定六项。
    parser.add_argument('--phase',choices=('p1','p2'),default='p1') # Linux阶段显式选择，默认保持P1。
    parser.add_argument('--host',default='airi@ssh.airi-dynamic.top')   # 个人Linux主机可以显式替换，不涉及公司机器。
    parser.add_argument('--graphics', action='store_true', help='explicitly enable the optional desktop')  # 默认 False，不读取图形目录。
    args=parser.parse_args()      # 把进程参数转为Namespace，后续通过args.action等属性读取。
    if args.action=='snapshots':  # 只有导出动作需要读取Git教学提交链。
        from snapshots import generate  # 局部导入后续提供的快照模块。
        generate()  # 核对固定main提交链和旧目录保护后导出阶段源码。
        return      # 当前动作完成，不继续后面的构建或分派。
    if args.action=='docs-check':  # 只检查重建与导航时无需启动QEMU。
        from docs_check import check_docs  # 只有明确来源检查才读取全部课程。
        check_docs()  # 核对正文重建与本地导航，不阻塞默认 test。
        return  # 当前动作完成，不继续后面的构建或分派。
    if args.graphics:  # G24已提供纯标准库资产检查，运行图形不依赖G26的截图工具。
        from graphics_assets import verify_fonts  # 显式选择后才读取图形目录。
        verify_fonts()  # 不导入Pillow或Cocoa。
    if args.action in ('ui-preview', 'ui-test') or (args.action == 'test' and args.graphics):  # 完整图形回归在G26后使用。
        from ui_preview import preview, check_frontend  # 主线不会导入这份可选工具。
    if args.action=='ui-preview':  # 主机窗口无需内核启动依赖。
        preview(args.app,args.state)  # 打开指定应用和服务状态。
        return  # 此动作已完成。
    if args.action=='ui-test':  # 主机行为与图像观察采用前章接口。
        check_frontend()  # 运行共享C测试与平台可用快照。
        return  # 此动作已完成。
    if args.action=='clean':  # 清理不需要再构建内核。
        run(make_command(target='clean'))  # 调用Make既有删除规则。
        return  # 清理结束。
    if args.action=='linux-lab':  # Linux对照程序由独立脚本传到个人主机运行。
        run([sys.executable,str(ROOT/'scripts/linux_lab.py'),'--host',args.host,'--phase',args.phase])  # 当前解释器执行个人主机实验。
        return  # 远程实验结束后不启动另一台虚拟机。
    if args.action=='doctor':  # doctor自己按需准备和构建。
        doctor()  # 查工具、构建、生成编辑器参数并检查ELF。
        return    # 环境检查结束。
    prepare()  # 下载并校验当前锁定引导器、协议头与固件。
    if args.graphics:  # 图形和串口使用不同产物目录，不复用旧目标。
        args.profile = 'desktop-' + args.profile  # 如 normal 变为 desktop-normal。
    if args.action=='kernel-debug':  # 内核GDB观察需要先得到普通配置带符号ELF。
        build('normal')  # 调试默认仍使用串口目标，测试宏不进入调试对象。
        run([sys.executable,str(ROOT/'scripts/kernel_debug.py'),'--host',args.host])  # 远端GDB连接本地QEMU。
        return  # 调试记录完成后结束本动作。
    if args.action=='test':  # 完整验证串联主机算法、可选前端、六种启动和三条设备输入。
        run(make_command(target='host-test'))  # 编译同一纯C实现并运行带ASan/UBSan的主机边界检查。
        if args.graphics:  # 普通 test 不导入或验证前端。
            check_frontend(screenshots=False)  # 图形模型检查仍有自己的一组断言。
        results=[]  # 列表依次保存各配置的实际启动结果。
        profiles = [('desktop-' + p) if args.graphics else p for p in PROFILES]  # 呈现方式和故障场景正交。
        for profile in profiles:  # 按固定六种配置独立编译与启动。
            build(profile)  # 生成对应配置的ELF和镜像，产物目录彼此隔离。
            if profile.endswith('normal'):  # 普通目标额外检查ELF结构。
                inspect_elf(profile)  # 保存机器、类型、依赖与符号报告。
            results.append(check(profile))  # 等待该配置完成标记并保存结果，任一失败异常中止。
        (ROOT/'artifacts'/('desktop-boot-tests.json' if args.graphics else 'boot-tests.json')).write_text(json.dumps(results,indent=2)+'\n')  # 六种启动结果编码为JSON数组。
        prefix = 'desktop-' if args.graphics else ''  # 本轮输入验证选择同一呈现方式。
        console_results=[check_console(prefix+'normal'),check_console(prefix+'no-framebuffer'),check_console(prefix+'normal',uart_input=False)]  # 顺序验证normal UART、无帧缓冲UART与normal PS/2三条真实输入路径。
        (ROOT/'artifacts'/('desktop-console-tests.json' if args.graphics else 'console-tests.json')).write_text(json.dumps(console_results,indent=2)+'\n')  # 保存三次实际输入验证结果。
    else:  # 普通build/run/debug共用指定配置构建路径。
        build(args.profile)  # build/run/debug先构建调用者选择的配置。
        if args.action!='build':  # 单独构建到此结束，run和debug继续启动虚拟机。
            work=ROOT/'artifacts'/('interactive-'+args.profile)  # 交互日志按配置分开保存。
            if args.action=='debug':  # debug把CPU停在复位并打开回环GDB端点。
                print('QEMU waits at reset. Remote GDB: target remote localhost:1234\n'  # 告诉调试者当前CPU尚未前进及连接地址。
                      'Then hbreak _start; continue. See dev-log/linux/01-entry.md.',flush=True)  # 提示入口硬件断点，flush保证运行虚拟机前已打印。
            if args.graphics:  # 图形交互仍将UART写入本轮文件。
                print(f'Serial log: {work/"serial.log"}',flush=True)  # 显示本次实际串口日志路径，立即刷新输出。
            cmd = command(args.profile,work,gui=args.graphics,debug=args.action=='debug')  # 先形成具体 QEMU 参数。
            if not args.graphics:  # 串口交互直接连接当前主机终端。
                cmd[cmd.index('-serial')+1] = 'mon:stdio'  # QEMU 用 Ctrl-A X 退出，Ctrl-A C 切监视器。
                print('Serial console: this terminal. Exit QEMU with Ctrl-A X.', flush=True)  # 字节不再写入文件。
            run(cmd)  # 启动所选串口或图形QEMU，debug动作才带-S和GDB。

if __name__=='__main__':  # 直接执行脚本才启动CLI，导入时仅定义接口。
    try:  # 入口错误统一转成明确的失败状态。
        main()  # 执行解析和分派，异常留给下面处理。
    except (RuntimeError,subprocess.CalledProcessError,AssertionError) as error:  # 三类异常都保留具体原因并转成非零退出。
        print(f'ERROR: {error}',file=sys.stderr)  # 错误文字送标准错误，不混入正常结果流。
        sys.exit(1)  # 失败状态交给shell和调用脚本，不能继续宣称通过。
```

## 运行后对照交错、局部值和资源回收

**在学习目录根目录运行：**

<!-- run -->
```sh
./airios test  # 实际运行主机边界、六个启动配置、三条设备输入。
```

正常结果应有六条 TASK_STEP：id 在 1、2 间交替，每个任务 round 从 0 到 2，local 分别保持 107、207。既有检查点记录中，随后出现 `[TASK_DEMO_OK] order=121212 separate_stacks=2 reclaimed=1` 并继续 READY。free 与 heap_free 回到创建之前，包含新分支空页表的回收；只有日志交错而资源未恢复，仍不能算本次生命周期完成。

stack-guard 的既有记录中，栈映射起点为 `0xffffe00000006000`，push 写到下面八字节 `0xffffe00000005ff8`，CR2 记录该地址，最终报告 `vector=8 error=0x0` 并 PANIC。这组地址来自该检查点固定布局，故障 RIP 则会随源码布局移动，应配合同次 ELF 定位。记录证明 DF 的 IST 接管，没有证明普通 PF 能在坏栈上返回。

在 `airi_context_switch` 断点处，`x/8gx $rsp` 先显示 call 留下的返回位置；单步六次 push 后，处理器 RSP 已指向压好的现场；再执行 `movq %rsp,(%rdi)`，旧任务的 rsp 字段才更新为这个位置。换入新 RSP 后，同一个 `$rsp` 表达式读取的已经是另一块映射。第一次 ret 进入 trampoline，后续 ret 回到目标任务上次的 switch 返回位置。暂停时分别查看 `tasks[1].rsp` 与 `tasks[2].rsp`，可沿独立栈找到不同 local。

本次切换共享 CR3，只换执行现场，没有切换进程地址空间。它与用户空间 fiber 都包含保留调用链、换栈继续，但还不是 Linux 线程或 BOSS bthread 的完整实现。时钟仍在计数，IRQ 只改 ticks 并发送 EOI，不调用 yield；后续用户态抢占会在完整中断现场边界选任务，内核仍不可抢占。

后续等待机制还需要区分能运行与正在等待：谁保存继续位置、谁在事件发生后唤醒、谁在借用结束后回收资源。BOSS 的条件变量和 bthread 等待也需要这些关系；一个回调函数本身并没有描述执行栈与资源寿命。
