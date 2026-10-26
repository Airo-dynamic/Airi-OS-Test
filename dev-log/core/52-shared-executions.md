# K52 · 在同一进程里调度多个共享资源的执行者

从 K51 继续。线程池中的两个工作函数需要访问同一份请求数据和FD，又要各自保留栈、局部错误和CPU继续点。fork会复制用户页，不能直接承担这项工作。这里提供自有的create/join接口：主执行者创建两个工作者，它们共享用户地址空间、FD表和异步资源，但使用各自的用户栈、内核陷入栈、TLS、FS和FPU保存页。

threads命令先让两个工作者读同一个打开文件，再跨真实PIT抢占修改不同本地值，主执行者join取得64位结果。随后覆盖八槽容量、重复创建、管道等待、跨进程TID拒绝、工作者触发整个进程退出与用户缺页。当前接口有明确范围：只有主执行者管理工作者，全系统最多八条额外执行链；没有pthread、detach、取消、TLS析构或主执行者独立退出。

<a id="thread-resource-owner"></a>
## 一个进程持有资源，多条执行链借它运行

user_process继续唯一持有memory、FD表、异步owner、PID和父关系，并内嵌主execution。新增workers[8]只保存额外执行链。每个execution通过process指针借所属进程的资源，tid表示本执行者；主tid初始等于pid，工作tid另从同一个单调身份空间取得，本次内核启动内不回绕复用。

例如进程PID60可以有TID60、61、62。三个执行者通过同一memory.space访问普通BSS，但FS分别指向三个本地运行块；普通变量可共享，编译器_Thread_local变量依据当前FS选择副本。不同栈属于不同执行链，却都映射在这个进程的用户地址空间中，因此这不是进程之间的内存安全隔离。

进程拥有生命周期：主执行者从main返回或任一执行者调用airi_exit，都结束整个进程；工作函数普通return只留下工作结果，等待主执行者join。该范围允许进程作为唯一资源拥有者，在结束时先停掉全部工作者，再归还共享页。当前没有实现让主执行者单独退出、其他工作者继续运行的pthread_exit语义，也没有借用重复描述代替共享引用关系。

在user.c增加WORKER_COUNT=8、EXECUTION_COUNT=16。execution增加所属进程、tid、join_tid和long结果，process保存当前ELF的TLS模板布局与只读映像输入指针。后者来自内核初始目录，必须持续有效；以后若从可释放文件缓冲装载ELF，还需另行持有模板，不能直接延用该借用合同。先替换槽预算与结构，再沿调度、创建和join接入实际函数；全部调用点闭合后再构建。

先在文件开头接入工作槽预算与IO_JOIN，原进程数和用户地址范围保持。

本章每组第一块用于定位旧文本，只敲第二块替换；新增的完整函数也接在给出的旧窗口处。全部连接闭合后再构建。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
#include <airi/time_abi.h> // 单调时间只输出固定数值，用户指针由当前根验证。
#include <airi/vector_abi.h> // 向量描述在一次内核调用内完整复制，文件操作前检查本次前缀。
#include <airi/tls.h> // 每次真实用户陷入取回FS，内核处理期间清零。
#include <airi/fpu.h> // 每进程拥有一份非用户映射的浮点/SSE状态。
#include <airi/async.h> // 进程轮次与固定缓冲资源的生命周期。
#include <airi/arch.h> // CPU空闲与用户调度门留在架构适配。
#include <airi/storage.h> // 用户存储请求经过有界副本进入日志服务。
#include <airi/file.h> // 每进程FD表共享打开文件对象。
#include <airi/console.h> // 内核命令消费服务结果，不依赖主题和绘图。
#include <airi/elf.h> // 独立用户ELF检查器。
#include <airi/user.h> // 完整陷入与安全用户返回。
#include <airi/memory.h> // 页面、内核栈与长期根的所有权。
#include <airi/vm.h> // 每个进程有独立低半区。
#include <airi/timer.h> // 用户时间片依赖真实IRQ0。
#include <airi/kernel.h> // 有界串口输出与故障报告。
#define USER_CODE 0x400000ull // 旧受控映像的用户入口。
#define USER_STACK 0x70000000ull // 初始栈最高一页保持原地址，新增页向下展开。
#define STACK_BASE 0xffffe10000000000ull // 所有进程陷入栈位于同一长期内核分支。
#define USER_HEAP 0x10000000ull // ELF静态范围之外的用户页分配起点。
#define USER_TLS 0x60000000ull // 静态TLS运行块不与ELF、动态堆和固定用户栈重叠。
#define USER_HEAP_END 0x20000000ull // 动态用户页只占这段独立范围，不接近初始用户栈。
#define PROCESS_COUNT 8u // 有界进程表，不做动态扩容。
enum user_state { USER_UNUSED, USER_RUNNABLE, USER_RUNNING, USER_WAITING, USER_IO_WAIT, USER_ZOMBIE, USER_REAP_PENDING }; // 退出与回收是两个不同状态。
enum io_operation { IO_NONE, IO_READ, IO_WRITE, IO_READY, IO_ASYNC, IO_SLEEP }; // 等待保存操作条件，不保留内核局部buffer。
struct user_page { uint64_t address, physical; unsigned flags; }; // fork需要把同一VA映射到另一个物理页。
#define PAGE_RECORDS (4096u / sizeof(struct user_page)) // 一物理页放170项，余下16字节不用。
#define RECORD_CHUNKS ((AIRI_USER_PAGES + PAGE_RECORDS - 1) / PAGE_RECORDS) // 1024项最多借七个记录页。
```
<!-- after -->
```c
#include <airi/time_abi.h> // 单调时间只输出固定数值，用户指针由当前根验证。
#include <airi/vector_abi.h> // 向量描述在一次内核调用内完整复制，文件操作前检查本次前缀。
#include <airi/tls.h> // 每次真实用户陷入取回FS，内核处理期间清零。
#include <airi/fpu.h> // 每条执行链拥有一份非用户映射的浮点/SSE状态。
#include <airi/async.h> // 进程轮次与固定缓冲资源的生命周期。
#include <airi/arch.h> // CPU空闲与用户调度门留在架构适配。
#include <airi/storage.h> // 用户存储请求经过有界副本进入日志服务。
#include <airi/file.h> // 每进程FD表共享打开文件对象。
#include <airi/console.h> // 内核命令消费服务结果，不依赖主题和绘图。
#include <airi/elf.h> // 独立用户ELF检查器。
#include <airi/user.h> // 完整陷入与安全用户返回。
#include <airi/memory.h> // 页面、内核栈与长期根的所有权。
#include <airi/vm.h> // 每个进程有独立低半区。
#include <airi/timer.h> // 用户时间片依赖真实IRQ0。
#include <airi/kernel.h> // 有界串口输出与故障报告。
#define USER_CODE 0x400000ull // 旧受控映像的用户入口。
#define USER_STACK 0x70000000ull // 初始栈最高一页保持原地址，新增页向下展开。
#define STACK_BASE 0xffffe10000000000ull // 所有进程陷入栈位于同一长期内核分支。
#define USER_HEAP 0x10000000ull // ELF静态范围之外的用户页分配起点。
#define USER_TLS 0x60000000ull // 静态TLS运行块不与ELF、动态堆和固定用户栈重叠。
#define USER_HEAP_END 0x20000000ull // 动态用户页只占这段独立范围，不接近初始用户栈。
#define PROCESS_COUNT 8u // 有界进程表，不做动态扩容。
#define WORKER_COUNT 8u // 全系统最多八条额外用户执行链，主执行者仍各占进程槽。
#define EXECUTION_COUNT (PROCESS_COUNT + WORKER_COUNT) // 调度同时遍历主执行者和工作执行者。
enum user_state { USER_UNUSED, USER_RUNNABLE, USER_RUNNING, USER_WAITING, USER_IO_WAIT, USER_ZOMBIE, USER_REAP_PENDING }; // 退出与回收是两个不同状态。
enum io_operation { IO_NONE, IO_READ, IO_WRITE, IO_READY, IO_ASYNC, IO_SLEEP, IO_JOIN }; // 等待保存操作条件，不保留内核局部buffer。
struct user_page { uint64_t address, physical; unsigned flags; }; // fork需要把同一VA映射到另一个物理页。
#define PAGE_RECORDS (4096u / sizeof(struct user_page)) // 一物理页放170项，余下16字节不用。
#define RECORD_CHUNKS ((AIRI_USER_PAGES + PAGE_RECORDS - 1) / PAGE_RECORDS) // 1024项最多借七个记录页。
```

替换原执行者、进程及当前对象声明，分别保存进程资源和真实running。

<!-- replace: kernel/process/user.c -->
<!-- before -->
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
static struct user_process processes[PROCESS_COUNT]; // 槽可以复用，PID单调增长，不等于槽下标。
static struct user_process *current; // 只在完整陷入边界改变当前身份。
```
<!-- after -->
```c
struct user_process; // 执行者回指所属进程，资源定义稍后闭合。
struct user_execution { // 一条用户执行链的CPU现场、内核陷入栈和等待条件。
    struct user_process *process; // 借所属进程的共享内存、FD与异步owner，不独立归还它们。
    unsigned tid, join_tid; // 全局不复用的执行身份，以及当前主执行者等待的工作身份。
    long result; // 工作函数的64位返回值，join取得后才能收拢执行者。
    uint64_t fs_base; // 当前执行者的用户FS基址，不拥有对应用户数据页。
    uint64_t fpu_physical; // 独立内核状态页，保存本执行者的x87/SSE寄存器。
    struct airi_kernel_region stack; // 真正进入内核以后使用的栈，离开它才能销毁。
    unsigned user_ticks; // 本执行者实际收到的用户时钟事件。
    uint64_t fault_error; // 最近一次结束本执行者的用户故障错误位。
    enum user_state state; // 主/工作执行者分别保存调度资格，不由共享PID推导。
    int64_t wait_target; // 等待的孩子PID条件，不是地址空间所有权。
    uint64_t wait_status; // wait结果的用户VA，只在当前等待期间借用。
    enum io_operation io; // 当前阻塞操作类型，不保存内核局部buffer。
    unsigned io_fd, io_events; // 当前I/O等待条件，FD本体归进程表。
    uint64_t io_address, io_bytes, io_deadline; // 当前操作借用的用户跨度和截止tick。
    struct airi_trap_frame initial; // 第一次进入用户时的寄存器值，不借临时栈。
    struct airi_trap_frame *frame; // 实际陷入以后指向本执行者内核栈上的现场。
};
struct user_process { // 进程持有共享资源，内嵌主执行者，额外工作执行者回指本对象。
    struct user_memory memory; // 进程持有用户页表、数据页和元数据页。
    bool async_owned; // 当前PID的异步owner只由进程打开、reset和关闭。
    struct airi_fd_table files; // FD引用属于进程，exec保留、fork复制表项引用。
    unsigned pid, parent; // 进程身份与父关系不随CPU执行现场一起替换。
    int exit_code; // 父进程wait取得的进程退出结果。
    struct user_execution execution; // 主执行者负责创建与join，普通进程退出结束整个组。
    struct airi_elf_tls tls; // 当前映像的已验证静态TLS模板布局，工作执行者从初值重新建立。
    const uint8_t *tls_image; // 借只读初始ELF字节，其生命周期覆盖整个用户会话。
};
_Static_assert(sizeof(struct user_process) <= 1024, "exec control block must stay small"); // exec候选仍在内核栈上，大小不能随用户容量膨胀。
static struct user_process processes[PROCESS_COUNT]; // 槽可以复用，PID单调增长，不等于槽下标。
static struct user_execution workers[WORKER_COUNT]; // 额外执行链独占内核栈/FPU，用户页缓存仍归所属进程。
static struct user_process *current; // 当前执行者所属进程，系统调用从这里访问共享资源。
static struct user_execution *running; // 当前真实CPU现场，不能由共享PID推导成主执行者。
```

<a id="thread-scheduler"></a>
## 切换的是执行者，系统调用访问的是所属进程

current保留进程指针，新增running指出真实执行者。把“当前CPU状态一定是current.execution”的假设撤掉后，保存FS/FPU、记录frame、增加用户tick和等待条件都使用running；用户页、FD和异步句柄仍通过current访问。getpid返回共享pid，新42号调用返回running.tid。

execution_at把八个主execution和八个worker组合成十六个调度位置。execution_slot逐个用指针相等查位置，不对两个不同数组内的指针作减法或大小比较。槽号只用于轮转与固定栈布局，不能用持续增长的tid直接索引数组。

select_execution先确定execution.process，再为这条执行链设置TSS陷入栈、激活所属用户根并返回其frame。同组切换仍统一写CR3，当前没有加入减少TLB失效的优化。时钟IRQ依然只操作有界状态；需要设备推进或重试等待时，先切到长期启动服务栈。新主执行者在首次运行前绑定process和tid，工作者也在完整准备以后才发布RUNNABLE。

