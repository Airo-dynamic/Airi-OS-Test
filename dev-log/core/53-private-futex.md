# K53 · 条件仍成立时睡眠，再按地址唤醒

K52 中的工作者已经共享普通内存，却只能用 yield 反复等待 gate。现在让工作者看见 word 为 1 时真正睡眠，主执行者发布新条件后再通知它。调用返回的位置仍在工作者原来的函数里，CPU 可以在它等待期间运行其他执行者，或者在无人可运行时回启动栈等待中断。

<a id="futex-editing"></a>
从 K52 的同一学习目录继续。下面每个 replace 同时列出 before 和 after：先在旧文件中找到完全相同、且只出现一次的 before，再用 after 替换这个窗口；before 只作定位，不重复敲入。未列出的函数和旁注保持原样，找不到或匹配多处时先核对起点，不能任选一处继续。完整修改闭合后再编译。

参考工程的重建器已通过 [T04](../tools/04-exact-edits.md) 支持这项动作；自己跟敲内核时直接完成上述局部编辑即可，不要求先实现作者工具。T04 保持独立附录路线，默认串口构建与测试都不读取它，GUI 也不是前置。

<a id="futex-lost-notification"></a>
## 看见旧条件以后，为什么还不能直接睡下

设共享 word 初始为 1。工作者先读到 1，决定等待；但在它调用内核以前，主执行者已经把 word 改成 2，并发出通知。此时工作者尚未登记，通知唤醒人数为 0。如果工作者接下来无条件睡眠，这次修改以后没有人必须再次通知，它可能一直等到超时。

等待接口因此同时接收 word 的地址和先前观察值 1。内核重新读取这四字节：实际值已是 2，就立即返回 -11，工作者回到用户代码重查条件；实际仍为 1，才登记等待。比较和登记之间不能插入另一个执行者的修改及通知，否则会重新出现同一个空隙。

本章固定单 CPU，内核不抢占用户系统调用，设备中断不运行其他用户代码。进入 43 号调用以后，从复制字值、比较到公开等待状态之间，没有另一个用户执行者运行。这是当前实现让比较与登记不可分割的依据。软件字节复制本身不提供跨 CPU 原子性；以后加入 SMP 时，必须重新保护比较和队列登记，不能只把 CPU 数量改成二。

用户字与等待记录属于两个地方。word 是用户地址空间中四字节对齐的原子对象；登记保存在 user_execution 的 io 类型、用户 VA 和截止 tick 中。这个 VA 不是内核可直接解引用的指针。内核通过所属进程的页表验证并复制值，只保存 VA 数值到等待结束。

<a id="futex-private-key"></a>
## 同一个数值地址，也可能是两份内存

fork 后父子都可以把 word 放在 0x402000 一类相同虚拟地址，但各自根页表指向不同物理页。只比较 VA 会使父进程的通知错误唤醒孩子。因此本章的私有等待键由“所属进程对象、用户 VA”共同确定。同一进程的多个 worker 借同一个 process，能够互相通知；fork 产生的新 process 即使 VA 相同也不匹配。

这项边界不支持跨进程共享内存的 futex，不按物理页做全局键，也不支持 PI、取消或信号。接口使用自有 int80 编号，与 Linux 二进制 ABI 分开。等待只接受 0–6000 个名义 PIT tick；0 预算先比较值，值相等时返回 -110，值不同仍返回 -11。通知接受 0–16，返回实际唤醒数，不积累通知信用。

先修改用户声明及错误常量。uint32_t 明确条件字宽度，unsigned ticks 是当前有界时间参数。调用返回原始负错误，不通过 errno；ETIMEDOUT 的名字供随后运行库使用，不把这次函数改成 POSIX 包装。

<!-- replace: labs/airios/include/airi_threads.h -->
<!-- before -->
```c
#ifndef AIRI_THREADS_H // 自有有界共享执行接口，尚不是pthread。
#define AIRI_THREADS_H // 包含保护。
typedef long (*airi_thread_function)(void *argument); // 工作函数返回完整64位数值，由主执行者join取得。
#ifdef __cplusplus // 后续C++调用使用相同C链接名。
extern "C" { // 不要求宿主线程库。
```
<!-- after -->
```c
#ifndef AIRI_THREADS_H // 自有有界共享执行接口，尚不是pthread。
#define AIRI_THREADS_H // 包含保护。
#include <stdint.h> // 等待字明确为四字节无符号值，不随指针宽度改变。
typedef long (*airi_thread_function)(void *argument); // 工作函数返回完整64位数值，由主执行者join取得。
#ifdef __cplusplus // 后续C++调用使用相同C链接名。
extern "C" { // 不要求宿主线程库。
```

<!-- replace: labs/airios/include/airi_threads.h -->
<!-- before -->
```c
long airi_thread_create(airi_thread_function function, void *argument); // 仅主执行者创建，正TID或原始负错误，不修改errno。
long airi_thread_join(long tid, long *result); // 仅主执行者收取同组一次结果，NULL可丢弃，0成功或负错误。
long airi_thread_id(void); // 同进程PID相同，每个执行者的TID不同。
#ifdef __cplusplus // 结束链接语言范围。
} // 工作函数ABI仍为普通当前目标C调用。
#endif // 结束语言差异。
```
<!-- after -->
```c
long airi_thread_create(airi_thread_function function, void *argument); // 仅主执行者创建，正TID或原始负错误，不修改errno。
long airi_thread_join(long tid, long *result); // 仅主执行者收取同组一次结果，NULL可丢弃，0成功或负错误。
long airi_thread_id(void); // 同进程PID相同，每个执行者的TID不同。
long airi_futex_wait(uint32_t *word, uint32_t expected, unsigned ticks); // 私有键、四字节对齐，有限PIT预算；0通知、-11值不符、-110到期。
long airi_futex_wake(uint32_t *word, unsigned count); // 至多唤醒十六个同进程已登记者，不修改字值，不累计通知。
#ifdef __cplusplus // 结束链接语言范围。
} // 工作函数ABI仍为普通当前目标C调用。
#endif // 结束语言差异。
```

