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

**局部更新 `include/airi/vm.h`。** 新增USER权限、借用标记和非活动空间销毁接口。

依次找到以下旧窗口，各用对应新文本完整替换；文件其他部分保留。

<!-- replace: include/airi/vm.h -->
<!-- before -->
```c
#define AIRI_VM_EXEC 2u // 显式可执行；不设置则 NX。
#define AIRI_VM_DEVICE 4u // 设备映射禁用普通缓存，不用于 RAM。
#define AIRI_VM_DEMO_ADDRESS 0xffffc00000000000ull // 演示使用的独立高半区地址。
struct airi_address_space { // 根页物理地址与访问页表页所需的现有直接映射。
    uint64_t root, direct_offset; // CR3 需要 root，C 遍历页表需要 direct_offset。
    struct airi_page_pool *pages; // 页表拥有者借用的页池，必须比地址空间存活更久。
};
```
<!-- after -->
```c
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
```

继续替换下一处：

<!-- replace: include/airi/vm.h -->
<!-- before -->
```c
bool airi_vm_lookup(const struct airi_address_space *space, uint64_t virtual_address, uint64_t *physical, unsigned *flags); // 查询页内地址，不存在返回 false。
bool airi_vm_activate(const struct airi_address_space *space); // x86 启用 NX/WP 并装 CR3；主机算法观察不执行此动作。
#endif // 结束包含保护。
```
<!-- after -->
```c
bool airi_vm_lookup(const struct airi_address_space *space, uint64_t virtual_address, uint64_t *physical, unsigned *flags); // 查询页内地址，不存在返回 false。
bool airi_vm_activate(const struct airi_address_space *space); // x86 启用 NX/WP 并装 CR3；主机算法观察不执行此动作。
bool airi_vm_clone_kernel(struct airi_address_space *space, const struct airi_address_space *kernel); // 新根只复制高半区链接，低半区从空开始。
bool airi_vm_destroy(struct airi_address_space *space); // 仅销毁非活动空间拥有的表，数据页仍归调用者。
#endif // 结束包含保护。
```

**局部更新 `kernel/arch/x86_64/paging.c`。** 普通map/unmap继续保留回滚和缓存顺序，高半区借用增加明确保护。

依次找到以下旧窗口，各用对应新文本完整替换；文件其他部分保留。

<!-- replace: kernel/arch/x86_64/paging.c -->
<!-- before -->
```c
#include <airi/base.h> // 新页表必须用自有 memset 清零。
#define PRESENT 1ull // x86 PTE 的存在位。
#define WRITE 2ull // 页表或叶子允许写入。
#define PWT 8ull // 设备映射的 write-through 位。
#define PCD 16ull // 设备映射的 cache-disable 位。
#define LARGE 128ull // 此实现拒绝已有大页项，不把它当下一级表。
```
<!-- after -->
```c
#include <airi/base.h> // 新页表必须用自有 memset 清零。
#define PRESENT 1ull // x86 PTE 的存在位。
#define WRITE 2ull // 页表或叶子允许写入。
#define USER 4ull // 每层都要允许user，不能只改叶子。
#define PWT 8ull // 设备映射的 write-through 位。
#define PCD 16ull // 设备映射的 cache-disable 位。
#define LARGE 128ull // 此实现拒绝已有大页项，不把它当下一级表。
```

继续替换下一处：

<!-- replace: kernel/arch/x86_64/paging.c -->
<!-- before -->
```c
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
```
<!-- after -->
```c
    uint64_t physical = airi_page_alloc(pages); // 根页也由页池记录所有权。
    if (!physical) // 耗尽时没有地址可清零。
        return false; // 保持输出对象不变。
    *s = (struct airi_address_space){.root=physical, .direct_offset=offset, .pages=pages}; // 此后地址空间持有根页，不能归还给其他用途。
    airi_memset(table(s, physical), 0, AIRI_PAGE_SIZE); // 清除旧所有者内容，初始没有任何映射。
    return true; // 成功拥有一个空根表。
}
bool airi_vm_map(struct airi_address_space *s, uint64_t address, uint64_t physical, unsigned flags) { // 按入口已验证的 flags 建立 4 KiB 映射，支持 supervisor 与 USER 叶子。
    if (!s || !s->root || !canonical(address) || address % AIRI_PAGE_SIZE || (physical & ~ADDRESS) || (flags & ~(AIRI_VM_WRITE | AIRI_VM_EXEC | AIRI_VM_DEVICE | AIRI_VM_USER))) // 拒绝不规范、未对齐、超物理位宽与未知权限。
        return false; // 输入错误不改变页表或页池。
    if (((flags & AIRI_VM_USER) && address >= AIRI_USER_LIMIT) || (s->borrowed_kernel && (!(flags & AIRI_VM_USER) || address >= AIRI_USER_LIMIT))) // 用户空间不能改写借用的内核高半区。
        return false; // 也不在借用空间中混入自有supervisor低半区分支。
    uint64_t made[3]; // 保存本次新借出的中间页表，失败时逆序归还。
    uint64_t *links[3]; // 对应父表项的地址，用于拆除本次建立的连接。
    unsigned count = 0; // 只有成功新建的中间表才计入回滚范围。
```