在原create_stack的位置加入执行槽映射、目标查找与忙检查，再以十六槽范围构造陷入栈。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
static bool create_stack(struct user_execution *execution, unsigned index) { // 每槽前面留一页保护页，所有栈共享已存在高半区分支。
    uint64_t address = STACK_BASE + index * 5 * 4096ull + 4096; // 四页数据与一页保护，PID增长不扩散地址。
    return airi_memory_region_create(&execution->stack, address, 4); // 完整成功后才借给用户根。
}
static struct user_page *owned_at(const struct user_memory *memory, unsigned index) { // 下标可等于count供新增项写入，但必须已有记录页。
```
<!-- after -->
```c
static struct user_execution *execution_at(unsigned index) { // 私有调用者只给0至EXECUTION_COUNT-1。
    return index < PROCESS_COUNT ? &processes[index].execution : &workers[index - PROCESS_COUNT]; // 两个固定数组组成一次调度轮转。
}
static unsigned execution_slot(const struct user_execution *execution) { // 不对不同数组中的指针作减法或大小比较。
    for (unsigned i = 0; i < EXECUTION_COUNT; ++i) // 指针相等可安全识别同一个真实对象。
        if (execution_at(i) == execution) // 候选局部对象不能交给调度器。
            return i; // 返回唯一槽号，不是TID。
    airi_panic("execution is outside scheduler slots"); // 调度内部归属错误必须停止。
}
static bool has_workers(const struct user_process *process) { // 已结束但未join的工作者仍占生命周期槽。
    for (unsigned i = 0; i < WORKER_COUNT; ++i) // 只检查额外执行者，不把主执行者算作忙。
        if (workers[i].process == process && workers[i].state != USER_UNUSED) // 包括ZOMBIE和等待安全回收者。
            return true; // 此阶段不允许同时替换或解除共享用户映射。
    return false; // 没有任何尚未收拢的工作者。
}
static struct user_execution *find_worker(const struct user_process *process, uint64_t tid) { // 工作身份必须属于调用者进程。
    for (unsigned i = 0; i < WORKER_COUNT; ++i) // PID数值相同也不能跨进程收取工作结果。
        if (workers[i].process == process && workers[i].tid == tid && workers[i].state != USER_UNUSED && workers[i].state != USER_REAP_PENDING) // 保留完整64位比较，不截断外来身份。
            return &workers[i]; // 借内核槽，用户只能收到返回值。
    return NULL; // 外进程、旧身份和重复join都没有有效目标。
}
static bool fd_waited(const struct user_process *process, uint64_t fd) { // 防止关闭再复用FD号码改变已经阻塞的操作。
    for (unsigned i = 0; i < EXECUTION_COUNT; ++i) { // 工作与主执行者可能都在读同一表。
        const struct user_execution *execution = execution_at(i); // 只观察状态，不接触用户buffer。
        if (execution->process == process && execution->state == USER_IO_WAIT && execution->io_fd == fd && (execution->io == IO_READ || execution->io == IO_WRITE || execution->io == IO_READY)) // 只保护确实借用该FD的等待。
            return true; // close返回忙，调用者可让等待先结束。
    }
    return false; // 无在途等待时沿既有FD引用规则关闭。
}
static bool create_stack(struct user_execution *execution, unsigned index) { // 每槽前面留一页保护页，所有栈共享已存在高半区分支。
    uint64_t address = STACK_BASE + index * 5 * 4096ull + 4096; // 四页数据与一页保护，PID增长不扩散地址。
    return airi_memory_region_create(&execution->stack, address, 4); // 完整成功后才借给用户根。
}
static struct user_page *owned_at(const struct user_memory *memory, unsigned index) { // 下标可等于count供新增项写入，但必须已有记录页。
```

用select_execution替换select_user，先确定所属进程，再选择独立CPU现场。

<!-- replace: kernel/process/user.c -->
<!-- before -->
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
<!-- after -->
```c
static struct airi_trap_frame *select_execution(struct user_execution *execution) { // 同时选择共享资源所有者与独立CPU现场。
    if (!execution->process) // 空执行槽不能拥有可运行资格。
        airi_panic("runnable execution has no process"); // 不能用旧PID猜一个资源表。
    current = execution->process; // FD、用户页与异步owner均从所属进程取得。
    running = execution; // FS/FPU/frame和等待条件使用本执行者。
    execution->state = USER_RUNNING; // 新目标即将获得CPU。
    airi_trap_set_stack(execution->stack.virtual_address + execution->stack.count * 4096ull); // TSS使用目标自己的内核栈。
    if (!airi_vm_activate(&current->memory.space)) // 同组切换可沿同一CR3，仍统一验证合法根。
        airi_panic("cannot activate selected user root"); // 不返回到缺映射的用户链。
    return airi_user_resume(execution->frame); // 返回原陷入位置或新工作入口。
}
```

<a id="thread-create-pages"></a>
## 先准备私有现场，再发布可运行资格

create_worker先确认调用者是主执行者，再检查桥接器entry和工作函数function在当前用户根中确实可执行。这里只验证用户RX映射，不验证函数签名或指令边界；程序仍负责使用匹配的long(void*)函数。参数argument只保存数值，内核不把它当指针解引用。

空工作槽取得四页内核栈和一页FPU状态，内核栈槽号为8加工作槽，避开原八条主陷入栈。创建者进入int80时已经保存真实FPU状态，新页复制这份512字节快照，因此继承创建者的浮点环境，之后状态页各自独立。这与TLS重新取得声明初值是两项不同规则；[POSIX创建语义](https://www.man7.org/linux/man-pages/man3/pthread_create.3p.html)也要求继承浮点环境，但本接口还不提供完整pthread合同。

工作用户栈从0x6e000000开始，每槽先留一页未映射保护，再映射十六页。工作TLS从USER_TLS加(index+1)*8192开始，每槽至多两页，主TLS仍在原基址。所有这些用户页继续占同一进程1024页预算；内核陷入栈和FPU页不计入该数据页数量。

join只归还工作者内核栈和FPU页，用户栈/TLS页保留为进程缓存，下次同槽复用前完整清零，再从原始ELF复制TLS初值并填写本工作者TCB。最多八槽，每槽十六页栈加至多两页TLS，缓存上限144数据页。工作返回以后即使映射还在，旧栈对象和本地对象的生命周期也已结束；程序不能因此继续使用旧借用。

创建前保存memory.count，失败时只解除本次在末尾登记的新页并归还数据，随后trim记录页和销毁未运行的execution。既有缓存、主对象和其他工作者不被回收。全部页面、FS、寄存器和所属进程准备好以后，才分配tid、place_frame并发布RUNNABLE；没有半个工作执行者能被IRQ选中。

在原airi_user_enter之前加入工作栈布局和创建路径；全部资源准备成功后才发布运行资格。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
void airi_user_enter(struct airi_trap_frame *f) { // 所有CS3来源在分派设备、timer或syscall之前共同保存。
```
<!-- after -->
```c
#define WORKER_STACK_BASE 0x6e000000ull // 额外用户栈独立于动态堆、主栈与TLS；每槽先留保护页。
static bool worker_code(uint64_t address) { // 入口必须是当前进程中真实可执行的用户映射。
    unsigned flags; // 从页表取得权限，不相信用户传入的函数类型。
    return address < AIRI_USER_LIMIT && airi_vm_lookup(&current->memory.space, address, NULL, &flags) && (flags & (AIRI_VM_USER | AIRI_VM_EXEC)) == (AIRI_VM_USER | AIRI_VM_EXEC); // 内核或仅RW数据地址均拒绝。
}
static bool worker_page(uint64_t address) { // 用户栈/TLS缓存只复用同一固定槽已有的RW/NX页面。
    unsigned flags; // 已有缓存必须保持预期权限。
    if (airi_vm_lookup(&current->memory.space, address, NULL, &flags)) // 该进程此前可能使用并join过这个槽。
        return flags == (AIRI_VM_USER | AIRI_VM_WRITE); // 不覆盖或升级未知映射。
    return owned_page(&current->memory, address, AIRI_VM_WRITE); // 新页面仍计入进程1024页预算。
}
static void worker_zero(uint64_t address, unsigned pages) { // 所有页已完整接入且没有活执行者借用这个缓存槽。
    for (unsigned i = 0; i < pages; ++i) { // 用户栈和TLS都逐页清零，不能继承上一次工作者的内容。
        uint64_t physical; // 使用HHDM写目标，不通过任意用户VA当内核裸指针。
        if (!airi_vm_lookup(&current->memory.space, address + i * 4096ull, &physical, NULL)) // 准备阶段已保证完整映射。
            airi_panic("prepared worker page is missing"); // 内部不变量失败不能发布执行资格。
        airi_memset(airi_memory_direct(physical), 0, 4096); // 每页起点已4096对齐，清完整缓存内容。
    }
}
static long create_worker(uint64_t entry, uint64_t function, uint64_t argument) { // 首版仅主执行者管理额外执行链。
    if (running != &current->execution) // 工作者不能递归创建，生命周期由主执行者统一收拢。
        return -16; // -EBUSY。
    if (!worker_code(entry) || !worker_code(function)) // 先验证两个真实代码地址，再分配任何内存。
        return -14; // -EFAULT，数据页或内核地址不能成为返回入口。
    if (next_pid == UINT32_MAX) // 与进程身份共享单调空间，不回绕重用旧TID。
        return -75; // -EOVERFLOW。
    unsigned index = 0; // 寻找全系统固定工作槽，不以TID索引数组。
    while (index < WORKER_COUNT && workers[index].state != USER_UNUSED) // 已结束但未join者仍占槽。
        ++index; // 只跳过仍有生命周期的执行者。
    if (index == WORKER_COUNT) // 容量不足不静默复制进程或复用未收结果。
        return -11; // -EAGAIN。
    struct user_execution *execution = &workers[index]; // 此槽尚未发布给调度器。
    unsigned before = current->memory.count; // 失败只撤销本次新增用户页，已有缓存与其他对象保留。
    if (!create_stack(execution, PROCESS_COUNT + index) || !create_fpu(execution)) // 内核栈与FPU均由本执行者独立持有。
        goto failed; // 任何成功的部分都沿同一回滚路径归还。
    airi_memcpy(airi_memory_direct(execution->fpu_physical), airi_memory_direct(running->fpu_physical), AIRI_FPU_BYTES); // 新工作者继承创建者已保存的浮点环境，但状态页不共享。
    uint64_t stack = WORKER_STACK_BASE + index * (AIRI_USER_STACK_PAGES + 1) * 4096ull + 4096; // 先跳过一页保护，再映射十六页栈。
    uint64_t tls = USER_TLS + (index + 1) * 8192ull; // 主TLS在原基址，每个工作槽最多两页且互不覆盖。
    for (unsigned i = 0; i < AIRI_USER_STACK_PAGES; ++i) // 用户栈页面归共享进程，而不是内核陷入栈区域。
        if (!worker_page(stack + i * 4096ull)) // 已有缓存可复用，页不足则回滚新建部分。
            goto failed; // 此时尚未修改任何活执行者的现场。
    for (unsigned i = 0; i < current->tls.pages; ++i) // 无TLS映像的页数为0，不强行建立本地块。
        if (!worker_page(tls + i * 4096ull)) // 每个工作者使用自己的运行副本。
            goto failed; // 不把主执行者修改后的数据复制成新初值。
    worker_zero(stack, AIRI_USER_STACK_PAGES); // 已完整准备的空闲槽从零用户栈开始。
    worker_zero(tls, current->tls.pages); // 清除tbss、填充和上一位工作者的内容。
    if (current->tls.memory_bytes) { // 初值来自内核只读ELF模板，不是主运行块。
        if (current->tls.file_bytes && !copy_user(current, (void *)(current->tls_image + current->tls.offset), tls + current->tls.data_offset, (size_t)current->tls.file_bytes, true)) // 布局已在装载时校验。
            airi_panic("cannot initialize prepared worker TLS"); // 映射不应在不可抢占准备中改变。
        execution->fs_base = tls + current->tls.pointer_offset; // TP仍按既有local-exec负偏移布局。
        uint64_t control[2] = {execution->fs_base, 0}; // 当前工作者自己的self指针与保留槽。
        if (!copy_user(current, control, execution->fs_base, sizeof(control), true)) // 只向已准备的TCB范围写入。
            airi_panic("cannot initialize worker TCB"); // 不发布半初始化FS。
    }
    execution->initial = (struct airi_trap_frame){0}; // 首次工作入口没有继承主执行者的GPR。
    execution->initial.rip = entry; // 用户库汇编桥接器负责调用真正工作函数和提交返回值。
    execution->initial.rdi = function; // 桥接器的第一个寄存器参数是函数地址。
    execution->initial.rsi = argument; // 第二个参数是不由内核解引用的用户数值。
    execution->initial.rsp = stack + AIRI_USER_STACK_PAGES * 4096ull - 16; // 桥接器call之前RSP按16对齐。
    execution->initial.cs = 27; // 当前用户代码段。
    execution->initial.ss = 35; // 当前用户栈段。
    execution->initial.rflags = 0x202; // 用户中断开启，无I/O特权。
    execution->process = current; // 共享既有内存、FD和异步owner，不打开第二份资源表。
    execution->tid = next_pid++; // 全部准备完成以后才发布新的执行身份。
    place_frame(execution); // 现场进入本执行者自己的内核栈。
    execution->state = USER_RUNNABLE; // 最后发布运行资格，IRQ此前只能看见空槽。
    airi_log("[THREAD_CREATE] pid=%u tid=%u slot=%u tp=0x%llx", current->pid, execution->tid, index, (unsigned long long)execution->fs_base); // 记录真实共享身份与本地寻址起点。
    return execution->tid; // 返回TID，仍不是pthread_t或Linux clone ABI。
failed: // 当前仍在主执行者栈上，可以立即收回尚未运行的工作者资源。
    while (current->memory.count > before) { // 只处理本次新增项，不能释放旧缓存页面。
        const struct user_page *page = owned_at(&current->memory, --current->memory.count); // 先退到最后一项有效所有权。
        if (!airi_vm_unmap(&current->memory.space, page->address) || !airi_memory_return_page(page->physical)) // 活动根先断映射，再归还数据页。
            airi_panic("cannot roll back worker user pages"); // 内部归属错误不得吞掉。
    }
    trim_records(&current->memory); // 新增但未使用的尾部记录页一并收回。
    destroy_execution(execution); // 栈/FPU的成功部分可独立归还，不关闭进程共享资源。
    return -12; // -ENOMEM，主执行者和其他已运行工作者保持。
}
void airi_user_enter(struct airi_trap_frame *f) { // 所有CS3来源在分派设备、timer或syscall之前共同保存。
```

<a id="thread-user-bridge"></a>
## 从内核给出的寄存器进入普通 C 函数

新建airi_threads.h与用户threads.c。create返回正TID或原始负错误，join返回0或负错误并可写一个long结果；它们不设置errno，也不使用pthread_t。库只把桥接器地址、函数地址和参数交给39号调用。40号是桥接器内部的工作结束入口，41号join，42号查询执行身份。

thread_call的int80使用既有RAX/RDI/RSI/RDX约定，memory clobber告诉编译器该调用边界可能访问内存；它不是互斥量，也不替代用户共享数据的C11同步。库与桥接器只链接到选择它们的用户ELF，USER_COMMON保持原样。

<!-- write: labs/airios/include/airi_threads.h -->
```c
#ifndef AIRI_THREADS_H // 自有有界共享执行接口，尚不是pthread。
#define AIRI_THREADS_H // 包含保护。
typedef long (*airi_thread_function)(void *argument); // 工作函数返回完整64位数值，由主执行者join取得。
#ifdef __cplusplus // 后续C++调用使用相同C链接名。
extern "C" { // 不要求宿主线程库。
#endif // C直接声明。
long airi_thread_create(airi_thread_function function, void *argument); // 仅主执行者创建，正TID或原始负错误，不修改errno。
long airi_thread_join(long tid, long *result); // 仅主执行者收取同组一次结果，NULL可丢弃，0成功或负错误。
long airi_thread_id(void); // 同进程PID相同，每个执行者的TID不同。
#ifdef __cplusplus // 结束链接语言范围。
} // 工作函数ABI仍为普通当前目标C调用。
#endif // 结束语言差异。
#endif // 结束包含保护。
```

<!-- write: labs/airios/libc/threads.c -->
```c
#include <airi_threads.h> // 不借pthread或Linux clone包装。
#include <stdint.h> // 指针以目标64位数值进入内核。
extern void airi_thread_entry(void); // 汇编桥接器被静态链接到当前用户可执行段。
static long thread_call(long number, uint64_t first, uint64_t second, uint64_t third) { // 与既有int80前三参数约定相同。
    __asm__ volatile("int $0x80" : "+a"(number) : "D"(first), "S"(second), "d"(third) : "memory", "cc"); // 内存clobber让调用边界不隐瞒共享读写。
    return number; // 负错误保持，不映射成尚未实现的pthread返回规则。
}
long airi_thread_create(airi_thread_function function, void *argument) { // 主执行者不复制自己的用户栈或TLS运行值。
    return thread_call(39, (uintptr_t)airi_thread_entry, (uintptr_t)function, (uintptr_t)argument); // 两个代码地址均由内核检查为用户RX。
}
long airi_thread_join(long tid, long *result) { // 内核在输出前核对完整long跨度。
    return thread_call(41, (uint64_t)tid, (uintptr_t)result, 0); // 未完成时调用链真正阻塞，不由库忙轮询。
}
long airi_thread_id(void) { // 只返回当前执行身份，不产生资源所有权。
    return thread_call(42, 0, 0, 0); // PID仍使用原始airi_getpid查询。
}
```

内核将新工作现场RIP设为airi_thread_entry，RDI放函数，RSI放参数，RSP在call之前16字节对齐。桥接器先把函数地址保存到RAX，再把真正参数移到RDI；否则覆盖RDI时会丢掉函数地址。GNU汇编的 `call *%rax` 表示跳到寄存器里的函数地址，同时压入8字节返回地址，普通C函数入口的RSP因而余8，满足当前ABI。

工作函数返回的long在RAX，桥接器移到RDI，再把RAX改成40进入内核。正常工作结束不返回旧桥接器，ud2只捕获意外返回。这个路径不使用新程序的argc/argv，也不调用用户_start或main。新建thread-entry.S。

<!-- write: labs/airios/thread-entry.S -->
```asm
.section .text /* 桥接器与工作函数都在用户RX段，不进入内核代码。 */
.global airi_thread_entry /* 用户库把该地址交给自有创建调用。 */
.type airi_thread_entry, @function /* 链接和调试工具知道这是一个函数入口。 */
airi_thread_entry: /* 内核给RDI函数、RSI参数，RSP在call前按16字节对齐。 */
    mov %rdi, %rax /* 先保存函数地址，下一步要复用RDI传真正参数。 */
    mov %rsi, %rdi /* 普通C工作函数的第一个参数使用RDI。 */
    call *%rax /* 间接调用压入8字节返回地址，满足C入口RSP余8的ABI。 */
    mov %rax, %rdi /* 工作函数返回的long完整交到线程返回调用参数。 */
    mov $40, %rax /* 自有工作者结束号，不是整个进程exit。 */
    int $0x80 /* 保留结果等主执行者join，不释放仍在使用的内核栈。 */
    ud2 /* 正常结束不会返回，异常返回形成明确用户故障。 */
.size airi_thread_entry, .-airi_thread_entry /* 调试符号范围不包含后面的非代码节。 */
.section .note.GNU-stack,"",@progbits /* 不申请可执行栈。 */
```

<a id="thread-join-lifetime"></a>
## join 等待结果，回收等待安全的栈

join先检查管理者身份、自等待、完整8字节输出和同进程目标。没有目标、旧TID或外进程TID返回−3，坏输出返回−14而不消费结果。目标已经结束时立即复制结果并标记REAP_PENDING；否则主执行者记录join_tid和输出地址，进入IO_JOIN等待。查状态与登记在同一不可抢占调用中完成，不会在两者之间睡过一次工作完成。

工作函数return后，桥接器执行40号调用，处理器转入这位工作者自己的陷入栈。内核把完整64位值保存到execution.result，标为ZOMBIE，再调度别人；进程FD与异步owner继续存在。此时结果已经形成，当前C调用却还没有离开工作者的内核栈。

主执行者若已在IO_JOIN等待，poll_waiters找到对应ZOMBIE以后，先复制结果，再撤销可join状态并唤醒主执行者。这次检查可能仍沿返回者的栈执行，所以这里只登记REAP_PENDING，不能顺手free那条栈。后续安全普通陷入或会话回收时，collect_pending确认目标不是running，才destroy_execution。

| 状态 | 能被调度 | 结果可join | 内核栈/FPU能否立即归还 |
| --- | --- | --- | --- |
| RUNNABLE/RUNNING | 是 | 尚未形成 | 否 |
| IO_WAIT | 否，条件满足后唤醒 | 尚未形成 | 否 |
| ZOMBIE | 否 | 是，一次 | 先保留到join |
| REAP_PENDING | 否 | 已消费 | 必须先离开目标内核栈 |
| UNUSED | 否 | 无 | 已归还，可重新创建 |

当前只有主执行者join，因而没有多个join者竞争、工作者间等待环或detach回收协议。进程退出则撤销整个组的运行资格，在离开组内栈后统一收拢所有工作者和共享内存。工作者CPU故障也结束所属进程，父进程仍可得到自有−142等故障编码；这不是Linux信号ABI。

接着在同一用户入口之前加入join_worker和return_worker，普通返回只结束工作者。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
void airi_user_enter(struct airi_trap_frame *f) { // 所有CS3来源在分派设备、timer或syscall之前共同保存。
```
<!-- after -->
```c
static struct airi_trap_frame *join_worker(struct airi_trap_frame *frame) { // 本轮只有主执行者能收取同组工作结果。
    if (running != &current->execution) // 不支持工作者之间的join图或自建管理树。
        frame->rax = (uint64_t)-16ll; // -EBUSY。
    else if (frame->rdi == running->tid) // 主执行者不能等待自己结束后再继续当前调用。
        frame->rax = (uint64_t)-22ll; // -EINVAL。
    else if (frame->rsi && !user_access(current, frame->rsi, sizeof(long), true)) // 完整结果输出在消费状态之前核对。
        frame->rax = (uint64_t)-14ll; // -EFAULT，目标仍可join。
    else { // 身份查找和等待登记在同一不可抢占内核调用中完成。
        struct user_execution *target = find_worker(current, frame->rdi); // 外进程或旧TID均不授权。
        if (!target) // 不返回别人的结果，也不重复收取。
            frame->rax = (uint64_t)-3ll; // -ESRCH。
        else if (target->state == USER_ZOMBIE) { // 结果已经形成，可以立即复制。
            if (frame->rsi && !copy_user(current, &target->result, frame->rsi, sizeof(target->result), true)) // 已验证，仍明确核对复制合同。
                airi_panic("validated join output changed"); // 当前没有并发页解除者。
            target->state = USER_REAP_PENDING; // 不在此处猜目标栈是否还被外层调用链使用。
            frame->rax = 0; // 本次结果只交出一次。
        } else { // 工作函数还在运行或等待I/O。
            running->join_tid = target->tid; // 以不可复用身份关联，不借用户槽指针。
            running->io = IO_JOIN; // 原系统调用在完成后原地继续。
            running->io_address = frame->rsi; // NULL表示只收拢，不读取工作结果。
            running->state = USER_IO_WAIT; // 条件完整后才撤销运行资格。
            ++io_block_count; // 统计真实阻塞，不用用户轮询模拟join。
            airi_log("[THREAD_JOIN_WAIT] pid=%u tid=%u", current->pid, target->tid); // 普通内核调用路径可输出。
            return schedule(REASON_IO); // 工作执行者继续，必要时进入PIT空闲等待。
        }
    }
    return airi_user_resume(frame); // 立即错误或已完成结果继续原调用方。
}
static struct airi_trap_frame *return_worker(struct airi_trap_frame *frame) { // 用户桥接器在工作函数return以后进入。
    if (running == &current->execution) { // 主执行者结束整个进程需使用既有exit接口。
        frame->rax = (uint64_t)-22ll; // 不把进程主入口静默变成工作者。
        return airi_user_resume(frame); // 原用户调用继续处理错误。
    }
    running->result = (long)frame->rdi; // 保留完整64位工作结果，不按进程exit的低八位截断。
    running->state = USER_ZOMBIE; // 结束运行，但结果和内核资源等join以后再收。
    airi_log("[THREAD_RETURN] pid=%u tid=%u user_ticks=%u", current->pid, running->tid, running->user_ticks); // 不关闭共享FD、异步owner或用户页。
    return schedule(REASON_EXIT); // 主执行者或别的工作者可继续，当前栈尚不能归还。
}
void airi_user_enter(struct airi_trap_frame *f) { // 所有CS3来源在分派设备、timer或syscall之前共同保存。
```

<a id="thread-boundaries"></a>
## 并发期间哪些资源操作明确返回忙

本轮不允许映像替换与共享映射解除同时发生。has_workers包含尚未join的ZOMBIE；已消费且安全回收的槽在普通陷入入口先清掉，最后一个join完成以后就可继续单执行者操作。

| 操作 | 当前规则 |
| --- | --- |
| create/join | 只由主执行者管理；工作者调用返回−16 |
| fork/exec/waitpid | 还有任何未收拢工作者时返回−16，原映像与资源保持 |
| airi_release | 工作执行者存在时返回−16；不在其他执行者阻塞期间解除其用户页 |
| close | 同组执行者正等该FD的read/write/ready时返回−16，防止关闭后号码复用改变在途目标 |
| 工作函数return | 只结束本工作者，保留进程资源与返回值 |
| airi_exit或用户CPU故障 | 结束整个进程组，其他进程继续 |

同组worker可以打开文件、读写、睡眠或使用同一异步owner。阻塞重试使用各execution自己的输出地址；CQ在pop前重查完整输出，避免先消费结果再发现输出无效。当前禁止并发解除映射、工作栈/TLS保留缓存，使这些等待期间的映射保持；用户仍需保证对象内容和生命周期，不得把正在使用的缓冲区当空闲对象复用。

FD关闭返回忙是当前有界策略，尚不是Linux关闭与在途I/O持有打开对象引用的完整行为。用户malloc也仍是K48的单线程分配器，多执行者不能无保护地同时调用它。本节不把这些限制隐藏成成功；后续futex、互斥量和兼容层要分别补足。

<a id="thread-kernel-source"></a>
## 同步进程退出、陷入与服务栈中的调用点

前面的结构、槽查找和工作接口已经接入，下面沿现有消费者补齐：poll_waiters遍历全部执行者，collect_pending安全归还worker，schedule和启动服务循环选择running，进程退出则收拢整组。fork复制原TLS模板描述，exec成功更新描述；两者仍按既有规则处理进程FD和异步epoch。桥接器内部40只走return_worker，原2号仍走整个进程exit_current。每组旧窗口定位这处连接，未列出的算法保持。

声明threads程序的只读映像边界。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
extern const uint8_t airi_process_elf_start[], airi_process_elf_end[]; // 新的fork/wait和抢占工作负载。
extern const uint8_t airi_vector_elf_start[], airi_vector_elf_end[]; // 显式偏移向量正常与坏范围观察。
extern const uint8_t airi_malloc_elf_start[], airi_malloc_elf_end[]; // 用户对象分配与页面缓存的独立ELF。
extern const uint8_t airi_posix_elf_start[], airi_posix_elf_end[]; // 显式链接errno与POSIX风格文件包装。
extern const uint8_t airi_ctls_elf_start[], airi_ctls_elf_end[]; // 编译器_Thread_local与静态模板。
extern const uint8_t airi_ctls_char_elf_start[], airi_ctls_char_elf_end[]; // 单字节小对齐模板。
```
<!-- after -->
```c
extern const uint8_t airi_process_elf_start[], airi_process_elf_end[]; // 新的fork/wait和抢占工作负载。
extern const uint8_t airi_vector_elf_start[], airi_vector_elf_end[]; // 显式偏移向量正常与坏范围观察。
extern const uint8_t airi_malloc_elf_start[], airi_malloc_elf_end[]; // 用户对象分配与页面缓存的独立ELF。
extern const uint8_t airi_threads_elf_start[], airi_threads_elf_end[]; // 同进程共享内存、FD与异步owner的真实工作执行者。
extern const uint8_t airi_posix_elf_start[], airi_posix_elf_end[]; // 显式链接errno与POSIX风格文件包装。
extern const uint8_t airi_ctls_elf_start[], airi_ctls_elf_end[]; // 编译器_Thread_local与静态模板。
extern const uint8_t airi_ctls_char_elf_start[], airi_ctls_char_elf_end[]; // 单字节小对齐模板。
```

在初始目录中注册threads，保留ctls-tail与其他既有名称。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    {"vector", airi_vector_elf_start, airi_vector_elf_end}, // 聚合、分散、短前缀与错误原子性。
    {"heap", airi_malloc_elf_start, airi_malloc_elf_end}, // 单线程用户对象与错误路径。
    {"heap-reset", airi_malloc_elf_start, airi_malloc_elf_end}, // 同一ELF新BSS和按需arena。
    {"libc", airi_posix_elf_start, airi_posix_elf_end}, // 用户库错误与文件结果转换。
    {"libc-reset", airi_posix_elf_start, airi_posix_elf_end}, // 同一ELF新错误槽和CLOEXEC观察。
    {"ctls", airi_ctls_elf_start, airi_ctls_elf_end}, // 自动建立静态TLS后运行普通C。
```
<!-- after -->
```c
    {"vector", airi_vector_elf_start, airi_vector_elf_end}, // 聚合、分散、短前缀与错误原子性。
    {"heap", airi_malloc_elf_start, airi_malloc_elf_end}, // 单线程用户对象与错误路径。
    {"heap-reset", airi_malloc_elf_start, airi_malloc_elf_end}, // 同一ELF新BSS和按需arena。
    {"threads", airi_threads_elf_start, airi_threads_elf_end}, // 独立TLS/CPU状态、join与整个进程退出。
    {"libc", airi_posix_elf_start, airi_posix_elf_end}, // 用户库错误与文件结果转换。
    {"libc-reset", airi_posix_elf_start, airi_posix_elf_end}, // 同一ELF新错误槽和CLOEXEC观察。
    {"ctls", airi_ctls_elf_start, airi_ctls_elf_end}, // 自动建立静态TLS后运行普通C。
```

FPU成功构造后的清理责任按执行者表述，分配流程保持。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    void *state = airi_memory_direct(execution->fpu_physical); // 内核HHDM别名，不给用户USER映射。
    airi_memset(state, 0, 4096); // 剩余页字节也不带旧内容。
    airi_fpu_new(state); // 只复制已验证模板，不改变当前用户硬件状态。
    return true; // 页句柄此后由进程清理者持有。
}
static bool user_access(const struct user_process *p, uint64_t address, size_t bytes, bool write) { // 检查完整用户范围，不直接解引用它。
    if (address >= AIRI_USER_LIMIT || bytes > AIRI_USER_LIMIT - address) // 排除内核地址与右端溢出。
```
<!-- after -->
```c
    void *state = airi_memory_direct(execution->fpu_physical); // 内核HHDM别名，不给用户USER映射。
    airi_memset(state, 0, 4096); // 剩余页字节也不带旧内容。
    airi_fpu_new(state); // 只复制已验证模板，不改变当前用户硬件状态。
    return true; // 页句柄此后由执行者持有，离开该执行链后才能归还。
}
static bool user_access(const struct user_process *p, uint64_t address, size_t bytes, bool write) { // 检查完整用户范围，不直接解引用它。
    if (address >= AIRI_USER_LIMIT || bytes > AIRI_USER_LIMIT - address) // 排除内核地址与右端溢出。
```

同步文件复制继续依靠当前不可抢占调用与禁止并发解除映射的条件。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
        return (long)received; // 实际取得的字节数。
    }
    long result = airi_fd_read(&p->files, (unsigned)fd, buffer, bytes, positioned, offset); // EOF和暂不可读分开。
    if (result > 0 && !copy_user(p, buffer, address, (size_t)result, true)) // 目标阻塞且单线程，已检查映射不应变化。
        airi_panic("validated file output mapping changed"); // 内部错误不丢字节后假装成功。
    return result; // 完成前缀或明确负错误。
}
```
<!-- after -->
```c
        return (long)received; // 实际取得的字节数。
    }
    long result = airi_fd_read(&p->files, (unsigned)fd, buffer, bytes, positioned, offset); // EOF和暂不可读分开。
    if (result > 0 && !copy_user(p, buffer, address, (size_t)result, true)) // 同一次不可抢占重试已经检查映射，本次复制前不能改变。
        airi_panic("validated file output mapping changed"); // 内部错误不丢字节后假装成功。
    return result; // 完成前缀或明确负错误。
}
```

向量副本同样在本次不可抢占调用中完成，局部缓冲不跨等待保存。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    if (write) // preadv不读取目标用户缓冲内容。
        for (unsigned i = 0; i < count && copied < bytes; ++i) { // 只聚合本次已验证前缀。
            size_t part = vectors[i].bytes > bytes - copied ? bytes - copied : (size_t)vectors[i].bytes; // 当前项实际长度。
            if (part && !copy_user(p, buffer + copied, vectors[i].address, part, false)) // 映射在不可抢占、单线程内核中保持。
                airi_panic("validated vector input changed"); // 不变量破坏不能交出半份文件操作。
            copied += part; // 下一项紧随上一个实际字节。
        }
```
<!-- after -->
```c
    if (write) // preadv不读取目标用户缓冲内容。
        for (unsigned i = 0; i < count && copied < bytes; ++i) { // 只聚合本次已验证前缀。
            size_t part = vectors[i].bytes > bytes - copied ? bytes - copied : (size_t)vectors[i].bytes; // 当前项实际长度。
            if (part && !copy_user(p, buffer + copied, vectors[i].address, part, false)) // 映射在当前单CPU不可抢占内核调用中保持。
                airi_panic("validated vector input changed"); // 不变量破坏不能交出半份文件操作。
            copied += part; // 下一项紧随上一个实际字节。
        }
```

