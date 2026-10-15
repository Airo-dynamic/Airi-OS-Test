# K16 · 接管四级页表，让页权限产生真实故障

从 K15 的物理页池继续。我们已经拥有几块 RAM，却仍靠 Limine 的页表访问它们。本节用自己的物理页保存四级表，把当前代码、栈、启动信息和直接映射重新连起来，再把根页物理地址装入 CR3。随后让同一数据页在 HHDM 和一个新地址出现，实际观察只读写入、NX 执行和解除映射后的读取。

保留页池与 K14 的异常入口，先新增 vm.h 和 paging.c，再更新链接边界、三个异常文件、memory.c、主机观察和 Makefile。最后才运行。此处建立的是内核地址空间，所有叶子都为 supervisor；用户页、用户指针检查和进程隔离还未实现。

<a id="four-levels"></a>
## 一次读指令怎样找到物理页

现在 CPU 用 48 位规范虚拟地址。第 47 位是高位扩展的符号位：低半区以上高位全零，高半区以上高位全一，夹在中间的地址无效。一个 4 KiB 页占低 12 位偏移；剩下的 36 位分成四组，每组 9 位，分别选择 PML4、PDPT、PD、PT 中的一个表项。每表 512 项、每项 8 字节，正好一页。CR3 存的是 PML4 的物理页地址，不是 C 指针。

以 `0xffffc00000000000` 为例，PML4 索引为 384，其余三个索引和页内偏移都是 0。这个分支和当前 HHDM、内核链接地址分开，可以在演示结束时完整拆除。CPU 从根取第 384 项，按里面的物理地址读下一表，再连续选择第 0 项，最终叶子给出数据页的物理起点，低 12 位偏移保持不变。

PTE 的低位不是地址：bit0 表示存在、bit1 允许写；bit63 是 NX，禁止取指。中间层本实现固定允许 supervisor 写，最终权限由叶子继续限制。所有层的用户位都未设置。CPU 做 supervisor 写时还需要 CR0.WP 开启，NX 需要处理器支持并开启 EFER.NXE。只在软件结构里写几个位不能证明硬件已经执行权限检查。

**新建 `include/airi/vm.h`。** 地址空间拥有根与中间页表，映射中的数据页仍由传入它的调用者持有。

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
## 创建一条路径，失败时收回本次所有权

C 遍历页表也要访问虚拟地址，所以把页表物理地址加 HHDM 偏移后再解引用。构建新根时还在用 Limine 的 HHDM；切换之后，新根会保留相同布局。新借出的页必须先清零再接到父表，否则旧内容可能被 CPU 当成存在的映射。

一次映射最多缺三张中间表。made 保存本次借出的物理页，links 保存指向它们的父表槽位；count 只统计已经建立的连接。中途耗尽、发现大页或重复叶子时，只拆本次连接，保留旧分支，失效活动地址空间的缓存后再归还新页。`goto rollback` 把多个失败点收拢到同一个所有权出口，不会跳过已经声明的清理状态。

下面先写辅助函数、create 和 map。物理地址掩码覆盖 x86 表项最多 52 位的地址字段，实际映射目标仍由固定平台的 RAM/设备描述提供；这不是对任意 CPU 物理位宽的探测。当前不支持大页，遇到大页标志就拒绝继续遍历。

**新建 `kernel/arch/x86_64/paging.c`，先写这一段：**

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
## 查询不访存，解除先断开再归还

软件 lookup 读取页表，返回物理地址和叶子权限，不读取目标数据页，因此不存在映射时返回 false，不主动引发 PF。walk 同时保存四层表指针和索引，unmap 才能沿原路检查空表。查询允许页内地址；映射与解除只接受页起点。

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

解除叶子后，不能立刻归还数据页，也不能先归还仍由父表引用的页表。先检查当前表是否仍有其他项；只有整表为空才断开父链接，并把物理地址暂存到 release。全部链接更新完成，再对目标 VA 执行 invlpg，使 TLB 和有关的分页结构缓存失效，最后才归还空表。相邻映射共享的表必须继续存在；根页则由地址空间一直持有。

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
## 开启硬件权限并安装新根

CPUID 扩展叶报告 NX 能力；内联汇编的输出约束 `=a`、`=d` 等指定寄存器，输入 `a/c` 提供叶号和子叶号。RDMSR/WRMSR 通过 ECX 选择 EFER，用 EDX:EAX 交接其 64 位值；必须保留原有 long-mode 位，只加 NXE。CR0 的 WP 位使内核写也遵守只读叶子。

固定环境使用四级表且不启用 PCID；代码明确拒绝五级或 PCID 配置。清 CR4.PGE 是为了使加载器可能留下的全局翻译也失效，再装 CR3。`memory` clobber 告诉编译器这个动作影响内存可见性，不能把前后的访问随意重排。主机编译版本只返回 false，不执行任何特权指令，硬件是否生效必须由 guest 验证。

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
## 切换前保留正在运行的东西

链接脚本新增三个页对齐符号：text 开始、text 结束、data 开始。requests/rodata 映射为只读 NX，text 为只读可执行，data/BSS 为可写 NX。当前栈、IDT、TSS、位图和 BootInfo 都在内核 BSS/数据范围，必须一起映射。物理内存中的内核别名也按同样边界关闭写权限，并全部 NX，避免通过 HHDM 绕过 text/rodata 的只读限制。

