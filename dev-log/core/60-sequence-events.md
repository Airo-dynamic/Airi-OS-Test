# K60 · 用变化序号连接观察与等待

K59 已区分“发出等待请求”和“真正睡下”。现在要让两个执行者等待一次状态变化：通知可能先于等待，也可能只是让人醒来、状态却还没有变。给这份变化一个可读取的序号，等待者记住旧序号，再把它与等待动作连接起来。

从 K59 的同一目录继续。本节新增用户态 airi_event，底层仍使用43／44号私有futex；不增加内核系统调用或图形依赖。它提供“记录变化、等待后重查”的基础，不保存任务、通知令牌或互斥所有权。下一节才把它和锁一起用于真正的有界工作队列。

<a id="event-version-model"></a>
## 旧观察与当前变化，用同一个四字节值比较

主先读序号0，随后发布一次变化，使它成为1。此时还没有等待者，通知返回唤醒人数0；但序号已经变了。主再拿旧快照0去等待，应直接知道“需要重查”，不应该无条件睡下。若稍后重新读取1，再等待1，则是在等下一次变化，不能领取过去那次通知。

因此 event 只保存一个原子访问的32位sequence。sequence回答“与我先前看到的版本是否相同”，不回答队列是否非空，更不计数可领取的工作。返回0仅表示调用者应再看自己的条件；内核原始通知即使不改sequence，也可以让等待返回0。本章会故意这样通知两位worker，观察它们重新检查后再次等待。

先新建 airi_event.h。快照函数返回long：当前目标long为64位，能把所有0至4294967295的版本都作为非负值返回，负数才是错误。等待实参仍是uint32_t，调用者在确认快照非负以后才窄化。对象必须在任何并发访问前初始化，保持有效到最后一位使用者结束，不能重置或复制活对象。

<!-- write: labs/airios/include/airi_event.h -->
```c
#ifndef AIRI_EVENT_H // 自有私有变化通知，不是eventfd或完整pthread条件变量。
#define AIRI_EVENT_H // 包含保护。
#include <stdint.h> // 等待字保持四字节。
typedef struct { uint32_t sequence; } airi_event; // 只记录变化版本；并发期间均由原子操作访问，不复制活对象。
#define AIRI_EVENT_INITIALIZER {0} // 发布以前初始化，不能重置还有等待者的对象。
#ifdef __cplusplus // 后续C++使用相同C符号。
extern "C" { // 不依赖宿主运行库。
#endif // C编译不需要语言声明。
long airi_event_sequence(airi_event *event); // 返回0至UINT32_MAX的快照或负错误；本目标long为64位。
long airi_event_wait(airi_event *event, uint32_t expected, unsigned ticks); // 0仅表示重查，-110到期；变化先于等待也不会睡错。
long airi_event_publish(airi_event *event); // 版本加一后通知至多十六位，返回实际唤醒数；饱和-75且不回绕。
#ifdef __cplusplus // 结束函数声明语言。
} // 类型可由C++聚合初始化。
#endif // 结束语言条件。
#endif // 结束包含保护。
```

<a id="event-read-wait"></a>
## 先读版本，最后睡眠检查仍交给内核

新建 event.c。布局检查保证整个event就是对齐四字节，原子操作由当前目标直接生成，不导入libatomic。valid只检查NULL与未对齐；它不能像内核页表检查那样替任意用户指针返回EFAULT，已释放或坏映射仍会触发用户故障。这个边界和用户互斥相同。

sequence使用acquire读取。wait先检查对象和最大6000tick预算，再比较当前版本；不相同直接返回0，避免一次必然比较失败的43号请求。相同才进入airi_futex_wait，内核重新比较并登记，覆盖用户读取与实际睡眠之间发生的发布。它返回-11时，版本已不再符合旧决定；返回0时，只能确定收到通知，都统一成“重查”的0。返回前再作一次acquire读取，其他错误保留。

这里不自动循环，因为循环必须围绕调用者真正关心的条件。若库内部只等“序号不同”才返回，就会隐藏没有改变条件的通知；若把0当成已有任务，又会把一次空通知误当成功取出工作。先写到wait结束：

<!-- write: labs/airios/libc/event.c -->
```c
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
```

<a id="event-publication"></a>
## 发布只前进，不把旧版本重新写回来

