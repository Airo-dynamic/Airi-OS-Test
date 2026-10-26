# K16 · 接管四级页表，让页权限产生真实故障

页池能交出一页 RAM，却还不能决定用哪个 C 地址读它。当前翻译依赖 Limine，本章将用自己借出的页保存表项，把正在执行的代码、栈、启动数据和 HHDM 都接到新根，再将根物理地址装入 CR3。

随后同一数据页经 HHDM 写入、经新别名读出，再分别尝试只读写入、NX 取指和解除后的读取。这样表项、CPU 检查、异常现场与最后回收属于同一对象的连续过程。

先写 vm.h/paging.c，再接链接边界、异常探针、memory 与主机观察，最后构建。页池和 K14 异常继续保留。本章只建内核 supervisor 映射，还没有用户位、进程隔离或用户指针接口。

<a id="four-levels"></a>

## 虚拟地址怎样逐层选中同一物理页

当前四级模式使用 48 位规范地址，第 47 位向高位扩展：低半区高位全零，高半区高位全一，中间空洞无效。4 KiB 页内偏移占低 12 位，其余 36 位分为四组九位，依次选择 PML4、PDPT、PD、PT 项。512 项乘八字节恰好一页，CR3 保存根页的物理地址，不是 C 指针。

演示地址 `0xffffc00000000000` 的 PML4 下标为 384，余下索引和偏移均为零。从根第 384 项取下层物理表地址，连续选三个第零项，最后叶子给数据页起点；若访问页内某字节，低十二位偏移原样加到物理起点。这个分支与 HHDM、链接地址分开，演示后可以独立拆掉。

表项还把权限和地址放在一起：bit0 存在、bit1 可写、bit63 NX。中间层固定 supervisor 可写，叶子再限制最终权限；所有层用户位均关闭。CPU 是否执行限制还取决于 CR0.WP、EFER.NXE，必须稍后真正启用并验证，不能只看软件中的位值。

**新建 `include/airi/vm.h`。** 地址空间拥有根与中间表，数据页仍归映射调用者。

<!-- write: include/airi/vm.h -->
```c
#ifndef AIRI_VM_H // 地址空间接口只展开一次。
#define AIRI_VM_H // 包含保护标记。
#include <airi/pages.h> // 页表页从物理页池借出。
#define AIRI_VM_WRITE 1u // 显式可写；不设置则只读。
#define AIRI_VM_EXEC 2u // 显式可执行；不设置则 NX。
#define AIRI_VM_DEVICE 4u // 设备映射禁用普通缓存，不用于 RAM。
#define AIRI_VM_DEMO_ADDRESS 0xffffc00000000000ull // 演示使用的独立高半区地址。
struct airi_address_space { // 根页物理地址与访问页表页所需的现有直接映射。
    uint64_t root, direct_offset; // CR3 需要 root，C 遍历页表需要 direct_offset。
    struct airi_page_pool *pages; // 页表拥有者借用的页池，必须比地址空间存活更久。
};
bool airi_vm_create(struct airi_address_space *space, struct airi_page_pool *pages, uint64_t direct_offset); // 借出并清零根页。
bool airi_vm_map(struct airi_address_space *space, uint64_t virtual_address, uint64_t physical, unsigned flags); // 创建一个 4 KiB 映射，拒绝覆盖与无效地址。
bool airi_vm_unmap(struct airi_address_space *space, uint64_t virtual_address); // 解除一个映射，回收空页表，不归还数据页。
bool airi_vm_lookup(const struct airi_address_space *space, uint64_t virtual_address, uint64_t *physical, unsigned *flags); // 查询页内地址，不存在返回 false。
bool airi_vm_activate(const struct airi_address_space *space); // x86 启用 NX/WP 并装 CR3；主机算法观察不执行此动作。
#endif // 结束包含保护。
```

<a id="mapping-transaction"></a>

## 新路径缺几张表，只借出并登记本次需要的部分

CPU 按物理地址读表，C 填表却需要虚拟指针。table 把表物理地址加 HHDM 偏移后解引用，建新根时借旧映射，切根后则依靠新根保留的同一布局。每张新表先清零才发布到父项，避免旧字节被误判为已有映射。

映射演示分支最多缺三张中间表，made 记本次新页，links 记父项地址，count 只计已经连接的部分。若第三张耗尽，前两张不是可以忽略的临时值：要清除父链接、失效活动缓存，再归还这两页，数据页仍归调用者。发现大页或已有叶子也走统一 rollback，只撤销本次新增，不破坏旧分支。

