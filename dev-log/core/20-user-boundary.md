# K20 · 进入 ring3，让系统调用成为受检查的边界

K19 的任务 1 换到任务 2，只改变当前调用链。两者仍能读写内核地址、操作设备；如果某个任务写坏另一份栈，换栈机制不会阻止它。现在需要让一段程序可以计算、保存自己的数据，却不能任意访问内核。我们把它放到 ring3，也就是处理器检查访问权限的用户态。

程序仍要输出文字，但它已不能自己操作 UART。它执行 int80 请求内核代办：先由硬件进入受保护的入口，内核检查调用号和整个用户缓冲区，再把结果交回用户现场。直接执行特权指令或读取内核页，则走用户故障路径，只结束这一次运行。因而本章同时建立两项关系：页表限制程序能访问什么，系统调用允许它怎样请求受控操作。

本节顺序运行四个受控演示：正常输出与两次非法缓冲区请求、用户cli、直接读取内核页、携带不规范RSP进入系统调用。结束后仍回启动栈与串口，所有临时页和表恢复。当前每次只有一个用户上下文，没有多进程轮转、fork、exec或ELF解析。程序字节由我们写的汇编产生，尚不把这段固定映像称为ELF装载器。

这一节依次扩展页表与内存服务、段表和完整陷入返回，写出用户汇编与上下文所有者，最后接到构建和观察。新接口到章末闭合前先不运行；图形无前置。

<a id="user-address-space"></a>
## 同一个数字地址由谁翻译

用户指令准备从 0x400000 取字节，这个数值必须经当前 CR3 指向的页表翻译。我们给它一份新的根：低半区从空开始，随后只加入它需要的代码与栈；高半区复制长期内核根的链接，让陷入内核以后仍能访问内核代码、TSS 和陷入栈。

这里复制的是链接，不是其后全部页表。用户根只拥有自己的根和低半区表，高半区仍向长期内核根借用，而且保持 supervisor 权限。因此“在用户根中可找到内核映射”和“用户有权访问内核”是两回事。长期根必须比借用者活得久；用户运行前也要先建立陷入栈所在的高半区分支。本阶段不会在运行期间新增一个尚未共享的内核根分支。

接着给 0x400000 建一页 RX 代码，给 0x70000000 建一页 RW/NX 栈。两页数据先清零，再填程序内容，避免把旧页面内容交给用户。x86 要求四层路径都允许 USER，不是只给叶子加 bit2；map 在新建路径时同步设置 USER，遇到已有 supervisor 路径就拒绝提升，也拒绝把高半区变成 USER。借用高半区的空间不能对该区 map/unmap。

退出时还要按同样的所有权拆除。destroy 只能处理当前 CPU 不活动的根，用户根递归遍历低 256 项，归还自有表，不触碰借用的高半区。叶子指向的数据页仍由进程所有者归还，不能因页表消失就丢失数据页账目。当前没有共享用户数据页、COW 或引用计数，回收依靠这份明确的生命周期。

**完整替换 `include/airi/vm.h`。** 新增USER权限、借用标记和非活动空间销毁接口。

<!-- write: include/airi/vm.h -->
```c
#ifndef AIRI_VM_H // 地址空间接口只展开一次。
#define AIRI_VM_H // 包含保护标记。
#include <airi/pages.h> // 页表页从物理页池借出。
#define AIRI_VM_WRITE 1u // 显式可写；不设置则只读。
#define AIRI_VM_EXEC 2u // 显式可执行；不设置则 NX。
#define AIRI_VM_DEVICE 4u // 设备映射禁用普通缓存，不用于 RAM。
#define AIRI_VM_USER 8u // 用户映射必须位于低半区，四层均允许用户访问。
#define AIRI_USER_LIMIT 0x0000800000000000ull // 48位规范地址的低半区右端。
#define AIRI_VM_DEMO_ADDRESS 0xffffc00000000000ull // 演示使用的独立高半区地址。
struct airi_address_space { // 根页物理地址与访问页表页所需的现有直接映射。
    uint64_t root, direct_offset; // CR3 需要 root，C 遍历页表需要 direct_offset。
    bool borrowed_kernel; // true时根的高半区只借用内核表，不能映射、解除或回收它。
    struct airi_page_pool *pages; // 页表拥有者借用的页池，必须比地址空间存活更久。
};
bool airi_vm_create(struct airi_address_space *space, struct airi_page_pool *pages, uint64_t direct_offset); // 借出并清零根页。
bool airi_vm_map(struct airi_address_space *space, uint64_t virtual_address, uint64_t physical, unsigned flags); // 创建一个 4 KiB 映射，拒绝覆盖与无效地址。
bool airi_vm_unmap(struct airi_address_space *space, uint64_t virtual_address); // 解除一个映射，回收空页表，不归还数据页。
bool airi_vm_lookup(const struct airi_address_space *space, uint64_t virtual_address, uint64_t *physical, unsigned *flags); // 查询页内地址，不存在返回 false。
bool airi_vm_activate(const struct airi_address_space *space); // x86 启用 NX/WP 并装 CR3；主机算法观察不执行此动作。
bool airi_vm_clone_kernel(struct airi_address_space *space, const struct airi_address_space *kernel); // 新根只复制高半区链接，低半区从空开始。
bool airi_vm_destroy(struct airi_address_space *space); // 仅销毁非活动空间拥有的表，数据页仍归调用者。
#endif // 结束包含保护。
```

**完整替换 `kernel/arch/x86_64/paging.c`。** 普通map/unmap继续保留回滚和缓存顺序，高半区借用增加明确保护。

<!-- write: kernel/arch/x86_64/paging.c -->
```c
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
    if (active_root == s->root) // 未激活的页表尚无当前 CPU 的缓存翻译。
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
```

<a id="user-pages-owner"></a>
## 用户栈和内核栈分别属于谁

用户执行期间，RSP 指向自己的用户页。如果 int80 继续在这份栈上保存内核现场，用户就能决定内核向哪里写。因此跨权限进入时，CPU 不采用用户 RSP，而从 TSS.RSP0 取得另一块内核栈；旧用户 RSP 作为待恢复的数据保存在现场中。当前为每次用户运行创建四页内核区域，用户根借到它的supervisor映射。外部输入不能替代TSS栈地址。这也决定了退出的先后。finish 仍在这块内核栈上执行：即使已经决定结束用户程序，函数自己的返回现场仍在其中。它先切回长期根并选择启动现场；汇编真正恢复启动栈以后，cleanup 才能销毁用户表、归还用户数据页，最后释放用户内核栈。

memory服务为内部所有者提供取页、还页、物理别名与克隆根入口。HHDM指针仍是内核借用，不暴露给用户程序或UI。用户数据清零避免泄漏此前页面内容，准备失败也通过同一cleanup回收半成品。

**完整替换 `include/airi/memory.h`。** 页表声明采用不完全类型，文本消费者继续只用原有数值快照。

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
struct airi_address_space; // 内核内部可持有地址空间，不向UI交出页表。
bool airi_memory_user_space(struct airi_address_space *space); // 从长期内核根借用高半区。
void airi_memory_use_kernel(void); // 返回启动内核地址空间后才能销毁用户空间。
uint64_t airi_memory_take_page(void); // 内核对象明确持有一个物理数据页。
bool airi_memory_return_page(uint64_t physical); // 映射和借用结束后归还。
void *airi_memory_direct(uint64_t physical); // 当前受支持RAM的HHDM别名，不创建新映射。
#endif // 结束包含保护。
```

**完整替换 `kernel/mm/memory.c`。** 原有初始化不变，末尾增加用户空间内部所有权操作。

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

<a id="ring3-gates"></a>
## 用段权限和TSS交接到内核

首次进入不能只把 CS 改成一个看似用户的数字，硬件还要按段描述符与门权限核对。GDT 依次保存 null、ring0 代码/数据、ring3 代码/数据和占两项的 TSS。内核 CS=8、SS=16 保持；用户代码项偏移 24 加 RPL3 得到 27，数据项偏移 32 加 RPL3 得到 35。TSS 移到偏移 40，ltr 也改为 40。DPL 与 RPL 共同参与检查，这些数值都有表项含义。

用户请求输出时执行 int80。IDT128 的 DPL3 中断门 0xee 允许这次软件进入；IDT129 则保留 DPL0，只让启动任务登记自己的返回现场并进入用户。两条入口都形成完整 176 字节现场，但 129 不向用户开放。

接下来沿 K14 的保存布局选择恢复对象。C 分派返回“应恢复哪份现场”的指针，返回这个指针时 CPU 仍在原内核栈；汇编把 RSP 装成该指针，pop/iretq 才开始读取所选现场。因此首次进入可以选预制用户现场，结束可以选保留的启动现场。普通 CPU 故障演示仍返回原 f，保持原先精确恢复语义。

首次 iretq 恢复 CS=27、SS=35、用户 RIP/RSP 与 RFLAGS，处理器由此降到 CPL3。初始 RFLAGS=0x202 保持必需 bit1 和 IF，IOPL 为 0，所以用户 cli 会故障，不能关闭系统时钟。时钟打断用户时，CPU 同样通过 TSS 换到内核栈；保存的 CS 表明这次是从 user 还是 kernel 进入，后续分派据此选择边界。

**完整替换 `include/airi/trap.h`。** dispatch返回现场指针，TSS栈顶与CR2读取继续留在架构接口。

<!-- write: include/airi/trap.h -->
```c
#ifndef AIRI_TRAP_H // 异常接口只展开一次。
#define AIRI_TRAP_H // 标记已经声明。
#include <airi/base.h> // 固定宽度整数和布尔值来自基础库。
struct airi_trap_frame { // 顺序严格对应 traps.S 的压栈次序。
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8; // 最后压入的寄存器位于最低地址。
    uint64_t rsi, rdi, rbp, rdx, rcx, rbx, rax; // C 调用前保存全部通用寄存器。
    uint64_t vector, error; // 汇编为无硬件错误码的异常补零，统一这两个位置。
    uint64_t rip, cs, rflags, rsp, ss; // 64 位模式由 CPU 压入的返回现场。
};
void airi_trap_init(void); // 安装自己的 GDT、TSS 和 IDT，保持可屏蔽中断关闭。
struct airi_trap_frame *airi_trap_dispatch(struct airi_trap_frame *frame); // 仅供汇编入口调用。
void airi_trap_demo(void); // 完整演示可恢复的 UD2 与带错误码的 GP。
_Noreturn void airi_trap_fatal_demo(void); // 演示未登记故障的停止路径。
enum airi_memory_fault { AIRI_FAULT_READ, AIRI_FAULT_WRITE, AIRI_FAULT_EXEC }; // 三种作者明确登记的页权限演示。
void airi_trap_memory_probe(uint64_t address, enum airi_memory_fault kind); // 要求真实产生对应PF并恢复，否则停止。
void airi_trap_set_stack(uint64_t top); // 进入用户态前设置TSS.RSP0，映射必须持续存在。
uint64_t airi_trap_fault_address(void); // 页故障时读取架构CR2，调用者不要把其他向量的旧值当本次地址。
#endif // 结束包含保护。
```

**新建 `include/airi/user.h`。** 用户所有者处理系统调用和用户故障，返回前还要检查现场。

<!-- write: include/airi/user.h -->
```c
#ifndef AIRI_USER_H // 用户态边界声明一次。
#define AIRI_USER_H // 包含保护。
#include <airi/trap.h> // 系统调用和用户故障使用完整CPU现场。
void airi_user_demo(void); // 顺序运行正常、非法指令、内核读取和坏栈四个ring3演示并回收。
struct airi_trap_frame *airi_user_trap(struct airi_trap_frame *frame); // 允许返回另一条完整中断调用链。
void airi_user_tick(void); // 当前单用户上下文的真实时钟计数，尚不调度多个进程。
struct airi_trap_frame *airi_user_resume(struct airi_trap_frame *frame); // 检查用户返回地址并清理不能交给iretq的标志。
#endif // 结束包含保护。
```

**完整替换 `kernel/arch/x86_64/traps.S`。** int80/int129产生统一vector/error，公共返回改用C返回的RAX现场指针。

<!-- write: kernel/arch/x86_64/traps.S -->
```asm
.section .text /* 异常入口属于可执行段。 */
.global airi_tables_load /* 由 C 初始化函数调用。 */
.type airi_tables_load,@function /* 标记为函数供 ELF 调试器识别。 */
airi_tables_load: /* rdi 指向 GDT 描述符，rsi 指向 IDT 描述符。 */
    lgdt (%rdi) /* 读取 10 字节的 GDT 界限与基地址。 */
    pushq $8 /* 远返回将选择 GDT 中的内核代码段。 */
    leaq 1f(%rip), %rax /* 1f 表示往后最近的数字标签 1。 */
    pushq %rax /* 压入远返回的目标指令地址。 */
    lretq /* 同时装入 RIP 和 CS，切到自己的代码段。 */
