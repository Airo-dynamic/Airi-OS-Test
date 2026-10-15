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
