# K50 · 为一条用户执行链单独保存现场与等待

从 K49 继续。用户地址空间已有独立所有者，CPU每次切换时还需要一组属于当前执行链的状态：从哪条用户指令继续、FS指向哪里、x87/SSE寄存器是什么，以及进入内核后使用哪条栈。这里把这些内容收进user_execution，页面和FD继续由user_process持有。

本节保持现有单执行者行为。当前user_process仍内嵌一个execution，调度器仍在八个进程槽之间轮转；没有共享线程、TID、join或新的系统调用。这个中间停点让已有的抢占、阻塞与exec路径分别接到明确对象，之后才增加同一地址空间里的第二条执行链。

<a id="execution-owner"></a>
## 用户页保存数据，执行链保存继续点

用户执行int80后，处理器先在当前内核陷入栈上保存RIP、RSP及权限返回信息，公共汇编再保存通用寄存器，C分派入口随后保存FS与FPU。frame记住这份真实栈内现场的位置；它本身不执行恢复。调度选中这条执行链后，返回汇编改用该现场，取回通用寄存器，再由iretq继续原来的用户指令。

进程被切走时，用户页仍由memory持有。要从刚才的指令正确继续，现场、FS、FPU和下一次陷入使用的内核栈必须一起属于所选执行者；这些状态只换一部分，内存中的用户数据仍在也无法保持计算。

等待也是执行链的状态。一次read暂时没有数据，需要保留这次调用的输出地址、长度和FD条件；read恢复后继续的是这条调用链，而不是把整个地址空间重新运行。waitpid保存孩子PID条件和用户结果地址，sleep保存截止tick，这些都不拥有用户页面。它们的有效性依赖进程内存仍存在，完成或exec后应清掉过期借用。

user_execution包含内核陷入栈、FPU页句柄、FS、真实frame与首次initial、用户tick和故障数值、调度状态与等待条件。user_process保留memory、FD表、异步owner、PID、父PID和进程退出码。父进程wait取得的退出码属于进程生命周期；本节没有把它变成线程返回值。

在user_memory之后加入下面的执行结构，并按新定义更新user_process。当前目标中执行描述424字节，完整进程控制块800字节；原1024字节控制块上限继续保护exec的栈上候选。user_memory仍为96字节，用户数据页上限不变。

替换原 user_process 及紧随的大小断言，在同一位置放入 execution 和新的进程结构。

本章每组第一块用于定位旧文本，只敲第二块替换。按正文顺序接齐全部调用点以后再构建。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
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
```
<!-- after -->
```c
struct user_execution { // 一条用户执行链的CPU现场、内核陷入栈和等待条件。
    uint64_t fs_base; // 当前执行者的用户FS基址，不拥有对应用户数据页。
    uint64_t fpu_physical; // 独立内核状态页，保存本执行者的x87/SSE寄存器。
    struct airi_kernel_region stack; // 真正进入内核以后使用的栈，离开它才能销毁。
    unsigned user_ticks; // 本执行者实际收到的用户时钟事件。
    uint64_t fault_error; // 最近一次结束本执行者的用户故障错误位。
    enum user_state state; // 当前仍每进程一执行者，调度资格由这里决定。
    int64_t wait_target; // 等待的孩子PID条件，不是地址空间所有权。
    uint64_t wait_status; // wait结果的用户VA，只在当前等待期间借用。
    enum io_operation io; // 当前阻塞操作类型，不保存内核局部buffer。
    unsigned io_fd, io_events; // 当前I/O等待条件，FD本体归进程表。
    uint64_t io_address, io_bytes, io_deadline; // 当前操作借用的用户跨度和截止tick。
    struct airi_trap_frame initial; // 第一次进入用户时的寄存器值，不借临时栈。
    struct airi_trap_frame *frame; // 实际陷入以后指向本执行者内核栈上的现场。
};
struct user_process { // 当前仍为一个进程内嵌一条执行链，尚无线程共享与多执行者调度。
    struct user_memory memory; // 进程持有用户页表、数据页和元数据页。
    bool async_owned; // 当前PID的异步owner只由进程打开、reset和关闭。
    struct airi_fd_table files; // FD引用属于进程，exec保留、fork复制表项引用。
    unsigned pid, parent; // 进程身份与父关系不随CPU执行现场一起替换。
    int exit_code; // 父进程wait取得的进程退出结果。
    struct user_execution execution; // 暂时唯一的执行者，后续线程需要明确共享进程资源。
};
_Static_assert(sizeof(struct user_process) <= 1024, "exec control block must stay small"); // exec候选仍在内核栈上，大小不能随用户容量膨胀。
```

<a id="execution-stack"></a>
## 创建内核栈时给出执行者和槽号

create_stack不再接收整个进程，只接收目标execution和一个已知合法的槽号index。地址仍是STACK_BASE加上index乘五页，再跳过一页保护页；四个数据页形成16KiB陷入栈，布局没有变化。现有调用在进程表中取slot(p)，明确传入 `&p->execution`。

exec的局部next不属于进程数组，不能对它做进程数组内的指针相减。prepare_elf仍由make_stack决定是否创建陷入栈，`make_stack && ...` 的短路求值确保exec传false时，右侧create_stack以及它的slot(p)实参都不会执行。新用户映像继续借当前那条内核栈，只有真正新建进程时才从进程槽计算栈位置。

create_fpu同样只接执行者，将新状态页句柄保存在execution；它不建立或改变用户页表。place_frame也只需要execution，把initial复制到其内核栈顶附近，再把frame设成真实栈内地址。三个函数都无需访问PID、FD或用户页面记录。

用执行者与已知槽号替换原 create_stack。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
static bool create_stack(struct user_process *p) { // 每槽前面留一页保护页，所有栈共享已存在高半区分支。
    uint64_t address = STACK_BASE + slot(p) * 5 * 4096ull + 4096; // 四页数据与一页保护，PID增长不扩散地址。
    return airi_memory_region_create(&p->stack, address, 4); // 完整成功后才借给用户根。
}
```
<!-- after -->
```c
static bool create_stack(struct user_execution *execution, unsigned index) { // 每槽前面留一页保护页，所有栈共享已存在高半区分支。
    uint64_t address = STACK_BASE + index * 5 * 4096ull + 4096; // 四页数据与一页保护，PID增长不扩散地址。
    return airi_memory_region_create(&execution->stack, address, 4); // 完整成功后才借给用户根。
}
```