**完整替换 `kernel/linker.ld`。** 符号表示地址位置，不是在内存中存放这些地址的变量。

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
## 恢复的是哪一条指令、哪一个栈

页故障向量是 14，CR2 给出访问的虚拟地址。错误码 bit0 表示保护失败而非不存在，bit1 表示写，bit2 表示用户访问，bit4 表示取指。本节的三个预期错误码分别为 3、17、0；处理函数还必须核对精确 RIP 和 CR2，不能让某个错误映射导致的其他 PF 也被误当成演示成功。

读写故障的 RIP 指向汇编里的 load/store，恢复标签跳过整条已知指令。NX 故障有一个关键区别：`call *%rdi` 已经把内部返回地址压栈，CPU 在目标地址取指时才报错，因此保存 RIP 是目标页地址，保存 RSP 已指向那个内部返回地址。NX 的恢复标签需要先丢弃这一项，再 ret 回 C 调用者；直接 ret 会回到错误的调用层次。

演示数据页第一个字节写成 ret。若 NX 没生效，这个函数会普通返回，C 仍会因为 recovered=false 而拒绝通过。这样完成标记证明的是硬件确实产生并恢复 PF，而不是调用恰好没有崩溃。

**完整替换 `include/airi/trap.h`。** 增加三种内存故障演示入口。

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

**完整替换 `kernel/arch/x86_64/traps.S`。** 原异常保存/恢复次序保持，增加三个可定位的访存/取指入口。

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

**完整替换 `kernel/arch/x86_64/trap.c`。** PF 恢复增加 CR2 比对，普通意外故障继续停止。

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

memory.c 从已验证地图重建所需低地址 RAM 的 HHDM，不复制旧页表。保留 RAM 的映射不等于允许分配它：BOOT_RECLAIM 等范围可以映射以保持借用，却仍不进入页池。任意 reserved/MMIO 范围不当作普通 RAM；有效帧缓冲单独保留原虚拟位置，采用 PWT/PCD 设备缓存属性，不可执行。CPU 端口访问的 UART 和 PS/2 不需要 MMIO 映射。

所有启动映射建立成功后才装 CR3。然后借出一个数据页，在 HHDM 写入标记，在新别名读取，执行三种 PF 演示，解除别名并回收空表，最后归还数据页。HHDM 是页池内存的长期别名，不按每次数据页借用创建和销毁；归还后调用者必须停止使用旧指针。空闲数量恢复到演示前值，根与启动表仍由内核长期持有。

**完整替换 `kernel/mm/memory.c`。** 保留 K15 页分配演示，接着建立并激活新地址空间。

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
## 验证半途失败和共享表的寿命

主机 arena 是一块真实的对齐数组，但其中的物理地址只是实验编号。offset 让编号 4096 转到数组起点，因此和 guest 使用同一遍历代码而不访问宿主页表。两个相邻叶子共享中间表，解除第一个不能回收仍被第二个使用的表；全部解除才归还三层空表。

耗尽演示把空闲页限制为两张，而创建新分支需要三张；第三次失败必须退还前两张，根项恢复零，传入的数据页仍由原调用者持有。这比单纯核对 map 返回 false 多验证了失败后的资源状态。主机不安装 CR3，真实 WP/NX/PF 证据仍来自 QEMU。

**新建 `tests/unit/vm_test.c`。** 这里所有模拟物理页都只能落在 arena 的十六页范围。

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

**完整替换 `Makefile`。** guest 增加架构页表实现，host-test 用 AIRI_HOST_TEST 排除特权安装。

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

本轮 QEMU 在 `[VM_READY]` 报告根物理地址 0x1000，随后三条 PF 分别为 error=0x3、0x11、0x0，CR2 都是 0xffffc00000000000；NX 一条的 RIP 也正是这个目标地址。最后 `[VM_DEMO_OK]` 报告表已回收并继续 READY。页池空闲数量随内核大小变动，以本轮日志为准。

调试时在 `airi_vm_activate` 前停住，用 GDB `p/x kernel_space.root` 与 `p/x $cr3` 比较尚未切换的两根；步过安装后再读 CR3。`p/x kernel_space.direct_offset` 给出访问根页的别名偏移，`x/512gx` 可以沿四层索引查看实际表项。PF 断点停在 `airi_trap_dispatch` 时，比较 frame->rip、frame->error 和 `$cr2`，尤其观察 NX 的栈为何多一个内部返回地址。调试命令是操作说明；硬件自动运行证据以本轮串口记录为准。

作者已经完整实现权限和解除映射的故障演示，不需要学习者自行破坏运行环境。与 Linux `mprotect` 的联系是：页权限由内核写入页表并使缓存失效，用户程序不直接装 CR3。Linux 通常把非法用户访问转为进程信号；当前 AiriOS 没有用户进程，只有精确登记的演示能恢复，其他故障停止整个内核。

当前仅支持 4 KiB supervisor 叶子、四级分页和固定平台；不支持用户位、大页、PCID、SMP TLB shootdown、按需缺页、交换或 COW。下一节在明确持有的页面上提供字节级内核堆，把页所有权和小对象寿命再分开。

实现依据：[Intel 系统编程手册](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html) 的四级分页、CR0.WP、EFER.NXE、页故障错误码和 INVLPG 章节。
