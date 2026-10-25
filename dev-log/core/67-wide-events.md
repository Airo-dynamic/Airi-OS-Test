# K67 · 完整宽快照、错误状态与独立通知

K66使内核能等一个完整64位值。现在建立对应的用户变化序号，先解决返回值：K60把32位快照放在非负long里，负数留给错误；64位全部取值已经占满同样宽的位型，最高位为1不能再被当成负错误。状态和数据需要分别交付。

从 K66 的同一目录继续。新增独立airi_event64，保留旧32位event及课程。宽接口还把advance与notify分开：先明确推进版本，再决定通知多少人；本节实际观察“只推进”和“只通知”各自能做什么，为后面的条件与队列交接留出清楚的提交位置。

<a id="event64-result-shape"></a>
## 一个64位数据值，不能再兼作所有错误编码

0x8000000000000000和UINT64_MAX都应该是合法版本。若仍转成当前有符号long作为结果，后者会与-1这样的错误位型重合，调用者无法仅凭一个返回字区分。本接口让函数return long只保存0或负错误，完整版本写入调用者的uint64_t输出。

| 调用结果 | return状态 | 私人输出内容 |
| --- | --- | --- |
| 高位版本成功 | 0 | 0x8000000000000000 |
| 最大版本成功 | 0 | UINT64_MAX |
| 显式坏输出 | -22 | 保持调用前71 |

main的局部value是保存数据的八字节对象，&value交给snapshot。函数形参value只是一个保存该地址的指针，*value才写入main那八字节；return0沿函数返回位置独立交付，不把数据强转成状态。输出必须私人、有效、对齐且不与活event重叠，不能把&event.sequence交进去，用普通写覆盖原子字段。

新建airi_event64.h。snapshot、wait、advance、notify各有独立合同；advance只改序号，notify只要求至多count人重查。0数量为空操作，不形成内核请求，但调用者仍必须提供有效存活对象。八字节类型和新原始接口对应，不会误唤醒旧窄键。

<!-- write: labs/airios/include/airi_event64.h -->
```c
#ifndef AIRI_EVENT64_H // 宽变化序号，不将最高位值混成负错误。
#define AIRI_EVENT64_H // 包含保护。
#include <stdint.h> // 完整64位版本及指针宽度。
typedef struct { uint64_t sequence; } airi_event64; // 并发期间只用原子操作访问，存活期间不移动/重置。
#define AIRI_EVENT64_INITIALIZER {0} // 在发布给任何执行者以前构造。
#ifdef __cplusplus // C++可以使用同一形状，不依赖宿主运行库。
extern "C" { // 保持C链接符号。
#endif // C直接声明。
long airi_event64_snapshot(airi_event64 *event, uint64_t *value); // 成功0且写私人不重叠输出，失败负错误且保持输出。
long airi_event64_wait(airi_event64 *event, uint64_t expected, unsigned ticks); // 0只表示重查，-110到期，完整64位比较。
long airi_event64_advance(airi_event64 *event); // 仅推进版本，0成功，饱和-75不回绕；不通知已登记者。
long airi_event64_notify(airi_event64 *event, unsigned count); // 仅通知至多count<=16人，不改版本，返回真实人数，0数量为空操作。
#ifdef __cplusplus // 结束函数链接语言。
} // 对象布局为当前项目ABI。
#endif // 结束语言差异。
#endif // 结束包含保护。
```

<a id="event64-snapshot-wait"></a>
## 私人输出保留全值，实际睡眠仍比较旧快照

新建event64.c。布局和编译检查要求对齐八字节及直接原子操作。snapshot先验证显式形状、输出对齐和字段别名，成功以后才acquire读序号并写输出；失败没有半写，也不把最高位保留给错误。

wait保持K60分工：先验证最大6000tick预算，当前版本不等于expected就在用户态返回0；相同才进46号比较登记。原调用被通知返回0、或内核比较失效返回-11，都变成“重查”的0，返回前再acquire读一次。到期仍为-110；它不循环替业务决定条件真假，也不授予任何对象所有权。

<!-- write: labs/airios/libc/event64.c -->
```c
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
```

<a id="event64-advance-notify"></a>
## 推进版本，与让已睡下的人恢复，是两个动作

advance用release CAS加一，并发失败后从回填的实际值重试。到UINT64_MAX仍返回-75、保持原值，不回绕成旧版本。64位扩大了可表达范围，但不是数学上的无限代次，最大值仍要有明确失败行为。

advance不调用47号。已经在内核登记的执行者不会因为用户字变化就自动返回，仍需要显式通知或到期。notify反过来不改序号，可以让某人醒来却仍看到旧条件，因此调用者每次返回都要重查。它验证count<=16，0直接空操作，其余交给47号返回真实通知人数。

