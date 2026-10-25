#include <airi_mutex.h> // 与普通程序相同的8字节锁和算法。
#include <airi_threads.h> // 真实工作执行者及请求计数。
#include <airi_libc.h> // 有限名义tick、主动交接和串口。
#include <stdatomic.h> // 观察夹具的握手不借被观察锁。
static airi_mutex guard = AIRI_MUTEX_INITIALIZER; // 四轮之间只在完全收拢以后复用。
static _Atomic unsigned target, armed, parked, gate, started; // 选定一个暂停位置，发布到达与继续许可。
static _Atomic unsigned calls, mismatches, notifications; // 记录真实WAIT返回，不把未登记误认为睡眠。
static void require(int condition, const char *message) { // 任一错误结束本真实进程组。
    if (!condition) { airi_puts(message); airi_puts("\n"); airi_exit(85); } // 不继续发布成功标记。
}
static void await(_Atomic unsigned *word, unsigned value) { // 所有观察握手都有总期限。
    uint64_t start = airi_ticks(); // 使用真实交付的100Hz事件，不把yield次数当时间。
    while (atomic_load_explicit(word, memory_order_acquire) != value) { // 对方release发布以后才继续当前操作。
        require(airi_ticks() - start < 100, "mutex race handshake timeout"); // 错位的暂停点不能无限挂住guest。
        airi_yield(); // 让真正的另一条用户执行链运行。
    }
}
void airi_mutex_observe(airi_mutex *mutex, unsigned stage, long result) { // 只有观察编译的锁对象调用，本文件提供最终链接定义。
    require(mutex == &guard, "unexpected observed mutex"); // 不把其他库内部锁纳入握手。
    if (stage == 4) { // 真实WAIT返回以后，result已经来自43号。
        atomic_fetch_add_explicit(&calls, 1, memory_order_relaxed); // 记录每次实际发出的等待请求，包括立即-11。
        if (result == -11) atomic_fetch_add_explicit(&mismatches, 1, memory_order_relaxed); // 比较失败没有真正睡下。
        if (result == 0) atomic_fetch_add_explicit(&notifications, 1, memory_order_relaxed); // 返回0表示真实通知，仍不表示授锁。
        return; // 返回观察点不能再次暂停同一个请求。
    }
    if (stage != atomic_load_explicit(&target, memory_order_relaxed) || !atomic_exchange_explicit(&armed, 0, memory_order_acq_rel)) return; // 仅命中选定点的第一次，后续重试保持算法运行。
    atomic_store_explicit(&parked, 1, memory_order_release); // 发布暂停点以前所有用户字段状态。
    await(&gate, 1); // 主执行者释放以后再继续原来的锁调用。
}
static long worker(void *argument) { // 每轮真实创建一位竞争者。
    (void)argument; // 观察配置在创建前通过原子字段发布。
    atomic_store_explicit(&started, 1, memory_order_release); // 第四轮不用暂停，但主执行者仍需知道worker已运行。
    if (airi_mutex_lock(&guard, 200)) return -220; // 共用普通实现的固定截止、登记、复查和内核WAIT。
    if (__atomic_load_n(&guard.contenders, __ATOMIC_SEQ_CST) != 0) return -221; // 本轮只有自己，取得以后必须已撤销。
    return airi_mutex_unlock(&guard); // 没有其他参与者，归还时不应通知。
}
int main(void) { // 暂停只属于本观察ELF，普通锁没有这些回调。
    static const char *begin[] = { // 固定字符串便于宿主划定每轮输出，不额外引入格式化库。
        "[MUTEX_RACE_BEGIN] stage=1\n", // 第一次失败以后、登记以前。
        "[MUTEX_RACE_BEGIN] stage=2\n", // 登记以后、复查以前。
        "[MUTEX_RACE_BEGIN] stage=3\n", // 复查失败以后、WAIT以前。
        "[MUTEX_RACE_BEGIN] stage=4\n" // 不暂停，让worker真正登记到内核。
    };
    static const char *done[] = { // 每个标记都在实际数值核对后输出。
        "[MUTEX_RACE_DONE] stage=1 participants=0 wake=skipped wait=none\n", // 无参与者释放，二次CAS直接取得。
        "[MUTEX_RACE_DONE] stage=2 participants=1 wake=empty wait=none\n", // 空通知允许存在，复查避免WAIT。
        "[MUTEX_RACE_DONE] stage=3 participants=1 wake=empty wait=mismatch\n", // 最后比较返回-11而不是睡错。
        "[MUTEX_RACE_DONE] stage=4 participants=1 wake=one wait=notified\n" // 已睡者确实被一次通知唤醒。
    };
    for (unsigned stage = 1; stage <= 4; ++stage) { // 每轮join完再重置握手，不能覆盖活观察状态。
        atomic_store_explicit(&target, stage, memory_order_relaxed); // 1至3暂停，4只观察真实等待返回。
        atomic_store_explicit(&armed, 1, memory_order_relaxed); // 重新允许选定点命中一次。
        atomic_store_explicit(&parked, 0, memory_order_relaxed); atomic_store_explicit(&gate, 0, memory_order_relaxed); // 清除上一轮到达与放行。
        atomic_store_explicit(&started, 0, memory_order_relaxed); atomic_store_explicit(&calls, 0, memory_order_relaxed); // 此时没有worker访问这些对象。
        atomic_store_explicit(&mismatches, 0, memory_order_relaxed); atomic_store_explicit(&notifications, 0, memory_order_relaxed); // 真实返回统计重新开始。
        airi_puts(begin[stage - 1]); // 在加锁前建立本轮日志边界。
        require(airi_mutex_lock(&guard, 0) == 0, "race primary owns guard"); // worker首次争取必定面对主所有权。
        long tid = airi_thread_create(worker, 0), result; // 不用宿主模拟或fork复制代替共享地址空间。
        require(tid > 0, "race worker create"); // 创建失败不继续等待不存在的人。
        if (stage < 4) await(&parked, 1); // 三轮分别停在不同真实语句之间。
        else { await(&started, 1); require(airi_sleep(3) == 0, "race actual waiter"); } // 主持锁睡三tick，worker有机会真正登记WAIT。
        require(__atomic_load_n(&guard.contenders, __ATOMIC_SEQ_CST) == (stage == 1 ? 0u : 1u), "race participant position"); // 握手位置必须与算法字段相符。
        uint64_t before = airi_thread_syscalls(); // 只测主的这次unlock，不混入另一执行者的观察回调。
        require(airi_mutex_unlock(&guard) == 0, "race primary release"); // 根据真实参与数省略、空通知或通知一位。
        uint64_t after = airi_thread_syscalls(); // 结束查询自身仍计一次。
        require(after - before == (stage == 1 ? 1u : 2u), "race release syscall count"); // 1只查询，2为44号加查询。
        atomic_store_explicit(&gate, 1, memory_order_release); // 前三轮从原暂停点继续；第四轮无暂停。
        require(airi_thread_join(tid, &result) == 0 && result == 0, "race worker result"); // 继续原锁调用，不能重开一次调用绕过窗口。
        require(atomic_load_explicit(&calls, memory_order_relaxed) == (stage >= 3 ? 1u : 0u), "race wait call count"); // 第三轮立即失败也是一次实际43号。
        require(atomic_load_explicit(&mismatches, memory_order_relaxed) == (stage == 3 ? 1u : 0u), "race compare mismatch"); // 只有复查以后释放走-11。
        require(atomic_load_explicit(&notifications, memory_order_relaxed) == (stage == 4 ? 1u : 0u), "race notification return"); // 已睡轮必须由真实通知返回0。
        require(__atomic_load_n(&guard.owner, __ATOMIC_SEQ_CST) == 0 && __atomic_load_n(&guard.contenders, __ATOMIC_SEQ_CST) == 0, "race final cleanup"); // 回收工作者后两字段均归零再进入下一轮。
        airi_puts(done[stage - 1]); // 所有状态和调用数均符合才发布本轮结束。
    }
    airi_puts("[MUTEX_RACES_OK] stages=4 actual_wait=1 compare_mismatch=1 normal_library=unchanged\n"); // 四个窗口由同一实际锁实现完成。
    return 0; // shell收取真实程序结果。
}