继续替换下一处：

<!-- replace: kernel/arch/x86_64/paging.c -->
<!-- before -->
```c
    for (unsigned level = 3; level > 0; --level) { // 依次消费第 39、30、21 位起的 9 位索引。
        unsigned index = (unsigned)((address >> (12 + level * 9)) & 511); // 512 项表占一页，每项 8 字节。
        uint64_t *entry = &current[index]; // 父表中指向下一层的槽位。
        if (*entry & LARGE) // 当前实现从不混合 2 MiB/1 GiB 大页。
            goto rollback; // 不把大页物理起点当页表解引用。
        if (!(*entry & PRESENT)) { // 缺少下一层时才借出页表页。
            uint64_t next = airi_page_alloc(s->pages); // 分配失败必须收回此前新建的表。
```
<!-- after -->
```c
    for (unsigned level = 3; level > 0; --level) { // 依次消费第 39、30、21 位起的 9 位索引。
        unsigned index = (unsigned)((address >> (12 + level * 9)) & 511); // 512 项表占一页，每项 8 字节。
        uint64_t *entry = &current[index]; // 父表中指向下一层的槽位。
        if ((*entry & LARGE) || ((*entry & PRESENT) && (flags & AIRI_VM_USER) && !(*entry & USER))) // 不支持大页，也不提升既有supervisor路径为user。
            goto rollback; // 不把大页物理起点当页表解引用。
        if (!(*entry & PRESENT)) { // 缺少下一层时才借出页表页。
            uint64_t next = airi_page_alloc(s->pages); // 分配失败必须收回此前新建的表。
```

继续替换下一处：

<!-- replace: kernel/arch/x86_64/paging.c -->
<!-- before -->
```c
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
```
<!-- after -->
```c
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
```

继续替换下一处：

<!-- replace: kernel/arch/x86_64/paging.c -->
<!-- before -->
```c
    if (physical) // 调用者可只关心是否存在。
        *physical = (leaf & ADDRESS) | (address & (AIRI_PAGE_SIZE - 1)); // 页内偏移在翻译前后保持不变。
    if (flags) // 可选返回本实现支持的叶子权限。
        *flags = ((leaf & WRITE) ? AIRI_VM_WRITE : 0) | ((leaf & NX) ? 0 : AIRI_VM_EXEC) | ((leaf & PCD) ? AIRI_VM_DEVICE : 0); // 中间表由本实现固定为 supervisor RW。
    return true; // 只表示软件页表存在，不代表设备或 RAM 访问一定成功。
}
bool airi_vm_unmap(struct airi_address_space *s, uint64_t address) { // 调用方必须避免解除当前指令、栈或页表自身的映射。
    uint64_t *tables[4]; // 保存由叶子向根回收所需的路径。
    unsigned indices[4]; // 对应每个父子链接。
    if (address % AIRI_PAGE_SIZE || !walk(s, address, tables, indices)) // 只接受存在的页起点。
        return false; // 不存在映射不会改变任何所有权。
    tables[0][indices[0]] = 0; // 先移除目标数据页的映射。
    uint64_t release[3]; // 在拆除父链接时暂存待归还的空表。
```
<!-- after -->
```c
    if (physical) // 调用者可只关心是否存在。
        *physical = (leaf & ADDRESS) | (address & (AIRI_PAGE_SIZE - 1)); // 页内偏移在翻译前后保持不变。
    if (flags) // 可选返回本实现支持的叶子权限。
        *flags = ((leaf & WRITE) ? AIRI_VM_WRITE : 0) | ((leaf & NX) ? 0 : AIRI_VM_EXEC) | ((leaf & PCD) ? AIRI_VM_DEVICE : 0) | ((leaf & USER) ? AIRI_VM_USER : 0); // 按叶子位还原支持的权限；USER 中间层许可已在建图时逐层检查。
    return true; // 只表示软件页表存在，不代表设备或 RAM 访问一定成功。
}
bool airi_vm_unmap(struct airi_address_space *s, uint64_t address) { // 调用方必须避免解除当前指令、栈或页表自身的映射。
    uint64_t *tables[4]; // 保存由叶子向根回收所需的路径。
    unsigned indices[4]; // 对应每个父子链接。
    if ((s && s->borrowed_kernel && address >= AIRI_USER_LIMIT) || address % AIRI_PAGE_SIZE || !walk(s, address, tables, indices)) // 只接受存在的页起点。
        return false; // 不存在映射不会改变任何所有权。
    tables[0][indices[0]] = 0; // 先移除目标数据页的映射。
    uint64_t release[3]; // 在拆除父链接时暂存待归还的空表。
```