poll_waiters改遍历全部执行者，按其process检查用户结果，并接入IO_JOIN的一次结果交付。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    return result; // 实际短数量、EOF0或原始负错误，不伪造全部完成。
}
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
<!-- after -->
```c
    return result; // 实际短数量、EOF0或原始负错误，不伪造全部完成。
}
static void poll_waiters(void) { // 仅普通内核上下文运行，IRQ本身不复制、分配或打印。
    for (unsigned i = 0; i < EXECUTION_COUNT; ++i) { // 有界等待队列覆盖所有主/工作执行者。
        struct user_execution *execution = execution_at(i); // 等待记录属于执行链。
        struct user_process *p = execution->process; // 用户映射和FD仍归共享进程。
        if (execution->state != USER_IO_WAIT) // 不碰正在运行、wait孩子或已退出者。
            continue; // 检查下一个等待槽。
        long result; // 恢复到用户同一次调用的结果。
        if (execution->io == IO_JOIN) { // 只有主执行者能登记，目标不会被其他工作者收取。
            struct user_execution *target = find_worker(p, execution->join_tid); // 主执行者保留完整等待身份。
            if (!target) // 目标内部丢失时向调用者报告，不解引用失效槽。
                result = -3; // -ESRCH。
            else if (target->state != USER_ZOMBIE) // 工作函数还没有结束。
                continue; // 主执行者仍然阻塞，其他工作者可继续。
            else if (execution->io_address && !copy_user(p, &target->result, execution->io_address, sizeof(target->result), true)) // 先形成用户输出，再撤销目标的可join状态。
                result = -14; // -EFAULT，目标仍保留ZOMBIE供重试。
            else { // 结果已完整交出，不能重复join。
                target->state = USER_REAP_PENDING; // 此时可能仍使用目标内核栈，只登记延后回收。
                result = 0; // 成功取得一次工作返回值。
            }
        } else if (execution->io == IO_SLEEP) { // 只有截止tick，没有用户缓冲或设备FD。
            if (airi_timer_ticks() < execution->io_deadline) // 时钟尚未到期，保留原现场等待。
                continue; // 不恢复用户忙轮询。
            result = 0; // 到期后同一次sleep系统调用成功返回。
        } else if (execution->io == IO_ASYNC) { // 原CQ等待仍保留同一个用户输出地址。
            struct airi_io_completion completion; // 先取得内核数值副本，不借用户指针给引擎。
            result = user_access(p, execution->io_address, sizeof(completion), true) ? airi_async_pop(p->pid, &completion) : -14; // 空CQ为-11，不代表I/O结果。
            if (result == -11) // 尚未形成可收集结果。
                continue; // 设备由启动服务栈推进，不在这里忙等。
            if (!result && !copy_user(p, &completion, execution->io_address, sizeof(completion), true)) // 本轮pop前已检查完整输出，当前不可抢占调用中映射保持。
                airi_panic("validated completion output mapping changed"); // 不能消费CQ后静默丢结果。
        } else if (execution->io == IO_READY) { // 订阅的是条件，不是一次成功read的承诺。
            result = airi_fd_ready(&p->files, execution->io_fd); // 当前条件可能被其他进程消费或改变。
            if (result >= 0) // 负错误必须原样唤醒，不能按位筛掉。
                result &= execution->io_events | AIRI_READY_HUP | AIRI_READY_ERROR; // 对端关闭即使未显式请求也应可见。
            if (!result && airi_timer_ticks() < execution->io_deadline) // 无条件成立且尚未超时。
                continue; // 保持等待，之后由状态变化或PIT服务重查。
        } else { // 阻塞read/write重新尝试同一个有界请求。
            result = file_transfer(p, execution->io_fd, execution->io_address, (size_t)execution->io_bytes, execution->io == IO_WRITE, false, 0); // 用户buffer仍由p持有。
            if (result == -11) // 暂时依旧没有进展。
                continue; // 不把原系统调用提前返回给用户忙等。
        }
        execution->frame->rax = (uint64_t)result; // 用户返回位置仍在原调用之后。
        execution->join_tid = 0; // 这一轮join身份不再作为等待条件。
        execution->io = IO_NONE; // 这一次等待不再持有请求地址。
        execution->io_address = execution->io_bytes = execution->io_deadline = 0; // 清过期的用户对象与截止值。
        execution->state = USER_RUNNABLE; // 结果就绪之后才发布运行资格。
        ++io_wake_count; // 实际唤醒，不把查询次数计成完成。
        airi_log("[WAIT_WAKE] pid=%u result=%ld", p->pid, result); // 普通内核服务栈输出，不在IRQ里打印。
    }
```

