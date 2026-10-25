#include <airi_event.h> // 单个四字节版本及原始返回合同。
#include <airi_threads.h> // 私有比较并等待与显式通知。
_Static_assert(sizeof(airi_event) == 4 && _Alignof(airi_event) >= 4, "event needs aligned four-byte word"); // 内核按四字节等待字比较。
_Static_assert(sizeof(long) == 8 && __atomic_always_lock_free(4, 0), "event needs 64-bit return and native atomics"); // 无符号32位版本必须可用非负long返回。
static int valid(airi_event *event) { // 库内只能拒绝显式坏形状，任意已释放或坏映射仍由用户故障边界处理。
    return event && !((uintptr_t)event & 3); // 并发期间对象必须保持有效，不移动或清零。
}
long airi_event_sequence(airi_event *event) { // 快照不是申请一个通知令牌。
    if (!valid(event)) return -22; // -EINVAL，在原子访存以前拒绝NULL/未对齐。
    return __atomic_load_n(&event->sequence, __ATOMIC_ACQUIRE); // 64位long能表达所有32位版本，不把最高位当错误。
}
long airi_event_wait(airi_event *event, uint32_t expected, unsigned ticks) { // 一次有界等待，不替调用者循环判断业务条件。
    if (!valid(event) || ticks > 6000) return -22; // 完整预算先检查，变化已发生也不能掩盖坏参数。
    if (__atomic_load_n(&event->sequence, __ATOMIC_ACQUIRE) != expected) return 0; // 先前变化可在用户态发现，省去必然-11的请求。
    long result = airi_futex_wait(&event->sequence, expected, ticks); // 快照以后发生的变化仍由内核比较封住睡眠窗口。
    if (!result || result == -11) { // 被通知与比较不符都只意味着应该重新检查。
        (void)__atomic_load_n(&event->sequence, __ATOMIC_ACQUIRE); // 返回前承接实际可见的发布；通知本身不保证业务条件为真。
        return 0; // 不授锁、不消费任务，也不积累可以晚领的通知次数。
    }
    return result; // 包括名义PIT到期-110，仍由上层决定如何退出等待循环。
}
long airi_event_publish(airi_event *event) { // 调用者先完成受保护状态变更，再发布版本并通知。
    if (!valid(event)) return -22; // 不允许NULL/未对齐进入CAS或内核。
    uint32_t expected = __atomic_load_n(&event->sequence, __ATOMIC_RELAXED); // 仅用于CAS尝试，尚不读取业务数据。
    do { // 多个发布者失败后沿CAS回填的新版本重试，不重复使用旧值。
        if (expected == UINT32_MAX) return -75; // -EOVERFLOW，不回绕成与旧快照相同的版本，也不伪造通知成功。
    } while (!__atomic_compare_exchange_n(&event->sequence, &expected, expected + 1, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED)); // 每次成功恰发布一次变化。
    return airi_futex_wake(&event->sequence, 16); // 当前全系统至多十六执行者；返回0也可能已推进版本，错误后不能倒退版本。
}