继续替换下一处：

<!-- replace: kernel/arch/x86_64/paging.c -->
<!-- before -->
```c
    return false; // 主机明确不能证明硬件激活。
#endif // 结束目标差异。
}
```
<!-- after -->
```c
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

用户执行期间，RSP 指向自己的用户页。int80 若继续在这里保存内核现场，就会让用户决定内核向哪里写。跨权限进入时，CPU 因而从 TSS.RSP0 取得另一份内核栈，把旧用户 RSP 只作为待恢复的数据保存。

本次 prepare 先用 `airi_memory_region_create(&process.stack,KERNEL_STACK,4)` 建立四页 supervisor 栈，再克隆用户根，使其高半区能借到这条映射。用户入口使用 0x70000000 的一页栈，陷入入口使用 process.stack 的高端，两者地址、权限和所有者分别明确；外部用户 RSP 不能替代 TSS 的栈地址。

退出仍要等当前内核调用离开这四页。finish 决定结束用户运行时，自己的调用现场还在其中；它先切回长期根并选择启动现场。C 返回以后，汇编真正把 RSP 换到启动现场，再 pop/iretq 回 run_one，cleanup 才能销毁用户表、归还用户数据页，最后释放用户内核栈。

memory服务为内部所有者提供取页、还页、物理别名与克隆根入口。HHDM指针仍是内核借用，不暴露给用户程序或UI。用户数据清零避免泄漏此前页面内容，准备失败也通过同一cleanup回收半成品。

**局部更新 `include/airi/memory.h`。** 页表声明采用不完全类型，文本消费者继续只用原有数值快照。

依次找到以下旧窗口，各用对应新文本完整替换；文件其他部分保留。

<!-- replace: include/airi/memory.h -->
<!-- before -->
```c
bool airi_memory_region_create(struct airi_kernel_region *region, uint64_t address, unsigned count); // 建立新的有界RW/NX内核区域，不覆盖已有映射。
bool airi_memory_region_free(struct airi_kernel_region *region); // 所有借用结束后先解除映射，再归还数据页。
#endif // 结束包含保护。
```
<!-- after -->
```c
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

**局部更新 `kernel/mm/memory.c`。** 原有初始化不变，末尾增加用户空间内部所有权操作。

在 `kernel/mm/memory.c` 末尾追加本次接口；已有实现保留。

<!-- append: kernel/mm/memory.c -->
```c

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

**局部更新 `include/airi/trap.h`。** dispatch返回现场指针，TSS栈顶与CR2读取继续留在架构接口。

依次找到以下旧窗口，各用对应新文本完整替换；文件其他部分保留。

<!-- replace: include/airi/trap.h -->
<!-- before -->
```c
};
void airi_trap_init(void); // 安装自己的 GDT、TSS 和 IDT，保持可屏蔽中断关闭。
void airi_trap_dispatch(struct airi_trap_frame *frame); // 仅供汇编入口调用。
void airi_trap_demo(void); // 完整演示可恢复的 UD2 与带错误码的 GP。
_Noreturn void airi_trap_fatal_demo(void); // 演示未登记故障的停止路径。
enum airi_memory_fault { AIRI_FAULT_READ, AIRI_FAULT_WRITE, AIRI_FAULT_EXEC }; // 三种作者明确登记的页权限演示。
void airi_trap_memory_probe(uint64_t address, enum airi_memory_fault kind); // 要求真实产生对应PF并恢复，否则停止。
#endif // 结束包含保护。
```
<!-- after -->
```c
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

**局部更新 `kernel/arch/x86_64/traps.S`。** int80/int129产生统一vector/error，公共返回改用C返回的RAX现场指针。

依次找到以下旧窗口，各用对应新文本完整替换；文件其他部分保留。

<!-- replace: kernel/arch/x86_64/traps.S -->
<!-- before -->
```asm
    movw %ax, %es /* 设置额外数据段选择子。 */
    movw %ax, %ss /* 设置内核栈段选择子。 */
    movw $24, %ax /* TSS 描述符从 GDT 第三项开始，占两项。 */
    ltr %ax /* 为双重故障加载独立栈信息。 */
    lidt (%rsi) /* 后续异常从自己的 IDT 取入口。 */
```
<!-- after -->
```asm
    movw %ax, %es /* 设置额外数据段选择子。 */
    movw %ax, %ss /* 设置内核栈段选择子。 */
    movw $40, %ax /* TSS 描述符从 GDT 第五项开始，占两项。 */
    ltr %ax /* 加载 TSS，提供双重故障 IST 与用户跨权时的 RSP0。 */
    lidt (%rsi) /* 后续异常从自己的 IDT 取入口。 */
```

