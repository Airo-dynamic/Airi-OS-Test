# K66 · 八字节等待与同地址宽度隔离

K60 的32位版本到顶会明确拒绝回绕，尚不足以把高频通知直接当作最终长驻组件。接下来先让内核能比较和等待一个完整64位值，为更宽变化序号提供基础；现有43／44号四字节接口继续保留，不把Linux futex的名字当成已有64位兼容ABI。

从 K65 的同一目录继续。新增自有46号WAIT64、47号WAKE64，用户库显式用uint64_t参数和八字节对齐地址。宽度不仅决定读取多少字节，还进入私有等待键；同一进程、同一个地址的四字节等待和八字节等待不能互相唤醒。本节尚未把旧event或队列升级为宽版本。

<a id="wide-futex-value"></a>
## 低四字节相同，完整八字节已经不同

观察字最初为1，主发布0x1122334400000001。低32位仍为1，高32位已经变化。八字节WAIT若把期望窄化成32位，就可能把旧1误当成仍相等而睡下，因此不能只是把用户指针类型改成uint64_t *，内核却继续读四字节。

当前x86-64是低地址先放低位字节，发布后的八个字节如下。四字节接口读低地址的前四个，仍是1；八字节接口必须比较全部，结果为1234605615003729921。

| 相对word地址 | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 十六进制字节 | 01 | 00 | 00 | 00 | 44 | 33 | 22 | 11 |

用户发布使用原生八字节原子操作，_Alignas(8)要求word起点在八字节边界。内核读取仍经copy_user的完整页表验证，不把用户VA当内核裸指针。比较和登记不可分割的依据仍是固定单CPU、内核不可抢占；软件字节复制本身不会因为长度变八就变成跨CPU原子操作。

<a id="wide-futex-key"></a>
## 同一个地址，宽度也是等待身份的一部分

新增IO_FUTEX64记录八字节等待。wake匹配同进程对象、相同VA和相同kind三项，不能只在WAIT时读八字节、WAKE时仍把所有同VA记录一起唤醒。kind保存在已有execution.io，不增加动态队列节点，保存地址和截止的字段继续复用。

先修改kernel/process/user.c中的io_operation枚举，保留相邻宏和状态作定位。观察ELF声明和目录项稍后在构建段加入；这里先把宽度接进内核等待记录。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
#define WORKER_COUNT 8u // 全系统最多八条额外用户执行链，主执行者仍各占进程槽。
#define EXECUTION_COUNT (PROCESS_COUNT + WORKER_COUNT) // 调度同时遍历主执行者和工作执行者。
enum user_state { USER_UNUSED, USER_RUNNABLE, USER_RUNNING, USER_WAITING, USER_IO_WAIT, USER_ZOMBIE, USER_REAP_PENDING }; // 退出与回收是两个不同状态。
enum io_operation { IO_NONE, IO_READ, IO_WRITE, IO_READY, IO_ASYNC, IO_SLEEP, IO_JOIN, IO_FUTEX }; // 等待保存操作条件，不保留内核局部buffer。
struct user_page { uint64_t address, physical; unsigned flags; }; // fork需要把同一VA映射到另一个物理页。
#define PAGE_RECORDS (4096u / sizeof(struct user_page)) // 一物理页放170项，余下16字节不用。
#define RECORD_CHUNKS ((AIRI_USER_PAGES + PAGE_RECORDS - 1) / PAGE_RECORDS) // 1024项最多借七个记录页。
```
<!-- after -->
```c
#define WORKER_COUNT 8u // 全系统最多八条额外用户执行链，主执行者仍各占进程槽。
#define EXECUTION_COUNT (PROCESS_COUNT + WORKER_COUNT) // 调度同时遍历主执行者和工作执行者。
enum user_state { USER_UNUSED, USER_RUNNABLE, USER_RUNNING, USER_WAITING, USER_IO_WAIT, USER_ZOMBIE, USER_REAP_PENDING }; // 退出与回收是两个不同状态。
enum io_operation { IO_NONE, IO_READ, IO_WRITE, IO_READY, IO_ASYNC, IO_SLEEP, IO_JOIN, IO_FUTEX, IO_FUTEX64 }; // 等待保存操作条件，不保留内核局部buffer。
struct user_page { uint64_t address, physical; unsigned flags; }; // fork需要把同一VA映射到另一个物理页。
#define PAGE_RECORDS (4096u / sizeof(struct user_page)) // 一物理页放170项，余下16字节不用。
#define RECORD_CHUNKS ((AIRI_USER_PAGES + PAGE_RECORDS - 1) / PAGE_RECORDS) // 1024项最多借七个记录页。
```

futex_value接收wide。宽请求复制到uint64_t；窄请求先复制到uint32_t局部值，再由C无符号扩展交给统一比较。不能把四字节直接复制进未初始化uint64_t并依赖恰好剩下零高位，也不把内核读取规则绑到示例的字节序上。helper在复制失败时立即返回，不读未初始化的局部值。

先在complete_io结束处插入这个读取助手，原futex_wake仍接在后面。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    ++io_wake_count; // 实际结束一次阻塞，包括错误和超时。
    airi_log("[WAIT_WAKE] pid=%u result=%ld", execution->process->pid, result); // 不在IRQ中打印或复制。
}
```
<!-- after -->
```c
    ++io_wake_count; // 实际结束一次阻塞，包括错误和超时。
    airi_log("[WAIT_WAKE] pid=%u result=%ld", execution->process->pid, result); // 不在IRQ中打印或复制。
}
static bool futex_value(uint64_t address, bool wide, uint64_t *value) { // 按请求宽度复制到同类型内核局部值，不靠字节序扩展窄读取。
    if (wide) return copy_user(current, value, address, sizeof(*value), false); // 验证并复制完整八字节，保留高32位。
    uint32_t narrow; // 原接口仍只读四字节。
    if (!copy_user(current, &narrow, address, sizeof(narrow), false)) return false; // 失败不读取未初始化局部值。
    *value = narrow; // C无符号扩展为64位，不把窄键变成八字节访问。
    return true; // 只有完整复制成功才参与比较。
}
```

