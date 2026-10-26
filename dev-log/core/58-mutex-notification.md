# K58 · 无竞争时省去解锁通知

K57 对同一局部锁做八对加锁、解锁，实测八次44号通知。每次释放时都没有别人访问这把锁，通知只能返回零。现在保留 TID 所有权和错误检查，给锁增加慢路径参与者计数：没有人准备等待时，释放只改用户内存；有人竞争时，仍执行通知。

从 K57 的同一目录继续。identity 的测量位置、45号计数和内核 futex 均保持，修改用户锁协议及相关观察。图形和作者工具仍不是前置。锁对象从4字节改为8字节，所有选用这个头的静态用户程序一起重编；不混用旧布局的二进制。

<a id="mutex-participant-window"></a>
## 为什么不能只在睡下以后加一个人数

主执行者 A 持有 guard，worker B 的第一次 CAS 失败并读到 A 的 TID。此时 B 还没有进入内核。K57 的 A 总会发通知；通知若早于 B 的 WAIT，内核比较会拒绝已经变化的旧值，所以 B 不会因为这次空通知而睡错。现在 A 想省去请求，需要在用户内存里知道是否有人还处于竞争过程；只看刚释放为0的 owner，无法判断别人是否已经睡着。

因此 contenders 记录的是“已进入慢路径、还没有离开的参与者”，包括尚未发 WAIT、正在等待、已经被唤醒而准备重试的人。B 在可能睡眠以前先加一，再重新 CAS；若 A 已释放，B 可以直接取得，连一次必然失败的 WAIT 都省去。若仍被占用，B 才把这次重新观察的 owner 交给 WAIT。取得锁、到期或错误时都减一，循环重试不重复增加。计数为正允许一次空通知，计数为零才允许省略通知；内核比较继续负责实际睡眠前的最后检查。

保留 owner 的0／非零TID含义，让错误所有者仍能被拒绝。增加的第二个四字节字段不传给内核，WAIT 的私有键仍为同进程中的 owner 地址。先更新 airi_mutex.h 的类型声明和初始化；字段在发布后都只能通过原子操作访问，不直接读写正在使用的锁，也不复制它。

<!-- replace: labs/airios/include/airi_mutex.h -->
<!-- before -->
```c
#ifndef AIRI_MUTEX_H // 有界私有互斥接口，不使用宿主pthread类型。
#define AIRI_MUTEX_H // 防止重复声明。
#include <stdint.h> // 锁字与TID同为四字节，0保留为空闲。
typedef struct { uint32_t owner; } airi_mutex; // 并发访问只经过运行库原子操作，不直接读写owner。
#define AIRI_MUTEX_INITIALIZER {0} // 在发布给其他执行者以前静态或局部初始化。
#ifdef __cplusplus // C++可以包含同一声明，不在头内暴露_Atomic语法。
extern "C" { // 调用已链接的C实现，保持符号名。
#endif // C编译时跳过链接语言范围。
```
<!-- after -->
```c
#ifndef AIRI_MUTEX_H // 有界私有互斥接口，不使用宿主pthread类型。
#define AIRI_MUTEX_H // 防止重复声明。
#include <stdint.h> // 锁字与TID同为四字节，0保留为空闲。
typedef struct { uint32_t owner, contenders; } airi_mutex; // owner为持有TID，contenders为慢路径参与者数；并发访问均须原子操作。
#define AIRI_MUTEX_INITIALIZER {0, 0} // 发布以前同时清空所有权与参与者数，不覆盖正在使用的锁。
#ifdef __cplusplus // C++可以包含同一声明，不在头内暴露_Atomic语法。
extern "C" { // 调用已链接的C实现，保持符号名。
#endif // C编译时跳过链接语言范围。
```

<a id="mutex-sc-order"></a>
## 两个字段需要同一套可推理的顺序

K54 的 acquire/release 已通过 owner 交接普通数据。现在 A 准备根据 contenders 决定是否通知，B 则先登记，再复查 owner 决定能否睡眠。需要排除的是双方各自漏看：A 把 B 当作尚未登记而省略通知，B 又把锁当作仍被占用而等待。两次判断分属不同原子对象，只说每个字段的读写不可分割，还没有建立它们之间的先后。