create_fpu 的页句柄改由 execution 接住，模板复制逻辑保持。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
static bool create_fpu(struct user_process *p) { // 构造器只给空控制块建立独立页所有权。
    p->fpu_physical = airi_memory_take_page(); // 一页足够容纳对齐512字节状态，与用户记录页分别归还。
    if (!p->fpu_physical) // 没有内存就不准备可运行身份。
        return false; // cleanup可回收此前的栈等资源。
    void *state = airi_memory_direct(p->fpu_physical); // 内核HHDM别名，不给用户USER映射。
    airi_memset(state, 0, 4096); // 剩余页字节也不带旧内容。
    airi_fpu_new(state); // 只复制已验证模板，不改变当前用户硬件状态。
    return true; // 页句柄此后由进程清理者持有。
}
```
<!-- after -->
```c
static bool create_fpu(struct user_execution *execution) { // 构造器只给空控制块建立独立页所有权。
    execution->fpu_physical = airi_memory_take_page(); // 一页足够容纳对齐512字节状态，与用户记录页分别归还。
    if (!execution->fpu_physical) // 没有内存就不准备可运行身份。
        return false; // cleanup可回收此前的栈等资源。
    void *state = airi_memory_direct(execution->fpu_physical); // 内核HHDM别名，不给用户USER映射。
    airi_memset(state, 0, 4096); // 剩余页字节也不带旧内容。
    airi_fpu_new(state); // 只复制已验证模板，不改变当前用户硬件状态。
    return true; // 页句柄此后由进程清理者持有。
}
```

place_frame 只接 execution，把初值放进它拥有的真实陷入栈。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
static void place_frame(struct user_process *p) { // 初始寄存器值完成后复制到自己拥有的内核栈。
    uint64_t top = p->stack.virtual_address + p->stack.count * 4096ull; // TSS以后从同一个栈顶压入真实现场。
    p->frame = (void *)(uintptr_t)(top - sizeof(*p->frame)); // 与公共入口的176字节布局一致。
    *p->frame = p->initial; // 不借用临时C结构或其他进程的内核栈。
}
```
<!-- after -->
```c
static void place_frame(struct user_execution *execution) { // 初始寄存器值完成后复制到自己拥有的内核栈。
    uint64_t top = execution->stack.virtual_address + execution->stack.count * 4096ull; // TSS以后从同一个栈顶压入真实现场。
    execution->frame = (void *)(uintptr_t)(top - sizeof(*execution->frame)); // 与公共入口的176字节布局一致。
    *execution->frame = execution->initial; // 不借用临时C结构或其他进程的内核栈。
}
```

<a id="execution-destroy"></a>
## 离开陷入栈以后才能销毁执行者

整进程cleanup仍先关闭进程级FD与异步owner，再销毁用户内存。原先单独归还FPU和陷入栈的部分收进destroy_execution：先归还状态页，再解除内核栈映射，最后清空全部现场和等待条件。调用者必须已经离开目标内核栈，并保证这条执行链不会再被调度；函数没有能力判断当前C调用正在使用哪条RSP。

因此用户exit仍先标记结束并切走，collect_pending在另一条内核栈上回收，或者完整会话结束后在启动栈上清理。只把函数改名而继续在当前栈上free仍会破坏返回链，新的结构不会自动解决这个顺序问题。失败构造尚未运行，可以直接销毁其已经成功取得的部分。

在原 cleanup 位置加入 destroy_execution，并让进程清理分别销毁内存和执行者。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
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
```
<!-- after -->
```c
static void destroy_execution(struct user_execution *execution) { // 调用者已离开这条内核栈，执行者也不能再恢复。
    if (execution->fpu_physical && !airi_memory_return_page(execution->fpu_physical)) // 用户地址空间不持有这份内核状态页。
        airi_panic("cannot reclaim user FPU state"); // 内部归属错误不能静默遗漏。
    if (execution->stack.count && !airi_memory_region_free(&execution->stack)) // 最后才能解除本执行者的内核陷入栈。
        airi_panic("cannot reclaim user kernel stack"); // 当前实际栈仍在使用时不得调用。
    airi_memset(execution, 0, sizeof(*execution)); // 清现场、等待借用和已归还句柄，不能再被选中。
}
static void cleanup(struct user_process *p) { // 目标根必须不活动，而且当前不能使用目标内核栈。
    if (p->async_owned) { // 失败构造或最终收拢只关闭真正拥有的owner。
        airi_async_close(p->pid); // ACTIVE副本归内核，取消不释放其后端目标。
        p->async_owned = false; // 重复清理不能再关同一轮次。
    }
    airi_fd_close_all(&p->files, false); // 整进程清理才关闭全部表项，exec不调用此路径清旧内存。
    destroy_memory(&p->memory); // 只销毁用户映射和数据，不能触及本进程的执行栈。
    destroy_execution(&p->execution); // CPU状态页和陷入栈分别归执行者，最后统一撤掉恢复资格。
    airi_memset(p, 0, sizeof(*p)); // 不留下可重复释放的身份或其他句柄。
}
```

<a id="execution-dispatch"></a>
## 调度时同时选择地址空间和执行现场

current当前仍指向一个user_process；select_user从p.memory激活页表，从p.execution设置TSS的内核栈并取得frame。进入用户的仍是同一套寄存器恢复代码。内存与执行者分开以后，不能只切页表而忘记FS/FPU或内核栈，也不能只换frame却沿另一进程的地址空间执行。

schedule把旧execution的RUNNING转成RUNNABLE，已WAITING或IO_WAIT的状态保持，再从原进程槽之后轮转。时钟IRQ仍只操作有界状态，把设备推进交给长期启动服务栈。所有processes[i].state访问改为processes[i].execution.state；页表和FD调用仍走进程成员，没有被顺手复制到执行结构。

用所选进程的 execution 设置TSS与frame，页表仍取memory.space。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
static struct airi_trap_frame *select_user(struct user_process *p) { // 只在完整陷入现场边界切根和TSS。
    current = p; // 之后的IRQ和系统调用归属这个身份。
    p->state = USER_RUNNING; // 新用户即将获得CPU。
    airi_trap_set_stack(p->stack.virtual_address + p->stack.count * 4096ull); // 陷入时使用目标自己的内核栈。
    if (!airi_vm_activate(&p->memory.space)) // 所有高半区内核映射在准备时已存在。
        airi_panic("cannot activate selected user root"); // 不返回到缺映射的调用链。
    return airi_user_resume(p->frame); // 用户返回地址与标志必须仍符合边界。
}
```
<!-- after -->
```c
static struct airi_trap_frame *select_user(struct user_process *p) { // 只在完整陷入现场边界切根和TSS。
    current = p; // 之后的IRQ和系统调用归属这个身份。
    p->execution.state = USER_RUNNING; // 新用户即将获得CPU。
    airi_trap_set_stack(p->execution.stack.virtual_address + p->execution.stack.count * 4096ull); // 陷入时使用目标自己的内核栈。
    if (!airi_vm_activate(&p->memory.space)) // 所有高半区内核映射在准备时已存在。
        airi_panic("cannot activate selected user root"); // 不返回到缺映射的调用链。
    return airi_user_resume(p->execution.frame); // 用户返回地址与标志必须仍符合边界。
}
```

陷入入口把实际FS与FPU存入current.execution，恢复入口取回同一execution的值；这两个函数的外部接口和汇编frame布局不变。用户时钟计数、故障错误位与各等待结果也从execution读取，输出中的PID仍是所属进程身份。fork复制的是父execution的用户寄存器值与FPU状态，再为子建立独立陷入栈，未复制正在运行的内核C调用链。

exec继续采用K49的局部内存替换。它单独更新current.execution的FPU页、FS和initial，frame仍指向原陷入现场f；execution.stack与user_ticks原地保留。等待目标和I/O借用清零，从新_start开始。它不对整个execution做赋值，否则会把候选中为空的陷入栈盖到仍在执行的当前栈描述上。