1: /* 已经使用自己的 CS=8。 */
    movw $16, %ax /* GDT 第二项是内核数据段。 */
    movw %ax, %ds /* 设置数据段选择子。 */
    movw %ax, %es /* 设置额外数据段选择子。 */
    movw %ax, %ss /* 设置内核栈段选择子。 */
    movw $40, %ax /* TSS 描述符从 GDT 第五项开始，占两项。 */
    ltr %ax /* 为双重故障加载独立栈信息。 */
    lidt (%rsi) /* 后续异常从自己的 IDT 取入口。 */
    ret /* 返回 C 初始化函数。 */
.size airi_tables_load, .-airi_tables_load /* 记录函数长度。 */

.macro TRAP number, error=0 /* 为每个异常生成同形的栈；默认没有硬件错误码。 */
.global airi_trap_\number /* C 入口表引用这个符号。 */
airi_trap_\number: /* 宏参数拼成唯一标签。 */
.if !\error /* 只有 CPU 没有压错误码时才补零。 */
    pushq $0 /* 补足统一的 error 槽位。 */
.endif /* 结束错误码差异。 */
    pushq $\number /* 每个入口固定自己的 vector。 */
    jmp airi_trap_common /* 共享寄存器保存和 C 调用。 */
.endm /* 结束宏定义。 */
TRAP 0 /* 除零，无硬件错误码。 */
TRAP 1 /* 调试异常。 */
TRAP 2 /* 不可屏蔽中断，当前不主动启用设备源。 */
TRAP 3 /* 断点。 */
TRAP 4 /* 溢出。 */
TRAP 5 /* 边界异常。 */
TRAP 6 /* 无效指令 UD。 */
TRAP 7 /* 设备不可用。 */
TRAP 8, 1 /* 双重故障，自带零错误码。 */
TRAP 9 /* 保留的旧协处理器向量。 */
TRAP 10, 1 /* 无效 TSS。 */
TRAP 11, 1 /* 段不存在。 */
TRAP 12, 1 /* 栈段故障。 */
TRAP 13, 1 /* 一般保护 GP。 */
TRAP 14, 1 /* 页故障 PF。 */
TRAP 15 /* 保留向量。 */
TRAP 16 /* x87 浮点异常。 */
TRAP 17, 1 /* 对齐检查。 */
TRAP 18 /* 机器检查。 */
TRAP 19 /* SIMD 浮点异常。 */
TRAP 20 /* 虚拟化异常。 */
TRAP 21, 1 /* 控制保护异常。 */
TRAP 22 /* 保留向量。 */
TRAP 23 /* 保留向量。 */
TRAP 24 /* 保留向量。 */
TRAP 25 /* 保留向量。 */
TRAP 26 /* 保留向量。 */
TRAP 27 /* 保留向量。 */
TRAP 28 /* Hypervisor 注入异常，不带错误码。 */
TRAP 29, 1 /* VMM 通信异常。 */
TRAP 30, 1 /* 安全异常。 */
TRAP 31 /* 保留向量。 */
TRAP 32 /* 主PIC IRQ0对应的时钟入口，无硬件错误码。 */
TRAP 128 /* DPL3的AiriOS系统调用门。 */
TRAP 129 /* DPL0启动/返回用户执行的内核门。 */
TRAP 255 /* 未使用的 IDT 项统一停止；可屏蔽中断仍关闭。 */
airi_trap_common: /* CPU 现场和 vector/error 已形成。 */
    cld /* C ABI 要求字符串方向标志为零；iretq 会恢复原标志。 */
    pushq %rax /* 先保存最后在 C 结构中出现的通用寄存器。 */
    pushq %rbx /* 保存被打断代码的 RBX。 */
    pushq %rcx /* 保存被打断代码的 RCX。 */
    pushq %rdx /* 保存被打断代码的 RDX。 */
    pushq %rbp /* 保存被打断代码的帧指针。 */
    pushq %rdi /* 保存被打断代码的第一参数寄存器。 */
    pushq %rsi /* 保存被打断代码的第二参数寄存器。 */
    pushq %r8 /* 保存通用寄存器 R8。 */
    pushq %r9 /* 保存通用寄存器 R9。 */
    pushq %r10 /* 保存通用寄存器 R10。 */
    pushq %r11 /* 保存通用寄存器 R11。 */
    pushq %r12 /* 保存原 R12，随后可借它保留现场指针。 */
    pushq %r13 /* 保存通用寄存器 R13。 */
    pushq %r14 /* 保存通用寄存器 R14。 */
    pushq %r15 /* 最后保存的 R15 位于结构起点。 */
    movq %rsp, %rdi /* C 的第一个实参是现场指针。 */
    movq %rsp, %r12 /* C ABI 保证被调用者保留 R12。 */
    andq $-16, %rsp /* call 之前对齐到 16 字节，不假设异常发生点的栈对齐。 */
    call airi_trap_dispatch /* C 负责报告与明确登记的演示恢复。 */
    movq %rax, %rsp /* C返回选定的完整现场，同一或另一条调用链。 */
    popq %r15 /* 从保存区域恢复通用寄存器。 */
    popq %r14 /* 顺序与压栈相反。 */
    popq %r13 /* 恢复 R13。 */
    popq %r12 /* 恢复被打断代码的原 R12。 */
    popq %r11 /* 恢复 R11。 */
    popq %r10 /* 恢复 R10。 */
    popq %r9 /* 恢复 R9。 */
    popq %r8 /* 恢复 R8。 */
    popq %rsi /* 恢复 RSI。 */
    popq %rdi /* 恢复 RDI。 */
    popq %rbp /* 恢复 RBP。 */
    popq %rdx /* 恢复 RDX。 */
    popq %rcx /* 恢复 RCX。 */
    popq %rbx /* 恢复 RBX。 */
    popq %rax /* 恢复 RAX。 */
    addq $16, %rsp /* 移除软件 vector 与统一 error 两个槽位。 */
    iretq /* 恢复 RIP、CS、RFLAGS、RSP、SS，继续被打断代码。 */

.global airi_probe_ud, airi_probe_ud_site, airi_probe_ud_resume /* C 用精确地址登记预期故障。 */
airi_probe_ud: /* 普通 C 调用进入演示函数。 */
airi_probe_ud_site: /* 仅这个地址的 UD 可以在演示时恢复。 */
    ud2 /* 必然触发向量 6，不是普通函数调用。 */
airi_probe_ud_resume: /* 处理器保存的 RIP 被明确改为此处。 */
    ret /* 回到调用演示的 C 函数。 */
.global airi_probe_gp, airi_probe_gp_site, airi_probe_gp_resume /* GP 演示地址供 C 比对。 */
airi_probe_gp: /* 原 AX 会由 C 调用约定视为可变。 */
    movw $0xffff, %ax /* 构造超出当前表界限的选择子。 */
