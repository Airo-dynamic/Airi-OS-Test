#include <airi_mutex.h> // owner布局与对外返回合同。
#include <airi_threads.h> // 真实执行身份与比较并等待。
#include <airi_libc.h> // 已交付PIT事件计数，不使用墙钟。
_Static_assert(sizeof(airi_mutex) == 4 && _Alignof(airi_mutex) >= 4, "mutex word must be aligned four bytes"); // 与私有futex键宽度一致。
_Static_assert(__atomic_always_lock_free(4, 0), "mutex needs native four-byte atomics"); // 当前目标不能导入未实现的libatomic。
static int valid(airi_mutex *mutex) { // C库仍要求调用者提供有效存活对象。
    return mutex && !((uintptr_t)mutex & 3); // 只拒绝NULL/未对齐；任意坏映射仍是用户CPU故障。
}
static int acquire(airi_mutex *mutex, uint32_t tid, uint32_t *observed) { // 原子比较只在空闲值0时写入调用者TID。
    *observed = 0; // 每次CAS都重新尝试空闲状态，不沿用前次失败值。
    return __atomic_compare_exchange_n(&mutex->owner, observed, tid, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED); // 强CAS，失败把实际持有者交回observed，成功承接前次release。
}
long airi_mutex_trylock(airi_mutex *mutex) { // 一次所有权争取，不调用WAIT。
    if (!valid(mutex)) return -22; // -EINVAL，在任何原子访存前拒绝显式坏形状。
    uint32_t tid = (uint32_t)airi_thread_id(), observed; // 当前TID由42号调用取得，不缓存fork会复制的旧身份。
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
    for (;;) { // 每次恢复先重新争取所有权，通知本身不授锁。
        long result = airi_futex_wait(&mutex->owner, observed, (unsigned)(deadline - now)); // 比较失败说明持有者已变化，应直接重试。
        if (result && result != -11) return result; // 包括真实-110到期和原始地址错误。
        if (acquire(mutex, tid, &observed)) return 0; // acquire读到release发布的空闲后才能访问受保护普通数据。
        if (observed == tid) return -35; // 防止错误重入变成永久等待。
        now = airi_ticks(); // 被抢占或重试也消耗原来的时间预算。
        if (now >= deadline) return -110; // 不能在相减前让remaining变成巨大无符号数。
    }
}
long airi_mutex_unlock(airi_mutex *mutex) { // 释放与通知是两个分别完成的动作。
    if (!valid(mutex)) return -22; // 只检查能在C函数内安全判断的形状。
    uint32_t tid = (uint32_t)airi_thread_id(); // 不允许另一执行者冒充当前持有者。
    if (!__atomic_compare_exchange_n(&mutex->owner, &tid, 0, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) return -1; // -EPERM，失败不改变owner。
    long result = airi_futex_wake(&mutex->owner, 1); // 先发布空闲，再通知；空隙由WAIT内部比较弥补。
    return result < 0 ? result : 0; // 若通知意外失败，所有权已经释放，不能倒退恢复旧TID。
}