继续替换下一处：

<!-- replace: kernel/arch/x86_64/traps.S -->
<!-- before -->
```asm
TRAP 31 /* 保留向量。 */
TRAP 32 /* 主PIC IRQ0对应的时钟入口，无硬件错误码。 */
TRAP 255 /* 未单独登记的 IDT 项进入统一停止入口。 */
airi_trap_common: /* CPU 现场和 vector/error 已形成。 */
```
<!-- after -->
```asm
TRAP 31 /* 保留向量。 */
TRAP 32 /* 主PIC IRQ0对应的时钟入口，无硬件错误码。 */
TRAP 128 /* DPL3的AiriOS系统调用门。 */
TRAP 129 /* DPL0启动/返回用户执行的内核门。 */
TRAP 255 /* 未单独登记的 IDT 项进入统一停止入口。 */
airi_trap_common: /* CPU 现场和 vector/error 已形成。 */
```

继续替换下一处：

<!-- replace: kernel/arch/x86_64/traps.S -->
<!-- before -->
```asm
    andq $-16, %rsp /* call 之前对齐到 16 字节，不假设异常发生点的栈对齐。 */
    call airi_trap_dispatch /* C 负责报告与明确登记的演示恢复。 */
    movq %r12, %rsp /* 丢弃调用前可能增加的对齐空间。 */
    popq %r15 /* 从保存区域恢复通用寄存器。 */
    popq %r14 /* 顺序与压栈相反。 */
```
<!-- after -->
```asm
    andq $-16, %rsp /* call 之前对齐到 16 字节，不假设异常发生点的栈对齐。 */
    call airi_trap_dispatch /* C 负责报告与明确登记的演示恢复。 */
    movq %rax, %rsp /* C返回选定的完整现场，同一或另一条调用链。 */
    popq %r15 /* 从保存区域恢复通用寄存器。 */
    popq %r14 /* 顺序与压栈相反。 */
```

**局部更新 `kernel/arch/x86_64/trap.c`。** 先按来源权限分流用户故障，CPU内核故障仍PANIC；user IRQ返回也要检查现场。

依次找到以下旧窗口，各用对应新文本完整替换；文件其他部分保留。

<!-- replace: kernel/arch/x86_64/trap.c -->
<!-- before -->
```c
#include <airi/timer.h> // 唯一开放的设备IRQ交给架构时钟驱动。
#include <airi/trap.h> // 异常现场的 C 布局。
```
<!-- after -->
```c
#include <airi/user.h> // 用户陷入可选择另一条完整返回现场。
#include <airi/timer.h> // 唯一开放的设备IRQ交给架构时钟驱动。
#include <airi/trap.h> // 异常现场的 C 布局。
```

继续替换下一处：

<!-- replace: kernel/arch/x86_64/trap.c -->
<!-- before -->
```c
_Static_assert(sizeof(struct idt_gate) == 16, "IDT layout"); // 每个门必须恰好 16 字节。
_Static_assert(sizeof(struct airi_trap_frame) == 176, "trap layout"); // 栈含 15+2+5 个 8 字节槽。
static uint64_t gdt[5]; // null、代码、数据和两项 TSS 描述符。
static struct idt_gate idt[256]; // 整张表属于内核静态内存，安装后持续有效。
static struct task_state tss; // TSS 也必须在 ltr 后持续存在。
static uint8_t double_fault_stack[16384] __attribute__((aligned(16))); // 原栈损坏时 DF 换这块栈。
extern const uintptr_t airi_trap_entries[32]; // 汇编产生的异常入口地址表。
extern void airi_trap_32(void); // IRQ0也复用完整通用寄存器保存入口。
extern void airi_trap_255(void); // 未启用的向量转到统一停止路径。
```
<!-- after -->
```c
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
```

继续替换下一处：