publish先读一个期望版本，用release CAS把它加一。另一个发布者可能已经抢先修改；失败CAS会把新的实际值写回expected，循环用这个新值重试。不能普通load后store old+1，否则两个发布者都可能把同一旧值加一，丢掉一次变化。release与读取它的acquire连接发布前后的内存操作；业务数据若仍会被多个执行者修改，还需要自己的锁，不能只凭版本变化随意读写普通对象。

本版禁止32位回绕。expected达到UINT32_MAX时返回-75，不改版本、不通知；否则成功后通知至多十六个执行者，与当前内核全系统执行槽上限一致。通知返回0仍表示版本已经推进；若通知意外返回错误，也不能把版本改回去，因为其他执行者可能已经看到新值。

这是一份有限生命周期的教学实现。连续高频发布最终会耗尽32位范围，部署版本还需要更宽的代次或在所有使用者静止后重建对象，不能把它称为无限运行的最终通知组件。本节先实际验证饱和行为，避免靠回绕假设掩盖旧快照再次相等的问题。

<!-- append: labs/airios/libc/event.c -->
```c
long airi_event_publish(airi_event *event) { // 调用者先完成受保护状态变更，再发布版本并通知。
    if (!valid(event)) return -22; // 不允许NULL/未对齐进入CAS或内核。
    uint32_t expected = __atomic_load_n(&event->sequence, __ATOMIC_RELAXED); // 仅用于CAS尝试，尚不读取业务数据。
    do { // 多个发布者失败后沿CAS回填的新版本重试，不重复使用旧值。
        if (expected == UINT32_MAX) return -75; // -EOVERFLOW，不回绕成与旧快照相同的版本，也不伪造通知成功。
    } while (!__atomic_compare_exchange_n(&event->sequence, &expected, expected + 1, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED)); // 每次成功恰发布一次变化。
    return airi_futex_wake(&event->sequence, 16); // 当前全系统至多十六执行者；返回0也可能已推进版本，错误后不能倒退版本。
}
```

<a id="event-worker-loop"></a>
## 两位worker醒来以后仍读同一个条件

新建 events.c，changed是普通通知对象，edge单独从UINT32_MAX-1开始，只用于最后一次发布和拒绝回绕。ready与rechecks是夹具握手：前者表示worker已经取得快照，后者记录等待返回后版本仍未变的次数。握手有100tick总期限，yield让真正的其他执行者运行，不把yield次数当时间。

<!-- write: labs/airios/events.c -->
```c
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
```

每位worker先读取changed=1并发布ready，然后记录一次start。循环检查sequence是否仍等于旧1；不同才交出新版本，当前观察应为2。相同时计算elapsed，把200-elapsed交给wait；不能每次醒来又给200tick，反复无变化通知也会消耗同一预算。

wait返回0以后立即重读版本。若仍是1，增加rechecks并回循环，绝不把“被叫醒”当作状态已变。这里唯一的条件就是原子版本值，本章没有借此读取未同步的普通业务对象；下一节队列的数据条件会放在互斥保护之下。

<!-- append: labs/airios/events.c -->
```c
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
```

<a id="event-before-and-after"></a>
## 先发生的发布，以及后来才取的新快照

main先验证显式坏形状与预算。edge最后一次发布返回唤醒0，版本仍推进到UINT32_MAX；再次发布返回-75并保持原值。changed从0发布成1，同样没有等待者；随后用旧0等待，用户态读到1便返回0。两次45号围住这个调用，差值只能为1，即结束查询自身，证明没有额外43号。

然后用新快照1配零预算得到-110；再给三tick，确实进入内核、到期后返回-110。这些都没有下一次发布，不能把已有版本1当成可保存到以后领取的通知。写main的前半段：

<!-- append: labs/airios/events.c -->
```c
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
```

主创建两位worker，等ready=2再睡三tick，让两人真正登记。接着故意直接调用44号，只通知、不改changed，必须唤醒两人；两人各自看到仍为1并增加rechecks。主等到两次重查后再睡三tick，让它们重新登记，最后正式publish把1改为2并唤醒两人。join必须分别取得2，不能只看一个总成功标记。

全部收拢以后，主用新快照2再等三tick，仍应到期。这样在同一真实对象上区分了旧快照遇到变化、无变化通知、真正发布以及过去通知不作为令牌保留。追加main余下部分：

<!-- append: labs/airios/events.c -->
```c
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
```

<a id="event-build-observe"></a>
## 静态链接通知库，让内核只提供既有等待机制