整进程cleanup先收拢本组worker的执行资源，再处理主execution与共享memory。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
        p->async_owned = false; // 重复清理不能再关同一轮次。
    }
    airi_fd_close_all(&p->files, false); // 整进程清理才关闭全部表项，exec不调用此路径清旧内存。
    destroy_memory(&p->memory); // 只销毁用户映射和数据，不能触及本进程的执行栈。
    destroy_execution(&p->execution); // CPU状态页和陷入栈分别归执行者，最后统一撤掉恢复资格。
    airi_memset(p, 0, sizeof(*p)); // 不留下可重复释放的身份或其他句柄。
}
```
<!-- after -->
```c
        p->async_owned = false; // 重复清理不能再关同一轮次。
    }
    airi_fd_close_all(&p->files, false); // 整进程清理才关闭全部表项，exec不调用此路径清旧内存。
    for (unsigned i = 0; i < WORKER_COUNT; ++i) // 退出/失败整组清理只归还本组拥有的额外执行链。
        if (workers[i].process == p) // 空槽或其他进程的执行者不能被误收。
            destroy_execution(&workers[i]); // 调用者已离开目标组的全部内核栈。
    destroy_memory(&p->memory); // 用户栈/TLS缓存页面也随进程内存一次归还。
    destroy_execution(&p->execution); // CPU状态页和陷入栈分别归执行者，最后统一撤掉恢复资格。
    airi_memset(p, 0, sizeof(*p)); // 不留下可重复释放的身份或其他句柄。
}
```

ELF准备成功时保存已校验TLS布局与长期只读映像，供以后每个worker从声明初值建立副本。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
            goto failed; // 未完整建立TP不发布可运行状态。
        airi_log("[ELF_TLS] bytes=%llu initialized=%llu alignment=%llu pages=%u data=0x%llx tp=0x%llx", (unsigned long long)view.tls.memory_bytes, (unsigned long long)view.tls.file_bytes, (unsigned long long)view.tls.alignment, view.tls.pages, (unsigned long long)data, (unsigned long long)p->execution.fs_base); // 模板与运行位置分别可观察。
    }
    initial_frame(p, view.entry, name); // 用户_start接收最小argc/argv协议。
    airi_log("[ELF_LOAD] entry=0x%llx segments=%u data_pages=%u argc=1", (unsigned long long)view.entry, view.count, p->memory.count); // 不含页表与内核栈的用户数据页数量。
    return 0; // 只准备新空间，尚未切CR3或覆盖旧现场。
```
<!-- after -->
```c
            goto failed; // 未完整建立TP不发布可运行状态。
        airi_log("[ELF_TLS] bytes=%llu initialized=%llu alignment=%llu pages=%u data=0x%llx tp=0x%llx", (unsigned long long)view.tls.memory_bytes, (unsigned long long)view.tls.file_bytes, (unsigned long long)view.tls.alignment, view.tls.pages, (unsigned long long)data, (unsigned long long)p->execution.fs_base); // 模板与运行位置分别可观察。
    }
    p->tls = view.tls; // 保存已校验模板布局，不能从后来修改过的主TLS复制初值。
    p->tls_image = view.image; // 初始ELF字节由内核只读目录长期持有。
    initial_frame(p, view.entry, name); // 用户_start接收最小argc/argv协议。
    airi_log("[ELF_LOAD] entry=0x%llx segments=%u data_pages=%u argc=1", (unsigned long long)view.entry, view.count, p->memory.count); // 不含页表与内核栈的用户数据页数量。
    return 0; // 只准备新空间，尚未切CR3或覆盖旧现场。
```