<!-- replace: kernel/arch/x86_64/trap.c -->
<!-- before -->
```c
    uintptr_t base = (uintptr_t)&tss; // TSS 描述符使用内核虚拟地址。
    uint64_t limit = sizeof(tss) - 1; // 描述符界限是最后一个有效字节偏移。
    gdt[3] = limit | ((base & 0xffffff) << 16) | (0x89ull << 40) | (((base >> 24) & 0xff) << 56); // available 64 位 TSS，地址低 32 位拆入描述符。
    gdt[4] = base >> 32; // TSS 地址高 32 位存到紧随的第二项。
    tss.ist[0] = (uintptr_t)(double_fault_stack + sizeof(double_fault_stack)); // 硬件从栈顶向低地址压入 DF 现场。
    tss.io_offset = sizeof(tss); // 当前 TSS 内没有 I/O 权限位图。
```
<!-- after -->
```c
    uintptr_t base = (uintptr_t)&tss; // TSS 描述符使用内核虚拟地址。
    uint64_t limit = sizeof(tss) - 1; // 描述符界限是最后一个有效字节偏移。
    gdt[3] = 0x00affa000000ffffull; // ring3、64位、只读可执行代码段。
    gdt[4] = 0x00cff2000000ffffull; // ring3、可写数据段。
    gdt[5] = limit | ((base & 0xffffff) << 16) | (0x89ull << 40) | (((base >> 24) & 0xff) << 56); // available 64 位 TSS，地址低 32 位拆入描述符。
    gdt[6] = base >> 32; // TSS 地址高 32 位存到紧随的第二项。
    tss.ist[0] = (uintptr_t)(double_fault_stack + sizeof(double_fault_stack)); // 硬件从栈顶向低地址压入 DF 现场。
    tss.io_offset = sizeof(tss); // 当前 TSS 内没有 I/O 权限位图。
```

继续替换下一处：

<!-- replace: kernel/arch/x86_64/trap.c -->
<!-- before -->
```c
        if (i == 32) // 唯一开放的设备向量有自己的编号，不能冒充未知255。
            address = (uintptr_t)airi_trap_32; // 指向TRAP32生成的入口。
        idt[i] = (struct idt_gate){(uint16_t)address, 8, i == 8 ? 1 : 0, 0x8e, (uint16_t)(address >> 16), (uint32_t)(address >> 32), 0}; // DPL0 中断门，只有 DF 使用 IST1。
    }
    struct table_pointer gdtr = {sizeof(gdt) - 1, (uintptr_t)gdt}; // CPU 复制描述符值，不保留这个局部对象指针。
```
<!-- after -->
```c
        if (i == 32) // 唯一开放的设备向量有自己的编号，不能冒充未知255。
            address = (uintptr_t)airi_trap_32; // 指向TRAP32生成的入口。
        if (i == 128 || i == 129) // 两个软件中断各有固定身份。
            address = i == 128 ? (uintptr_t)airi_trap_128 : (uintptr_t)airi_trap_129; // 不能让它们落入未知向量255。
        idt[i] = (struct idt_gate){(uint16_t)address, 8, i == 8 ? 1 : 0, i == 128 ? 0xee : 0x8e, (uint16_t)(address >> 16), (uint32_t)(address >> 32), 0}; // int80门DPL3，其余DPL0；仅DF使用IST1。
    }
    struct table_pointer gdtr = {sizeof(gdt) - 1, (uintptr_t)gdt}; // CPU 复制描述符值，不保留这个局部对象指针。
```

继续替换下一处：

<!-- replace: kernel/arch/x86_64/trap.c -->
<!-- before -->
```c
    airi_log("[TRAP_READY] vectors=32 cs=8 double_fault_ist=1 interrupts=off"); // 该标记只表示异常设施，不宣称调度或 IRQ 完成。
}
void airi_trap_dispatch(struct airi_trap_frame *f) { // 汇编传入的指针指向当前异常栈。
    if (f->vector == 32) { // PIC重映射后的时钟，不能作为CPU故障停止。
        airi_timer_interrupt(); // 计数并确认IRQ，保持当前任务现场不变。
        return; // 仍由同一汇编入口恢复全部寄存器和RFLAGS。
    }
    uint64_t cr2; // 读取最近页故障的线性地址，非 PF 时仅作为附带状态。
```
<!-- after -->
```c
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
```

继续替换下一处：

<!-- replace: kernel/arch/x86_64/trap.c -->
<!-- before -->
```c
        expected_site = 0; // 一次性登记在返回前消费，不能无限跳过相同错误。
        recovered = true; // 调用者据此检查故障确实发生。
        return; // 汇编恢复寄存器并执行 iretq。
    }
    airi_panic("unhandled CPU exception"); // 未预期异常不返回有问题的指令。
```
<!-- after -->
```c
        expected_site = 0; // 一次性登记在返回前消费，不能无限跳过相同错误。
        recovered = true; // 调用者据此检查故障确实发生。
        return f; // 汇编恢复同一个故障演示现场并执行iretq。
    }
    airi_panic("unhandled CPU exception"); // 未预期异常不返回有问题的指令。
```

继续替换下一处：