<!-- replace: labs/airios/include/errno.h -->
<!-- before -->
```c
#define ESPIPE 29 // 字节流不能定位。
#define EPIPE 32 // 没有读端；当前内核不附带SIGPIPE。
#define ENOSYS 38 // 当前未实现的操作。
#define EOVERFLOW 75 // 不能表示请求结果。
#ifdef __cplusplus // 同一头以后可被C++源码使用，函数仍用C链接名。
extern "C" { // 防止把运行库入口编码成C++重载符号。
```
<!-- after -->
```c
#define ESPIPE 29 // 字节流不能定位。
#define EPIPE 32 // 没有读端；当前内核不附带SIGPIPE。
#define ENOSYS 38 // 当前未实现的操作。
#define ETIMEDOUT 110 // 有界等待预算已经用完。
#define EOVERFLOW 75 // 不能表示请求结果。
#ifdef __cplusplus // 同一头以后可被C++源码使用，函数仍用C链接名。
extern "C" { // 防止把运行库入口编码成C++重载符号。
```

<a id="futex-wait-record"></a>
## 让等待仍停在原来的系统调用里

在 kernel/process/user.c 的等待种类中加入 IO_FUTEX。已有 io_address 保存键的 VA，io_deadline 保存本次绝对截止，execution.process 已经指出所属资源对象；无需再分配一份队列节点，也不把普通共享变量塞进内核全局表。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
#define WORKER_COUNT 8u // 全系统最多八条额外用户执行链，主执行者仍各占进程槽。
#define EXECUTION_COUNT (PROCESS_COUNT + WORKER_COUNT) // 调度同时遍历主执行者和工作执行者。
enum user_state { USER_UNUSED, USER_RUNNABLE, USER_RUNNING, USER_WAITING, USER_IO_WAIT, USER_ZOMBIE, USER_REAP_PENDING }; // 退出与回收是两个不同状态。
enum io_operation { IO_NONE, IO_READ, IO_WRITE, IO_READY, IO_ASYNC, IO_SLEEP, IO_JOIN }; // 等待保存操作条件，不保留内核局部buffer。
struct user_page { uint64_t address, physical; unsigned flags; }; // fork需要把同一VA映射到另一个物理页。
#define PAGE_RECORDS (4096u / sizeof(struct user_page)) // 一物理页放170项，余下16字节不用。
#define RECORD_CHUNKS ((AIRI_USER_PAGES + PAGE_RECORDS - 1) / PAGE_RECORDS) // 1024项最多借七个记录页。
```
<!-- after -->
```c
#define WORKER_COUNT 8u // 全系统最多八条额外用户执行链，主执行者仍各占进程槽。
#define EXECUTION_COUNT (PROCESS_COUNT + WORKER_COUNT) // 调度同时遍历主执行者和工作执行者。
enum user_state { USER_UNUSED, USER_RUNNABLE, USER_RUNNING, USER_WAITING, USER_IO_WAIT, USER_ZOMBIE, USER_REAP_PENDING }; // 退出与回收是两个不同状态。
enum io_operation { IO_NONE, IO_READ, IO_WRITE, IO_READY, IO_ASYNC, IO_SLEEP, IO_JOIN, IO_FUTEX }; // 等待保存操作条件，不保留内核局部buffer。
struct user_page { uint64_t address, physical; unsigned flags; }; // fork需要把同一VA映射到另一个物理页。
#define PAGE_RECORDS (4096u / sizeof(struct user_page)) // 一物理页放170项，余下16字节不用。
#define RECORD_CHUNKS ((AIRI_USER_PAGES + PAGE_RECORDS - 1) / PAGE_RECORDS) // 1024项最多借七个记录页。
```

等待结束以后，需要把结果写回原来的 frame，再使执行者可运行。把 poll_waiters 末尾已有的公共完成动作抽成 complete_io，显式清掉旧 FD、用户地址和截止。一次通知只是让阻塞调用返回，不能替调用方改 word，也不授予锁所有权。

同一位置先放入 futex_wake。通知者提供当前进程和 VA；扫描十六个实际执行槽，只处理 USER_IO_WAIT 且 io 为 IO_FUTEX 的匹配者。过期记录用 -110 结束，不计入成功唤醒数；未过期记录用 0 结束，达到 count 就停止。顺序来自当前槽扫描，不承诺公平或 FIFO。

word 即使保持为 1，显式 wake 仍可让原调用返回 0。这正是后面的三等待者观察：它检验通知机制，而不是一个完整锁协议。业务代码在醒来后仍要用原子读重新判断自己的谓词，不能把返回 0 当成数据已经就绪。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    }
    return result; // 实际短数量、EOF0或原始负错误，不伪造全部完成。
}
static void poll_waiters(void) { // 仅普通内核上下文运行，IRQ本身不复制、分配或打印。
    for (unsigned i = 0; i < EXECUTION_COUNT; ++i) { // 有界等待队列覆盖所有主/工作执行者。
        struct user_execution *execution = execution_at(i); // 等待记录属于执行链。
```
<!-- after -->
```c
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
```