airi_probe_gp_site: /* 故障报告的 RIP 应指向这条装载。 */
    movw %ax, %ds /* 无效选择子触发带硬件错误码的 GP。 */
airi_probe_gp_resume: /* 故障装载未成功，原 DS 仍有效。 */
    ret /* 返回 C 检查点。 */

.global airi_probe_read, airi_probe_read_site, airi_probe_read_resume /* 访问地址由 rdi 传入。 */
airi_probe_read: /* 调用者登记预期的未映射读取。 */
airi_probe_read_site: /* PF 的保存 RIP 指向这条 load。 */
    movq (%rdi), %rax /* 未映射数据访问产生 PF。 */
airi_probe_read_resume: /* 恢复时不再访问坏地址。 */
    ret /* 返回 C 检查状态。 */
.global airi_probe_write, airi_probe_write_site, airi_probe_write_resume /* supervisor 只读权限演示。 */
airi_probe_write: /* rdi 是只读映射的虚拟起点。 */
airi_probe_write_site: /* PF 的保存 RIP 指向这条 store。 */
    movq $0x55, (%rdi) /* CR0.WP 开启后只读叶子拒绝这次写。 */
airi_probe_write_resume: /* 恢复时跳过整条已知 store。 */
    ret /* 返回调用者。 */
.global airi_probe_execute, airi_probe_execute_resume /* NX 页的执行入口与恢复标签。 */
airi_probe_execute: /* 调用目标地址由 rdi 传入。 */
    call *%rdi /* 返回地址先入栈，取指时因 NX 产生 PF，RIP 是目标地址。 */
    ret /* 若未发生 PF，则 C 调用者会报告演示未发生。 */
airi_probe_execute_resume: /* PF 现场的 RSP 已包含上面 call 压入的返回地址。 */
    addq $8, %rsp /* 丢弃不能再使用的内部 call 返回地址。 */
    ret /* 回到调用 probe_execute 的 C 函数。 */

.section .rodata /* 入口地址表只读。 */
.global airi_trap_entries /* C 用这张表填 IDT。 */
airi_trap_entries: /* 连续 32 个 64 位入口地址。 */
.irp n,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31 /* irp 将下面一项按 n 展开。 */
    .quad airi_trap_\n /* 地址必须与所属向量一致。 */
.endr /* 结束重复展开。 */
.section .note.GNU-stack,"",@progbits /* 声明不需要可执行栈。 */
```

**完整替换 `kernel/arch/x86_64/trap.c`。** 先按来源权限分流用户故障，CPU内核故障仍PANIC；user IRQ返回也要检查现场。

<!-- write: kernel/arch/x86_64/trap.c -->
```c
#include <airi/user.h> // 用户陷入可选择另一条完整返回现场。
#include <airi/timer.h> // 唯一开放的设备IRQ交给架构时钟驱动。
#include <airi/trap.h> // 异常现场的 C 布局。
#include <airi/kernel.h> // 日志、清零和停止接口。
struct table_pointer { uint16_t limit; uint64_t base; } __attribute__((packed)); // CPU 要求 2+8 字节，中间不能有填充。
struct idt_gate { // 一个 64 位中断门占 16 字节。
    uint16_t low, selector; // 入口最低 16 位和目标代码段。
    uint8_t ist, flags; // 独立栈编号和存在/权限/门类型。
    uint16_t middle; // 入口中间 16 位。
    uint32_t high, zero; // 入口最高 32 位及必须清零的保留项。
} __attribute__((packed)); // 位布局必须符合处理器读取格式。
struct task_state { // 64 位 TSS 只保存硬件切栈所需地址，不承担任务调度。
    uint32_t reserved0; // 固定保留字段。
    uint64_t rsp[3]; // 从用户态进入内核时未来使用的栈。
    uint64_t reserved1, ist[7], reserved2; // IST1 放双重故障栈，其他保持零。
    uint16_t reserved3, io_offset; // I/O 权限位图起点设到结构之后，当前不提供位图。
} __attribute__((packed)); // TSS 的硬件偏移不能被 C 对齐改变。
_Static_assert(sizeof(struct task_state) == 104, "TSS layout"); // 编译时拒绝错误结构长度。
_Static_assert(sizeof(struct idt_gate) == 16, "IDT layout"); // 每个门必须恰好 16 字节。
_Static_assert(sizeof(struct airi_trap_frame) == 176, "trap layout"); // 栈含 15+2+5 个 8 字节槽。
static uint64_t gdt[7]; // null、ring0代码/数据、ring3代码/数据和两项TSS描述符。
static struct idt_gate idt[256]; // 整张表属于内核静态内存，安装后持续有效。
static struct task_state tss; // TSS 也必须在 ltr 后持续存在。
static uint8_t double_fault_stack[16384] __attribute__((aligned(16))); // 原栈损坏时 DF 换这块栈。
extern const uintptr_t airi_trap_entries[32]; // 汇编产生的异常入口地址表。
extern void airi_trap_128(void), airi_trap_129(void); // 系统调用与内核启动用户上下文的门。
extern void airi_trap_32(void); // IRQ0也复用完整通用寄存器保存入口。
extern void airi_trap_255(void); // 未启用的向量转到统一停止路径。
extern void airi_tables_load(const void *, const void *); // 汇编依次加载 GDT/TSS/IDT。
extern void airi_probe_ud(void), airi_probe_gp(void); // 两个由作者明确触发的故障函数。
extern char airi_probe_ud_site[], airi_probe_ud_resume[]; // UD 精确指令与恢复地址。
extern char airi_probe_gp_site[], airi_probe_gp_resume[]; // GP 精确指令与恢复地址。
static uintptr_t expected_site, expected_resume; // 仅同步演示期间非零，不允许一般内核故障被吞掉。
static uint64_t expected_vector, expected_error; // 同时核对向量与硬件错误码。
static uint64_t expected_cr2; // PF 必须同时核对本次访问的线性地址。
static bool recovered; // 处理器真正进入处理函数后才置位。
void airi_trap_init(void) { // 调用时 IF 仍为零，尚不接收普通设备中断。
    gdt[1] = 0x00af9a000000ffffull; // present、ring0、64 位可读代码段。
    gdt[2] = 0x00cf92000000ffffull; // present、ring0、可写数据段。
    uintptr_t base = (uintptr_t)&tss; // TSS 描述符使用内核虚拟地址。
    uint64_t limit = sizeof(tss) - 1; // 描述符界限是最后一个有效字节偏移。
    gdt[3] = 0x00affa000000ffffull; // ring3、64位、只读可执行代码段。
    gdt[4] = 0x00cff2000000ffffull; // ring3、可写数据段。
    gdt[5] = limit | ((base & 0xffffff) << 16) | (0x89ull << 40) | (((base >> 24) & 0xff) << 56); // available 64 位 TSS，地址低 32 位拆入描述符。
    gdt[6] = base >> 32; // TSS 地址高 32 位存到紧随的第二项。
    tss.ist[0] = (uintptr_t)(double_fault_stack + sizeof(double_fault_stack)); // 硬件从栈顶向低地址压入 DF 现场。
    tss.io_offset = sizeof(tss); // 当前 TSS 内没有 I/O 权限位图。
    for (unsigned i = 0; i < 256; ++i) { // 全部项目初始化，避免继承加载器状态。
        uintptr_t address = i < 32 ? airi_trap_entries[i] : (uintptr_t)airi_trap_255; // 未使用向量也有明确停止入口。
        if (i == 32) // 唯一开放的设备向量有自己的编号，不能冒充未知255。
            address = (uintptr_t)airi_trap_32; // 指向TRAP32生成的入口。
        if (i == 128 || i == 129) // 两个软件中断各有固定身份。
            address = i == 128 ? (uintptr_t)airi_trap_128 : (uintptr_t)airi_trap_129; // 不能让它们落入未知向量255。
        idt[i] = (struct idt_gate){(uint16_t)address, 8, i == 8 ? 1 : 0, i == 128 ? 0xee : 0x8e, (uint16_t)(address >> 16), (uint32_t)(address >> 32), 0}; // int80门DPL3，其余DPL0；仅DF使用IST1。
    }
    struct table_pointer gdtr = {sizeof(gdt) - 1, (uintptr_t)gdt}; // CPU 复制描述符值，不保留这个局部对象指针。
    struct table_pointer idtr = {sizeof(idt) - 1, (uintptr_t)idt}; // IDT 本体则在静态区持续存在。
    airi_tables_load(&gdtr, &idtr); // 装载表和选择子，函数返回后表已经由本内核拥有。
    airi_log("[TRAP_READY] vectors=32 cs=8 double_fault_ist=1 interrupts=off"); // 该标记只表示异常设施，不宣称调度或 IRQ 完成。
}
struct airi_trap_frame *airi_trap_dispatch(struct airi_trap_frame *f) { // 汇编传入的指针指向当前异常栈。
    if (f->vector == 128 || f->vector == 129 || ((f->cs & 3) == 3 && f->vector != 32)) // 用户系统调用和用户故障由用户上下文所有者处理。
        return airi_user_trap(f); // 返回哪条完整现场由明确边界决定。
    if (f->vector == 32) { // PIC重映射后的时钟，不能作为CPU故障停止。
        airi_timer_interrupt(); // 计数并确认IRQ，保持当前任务现场不变。
        if ((f->cs & 3) == 3) // IRQ真正打断了ring3执行。
            airi_user_tick(); // 当前仅计数，不提前宣称多进程抢占。
        return (f->cs & 3) == 3 ? airi_user_resume(f) : f; // 用户返回仍检查规范地址和标志，kernel直接恢复。
    }
    uint64_t cr2; // 读取最近页故障的线性地址，非 PF 时仅作为附带状态。
    __asm__ volatile("mov %%cr2,%0" : "=r"(cr2)); // =r 请求输出到编译器分配的通用寄存器。
    airi_log("[EXCEPTION] vector=%llu error=0x%llx rip=0x%llx cs=0x%llx rsp=0x%llx cr2=0x%llx", // 保存实际 CPU 返回现场，便于反汇编定位。
             (unsigned long long)f->vector, (unsigned long long)f->error, (unsigned long long)f->rip, // 向量/错误码/指令位置分别输出。
             (unsigned long long)f->cs, (unsigned long long)f->rsp, (unsigned long long)cr2); // 栈地址来自 CPU，而非 C 局部变量。
    if (expected_site && f->rip == expected_site && f->vector == expected_vector && f->error == expected_error && (f->vector != 14 || cr2 == expected_cr2)) { // 只接受精确登记的作者演示。
        f->rip = expected_resume; // 修改 iretq 将恢复的指令地址，不猜指令长度。
        expected_site = 0; // 一次性登记在返回前消费，不能无限跳过相同错误。
        recovered = true; // 调用者据此检查故障确实发生。
        return f; // 汇编恢复同一个故障演示现场并执行iretq。
    }
    airi_panic("unhandled CPU exception"); // 未预期异常不返回有问题的指令。
}
static void demonstrate(void (*probe)(void), char *site, char *resume, uint64_t vector, uint64_t error) { // 普通函数指针调用可触发不同演示。
    expected_site = (uintptr_t)site; // 保存故障指令的精确地址。
    expected_resume = (uintptr_t)resume; // 保存对应的恢复位置。
    expected_vector = vector; // UD=6，GP=13。
    expected_error = error; // UD 补零；GP 是非法选择子的索引与表标记。
    recovered = false; // 防止前一次演示的结果被重用。
    probe(); // 同一 CPU 同步触发、报告并恢复。
    if (!recovered || expected_site) // 函数返回还不能证明 CPU 真的经历故障。
        airi_panic("exception probe did not recover"); // 明确拒绝意外绕过故障的实现。
}
void airi_trap_demo(void) { // 每次正常启动都验证两种栈形状。
    demonstrate(airi_probe_ud, airi_probe_ud_site, airi_probe_ud_resume, 6, 0); // 验证无硬件错误码的入口。
    demonstrate(airi_probe_gp, airi_probe_gp_site, airi_probe_gp_resume, 13, 0xfffc); // 验证有硬件错误码的入口与 iretq。
    airi_log("[TRAP_DEMO_OK] ud=6 gp=13 resumed=2"); // 两个 probe 都返回后才宣布通过。
}
_Noreturn void airi_trap_fatal_demo(void) { // 命令演示真正的未处理异常。
    expected_site = 0; // 不登记恢复，故障必须走停止路径。
    airi_probe_ud(); // 执行真实 UD2，处理器进入向量 6。
    airi_panic("fatal probe unexpectedly returned"); // 若错误返回也必须停止。
}

