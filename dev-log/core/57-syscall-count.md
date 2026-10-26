# K57 · 数清同一执行者真正发出的系统调用

K56 让普通 TID 查询留在用户态，但局部锁的 unlock 仍调用44号通知。现在想知道八次加锁、解锁到底进入内核几次，单看调用了哪些 C 函数并不够：一个函数可以只读用户内存，也可以在循环里发出多个请求。先在真正的系统调用入口计数，再运行同一段锁操作，得到下一步优化前的基线。

从 K56 的同一目录继续。本节保持互斥算法不变，增加每执行者计数和45号观察接口；identity 仍走原始 fork、exec、工作槽复用。最终既能数清当前八次解锁通知，也能区分“换了映像”和“创建了执行者”。作者工具 T04 和图形支线均不是前置。

<a id="syscalls-owner"></a>
## 时钟会插入执行，计数仍归当前执行者

identity 的局部锁放在 inspect 的调用栈上，其他执行者不访问它。它不会竞争，但运行期间仍可能收到 PIT 中断并切到另一位工作者。如果所有请求共用一个全局计数，前后两次读取之间就会混入别人的输出、时钟查询和退出，无法据此计算这把锁的成本。

K52 已把实际执行者放进 running，把共享内存和 FD 所属进程放进 current。请求由哪条执行链发出，就累加到那份 user_execution；同 PID 下的三个工作者分别保存自己的数值。时钟 IRQ 有独立入口，用户故障也应先退出当前组，都不能算成用户主动请求。128号门进来的未知编号或失败请求仍计一次，因为它们确实跨过了同一道边界。

先给每个执行者保存数值的位置。在 user_execution 的 user_ticks 后加入 uint64_t syscall_count，计数因而跟随这份执行状态，不跟随共享PID或用户页。64位上限为 UINT64_MAX，到顶便停止增加；正常观察离它很远，饱和后的差值不能再当精确测量。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    uint64_t fpu_physical; // 独立内核状态页，保存本执行者的x87/SSE寄存器。
    struct airi_kernel_region stack; // 真正进入内核以后使用的栈，离开它才能销毁。
    unsigned user_ticks; // 本执行者实际收到的用户时钟事件。
    uint64_t fault_error; // 最近一次结束本执行者的用户故障错误位。
    enum user_state state; // 主/工作执行者分别保存调度资格，不由共享PID推导。
    int64_t wait_target; // 等待的孩子PID条件，不是地址空间所有权。
```
<!-- after -->
```c
    uint64_t fpu_physical; // 独立内核状态页，保存本执行者的x87/SSE寄存器。
    struct airi_kernel_region stack; // 真正进入内核以后使用的栈，离开它才能销毁。
    unsigned user_ticks; // 本执行者实际收到的用户时钟事件。
    uint64_t syscall_count; // 本执行者实际进入用户128号门的请求数，含查询自身，到上限饱和。
    uint64_t fault_error; // 最近一次结束本执行者的用户故障错误位。
    enum user_state state; // 主/工作执行者分别保存调度资格，不由共享PID推导。
    int64_t wait_target; // 等待的孩子PID条件，不是地址空间所有权。