goto 把多个失败点汇到同一所有权出口，清理依赖的状态已在前面声明。地址掩码取最多 52 位物理字段，不是探测所有 CPU 位宽；实际范围由固定平台地图提供。当前不支持大页，不能把大页物理起点当下级表继续访问。

**新建 `kernel/arch/x86_64/paging.c`，先写辅助函数、根创建与 map：**

<!-- write: kernel/arch/x86_64/paging.c -->
```c
#include <airi/vm.h> // 四级页表的有界接口。
#include <airi/base.h> // 新页表必须用自有 memset 清零。
#define PRESENT 1ull // x86 PTE 的存在位。
#define WRITE 2ull // 页表或叶子允许写入。
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
    *s = (struct airi_address_space){physical, offset, pages}; // 此后地址空间持有根页，不能归还给其他用途。
    airi_memset(table(s, physical), 0, AIRI_PAGE_SIZE); // 清除旧所有者内容，初始没有任何映射。
    return true; // 成功拥有一个空根表。
}
bool airi_vm_map(struct airi_address_space *s, uint64_t address, uint64_t physical, unsigned flags) { // 本阶段只建立 supervisor 4 KiB 映射。
    if (!s || !s->root || !canonical(address) || address % AIRI_PAGE_SIZE || (physical & ~ADDRESS) || (flags & ~(AIRI_VM_WRITE | AIRI_VM_EXEC | AIRI_VM_DEVICE))) // 拒绝不规范、未对齐、超物理位宽与未知权限。
        return false; // 输入错误不改变页表或页池。
    uint64_t made[3]; // 保存本次新借出的中间页表，失败时逆序归还。
    uint64_t *links[3]; // 对应父表项的地址，用于拆除本次建立的连接。
    unsigned count = 0; // 只有成功新建的中间表才计入回滚范围。
    uint64_t *current = table(s, s->root); // 从 PML4 根表开始。
    for (unsigned level = 3; level > 0; --level) { // 依次消费第 39、30、21 位起的 9 位索引。
        unsigned index = (unsigned)((address >> (12 + level * 9)) & 511); // 512 项表占一页，每项 8 字节。
        uint64_t *entry = &current[index]; // 父表中指向下一层的槽位。
        if (*entry & LARGE) // 当前实现从不混合 2 MiB/1 GiB 大页。
            goto rollback; // 不把大页物理起点当页表解引用。
        if (!(*entry & PRESENT)) { // 缺少下一层时才借出页表页。
            uint64_t next = airi_page_alloc(s->pages); // 分配失败必须收回此前新建的表。
            if (!next) // 耗尽没有产生所有权。
                goto rollback; // 已有映射保持不变。
            airi_memset(table(s, next), 0, AIRI_PAGE_SIZE); // 先清零，避免把旧内容解释成有效映射。
            made[count] = next; // 保存本次页表物理地址。
            links[count++] = entry; // 先存父链接，再增加有效回滚条数。
            *entry = next | PRESENT | WRITE; // 中间层允许写，最终叶子仍独立决定权限。
        }
        current = table(s, *entry & ADDRESS); // 去掉权限位后取得下一层物理表的直接映射。
    }
    uint64_t *leaf = &current[(address >> 12) & 511]; // 最后一级的索引消费第 12–20 位。
    if (*leaf & PRESENT) // 禁止静默覆盖已有映射，避免失去原数据页所有者。
        goto rollback; // 此时通常没有新建表，仍走统一回滚出口。
    *leaf = physical | PRESENT | ((flags & AIRI_VM_WRITE) ? WRITE : 0) | ((flags & AIRI_VM_EXEC) ? 0 : NX) | ((flags & AIRI_VM_DEVICE) ? PWT | PCD : 0); // 叶子写入明确权限与物理地址。
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
```

<a id="lookup-unmap"></a>

## 软件查表，不读取叶子所指的数据

map 已能填一条路径，lookup 需要回答它是否存在。walk 收集四层表指针和索引，lookup 读叶子地址/权限并加页内偏移；目标数据页没有被读取，所以缺映射只返回 false，不主动触发 PF。

查询可接页内地址，map/unmap 则要求页起点。保存整条路径还为下一步反向检查共享表提供依据，不必从一个物理结果猜父项在哪里。

**继续追加到 `kernel/arch/x86_64/paging.c`：**