futex_wake根据kind选择8或4字节，检查相应自然对齐与完整可访问跨度。数量仍是完整64位并限至16，不先窄化。扫描只完成同kind等待，过期者仍返回-110但不计入成功通知人数，通知也不改原字。

再替换futex_wake开头到过期分支的旧窗口。已有后半段继续完成唤醒；下一窗口只修改其日志选择。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
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
```
<!-- after -->
```c
static long futex_wake(uint64_t address, uint64_t count, enum io_operation kind) { // 私有键由进程对象、VA及等待宽度共同组成。
    unsigned bytes = kind == IO_FUTEX64 ? 8 : 4; // 只有固定系统调用入口能选择这两种内部类型。
    if ((address & (bytes - 1)) || count > EXECUTION_COUNT) // 按四/八字节自然对齐，完整数量不窄化。
        return -22; // -EINVAL，尚未唤醒任何等待者。
    if (!user_access(current, address, bytes, false)) // wake虽不读写字值，也要求当前地址有效。
        return -14; // -EFAULT，不能用内核地址匹配某个队列。
    uint64_t now = airi_timer_ticks(); // 同一次wake内采用固定截止快照。
    unsigned woken = 0; // 仅成功wake的数量，不包含过期清理。
    for (unsigned i = 0; i < EXECUTION_COUNT && woken < count; ++i) { // 最多扫描十六槽，无额外分配。
        struct user_execution *target = execution_at(i); // 借当前执行表中的稳定槽。
        if (target->process != current || target->state != USER_IO_WAIT || target->io != kind || target->io_address != address) // 同VA不同宽度或fork后的进程都不混合。
            continue; // 不影响别的地址、进程或等待种类。
        if (now >= target->io_deadline) { // 到期与通知相遇时先给出超时结果。
            complete_io(target, -110); // -ETIMEDOUT，不能算作成功唤醒。
```

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
        complete_io(target, 0); // 不修改用户字，也不替用户取得互斥锁。
        ++woken; // 仅累计原来确实已经阻塞的执行者。
    }
    airi_log("[FUTEX_WAKE] pid=%u address=0x%llx count=%llu woken=%u", current->pid, (unsigned long long)address, (unsigned long long)count, woken); // 明确私有进程与实际数量。
    return woken; // 0表示此刻没有匹配等待，不保存通知信用。
}
static void poll_waiters(void) { // 仅普通内核上下文运行，IRQ本身不复制、分配或打印。
```
<!-- after -->
```c
        complete_io(target, 0); // 不修改用户字，也不替用户取得互斥锁。
        ++woken; // 仅累计原来确实已经阻塞的执行者。
    }
    airi_log(kind == IO_FUTEX64 ? "[FUTEX64_WAKE] pid=%u address=0x%llx count=%llu woken=%u" : "[FUTEX_WAKE] pid=%u address=0x%llx count=%llu woken=%u", current->pid, (unsigned long long)address, (unsigned long long)count, woken); // 明确私有进程与实际数量。
    return woken; // 0表示此刻没有匹配等待，不保存通知信用。
}
static void poll_waiters(void) { // 仅普通内核上下文运行，IRQ本身不复制、分配或打印。
```

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
                    result = 0; // 唯一收取成功。
                }
            }
        } else if (execution->io == IO_FUTEX) { // 内存字变化不会自动结束等待，必须显式wake或截止。
            if (airi_timer_ticks() < execution->io_deadline) // 未到期只保留登记，不重新读取条件字。
                continue; // 用户需要先发布条件，再调用wake。
            result = -110; // -ETIMEDOUT，不伪造成功通知。
```
<!-- after -->
```c
                    result = 0; // 唯一收取成功。
                }
            }
        } else if (execution->io == IO_FUTEX || execution->io == IO_FUTEX64) { // 四/八字节内存等待都只由显式通知或截止结束。
            if (airi_timer_ticks() < execution->io_deadline) // 未到期只保留登记，不重新读取条件字。
                continue; // 用户需要先发布条件，再调用wake。
            result = -110; // -ETIMEDOUT，不伪造成功通知。
```

<a id="wide-futex-dispatch"></a>
## 先固定本次宽度，再把RAX写成返回值

同一分支处理43号和46号，先保存wide=f->rax==46，之后RAX会被返回值覆盖，不能再据它判断本次是什么键。四字节请求保留期望值不超过UINT32_MAX的限制，八字节请求接受完整uint64_t，包括最高位全1。两种预算都先以完整RDX检查，不把2^32截成零。

读取以后统一比较value与RSI；不相同立即-11，相同而零预算-110，正常有预算才登记。io分别保存IO_FUTEX或IO_FUTEX64，deadline仍只算一次。44／47号通知同样先选kind再写返回值。日志用不同标签区分宽键，原窄日志的字段值与结果保持。

poll_waiters把两种futex都交给相同截止路径，不因为用户内存后来变化就自行醒来。显式通知和到期仍是结束登记的两种原因；FD关闭保护只检查真正借FD的操作，不把新增宽等待误当FD0读取。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
        f->rax = running->tid; // 内核身份始终来自真实正在运行的执行者。
        airi_log("[THREAD_ID_QUERY] pid=%u tid=%u", current->pid, running->tid); // 普通系统调用中记录真实陷入，不能用用户缓存计数冒充。
    }
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
    else if (f->rax == 45) { // 每执行者观察计数，不把其他工作者的请求混入同一次测量。
        f->rax = running->syscall_count; // 包括本次查询入口，测量差值需扣掉结束查询的一次。
        airi_log("[THREAD_SYSCALLS] pid=%u tid=%u count=%llu", current->pid, running->tid, (unsigned long long)running->syscall_count); // 真实内核计数，不是用户推测。
```
<!-- after -->
```c
        f->rax = running->tid; // 内核身份始终来自真实正在运行的执行者。
        airi_log("[THREAD_ID_QUERY] pid=%u tid=%u", current->pid, running->tid); // 普通系统调用中记录真实陷入，不能用用户缓存计数冒充。
    }
    else if (f->rax == 43 || f->rax == 46) { // 保留四字节接口，46号显式选择八字节私有等待。
        bool wide = f->rax == 46; // 在返回值覆盖RAX以前固定本次宽度。
        uint64_t value; // 类型化读取后统一比较，宽等待保留高位，不跨阻塞保留HHDM指针。
        uint64_t now = airi_timer_ticks(); // 截止只计算一次，不因轮询延长。
        if ((f->rdi & (wide ? 7 : 3)) || (!wide && f->rsi > UINT32_MAX) || f->rdx > 6000) // 八/四对齐、窄期望值与完整超时先检查。
            f->rax = (uint64_t)-22ll; // -EINVAL，尚未读取或登记。
        else if (!futex_value(f->rdi, wide, &value)) // 以当前进程页表验证并复制所选完整跨度。
            f->rax = (uint64_t)-14ll; // -EFAULT，内核不直接解引用用户VA。
        else if (value != f->rsi) // 另一执行者若已发布新值，旧等待决定已失效。
            f->rax = (uint64_t)-11ll; // -EAGAIN，立即返回用户重查条件。
        else if (!f->rdx) // 值相等但不给等待预算，也不能登记一个永不到期项。
            f->rax = (uint64_t)-110ll; // -ETIMEDOUT。
        else if (now > UINT64_MAX - f->rdx) // 截止加法不能回绕。
            f->rax = (uint64_t)-75ll; // -EOVERFLOW。
        else { // 当前固定单CPU、内核不可抢占；比较到登记之间没有其他用户store或wake。
            running->io = wide ? IO_FUTEX64 : IO_FUTEX; // 宽度进入等待键，不仅决定本次读取多少字节。
            running->io_fd = running->io_events = 0; // 内存等待不占FD。
            running->io_address = f->rdi; // 保存用户VA；进程指针已由execution持有。
            running->io_bytes = 0; // 不保存旧字值；醒来由用户重查同步协议。
            running->io_deadline = now + f->rdx; // 到期由普通服务栈结束原调用。
            running->state = USER_IO_WAIT; // 条件完整后再从可运行集合撤销。
            ++io_block_count; // 只记录真实阻塞，不计EAGAIN。
            airi_log(wide ? "[FUTEX64_WAIT] pid=%u tid=%u address=0x%llx expected=%llu" : "[FUTEX_WAIT] pid=%u tid=%u address=0x%llx expected=%llu", current->pid, running->tid, (unsigned long long)f->rdi, (unsigned long long)value); // 明确本次登记的实际身份。
            return schedule(REASON_IO); // 其他执行者继续；全阻塞时回启动栈hlt。
        }
    }
    else if (f->rax == 44 || f->rax == 47) { // 两种宽度分别通知至多给定数量，不混合匹配。
        enum io_operation kind = f->rax == 47 ? IO_FUTEX64 : IO_FUTEX; // 先固定键类型再写返回值。
        f->rax = (uint64_t)futex_wake(f->rdi, f->rsi, kind); // 不修改用户字，返回真正唤醒的数量。
    }
    else if (f->rax == 45) { // 每执行者观察计数，不把其他工作者的请求混入同一次测量。
        f->rax = running->syscall_count; // 包括本次查询入口，测量差值需扣掉结束查询的一次。
        airi_log("[THREAD_SYSCALLS] pid=%u tid=%u count=%llu", current->pid, running->tid, (unsigned long long)running->syscall_count); // 真实内核计数，不是用户推测。
```

<a id="wide-futex-user-entry"></a>
## 头和寄存器参数都保留完整宽度

在airi_threads.h增加wait64／wake64，threads.c追加两个包装。46号的RSI接收uint64_t expected，不经unsigned或uint32_t中间变量；47号明确只通知宽键。原32位函数名字与编号不改，调用者不需要猜地址指向什么类型来决定内核行为。

<!-- replace: labs/airios/include/airi_threads.h -->
<!-- before -->
```c
uint64_t airi_thread_syscalls(void); // 当前执行者累计用户请求数，含本次查询，fork/worker重新开始，exec保留。
long airi_futex_wait(uint32_t *word, uint32_t expected, unsigned ticks); // 私有键、四字节对齐，有限PIT预算；0通知、-11值不符、-110到期。
long airi_futex_wake(uint32_t *word, unsigned count); // 至多唤醒十六个同进程已登记者，不修改字值，不累计通知。
#ifdef __cplusplus // 结束链接语言范围。
} // 工作函数ABI仍为普通当前目标C调用。
#endif // 结束语言差异。
```
<!-- after -->
```c
uint64_t airi_thread_syscalls(void); // 当前执行者累计用户请求数，含本次查询，fork/worker重新开始，exec保留。
long airi_futex_wait(uint32_t *word, uint32_t expected, unsigned ticks); // 私有键、四字节对齐，有限PIT预算；0通知、-11值不符、-110到期。
long airi_futex_wake(uint32_t *word, unsigned count); // 至多唤醒十六个同进程已登记者，不修改字值，不累计通知。
long airi_futex_wait64(uint64_t *word, uint64_t expected, unsigned ticks); // 八字节对齐，全64位比较，键与同VA四字节等待隔离。
long airi_futex_wake64(uint64_t *word, unsigned count); // 只通知同进程/VA的八字节等待，不改值或唤醒窄等待。
#ifdef __cplusplus // 结束链接语言范围。
} // 工作函数ABI仍为普通当前目标C调用。
#endif // 结束语言差异。
```

<!-- append: labs/airios/libc/threads.c -->
```c
long airi_futex_wait64(uint64_t *word, uint64_t expected, unsigned ticks) { // 自有46号，不是Linux futex扩展ABI。
    return thread_call(46, (uintptr_t)word, expected, ticks); // 高32位完整进入RSI，参数不窄化。
}
long airi_futex_wake64(uint64_t *word, unsigned count) { // 宽键显式使用47号，不能让调用者自己猜宽度。
    return thread_call(47, (uintptr_t)word, count, 0); // 同样返回真实已唤醒数量。
}
```

<a id="wide-futex-observation"></a>
## 先通知无人，再用旧1检验高位比较

新建futex64-probe.c。word显式八字节对齐，marker低四字节不变，ready表示两位worker已开始，returned位0表示窄等待者、位1表示宽等待者。静态检查要求当前目标八字节原子可直接生成指令，不引入缺失的libatomic。

raw为完整参数错误观察保留三个64位寄存器值，不能通过本来只接收unsigned的C包装测试2^32参数，否则错误可能在进入内核以前就被截掉。

<!-- write: labs/airios/futex64-probe.c -->
```c
#include <airi_threads.h> // 同时调用显式四字节与八字节接口。
#include <airi_libc.h> // 真实PIT、串口及主动交接。
#include <stdatomic.h> // 夹具就绪和已返回位图分别原子发布。
static _Alignas(8) uint64_t word = 1; // _Alignas明确要求八字节边界，两个等待宽度故意用同一个VA。
static const uint64_t marker = 0x1122334400000001ull; // 只改变高32位，低四字节仍为1。
static _Atomic unsigned ready, returned; // 位0代表窄等待者，位1代表宽等待者。
_Static_assert(__atomic_always_lock_free(8, 0), "probe requires native eight-byte atomics"); // 不把用户发布偷偷交给缺失libatomic。
static void require(int condition, const char *message) { // 错误结束本真实用户进程组。
    if (!condition) { airi_puts(message); airi_puts("\n"); airi_exit(91); } // 不继续输出成功。
}
static long raw(long number, uintptr_t address, uint64_t second, uint64_t third) { // 绕开unsigned包装，检查内核完整宽参数。
    __asm__ volatile("int $0x80" : "+a"(number) : "D"(address), "S"(second), "d"(third) : "memory", "cc"); // 保留64位数量与预算。
    return number; // 原始负错误不写errno。
}
```

waiter的argument由主传入0或1，分别选择窄等待1和宽等待marker；两个调用传入同一个word地址。它只把窄指针的数值交给接口，不在用户C里用不兼容的uint32_t类型解引用这个uint64_t对象；读取对象表示由内核受控复制完成。原WAIT返回0以后，它才将returned对应位设为1，因此主能区分“已开始”和“这一种等待已经返回”。

<!-- append: labs/airios/futex64-probe.c -->
```c
static long waiter(void *argument) { // 同地址分别登记两种宽度，均不修改word。
    unsigned wide = (unsigned)(uintptr_t)argument; // 0为32位，1为64位。
    atomic_fetch_add_explicit(&ready, 1, memory_order_release); // 只表示已经开始，主随后让出三tick供真正登记。
    long result = wide ? airi_futex_wait64(&word, marker, 200) : airi_futex_wait((uint32_t *)&word, 1, 200); // 仅传窄指针数值，用户不以不兼容类型解引用word。
    if (result) return result; // 超时或错误不能记作通知完成。
    atomic_fetch_or_explicit(&returned, 1u << wide, memory_order_release); // 标明实际返回的是哪位。
    return 0; // 同一个原等待调用正常完成。
}
```

main先检验四字节对齐但不满足八字节的地址、内核高地址、2^32数量与预算。发布marker后，WAKE64返回无人0，WAIT64旧1必须立即-11。UINT64_MAX也是合法期望，只是与当前值不同，应-11而非窄值EINVAL。

接着窄接口用1、宽接口用marker，各配零预算都立即-110，没有登记；宽接口再给三tick，必须真实等待到期。这里值比较、参数宽度和到期是分别观察的，不用同一个成功字符串掩盖不同分支。

<!-- append: labs/airios/futex64-probe.c -->
```c
int main(void) { // 先检查完整数值，再观察同VA不同宽度的键。
    require(airi_futex_wait64((uint64_t *)((uintptr_t)&word + 4), 1, 1) == -22 && airi_futex_wake64((uint64_t *)((uintptr_t)&word + 4), 1) == -22, "futex64 alignment"); // 四字节对齐不足以满足宽接口。
    require(airi_futex_wait64((uint64_t *)(uintptr_t)0xffff800000000000ull, 1, 1) == -14 && airi_futex_wake64((uint64_t *)(uintptr_t)0xffff800000000000ull, 1) == -14, "futex64 invalid address"); // 内核拒绝高地址，不直接解引用。
    require(raw(46, (uintptr_t)&word, 1, 1ull << 32) == -22 && raw(47, (uintptr_t)&word, 1ull << 32, 0) == -22, "futex64 full budget and count"); // 不把过宽参数截成0。
    __atomic_store_n(&word, marker, __ATOMIC_RELEASE); // 发布前1，发布后只高位变化，所有后续worker只读。
    require(airi_futex_wake64(&word, 16) == 0 && airi_futex_wait64(&word, 1, 20) == -11, "futex64 prewake high comparison"); // 先通知0人，旧低值1也不能误睡。
    require(airi_futex_wait64(&word, UINT64_MAX, 1) == -11, "futex64 full expected range"); // 高位全1是合法期望，比较不符而不是窄值EINVAL。
    require(airi_futex_wait((uint32_t *)&word, 1, 0) == -110 && airi_futex_wait64(&word, marker, 0) == -110, "futex64 zero budget"); // 两种宽度相符而无预算都不到队列。
    uint64_t start = airi_ticks(); // 宽等待也由同一PIT截止结束。
    require(airi_futex_wait64(&word, marker, 3) == -110 && airi_ticks() - start >= 3, "futex64 real timeout"); // 必须实际睡过三tick。
```

<a id="wide-futex-two-orders"></a>
## 两人同VA登记，两种通知顺序都只完成自己

每轮主创建一窄一宽两位worker，地址完全相同。ready=2以后再让出三tick给实际登记。第一轮先WAKE32，必须恰返回1，随后returned只有位0；宽者仍未返回。再WAKE64才让位1完成，join两人均0，word保持marker。

第二轮反过来，先WAKE64只出现位1，再WAKE32完成位0。只测一个顺序不足以证明两个方向都隔离：若总先把窄者取走，再看宽通知，就可能漏掉宽通知本来会误唤醒窄者的问题。每轮都先join再清夹具，最后WAKE64无人0，确认没有残留登记。

<!-- append: labs/airios/futex64-probe.c -->
```c
    for (unsigned round = 0; round < 2; ++round) { // 一轮先窄后宽，另一轮相反，双向检查不混唤醒。
        atomic_store_explicit(&ready, 0, memory_order_relaxed); atomic_store_explicit(&returned, 0, memory_order_relaxed); // 前轮已join，才清夹具状态。
        long narrow = airi_thread_create(waiter, (void *)(uintptr_t)0), wide = airi_thread_create(waiter, (void *)(uintptr_t)1), result; // 两人共享同一进程与word地址。
        require(narrow > 0 && wide > 0, "futex64 typed waiters"); // 不能用单线程冒充隔离。
        start = airi_ticks(); // 有限准备期限。
        while (atomic_load_explicit(&ready, memory_order_acquire) != 2) { require(airi_ticks() - start < 100, "futex64 waiters ready"); airi_yield(); } // 真正让两worker运行。
        require(airi_sleep(3) == 0, "futex64 both registered"); // 后面内核日志分别核对实际登记。
        long first = round ? airi_futex_wake64(&word, 16) : airi_futex_wake((uint32_t *)&word, 16); // 第一次只选择一个宽度。
        require(first == 1 && airi_sleep(3) == 0 && atomic_load_explicit(&returned, memory_order_acquire) == (round ? 2u : 1u), "futex64 first type isolated"); // 另一宽度必须仍未返回。
        long second = round ? airi_futex_wake((uint32_t *)&word, 16) : airi_futex_wake64(&word, 16); // 再通知另一种键。
        require(second == 1 && airi_thread_join(narrow, &result) == 0 && result == 0 && airi_thread_join(wide, &result) == 0 && result == 0, "futex64 second type isolated"); // 两个原调用分别完成。
        require(atomic_load_explicit(&returned, memory_order_acquire) == 3 && __atomic_load_n(&word, __ATOMIC_ACQUIRE) == marker, "futex64 word unchanged"); // 通知不改原子字。
    }
    require(airi_futex_wake64(&word, 16) == 0, "futex64 no leftover waiter"); // 四个worker都已join，不能留幽灵登记。
    airi_puts("[FUTEX64_USER_OK] high_bits=compared timeout=pit widths=isolated rounds=2 word=unchanged\n"); // 参数、值、超时及双向键隔离都完成以后才输出。
    return 0; // shell收取真实成功。
}
```

<a id="wide-futex-build-evidence"></a>
## 新宽接口与旧32位回归在同一内核共存

futex64.elf只链接既有原始线程库和入口，用户观察直接发46／47号，不借pthread或宿主同步。打包完整ELF并注册futex64，串口默认路径仍不读取前端或字体。

<!-- append: Makefile -->
```makefile

# 宽等待观察只选择原始线程库，不把pthread语义混入46/47号。
$(OUT)/user/futex64.elf: $(USER_COMMON) $(OUT)/user/threads-lib.o $(OUT)/user/thread-entry.o $(OUT)/user/futex64-probe.o labs/airios/program.ld
# 静态链接真实八/四字节比较与通知调用者。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(filter %.o,$^) -o $@
# 初始目录只借实际完成ELF。
$(OUT)/labs/airios/images.o: $(OUT)/user/futex64.elf
# 当前配置路径交给incbin。
$(OUT)/labs/airios/images.o: CPPFLAGS += -DAIRI_FUTEX64_ELF='"$(OUT)/user/futex64.elf"'
```

<!-- append: labs/airios/images.S -->
```asm
.balign 16 /* 八字节私有等待的真实用户观察。 */
.global airi_futex64_elf_start, airi_futex64_elf_end /* 目录借完整文件边界。 */
airi_futex64_elf_start: /* 从静态ELF头校验。 */
.incbin AIRI_FUTEX64_ELF /* 不使用宿主futex替身。 */
airi_futex64_elf_end: /* 右端不含字节。 */
```

ELF已经有完整打包范围，接着在kernel/process/user.c的边界声明和目录表中加入futex64：

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
extern const uint8_t airi_pthread_elf_start[], airi_pthread_elf_end[]; // POSIX形状的用户包装不替代真实共享执行者。
extern const uint8_t airi_join_elf_start[], airi_join_elf_end[]; // 对等worker收取与有界环检测观察。
extern const uint8_t airi_pthread_mutex_elf_start[], airi_pthread_mutex_elf_end[]; // 默认类型与对象生命周期的用户观察。
extern const uint8_t airi_identity_elf_start[], airi_identity_elf_end[]; // 原始fork、exec与工作槽复用中的本地TID观察。
extern const uint8_t airi_heap_threads_elf_start[], airi_heap_threads_elf_end[]; // 同一arena并发分配、跨执行者归还与快照。
extern const uint8_t airi_mutex_elf_start[], airi_mutex_elf_end[]; // 原子所有权与futex等待组合的真实用户观察。
```
<!-- after -->
```c
extern const uint8_t airi_pthread_elf_start[], airi_pthread_elf_end[]; // POSIX形状的用户包装不替代真实共享执行者。
extern const uint8_t airi_join_elf_start[], airi_join_elf_end[]; // 对等worker收取与有界环检测观察。
extern const uint8_t airi_pthread_mutex_elf_start[], airi_pthread_mutex_elf_end[]; // 默认类型与对象生命周期的用户观察。
extern const uint8_t airi_futex64_elf_start[], airi_futex64_elf_end[]; // 同地址宽度隔离和完整64位比较。
extern const uint8_t airi_identity_elf_start[], airi_identity_elf_end[]; // 原始fork、exec与工作槽复用中的本地TID观察。
extern const uint8_t airi_heap_threads_elf_start[], airi_heap_threads_elf_end[]; // 同一arena并发分配、跨执行者归还与快照。
extern const uint8_t airi_mutex_elf_start[], airi_mutex_elf_end[]; // 原子所有权与futex等待组合的真实用户观察。
```

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    {"pthread", airi_pthread_elf_start, airi_pthread_elf_end}, // 正错误、启动包和指针返回观察。
    {"join", airi_join_elf_start, airi_join_elf_end}, // 同组认领、坏输出及循环等待拒绝。
    {"pthread-mutex", airi_pthread_mutex_elf_start, airi_pthread_mutex_elf_end}, // 公开互斥接口与真实内部等待重试。
    {"identity", airi_identity_elf_start, airi_identity_elf_end}, // 版本化运行头和原始查询对照。
    {"identity-reset", airi_identity_elf_start, airi_identity_elf_end}, // 成功exec后沿用PID并重建运行头。
    {"heap-threads", airi_heap_threads_elf_start, airi_heap_threads_elf_end}, // 显式持内部锁yield的用户观察程序。
```
<!-- after -->
```c
    {"pthread", airi_pthread_elf_start, airi_pthread_elf_end}, // 正错误、启动包和指针返回观察。
    {"join", airi_join_elf_start, airi_join_elf_end}, // 同组认领、坏输出及循环等待拒绝。
    {"pthread-mutex", airi_pthread_mutex_elf_start, airi_pthread_mutex_elf_end}, // 公开互斥接口与真实内部等待重试。
    {"futex64", airi_futex64_elf_start, airi_futex64_elf_end}, // 宽键与原四字节键共存。
    {"identity", airi_identity_elf_start, airi_identity_elf_end}, // 版本化运行头和原始查询对照。
    {"identity-reset", airi_identity_elf_start, airi_identity_elf_end}, // 成功exec后沿用PID并重建运行头。
    {"heap-threads", airi_heap_threads_elf_start, airi_heap_threads_elf_end}, // 显式持内部锁yield的用户观察程序。
```

宿主要求三次FUTEX64_WAIT：主三tick超时，加两轮宽worker；两次FUTEX_WAIT来自两轮窄worker。五次记录的VA必须相同，宽期望值均为完整marker，窄期望均1，不能拿两个不同变量冒充同址隔离。

宽通知实际人数依次0、1、1、0，分别是早通知、两轮宽释放、末尾清理；窄通知两次均1。无效调用在登记或通知以前被拒绝，不产生成功通知记录。guest另以returned位图确认第一次通知以后另一宽度还没有返回，不能只看总共最终两人都结束。

<!-- replace: scripts/shell_test.py -->
<!-- before -->
```python
                assert '[FUTEX_WAIT]' in pmutex_output and 'result=-110' in pmutex_output and 'operation=sleep ticks=205' in pmutex_output # 底层确实到期后重试，公开两位仍完成。
                pmutex_owner=re.search(r'\[EXEC_COMMIT\] pid=(\d+) program=pthread-mutex ',pmutex_output) # 不固定本轮PID。
                assert pmutex_owner and len(re.findall(r'\[THREAD_CREATE\] pid='+pmutex_owner.group(1)+r' tid=',pmutex_output))==2 and len(re.findall(r'\[THREAD_RETURN\] pid='+pmutex_owner.group(1)+r' tid=',pmutex_output))==2 # 不是主或宿主代算64次更新。
                offset=len(text); wire.sendall(b'heap-threads\n'); text=wait('[SHELL_COMMAND_DONE] name=heap-threads code=0',offset) # 四个真实执行者共享唯一arena，观察对象交接。
                heap_threads=text[offset:] # 不用旧heap命令代替并发结果。
                assert '[MALLOC_THREADS_OK] workers=4 rounds=96 arena=262144 cross_free=4 snapshots=consistent errno=private critical_yield=1' in heap_threads # 每个工作者和主快照均由guest核对。
```
<!-- after -->
```python
                assert '[FUTEX_WAIT]' in pmutex_output and 'result=-110' in pmutex_output and 'operation=sleep ticks=205' in pmutex_output # 底层确实到期后重试，公开两位仍完成。
                pmutex_owner=re.search(r'\[EXEC_COMMIT\] pid=(\d+) program=pthread-mutex ',pmutex_output) # 不固定本轮PID。
                assert pmutex_owner and len(re.findall(r'\[THREAD_CREATE\] pid='+pmutex_owner.group(1)+r' tid=',pmutex_output))==2 and len(re.findall(r'\[THREAD_RETURN\] pid='+pmutex_owner.group(1)+r' tid=',pmutex_output))==2 # 不是主或宿主代算64次更新。
                offset=len(text); wire.sendall(b'futex64\n'); text=wait('[SHELL_COMMAND_DONE] name=futex64 code=0',offset) # 同进程同VA的两种等待宽度必须隔离。
                wide_output=text[offset:] # 只核对本条命令的宽比较、截止和通知。
                assert '[FUTEX64_USER_OK] high_bits=compared timeout=pit widths=isolated rounds=2 word=unchanged' in wide_output # guest已核对位图、错误和完整word。
                wide_waits=re.findall(r'\[FUTEX64_WAIT\] pid=\d+ tid=\d+ address=(0x[0-9a-f]+) expected=(\d+)',wide_output) # 一主超时和两轮宽worker。
                narrow_waits=re.findall(r'\[FUTEX_WAIT\] pid=\d+ tid=\d+ address=(0x[0-9a-f]+) expected=(\d+)',wide_output) # 每轮一位窄worker。
                assert len(wide_waits)==3 and len(narrow_waits)==2 and len({address for address,expected in wide_waits+narrow_waits})==1 # 真正相同VA，不是用不同变量逃开隔离要求。
                assert all(expected==str(0x1122334400000001) for address,expected in wide_waits) and all(expected=='1' for address,expected in narrow_waits) # 高32位保留，窄键只看自己的四字节。
                assert re.findall(r'\[FUTEX64_WAKE\].*count=16 woken=(\d+)',wide_output)==['0','1','1','0'] and re.findall(r'\[FUTEX_WAKE\].*count=16 woken=(\d+)',wide_output)==['1','1'] # 两种先后顺序都各唤醒自己一人。
                offset=len(text); wire.sendall(b'heap-threads\n'); text=wait('[SHELL_COMMAND_DONE] name=heap-threads code=0',offset) # 四个真实执行者共享唯一arena，观察对象交接。
                heap_threads=text[offset:] # 不用旧heap命令代替并发结果。
                assert '[MALLOC_THREADS_OK] workers=4 rounds=96 arena=262144 cross_free=4 snapshots=consistent errno=private critical_yield=1' in heap_threads # 每个工作者和主快照均由guest核对。
```

<!-- replace: scripts/shell_test.py -->
<!-- before -->
```python
                offset=len(text); wire.sendall(b'echo after-shell\n'); text=wait('[CONSOLE_UPDATE] view=0 commands=2 page=0',offset) # 回到内核后输入仍完整。
                assert 'after-shell' in text[offset:] # 不只是退出而丢了后续UART。
                result={'passed':True,'ring3_shell':True,'ram_file_and_device_readback':True,'other_user_during_cq_wait':True,'qmp_iops':1,'error_exec_survived':True,'returned_to_monitor':True,'orphan_rounds':24,'memory_reuse_rounds':128,'capacity_rounds':4,'capacity_pages':1024,'simd_preempt_fork_exec':True,'fs_base_preempt_fork_exec':True,'compiler_static_tls':True,'posix_errno_io_subset':True,'positioned_vectors':True} # 只发布已观察事实。
                result.update(pthread_mutex_source=True,pthread_mutex_internal_timeout_retried=True,peer_worker_join=True,join_claim_unique=True,join_cycle_rejected=True,pthread_source_subset=True,pthread_workers=36,pthread_packets_reclaimed=True,bounded_queue=True,queue_capacity=4,queue_jobs=48,queue_close_drains=True,sequence_event=True,event_waits=6,event_broadcast_workers=2,event_false_wake_rechecked=True,mutex_race_stages=4,mutex_race_actual_waits=1,mutex_race_mismatches=1,per_execution_syscall_count=True,uncontended_mutex_pairs=8,uncontended_mutex_syscalls=0,runtime_tcb_identity=True,identity_reference_queries=7,shared_malloc=True,malloc_workers=4,malloc_rounds=96,owned_mutex=True,mutex_critical_updates=64,private_futex=True,futex_registrations=8,user_heap=True,shared_executions=True,thread_commands=2,worker_capacity=8,worker_reuse_rounds=24) # 关键字把实际已验收字段加入结果字典，不替代上面的逐命令检查。
                result_path.write_text(json.dumps(result,indent=2)+'\n'); print('PASS user shell: alloc/file/async wait/other task/CQ/readback, failed exec, return to monitor') # 阶段演示实际完成。
                return result # 总入口可收集。
            finally: # 包括断言、超时和QEMU错误。
```
<!-- after -->
```python
                offset=len(text); wire.sendall(b'echo after-shell\n'); text=wait('[CONSOLE_UPDATE] view=0 commands=2 page=0',offset) # 回到内核后输入仍完整。
                assert 'after-shell' in text[offset:] # 不只是退出而丢了后续UART。
                result={'passed':True,'ring3_shell':True,'ram_file_and_device_readback':True,'other_user_during_cq_wait':True,'qmp_iops':1,'error_exec_survived':True,'returned_to_monitor':True,'orphan_rounds':24,'memory_reuse_rounds':128,'capacity_rounds':4,'capacity_pages':1024,'simd_preempt_fork_exec':True,'fs_base_preempt_fork_exec':True,'compiler_static_tls':True,'posix_errno_io_subset':True,'positioned_vectors':True} # 只发布已观察事实。
                result.update(private_futex64=True,futex_width_isolation_rounds=2,pthread_mutex_source=True,pthread_mutex_internal_timeout_retried=True,peer_worker_join=True,join_claim_unique=True,join_cycle_rejected=True,pthread_source_subset=True,pthread_workers=36,pthread_packets_reclaimed=True,bounded_queue=True,queue_capacity=4,queue_jobs=48,queue_close_drains=True,sequence_event=True,event_waits=6,event_broadcast_workers=2,event_false_wake_rechecked=True,mutex_race_stages=4,mutex_race_actual_waits=1,mutex_race_mismatches=1,per_execution_syscall_count=True,uncontended_mutex_pairs=8,uncontended_mutex_syscalls=0,runtime_tcb_identity=True,identity_reference_queries=7,shared_malloc=True,malloc_workers=4,malloc_rounds=96,owned_mutex=True,mutex_critical_updates=64,private_futex=True,futex_registrations=8,user_heap=True,shared_executions=True,thread_commands=2,worker_capacity=8,worker_reuse_rounds=24) # 关键字把实际已验收字段加入结果字典，不替代上面的逐命令检查。
                result_path.write_text(json.dumps(result,indent=2)+'\n'); print('PASS user shell: alloc/file/async wait/other task/CQ/readback, failed exec, return to monitor') # 阶段演示实际完成。
                return result # 总入口可收集。
            finally: # 包括断言、超时和QEMU错误。
```

完整实现闭合，运行：

<!-- run -->
```sh
./airios build  # 同时编译完整64位比较及原32位接口
```

<!-- run -->
```sh
./airios test  # 旧等待回归、宽高位比较和两种同址通知顺序
```

<a id="wide-futex-results"></a>
## 字节跨度改变以后，等待身份也保持明确

交互执行`./airios run`、`run sh`、`futex64`，本轮实际宽等待字地址为0x402000，期望打印1234605615003729921，即0x1122334400000001。

```text
[FUTEX64_USER_OK] high_bits=compared timeout=pit widths=isolated rounds=2 word=unchanged
[SHELL_COMMAND_DONE] name=futex64 code=0
```

五次真实登记用相同VA；每次通知只完成本宽度的一人，另一种要等自己的通知。高位变化能使旧1返回-11，内核高地址与过宽数量都明确拒绝后程序继续。原有32位futex、用户互斥、队列、pthread与peer join仍由完整test回归。

这是AiriOS自有八字节接口，不宣称兼容Linux futex。进程私有隔离继续复用同一process检查，本节新观察重点是完整高位与宽度；没有把新的64位跨进程实验记成已运行。也没有只增加比较宽度就宣称旧event已经升级，下一阶段还要处理完整uint64_t快照怎样与负错误分别返回，再连接长驻通知。