```

字段已加入，接下来在真正的请求路径累加。位置在拒绝内核错误、保存现场并处理用户故障之后，在按 RAX 编号分派服务之前；已有启动129号门和故障返回仍先离开，时钟IRQ也不走这条用户请求计数。只有真正经过128号门的请求到这里才加一，未知编号或随后失败仍算一次。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
        airi_log("[USER_FAULT] pid=%u vector=%llu error=0x%llx rip=0x%llx cr2=0x%llx", current->pid, (unsigned long long)f->vector, (unsigned long long)f->error, (unsigned long long)f->rip, (unsigned long long)(f->vector == 14 ? airi_trap_fault_address() : 0)); // PF才读取本次CR2。
        return exit_current(-(128 + (int)f->vector)); // 自有故障编码，不宣称Linux信号ABI。
    }
    if (f->rax == 1 || f->rax == 10 || f->rax == 15 || f->rax == 16) { // 同步文件与流的完整副本边界。
        bool write = f->rax == 1 || f->rax == 16; // 输出到流或文件。
        bool positioned = f->rax == 15 || f->rax == 16; // 显式偏移不支持阻塞流。
```
<!-- after -->
```c
        airi_log("[USER_FAULT] pid=%u vector=%llu error=0x%llx rip=0x%llx cr2=0x%llx", current->pid, (unsigned long long)f->vector, (unsigned long long)f->error, (unsigned long long)f->rip, (unsigned long long)(f->vector == 14 ? airi_trap_fault_address() : 0)); // PF才读取本次CR2。
        return exit_current(-(128 + (int)f->vector)); // 自有故障编码，不宣称Linux信号ABI。
    }
    if (running->syscall_count != UINT64_MAX) ++running->syscall_count; // 只有真正用户系统调用到这里，IRQ、故障和内核启动门不计入。
    if (f->rax == 1 || f->rax == 10 || f->rax == 15 || f->rax == 16) { // 同步文件与流的完整副本边界。
        bool write = f->rax == 1 || f->rax == 16; // 输出到流或文件。
        bool positioned = f->rax == 15 || f->rax == 16; // 显式偏移不支持阻塞流。
```

45号返回的是入口已经加一之后的数值。例如第一次发请求就是查询45，结果为1。除入口已经加一外，45号不读取用户指针，也不切换执行者的调度状态；日志里的 pid、tid 来自真实 current/running。这个接口不是 Linux ABI，也不是耗时测量，不能从次数直接推算纳秒或吞吐。接着在44号之后、未知编号返回之前加入查询分支，让它读取刚才那份 running 的计数。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    }
    else if (f->rax == 44) // 显式通知至多给定数量，不保存信号量计数。
        f->rax = (uint64_t)futex_wake(f->rdi, f->rsi); // 不修改用户字，返回真正唤醒的数量。
    else // 尚未提供的接口都明确拒绝。
        f->rax = (uint64_t)-38ll; // -ENOSYS。
    poll_waiters(); // 文件/流修改完成后立即重查其他等待者，不等用户轮询。
```
<!-- after -->
```c
    }
    else if (f->rax == 44) // 显式通知至多给定数量，不保存信号量计数。
        f->rax = (uint64_t)futex_wake(f->rdi, f->rsi); // 不修改用户字，返回真正唤醒的数量。
    else if (f->rax == 45) { // 每执行者观察计数，不把其他工作者的请求混入同一次测量。
        f->rax = running->syscall_count; // 包括本次查询入口，测量差值需扣掉结束查询的一次。
        airi_log("[THREAD_SYSCALLS] pid=%u tid=%u count=%llu", current->pid, running->tid, (unsigned long long)running->syscall_count); // 真实内核计数，不是用户推测。
    }
    else // 尚未提供的接口都明确拒绝。
        f->rax = (uint64_t)-38ll; // -ENOSYS。
    poll_waiters(); // 文件/流修改完成后立即重查其他等待者，不等用户轮询。