<a id="execution-source"></a>
## 同步所有现场与等待调用点

本节只更新user.c，成员与调用点按当前操作一起改动：

- 调度和陷入从execution取得state、frame、FS/FPU、tick与故障状态。
- 等待路径从execution记录和清除孩子条件、FD条件、用户跨度与截止时间。
- 创建路径向create_stack传入执行者和槽号，create_fpu与place_frame只接执行者；cleanup调用destroy_execution。

前面的结构和直接助手已经更新，下面沿等待、构造、调度、替换和陷入消费者接齐成员路径。每个窗口直接定位原语句，全部完成后再编译。构建脚本、用户程序和系统调用编号保持K49状态。

poll_waiters 仍遍历进程，但等待条件与结果现场都从该进程的 execution 取得。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
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
```
<!-- after -->
```c
static void poll_waiters(void) { // 仅普通内核上下文运行，IRQ本身不复制、分配或打印。
    for (unsigned i = 0; i < PROCESS_COUNT; ++i) { // 本阶段有界等待队列由进程槽直接表示。
        struct user_process *p = &processes[i]; // 控制块在等待期间不会回收。
        if (p->execution.state != USER_IO_WAIT) // 不碰正在运行、wait孩子或已退出者。
            continue; // 检查下一个等待槽。
        long result; // 恢复到用户同一次调用的结果。
        if (p->execution.io == IO_SLEEP) { // 只有截止tick，没有用户缓冲或设备FD。
            if (airi_timer_ticks() < p->execution.io_deadline) // 时钟尚未到期，保留原现场等待。
                continue; // 不恢复用户忙轮询。
            result = 0; // 到期后同一次sleep系统调用成功返回。
        } else if (p->execution.io == IO_ASYNC) { // 原CQ等待仍保留同一个用户输出地址。
            struct airi_io_completion completion; // 先取得内核数值副本，不借用户指针给引擎。
            result = airi_async_pop(p->pid, &completion); // 空CQ为-11，不代表I/O结果。
            if (result == -11) // 尚未形成可收集结果。
                continue; // 设备由启动服务栈推进，不在这里忙等。
            if (!result && !copy_user(p, &completion, p->execution.io_address, sizeof(completion), true)) // 等待前已验证，单线程阻塞期间映射不能改变。
                airi_panic("validated completion output mapping changed"); // 不能消费CQ后静默丢结果。
        } else if (p->execution.io == IO_READY) { // 订阅的是条件，不是一次成功read的承诺。
            result = airi_fd_ready(&p->files, p->execution.io_fd); // 当前条件可能被其他进程消费或改变。
            if (result >= 0) // 负错误必须原样唤醒，不能按位筛掉。
                result &= p->execution.io_events | AIRI_READY_HUP | AIRI_READY_ERROR; // 对端关闭即使未显式请求也应可见。
            if (!result && airi_timer_ticks() < p->execution.io_deadline) // 无条件成立且尚未超时。
                continue; // 保持等待，之后由状态变化或PIT服务重查。
        } else { // 阻塞read/write重新尝试同一个有界请求。
            result = file_transfer(p, p->execution.io_fd, p->execution.io_address, (size_t)p->execution.io_bytes, p->execution.io == IO_WRITE, false, 0); // 用户buffer仍由p持有。
            if (result == -11) // 暂时依旧没有进展。
                continue; // 不把原系统调用提前返回给用户忙等。
        }
        p->execution.frame->rax = (uint64_t)result; // 用户返回位置仍在原调用之后。
        p->execution.io = IO_NONE; // 这一次等待不再持有请求地址。
        p->execution.io_address = p->execution.io_bytes = p->execution.io_deadline = 0; // 清过期的用户对象与截止值。
        p->execution.state = USER_RUNNABLE; // 结果就绪之后才发布运行资格。
        ++io_wake_count; // 实际唤醒，不把查询次数计成完成。
        airi_log("[WAIT_WAKE] pid=%u result=%ld", p->pid, result); // 普通内核服务栈输出，不在IRQ里打印。
    }
```

initial_frame 将首次寄存器值放入 execution.initial，内存内容仍经进程memory填写。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
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
static bool owned_page(struct user_memory *memory, uint64_t address, unsigned flags) { // 一次只建立一页，失败页面仍可由统一cleanup找到。
    if (memory->count == AIRI_USER_PAGES) // 包括ELF、动态页和用户栈，不包括记录页。
```
<!-- after -->
```c
        words[3] = 0; // 空环境，当前没有完整ELF辅助向量。
        stack_pointer = USER_STACK + words_offset; // CPU初始RSP指向argc槽。
    }
    p->execution.frame = &p->execution.initial; // 先保存独立寄存器值，place_frame稍后复制进实际内核栈。
    airi_memset(p->execution.frame, 0, sizeof(*p->execution.frame)); // 首次没有旧GPR。
    p->execution.frame->rip = entry; // ELF入口或已知汇编入口。
    p->execution.frame->cs = 27; // 用户代码段与RPL3。
    p->execution.frame->ss = 35; // 用户数据段与RPL3。
    p->execution.frame->rsp = stack_pointer; // 使用刚准备的用户布局。
    p->execution.frame->rflags = 0x202; // 用户IF开启，IOPL为零。
}
static bool owned_page(struct user_memory *memory, uint64_t address, unsigned flags) { // 一次只建立一页，失败页面仍可由统一cleanup找到。
    if (memory->count == AIRI_USER_PAGES) // 包括ELF、动态页和用户栈，不包括记录页。
```

ELF装载把内核栈、FPU和FS交给execution；make_stack为false时不求值slot(p)，静态TLS页面仍交memory。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
        airi_log("[ELF_REJECTED] %s", error); // 给出精确范围/权限原因。
        return -8; // -ENOEXEC，旧输出和调用者原上下文都未改变。
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
```
<!-- after -->
```c
        airi_log("[ELF_REJECTED] %s", error); // 给出精确范围/权限原因。
        return -8; // -ENOEXEC，旧输出和调用者原上下文都未改变。
    }
    if (make_stack && !create_stack(&p->execution, slot(p))) // 内核陷入栈先完成，再借高半区。
        return -12; // -ENOMEM，区域接口已回滚失败。
    if (!create_fpu(&p->execution) || !airi_memory_user_space(&p->memory.space)) // 浮点页与独立低半区都必须完整建立。
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
        p->execution.fs_base = USER_TLS + view.tls.pointer_offset; // TP位于TLS数据之后，不指向模板p_vaddr。
        uint64_t control[2] = {p->execution.fs_base, 0}; // FS:0为self，第二字保留给后续自有运行库。
        if (!copy_user(p, control, p->execution.fs_base, sizeof(control), true)) // TCB属于当前新用户根，不能直接写未活动用户VA。
            goto failed; // 未完整建立TP不发布可运行状态。
        airi_log("[ELF_TLS] bytes=%llu initialized=%llu alignment=%llu pages=%u data=0x%llx tp=0x%llx", (unsigned long long)view.tls.memory_bytes, (unsigned long long)view.tls.file_bytes, (unsigned long long)view.tls.alignment, view.tls.pages, (unsigned long long)data, (unsigned long long)p->execution.fs_base); // 模板与运行位置分别可观察。
    }
    initial_frame(p, view.entry, name); // 用户_start接收最小argc/argv协议。
    airi_log("[ELF_LOAD] entry=0x%llx segments=%u data_pages=%u argc=1", (unsigned long long)view.entry, view.count, p->memory.count); // 不含页表与内核栈的用户数据页数量。