本节把相关 CAS、加减和查询统一为 __ATOMIC_SEQ_CST，简称 SC，即顺序一致。编译器提供的这种原子顺序，使这些 SC 操作能放进同一总顺序，同时遵守每条执行链自己的先后。成功 CAS 仍包含此前 acquire/release 所需的数据发布关系；失败 CAS 也参与这里的顺序。内建接口和顺序的含义见 [GCC 原子操作文档](https://gcc.gnu.org/onlinedocs/gcc/_005f_005fatomic-Builtins.html)，本节的等待协议则由下面的具体顺序推导。

沿同一把 guard，先固定证明里四个符号的实际动作：

| 执行者 | 符号 | 这一次SC操作 |
| --- | --- | --- |
| 竞争者B | X | 给 contenders 加一，登记自己仍在慢路径 |
| 竞争者B | Y | 登记后再次 CAS owner；这里分析它仍失败的分支 |
| 持有者A | A1 | 成功把自己的 owner 从 TID 改为0，释放本次所有权 |
| 持有者A | A2 | 在释放之后读取 contenders，决定是否发通知 |

B 自己先做 X 再做 Y，A 自己先做 A1 再做 A2。先看 Y 仍观察到 A 这次尚未释放的所有权、且 B 一直留在慢路径的情况：Y 必须排在这次 A1 之前，于是同一总顺序包含 X、Y、A1、A2。A2 到来时 B 的登记尚未撤销，不能读零并把它当成尚未参加。其他人的加入和退出可以改变总数，但每人只撤销自己的一次登记，不会替仍在等待的 B 减掉那一份。

再看 A2 读到零、B 之后才登记的情况，顺序是 A1、A2、X、Y。省去通知时 B 尚未参加，所以 B 的登记后复查 Y 不能省略：没有新持有者便直接取得；已有别人取得，则失败会给出新持有者，后者释放时还要按同一协议检查参与数。这里用登记后的 Y 重新争取空闲锁，不带第一次失败的旧 observed 直接等待。若某人此前已取得、到期或出错而撤销登记，它也不再是这次通知必须照顾的竞争者。

先修改 mutex.c 的布局检查和 acquire，保留四字节原子是否能直接生成目标指令的检查。trylock 仍只试一次，TID 来自 K56 的有效运行头；本节没有把它改回每次查询内核。

<!-- replace: labs/airios/libc/mutex.c -->
<!-- before -->
```c
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
```
<!-- after -->
```c
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
```

<a id="mutex-participant-lifetime"></a>
## 先登记和复查，再把剩余预算交给WAIT

lock 的最前面保持原路径：检查对象形状和预算，一次 CAS 成功就返回0，既不读时钟也不改 contenders。失败且是自己持有返回-35；失败且零预算返回-110；这几种都尚未登记，所以不能减人数。只有确实需要等待时才读取开始 tick、检查加法并固定 deadline。

接着加一登记，进入统一循环。循环第一件事再次 CAS，覆盖上一节的 Y；成功赋 result=0 并离开。仍占用时重读当前 tick，达到固定截止就赋-110离开，否则用 deadline-now 作为本次 WAIT 的剩余预算。WAIT 返回0只表示通知，-11表示比较时 owner 已变，两者都回到循环重新争取，不重置截止，也不再增加人数。

WAIT 自己还会比较 owner 和本次 observed。如果持有者在复查以后、进入内核以前释放，通知可以恰好找不到已登记到内核的等待者，但值比较会拒绝旧值，让调用者回去重试。如果原持有者又取得同一把锁，之后释放仍会看见这个尚未撤销的参与者并通知。用户计数和内核比较各自补上不同位置的窗口。

登记后的所有 break 最终汇入一次 fetch_sub，再返回 result。取得锁后撤销的是“还在竞争”的资格，owner 仍是当前TID；不是释放锁。等待超时后也必须减掉，否则以后每次无竞争释放都会误发通知。当前固定执行者总数最多16，同一个调用只增加一次，因此不会把32位人数加到溢出；这项界限依赖当前有界线程模型。

unlock 先以 SC CAS 确认自身并发布 owner=0，再 SC 读取 contenders。为0直接返回；非零调用原44号最多通知一人。比较失败仍为-1，不修改 owner 或人数。通知意外失败时 owner 已释放，继续返回错误，不把旧TID写回来。

修改余下窗口，从原 lock 的 deadline 附近一直到 unlock 结束。保留已有的首次 CAS 和参数分支：

<!-- replace: labs/airios/libc/mutex.c -->
<!-- before -->
```c
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
```
<!-- after -->
```c
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
```

这套顺序没有引入公平排队：新到者仍可能抢在刚醒的人之前取得锁。它也不增加 robust 恢复或优先级继承。当前内核 WAIT 的比较与登记依赖单 CPU、不可抢占内核；用户字段采用 SC 并不自动让内核调度、等待队列和设备都支持 SMP。

<a id="mutex-withdraw-observation"></a>
## 到期的人撤销自己，已经睡着的人继续存在

mutex-probe 先保持主执行者持有 guard，让两个 change 工作者都无法取得。三tick后检查 contenders=2，确认它们仍在慢路径；诊断本身也用原子 load，不给并发字段增加普通读取。

接着创建第三个 foreign 工作者。主执行者仍不释放，所以它的三tick等待必须到期；join 取得-110之后，再读人数应回到2。变化的是第三人的登记，原来两人的参与不能一起被清掉。

主随后释放，让两个 change 轮流完成64次 counter/checksum 更新。它们持锁时主动 yield，另一方仍应实际进入 WAIT；全部 join 后 contenders=0，再取得锁检查数据，最后释放便可省去通知。

fork 出的子进程中，abandon 工作者取得锁后不解锁就返回。该子进程的主执行者 join 只收拢工作者，不会清 owner；随后它等到-110，自己的登记要减回0，但死亡TID仍留在 owner，unlock 仍返回-1。人数已归零说明没有慢路径参与者，不能据此推断所有权空闲。

子进程退出后，原进程 waitpid 收到41，再取得自己的 guard。这里保留的是 fork 后另一份私有内存状态，不是把子进程里死亡持有者的锁恢复了。

把这些断言加入现有 mutex-probe，并在全部检查之后增加参与者标记：

<!-- replace: labs/airios/mutex-probe.c -->
<!-- before -->
```c
        require(airi_ticks() - start < 100, "contender deadline"); airi_yield(); // 准备阶段不伪装为锁实现。
    }
    require(airi_sleep(3) == 0, "contender registration"); // 后面的FUTEX_WAIT日志核对真实登记。
    long tester = airi_thread_create(foreign, 0), result; // 在仍持有guard时进行非法释放和超时观察。
    require(tester > 0 && airi_thread_join(tester, &result) == 0 && result == -110, "foreign owner and timeout"); // 主join期间仍持锁，第三人必须到期。
    require(counter == 0 && checksum == 0x5a5a && airi_mutex_unlock(&guard) == 0, "publish initial release"); // 两位工作者此前都没有进入临界区。
    require(airi_thread_join(first, &result) == 0 && result == 32, "join first"); // 主执行者等真实结果，不读取运行中的普通数据。
    require(airi_thread_join(second, &result) == 0 && result == 32, "join second"); // 两人的临界区现已全部结束。
    require(airi_mutex_trylock(&guard) == 0 && counter == 64 && checksum == (64 ^ 0x5a5a), "protected ordinary data"); // 加锁后核对最终状态。
    require(airi_mutex_unlock(&guard) == 0, "final unlock"); // 不把持锁对象复制到下面的fork。
    long child = airi_fork(); require(child >= 0, "owner death fork"); // 本时刻没有worker且锁空闲。
```
<!-- after -->
```c
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
```

<!-- replace: labs/airios/mutex-probe.c -->
<!-- before -->
```c
        long owner = airi_thread_create(abandon, 0); // 新TID不会在本次内核启动内复用。
        require(owner > 0 && airi_thread_join(owner, &result) == 0 && result == 9, "abandoned owner joined"); // 执行者已收拢，锁字仍保存它的旧身份。
        require(airi_mutex_lock(&guard, 3) == -110 && airi_mutex_unlock(&guard) == -1, "nonrobust owner remains"); // 不假冒EOWNERDEAD恢复或强制解锁。
        airi_exit(41); // 用进程重启边界结束这份不可恢复的观察状态。
    }
    int status; // 孩子退出结果和worker的long返回值分开。
    require(airi_waitpid(child, &status) == child && status == 41, "nonrobust child observed"); // 父从另一页表取得进程状态。
    require(airi_mutex_lock(&guard, 0) == 0 && airi_mutex_unlock(&guard) == 0, "parent mutex unchanged"); // 子的遗留锁不污染父进程。
    airi_puts("[MUTEX_USER_OK] counter=64 critical_yield=1 foreign=denied self=deadlock timeout=pit owner_death=nonrobust\n"); // 所有正常、错误和寿命场景完成后才发布。
    return 0; // shell取得当前命令的真实成功结果。
}
```
<!-- after -->
```c
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
```

<a id="mutex-same-measurement"></a>
## 原来的八对操作，只剩测量结束查询

identity 的局部 own 不会有竞争者，因此 lock 第一次 CAS 成功，不读取时钟；unlock 发布空闲并读到 contenders=0，直接返回。八对操作不再调用44号，TID仍从用户头读。计数窗口和循环次数不变，仅把期望差值由9改为1：那一次仍是结束查询45自身，并不是锁请求。

修改 inspect 的这一个窗口。成功打印移出测量的顺序继续保持，因此不会把 puts 纳入这次差值：

<!-- replace: labs/airios/identity.c -->
<!-- before -->
```c
    for (unsigned i = 0; i < 128; ++i) require(airi_thread_id() == actual, "fast identity stable"); // 应只有TCB读，不重复42号查询。
    airi_mutex own = AIRI_MUTEX_INITIALIZER; // 当前调用栈独有，不制造另一项竞争。
    uint64_t before = airi_thread_syscalls(); // 记录当前执行者计数，不用全局计数混入其他工作者。
    for (unsigned i = 0; i < 8; ++i) require(airi_mutex_lock(&own, 0) == 0 && airi_mutex_unlock(&own) == 0, "mutex uses local identity"); // unlock仍调用44，不能把整个锁宣称为零陷入。
    uint64_t after = airi_thread_syscalls(); // 结束查询自身也增加一次，不能当作锁的开销。
    require(after - before == 9, "uncontended mutex syscall baseline"); // 八次unlock44加一次结束查询，身份读取已不陷入。
    airi_puts("[MUTEX_PROFILE] loops=8 delta=9\n"); // 只有实际内核计数差符合才打印，不从函数名猜次数。
    return actual; // 后续用独立原始参考比较，不自证同一缓存。
}
static long worker(void *argument) { // 原始声明值与本次内核TID都须属于新执行者。
```
<!-- after -->
```c
    for (unsigned i = 0; i < 128; ++i) require(airi_thread_id() == actual, "fast identity stable"); // 应只有TCB读，不重复42号查询。
    airi_mutex own = AIRI_MUTEX_INITIALIZER; // 当前调用栈独有，不制造另一项竞争。
    uint64_t before = airi_thread_syscalls(); // 记录当前执行者计数，不用全局计数混入其他工作者。
    for (unsigned i = 0; i < 8; ++i) require(airi_mutex_lock(&own, 0) == 0 && airi_mutex_unlock(&own) == 0, "mutex uses local identity"); // 有效TCB且无人参与竞争，八对操作应全部留在用户态。
    uint64_t after = airi_thread_syscalls(); // 结束查询自身也增加一次，不能当作锁的开销。
    require(after - before == 1, "uncontended mutex skips notification"); // 只有结束查询45的一次，八对锁不额外发出请求。
    airi_puts("[MUTEX_PROFILE] loops=8 delta=1\n"); // 只有实际内核计数差符合才打印，不从函数名猜次数。
    return actual; // 后续用独立原始参考比较，不自证同一缓存。
}
static long worker(void *argument) { // 原始声明值与本次内核TID都须属于新执行者。
```

宿主把同一六个窗口的期望改为1，并明确拒绝 identity 命令范围内的任何 FUTEX_WAIT/FUTEX_WAKE。这样“仍发了空唤醒、恰好返回零”也不能冒充省去了调用。mutex 和 heap-threads 两条有意争用的命令继续要求真实 WAIT，不能以删掉所有阻塞来满足无竞争成本。

在 scripts/shell_test.py 加入参与者标记，修改计数期望及机器结果字段：

<!-- replace: scripts/shell_test.py -->
<!-- before -->
```python
                offset=len(text); wire.sendall(b'mutex\n'); text=wait('[SHELL_COMMAND_DONE] name=mutex code=0',offset) # 真正共享普通数据在带锁yield时也不能半更新。
                mutex_output=text[offset:] # 当前程序而非旧日志提供结果。
                assert '[MUTEX_USER_OK] counter=64 critical_yield=1 foreign=denied self=deadlock timeout=pit owner_death=nonrobust' in mutex_output # 所有权、真实等待和非robust寿命都由guest核对。
                assert len(re.findall(r'\[FUTEX_WAIT\] pid=',mutex_output))>=4 and 'result=-110' in mutex_output # 两位争用者、超时者和死亡所有者观察确实进入等待。
                offset=len(text); wire.sendall(b'heap-threads\n'); text=wait('[SHELL_COMMAND_DONE] name=heap-threads code=0',offset) # 四个真实执行者共享唯一arena，观察对象交接。
                heap_threads=text[offset:] # 不用旧heap命令代替并发结果。
```
<!-- after -->
```python
                offset=len(text); wire.sendall(b'mutex\n'); text=wait('[SHELL_COMMAND_DONE] name=mutex code=0',offset) # 真正共享普通数据在带锁yield时也不能半更新。
                mutex_output=text[offset:] # 当前程序而非旧日志提供结果。
                assert '[MUTEX_USER_OK] counter=64 critical_yield=1 foreign=denied self=deadlock timeout=pit owner_death=nonrobust' in mutex_output # 所有权、真实等待和非robust寿命都由guest核对。
                assert '[MUTEX_PARTICIPANTS_OK] blocked=2 timeout=withdrawn acquired=withdrawn dead_owner=retained' in mutex_output # 真实竞争和到期均撤销参与者，死亡owner仍保留。
                assert len(re.findall(r'\[FUTEX_WAIT\] pid=',mutex_output))>=4 and 'result=-110' in mutex_output # 两位争用者、超时者和死亡所有者观察确实进入等待。
                offset=len(text); wire.sendall(b'heap-threads\n'); text=wait('[SHELL_COMMAND_DONE] name=heap-threads code=0',offset) # 四个真实执行者共享唯一arena，观察对象交接。
                heap_threads=text[offset:] # 不用旧heap命令代替并发结果。
```

<!-- replace: scripts/shell_test.py -->
<!-- before -->
```python
                assert len(counts)==5 and sum(map(len,counts.values()))==18 # 父三次、孩子跨exec六次、三位worker各三次。
                root_counts=counts[(root_id,root_id)] # 父执行者已有shell exec请求，不硬编码第一项。
                child_counts=counts[(child_id,child_id)] # exec只换映像，仍积累同一执行者的请求。
                assert len(root_counts)==3 and root_counts[2]-root_counts[1]==9 # 八次unlock通知，加结束查询自身一次。
                assert len(child_counts)==6 and child_counts[0]==1 and child_counts[2]-child_counts[1]==9 and child_counts[5]-child_counts[4]==9 and child_counts[3]>child_counts[2] # fork清零、exec保留，两个相同局部锁窗口分别计数。
                for tid in identity_workers: # 工作者可以交替运行，仍分别验证各自三个值。
                    samples=counts[(root_id,tid)] # 共享PID不能合并不同TID的测量。
                    assert len(samples)==3 and samples[0]==1 and samples[2]-samples[1]==9 # 新执行者首查询为1，无竞争八对操作仍有八次通知。
                assert identity_output.count('[MUTEX_PROFILE] loops=8 delta=9')==6 # guest与宿主按同一真实测量各自核对。
                for repetition in range(2): # 同一shell连续完成两次线程命令，上一组退出回收后应能重新创建。
                    offset=len(text); wire.sendall(b'threads\n'); text=wait('[SHELL_COMMAND_DONE] name=threads code=0',offset) # 真正同地址空间执行，不能由fork复制或宿主线程模拟。
                    thread_output=text[offset:] # 所有身份与标记均限定在这条命令之后。
```
<!-- after -->
```python
                assert len(counts)==5 and sum(map(len,counts.values()))==18 # 父三次、孩子跨exec六次、三位worker各三次。
                root_counts=counts[(root_id,root_id)] # 父执行者已有shell exec请求，不硬编码第一项。
                child_counts=counts[(child_id,child_id)] # exec只换映像，仍积累同一执行者的请求。
                assert len(root_counts)==3 and root_counts[2]-root_counts[1]==1 # 八对锁不发请求，只有结束查询自身一次。
                assert len(child_counts)==6 and child_counts[0]==1 and child_counts[2]-child_counts[1]==1 and child_counts[5]-child_counts[4]==1 and child_counts[3]>child_counts[2] # fork清零、exec保留，两个相同局部锁窗口分别计数。
                for tid in identity_workers: # 工作者可以交替运行，仍分别验证各自三个值。
                    samples=counts[(root_id,tid)] # 共享PID不能合并不同TID的测量。
                    assert len(samples)==3 and samples[0]==1 and samples[2]-samples[1]==1 # 新执行者首查询为1，八对无竞争操作省去全部通知。
                assert identity_output.count('[MUTEX_PROFILE] loops=8 delta=1')==6 # guest与宿主按同一真实测量各自核对。
                assert '[FUTEX_WAIT]' not in identity_output and '[FUTEX_WAKE]' not in identity_output # 局部锁没有竞争者，不能用返回0的空通知冒充用户态路径。
                for repetition in range(2): # 同一shell连续完成两次线程命令，上一组退出回收后应能重新创建。
                    offset=len(text); wire.sendall(b'threads\n'); text=wait('[SHELL_COMMAND_DONE] name=threads code=0',offset) # 真正同地址空间执行，不能由fork复制或宿主线程模拟。
                    thread_output=text[offset:] # 所有身份与标记均限定在这条命令之后。
```

上面的查询差值仍是1，因为结束查询45也进入了内核。机器结果里的 uncontended_mutex_syscalls 记录锁额外发出的请求，所以在这里写0：它已扣掉那一次观察请求。继续只更新下面结果窗口，PID/TID分组和其他功能字段仍由前面的实际断言支撑。

<!-- replace: scripts/shell_test.py -->
<!-- before -->
```python
                offset=len(text); wire.sendall(b'echo after-shell\n'); text=wait('[CONSOLE_UPDATE] view=0 commands=2 page=0',offset) # 回到内核后输入仍完整。
                assert 'after-shell' in text[offset:] # 不只是退出而丢了后续UART。
                result={'passed':True,'ring3_shell':True,'ram_file_and_device_readback':True,'other_user_during_cq_wait':True,'qmp_iops':1,'error_exec_survived':True,'returned_to_monitor':True,'orphan_rounds':24,'memory_reuse_rounds':128,'capacity_rounds':4,'capacity_pages':1024,'simd_preempt_fork_exec':True,'fs_base_preempt_fork_exec':True,'compiler_static_tls':True,'posix_errno_io_subset':True,'positioned_vectors':True} # 只发布已观察事实。
                result.update(per_execution_syscall_count=True,uncontended_mutex_pairs=8,uncontended_mutex_syscalls=8,runtime_tcb_identity=True,identity_reference_queries=7,shared_malloc=True,malloc_workers=4,malloc_rounds=96,owned_mutex=True,mutex_critical_updates=64,private_futex=True,futex_registrations=8,user_heap=True,shared_executions=True,thread_commands=2,worker_capacity=8,worker_reuse_rounds=24) # 关键字把实际已验收字段加入结果字典，不替代上面的逐命令检查。
                result_path.write_text(json.dumps(result,indent=2)+'\n'); print('PASS user shell: alloc/file/async wait/other task/CQ/readback, failed exec, return to monitor') # 阶段演示实际完成。
                return result # 总入口可收集。
            finally: # 包括断言、超时和QEMU错误。
```
<!-- after -->
```python
                offset=len(text); wire.sendall(b'echo after-shell\n'); text=wait('[CONSOLE_UPDATE] view=0 commands=2 page=0',offset) # 回到内核后输入仍完整。
                assert 'after-shell' in text[offset:] # 不只是退出而丢了后续UART。
                result={'passed':True,'ring3_shell':True,'ram_file_and_device_readback':True,'other_user_during_cq_wait':True,'qmp_iops':1,'error_exec_survived':True,'returned_to_monitor':True,'orphan_rounds':24,'memory_reuse_rounds':128,'capacity_rounds':4,'capacity_pages':1024,'simd_preempt_fork_exec':True,'fs_base_preempt_fork_exec':True,'compiler_static_tls':True,'posix_errno_io_subset':True,'positioned_vectors':True} # 只发布已观察事实。
                result.update(per_execution_syscall_count=True,uncontended_mutex_pairs=8,uncontended_mutex_syscalls=0,runtime_tcb_identity=True,identity_reference_queries=7,shared_malloc=True,malloc_workers=4,malloc_rounds=96,owned_mutex=True,mutex_critical_updates=64,private_futex=True,futex_registrations=8,user_heap=True,shared_executions=True,thread_commands=2,worker_capacity=8,worker_reuse_rounds=24) # 关键字把实际已验收字段加入结果字典，不替代上面的逐命令检查。
                result_path.write_text(json.dumps(result,indent=2)+'\n'); print('PASS user shell: alloc/file/async wait/other task/CQ/readback, failed exec, return to monitor') # 阶段演示实际完成。
                return result # 总入口可收集。
            finally: # 包括断言、超时和QEMU错误。
```

已有构建依赖会重编头的全部消费者，包括普通 malloc 和观察 malloc；本节不用新增用户程序或另一个锁实现。运行完整入口：

<!-- run -->
```sh
./airios build  # 重建所有使用8字节锁布局的用户对象
```

<!-- run -->
```sh
./airios test  # 无竞争计数、真实竞争、超时撤销和共享分配一起验证
```

<a id="mutex-fast-results"></a>
## 同一执行路径上的前后差别

交互运行 `./airios run`、`run sh`，依次输入 `mutex`、`heap-threads`、`identity`。本轮实际输出包括：

```text
[MUTEX_PARTICIPANTS_OK] blocked=2 timeout=withdrawn acquired=withdrawn dead_owner=retained
[MUTEX_USER_OK] counter=64 critical_yield=1 foreign=denied self=deadlock timeout=pit owner_death=nonrobust
[MALLOC_THREADS_OK] workers=4 rounds=96 arena=262144 cross_free=4 snapshots=consistent errno=private critical_yield=1
[THREAD_SYSCALLS] pid=83 tid=83 count=2
[THREAD_SYSCALLS] pid=83 tid=83 count=4
[THREAD_SYSCALLS] pid=83 tid=83 count=5
[MUTEX_PROFILE] loops=8 delta=1
```

主的2、4与K57相同：先前的exec、入口查询、原始42号参考和开始查询都没变。随后八次锁释放不再通知，结束查询只把4变成5。孩子的值为1、3、4、8、10、11，三worker各为1、3、4；父子关系与计数生命周期仍由同一宿主断言核对。18条45号、七条42号和六次测量数量不变，变化来自每个八次局部锁窗口移除了八次44号。

Linux 的 futex 也把无竞争同步留在用户态，需要睡眠时才进入内核；比较与阻塞是内核提供的原子动作。[futex 手册](https://man7.org/linux/man-pages/man2/futex.2.html)给出了这层分工。本项目保留有界超时、TID所有权和简单参与数，不兼容完整pthread布局或Linux futex ABI，Linux 对照实验仍等待真实主机验证。

次数下降只证明发出的请求减少，尚无延迟或吞吐的测量。共享 malloc 首次申请 arena 仍要请求用户页，其他操作也可能做实际 I/O，不能把局部无竞争锁的结果扩大成“malloc永不陷入”。后续继续建立条件等待与线程源码接口，同时保留这条可复现的成本观察。
