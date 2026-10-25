#include <airi_event64.h> // 数据值与状态码使用不同返回位置。
#include <airi_threads.h> // 八字节私有等待键，不能误用原32位接口。
_Static_assert(sizeof(airi_event64) == 8 && _Alignof(airi_event64) >= 8 && __atomic_always_lock_free(8, 0), "wide event needs native aligned atomics"); // 本目标不导入额外原子运行库。
static int valid(airi_event64 *event) { // 显式坏形状先拒绝，其他无效对象仍违反用户库寿命合同。
    return event && !((uintptr_t)event & 7); // 自然八字节对齐。
}
long airi_event64_snapshot(airi_event64 *event, uint64_t *value) { // output必须属于调用者私人存储，不能是活版本本身。
    if (!valid(event) || !value || ((uintptr_t)value & 7) || value == &event->sequence) return -22; // 失败不写输出，不用普通写覆盖原子字段。
    *value = __atomic_load_n(&event->sequence, __ATOMIC_ACQUIRE); // 包括UINT64_MAX在内都作为数据保存，不窄化到long。
    return 0; // 状态与完整数据分别交付。
}
long airi_event64_wait(airi_event64 *event, uint64_t expected, unsigned ticks) { // 不替调用者决定业务条件。
    if (!valid(event) || ticks > 6000) return -22; // 完整预算先验证。
    if (__atomic_load_n(&event->sequence, __ATOMIC_ACQUIRE) != expected) return 0; // 早已变化则无需46号。
    long result = airi_futex_wait64(&event->sequence, expected, ticks); // 实际睡眠前仍由内核比较并登记。
    if (!result || result == -11) { // 真通知与值不符均要求回去重查。
        (void)__atomic_load_n(&event->sequence, __ATOMIC_ACQUIRE); // 返回前读取实际可见的发布。
        return 0; // 不授锁，也不把0当已领取工作。
    }
    return result; // 包括真实PIT到期，交给上层总预算策略。
}
long airi_event64_advance(airi_event64 *event) { // 与唤醒分开，便于明确状态提交和通知位置。
    if (!valid(event)) return -22; // 不让显式坏形状进入原子操作。
    uint64_t expected = __atomic_load_n(&event->sequence, __ATOMIC_RELAXED); // 作为CAS旧值，不据此访问业务数据。
    do { // 并发发布失败后使用CAS回填的新值继续。
        if (expected == UINT64_MAX) return -75; // 到顶明确失败，不重新变0与旧快照混淆。
    } while (!__atomic_compare_exchange_n(&event->sequence, &expected, expected + 1, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED)); // 成功只推进一次。
    return 0; // 不调用47号，已登记者仍须单独通知或到期。
}
long airi_event64_notify(airi_event64 *event, unsigned count) { // 可以要求重查而不推进版本。
    if (!valid(event) || count > 16) return -22; // 当前全系统有界十六执行槽。
    if (!count) return 0; // 零通知不产生内核请求，仍要求调用者给有效存活对象。
    return airi_futex_wake64(&event->sequence, count); // 实际人数或负错误，不倒退版本或积累令牌。
}