另编event-lib.o，仅events.elf选择它，链接既有线程库、真实入口和完整观察main。images.S打包最终ELF，内核目录增加events名称，不复制库算法到内核。仍由用户库的CAS维护sequence，内核只验证四字节地址、比较旧值、登记和通知。

<!-- append: Makefile -->
```makefile

# 变化通知只链接到显式选择它的用户程序。
$(OUT)/user/event-lib.o: labs/airios/libc/event.c Makefile
# 不与观察程序events.o混名。
	@mkdir -p $(dir $@)
# 使用相同用户目标和传递头依赖。
	$(CC) $(USER_FLAGS) -Ilabs/airios/include -c $< -o $@
# 真实线程等待沿既有int80边界，不借宿主同步库。
$(OUT)/user/events.elf: $(USER_COMMON) $(OUT)/user/threads-lib.o $(OUT)/user/thread-entry.o $(OUT)/user/event-lib.o $(OUT)/user/events.o labs/airios/program.ld
# 独立ELF包含自有通知算法和完整观察入口。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(filter %.o,$^) -o $@
# 初始目录只借最终链接字节。
$(OUT)/labs/airios/images.o: $(OUT)/user/events.elf
# 当前配置路径交给incbin，串口主线不读取图形。
$(OUT)/labs/airios/images.o: CPPFLAGS += -DAIRI_EVENTS_ELF='"$(OUT)/user/events.elf"'
```

<!-- append: labs/airios/images.S -->
```asm
.balign 16 /* 独立变化通知观察映像。 */
.global airi_events_elf_start, airi_events_elf_end /* 目录借用实际ELF范围。 */
airi_events_elf_start: /* 从文件头开始校验。 */
.incbin AIRI_EVENTS_ELF /* 完整静态用户程序，不执行宿主程序。 */
airi_events_elf_end: /* 范围右端不含字节。 */
```

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
extern const uint8_t airi_vector_elf_start[], airi_vector_elf_end[]; // 显式偏移向量正常与坏范围观察。
extern const uint8_t airi_malloc_elf_start[], airi_malloc_elf_end[]; // 用户对象分配与页面缓存的独立ELF。
extern const uint8_t airi_mutex_races_elf_start[], airi_mutex_races_elf_end[]; // 与普通锁独立链接的真实交错观察ELF。
extern const uint8_t airi_identity_elf_start[], airi_identity_elf_end[]; // 原始fork、exec与工作槽复用中的本地TID观察。
extern const uint8_t airi_heap_threads_elf_start[], airi_heap_threads_elf_end[]; // 同一arena并发分配、跨执行者归还与快照。
extern const uint8_t airi_mutex_elf_start[], airi_mutex_elf_end[]; // 原子所有权与futex等待组合的真实用户观察。
```
<!-- after -->
```c
extern const uint8_t airi_vector_elf_start[], airi_vector_elf_end[]; // 显式偏移向量正常与坏范围观察。
extern const uint8_t airi_malloc_elf_start[], airi_malloc_elf_end[]; // 用户对象分配与页面缓存的独立ELF。
extern const uint8_t airi_mutex_races_elf_start[], airi_mutex_races_elf_end[]; // 与普通锁独立链接的真实交错观察ELF。
extern const uint8_t airi_events_elf_start[], airi_events_elf_end[]; // 用户变化通知通过同一真实futex运行。
extern const uint8_t airi_identity_elf_start[], airi_identity_elf_end[]; // 原始fork、exec与工作槽复用中的本地TID观察。
extern const uint8_t airi_heap_threads_elf_start[], airi_heap_threads_elf_end[]; // 同一arena并发分配、跨执行者归还与快照。
extern const uint8_t airi_mutex_elf_start[], airi_mutex_elf_end[]; // 原子所有权与futex等待组合的真实用户观察。
```

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    {"heap", airi_malloc_elf_start, airi_malloc_elf_end}, // 单线程用户对象与错误路径。
    {"heap-reset", airi_malloc_elf_start, airi_malloc_elf_end}, // 同一ELF新BSS和按需arena。
    {"mutex-races", airi_mutex_races_elf_start, airi_mutex_races_elf_end}, // 暂停点只存在于此观察映像。
    {"identity", airi_identity_elf_start, airi_identity_elf_end}, // 版本化运行头和原始查询对照。
    {"identity-reset", airi_identity_elf_start, airi_identity_elf_end}, // 成功exec后沿用PID并重建运行头。
    {"heap-threads", airi_heap_threads_elf_start, airi_heap_threads_elf_end}, // 显式持内部锁yield的用户观察程序。
```
<!-- after -->
```c
    {"heap", airi_malloc_elf_start, airi_malloc_elf_end}, // 单线程用户对象与错误路径。
    {"heap-reset", airi_malloc_elf_start, airi_malloc_elf_end}, // 同一ELF新BSS和按需arena。
    {"mutex-races", airi_mutex_races_elf_start, airi_mutex_races_elf_end}, // 暂停点只存在于此观察映像。
    {"events", airi_events_elf_start, airi_events_elf_end}, // 版本快照、无变化通知与广播观察。
    {"identity", airi_identity_elf_start, airi_identity_elf_end}, // 版本化运行头和原始查询对照。
    {"identity-reset", airi_identity_elf_start, airi_identity_elf_end}, // 成功exec后沿用PID并重建运行头。
    {"heap-threads", airi_heap_threads_elf_start, airi_heap_threads_elf_end}, // 显式持内部锁yield的用户观察程序。
```