```

新建执行者的控制槽原本就从零开始，回收也会清零。fork 只复制用户页和指定 CPU 现场，不复制整份父 execution，因此孩子即使从父的调用点继续，也没有发出父的6号请求，计数仍为零。worker 槽复用同样从零开始。exec 保留当前 execution，只替换映像、FS 和相关内存；原来的请求数随这个仍在运行的执行者保留，不采用候选映像的零计数。

<a id="syscalls-sampling"></a>
## 先读开始值，八次解锁，再读结束值

在 airi_threads.h 声明查询，在 threads.c 末尾追加包装。沿用 thread_call 的整数寄存器传递，编号45，不带参数；返回值转回 uint64_t，保留 RAX 中全部64位。即使最高位为1，也不能把这个接口当成“负错误码”解释，因为它的合同是无符号饱和计数。

<!-- replace: labs/airios/include/airi_threads.h -->
<!-- before -->
```c
long airi_thread_create(airi_thread_function function, void *argument); // 仅主执行者创建，正TID或原始负错误，不修改errno。
long airi_thread_join(long tid, long *result); // 仅主执行者收取同组一次结果，NULL可丢弃，0成功或负错误。
long airi_thread_id(void); // 有效静态TLS头直接提供当前TID，旧版本退回42号；同进程PID仍共享。
long airi_futex_wait(uint32_t *word, uint32_t expected, unsigned ticks); // 私有键、四字节对齐，有限PIT预算；0通知、-11值不符、-110到期。
long airi_futex_wake(uint32_t *word, unsigned count); // 至多唤醒十六个同进程已登记者，不修改字值，不累计通知。
#ifdef __cplusplus // 结束链接语言范围。
```
<!-- after -->
```c
long airi_thread_create(airi_thread_function function, void *argument); // 仅主执行者创建，正TID或原始负错误，不修改errno。
long airi_thread_join(long tid, long *result); // 仅主执行者收取同组一次结果，NULL可丢弃，0成功或负错误。
long airi_thread_id(void); // 有效静态TLS头直接提供当前TID，旧版本退回42号；同进程PID仍共享。
uint64_t airi_thread_syscalls(void); // 当前执行者累计用户请求数，含本次查询，fork/worker重新开始，exec保留。
long airi_futex_wait(uint32_t *word, uint32_t expected, unsigned ticks); // 私有键、四字节对齐，有限PIT预算；0通知、-11值不符、-110到期。
long airi_futex_wake(uint32_t *word, unsigned count); // 至多唤醒十六个同进程已登记者，不修改字值，不累计通知。
#ifdef __cplusplus // 结束链接语言范围。
```

<!-- append: labs/airios/libc/threads.c -->
```c
uint64_t airi_thread_syscalls(void) { // 此观察本身是一次真实45号系统调用。
    return (uint64_t)thread_call(45, 0, 0, 0); // 保留完整64位无符号计数，上限饱和不回绕。
}
```

现在把两次查询放进 inspect，紧贴原来的八次局部锁操作。起点以前的一次原始42号参考仍保留，但它发生在开始快照之前，不进入本次差值。循环中每次 lock 的 TID 来自版本化用户头，CAS 一次成功；unlock 释放 owner 后仍发44号。因此循环恰有八个请求。

结束查询自身也是第九个请求：假设开始返回4，八次通知以后累计12，结束查询入口加一才返回13，所以 after-before=9。开始查询已包含在 before 中，会被相减抵消；只需从差值减去结束查询的一次。本节先断言9，再打印对应标记，不能预写成功字符串来代替计数。

还在每位新 worker 和 fork 孩子的首个请求处查询，要求返回1。普通 main 可能由 shell 通过 exec 进入，不能假设从未发过请求；重置 main 则应继续看到 exec 前已有的累计值。依次修改 identity.c 的三个旧窗口：第一个接 inspect 的测量与worker首查询，第二个接 main 的普通/exec入口，第三个接原始fork孩子的首查询。其余控制头校准、PIT循环和收取步骤保持：

<!-- replace: labs/airios/identity.c -->
<!-- before -->
```c
    require(value->self == (uintptr_t)value && value->version == AIRI_RUNTIME_VERSION && value->tid == (uint32_t)actual, "published TCB identity"); // 未更新的fork/exec候选0会在这里失败。
    for (unsigned i = 0; i < 128; ++i) require(airi_thread_id() == actual, "fast identity stable"); // 应只有TCB读，不重复42号查询。
    airi_mutex own = AIRI_MUTEX_INITIALIZER; // 当前调用栈独有，不制造另一项竞争。
    for (unsigned i = 0; i < 8; ++i) require(airi_mutex_lock(&own, 0) == 0 && airi_mutex_unlock(&own) == 0, "mutex uses local identity"); // unlock仍调用44，不能把整个锁宣称为零陷入。
    return actual; // 后续用独立原始参考比较，不自证同一缓存。
}
static long worker(void *argument) { // 原始声明值与本次内核TID都须属于新执行者。
    unsigned index = (unsigned)(uintptr_t)argument; // 由主执行者提供0、1或2。
    require(local == 42, "worker fresh TLS"); // 不继承主执行者改成99的运行值。
    long identity = inspect(); // 该worker恰有一次真实参考查询。
    require(identity != airi_getpid(), "worker TID differs from shared PID"); // 同进程身份与执行者身份分别判断。
    local = 200 + index; // 每个实际TLS副本保留不同观察值。
```
<!-- after -->
```c
    require(value->self == (uintptr_t)value && value->version == AIRI_RUNTIME_VERSION && value->tid == (uint32_t)actual, "published TCB identity"); // 未更新的fork/exec候选0会在这里失败。
    for (unsigned i = 0; i < 128; ++i) require(airi_thread_id() == actual, "fast identity stable"); // 应只有TCB读，不重复42号查询。
    airi_mutex own = AIRI_MUTEX_INITIALIZER; // 当前调用栈独有，不制造另一项竞争。
    uint64_t before = airi_thread_syscalls(); // 记录当前执行者计数，不用全局计数混入其他工作者。
    for (unsigned i = 0; i < 8; ++i) require(airi_mutex_lock(&own, 0) == 0 && airi_mutex_unlock(&own) == 0, "mutex uses local identity"); // unlock仍调用44，不能把整个锁宣称为零陷入。
    uint64_t after = airi_thread_syscalls(); // 结束查询自身也增加一次，不能当作锁的开销。
    require(after - before == 9, "uncontended mutex syscall baseline"); // 八次unlock44加一次结束查询，身份读取已不陷入。
    airi_puts("[MUTEX_PROFILE] loops=8 delta=9\n"); // 只有实际内核计数差符合才打印，不从函数名猜次数。
    return actual; // 后续用独立原始参考比较，不自证同一缓存。
}
static long worker(void *argument) { // 原始声明值与本次内核TID都须属于新执行者。
    unsigned index = (unsigned)(uintptr_t)argument; // 由主执行者提供0、1或2。
    require(local == 42, "worker fresh TLS"); // 不继承主执行者改成99的运行值。
    require(airi_thread_syscalls() == 1, "new worker syscall counter"); // 工作入口没有继承创建者的累计值。
    long identity = inspect(); // 该worker恰有一次真实参考查询。
    require(identity != airi_getpid(), "worker TID differs from shared PID"); // 同进程身份与执行者身份分别判断。
    local = 200 + index; // 每个实际TLS副本保留不同观察值。
```

<!-- replace: labs/airios/identity.c -->
<!-- before -->
```c
int main(int argc, char **argv) { // exec前后使用同一ELF的不同目录名。
    require(argc == 1, "identity argv"); // 最小启动协议仍是单项程序名。
    if (named(argv[0], "identity-reset")) { // 成功exec重新建立TLS与运行头，PID仍为原孩子。
        require(local == 42 && inspect() == airi_getpid(), "exec fresh TLS and retained PID"); // 不能把候选的0或旧运行值123带入新main。
        airi_puts("[IDENTITY_EXEC_OK] tls=42 tcb=current version=1\n"); // 原始参考查询与字段均已经检查。
        return 0; // 父wait取得成功结果。
    }
    long parent = inspect(); // 主执行者版本头在首次进入用户前即有真实TID。
    require(parent == airi_getpid(), "primary TID"); // 主执行者初始TID等于PID。
    struct airi_runtime_tcb *value = control(); // 本阶段只在无worker时修改自己的版本，观察后立即恢复。
```
<!-- after -->
```c
int main(int argc, char **argv) { // exec前后使用同一ELF的不同目录名。
    require(argc == 1, "identity argv"); // 最小启动协议仍是单项程序名。
    if (named(argv[0], "identity-reset")) { // 成功exec重新建立TLS与运行头，PID仍为原孩子。
        require(airi_thread_syscalls() > 1, "exec keeps execution counter"); // 新main不等于新执行者，exec自身和此前请求继续累计。
        require(local == 42 && inspect() == airi_getpid(), "exec fresh TLS and retained PID"); // 不能把候选的0或旧运行值123带入新main。
        airi_puts("[IDENTITY_EXEC_OK] tls=42 tcb=current version=1\n"); // 原始参考查询与字段均已经检查。
        return 0; // 父wait取得成功结果。
    }
    require(airi_thread_syscalls() >= 1, "primary syscall counter"); // 经shell的exec进入时已经有请求，不能硬编码初始值1。
    long parent = inspect(); // 主执行者版本头在首次进入用户前即有真实TID。
    require(parent == airi_getpid(), "primary TID"); // 主执行者初始TID等于PID。
    struct airi_runtime_tcb *value = control(); // 本阶段只在无worker时修改自己的版本，观察后立即恢复。
```

<!-- replace: labs/airios/identity.c -->
<!-- before -->
```c
    long child = raw(6); // 直接系统调用，证明身份更新不依赖库的fork回调。
    require(child >= 0, "raw fork"); // 失败不进入孩子逻辑。
    if (!child) { // 新地址空间保留调用继续点和普通TLS运行值。
        require(local == 99 && inspect() == airi_getpid() && airi_thread_id() != parent, "raw fork child identity"); // 只有TCB身份被内核改为孩子。
        local = 123; // 与随后exec的声明初值区别。
        airi_exec("identity-reset"); // 成功进入新main，保留同一孩子PID。
```
<!-- after -->
```c
    long child = raw(6); // 直接系统调用，证明身份更新不依赖库的fork回调。
    require(child >= 0, "raw fork"); // 失败不进入孩子逻辑。
    if (!child) { // 新地址空间保留调用继续点和普通TLS运行值。
        require(airi_thread_syscalls() == 1, "fork child starts counter"); // 孩子恢复父调用点，但没有实际进入父的6号请求。
        require(local == 99 && inspect() == airi_getpid() && airi_thread_id() != parent, "raw fork child identity"); // 只有TCB身份被内核改为孩子。
        local = 123; // 与随后exec的声明初值区别。
        airi_exec("identity-reset"); // 成功进入新main，保留同一孩子PID。
```

<a id="syscalls-host-grouping"></a>
## 交替到来的日志，按真实 PID 和 TID 还原

guest 自己检查差值；宿主再消费45号日志，核对执行者生命周期。主执行者有入口、开始、结束三个快照，孩子在 exec 前后各三个，共六个。三位 worker 各三个，因此共有18条查询，分别属于五条执行链。孩子 exec 后依然使用同一个 (pid, tid)，不能因为再进了一次 main 就建立新计数。

宿主的 counts 字典以两个字符串组成的元组 (pid,tid) 为键，值是该执行者按日志顺序取得的整数列表。setdefault 在首次遇到键时放入空列表，随后 append 追加本次 count；它只合并同一条执行链，别人的日志插在中间也不改变各自顺序。map(len,counts.values()) 对每个列表取长度，sum 得到总条数18。

父的身份来自本次 EXEC_COMMIT，孩子来自该父的 FORK，worker 来自同一 PID 的 THREAD_CREATE，不固定恰好出现的83、84。root_counts[2]-root_counts[1] 取同一局部窗口的结束减开始；孩子的前后两个窗口分别是[2]-[1]、[5]-[4]。孩子[3]还必须大于[2]，说明 exec 自身及周围请求没有重置计数。保留原来的七次42号查询和真实用户 tick 核对，再加入这些条件：

<!-- replace: scripts/shell_test.py -->
<!-- before -->
```python
                    assert queries.count((identity_owner.group(1),tid))==1 # 大量快速读取和互斥调用不增加查询陷入。
                    returned=re.search(r'\[THREAD_RETURN\] pid='+identity_owner.group(1)+r' tid='+tid+r' user_ticks=(\d+)',identity_output) # 独立用户tick来自真实IRQ。
                    assert returned and int(returned.group(1))>=2 # 不把主动yield当抢占隔离证据。
                for repetition in range(2): # 同一shell连续完成两次线程命令，上一组退出回收后应能重新创建。
                    offset=len(text); wire.sendall(b'threads\n'); text=wait('[SHELL_COMMAND_DONE] name=threads code=0',offset) # 真正同地址空间执行，不能由fork复制或宿主线程模拟。
                    thread_output=text[offset:] # 所有身份与标记均限定在这条命令之后。
```
<!-- after -->
```python
                    assert queries.count((identity_owner.group(1),tid))==1 # 大量快速读取和互斥调用不增加查询陷入。
                    returned=re.search(r'\[THREAD_RETURN\] pid='+identity_owner.group(1)+r' tid='+tid+r' user_ticks=(\d+)',identity_output) # 独立用户tick来自真实IRQ。
                    assert returned and int(returned.group(1))>=2 # 不把主动yield当抢占隔离证据。
                counts={} # 以真实(pid,tid)为键，各自保存计数快照；其他工作者输出可以插入其间。
                for pid,tid,count in re.findall(r'\[THREAD_SYSCALLS\] pid=(\d+) tid=(\d+) count=(\d+)',identity_output): # 只消费本条命令真实45号记录。
                    counts.setdefault((pid,tid),[]).append(int(count)) # 第一次建立空列表，随后按该执行者顺序追加整数。
                root_id=identity_owner.group(1) # 与上面的42号参考使用同一次真实进程身份。
                child_match=re.search(r'\[FORK\] parent='+root_id+r' child=(\d+)',identity_output) # exec沿同一孩子继续，不能固定本轮数字。
                assert child_match # 必须确实走过原始fork。
                child_id=child_match.group(1) # 孩子的主TID等于它自己的PID。
                assert len(counts)==5 and sum(map(len,counts.values()))==18 # 父三次、孩子跨exec六次、三位worker各三次。
                root_counts=counts[(root_id,root_id)] # 父执行者已有shell exec请求，不硬编码第一项。
                child_counts=counts[(child_id,child_id)] # exec只换映像，仍积累同一执行者的请求。
                assert len(root_counts)==3 and root_counts[2]-root_counts[1]==9 # 八次unlock通知，加结束查询自身一次。
                assert len(child_counts)==6 and child_counts[0]==1 and child_counts[2]-child_counts[1]==9 and child_counts[5]-child_counts[4]==9 and child_counts[3]>child_counts[2] # fork清零、exec保留，两个相同局部锁窗口分别计数。
                for tid in identity_workers: # 工作者可以交替运行，仍分别验证各自三个值。
                    samples=counts[(root_id,tid)] # 共享PID不能合并不同TID的测量。
                    assert len(samples)==3 and samples[0]==1 and samples[2]-samples[1]==9 # 新执行者首查询为1，无竞争八对操作仍有八次通知。
                assert identity_output.count('[MUTEX_PROFILE] loops=8 delta=9')==6 # guest与宿主按同一真实测量各自核对。
                for repetition in range(2): # 同一shell连续完成两次线程命令，上一组退出回收后应能重新创建。
                    offset=len(text); wire.sendall(b'threads\n'); text=wait('[SHELL_COMMAND_DONE] name=threads code=0',offset) # 真正同地址空间执行，不能由fork复制或宿主线程模拟。
                    thread_output=text[offset:] # 所有身份与标记均限定在这条命令之后。
```

查询差值9包括结束查询自身，锁的额外请求仍是八次44号。下面机器结果把这两个量分别记作八对操作与八次锁请求；per_execution_syscall_count 只记录前面的PID/TID分组检查已经通过，不代替实际列表与差值断言。

<!-- replace: scripts/shell_test.py -->
<!-- before -->
```python
                offset=len(text); wire.sendall(b'echo after-shell\n'); text=wait('[CONSOLE_UPDATE] view=0 commands=2 page=0',offset) # 回到内核后输入仍完整。
                assert 'after-shell' in text[offset:] # 不只是退出而丢了后续UART。
                result={'passed':True,'ring3_shell':True,'ram_file_and_device_readback':True,'other_user_during_cq_wait':True,'qmp_iops':1,'error_exec_survived':True,'returned_to_monitor':True,'orphan_rounds':24,'memory_reuse_rounds':128,'capacity_rounds':4,'capacity_pages':1024,'simd_preempt_fork_exec':True,'fs_base_preempt_fork_exec':True,'compiler_static_tls':True,'posix_errno_io_subset':True,'positioned_vectors':True} # 只发布已观察事实。
                result.update(runtime_tcb_identity=True,identity_reference_queries=7,shared_malloc=True,malloc_workers=4,malloc_rounds=96,owned_mutex=True,mutex_critical_updates=64,private_futex=True,futex_registrations=8,user_heap=True,shared_executions=True,thread_commands=2,worker_capacity=8,worker_reuse_rounds=24) # 关键字把实际已验收字段加入结果字典，不替代上面的逐命令检查。
                result_path.write_text(json.dumps(result,indent=2)+'\n'); print('PASS user shell: alloc/file/async wait/other task/CQ/readback, failed exec, return to monitor') # 阶段演示实际完成。
                return result # 总入口可收集。
            finally: # 包括断言、超时和QEMU错误。
```
<!-- after -->
```python
                offset=len(text); wire.sendall(b'echo after-shell\n'); text=wait('[CONSOLE_UPDATE] view=0 commands=2 page=0',offset) # 回到内核后输入仍完整。
                assert 'after-shell' in text[offset:] # 不只是退出而丢了后续UART。
                result={'passed':True,'ring3_shell':True,'ram_file_and_device_readback':True,'other_user_during_cq_wait':True,'qmp_iops':1,'error_exec_survived':True,'returned_to_monitor':True,'orphan_rounds':24,'memory_reuse_rounds':128,'capacity_rounds':4,'capacity_pages':1024,'simd_preempt_fork_exec':True,'fs_base_preempt_fork_exec':True,'compiler_static_tls':True,'posix_errno_io_subset':True,'positioned_vectors':True} # 只发布已观察事实。
                result.update(per_execution_syscall_count=True,uncontended_mutex_pairs=8,uncontended_mutex_syscalls=8,runtime_tcb_identity=True,identity_reference_queries=7,shared_malloc=True,malloc_workers=4,malloc_rounds=96,owned_mutex=True,mutex_critical_updates=64,private_futex=True,futex_registrations=8,user_heap=True,shared_executions=True,thread_commands=2,worker_capacity=8,worker_reuse_rounds=24) # 关键字把实际已验收字段加入结果字典，不替代上面的逐命令检查。
                result_path.write_text(json.dumps(result,indent=2)+'\n'); print('PASS user shell: alloc/file/async wait/other task/CQ/readback, failed exec, return to monitor') # 阶段演示实际完成。
                return result # 总入口可收集。
            finally: # 包括断言、超时和QEMU错误。
```

完成这些修改后，既有 Makefile 和用户镜像目录会自动重编这几个已注册对象，不需要新增程序或链接依赖。运行：

<!-- run -->
```sh
./airios build  # 编译每执行者计数及同一identity程序的测量
```

<!-- run -->
```sh
./airios test  # 同时运行真实shell、计数生命周期和既有故障/存储路径
```

<a id="syscalls-results"></a>
## 本次实际值怎样对应八次通知

交互运行 `./airios run`，在内核提示符输入 `run sh`，再输入 `identity`。本轮主执行者的快照如下，其他轮次 PID 可不同：

```text
[THREAD_SYSCALLS] pid=83 tid=83 count=2
[THREAD_SYSCALLS] pid=83 tid=83 count=4
[THREAD_SYSCALLS] pid=83 tid=83 count=13
[MUTEX_PROFILE] loops=8 delta=9
```

第一次返回2，因为 shell 的孩子已经用7号 exec 进入 identity，然后才查询45。inspect 做一次原始42号，开始查询便得到4；八次44号和结束45号让它成为13。4到13之间没有 puts，成功文字在结束查询以后才输出，因此这次差值不包含打印文字的1号请求。

同一轮孩子的六个值为1、3、12、16、18、27。1表明 fork 创建了新的执行计数；3到12仍是同一八次通知窗口。随后孩子查询 PID、调用 exec，重置入口查询得到16，没有重新变成1；18到27又测同样八对锁。三个 worker 分别得到1、3、12。它们可以被真实 PIT 抢占，但不会把彼此的请求加进自己的列表。

上述三个生命周期位置共运行六次 inspect，实际出现六条 delta=9。每次扣去结束查询，都是八次锁相关请求；K56 的大量快速身份读取仍不增加42号参考数量。这不是“锁只有一次 CAS 所以没有系统调用”的推测，也不证明锁耗时降低。下一节将保留这个观察窗口，修改通知协议以后用同一差值比较。

本节没有更改等待、截止或错误行为。K54 的错误所有者、超时、带锁 yield 和非 robust 死亡路径，K55 的共享分配与失败保留仍由完整 test 检查。固定单 CPU、不可抢占内核继续成立；本轮没有新的 Linux 或图形实测，Linux 连接失败记录也不由这份 guest 输出替代。