<!-- replace: kernel/arch/x86_64/trap.c -->
<!-- before -->
```c
        airi_panic("page fault probe did not recover"); // 不把普通函数返回误记为权限生效。
}
```
<!-- after -->
```c
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

在 `kernel/arch/x86_64/trap.c` 中同步本阶段已变化的条件说明；执行语句保留。旧块用于定位，只写回对应新块。

<!-- replace: kernel/arch/x86_64/trap.c -->
<!-- before -->
```c
} __attribute__((packed)); // 位布局必须符合处理器读取格式。
struct task_state { // 64 位 TSS 只保存硬件切栈所需地址，不承担任务调度。
    uint32_t reserved0; // 固定保留字段。
    uint64_t rsp[3]; // 从用户态进入内核时未来使用的栈。
    uint64_t reserved1, ist[7], reserved2; // IST1 放双重故障栈，其他保持零。
    uint16_t reserved3, io_offset; // I/O 权限位图起点设到结构之后，当前不提供位图。
} __attribute__((packed)); // TSS 的硬件偏移不能被 C 对齐改变。
```
<!-- after -->
```c
} __attribute__((packed)); // 位布局必须符合处理器读取格式。
struct task_state { // 64 位 TSS 只保存硬件切栈所需地址，不承担任务调度。
    uint32_t reserved0; // 固定保留字段。
    uint64_t rsp[3]; // TSS 的 RSP0–RSP2；当前只设置 RSP0 供用户跨权进入 ring0。
    uint64_t reserved1, ist[7], reserved2; // IST1 放双重故障栈，其他保持零。
    uint16_t reserved3, io_offset; // I/O 权限位图起点设到结构之后，当前不提供位图。
} __attribute__((packed)); // TSS 的硬件偏移不能被 C 对齐改变。
```

<a id="int80-contract"></a>
## 这条系统调用具体传什么

程序在用户态准备的是调用参数，内核取得这些数值以后才决定能否代办。这是 AiriOS 自有 int80 ABI，与 Linux 的 syscall 指令和编号不兼容：RAX 交调用号并带回结果，RDI/RSI/RDX 交前三个参数，其他保存的通用寄存器恢复原值。当前编号为 1=write(fd,ptr,len)、2=exit(code)、3=getpid、5=ticks，未知号返回 −38。

write 只接受 fd1/2，逻辑长度最多 256 字节；错误分别为 −9（FD）、−22（长度）和 −14（用户范围），成功返回逻辑长度。指针只是用户交来的数，不能直接交给串口：先确认范围不越用户低半区，再逐页确认完整范围都有 USER 叶子。

用户汇编实际给出的第二个坏范围是 `ptr=0x70000ffc,len=8`：前四字节仍在栈页内，后四字节已越过 0x70001000，那里没有映射。copy_user 的第一轮只查权限，不复制或输出；走到下一页失败便整体返回 false，write 将 −14 存进保存的 RAX，前四字节也不会送出。若边复制边输出再查下一页，就会形成失败却已输出前缀的不同语义。

整段通过后，第二轮才逐页查询物理位置，经 HHDM 把字节复制到当前内核栈的 buffer。串口同步消费这份副本，返回逻辑 len；汇编恢复保存的 RAX，用户才用 cmpq 检查结果。当前没有并发映射修改或按需缺页，两轮处于同一次 IF 关闭的陷入中，检查成立的映射在复制时继续有效。

长度输出还允许缓冲区中有 NUL，不能用字符串终止零代替 len。UART 仍把文本换行转换为 CRLF，返回值却是用户提供的逻辑字节数；它没有宣称完整 POSIX 文件写入语义。

**局部更新 `include/airi/kernel.h`。** 增加同步长度输出声明。

依次找到以下旧窗口，各用对应新文本完整替换；文件其他部分保留。

<!-- replace: include/airi/kernel.h -->
<!-- before -->
```c
_Noreturn void airi_panic(const char *message);      // 先报告具体错误再停机，参数须为有效 NUL 字符串
bool airi_dashboard_draw(const AiriBootInfo *info);  // 预留图形接口，当前入口未调用，因此本章无需提供定义
#endif  // 结束头文件包含保护，后续使用同一公开声明
```
<!-- after -->
```c
_Noreturn void airi_panic(const char *message);      // 先报告具体错误再停机，参数须为有效 NUL 字符串
bool airi_dashboard_draw(const AiriBootInfo *info);  // 预留图形接口，当前入口未调用，因此本章无需提供定义
void airi_serial_write_bytes(const void *bytes, size_t count); // 按长度同步输出用户副本，不依赖NUL。
#endif  // 结束头文件包含保护，后续使用同一公开声明
```

**局部更新 `kernel/arch/x86_64/serial.c`。** 新接口只读取内核已经验证并复制好的范围。

在 `kernel/arch/x86_64/serial.c` 末尾追加本次接口；已有实现保留。

<!-- append: kernel/arch/x86_64/serial.c -->
```c

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
```

正常映像已给出真实 write、两次坏范围、tick 与 exit。以下三段各有独立 start/end，run_one 会依次复制其中一段；cli、内核读取和坏 RSP 分别暴露指令权限、页权限与返回检查，不能混成同一种拒绝。

**继续追加三份独立故障映像：**

<!-- append: labs/airios/probe.S -->
```asm
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
static unsigned next_pid = 1; // 每次完整准备后分配新身份，不用槽下标代替 PID。
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
```

copy_user 已能把整个用户范围变成本次内核副本。用户结束或故障还要选择一份继续执行的现场：finish 先在当前用户内核栈上切回长期根，再把结束码放进保存的启动现场 RAX，交回其指针；这里尚未释放栈。

**继续追加结束交接与系统调用分派：**

<!-- append: kernel/process/user.c -->
```c
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
```

分派已经能在 int80 后返回检查过的用户现场，或在 exit/故障后返回启动现场。时钟只对真实 user 来源累加；cleanup 则只有回到启动栈后才调用，按表、数据、内核栈顺序结束所有权，失败准备也走同一出口。

**继续追加用户 tick 与离栈后的清理：**

<!-- append: kernel/process/user.c -->
```c
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
```

回收出口已成立，现在准备一次正常运行。内核栈必须先映射，用户根才能借到它；代码和用户栈页先登记、清零，再映到两个低半区地址。最后在内核栈顶下方预制 176 字节现场，RIP=0x400000、CS/SS=27/35、RFLAGS=0x202；此时仍未进入用户态。

**继续追加准备及失败回滚：**

<!-- append: kernel/process/user.c -->
```c
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
```

prepare 返回后，process 持有根、两份数据页和四页内核栈，process.frame 指向预制现场。run_one 才执行 int129：保留启动现场、装用户根并选 process.frame，由公共返回进入 ring3。用户结束后恢复到这条 int129 之后，再核对结果、cleanup 和前后资源快照。

**继续追加四次顺序运行：**

<!-- append: kernel/process/user.c -->
```c
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

