# K49 · 单独持有用户地址空间，并让 exec 只替换它

从 K48 继续。用户malloc已经依赖进程中的页面、BSS块链和TLS错误槽；同一个进程成功exec后，这些用户内容应重新建立，而PID、父关系、仍在执行的内核栈和普通FD继续存在。本节沿heap的孩子从旧arena替换到heap-reset的过程，将这两组资源在内核中明确分开。

当前仍是一进程一个执行者。本节不增加线程调用或共享地址空间，而是先收拢用户内存所有权，并移除exec对整份旧进程控制块的复制。后续线程还需要决定进程级资源与每条执行链的生命周期；这里完成的是其中的内存边界。

<a id="memory-owner"></a>
## 根页表与页记录描述同一份用户内存

旧user_process把space、七个记录页句柄和count与FD、CPU现场及内核栈并排保存。exec为了临时保存旧页，会先复制整个控制块；这也复制了FD、异步owner和正在使用的内核栈描述，清理之前必须逐项撤掉不该释放的别名。用户资源越多，整个控制块副本越难说明谁还能归还哪一项。

新增user_memory，把space、records、count放在一起。space描述页表，records保存数据页的VA、PA和权限，count限定有效记录；三者共同描述一份需要整体销毁的用户地址空间。结构只保存句柄和计数，不内嵌1024页。当前目标编译出的描述为96字节，完整进程控制块为792字节，仍保持内核栈上的候选大小上限。

用户栈和内核栈需要继续分开：用户代码用的64KiB栈属于memory记录；进入int80或中断以后使用的16KiB内核陷入栈由user_process.stack持有。静态TLS数据页也在memory中，但CPU用于寻址的FS基址只是每个执行者的寄存器状态，仍在fs_base。独立FPU保存页不映射给用户，继续单独持有。

先在现有 user.c 的 user_page 与分页记录常量之后接入 user_memory，将原进程布局换为内嵌这个成员的布局。原空间、记录页与计数进入 memory，栈、FD、CPU状态与身份继续各自持有；随后再接各个页面消费者。

