#include <airi_event64.h> // 完整值写到私人uint64_t，long只返回状态。
#include <airi_threads.h> // 真正用户执行者及请求计数。
#include <airi_libc.h> // PIT、串口与主动交接。
#include <stdatomic.h> // 观察进度与结束数独立原子发布。
static airi_event64 high = {0x8000000000000000ull}, edge = {UINT64_MAX - 1}, changed = AIRI_EVENT64_INITIALIZER; // 高位、最后一次推进和普通等待分开观察。
static _Atomic unsigned ready, rechecks, done; // 分别表示开始、无变化返回和真正读到新版本。
static void require(int condition, const char *message) { // 失败终止本真实用户进程组。
    if (!condition) { airi_puts(message); airi_puts("\n"); airi_exit(92); } // 不发布虚假成功。
}
static void await(_Atomic unsigned *word, unsigned value) { // 夹具总等待有界。
    uint64_t start = airi_ticks(); // 同一名义时钟。
    while (atomic_load_explicit(word, memory_order_acquire) < value) { // 对方release发布以后继续。
        require(airi_ticks() - start < 100, "event64 handshake timeout"); airi_yield(); // 让真实其他执行者运行。
    }
}
static long waiter(void *argument) { // 两人等同一个宽版本，但不消费通知令牌。
    (void)argument; uint64_t expected, current; // 所有版本数据都保留完整64位。
    if (airi_event64_snapshot(&changed, &expected) || expected != 0) return -250; // 第一份快照在主推进以前取得。
    atomic_fetch_add_explicit(&ready, 1, memory_order_release); // 仅表示已取得旧观察。
    uint64_t start = airi_ticks(); // 重查不续借200tick。
    for (;;) { // 被通知以后仍查版本，0返回不直接作完成。
        if (airi_event64_snapshot(&changed, &current)) return -251; // 失败不读取未形成的输出。
        if (current != expected) { atomic_fetch_add_explicit(&done, 1, memory_order_release); return current == 1 ? 1 : -252; } // 真正变化才完成。
        uint64_t elapsed = airi_ticks() - start; // 所有调度消耗原来的预算。
        if (elapsed >= 200 || airi_event64_wait(&changed, expected, (unsigned)(200 - elapsed))) return -253; // 到期不是伪造变化。
        if (airi_event64_snapshot(&changed, &current)) return -254; // 返回以后重新读实际值。
        if (current == expected) atomic_fetch_add_explicit(&rechecks, 1, memory_order_release); // 仅通知没有变化，应回循环。
    }
}
int main(void) { // 所有输出槽都属于当前调用栈，不与事件字段重叠。
    uint64_t value = 71; // 失败必须保持哨兵。
    require(airi_event64_snapshot(0, &value) == -22 && airi_event64_snapshot(&changed, 0) == -22 && airi_event64_snapshot(&changed, (uint64_t *)((uintptr_t)&value + 4)) == -22 && value == 71, "event64 output validation"); // NULL/未对齐不写半个结果。
    require(airi_event64_snapshot(&changed, &changed.sequence) == -22 && airi_event64_notify(&changed, 17) == -22 && airi_event64_notify(&changed, 0) == 0 && airi_event64_wait(&changed, 0, 6001) == -22, "event64 alias and budget"); // 原子字段不能充当私人普通输出。
    require(airi_event64_snapshot(&high, &value) == 0 && value == 0x8000000000000000ull && airi_event64_advance(&high) == 0, "event64 sign bit data"); // 最高位为1仍是成功数据。
    uint64_t before = airi_thread_syscalls(); // 只测旧高位快照的等待。
    require(airi_event64_wait(&high, 0x8000000000000000ull, 20) == 0, "event64 changed high snapshot"); // 当前高值已加1，用户态直接重查。
    uint64_t after = airi_thread_syscalls(); // 结束查询自身一次。
    require(after - before == 1 && airi_event64_snapshot(&high, &value) == 0 && value == 0x8000000000000001ull, "event64 fast and full output"); // 不把最高位截掉或当错误。
    require(airi_event64_snapshot(&edge, &value) == 0 && value == UINT64_MAX - 1 && airi_event64_advance(&edge) == 0 && airi_event64_snapshot(&edge, &value) == 0 && value == UINT64_MAX, "event64 maximum data"); // 全1值也能成功交付。
    require(airi_event64_advance(&edge) == -75 && airi_event64_snapshot(&edge, &value) == 0 && value == UINT64_MAX, "event64 saturation unchanged"); // 不以回绕伪装新代次。
    uint64_t start = airi_ticks(); require(airi_event64_wait(&high, 0x8000000000000001ull, 3) == -110 && airi_ticks() - start >= 3, "event64 full high timeout"); // 当前值相同则实际宽等待到期。
    long first = airi_thread_create(waiter, 0), second = airi_thread_create(waiter, 0), result; // 同一进程/宽键，两位真实worker。
    require(first > 0 && second > 0, "event64 workers"); await(&ready, 2); require(airi_sleep(3) == 0, "event64 initial registration"); // 给实际登记机会。
    require(airi_event64_notify(&changed, 1) == 1, "event64 notify without advance"); // 一位先醒，但值仍0。
    await(&rechecks, 1); require(airi_sleep(3) == 0 && atomic_load_explicit(&rechecks, memory_order_acquire) == 1, "event64 false notification rechecked"); // 仅一人无变化返回并再次等待。
    require(airi_event64_advance(&changed) == 0 && airi_sleep(3) == 0 && atomic_load_explicit(&done, memory_order_acquire) == 0, "event64 advance alone not wake"); // 已登记者不会仅因字变1自动恢复。
    require(airi_event64_notify(&changed, 16) == 2 && airi_thread_join(first, &result) == 0 && result == 1 && airi_thread_join(second, &result) == 0 && result == 1, "event64 broadcast observes advance"); // 两人重新读到1才完成。
    require(airi_event64_notify(&changed, 16) == 0 && airi_event64_snapshot(&changed, &value) == 0 && value == 1, "event64 no late credit"); // 无人通知不累积令牌。
    start = airi_ticks(); require(airi_event64_wait(&changed, value, 3) == -110 && airi_ticks() - start >= 3, "event64 fresh snapshot timeout"); // 后来新取1只等未来变化。
    airi_puts("[EVENT64_USER_OK] snapshot=full64 errors=separate advance=not_wake false_wake=rechecked broadcast=2 saturation=refused\n"); // 数值、状态与两个动作均实际观察。
    return 0; // shell收取真实结果。
}