以后在锁下修改业务状态时，可以把推进版本和释放锁后的通知放在不同位置，但序号本身仍不是锁：不能据此任意读写会被其他执行者继续修改的普通对象。本章只观察原子版本，尚未改变旧队列的提交顺序。

<!-- append: labs/airios/libc/event64.c -->
```c
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
```

<a id="event64-worker-loop"></a>
## 三个对象，分别验证高位、上限与等待

新建events64.c。high从最高位为1开始，edge从最大值前一位开始，changed从0开始。三者职责分开，不能把极值观察产生的通知借给普通等待。ready表示两worker取得了旧0，rechecks记录无变化返回，done只在真正读到新版本时增加。

两worker用status检查snapshot成功，再读取私人expected/current。它们只等待changed从0到1，因此工作函数最后可以返回已验证的1；这不是用long重新承载任意64位版本。所有版本快照本身仍通过uint64_t输出。循环共享一次200tick起点，通知后不续借预算。

<!-- write: labs/airios/events64.c -->
```c
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
```

<a id="event64-high-and-errors"></a>
## 全1可以成功，失败仍保留调用前输出

main从私人value=71开始，验证NULL、未对齐和输出字段别名的-22及保持。notify17拒绝，notify0不请求内核，坏等待预算也先拒绝。

high的snapshot必须状态0、输出0x8000000000000000，再advance到0x8000000000000001。旧高值等待在用户态发现不同，围住它的两次45号差值为1，只有结束查询自身。edge最后一次推进到UINT64_MAX，snapshot仍状态0并完整写出全1；再advance才-75，序号保持，后续snapshot依然成功。

high给当前完整值三tick等待，必须真实46号到期，内核日志的期望为9223372036854775809。最高位不只在一个用户比较中存在，也确实经过宽等待边界。

<!-- append: labs/airios/events64.c -->
```c
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
```

<a id="event64-two-actions-observation"></a>
## 一人被叫醒重查，两人仍需真正的通知

主等ready=2并让两人实际登记，先notify1而不advance。必须恰唤醒一人，该worker读到仍是0，增加rechecks再等待；主等到这次重查以后再让三tick，另一位没有被通知，所以rechecks仍只有1。

主接着只advance changed到1，再睡三tick，done仍须为0。此时两人已经登记，内核没有按值轮询把它们自动恢复。最后notify16才实际唤醒两人，各自读取1、完成并join返回1。通知返回人数与版本是否改变是两项独立结果。

收拢以后notify16无人0，不积累信用；主重新snapshot取得1，再等这个新值三tick仍到期。过去那次推进和无人通知不能变成以后领取的令牌。追加main余下部分：

<!-- append: labs/airios/events64.c -->
```c
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
```

<a id="event64-build-evidence"></a>
## 新接口独立选择，旧32位章节与程序保持

event64-lib.o编译宽通知库，events64.elf只链接已有原始线程入口和完整观察main。旧event.c不移动、不替换，旧队列仍按原接口运行；本阶段提供可选择的宽机制，不声称已完成全部调用方迁移。打包实际ELF并注册events64。

<!-- append: Makefile -->
```makefile

# 宽事件是独立源码接口，旧32位event继续保留。
$(OUT)/user/event64-lib.o: labs/airios/libc/event64.c Makefile
# 与观察main的events64.o分别命名。
	@mkdir -p $(dir $@)
# 同一用户目标，完整宽原子无需宿主库。
	$(CC) $(USER_FLAGS) -Ilabs/airios/include -c $< -o $@
# 序号与通知分离观察只选择原始线程运行库。
$(OUT)/user/events64.elf: $(USER_COMMON) $(OUT)/user/threads-lib.o $(OUT)/user/thread-entry.o $(OUT)/user/event64-lib.o $(OUT)/user/events64.o labs/airios/program.ld
# 数据与错误返回均在真实guest中验证。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(filter %.o,$^) -o $@
# 初始目录只读实际完整ELF。
$(OUT)/labs/airios/images.o: $(OUT)/user/events64.elf
# 路径仅供incbin消费。
$(OUT)/labs/airios/images.o: CPPFLAGS += -DAIRI_EVENTS64_ELF='"$(OUT)/user/events64.elf"'
```