```

正常系统调用或 user IRQ 都还会回用户指令，不能只因入口用了安全内核栈就信任保存的用户 RSP。下面把 CS/SS、低半区 RIP/RSP 和 RFLAGS 统一检查；坏现场转 finish(-22)，有效现场才交给公共 iretq。

**最后追加共同的用户返回检查：**

<!-- append: kernel/process/user.c -->
```c
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

**局部更新 `tests/unit/vm_test.c`。** 最后用destroy而非直接归还根页，核对双重销毁拒绝。

依次找到以下旧窗口，各用对应新文本完整替换；文件其他部分保留。

<!-- replace: tests/unit/vm_test.c -->
<!-- before -->
```c
    for (unsigned i = 0; i < 13; ++i) // 归还所有测试借用。
        assert(held[i] && airi_page_free(&pool, held[i])); // 每个页面必须仍属于原调用者。
    assert(!airi_vm_activate(&space)); // 宿主版本明确不执行特权安装，不能记成硬件验证。
    assert(airi_page_free(&pool, space.root) && pool.free == 16); // 空根不再使用后，测试手动销毁最后一个所有权。
    puts("PASS vm: translation, permissions, shared tables, unmap, exhaustion rollback"); // 全部状态核对通过才打印。
    return 0; // 正常退出宿主观察。
```
<!-- after -->
```c
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
```

**局部更新 `Makefile`。** 加入用户所有者C和受控用户汇编对象，不依赖宿主libc或图形。

依次找到以下旧窗口，各用对应新文本完整替换；文件其他部分保留。

<!-- replace: Makefile -->
<!-- before -->
```make
SOURCES := kernel/main.c kernel/boot/limine.c kernel/boot/info.c \
    kernel/lib/base.c kernel/lib/runtime.c kernel/arch/x86_64/serial.c \
    kernel/console/console.c kernel/arch/x86_64/input.c kernel/arch/x86_64/cpu.c kernel/arch/x86_64/trap.c kernel/mm/pages.c kernel/mm/memory.c kernel/arch/x86_64/paging.c kernel/mm/heap.c kernel/arch/x86_64/timer.c kernel/task/task.c
# 只有 desktop- 前缀目标才读取图形支线自己的构建片段。
ifneq ($(filter desktop-%,$(PROFILE)),)
```
<!-- after -->
```make
SOURCES := kernel/main.c kernel/boot/limine.c kernel/boot/info.c \
    kernel/lib/base.c kernel/lib/runtime.c kernel/arch/x86_64/serial.c \
    kernel/console/console.c kernel/arch/x86_64/input.c kernel/arch/x86_64/cpu.c kernel/arch/x86_64/trap.c kernel/mm/pages.c kernel/mm/memory.c kernel/arch/x86_64/paging.c kernel/mm/heap.c kernel/arch/x86_64/timer.c kernel/task/task.c kernel/process/user.c
# 只有 desktop- 前缀目标才读取图形支线自己的构建片段。
ifneq ($(filter desktop-%,$(PROFILE)),)
```

继续替换下一处：

