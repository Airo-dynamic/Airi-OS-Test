#include <airi_threads.h> // 同时调用显式四字节与八字节接口。
#include <airi_libc.h> // 真实PIT、串口及主动交接。
#include <stdatomic.h> // 夹具就绪和已返回位图分别原子发布。
static _Alignas(8) uint64_t word = 1; // _Alignas明确要求八字节边界，两个等待宽度故意用同一个VA。
static const uint64_t marker = 0x1122334400000001ull; // 只改变高32位，低四字节仍为1。
static _Atomic unsigned ready, returned; // 位0代表窄等待者，位1代表宽等待者。
_Static_assert(__atomic_always_lock_free(8, 0), "probe requires native eight-byte atomics"); // 不把用户发布偷偷交给缺失libatomic。
static void require(int condition, const char *message) { // 错误结束本真实用户进程组。
    if (!condition) { airi_puts(message); airi_puts("\n"); airi_exit(91); } // 不继续输出成功。
}
static long raw(long number, uintptr_t address, uint64_t second, uint64_t third) { // 绕开unsigned包装，检查内核完整宽参数。
    __asm__ volatile("int $0x80" : "+a"(number) : "D"(address), "S"(second), "d"(third) : "memory", "cc"); // 保留64位数量与预算。
    return number; // 原始负错误不写errno。
}
static long waiter(void *argument) { // 同地址分别登记两种宽度，均不修改word。
    unsigned wide = (unsigned)(uintptr_t)argument; // 0为32位，1为64位。
    atomic_fetch_add_explicit(&ready, 1, memory_order_release); // 只表示已经开始，主随后让出三tick供真正登记。
    long result = wide ? airi_futex_wait64(&word, marker, 200) : airi_futex_wait((uint32_t *)&word, 1, 200); // 仅传窄指针数值，用户不以不兼容类型解引用word。
    if (result) return result; // 超时或错误不能记作通知完成。
    atomic_fetch_or_explicit(&returned, 1u << wide, memory_order_release); // 标明实际返回的是哪位。
    return 0; // 同一个原等待调用正常完成。
}
int main(void) { // 先检查完整数值，再观察同VA不同宽度的键。
    require(airi_futex_wait64((uint64_t *)((uintptr_t)&word + 4), 1, 1) == -22 && airi_futex_wake64((uint64_t *)((uintptr_t)&word + 4), 1) == -22, "futex64 alignment"); // 四字节对齐不足以满足宽接口。
    require(airi_futex_wait64((uint64_t *)(uintptr_t)0xffff800000000000ull, 1, 1) == -14 && airi_futex_wake64((uint64_t *)(uintptr_t)0xffff800000000000ull, 1) == -14, "futex64 invalid address"); // 内核拒绝高地址，不直接解引用。
    require(raw(46, (uintptr_t)&word, 1, 1ull << 32) == -22 && raw(47, (uintptr_t)&word, 1ull << 32, 0) == -22, "futex64 full budget and count"); // 不把过宽参数截成0。
    __atomic_store_n(&word, marker, __ATOMIC_RELEASE); // 发布前1，发布后只高位变化，所有后续worker只读。
    require(airi_futex_wake64(&word, 16) == 0 && airi_futex_wait64(&word, 1, 20) == -11, "futex64 prewake high comparison"); // 先通知0人，旧低值1也不能误睡。
    require(airi_futex_wait64(&word, UINT64_MAX, 1) == -11, "futex64 full expected range"); // 高位全1是合法期望，比较不符而不是窄值EINVAL。
    require(airi_futex_wait((uint32_t *)&word, 1, 0) == -110 && airi_futex_wait64(&word, marker, 0) == -110, "futex64 zero budget"); // 两种宽度相符而无预算都不到队列。
    uint64_t start = airi_ticks(); // 宽等待也由同一PIT截止结束。
    require(airi_futex_wait64(&word, marker, 3) == -110 && airi_ticks() - start >= 3, "futex64 real timeout"); // 必须实际睡过三tick。
    for (unsigned round = 0; round < 2; ++round) { // 一轮先窄后宽，另一轮相反，双向检查不混唤醒。
        atomic_store_explicit(&ready, 0, memory_order_relaxed); atomic_store_explicit(&returned, 0, memory_order_relaxed); // 前轮已join，才清夹具状态。
        long narrow = airi_thread_create(waiter, (void *)(uintptr_t)0), wide = airi_thread_create(waiter, (void *)(uintptr_t)1), result; // 两人共享同一进程与word地址。
        require(narrow > 0 && wide > 0, "futex64 typed waiters"); // 不能用单线程冒充隔离。
        start = airi_ticks(); // 有限准备期限。
        while (atomic_load_explicit(&ready, memory_order_acquire) != 2) { require(airi_ticks() - start < 100, "futex64 waiters ready"); airi_yield(); } // 真正让两worker运行。
        require(airi_sleep(3) == 0, "futex64 both registered"); // 后面内核日志分别核对实际登记。
        long first = round ? airi_futex_wake64(&word, 16) : airi_futex_wake((uint32_t *)&word, 16); // 第一次只选择一个宽度。
        require(first == 1 && airi_sleep(3) == 0 && atomic_load_explicit(&returned, memory_order_acquire) == (round ? 2u : 1u), "futex64 first type isolated"); // 另一宽度必须仍未返回。
        long second = round ? airi_futex_wake((uint32_t *)&word, 16) : airi_futex_wake64(&word, 16); // 再通知另一种键。
        require(second == 1 && airi_thread_join(narrow, &result) == 0 && result == 0 && airi_thread_join(wide, &result) == 0 && result == 0, "futex64 second type isolated"); // 两个原调用分别完成。
        require(atomic_load_explicit(&returned, memory_order_acquire) == 3 && __atomic_load_n(&word, __ATOMIC_ACQUIRE) == marker, "futex64 word unchanged"); // 通知不改原子字。
    }
    require(airi_futex_wake64(&word, 16) == 0, "futex64 no leftover waiter"); // 四个worker都已join，不能留幽灵登记。
    airi_puts("[FUTEX64_USER_OK] high_bits=compared timeout=pit widths=isolated rounds=2 word=unchanged\n"); // 参数、值、超时及双向键隔离都完成以后才输出。
    return 0; // shell收取真实成功。
}