<!-- append: labs/airios/images.S -->
```asm
.balign 16 /* 完整64位序号接口的实际用户观察。 */
.global airi_events64_elf_start, airi_events64_elf_end /* 目录借用只读文件范围。 */
airi_events64_elf_start: /* 从静态ELF头开始。 */
.incbin AIRI_EVENTS64_ELF /* 不以宿主计算结果替代guest。 */
airi_events64_elf_end: /* 右端不含字节。 */
```

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
extern const uint8_t airi_join_elf_start[], airi_join_elf_end[]; // 对等worker收取与有界环检测观察。
extern const uint8_t airi_pthread_mutex_elf_start[], airi_pthread_mutex_elf_end[]; // 默认类型与对象生命周期的用户观察。
extern const uint8_t airi_futex64_elf_start[], airi_futex64_elf_end[]; // 同地址宽度隔离和完整64位比较。
extern const uint8_t airi_identity_elf_start[], airi_identity_elf_end[]; // 原始fork、exec与工作槽复用中的本地TID观察。
extern const uint8_t airi_heap_threads_elf_start[], airi_heap_threads_elf_end[]; // 同一arena并发分配、跨执行者归还与快照。
extern const uint8_t airi_mutex_elf_start[], airi_mutex_elf_end[]; // 原子所有权与futex等待组合的真实用户观察。
```
<!-- after -->
```c
extern const uint8_t airi_join_elf_start[], airi_join_elf_end[]; // 对等worker收取与有界环检测观察。
extern const uint8_t airi_pthread_mutex_elf_start[], airi_pthread_mutex_elf_end[]; // 默认类型与对象生命周期的用户观察。
extern const uint8_t airi_futex64_elf_start[], airi_futex64_elf_end[]; // 同地址宽度隔离和完整64位比较。
extern const uint8_t airi_events64_elf_start[], airi_events64_elf_end[]; // 完整数值快照与独立推进/通知。
extern const uint8_t airi_identity_elf_start[], airi_identity_elf_end[]; // 原始fork、exec与工作槽复用中的本地TID观察。
extern const uint8_t airi_heap_threads_elf_start[], airi_heap_threads_elf_end[]; // 同一arena并发分配、跨执行者归还与快照。
extern const uint8_t airi_mutex_elf_start[], airi_mutex_elf_end[]; // 原子所有权与futex等待组合的真实用户观察。
```

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    {"join", airi_join_elf_start, airi_join_elf_end}, // 同组认领、坏输出及循环等待拒绝。
    {"pthread-mutex", airi_pthread_mutex_elf_start, airi_pthread_mutex_elf_end}, // 公开互斥接口与真实内部等待重试。
    {"futex64", airi_futex64_elf_start, airi_futex64_elf_end}, // 宽键与原四字节键共存。
    {"identity", airi_identity_elf_start, airi_identity_elf_end}, // 版本化运行头和原始查询对照。
    {"identity-reset", airi_identity_elf_start, airi_identity_elf_end}, // 成功exec后沿用PID并重建运行头。
    {"heap-threads", airi_heap_threads_elf_start, airi_heap_threads_elf_end}, // 显式持内部锁yield的用户观察程序。
```
<!-- after -->
```c
    {"join", airi_join_elf_start, airi_join_elf_end}, // 同组认领、坏输出及循环等待拒绝。
    {"pthread-mutex", airi_pthread_mutex_elf_start, airi_pthread_mutex_elf_end}, // 公开互斥接口与真实内部等待重试。
    {"futex64", airi_futex64_elf_start, airi_futex64_elf_end}, // 宽键与原四字节键共存。
    {"events64", airi_events64_elf_start, airi_events64_elf_end}, // 高位数据、输出错误和宽通知观察。
    {"identity", airi_identity_elf_start, airi_identity_elf_end}, // 版本化运行头和原始查询对照。
    {"identity-reset", airi_identity_elf_start, airi_identity_elf_end}, // 成功exec后沿用PID并重建运行头。
    {"heap-threads", airi_heap_threads_elf_start, airi_heap_threads_elf_end}, // 显式持内部锁yield的用户观察程序。
```

宿主核对五次FUTEX64_WAIT：一次高值到期、changed初始两人、无变化返回的一人再登记，以及最后新值1到期。期望分别为高完整值一次、0三次、末尾1一次。

只有显式notify产生三条WAKE64，(count,woken)依次(1,1)、(16,2)、(16,0)；advance及notify0不产生47号。guest还在仅推进后检查done0，不能只凭最终两人完成来推断动作已经分离。

