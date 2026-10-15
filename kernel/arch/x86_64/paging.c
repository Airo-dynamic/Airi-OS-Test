#include <airi/vm.h> // 四级页表的有界接口。
#include <airi/base.h> // 新页表必须用自有 memset 清零。
#define PRESENT 1ull // x86 PTE 的存在位。
#define WRITE 2ull // 页表或叶子允许写入。
#define USER 4ull // 每层都要允许user，不能只改叶子。
#define PWT 8ull // 设备映射的 write-through 位。
#define PCD 16ull // 设备映射的 cache-disable 位。
#define LARGE 128ull // 此实现拒绝已有大页项，不把它当下一级表。
#define NX (1ull << 63) // EFER.NXE 开启后，叶子该位禁止取指。
#define ADDRESS 0x000ffffffffff000ull // x86 页表中的物理页地址部分。
static uint64_t active_root; // 只有当前 CPU 激活的地址空间需要立即失效 TLB。
static bool canonical(uint64_t address) { // 本阶段固定 48 位虚拟地址、四级分页。
    uint64_t top = address >> 47; // 第 47 位及以上应全部为零或全部为一。
    return top == 0 || top == 0x1ffff; // 拒绝中间不规范地址。
}
static uint64_t *table(const struct airi_address_space *s, uint64_t physical) { // 物理页表页通过直接映射访问。
    return (uint64_t *)(uintptr_t)(s->direct_offset + physical); // 转换不会创建映射，调用者必须已有这段 HHDM。
}
static void invalidate(const struct airi_address_space *s, uint64_t address) { // 只使当前活动映射的旧翻译失效。
#ifndef AIRI_HOST_TEST // 主机检查不执行特权指令。
    if (active_root == s->root || (!s->borrowed_kernel && address >= AIRI_USER_LIMIT)) // 内核高半区被用户根借用，即使当前CR3不同也必须失效共享映射。
        __asm__ volatile("invlpg (%0)" :: "r"(address) : "memory"); // 同时阻止编译器把相关内存访问跨过失效动作。
#else // 主机只观察表项和所有权。
    (void)s; // 不使用宿主 CPU 的页表。
    (void)address; // 不把模拟地址交给宿主 MMU。
#endif // 结束目标差异。
}
bool airi_vm_create(struct airi_address_space *s, struct airi_page_pool *pages, uint64_t offset) { // 输出对象此前不能持有未释放的地址空间。
    if (!s || !pages || !offset) // 缺少所有者或可访问别名时不能写页表。
        return false; // 不分配任何页。
    uint64_t physical = airi_page_alloc(pages); // 根页也由页池记录所有权。
    if (!physical) // 耗尽时没有地址可清零。
        return false; // 保持输出对象不变。
    *s = (struct airi_address_space){.root=physical, .direct_offset=offset, .pages=pages}; // 此后地址空间持有根页，不能归还给其他用途。
    airi_memset(table(s, physical), 0, AIRI_PAGE_SIZE); // 清除旧所有者内容，初始没有任何映射。
    return true; // 成功拥有一个空根表。
}
bool airi_vm_map(struct airi_address_space *s, uint64_t address, uint64_t physical, unsigned flags) { // 本阶段只建立 supervisor 4 KiB 映射。
    if (!s || !s->root || !canonical(address) || address % AIRI_PAGE_SIZE || (physical & ~ADDRESS) || (flags & ~(AIRI_VM_WRITE | AIRI_VM_EXEC | AIRI_VM_DEVICE | AIRI_VM_USER))) // 拒绝不规范、未对齐、超物理位宽与未知权限。
        return false; // 输入错误不改变页表或页池。
    if (((flags & AIRI_VM_USER) && address >= AIRI_USER_LIMIT) || (s->borrowed_kernel && (!(flags & AIRI_VM_USER) || address >= AIRI_USER_LIMIT))) // 用户空间不能改写借用的内核高半区。
        return false; // 也不在借用空间中混入自有supervisor低半区分支。
    uint64_t made[3]; // 保存本次新借出的中间页表，失败时逆序归还。
    uint64_t *links[3]; // 对应父表项的地址，用于拆除本次建立的连接。
    unsigned count = 0; // 只有成功新建的中间表才计入回滚范围。
    uint64_t *current = table(s, s->root); // 从 PML4 根表开始。
    for (unsigned level = 3; level > 0; --level) { // 依次消费第 39、30、21 位起的 9 位索引。
        unsigned index = (unsigned)((address >> (12 + level * 9)) & 511); // 512 项表占一页，每项 8 字节。
        uint64_t *entry = &current[index]; // 父表中指向下一层的槽位。
        if ((*entry & LARGE) || ((*entry & PRESENT) && (flags & AIRI_VM_USER) && !(*entry & USER))) // 不支持大页，也不提升既有supervisor路径为user。
            goto rollback; // 不把大页物理起点当页表解引用。
        if (!(*entry & PRESENT)) { // 缺少下一层时才借出页表页。
            uint64_t next = airi_page_alloc(s->pages); // 分配失败必须收回此前新建的表。
            if (!next) // 耗尽没有产生所有权。
                goto rollback; // 已有映射保持不变。
            airi_memset(table(s, next), 0, AIRI_PAGE_SIZE); // 先清零，避免把旧内容解释成有效映射。
            made[count] = next; // 保存本次页表物理地址。
            links[count++] = entry; // 先存父链接，再增加有效回滚条数。
            *entry = next | PRESENT | WRITE | ((flags & AIRI_VM_USER) ? USER : 0); // 中间层允许写，最终叶子仍独立决定权限。
        }
        current = table(s, *entry & ADDRESS); // 去掉权限位后取得下一层物理表的直接映射。
    }
    uint64_t *leaf = &current[(address >> 12) & 511]; // 最后一级的索引消费第 12–20 位。
    if (*leaf & PRESENT) // 禁止静默覆盖已有映射，避免失去原数据页所有者。
        goto rollback; // 此时通常没有新建表，仍走统一回滚出口。
    *leaf = physical | PRESENT | ((flags & AIRI_VM_USER) ? USER : 0) | ((flags & AIRI_VM_WRITE) ? WRITE : 0) | ((flags & AIRI_VM_EXEC) ? 0 : NX) | ((flags & AIRI_VM_DEVICE) ? PWT | PCD : 0); // 叶子写入明确权限、用户位与物理地址。
    invalidate(s, address); // 同地址曾有负面或旧翻译时也不依赖隐式刷新。
    return true; // 数据页仍由调用者持有，页表只拥有中间表与根页。
rollback: // 失败只拆本次新建的链接，不修改早已存在的分支。
    for (unsigned i = count; i > 0; --i) // 先拆完本次全部连接，再处理缓存与所有权。
        *links[i - 1] = 0; // 父项不再引用新建的子表。
    invalidate(s, address); // 阻止活动地址空间继续缓存即将归还的页表路径。
    for (unsigned i = count; i > 0; --i) // 失效之后才允许其他用途复用这些页。
        airi_page_free(s->pages, made[i - 1]); // 精确归还本次拥有的页。
    return false; // 调用者仍拥有传入的数据页。
}
static bool walk(const struct airi_address_space *s, uint64_t address, uint64_t **tables, unsigned *indices) { // 收集四层路径供查询或解除使用。
    if (!s || !s->root || !canonical(address)) // 查询页内地址可以不对齐，但仍须规范。
        return false; // 无效地址空间不访问内存。
    tables[3] = table(s, s->root); // 按硬件层号保存根表。
    for (int level = 3; level >= 0; --level) { // 包括最终叶子所在的第零层。
        indices[level] = (unsigned)((address >> (12 + level * 9)) & 511); // 每层选出 9 位索引。
        uint64_t entry = tables[level][indices[level]]; // 读取当前层的实际表项。
        if (!(entry & PRESENT) || (level && (entry & LARGE))) // 缺项或未支持的大页都不继续解引用。
            return false; // 此 API 不假装知道其他页表格式。
        if (level) // 叶子存数据页，不再把它当页表。
            tables[level - 1] = table(s, entry & ADDRESS); // 为下一轮保留子表地址。
    }
    return true; // 四级路径完整且最终叶子存在。
}
bool airi_vm_lookup(const struct airi_address_space *s, uint64_t address, uint64_t *physical, unsigned *flags) { // 软件查询不触发对目标数据页的访问。
    uint64_t *tables[4]; // 保存当前遍历路径。
    unsigned indices[4]; // 每层所选的表项编号。
    if (!walk(s, address, tables, indices)) // 未映射地址返回失败，不制造缺页异常。
        return false; // 输出参数保持调用前值。
    uint64_t leaf = tables[0][indices[0]]; // 取最终 4 KiB 叶子。
    if (physical) // 调用者可只关心是否存在。
        *physical = (leaf & ADDRESS) | (address & (AIRI_PAGE_SIZE - 1)); // 页内偏移在翻译前后保持不变。
    if (flags) // 可选返回本实现支持的叶子权限。
        *flags = ((leaf & WRITE) ? AIRI_VM_WRITE : 0) | ((leaf & NX) ? 0 : AIRI_VM_EXEC) | ((leaf & PCD) ? AIRI_VM_DEVICE : 0) | ((leaf & USER) ? AIRI_VM_USER : 0); // 中间表由本实现固定为 supervisor RW。
    return true; // 只表示软件页表存在，不代表设备或 RAM 访问一定成功。
}
bool airi_vm_unmap(struct airi_address_space *s, uint64_t address) { // 调用方必须避免解除当前指令、栈或页表自身的映射。
    uint64_t *tables[4]; // 保存由叶子向根回收所需的路径。
    unsigned indices[4]; // 对应每个父子链接。
    if ((s && s->borrowed_kernel && address >= AIRI_USER_LIMIT) || address % AIRI_PAGE_SIZE || !walk(s, address, tables, indices)) // 只接受存在的页起点。
        return false; // 不存在映射不会改变任何所有权。
    tables[0][indices[0]] = 0; // 先移除目标数据页的映射。
    uint64_t release[3]; // 在拆除父链接时暂存待归还的空表。
    unsigned count = 0; // 全部链接更新并失效后再统一归还。
    for (unsigned level = 0; level < 3; ++level) { // 根页由地址空间持续持有，不在这里回收。
        bool empty = true; // 初始认为当前子表没有剩余映射。
        for (unsigned i = 0; i < 512; ++i) // 检查整张子表，而不是只检查刚移除的项。
            if (tables[level][i]) // 任意非零项都说明还有占用。
                empty = false; // 保留该表与所有上层链接。
        if (!empty) // 仍被其他页面使用时必须停止向上回收。
            break; // 不能释放还有映射的表。
        uint64_t *parent = &tables[level + 1][indices[level + 1]]; // 父项拥有当前子表物理地址。
        uint64_t physical = *parent & ADDRESS; // 在清链接前保存要归还的物理页。
        *parent = 0; // 不再让硬件或下一次 walk 找到已释放表。
        release[count++] = physical; // 此时只登记，不提前复用页表内存。
    }
    invalidate(s, address); // 叶子和空父表均拆除后，使翻译与分页结构缓存失效。
    for (unsigned i = 0; i < count; ++i) // CPU 不再引用这些空表后才交还页池。
        airi_page_free(s->pages, release[i]); // 不归还由调用者持有的数据页。
    return true; // 数据页未归还，调用者仍须结束其他别名并处理所有权。
}
bool airi_vm_activate(const struct airi_address_space *s) { // 仅在新表已覆盖当前指令、栈、静态数据和 HHDM 后调用。
#ifndef AIRI_HOST_TEST // 主机观察只验证算法，不安装模拟地址空间。
    uint32_t a, b, c, d; // CPUID 通过四个 32 位寄存器返回能力。
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x80000000u), "c"(0)); // 先确定扩展能力叶是否存在。
    if (a < 0x80000001u || !s || !s->root) // 不读取不存在的扩展叶。
        return false; // 不能宣称安装成功。
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x80000001u), "c"(0)); // 获取扩展处理器特性。
    if (!(d & (1u << 20))) // NX 支持位必须存在。
        return false; // 页权限演示不能静默忽略 NX。
    uint64_t cr4; // CR4 控制分页格式与全局翻译。
    __asm__ volatile("mov %%cr4,%0" : "=r"(cr4)); // 读取当前处理器配置。
    if (cr4 & ((1ull << 12) | (1ull << 17))) // 当前只支持四级、未启用 PCID 的固定启动环境。
        return false; // 不把五级根或 PCID 模型误用为本实现。
    __asm__ volatile("rdmsr" : "=a"(a), "=d"(d) : "c"(0xc0000080u)); // 读取 EFER，保留原有 long-mode 等标志。
    a |= 1u << 11; // EFER.NXE 使页表 NX 位真正生效。
    __asm__ volatile("wrmsr" :: "a"(a), "d"(d), "c"(0xc0000080u) : "memory"); // 写回同一 MSR，保留高 32 位。
    uint64_t cr0; // CR0.WP 决定 supervisor 写只读页也需要受限制。
    __asm__ volatile("mov %%cr0,%0" : "=r"(cr0)); // 读取原 CR0。
    cr0 |= 1ull << 16; // 保留其他位并开启 WP。
    __asm__ volatile("mov %0,%%cr0" :: "r"(cr0) : "memory"); // 让后续 supervisor 写遵守只读权限。
    cr4 &= ~(1ull << 7); // 清 PGE，刷新可能来自加载器的全局翻译。
    __asm__ volatile("mov %0,%%cr4" :: "r"(cr4) : "memory"); // 避免旧全局项绕过新页表权限。
    __asm__ volatile("mov %0,%%cr3" :: "r"(s->root) : "memory"); // 当前地址翻译从这一物理根页开始。
    active_root = s->root; // 后续 map/unmap 对该地址空间执行 invlpg。
    return true; // 只有 CR3 切换后仍正常执行到这里才返回成功。
