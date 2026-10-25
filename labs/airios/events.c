#include <airi_event.h> // 自有变化序号通知。
#include <airi_threads.h> // 实际线程与独立请求计数。
#include <airi_libc.h> // PIT、主动交接和串口。
#include <stdatomic.h> // 夹具准备/重查次数单独原子发布。
static airi_event changed = AIRI_EVENT_INITIALIZER; // 本程序的唯一普通等待键，初始版本0。
static airi_event edge = {UINT32_MAX - 1}; // 独立对象观察最后一次发布和饱和，不重置活对象。
static _Atomic unsigned ready, rechecks; // 两worker是否运行，以及无版本变化返回的重查次数。
static void require(int condition, const char *message) { // 错误结束真实进程组。
    if (!condition) { airi_puts(message); airi_puts("\n"); airi_exit(86); } // 不让成功字符串越过失败。
}
static void await(_Atomic unsigned *word, unsigned value) { // 握手仍有总期限，不把yield次数当时间。
    uint64_t start = airi_ticks(); // 同一名义时钟来源。
    while (atomic_load_explicit(word, memory_order_acquire) < value) { // 对方release发布到达以后继续。
        require(airi_ticks() - start < 100, "event handshake timeout"); // 错误调度位置不能无限挂起。
        airi_yield(); // 交给真正的其他用户执行者。
    }
}
static long worker(void *argument) { // 两人观察同一个版本变化，通知不消费任务。
    (void)argument; // 本轮没有每worker独占参数。
    long expected = airi_event_sequence(&changed); // 发布以前的实际版本1，失败不能窄化成巨大期望。
    if (expected != 1) return -230; // 每人必须在最后发布以前先运行。
    atomic_fetch_add_explicit(&ready, 1, memory_order_release); // 只表示已取得快照，还不等于内核已登记。
    uint64_t start = airi_ticks(); // 重查不能重新借200tick。
    for (;;) { // 即使被通知返回0，仍要判断版本是否真的改变。
        long current = airi_event_sequence(&changed); // acquire快照，不读取未同步的普通业务对象。
        if (current < 0) return current; // 形状错误不转成合法版本。
        if (current != expected) return current; // 本轮只有最后一次发布，实际返回2。
        uint64_t elapsed = airi_ticks() - start; // 所有通知和重试共用同一个开始位置。
        if (elapsed >= 200) return -110; // 剩余预算相减以前先检查。
        long result = airi_event_wait(&changed, (uint32_t)expected, (unsigned)(200 - elapsed)); // 保留快照到实际WAIT的比较关系。
        if (result) return result; // 到期或其他错误不伪装成变化。
        if (airi_event_sequence(&changed) == expected) atomic_fetch_add_explicit(&rechecks, 1, memory_order_release); // 无变化的通知必须回循环，不当作完成。
    }
}
int main(void) { // 所有通知对象在创建worker以前已初始化。
    require(airi_event_sequence(0) == -22 && airi_event_publish((airi_event *)((uintptr_t)&changed + 1)) == -22, "event invalid shape"); // 显式坏形状在用户原子指令以前拒绝。
    require(airi_event_wait(&changed, 0, 6001) == -22, "event invalid budget"); // 即使版本相符也不能扩大接口预算。
    require(airi_event_sequence(&edge) == (long)UINT32_MAX - 1 && airi_event_publish(&edge) == 0, "event last publication"); // 唤醒0不等于未发布。
    require(airi_event_sequence(&edge) == (long)UINT32_MAX && airi_event_publish(&edge) == -75 && airi_event_sequence(&edge) == (long)UINT32_MAX, "event refuses wrap"); // 饱和错误不改版本。
    long old = airi_event_sequence(&changed); // 普通对象最初是0。
    require(old == 0 && airi_event_publish(&changed) == 0 && airi_event_sequence(&changed) == 1, "event publication before wait"); // 没有等待者，仍推进版本。
    uint64_t before = airi_thread_syscalls(); // 测量旧快照等待是否在用户态直接返回。
    require(airi_event_wait(&changed, (uint32_t)old, 20) == 0, "event old snapshot recheck"); // 当前1不等于旧0，不睡眠。
    uint64_t after = airi_thread_syscalls(); // 结束查询自身计一次。
    require(after - before == 1, "event changed snapshot stays local"); // 这次wait没有43号请求。
    require(airi_event_wait(&changed, 1, 0) == -110, "event zero budget"); // 同版本没有变化，无预算立即到期。
    uint64_t start = airi_ticks(); // 同版本真实等待三tick。
    require(airi_event_wait(&changed, 1, 3) == -110 && airi_ticks() - start >= 3, "event real timeout"); // 不能把先前发布当可晚领取的令牌。
    long first = airi_thread_create(worker, 0), second = airi_thread_create(worker, 0), result; // 实际同进程共享一个等待键。
    require(first > 0 && second > 0, "event workers"); // 不使用宿主线程。
    await(&ready, 2); require(airi_sleep(3) == 0, "event first registration"); // 准备位置之后给两人实际登记机会。
    require(airi_futex_wake(&changed.sequence, 16) == 2, "event notification without change"); // 故意只通知，不推进版本，不伪称业务发生变化。
    await(&rechecks, 2); require(airi_sleep(3) == 0, "event repeated registration"); // 两人均发现条件未变并再次等待。
    require(airi_event_sequence(&changed) == 1 && airi_event_publish(&changed) == 2, "event publish to both waiters"); // 版本1变2，实际唤醒两位。
    require(airi_thread_join(first, &result) == 0 && result == 2 && airi_thread_join(second, &result) == 0 && result == 2, "event broadcast results"); // 每人都重新读到2，不消费唯一令牌。
    require(atomic_load_explicit(&rechecks, memory_order_acquire) == 2, "event false wake rechecked"); // 原始通知不能直接结束工作。
    start = airi_ticks(); // 新读版本2以后再等，不会领取旧发布。
    require(airi_event_wait(&changed, 2, 3) == -110 && airi_ticks() - start >= 3, "event late fresh snapshot timeout"); // 当前没有下一次变化。
    airi_puts("[EVENT_USER_OK] old_snapshot=changed timeout=pit false_wake=rechecked broadcast=2 late_wait=timeout saturation=refused\n"); // 所有正常、先通知、无变化返回和失败边界已完成。
    return 0; // shell取得真实成功结果。
}