<!-- replace: scripts/shell_test.py -->
<!-- before -->
```python
                assert len(wide_waits)==3 and len(narrow_waits)==2 and len({address for address,expected in wide_waits+narrow_waits})==1 # 真正相同VA，不是用不同变量逃开隔离要求。
                assert all(expected==str(0x1122334400000001) for address,expected in wide_waits) and all(expected=='1' for address,expected in narrow_waits) # 高32位保留，窄键只看自己的四字节。
                assert re.findall(r'\[FUTEX64_WAKE\].*count=16 woken=(\d+)',wide_output)==['0','1','1','0'] and re.findall(r'\[FUTEX_WAKE\].*count=16 woken=(\d+)',wide_output)==['1','1'] # 两种先后顺序都各唤醒自己一人。
                offset=len(text); wire.sendall(b'heap-threads\n'); text=wait('[SHELL_COMMAND_DONE] name=heap-threads code=0',offset) # 四个真实执行者共享唯一arena，观察对象交接。
                heap_threads=text[offset:] # 不用旧heap命令代替并发结果。
                assert '[MALLOC_THREADS_OK] workers=4 rounds=96 arena=262144 cross_free=4 snapshots=consistent errno=private critical_yield=1' in heap_threads # 每个工作者和主快照均由guest核对。
```
<!-- after -->
```python
                assert len(wide_waits)==3 and len(narrow_waits)==2 and len({address for address,expected in wide_waits+narrow_waits})==1 # 真正相同VA，不是用不同变量逃开隔离要求。
                assert all(expected==str(0x1122334400000001) for address,expected in wide_waits) and all(expected=='1' for address,expected in narrow_waits) # 高32位保留，窄键只看自己的四字节。
                assert re.findall(r'\[FUTEX64_WAKE\].*count=16 woken=(\d+)',wide_output)==['0','1','1','0'] and re.findall(r'\[FUTEX_WAKE\].*count=16 woken=(\d+)',wide_output)==['1','1'] # 两种先后顺序都各唤醒自己一人。
                offset=len(text); wire.sendall(b'events64\n'); text=wait('[SHELL_COMMAND_DONE] name=events64 code=0',offset) # 完整uint64数据不与负错误混合，推进与通知单独观察。
                wide_events=text[offset:] # 不借前一个宽futex命令的记录。
                assert '[EVENT64_USER_OK] snapshot=full64 errors=separate advance=not_wake false_wake=rechecked broadcast=2 saturation=refused' in wide_events # guest已经检查全1输出与保持、无变化返回、仅推进不唤醒。
                samples=re.findall(r'\[FUTEX64_WAIT\] pid=\d+ tid=\d+ address=(0x[0-9a-f]+) expected=(\d+)',wide_events) # 高值一次、普通键初始两次/重查一次/迟到一次。
                assert len(samples)==5 and [expected for address,expected in samples].count(str(0x8000000000000001))==1 and [expected for address,expected in samples].count('0')==3 and samples[-1][1]=='1' # 不是最高位被窄化后的假结果。
                assert re.findall(r'\[FUTEX64_WAKE\].*count=(\d+) woken=(\d+)',wide_events)==[('1','1'),('16','2'),('16','0')] # 推进无47号，只有显式通知进入内核。
                offset=len(text); wire.sendall(b'heap-threads\n'); text=wait('[SHELL_COMMAND_DONE] name=heap-threads code=0',offset) # 四个真实执行者共享唯一arena，观察对象交接。
                heap_threads=text[offset:] # 不用旧heap命令代替并发结果。
                assert '[MALLOC_THREADS_OK] workers=4 rounds=96 arena=262144 cross_free=4 snapshots=consistent errno=private critical_yield=1' in heap_threads # 每个工作者和主快照均由guest核对。
```