<!-- append: kernel/arch/x86_64/paging.c -->
```c
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
        *flags = ((leaf & WRITE) ? AIRI_VM_WRITE : 0) | ((leaf & NX) ? 0 : AIRI_VM_EXEC) | ((leaf & PCD) ? AIRI_VM_DEVICE : 0); // 中间表由本实现固定为 supervisor RW。
    return true; // 只表示软件页表存在，不代表设备或 RAM 访问一定成功。
}
```

解除演示叶子后，先确认所在表是否还有别的非零项。相邻别名可能共用它，有其他项就停止向上回收；只有整表为空，才断开父链接并把待归还物理页记在 release。根始终由地址空间持有，不在这次 unmap 中释放。

所有链接已断开，再执行 invlpg 失效该 VA 的 TLB/分页结构缓存，之后才让页池复用空表。否则硬件可能沿旧缓存进入已交给别人的表内存。unmap 不归还数据页，调用者还要确认没有自己的其他借用，再单独处理它的所有权。

**继续追加到 `kernel/arch/x86_64/paging.c`：**

<!-- append: kernel/arch/x86_64/paging.c -->
```c
bool airi_vm_unmap(struct airi_address_space *s, uint64_t address) { // 调用方必须避免解除当前指令、栈或页表自身的映射。
    uint64_t *tables[4]; // 保存由叶子向根回收所需的路径。
    unsigned indices[4]; // 对应每个父子链接。
    if (address % AIRI_PAGE_SIZE || !walk(s, address, tables, indices)) // 只接受存在的页起点。
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
```

<a id="install-cr3"></a>

## 表已构造好，还需让硬件使用并执行权限

activate 先通过 CPUID 扩展叶检查 NX，输出 a/d 等约束对应返回寄存器，输入 a/c 选择叶/子叶。RDMSR/WRMSR 由 ECX 选择 EFER、EDX:EAX 传完整值，保留原 long-mode 位，只增加 NXE；再设 CR0.WP，让 supervisor 写同样受只读页限制。

当前仅接受四级且无 PCID，发现五级或 PCID 就拒绝。清 CR4.PGE 使加载器可能遗留的全局翻译失效，再装根物理地址到 CR3。memory clobber 约束编译器不将相关访问任意移过操作，但不替代这些硬件动作。

主机测试分支明确返回 false，不安装宿主页表。因此算法查表通过不能证明 WP/NX 生效，真正证据要来自后面的 guest 故障。

**最后追加到 `kernel/arch/x86_64/paging.c`：**

<!-- append: kernel/arch/x86_64/paging.c -->
```c
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
```

<a id="kernel-mappings"></a>

## 换根以后的下一条指令和当前栈也要能访问

不能先装空根再补映射。链接脚本给出页对齐的 text 起止和 data 起点：requests/rodata 只读 NX，text 只读可执行，data/BSS 可写 NX。BSS 还放当前栈、IDT、TSS、位图、BootInfo，均需在新根中继续有效。

同一内核物理页还有 HHDM 别名，若只限制链接地址而别名仍可写，便能绕过 text/rodata 保护。因此它们在直接映射中也只读且全部 NX。链接符号是地址标签，不是保存地址值的变量。

**完整替换 `kernel/linker.ld`：**