公共出口已经写入结果并清除借用条件。把原来 poll_waiters 尾部的重复动作替换成这个调用，其他 read、CQ 和 join 等待仍从同一个原 frame 返回；原有 WAIT_WAKE 日志格式保持，既有设备等待观察继续有效。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
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
}
static void destroy_memory(struct user_memory *memory) { // 目标根必须不活动，调用者已保存完需要的用户数值。
```
<!-- after -->
```c
            if (result == -11) // 暂时依旧没有进展。
                continue; // 不把原系统调用提前返回给用户忙等。
        }
        complete_io(execution, result); // 所有等待通过同一出口解除条件并发布结果。
    }
}
static void destroy_memory(struct user_memory *memory) { // 目标根必须不活动，调用者已保存完需要的用户数值。
```

通知现在可以结束一个等待，截止也需要相同出口。IO_FUTEX 分支只比较绝对截止，不重读 word。如果主执行者只改内存却忘记 wake，执行者仍等到超时；内核没有替业务程序轮询条件。普通服务栈负责检查，IRQ 仍只记录事件并交接服务，不在中断栈复制用户数据或打印。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
                target->state = USER_REAP_PENDING; // 此时可能仍使用目标内核栈，只登记延后回收。
                result = 0; // 成功取得一次工作返回值。
            }
        } else if (execution->io == IO_SLEEP) { // 只有截止tick，没有用户缓冲或设备FD。
            if (airi_timer_ticks() < execution->io_deadline) // 时钟尚未到期，保留原现场等待。
                continue; // 不恢复用户忙轮询。
```
<!-- after -->
```c
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
```

登记之前最后一道检查在系统调用分派。四字节对齐、完整 64 位期望值范围和超时上限先核对，再通过 copy_user 验证整个用户读范围。地址有效也不等于值仍相等：旧判断失效返回 EAGAIN，未把 running 移出可运行集合。只有值相等、有正预算且截止加法不溢出，才保存条件并设为 USER_IO_WAIT。

schedule 看见已经撤销运行资格的原执行者，会选择别人；若全在等待，就暂回启动栈。通知或截止以后，complete_io 写入的结果由原 frame 恢复到 RAX。工作者继续 43 号调用后面的指令，用户库得到 0、-11 或 -110；不会重新进入工作函数开头。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
        return join_worker(f); // 未完成时真正阻塞于原frame。
    else if (f->rax == 42) // 执行身份与共享PID分别查询。
        f->rax = running->tid; // 同组工作者的TID各自不同。
    else // 尚未提供的接口都明确拒绝。
        f->rax = (uint64_t)-38ll; // -ENOSYS。
    poll_waiters(); // 文件/流修改完成后立即重查其他等待者，不等用户轮询。
```
<!-- after -->
```c
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
```

<a id="futex-user-boundary"></a>
## 用户调用传地址，原子操作发布条件

在 labs/airios/libc/threads.c 末尾追加两份包装。已有 thread_call 使用 RAX 传编号，RDI/RSI/RDX 传三个数值；memory clobber 防止编译器把本次调用前后的普通内存操作偷偷跨过调用边界，但它不能替其他执行者建立业务数据的 release/acquire 同步。

<!-- append: labs/airios/libc/threads.c -->
```c
long airi_futex_wait(uint32_t *word, uint32_t expected, unsigned ticks) { // 不改errno，返回自有内核原始结果。
    return thread_call(43, (uintptr_t)word, expected, ticks); // 条件字由调用者以原子操作读写，内核只负责比较和阻塞。
}
long airi_futex_wake(uint32_t *word, unsigned count) { // 用户先发布新条件，再通知已登记者。
    return thread_call(44, (uintptr_t)word, count, 0); // 返回数可能为0，不代表内存条件没有改变。
}
```

观察程序使用 C11 原子字。store_release 发布新条件，load_acquire 取得条件；传给系统调用时只转换指针形状，用户代码并不通过转换后的普通 uint32_t 指针并发读写。静态断言确认当前目标的原子对象恰为四字节且不借 libatomic。新程序没有静态 TLS，这也让实际 worker 经过无 TLS 模板的创建路径。

新建 labs/airios/futex-probe.c，先建立共享字、通道及辅助函数。ready 只说明某个工作者即将进入 WAIT，不是内核已经登记的证明；prepared 暂停主执行者三个真实 tick 给它执行机会，之后由实际 wake 的返回人数核对登记是否成立。协调自身也有截止，不能把夹具卡住当成功。

<!-- write: labs/airios/futex-probe.c -->
```c
#include <airi_libc.h> // 真实用户进程、管道与PIT等待入口。
#include <airi_threads.h> // 共享执行者和私有内存等待接口。
#include <stdatomic.h> // 条件字发布使用C11原子操作，不能用volatile替代。
_Static_assert(sizeof(_Atomic uint32_t) == 4 && ATOMIC_INT_LOCK_FREE == 2, "futex word must be lock-free four bytes"); // 当前目标不借libatomic。
static _Atomic uint32_t word = 1, other = 1; // 两个不同用户VA，即使字值相同也不共用等待键。
static _Atomic unsigned ready; // 仅为观察程序协调登记前的准备动作。
static unsigned notice[2], permission[2]; // 两条单向管道把检查和通知安排到确定的先后位置。
static uint32_t *address(_Atomic uint32_t *value) { // 自有ABI接收内存地址，不通过这个窄化指针作C普通读写。
    return (uint32_t *)value; // 内核按字节读取对象表示，用户始终通过原子接口访问值。
}
static void require(int condition, const char *message) { // 观察失败从真实用户进程退出，不能继续打印成功。
    if (!condition) { airi_puts(message); airi_puts("\n"); airi_exit(81); } // 保留当前失败操作的名称。
}
static long raw(long number, uint64_t first, uint64_t second, uint64_t third) { // 完整64位错误参数不能先经过包装器窄化。
    __asm__ volatile("int $0x80" : "+a"(number) : "D"(first), "S"(second), "d"(third) : "memory", "cc"); // 与当前自有系统调用约定相同。
    return number; // 原始负错误，不依赖errno库。
}
static void prepared(unsigned count) { // 夹具的ready不是内核等待队列本身。
    uint64_t start = airi_ticks(); // 给夹具保留有限推进预算。
    while (atomic_load_explicit(&ready, memory_order_acquire) != count) { // acquire承接各工作者准备动作。
        require(airi_ticks() - start < 100, "ready deadline"); // 错误工作者不能让演示无限等待。
        airi_yield(); // 这里只协调实验，不冒充内核阻塞能力。
    }
    require(airi_sleep(3) == 0, "registration window"); // 让准备后的工作者进入43号调用；后面的wake数量再验证真实登记。
}
static long sleeper(void *argument) { // 同一函数可在不同进程的相同VA处等待。
    _Atomic uint32_t *value = argument; // 参数对象存活到join或整个进程结束。
    atomic_fetch_add_explicit(&ready, 1, memory_order_release); // 告知主执行者即将登记，不能代替登记本身。
    return airi_futex_wait(address(value), 1, 200); // 真正暂停于原系统调用，最多名义两秒。
}
static long late_waiter(void *argument) { // 确保wake发生在本次WAIT之前。
    (void)argument; // 参数为空，使用本进程已经建立的两条管道。
    uint32_t observed = atomic_load_explicit(&word, memory_order_acquire); // 先看到旧条件1，尚未调用WAIT。
    unsigned char byte = 'R'; // 通过管道给出已观察的证据。
    if (observed != 1 || airi_write(notice[1], &byte, 1) != 1 || airi_read(permission[0], &byte, 1) != 1) // 读许可前主执行者先改变字并wake。
        return -90; // 所有错误由join保留，不变成成功。
    return airi_futex_wait(address(&word), observed, 200); // 内核必须发现已经是2，立即返回-11。
}
static void close_channels(void) { // 只在所有使用者结束后关闭这些本进程FD。
    for (unsigned i = 0; i < 2; ++i) { // read端和write端各自是一份引用。
        require(airi_close(notice[i]) == 0, "close notice"); // 前一个观察的管道不泄漏到下一个。
        require(airi_close(permission[i]) == 0, "close permission"); // 没有在途等待时允许关闭。
    }
}
```

入口先用 raw 传入未经过窄化的错误参数：未对齐、NULL、内核地址、超过 32 位的期望值及过大数量都不能变成合法操作。随后从同一 word=1 观察 0 预算的两种结果，再真正等三个 PIT tick。此时没有工作者可以通知它，-110 必须来自内核截止；airi_ticks 的差值确认实际经过事件，不用宿主 sleep 代替。

<!-- append: labs/airios/futex-probe.c -->
```c
int main(void) { // 从没有其他工作者的独立用户ELF进入。
    uint64_t key = (uintptr_t)address(&word); // 固定本次原始错误调用使用的实际地址。
    require(raw(43, key + 1, 1, 1) == -22 && raw(44, key + 1, 1, 0) == -22, "unaligned word"); // 两种操作都拒绝未对齐地址。
    require(raw(43, 0, 1, 1) == -14 && raw(44, 0, 1, 0) == -14, "null word"); // NULL不能当作私有键。
    require(raw(43, key, 1ull << 32, 1) == -22 && raw(43, key, 1, 6001) == -22, "wide wait arguments"); // 不允许截断成合法期望值或截止。
    require(raw(44, key, 17, 0) == -22 && raw(44, key, 1ull << 32, 0) == -22, "wide wake count"); // 当前最多十六个调度槽。
    require(raw(43, 0xffff800000000000ull, 1, 1) == -14, "kernel word"); // 特权页不能借系统调用读取。
    require(airi_futex_wait(address(&word), 0, 0) == -11 && airi_futex_wait(address(&word), 1, 0) == -110, "zero timeout order"); // 先比较值，值相等才判断到期。
    uint64_t start = airi_ticks(); // 没有工作者能主动唤醒这次等待。
    require(airi_futex_wait(address(&word), 1, 3) == -110 && airi_ticks() - start >= 3, "real PIT timeout"); // 真实截止返回，不是用户忙等三轮。
```

现在重现章首的空隙。late_waiter 先读到 1，再经 notice 送出 R，随后阻塞在 permission 管道。主执行者收到 R 后发布 2，wake 返回 0，因为工作者根本还没进入 futex WAIT。最后经 permission 放行，工作者带旧值 1 进入内核，比较读到 2，立即返回 -11。这里管道分别确定观察和放行，不靠一个不确定的延时假设通知顺序。

<!-- append: labs/airios/futex-probe.c -->
```c
    require(airi_pipe(notice, 0) == 0 && airi_pipe(permission, 0) == 0, "prewake pipes"); // 两条通道互不消费对方通知。
    long tid = airi_thread_create(late_waiter, 0), result; // 先让工作者读取1并在许可管道上等待。
    unsigned char byte; // 主执行者自己的栈缓冲区。
    require(tid > 0 && airi_read(notice[0], &byte, 1) == 1 && byte == 'R', "old condition observed"); // 不是靠宿主延时猜先后。
    atomic_store_explicit(&word, 2, memory_order_release); // 先发布新条件，再通知。
    require(airi_futex_wake(address(&word), 16) == 0, "prewake has no waiter"); // 通知不积攒，工作者此刻还在管道等待。
    require(airi_write(permission[1], "G", 1) == 1 && airi_thread_join(tid, &result) == 0 && result == -11, "late WAIT rejects stale value"); // 没有等待信用仍不丢更新，因为内核再比较值。
    close_channels(); // 工作者已结束才归还共享表中的引用。
    airi_puts("[FUTEX_PREWAKE_OK] old=1 published=2 wake=0 wait=-11\n"); // 实际先通知后等待路径已经完成。
```

把 word 恢复为 1，并创建三个 sleeper。prepared 给出执行机会后，other 地址的通知返回 0；count=0 也返回 0。对 word 先通知一个，再通知最多十六个，实际返回依次为 1、2。原子读取仍是 1，三个 join 却都得到 0：通知只结束等待，字值完全由用户协议拥有。

<!-- append: labs/airios/futex-probe.c -->
```c
    atomic_store_explicit(&word, 1, memory_order_release); // 新一轮的三个等待者共同等1。
    atomic_store_explicit(&ready, 0, memory_order_relaxed); // 前一轮无人再使用ready。
    long ids[3]; // 保存真实TID，而非工作槽编号。
    for (unsigned i = 0; i < 3; ++i) { // 发布三条同组执行链。
        ids[i] = airi_thread_create(sleeper, &word); // 它们共享同一个原子对象。
        require(ids[i] > 0, "create three waiters"); // 容量失败不能继续假设三人登记。
    }
    prepared(3); // wake的1与2结果会核对实际队列规模。
    require(airi_futex_wake(address(&other), 16) == 0 && airi_futex_wake(address(&word), 0) == 0, "different key and zero count"); // 不因字值相同而通知其他地址。
    require(airi_futex_wake(address(&word), 1) == 1 && airi_futex_wake(address(&word), 16) == 2, "bounded wake counts"); // 第一次仅一人，第二次收取余下两人。
    require(atomic_load_explicit(&word, memory_order_acquire) == 1, "wake leaves word unchanged"); // 成功通知并没有取得锁或修改业务条件。
    for (unsigned i = 0; i < 3; ++i) // 工作者醒后返回，主执行者负责join和回收。
        require(airi_thread_join(ids[i], &result) == 0 && result == 0, "join notified waiter"); // 通知路径的WAIT返回0。
    airi_puts("[FUTEX_COUNT_OK] waiters=3 wake_one=1 wake_rest=2 unchanged=1\n"); // 有界数量与无业务写入均实际观察。
```

三个工作者都已 join 后再 fork，避免突破 K52 的共享内存生命周期限制。父子分别创建一名 sleeper，二者 VA 相同。孩子先登记并送 C，然后等父放行；父侧 wake 必须只返回 1，随后孩子自己的 wake 也必须仍返回 1。若内核只用 VA 作键，父会一次带走两人，或者孩子已无等待者，两个结果都会失败。两边先关闭不用的管道端，失败时不会因自己留着一个写端而无限等 EOF。

<!-- append: labs/airios/futex-probe.c -->
```c
    atomic_store_explicit(&ready, 0, memory_order_relaxed); // fork前再次建立无人使用的初始夹具状态。
    require(airi_pipe(notice, 0) == 0 && airi_pipe(permission, 0) == 0, "private key channels"); // 父子使用管道协调，内存字仍分别归各自页表。
    long child = airi_fork(); // 此时没有未join工作者，符合已有fork边界。
    require(child >= 0, "private fork"); // 不把失败当孩子路径。
    if (!child) { // 子进程复制了相同数值的用户VA，但拥有另一份内存。
        require(airi_close(notice[0]) == 0 && airi_close(permission[1]) == 0, "child channel ends"); // 不需要的端点在启动工作者前关闭。
        long own = airi_thread_create(sleeper, &word); // 子进程也在同一个虚拟地址等1。
        require(own > 0, "child waiter"); prepared(1); // 先确认子侧实际登记机会。
        require(airi_write(notice[1], "C", 1) == 1 && airi_read(permission[0], &byte, 1) == 1, "child awaits parent wake"); // 父侧先检查只唤醒自己。
        require(airi_futex_wake(address(&word), 16) == 1 && airi_thread_join(own, &result) == 0 && !result, "child private wake"); // 父侧通知不能提前拿走这个等待者。
        airi_exit(42); // 父wait观察整组正常结果。
    }
    require(airi_close(notice[1]) == 0 && airi_close(permission[0]) == 0, "parent channel ends"); // 父只接收子已登记通知并发出放行。
    tid = airi_thread_create(sleeper, &word); // 父侧相同VA的独立键。
    require(tid > 0, "parent waiter"); prepared(1); // 两个进程各有一名登记者。
    require(airi_read(notice[0], &byte, 1) == 1 && byte == 'C', "child registered"); // 在子侧仍被保留时执行父wake。
    require(airi_futex_wake(address(&word), 16) == 1 && airi_thread_join(tid, &result) == 0 && !result, "parent private wake"); // 若只按VA匹配会错误返回2。
    require(airi_write(permission[1], "G", 1) == 1, "release child"); // 子随后自行唤醒它的等待者。
    int status; // wait返回的是进程结果，和worker的long返回值分开。
    require(airi_waitpid(child, &status) == child && status == 42, "private child exit"); // 子侧同时验证自己的wake仍为1。
    require(airi_close(notice[0]) == 0 && airi_close(permission[1]) == 0, "parent close channels"); // 全部通道引用均已失去使用者。
```

最后让另一个孩子带着未 join 的 futex 等待者退出 17。已有 exit_current 把整组 worker 标为 REAP_PENDING；poll 和 wake 都只处理 USER_IO_WAIT，因此死亡组已经失去等待资格。离开其内核栈后，collect_pending 再清控制块并归还独立栈/FPU。父接着在同一 VA 创建新等待者，wake 必须恰好为 1，不能把旧组记录算进去。用户页仍由所属进程统一回收，等待项不独立拥有这些页。

<!-- append: labs/airios/futex-probe.c -->
```c
    atomic_store_explicit(&ready, 0, memory_order_relaxed); // 最后观察带等待者的进程退出。
    child = airi_fork(); require(child >= 0, "cleanup fork"); // 仍从没有worker的进程分叉。
    if (!child) { // 让子进程退出时仍有未join的futex等待者。
        require(airi_thread_create(sleeper, &word) > 0, "exit waiter"); prepared(1); // 原系统调用确实阻塞过。
        airi_exit(17); // 整组退出必须撤销等待并延后回收栈。
    }
    require(airi_waitpid(child, &status) == child && status == 17, "cleanup child exit"); // 父仍可继续创建自己的工作者。
    tid = airi_thread_create(sleeper, &word); require(tid > 0, "post-exit waiter"); prepared(1); // 工作槽可复用而等待身份不能复用。
    require(airi_futex_wake(address(&word), 16) == 1 && airi_thread_join(tid, &result) == 0 && !result, "no stale waiter"); // 只存在当前父进程的一个登记者。
    airi_puts("[FUTEX_USER_OK] compare=atomic prewake=checked timeout=pit counts=1,2 private=process,va exit=clean\n"); // 所有错误、通知、私有键与退出路径完成后才发布。
    return 0; // 由用户shell取得真实成功退出。
}
```

<a id="futex-image-catalog"></a>
## 把独立程序接入镜像，再输出完整目录

在 images.S 和 Makefile 末尾追加本配置的独立 ELF。futex.elf 只链接原始用户库、共享执行库、入口桥接与自己的观察程序，不链接宿主 pthread 或 errno 实现。incbin 等待同配置链接完成，不从主机文件输出伪造 guest 观察。

<!-- append: labs/airios/images.S -->
```asm

.balign 8 /* 私有内存等待探针仍经静态ELF装载。 */
.global airi_futex_elf_start, airi_futex_elf_end /* 完整只读输入范围。 */
airi_futex_elf_start: /* 含自有线程库的用户程序。 */
.incbin AIRI_FUTEX_ELF /* 当前配置交叉链接产物。 */
airi_futex_elf_end: /* 输入文件的排他右端。 */
```

<!-- append: Makefile -->
```makefile

# 私有futex观察选择共享执行库，不链接errno或宿主pthread。
$(OUT)/user/futex.elf: $(USER_COMMON) $(OUT)/user/threads-lib.o $(OUT)/user/thread-entry.o $(OUT)/user/futex-probe.o labs/airios/program.ld
# 同一静态链接器和用户段权限仍由既有脚本固定。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(filter %.o,$^) -o $@
# 包装对象等待本配置的真实新程序。
$(OUT)/labs/airios/images.o: $(OUT)/user/futex.elf
# incbin只读取本配置文件，不读取宿主程序。
$(OUT)/labs/airios/images.o: CPPFLAGS += -DAIRI_FUTEX_ELF='"$(OUT)/user/futex.elf"'
```

在内核声明这个只读映像范围，并加入真实程序目录。新增名称以后目录已超过旧的 256 字节，所以同时把内核的目录构造数组增到 512。容量扩大不改变原始 write 的单次上限，也不让小缓冲收到半份目录：programs 仍检查整份结果及末尾 NUL。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
extern const uint8_t airi_process_elf_start[], airi_process_elf_end[]; // 新的fork/wait和抢占工作负载。
extern const uint8_t airi_vector_elf_start[], airi_vector_elf_end[]; // 显式偏移向量正常与坏范围观察。
extern const uint8_t airi_malloc_elf_start[], airi_malloc_elf_end[]; // 用户对象分配与页面缓存的独立ELF。
extern const uint8_t airi_threads_elf_start[], airi_threads_elf_end[]; // 同进程共享内存、FD与异步owner的真实工作执行者。
extern const uint8_t airi_posix_elf_start[], airi_posix_elf_end[]; // 显式链接errno与POSIX风格文件包装。
extern const uint8_t airi_ctls_elf_start[], airi_ctls_elf_end[]; // 编译器_Thread_local与静态模板。
```
<!-- after -->
```c
extern const uint8_t airi_process_elf_start[], airi_process_elf_end[]; // 新的fork/wait和抢占工作负载。
extern const uint8_t airi_vector_elf_start[], airi_vector_elf_end[]; // 显式偏移向量正常与坏范围观察。
extern const uint8_t airi_malloc_elf_start[], airi_malloc_elf_end[]; // 用户对象分配与页面缓存的独立ELF。
extern const uint8_t airi_futex_elf_start[], airi_futex_elf_end[]; // 私有内存等待与通知的真实用户程序。
extern const uint8_t airi_threads_elf_start[], airi_threads_elf_end[]; // 同进程共享内存、FD与异步owner的真实工作执行者。
extern const uint8_t airi_posix_elf_start[], airi_posix_elf_end[]; // 显式链接errno与POSIX风格文件包装。
extern const uint8_t airi_ctls_elf_start[], airi_ctls_elf_end[]; // 编译器_Thread_local与静态模板。
```

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    {"vector", airi_vector_elf_start, airi_vector_elf_end}, // 聚合、分散、短前缀与错误原子性。
    {"heap", airi_malloc_elf_start, airi_malloc_elf_end}, // 单线程用户对象与错误路径。
    {"heap-reset", airi_malloc_elf_start, airi_malloc_elf_end}, // 同一ELF新BSS和按需arena。
    {"threads", airi_threads_elf_start, airi_threads_elf_end}, // 独立TLS/CPU状态、join与整个进程退出。
    {"libc", airi_posix_elf_start, airi_posix_elf_end}, // 用户库错误与文件结果转换。
    {"libc-reset", airi_posix_elf_start, airi_posix_elf_end}, // 同一ELF新错误槽和CLOEXEC观察。
```
<!-- after -->
```c
    {"vector", airi_vector_elf_start, airi_vector_elf_end}, // 聚合、分散、短前缀与错误原子性。
    {"heap", airi_malloc_elf_start, airi_malloc_elf_end}, // 单线程用户对象与错误路径。
    {"heap-reset", airi_malloc_elf_start, airi_malloc_elf_end}, // 同一ELF新BSS和按需arena。
    {"futex", airi_futex_elf_start, airi_futex_elf_end}, // 相等比较与登记、超时及进程私有键。
    {"threads", airi_threads_elf_start, airi_threads_elf_end}, // 独立TLS/CPU状态、join与整个进程退出。
    {"libc", airi_posix_elf_start, airi_posix_elf_end}, // 用户库错误与文件结果转换。
    {"libc-reset", airi_posix_elf_start, airi_posix_elf_end}, // 同一ELF新错误槽和CLOEXEC观察。
```

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
        f->rax = (uint64_t)result; // 用户看到的只是本次时刻数值。
    }
    else if (f->rax == 30) { // 目录只返回有界文本，不暴露ELF或内核对象地址。
        char names[256]; size_t used = 0; // 当前固定目录完整小于此上限。
        for (unsigned i=0; i<sizeof(executables)/sizeof(executables[0]); ++i) { // 结果与exec使用同一真实注册表。
            size_t n=airi_strlen(executables[i].name); // 名称包含内部演示目标，用户仍可选择尝试。
            if (used+n+2>sizeof(names)) // 防止以后目录增长造成栈覆盖。
```
<!-- after -->
```c
        f->rax = (uint64_t)result; // 用户看到的只是本次时刻数值。
    }
    else if (f->rax == 30) { // 目录只返回有界文本，不暴露ELF或内核对象地址。
        char names[512]; size_t used = 0; // 当前固定目录完整小于此上限。
        for (unsigned i=0; i<sizeof(executables)/sizeof(executables[0]); ++i) { // 结果与exec使用同一真实注册表。
            size_t n=airi_strlen(executables[i].name); // 名称包含内部演示目标，用户仍可选择尝试。
            if (used+n+2>sizeof(names)) // 防止以后目录增长造成栈覆盖。
```

shell 的查询缓冲同样改成 512，并更新 NULL 输出检查使用的容量，保证测试仍走 EFAULT 而不是先撞到 ENOSPC。目录成功返回长度 n 以后，原来的 airi_puts 只能一次提交最多 256 字节，不能直接输出全部。新循环每次至多提交 256，按实际返回的 step 前进；错误或零进度停止。这样末尾四个有效字节也能送出，不会因“查询已成功”就漏掉发送动作。

<!-- replace: labs/airios/sh.c -->
<!-- before -->
```c
}
int main(void) { // 内核监控台run sh后从此用户入口继续。
    char guard[1]={'Q'}; // 小容量失败不能覆盖用户哨兵。
    if (airi_programs(guard,1)!=-28 || guard[0]!='Q' || airi_programs((char *)0,256)!=-14 || airi_read(0,guard,0)!=0 || airi_read(0,(void *)0,1)!=-14) // 目录与UART输出先完整验证，空读不阻塞。
        return 2; // 未通过不能发布shell就绪。
    char line[64]; size_t used=0; int overflow=0,skip_lf=0; // 固定输入预算，过长行不截断执行。
    airi_puts("[USER_SHELL_READY]\nCommands: help programs exit, or one program name.\n"); // 交互与内核monitor的run语法不同。
```
<!-- after -->
```c
}
int main(void) { // 内核监控台run sh后从此用户入口继续。
    char guard[1]={'Q'}; // 小容量失败不能覆盖用户哨兵。
    if (airi_programs(guard,1)!=-28 || guard[0]!='Q' || airi_programs((char *)0,512)!=-14 || airi_read(0,guard,0)!=0 || airi_read(0,(void *)0,1)!=-14) // 目录与UART输出先完整验证，空读不阻塞。
        return 2; // 未通过不能发布shell就绪。
    char line[64]; size_t used=0; int overflow=0,skip_lf=0; // 固定输入预算，过长行不截断执行。
    airi_puts("[USER_SHELL_READY]\nCommands: help programs exit, or one program name.\n"); // 交互与内核monitor的run语法不同。
```

<!-- replace: labs/airios/sh.c -->
<!-- before -->
```c
        if (equal(line,"exit")) { airi_puts("[USER_SHELL_EXIT]\n"); return 0; } // 退出与普通孩子结束分开。
        if (equal(line,"help")) { airi_puts("Use programs, one name, or exit. No arguments, redirects, jobs or nested sh.\n"); continue; } // 准确说明当前语法。
        if (equal(line,"programs")) { // 目录从内核真实注册表查询，避免维护第二套名单。
            char names[256]; long n=airi_programs(names,sizeof names); // 完整结果或错误，不显示半目录。
            if (n>=0) airi_puts(names); else airi_puts("catalog unavailable\n"); // 名称文本没有内部指针。
            continue; // 内建查询不用fork。
        }
        if (equal(line,"sh")) { airi_puts("nested sh has no separate input terminal\n"); continue; } // 不与父竞争单UART。
```
<!-- after -->
```c
        if (equal(line,"exit")) { airi_puts("[USER_SHELL_EXIT]\n"); return 0; } // 退出与普通孩子结束分开。
        if (equal(line,"help")) { airi_puts("Use programs, one name, or exit. No arguments, redirects, jobs or nested sh.\n"); continue; } // 准确说明当前语法。
        if (equal(line,"programs")) { // 目录从内核真实注册表查询，避免维护第二套名单。
            char names[512]; long n=airi_programs(names,sizeof names); // 完整结果或错误，不显示半目录。
            if (n < 0) airi_puts("catalog unavailable\n"); // 查询失败不能消费未初始化目录。
            else { // 目录超过一次write上限，逐段提交实际有效字节。
                size_t sent = 0; // 已成功交给终端的前缀，不含末尾NUL。
                while (sent < (size_t)n) { // 短写只前进实际数量，不跳过尚未发送的部分。
                    size_t part = (size_t)n - sent; // 当前仍未输出的字节。
                    if (part > 256) part = 256; // 原始write仍保留有界输入合同。
                    long step = airi_write(1, names + sent, part); // 目录在本次命令栈上持续有效。
                    if (step <= 0) { airi_puts("catalog output failed\n"); break; } // 错误或零进度不能无限重试。
                    sent += (size_t)step; // 仅交付实际成功的前缀。
                }
            }
            continue; // 内建查询不用fork。
        }
        if (equal(line,"sh")) { airi_puts("nested sh has no separate input terminal\n"); continue; } // 不与父竞争单UART。
```

<a id="futex-observe"></a>
## 从真实 shell 进入等待，再回到命令输入

自动观察在现有 shell 会话里发送 futex，然后只核对这条命令之后的输出。八项 FUTEX_WAIT 登记分别来自一次主执行者超时、三个同键等待、私有父子各一个、退出孩子及最后复用者各一个。先通知后等待返回 -11，所以那次没有登记，不能把它计成第九项。

现有 threads、存储和 init 观察保持。programs 检查继续要求看到完整目录中的 demo 与 async-reset，能够发现新数组够大但旧输出仍失败的情况。

<!-- replace: scripts/shell_test.py -->
<!-- before -->
```python
                heap_output=text[offset:] # 只接受本条命令与它实际exec的孩子记录。
                assert '[MALLOC_USER_OK] arena=262144 align=16 calloc=zero overflow=checked realloc=failure_preserved reuse=128 fork=private exec=reset' in heap_output # 对齐、复用、失败保留与父子隔离均由guest观察。
                assert '[MALLOC_EXEC_RESET_OK] arena=lazy objects=zero errno=zero' in heap_output # 成功exec必须重新建立BSS和TLS，不能继承旧arena。
                for repetition in range(2): # 同一shell连续完成两次线程命令，上一组退出回收后应能重新创建。
                    offset=len(text); wire.sendall(b'threads\n'); text=wait('[SHELL_COMMAND_DONE] name=threads code=0',offset) # 真正同地址空间执行，不能由fork复制或宿主线程模拟。
                    thread_output=text[offset:] # 所有身份与标记均限定在这条命令之后。
```
<!-- after -->
```python
                heap_output=text[offset:] # 只接受本条命令与它实际exec的孩子记录。
                assert '[MALLOC_USER_OK] arena=262144 align=16 calloc=zero overflow=checked realloc=failure_preserved reuse=128 fork=private exec=reset' in heap_output # 对齐、复用、失败保留与父子隔离均由guest观察。
                assert '[MALLOC_EXEC_RESET_OK] arena=lazy objects=zero errno=zero' in heap_output # 成功exec必须重新建立BSS和TLS，不能继承旧arena。
                offset=len(text); wire.sendall(b'futex\n'); text=wait('[SHELL_COMMAND_DONE] name=futex code=0',offset) # 真正共享地址空间中的比较、登记与显式通知。
                futex_output=text[offset:] # 不接受其他命令的旧等待标记。
                assert '[FUTEX_PREWAKE_OK] old=1 published=2 wake=0 wait=-11' in futex_output # 通知先发生，旧判断不能使后续调用睡下。
                assert '[FUTEX_COUNT_OK] waiters=3 wake_one=1 wake_rest=2 unchanged=1' in futex_output # 数量与字不被修改分别由guest检查。
                assert '[FUTEX_USER_OK] compare=atomic prewake=checked timeout=pit counts=1,2 private=process,va exit=clean' in futex_output # 私有键、退出清理与真实PIT超时都已完成。
                assert len(re.findall(r'\[FUTEX_WAIT\] pid=',futex_output))==8 # 一次超时、三次同键、父子各一、退出孩子与复用者各一。
                for repetition in range(2): # 同一shell连续完成两次线程命令，上一组退出回收后应能重新创建。
                    offset=len(text); wire.sendall(b'threads\n'); text=wait('[SHELL_COMMAND_DONE] name=threads code=0',offset) # 真正同地址空间执行，不能由fork复制或宿主线程模拟。
                    thread_output=text[offset:] # 所有身份与标记均限定在这条命令之后。
```

<!-- replace: scripts/shell_test.py -->
<!-- before -->
```python
                offset=len(text); wire.sendall(b'echo after-shell\n'); text=wait('[CONSOLE_UPDATE] view=0 commands=2 page=0',offset) # 回到内核后输入仍完整。
                assert 'after-shell' in text[offset:] # 不只是退出而丢了后续UART。
                result={'passed':True,'ring3_shell':True,'ram_file_and_device_readback':True,'other_user_during_cq_wait':True,'qmp_iops':1,'error_exec_survived':True,'returned_to_monitor':True,'orphan_rounds':24,'memory_reuse_rounds':128,'capacity_rounds':4,'capacity_pages':1024,'simd_preempt_fork_exec':True,'fs_base_preempt_fork_exec':True,'compiler_static_tls':True,'posix_errno_io_subset':True,'positioned_vectors':True} # 只发布已观察事实。
                result.update(user_heap=True,shared_executions=True,thread_commands=2,worker_capacity=8,worker_reuse_rounds=24) # 关键字把实际已验收字段加入结果字典，不替代上面的逐命令检查。
                result_path.write_text(json.dumps(result,indent=2)+'\n'); print('PASS user shell: alloc/file/async wait/other task/CQ/readback, failed exec, return to monitor') # 阶段演示实际完成。
                return result # 总入口可收集。
            finally: # 包括断言、超时和QEMU错误。
```
<!-- after -->
```python
                offset=len(text); wire.sendall(b'echo after-shell\n'); text=wait('[CONSOLE_UPDATE] view=0 commands=2 page=0',offset) # 回到内核后输入仍完整。
                assert 'after-shell' in text[offset:] # 不只是退出而丢了后续UART。
                result={'passed':True,'ring3_shell':True,'ram_file_and_device_readback':True,'other_user_during_cq_wait':True,'qmp_iops':1,'error_exec_survived':True,'returned_to_monitor':True,'orphan_rounds':24,'memory_reuse_rounds':128,'capacity_rounds':4,'capacity_pages':1024,'simd_preempt_fork_exec':True,'fs_base_preempt_fork_exec':True,'compiler_static_tls':True,'posix_errno_io_subset':True,'positioned_vectors':True} # 只发布已观察事实。
                result.update(private_futex=True,futex_registrations=8,user_heap=True,shared_executions=True,thread_commands=2,worker_capacity=8,worker_reuse_rounds=24) # 关键字把实际已验收字段加入结果字典，不替代上面的逐命令检查。
                result_path.write_text(json.dumps(result,indent=2)+'\n'); print('PASS user shell: alloc/file/async wait/other task/CQ/readback, failed exec, return to monitor') # 阶段演示实际完成。
                return result # 总入口可收集。
            finally: # 包括断言、超时和QEMU错误。
```

<!-- run -->
```sh
./airios build  # 生成独立用户ELF和串口镜像，不读取前端目录
```

<!-- run -->
```sh
./airios test  # 有截止地运行正常、故障、shell、监督和存储路径
```

本轮真实串口出现：

```text
[FUTEX_PREWAKE_OK] old=1 published=2 wake=0 wait=-11
[FUTEX_COUNT_OK] waiters=3 wake_one=1 wake_rest=2 unchanged=1
[FUTEX_USER_OK] compare=atomic prewake=checked timeout=pit counts=1,2 private=process,va exit=clean
[SHELL_COMMAND_DONE] name=futex code=0
```

地址和 PID 随本次链接及前序命令变化，应以同一次日志的 FUTEX_WAIT/FUTEX_WAKE 关联，不把范例数值当固定分配结果。交互观察可运行 `./airios run`，在内核提示符输入 `run sh`，再输入 `futex`、`programs`、`exit`；最后回到内核提示符。`./airios kernel-debug` 中可在 futex_wake 与 complete_io 设断点，检查 target 的 process、io_address、state 和 frame->rax；命中 raw WAIT 比较失效时不会进入 USER_IO_WAIT。

若要完整观察先通知后的拒绝，沿本程序 late_waiter 的旧 observed=1、主侧 store=2、wake=0、许可管道返回四步走到 43 号分支：copy_user 读到 2，结果为 -11，登记字段保持原状态。比较失败时没有新的等待项，主执行者先前发出的通知也没有被保存。

<a id="futex-linux"></a>
## 接回 Linux 的等待与数据发布

[Linux FUTEX_WAIT](https://man7.org/linux/man-pages/man2/FUTEX_WAIT.2const.html)同样在内核比较期望值，并将比较和开始等待作为不可分割的操作；调用方须在返回后重查条件。[FUTEX_WAKE](https://man7.org/linux/man-pages/man2/FUTEX_WAKE.2const.html)返回实际唤醒数，不提供锁所有权或选人顺序保证。AiriOS 借鉴这项交接模型，参数、错误返回和时间表示仍是本项目的有界接口。

K52 已提供 labs/linux/p5-threads/threads.c 的 Linux 对照：pthread 条件变量保护有界队列，另以 FUTEX_WAIT_PRIVATE/FUTEX_WAKE_PRIVATE 发布 payload，等待线程在 acquire 读到条件后才读普通数据。Linux 包装的失败通过 errno，AiriOS 包装直接返回负数；不能用相同名字抹平这项差别。运行 `./airios linux-lab --phase p5-threads` 会先连接个人主机，再上传到独立目录并取回证据。连接失败时只保留失败记录，不能把 Mac 上的编译或 guest 的成功改记成 Linux 实测。

当前的 futex 是运行库原语，还没有互斥锁、条件变量或线程安全 malloc。下一步可以把“先原子争取所有权，争取失败才 WAIT，释放时发布再 WAKE”组合成有界互斥；本章三名 sleeper 醒后不重查业务条件，只用于观察裸通知，不是可直接复制到业务里的锁实现。