extern void airi_probe_read(uint64_t), airi_probe_write(uint64_t), airi_probe_execute(uint64_t); // 汇编产生真实访存或取指。
extern char airi_probe_read_site[], airi_probe_read_resume[]; // 未映射读取的指令与恢复标签。
extern char airi_probe_write_site[], airi_probe_write_resume[], airi_probe_execute_resume[]; // 只读写入与 NX 取指的恢复标签。
void airi_trap_memory_probe(uint64_t address, enum airi_memory_fault kind) { // 只用于作者可预测的三种 supervisor 故障。
    expected_vector = 14; // 三种情况都由分页硬件产生 PF。
    expected_cr2 = address; // PF 报告的虚拟地址必须和本次实参一致。
    recovered = false; // 不继承之前演示的成功状态。
    if (kind == AIRI_FAULT_READ) { // 演示未映射地址的读访问。
        expected_site = (uintptr_t)airi_probe_read_site; // RIP 位于已知 load 指令。
        expected_resume = (uintptr_t)airi_probe_read_resume; // 返回标签不再访问目标。
        expected_error = 0; // supervisor、read、not-present。
        airi_probe_read(address); // 真实读指令应立即触发 PF。
    } else if (kind == AIRI_FAULT_WRITE) { // 演示只读页拒绝 supervisor 写。
        expected_site = (uintptr_t)airi_probe_write_site; // RIP 位于已知 store 指令。
        expected_resume = (uintptr_t)airi_probe_write_resume; // 跳过完整 store。
        expected_error = 3; // present=1、write=1、user=0。
        airi_probe_write(address); // CR0.WP 必须已经开启。
    } else { // 演示 NX 叶子拒绝取指。
        expected_site = address; // 取指故障 RIP 是目标，不是 call 本身。
        expected_resume = (uintptr_t)airi_probe_execute_resume; // 特殊恢复标签还会丢弃内部 call 的返回地址。
        expected_error = 17; // present=1、instruction-fetch=1、user=0。
        airi_probe_execute(address); // 目标含 ret 字节，若 NX 不生效也能返回并被下方检查拒绝。
    }
    if (!recovered || expected_site) // 只有处理器进入了正确 PF 入口才算完成。
        airi_panic("page fault probe did not recover"); // 不把普通函数返回误记为权限生效。
}

void airi_trap_set_stack(uint64_t top) { // 用户陷入时CPU从这里取ring0栈，不借用户提供的RSP。
    if (!top || top % 16) // 当前入口要求16字节对齐的有效栈顶。
        airi_panic("invalid ring0 stack top"); // 不装入可导致首次陷入即双重故障的描述。
    tss.rsp[0] = top; // 所有者必须在用户运行期间保留完整映射。
}

uint64_t airi_trap_fault_address(void) { // x86控制寄存器读取只留在架构实现。
    uint64_t address; // CR2保存最近页故障的线性地址。
    __asm__ volatile("mov %%cr2,%0" : "=r"(address)); // 输出约束让编译器分配通用寄存器。
    return address; // 仅向PF消费者交出本次数值。
}
```

<a id="int80-contract"></a>
## 这条系统调用具体传什么

程序在用户态准备的是调用参数，内核取得这些数值以后才决定能否代办。这是 AiriOS 自有 int80 ABI，与 Linux 的 syscall 指令和编号不兼容：RAX 交调用号并带回结果，RDI/RSI/RDX 交前三个参数，其他保存的通用寄存器恢复原值。当前编号为 1=write(fd,ptr,len)、2=exit(code)、3=getpid、5=ticks，未知号返回 −38。

write 只接受 fd1/2，逻辑长度最多 256 字节；错误分别为 −9（FD）、−22（长度）和 −14（用户范围），成功返回逻辑长度。指针只是用户交来的数，不能直接交给串口：先确认范围不越用户低半区，再逐页确认完整范围都有 USER 叶子。

假设有效页最后两个字节起读四字节，而下一页不存在，完整检查会先返回 −14，连前两个字节也不发送。若边复制边输出再检查下一页，就会形成失败却已经输出前缀的不同语义。完整检查通过后，内核按页经 HHDM 复制到本次内核栈数组，再同步送 UART。当前无并发映射修改或按需缺页，检查与复制处在同一次 IF 关闭的陷入中。

长度输出还允许缓冲区中有 NUL，不能用字符串终止零代替 len。UART 仍把文本换行转换为 CRLF，返回值却是用户提供的逻辑字节数；它没有宣称完整 POSIX 文件写入语义。

**完整替换 `include/airi/kernel.h`。** 增加同步长度输出声明。

<!-- write: include/airi/kernel.h -->
```c
#ifndef AIRI_KERNEL_H   // 首次包含内核共用接口时进入，防止重复声明
#define AIRI_KERNEL_H   // 标记本头已处理，末尾 endif 结束保护范围
#include <airi/boot.h>  // 使用双方共同的字段布局和枚举常量
#include <airi/base.h>  // 按公开声明编译自己的字节函数，保证定义与调用使用同一签名

void airi_serial_init(void);  // 配置 COM1 UART，之后数据端口才能按当前格式发送字符
void airi_serial_write(const char *s);  // 同步发送 NUL 之前的字节，不保留调用者指针
void airi_log(const char *fmt, ...);    // 在局部数组形成日志，再通过串口逐字发送
_Noreturn void airi_halt(void);  // 使当前 CPU 停在不返回的循环中，QEMU 主机进程仍存在
_Noreturn void airi_panic(const char *message);      // 先报告具体错误再停机，参数须为有效 NUL 字符串
bool airi_dashboard_draw(const AiriBootInfo *info);  // 预留图形接口，当前入口未调用，因此本章无需提供定义
void airi_serial_write_bytes(const void *bytes, size_t count); // 按长度同步输出用户副本，不依赖NUL。
#endif  // 结束头文件包含保护，后续使用同一公开声明
```

**完整替换 `kernel/arch/x86_64/serial.c`。** 新接口只读取内核已经验证并复制好的范围。

<!-- write: kernel/arch/x86_64/serial.c -->
```c
#include <airi/kernel.h>  // 取得串口、日志、停机及基础函数的共同声明

