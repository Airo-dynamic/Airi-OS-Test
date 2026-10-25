#include <airi_mutex.h> // owner布局与对外返回合同。
#include <airi_threads.h> // 真实执行身份与比较并等待。
#include <airi_libc.h> // 已交付PIT事件计数，不使用墙钟。
_Static_assert(sizeof(airi_mutex) == 8 && _Alignof(airi_mutex) >= 4, "mutex needs two aligned four-byte words"); // 两个四字节原子字段，WAIT仍只使用owner地址。
_Static_assert(__atomic_always_lock_free(4, 0), "mutex needs native four-byte atomics"); // 当前目标不能导入未实现的libatomic。
static int valid(airi_mutex *mutex) { // C库仍要求调用者提供有效存活对象。
    return mutex && !((uintptr_t)mutex & 3); // 只拒绝NULL/未对齐；任意坏映射仍是用户CPU故障。
}
static int acquire(airi_mutex *mutex, uint32_t tid, uint32_t *observed) { // 原子比较只在空闲值0时写入调用者TID。
    *observed = 0; // 每次CAS都重新尝试空闲状态，不沿用前次失败值。
    return __atomic_compare_exchange_n(&mutex->owner, observed, tid, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); // 成功承接普通数据发布，失败也进入与登记/释放一致的全局原子顺序。
}
long airi_mutex_trylock(airi_mutex *mutex) { // 一次所有权争取，不调用WAIT。
    if (!valid(mutex)) return -22; // -EINVAL，在任何原子访存前拒绝显式坏形状。
    uint32_t tid = (uint32_t)airi_thread_id(), observed; // 有效版本运行头直接提供TID，旧版本才进入42号后备。
    return acquire(mutex, tid, &observed) ? 0 : -16; // -EBUSY，包括本执行者已经持有的情况。
}
long airi_mutex_lock(airi_mutex *mutex, unsigned ticks) { // 首次失败后只建立一个固定截止。
    if (!valid(mutex) || ticks > 6000) return -22; // 完整接口预算先检查，不能锁住后才报参数错误。
    uint32_t tid = (uint32_t)airi_thread_id(), observed; // 一个调用内保留同一真实身份。
    if (acquire(mutex, tid, &observed)) return 0; // 空闲时不读取时钟，也不登记内存等待。
    if (observed == tid) return -35; // -EDEADLK，不把自己的锁睡成超时。
    if (!ticks) return -110; // 仍被别人占用且没有等待预算。
    uint64_t now = airi_ticks(); // 从首次确需等待的位置计算名义时间。
    if (now > UINT64_MAX - ticks) return -75; // 截止不能回绕。
    uint64_t deadline = now + ticks; // 重试或通知都不能延长这次总预算。
    __atomic_fetch_add(&mutex->contenders, 1, __ATOMIC_SEQ_CST); // 先登记可能等待的参与者，再重新检查owner，不能反过来留下漏通知窗口。
    long result; // 所有登记后的成功/失败共用一个撤销出口。
    for (;;) { // 初次也先重新CAS；醒来只说明可重试，不代表已经获锁。
        if (acquire(mutex, tid, &observed)) { result = 0; break; } // 登记后锁可能早已空闲，先争取避免带旧owner睡下。
        if (observed == tid) { result = -35; break; } // 错误重入仍须撤销本次参与计数。
        now = airi_ticks(); // 所有重试共用同一次固定截止，不续借预算。
        if (now >= deadline) { result = -110; break; } // 相减前先拒绝到期，防止无符号回绕。
        result = airi_futex_wait(&mutex->owner, observed, (unsigned)(deadline - now)); // 登记到内核前再次比较实际owner，解决复查以后的释放。
        if (result && result != -11) break; // 超时/错误统一撤销，值改变-11则回循环争取。
    }
    __atomic_fetch_sub(&mutex->contenders, 1, __ATOMIC_SEQ_CST); // 取得、超时及错误各减一次；重试期间一直保留登记。
    return result; // 0才拥有普通数据访问权，负值未取得owner。
}
long airi_mutex_unlock(airi_mutex *mutex) { // 释放与通知是两个分别完成的动作。
    if (!valid(mutex)) return -22; // 只检查能在C函数内安全判断的形状。
    uint32_t tid = (uint32_t)airi_thread_id(); // 不允许另一执行者冒充当前持有者。
    if (!__atomic_compare_exchange_n(&mutex->owner, &tid, 0, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) return -1; // -EPERM；成功发布普通数据并与参与者登记统一排序，失败不改owner。
    if (!__atomic_load_n(&mutex->contenders, __ATOMIC_SEQ_CST)) return 0; // 没有慢路径参与者时省去44号；已登记者必须重查owner。
    long result = airi_futex_wake(&mutex->owner, 1); // 先发布空闲，再通知；空隙由WAIT内部比较弥补。
    return result < 0 ? result : 0; // 若通知意外失败，所有权已经释放，不能倒退恢复旧TID。
}
