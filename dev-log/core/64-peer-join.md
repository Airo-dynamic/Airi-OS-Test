# K64 · 对等join的唯一认领与循环等待

K63允许worker创建其他worker，但只有主收取结果。现在让worker也能join同组worker，首先要回答两个问题：主与A同时要收取B，谁拥有这一次结果；A已经等B，B再来等A，会不会把两人都睡下。只删掉“不是主就拒绝”的检查，无法处理这两个状态。

从 K63 的同一目录继续。本节给每个worker结果增加唯一收取者，登记新等待边以前检查有界join链。正常子执行者由创建它的worker收取，重复认领与循环分别明确拒绝；主执行者本体仍不是可join目标，它独立退出的语义尚未实现。

<a id="join-two-directions"></a>
## A等谁，与谁能收取B，是两份相连的状态

此前只有一位主执行者能join，不需要另存“这个目标已许给谁”。放开以后，A可能已在等B，主又来join B；如果两人都登记，B返回的同一个结果可能被重复交出，先收取者还可能让另一人引用待销毁的槽。因此认领记在目标上，而不只存在等待者的局部变量里。

已有join_tid保存“我正在等哪个TID”，新增joiner_tid保存“哪个TID已认领我的结果”。在A等待B时，两边分别为A.join_tid=B.tid、B.joiner_tid=A.tid。主试图收取B就检查B的joiner_tid，已有A则返回-22，不改变A资格。0表示尚无认领，实际TID非零。

唯一认领仍不能单独防死锁：A认领B、B认领A，每个目标都只给一人，但两人都等对方结束。新边running->target要写入以前，从target沿join_tid向前走；如果回到running，拒绝-35，不登记这条边。链的方向来自正在等待的执行者，不能反过来沿joiner_tid走。