<!-- write: kernel/linker.ld -->
```ld
/* 请求节布局采用 Limine 官方 0BSD C 模板，许可来源见 THIRD_PARTY。 */
OUTPUT_FORMAT(elf64-x86-64)  /* 输出采用 x86-64 的 64 位 ELF 格式 */
ENTRY(_start)  /* 把符号 _start 的最终虚拟地址写入 ELF 入口字段 */
PHDRS {  /* 声明装载段表，下面的各节会显式归入这些段 */
    requests PT_LOAD FLAGS(4);  /* 引导请求段标为可读，固定协议仍允许引导器在交接前填写响应 */
    text PT_LOAD FLAGS(5);  /* 代码装载段可读可执行，4|1 得到权限数值 5 */
    rodata PT_LOAD FLAGS(4);  /* 只读常量装载段仅允许读，权限数值 4 */
    data PT_LOAD FLAGS(6);  /* 可写数据装载段允许读写，4|2 得到 6 */
}
SECTIONS {  /* 给输入节安排虚拟地址及其归属装载段 */
    . = 0xffffffff80000000;  /* 位置计数器 . 从固定的内核高地址起点开始 */
    __kernel_start = .;  /* 导出此处地址标签，适配层用它计算虚拟地址跨度 */
    .limine_requests : {  /* 建立一个输出请求节，内部按起点、请求、终点顺序收集 */
        KEEP(*(.limine_requests_start))  /* 保留所有输入对象中的请求起始标记节 */
        KEEP(*(.limine_requests))  /* 保留所有输入对象中的实际协议请求节 */
        KEEP(*(.limine_requests_end))  /* 保留所有请求结束标记，防止构建时删除 */
    } :requests  /* 将刚构成的输出请求节放入 requests 装载段 */
    . = ALIGN(4096);  /* 将下一位置向上补齐至 4096 字节边界，分开不同权限页 */
    __text_start = .; /* 页表以页对齐符号限定可执行范围。 */
    .text : { *(.text .text.*) } :text  /* 收集 .text 及其子节，全部放入可执行 text 段 */
    . = ALIGN(4096);  /* 将下一位置向上补齐至 4096 字节边界，分开不同权限页 */
    __text_end = .; /* text尾部已补到整页；此后不允许执行。 */
    .rodata : { *(.rodata .rodata.*) } :rodata  /* 收集只读数据及子节，放入 rodata 段 */
    . = ALIGN(4096);  /* 将下一位置向上补齐至 4096 字节边界，分开不同权限页 */
    __data_start = .; /* 此处开始允许写，前面的请求和常量保持只读。 */
    .data : { *(.data .data.*) } :data  /* 收集有初值的可写数据，放入 data 段 */
    .bss : { *(.bss .bss.*) *(COMMON) } :data  /* 收集零初始化与 COMMON 符号，在 data 段保留内存空间 */
    . = ALIGN(4096);  /* 将下一位置向上补齐至 4096 字节边界，分开不同权限页 */
    __kernel_end = .;  /* 导出包含 BSS 及尾部页对齐的右端地址标签 */
    /DISCARD/ : { *(.eh_frame*) *(.note*) }  /* 丢弃此内核不消费的展开/说明元数据，不参与入口执行 */
}
```

<a id="page-fault-probe"></a>

## 同一页的三种拒绝，对应不同的故障现场

PF 向量 14，CR2 给出本次访问 VA。错误码 bit0 区分保护/不存在、bit1 表示写、bit2 用户访问、bit4 取指；本次只读写、NX 取指、未映射读分别应为 3、17、0。还必须同时核对精确 RIP 与 CR2，不能把别处映错的 PF 当演示完成。

load/store 故障停在已知探针指令，resume 跳到对应后继。NX 则在 `call *%rdi` 已压内部返回地址后，CPU 到目标页取指才失败：保存 RIP 是目标 VA，保存 RSP 已多一项内部返回地址。恢复标签先丢弃这一项，再 ret 回外层 C，不能直接沿错误层次返回。

数据页首字节预写 ret。若 NX 没生效，它可以普通返回，recovered 却仍为 false，C 会拒绝成功。因此检查验证的是确实发生并恢复预期异常，而非调用没有崩溃。

