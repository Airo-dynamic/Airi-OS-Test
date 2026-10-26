#include <airi/memory.h> // 内存服务对消费者只交出快照和对象。
#include <airi/heap.h> // 页面映射与对象块链分别维护，扩展后保留活对象。
#include <airi/vm.h> // 使用自有四级页表建立内核映射。
#include <airi/trap.h> // 真实页权限故障由异常入口报告。
#include <airi/pages.h> // 物理页分配与启动信息。
#include <airi/kernel.h> // 日志和具体失败出口。
static struct airi_page_pool pages; // 页池元数据放在内核 BSS，不能自己被分配。
static void heap_demo(void); // 教学观察不属于设备或资源初始化。
static void heap_init(void); // 自有页表建立后才映射堆backing。
static void virtual_memory_init(const AiriBootInfo *info); // 页池就绪后才建立并激活页表。
void airi_memory_init(const AiriBootInfo *info) { // 在异常设施安装后调用，仍使用加载器页表。
    if (!airi_pages_init(&pages, info)) // 没有可管理页就不能继续建立内存服务。
        airi_panic("physical page pool has no usable pages"); // 停止，而不是假装分配成功。
    airi_log("[PAGES_READY] page_bytes=4096 capacity_bytes=%llu managed=%u free=%u", // 明确固定上限和实际管理数量。
             (unsigned long long)AIRI_PHYSICAL_LIMIT, pages.total, pages.free); // 内存地图中的非 usable 页面不在统计中。
    virtual_memory_init(info); // 只建立并激活长期页表，不触发权限故障。
    heap_init(); // 初始映射64KiB，服务启动不先运行容量演示。
}
static void physical_memory_demo(void) { // 页池已经初始化，临时借出和归还只用于诊断配置。
    uint32_t before = pages.free; // 保存演示前空闲数量，最后必须恢复。
    uint64_t first = airi_page_alloc(&pages); // 取得第一个物理页，0 是失败。
    uint64_t second = airi_page_alloc(&pages); // 第二个页面必须独立借出。
    if (!first || !second || first == second) // 耗尽和重复借出都不允许继续写入。
        airi_panic("physical page allocation failed"); // 停止潜在的内存破坏。
    volatile uint64_t *mapped = airi_memory_direct(first); // HHDM 将物理页转换为当前可访问的虚拟别名。
    *mapped = 0x4149524950414745ull; // 真正写入刚借出的 RAM，volatile 保留本次普通 RAM 写读，不提供并发同步。
    if (*mapped != 0x4149524950414745ull || pages.free != before - 2) // 验证内容和所有权统计是同一次分配结果。
        airi_panic("physical page write/accounting failed"); // 不能只凭非零地址宣称可用。
    if (!airi_page_free(&pages, first) || airi_page_free(&pages, first) || airi_page_free(&pages, second + 1)) // 验证正常归还、重复释放拒绝和未对齐释放拒绝。
        airi_panic("physical page release validation failed"); // 失败释放不应改变 second 的所有权。
    uint64_t again = airi_page_alloc(&pages); // 应复用刚归还的低地址页。
    if (again != first || !airi_page_free(&pages, again) || !airi_page_free(&pages, second) || pages.free != before) // 所有演示借用在结束前归还。
        airi_panic("physical page reuse/accounting failed"); // 任何泄漏都阻止后续阶段。
    airi_log("[PAGES_DEMO_OK] first=0x%llx second=0x%llx reused=0x%llx free=%u", // 日志给出本轮真实分配地址。
             (unsigned long long)first, (unsigned long long)second, (unsigned long long)again, pages.free); // 记录数值，不在归还后解引用旧指针。
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
}
static void virtual_memory_demo(void) { // 安装长期根以后才执行可恢复的只读/NX/未映射故障。
    uint32_t before = pages.free; // 演示只临时创建一个独立分支，最后应全部回收。
    uint64_t data = airi_page_alloc(&pages); // 调用者拥有这个数据页，页表不会替它归还。
    if (!data) // 演示也必须处理耗尽。
        airi_panic("cannot allocate paging demonstration page"); // 不写失败哨兵。
    uint64_t *alias = airi_memory_direct(data); // 新根已保留全部受管理 RAM 的 HHDM。
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
    airi_log("[HEAP_READY] bytes=%zu limit=1048576 chunks=%u", heap.bytes, heap_chunks); // 初始化只报告已经持有的最小容量。
}
static void heap_demo(void) { // 小对象、扩展与耗尽留在诊断入口，服务启动不执行。
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
void airi_memory_demo(void) { // 从完整初始化状态执行三类观察，不能代替初始化。
    physical_memory_demo(); // 临时页面的内容和归还。
    virtual_memory_demo(); // 真实硬件权限故障与精确恢复。
    heap_demo(); // 对象分割/合并及1MiB容量边界。
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