static inline void out8(uint16_t port, uint8_t value) {  // 本文件的八位设备写接口，port 为端口号而非内存地址
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));  // 无输出约束；value 放 AL，port 用八位立即数或 DX，写设备一个字节
}
static inline uint8_t in8(uint16_t port) {  // 按给定 I/O 端口读取一个设备状态或数据字节
    uint8_t value;  // 准备接收 AL 输出的 C 局部字节值
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));  // 从端口读到 AL，=a 告诉编译器 value 是本条汇编写出的结果
    return value;  // 将本次从设备读到的字节交回 C 调用方
}
void airi_serial_init(void) {  // 按固定 COM1 基址依次配置中断、速率和收发格式
    out8(0x3f9, 0);  /* Disable UART interrupts; P1 uses polling. */  // DLAB 还为零时关闭 UART 自身中断，P1 通过轮询发送
    out8(0x3fb, 0x80);  // 置 DLAB，使下两次基址读写选择波特率除数寄存器
    out8(0x3f8, 1);  /* Divisor 1: 115200 baud. */  // 除数低字节写 1，配合高字节 0 得到 115200 波特
    out8(0x3f9, 0);  // DLAB 为一时这是除数高字节，和低字节 1 组成完整除数
    out8(0x3fb, 3);  /* 8 data bits, no parity, 1 stop bit. */  // 清 DLAB 并选择八数据位、无校验、一停止位，恢复发送数据用途
    out8(0x3fa, 0xc7);  // 启用并清空 FIFO，接收触发档位设置为 14 字节
    out8(0x3fc, 0x0b);  // 设置 DTR、RTS、OUT2 控制位，当前发送不按调制解调器状态流控
}
static void putchar_serial(char c) {  // 尝试发送一个字节，轮询用次数上限避免设备缺失时永久等待
    /* UART 不就绪时仍有尝试次数上限，耗尽后丢弃该字符。 */
    for (unsigned n = 0; n < 1000000; ++n) {  // 最多读取状态一百万次，这是尝试次数而非固定持续时间
        if (in8(0x3fd) & 0x20) {  // 只保留线路状态第五位，为一表示发送保持部分可接受下一字节
            out8(0x3f8, (uint8_t)c);  // 把字符转成八位数值提交给 UART 数据寄存器
            return;  // 此字节已提交 UART，返回字符串循环处理下一字节
        }
        __asm__ volatile("pause");  // 未就绪时给 CPU 忙等提示，未进行任务切换
    }
}
void airi_serial_write(const char *s) {  // 同步遍历调用者字符串，所有读取在返回之前完成
    while (*s) {  // 当前字节不为 NUL 时继续发送，字符串结束标记本身不写设备
        if (*s == '\n')  // 当前字符是换行时，先补一个回车让终端回到行首
            putchar_serial('\r');  // 先发送回车，下一次发送原换行
        putchar_serial(*s++);  // 发送当前字符后前移指针；终止零只结束循环，不发送
    }
}
void airi_log(const char *fmt, ...) {  // 格式化和发送在同一个调用内完成，不借出局部数组
    char line[512];  // 栈上固定容量包含终止零，最多保存 511 个日志字符
    va_list args;    // 保存本次可变参数遍历状态，不是实际参数数组
    va_start(args, fmt);  // 用最后一个具名参数 fmt 作为起点初始化遍历
    airi_vsnprintf(line, sizeof(line), fmt, args);  // 用完整数组容量形成本次日志，过长时按已有规则截断
    va_end(args);  // 结束本次遍历，和 va_start 成对使用
    airi_serial_write(line);  // 同步发送已形成的字符，返回后不再持有 line
    airi_serial_write("\n");  // 在本次格式化日志末尾发送换行，使观察器取得一条完整记录
}
_Noreturn void airi_halt(void) {  // 函数声明不返回，实际通过反复停止 CPU 实现
    for (;;)  // 省略三个条件的循环永不自然结束，恢复执行后再次停机
        __asm__ volatile("cli; hlt");  // 先关闭可屏蔽中断，再暂停当前 CPU；这不会结束主机 QEMU
}
_Noreturn void airi_panic(const char *message) {  // 收到有效错误文字后先写日志，再阻止内核继续
    airi_log("[AIRI_PANIC] %s", message);  // 将当前错误拼进统一失败标记，供主机定位
    airi_halt();  // 当前入口到此停止，不再向调用者返回
}

void airi_serial_write_bytes(const void *bytes, size_t count) { // 当前串口保持文本换行转换，但逻辑长度可以包含NUL。
    const uint8_t *data = bytes; // 按字节读取已验证的内核副本。
    for (size_t i = 0; i < count; ++i) { // 严格使用调用者长度，不扫描数组外终止符。
        if (data[i] == '\n') // 与原文字输出保持CRLF行为。
            putchar_serial('\r'); // 原始换行之前增加回车。
        putchar_serial((char)data[i]); // 包括零字节也真正交给UART。
    }
}
```

<a id="user-program-bytes"></a>
## 用户程序也从自己的指令开始

页表与返回路径已经有了约定，现在需要一段真正以 CPL3 运行的指令来穿过它们。labs/airios 与 labs/linux 分开：这里的汇编由交叉工具链编入内核只读段，再复制到USER RX页执行，宿主和内核本身都不调用这段代码。映像内部消息用RIP相对引用，固定用户栈和内核保护测试地址用立即数。没有重定位、动态链接或完整程序运行时；下一步将用独立静态ELF替代这种受控映像。

正常程序先给自己的RFLAGS置NT，再getpid，验证内核返回会清掉不适用于iretq的标志。接着输出真实用户消息，两次write分别传内核地址和跨栈末尾的范围，必须得到−14。最后在用户态执行小循环并读取ticks，至少经过两次真实IRQ，输出成功标记并exit0。若行为不符退出90，内核拒绝该结果。

其他三段分别执行cli、直接读确实已映射的内核请求页、用不规范RSP执行系统调用。前两者应得到用户GP和保护PF；最后一种由返回前检查拒绝，不能让坏现场在ring0的iretq处引发内核GP。

**新建 `labs/airios/probe.S`。** `.ascii`只存实际字节，标签差给长度，不依赖C字符串终止。

<!-- write: labs/airios/probe.S -->
```asm
.section .rodata.user_probe,"a" /* 这些字节由内核复制到用户RX页，内核本身不执行。 */
.global airi_user_probe_start, airi_user_probe_end /* 连续映像边界，不是ELF装载接口。 */
airi_user_probe_start: /* 映像内引用用RIP相对；固定布局的测试地址保留立即数。 */
    pushfq /* 用户保存可读标志到自己的有效栈。 */
    orq $0x4000, (%rsp) /* 用户尝试置NT，内核返回时必须清掉。 */
    popfq /* 该标志不能被直接带入内核iretq。 */
    movq $3, %rax /* AiriOS getpid，编号与Linux不兼容。 */
    int $0x80 /* DPL3的门切到TSS内核栈。 */
    cmpq $1, %rax /* 第一个演示程序应得到PID1。 */
    jne 9f /* 返回身份错误则明确退出失败。 */
    movq $1, %rax /* AiriOS write(fd, pointer, bytes)。 */
    movq $1, %rdi /* 只支持串口stdout，fd为1。 */
    leaq 5f(%rip), %rsi /* 消息地址随映像复制一起移动。 */
    movq $(6f-5f), %rdx /* 长度不含隐式NUL。 */
    int $0x80 /* 内核先检查完整用户范围再复制。 */
    movq $1, %rax /* 再次请求write，故意传内核高地址。 */
    movq $0xffff800000000000, %rsi /* 用户不能借系统调用读取内核数据。 */
    movq $8, %rdx /* 小长度也必须检查地址权限。 */
    int $0x80 /* 应返回-EFAULT而不让内核PF。 */
    cmpq $-14, %rax /* 只接受明确的地址错误。 */
    jne 9f /* 错误处理缺失就退出失败。 */
    movq $1, %rax /* 演示跨越映射右边界的缓冲区。 */
    movq $0x70000ffc, %rsi /* 栈最后一页末尾四字节有效。 */
    movq $8, %rdx /* 后四字节超出用户栈映射。 */
    int $0x80 /* 必须整段拒绝，不提前输出前四字节。 */
    cmpq $-14, %rax /* 边界跨页也必须返回-EFAULT。 */
    jne 9f /* 不能只查起点。 */
    movq $5, %rax /* 读取当前实际时钟tick。 */
    int $0x80 /* 这里只返回数值，不阻塞。 */
    movq %rax, %rbx /* 保存用户态等待基准。 */
1: /* 主动在用户态消耗一小段CPU，等待真实IRQ打断。 */
    movl $200000, %ecx /* 有界内部循环不依赖宿主CPU频率。 */
2: /* 每次只给处理器忙等提示。 */
    pause /* 与内核相同指令，但当前CPL为3。 */
    decl %ecx /* 当前循环剩余步数减少。 */
    jnz 2b /* 2b引用前面最近标签2。 */
    movq $5, %rax /* 重新查询tick。 */
    int $0x80 /* 内核系统调用时IF为零，tick需在用户执行期间交付。 */
    subq %rbx, %rax /* 计算这段用户执行期间经过的事件数。 */
    cmpq $2, %rax /* 至少两次真实时钟。 */
    jb 1b /* 不用固定空循环次数冒充IRQ完成。 */
    movq $1, %rax /* 输出非法指针都被拒绝的用户标记。 */
    movq $1, %rdi /* stdout。 */
    leaq 7f(%rip), %rsi /* 用户映像内的第二段消息。 */
    movq $(8f-7f), %rdx /* 只输出实际消息字节。 */
    int $0x80 /* 输出仍经过内核检查。 */
    xorq %rdi, %rdi /* 正常退出码0。 */
    jmp 10f /* 两条路径共用exit系统调用。 */
9: /* 任意行为不符时由用户程序明确失败。 */
    movq $90, %rdi /* 非零退出码交给内核验收。 */
10: /* 不依赖宿主运行时结束用户执行。 */
    movq $2, %rax /* AiriOS exit。 */
    int $0x80 /* 成功退出不会返回此处。 */
    ud2 /* 若内核错误地返回，触发明确用户故障。 */
5: .ascii "[USER_HELLO] ring3 via int80\n" /* 用户缓冲区，不是内核伪造输出。 */
6: /* 第一段消息的半开右端。 */
7: .ascii "[USER_POINTERS_OK] kernel and crossing buffers rejected\n" /* 用户已经核对两次-EFAULT。 */
8: /* 第二段消息的右端。 */
airi_user_probe_end: /* 内核只复制边界之间的字节。 */
.global airi_user_bad_start, airi_user_bad_end /* 第二个程序只验证用户指令权限。 */
airi_user_bad_start: /* CPL3没有IOPL权限修改IF。 */
    cli /* 应产生用户GP，而不是关闭系统时钟。 */
    ud2 /* 若cli错误地成功，仍不能被当成正常退出。 */