collect_pending确认目标不是running以后，才回收已经join的工作内核栈与FPU。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    return NULL; // 表满返回明确失败。
}
static void collect_pending(void) { // 在其他用户陷入栈或启动服务栈上回收，跳过仍在使用的当前进程栈。
    for (unsigned i = 0; i < PROCESS_COUNT; ++i) // 遍历已取得延后回收资格的进程，包括用户 wait 后的孩子与内核托管孤儿。
        if (&processes[i] != current && processes[i].execution.state == USER_REAP_PENDING) { // 当前真正运行的内核栈不能在此回收。
            unsigned pid = processes[i].pid; // cleanup清零控制块前保存诊断身份。
```
<!-- after -->
```c
    return NULL; // 表满返回明确失败。
}
static void collect_pending(void) { // 在其他用户陷入栈或启动服务栈上回收，跳过仍在使用的当前进程栈。
    for (unsigned i = 0; i < WORKER_COUNT; ++i) // 先收拢已经join且不在当前栈上的工作者。
        if (&workers[i] != running && workers[i].state == USER_REAP_PENDING) // 退出的当前内核调用链尚不能归还。
            destroy_execution(&workers[i]); // 用户栈/TLS映射缓存继续由进程拥有。
    for (unsigned i = 0; i < PROCESS_COUNT; ++i) // 遍历已取得延后回收资格的进程，包括用户 wait 后的孩子与内核托管孤儿。
        if (&processes[i] != current && processes[i].execution.state == USER_REAP_PENDING) { // 当前真正运行的内核栈不能在此回收。
            unsigned pid = processes[i].pid; // cleanup清零控制块前保存诊断身份。
```

schedule在十六个执行槽中轮转，普通服务栈继续推进等待，current随目标process选择。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    service_from_timer = from_timer; // 真正IRQ导致的换身份计数另记。
    airi_memory_use_kernel(); // 启动服务不依赖某个用户根。
    kernel_return->rax = 0; // 暂停结果不当成应用最终退出值。
    return kernel_return; // 汇编离开当前进程栈，之后才能安全服务其他等待者。
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
<!-- after -->
```c
    service_from_timer = from_timer; // 真正IRQ导致的换身份计数另记。
    airi_memory_use_kernel(); // 启动服务不依赖某个用户根。
    kernel_return->rax = 0; // 暂停结果不当成应用最终退出值。
    return kernel_return; // 汇编离开当前执行者栈，之后才能安全服务其他等待者。
}
static struct airi_trap_frame *schedule(enum schedule_reason reason) { // 当前完整frame边界可切换，但IRQ不直接执行复杂I/O。
    struct user_execution *old = running; // 仍暂用old的内核栈。
    if (old->state == USER_RUNNING) // 等待或退出不能误改回可运行。
        old->state = USER_RUNNABLE; // 时间片结束仍可运行。
    if (reason != REASON_TIMER) // 普通内核边界可重试等待条件。
        poll_waiters(); // 已有字节、关闭或错误形成后再唤醒。
    else { // IRQ只检查状态，并把设备推进交给启动服务栈。
        bool service = airi_async_work(); // 即使无人等待，已接收请求也不能被纯计算用户饿死。
        for (unsigned i = 0; i < EXECUTION_COUNT; ++i) // 管道、有限条件与CQ等待都需要普通服务。
            service |= execution_at(i)->state == USER_IO_WAIT; // 不在IRQ里复制、分配或打印。
        if (service) // 完整用户frame已经保存。
            return pause_for_service(true); // 由启动栈pump与重查，再进入下一个用户。
    }
    for (unsigned n = 1; n <= EXECUTION_COUNT; ++n) { // 从old之后轮转一圈。
        struct user_execution *next = execution_at((execution_slot(old) + n) % EXECUTION_COUNT); // PID与槽仍分开。
        if (next->state == USER_RUNNABLE) { // 仅可运行者有可返回的用户现场。
            if (next != old && reason == REASON_TIMER) // 真IRQ切不同身份。
                ++timer_switches; // 主动阻塞不冒充抢占。
            return select_execution(next); // 汇编恢复目标frame。
        }
    }
    for (unsigned i = 0; i < EXECUTION_COUNT; ++i) // 没有可运行者时检查仍在等待的活进程。
        if (execution_at(i)->state == USER_WAITING || execution_at(i)->state == USER_IO_WAIT) // 可能正在等设备、超时或未来对端。
            return pause_for_service(false); // 允许启动栈hlt等待真实IRQ，不PANIC用户死锁。
    session_active = false; // 所有用户均结束，此时才算完整会话退出。
    session_idle = false; // 不再保留暂停身份。
```

exit_current先终止整组执行资格，保存真正故障/退出结果，离开组内栈以后才清共享资源。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    kernel_return->rax = (uint64_t)(int64_t)session_result; // 根用户的真实退出结果。
    return kernel_return; // 回启动栈后统一回收残留资源。
}
static struct airi_trap_frame *exit_current(int code) { // CPU还在当前进程内核栈上，先结束身份而不释放栈。
    struct user_process *p = current; // 当前退出者的稳定控制块。
    if (p->async_owned) { // 用户身份结束，不能把旧CQ交给之后复用的进程槽。
        airi_async_close(p->pid); // QUEUED撤销，ACTIVE内核副本保持到收集。
        p->async_owned = false; // cleanup不重复关闭未来的新epoch。
    }
    if (input_pid == p->pid) // 当前前台shell停止后，init的新孩子才能取得UART。
        input_pid = 0; // 只释放本进程的消费权，不替其他身份撤销。
    airi_fd_close_all(&p->files, false); // exit立即释放文件，僵尸只保留等待状态与待回收内存。
    p->exit_code = code; // 保存给父进程wait读取。
    p->execution.state = USER_ZOMBIE; // 不再运行，但资源和状态尚存在。
    if (p->pid == root_pid) // 整个会话的结果以最初根用户为准。
        session_result = code; // 孩子退出不改变根结果。
```
<!-- after -->
```c
    kernel_return->rax = (uint64_t)(int64_t)session_result; // 根用户的真实退出结果。
    return kernel_return; // 回启动栈后统一回收残留资源。
}
static struct airi_trap_frame *exit_current(int code) { // CPU还在当前执行者内核栈上，先结束整个进程而不释放当前栈。
    struct user_process *p = current; // 当前退出者的稳定控制块。
    if (p->async_owned) { // 用户身份结束，不能把旧CQ交给之后复用的进程槽。
        airi_async_close(p->pid); // QUEUED撤销，ACTIVE内核副本保持到收集。
        p->async_owned = false; // cleanup不重复关闭未来的新epoch。
    }
    if (input_pid == p->pid) // 当前前台shell停止后，init的新孩子才能取得UART。
        input_pid = 0; // 只释放本进程的消费权，不替其他身份撤销。
    airi_fd_close_all(&p->files, false); // exit立即释放文件，僵尸只保留等待状态与待回收内存。
    p->exit_code = code; // 保存给父进程wait读取，工作者的普通return不走这里。
    p->execution.fault_error = running->fault_error; // 工作执行者的CPU故障同样形成整个进程的退出证据。
    for (unsigned i = 0; i < WORKER_COUNT; ++i) // 进程退出终止所有工作者，不再恢复它们的等待或用户入口。
        if (workers[i].process == p) // 包括当前真正执行exit的工作者。
            workers[i].state = USER_REAP_PENDING; // 此刻只撤销运行资格，离开目标组栈后才真正归还。
    p->execution.state = USER_ZOMBIE; // 不再运行，但资源和状态尚存在。
    if (p->pid == root_pid) // 整个会话的结果以最初根用户为准。
        session_result = code; // 孩子退出不改变根结果。
```

fork在有worker时返回忙；允许复制时，为新主execution绑定自己的process和TID，并保存TLS模板。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    return schedule(REASON_EXIT); // 子进程或父进程可继续，未必立刻回启动栈。
}
static long fork_current(struct airi_trap_frame *parent_frame) { // 首版逐页复制，不实现COW。
    if (next_pid == UINT32_MAX) // 固定32位正PID不允许回绕命中旧身份。
        return -75; // -EOVERFLOW。
    struct user_process *parent = current; // 父的用户数据与完整返回现场。
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
```
<!-- after -->
```c
    return schedule(REASON_EXIT); // 子进程或父进程可继续，未必立刻回启动栈。
}
static long fork_current(struct airi_trap_frame *parent_frame) { // 首版逐页复制，不实现COW。
    if (has_workers(current)) // 包括未join的结束者，先收拢共享执行链再改变进程映像或页映射。
        return -16; // -EBUSY，当前共享资源保持。
    if (next_pid == UINT32_MAX) // 固定32位正PID不允许回绕命中旧身份。
        return -75; // -EOVERFLOW。
    struct user_process *parent = current; // 父的用户数据与完整返回现场。
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
    child->execution.process = child; // 主执行者借自己的进程资源。
    child->execution.tid = child->pid; // 初始执行者的TID与PID数值相同。
    child->tls = parent->tls; // fork复制用户TLS当前值，后续新工作者仍从原ELF模板建立。
    child->tls_image = parent->tls_image; // 只读输入字节由内核目录长期持有。
    child->parent = parent->pid; // 关系用身份而不是可能复用的槽指针表示。
    if (airi_async_open(child->pid)) // fork创建空上下文，用户内存里的父handle整数并不授权子访问。
        goto failed; // 其余子资源统一回滚，父不变。
```

waitpid同样先拒绝未收拢worker，等待条件转存running，进程父子关系仍按current判断。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    return -12; // -ENOMEM，父的地址空间和返回点不变。
}
static struct airi_trap_frame *wait_current(struct airi_trap_frame *f) { // 自有wait ABI返回PID，status写普通int而非Linux编码。
    int64_t target = (int64_t)f->rdi; // -1表示任意孩子，其他必须为正PID。
    uint64_t status = f->rsi; // NULL允许只回收不读取退出码。
    if ((target < 1 && target != -1) || (status && !user_access(current, status, sizeof(int), true))) { // 输出范围在阻塞之前就核对。
        f->rax = (target < 1 && target != -1) ? (uint64_t)-22ll : (uint64_t)-14ll; // 参数错误不改变调度状态。
        return airi_user_resume(f); // 当前进程仍运行，可换参数重试。
    }
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
<!-- after -->
```c
    return -12; // -ENOMEM，父的地址空间和返回点不变。
}
static struct airi_trap_frame *wait_current(struct airi_trap_frame *f) { // 自有wait ABI返回PID，status写普通int而非Linux编码。
    if (has_workers(current)) { // 本轮仅主执行者join工作者，不混入并发进程wait结果借用。
        f->rax = (uint64_t)-16ll; // -EBUSY，调用方可先join再waitpid。
        return airi_user_resume(f); // 原用户调用链继续。
    }
    int64_t target = (int64_t)f->rdi; // -1表示任意孩子，其他必须为正PID。
    uint64_t status = f->rsi; // NULL允许只回收不读取退出码。
    if ((target < 1 && target != -1) || (status && !user_access(current, status, sizeof(int), true))) { // 输出范围在阻塞之前就核对。
        f->rax = (target < 1 && target != -1) ? (uint64_t)-22ll : (uint64_t)-14ll; // 参数错误不改变调度状态。
        return airi_user_resume(f); // 当前进程仍运行，可换参数重试。
    }
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
    running->wait_target = target; // 保存要等待的身份条件。
    running->wait_status = status; // 已拒绝工作者并发，父阻塞期间不会由同组执行者改变映射。
    running->state = USER_WAITING; // 先记录条件与结果地址，再让调度器跳过父。
    ++wait_blocks; // 真实阻塞，而非轮询返回成功。
    return schedule(REASON_WAIT); // 孩子结束时填写此f->rax并唤醒父。
}
```

动态释放先确认本组没有worker，再按原记录与映射顺序归还页面；查找下标不跨用户执行保存。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
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
```
<!-- after -->
```c
    trim_records(&current->memory); // 本批回滚后，尾部没有活项的记录页也一起归还。
    return -12; // 不返回半段用户范围。
}
static unsigned owned_index(uint64_t address) { // 只在当前进程的不可抢占系统调用中查询，不跨用户执行保留下标。
    for (unsigned i = 0; i < current->memory.count; ++i) // 有效项之外可能有旧字节，不参与查找。
        if (owned_at(&current->memory, i)->address == address) // 精确页起点，不接受页内指针。
            return i; // 下标只借给本次操作，不跨状态修改保存。
    return current->memory.count; // count本身是未找到哨兵，不能作为数组下标解引用。
}
static long release_user(uint64_t address, uint64_t bytes) { // 按页解除动态用户映射，不是C malloc对象free。
    if (has_workers(current)) // 包括未join的结束者，先收拢共享执行链再改变进程映像或页映射。
        return -16; // -EBUSY，当前共享资源保持。
    if (!bytes || bytes > AIRI_USER_REQUEST_PAGES * 4096ull || (bytes & 4095) || (address & 4095) || address < USER_HEAP || address >= USER_HEAP_END || bytes > USER_HEAP_END - address) // 短路先限地址再做右端减法。
        return -22; // -EINVAL；不舍入无效长度，不动任何页。
    unsigned count = (unsigned)(bytes / 4096); // 参数已限制为一到六十四个完整页。
```

exec先拒绝活跃或未join的工作者，提交新TLS模板与内存时只更新主running的CPU状态和旧等待借用。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    return 0; // 后续分配可复用空洞，新物理页按原owned_page逻辑清零。
}
static long exec_current(struct airi_trap_frame *f) { // 成功不返回旧用户调用，失败必须保持旧PID、空间和现场。
    char name[64]; // 先把用户名称复制到当前内核栈，后续不再借旧用户页。
    bool terminated = false; // 不能把未终止输入交给strlen。
    for (unsigned i = 0; i < sizeof(name); ++i) { // 有界逐字节读取，允许跨用户页但不越过上限。
        if (f->rdi > UINT64_MAX - i || !copy_user(current, &name[i], f->rdi + i, 1, false)) // 名称本身也可能是坏指针。
            return -14; // -EFAULT，旧用户继续。
        if (!name[i]) { // 取得完整NUL字符串后才查目录。
            terminated = true; // 后续strlen有明确终止界限。
            break; // 不读取字符串后无关字节。
        }
    }
    if (!terminated) // 超过本项目名称预算。
        return -36; // -ENAMETOOLONG。
    const struct executable *image = find_program(name); // 不存在目标不用分配新根。
    if (!image) // 和损坏格式是两种不同失败。
        return -2; // -ENOENT。
    bool acquire_input = supervisor_pid && current->parent == supervisor_pid && airi_strlen(name) == 2 && !airi_memcmp(name, "sh", 2); // 只有init的直接shell孩子可获得新前台输入。
    if (acquire_input && input_pid && input_pid != current->pid) // 同一个init也不能同时授权两个读者。
        return -16; // -EBUSY；尚未触碰旧映像或FD。
    struct user_process next = {0}; // 临时所有者不在进程表中，尚不能被调度。
    int error = prepare_elf(&next, image->start, (size_t)((uintptr_t)image->end - (uintptr_t)image->start), name, false); // 不创建第二条内核栈，先完整准备新用户空间。
    if (error) // 构造器已回滚next，current所有字段和旧f不变。
        return error; // -ENOEXEC或-ENOMEM。
    int async_error = airi_async_reset(current->pid); // 新ELF完整后，提交前确认可以进入新资源epoch。
    if (async_error) { // reset失败发生在撤销前，旧映像与旧I/O资源仍有效。
        cleanup(&next); // next尚未拥有当前PID的owner。
        return async_error; // 原用户调用可继续处理失败。
    }
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
<!-- after -->
```c
    return 0; // 后续分配可复用空洞，新物理页按原owned_page逻辑清零。
}
static long exec_current(struct airi_trap_frame *f) { // 成功不返回旧用户调用，失败必须保持旧PID、空间和现场。
    if (has_workers(current)) // 包括未join的结束者，先收拢共享执行链再改变进程映像或页映射。
        return -16; // -EBUSY，当前共享资源保持。
    char name[64]; // 先把用户名称复制到当前内核栈，后续不再借旧用户页。
    bool terminated = false; // 不能把未终止输入交给strlen。
    for (unsigned i = 0; i < sizeof(name); ++i) { // 有界逐字节读取，允许跨用户页但不越过上限。
        if (f->rdi > UINT64_MAX - i || !copy_user(current, &name[i], f->rdi + i, 1, false)) // 名称本身也可能是坏指针。
            return -14; // -EFAULT，旧用户继续。
        if (!name[i]) { // 取得完整NUL字符串后才查目录。
            terminated = true; // 后续strlen有明确终止界限。
            break; // 不读取字符串后无关字节。
        }
    }
    if (!terminated) // 超过本项目名称预算。
        return -36; // -ENAMETOOLONG。
    const struct executable *image = find_program(name); // 不存在目标不用分配新根。
    if (!image) // 和损坏格式是两种不同失败。
        return -2; // -ENOENT。
    bool acquire_input = supervisor_pid && current->parent == supervisor_pid && airi_strlen(name) == 2 && !airi_memcmp(name, "sh", 2); // 只有init的直接shell孩子可获得新前台输入。
    if (acquire_input && input_pid && input_pid != current->pid) // 同一个init也不能同时授权两个读者。
        return -16; // -EBUSY；尚未触碰旧映像或FD。
    struct user_process next = {0}; // 临时所有者不在进程表中，尚不能被调度。
    int error = prepare_elf(&next, image->start, (size_t)((uintptr_t)image->end - (uintptr_t)image->start), name, false); // 不创建第二条内核栈，先完整准备新用户空间。
    if (error) // 构造器已回滚next，current所有字段和旧f不变。
        return error; // -ENOEXEC或-ENOMEM。
    int async_error = airi_async_reset(current->pid); // 新ELF完整后，提交前确认可以进入新资源epoch。
    if (async_error) { // reset失败发生在撤销前，旧映像与旧I/O资源仍有效。
        cleanup(&next); // next尚未拥有当前PID的owner。
        return async_error; // 原用户调用可继续处理失败。
    }
    unsigned old_pages = current->memory.count; // 清理前保留替换规模，不复制整进程控制块。
    struct user_memory old_memory = take_memory(&current->memory); // 旧根此刻仍活动，其所有权暂由局部描述接住。
    current->memory = take_memory(&next.memory); // 新根所有权进入原进程槽，候选来源立即为空。
    current->tls = next.tls; // 当前映像改变以后，后续工作者使用新的模板布局。
    current->tls_image = next.tls_image; // 借新ELF初值，不沿用旧静态模板。
    uint64_t old_fpu = running->fpu_physical; // 旧用户CPU状态页不属于用户地址空间。
    running->fpu_physical = next.execution.fpu_physical; // 替换为准备好的默认x87/SSE状态。
    next.execution.fpu_physical = 0; // 候选从此不能再归还这页。
    running->fs_base = next.execution.fs_base; // 新映像的静态TLS指针，或无TLS时的0。
    running->initial = next.execution.initial; // 保存新入口数值，不保存指向候选栈局部对象的指针。
    running->frame = f; // 原内核陷入栈及TSS继续属于同一执行者。
    *f = running->initial; // 全部可失败准备完成之后才覆盖旧RIP/RSP和寄存器。
    running->fault_error = 0; // 新映像没有旧用户故障报告。
    current->exit_code = 0; // 运行中的新映像还没有退出结果。
    running->wait_target = 0; // 原先完成的wait条件不能成为新映像状态。
    running->wait_status = 0; // 撤销旧用户输出地址的数值借用。
    running->io = IO_NONE; // exec发生在运行系统调用中，没有需要继续的旧阻塞操作。
    running->io_fd = running->io_events = 0; // FD表原地保留，但旧等待条件清零。
    running->io_address = running->io_bytes = running->io_deadline = 0; // 不把旧空间中的请求指针带到新映像。
    airi_fd_close_all(&current->files, true); // 进程FD表原地保留，成功提交才关闭CLOEXEC。
    if (input_pid == current->pid) // 若前台进程自行exec，旧输入权限不能漏到其他映像。
        input_pid = 0; // 成功提交时才撤销，失败exec仍保留旧权限。
    if (acquire_input) { // 本次新映像确实是init管理的shell。
        airi_fd_close(&current->files, 0); // 无表项时-9可忽略；已有普通FD0也按此启动策略关闭。
        if (!airi_fd_attach_input(&current->files)) // 新FD0必须为空且输入引用成功接入。
            airi_panic("cannot attach supervised shell input"); // 事务提交内部不变量错误。
        input_pid = current->pid; // 文件引用完成后才发布唯一消费身份。
    }
    running->state = USER_RUNNING; // PID、父关系、已运行tick、内核栈和异步owner均原地保持。
    if (!airi_vm_activate(&current->memory.space)) // 新根已借到当前内核栈及内核映射。
        airi_panic("cannot activate committed exec image"); // 硬件不变量失败不能回旧用户继续。
    destroy_memory(&old_memory); // CR3已切到新根，只归还局部接住的旧用户内存。
```

共同用户入口确认current与running均有效，把实际FS和FPU保存给这条真实执行链。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    airi_log("[THREAD_RETURN] pid=%u tid=%u user_ticks=%u", current->pid, running->tid, running->user_ticks); // 不关闭共享FD、异步owner或用户页。
    return schedule(REASON_EXIT); // 主执行者或别的工作者可继续，当前栈尚不能归还。
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
<!-- after -->
```c
    airi_log("[THREAD_RETURN] pid=%u tid=%u user_ticks=%u", current->pid, running->tid, running->user_ticks); // 不关闭共享FD、异步owner或用户页。
    return schedule(REASON_EXIT); // 主执行者或别的工作者可继续，当前栈尚不能归还。
}

void airi_user_enter(struct airi_trap_frame *f) { // 所有CS3来源在分派设备、timer或syscall之前共同保存。
    if (!session_active || !current || !running || running->state != USER_RUNNING || (f->cs & 3) != 3) // 当前实际栈必须有运行所有者。
        airi_panic("CPU state without a running user"); // 内核int129不经过这个入口。
    running->fs_base = airi_tls_take(); // 先取真实用户基址，再把硬件FS清零供内核处理。
    airi_fpu_save(airi_memory_direct(running->fpu_physical)); // x87/XMM也在此唯一入口保存，不能因设备没有新ISR而跳过。
}
struct airi_trap_frame *airi_user_resume(struct airi_trap_frame *f) { // 所有用户iretq路径使用同一规范地址和标志策略。
    if (f->cs != 27 || f->ss != 35 || f->rip >= AIRI_USER_LIMIT || f->rsp >= AIRI_USER_LIMIT) { // 坏返回不能变成内核GP。
```

统一返回与设备/PIT来源都使用running的frame、FS/FPU和tick，内存与FD仍从current取得。

<!-- replace: kernel/process/user.c -->
<!-- before -->
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
<!-- after -->
```c
    }
    f->rflags &= ~((3ull << 12) | (1ull << 14) | (1ull << 17)); // 清IOPL、NT和VM。
    f->rflags |= 0x202; // 保留必需bit1并保持用户IF。
    airi_tls_restore(running->fs_base); // 与所选用户身份一起恢复，随后内核C不会通过FS访问数据。
    airi_fpu_restore(airi_memory_direct(running->fpu_physical)); // 当前身份已确定，恢复后内核和公共汇编不再使用浮点/XMM。
    return f; // 返回选中执行链的完整现场。
}
struct airi_trap_frame *airi_user_device_irq(struct airi_trap_frame *f) { // 本函数只在真实设备IRQ打断ring3后调用。
    if (!session_active || !current || !running || running->state != USER_RUNNING) // 用户返回必须有明确现场所有者。
        airi_panic("device IRQ without a running user"); // 不把未知栈交给启动继续点。
    if (!airi_async_work()) // 同步启动请求或已收集的旧通知无需切用户身份。
        return airi_user_resume(f); // 用户返回策略仍统一核对。
    running->frame = f; // 保存完整GPR与用户RIP/RSP，稍后从同一指令继续。
    running->state = USER_RUNNABLE; // 不丢掉被设备提示打断的计算用户运行资格。
    return pause_for_service(false); // 切到长期普通栈后才pump、复制用户输出和唤醒等待者。
}
struct airi_trap_frame *airi_user_timer(struct airi_trap_frame *f) { // IRQ路径不分配、不回收、不打印，只有状态与根选择。
    if (!session_active || !current || !running || running->state != USER_RUNNING) // CS3的IRQ必须有明确用户所有者。
        airi_panic("user IRQ without a running process"); // 不把未知内核栈交给调度器。
    running->frame = f; // 保存真正被打断的位置，不只保存C ABI寄存器。
    ++running->user_ticks; // 实际CPU用户时间片事件数。
    if (running->user_ticks % 2 == 0) // 每两个实际IRQ尝试一次round-robin。
        return schedule(REASON_TIMER); // 没有其他可运行者时恢复同一个进程。
    return airi_user_resume(f); // 也不能跳过坏返回检查。
}
struct airi_trap_frame *airi_user_trap(struct airi_trap_frame *f) { // 内核仍不可抢占，只在完整用户陷入边界处理进程状态。
    if (f->vector == 129 && f->cs == 8 && (!session_active || session_idle) && current && running && running->state == USER_RUNNABLE) { // 内核启动一组已经准备好的用户。
        kernel_return = f; // 启动栈的继续位置一直保留。
        session_idle = false; // 当前int129重新登记了仍在启动栈上的继续位置。
        session_active = true; // 之后的CS3陷入有会话归属。
        return select_execution(running); // 按目标根和TSS进入首个用户。
    }
    if (!session_active || !current || !running || running->state != USER_RUNNING || (f->cs & 3) != 3) // 内核错误不能伪装为用户故障。
        airi_panic("user trap without a running process"); // 维持故障边界。
    collect_pending(); // 现在确实在另一条内核栈上，回收已经离开的孩子。
    running->frame = f; // 系统调用与故障都以当前实际现场为继续点。
    if (f->vector != 128) { // 用户CPU故障只结束拥有者。
        running->fault_error = f->error; // 保存权限故障证据。
        airi_log("[USER_FAULT] pid=%u vector=%llu error=0x%llx rip=0x%llx cr2=0x%llx", current->pid, (unsigned long long)f->vector, (unsigned long long)f->error, (unsigned long long)f->rip, (unsigned long long)(f->vector == 14 ? airi_trap_fault_address() : 0)); // PF才读取本次CR2。
        return exit_current(-(128 + (int)f->vector)); // 自有故障编码，不宣称Linux信号ABI。
    }
```

同步read/write阻塞时，每条running分别保存自己的FD和用户范围。

<!-- replace: kernel/process/user.c -->
<!-- before -->
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
<!-- after -->
```c
        bool positioned = f->rax == 15 || f->rax == 16; // 显式偏移不支持阻塞流。
        long result = file_transfer(current, f->rdi, f->rsi, (size_t)f->rdx, write, positioned, f->r10); // 先实际尝试一次。
        if (result == -11 && !positioned && !airi_fd_nonblocking(&current->files, (unsigned)f->rdi)) { // 阻塞流暂不可完成，用户不能提前看到EAGAIN。
            running->io = write ? IO_WRITE : IO_READ; // 保存需要重试的操作种类。
            running->io_fd = (unsigned)f->rdi; // 此时FD已由file_transfer完整验证。
            running->io_address = f->rsi; // 用户页继续归此进程所有，不借内核局部buffer。
            running->io_bytes = f->rdx; // 一次最多256字节，已经验证跨度。
            running->state = USER_IO_WAIT; // 条件和数据先登记，再移出可运行集合。
            ++io_block_count; // 实际阻塞计数。
            airi_log("[WAIT_REGISTER] pid=%u operation=%s fd=%u", current->pid, write ? "write" : "read", running->io_fd); // 普通系统调用上下文记录。
            return schedule(REASON_IO); // 选择别的用户或进入真正内核空闲等待。
        }
        f->rax = (uint64_t)result; // 非阻塞仍把EAGAIN交给用户，其他结果直接返回。
```

close先检查同组是否仍有执行者等待这个FD，防止号码复用改变在途目标。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
        }
        f->rax = copied && f->rsi <= 31 ? (uint64_t)airi_fd_open(&current->files, path, (unsigned)f->rsi) : (uint64_t)-14ll; // 错误路径不变更文件。
    } else if (f->rax == 12) // close撤销当前表项引用。
        f->rax = (f->rdi < AIRI_FD_COUNT ? (uint64_t)airi_fd_close(&current->files, (unsigned)f->rdi) : (uint64_t)-9ll); // 不删除文件内容。
    else if (f->rax == 13) // dup共享打开对象，标志放在新表项。
        f->rax = (f->rdi < AIRI_FD_COUNT ? (uint64_t)airi_fd_dup(&current->files, (unsigned)f->rdi, f->rsi != 0) : (uint64_t)-9ll); // 单CPU内核中同步加引用。
    else if (f->rax == 14) // 仅绝对定位，不实现Linux全部whence。
```
<!-- after -->
```c
        }
        f->rax = copied && f->rsi <= 31 ? (uint64_t)airi_fd_open(&current->files, path, (unsigned)f->rsi) : (uint64_t)-14ll; // 错误路径不变更文件。
    } else if (f->rax == 12) // close撤销当前表项引用。
        f->rax = (f->rdi >= AIRI_FD_COUNT ? (uint64_t)-9ll : fd_waited(current, f->rdi) ? (uint64_t)-16ll : (uint64_t)airi_fd_close(&current->files, (unsigned)f->rdi)); // 不删除文件内容。
    else if (f->rax == 13) // dup共享打开对象，标志放在新表项。
        f->rax = (f->rdi < AIRI_FD_COUNT ? (uint64_t)airi_fd_dup(&current->files, (unsigned)f->rdi, f->rsi != 0) : (uint64_t)-9ll); // 单CPU内核中同步加引用。
    else if (f->rax == 14) // 仅绝对定位，不实现Linux全部whence。
```

ready等待把事件与绝对截止登记在本running。

<!-- replace: kernel/process/user.c -->
<!-- before -->
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
<!-- after -->
```c
            if (result >= 0) // 负错误不参与位运算。
                result &= (long)f->rsi | AIRI_READY_HUP | AIRI_READY_ERROR; // 关闭与错误始终报告。
            if (!result && f->rdx) { // 条件未满足且愿意有限等待。
                running->io = IO_READY; // 不读写任何字节，只重查条件。
                running->io_fd = (unsigned)f->rdi; // 属于此进程的稳定表项。
                running->io_events = (unsigned)f->rsi; // 保存订阅条件。
                running->io_deadline = airi_timer_ticks() + f->rdx; // 真实时钟截止，不靠循环次数。
                running->state = USER_IO_WAIT; // 原frame已保存在进程中。
                ++io_block_count; // 这次调用确实阻塞。
                airi_log("[WAIT_REGISTER] pid=%u operation=ready fd=%u", current->pid, running->io_fd); // 由普通内核上下文记录。
                return schedule(REASON_IO); // 返回到同一次wait调用之后。
            }
        }
