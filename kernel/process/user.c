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
enum io_operation { IO_NONE, IO_READ, IO_WRITE, IO_READY, IO_ASYNC, IO_SLEEP, IO_JOIN, IO_FUTEX }; // 等待保存操作条件，不保留内核局部buffer。
struct user_page { uint64_t address, physical; unsigned flags; }; // fork需要把同一VA映射到另一个物理页。
#define PAGE_RECORDS (4096u / sizeof(struct user_page)) // 一物理页放170项，余下16字节不用。
#define RECORD_CHUNKS ((AIRI_USER_PAGES + PAGE_RECORDS - 1) / PAGE_RECORDS) // 1024项最多借七个记录页。
struct user_memory { // 用户地址空间及其页面所有权，不含CPU现场、内核栈、FD或异步owner。
    struct airi_address_space space; // 自有根和低半区页表，高半区继续借用。
    uint64_t records[RECORD_CHUNKS]; // 元数据页记录本地址空间拥有的数据页，不映射给用户。
    unsigned count; // 包括用户代码、动态页、TLS与用户栈，不包括内核陷入栈或FPU页。
};
_Static_assert(sizeof(struct user_memory) <= 128, "user memory owner must stay bounded"); // 按值转移只复制小描述，不复制1024页内容。
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
static unsigned next_pid = 1, root_pid, timer_switches, wait_blocks; // 每次运行会话的根身份和实际行为计数。
static unsigned io_block_count, io_wake_count, idle_halt_count; // 会话真实I/O阻塞与唤醒次数。
static bool session_idle, service_from_timer; // 是否暂回启动栈服务，和整个会话结束分开。
static unsigned supervisor_pid, input_pid; // 可选init身份与当前唯一UART输入身份，不是进程槽。
static bool session_active; // 只有用户会话活动时才接受用户IRQ或系统调用。
static int session_result; // 根用户退出结果，不用最后一个孩子的结果覆盖。
static struct airi_trap_frame *kernel_return; // 启动栈一直保留到整个用户会话结束。
extern const uint8_t airi_user_probe_start[], airi_user_probe_end[]; // 受控正常映像。
extern const uint8_t airi_user_bad_start[], airi_user_bad_end[]; // 用户cli故障。
extern const uint8_t airi_user_load_bad_start[], airi_user_load_bad_end[]; // 用户读supervisor页故障。
extern const uint8_t airi_user_rsp_bad_start[], airi_user_rsp_bad_end[]; // 不规范用户返回栈。
extern const uint8_t airi_hello_elf_start[], airi_hello_elf_end[]; // 既有独立用户C程序。
extern const uint8_t airi_process_elf_start[], airi_process_elf_end[]; // 新的fork/wait和抢占工作负载。
extern const uint8_t airi_vector_elf_start[], airi_vector_elf_end[]; // 显式偏移向量正常与坏范围观察。
extern const uint8_t airi_malloc_elf_start[], airi_malloc_elf_end[]; // 用户对象分配与页面缓存的独立ELF。
extern const uint8_t airi_mutex_elf_start[], airi_mutex_elf_end[]; // 原子所有权与futex等待组合的真实用户观察。
extern const uint8_t airi_futex_elf_start[], airi_futex_elf_end[]; // 私有内存等待与通知的真实用户程序。
extern const uint8_t airi_threads_elf_start[], airi_threads_elf_end[]; // 同进程共享内存、FD与异步owner的真实工作执行者。
extern const uint8_t airi_posix_elf_start[], airi_posix_elf_end[]; // 显式链接errno与POSIX风格文件包装。
extern const uint8_t airi_ctls_elf_start[], airi_ctls_elf_end[]; // 编译器_Thread_local与静态模板。
extern const uint8_t airi_ctls_char_elf_start[], airi_ctls_char_elf_end[]; // 单字节小对齐模板。
extern const uint8_t airi_ctls_page_elf_start[], airi_ctls_page_elf_end[]; // 最大纯零模板与独立TCB页。
extern const uint8_t airi_ctls_tail_elf_start[], airi_ctls_tail_elf_end[]; // TLS零尾部大于普通LOAD的真实链接器布局。
extern const uint8_t airi_tls_big_elf_start[], airi_tls_big_elf_end[]; // 真正含超限PT_TLS的错误ELF。
extern const uint8_t airi_tls_elf_start[], airi_tls_elf_end[]; // FS本地数据与切换探针。
extern const uint8_t airi_tls_reset_elf_start[], airi_tls_reset_elf_end[]; // 成功exec清基址观察。
extern const uint8_t airi_simd_elf_start[], airi_simd_elf_end[]; // 浮点、抢占和继承探针。
extern const uint8_t airi_simd_reset_elf_start[], airi_simd_reset_elf_end[]; // exec新状态观察。
extern const uint8_t airi_init_elf_start[], airi_init_elf_end[]; // 用户态常驻监督程序。
extern const uint8_t airi_capacity_elf_start[], airi_capacity_elf_end[]; // 大BSS、深用户栈与跨记录页的fork/exec。
extern const uint8_t airi_memory_elf_start[], airi_memory_elf_end[]; // 用户页面归还与空洞复用工作负载。
extern const uint8_t airi_orphans_elf_start[], airi_orphans_elf_end[]; // 持续会话中的孤儿回收与槽复用演示。
extern const uint8_t airi_shell_elf_start[], airi_shell_elf_end[]; // 真正用户shell，不是内核命令别名。
extern const uint8_t airi_demo_elf_start[], airi_demo_elf_end[]; // 用户页分配与异步存储贯通演示。
extern const uint8_t airi_async_elf_start[], airi_async_elf_end[]; // 用户注册、CQ等待与exec资源轮次。
extern const uint8_t airi_waits_elf_start[], airi_waits_elf_end[]; // 真正阻塞与超时工作负载。
extern const uint8_t airi_pipes_elf_start[], airi_pipes_elf_end[]; // 非阻塞管道用户程序。
extern const uint8_t airi_storage_elf_start[], airi_storage_elf_end[]; // 写入和恢复共用一份明确分支的用户ELF。
extern const uint8_t airi_files_elf_start[], airi_files_elf_end[]; // 内存文件用户工作负载。
extern const uint8_t airi_worker_elf_start[], airi_worker_elf_end[]; // exec目标是另一个独立静态ELF。
extern const uint8_t airi_broken_elf_start[], airi_broken_elf_end[]; // 受控损坏输入，验证旧进程保持。
struct executable { const char *name; const uint8_t *start, *end; }; // 初始映像目录尚不是通用文件系统。
static const struct executable executables[] = { // 每份映像字节由内核只读段长期持有。
    {"sh", airi_shell_elf_start, airi_shell_elf_end}, // 独占串口输入的可选用户程序。
    {"init", airi_init_elf_start, airi_init_elf_end}, // 管理前台shell的常驻用户进程。
    {"vector", airi_vector_elf_start, airi_vector_elf_end}, // 聚合、分散、短前缀与错误原子性。
    {"heap", airi_malloc_elf_start, airi_malloc_elf_end}, // 单线程用户对象与错误路径。
    {"heap-reset", airi_malloc_elf_start, airi_malloc_elf_end}, // 同一ELF新BSS和按需arena。
    {"mutex", airi_mutex_elf_start, airi_mutex_elf_end}, // 临界区、错误所有者、截止与非robust寿命。
    {"futex", airi_futex_elf_start, airi_futex_elf_end}, // 相等比较与登记、超时及进程私有键。
    {"threads", airi_threads_elf_start, airi_threads_elf_end}, // 独立TLS/CPU状态、join与整个进程退出。
    {"libc", airi_posix_elf_start, airi_posix_elf_end}, // 用户库错误与文件结果转换。
    {"libc-reset", airi_posix_elf_start, airi_posix_elf_end}, // 同一ELF新错误槽和CLOEXEC观察。
    {"ctls", airi_ctls_elf_start, airi_ctls_elf_end}, // 自动建立静态TLS后运行普通C。
    {"ctls-reset", airi_ctls_elf_start, airi_ctls_elf_end}, // 同一映像成功exec后观察新模板。
    {"ctls-char", airi_ctls_char_elf_start, airi_ctls_char_elf_end}, // 小模板的负偏移与TP对齐。
    {"ctls-page", airi_ctls_page_elf_start, airi_ctls_page_elf_end}, // 两页运行块的真实边界。
    {"ctls-tail", airi_ctls_tail_elf_start, airi_ctls_tail_elf_end}, // 普通BSS与TLS零尾部各自分配。
    {"tls-too-big", airi_tls_big_elf_start, airi_tls_big_elf_end}, // 真实装载器必须拒绝，不进入用户main。
    {"tls", airi_tls_elf_start, airi_tls_elf_end}, // 每个用户身份独立FS基址。
    {"tls-reset", airi_tls_reset_elf_start, airi_tls_reset_elf_end}, // 新ELF不沿用旧FS。
    {"simd", airi_simd_elf_start, airi_simd_elf_end}, // 真实SSE/x87隔离与fork/exec。
    {"simd-reset", airi_simd_reset_elf_start, airi_simd_reset_elf_end}, // 新ELF入口的默认状态。
    {"capacity", airi_capacity_elf_start, airi_capacity_elf_end}, // 预算耗尽后回收并继续运行。
    {"memory", airi_memory_elf_start, airi_memory_elf_end}, // 同一地址空间内释放与重新申请。
    {"orphans", airi_orphans_elf_start, airi_orphans_elf_end}, // 不结束根会话也能回收退出的孤儿。
    {"demo", airi_demo_elf_start, airi_demo_elf_end}, // 页、任务和设备完成串联。
    {"hello", airi_hello_elf_start, airi_hello_elf_end}, // 独立C运行库与BSS观察。
    {"processes", airi_process_elf_start, airi_process_elf_end}, // fork/exec/wait实际工作负载。
    {"worker", airi_worker_elf_start, airi_worker_elf_end},
    {"async", airi_async_elf_start, airi_async_elf_end}, // 完整用户提交/完成路径。
    {"async-reset", airi_async_elf_start, airi_async_elf_end}, // 同一PID新epoch的验证入口。
    {"waits", airi_waits_elf_start, airi_waits_elf_end}, // 阻塞流和就绪条件等待。
    {"pipes", airi_pipes_elf_start, airi_pipes_elf_end}, // 有界FIFO与关闭语义。
    {"storage", airi_storage_elf_start, airi_storage_elf_end}, // 显式提交记录。
    {"recover", airi_storage_elf_start, airi_storage_elf_end}, // 只读取已恢复记录。
    {"files", airi_files_elf_start, airi_files_elf_end}, // 真实用户FD演示。
    {"broken", airi_broken_elf_start, airi_broken_elf_end}, // 用于完整的坏ELF失败演示。
};
static const struct executable *find_program(const char *name) { // 输入是内核已复制的短NUL字符串。
    size_t bytes = airi_strlen(name); // 只读当前有效字符串，不保存其指针。
    for (unsigned i = 0; i < sizeof(executables) / sizeof(executables[0]); ++i) // 遍历固定目录。
        if (airi_strlen(executables[i].name) == bytes && !airi_memcmp(executables[i].name, name, bytes)) // 长度与全部名称字节都相同才匹配。
            return &executables[i]; // 只借长期静态目录项。
    return NULL; // 未知名称不触碰任何用户地址空间。
}
static unsigned slot(const struct user_process *p) { // 只接受processes中的真实槽，不接受临时复制对象。
    return (unsigned)(p - processes); // 同一数组内指针差得到稳定槽号。
}
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
static bool create_fpu(struct user_execution *execution) { // 构造器只给空控制块建立独立页所有权。
    execution->fpu_physical = airi_memory_take_page(); // 一页足够容纳对齐512字节状态，与用户记录页分别归还。
    if (!execution->fpu_physical) // 没有内存就不准备可运行身份。
        return false; // cleanup可回收此前的栈等资源。
    void *state = airi_memory_direct(execution->fpu_physical); // 内核HHDM别名，不给用户USER映射。
    airi_memset(state, 0, 4096); // 剩余页字节也不带旧内容。
    airi_fpu_new(state); // 只复制已验证模板，不改变当前用户硬件状态。
    return true; // 页句柄此后由执行者持有，离开该执行链后才能归还。
}
static bool user_access(const struct user_process *p, uint64_t address, size_t bytes, bool write) { // 检查完整用户范围，不直接解引用它。
    if (address >= AIRI_USER_LIMIT || bytes > AIRI_USER_LIMIT - address) // 排除内核地址与右端溢出。
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
        if (to_user) // wait结果由内核写入父进程自己的页面。
            airi_memcpy(airi_memory_direct(physical), (uint8_t *)buffer + done, part); // 不切到父用户根也能访问其已拥有物理页。
        else // write系统调用把用户数据复制进内核副本。
            airi_memcpy((uint8_t *)buffer + done, airi_memory_direct(physical), part); // 不信任原始VA作为内核指针。
        done += part; // 推进下一段。
    }
    return true; // 同步结束，不借出buffer指针。
}
static long file_transfer(struct user_process *p, uint64_t fd, uint64_t address, size_t bytes, bool write, bool positioned, uint64_t offset) { // 允许在另一根的阻塞进程上重试，同步副本属于当前内核栈。
    char buffer[256]; // 一次有界副本，不跨调度保留。
    if (fd >= AIRI_FD_COUNT) // 完整64位编号先检查再转换。
        return -9; // -EBADF。
    if (bytes > sizeof(buffer)) // 不让用户长度覆盖内核栈。
        return -22; // -EINVAL。
    if (!user_access(p, address, bytes, !write)) // 输出在消费流或偏移之前完整验证。
        return -14; // -EFAULT。
    if (write) { // 用户内容在本次尝试内复制。
        copy_user(p, buffer, address, bytes, false); // 通过目标自己的页表，不解引用原始用户VA。
        if (!positioned && airi_fd_console(&p->files, (unsigned)fd)) { // 固定终端同步发送。
            airi_serial_write_bytes(buffer, bytes); // 不保存局部buffer给设备。
            return (long)bytes; // 返回逻辑进展。
        }
        return airi_fd_write(&p->files, (unsigned)fd, buffer, bytes, positioned, offset); // 满管道可返回EAGAIN。
    }
    if (airi_fd_input(&p->files, (unsigned)fd)) { // 固定输入对象的引用与当前前台读取资格分别检查。
        if (p->pid != input_pid) // fork复制的FD引用本身不转移UART消费权。
            return -13; // -EACCES；不消费设备字节。
        if (positioned) // 字节流没有文件偏移。
            return -29; // -ESPIPE，不消费字节。
        if (!bytes) // 零长度读取不能登记永久等待，也不消费UART。
            return 0; // 有效输入FD的空操作。
        size_t received = 0; // 返回当前可取得的前缀，不等待填满用户请求。
        while (received < bytes) { // bytes与整个用户输出范围已在上方验证。
            int c = airi_arch_serial_read(); // 设备寄存器只在架构层。
            if (c < 0) // 暂无字节和字符NUL不同。
                break; // 让已有前缀先交给用户。
            buffer[received++] = (uint8_t)c; // 固定内核副本不会越过256字节。
        }
        if (!received) // 空输入走同一个IO_READ持久等待机制。
            return -11; // 由陷入分派登记等待，不让shell忙轮询。
        if (!copy_user(p, buffer, address, received, true)) // 消费UART前已经完整验证用户范围。
            airi_panic("validated UART output mapping changed"); // 内部错误不能静默丢字节。
        return (long)received; // 实际取得的字节数。
    }
    long result = airi_fd_read(&p->files, (unsigned)fd, buffer, bytes, positioned, offset); // EOF和暂不可读分开。
    if (result > 0 && !copy_user(p, buffer, address, (size_t)result, true)) // 同一次不可抢占重试已经检查映射，本次复制前不能改变。
        airi_panic("validated file output mapping changed"); // 内部错误不丢字节后假装成功。
    return result; // 完成前缀或明确负错误。
}
static long vector_transfer(struct user_process *p, uint64_t fd, uint64_t descriptors, uint64_t count, uint64_t offset, bool write) { // 当前只支持可定位文件，一次有界副本。
    if (fd >= AIRI_FD_COUNT) // 在窄化整数前检查完整FD。
        return -9; // -EBADF。
    if (count > AIRI_VECTOR_COUNT || offset > INT64_MAX) // 负count/offset转为无符号后也会在这里拒绝。
        return -22; // -EINVAL，不读取描述或改变文件。
    struct airi_iovec vectors[AIRI_VECTOR_COUNT]; // 当前调用持有的128字节描述快照。
    if (count && !copy_user(p, vectors, descriptors, (size_t)count * sizeof(vectors[0]), false)) // 整个数组必须可读，不在用户库中直接解引用。
        return -14; // -EFAULT，文件尚未操作。
    uint64_t total = 0; // 所有项串联的逻辑长度，不把溢出截成小请求。
    for (unsigned i = 0; i < count; ++i) { // 固定最多八项。
        if (vectors[i].bytes > (uint64_t)INT64_MAX - total) // 返回类型可表达范围在做加法前检查。
            return -22; // -EINVAL，拒绝负数式回绕或不可表示总量。
        total += vectors[i].bytes; // 总长仅决定本次有限前缀。
    }
    size_t bytes = total > AIRI_VECTOR_BYTES ? AIRI_VECTOR_BYTES : (size_t)total; // 长向量也只发起一次至多256字节操作。
    size_t planned = 0; // 本次会触及的前缀，越过256后的地址不访问。
    for (unsigned i = 0; i < count && planned < bytes; ++i) { // 先验证全部实际前缀，再进行任何I/O或输出。
        size_t part = vectors[i].bytes > bytes - planned ? bytes - planned : (size_t)vectors[i].bytes; // 一项可能只使用前半。
        if (part && !user_access(p, vectors[i].address, part, !write)) // 写文件读用户，读文件写用户；零长不检查无意义基址。
            return -14; // -EFAULT，前面合法项也未产生文件或输出副作用。
        planned += part; // 不用地址加长度形成可能回绕的右端。
    }
    uint8_t buffer[AIRI_VECTOR_BYTES]; // 同步调用内聚合/分散，不给设备DMA，不跨返回保留。
    size_t copied = 0; // gather按数组顺序形成同一文件操作的连续输入。
    if (write) // preadv不读取目标用户缓冲内容。
        for (unsigned i = 0; i < count && copied < bytes; ++i) { // 只聚合本次已验证前缀。
            size_t part = vectors[i].bytes > bytes - copied ? bytes - copied : (size_t)vectors[i].bytes; // 当前项实际长度。
            if (part && !copy_user(p, buffer + copied, vectors[i].address, part, false)) // 映射在当前单CPU不可抢占内核调用中保持。
                airi_panic("validated vector input changed"); // 不变量破坏不能交出半份文件操作。
            copied += part; // 下一项紧随上一个实际字节。
        }
    long result = write ? airi_fd_write(&p->files, (unsigned)fd, buffer, bytes, true, offset) : airi_fd_read(&p->files, (unsigned)fd, buffer, bytes, true, offset); // 仅调用一次文件层，显式偏移不改共享位置。
    if (!write && result > 0) { // 只分散实际读到的字节，不用零覆盖尚未完成尾部。
        copied = 0; // 从第一个目标重新开始。
        for (unsigned i = 0; i < count && copied < (size_t)result; ++i) { // 本轮返回数不超过已验证256字节。
            size_t part = vectors[i].bytes > (size_t)result - copied ? (size_t)result - copied : (size_t)vectors[i].bytes; // EOF可能在项中间结束。
            if (part && !copy_user(p, buffer + copied, vectors[i].address, part, true)) // 描述快照不受输出覆盖用户数组影响。
                airi_panic("validated vector output changed"); // 不能消费文件后掩盖复制失败。
            copied += part; // 未触及目标保留原有内容。
        }
    }
    return result; // 实际短数量、EOF0或原始负错误，不伪造全部完成。
}
static void complete_io(struct user_execution *execution, long result) { // 普通服务或wake完成同一条已登记的等待。
    execution->frame->rax = (uint64_t)result; // 原系统调用返回位置不变，只交付这次结果。
    execution->join_tid = 0; // 旧join身份不再授权收取。
    execution->io = IO_NONE; // 解除用户地址和条件的借用。
    execution->io_fd = execution->io_events = 0; // 不让下次操作继承旧FD条件。
    execution->io_address = execution->io_bytes = execution->io_deadline = 0; // 丢弃旧对象身份和截止。
    execution->state = USER_RUNNABLE; // 返回值完整后才允许重新选中。
    ++io_wake_count; // 实际结束一次阻塞，包括错误和超时。
    airi_log("[WAIT_WAKE] pid=%u result=%ld", execution->process->pid, result); // 不在IRQ中打印或复制。
}
static long futex_wake(uint64_t address, uint64_t count) { // 私有键由进程对象和用户虚拟地址共同组成。
    if ((address & 3) || count > EXECUTION_COUNT) // 必须四字节对齐，完整64位数量不能窄化。
        return -22; // -EINVAL，尚未唤醒任何等待者。
    if (!user_access(current, address, sizeof(uint32_t), false)) // wake虽不读写字值，也要求当前地址有效。
        return -14; // -EFAULT，不能用内核地址匹配某个队列。
    uint64_t now = airi_timer_ticks(); // 同一次wake内采用固定截止快照。
    unsigned woken = 0; // 仅成功wake的数量，不包含过期清理。
    for (unsigned i = 0; i < EXECUTION_COUNT && woken < count; ++i) { // 最多扫描十六槽，无额外分配。
        struct user_execution *target = execution_at(i); // 借当前执行表中的稳定槽。
        if (target->process != current || target->state != USER_IO_WAIT || target->io != IO_FUTEX || target->io_address != address) // fork后的相同VA属于不同进程键。
            continue; // 不影响别的地址、进程或等待种类。
        if (now >= target->io_deadline) { // 到期与通知相遇时先给出超时结果。
            complete_io(target, -110); // -ETIMEDOUT，不能算作成功唤醒。
            continue; // 仍可唤醒后续未到期等待者。
        }
        complete_io(target, 0); // 不修改用户字，也不替用户取得互斥锁。
        ++woken; // 仅累计原来确实已经阻塞的执行者。
    }
    airi_log("[FUTEX_WAKE] pid=%u address=0x%llx count=%llu woken=%u", current->pid, (unsigned long long)address, (unsigned long long)count, woken); // 明确私有进程与实际数量。
    return woken; // 0表示此刻没有匹配等待，不保存通知信用。
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
        } else if (execution->io == IO_FUTEX) { // 内存字变化不会自动结束等待，必须显式wake或截止。
            if (airi_timer_ticks() < execution->io_deadline) // 未到期只保留登记，不重新读取条件字。
                continue; // 用户需要先发布条件，再调用wake。
            result = -110; // -ETIMEDOUT，不伪造成功通知。
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
        complete_io(execution, result); // 所有等待通过同一出口解除条件并发布结果。
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
    for (unsigned i = 0; i < WORKER_COUNT; ++i) // 退出/失败整组清理只归还本组拥有的额外执行链。
        if (workers[i].process == p) // 空槽或其他进程的执行者不能被误收。
            destroy_execution(&workers[i]); // 调用者已离开目标组的全部内核栈。
    destroy_memory(&p->memory); // 用户栈/TLS缓存页面也随进程内存一次归还。
    destroy_execution(&p->execution); // CPU状态页和陷入栈分别归执行者，最后统一撤掉恢复资格。
    airi_memset(p, 0, sizeof(*p)); // 不留下可重复释放的身份或其他句柄。
}
static void initial_frame(struct user_process *p, uint64_t entry, const char *name) { // 输入页已完整映射并清零，最后才发布首次返回现场。
    uint64_t stack_pointer = USER_STACK + 4096 - 16; // 旧受控汇编映像不读取argc，保持原布局。
    if (name) { // C程序使用本项目最小argc/argv栈协议。
        size_t bytes = airi_strlen(name) + 1; // 包含用户字符串的终止零。
        if (bytes > 64) // 当前程序名是固定短字面量，不支持无界参数。
            airi_panic("initial program name exceeds stack budget"); // 不覆盖用户启动布局。
        uint64_t physical; // 当前根未激活，通过软件翻译找到用户栈数据页。
        if (!airi_vm_lookup(&p->memory.space, USER_STACK, &physical, NULL)) // 栈必须已建立。
            airi_panic("initial user stack mapping is missing"); // 不猜测owned数组中的位置。
        uint8_t *alias = airi_memory_direct(physical); // 通过内核HHDM填写尚未运行的用户栈。
        size_t name_offset = 4096 - bytes; // 字符串放在栈页高端。
        airi_memcpy(alias + name_offset, name, bytes); // 用户只得到自己的字符串副本。
        size_t words_offset = (name_offset & ~(size_t)15) - 4 * sizeof(uint64_t); // 四个64位槽，入口RSP仍16字节对齐。
        uint64_t *words = (uint64_t *)(alias + words_offset); // 已对齐的栈内数组。
        words[0] = 1; // argc目前固定一个程序名。
        words[1] = USER_STACK + name_offset; // argv[0]必须是用户VA，不是HHDM指针。
        words[2] = 0; // argv[1]结束指针数组。
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
    struct airi_elf_view view; // 只借输入ELF直到复制结束。
    const char *error = airi_elf_inspect(image, bytes, &view); // 在任何资源分配前校验整个输入。
    if (error) { // 损坏格式不能触发后续内存写入。
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
    p->tls = view.tls; // 保存已校验模板布局，不能从后来修改过的主TLS复制初值。
    p->tls_image = view.image; // 初始ELF字节由内核只读目录长期持有。
    initial_frame(p, view.entry, name); // 用户_start接收最小argc/argv协议。
    airi_log("[ELF_LOAD] entry=0x%llx segments=%u data_pages=%u argc=1", (unsigned long long)view.entry, view.count, p->memory.count); // 不含页表与内核栈的用户数据页数量。
    return 0; // 只准备新空间，尚未切CR3或覆盖旧现场。
failed: // 长期根仍活动，用户空间从未开始执行。
    cleanup(p); // 页表、用户数据和内核栈全部回收。
    return -12; // -ENOMEM，不提供部分映像。
}
static void place_frame(struct user_execution *execution) { // 初始寄存器值完成后复制到自己拥有的内核栈。
    uint64_t top = execution->stack.virtual_address + execution->stack.count * 4096ull; // TSS以后从同一个栈顶压入真实现场。
    execution->frame = (void *)(uintptr_t)(top - sizeof(*execution->frame)); // 与公共入口的176字节布局一致。
    *execution->frame = execution->initial; // 不借用临时C结构或其他进程的内核栈。
}
static bool prepare_flat(struct user_process *p, const uint8_t *image, size_t bytes) { // 保留受控指令故障回归，不作为ELF加载接口。
    if (!bytes || bytes > 4096 || !create_stack(&p->execution, slot(p))) // 本节旧映像最多一页。
        return false; // 栈创建失败已回滚。
    if (!create_fpu(&p->execution) || !airi_memory_user_space(&p->memory.space) || !owned_page(&p->memory, USER_CODE, AIRI_VM_EXEC) || !owned_page(&p->memory, USER_STACK, AIRI_VM_WRITE)) // 分别拥有根、代码和栈。
        goto failed; // 所有权列表可清理全部已成功部分。
    airi_memcpy(airi_memory_direct(owned_at(&p->memory, 0)->physical), image, bytes); // 映像内引用保持旧固定布局。
    initial_frame(p, USER_CODE, NULL); // 受控汇编不读取argc/argv。
    return true; // 尚未设置可运行状态。
failed: // 目标从未被调度，当前根和栈均不属于它。
    cleanup(p); // 完整回滚。
    return false; // 不运行半个上下文。
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
static void collect_pending(void) { // 只在新的用户陷入入口调用，不在旧进程退出的C调用中释放其栈。
    for (unsigned i = 0; i < WORKER_COUNT; ++i) // 先收拢已经join且不在当前栈上的工作者。
        if (&workers[i] != running && workers[i].state == USER_REAP_PENDING) // 退出的当前内核调用链尚不能归还。
            destroy_execution(&workers[i]); // 用户栈/TLS映射缓存继续由进程拥有。
    for (unsigned i = 0; i < PROCESS_COUNT; ++i) // 回收父进程已取得状态的孩子。
        if (&processes[i] != current && processes[i].execution.state == USER_REAP_PENDING) { // 当前真正运行的内核栈不能在此回收。
            unsigned pid = processes[i].pid; // cleanup清零控制块前保存诊断身份。
            bool orphan = processes[i].parent == 0; // 用户wait回收与内核托管回收分开观察。
            cleanup(&processes[i]); // 非活动根和已离开的内核栈可安全解除。
            if (orphan) // 不把普通父wait冒充自动托管。
                airi_log("[ORPHAN_REAP] pid=%u released=1", pid); // 只在内存和FD资源实际释放后发布。
        }
}
enum schedule_reason { REASON_TIMER, REASON_YIELD, REASON_WAIT, REASON_EXIT, REASON_IO }; // 原因用于验证真实抢占，不混成主动yield。
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
static struct airi_trap_frame *pause_for_service(bool from_timer) { // 完整用户frame已保存，暂回长期启动栈做可延后工作。
    session_idle = true; // 会话仍active，不是用户程序已经退出。
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
    airi_memory_use_kernel(); // 最后一个用户根也变成不活动。
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
    child->async_owned = true; // 成功后才记资源所有权。
    place_frame(&child->execution); // 只把用户返回现场放入子内核栈，不复制父的内核C调用链。
    child->execution.state = USER_RUNNABLE; // 准备完成后才让IRQ调度器看见它。
    airi_log("[FORK] parent=%u child=%u copied_pages=%u", parent->pid, child->pid, child->memory.count); // 实际复制数量含用户栈与堆。
    return child->pid; // 父分支得到正子PID。
failed: // 还没有向调度器发布孩子，当前仍在父根和父内核栈上。
    cleanup(child); // 删除不活动子根和其独立栈，父继续原系统调用。
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
static long allocate_user(size_t bytes) { // 按页提供有界用户内存，不是完整malloc。
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
            goto rollback; // 游标尚未前移。
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
    if (!airi_memory_return_page(old_fpu)) // 硬件随后从新状态页恢复，不再借旧页指针。
        airi_panic("cannot reclaim replaced user FPU state"); // 内部归还错误不能伪装成普通exec失败。
    airi_log("[EXEC_COMMIT] pid=%u program=%s old_pages=%u new_pages=%u", current->pid, name, old_pages, current->memory.count); // 名称是内核副本，不引用刚释放用户页。
    return 0; // 调用者直接恢复新f，不把这个0写回旧用户调用。
}
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
    if (!session_active || !current || !running || running->state != USER_RUNNING || (f->cs & 3) != 3) // 当前实际栈必须有运行所有者。
        airi_panic("CPU state without a running user"); // 内核int129不经过这个入口。
    running->fs_base = airi_tls_take(); // 先取真实用户基址，再把硬件FS清零供内核处理。
    airi_fpu_save(airi_memory_direct(running->fpu_physical)); // x87/XMM也在此唯一入口保存，不能因设备没有新ISR而跳过。
}
struct airi_trap_frame *airi_user_resume(struct airi_trap_frame *f) { // 所有用户iretq路径使用同一规范地址和标志策略。
    if (f->cs != 27 || f->ss != 35 || f->rip >= AIRI_USER_LIMIT || f->rsp >= AIRI_USER_LIMIT) { // 坏返回不能变成内核GP。
        airi_log("[USER_RETURN_REJECTED] pid=%u rip=0x%llx rsp=0x%llx", current->pid, (unsigned long long)f->rip, (unsigned long long)f->rsp); // 不解引用错误地址。
        return exit_current(-22); // 只结束拥有这个现场的用户。
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
    if (f->rax == 1 || f->rax == 10 || f->rax == 15 || f->rax == 16) { // 同步文件与流的完整副本边界。
        bool write = f->rax == 1 || f->rax == 16; // 输出到流或文件。
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
    } else if (f->rax == 2) // 退出后不再恢复此用户指令。
        return exit_current((int)(f->rdi & 255)); // 普通退出码0–255。
    else if (f->rax == 3) // getpid查询当前身份。
        f->rax = current->pid; // 槽复用不改变已运行PID语义。
    else if (f->rax == 4) // 显式yield让出一次时间片。
        return schedule(REASON_YIELD); // 和真实timer抢占分开计数。
    else if (f->rax == 5) // 单调tick，不是墙钟。
        f->rax = airi_timer_ticks(); // IRQ在用户执行期间更新。
    else if (f->rax == 6) // fork复制所有用户页与返回现场。
        f->rax = (uint64_t)fork_current(f); // 父得到子PID，孩子自己的frame已经设为0。
    else if (f->rax == 7) { // exec失败保留旧现场，成功已把f替换成新入口。
        long result = exec_current(f); // 名称复制与新空间事务在不可抢占内核完成。
        if (result < 0) // 只有失败才向旧用户调用返回错误。
            f->rax = (uint64_t)result; // PID和旧地址空间仍保持。
        return airi_user_resume(f); // 成功恢复新_start，失败恢复旧调用下一条指令。
    } else if (f->rax == 8) // wait可直接回收或真的阻塞。
        return wait_current(f); // 结果恢复到父同一次调用之后。
    else if (f->rax == 9) // 申请零填充用户页。
        f->rax = (uint64_t)allocate_user((size_t)f->rdi); // 不成功不前移堆游标。
    else if (f->rax == 11) { // open只消费内核持有的短路径副本。
        char path[32]; // 与平坦目录名容量一致。
        bool copied = false; // 必须看到终止零才可交给文件层。
        for (size_t i = 0; i < sizeof(path); ++i) { // 逐字节检查，不能strlen用户指针。
            if (!copy_user(current, &path[i], f->rdi + i, 1, false)) // 未映射或内核地址拒绝。
                break; // 不发生创建或截断。
            if (!path[i]) { // 有界NUL字符串完成。
                copied = true; // 副本可在当前内核调用中使用。
                break; // 不额外读取终止零后面的用户页。
            }
        }
        f->rax = copied && f->rsi <= 31 ? (uint64_t)airi_fd_open(&current->files, path, (unsigned)f->rsi) : (uint64_t)-14ll; // 错误路径不变更文件。
    } else if (f->rax == 12) // close撤销当前表项引用。
        f->rax = (f->rdi >= AIRI_FD_COUNT ? (uint64_t)-9ll : fd_waited(current, f->rdi) ? (uint64_t)-16ll : (uint64_t)airi_fd_close(&current->files, (unsigned)f->rdi)); // 不删除文件内容。
    else if (f->rax == 13) // dup共享打开对象，标志放在新表项。
        f->rax = (f->rdi < AIRI_FD_COUNT ? (uint64_t)airi_fd_dup(&current->files, (unsigned)f->rdi, f->rsi != 0) : (uint64_t)-9ll); // 单CPU内核中同步加引用。
    else if (f->rax == 14) // 仅绝对定位，不实现Linux全部whence。
        f->rax = (f->rdi < AIRI_FD_COUNT ? (uint64_t)airi_fd_seek(&current->files, (unsigned)f->rdi, f->rsi) : (uint64_t)-9ll); // 共享打开对象的偏移随之变化。
    else if (f->rax == 17 || f->rax == 18) { // 日志append与read不是通用文件ABI。
        uint8_t buffer[480]; // payload上限与磁盘格式一致，设备不直接访问用户页。
        long result; // 实际长度、sequence或负错误。
        if (f->rax == 17) { // RDI输入地址，RSI长度。
            if (!f->rsi || f->rsi > sizeof(buffer)) // 不创建空记录或无界内核副本。
                result = -22; // -EINVAL。
            else if (!copy_user(current, buffer, f->rdi, (size_t)f->rsi, false)) // 完整复制后才可能产生磁盘副作用。
                result = -14; // -EFAULT。
            else { // 设备同步等待临时开IRQ，但不会在CS0切用户。
                airi_log("[STORAGE_REQUEST] pid=%u operation=append bytes=%llu wait=kernel", current->pid, (unsigned long long)f->rsi); // 诚实区分同步内核等待与后续用户阻塞。
                result = airi_storage_append(buffer, (size_t)f->rsi); // 成功已完成数据和提交同步。
            }
        } else { // RDI索引，RSI用户输出，RDX容量。
            if (f->rdi >= 64 || f->rdx > sizeof(buffer)) // 完整64位索引不能截断成其他记录。
                result = -22; // -EINVAL。
            else if (!user_access(current, f->rsi, (size_t)f->rdx, true)) // 先验证完整用户输出范围。
                result = -14; // -EFAULT，不进行设备I/O。
            else { // 仅将校验通过的实际长度复制到用户。
                result = airi_storage_read((unsigned)f->rdi, buffer, (size_t)f->rdx); // 设备数据和commit重新核对。
                if (result > 0 && !copy_user(current, buffer, f->rsi, (size_t)result, true)) // 当前映射不应变化。
                    airi_panic("validated storage output mapping changed"); // 内部不变量破坏明确停止。
            }
        }
        f->rax = (uint64_t)result; // 同一次用户调用取得精确结果。
    } else if (f->rax == 19) // 已恢复前缀数值查询。
        f->rax = (uint64_t)airi_storage_count(); // 缺设备或不确定状态返回负数。
    else if (f->rax == 20) { // pipe原子创建并返回两个整数，不暴露内部环。
        unsigned ends[2]; // 内核副本，发布前用户输出已验证。
        long result; // 0或负错误。
        if (f->rsi > 1) // 明确接受阻塞0与非阻塞1，其他参数拒绝。
            result = -22; // -EINVAL。
        else if (!user_access(current, f->rdi, sizeof(ends), true)) // 坏输出不能创建无法交给用户的FD。
            result = -14; // -EFAULT。
        else { // 没有半次FD分配的用户可见状态。
            result = airi_fd_pipe(&current->files, ends, f->rsi != 0); // 两端和缓冲都齐后才成功。
            if (!result && !copy_user(current, ends, f->rdi, sizeof(ends), true)) // 映射在本次调用中不应变化。
                airi_panic("validated pipe output mapping changed"); // 明确内核不变量。
        }
        f->rax = (uint64_t)result; // 失败输出保持原值。
    } else if (f->rax == 21) // 当前就绪条件查询，不保存通知事件。
        f->rax = f->rdi < AIRI_FD_COUNT ? (uint64_t)airi_fd_ready(&current->files, (unsigned)f->rdi) : (uint64_t)-9ll; // 完整64位编号先限制。
    else if (f->rax == 22) { // 等待管道条件，有限tick超时，0表示只查询。
        long result; // 事件掩码、0超时或负错误。
        if (f->rdi >= AIRI_FD_COUNT || !f->rsi || (f->rsi & ~3ull) || f->rdx > 500) // 只接受READ/WRITE请求，最多五秒。
            result = -22; // -EINVAL。
        else { // 先看当前条件，避免已经满足还去睡眠。
            result = airi_fd_ready(&current->files, (unsigned)f->rdi); // FD错误原样返回。
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
        f->rax = (uint64_t)result; // 立即就绪、纯查询、错误统一返回。
    }
    else if (f->rax == 23) // 固定注册缓冲申请，不返回内核地址。
        f->rax = (uint64_t)airi_async_buffer_alloc(current->pid); // 正handle或负错误。
    else if (f->rax == 24 || f->rax == 25) { // 注册缓冲与用户范围之间同步复制。
        uint8_t buffer[512]; // 设备不访问这个内核栈副本，之后另由请求单元持有。
        bool put = f->rax == 24; // put用户输入，get用户输出。
        long result; // 0或明确错误，字节数由已验证请求长度决定。
        if (f->rdx > sizeof(buffer)) // 完整64位长度先限制。
            result = -22; // -EINVAL。
        else if (!user_access(current, f->rsi, (size_t)f->rdx, !put)) // 输出必须可写，输入只需可读。
            result = -14; // -EFAULT，没有引擎副作用。
        else if (put) { // 输入先复制完再可能覆盖注册对象。
            copy_user(current, buffer, f->rsi, (size_t)f->rdx, false); // 不把用户VA交给资源引擎。
            result = airi_async_buffer_put(current->pid, f->rdi, buffer, (size_t)f->rdx); // BUSY拒绝，尾部清零。
        } else { // get先检查owner、handle和IDLE。
            result = airi_async_buffer_get(current->pid, f->rdi, buffer, (size_t)f->rdx); // 不收集CQ不能读取BUSY。
            if (!result && !copy_user(current, buffer, f->rsi, (size_t)f->rdx, true)) // 映射已完整验证且当前不可抢占。
                airi_panic("validated registered-buffer output changed"); // 内部错误不能静默复制。
        }
        f->rax = (uint64_t)result; // 用户自己判断资源状态。
    } else if (f->rax == 26) // 注册buffer释放不等同用户堆free。
        f->rax = (uint64_t)airi_async_buffer_free(current->pid, f->rdi); // 完整代数句柄不截断。
    else if (f->rax == 27) { // 提交描述全部复制后才接受进SQ。
        struct airi_io_submission submission; // 固定16字节无指针记录。
        if (f->rdi != AIRI_IO_READ && f->rdi != AIRI_IO_WRITE && f->rdi != AIRI_IO_FLUSH) // 完整64位操作先验证，不能截断成合法编号。
            f->rax = (uint64_t)-22ll; // 格式错误无CQ。
        else if (!copy_user(current, &submission, f->rdx, sizeof(submission), false)) // sector和cookie都由当前用户副本给出。
            f->rax = (uint64_t)-14ll; // 未接受，没有缓冲所有权变化。
        else // 已复制的数值交给资源引擎，不借用户提交结构。
            f->rax = (uint64_t)airi_async_submit(current->pid, (unsigned)f->rdi, f->rsi, &submission); // 0仅接收，I/O结果以后由CQ给。
    } else if (f->rax == 28) { // CQ收集可以非阻塞或保持原调用等待。
        struct airi_io_completion completion; // 先到内核数值副本。
        long result; // 0交出一条CQ，-11空，其他负错误。
        if (f->rsi > 1) // 只接受0不等、1阻塞。
            result = -22; // -EINVAL。
        else if (!user_access(current, f->rdi, sizeof(completion), true)) // 必须在消费CQ之前验证完整用户输出。
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
            }
        }
        f->rax = (uint64_t)result; // 具体I/O错误在completion.result，不是这层0。
    } else if (f->rax == 29) { // 有界只读状态供用户与未来呈现消费者观察。
        struct airi_io_state state; // 不含内核地址或用户buffer指针。
        long result; // 当前轮次状态或负错误。
        if (!user_access(current, f->rdi, sizeof(state), true)) // 输出先验证，快照不会消耗资源。
            result = -14; // -EFAULT。
        else { // 当前PID由内核决定，用户不能查询或修改另一个owner。
            result = airi_async_state(current->pid, &state); // 包括同PID旧轮次待收集的ACTIVE数量。
            if (!result && !copy_user(current, &state, f->rdi, sizeof(state), true)) // 当前映射应保持稳定。
                airi_panic("validated I/O state output changed"); // 明确内部失败。
        }
        f->rax = (uint64_t)result; // 用户看到的只是本次时刻数值。
    }
    else if (f->rax == 30) { // 目录只返回有界文本，不暴露ELF或内核对象地址。
        char names[512]; size_t used = 0; // 当前固定目录完整小于此上限。
        for (unsigned i=0; i<sizeof(executables)/sizeof(executables[0]); ++i) { // 结果与exec使用同一真实注册表。
            size_t n=airi_strlen(executables[i].name); // 名称包含内部演示目标，用户仍可选择尝试。
            if (used+n+2>sizeof(names)) // 防止以后目录增长造成栈覆盖。
                airi_panic("program catalog exceeds bounded ABI"); // 维护者必须扩大显式上限。
            airi_memcpy(names+used,executables[i].name,n); used+=n; // 复制有效名称前缀。
            names[used++]='\n'; // 每行一个名字，界面自己呈现。
        }
        names[used]=0; // 查询返回含NUL的完整文本，不返回截断目录。
        if (f->rsi < used+1) // 小容量不能写一半。
            f->rax=(uint64_t)-28ll; // -ENOSPC。
        else if (!copy_user(current,names,f->rdi,used+1,true)) // 验证实际输出跨度。
            f->rax=(uint64_t)-14ll; // -EFAULT。
        else // 完整复制后才发布有效长度。
            f->rax=used; // 不含末尾NUL，与strlen一致。
    }
    else if (f->rax == 31) // 只归还调用者自己的动态页面，不能释放ELF或初始栈。
        f->rax = (uint64_t)release_user(f->rdi, f->rsi); // 负参数错误保持全部原映射。
    else if (f->rax == 32) { // 单调tick睡眠，复用持久等待现场而非用户忙等。
        uint64_t now = airi_timer_ticks(); // 截止以真实PIT事件计数为基准。
        if (f->rdi > 6000) // 一次最多6000tick（名义六十秒），完整64位参数先验证。
            f->rax = (uint64_t)-22ll; // -EINVAL，不改变运行状态。
        else if (!f->rdi) // 零长度是立即成功，不登记等待。
            f->rax = 0; // 和主动yield分开。
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
        }
    }
    else if (f->rax == 33) { // 自有FS基址设置，只验证数值，不固定或分配其目标页。
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
        if (f->rdi >= AIRI_FD_COUNT) // 完整64位编号不能截成另一个合法FD。
            f->rax = (uint64_t)-9ll; // -EBADF。
        else if (f->rdx > 2) // 原始调用也不能靠窄化whence绕过检查。
            f->rax = (uint64_t)-22ll; // -EINVAL。
        else // RSI位模式在当前二补数x86-64目标解释为有符号偏移。
            f->rax = (uint64_t)airi_fd_seek_from(&current->files, (unsigned)f->rdi, (int64_t)f->rsi, (unsigned)f->rdx); // 一次读取基准、验证并提交，旧14号绝对接口保留。
    }
    else if (f->rax == 38) { // 只提供PIT单调时钟，不伪造RTC墙钟。
        if (f->rdi != AIRI_CLOCK_MONOTONIC) // 完整参数不截成另一个合法编号。
            f->rax = (uint64_t)-22ll; // -EINVAL，输出保持。
        else { // tick仅由真实IRQ增加，同一CPU读出一次完整快照。
            uint64_t ticks = airi_timer_ticks(); // 基准是本内核PIT初始化，不含固件启动时间。
            struct airi_time_value value = {ticks / 100, (ticks % 100) * 10000000ull}; // 名义100Hz换算；纳秒单位不提高实际分辨率。
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
    else if (f->rax == 43) { // 自有私有futex等待，超时单位为名义PIT tick，不是Linux ABI。
        uint32_t value; // 本次比较取得四字节数值副本，不跨阻塞保留HHDM指针。
        uint64_t now = airi_timer_ticks(); // 截止只计算一次，不因轮询延长。
        if ((f->rdi & 3) || f->rsi > UINT32_MAX || f->rdx > 6000) // 对齐、期望值宽度与有界超时先完整检查。
            f->rax = (uint64_t)-22ll; // -EINVAL，尚未读取或登记。
        else if (!copy_user(current, &value, f->rdi, sizeof(value), false)) // 以进程页表验证整个四字节读范围。
            f->rax = (uint64_t)-14ll; // -EFAULT，内核不直接解引用用户VA。
        else if (value != (uint32_t)f->rsi) // 另一执行者若已发布新值，旧等待决定已失效。
            f->rax = (uint64_t)-11ll; // -EAGAIN，立即返回用户重查条件。
        else if (!f->rdx) // 值相等但不给等待预算，也不能登记一个永不到期项。
            f->rax = (uint64_t)-110ll; // -ETIMEDOUT。
        else if (now > UINT64_MAX - f->rdx) // 截止加法不能回绕。
            f->rax = (uint64_t)-75ll; // -EOVERFLOW。
        else { // 当前固定单CPU、内核不可抢占；比较到登记之间没有其他用户store或wake。
            running->io = IO_FUTEX; // 等待身份属于执行者，不属于共享进程本身。
            running->io_fd = running->io_events = 0; // 内存等待不占FD。
            running->io_address = f->rdi; // 保存用户VA；进程指针已由execution持有。
            running->io_bytes = 0; // 不保存旧字值；醒来由用户重查同步协议。
            running->io_deadline = now + f->rdx; // 到期由普通服务栈结束原调用。
            running->state = USER_IO_WAIT; // 条件完整后再从可运行集合撤销。
            ++io_block_count; // 只记录真实阻塞，不计EAGAIN。
            airi_log("[FUTEX_WAIT] pid=%u tid=%u address=0x%llx expected=%u", current->pid, running->tid, (unsigned long long)f->rdi, value); // 明确本次登记的实际身份。
            return schedule(REASON_IO); // 其他执行者继续；全阻塞时回启动栈hlt。
        }
    }
    else if (f->rax == 44) // 显式通知至多给定数量，不保存信号量计数。
        f->rax = (uint64_t)futex_wake(f->rdi, f->rsi); // 不修改用户字，返回真正唤醒的数量。
    else // 尚未提供的接口都明确拒绝。
        f->rax = (uint64_t)-38ll; // -ENOSYS。
    poll_waiters(); // 文件/流修改完成后立即重查其他等待者，不等用户轮询。
    return airi_user_resume(f); // 普通调用继续同一用户，仍检查RSP与标志。
}
struct user_result { int code; unsigned ticks, switches, blocks; uint64_t fault_error; unsigned io_blocks, io_wakes, idle_halts; }; // 启动任务只取得数值，不借已回收控制块。
static int launch(const uint8_t *image, size_t bytes, const char *name, bool elf, struct user_result *result) { // 可用于启动演示或串口命令，程序失败不等于内核失败。
    if (session_active) // 当前只有一个由启动栈收拢的用户会话。
        return -16; // -EBUSY，不能覆盖已有kernel_return。
    if (next_pid == UINT32_MAX) // 不能把回绕后的0当合法新owner。
        return -75; // -EOVERFLOW，尚未分配任何资源。
    struct airi_memory_stats before, after; // 包含根、所有孩子和内核栈。
    airi_memory_snapshot(&before); // 在任何资源分配前取基准。
    struct user_process *root = unused(); // 上次会话已回收的空槽。
    if (!root) // 仍有未回收身份就明确失败。
        return -11; // -EAGAIN。
    int error = elf ? prepare_elf(root, image, bytes, name, true) : (prepare_flat(root, image, bytes) ? 0 : -12); // ELF保留格式与资源错误之别。
    if (error) { // 失败构造也必须完整恢复资源。
        airi_memory_snapshot(&after); // 不能只看错误码忽略泄漏。
        if (!airi_memory_same_usage(&before, &after) || airi_files_open_count()) // 半份新映像不应改变空闲数量。
            airi_panic("failed user launch leaked resources"); // 内部错误与普通坏文件分开。
        return error; // 调用者可以继续命令，不停机。
    }
    airi_fd_init(&root->files); // 初始stdout/stderr归本次根进程持有。
    bool direct_shell = name && airi_strlen(name)==2 && !airi_memcmp(name,"sh",2); // init自身不读UART，只监督自己的shell孩子。
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
    airi_memory_snapshot(&after); // 全部页和栈回收后验证。
    if (!airi_memory_same_usage(&before, &after) || airi_files_open_count()) // 用户程序退出码可以任意，但内核资源必须恢复。
        airi_panic("user process session leaked memory"); // 不把泄漏当成应用错误跳过。
    return 0; // 表示已完整启动与收拢，程序结果另在result.code。
}
static void run_one(const uint8_t *image, size_t bytes, const char *name, int expected, bool elf, bool require_ticks, bool require_scheduler) { // 作者启动演示使用具体预期，不控制普通命令的合法退出码。
    struct user_result result; // 会话结束只消费数值副本。
    if (launch(image, bytes, name, elf, &result) || result.code != expected || (require_ticks && result.ticks < 2) || (expected == -142 && result.fault_error != 5) || (require_scheduler && (!result.switches || !result.blocks))) // 同时验证真正执行、故障、抢占和等待。
        airi_panic("user session ended without expected semantics"); // 固定演示有误才拒绝发布READY。
    if (require_scheduler) // 实际多用户会话单独报告行为证据。
        airi_log("[PROCESS_DEMO_OK] fork=copy exec=transaction wait=blocked timer_switches=%u wait_blocks=%u reclaimed=1", result.switches, result.blocks); // 计数来自真实IRQ与状态转换。
}
void airi_user_init(void) { // 启动只调用一次；不依赖先运行任何用户演示。
    airi_files_init(); // 为普通用户与init同样建立只读seed和空RAM目录。
    airi_log("[USER_RESOURCES_READY] slots=%u elf=static", PROCESS_COUNT); // 仅声明资源可用，不宣称已经运行服务。
}
int airi_user_boot(const char *name) { // 输入来自内核固定启动策略，不接受外部未验证指针。
    const struct executable *image = find_program(name); // 与命令入口使用同一真实映像目录。
    if (!image) // 不存在程序时尚未改变任何进程资源。
        return -2; // -ENOENT，交给启动恢复策略。
    struct user_result result; // 持续会话停止后才取得根退出状态。
    int error = launch(image->start, (size_t)((uintptr_t)image->end - (uintptr_t)image->start), name, true, &result); // 准备失败与正常返回均由launch核对回收。
    return error ? error : result.code; // 初始化失败或用户退出都不能被当作仍在服务。
}
void airi_user_demo(void) { // 串口主线的真实用户边界与多进程回归。
    // 用户资源已由airi_user_init建立，演示不重置正式初始化状态。
    run_one(airi_user_probe_start, (size_t)((uintptr_t)airi_user_probe_end - (uintptr_t)airi_user_probe_start), NULL, 0, false, true, false); // 原有非法指针与用户IRQ验证。
    run_one(airi_user_bad_start, (size_t)((uintptr_t)airi_user_bad_end - (uintptr_t)airi_user_bad_start), NULL, -141, false, false, false); // 用户cli结束目标进程，内核继续。
    run_one(airi_user_load_bad_start, (size_t)((uintptr_t)airi_user_load_bad_end - (uintptr_t)airi_user_load_bad_start), NULL, -142, false, false, false); // USER保护PF。
    run_one(airi_user_rsp_bad_start, (size_t)((uintptr_t)airi_user_rsp_bad_end - (uintptr_t)airi_user_rsp_bad_start), NULL, -22, false, false, false); // 坏返回栈不击穿内核。
    run_one(airi_hello_elf_start, (size_t)((uintptr_t)airi_hello_elf_end - (uintptr_t)airi_hello_elf_start), "hello", 0, true, false, false); // 独立C和自有用户库。
    run_one(airi_process_elf_start, (size_t)((uintptr_t)airi_process_elf_end - (uintptr_t)airi_process_elf_start), "processes", 0, true, false, true); // 真实多进程复制、抢占、阻塞与回收。
    run_one(airi_files_elf_start, (size_t)((uintptr_t)airi_files_elf_end - (uintptr_t)airi_files_elf_start), "files", 0, true, false, false); // 文件内容、共享偏移与exec引用回收。
    run_one(airi_pipes_elf_start, (size_t)((uintptr_t)airi_pipes_elf_end - (uintptr_t)airi_pipes_elf_start), "pipes", 0, true, false, false); // 真实FIFO、背压、关闭和fork共享。
    struct user_result waits_result; // 数值证据不借已回收控制块。
    if (launch(airi_waits_elf_start, (size_t)((uintptr_t)airi_waits_elf_end - (uintptr_t)airi_waits_elf_start), "waits", true, &waits_result) || waits_result.code || waits_result.io_blocks < 4 || waits_result.io_wakes < 4 || !waits_result.idle_halts) // 读、写、空闲与纯计算并行超时四种实际等待均须发生。
        airi_panic("blocking I/O did not preserve waiting semantics"); // 不把用户循环重试当内核阻塞。
    airi_log("[WAIT_DEMO_OK] blocked=%u woken=%u idle_halts=%u", waits_result.io_blocks, waits_result.io_wakes, waits_result.idle_halts); // 统计来自真实状态变化。
    struct user_result async_result; // 快设备可能立即有CQ，真实阻塞次数按观察记录。
    if (launch(airi_async_elf_start, (size_t)((uintptr_t)airi_async_elf_end - (uintptr_t)airi_async_elf_start), "async", true, &async_result) || async_result.code) // 正常和受限设备由用户程序明确分支。
        airi_panic("user asynchronous lifecycle demo failed"); // 不拿submit0冒充I/O成功。
    airi_log("[ASYNC_USER_DEMO_OK] blocked=%u woken=%u switches=%u pools=reclaimed", async_result.io_blocks, async_result.io_wakes, async_result.switches); // 静态池已由launch排空并核对。
    airi_log("[USER_DEMO_OK] ring=3 syscall=int80 bad_pointers=2 user_gp=13 user_pf=14 bad_rsp=rejected reclaimed=1"); // 完整会话结束，仍回串口命令。
}

bool airi_user_command(struct airi_console *console, const char *command) { // 系统命令消费者独立于图形主题扩展。
    size_t bytes = airi_strlen(command); // 先取得有效长度，固定前缀比较不能越过短字符串。
    if (bytes == 8 && !airi_memcmp(command, "programs", 8)) { // 显示当前初始ELF目录。
        airi_console_print(console,"programs: registered ELF images"); // 表头不硬编码另一套程序名。
        for (unsigned i=0; i<sizeof(executables)/sizeof(executables[0]); ++i) // 与exec共享真实目录，不维护第二份静态名称。
            airi_console_print(console, "%s", executables[i].name); // broken是明确损坏的演示输入。
        return true; // 已处理，不再交给图形扩展。
    }
    if (bytes < 4 || airi_memcmp(command, "run ", 4)) // 只接受明确的run空格前缀。
        return false; // 其他命令继续原分派链。
    const char *name = command + 4; // 只在本次调用期借控制台的命令副本。
    const struct executable *image = find_program(name); // 目前目录名不是通用文件系统路径。
    if (!image) { // 错别字不影响内核继续输入。
        airi_console_print(console, "program not found: %s", name); // 给出当前名称。
        return true; // 请求已得到明确结果。
    }
#ifdef AIRI_GRAPHICAL_CONSOLE // 当前图形终端尚未接入用户TTY，不能让窗口命令陷入不可见输入等待。
    if ((bytes==6 && !airi_memcmp(name,"sh",2)) || (bytes==8 && !airi_memcmp(name,"init",4))) { // shell及其常驻监督模式都需要实际UART输入。
        airi_console_print(console,"User shell uses UART: start ./airios run, then run sh."); // 给出实际可操作入口。
        return true; // 图形桌面立即继续接收输入。
    }
#endif // 纯串口主线没有此限制。
    struct user_result result; // 子进程都结束后取得数值退出状态。
    int error = launch(image->start, (size_t)((uintptr_t)image->end - (uintptr_t)image->start), name, true, &result); // 同步执行并完整回收，不借回调局部对象给用户。
    if (error) // 例如坏ELF返回-8，不是PANIC。
        airi_console_print(console, "cannot load %s: error=%d", name, error); // 串口详细日志还给出解析原因。
    else // 程序可以合法返回非零，例如worker的23。
        airi_console_print(console, "program %s exited %d", name, result.code); // 不把应用结果当成内核停止。
    return true; // GUI是否存在都不影响服务执行。
}