airi_user_bad_end: /* 第二段独立映像右端。 */
.section .note.GNU-stack,"",@progbits /* 映像不要求可执行栈。 */
.section .rodata.user_probe,"a" /* 继续保存独立受控映像，不要求内核执行。 */
.global airi_user_load_bad_start, airi_user_load_bad_end /* 验证硬件用户位而非系统调用软件检查。 */
airi_user_load_bad_start: /* kernel起点确实已映射，但所有层禁止user。 */
    movq $0xffffffff80000000, %rax /* 固定内核只读请求段地址。 */
    movq (%rax), %rax /* ring3读supervisor页，应产生PF error=5。 */
    ud2 /* 权限若错误地允许读取，不能宣称正常退出。 */
airi_user_load_bad_end: /* 第三段映像右端。 */
.global airi_user_rsp_bad_start, airi_user_rsp_bad_end /* 验证内核不能把坏RSP直接交给iretq。 */
airi_user_rsp_bad_start: /* mov寄存器本身不会访问这个地址。 */
    movq $0x0000800000000000, %rsp /* 构造48位不规范用户栈地址。 */
    movq $3, %rax /* getpid仍能经TSS内核栈进入系统调用。 */
    int $0x80 /* 返回前必须拒绝坏现场，不能让iretq在kernel里GP。 */
    ud2 /* 如果错误地返回用户，则结果与期望不符。 */
airi_user_rsp_bad_end: /* 第四段映像右端。 */
```

<a id="safe-user-return"></a>
## 返回检查和资源回收都不能提前省掉

最后一种演示刻意把“进入安全”与“返回安全”分开。用户可以用 mov 把不规范数值装入 RSP，寄存器赋值本身不会访存；int80 仍可能通过 TSS 安全进入内核。此时内核栈有效，并不能证明现场中保存的用户栈也有效。如果直接把保存值交给iretq，CPU会在内核返回指令处拒绝它，形成内核GP。resume因此检查固定用户CS/SS和低半区规范RIP/RSP，拒绝后结束用户上下文；同时清IOPL、NT、VM并保持IF及bit1。普通系统调用和user IRQ都走同一检查。

用户故障通过CS的低两位识别；kernel来源不会被当成用户错误吞掉。用户cli的GP记为自有结果−141，直接读supervisor页的PF记为−142，并要求错误码5（present+user，读而非写），CR2必须指向0xffffffff80000000。这个第三演示实际验证硬件USER位，不只是软件拦截系统调用参数。

finish先恢复长期内核根，把结果写进保留的启动现场，返回该现场指针。此时仍在用户内核栈上，不能释放它。iretq真正回到启动栈后再cleanup，销毁不活动用户表、归还代码和用户栈数据页，最后释放内核栈区域。每次都比较前后页池与堆快照，防止把进程隔离建立在不断泄漏资源上。

**新建 `kernel/process/user.c`。** 当前是单上下文顺序演示，不是已实现fork/wait的进程调度器。

<!-- write: kernel/process/user.c -->
```c
#include <airi/user.h> // 用户陷入和演示入口。
#include <airi/memory.h> // 物理页、内核区域与长期内核根。
#include <airi/vm.h> // 用户根借用高半区，低半区单独拥有。
#include <airi/timer.h> // 系统调用只查询实际tick。
#include <airi/kernel.h> // 串口字节输出与失败停止。
#define USER_CODE 0x400000ull // 首个受控用户程序的执行地址。
#define USER_STACK 0x70000000ull // 一页RW/NX用户栈，前后都没有映射。
#define KERNEL_STACK 0xffffe10000001000ull // 用户陷入内核使用的四页栈，前面留保护页。
static struct { // 当前检查点同一时刻只有一个用户上下文。
    struct airi_address_space space; // 自有根与低半区表，借用长期内核映射。
    struct airi_kernel_region stack; // TSS.RSP0指向该内核栈的高端。
    uint64_t owned[2]; // 代码页与用户栈数据页由本对象持有。
    uint64_t fault_error; // 保留用户硬件PF的实际错误位以验证supervisor保护。
    unsigned count, pid, user_ticks; // 数据页数量、用户身份与真实用户态IRQ数。
    bool running; // 只有CPU正在运行本用户或处理其陷入时为真。
    struct airi_trap_frame *frame; // 当前内核栈上完整的用户返回现场。
} process; // 不依赖内核堆扩容，不把它宣称为通用进程表。
static unsigned next_pid = 1; // 两次顺序运行得到不同身份。
static struct airi_trap_frame *kernel_return; // int129保存启动任务继续执行所需的完整现场。
extern const uint8_t airi_user_probe_start[], airi_user_probe_end[]; // 链接进只读段的受控位置无关映像。
extern const uint8_t airi_user_load_bad_start[], airi_user_load_bad_end[]; // 用户直接读取内核映射的硬件权限验证。
extern const uint8_t airi_user_rsp_bad_start[], airi_user_rsp_bad_end[]; // 不规范返回栈必须由内核拒绝。
extern const uint8_t airi_user_bad_start[], airi_user_bad_end[]; // 特权指令故障的第二个映像。
static bool copy_user(void *output, uint64_t address, size_t bytes) { // 只接受本进程低半区且每页USER的完整范围。
    if (address >= AIRI_USER_LIMIT || bytes > AIRI_USER_LIMIT - address) // 先排除右端溢出与内核地址。
        return false; // 不触碰传入地址。
    for (size_t done = 0; done < bytes;) { // 在任何复制或输出前先验证所有跨越的页。
        unsigned flags; // 取得当前页的叶子权限。
        if (!airi_vm_lookup(&process.space, address + done, NULL, &flags) || !(flags & AIRI_VM_USER)) // 必须存在且允许用户访问。
            return false; // 跨到未映射页也整体拒绝。
        size_t part = 4096 - ((address + done) & 4095); // 只推进到当前页末尾。
        if (part > bytes - done) // 最后一段可能不足一页。
            part = bytes - done; // 限制到实际剩余长度。
        done += part; // 继续验证下一页。
    }
    for (size_t done = 0; done < bytes;) { // 只有完整范围通过后才复制到内核有界数组。
        uint64_t physical; // 页内偏移由lookup保留。
        airi_vm_lookup(&process.space, address + done, &physical, NULL); // 当前IF为零，尚无其他执行者修改本进程映射。
        size_t part = 4096 - ((address + done) & 4095); // 本次只读取一页中的有效段。
        if (part > bytes - done) // 最后一段限制长度。
            part = bytes - done; // 不读取请求外字节。
        airi_memcpy((uint8_t *)output + done, airi_memory_direct(physical), part); // 通过已验证物理页的HHDM别名复制，不直接信任用户指针。
        done += part; // 继续复制剩余段。
    }
    return true; // 当前实现没有按需缺页或并发映射变化。
}
static struct airi_trap_frame *finish(int code) { // 仍在用户的内核栈上，不能在此释放它。
    airi_log("[USER_EXIT] pid=%u code=%d user_ticks=%u", process.pid, code, process.user_ticks); // 输出实际退出或故障结果。
    process.running = false; // 后续kernel-only时钟不再计为用户执行。
    airi_memory_use_kernel(); // 先切回长期根，旧用户根稍后由启动任务销毁。
    kernel_return->rax = (uint64_t)(int64_t)code; // int129返回值交给启动调用方。
    return kernel_return; // 汇编将从另一条完整现场恢复，而非返回用户坏指令。
}
struct airi_trap_frame *airi_user_trap(struct airi_trap_frame *f) { // C只在完整现场已保存后选择继续位置。
    if (f->vector == 129 && f->cs == 8 && !process.running && process.space.root) { // 只有内核能通过DPL0入口启动已准备用户上下文。
        kernel_return = f; // 保留启动栈上的现场，栈会一直存在。
        airi_trap_set_stack(process.stack.virtual_address + process.stack.count * 4096ull); // 硬件从user进入kernel时切到本进程栈。
        if (!airi_vm_activate(&process.space)) // 高半区已包含代码、启动栈、TSS与本内核栈。
            airi_panic("cannot activate user address space"); // 切换失败不能进入ring3。
        process.running = true; // 之后的用户陷入才能引用当前身份。
        return process.frame; // iretq按CS/SS降低到ring3，并启用用户RFLAGS.IF。
    }
    if (!process.running || (f->cs & 3) != 3) // 不能把内核故障或非法启动请求当成用户调用。
        airi_panic("user trap without a running user context"); // 内核错误仍停止整个系统。
    process.frame = f; // 保存这次真实硬件形成的用户返回现场。
    if (f->vector != 128) { // 用户CPU故障终止该用户执行，不吞掉内核故障。
        process.fault_error = f->error; // 在清理前保留本次权限错误位。
        airi_log("[USER_FAULT] pid=%u vector=%llu error=0x%llx rip=0x%llx cr2=0x%llx", process.pid, (unsigned long long)f->vector, (unsigned long long)f->error, (unsigned long long)f->rip, (unsigned long long)(f->vector == 14 ? airi_trap_fault_address() : 0)); // 实际CPL3故障信息。
        return finish(-(128 + (int)f->vector)); // 自有教学退出编码，不宣称Linux wait ABI。
    }
    if (f->rax == 1) { // write(fd, pointer, bytes)，目前仅串口stdout/stderr。
        char buffer[256]; // 内核栈有界副本，不借用户缓冲给后续异步执行。
        if (f->rdi != 1 && f->rdi != 2) // 当前没有通用FD表。
            f->rax = (uint64_t)-9ll; // -EBADF。
        else if (f->rdx > sizeof(buffer)) // 防止用户指定巨大长度覆盖内核栈。
            f->rax = (uint64_t)-22ll; // -EINVAL。
        else if (!copy_user(buffer, f->rsi, (size_t)f->rdx)) // 验证完整范围后才复制。
            f->rax = (uint64_t)-14ll; // -EFAULT，内核继续运行。
        else { // 缓冲区现在由本次内核调用持有。
            airi_serial_write_bytes(buffer, (size_t)f->rdx); // 按长度输出，包括潜在NUL，不依赖字符串终止。
            f->rax = f->rdx; // 返回本次逻辑输出字节数。
        }
    } else if (f->rax == 2) // exit不返回当前用户指令。
        return finish((int)(f->rdi & 255)); // 最小退出码范围明确为0–255。
    else if (f->rax == 3) // getpid返回当前上下文身份。
        f->rax = process.pid; // 用户不能伪造内核持有的PID。
    else if (f->rax == 5) // 查询单调tick，不是墙钟时间。
        f->rax = airi_timer_ticks(); // 内核中断门当前关闭IF，不在调用中调度。
    else // 未提供的ABI编号必须明确拒绝。
        f->rax = (uint64_t)-38ll; // -ENOSYS，不返回未初始化寄存器作为结果。
    return airi_user_resume(f); // 只把经过规范地址与标志检查的现场交给iretq。
}
void airi_user_tick(void) { // 仅由CS表明来自user的IRQ调用。
    if (!process.running) // 用户态IRQ必须有活的内核所有者。
        airi_panic("user timer without current context"); // 不静默把未知栈当当前进程。
    ++process.user_ticks; // 当前只有一个用户上下文，不宣称已经轮转抢占多个进程。
}
static void cleanup(void) { // 必须已经回到启动栈和长期内核根。
    if (process.space.root && !airi_vm_destroy(&process.space)) // 先解除并归还自有页表，不动借用的内核表。
        airi_panic("cannot destroy inactive user tables"); // 不能归还仍被活动根引用的数据页。
    while (process.count) // 用户代码与用户栈数据页分别归还。
        if (!airi_memory_return_page(process.owned[--process.count])) // 先退到最后一个真正拥有的页。
            airi_panic("cannot return owned user page"); // 归属错误不继续复用内存。
    if (process.stack.count && !airi_memory_region_free(&process.stack)) // 启动栈继续运行，因此此处才可销毁用户内核栈。
        airi_panic("cannot reclaim user kernel stack"); // 不能在finish中提前做这一步。
    airi_memset(&process, 0, sizeof(process)); // 不留下可重复释放的根或句柄。
    kernel_return = NULL; // 原现场已返回，下一次必须重新登记。
}
static bool prepare(const uint8_t *image, size_t bytes) { // 本节是受控位置无关字节映像，下一节再解析独立ELF。
    if (!bytes || bytes > 4096 || !airi_memory_region_create(&process.stack, KERNEL_STACK, 4)) // 内核栈先存在，用户根才能借到它的高半区链接。
        return false; // 没有完整栈就不克隆根。
    if (!airi_memory_user_space(&process.space)) // 根低半区从空开始。
        goto failed; // 已创建内核栈需要回收。
    for (unsigned i = 0; i < 2; ++i) { // 代码一页、用户栈一页。
        uint64_t physical = airi_memory_take_page(); // 数据页所有权属于process。
        if (!physical) // 耗尽时没有本次页可操作。
            goto failed; // 统一回收前面成功部分。
        process.owned[process.count++] = physical; // 先登记所有权，后面任何失败都能找到它。
        airi_memset(airi_memory_direct(physical), 0, 4096); // 用户不能看到旧内核所有者数据。
    }
    airi_memcpy(airi_memory_direct(process.owned[0]), image, bytes); // 复制受控程序，内核原始字节本身从不执行。
    if (!airi_vm_map(&process.space, USER_CODE, process.owned[0], AIRI_VM_USER | AIRI_VM_EXEC) || !airi_vm_map(&process.space, USER_STACK, process.owned[1], AIRI_VM_USER | AIRI_VM_WRITE)) // 用户代码RX，栈RW/NX。
        goto failed; // 页表与数据分别由cleanup回收。
    process.pid = next_pid++; // 准备完成才分配运行身份。
    process.frame = (void *)(uintptr_t)(KERNEL_STACK + 4 * 4096 - sizeof(struct airi_trap_frame)); // 初次iretq现场位于本内核栈顶下方。
    airi_memset(process.frame, 0, sizeof(*process.frame)); // 首次没有旧寄存器值。
    process.frame->rip = USER_CODE; // 处理器将从低半区用户代码取指。
    process.frame->cs = 27; // GDT的ring3代码选择子24加RPL3。
    process.frame->ss = 35; // GDT的ring3数据选择子32加RPL3。
    process.frame->rsp = USER_STACK + 4096 - 16; // 初始用户栈保持16字节对齐。
    process.frame->rflags = 0x202; // 必需bit1加IF，IOPL仍为0，用户不能cli。
    return true; // 还没有切根和进入用户执行。
failed: // 当前仍在长期根和启动栈，半成品都可安全释放。
    cleanup(); // 拆用户表、归还数据、最后解除内核栈。
    return false; // 不运行部分准备的映像。
}
static void run_one(const uint8_t *image, size_t bytes, int expected) { // 同一启动调用链顺序运行四个隔离演示。
    struct airi_memory_stats before, after; // 统计包含页表和两种栈的整个生命周期。
    airi_memory_snapshot(&before); // 准备之前基准。
    if (!prepare(image, bytes)) // 失败已完整回滚。
        airi_panic("cannot prepare user demonstration"); // 不把半成品交给iretq。
    long code; // int129恢复到启动现场时由RAX交回结束原因。
    __asm__ volatile("int $0x81" : "=a"(code) :: "memory", "cc"); // DPL0门保存当前完整现场，再切到受控用户现场。
    if (code != expected || (!expected && process.user_ticks < 2) || (expected == -142 && process.fault_error != 5)) // 正常程序必须真实运行并被用户态IRQ打断。
        airi_panic("user context ended with unexpected result"); // 用户GP与正常退出分别验证。
    cleanup(); // 已经回启动栈，才允许归还用户内核栈。
    airi_memory_snapshot(&after); // 取得本轮结束后的真实空闲数量。
    if (before.pages_free != after.pages_free || before.heap_free != after.heap_free) // 根、子表、数据页和内核栈均须回收。
        airi_panic("user context leaked owned memory"); // 不让隔离演示留下隐藏成本。
}
void airi_user_demo(void) { // 当前只验证边界，不提供shell启动和完整进程调度。
    run_one(airi_user_probe_start, (size_t)((uintptr_t)airi_user_probe_end - (uintptr_t)airi_user_probe_start), 0); // 用户输出、非法指针和真实IRQ后正常退出。
    run_one(airi_user_bad_start, (size_t)((uintptr_t)airi_user_bad_end - (uintptr_t)airi_user_bad_start), -141); // CPL3执行cli得到GP13，仅结束该用户。
    run_one(airi_user_load_bad_start, (size_t)((uintptr_t)airi_user_load_bad_end - (uintptr_t)airi_user_load_bad_start), -142); // 真实user读内核PTE得到PF error=5。
    run_one(airi_user_rsp_bad_start, (size_t)((uintptr_t)airi_user_rsp_bad_end - (uintptr_t)airi_user_rsp_bad_start), -22); // 在内核iretq之前拒绝不规范RSP。
    airi_log("[USER_DEMO_OK] ring=3 syscall=int80 bad_pointers=2 user_gp=13 user_pf=14 bad_rsp=rejected reclaimed=1"); // 内核恢复后继续串口，不宣称ELF/fork已经完成。
}