自动会话在mutex-races之后执行events，再继续原来的共享分配与身份观察。六次FUTEX_WAIT均属于changed同一私有键：主前后两次真实超时，两个worker各自登记两次。前五次expected=1，最后新快照为2。零预算没有登记，旧0的等待更没有进入43号。

本命令的四次16人上限通知，实际唤醒数依次为0、0、2、2：edge最后发布、changed先发布、仅通知不改版本、最后广播。被拒绝的饱和发布不发通知。宿主以正则从内核日志提取pid、tid、address、expected，再把(pid,address)组成集合，确认六次都属于同一个私有键；不会用其他命令的等待凑数量。

<!-- replace: scripts/shell_test.py -->
<!-- before -->
```python
                    wakes=re.findall(r'\[FUTEX_WAKE\] pid=\d+ address=0x[0-9a-f]+ count=1 woken=(\d+)',section) # 实际通知数量来自内核，不从用户标记猜测。
                    assert len(waits)==(1 if stage==4 else 0) # 只有第四轮真正进入睡眠集合。
                    assert wakes==([] if stage==1 else ['1' if stage==4 else '0']) # 第一轮省略，第二/三轮空通知，第四轮真实唤醒一人。
                offset=len(text); wire.sendall(b'heap-threads\n'); text=wait('[SHELL_COMMAND_DONE] name=heap-threads code=0',offset) # 四个真实执行者共享唯一arena，观察对象交接。
                heap_threads=text[offset:] # 不用旧heap命令代替并发结果。
                assert '[MALLOC_THREADS_OK] workers=4 rounds=96 arena=262144 cross_free=4 snapshots=consistent errno=private critical_yield=1' in heap_threads # 每个工作者和主快照均由guest核对。
```
<!-- after -->
```python
                    wakes=re.findall(r'\[FUTEX_WAKE\] pid=\d+ address=0x[0-9a-f]+ count=1 woken=(\d+)',section) # 实际通知数量来自内核，不从用户标记猜测。
                    assert len(waits)==(1 if stage==4 else 0) # 只有第四轮真正进入睡眠集合。
                    assert wakes==([] if stage==1 else ['1' if stage==4 else '0']) # 第一轮省略，第二/三轮空通知，第四轮真实唤醒一人。
                offset=len(text); wire.sendall(b'events\n'); text=wait('[SHELL_COMMAND_DONE] name=events code=0',offset) # 变化序号与通知分开，旧快照、无变化通知和真实广播均运行。
                events_output=text[offset:] # 不能借其他命令的等待记录。
                assert '[EVENT_USER_OK] old_snapshot=changed timeout=pit false_wake=rechecked broadcast=2 late_wait=timeout saturation=refused' in events_output # guest已核对饱和与每位worker返回2。
                event_waits=re.findall(r'\[FUTEX_WAIT\] pid=(\d+) tid=(\d+) address=(0x[0-9a-f]+) expected=(\d+)',events_output) # 实际登记来自内核，零预算不登记。
                assert len(event_waits)==6 and len({(pid,address) for pid,tid,address,expected in event_waits})==1 # 两次主超时加两位各两次等待，同一私有键。
                assert [expected for pid,tid,address,expected in event_waits].count('1')==5 and event_waits[-1][3]=='2' # 最后新快照不会领取旧通知。
                event_wakes=re.findall(r'\[FUTEX_WAKE\] pid=\d+ address=0x[0-9a-f]+ count=16 woken=(\d+)',events_output) # 饱和对象末次发布、先发布、无变化通知、最后广播。
                assert event_wakes==['0','0','2','2'] # 拒绝回绕不发通知；两次真正广播分别唤醒两人。
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
                result.update(mutex_race_stages=4,mutex_race_actual_waits=1,mutex_race_mismatches=1,per_execution_syscall_count=True,uncontended_mutex_pairs=8,uncontended_mutex_syscalls=0,runtime_tcb_identity=True,identity_reference_queries=7,shared_malloc=True,malloc_workers=4,malloc_rounds=96,owned_mutex=True,mutex_critical_updates=64,private_futex=True,futex_registrations=8,user_heap=True,shared_executions=True,thread_commands=2,worker_capacity=8,worker_reuse_rounds=24) # 关键字把实际已验收字段加入结果字典，不替代上面的逐命令检查。
                result_path.write_text(json.dumps(result,indent=2)+'\n'); print('PASS user shell: alloc/file/async wait/other task/CQ/readback, failed exec, return to monitor') # 阶段演示实际完成。
                return result # 总入口可收集。
            finally: # 包括断言、超时和QEMU错误。
```
<!-- after -->
```python
                offset=len(text); wire.sendall(b'echo after-shell\n'); text=wait('[CONSOLE_UPDATE] view=0 commands=2 page=0',offset) # 回到内核后输入仍完整。
                assert 'after-shell' in text[offset:] # 不只是退出而丢了后续UART。
                result={'passed':True,'ring3_shell':True,'ram_file_and_device_readback':True,'other_user_during_cq_wait':True,'qmp_iops':1,'error_exec_survived':True,'returned_to_monitor':True,'orphan_rounds':24,'memory_reuse_rounds':128,'capacity_rounds':4,'capacity_pages':1024,'simd_preempt_fork_exec':True,'fs_base_preempt_fork_exec':True,'compiler_static_tls':True,'posix_errno_io_subset':True,'positioned_vectors':True} # 只发布已观察事实。
                result.update(sequence_event=True,event_waits=6,event_broadcast_workers=2,event_false_wake_rechecked=True,mutex_race_stages=4,mutex_race_actual_waits=1,mutex_race_mismatches=1,per_execution_syscall_count=True,uncontended_mutex_pairs=8,uncontended_mutex_syscalls=0,runtime_tcb_identity=True,identity_reference_queries=7,shared_malloc=True,malloc_workers=4,malloc_rounds=96,owned_mutex=True,mutex_critical_updates=64,private_futex=True,futex_registrations=8,user_heap=True,shared_executions=True,thread_commands=2,worker_capacity=8,worker_reuse_rounds=24) # 关键字把实际已验收字段加入结果字典，不替代上面的逐命令检查。
                result_path.write_text(json.dumps(result,indent=2)+'\n'); print('PASS user shell: alloc/file/async wait/other task/CQ/readback, failed exec, return to monitor') # 阶段演示实际完成。
                return result # 总入口可收集。
            finally: # 包括断言、超时和QEMU错误。
```