**完整替换 `include/airi/trap.h`，声明三种内存探针：**

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
void airi_trap_dispatch(struct airi_trap_frame *frame); // 仅供汇编入口调用。
void airi_trap_demo(void); // 完整演示可恢复的 UD2 与带错误码的 GP。
_Noreturn void airi_trap_fatal_demo(void); // 演示未登记故障的停止路径。
enum airi_memory_fault { AIRI_FAULT_READ, AIRI_FAULT_WRITE, AIRI_FAULT_EXEC }; // 三种作者明确登记的页权限演示。
void airi_trap_memory_probe(uint64_t address, enum airi_memory_fault kind); // 要求真实产生对应PF并恢复，否则停止。
#endif // 结束包含保护。
```

汇编新增可定位的 load/store 与间接调用现场，原寄存器保存和异常返回次序不变。每个恢复标签只适合自己登记的指令与栈形状，特别保留 NX 的内部返回项清理。

**完整替换 `kernel/arch/x86_64/traps.S`：**

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
    movw $24, %ax /* TSS 描述符从 GDT 第三项开始，占两项。 */
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
    movq %r12, %rsp /* 丢弃调用前可能增加的对齐空间。 */
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

C 分派在既有 RIP/vector/error 条件上增加 CR2。只有全部匹配才改恢复地址并登记 recovered；普通意外故障仍 panic，不因现在有三种演示就普遍允许跳过 PF。

**完整替换 `kernel/arch/x86_64/trap.c`：**

<!-- write: kernel/arch/x86_64/trap.c -->
```c
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
static uint64_t gdt[5]; // null、代码、数据和两项 TSS 描述符。
static struct idt_gate idt[256]; // 整张表属于内核静态内存，安装后持续有效。
static struct task_state tss; // TSS 也必须在 ltr 后持续存在。
static uint8_t double_fault_stack[16384] __attribute__((aligned(16))); // 原栈损坏时 DF 换这块栈。
extern const uintptr_t airi_trap_entries[32]; // 汇编产生的异常入口地址表。
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
    gdt[3] = limit | ((base & 0xffffff) << 16) | (0x89ull << 40) | (((base >> 24) & 0xff) << 56); // available 64 位 TSS，地址低 32 位拆入描述符。
    gdt[4] = base >> 32; // TSS 地址高 32 位存到紧随的第二项。
    tss.ist[0] = (uintptr_t)(double_fault_stack + sizeof(double_fault_stack)); // 硬件从栈顶向低地址压入 DF 现场。
    tss.io_offset = sizeof(tss); // 当前 TSS 内没有 I/O 权限位图。
    for (unsigned i = 0; i < 256; ++i) { // 全部项目初始化，避免继承加载器状态。
        uintptr_t address = i < 32 ? airi_trap_entries[i] : (uintptr_t)airi_trap_255; // 未使用向量也有明确停止入口。
        idt[i] = (struct idt_gate){(uint16_t)address, 8, i == 8 ? 1 : 0, 0x8e, (uint16_t)(address >> 16), (uint32_t)(address >> 32), 0}; // DPL0 中断门，只有 DF 使用 IST1。
    }
    struct table_pointer gdtr = {sizeof(gdt) - 1, (uintptr_t)gdt}; // CPU 复制描述符值，不保留这个局部对象指针。
    struct table_pointer idtr = {sizeof(idt) - 1, (uintptr_t)idt}; // IDT 本体则在静态区持续存在。
    airi_tables_load(&gdtr, &idtr); // 装载表和选择子，函数返回后表已经由本内核拥有。
    airi_log("[TRAP_READY] vectors=32 cs=8 double_fault_ist=1 interrupts=off"); // 该标记只表示异常设施，不宣称调度或 IRQ 完成。
}
void airi_trap_dispatch(struct airi_trap_frame *f) { // 汇编传入的指针指向当前异常栈。
    uint64_t cr2; // 读取最近页故障的线性地址，非 PF 时仅作为附带状态。
    __asm__ volatile("mov %%cr2,%0" : "=r"(cr2)); // =r 请求输出到编译器分配的通用寄存器。
    airi_log("[EXCEPTION] vector=%llu error=0x%llx rip=0x%llx cs=0x%llx rsp=0x%llx cr2=0x%llx", // 保存实际 CPU 返回现场，便于反汇编定位。
             (unsigned long long)f->vector, (unsigned long long)f->error, (unsigned long long)f->rip, // 向量/错误码/指令位置分别输出。
             (unsigned long long)f->cs, (unsigned long long)f->rsp, (unsigned long long)cr2); // 栈地址来自 CPU，而非 C 局部变量。
    if (expected_site && f->rip == expected_site && f->vector == expected_vector && f->error == expected_error && (f->vector != 14 || cr2 == expected_cr2)) { // 只接受精确登记的作者演示。
        f->rip = expected_resume; // 修改 iretq 将恢复的指令地址，不猜指令长度。
        expected_site = 0; // 一次性登记在返回前消费，不能无限跳过相同错误。
        recovered = true; // 调用者据此检查故障确实发生。
        return; // 汇编恢复寄存器并执行 iretq。
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
```

## 先重建启动所需映射，再让数据页走完整生命周期

memory.c 从已验证地图重建低地址 RAM 的 HHDM，不复制旧表。BOOT_RECLAIM 等范围可以为现有借用保留映射，却仍不进入可分配池；可访问与可借出是两个判断。任意 reserved/MMIO 不作普通 RAM，帧缓冲另保留原 VA 并用 PWT/PCD 设备缓存属性、NX；UART/PS2 的端口访问无需 MMIO。

全部启动映射成功才装 CR3。接着记空闲数量，借一个数据页，经 HHDM 写 0xc3，再以演示 VA 建只读 NX 别名并读回同一内容。随后写保护与 NX 故障，解除别名、回收空表，再读该地址产生不存在 PF；最后归还数据页，数量必须回到演示前。

HHDM 是受管理 RAM 的长期别名，不随每次借页建立/销毁，归还后旧指针仍有地址却已失去使用权。根和启动表继续由内核长期持有，恢复的只是临时演示资源。

**完整替换 `kernel/mm/memory.c`。** 先保留 K15 演示，再建立、激活与检查新空间。

<!-- write: kernel/mm/memory.c -->
```c
#include <airi/vm.h> // 使用自有四级页表建立内核映射。
#include <airi/trap.h> // 真实页权限故障由异常入口报告。
#include <airi/pages.h> // 物理页分配与启动信息。
#include <airi/kernel.h> // 日志和具体失败出口。
static struct airi_page_pool pages; // 页池元数据放在内核 BSS，不能自己被分配。
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
```

<a id="vm-boundaries"></a>

## 主机用同一算法核对共享路径与失败回滚

arena 是真实对齐数组，模拟物理地址只是编号，offset 将编号 4096 对到数组起点。相同 table/walk 算法因此可运行，却不会接管宿主页表。

两个相邻叶子共用中间表，解除第一项必须留下第二项的路径，全部解除才回收三层空表。另将空闲表页限为两张，新分支需三张，第三次失败应收回前两张、根项归零，调用者的数据页不被误收。只检查 map=false 无法证明这些结果，所以继续检查资源状态。真实权限和 CR3 仍由 guest 证明。

**新建 `tests/unit/vm_test.c`。** 模拟物理页只允许落在 arena 的十六页范围。

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
    assert(!airi_vm_activate(&space)); // 宿主版本明确不执行特权安装，不能记成硬件验证。
    assert(airi_page_free(&pool, space.root) && pool.free == 16); // 空根不再使用后，测试手动销毁最后一个所有权。
    puts("PASS vm: translation, permissions, shared tables, unmap, exhaustion rollback"); // 全部状态核对通过才打印。
    return 0; // 正常退出宿主观察。
}
```

guest 加入 paging.c，主机同样编译算法，但以 AIRI_HOST_TEST 排除特权安装。构建继续保留先前异常、页池与串口实现，才能观察换根前后执行结果。

**完整替换 `Makefile`：**

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
    kernel/console/console.c kernel/arch/x86_64/input.c kernel/arch/x86_64/cpu.c kernel/arch/x86_64/trap.c kernel/mm/pages.c kernel/mm/memory.c kernel/arch/x86_64/paging.c
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

所有映射与故障恢复均已接入，运行主机边界检查和当前配置的真实启动回归。

<!-- run -->
```sh
./airios build  # 构建自有页表内核与镜像。
PYTHONPATH=scripts python3 - <<'PYVM'  # 原样交给Python，保留引号和寄存器文字。
import subprocess  # 使用现有外部命令边界。
from pathlib import Path  # 读取本次串口记录。
from tools import ROOT, make_command  # 实际工程根和选定工具。
from boot import check  # 有界运行当前镜像。
subprocess.run(make_command(target='host-test'), check=True)  # 地址转换、共享表和耗尽回滚。
check('normal')  # 实际装CR3并观察CPU页权限。
text = (ROOT / 'artifacts/boot-normal/serial.log').read_text()  # 这是本轮新建的日志。
assert '[VM_DEMO_OK]' in text  # 三种故障与资源恢复必须真的完成。
for code in ('0x3', '0x11', '0x0'):  # 每一种预期错误码都须来自PF。
    assert 'vector=14 error=' + code + ' ' in text  # 边界空格避免前缀误匹配。
PYVM
```

既有 QEMU 记录中，VM_READY 的根物理地址为 0x1000，三次 PF 的 error 为 0x3、0x11、0，CR2 都是演示地址；NX 的 RIP 也为目标页。VM_DEMO_OK 后继续 READY。页池空闲量随内核大小变化，应比较本次演示前后，不能固守某个历史数量。

调试 activate 前比较 kernel_space.root 与 CR3，安装后再读 CR3；direct_offset 给出访问物理根页的别名，x/512gx 可按索引走四层。PF 断点比较 frame 的 rip/error 与 CR2，尤其看 NX 多出的内部返回地址。这些调试位置用于解释现场；既有运行范围见验证记录。

Linux mprotect 同样由内核更新权限并失效缓存，用户不会直接装 CR3。Linux 用户非法访问通常得到信号，当前自制内核只有精确登记演示可恢复，其他 PF 停止系统。

当前仅支持固定平台、四级、4 KiB supervisor 叶子；无用户位、大页、PCID、SMP TLB shootdown、按需缺页、交换或 COW。下一章在已拥有的映射上分配小对象，把数据页寿命与字节块寿命再分开。依据 [Intel 系统编程手册](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html) 的分页、WP/NXE、PF 与 INVLPG。