struct airi_trap_frame *airi_user_resume(struct airi_trap_frame *f) { // CPU将以ring3权限恢复，错误返回现场不能先在ring0引发GP。
    if (f->cs != 27 || f->ss != 35 || f->rip >= AIRI_USER_LIMIT || f->rsp >= AIRI_USER_LIMIT) { // 本ABI只允许低半区规范地址与固定用户段。
        airi_log("[USER_RETURN_REJECTED] pid=%u rip=0x%llx rsp=0x%llx", process.pid, (unsigned long long)f->rip, (unsigned long long)f->rsp); // 记录被拒绝值，不解引用它。
        return finish(-22); // 结束当前用户并回启动栈，不把坏RSP交给iretq。
    }
    f->rflags &= ~((3ull << 12) | (1ull << 14) | (1ull << 17)); // 清IOPL、NT和VM，防止用户标志影响内核返回语义。
    f->rflags |= 0x202; // 保持保留bit1和用户IF，当前不支持用户禁用调度时钟。
    return f; // 内存访问权限继续由user页表硬件检查。
}
```

资源清理需要单独核对：若销毁用户根时误把借用的高半区表一起归还，当前演示也许已经退出，下一次内核访问却会损坏。主机页表观察增加用户根测试：子根借用内核高半区，建立自己的USER低半区，禁止修改共享分支，销毁子根后原内核映射必须仍可查询。它验证所有权算法，ring3权限、用户IRQ和返回检查则必须由QEMU实际执行。

**完整替换 `tests/unit/vm_test.c`。** 最后用destroy而非直接归还根页，核对双重销毁拒绝。

<!-- write: tests/unit/vm_test.c -->
```c
#include <assert.h> // 作者的地址转换与回滚检查。
#include <stdio.h> // 只在宿主输出结果。
#include <airi/vm.h> // 使用真实的四级表算法。
static uint8_t arena[16 * 4096] __attribute__((aligned(4096))); // 模拟十六个物理页的内容，绝不装入宿主 CR3。
static struct airi_page_pool pool; // 分配器所有权状态与 guest 实现相同。
int main(void) { // 宿主仅观察内存中的表和资源变化。
    uint64_t offset = (uintptr_t)arena - 4096; // 模拟 PA=4096 对应 arena 起点的直接映射。
    AiriBootInfo info = {.magic=AIRI_BOOT_MAGIC, .version=AIRI_BOOT_VERSION, .size=sizeof(info), .direct_map_offset=offset, .kernel_physical_base=0x900000, .kernel_virtual_base=0xffffffff80000000ull, .kernel_size=4096, .region_count=1}; // 内核范围不在模拟 arena 中。
    info.regions[0] = (struct airi_memory_region){4096, sizeof(arena), AIRI_MEMORY_USABLE, 0}; // 只允许借出 arena 真正能承载的页。
    assert(airi_pages_init(&pool, &info) && pool.free == 16); // 所有可表示页面初始空闲。
    struct airi_address_space space = {0}; // 尚未拥有根页。
    assert(airi_vm_create(&space, &pool, offset)); // 根页消耗一个物理页。
    uint64_t data = airi_page_alloc(&pool); // 模拟数据页仍由调用者持有。
    assert(data && pool.free == 14); // 根与数据各一页。
    assert(airi_vm_map(&space, AIRI_VM_DEMO_ADDRESS, data, AIRI_VM_WRITE)); // 首次映射需另外三张中间表。
    assert(pool.free == 11); // 共五页被根、数据和三层表持有。
    uint64_t translated = 0; // 接收带页内偏移的翻译结果。
    unsigned flags = 0; // 接收叶子权限。
    assert(airi_vm_lookup(&space, AIRI_VM_DEMO_ADDRESS + 57, &translated, &flags)); // 软件查询允许页内地址。
    assert(translated == data + 57 && flags == AIRI_VM_WRITE); // 保留偏移，默认 NX。
    assert(!airi_vm_map(&space, AIRI_VM_DEMO_ADDRESS, data, 0) && pool.free == 11); // 拒绝覆盖，不消耗额外页。
    assert(!airi_vm_map(&space, AIRI_VM_DEMO_ADDRESS + 1, data, 0)); // 拒绝未对齐 VA。
    assert(!airi_vm_map(&space, 0x0000800000000000ull, data, 0)); // 拒绝不规范 48 位地址。
    assert(!airi_vm_map(&space, AIRI_VM_DEMO_ADDRESS + 4096, data + 1, 0)); // 拒绝未对齐 PA。
    assert(!airi_vm_map(&space, AIRI_VM_DEMO_ADDRESS + 4096, data, 8)); // 拒绝接口未声明的权限。
    assert(airi_vm_map(&space, AIRI_VM_DEMO_ADDRESS + 4096, data, AIRI_VM_EXEC)); // 相邻映射共享所有中间表，演示允许受控物理别名。
    assert(pool.free == 11 && airi_vm_unmap(&space, AIRI_VM_DEMO_ADDRESS)); // 解除第一页不能回收还有第二页的表。
    assert(pool.free == 11 && !airi_vm_lookup(&space, AIRI_VM_DEMO_ADDRESS, NULL, NULL)); // 原叶子必须消失。
    assert(airi_vm_lookup(&space, AIRI_VM_DEMO_ADDRESS + 4096, &translated, &flags)); // 相邻映射仍在。
    assert(translated == data && flags == AIRI_VM_EXEC); // 第二叶子独立拥有自己的权限。
    assert(airi_vm_unmap(&space, AIRI_VM_DEMO_ADDRESS + 4096) && pool.free == 14); // 最后一个叶子移除，三张空表全部归还。
    assert(!airi_vm_unmap(&space, AIRI_VM_DEMO_ADDRESS + 4096)); // 再次解除不存在映射必须失败。
    assert(airi_page_free(&pool, data) && pool.free == 15); // 只有调用者归还数据页，根仍由地址空间持有。
    uint64_t held[13]; // 借走剩余空间，只留两页，构造第三张表分配失败。
    for (unsigned i = 0; i < 13; ++i) // 有界准备明确的耗尽环境。
        held[i] = airi_page_alloc(&pool); // 这些页面暂由测试持有。
    assert(pool.free == 2); // 映射独立分支需要三页，因此必须失败。
    assert(!airi_vm_map(&space, AIRI_VM_DEMO_ADDRESS, held[0], 0)); // 前两层成功、第三层失败后应回滚。
    assert(pool.free == 2 && !airi_vm_lookup(&space, AIRI_VM_DEMO_ADDRESS, NULL, NULL)); // 回滚既不泄漏，也不留下半条路径。
    uint64_t *root = (uint64_t *)(uintptr_t)(offset + space.root); // 模拟直接映射指向真实 arena 内存。
    for (unsigned i = 0; i < 512; ++i) // 本测试最终没有任何叶子，根应恢复全零。
        assert(root[i] == 0); // 不留下指向已释放表的父链接。
    for (unsigned i = 0; i < 13; ++i) // 归还所有测试借用。
        assert(held[i] && airi_page_free(&pool, held[i])); // 每个页面必须仍属于原调用者。
    data = airi_page_alloc(&pool); // 新测试的数据仍由调用者拥有。
    assert(data && airi_vm_map(&space, AIRI_VM_DEMO_ADDRESS, data, AIRI_VM_WRITE)); // 建立要借给用户根的内核高半区路径。
    struct airi_address_space child = {0}; // 用户空间尚无任何所有权。
    assert(airi_vm_clone_kernel(&child, &space)); // 只借高半区中间表，单独拥有根。
    assert(airi_vm_map(&child, 0x400000, data, AIRI_VM_USER | AIRI_VM_EXEC)); // 自有低半区四层允许USER。
    assert(airi_vm_lookup(&child, 0x400000, &translated, &flags) && (flags & AIRI_VM_USER)); // 叶子报告用户权限。
    assert(!airi_vm_map(&child, AIRI_VM_DEMO_ADDRESS + 4096, data, AIRI_VM_WRITE)); // 不允许子根覆盖借用内核分支。
    assert(!airi_vm_unmap(&child, AIRI_VM_DEMO_ADDRESS)); // 也不能解除共享内核映射。
    assert(!airi_vm_map(&space, AIRI_VM_DEMO_ADDRESS + 4096, data, AIRI_VM_USER)); // 高半区不能被标成USER。
    assert(airi_vm_destroy(&child) && !airi_vm_destroy(&child)); // 只回收用户自有表，第二次拒绝。
    assert(airi_vm_lookup(&space, AIRI_VM_DEMO_ADDRESS, &translated, NULL) && translated == data); // 销毁用户根不破坏原内核表。
    assert(airi_vm_unmap(&space, AIRI_VM_DEMO_ADDRESS) && airi_page_free(&pool, data)); // 内核所有者最后自己结束高半区映射和数据页。
    assert(pool.free == 15); // 根仍被内核持有，无其他泄漏。
    assert(!airi_vm_activate(&space)); // 宿主版本明确不执行特权安装，不能记成硬件验证。
    assert(airi_vm_destroy(&space) && pool.free == 16); // 空根不再使用后，测试手动销毁最后一个所有权。
    puts("PASS vm: translation, permissions, shared tables, unmap, exhaustion rollback"); // 全部状态核对通过才打印。
    return 0; // 正常退出宿主观察。
}
```

**完整替换 `Makefile`。** 加入用户所有者C和受控用户汇编对象，不依赖宿主libc或图形。

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
    kernel/console/console.c kernel/arch/x86_64/input.c kernel/arch/x86_64/cpu.c kernel/arch/x86_64/trap.c kernel/mm/pages.c kernel/mm/memory.c kernel/arch/x86_64/paging.c kernel/mm/heap.c kernel/arch/x86_64/timer.c kernel/task/task.c kernel/process/user.c
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
OBJECTS := $(patsubst %.c,$(OUT)/%.o,$(SOURCES)) $(OUT)/kernel/arch/x86_64/entry.o $(OUT)/kernel/arch/x86_64/traps.o $(OUT)/kernel/arch/x86_64/switch.o $(OUT)/labs/airios/probe.o
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

**完整替换 `kernel/main.c`。** 内核任务演示回收后才顺序运行user，全部恢复后继续console。

<!-- write: kernel/main.c -->
```c
#include <airi/user.h> // 验证ring3系统调用和故障隔离。
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
    airi_user_demo(); // 正常与非法用户程序结束后仍回到启动栈和串口。
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