```

受控flat构造同样传入执行者和真实槽号，原用户映像布局保持。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    *execution->frame = execution->initial; // 不借用临时C结构或其他进程的内核栈。
}
static bool prepare_flat(struct user_process *p, const uint8_t *image, size_t bytes) { // 保留受控指令故障回归，不作为ELF加载接口。
    if (!bytes || bytes > 4096 || !create_stack(p)) // 本节旧映像最多一页。
        return false; // 栈创建失败已回滚。
    if (!create_fpu(p) || !airi_memory_user_space(&p->memory.space) || !owned_page(&p->memory, USER_CODE, AIRI_VM_EXEC) || !owned_page(&p->memory, USER_STACK, AIRI_VM_WRITE)) // 分别拥有根、代码和栈。
        goto failed; // 所有权列表可清理全部已成功部分。
    airi_memcpy(airi_memory_direct(owned_at(&p->memory, 0)->physical), image, bytes); // 映像内引用保持旧固定布局。
    initial_frame(p, USER_CODE, NULL); // 受控汇编不读取argc/argv。
```
<!-- after -->
```c
    *execution->frame = execution->initial; // 不借用临时C结构或其他进程的内核栈。
}
static bool prepare_flat(struct user_process *p, const uint8_t *image, size_t bytes) { // 保留受控指令故障回归，不作为ELF加载接口。
    if (!bytes || bytes > 4096 || !create_stack(&p->execution, slot(p))) // 本节旧映像最多一页。
        return false; // 栈创建失败已回滚。
    if (!create_fpu(&p->execution) || !airi_memory_user_space(&p->memory.space) || !owned_page(&p->memory, USER_CODE, AIRI_VM_EXEC) || !owned_page(&p->memory, USER_STACK, AIRI_VM_WRITE)) // 分别拥有根、代码和栈。
        goto failed; // 所有权列表可清理全部已成功部分。
    airi_memcpy(airi_memory_direct(owned_at(&p->memory, 0)->physical), image, bytes); // 映像内引用保持旧固定布局。
    initial_frame(p, USER_CODE, NULL); // 受控汇编不读取argc/argv。
```