修改user_execution的字段声明。其余源码修改也在同一文件，本节后面逐项解释；先完成这些确定窗口，再更新用户入口。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
struct user_process; // 执行者回指所属进程，资源定义稍后闭合。
struct user_execution { // 一条用户执行链的CPU现场、内核陷入栈和等待条件。
    struct user_process *process; // 借所属进程的共享内存、FD与异步owner，不独立归还它们。
    unsigned tid, join_tid; // 全局不复用的执行身份，以及当前主执行者等待的工作身份。
    long result; // 工作函数的64位返回值，join取得后才能收拢执行者。
    uint64_t fs_base; // 当前执行者的用户FS基址，不拥有对应用户数据页。
    uint64_t fpu_physical; // 独立内核状态页，保存本执行者的x87/SSE寄存器。
```
<!-- after -->
```c
struct user_process; // 执行者回指所属进程，资源定义稍后闭合。
struct user_execution { // 一条用户执行链的CPU现场、内核陷入栈和等待条件。
    struct user_process *process; // 借所属进程的共享内存、FD与异步owner，不独立归还它们。
    unsigned tid, join_tid, joiner_tid; // 本执行者身份、正在等待的目标，以及认领本执行者结果的唯一收取者；0表示尚无。
    long result; // 工作函数的64位返回值，join取得后才能收拢执行者。
    uint64_t fs_base; // 当前执行者的用户FS基址，不拥有对应用户数据页。
    uint64_t fpu_physical; // 独立内核状态页，保存本执行者的x87/SSE寄存器。
```

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
extern const uint8_t airi_events_elf_start[], airi_events_elf_end[]; // 用户变化通知通过同一真实futex运行。
extern const uint8_t airi_queue_elf_start[], airi_queue_elf_end[]; // 队列算法位于用户态，内核继续提供既有线程/等待。
extern const uint8_t airi_pthread_elf_start[], airi_pthread_elf_end[]; // POSIX形状的用户包装不替代真实共享执行者。
extern const uint8_t airi_identity_elf_start[], airi_identity_elf_end[]; // 原始fork、exec与工作槽复用中的本地TID观察。
extern const uint8_t airi_heap_threads_elf_start[], airi_heap_threads_elf_end[]; // 同一arena并发分配、跨执行者归还与快照。
extern const uint8_t airi_mutex_elf_start[], airi_mutex_elf_end[]; // 原子所有权与futex等待组合的真实用户观察。
```
<!-- after -->
```c
extern const uint8_t airi_events_elf_start[], airi_events_elf_end[]; // 用户变化通知通过同一真实futex运行。
extern const uint8_t airi_queue_elf_start[], airi_queue_elf_end[]; // 队列算法位于用户态，内核继续提供既有线程/等待。
extern const uint8_t airi_pthread_elf_start[], airi_pthread_elf_end[]; // POSIX形状的用户包装不替代真实共享执行者。
extern const uint8_t airi_join_elf_start[], airi_join_elf_end[]; // 对等worker收取与有界环检测观察。
extern const uint8_t airi_identity_elf_start[], airi_identity_elf_end[]; // 原始fork、exec与工作槽复用中的本地TID观察。
extern const uint8_t airi_heap_threads_elf_start[], airi_heap_threads_elf_end[]; // 同一arena并发分配、跨执行者归还与快照。
extern const uint8_t airi_mutex_elf_start[], airi_mutex_elf_end[]; // 原子所有权与futex等待组合的真实用户观察。
```

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    {"events", airi_events_elf_start, airi_events_elf_end}, // 版本快照、无变化通知与广播观察。
    {"queue", airi_queue_elf_start, airi_queue_elf_end}, // 有界生产消费、关闭排空与背压。
    {"pthread", airi_pthread_elf_start, airi_pthread_elf_end}, // 正错误、启动包和指针返回观察。
    {"identity", airi_identity_elf_start, airi_identity_elf_end}, // 版本化运行头和原始查询对照。
    {"identity-reset", airi_identity_elf_start, airi_identity_elf_end}, // 成功exec后沿用PID并重建运行头。
    {"heap-threads", airi_heap_threads_elf_start, airi_heap_threads_elf_end}, // 显式持内部锁yield的用户观察程序。
```
<!-- after -->
```c
    {"events", airi_events_elf_start, airi_events_elf_end}, // 版本快照、无变化通知与广播观察。
    {"queue", airi_queue_elf_start, airi_queue_elf_end}, // 有界生产消费、关闭排空与背压。
    {"pthread", airi_pthread_elf_start, airi_pthread_elf_end}, // 正错误、启动包和指针返回观察。
    {"join", airi_join_elf_start, airi_join_elf_end}, // 同组认领、坏输出及循环等待拒绝。
    {"identity", airi_identity_elf_start, airi_identity_elf_end}, // 版本化运行头和原始查询对照。
    {"identity-reset", airi_identity_elf_start, airi_identity_elf_end}, // 成功exec后沿用PID并重建运行头。
    {"heap-threads", airi_heap_threads_elf_start, airi_heap_threads_elf_end}, // 显式持内部锁yield的用户观察程序。
```

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
        f->rax = (uint64_t)create_worker(f->rdi, f->rsi, f->rdx); // 函数入口和用户参数完整复制成寄存器值。
    else if (f->rax == 40) // 用户工作函数返回，只结束本执行者。
        return return_worker(f); // 不关闭进程共享FD或异步owner。
    else if (f->rax == 41) // 主执行者收取一次同组工作结果。
        return join_worker(f); // 未完成时真正阻塞于原frame。
    else if (f->rax == 42) { // 原始查询保留为参考与版本后备，不读取用户TCB授权。
        f->rax = running->tid; // 内核身份始终来自真实正在运行的执行者。
```
<!-- after -->
```c
        f->rax = (uint64_t)create_worker(f->rdi, f->rsi, f->rdx); // 函数入口和用户参数完整复制成寄存器值。
    else if (f->rax == 40) // 用户工作函数返回，只结束本执行者。
        return return_worker(f); // 不关闭进程共享FD或异步owner。
    else if (f->rax == 41) // 同组执行者唯一收取worker，认领和环检测由内核完成。
        return join_worker(f); // 未完成时真正阻塞于原frame。
    else if (f->rax == 42) { // 原始查询保留为参考与版本后备，不读取用户TCB授权。
        f->rax = running->tid; // 内核身份始终来自真实正在运行的执行者。
```

<a id="join-claim-and-cycle"></a>
## 先验证输出，再检查链，最后认领与登记

join_cycle最多走EXECUTION_COUNT即16步，不递归消耗内核栈。遇到running说明会闭环；目标不在IO_JOIN等待中，链在这里结束；沿同组身份找不到下一个也结束。超过有界槽数仍不结束时拒绝，避免在已循环或内部不一致的链上无限遍历。当前所有新边都经过同样检查，因此正常状态不会先藏一条未检出的环。

join_worker先检查自己等待自己，原始41号现在也统一返回-35。其他执行者试图join主TID返回-95；主入口return/exit仍结束整个组，不能伪造一个主线程独立结果。然后验证完整用户输出跨度，错误返回-14，此时尚未触碰任何认领；即使目标已给A，主传坏输出也不能夺走A资格。

找到同组目标后，先检查新边是否闭环，再检查目标是否已被别人认领。都通过才写target->joiner_tid=running->tid。目标已结束则立即交出结果，标记REAP_PENDING；未结束则在running保存join_tid、IO_JOIN和用户输出VA，再撤销运行资格。THREAD_JOIN_WAIT保留原有pid/tid字段，追加joiner，区分目标与收取者。

身份查找、链检查、认领和等待登记在同一次不可抢占内核调用内完成。依据仍是固定单CPU及内核不会在中途运行另一个用户调用，字段无需在本版另用用户原子操作；这并不是未来SMP可以不加同步的证明。结果消费以后仍要等离开目标内核栈再销毁，不能因为“别人已经join”就立即释放脚下的栈。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    destroy_execution(execution); // 栈/FPU的成功部分可独立归还，不关闭进程共享资源。
    return -12; // -ENOMEM，调用者和其他已运行执行者保持。
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
```
<!-- after -->
```c
    destroy_execution(execution); // 栈/FPU的成功部分可独立归还，不关闭进程共享资源。
    return -12; // -ENOMEM，调用者和其他已运行执行者保持。
}
static bool join_cycle(struct user_execution *target) { // 新边running->target不能回到running，所有边都在同进程内。
    for (unsigned count = 0; count < EXECUTION_COUNT; ++count) { // 固定十六执行槽给出遍历上限，不递归借内核栈。
        if (target == running) return true; // 已有等待链回到调用者，加入新边会成环。
        if (target->state != USER_IO_WAIT || target->io != IO_JOIN) return false; // 不在join上等别人，链在这里结束。
        target = find_worker(current, target->join_tid); // 沿本执行者正在等待的目标身份前进。
        if (!target) return false; // 已不可join的末端不能继续形成活等待链。
    }
    return true; // 超过有界槽数仍未结束，拒绝而不无限遍历损坏/循环链。
}
static struct airi_trap_frame *join_worker(struct airi_trap_frame *frame) { // 同组主/worker都能收取worker，主执行者本体暂不是目标。
    if (frame->rdi == running->tid) // 自己不能等到自己结束再继续。
        frame->rax = (uint64_t)-35ll; // -EDEADLK，原始接口也统一报告。
    else if (frame->rdi == current->pid) // 主入口普通返回/exit仍结束整个组，尚无独立主退出协议。
        frame->rax = (uint64_t)-95ll; // -ENOTSUP，不能承诺主线程可join。
    else if (frame->rsi && !user_access(current, frame->rsi, sizeof(long), true)) // 先验证完整输出，再取得任何认领。
        frame->rax = (uint64_t)-14ll; // -EFAULT，不能偷走其他人的目标。
    else { // 身份、链检查、认领和登记仍在单CPU不可抢占的同一次内核调用内。
        struct user_execution *target = find_worker(current, frame->rdi); // 外组、旧身份与待销毁者均不授权。
        if (!target) frame->rax = (uint64_t)-3ll; // -ESRCH，目标不存在或已经交出。
        else if (join_cycle(target)) { // 在写入新等待边以前拒绝闭环。
            frame->rax = (uint64_t)-35ll; // 不认领，不改变现有等待链。
            airi_log("[THREAD_JOIN_CYCLE] pid=%u joiner=%u target=%u", current->pid, running->tid, target->tid); // 真实被拒绝的最后一条边。
        } else if (target->joiner_tid && target->joiner_tid != running->tid) { // 同一结果不能被多人并发收取。
            frame->rax = (uint64_t)-22ll; // -EINVAL，原认领者保留资格。
            airi_log("[THREAD_JOIN_BUSY] pid=%u target=%u joiner=%u owner=%u", current->pid, target->tid, running->tid, target->joiner_tid); // 用真实内核身份观察冲突。
        } else { // 从这里开始，直到结果交出或错误撤销，认领只归当前执行者。
            target->joiner_tid = running->tid; // 先认领再公开等待，其他执行者不能插入另一个合法等待者。
            if (target->state == USER_ZOMBIE) { // 已结束也遵守同一认领与结果提交规则。
                if (frame->rsi && !copy_user(current, &target->result, frame->rsi, sizeof(target->result), true)) airi_panic("validated join output changed"); // 同一不可抢占调用内映射不应变化。
                target->state = USER_REAP_PENDING; // 只登记回收，保持离开目标栈以后才销毁。
                frame->rax = 0; // 结果只交出一次。
            } else { // 未完成时保存当前调用的继续条件。
                running->join_tid = target->tid; // 出边记在等待者自己，入边认领记在target。
                running->io = IO_JOIN; // 同一个41号调用以后原地返回。
                running->io_address = frame->rsi; // NULL表示只收拢，不读结果。
                running->state = USER_IO_WAIT; // 完整登记以后才撤销运行资格。
                ++io_block_count; // 只统计实际阻塞。
                airi_log("[THREAD_JOIN_WAIT] pid=%u tid=%u joiner=%u", current->pid, target->tid, running->tid); // 保留旧prefix，追加真实等待者。
                return schedule(REASON_IO); // 目标和其他执行者继续，所有等待仍可进入服务栈。
            }
        }
    }
    return airi_user_resume(frame); // 立即错误或已结束结果继续当前用户调用。
}
static struct airi_trap_frame *return_worker(struct airi_trap_frame *frame) { // 用户桥接器在工作函数return以后进入。
    if (running == &current->execution) { // 主执行者结束整个进程需使用既有exit接口。
```

<a id="join-resume-and-release"></a>
## 目标返回时，只完成认领者原来的调用

poll_waiters不再假定只有主等待。它从每位等待者自己的join_tid找到目标，核对target.joiner_tid恰是这一位；若不一致是内核状态错误，不能交给另一人或打印成功。目标还未结束便继续等待，不轮询伪造用户完成。

结果完整复制以后，目标变REAP_PENDING，再把原41号调用完成为0。此时可能还在目标刚结束的栈上，真正回收仍由已有collect_pending在安全位置执行，销毁会清零所有认领字段。

若阻塞后的输出复制失败，目标仍保持ZOMBIE，并先把它的joiner_tid撤回0，再让原调用返回-14，后续有效调用还能重试。当前共享worker期间禁止解除相关用户页，正常路径不会制造这种映射变化，但失败状态仍要闭合。没有单独取消joiner的协议；进程退出或用户故障依旧结束整个组，不能推断为已支持pthread取消。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
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
```
<!-- after -->
```c
        if (execution->state != USER_IO_WAIT) // 不碰正在运行、wait孩子或已退出者。
            continue; // 检查下一个等待槽。
        long result; // 恢复到用户同一次调用的结果。
        if (execution->io == IO_JOIN) { // 主或worker均可等待，目标必须仍由这个执行者唯一认领。
            struct user_execution *target = find_worker(p, execution->join_tid); // 等待身份属于当前同进程worker，不借槽指针。
            if (!target) // 内部丢失目标仍明确返回，不解引用已经清除的槽。
                result = -3; // -ESRCH。
            else { // 目标存活时核对认领方向，不能由另一个等待者抢走。
                if (target->joiner_tid != execution->tid) airi_panic("join waiter lost its claim"); // 内核不变量，不能伪造成成功。
                if (target->state != USER_ZOMBIE) continue; // 原调用保持阻塞，目标仍可运行或等其他条件。
                if (execution->io_address && !copy_user(p, &target->result, execution->io_address, sizeof(target->result), true)) { // 结果提交失败仍保留目标结果。
                    target->joiner_tid = 0; // 失败调用不再持认领，后续允许有效收取者重试。
                    result = -14; // -EFAULT，目标仍为ZOMBIE。
                } else { // 结果已经完整交出，任何人都不能再次取得。
                    target->state = USER_REAP_PENDING; // 可能仍站在目标栈上，离开以后再销毁。
                    result = 0; // 唯一收取成功。
                }
            }
        } else if (execution->io == IO_FUTEX) { // 内存字变化不会自动结束等待，必须显式wake或截止。
            if (airi_timer_ticks() < execution->io_deadline) // 未到期只保留登记，不重新读取条件字。
```

本目标编译得到execution为464字节、完整process为904字节，仍满足已有1024字节控制块上限。新字段不申请额外动态内存。等待者的出边与目标的认领虽然都保存TID，生命周期与销毁责任各自清楚，不能只清一边就把目标再交给其他人。

<a id="join-user-boundary"></a>
## 用户包装放开调用者限制，保留主目标边界

更新airi_threads与pthread的join声明。pthread包装不再要求调用者self等于PID，而是拒绝thread等于主PID；self join仍先给EDEADLK。其他同组worker目标直接交给内核，由那里统一检查认领与链。私人long结果槽、成功后写void *、正错误和errno保持继续复用。

旧threads-probe把原始self join的期望从-22改为-35，修改仅从本检查点追加；此前历史仍对应旧返回。pthread-probe原先让worker join主目标，仍应ENOTSUP，只更新说明，不能误把主也变成可收取。

<!-- replace: labs/airios/include/airi_threads.h -->
<!-- before -->
```c
extern "C" { // 不要求宿主线程库。
#endif // C直接声明。
long airi_thread_create(airi_thread_function function, void *argument); // 同进程任一执行者可创建，正TID或原始负错误，不修改errno。
long airi_thread_join(long tid, long *result); // 仅主执行者收取同组一次结果，NULL可丢弃，0成功或负错误。
long airi_thread_id(void); // 有效静态TLS头直接提供当前TID，旧版本退回42号；同进程PID仍共享。
uint64_t airi_thread_syscalls(void); // 当前执行者累计用户请求数，含本次查询，fork/worker重新开始，exec保留。
long airi_futex_wait(uint32_t *word, uint32_t expected, unsigned ticks); // 私有键、四字节对齐，有限PIT预算；0通知、-11值不符、-110到期。
```
<!-- after -->
```c
extern "C" { // 不要求宿主线程库。
#endif // C直接声明。
long airi_thread_create(airi_thread_function function, void *argument); // 同进程任一执行者可创建，正TID或原始负错误，不修改errno。
long airi_thread_join(long tid, long *result); // 同组执行者可唯一收取worker，NULL可丢弃；自等/环-35，重复认领-22，主目标-95。
long airi_thread_id(void); // 有效静态TLS头直接提供当前TID，旧版本退回42号；同进程PID仍共享。
uint64_t airi_thread_syscalls(void); // 当前执行者累计用户请求数，含本次查询，fork/worker重新开始，exec保留。
long airi_futex_wait(uint32_t *word, uint32_t expected, unsigned ticks); // 私有键、四字节对齐，有限PIT预算；0通知、-11值不符、-110到期。
```

<!-- replace: labs/airios/include/pthread.h -->
<!-- before -->
```c
extern "C" { // 不链接宿主pthread。
#endif // C直接声明。
int pthread_create(pthread_t *thread, const pthread_attr_t *attributes, void *(*function)(void *), void *argument); // 同进程执行者可创建，成功0并写身份，失败正错误且不改errno。
int pthread_join(pthread_t thread, void **result); // 当前仅主执行者收取，NULL丢弃结果；成功才写私人输出。
pthread_t pthread_self(void); // 当前有效运行头提供身份，不能跨进程当授权句柄。
int pthread_equal(pthread_t left, pthread_t right); // 相同非零，不同0，不将可移植比较写成数值猜测。
#ifdef __cplusplus // 结束C链接声明。
```
<!-- after -->
```c
extern "C" { // 不链接宿主pthread。
#endif // C直接声明。
int pthread_create(pthread_t *thread, const pthread_attr_t *attributes, void *(*function)(void *), void *argument); // 同进程执行者可创建，成功0并写身份，失败正错误且不改errno。
int pthread_join(pthread_t thread, void **result); // 同组worker结果唯一收取，NULL丢弃；主执行者暂不可作目标，成功才写私人输出。
pthread_t pthread_self(void); // 当前有效运行头提供身份，不能跨进程当授权句柄。
int pthread_equal(pthread_t left, pthread_t right); // 相同非零，不同0，不将可移植比较写成数值猜测。
#ifdef __cplusplus // 结束C链接声明。
```

<!-- replace: labs/airios/libc/pthread.c -->
<!-- before -->
```c
    errno = saved; // 创建过程的内部操作不污染调用者错误状态。
    return 0; // 表示执行者已建立，不保证函数尚未开始或已经结束。
}
int pthread_join(pthread_t thread, void **result) { // 当前不支持worker之间的join图。
    if (!thread || thread > UINT32_MAX) return ESRCH; // 不截断过宽身份命中别人。
    if (pthread_equal(thread, pthread_self())) return EDEADLK; // 自己不能等到自己结束再继续，先于主执行者限制报告。
    if (pthread_self() != (pthread_t)airi_getpid()) return ENOTSUP; // 不伪装完整POSIX的对等线程join能力。
    long value; // 内核只写这个已经存在的私人long槽。
    long error = airi_thread_join((long)thread, result ? &value : 0); // NULL仍执行真实join并回收，不创建虚假结果。
    if (error) return (int)-error; // 失败保持调用者输出，原始接口不改errno。
```
<!-- after -->
```c
    errno = saved; // 创建过程的内部操作不污染调用者错误状态。
    return 0; // 表示执行者已建立，不保证函数尚未开始或已经结束。
}
int pthread_join(pthread_t thread, void **result) { // 同组执行者可join worker，由内核认领并拒绝循环等待。
    if (!thread || thread > UINT32_MAX) return ESRCH; // 不截断过宽身份命中别人。
    if (pthread_equal(thread, pthread_self())) return EDEADLK; // 自己不能等到自己结束再继续，先于主目标限制报告。
    if (thread == (pthread_t)airi_getpid()) return ENOTSUP; // 主执行者尚无独立退出结果，不伪装可join主目标。
    long value; // 内核只写这个已经存在的私人long槽。
    long error = airi_thread_join((long)thread, result ? &value : 0); // NULL仍执行真实join并回收，不创建虚假结果。
    if (error) return (int)-error; // 失败保持调用者输出，原始接口不改errno。
```

<!-- replace: labs/airios/threads-probe.c -->
<!-- before -->
```c
    if (airi_thread_id() != expected_pid || !fresh()) // 主TID初始等于PID，新ELF TLS为模板值。
        return 70; // 不能由旧进程槽猜执行者身份。
    local = 99; errno = EBUSY; // 后续新工作者不能继承这两个运行值。
    if (airi_thread_create(NULL, NULL) != -14 || airi_thread_join(expected_pid, NULL) != -22 || airi_thread_join(-1, NULL) != -3) // 非RX入口、自等待和无目标分别拒绝。
        return 71; // 参数错误不创建资源。
    void *pages[80]; size_t lengths[80], used = 0; // 保存实际成功的原始页申请，用于恢复预算。
    for (size_t step = 64 * 4096; step; step = step == 64 * 4096 ? 4096 : 0) // 先大批再补单页，填满当前用户数据页预算。
```
<!-- after -->
```c
    if (airi_thread_id() != expected_pid || !fresh()) // 主TID初始等于PID，新ELF TLS为模板值。
        return 70; // 不能由旧进程槽猜执行者身份。
    local = 99; errno = EBUSY; // 后续新工作者不能继承这两个运行值。
    if (airi_thread_create(NULL, NULL) != -14 || airi_thread_join(expected_pid, NULL) != -35 || airi_thread_join(-1, NULL) != -3) // 非RX入口、自等待和无目标分别拒绝。
        return 71; // 参数错误不创建资源。
    void *pages[80]; size_t lengths[80], used = 0; // 保存实际成功的原始页申请，用于恢复预算。
    for (size_t step = 64 * 4096; step; step = step == 64 * 4096 ? 4096 : 0) // 先大批再补单页，填满当前用户数据页预算。
```

<!-- replace: labs/airios/pthread-probe.c -->
<!-- before -->
```c
    require(errno == 0 && local_marker == 42 && !pthread_equal(pthread_self(), primary), "pthread fresh errno and identity"); // 新TLS不继承主71。
    token->identity = pthread_self(); // 只写自己那一份输出，主尚未读取。
    errno = 101 + (int)token->index; local_marker = 100 + token->index; // 每位执行者保存不同运行值。
    if (!token->index) { // 第一位创建子执行者，并保留尚未支持的对等join观察。
        void *output = (void *)(uintptr_t)99; // 尚未支持的对等join仍保留失败输出哨兵。
        require(pthread_create(&descendant, 0, leaf, token) == 0, "pthread nested create succeeds"); // 包由这个creator准备，资源归同一process。
        require(pthread_join(primary, &output) == ENOTSUP && output == (void *)(uintptr_t)99, "pthread peer join unsupported"); // 保留当前主管理限制。
        require(pthread_join(pthread_self(), 0) == EDEADLK, "pthread worker self join"); // 自己等待自己明确报告。
    }
    airi_yield(); // 在真正共享执行者之间交替，errno仍属于自己。
```
<!-- after -->
```c
    require(errno == 0 && local_marker == 42 && !pthread_equal(pthread_self(), primary), "pthread fresh errno and identity"); // 新TLS不继承主71。
    token->identity = pthread_self(); // 只写自己那一份输出，主尚未读取。
    errno = 101 + (int)token->index; local_marker = 100 + token->index; // 每位执行者保存不同运行值。
    if (!token->index) { // 第一位创建子执行者，并观察暂不可join的主执行者目标。
        void *output = (void *)(uintptr_t)99; // 尚未支持的对等join仍保留失败输出哨兵。
        require(pthread_create(&descendant, 0, leaf, token) == 0, "pthread nested create succeeds"); // 包由这个creator准备，资源归同一process。
        require(pthread_join(primary, &output) == ENOTSUP && output == (void *)(uintptr_t)99, "pthread primary target unsupported"); // 只是主目标暂不支持，普通worker之间已可收取。
        require(pthread_join(pthread_self(), 0) == EDEADLK, "pthread worker self join"); // 自己等待自己明确报告。
    }
    airi_yield(); // 在真正共享执行者之间交替，errno仍属于自己。
```

<a id="join-observer-setup"></a>
## 先完成一条正常的worker到worker收取

新建join-probe.c。tokens由主持有，参数与返回值都不借结束后的工作栈。两个版本门分别放行A和B，attempted表示A已准备发join，cycle_done表示B确实收到反向边错误。握手有期限，不把标志本身当内核已登记证据。

raw_join只用于坏输出观察：pthread包装会先让内核写自己的私人long槽，不能用故意无效的void **直接调用包装来验证内核。这里以41号和完整高地址进入原始边界，检查内核先验证输出、不会直接解引用这个地址；返回仍是原始负错误。

<!-- write: labs/airios/join-probe.c -->
```c
#include <pthread.h> // 使用同进程worker之间的源码join入口。
#include <airi_event.h> // 分别控制A开始join与B尝试反向边。
#include <airi_malloc.h> // 最后检查启动包已经全部归还。
#include <airi_libc.h> // 有限握手、串口与进程回收。
#include <errno.h> // 正错误和各执行者错误哨兵。
#include <stdatomic.h> // 观察进度在join以外独立发布。
static unsigned tokens[4]; // 参数/结果在main持有的稳定对象中，不借已结束工作栈。
static pthread_t peers[2]; // 主创建A/B后才发布start_a，回调不会读未完成身份。
static airi_event start_a = AIRI_EVENT_INITIALIZER, start_b = AIRI_EVENT_INITIALIZER; // 两条边分别放行。
static _Atomic unsigned attempted, cycle_done; // A准备join，B已实际收到闭环错误。
static void require(int condition, const char *message) { // 错误结束整个真实进程组。
    if (!condition) { airi_puts(message); airi_puts("\n"); airi_exit(89); } // 不继续输出成功。
}
static void gate(airi_event *event) { // 仅观察握手，不替内核join轮询结果。
    uint64_t start = airi_ticks(); // 所有重试共用一次总预算。
    while (airi_event_sequence(event) == 0) { // 允许发布早于真正WAIT。
        uint64_t elapsed = airi_ticks() - start; // PIT时间继续经过。
        require(elapsed < 200 && airi_event_wait(event, 0, (unsigned)(200 - elapsed)) == 0, "join gate timeout"); // 有限等待。
    }
}
static void await(_Atomic unsigned *word) { // 主只等观察位置，不拿目标结果。
    uint64_t start = airi_ticks(); // 独立100tick握手期限。
    while (!atomic_load_explicit(word, memory_order_acquire)) { // 对方release发布以后读取依赖状态。
        require(airi_ticks() - start < 100, "join observation timeout"); airi_yield(); // 真正让其他用户执行者运行。
    }
}
static long raw_join(pthread_t target, uintptr_t output) { // 只为观察内核输出验证，绕开用户包装的私人long槽。
    long number = 41; // 仍是自有41号，不是Linux线程ABI。
    __asm__ volatile("int $0x80" : "+a"(number) : "D"(target), "S"(output), "d"(0ull) : "memory", "cc"); // 完整64位地址进入原始边界。
    return number; // 负错误不经过errno翻译。
}
```

simple_parent在worker里创建simple_child，随后自己join child，取得tokens[0]。child先实际sleep两tick，再普通返回，parent的errno91保持。主只join parent取得tokens[1]，不再替它收取child。两级调用都走正确类型适配器，内核结果分别只给各自认领者。

<!-- append: labs/airios/join-probe.c -->
```c
static void *simple_child(void *argument) { // 被另一个worker创建并收取。
    require(errno == 0 && airi_sleep(2) == 0, "peer child initial state"); // 新TLS和实际结束前等待。
    return argument; // 正确类型指针返回。
}
static void *simple_parent(void *argument) { // 不经主中转收取自己的子执行者。
    pthread_t child; void *result; errno = 91; // 当前执行者独立错误槽。
    require(pthread_create(&child, 0, simple_child, &tokens[0]) == 0 && pthread_join(child, &result) == 0 && result == &tokens[0] && errno == 91, "peer creates and joins"); // 内核必须让这个worker真正等待/收取。
    return argument; // 主只收取parent，不再重复收取child。
}
```

<a id="join-cycle-observation"></a>
## A已经等待B以后，再让B尝试反向边

left是A：先等start_a，主创建两个身份后才放行，因此它不会读未准备好的peers[1]。A发布attempted再join B，结果应是B返回的tokens[2]；随后A自己返回tokens[3]供主收取。

right是B：先等start_b，只有主确认A的认领已存在才放行。它join A会从A的出边走回B，收到EDEADLK，私人结果99和errno93保持。B发布cycle_done以后正常返回，没有把自己登记进反向等待；因此能让A完成。主等这个反向尝试结束以后才认领A，避免用自己的认领干扰闭环观察。

<!-- append: labs/airios/join-probe.c -->
```c
static void *left(void *argument) { // A先形成A->B等待边。
    gate(&start_a); errno = 92; // 两个身份已由主完整发布。
    atomic_store_explicit(&attempted, 1, memory_order_release); // 主随后给出实际登记机会。
    void *result; // B的指针结果只交给这一位收取者。
    require(pthread_join(peers[1], &result) == 0 && result == &tokens[2] && errno == 92, "peer claimed result"); // B闭环请求失败以后会正常返回。
    return argument; // A也有自己的结果供主join。
}
static void *right(void *argument) { // B在主确认A认领以后才尝试B->A。
    gate(&start_b); errno = 93; // 不与A的首次登记竞赛猜先后。
    void *result = (void *)(uintptr_t)99; // 被拒绝的边不得改结果输出。
    require(pthread_join(peers[0], &result) == EDEADLK && result == (void *)(uintptr_t)99 && errno == 93, "peer join cycle rejected"); // 内核沿A->B回到自己，不能把两人都睡下。
    atomic_store_explicit(&cycle_done, 1, memory_order_release); // 主在反向尝试以后才认领A，避免干扰这个闭环观察。
    return argument; // B没有形成新边，可以正常结束唤醒A。
}
```

main先跑正常两级收取，再创建A/B。放行A并给三tick登记机会，B仍在门上不会结束。主先对B传高地址，要求-14；再用有效包装竞争B，要求EINVAL且输出不变，证明A的认领既没被坏地址撤走，也没被主抢走。

| 实际步骤 | 已登记等待边 | 目标认领与结果 |
| --- | --- | --- |
| A join B | A到B | B的结果归A |
| 主也join B | 仍只有A到B | 主返回22，不抢认领 |
| B尝试join A | 仍只有A到B | B返回35，没有反向边 |
| B普通返回 | A原41号完成 | B结果只交给A |
| A普通返回 | 主可收取A | 主得到A结果，不能再收B |

最后join A后，再收B得到本实现的ESRCH诊断扩展，输出保持。活启动包为0，接着fork/wait仍能继续，确认没有留下worker生命周期使has_workers永久报忙。这个fork是普通进程，不计入四位worker数量。

<!-- append: labs/airios/join-probe.c -->
```c
int main(void) { // 主仍可join worker，但不再是唯一可收取者。
    errno = 71; pthread_t parent; void *result; // 主错误状态不随其他执行者改变。
    require(pthread_create(&parent, 0, simple_parent, &tokens[1]) == 0 && pthread_join(parent, &result) == 0 && result == &tokens[1], "peer nested completion"); // child由parent收取，parent由主收取。
    require(pthread_create(&peers[0], 0, left, &tokens[3]) == 0 && pthread_create(&peers[1], 0, right, &tokens[2]) == 0, "peer cycle pair create"); // 两人先等各自门。
    require(airi_event_publish(&start_a) >= 0, "peer start A"); await(&attempted); // A即将调用join B。
    require(airi_sleep(3) == 0, "peer A registers claim"); // B门仍关闭，A应真正停在41号中。
    require(raw_join(peers[1], 0xffff800000000000ull) == -14, "peer bad output cannot steal claim"); // 坏内核地址在认领以前拒绝，A资格仍保留。
    result = (void *)(uintptr_t)99; // 主尝试成为B第二位收取者。
    require(pthread_join(peers[1], &result) == EINVAL && result == (void *)(uintptr_t)99 && errno == 71, "peer duplicate claim rejected"); // 返回22，不能把B从A手中交给主。
    airi_puts("[JOIN_CLAIM_OK] competing=22 bad_output=14 unchanged=1\n"); // 两种拒绝都真实发生以后才输出。
    require(airi_event_publish(&start_b) >= 0, "peer start B"); await(&cycle_done); // B实际尝试反向边并收到35。
    require(pthread_join(peers[0], &result) == 0 && result == &tokens[3], "peer A final result"); // B先结束，A收取B，再由主收取A。
    result = (void *)(uintptr_t)99; require(pthread_join(peers[1], &result) == ESRCH && result == (void *)(uintptr_t)99, "peer result consumed exactly once"); // 当前不复用TID的诊断扩展，不能再拿一次结果。
    struct airi_malloc_info info = airi_malloc_info(); // 四位回调前均归还启动包。
    require(info.live_objects == 0 && info.live_bytes == 0 && errno == 71, "peer resources and errno"); // 没有隐藏包或错误槽污染。
    long child = airi_fork(); require(child >= 0, "peer no leftover worker lifecycle"); // 所有目标已收拢，has_workers不应留下忙资格。
    if (!child) airi_exit(0); // 普通进程复制，不把它算成第五位worker。
    int status; require(airi_waitpid(child, &status) == child && status == 0, "peer process continues"); // join图结束以后进程继续服务。
    airi_puts("[JOIN_USER_OK] peers=4 nested=joined claim=unique cycle=35 bad_output=14 result=once resources=clean\n"); // 正常、竞争、闭环和回收实际完成。
    return 0; // shell取得真实成功。
}
```

<a id="join-build-evidence"></a>
## 主机按真实两条拒绝关系核对等待图

join.elf链接同一pthread、malloc、普通mutex、event和真实线程运行库，打包完整静态ELF并注册join名称。没有引入宿主线程或另一个调度器。

<!-- append: Makefile -->
```makefile

# peer join观察链接同一pthread/分配器，不提供宿主替身。
$(OUT)/user/join.elf: $(USER_COMMON) $(OUT)/user/posix.o $(OUT)/user/heap-algorithm.o $(OUT)/user/malloc.o $(OUT)/user/mutex-lib.o $(OUT)/user/threads-lib.o $(OUT)/user/thread-entry.o $(OUT)/user/event-lib.o $(OUT)/user/pthread-lib.o $(OUT)/user/join-probe.o labs/airios/program.ld
# 用户适配和内核41号共同构成实际收取路径。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(filter %.o,$^) -o $@
# 初始目录借完成的静态映像。
$(OUT)/labs/airios/images.o: $(OUT)/user/join.elf
# 只用于只读incbin范围。
$(OUT)/labs/airios/images.o: CPPFLAGS += -DAIRI_JOIN_ELF='"$(OUT)/user/join.elf"'
```

<!-- append: labs/airios/images.S -->
```asm
.balign 16 /* 对等收取与闭环拒绝的实际用户映像。 */
.global airi_join_elf_start, airi_join_elf_end /* 目录借用完整ELF边界。 */
airi_join_elf_start: /* 包含文件头和所有用户段。 */
.incbin AIRI_JOIN_ELF /* 不借宿主线程演示。 */
airi_join_elf_end: /* 右端不包含字节。 */
```

宿主运行join并核对四次真实创建和返回。busy记录(target,joiner,owner)应为(B,主,A)，cycle记录(joiner,target)应为(B,A)，再找实际THREAD_JOIN_WAIT中的目标B与joiner A。这样“主争抢B”和“B试图等A”是同一组真实对象，不能拿别的错误日志凑出一张图。

<!-- replace: scripts/shell_test.py -->
<!-- before -->
```python
                child_return=nested.index('[THREAD_RETURN] pid='+pthread_owner.group(1)+' tid='+children[0][0]+' ') # gate在replacement join以后才发布。
                assert replacement_return < child_return and re.search(r'\[FUTEX_WAIT\] pid='+pthread_owner.group(1)+r' tid='+children[0][0]+r' ',nested) # 不是leaf早已结束后的虚假寿命演示。

                offset=len(text); wire.sendall(b'heap-threads\n'); text=wait('[SHELL_COMMAND_DONE] name=heap-threads code=0',offset) # 四个真实执行者共享唯一arena，观察对象交接。
                heap_threads=text[offset:] # 不用旧heap命令代替并发结果。
                assert '[MALLOC_THREADS_OK] workers=4 rounds=96 arena=262144 cross_free=4 snapshots=consistent errno=private critical_yield=1' in heap_threads # 每个工作者和主快照均由guest核对。
```
<!-- after -->
```python
                child_return=nested.index('[THREAD_RETURN] pid='+pthread_owner.group(1)+' tid='+children[0][0]+' ') # gate在replacement join以后才发布。
                assert replacement_return < child_return and re.search(r'\[FUTEX_WAIT\] pid='+pthread_owner.group(1)+r' tid='+children[0][0]+r' ',nested) # 不是leaf早已结束后的虚假寿命演示。

                offset=len(text); wire.sendall(b'join\n'); text=wait('[SHELL_COMMAND_DONE] name=join code=0',offset) # 对等worker真实收取，并在登记以前拒绝争抢与环。
                join_output=text[offset:] # 不借其他命令的认领或结果。
                assert '[JOIN_CLAIM_OK] competing=22 bad_output=14 unchanged=1' in join_output and '[JOIN_USER_OK] peers=4 nested=joined claim=unique cycle=35 bad_output=14 result=once resources=clean' in join_output # guest已检查原指针、输出保持及之后fork继续。
                join_owner=re.search(r'\[EXEC_COMMIT\] pid=(\d+) program=join ',join_output) # 真实同进程身份。
                assert join_owner # 必须确实执行新静态映像。
                assert len(re.findall(r'\[THREAD_CREATE\] pid='+join_owner.group(1)+r' tid=',join_output))==4 and len(re.findall(r'\[THREAD_RETURN\] pid='+join_owner.group(1)+r' tid=',join_output))==4 # 两层普通join以及A/B均收拢。
                busy=re.findall(r'\[THREAD_JOIN_BUSY\] pid='+join_owner.group(1)+r' target=(\d+) joiner=(\d+) owner=(\d+)',join_output) # 主试图争抢B，原认领者A保持。
                cycles=re.findall(r'\[THREAD_JOIN_CYCLE\] pid='+join_owner.group(1)+r' joiner=(\d+) target=(\d+)',join_output) # B->A会回到B。
                assert len(busy)==1 and len(cycles)==1 and busy[0][1]==join_owner.group(1) and cycles[0]==(busy[0][0],busy[0][2]) # 使用真实内核关系，不硬编码本轮数字。
                assert '[THREAD_JOIN_WAIT] pid='+join_owner.group(1)+' tid='+busy[0][0]+' joiner='+busy[0][2] in join_output # A已经真的等待B，环不是用户自报的虚拟图。
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
                result.update(pthread_source_subset=True,pthread_workers=36,pthread_packets_reclaimed=True,bounded_queue=True,queue_capacity=4,queue_jobs=48,queue_close_drains=True,sequence_event=True,event_waits=6,event_broadcast_workers=2,event_false_wake_rechecked=True,mutex_race_stages=4,mutex_race_actual_waits=1,mutex_race_mismatches=1,per_execution_syscall_count=True,uncontended_mutex_pairs=8,uncontended_mutex_syscalls=0,runtime_tcb_identity=True,identity_reference_queries=7,shared_malloc=True,malloc_workers=4,malloc_rounds=96,owned_mutex=True,mutex_critical_updates=64,private_futex=True,futex_registrations=8,user_heap=True,shared_executions=True,thread_commands=2,worker_capacity=8,worker_reuse_rounds=24) # 关键字把实际已验收字段加入结果字典，不替代上面的逐命令检查。
                result_path.write_text(json.dumps(result,indent=2)+'\n'); print('PASS user shell: alloc/file/async wait/other task/CQ/readback, failed exec, return to monitor') # 阶段演示实际完成。
                return result # 总入口可收集。
            finally: # 包括断言、超时和QEMU错误。
```
<!-- after -->
```python
                offset=len(text); wire.sendall(b'echo after-shell\n'); text=wait('[CONSOLE_UPDATE] view=0 commands=2 page=0',offset) # 回到内核后输入仍完整。
                assert 'after-shell' in text[offset:] # 不只是退出而丢了后续UART。
                result={'passed':True,'ring3_shell':True,'ram_file_and_device_readback':True,'other_user_during_cq_wait':True,'qmp_iops':1,'error_exec_survived':True,'returned_to_monitor':True,'orphan_rounds':24,'memory_reuse_rounds':128,'capacity_rounds':4,'capacity_pages':1024,'simd_preempt_fork_exec':True,'fs_base_preempt_fork_exec':True,'compiler_static_tls':True,'posix_errno_io_subset':True,'positioned_vectors':True} # 只发布已观察事实。
                result.update(peer_worker_join=True,join_claim_unique=True,join_cycle_rejected=True,pthread_source_subset=True,pthread_workers=36,pthread_packets_reclaimed=True,bounded_queue=True,queue_capacity=4,queue_jobs=48,queue_close_drains=True,sequence_event=True,event_waits=6,event_broadcast_workers=2,event_false_wake_rechecked=True,mutex_race_stages=4,mutex_race_actual_waits=1,mutex_race_mismatches=1,per_execution_syscall_count=True,uncontended_mutex_pairs=8,uncontended_mutex_syscalls=0,runtime_tcb_identity=True,identity_reference_queries=7,shared_malloc=True,malloc_workers=4,malloc_rounds=96,owned_mutex=True,mutex_critical_updates=64,private_futex=True,futex_registrations=8,user_heap=True,shared_executions=True,thread_commands=2,worker_capacity=8,worker_reuse_rounds=24) # 关键字把实际已验收字段加入结果字典，不替代上面的逐命令检查。
                result_path.write_text(json.dumps(result,indent=2)+'\n'); print('PASS user shell: alloc/file/async wait/other task/CQ/readback, failed exec, return to monitor') # 阶段演示实际完成。
                return result # 总入口可收集。
            finally: # 包括断言、超时和QEMU错误。
```

完整实现闭合，运行：

<!-- run -->
```sh
./airios build  # 编译唯一认领和有界等待链，保持离栈后回收
```

<!-- run -->
```sh
./airios test  # 对等收取、争抢、闭环、坏输出和后续进程继续
```

<a id="join-results"></a>
## 拒绝最后一条边，已经登记的等待仍能完成

交互执行`./airios run`、`run sh`、`join`。本轮主PID132、A为135、B为136，实际记录：

```text
[THREAD_JOIN_BUSY] pid=132 target=136 joiner=132 owner=135
[JOIN_CLAIM_OK] competing=22 bad_output=14 unchanged=1
[THREAD_JOIN_CYCLE] pid=132 joiner=136 target=135
[JOIN_USER_OK] peers=4 nested=joined claim=unique cycle=35 bad_output=14 result=once resources=clean
```

主没有抢走A的目标，B也没有写入反向等待边。B正常结束以后，A原来的41号得到结果，A再结束，主才得到A的结果；不是把死锁中的两人强制清掉来打印成功。坏输出被内核拒绝，程序仍可收取并继续fork，结果和资源边界都实际经过。

当前仅worker结果可join，主执行者独立退出、detach、取消清理与完整pthread属性仍未实现。认领与链检查依赖单CPU不可抢占内核，未来SMP需要同步同一组状态。多方join的明确错误是本项目受控行为，不宣称所有平台对无效或重复join都具有相同扩展。后续继续接入互斥与条件接口，保留这些真实生命周期检查。