```

CQ收集前仍核对完整用户输出，阻塞时只保存该running的借用。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
            result = -14; // 不丢完成、信用与buffer状态。
        else { // 当前owner的CQ与user输出身份明确。
            result = airi_async_pop(current->pid, &completion); // 可能设备很快，已有CQ直接返回。
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
<!-- after -->
```c
            result = -14; // 不丢完成、信用与buffer状态。
        else { // 当前owner的CQ与user输出身份明确。
            result = airi_async_pop(current->pid, &completion); // 可能设备很快，已有CQ直接返回。
            if (!result && !copy_user(current, &completion, f->rdi, sizeof(completion), true)) // 同次不可抢占调用中完整映射稳定。
                airi_panic("validated completion output changed"); // 不能消费后隐瞒复制失败。
            if (result == -11 && f->rsi) { // 空CQ且选择阻塞，保留这同一次调用。
                running->io = IO_ASYNC; // poll_waiters只收集，设备推进在启动栈。
                running->io_address = f->rdi; // 用户输出页仍由进程拥有。
                running->io_bytes = sizeof(completion); // 固定ABI长度，不借C局部对象。
                running->state = USER_IO_WAIT; // 先保存条件和输出，再撤销运行资格。
                ++io_block_count; // 记录真实CQ等待，不强迫快设备也假装阻塞。
                airi_log("[WAIT_REGISTER] pid=%u operation=cq", current->pid); // 等设备完成后原调用继续。
                return schedule(REASON_IO); // 可切其他用户或进入启动服务空闲。
```

sleep继续保存一次绝对截止，属于具体running的等待。

<!-- replace: kernel/process/user.c -->
<!-- before -->
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
<!-- after -->
```c
        else if (now > UINT64_MAX - f->rdi) // 不让截止计数回绕成已经过期。
            f->rax = (uint64_t)-75ll; // -EOVERFLOW。
        else { // 请求完整有效，保存条件后再退出可运行集合。
            running->io = IO_SLEEP; // poll_waiters不再把它当FD操作。
            running->io_fd = running->io_events = 0; // 清与此次请求无关的旧数值。
            running->io_address = running->io_bytes = 0; // sleep不借用用户指针。
            running->io_deadline = now + f->rdi; // 保留绝对截止，不在每次轮询延长。
            running->state = USER_IO_WAIT; // 此前已经保存实际系统调用frame。
            ++io_block_count; // 记录真正阻塞，而非返回0的空操作。
            airi_log("[WAIT_REGISTER] pid=%u operation=sleep ticks=%llu", current->pid, (unsigned long long)f->rdi); // 普通调用栈记录，不在IRQ打印。
            return schedule(REASON_IO); // 其他用户继续；全阻塞时启动栈hlt等PIT。
```

FS设置与查询读写running的保存值，仍受原低半区范围限制。

<!-- replace: kernel/process/user.c -->
<!-- before -->
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
<!-- after -->
```c
        if (f->rdi >= AIRI_USER_LIMIT) // 非规范中间地址与所有内核高半区都在特权WRMSR之前拒绝。
            f->rax = (uint64_t)-22ll; // -EINVAL，旧保存值不变。
        else { // 0表示停用，其余低半区值即使尚未映射也可登记。
            running->fs_base = f->rdi; // 当前硬件仍为内核0，统一返回入口才写用户值。
            f->rax = 0; // 不解引用、不读取该用户地址。
        }
    }
    else if (f->rax == 34) // 查询的是陷入时从硬件读取的真实当前值。
        f->rax = running->fs_base; // 低半区基址可由long无歧义表示，不返回内核地址。
    else if (f->rax == 35 || f->rax == 36) // 自有显式偏移向量操作，R10保存第四参数。
        f->rax = (uint64_t)vector_transfer(current, f->rdi, f->rsi, f->rdx, f->r10, f->rax == 36); // 描述和前缀在调用内完成，不建立异步借用。
    else if (f->rax == 37) { // 有符号相对定位仍由打开对象的所有者一次完成。
```

在原时钟调用之后接入39至42：创建、工作返回、join和当前TID；2号进程退出保持原路径。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
            f->rax = copy_user(current, &value, f->rsi, sizeof(value), true) ? 0 : (uint64_t)-14ll; // 完整输出先验证，坏指针不产生用户CPU故障。
        }
    }
    else // 尚未提供的接口都明确拒绝。
        f->rax = (uint64_t)-38ll; // -ENOSYS。
    poll_waiters(); // 文件/流修改完成后立即重查其他等待者，不等用户轮询。
```
<!-- after -->
```c
            f->rax = copy_user(current, &value, f->rsi, sizeof(value), true) ? 0 : (uint64_t)-14ll; // 完整输出先验证，坏指针不产生用户CPU故障。
        }
    }
    else if (f->rax == 39) // 自有共享执行者创建，不是Linux clone或pthread ABI。
        f->rax = (uint64_t)create_worker(f->rdi, f->rsi, f->rdx); // 函数入口和用户参数完整复制成寄存器值。
    else if (f->rax == 40) // 用户工作函数返回，只结束本执行者。
        return return_worker(f); // 不关闭进程共享FD或异步owner。
    else if (f->rax == 41) // 主执行者收取一次同组工作结果。
        return join_worker(f); // 未完成时真正阻塞于原frame。
    else if (f->rax == 42) // 执行身份与共享PID分别查询。
        f->rax = running->tid; // 同组工作者的TID各自不同。
    else // 尚未提供的接口都明确拒绝。
        f->rax = (uint64_t)-38ll; // -ENOSYS。
    poll_waiters(); // 文件/流修改完成后立即重查其他等待者，不等用户轮询。
```

launch绑定主execution的process与TID，启动服务循环选择具体执行者，结束时撤销running。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    if (direct_shell && !airi_fd_attach_input(&root->files)) // 根的新FD0必须为空。
        airi_panic("cannot attach direct shell input"); // 初始化不变量不成立。
    root->pid = next_pid++; // 完整准备后才取得运行身份。
    int async_error = airi_async_open(root->pid); // 完整映像准备后才登记新进程资源上下文。
    if (async_error) { // 未向用户发布，此时根与栈不活动。
        cleanup(root); // 回滚页面、FD与内核栈，尚未拥有异步owner。
        airi_memory_snapshot(&after); // 失败不能留下普通内存泄漏。
        if (!airi_memory_same_usage(&before, &after)) // 静态池外的资源同样核对。
            airi_panic("failed async owner launch leaked memory"); // 内部不变量明确停止。
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
    kernel_return = NULL; // 下次会话重新登记。
    airi_async_drain(); // 所有owner已关闭，内核仍须结束无主ACTIVE并核对静态池完全回收。
```
<!-- after -->
```c
    if (direct_shell && !airi_fd_attach_input(&root->files)) // 根的新FD0必须为空。
        airi_panic("cannot attach direct shell input"); // 初始化不变量不成立。
    root->pid = next_pid++; // 完整准备后才取得运行身份。
    root->execution.process = root; // 会话主执行者显式借所属进程。
    root->execution.tid = root->pid; // 主TID初始与PID相同，额外执行者另取身份。
    int async_error = airi_async_open(root->pid); // 完整映像准备后才登记新进程资源上下文。
    if (async_error) { // 未向用户发布，此时根与栈不活动。
        cleanup(root); // 回滚页面、FD与内核栈，尚未拥有异步owner。
        airi_memory_snapshot(&after); // 失败不能留下普通内存泄漏。
        if (!airi_memory_same_usage(&before, &after)) // 静态池外的资源同样核对。
            airi_panic("failed async owner launch leaked memory"); // 内部不变量明确停止。
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
    current = root; running = &root->execution; // 内核启动门同时选择进程与主执行者。
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
            struct user_execution *next = NULL; // 每次重查后重新选择真实执行者。
            for (unsigned n = 1; n <= EXECUTION_COUNT; ++n) // 轮转覆盖主/工作执行者。
                if (execution_at((execution_slot(running) + n) % EXECUTION_COUNT)->state == USER_RUNNABLE) { // 等待者可能刚被唤醒。
                    next = execution_at((execution_slot(running) + n) % EXECUTION_COUNT); // 保存控制块，不提前切根。
                    break; // 首个可运行者获得下一次int129。
                }
            if (next) { // 服务后已有用户可继续。
                if (service_from_timer && next != running) // 只有来自真实IRQ的身份改变才计抢占。
                    ++timer_switches; // 不把用户主动阻塞算成IRQ切换。
                current = next->process; running = next; // int129随后正式设置目标根、TSS与现场。
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
    current = NULL; running = NULL; // 不保留已清掉的进程或执行者指针。
    supervisor_pid = input_pid = 0; // 有限会话结束后不能把授权留给下次运行。
    kernel_return = NULL; // 下次会话重新登记。
    airi_async_drain(); // 所有owner已关闭，内核仍须结束无主ACTIVE并核对静态池完全回收。
```

既有用户cli故障观察的说明改为结束目标进程，运行语句保持。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
void airi_user_demo(void) { // 串口主线的真实用户边界与多进程回归。
    // 用户资源已由airi_user_init建立，演示不重置正式初始化状态。
    run_one(airi_user_probe_start, (size_t)((uintptr_t)airi_user_probe_end - (uintptr_t)airi_user_probe_start), NULL, 0, false, true, false); // 原有非法指针与用户IRQ验证。
    run_one(airi_user_bad_start, (size_t)((uintptr_t)airi_user_bad_end - (uintptr_t)airi_user_bad_start), NULL, -141, false, false, false); // 用户cli只杀当前身份。
    run_one(airi_user_load_bad_start, (size_t)((uintptr_t)airi_user_load_bad_end - (uintptr_t)airi_user_load_bad_start), NULL, -142, false, false, false); // USER保护PF。
    run_one(airi_user_rsp_bad_start, (size_t)((uintptr_t)airi_user_rsp_bad_end - (uintptr_t)airi_user_rsp_bad_start), NULL, -22, false, false, false); // 坏返回栈不击穿内核。
    run_one(airi_hello_elf_start, (size_t)((uintptr_t)airi_hello_elf_end - (uintptr_t)airi_hello_elf_start), "hello", 0, true, false, false); // 独立C和自有用户库。
```
<!-- after -->
```c
void airi_user_demo(void) { // 串口主线的真实用户边界与多进程回归。
    // 用户资源已由airi_user_init建立，演示不重置正式初始化状态。
    run_one(airi_user_probe_start, (size_t)((uintptr_t)airi_user_probe_end - (uintptr_t)airi_user_probe_start), NULL, 0, false, true, false); // 原有非法指针与用户IRQ验证。
    run_one(airi_user_bad_start, (size_t)((uintptr_t)airi_user_bad_end - (uintptr_t)airi_user_bad_start), NULL, -141, false, false, false); // 用户cli结束目标进程，内核继续。
    run_one(airi_user_load_bad_start, (size_t)((uintptr_t)airi_user_load_bad_end - (uintptr_t)airi_user_load_bad_start), NULL, -142, false, false, false); // USER保护PF。
    run_one(airi_user_rsp_bad_start, (size_t)((uintptr_t)airi_user_rsp_bad_end - (uintptr_t)airi_user_rsp_bad_start), NULL, -22, false, false, false); // 坏返回栈不击穿内核。
    run_one(airi_hello_elf_start, (size_t)((uintptr_t)airi_hello_elf_end - (uintptr_t)airi_hello_elf_start), "hello", 0, true, false, false); // 独立C和自有用户库。
```

<a id="thread-user-publication"></a>
## 用原子发布共享输入，用 TLS 保存各自的观察值

新建threads-probe.c。普通shared_fd、shared_handle、expected_pid和jobs在发布之前写好；主执行者对gate以release存1，工作者以acquire读到这个1之后才读取这些输入。这两项必须操作同一个原子对象且读到对应发布，才建立普通数据的可见关系。只有“值最后变成1”或随便睡一会儿不能替代这项关系。

`_Atomic unsigned`让gate的读写通过C11原子接口完成，`ATOMIC_INT_LOCK_FREE==2`表示该目标类型始终无需库锁，当前LLVM可直接生成目标指令，不借未实现的libatomic。release/acquire保证发布顺序，不负责把工作者从内核睡眠中唤醒；本例gate等待用yield安排观察，真正阻塞由后面的join与管道承担。volatile只用在TLS或浮点观察上迫使实际访问，它不承担线程同步。

fresh读取声明初值7、全部33字节零数组和errno0。主执行者随后把自己的值改成99与EBUSY，工作者仍应从声明模板开始；每轮工作又留下91、0xab和201，下一次缓存复用必须重新清除。先写全局记录和fresh，再接上计算工作函数。

<!-- write: labs/airios/threads-probe.c -->
```c
#include <airi_threads.h> // 自有共享执行接口，不链接宿主pthread。
#include <airi_libc.h> // 原始进程、页、FD和异步调用保留负错误。
#include <errno.h> // 每条执行链应取得自己新建的静态TLS错误槽。
#include <stdatomic.h> // 共享发布使用明确的C11原子操作，不把volatile当同步。
_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "thread probe needs lock-free int atomics"); // 当前x86-64不借未实现的libatomic。
static _Thread_local volatile unsigned local = 7; // 文件初值，每个新工作者从7而非父当前值99开始。
static _Thread_local volatile unsigned char zero[33]; // 复用工作槽也须重新清零完整tbss。
static _Atomic unsigned gate, capacity_gate, pipe_entered; // 主执行者以release发布，工作者以acquire观察。
static _Atomic unsigned observed[2], opened; // 工作结果不通过无同步的普通全局写入传播。
static long expected_pid; // 创建前设定，发布gate以后只读。
static unsigned shared_fd; // 工作执行者访问的同一进程表项。
static uint64_t shared_handle; // 注册buffer属于进程PID，工作者共享同一owner。
struct job { unsigned index; }; // 参数存活在主执行者栈上，直到两次join都结束。
static int fresh(void) { // 普通C本地变量的初始值不能从主执行者运行块复制。
    if (local != 7 || errno) // 父当前值分别为99与EBUSY。
        return 0; // 不以FS地址不同代替内容正确。
    for (unsigned i = 0; i < sizeof(zero); ++i) // 检查整个新tbss。
        if (zero[i]) // 上一次工作者留下的修改也必须清除。
            return 0; // 未初始化的复用槽不能继续。
    return 1; // 只读当前执行者的真实TLS。
}
```

<a id="thread-private-cpu"></a>
## 继承入场浮点环境，抢占以后各自保持修改

主执行者创建前把MXCSR设成0x7f80、x87控制字设成0x0f7f，工作者先确认继承这组值，再各自改成不同舍入控制。循环通过真实FS本地访问和控制寄存器查询跨二十个PIT tick检查，普通volatile double乘法仍实际使用SSE2。3.25乘2精确为6.5，避免舍入方向改变样例结果。

工作函数必须恢复入场控制值再返回，因这些控制位属于普通调用约定要求保持的环境；失败观察也走同一个restore出口。主执行者join后检查自己的第三组控制未被污染，再恢复原入场值。64位函数结果故意超过低八位，防止把工作return错误接到整个进程的exit编码上。接上compute。

<!-- append: labs/airios/threads-probe.c -->
```c
static long compute(void *argument) { // 两个工作者共享进程与FD，但CPU状态和TLS分别保存。
    struct job *job = argument; // 主执行者在join之前不会结束此参数对象。
    while (!atomic_load_explicit(&gate, memory_order_acquire)) // 创建后暂等主执行者完成共享输入和忙路径观察。
        airi_yield(); // 此发布等待只是测试夹具，尚不是用户futex。
    if (!fresh() || airi_getpid() != expected_pid || airi_thread_id() == expected_pid) // acquire读到发布以后再读取普通共享输入；同PID、不同TID且新TLS初值。
        return -101; // 通过join返回具体失败，不伪造进程成功。
    unsigned char byte; // 每个工作者有独立用户栈输出。
    if (airi_read(shared_fd, &byte, 1) != 1) // 共享打开对象的偏移应被两个调用各消费一次。
        return -102; // 主执行者随后应读到第三个字节c。
    atomic_store_explicit(&observed[job->index], byte, memory_order_release); // 内容由主执行者在join后读取。
    if (!job->index) { // 一位工作者另建FD，主执行者之后必须能在同一表中使用。
        long fd = airi_open("/seed", AIRI_READ); // 独立open从offset0开始。
        if (fd < 0 || airi_io_put(shared_handle, "T", 1)) // 异步buffer同样使用共享PID授权。
            return -103; // 工作者结束不能关闭这份进程资源。
        atomic_store_explicit(&opened, (unsigned)fd, memory_order_release); // 发布新表项号码。
    }
    local = 10 + job->index; // 两个工作者使用同一编译器符号，运行值分别为10与11。
    zero[32] = (unsigned char)(30 + job->index); // 修改tbss尾部供下次槽复用观察。
    errno = (int)(200 + job->index); // 错误槽也是独立运行副本。
    unsigned mxcsr, configured = 0x1f80u | ((job->index + 1) << 13); // 分别使用不同舍入控制，正常返回前再恢复。
    unsigned short control, configured_control = (unsigned short)(0x037fu | ((job->index + 1) << 10)); // x87控制也要随执行者保持。
    __asm__ volatile("stmxcsr %0; fnstcw %1" : "=m"(mxcsr), "=m"(control)); // 直接观察入场硬件控制位。
    if (mxcsr != 0x7f80 || control != 0x0f7f) // 新工作者继承主执行者在创建前修改的浮点环境，不重新置为新进程默认值。
        return -104; // 不能只给新TLS而遗漏环境继承。
    unsigned inherited_mxcsr = mxcsr; unsigned short inherited_control = control; // 函数返回前仍须恢复自己入场时的控制值。
    __asm__ volatile("ldmxcsr %0; fldcw %1" : : "m"(configured), "m"(configured_control)); // 设定本执行者自己的两个控制值。
    long value = 0x1234567800000000l + job->index; // 正常与失败都走控制位恢复出口。
    uint64_t start = airi_ticks(); // 下一个循环必须跨真实PIT抢占窗口。
    do { // 普通用户计算不以yield冒充切换。
        for (unsigned i = 0; i < 10000; ++i) { // 反复通过FS和真实控制寄存器观察。
            __asm__ volatile("stmxcsr %0; fnstcw %1" : "=m"(mxcsr), "=m"(control)); // 不从静态副本猜硬件状态。
            if (mxcsr != configured || control != configured_control || local != 10 + job->index || zero[32] != 30 + job->index || errno != (int)(200 + job->index)) { // 任何串状态都让实际join结果失败。
                value = -105; // 失败也须遵循普通C调用的控制位保持约定。
                goto restore; // 不从中间分支绕过恢复。
            }
        }
        volatile double input = 3.25, multiplier = 2.0; // 当前用户编译选项真正生成SSE2浮点运算。
        volatile double product = input * multiplier; // 精确6.5不受当前舍入方向影响。
        if (product != 6.5) { // 各自用户栈和寄存器内容必须正确。
            value = -106; // 当前目标不借宿主浮点运行时。
            goto restore; // 仍先恢复再返回错误。
        }
    } while (airi_ticks() - start < 20); // 二十个真实tick，不以宿主sleep代替。
restore: // 无论观察成功或失败，工作函数都按C ABI归还控制状态。
    mxcsr = inherited_mxcsr; control = inherited_control; // 普通C调用返回前恢复本次继承的入场控制状态。
    __asm__ volatile("ldmxcsr %0; fldcw %1" : : "m"(mxcsr), "m"(control)); // 恢复后桥接器提交64位函数返回值。
    return value; // join不得按进程exit的低八位截断。
}
```

其余工作函数分别承担槽复用、真实管道read、显式进程exit与CPU缺页。short_job等capacity_gate发布以后才读取普通身份输入，并修改TLS供下一轮复用观察；pipe_entered只说明用户即将read，真正的内核阻塞仍由WAIT_REGISTER和后续close忙验证。fault_job使用明确汇编写地址0，避免C空指针未定义行为被优化成别的指令。继续追加这些函数。

<!-- append: labs/airios/threads-probe.c -->
```c
static long short_job(void *argument) { // 用于容量、重复创建和TLS缓存重置。
    (void)argument; // 本工作函数没有额外输入对象。
    while (!atomic_load_explicit(&capacity_gate, memory_order_acquire)) // 容量阶段让全部八槽同时存活。
        airi_yield(); // gate释放之前不能提前结束并影响容量结论。
    if (!fresh() || airi_getpid() != expected_pid) // acquire以后再读创建前的普通进程身份输入，同时核对本地模板初值。
        return -107; // 上一轮tbss或errno不能残留。
    local = 91; zero[32] = 0xab; errno = 201; // 故意留下非初值给下一轮观察。
    return airi_thread_id(); // 主执行者可以把完整结果与创建得到的TID对应。
}
static long pipe_job(void *argument) { // 工作者阻塞read时主执行者还应能继续其他调用。
    unsigned fd = (unsigned)(uintptr_t)argument; // FD号码作为不解引用的数值参数传入。
    atomic_store_explicit(&pipe_entered, 1, memory_order_release); // 标志只说明即将read，串口WAIT_REGISTER才说明内核已阻塞。
    char byte; // 当前工作用户栈在join后继续缓存，不在阻塞期间解除。
    long result = airi_read(fd, &byte, 1); // 无字节时真正登记IO_READ等待。
    return result == 1 ? (unsigned char)byte : -108; // 主执行者join应得到发送的Z。
}
static long process_exit_job(void *argument) { // 与工作函数return不同，原始exit结束整个进程组。
    char byte; // 先等父进程验证外组TID不能join。
    if (airi_read((unsigned)(uintptr_t)argument, &byte, 1) != 1) // 真实管道控制何时触发退出。
        return -109; // 不能提前把目标变成已收取状态。
    airi_exit(77); // 本执行者和它的主执行者一起停止，共享资源只关闭一次。
}
static long fault_job(void *argument) { // 用户故障必须结束目标进程，不能击穿内核或父进程。
    (void)argument; // 明确不借主执行者对象。
    __asm__ volatile("movl $1, (%0)" : : "r"((uintptr_t)0) : "memory"); // 显式CPU写未映射0地址，不让C空指针UB被编译器替换成其他指令。
    return -110; // 正常PF14路径不会执行。
}
```

<a id="thread-main-flow"></a>
## 从页预算失败恢复，再创建两个共享工作者

main首先检查主tid等于pid及本地初值，再用原始页接口填满进程预算，随后只归还八页。create应在八页用户栈已经接入、第九页无法取得时返回−12；新增数据页、跨记录边界取得的元数据页、内核栈与FPU都必须回滚。失败后重新申请八页应成功，再多一页应失败，核对不是只清空控制块。最后按各自剩余长度归还原始页，恢复正常创建能力。

主执行者另借一页观察并发release忙，打开seed并注册一个异步buffer。两个compute工作者通过同一FD分别读a/b，主执行者随后应读c；一位工作者还新open另一FD，返回后主执行者必须能在同一表中读取它，不能仅以共享打开对象偏移代替共享FD表。该工作者把注册buffer从Q改成T，主随后读回并free，验证同PID资源授权与return不关闭owner。这里验证的是注册buffer共享，未计为多消费者SQ/CQ压力测试。

创建以后先检查并发fork/exec/waitpid/release和坏join输出，发布gate，再实际join。每个参数对象jobs[i]位于主用户栈，直到两个join结束都保持有效。下面先写main到这组共享资源检查结束，后面的同一main继续追加。

<!-- append: labs/airios/threads-probe.c -->
```c
int main(void) { // 主执行者负责创建和收拢，不在库中伪造pthread对象。
    expected_pid = airi_getpid(); // 创建任何工作者之前发布固定进程身份。
    if (airi_thread_id() != expected_pid || !fresh()) // 主TID初始等于PID，新ELF TLS为模板值。
        return 70; // 不能由旧进程槽猜执行者身份。
    local = 99; errno = EBUSY; // 后续新工作者不能继承这两个运行值。
    if (airi_thread_create(NULL, NULL) != -14 || airi_thread_join(expected_pid, NULL) != -22 || airi_thread_join(-1, NULL) != -3) // 非RX入口、自等待和无目标分别拒绝。
        return 71; // 参数错误不创建资源。
    void *pages[80]; size_t lengths[80], used = 0; // 保存实际成功的原始页申请，用于恢复预算。
    for (size_t step = 64 * 4096; step; step = step == 64 * 4096 ? 4096 : 0) // 先大批再补单页，填满当前用户数据页预算。
        for (;;) { // 每个步长申请到失败。
            void *page = airi_alloc(step); // 不经过尚未同步的malloc。
            if (!page) // 大批失败后还有可能容纳小批。
                break; // 不记录失败指针。
            if (used == 80) // 保护当前有界记录容量。
                return 72; // 固定1024页预算不应越界。
            pages[used] = page; lengths[used++] = step; // 每项保留真实页面所有权和长度。
        }
    size_t available = 8 * 4096; // 留八页，故意让工作用户栈在已经建立部分页面以后失败。
    while (available) { // 归还最后几批的后缀，不假定尾项一定是单页。
        if (!used) // 已填满的预算应有足够原始页，仍保护记录下标。
            return 72; // 不因异常环境在用户栈上越界。
        --used; // 取最后一个仍持有的范围。
        size_t part = lengths[used] < available ? lengths[used] : available; // 最多归还当前范围，全部长度都按页对齐。
        if (airi_release((unsigned char *)pages[used] + lengths[used] - part, part)) // 只解除该范围的页对齐后缀。
            return 73; // 失败时不假装已经腾出预算。
        lengths[used] -= part; available -= part; // 记录剩余前缀和还需要腾出的字节。
        if (lengths[used]) // 部分归还时保留同一起点的前缀所有权。
            ++used; // 当前项仍是有效记录，下一轮只会在还有剩余目标时继续。
    }
    if (airi_thread_create(short_job, NULL) != -12) // 八页用户栈已接入，第九页触及预算，内核栈/FPU和新增记录页都应回滚。
        return 73; // 不发布半个工作执行者。
    void *restored = airi_alloc(8 * 4096); // 失败后必须能重新取得全部八页，不能只回滚控制块。
    if (!restored || airi_alloc(4096)) // 重新占满以后第九页应失败，核对净数据页数量。
        return 73; // 漏页或错误归还都不能继续发布成功。
    pages[used] = restored; lengths[used++] = 8 * 4096; // 保存这次真实恢复的所有权，后面一起归还。
    while (used) { // 失败创建不得把这些原始页误收走。
        --used; // 只索引真实成功项。
        if (airi_release(pages[used], lengths[used])) // 先恢复用户页预算，再真正创建工作者。
            return 74; // 不继续忽略映射归属错误。
    }
    void *guard_page = airi_alloc(4096); // 后续工作期间的释放应明确返回忙。
    long fd = airi_open("/seed", AIRI_READ), handle = airi_io_alloc(); // 文件和注册buffer都由进程共享。
    if (!guard_page || fd < 0 || handle < 0 || airi_io_put((uint64_t)handle, "Q", 1)) // 真实资源完整准备后才发布输入。
        return 75; // 不在无效FD或句柄上启动工作函数。
    shared_fd = (unsigned)fd; shared_handle = (uint64_t)handle; // gate发布之前设置只读共享输入。
    unsigned saved_mxcsr, observed_mxcsr, parent_mxcsr = 0x7f80; // 父使用第三种舍入方向，与两个工作者的配置都不同。
    unsigned short saved_control, observed_control, parent_control = 0x0f7f; // 记录父原环境，正常与错误出口都应恢复。
    __asm__ volatile("stmxcsr %0; fnstcw %1" : "=m"(saved_mxcsr), "=m"(saved_control)); // 在创建之前取得真实主执行者环境。
    __asm__ volatile("ldmxcsr %0; fldcw %1" : : "m"(parent_mxcsr), "m"(parent_control)); // 工作者应继承这个已修改的环境，而非硬编码默认值。
    struct job jobs[2] = {{0}, {1}}; // 两个参数对象在整个join期间保持存活。
    long first = airi_thread_create(compute, &jobs[0]), second = airi_thread_create(compute, &jobs[1]); // 同进程两个独立执行者。
    if (first < 0 || second < 0 || first == second || first == expected_pid || second == expected_pid) { // 身份不能与PID或彼此混同。
        __asm__ volatile("ldmxcsr %0; fldcw %1" : : "m"(saved_mxcsr), "m"(saved_control)); // 创建失败也先恢复父环境。
        return 76; // 后续按真实返回身份join。
    }
    if (airi_fork() != -16 || airi_exec("broken") != -16 || airi_waitpid(-1, NULL) != -16 || airi_release(guard_page, 4096) != -16 || airi_thread_join(first, (long *)(uintptr_t)0xffff800000000000ull) != -14) { // 并发映像操作拒绝，坏join输出不能消费目标。
        __asm__ volatile("ldmxcsr %0; fldcw %1" : : "m"(saved_mxcsr), "m"(saved_control)); // 失败不把修改后的环境带回_start。
        return 77; // 旧进程内存和两个工作者保持。
    }
    atomic_store_explicit(&gate, 1, memory_order_release); // 发布共享输入，允许两个工作函数进入计算阶段。
    long result = 0; // 内核检查完整8字节用户输出。
    if (airi_thread_join(first, &result) || result != 0x1234567800000000l || airi_thread_join(second, &result) || result != 0x1234567800000001l || airi_thread_join(first, NULL) != -3) { // 真阻塞、64位结果与一次收取均实际观察。
        __asm__ volatile("ldmxcsr %0; fldcw %1" : : "m"(saved_mxcsr), "m"(saved_control)); // 观察失败仍遵循主调用环境恢复约定。
        return 78; // 不接受只看到THREAD_CREATE就算运行。
    }
    __asm__ volatile("stmxcsr %0; fnstcw %1" : "=m"(observed_mxcsr), "=m"(observed_control)); // 两位工作者结束后观察父自己的真实硬件控制值。
    __asm__ volatile("ldmxcsr %0; fldcw %1" : : "m"(saved_mxcsr), "m"(saved_control)); // 先恢复入场环境，再解释观察结果。
    if (observed_mxcsr != parent_mxcsr || observed_control != parent_control) // 工作者各自修改控制不能污染父的第三种环境。
        return 79; // 此时父环境已经恢复，后续代码不依赖观察值。
    unsigned a = atomic_load_explicit(&observed[0], memory_order_acquire), b = atomic_load_explicit(&observed[1], memory_order_acquire); // 配合工作者的release读取各自完成数据。
    char byte; // 共享偏移已经由两个工作者消费a/b。
    if (!((a == 'a' && b == 'b') || (a == 'b' && b == 'a')) || airi_read(shared_fd, &byte, 1) != 1 || byte != 'c' || local != 99 || errno != EBUSY) // 主TLS保持，两个工作者共享文件位置。
        return 79; // 不能把复制进程冒充共享执行。
    unsigned created = atomic_load_explicit(&opened, memory_order_acquire); // 工作者新打开的表项应仍在进程中。
    if (airi_read(created, &byte, 1) != 1 || byte != 'a' || airi_io_get(shared_handle, &byte, 1) || byte != 'T' || airi_close(created) || airi_close(shared_fd) || airi_io_free(shared_handle) || airi_release(guard_page, 4096)) // 工作结束保留共享FD/owner，全部join后可释放原始页。
        return 80; // 共享资源应只由进程持有一次。
```

八槽观察先让全部short_job在gate上保持存活，第九次创建必须返回−11；发布以后逐一按TID收结果。随后二十四轮创建/join超过全局槽数，最后一次NULL输出同时观察丢弃结果。每次新工作者都检查TLS初值，缓存复用不等于复用旧本地对象。

管道工作者在空管道上read，主执行者给它真实PIT运行窗口，再确认关闭那个仍被等待的FD返回−16，写Z唤醒，join取得Z以后才关闭端点。用户发布标志不能证明已进入内核等待，所以自动观察还核对本进程实际WAIT_REGISTER记录。继续main。

<!-- append: labs/airios/threads-probe.c -->
```c
    long identities[8]; // 固定全局八工作槽容量。
    for (unsigned i = 0; i < 8; ++i) // gate未开放，八个工作者同时占槽。
        if ((identities[i] = airi_thread_create(short_job, NULL)) < 0) // 原始页失败后的资源也必须恢复可用。
            return 81; // 不让未创建项参与join。
    if (airi_thread_create(short_job, NULL) != -11) // 第九个工作者必须明确容量不足。
        return 82; // 不覆盖仍活着或尚未收取的执行者。
    atomic_store_explicit(&capacity_gate, 1, memory_order_release); // 一次发布，让所有工作者可正常return。
    for (unsigned i = 0; i < 8; ++i) // 按返回身份收取，结果顺序不由执行完成顺序推断。
        if (airi_thread_join(identities[i], &result) || result != identities[i]) // 每个结果是其真实gettid。
            return 83; // 不混用槽号或旧身份。
    for (unsigned i = 0; i < 24; ++i) { // 超过八槽容量的连续复用，不结束进程恢复。
        long tid = airi_thread_create(short_job, NULL); // 重建TLS初值，保留用户页缓存但另建CPU状态与内核栈。
        if (tid < 0 || airi_thread_join(tid, i == 23 ? NULL : &result) || (i != 23 && result != tid)) // 最后一轮同时观察丢弃结果。
            return 84; // 上一位工作者的local/tbss/errno不能进入下一轮。
    }
    unsigned ends[2]; // 工作者阻塞在同一进程管道表上。
    if (airi_pipe(ends, 0)) // 真实阻塞管道，不借宿主注入。
        return 85; // 不在无效端点上继续。
    long reader = airi_thread_create(pipe_job, (void *)(uintptr_t)ends[0]); // 参数只是FD号码。
    if (reader < 0) // 创建必须成功才等待发布。
        return 86; // 避免无限等一个不存在的工作者。
    while (!atomic_load_explicit(&pipe_entered, memory_order_acquire)) // 这只说明用户即将read，不据此假定内核已经登记。
        airi_yield(); // 让工作者先获得CPU。
    if (airi_sleep(3) || airi_close(ends[0]) != -16 || airi_write(ends[1], "Z", 1) != 1 || airi_thread_join(reader, &result) || result != 'Z' || airi_close(ends[0]) || airi_close(ends[1])) // 实际PIT窗口后确认close忙，再写入唤醒并收取。
        return 87; // 阻塞FD不能被关闭后复用到另一个对象。
```

最后两次fork发生在工作者已全部join以后。第一个孩子创建工作者，把其真实tid通过管道交给父；父join该外组tid得到−3，再发触发字节。孩子工作者调用airi_exit(77)，孩子主执行者即使正在join也必须随整个组结束，父wait得到77。第二个孩子的工作者写未映射0地址，父得到−142并继续读取自己的TLS。

两项都用实际进程与管道完成，不把宿主线程、日志字符串或QEMU终止当作故障隔离。接入剩余代码并闭合main。

<!-- append: labs/airios/threads-probe.c -->
```c
    unsigned channel[2], trigger[2]; // 父取得外进程工作TID，再控制它触发整进程退出。
    if (airi_pipe(channel, 0) || airi_pipe(trigger, 0)) // 两条真实管道分别传身份和触发字节。
        return 88; // 资源错误不能伪装成权限隔离成功。
    long child = airi_fork(); // 当前全部工作者已join，因此重新允许fork。
    if (child < 0) // 父映像和用户页缓存仍可复制。
        return 89; // 不把负值当孩子身份。
    if (!child) { // 孩子另开自己的工作者，保留复制来的管道FD引用。
        long tid = airi_thread_create(process_exit_job, (void *)(uintptr_t)trigger[0]); // 工作者先等父许可。
        if (tid < 0 || airi_write(channel[1], &tid, sizeof(tid)) != sizeof(tid)) // 完整身份经内核管道交出。
            return 90; // 不能让父永久等不到身份。
        airi_thread_join(tid, NULL); // 主执行者真实等待，工作exit77应连同这条等待一起结束进程。
        return 96; // 只有工作函数意外普通返回时才到达，不能冒充整组退出。
    }
    long foreign; int status = 0; // 外组TID不是本进程可join的能力。
    if (airi_read(channel[0], &foreign, sizeof(foreign)) != sizeof(foreign) || airi_thread_join(foreign, NULL) != -3 || airi_write(trigger[1], "!", 1) != 1 || airi_waitpid(child, &status) != child || status != 77) // 同时验证跨组拒绝与worker exit结束整个孩子组。
        return 91; // 父仍应继续。
    if (airi_close(channel[0]) || airi_close(channel[1]) || airi_close(trigger[0]) || airi_close(trigger[1])) // 归还父持有的管道引用。
        return 92; // 子整组退出已关闭自己的副本。
    child = airi_fork(); // 再验证CPU故障的组退出与父隔离。
    if (child < 0) // 前一组的工作槽必须已经回收。
        return 93; // 不跳过故障演示。
    if (!child) { // 故障只发生在孩子地址空间中。
        long tid = airi_thread_create(fault_job, NULL); // 工作者应真正进入用户函数触发PF14。
        if (tid < 0) // 资源失败不能替代真实用户异常。
            return 94; // 资源失败不能当成页故障成功。
        airi_thread_join(tid, NULL); // 故障应终止整个孩子组，主执行者不从这里继续。
        return 97; // 普通返回说明预期PF14未发生。
    }
    if (airi_waitpid(child, &status) != child || status != -142 || local != 99 || errno != EBUSY) // 故障组回收后父的TLS和执行能力保持。
        return 95; // 不以整个QEMU停止代替进程隔离。
    airi_puts("[THREAD_USER_OK] shared=memory,fd,async tls=fresh cpu=private join=64bit capacity=8 reuse=24 busy=checked group_exit=77 worker_fault=pf14\n"); // 全部真实正常与故障路径结束后发布。
    return 0; // shell继续，整会话最后核对全部进程和工作者资源归还。
}
```

<a id="thread-build"></a>
## 链接用户库、桥接器与真实观察程序

Makefile为threads-lib.o明确选择libc/threads.c，thread-entry.o选择用户汇编，threads-probe.o来自观察程序；这三个不同目标避免库与程序使用同一个模式规则名字。独立threads.elf再选择posix.o以提供真实TLS errno，原始程序的USER_COMMON保持不变。images.S加入实际ELF范围，user.c目录只增加threads一个名字，并保留K51的ctls-tail。

新增后的目录总长255字节，包含NUL，仍满足256字节容量。下面把线程库与观察目标接到既有构建和包装中；没有图形源码、字体、Cocoa或宿主pthread依赖。

在既有malloc映像规则与ctls-tail规则之间加入线程库、桥接器和观察ELF，目标名称分别保存。

<!-- replace: Makefile -->
<!-- before -->
```make
# 本配置产物路径仅作为.incbin输入。
$(OUT)/labs/airios/images.o: CPPFLAGS += -DAIRI_MALLOC_ELF='"$(OUT)/user/malloc-probe.elf"'

# 单独观察TLS零尾部与较小普通BSS，不需要先实现多个执行者。
$(OUT)/user/ctls-tail.elf: $(USER_COMMON) $(OUT)/user/ctls-tail.o labs/airios/program.ld
# 文件初值来自tdata，tbss运行空间不由普通LOAD长度替代。
```
<!-- after -->
```make
# 本配置产物路径仅作为.incbin输入。
$(OUT)/labs/airios/images.o: CPPFLAGS += -DAIRI_MALLOC_ELF='"$(OUT)/user/malloc-probe.elf"'

# 共享执行库显式链接，不让早期原始程序自动依赖线程入口。
$(OUT)/user/threads-lib.o: labs/airios/libc/threads.c Makefile
# 单独命名，与观察程序threads-probe.o分别保存。
	@mkdir -p $(dir $@)
# 使用当前用户C目标和自动头依赖。
	$(CC) $(USER_FLAGS) -Ilabs/airios/include -c $< -o $@
# 用户桥接器的call遵循普通C ABI，工作返回再进入自有内核调用。
$(OUT)/user/thread-entry.o: labs/airios/thread-entry.S Makefile
# 它不属于内核架构入口对象。
	@mkdir -p $(dir $@)
# 用户汇编与C使用同一无红区、SSE2目标配置。
	$(CC) $(USER_FLAGS) -c $< -o $@
# 观察程序选择errno/TLS和共享执行库，不调用宿主pthread。
$(OUT)/user/threads.elf: $(USER_COMMON) $(OUT)/user/posix.o $(OUT)/user/threads-lib.o $(OUT)/user/thread-entry.o $(OUT)/user/threads-probe.o labs/airios/program.ld
# 一份静态用户ELF，全部自有运行符号在链接时解析。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(filter %.o,$^) -o $@
# 初始目录只包本配置真正链接的共享执行程序。
$(OUT)/labs/airios/images.o: $(OUT)/user/threads.elf
# 路径宏供只读incbin消费。
$(OUT)/labs/airios/images.o: CPPFLAGS += -DAIRI_THREADS_ELF='"$(OUT)/user/threads.elf"'

# 单独观察TLS零尾部与较小普通BSS，不需要先实现多个执行者。
$(OUT)/user/ctls-tail.elf: $(USER_COMMON) $(OUT)/user/ctls-tail.o labs/airios/program.ld
# 文件初值来自tdata，tbss运行空间不由普通LOAD长度替代。
```

在给出的唯一包装窗口接入threads映像，仍读取本配置实际ELF。

<!-- replace: labs/airios/images.S -->
<!-- before -->
```asm
.incbin AIRI_MALLOC_ELF /* 由本配置静态链接，不借宿主malloc。 */
airi_malloc_elf_end: /* 输入范围的排他右端。 */

.balign 8 /* TLS零尾部观察仍经真实静态ELF装载。 */
.global airi_ctls_tail_elf_start, airi_ctls_tail_elf_end /* 输入范围供真实初始程序目录使用。 */
airi_ctls_tail_elf_start: /* 普通BSS小于tbss的最小程序。 */
```
<!-- after -->
```asm
.incbin AIRI_MALLOC_ELF /* 由本配置静态链接，不借宿主malloc。 */
airi_malloc_elf_end: /* 输入范围的排他右端。 */

.balign 8 /* 共享线程观察仍作为独立用户ELF装载。 */
.global airi_threads_elf_start, airi_threads_elf_end /* 目录只保存这份只读映像范围。 */
airi_threads_elf_start: /* 自有运行库含C11 TLS和共享执行桥接器。 */
.incbin AIRI_THREADS_ELF /* 当前配置真正交叉链接的文件。 */
airi_threads_elf_end: /* 完整输入文件的排他右端。 */

.balign 8 /* TLS零尾部观察仍经真实静态ELF装载。 */
.global airi_ctls_tail_elf_start, airi_ctls_tail_elf_end /* 输入范围供真实初始程序目录使用。 */
airi_ctls_tail_elf_start: /* 普通BSS小于tbss的最小程序。 */
```

<a id="thread-observe"></a>
## 观察真实 TID、阻塞和故障以后再继续 shell

shell_test在heap结束以后连续运行两次threads，每次按该命令的进程PID收集35个互不重复的工作tid：两计算、八容量、二十四复用、一个管道。前两个返回记录必须各有至少两个真实user_ticks，并实际出现join与工作管道的阻塞。子进程的两个工作者不混进本组计数，整条命令最后必须退出0。

在heap之后连续运行两次threads，按本次PID收集工作TID、真实tick及join/管道等待记录。

<!-- replace: scripts/shell_test.py -->
<!-- before -->
```python
                heap_output=text[offset:] # 只接受本条命令与它实际exec的孩子记录。
                assert '[MALLOC_USER_OK] arena=262144 align=16 calloc=zero overflow=checked realloc=failure_preserved reuse=128 fork=private exec=reset' in heap_output # 对齐、复用、失败保留与父子隔离均由guest观察。
                assert '[MALLOC_EXEC_RESET_OK] arena=lazy objects=zero errno=zero' in heap_output # 成功exec必须重新建立BSS和TLS，不能继承旧arena。
                offset=len(text); wire.sendall(b'orphans\n'); text=wait('[SHELL_COMMAND_DONE] name=orphans code=0',offset) # 同一用户shell持续服务，不靠返回monitor收拢孙进程。
                orphan_output=text[offset:] # 只核对本条命令的身份记录。
                assert '[ORPHAN_USER_OK] rounds=24 live=12 zombie=12 session=alive' in orphan_output # 超过八槽容量仍能连续fork。
```
<!-- after -->
```python
                heap_output=text[offset:] # 只接受本条命令与它实际exec的孩子记录。
                assert '[MALLOC_USER_OK] arena=262144 align=16 calloc=zero overflow=checked realloc=failure_preserved reuse=128 fork=private exec=reset' in heap_output # 对齐、复用、失败保留与父子隔离均由guest观察。
                assert '[MALLOC_EXEC_RESET_OK] arena=lazy objects=zero errno=zero' in heap_output # 成功exec必须重新建立BSS和TLS，不能继承旧arena。
                for repetition in range(2): # 同一shell连续完成两次线程命令，上一组退出回收后应能重新创建。
                    offset=len(text); wire.sendall(b'threads\n'); text=wait('[SHELL_COMMAND_DONE] name=threads code=0',offset) # 真正同地址空间执行，不能由fork复制或宿主线程模拟。
                    thread_output=text[offset:] # 所有身份与标记均限定在这条命令之后。
                    assert '[THREAD_USER_OK] shared=memory,fd,async tls=fresh cpu=private join=64bit capacity=8 reuse=24 busy=checked group_exit=77 worker_fault=pf14' in thread_output # 用户主动检查正常、容量、生命周期与故障。
                    owner=re.search(r'\[EXEC_COMMIT\] pid=(\d+) program=threads ',thread_output) # 进程PID与各工作TID分别读取。
                    assert owner # 不能拿其他程序的执行身份满足条件。
                    identities=re.findall(r'\[THREAD_CREATE\] pid='+owner.group(1)+r' tid=(\d+) ',thread_output) # 首两个是跨PIT的计算工作者。
                    assert len(identities)==35 and len(set(identities))==35 # 两计算、八容量、二十四复用和一个管道工作者全部真实创建。
                    for identity in identities[:2]: # 两个CPU/TLS观察都必须实际获得时间片。
                        returned=re.search(r'\[THREAD_RETURN\] pid='+owner.group(1)+r' tid='+identity+r' user_ticks=(\d+)',thread_output) # 每个结束记录保留真实IRQ数。
                        assert returned and int(returned.group(1))>=2 # 不能只有主执行者主动yield而没有工作抢占窗口。
                    assert '[THREAD_JOIN_WAIT]' in thread_output and re.search(r'\[WAIT_REGISTER\] pid='+owner.group(1)+r' operation=read ',thread_output) # join与工作管道都真实阻塞。
                offset=len(text); wire.sendall(b'orphans\n'); text=wait('[SHELL_COMMAND_DONE] name=orphans code=0',offset) # 同一用户shell持续服务，不靠返回monitor收拢孙进程。
                orphan_output=text[offset:] # 只核对本条命令的身份记录。
                assert '[ORPHAN_USER_OK] rounds=24 live=12 zombie=12 session=alive' in orphan_output # 超过八槽容量仍能连续fork。
```

完整shell序列完成后，结果记录加入共享执行者与独立CPU/TLS观察。

<!-- replace: scripts/shell_test.py -->
<!-- before -->
```python
                offset=len(text); wire.sendall(b'echo after-shell\n'); text=wait('[CONSOLE_UPDATE] view=0 commands=2 page=0',offset) # 回到内核后输入仍完整。
                assert 'after-shell' in text[offset:] # 不只是退出而丢了后续UART。
                result={'passed':True,'ring3_shell':True,'ram_file_and_device_readback':True,'other_user_during_cq_wait':True,'qmp_iops':1,'error_exec_survived':True,'returned_to_monitor':True,'orphan_rounds':24,'memory_reuse_rounds':128,'capacity_rounds':4,'capacity_pages':1024,'simd_preempt_fork_exec':True,'fs_base_preempt_fork_exec':True,'compiler_static_tls':True,'posix_errno_io_subset':True,'positioned_vectors':True} # 只发布已观察事实。
                result_path.write_text(json.dumps(result,indent=2)+'\n'); print('PASS user shell: alloc/file/async wait/other task/CQ/readback, failed exec, return to monitor') # 阶段演示实际完成。
                return result # 总入口可收集。
            finally: # 包括断言、超时和QEMU错误。
```
<!-- after -->
```python
                offset=len(text); wire.sendall(b'echo after-shell\n'); text=wait('[CONSOLE_UPDATE] view=0 commands=2 page=0',offset) # 回到内核后输入仍完整。
                assert 'after-shell' in text[offset:] # 不只是退出而丢了后续UART。
                result={'passed':True,'ring3_shell':True,'ram_file_and_device_readback':True,'other_user_during_cq_wait':True,'qmp_iops':1,'error_exec_survived':True,'returned_to_monitor':True,'orphan_rounds':24,'memory_reuse_rounds':128,'capacity_rounds':4,'capacity_pages':1024,'simd_preempt_fork_exec':True,'fs_base_preempt_fork_exec':True,'compiler_static_tls':True,'posix_errno_io_subset':True,'positioned_vectors':True} # 只发布已观察事实。
                result.update(user_heap=True,shared_executions=True,thread_commands=2,worker_capacity=8,worker_reuse_rounds=24) # 关键字把实际已验收字段加入结果字典，不替代上面的逐命令检查。
                result_path.write_text(json.dumps(result,indent=2)+'\n'); print('PASS user shell: alloc/file/async wait/other task/CQ/readback, failed exec, return to monitor') # 阶段演示实际完成。
                return result # 总入口可收集。
            finally: # 包括断言、超时和QEMU错误。
```

<!-- run -->
```sh
./airios build  # 交叉链接自有用户线程库、桥接器、观察程序及串口内核。
./airios test  # 验证全部既有路径和共享执行者的正常、容量、生命周期与故障行为。
```

交互运行./airios run，在内核提示符输入run sh，用户提示符输入threads，再threads，最后exit。每次都从新的用户ELF开始，主执行者的35个工作者在同一个进程里创建和收拢；不要把两次不同进程混成一个全局固定PID。

```text
run sh
threads
threads
exit
```

PID、tid和调度次数按实际会话变化。成功时可观察多条THREAD_CREATE、THREAD_JOIN_WAIT、THREAD_RETURN，孩子的USER_EXIT code=77及USER_FAULT vector=14，最后有：

```text
[THREAD_USER_OK] shared=memory,fd,async tls=fresh cpu=private join=64bit capacity=8 reuse=24 busy=checked group_exit=77 worker_fault=pf14
[SHELL_COMMAND_DONE] name=threads code=0
```

用户库没有未解析的pthread或libatomic符号。线程程序本身的TLS初值4字节、总长65字节，普通LOAD内存56字节，使用K51已修正的零尾部规则。全部结束后shell继续，整个会话收尾核对共享页、FD、内核堆活对象及异步池归还；内核栈/FPU不应因反复join逐轮遗失。

本轮个人Linux的p5-threads连接重新尝试，SSH返回255，未执行远端编译、pthread或futex，仍标记verified=false。Linux语义对照可继续读[L05](../linux/05-threads.md)和[pthreads说明](https://man7.org/linux/man-pages/man7/pthreads.7.html)，本章自有ABI不冒充Linux二进制或完整POSIX。下一步是在这个真实共享与等待路径上建立比较入睡、唤醒与互斥量，而不是用yield循环充当最终同步库。
