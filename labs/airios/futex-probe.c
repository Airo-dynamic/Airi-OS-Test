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
    require(airi_pipe(notice, 0) == 0 && airi_pipe(permission, 0) == 0, "prewake pipes"); // 两条通道互不消费对方通知。
    long tid = airi_thread_create(late_waiter, 0), result; // 先让工作者读取1并在许可管道上等待。
    unsigned char byte; // 主执行者自己的栈缓冲区。
    require(tid > 0 && airi_read(notice[0], &byte, 1) == 1 && byte == 'R', "old condition observed"); // 不是靠宿主延时猜先后。
    atomic_store_explicit(&word, 2, memory_order_release); // 先发布新条件，再通知。
    require(airi_futex_wake(address(&word), 16) == 0, "prewake has no waiter"); // 通知不积攒，工作者此刻还在管道等待。
    require(airi_write(permission[1], "G", 1) == 1 && airi_thread_join(tid, &result) == 0 && result == -11, "late WAIT rejects stale value"); // 没有等待信用仍不丢更新，因为内核再比较值。
    close_channels(); // 工作者已结束才归还共享表中的引用。
    airi_puts("[FUTEX_PREWAKE_OK] old=1 published=2 wake=0 wait=-11\n"); // 实际先通知后等待路径已经完成。
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