本章每组第一块只用于定位旧文本，敲入第二块替换，不把两块同时加入文件。按正文顺序完成全部连接后再构建。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
struct user_page { uint64_t address, physical; unsigned flags; }; // fork需要把同一VA映射到另一个物理页。
#define PAGE_RECORDS (4096u / sizeof(struct user_page)) // 一物理页放170项，余下16字节不用。
#define RECORD_CHUNKS ((AIRI_USER_PAGES + PAGE_RECORDS - 1) / PAGE_RECORDS) // 1024项最多借七个记录页。
struct user_process { // 所有权和调度状态由内核持有，用户只能看到PID。
    bool async_owned; // 本控制块是否持有当前PID的异步owner，局部旧exec副本必须清除此所有权。
    struct airi_fd_table files; // exec转移表项，fork增加同一打开对象的引用。
    struct airi_address_space space; // 根和低半区表自有，高半区借用。
    uint64_t fs_base; // 用户FS基址只保存数值，映射由原用户页所有权管理。
    uint64_t fpu_physical; // 独立状态页不塞进exec局部控制块，也不算用户数据页。
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
```
<!-- after -->
```c
struct user_page { uint64_t address, physical; unsigned flags; }; // fork需要把同一VA映射到另一个物理页。
#define PAGE_RECORDS (4096u / sizeof(struct user_page)) // 一物理页放170项，余下16字节不用。
#define RECORD_CHUNKS ((AIRI_USER_PAGES + PAGE_RECORDS - 1) / PAGE_RECORDS) // 1024项最多借七个记录页。
struct user_memory { // 用户地址空间及其页面所有权，不含CPU现场、内核栈、FD或异步owner。
    struct airi_address_space space; // 自有根和低半区页表，高半区继续借用。
    uint64_t records[RECORD_CHUNKS]; // 元数据页记录本地址空间拥有的数据页，不映射给用户。
    unsigned count; // 包括用户代码、动态页、TLS与用户栈，不包括内核陷入栈或FPU页。
};
_Static_assert(sizeof(struct user_memory) <= 128, "user memory owner must stay bounded"); // 按值转移只复制小描述，不复制1024页内容。
struct user_process { // 当前仍是一进程一执行者；本次分出内存所有者，不宣称已共享线程。
    struct user_memory memory; // 唯一持有本进程当前地址空间的描述。
    bool async_owned; // 本进程是否拥有当前PID的异步owner，exec只reset轮次而不复制该所有权。
    struct airi_fd_table files; // exec原地保留表项，fork增加同一打开对象的引用。
    uint64_t fs_base; // 用户FS基址只保存数值，映射由原用户页所有权管理。
    uint64_t fpu_physical; // 独立状态页不塞进exec局部控制块，也不算用户数据页。
    struct airi_kernel_region stack; // 不能在仍使用它的C调用中释放。
    unsigned pid, parent, user_ticks; // 身份、父PID和本进程实际IRQ数与页数量分开。
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
_Static_assert(sizeof(struct user_process) <= 1024, "exec control block must stay small"); // exec候选仍在内核栈上，大小不能随用户容量膨胀。
static struct user_process processes[PROCESS_COUNT]; // 槽可以复用，PID单调增长，不等于槽下标。
static struct user_process *current; // 只在完整陷入边界改变当前身份。
static unsigned next_pid = 1, root_pid, timer_switches, wait_blocks; // 每次运行会话的根身份和实际行为计数。
```

<a id="memory-callers"></a>
## 需要页面的函数只接收页面所有者

owned_at、trim_records和owned_page只需要地址空间和数据页记录，参数改为user_memory指针。它们不能再通过这个参数访问FD、PID或陷入栈。调用方若仍拿着进程指针p，就明确传入 `&p->memory`；`&`取得这个内嵌成员的地址，未分配第二份资源，也未复制页面。

其余只读检查继续从进程找到当前memory：user_access和copy_user查询 `p->memory.space`，initial_frame在同一根中找用户栈，select_user激活目标memory.space。ELF装载和原始页申请把新页交到该memory，动态release在同一个记录集合中验证并移除。失败回滚只削减本批新增项，顺序仍是先断活动映射再归还物理页。

fork仍逐页深复制：父的memory记录提供原VA/PA，孩子的独立memory建立相同VA的新页，再复制4096字节内容。它没有调用下面的所有权转移函数，不能通过复制96字节描述让两个地址空间持有同一批物理页。用户FD表则保留既有clone语义，增加同一个打开对象的引用。

先把记录访问与裁尾助手收窄到 user_memory 参数，函数内只能取得页表和页归属。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
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
static bool create_fpu(struct user_process *p) { // 构造器只给空控制块建立独立页所有权。
```
<!-- after -->
```c
    uint64_t address = STACK_BASE + slot(p) * 5 * 4096ull + 4096; // 四页数据与一页保护，PID增长不扩散地址。
    return airi_memory_region_create(&p->stack, address, 4); // 完整成功后才借给用户根。
}
static struct user_page *owned_at(const struct user_memory *memory, unsigned index) { // 下标可等于count供新增项写入，但必须已有记录页。
    unsigned chunk = index / PAGE_RECORDS; // 整数除法选出记录页，不要求各物理页连续。
    if (index >= AIRI_USER_PAGES || !memory->records[chunk]) // 所有调用均来自内核已经验证的有效项。
        airi_panic("user page record is missing"); // 内部越界不能返回任意HHDM指针。
    struct user_page *entries = airi_memory_direct(memory->records[chunk]); // 别名只借用，物理所有权仍在records中。
    return &entries[index % PAGE_RECORDS]; // 余数选页内项；单项不会跨物理页。
}
static void trim_records(struct user_memory *memory) { // count已缩小，释放不再容纳任何活记录的尾部元数据页。
    unsigned needed = (memory->count + PAGE_RECORDS - 1) / PAGE_RECORDS; // 零项需要零页，其余向上取整。
    for (unsigned i = needed; i < RECORD_CHUNKS; ++i) { // 固定七槽，只检查完全位于有效项之外的页。
        if (!memory->records[i]) // 尚未分配的槽没有物理所有权。
            continue; // 不把0哨兵交给页池。
        if (!airi_memory_return_page(memory->records[i])) // 没有独立VA映射，HHDM随内核始终存在。
            airi_panic("cannot reclaim user page records"); // 归属错误不能静默复用页面。
        memory->records[i] = 0; // 归还后立即撤掉唯一所有权记录。
    }
}
static bool create_fpu(struct user_process *p) { // 构造器只给空控制块建立独立页所有权。
```

用户范围检查和实际复制仍从进程开始，只把查询根改为它的 memory.space。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
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
```
<!-- after -->
```c
        return false; // 零长度也保留本项目地址策略。
    for (size_t done = 0; done < bytes;) { // 跨越的每一页都必须满足权限。
        unsigned flags; // 当前叶子权限。
        if (!airi_vm_lookup(&p->memory.space, address + done, NULL, &flags) || !(flags & AIRI_VM_USER) || (write && !(flags & AIRI_VM_WRITE))) // 输出指针还要求用户可写。
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
        airi_vm_lookup(&p->memory.space, address + done, &physical, NULL); // 完整检查后页表在本次内核操作中保持不变。
        size_t part = 4096 - ((address + done) & 4095); // 当前页边界。
        if (part > bytes - done) // 不超出剩余内容。
            part = bytes - done; // 缩到最后一段。
```

owned_page 同样只接内存所有者，记录位置、数据页与映射的原回滚次序保持。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
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
```
<!-- after -->
```c
    p->frame->rsp = stack_pointer; // 使用刚准备的用户布局。
    p->frame->rflags = 0x202; // 用户IF开启，IOPL为零。
}
static bool owned_page(struct user_memory *memory, uint64_t address, unsigned flags) { // 一次只建立一页，失败页面仍可由统一cleanup找到。
    if (memory->count == AIRI_USER_PAGES) // 包括ELF、动态页和用户栈，不包括记录页。
        return false; // 不越过固定记录容量。
    unsigned chunk = memory->count / PAGE_RECORDS; // 下一项应写入哪一页元数据。
    if (!memory->records[chunk]) { // 仅跨过170项边界时才增加一个记录页。
        memory->records[chunk] = airi_memory_take_page(); // 记录页与用户数据页分别拥有、分别归还。
        if (!memory->records[chunk]) // 元数据耗尽不能继续分配没有记录位置的数据页。
            return false; // 旧有效项和旧映射完全保持。
        airi_memset(airi_memory_direct(memory->records[chunk]), 0, 4096); // 清除前一个所有者的地址记录。
    }
    uint64_t physical = airi_memory_take_page(); // 取得真正归本上下文持有的物理页。
    if (!physical) { // 可能刚借出记录页，却还未增加有效数据项。
        trim_records(memory); // 收回本次没有承载活项的新记录页。
        return false; // 调用方再回滚本批此前成功的数据页。
    }
    airi_memset(airi_memory_direct(physical), 0, 4096); // 文件尾部与BSS默认都从零开始。
    if (!airi_vm_map(&memory->space, address, physical, flags | AIRI_VM_USER)) { // 单页映射失败还没有发布到所有权数组。
        airi_memory_return_page(physical); // 明确归还本次未接入数据页。
        trim_records(memory); // 映射失败也不能遗留空元数据页。
        return false; // 调用者回滚此前成功的部分。
    }
    *owned_at(memory, memory->count) = (struct user_page){address, physical, flags | AIRI_VM_USER}; // 保存VA/PA/权限供fork复制与精确回收。
    ++memory->count; // 记录完整后才发布有效项；指针不跨记录页释放保留。
    return true; // 每个已登记数据页都有对应映射。
}
static int prepare_elf(struct user_process *p, const uint8_t *image, size_t bytes, const char *name, bool make_stack) { // 空目标只拥有新用户资源；exec不另建正在使用的内核栈。
```

<a id="memory-destroy"></a>
## 只销毁用户内存，不借机关闭 FD 或内核栈

将旧cleanup中页表、数据页与记录页的归还提取为destroy_memory。目标根必须不活动：airi_vm_destroy先归还目标自有页表，不销毁借用高半区；随后沿仍有效的records归还数据页，count到零以后再释放保存记录的元数据页。数据页列表可在页表销毁后继续使用，因为记录页经长期HHDM访问，不依赖目标用户根。

最后将整个描述清零，撤销下一次归还资格。这个函数不关闭FD、不关闭异步owner，也不释放内核陷入栈或FPU状态页。整进程cleanup仍依次处理这些独立资源；exec清旧用户内存时只调用更窄的destroy_memory。

exec 需要让 current.memory 改持新映像，但切根以前，旧页还必须有人负责保留和归还。take_memory 先把旧根与记录页句柄交给局部 old_memory，再清空来源描述。清掉的是管理记录本身，旧页表和数据页仍在；处理器的 CR3 也仍指向旧根，不能在这里调用 destroy_memory。

C结构赋值本来只复制数值，不会让来源自动失去归还资格。take_memory 显式清来源，调用方必须接住返回值，才能让同一批页只留一份清理责任。随后对 next.memory 再做一次，把新内存交入 current；这两次调用都不复制页面、不解除映射，也不执行切根。

在原 cleanup 的位置加入 destroy_memory 与 take_memory，并让整进程 cleanup 调用较窄的销毁函数。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
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
    if (p->fpu_physical && !airi_memory_return_page(p->fpu_physical)) // CPU寄存器不保留状态页指针，切离后可归还。
        airi_panic("cannot reclaim user FPU state"); // 不能漏掉独立于用户数据的内核页面。
    if (p->stack.count && !airi_memory_region_free(&p->stack)) // 调用者已经离开目标内核栈，才可销毁它。
        airi_panic("cannot reclaim user kernel stack"); // 必须已经离开目标内核栈，不能在退出者仍使用它时提前回收。
    airi_memset(p, 0, sizeof(*p)); // 不留下可重复释放的根或句柄。
}
static void initial_frame(struct user_process *p, uint64_t entry, const char *name) { // 输入页已完整映射并清零，最后才发布首次返回现场。
    uint64_t stack_pointer = USER_STACK + 4096 - 16; // 旧受控汇编映像不读取argc，保持原布局。
```
<!-- after -->
```c
        airi_log("[WAIT_WAKE] pid=%u result=%ld", p->pid, result); // 普通内核服务栈输出，不在IRQ里打印。
    }
}
static void destroy_memory(struct user_memory *memory) { // 目标根必须不活动，调用者已保存完需要的用户数值。
    if (memory->space.root && !airi_vm_destroy(&memory->space)) // 先去掉自有页表对数据页的映射，借用高半区不归还。
        airi_panic("cannot destroy inactive user tables"); // 不能继续释放仍被活动根引用的数据页。
    while (memory->count) // 页表已不再引用这些自有数据页。
        if (!airi_memory_return_page(owned_at(memory, --memory->count)->physical)) // 先退到最后一个真实有效项。
            airi_panic("cannot return owned user page"); // 内部归属错误不能静默复用。
    trim_records(memory); // 最后一个数据项归还后，元数据页不再有消费者。
    airi_memset(memory, 0, sizeof(*memory)); // 销毁结束不留下可重复归还的根或记录页句柄。
}
static struct user_memory take_memory(struct user_memory *source) { // 按值转移所有权，不复制用户页面也不改变CR3。
    struct user_memory value = *source; // 临时描述保存同一组根和记录页句柄。
    airi_memset(source, 0, sizeof(*source)); // 返回前撤销来源的归还资格，调用者必须接住返回值。
    return value; // 只有接收者继续拥有这些资源；转移本身不分配或失败。
}
static void cleanup(struct user_process *p) { // 目标根必须不活动，而且当前不能使用目标内核栈。
    if (p->async_owned) { // 失败构造或最终收拢只关闭真正拥有的owner。
        airi_async_close(p->pid); // ACTIVE副本归内核，取消不释放其后端目标。
        p->async_owned = false; // 重复清理不能再关同一轮次。
    }
    airi_fd_close_all(&p->files, false); // 整进程清理才关闭全部表项，exec不调用此路径清旧内存。
    destroy_memory(&p->memory); // 只销毁用户映射和数据，不能触及本进程的执行栈。
    if (p->fpu_physical && !airi_memory_return_page(p->fpu_physical)) // CPU寄存器不保留状态页指针，切离后可归还。
        airi_panic("cannot reclaim user FPU state"); // FPU页独立于用户页所有权。
    if (p->stack.count && !airi_memory_region_free(&p->stack)) // 调用者已经离开目标内核栈，才可销毁它。
        airi_panic("cannot reclaim user kernel stack"); // 必须已经离开目标内核栈，不能在退出者仍使用它时提前回收。
    airi_memset(p, 0, sizeof(*p)); // 不留下可重复释放的身份或其他句柄。
}
static void initial_frame(struct user_process *p, uint64_t entry, const char *name) { // 输入页已完整映射并清零，最后才发布首次返回现场。
    uint64_t stack_pointer = USER_STACK + 4096 - 16; // 旧受控汇编映像不读取argc，保持原布局。
```

<a id="memory-exec"></a>
## 新映像准备成功以后，才从当前进程取走旧内存

exec前半段保持原来的失败语义：先复制64字节以内的程序名，找目录并校验ELF，再在局部next中准备完整新页、TLS与默认FPU状态。next不另建内核陷入栈，也不取得当前进程的FD或异步owner。任一步失败只cleanup(next)，旧用户可以从原系统调用点继续。异步epoch的reset也放在资源转移之前，reset失败仍只回滚候选。

准备完成以后，先把当前memory转给局部old_memory；此刻处理器CR3仍指向旧根，旧根已经由局部对象接住，不能在这里销毁。再把next.memory转入current.memory，同时清空next.memory。这样当前进程槽持有新内存，局部描述持有旧内存，两批实际页面仍然都有效。

随后将新FPU页句柄交给current并清空来源，旧FPU页句柄用一个数值暂存。新FS基址来自next静态TLS布局；initial是寄存器值副本，复制到原陷入现场f以后，current.frame继续指向原内核栈上的真实现场。不能令frame指向局部next.initial，否则C函数返回后会留下过期栈地址。

PID、父PID、user_ticks、内核栈、FD表和async_owned完全不搬动。旧wait条件、旧用户结果地址和I/O等待字段显式清零，避免把旧映像中的指针数值留给新映像。普通FD原地保留，只在成功提交后关闭CLOEXEC；init的唯一UART输入授权也继续在此提交点处理。

最后激活current.memory.space，新根借用同一高半区，因此当前内核C调用仍沿原陷入栈继续。只有切根以后，才destroy_memory(old_memory)并归还旧FPU页。若先销毁旧根，处理器仍可能依赖其中的当前页表；若使用整进程cleanup清旧资源，则又会把FD和仍在使用的内核栈带入归还路径。

下面直接替换 exec_current 准备成功后的提交区，旧窗口从整控制块副本开始，直到提交日志结束。前面的名称复制、校验、失败回滚和异步reset保持；两次 take 完成以后，仍要继续替换 CPU 状态并真正激活新根，才能销毁旧内存。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
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
void airi_user_enter(struct airi_trap_frame *f) { // 所有CS3来源在分派设备、timer或syscall之前共同保存。
```
<!-- after -->
```c
        cleanup(&next); // next尚未拥有当前PID的owner。
        return async_error; // 原用户调用可继续处理失败。
    }
    unsigned old_pages = current->memory.count; // 清理前保留替换规模，不复制整进程控制块。
    struct user_memory old_memory = take_memory(&current->memory); // 旧根此刻仍活动，其所有权暂由局部描述接住。
    current->memory = take_memory(&next.memory); // 新根所有权进入原进程槽，候选来源立即为空。
    uint64_t old_fpu = current->fpu_physical; // 旧用户CPU状态页不属于用户地址空间。
    current->fpu_physical = next.fpu_physical; // 替换为准备好的默认x87/SSE状态。
    next.fpu_physical = 0; // 候选从此不能再归还这页。
    current->fs_base = next.fs_base; // 新映像的静态TLS指针，或无TLS时的0。
    current->initial = next.initial; // 保存新入口数值，不保存指向候选栈局部对象的指针。
    current->frame = f; // 原内核陷入栈及TSS继续属于同一执行者。
    *f = current->initial; // 全部可失败准备完成之后才覆盖旧RIP/RSP和寄存器。
    current->fault_error = 0; // 新映像没有旧用户故障报告。
    current->exit_code = 0; // 运行中的新映像还没有退出结果。
    current->wait_target = 0; // 原先完成的wait条件不能成为新映像状态。
    current->wait_status = 0; // 撤销旧用户输出地址的数值借用。
    current->io = IO_NONE; // exec发生在运行系统调用中，没有需要继续的旧阻塞操作。
    current->io_fd = current->io_events = 0; // FD表原地保留，但旧等待条件清零。
    current->io_address = current->io_bytes = current->io_deadline = 0; // 不把旧空间中的请求指针带到新映像。
    airi_fd_close_all(&current->files, true); // 进程FD表原地保留，成功提交才关闭CLOEXEC。
    if (input_pid == current->pid) // 若前台进程自行exec，旧输入权限不能漏到其他映像。
        input_pid = 0; // 成功提交时才撤销，失败exec仍保留旧权限。
    if (acquire_input) { // 本次新映像确实是init管理的shell。
        airi_fd_close(&current->files, 0); // 无表项时-9可忽略；已有普通FD0也按此启动策略关闭。
        if (!airi_fd_attach_input(&current->files)) // 新FD0必须为空且输入引用成功接入。
            airi_panic("cannot attach supervised shell input"); // 事务提交内部不变量错误。
        input_pid = current->pid; // 文件引用完成后才发布唯一消费身份。
    }
    current->state = USER_RUNNING; // PID、父关系、已运行tick、内核栈和异步owner均原地保持。
    if (!airi_vm_activate(&current->memory.space)) // 新根已借到当前内核栈及内核映射。
        airi_panic("cannot activate committed exec image"); // 硬件不变量失败不能回旧用户继续。
    destroy_memory(&old_memory); // CR3已切到新根，只归还局部接住的旧用户内存。
    if (!airi_memory_return_page(old_fpu)) // 硬件随后从新状态页恢复，不再借旧页指针。
        airi_panic("cannot reclaim replaced user FPU state"); // 内部归还错误不能伪装成普通exec失败。
    airi_log("[EXEC_COMMIT] pid=%u program=%s old_pages=%u new_pages=%u", current->pid, name, old_pages, current->memory.count); // 名称是内核副本，不引用刚释放用户页。
    return 0; // 调用者直接恢复新f，不把这个0写回旧用户调用。
}
void airi_user_enter(struct airi_trap_frame *f) { // 所有CS3来源在分派设备、timer或syscall之前共同保存。
```

| 时点 | 处理器使用的用户根 | 当前槽的内存描述 | 局部描述 | 仍在执行的内核栈 |
| --- | --- | --- | --- | --- |
| next准备中 | 旧根 | 旧内存 | next持新内存 | 原栈 |
| 两次take完成 | 旧根 | 新内存 | old_memory持旧内存，next为空 | 原栈 |
| 激活新根 | 新根 | 新内存 | old_memory仍持旧内存 | 原栈 |
| 旧内存销毁后 | 新根 | 新内存 | old_memory为空 | 原栈 |

上表中“持有”表示负责归还的描述，不表示处理器当前正在用哪个CR3。所有提交动作发生在当前单核不可抢占内核中，普通IRQ不会在两次转移之间切到别的用户；这不构成未来SMP的同步协议。

<a id="memory-source"></a>
## 更新同一份 user.c 的调用点

内存描述和 exec 提交已经接好，剩下的是让现有消费者找到同一个 memory 成员。下面逐组给出初始现场、两种装载、调度选择、fork和动态页的位置；调用号、用户ELF、页面上限、FD实现与构建规则保持。仍只修改 user.c，K48的其余文件保留，全部完成后再构建运行。

initial_frame 查找新用户栈时，经 memory.space 取得物理页，原argc/argv布局保持。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
        if (bytes > 64) // 当前程序名是固定短字面量，不支持无界参数。
            airi_panic("initial program name exceeds stack budget"); // 不覆盖用户启动布局。
        uint64_t physical; // 当前根未激活，通过软件翻译找到用户栈数据页。
        if (!airi_vm_lookup(&p->space, USER_STACK, &physical, NULL)) // 栈必须已建立。
            airi_panic("initial user stack mapping is missing"); // 不猜测owned数组中的位置。
        uint8_t *alias = airi_memory_direct(physical); // 通过内核HHDM填写尚未运行的用户栈。
        size_t name_offset = 4096 - bytes; // 字符串放在栈页高端。
```
<!-- after -->
```c
        if (bytes > 64) // 当前程序名是固定短字面量，不支持无界参数。
            airi_panic("initial program name exceeds stack budget"); // 不覆盖用户启动布局。
        uint64_t physical; // 当前根未激活，通过软件翻译找到用户栈数据页。
        if (!airi_vm_lookup(&p->memory.space, USER_STACK, &physical, NULL)) // 栈必须已建立。
            airi_panic("initial user stack mapping is missing"); // 不猜测owned数组中的位置。
        uint8_t *alias = airi_memory_direct(physical); // 通过内核HHDM填写尚未运行的用户栈。
        size_t name_offset = 4096 - bytes; // 字符串放在栈页高端。
```

prepare_elf 的新根、段、用户栈与TLS都交给 p->memory，最终页数也从同一描述读取。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    }
    if (make_stack && !create_stack(p)) // 内核陷入栈先完成，再借高半区。
        return -12; // -ENOMEM，区域接口已回滚失败。
    if (!create_fpu(p) || !airi_memory_user_space(&p->space)) // 浮点页与独立低半区都必须完整建立。
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
    if (view.tls.memory_bytes) { // 无TLS程序继续FS=0，有模板才建立独立运行副本。
        for (unsigned i = 0; i < view.tls.pages; ++i) // 当前最多两页，含TP后的16字节TCB。
            if (!owned_page(p, USER_TLS + i * 4096ull, AIRI_VM_WRITE)) // 仍由用户页记录持有，RW/NX且fork逐页复制。
                goto failed; // 任何分配失败回滚整个新映像。
        uint64_t data = USER_TLS + view.tls.data_offset; // 小对齐模板可能在块首留下padding，保证负偏移相对TP正确。
        if (view.tls.file_bytes && !copy_user(p, (void *)(view.image + view.tls.offset), data, (size_t)view.tls.file_bytes, true)) // 按目标页查物理别名，初值可能跨页。
            goto failed; // 只复制filesz，memsz剩余保持owned_page的零填充。
        p->fs_base = USER_TLS + view.tls.pointer_offset; // TP位于TLS数据之后，不指向模板p_vaddr。
        uint64_t control[2] = {p->fs_base, 0}; // FS:0为self，第二字保留给后续自有运行库。
        if (!copy_user(p, control, p->fs_base, sizeof(control), true)) // TCB属于当前新用户根，不能直接写未活动用户VA。
            goto failed; // 未完整建立TP不发布可运行状态。
        airi_log("[ELF_TLS] bytes=%llu initialized=%llu alignment=%llu pages=%u data=0x%llx tp=0x%llx", (unsigned long long)view.tls.memory_bytes, (unsigned long long)view.tls.file_bytes, (unsigned long long)view.tls.alignment, view.tls.pages, (unsigned long long)data, (unsigned long long)p->fs_base); // 模板与运行位置分别可观察。
    }
    initial_frame(p, view.entry, name); // 用户_start接收最小argc/argv协议。
    airi_log("[ELF_LOAD] entry=0x%llx segments=%u data_pages=%u argc=1", (unsigned long long)view.entry, view.count, p->count); // 不含页表与内核栈的用户数据页数量。
    return 0; // 只准备新空间，尚未切CR3或覆盖旧现场。
failed: // 候选根尚未激活，调用者原根与当前内核栈仍有效；候选用户映像尚未开始执行。
    cleanup(p); // 页表、用户数据和内核栈全部回收。
```
<!-- after -->
```c
    }
    if (make_stack && !create_stack(p)) // 内核陷入栈先完成，再借高半区。
        return -12; // -ENOMEM，区域接口已回滚失败。
    if (!create_fpu(p) || !airi_memory_user_space(&p->memory.space)) // 浮点页与独立低半区都必须完整建立。
        goto failed; // 后续失败均收回所有已持有资源。
    for (unsigned i = 0; i < view.count; ++i) { // 每段独立赋予对应权限。
        const struct airi_elf_segment *segment = &view.segments[i]; // 元数据已经过统一校验。
        unsigned flags = ((segment->flags & 2) ? AIRI_VM_WRITE : 0) | ((segment->flags & 1) ? AIRI_VM_EXEC : 0); // ELF PF标志转换成本内核权限枚举。
        uint64_t first = segment->address & ~4095ull; // 覆盖段首所在页。
        uint64_t end = (segment->address + segment->memory_bytes + 4095) & ~4095ull; // 包含完整BSS跨度。
        for (uint64_t address = first; address < end; address += 4096) // 所有页面先清零再接入。
            if (!owned_page(&p->memory, address, flags)) // 物理耗尽或映射失败都终止本次装载。
                goto failed; // 清理根/表后再归还数据页。
        for (uint64_t done = 0; done < segment->file_bytes;) { // 只复制filesz字节，memsz剩余继续保持零。
            uint64_t physical; // lookup结果包含当前页内偏移。
            if (!airi_vm_lookup(&p->memory.space, segment->address + done, &physical, NULL)) // 已映射跨度必须可查询。
                goto failed; // 内部装载状态不一致也不运行。
            uint64_t part = 4096 - ((segment->address + done) & 4095); // 当前页内可复制长度。
            if (part > segment->file_bytes - done) // 不从文件右端外复制字节。
                part = segment->file_bytes - done; // 最后一次缩到实际剩余。
            airi_memcpy(airi_memory_direct(physical), view.image + segment->offset + done, (size_t)part); // 内核控制的物理别名可初始化最终只读/可执行页。
            done += part; // 下一次继续剩余文件内容。
        }
    }
    for (unsigned i = 0; i < AIRI_USER_STACK_PAGES; ++i) // 最高页地址不变，向低地址增加十五页。
        if (!owned_page(&p->memory, USER_STACK - i * 4096ull, AIRI_VM_WRITE)) // 栈RW/NX，最低页之下仍无映射。
            goto failed; // 任意一页失败均回收整份新映像。
    if (view.tls.memory_bytes) { // 无TLS程序继续FS=0，有模板才建立独立运行副本。
        for (unsigned i = 0; i < view.tls.pages; ++i) // 当前最多两页，含TP后的16字节TCB。
            if (!owned_page(&p->memory, USER_TLS + i * 4096ull, AIRI_VM_WRITE)) // 仍由用户页记录持有，RW/NX且fork逐页复制。
                goto failed; // 任何分配失败回滚整个新映像。
        uint64_t data = USER_TLS + view.tls.data_offset; // 小对齐模板可能在块首留下padding，保证负偏移相对TP正确。
        if (view.tls.file_bytes && !copy_user(p, (void *)(view.image + view.tls.offset), data, (size_t)view.tls.file_bytes, true)) // 按目标页查物理别名，初值可能跨页。
            goto failed; // 只复制filesz，memsz剩余保持owned_page的零填充。
        p->fs_base = USER_TLS + view.tls.pointer_offset; // TP位于TLS数据之后，不指向模板p_vaddr。
        uint64_t control[2] = {p->fs_base, 0}; // FS:0为self，第二字保留给后续自有运行库。
        if (!copy_user(p, control, p->fs_base, sizeof(control), true)) // TCB属于当前新用户根，不能直接写未活动用户VA。
            goto failed; // 未完整建立TP不发布可运行状态。
        airi_log("[ELF_TLS] bytes=%llu initialized=%llu alignment=%llu pages=%u data=0x%llx tp=0x%llx", (unsigned long long)view.tls.memory_bytes, (unsigned long long)view.tls.file_bytes, (unsigned long long)view.tls.alignment, view.tls.pages, (unsigned long long)data, (unsigned long long)p->fs_base); // 模板与运行位置分别可观察。
    }
    initial_frame(p, view.entry, name); // 用户_start接收最小argc/argv协议。
    airi_log("[ELF_LOAD] entry=0x%llx segments=%u data_pages=%u argc=1", (unsigned long long)view.entry, view.count, p->memory.count); // 不含页表与内核栈的用户数据页数量。
    return 0; // 只准备新空间，尚未切CR3或覆盖旧现场。
failed: // 候选根尚未激活，调用者原根与当前内核栈仍有效；候选用户映像尚未开始执行。
    cleanup(p); // 页表、用户数据和内核栈全部回收。
```

受控 flat 映像也使用同一个内存成员，仍保留其原一页代码和一页栈。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
static bool prepare_flat(struct user_process *p, const uint8_t *image, size_t bytes) { // 保留受控指令故障回归，不作为ELF加载接口。
    if (!bytes || bytes > 4096 || !create_stack(p)) // 本节旧映像最多一页。
        return false; // 栈创建失败已回滚。
    if (!create_fpu(p) || !airi_memory_user_space(&p->space) || !owned_page(p, USER_CODE, AIRI_VM_EXEC) || !owned_page(p, USER_STACK, AIRI_VM_WRITE)) // 分别拥有根、代码和栈。
        goto failed; // 所有权列表可清理全部已成功部分。
    airi_memcpy(airi_memory_direct(owned_at(p, 0)->physical), image, bytes); // 映像内引用保持旧固定布局。
    initial_frame(p, USER_CODE, NULL); // 受控汇编不读取argc/argv。
    return true; // 尚未设置可运行状态。
failed: // 目标从未被调度，当前根和栈均不属于它。
```
<!-- after -->
```c
static bool prepare_flat(struct user_process *p, const uint8_t *image, size_t bytes) { // 保留受控指令故障回归，不作为ELF加载接口。
    if (!bytes || bytes > 4096 || !create_stack(p)) // 本节旧映像最多一页。
        return false; // 栈创建失败已回滚。
    if (!create_fpu(p) || !airi_memory_user_space(&p->memory.space) || !owned_page(&p->memory, USER_CODE, AIRI_VM_EXEC) || !owned_page(&p->memory, USER_STACK, AIRI_VM_WRITE)) // 分别拥有根、代码和栈。
        goto failed; // 所有权列表可清理全部已成功部分。
    airi_memcpy(airi_memory_direct(owned_at(&p->memory, 0)->physical), image, bytes); // 映像内引用保持旧固定布局。
    initial_frame(p, USER_CODE, NULL); // 受控汇编不读取argc/argv。
    return true; // 尚未设置可运行状态。
failed: // 目标从未被调度，当前根和栈均不属于它。
```

select_user 激活所选进程的 memory.space，TSS和用户frame仍沿旧执行身份选择。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    current = p; // 之后的IRQ和系统调用归属这个身份。
    p->state = USER_RUNNING; // 新用户即将获得CPU。
    airi_trap_set_stack(p->stack.virtual_address + p->stack.count * 4096ull); // 陷入时使用目标自己的内核栈。
    if (!airi_vm_activate(&p->space)) // 所有高半区内核映射在准备时已存在。
        airi_panic("cannot activate selected user root"); // 不返回到缺映射的调用链。
    return airi_user_resume(p->frame); // 用户返回地址与标志必须仍符合边界。
}
```
<!-- after -->
```c
    current = p; // 之后的IRQ和系统调用归属这个身份。
    p->state = USER_RUNNING; // 新用户即将获得CPU。
    airi_trap_set_stack(p->stack.virtual_address + p->stack.count * 4096ull); // 陷入时使用目标自己的内核栈。
    if (!airi_vm_activate(&p->memory.space)) // 所有高半区内核映射在准备时已存在。
        airi_panic("cannot activate selected user root"); // 不返回到缺映射的调用链。
    return airi_user_resume(p->frame); // 用户返回地址与标志必须仍符合边界。
}
```

fork 从父memory读取记录，向孩子独立memory建立新页，日志按孩子的实际页数报告。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    struct user_process *child = unused(); // 未发布的新槽。
    if (!child) // 表满没有可分配身份。
        return -11; // -EAGAIN。
    if (!create_stack(child) || !create_fpu(child) || !airi_memory_user_space(&child->space)) // 先创建独立内核栈和空低半区。
        goto failed; // 父状态从未改变。
    for (unsigned i = 0; i < parent->count; ++i) { // 包括用户栈和此前申请的堆页。
        const struct user_page *page = owned_at(parent, i); // 每页原VA、权限和物理地址。
        if (!owned_page(child, page->address, page->flags)) // 子页在相同VA，但物理所有权独立。
            goto failed; // 回滚全部子资源，不改变父页面。
        airi_memcpy(airi_memory_direct(owned_at(child, child->count - 1)->physical), airi_memory_direct(page->physical), 4096); // 复制完整页内容，父子以后写入互不影响。
    }
    child->fs_base = parent->fs_base; // fork继承同一用户VA，指向复制后的子页面，不共享物理内容。
    airi_memcpy(airi_memory_direct(child->fpu_physical), airi_memory_direct(parent->fpu_physical), AIRI_FPU_BYTES); // 父陷入时已保存，子复制真实状态而非共享状态页。
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
```
<!-- after -->
```c
    struct user_process *child = unused(); // 未发布的新槽。
    if (!child) // 表满没有可分配身份。
        return -11; // -EAGAIN。
    if (!create_stack(child) || !create_fpu(child) || !airi_memory_user_space(&child->memory.space)) // 先创建独立内核栈和空低半区。
        goto failed; // 父状态从未改变。
    for (unsigned i = 0; i < parent->memory.count; ++i) { // 包括用户栈和此前申请的堆页。
        const struct user_page *page = owned_at(&parent->memory, i); // 每页原VA、权限和物理地址。
        if (!owned_page(&child->memory, page->address, page->flags)) // 子页在相同VA，但物理所有权独立。
            goto failed; // 回滚全部子资源，不改变父页面。
        airi_memcpy(airi_memory_direct(owned_at(&child->memory, child->memory.count - 1)->physical), airi_memory_direct(page->physical), 4096); // 复制完整页内容，父子以后写入互不影响。
    }
    child->fs_base = parent->fs_base; // fork继承同一用户VA，指向复制后的子页面，不共享物理内容。
    airi_memcpy(airi_memory_direct(child->fpu_physical), airi_memory_direct(parent->fpu_physical), AIRI_FPU_BYTES); // 父陷入时已保存，子复制真实状态而非共享状态页。
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
    airi_log("[FORK] parent=%u child=%u copied_pages=%u", parent->pid, child->pid, child->memory.count); // 实际复制数量含用户栈与堆。
    return child->pid; // 父分支得到正子PID。
failed: // 还没有向调度器发布孩子，当前仍在父根和父内核栈上。
    cleanup(child); // 删除不活动子根和其独立栈，父继续原系统调用。
```

动态申请、失败回滚、查找与释放都消费 current->memory；整段预检和末项搬移后裁尾的次序不变。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
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
            goto rollback; // 本批映射尚未完整提交，已有用户页保持。
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
```
<!-- after -->
```c
    if (!bytes || bytes > AIRI_USER_REQUEST_PAGES * 4096ull) // 一次最多六十四页，取整加法保持有界。
        return -22; // -EINVAL。
    unsigned count = (unsigned)((bytes + 4095) / 4096); // 请求向上取整为完整页。
    if (count > AIRI_USER_PAGES - current->memory.count) // 先检查整批有效项容量。
        return -12; // -ENOMEM。
    uint64_t address = USER_HEAP; // 从低端找能容纳本次整段的空洞，不依赖旧分配顺序。
    for (;;) { // 每次冲突越过至少一个已拥有页，搜索被1024项预算限制。
        bool conflict = false; // 本候选范围是否撞上现有用户数据页。
        for (unsigned i = 0; i < current->memory.count; ++i) { // owned可能因释放而换序，不要求按VA排序。
            uint64_t used = owned_at(&current->memory, i)->address; // 当前页仍由此进程拥有。
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
    unsigned before = current->memory.count; // 失败只回滚本次新增映射。
    for (unsigned i = 0; i < count; ++i) // 页物理可以不连续，用户VA连续。
        if (!owned_page(&current->memory, address + i * 4096ull, AIRI_VM_WRITE)) // 成功页面已在owned中登记。
            goto rollback; // 本批映射尚未完整提交，已有用户页保持。
    airi_log("[USER_ALLOC] pid=%u address=0x%llx pages=%u zeroed=1", current->pid, (unsigned long long)address, count); // 只记录已完整提交的用户映射。
    return (long)address; // 用户可显式归还完整页，未归还页仍在exit统一清理。
rollback: // 已有用户页面不受本次耗尽影响。
    while (current->memory.count > before) { // 只处理新增的成功项。
        const struct user_page *page = owned_at(&current->memory, --current->memory.count); // 先退到最后一项。
        if (!airi_vm_unmap(&current->memory.space, page->address) || !airi_memory_return_page(page->physical)) // 活动根必须先断映射并失效，再归还。
            airi_panic("cannot roll back user page allocation"); // 内部所有权错误不能被吞掉。
    }
    trim_records(&current->memory); // 本批回滚后，尾部没有活项的记录页也一起归还。
    return -12; // 不返回半段用户范围。
}
static unsigned owned_index(uint64_t address) { // 只在当前单线程进程的不可抢占系统调用中查询。
    for (unsigned i = 0; i < current->memory.count; ++i) // 有效项之外可能有旧字节，不参与查找。
        if (owned_at(&current->memory, i)->address == address) // 精确页起点，不接受页内指针。
            return i; // 下标只借给本次操作，不跨状态修改保存。
    return current->memory.count; // count本身是未找到哨兵，不能作为数组下标解引用。
}
static long release_user(uint64_t address, uint64_t bytes) { // 按页解除动态用户映射，不是C malloc对象free。
    if (!bytes || bytes > AIRI_USER_REQUEST_PAGES * 4096ull || (bytes & 4095) || (address & 4095) || address < USER_HEAP || address >= USER_HEAP_END || bytes > USER_HEAP_END - address) // 短路先限地址再做右端减法。
        return -22; // -EINVAL；不舍入无效长度，不动任何页。
    unsigned count = (unsigned)(bytes / 4096); // 参数已限制为一到六十四个完整页。
    for (unsigned i = 0; i < count; ++i) { // 第一遍先验证整段，避免中途坏页留下部分解除。
        unsigned index = owned_index(address + i * 4096ull); // 当前目标VA属于动态页范围。
        if (index == current->memory.count || owned_at(&current->memory, index)->flags != (AIRI_VM_USER | AIRI_VM_WRITE)) // 既需真实所有权，也只接受RW/NX用户动态页。
            return -22; // 任何一页不存在，整段保持原样。
    }
    for (unsigned i = 0; i < count; ++i) { // 第二遍提交，当前没有其他线程改变本地址空间。
        unsigned index = owned_index(address + i * 4096ull); // 每次重新查，上一轮可能移动了owned末项。
        struct user_page page = *owned_at(&current->memory, index); // 在压缩数组前保留当前VA与PA数值。
        if (!airi_vm_unmap(&current->memory.space, page.address) || !airi_memory_return_page(page.physical)) // 先断开活动根映射并invlpg，才归还物理页。
            airi_panic("cannot release owned dynamic user page"); // 内部不变量失败不能返回假成功。
        *owned_at(&current->memory, index) = *owned_at(&current->memory, --current->memory.count); // 末项可位于另一记录页，复制后才允许回收尾页。
    }
    trim_records(&current->memory); // 全段压缩结束，任何有效项都不再借用空尾页。
    airi_log("[USER_RELEASE] pid=%u address=0x%llx pages=%u", current->pid, (unsigned long long)address, count); // 只记录整段真正归还后的结果。
    return 0; // 后续分配可复用空洞，新物理页按原owned_page逻辑清零。
}
```

<a id="memory-observe"></a>
## 用原有故障和资源回收路径观察拆分

<!-- run -->
```sh
./airios build  # 编译全部内存成员与调用点，生成当前串口镜像。
./airios test  # 覆盖正常装载、坏ELF、用户页耗尽、fork、exec、TLS、FD和异步退出回收。
```

交互仍从内核监控台run sh进入用户shell，依次运行heap、ctls、libc、capacity和async，再exit。heap的孩子持有64页arena后成功替换为heap-reset；ctls检查新旧本地模板；libc核对CLOEXEC和错误槽；capacity的孩子从1024数据页替换为小worker；async检查旧epoch与进行中设备请求的退役。每条命令都必须按实际退出码结束，而不是依赖QEMU仍存活。

```sh
./airios run  # 启动固定q35镜像，下面输入交给真实UART。
```

```text
run sh
heap
ctls
libc
capacity
async
exit
```

`[EXEC_COMMIT]`继续输出同一PID、程序名、旧数据页数与新数据页数，不新增虚构线程或共享内存标记。坏ELF必须出现拒绝后继续原用户路径；成功exec不返回旧调用。完整测试还会运行normal十二代init、service及无帧缓冲服务，并在会话结束核对页、堆活对象、FD与异步池归还。

本节只把原有可观察行为保持在更窄的资源操作上，不新增用户功能。Linux的exec对照继续沿L03观察同PID替换和FD继承。未来线程不能直接复制这个user_memory描述作为共享：需要让多个执行者引用同一份进程资源，最后一个拥有者退出时才归还，并为每个执行者独立安排FPU、FS、陷入栈、用户栈和TLS运行块。当前1024页预算依然按进程计，尚无线程API或futex。