#else // 没有任何宿主特权动作。
    (void)s; // 不安装模拟的根页。
    (void)active_root; // 保留同一源码中的状态声明而避免未使用警告。
    return false; // 主机明确不能证明硬件激活。
#endif // 结束目标差异。
}

bool airi_vm_clone_kernel(struct airi_address_space *space, const struct airi_address_space *kernel) { // 内核根和其高半区表必须比所有用户空间存活更久。
    if (!kernel || !kernel->root || kernel->borrowed_kernel || !airi_vm_create(space, kernel->pages, kernel->direct_offset)) // 只从真实内核根创建新所有者。
        return false; // 失败时不建立共享链接。
    airi_memcpy(table(space, space->root) + 256, table(kernel, kernel->root) + 256, 256 * sizeof(uint64_t)); // 仅借用高半区，用户低半区保持全零。
    space->borrowed_kernel = true; // 后续map/unmap/destroy必须尊重这条所有权边界。
    return true; // 借出的只有根页，不复制内核所有表。
}
static void destroy_table(struct airi_address_space *space, uint64_t physical, unsigned level, unsigned entries) { // 递归深度固定四层，数据页不在此归还。
    uint64_t *current = table(space, physical); // 非活动表仍通过当前内核HHDM可访问。
    for (unsigned i = 0; i < entries; ++i) // 用户根只遍历低256项，其他自有层遍历512项。
        if (level && (current[i] & PRESENT)) // 叶子描述的是外部数据页，不能误当子表释放。
            destroy_table(space, current[i] & ADDRESS, level - 1, 512); // 自有中间表按子到父归还。
    airi_page_free(space->pages, physical); // 当前不活动，不会有CPU继续遍历这个根。
}
bool airi_vm_destroy(struct airi_address_space *space) { // 调用者先切离该空间，并结束所有借用。
    if (!space || !space->root || space->root == active_root) // 禁止销毁当前CPU正在使用的根。
        return false; // 不能在释放页表后继续执行它。
    destroy_table(space, space->root, 3, space->borrowed_kernel ? 256 : 512); // 借用的内核高半区从不回收。
    airi_memset(space, 0, sizeof(*space)); // 清掉根地址，防止重复销毁。
    return true; // 数据页仍由创建进程的调用者逐一归还。
}