<!-- replace: Makefile -->
<!-- before -->
```make
CASE := $(patsubst desktop-%,%,$(PROFILE))
# patsubst把每个.c相对路径改为当前OUT下的.o，汇编入口对象另加入。
OBJECTS := $(patsubst %.c,$(OUT)/%.o,$(SOURCES)) $(OUT)/kernel/arch/x86_64/entry.o $(OUT)/kernel/arch/x86_64/traps.o $(OUT)/kernel/arch/x86_64/switch.o
# 交叉目标为无宿主ELF；C11/O2/调试信息/警告作为错误。freestanding与fno-builtin关闭宿主假设；禁栈保护和PIC/PIE；不使用red zone与浮点寄存器；kernel地址模型；MMD/MP生成头文件依赖。
CFLAGS := --target=x86_64-unknown-none-elf -std=c11 -O2 -g -Wall -Wextra -Werror \
```
<!-- after -->
```make
CASE := $(patsubst desktop-%,%,$(PROFILE))
# patsubst把每个.c相对路径改为当前OUT下的.o，汇编入口对象另加入。
OBJECTS := $(patsubst %.c,$(OUT)/%.o,$(SOURCES)) $(OUT)/kernel/arch/x86_64/entry.o $(OUT)/kernel/arch/x86_64/traps.o $(OUT)/kernel/arch/x86_64/switch.o $(OUT)/labs/airios/probe.o
# 交叉目标为无宿主ELF；C11/O2/调试信息/警告作为错误。freestanding与fno-builtin关闭宿主假设；禁栈保护和PIC/PIE；不使用red zone与浮点寄存器；kernel地址模型；MMD/MP生成头文件依赖。
CFLAGS := --target=x86_64-unknown-none-elf -std=c11 -O2 -g -Wall -Wextra -Werror \
```

**局部更新 `kernel/main.c`。** 内核任务演示回收后才顺序运行user，全部恢复后继续console。

依次找到以下旧窗口，各用对应新文本完整替换；文件其他部分保留。

<!-- replace: kernel/main.c -->
<!-- before -->
```c
#include <airi/task.h> // 两个独立栈内核任务主动交接并回收。
#include <airi/timer.h> // 内存建立后开始真实设备中断。
```
<!-- after -->
```c
#include <airi/user.h> // 验证ring3系统调用和故障隔离。
#include <airi/task.h> // 两个独立栈内核任务主动交接并回收。
#include <airi/timer.h> // 内存建立后开始真实设备中断。
```

继续替换下一处：

<!-- replace: kernel/main.c -->
<!-- before -->
```c
    airi_timer_demo(); // 从PIT/PIC收到真实IRQ0，hlt后恢复到正常上下文。
    airi_task_demo(); // 当前只在正常内核上下文主动切换，IRQ不抢占内核任务。
    airi_console_init(&console, info);  // 初始化内核静态控制台，借用整个启动期间有效的 info。
    console.read_memory = airi_memory_snapshot; // 文本查询只读取数值，不持有分配器内部对象。
```
<!-- after -->
```c
    airi_timer_demo(); // 从PIT/PIC收到真实IRQ0，hlt后恢复到正常上下文。
    airi_task_demo(); // 当前只在正常内核上下文主动切换，IRQ不抢占内核任务。
    airi_user_demo(); // 正常与非法用户程序结束后仍回到启动栈和串口。
    airi_console_init(&console, info);  // 初始化内核静态控制台，借用整个启动期间有效的 info。
    console.read_memory = airi_memory_snapshot; // 文本查询只读取数值，不持有分配器内部对象。
```

**局部更新 `scripts/boot.py`。** 正常READY还必须包含完整USER_DEMO_OK，不把只进ring3当全部完成。

依次找到以下旧窗口，各用对应新文本完整替换；文件其他部分保留。

<!-- replace: scripts/boot.py -->
<!-- before -->
```python
                        assert '[AIRI_P1_READY]' not in text  # 错误路径不得出现成功标记。
                    else:  # 按当前profile的另一条完整验证路径处理。
                        assert '[TASK_DEMO_OK]' in text # 两个独立栈必须交错并全部回收。
                        assert '[TIMER_DEMO_OK]' in text # 时钟必须在真实IRQ唤醒后完成。
```
<!-- after -->
```python
                        assert '[AIRI_P1_READY]' not in text  # 错误路径不得出现成功标记。
                    else:  # 按当前profile的另一条完整验证路径处理。
                        assert '[USER_DEMO_OK]' in text # 用户输出、指针拒绝和用户故障后内核必须继续。
                        assert '[TASK_DEMO_OK]' in text # 两个独立栈必须交错并全部回收。
                        assert '[TIMER_DEMO_OK]' in text # 时钟必须在真实IRQ唤醒后完成。
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