<!-- replace: scripts/shell_test.py -->
<!-- before -->
```python
                offset=len(text); wire.sendall(b'echo after-shell\n'); text=wait('[CONSOLE_UPDATE] view=0 commands=2 page=0',offset) # 回到内核后输入仍完整。
                assert 'after-shell' in text[offset:] # 不只是退出而丢了后续UART。
                result={'passed':True,'ring3_shell':True,'ram_file_and_device_readback':True,'other_user_during_cq_wait':True,'qmp_iops':1,'error_exec_survived':True,'returned_to_monitor':True,'orphan_rounds':24,'memory_reuse_rounds':128,'capacity_rounds':4,'capacity_pages':1024,'simd_preempt_fork_exec':True,'fs_base_preempt_fork_exec':True,'compiler_static_tls':True,'posix_errno_io_subset':True,'positioned_vectors':True} # 只发布已观察事实。
                result.update(private_futex64=True,futex_width_isolation_rounds=2,pthread_mutex_source=True,pthread_mutex_internal_timeout_retried=True,peer_worker_join=True,join_claim_unique=True,join_cycle_rejected=True,pthread_source_subset=True,pthread_workers=36,pthread_packets_reclaimed=True,bounded_queue=True,queue_capacity=4,queue_jobs=48,queue_close_drains=True,sequence_event=True,event_waits=6,event_broadcast_workers=2,event_false_wake_rechecked=True,mutex_race_stages=4,mutex_race_actual_waits=1,mutex_race_mismatches=1,per_execution_syscall_count=True,uncontended_mutex_pairs=8,uncontended_mutex_syscalls=0,runtime_tcb_identity=True,identity_reference_queries=7,shared_malloc=True,malloc_workers=4,malloc_rounds=96,owned_mutex=True,mutex_critical_updates=64,private_futex=True,futex_registrations=8,user_heap=True,shared_executions=True,thread_commands=2,worker_capacity=8,worker_reuse_rounds=24) # 关键字把实际已验收字段加入结果字典，不替代上面的逐命令检查。
                result_path.write_text(json.dumps(result,indent=2)+'\n'); print('PASS user shell: alloc/file/async wait/other task/CQ/readback, failed exec, return to monitor') # 阶段演示实际完成。
                return result # 总入口可收集。
            finally: # 包括断言、超时和QEMU错误。
```
<!-- after -->
```python
                offset=len(text); wire.sendall(b'echo after-shell\n'); text=wait('[CONSOLE_UPDATE] view=0 commands=2 page=0',offset) # 回到内核后输入仍完整。
                assert 'after-shell' in text[offset:] # 不只是退出而丢了后续UART。
                result={'passed':True,'ring3_shell':True,'ram_file_and_device_readback':True,'other_user_during_cq_wait':True,'qmp_iops':1,'error_exec_survived':True,'returned_to_monitor':True,'orphan_rounds':24,'memory_reuse_rounds':128,'capacity_rounds':4,'capacity_pages':1024,'simd_preempt_fork_exec':True,'fs_base_preempt_fork_exec':True,'compiler_static_tls':True,'posix_errno_io_subset':True,'positioned_vectors':True} # 只发布已观察事实。
                result.update(wide_event64=True,event64_value_error_separate=True,event64_advance_notify_separate=True,private_futex64=True,futex_width_isolation_rounds=2,pthread_mutex_source=True,pthread_mutex_internal_timeout_retried=True,peer_worker_join=True,join_claim_unique=True,join_cycle_rejected=True,pthread_source_subset=True,pthread_workers=36,pthread_packets_reclaimed=True,bounded_queue=True,queue_capacity=4,queue_jobs=48,queue_close_drains=True,sequence_event=True,event_waits=6,event_broadcast_workers=2,event_false_wake_rechecked=True,mutex_race_stages=4,mutex_race_actual_waits=1,mutex_race_mismatches=1,per_execution_syscall_count=True,uncontended_mutex_pairs=8,uncontended_mutex_syscalls=0,runtime_tcb_identity=True,identity_reference_queries=7,shared_malloc=True,malloc_workers=4,malloc_rounds=96,owned_mutex=True,mutex_critical_updates=64,private_futex=True,futex_registrations=8,user_heap=True,shared_executions=True,thread_commands=2,worker_capacity=8,worker_reuse_rounds=24) # 关键字把实际已验收字段加入结果字典，不替代上面的逐命令检查。
                result_path.write_text(json.dumps(result,indent=2)+'\n'); print('PASS user shell: alloc/file/async wait/other task/CQ/readback, failed exec, return to monitor') # 阶段演示实际完成。
                return result # 总入口可收集。
            finally: # 包括断言、超时和QEMU错误。
```

完整实现闭合，运行：

<!-- run -->
```sh
./airios build  # 静态链接完整宽快照与独立推进/通知接口
```

<!-- run -->
```sh
./airios test  # 高位与全1数据、错误保持、仅推进和仅通知分别验证
```

<a id="event64-results"></a>
## 状态、数据与通知人数，各自保留含义

交互执行`./airios run`、`run sh`、`events64`，本轮实际输出：

```text
[EVENT64_USER_OK] snapshot=full64 errors=separate advance=not_wake false_wake=rechecked broadcast=2 saturation=refused
[SHELL_COMMAND_DONE] name=events64 code=0
```

快照的0表示成功，完整uint64_t另存；wait的0表示重查；advance的0表示版本已推进；notify的非负值才是实际通知人数。同样一个0出现在不同合同中，不能用一个“成功”词把这些状态混成业务结果。

宽版本、私有键、原子操作和生命周期仍按当前固定单CPU实现，不能据此宣称SMP。旧32位event与队列暂未升级，条件变量的互斥释放／重新取得和使用者引用也尚待接入；后续在这些明确边界上继续开发。