实现闭合，运行：

<!-- run -->
```sh
./airios build  # 链接自有事件序号库及真实线程观察
```

<!-- run -->
```sh
./airios test  # 旧快照、无变化通知、广播、到期与饱和均实际运行
```

<a id="event-results"></a>
## 序号不提供业务结果，醒来以后仍需重查

交互执行`./airios run`、`run sh`、`events`，本轮实际输出：

```text
[EVENT_USER_OK] old_snapshot=changed timeout=pit false_wake=rechecked broadcast=2 late_wait=timeout saturation=refused
[SHELL_COMMAND_DONE] name=events code=0
```

沿同一个changed地址，可以看到一次主超时、两位worker初次等待、无变化通知唤醒2、两位worker再次等待、正式发布唤醒2，以及最后主等待新版本2到期。只有正式发布修改版本；原始wake只让人恢复。0因此始终解释为“重查条件”，不能直接解释成“取得一项工作”。

这里沿用K53的进程/VA私有键，不是跨进程共享通知，也没有eventfd的文件描述符和可读计数。当前没有完整pthread条件变量的互斥释放／重新取得合同。无竞争通知仍可发44号，不把mutex的省略通知优化搬来冒充同一协议。下一节会把队列空满状态放在锁下，取得版本后再释放锁等待，并把每次返回接回同一个条件循环。
