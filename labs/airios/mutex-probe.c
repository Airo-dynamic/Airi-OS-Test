#include <airi_mutex.h> // 自有有界互斥，不链接pthread。
#include <airi_threads.h> // 工作创建、join和身份。
#include <airi_libc.h> // 进程、时间和串口。
#include <stdatomic.h> // 夹具准备位置单独发布，不借锁内普通数据传信。
static airi_mutex guard = AIRI_MUTEX_INITIALIZER; // 在创建任何工作者以前建立空闲锁。
static unsigned counter, checksum = 0x5a5a; // 两个普通对象只在持锁期间一起更新。
static _Atomic unsigned ready; // 仅协调工作者开始尝试，不表示已经登记WAIT。
static void require(int condition, const char *message) { // 出错终止当前真实用户进程组。
    if (!condition) { airi_puts(message); airi_puts("\n"); airi_exit(82); } // 不让后续成功标记掩盖错误。
}
static long change(void *argument) { // 两个工作者使用相同锁保护同一份普通状态。
    (void)argument; // 当前只比较共同累计结果。
    atomic_fetch_add_explicit(&ready, 1, memory_order_release); // 主执行者仍持锁，随后给出登记机会。
    for (unsigned i = 0; i < 32; ++i) { // 每人完成32次真实临界区更新。
        if (airi_mutex_lock(&guard, 200)) return -100; // 争用时必须阻塞，通知后重新CAS。
        unsigned before = counter; // 先读旧值，稍后才写新值。
        if (checksum != (before ^ 0x5a5a)) return -101; // 半更新状态不能跨持有者泄露。
        airi_yield(); // 故意带锁让出CPU，迫使另一执行者面对真实争用。
        counter = before + 1; // 原子所有权保护普通非原子数据。
        checksum = counter ^ 0x5a5a; // 与counter构成一个必须一起观察的状态。
        if (airi_mutex_unlock(&guard)) return -102; // release先发布全部普通写，再通知等待者。
    }
    return 32; // 每个真实worker交出自己的完成数。
}
static long foreign(void *argument) { // 主执行者持锁时不能让另一执行者解除所有权。
    (void)argument; // 共享guard在进程BSS中长期有效。
    if (airi_mutex_unlock(&guard) != -1 || airi_mutex_trylock(&guard) != -16) return -103; // 两次失败都不改主执行者TID。
    uint64_t start = airi_ticks(); // 本次锁一直由主执行者持有。
    long result = airi_mutex_lock(&guard, 3); // 使用真实futex截止，不忙等三轮。
    return result == -110 && airi_ticks() - start >= 3 ? result : -104; // 超时不能误记成已经取得锁。
}
static long abandon(void *argument) { // 明确观察非robust锁的边界。
    (void)argument; // 只在独立孩子里留下无法正常释放的锁。
    return airi_mutex_lock(&guard, 20) == 0 ? 9 : -105; // 故意普通返回而不unlock，join不会替它释放业务对象。
}
int main(void) { // 单独ELF初始counter为0，guard空闲。
    require(airi_mutex_trylock(0) == -22 && airi_mutex_unlock((airi_mutex *)((uintptr_t)&guard + 1)) == -22, "invalid mutex shape"); // 显式坏形状在CPU原子访存前拒绝。
    require(airi_mutex_lock(&guard, 6001) == -22 && airi_mutex_unlock(&guard) == -1, "invalid budget and empty unlock"); // 参数失败后锁仍空闲。
    require(airi_mutex_lock(&guard, 0) == 0 && airi_mutex_trylock(&guard) == -16 && airi_mutex_lock(&guard, 3) == -35, "self ownership"); // 零预算允许立即取得，重入明确失败。
    long first = airi_thread_create(change, 0), second = airi_thread_create(change, 0); // 两条执行者都先面对主执行者持有的锁。
    require(first > 0 && second > 0, "create contenders"); // 不能用一条执行者冒充互斥。
    uint64_t start = airi_ticks(); // 观察夹具也有限制。
    while (atomic_load_explicit(&ready, memory_order_acquire) != 2) { // 等两人已进入工作函数。
        require(airi_ticks() - start < 100, "contender deadline"); airi_yield(); // 准备阶段不伪装为锁实现。
    }
    require(airi_sleep(3) == 0, "contender registration"); // 后面的FUTEX_WAIT日志核对真实登记。
    require(__atomic_load_n(&guard.contenders, __ATOMIC_SEQ_CST) == 2, "two registered contenders"); // 两人均在主执行者持锁期间等待，诊断读取也必须原子化。
    long tester = airi_thread_create(foreign, 0), result; // 在仍持有guard时进行非法释放和超时观察。
    require(tester > 0 && airi_thread_join(tester, &result) == 0 && result == -110, "foreign owner and timeout"); // 主join期间仍持锁，第三人必须到期。
    require(__atomic_load_n(&guard.contenders, __ATOMIC_SEQ_CST) == 2, "timeout withdrew contender"); // 第三人到期退出以后恰好剩原来两位，不残留计数。
    require(counter == 0 && checksum == 0x5a5a && airi_mutex_unlock(&guard) == 0, "publish initial release"); // 两位工作者此前都没有进入临界区。
    require(airi_thread_join(first, &result) == 0 && result == 32, "join first"); // 主执行者等真实结果，不读取运行中的普通数据。
    require(airi_thread_join(second, &result) == 0 && result == 32, "join second"); // 两人的临界区现已全部结束。
    require(__atomic_load_n(&guard.contenders, __ATOMIC_SEQ_CST) == 0, "all contenders withdrew"); // 已取得和正常结束的慢路径同样必须撤销。
    require(airi_mutex_trylock(&guard) == 0 && counter == 64 && checksum == (64 ^ 0x5a5a), "protected ordinary data"); // 加锁后核对最终状态。
    require(airi_mutex_unlock(&guard) == 0, "final unlock"); // 不把持锁对象复制到下面的fork。
    long child = airi_fork(); require(child >= 0, "owner death fork"); // 本时刻没有worker且锁空闲。
    if (!child) { // 持有者返回而未释放的情况只影响这个独立地址空间。
        long owner = airi_thread_create(abandon, 0); // 新TID不会在本次内核启动内复用。
        require(owner > 0 && airi_thread_join(owner, &result) == 0 && result == 9, "abandoned owner joined"); // 执行者已收拢，锁字仍保存它的旧身份。
        require(airi_mutex_lock(&guard, 3) == -110 && airi_mutex_unlock(&guard) == -1, "nonrobust owner remains"); // 不假冒EOWNERDEAD恢复或强制解锁。
        require(__atomic_load_n(&guard.contenders, __ATOMIC_SEQ_CST) == 0, "dead owner timeout withdrew"); // 超时清参与数，不伪造死亡owner已释放。
        airi_exit(41); // 用进程重启边界结束这份不可恢复的观察状态。
    }
    int status; // 孩子退出结果和worker的long返回值分开。
    require(airi_waitpid(child, &status) == child && status == 41, "nonrobust child observed"); // 父从另一页表取得进程状态。
    require(airi_mutex_lock(&guard, 0) == 0 && airi_mutex_unlock(&guard) == 0, "parent mutex unchanged"); // 子的遗留锁不污染父进程。
    airi_puts("[MUTEX_PARTICIPANTS_OK] blocked=2 timeout=withdrawn acquired=withdrawn dead_owner=retained\n"); // 登记和撤销均经真实竞争路径核对。
    airi_puts("[MUTEX_USER_OK] counter=64 critical_yield=1 foreign=denied self=deadlock timeout=pit owner_death=nonrobust\n"); // 所有正常、错误和寿命场景完成后才发布。
    return 0; // shell取得当前命令的真实成功结果。
}