身份查找、空槽和延后回收改看execution.state，PID及父关系仍在进程中。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
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
static void collect_pending(void) { // 在其他用户陷入栈或启动服务栈上回收，跳过仍在使用的当前进程栈。
    for (unsigned i = 0; i < PROCESS_COUNT; ++i) // 遍历已取得延后回收资格的进程，包括用户 wait 后的孩子与内核托管孤儿。
        if (&processes[i] != current && processes[i].state == USER_REAP_PENDING) { // 当前真正运行的内核栈不能在此回收。
            unsigned pid = processes[i].pid; // cleanup清零控制块前保存诊断身份。
            bool orphan = processes[i].parent == 0; // 用户wait回收与内核托管回收分开观察。
            cleanup(&processes[i]); // 非活动根和已离开的内核栈可安全解除。
```
<!-- after -->
```c
}
static struct user_process *find_pid(unsigned pid) { // PID与数组槽分开，退出回收后不会匹配旧身份。
    for (unsigned i = 0; i < PROCESS_COUNT; ++i) // 遍历有界进程表。
        if (processes[i].execution.state != USER_UNUSED && processes[i].pid == pid) // 只查仍存活或待回收的身份。
            return &processes[i]; // 内核内部借用，不向用户交指针。
    return NULL; // 未找到活身份。
}
static struct user_process *unused(void) { // 准备中的槽不会被发布为RUNNABLE。
    for (unsigned i = 0; i < PROCESS_COUNT; ++i) // 固定容量避免无界内核分配。
        if (processes[i].execution.state == USER_UNUSED) // 调用者在不可抢占内核中独占准备。
            return &processes[i]; // 后续失败必须清掉全部所有权。
    return NULL; // 表满返回明确失败。
}
static void collect_pending(void) { // 在其他用户陷入栈或启动服务栈上回收，跳过仍在使用的当前进程栈。
    for (unsigned i = 0; i < PROCESS_COUNT; ++i) // 遍历已取得延后回收资格的进程，包括用户 wait 后的孩子与内核托管孤儿。
        if (&processes[i] != current && processes[i].execution.state == USER_REAP_PENDING) { // 当前真正运行的内核栈不能在此回收。
            unsigned pid = processes[i].pid; // cleanup清零控制块前保存诊断身份。
            bool orphan = processes[i].parent == 0; // 用户wait回收与内核托管回收分开观察。
            cleanup(&processes[i]); // 非活动根和已离开的内核栈可安全解除。
```

schedule只改变execution的运行资格，继续从原进程槽轮转并按等待条件交普通服务栈。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
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
```
<!-- after -->
```c
}
static struct airi_trap_frame *schedule(enum schedule_reason reason) { // 当前完整frame边界可切换，但IRQ不直接执行复杂I/O。
    struct user_process *old = current; // 仍暂用old的内核栈。
    if (old->execution.state == USER_RUNNING) // 等待或退出不能误改回可运行。
        old->execution.state = USER_RUNNABLE; // 时间片结束仍可运行。
    if (reason != REASON_TIMER) // 普通内核边界可重试等待条件。
        poll_waiters(); // 已有字节、关闭或错误形成后再唤醒。
    else { // IRQ只检查状态，并把设备推进交给启动服务栈。
        bool service = airi_async_work(); // 即使无人等待，已接收请求也不能被纯计算用户饿死。
        for (unsigned i = 0; i < PROCESS_COUNT; ++i) // 管道、有限条件与CQ等待都需要普通服务。
            service |= processes[i].execution.state == USER_IO_WAIT; // 不在IRQ里复制、分配或打印。
        if (service) // 完整用户frame已经保存。
            return pause_for_service(true); // 由启动栈pump与重查，再进入下一个用户。
    }
    for (unsigned n = 1; n <= PROCESS_COUNT; ++n) { // 从old之后轮转一圈。
        struct user_process *next = &processes[(slot(old) + n) % PROCESS_COUNT]; // PID与槽仍分开。
        if (next->execution.state == USER_RUNNABLE) { // 仅可运行者有可返回的用户现场。
            if (next != old && reason == REASON_TIMER) // 真IRQ切不同身份。
                ++timer_switches; // 主动阻塞不冒充抢占。
            return select_user(next); // 汇编恢复目标frame。
        }
    }
    for (unsigned i = 0; i < PROCESS_COUNT; ++i) // 没有可运行者时检查仍在等待的活进程。
        if (processes[i].execution.state == USER_WAITING || processes[i].execution.state == USER_IO_WAIT) // 可能正在等设备、超时或未来对端。
            return pause_for_service(false); // 允许启动栈hlt等待真实IRQ，不PANIC用户死锁。
    session_active = false; // 所有用户均结束，此时才算完整会话退出。
    session_idle = false; // 不再保留暂停身份。
```

exit保存进程退出码，再把当前与孩子的执行状态分别登记；父wait的结果仍属于进程。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
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
```
<!-- after -->
```c
        input_pid = 0; // 只释放本进程的消费权，不替其他身份撤销。
    airi_fd_close_all(&p->files, false); // exit立即释放文件，僵尸只保留等待状态与待回收内存。
    p->exit_code = code; // 保存给父进程wait读取。
    p->execution.state = USER_ZOMBIE; // 不再运行，但资源和状态尚存在。
    if (p->pid == root_pid) // 整个会话的结果以最初根用户为准。
        session_result = code; // 孩子退出不改变根结果。
    for (unsigned i = 0; i < PROCESS_COUNT; ++i) { // 父退出时交接全部直接孩子，不等待整次会话收拢。
        struct user_process *child = &processes[i]; // 活孩子和未收的僵尸都可能仍引用此父PID。
        if (child->execution.state == USER_UNUSED || child->parent != p->pid) // 只接管本次退出者真正拥有的孩子。
            continue; // 不改其他父子关系。
        child->parent = 0; // PID0表示内核托管者，不是可运行的用户进程。
        if (child->execution.state == USER_ZOMBIE) // 父已无法取得这份退出状态。
            child->execution.state = USER_REAP_PENDING; // 下一条安全内核栈回收，不让僵尸永久占槽。
        airi_log("[ORPHAN_ADOPT] pid=%u former_parent=%u finished=%u", child->pid, p->pid, child->execution.state == USER_REAP_PENDING); // 活孩子仍正常调度或等待。
    }
    struct user_process *parent = find_pid(p->parent); // 父身份可能已退出或不存在。
    if (p->pid != root_pid && (!parent || parent->execution.state == USER_ZOMBIE || parent->execution.state == USER_REAP_PENDING)) { // 会话根的统计保留到launch结束；普通孤儿无需用户wait。
        p->parent = 0; // 不再保留已失效的用户父PID。
        p->execution.state = USER_REAP_PENDING; // 当前栈仍在使用，只登记延后回收资格。
        airi_log("[ORPHAN_EXIT] pid=%u code=%d deferred=1", p->pid, code); // 真正释放发生在离开此栈后。
    }
    if (parent && parent->execution.state == USER_WAITING && (parent->execution.wait_target == -1 || (uint64_t)parent->execution.wait_target == p->pid)) { // 只有真正等待此孩子的父进程可以被唤醒。
        bool copied = !parent->execution.wait_status || copy_user(parent, &code, parent->execution.wait_status, sizeof(code), true); // 通过父进程自己的页表写结果。
        parent->execution.frame->rax = copied ? p->pid : (uint64_t)-14ll; // 返回子PID或明确地址错误。
        parent->execution.state = USER_RUNNABLE; // 结果形成后才公开可运行状态。
        parent->execution.wait_target = 0; // 这一次等待已经结束。
        parent->execution.wait_status = 0; // 不保留用户局部对象的过期地址。
        if (copied) // 地址错误时保留僵尸，让父进程能用有效指针重试。
            p->execution.state = USER_REAP_PENDING; // 下一次别的用户陷入后才能释放本次仍在用的内核栈。
    }
    airi_log("[USER_EXIT] pid=%u code=%d user_ticks=%u", p->pid, code, p->execution.user_ticks); // 正常系统调用上下文记录身份结果。
    return schedule(REASON_EXIT); // 子进程或父进程可继续，未必立刻回启动栈。
}
static long fork_current(struct airi_trap_frame *parent_frame) { // 首版逐页复制，不实现COW。
```

fork取得孩子自己的内核栈和FPU页，复制父execution的用户现场与FS，最后才发布孩子执行资格。

<!-- replace: kernel/process/user.c -->
<!-- before -->
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
```
<!-- after -->
```c
    struct user_process *child = unused(); // 未发布的新槽。
    if (!child) // 表满没有可分配身份。
        return -11; // -EAGAIN。
    if (!create_stack(&child->execution, slot(child)) || !create_fpu(&child->execution) || !airi_memory_user_space(&child->memory.space)) // 先创建独立内核栈和空低半区。
        goto failed; // 父状态从未改变。
    for (unsigned i = 0; i < parent->memory.count; ++i) { // 包括用户栈和此前申请的堆页。
        const struct user_page *page = owned_at(&parent->memory, i); // 每页原VA、权限和物理地址。
        if (!owned_page(&child->memory, page->address, page->flags)) // 子页在相同VA，但物理所有权独立。
            goto failed; // 回滚全部子资源，不改变父页面。
        airi_memcpy(airi_memory_direct(owned_at(&child->memory, child->memory.count - 1)->physical), airi_memory_direct(page->physical), 4096); // 复制完整页内容，父子以后写入互不影响。
    }
    child->execution.fs_base = parent->execution.fs_base; // fork继承同一用户VA，指向复制后的子页面，不共享物理内容。
    airi_memcpy(airi_memory_direct(child->execution.fpu_physical), airi_memory_direct(parent->execution.fpu_physical), AIRI_FPU_BYTES); // 父陷入时已保存，子复制真实状态而非共享状态页。
    airi_fd_clone(&child->files, &parent->files); // 页复制全部成功后再取得FD引用，失败cleanup仍能撤销。
    child->execution.initial = *parent_frame; // 用户RIP/RSP和寄存器从同一次fork返回点继续。
    child->execution.initial.rax = 0; // 子分支看到fork返回0。
    child->pid = next_pid++; // 全部内存成功后才分配PID。
    child->parent = parent->pid; // 关系用身份而不是可能复用的槽指针表示。
    if (airi_async_open(child->pid)) // fork创建空上下文，用户内存里的父handle整数并不授权子访问。
        goto failed; // 其余子资源统一回滚，父不变。
    child->async_owned = true; // 成功后才记资源所有权。
    place_frame(&child->execution); // 只把用户返回现场放入子内核栈，不复制父的内核C调用链。
    child->execution.state = USER_RUNNABLE; // 准备完成后才让IRQ调度器看见它。
    airi_log("[FORK] parent=%u child=%u copied_pages=%u", parent->pid, child->pid, child->memory.count); // 实际复制数量含用户栈与堆。
    return child->pid; // 父分支得到正子PID。
failed: // 还没有向调度器发布孩子，当前仍在父根和父内核栈上。
```

wait的孩子状态与父等待借用移入execution，输出仍按各自进程内存验证。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
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
```
<!-- after -->
```c
    bool found = false; // 区分没有孩子与孩子仍在运行。
    for (unsigned i = 0; i < PROCESS_COUNT; ++i) { // 只匹配当前PID的子关系。
        struct user_process *child = &processes[i]; // 槽可能已经回收或属于其他父进程。
        if (child->execution.state == USER_UNUSED || child->execution.state == USER_REAP_PENDING || child->parent != current->pid || (target != -1 && (uint64_t)target != child->pid)) // 不允许等待别人的子进程。
            continue; // 查找下一个可能孩子。
        found = true; // 至少有一个真实孩子。
        if (child->execution.state == USER_ZOMBIE) { // 已结束孩子可以在父内核栈上立即回收。
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
    current->execution.wait_target = target; // 保存要等待的身份条件。
    current->execution.wait_status = status; // 父阻塞期间单线程不会修改这些用户映射。
    current->execution.state = USER_WAITING; // 先记录条件与结果地址，再让调度器跳过父。
    ++wait_blocks; // 真实阻塞，而非轮询返回成功。
    return schedule(REASON_WAIT); // 孩子结束时填写此f->rax并唤醒父。
}
```

exec只更新execution中需要替换的CPU状态与旧等待借用，原栈和user_ticks保持，不给整个execution赋值。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
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
```
<!-- after -->
```c
    unsigned old_pages = current->memory.count; // 清理前保留替换规模，不复制整进程控制块。
    struct user_memory old_memory = take_memory(&current->memory); // 旧根此刻仍活动，其所有权暂由局部描述接住。
    current->memory = take_memory(&next.memory); // 新根所有权进入原进程槽，候选来源立即为空。
    uint64_t old_fpu = current->execution.fpu_physical; // 旧用户CPU状态页不属于用户地址空间。
    current->execution.fpu_physical = next.execution.fpu_physical; // 替换为准备好的默认x87/SSE状态。
    next.execution.fpu_physical = 0; // 候选从此不能再归还这页。
    current->execution.fs_base = next.execution.fs_base; // 新映像的静态TLS指针，或无TLS时的0。
    current->execution.initial = next.execution.initial; // 保存新入口数值，不保存指向候选栈局部对象的指针。
    current->execution.frame = f; // 原内核陷入栈及TSS继续属于同一执行者。
    *f = current->execution.initial; // 全部可失败准备完成之后才覆盖旧RIP/RSP和寄存器。
    current->execution.fault_error = 0; // 新映像没有旧用户故障报告。
    current->exit_code = 0; // 运行中的新映像还没有退出结果。
    current->execution.wait_target = 0; // 原先完成的wait条件不能成为新映像状态。
    current->execution.wait_status = 0; // 撤销旧用户输出地址的数值借用。
    current->execution.io = IO_NONE; // exec发生在运行系统调用中，没有需要继续的旧阻塞操作。
    current->execution.io_fd = current->execution.io_events = 0; // FD表原地保留，但旧等待条件清零。
    current->execution.io_address = current->execution.io_bytes = current->execution.io_deadline = 0; // 不把旧空间中的请求指针带到新映像。
    airi_fd_close_all(&current->files, true); // 进程FD表原地保留，成功提交才关闭CLOEXEC。
    if (input_pid == current->pid) // 若前台进程自行exec，旧输入权限不能漏到其他映像。
        input_pid = 0; // 成功提交时才撤销，失败exec仍保留旧权限。
    if (acquire_input) { // 本次新映像确实是init管理的shell。
        airi_fd_close(&current->files, 0); // 无表项时-9可忽略；已有普通FD0也按此启动策略关闭。
        if (!airi_fd_attach_input(&current->files)) // 新FD0必须为空且输入引用成功接入。
            airi_panic("cannot attach supervised shell input"); // 事务提交内部不变量错误。
        input_pid = current->pid; // 文件引用完成后才发布唯一消费身份。
    }
    current->execution.state = USER_RUNNING; // PID、父关系、已运行tick、内核栈和异步owner均原地保持。
    if (!airi_vm_activate(&current->memory.space)) // 新根已借到当前内核栈及内核映射。
        airi_panic("cannot activate committed exec image"); // 硬件不变量失败不能回旧用户继续。
    destroy_memory(&old_memory); // CR3已切到新根，只归还局部接住的旧用户内存。
```

共同用户入口先把实际FS/FPU保存进current.execution，再进入原分派。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    return 0; // 调用者直接恢复新f，不把这个0写回旧用户调用。
}
void airi_user_enter(struct airi_trap_frame *f) { // 所有CS3来源在分派设备、timer或syscall之前共同保存。
    if (!session_active || !current || current->state != USER_RUNNING || (f->cs & 3) != 3) // 当前实际栈必须有运行所有者。
        airi_panic("CPU state without a running user"); // 内核int129不经过这个入口。
    current->fs_base = airi_tls_take(); // 先取真实用户基址，再把硬件FS清零供内核处理。
    airi_fpu_save(airi_memory_direct(current->fpu_physical)); // x87/XMM也在此唯一入口保存，不能因设备没有新ISR而跳过。
}
struct airi_trap_frame *airi_user_resume(struct airi_trap_frame *f) { // 所有用户iretq路径使用同一规范地址和标志策略。
    if (f->cs != 27 || f->ss != 35 || f->rip >= AIRI_USER_LIMIT || f->rsp >= AIRI_USER_LIMIT) { // 坏返回不能变成内核GP。
```
<!-- after -->
```c
    return 0; // 调用者直接恢复新f，不把这个0写回旧用户调用。
}
void airi_user_enter(struct airi_trap_frame *f) { // 所有CS3来源在分派设备、timer或syscall之前共同保存。
    if (!session_active || !current || current->execution.state != USER_RUNNING || (f->cs & 3) != 3) // 当前实际栈必须有运行所有者。
        airi_panic("CPU state without a running user"); // 内核int129不经过这个入口。
    current->execution.fs_base = airi_tls_take(); // 先取真实用户基址，再把硬件FS清零供内核处理。
    airi_fpu_save(airi_memory_direct(current->execution.fpu_physical)); // x87/XMM也在此唯一入口保存，不能因设备没有新ISR而跳过。
}
struct airi_trap_frame *airi_user_resume(struct airi_trap_frame *f) { // 所有用户iretq路径使用同一规范地址和标志策略。
    if (f->cs != 27 || f->ss != 35 || f->rip >= AIRI_USER_LIMIT || f->rsp >= AIRI_USER_LIMIT) { // 坏返回不能变成内核GP。
```

统一返回、设备与PIT入口从同一execution恢复或保存，普通进程内存和PID仍通过current取得。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    }
    f->rflags &= ~((3ull << 12) | (1ull << 14) | (1ull << 17)); // 清IOPL、NT和VM。
    f->rflags |= 0x202; // 保留必需bit1并保持用户IF。
    airi_tls_restore(current->fs_base); // 与所选用户身份一起恢复，随后内核C不会通过FS访问数据。
    airi_fpu_restore(airi_memory_direct(current->fpu_physical)); // 当前身份已确定，恢复后内核和公共汇编不再使用浮点/XMM。
    return f; // 返回选中进程的完整现场。
}
struct airi_trap_frame *airi_user_device_irq(struct airi_trap_frame *f) { // 本函数只在真实设备IRQ打断ring3后调用。
    if (!session_active || !current || current->state != USER_RUNNING) // 用户返回必须有明确现场所有者。
        airi_panic("device IRQ without a running user"); // 不把未知栈交给启动继续点。
    if (!airi_async_work()) // 同步启动请求或已收集的旧通知无需切用户身份。
        return airi_user_resume(f); // 用户返回策略仍统一核对。
    current->frame = f; // 保存完整GPR与用户RIP/RSP，稍后从同一指令继续。
    current->state = USER_RUNNABLE; // 不丢掉被设备提示打断的计算用户运行资格。
    return pause_for_service(false); // 切到长期普通栈后才pump、复制用户输出和唤醒等待者。
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
```
<!-- after -->
```c
    }
    f->rflags &= ~((3ull << 12) | (1ull << 14) | (1ull << 17)); // 清IOPL、NT和VM。
    f->rflags |= 0x202; // 保留必需bit1并保持用户IF。
    airi_tls_restore(current->execution.fs_base); // 与所选用户身份一起恢复，随后内核C不会通过FS访问数据。
    airi_fpu_restore(airi_memory_direct(current->execution.fpu_physical)); // 当前身份已确定，恢复后内核和公共汇编不再使用浮点/XMM。
    return f; // 返回选中进程的完整现场。
}
struct airi_trap_frame *airi_user_device_irq(struct airi_trap_frame *f) { // 本函数只在真实设备IRQ打断ring3后调用。
    if (!session_active || !current || current->execution.state != USER_RUNNING) // 用户返回必须有明确现场所有者。
        airi_panic("device IRQ without a running user"); // 不把未知栈交给启动继续点。
    if (!airi_async_work()) // 同步启动请求或已收集的旧通知无需切用户身份。
        return airi_user_resume(f); // 用户返回策略仍统一核对。
    current->execution.frame = f; // 保存完整GPR与用户RIP/RSP，稍后从同一指令继续。
    current->execution.state = USER_RUNNABLE; // 不丢掉被设备提示打断的计算用户运行资格。
    return pause_for_service(false); // 切到长期普通栈后才pump、复制用户输出和唤醒等待者。
}
struct airi_trap_frame *airi_user_timer(struct airi_trap_frame *f) { // IRQ路径不分配、不回收、不打印，只有状态与根选择。
    if (!session_active || !current || current->execution.state != USER_RUNNING) // CS3的IRQ必须有明确用户所有者。
        airi_panic("user IRQ without a running process"); // 不把未知内核栈交给调度器。
    current->execution.frame = f; // 保存真正被打断的位置，不只保存C ABI寄存器。
    ++current->execution.user_ticks; // 实际CPU用户时间片事件数。
    if (current->execution.user_ticks % 2 == 0) // 每两个实际IRQ尝试一次round-robin。
        return schedule(REASON_TIMER); // 没有其他可运行者时恢复同一个进程。
    return airi_user_resume(f); // 也不能跳过坏返回检查。
}
struct airi_trap_frame *airi_user_trap(struct airi_trap_frame *f) { // 内核仍不可抢占，只在完整用户陷入边界处理进程状态。
    if (f->vector == 129 && f->cs == 8 && (!session_active || session_idle) && current && current->execution.state == USER_RUNNABLE) { // 内核启动一组已经准备好的用户。
        kernel_return = f; // 启动栈的继续位置一直保留。
        session_idle = false; // 当前int129重新登记了仍在启动栈上的继续位置。
        session_active = true; // 之后的CS3陷入有会话归属。
        return select_user(current); // 按目标根和TSS进入首个用户。
    }
    if (!session_active || !current || current->execution.state != USER_RUNNING || (f->cs & 3) != 3) // 内核错误不能伪装为用户故障。
        airi_panic("user trap without a running process"); // 维持故障边界。
    collect_pending(); // 现在确实在另一条内核栈上，回收已经离开的孩子。
    current->execution.frame = f; // 系统调用与故障都以当前实际现场为继续点。
    if (f->vector != 128) { // 用户CPU故障只结束拥有者。
        current->execution.fault_error = f->error; // 保存权限故障证据。
        airi_log("[USER_FAULT] pid=%u vector=%llu error=0x%llx rip=0x%llx cr2=0x%llx", current->pid, (unsigned long long)f->vector, (unsigned long long)f->error, (unsigned long long)f->rip, (unsigned long long)(f->vector == 14 ? airi_trap_fault_address() : 0)); // PF才读取本次CR2。
        return exit_current(-(128 + (int)f->vector)); // 自有故障编码，不宣称Linux信号ABI。
    }
```

同步read/write阻塞时，把FD、输出范围与原继续点保存在当前execution。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
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
```
<!-- after -->
```c
        bool positioned = f->rax == 15 || f->rax == 16; // 显式偏移不支持阻塞流。
        long result = file_transfer(current, f->rdi, f->rsi, (size_t)f->rdx, write, positioned, f->r10); // 先实际尝试一次。
        if (result == -11 && !positioned && !airi_fd_nonblocking(&current->files, (unsigned)f->rdi)) { // 阻塞流暂不可完成，用户不能提前看到EAGAIN。
            current->execution.io = write ? IO_WRITE : IO_READ; // 保存需要重试的操作种类。
            current->execution.io_fd = (unsigned)f->rdi; // 此时FD已由file_transfer完整验证。
            current->execution.io_address = f->rsi; // 用户页继续归此进程所有，不借内核局部buffer。
            current->execution.io_bytes = f->rdx; // 一次最多256字节，已经验证跨度。
            current->execution.state = USER_IO_WAIT; // 条件和数据先登记，再移出可运行集合。
            ++io_block_count; // 实际阻塞计数。
            airi_log("[WAIT_REGISTER] pid=%u operation=%s fd=%u", current->pid, write ? "write" : "read", current->execution.io_fd); // 普通系统调用上下文记录。
            return schedule(REASON_IO); // 选择别的用户或进入真正内核空闲等待。
        }
        f->rax = (uint64_t)result; // 非阻塞仍把EAGAIN交给用户，其他结果直接返回。
```

就绪等待的事件和绝对截止也属于当前execution。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
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
```
<!-- after -->
```c
            if (result >= 0) // 负错误不参与位运算。
                result &= (long)f->rsi | AIRI_READY_HUP | AIRI_READY_ERROR; // 关闭与错误始终报告。
            if (!result && f->rdx) { // 条件未满足且愿意有限等待。
                current->execution.io = IO_READY; // 不读写任何字节，只重查条件。
                current->execution.io_fd = (unsigned)f->rdi; // 属于此进程的稳定表项。
                current->execution.io_events = (unsigned)f->rsi; // 保存订阅条件。
                current->execution.io_deadline = airi_timer_ticks() + f->rdx; // 真实时钟截止，不靠循环次数。
                current->execution.state = USER_IO_WAIT; // 原frame已保存在进程中。
                ++io_block_count; // 这次调用确实阻塞。
                airi_log("[WAIT_REGISTER] pid=%u operation=ready fd=%u", current->pid, current->execution.io_fd); // 由普通内核上下文记录。
                return schedule(REASON_IO); // 返回到同一次wait调用之后。
            }
        }
```

CQ等待保存同一execution的输出借用，设备推进仍交普通启动服务栈。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
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
```
<!-- after -->
```c
            if (!result && !copy_user(current, &completion, f->rdi, sizeof(completion), true)) // 单线程映射稳定。
                airi_panic("validated completion output changed"); // 不能消费后隐瞒复制失败。
            if (result == -11 && f->rsi) { // 空CQ且选择阻塞，保留这同一次调用。
                current->execution.io = IO_ASYNC; // poll_waiters只收集，设备推进在启动栈。
                current->execution.io_address = f->rdi; // 用户输出页仍由进程拥有。
                current->execution.io_bytes = sizeof(completion); // 固定ABI长度，不借C局部对象。
                current->execution.state = USER_IO_WAIT; // 先保存条件和输出，再撤销运行资格。
                ++io_block_count; // 记录真实CQ等待，不强迫快设备也假装阻塞。
                airi_log("[WAIT_REGISTER] pid=%u operation=cq", current->pid); // 等设备完成后原调用继续。
                return schedule(REASON_IO); // 可切其他用户或进入启动服务空闲。
```

sleep继续使用原绝对截止，登记与恢复的执行资格从execution取得。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
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
```
<!-- after -->
```c
        else if (now > UINT64_MAX - f->rdi) // 不让截止计数回绕成已经过期。
            f->rax = (uint64_t)-75ll; // -EOVERFLOW。
        else { // 请求完整有效，保存条件后再退出可运行集合。
            current->execution.io = IO_SLEEP; // poll_waiters不再把它当FD操作。
            current->execution.io_fd = current->execution.io_events = 0; // 清与此次请求无关的旧数值。
            current->execution.io_address = current->execution.io_bytes = 0; // sleep不借用用户指针。
            current->execution.io_deadline = now + f->rdi; // 保留绝对截止，不在每次轮询延长。
            current->execution.state = USER_IO_WAIT; // 此前已经保存实际系统调用frame。
            ++io_block_count; // 记录真正阻塞，而非返回0的空操作。
            airi_log("[WAIT_REGISTER] pid=%u operation=sleep ticks=%llu", current->pid, (unsigned long long)f->rdi); // 普通调用栈记录，不在IRQ打印。
            return schedule(REASON_IO); // 其他用户继续；全阻塞时启动栈hlt等PIT。
```

FS的保存值属于execution，setter/getter仍保留原调用号与范围。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
        if (f->rdi >= AIRI_USER_LIMIT) // 非规范中间地址与所有内核高半区都在特权WRMSR之前拒绝。
            f->rax = (uint64_t)-22ll; // -EINVAL，旧保存值不变。
        else { // 0表示停用，其余低半区值即使尚未映射也可登记。
            current->fs_base = f->rdi; // 当前硬件仍为内核0，统一返回入口才写用户值。
            f->rax = 0; // 不解引用、不读取该用户地址。
        }
    }
    else if (f->rax == 34) // 查询的是陷入时从硬件读取的真实当前值。
        f->rax = current->fs_base; // 低半区基址可由long无歧义表示，不返回内核地址。
    else if (f->rax == 35 || f->rax == 36) // 自有显式偏移向量操作，R10保存第四参数。
        f->rax = (uint64_t)vector_transfer(current, f->rdi, f->rsi, f->rdx, f->r10, f->rax == 36); // 描述和前缀在调用内完成，不建立异步借用。
    else if (f->rax == 37) { // 有符号相对定位仍由打开对象的所有者一次完成。
```
<!-- after -->
```c
        if (f->rdi >= AIRI_USER_LIMIT) // 非规范中间地址与所有内核高半区都在特权WRMSR之前拒绝。
            f->rax = (uint64_t)-22ll; // -EINVAL，旧保存值不变。
        else { // 0表示停用，其余低半区值即使尚未映射也可登记。
            current->execution.fs_base = f->rdi; // 当前硬件仍为内核0，统一返回入口才写用户值。
            f->rax = 0; // 不解引用、不读取该用户地址。
        }
    }
    else if (f->rax == 34) // 查询的是陷入时从硬件读取的真实当前值。
        f->rax = current->execution.fs_base; // 低半区基址可由long无歧义表示，不返回内核地址。
    else if (f->rax == 35 || f->rax == 36) // 自有显式偏移向量操作，R10保存第四参数。
        f->rax = (uint64_t)vector_transfer(current, f->rdi, f->rsi, f->rdx, f->r10, f->rax == 36); // 描述和前缀在调用内完成，不建立异步借用。
    else if (f->rax == 37) { // 有符号相对定位仍由打开对象的所有者一次完成。
```

launch向主execution放置首次frame，服务循环与最终统计也读取这份执行状态。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
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
```
<!-- after -->
```c
        return async_error; // 应用可见普通资源失败。
    }
    root->async_owned = true; // cleanup和exit之后负责释放。
    place_frame(&root->execution); // 寄存器值进入本进程真实内核栈。
    root->execution.state = USER_RUNNABLE; // 发布首次运行资格。
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
                if (processes[(slot(current) + n) % PROCESS_COUNT].execution.state == USER_RUNNABLE) { // 等待者可能刚被唤醒。
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
    *result = (struct user_result){(int)code, root->execution.user_ticks, timer_switches, wait_blocks, root->execution.fault_error, io_block_count, io_wake_count, idle_halt_count}; // 回收前复制必要数值。
    for (unsigned i = 0; i < PROCESS_COUNT; ++i) // 当前不在任何用户内核栈上。
        if (processes[i].execution.state != USER_UNUSED) // wait已回收槽不重复处理。
            cleanup(&processes[i]); // 收拢剩余僵尸和未被父wait的资源。
    current = NULL; // 不保留已清掉的身份指针。
    supervisor_pid = input_pid = 0; // 有限会话结束后不能把授权留给下次运行。
```

<a id="execution-observe"></a>
## 原有纯计算、阻塞和替换都要继续运行

<!-- run -->
```sh
./airios build  # 编译全部新成员路径与执行者参数，不能留旧state访问。
./airios test  # 检查抢占、等待、用户故障隔离、fork/exec和整个会话回收。
```

首次编译若还有 `no member named state`，说明某个调度局部指针仍在读旧成员；例如schedule里的old也必须改为old->execution.state，不能只改current和processes数组。构建通过后继续观察实际用户运行，类型检查不能证明每次切换选中了正确执行者。

交互可以从run sh进入simd、tls、ctls、heap，再退出。这几条原程序分别让真实x87/SSE和FS跨PIT抢占、静态TLS跨fork/exec、arena跨坏与成功exec。shell测试也会执行阻塞管道、CQ等待、页耗尽及重复回收，继续要求真实命令退出为0。

```sh
./airios run  # 当前仍为纯串口主线，无图形源码依赖。
```

```text
run sh
simd
tls
ctls
heap
exit
```

下一个共享执行者不能靠复制user_process实现：那会再次复制页面和FD所有权。它需要引用同一进程资源，同时取得自己的内核栈、FPU状态、用户栈、TLS和等待记录。用户malloc目前仍单线程，新增执行者不会自动让分配器或其他用户对象具备同步；pthread风格接口与futex须在真实共享和生命周期规则成立后接入。Linux线程对照继续沿[L05的pthread与futex实验](../linux/05-threads.md)，但当前个人主机状态仍需单独验证。