**完整替换 `scripts/boot.py`。** 正常READY还必须包含完整USER_DEMO_OK，不把只进ring3当全部完成。

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
                        assert '[USER_DEMO_OK]' in text # 用户输出、指针拒绝和用户故障后内核必须继续。
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

<!-- run -->
```sh
./airios test  # 六种启动与真实输入必须在所有用户演示后继续成立。
```

既有检查点在同一 QEMU 中的四次用户运行分别记录：

- pid1 输出 USER_HELLO、USER_POINTERS_OK，正常退出 0，并记录多次 user_ticks。
- pid2 在用户 cli 处得到 GP13。
- pid3 读取内核页，得到 PF14、error=0x5，CR2=0xffffffff80000000。
- pid4 把 RSP 设为 0x800000000000，在返回前由 USER_RETURN_REJECTED 拒绝。

最后仍有 USER_DEMO_OK、READY 和串口命令。PID 随这组固定前置运行产生；tick 次数受宿主调度影响，不作为固定 ABI。

调试时在airi_user_trap查看frame->cs、ss、rip、rsp，用户来源应是27/35；QEMU info registers可确认实际CPL3。系统调用入口已经在TSS内核栈上，frame->rsp才是保存的用户栈。对比process.space.root与当前CR3，再跟到finish切回长期根，只有返回到run_one后才观察cleanup释放栈。调试位置与自动 guest 输出分别核对；Linux远端GDB仍受L02已记录的连接限制。

当前仅受控一页用户程序、单页用户栈、同步小write与顺序运行。未实现ELF段校验、argc/argv、独立用户libc、动态用户内存、进程轮转、fork/exec/wait、文件FD表或信号。下一检查点从静态ELF与用户入口开始扩展，不把当前汇编字节复制冒充完整程序装载。

依据：[Intel系统编程手册](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html)的页级用户权限、段DPL/RPL、TSS.RSP0、INT门与IRET返回检查章节。Linux对照继续使用自己的系统调用ABI，不能把本节编号交给Linux内核。
