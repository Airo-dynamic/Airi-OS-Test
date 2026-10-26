# K54 · 取得所有权以后，才进入临界区

K53 可以按地址唤醒等待者，但三个 sleeper 醒来时 word 仍是 1。通知只让调用继续，不能保证两个工作者不会同时改 counter。现在在同一进程里增加一把锁：空闲值为 0，成功取得时保存当前 TID；其他执行者看到非零持有者后，才借 futex 暂停。释放方先发布空闲，再通知一人重新争取。

从 K53 的同一目录继续，使用相同的 before/after 局部编辑。内核等待机制保持，新增实现主要在用户库；作者工具和 GUI 均不是前置。最终观察让两名工作者在持锁时故意 yield，并各更新普通 counter/checksum 三十二次。

<a id="mutex-owner-cas"></a>
## 同时看见空闲，也只能有一人写入自己的身份

如果先读 owner==0，再单独写 owner=tid，两个执行者可以都读到 0，然后各自以为取得所有权。把这两步组合成原子比较交换，简称 CAS：仅当当前内存仍等于期望值 0，才写入本执行者的 TID；失败则取得实际观察到的持有者。

例如主执行者 TID72 已持锁，工作者 TID73 尝试 CAS(0→73) 失败，得到 observed=72。它随后用 WAIT(owner地址,72) 等待。如果主执行者已经释放，内核会看见 0 并返回 -11；若仍为 72，内核登记睡眠。醒来后必须重新 CAS，不能因为收到通知就直接进入临界区：另一个可运行者可能已经先拿走了锁。

TID72/73 只用于演算，不是每次日志必然的身份。当前内核在一次启动内单调分配非零 TID，不回绕复用，因此锁保存的旧身份不会自动变成另一个新工作者。持有者退出时是否清理锁是另一项协议，本章稍后实际观察不清理的后果。

新建 labs/airios/include/airi_mutex.h。owner 对外保留四字节布局，以便 C 和 C++ 都能包含同一声明；初始化完成后，所有并发访问都只走库内原子操作，不能直接给字段赋值。静态或局部 INITIALIZER 必须发生在对象发布之前。锁对象必须保持有效到最后一次等待和使用结束，不能复制一把正在使用的锁。

<!-- write: labs/airios/include/airi_mutex.h -->
```c
#ifndef AIRI_MUTEX_H // 有界私有互斥接口，不使用宿主pthread类型。
#define AIRI_MUTEX_H // 防止重复声明。
#include <stdint.h> // 锁字与TID同为四字节，0保留为空闲。
typedef struct { uint32_t owner; } airi_mutex; // 并发访问只经过运行库原子操作，不直接读写owner。
#define AIRI_MUTEX_INITIALIZER {0} // 在发布给其他执行者以前静态或局部初始化。
#ifdef __cplusplus // C++可以包含同一声明，不在头内暴露_Atomic语法。
extern "C" { // 调用已链接的C实现，保持符号名。
#endif // C编译时跳过链接语言范围。
long airi_mutex_trylock(airi_mutex *mutex); // 0取得，已占用-16；不等待。
long airi_mutex_lock(airi_mutex *mutex, unsigned ticks); // 有限PIT预算，重入-35，到期-110，0只尝试一次。
long airi_mutex_unlock(airi_mutex *mutex); // 只允许持有TID释放；非持有者-1，随后最多通知一人。
#ifdef __cplusplus // 结束函数的C链接声明。
} // 类型仍能由C++按普通聚合初始化。
#endif // 结束语言差异。
#endif // 结束包含保护。
```

<a id="mutex-acquire-release"></a>
## 所有权也交接普通数据的可见性

锁不只保护一个整数。change 将先读 counter，yield 后才写 counter+1，再更新 checksum。下一个持有者必须一起看到这两份普通数据，因此释放所有权使用 release，成功取得使用 acquire，二者通过同一个 owner 的发布与读取建立顺序。

实现使用目标编译器的 __atomic 内建。sizeof 核对整个锁对象为四字节，_Alignof 核对起点对齐至少四字节；__atomic_always_lock_free(4,0) 则在未绑定具体对象指针时，确认这四字节操作可以直接生成指令，不能悄悄依赖外部 libatomic。这些是编译器扩展，不是本项目另外实现的普通 C 函数。

沿刚才TID73的尝试看比较交换：第四实参0选择强CAS，不允许无理由失败。成功时以acquire取得锁并读取受保护数据；失败者尚无读取这些普通数据的权限，只需relaxed取得持有者数值。observed是本次尝试的栈上输出对象，失败后变成72，随后才将这个实际观察值交给WAIT再次比较。

先新建 labs/airios/libc/mutex.c，加入布局检查、valid、acquire 和 trylock。C 函数可先判断 NULL 与未对齐，但无法替任意坏映射完成内核式的 EFAULT 检查；传入已释放对象或随意地址仍可能造成用户 CPU 故障。trylock 只做一次尝试，包含自己已持有时都返回 -16。

<!-- write: labs/airios/libc/mutex.c -->
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
```

<a id="mutex-deadline-loop"></a>
## 失败以后，用同一个截止重试

lock 先尝试取得所有权。成功就返回 0，不读取时钟或调用 WAIT；自己已经持有时返回 -35，避免把非递归重入睡成超时。0 预算允许这一次立即尝试，只有仍被别人占用才返回 -110。过大参数先被拒绝，这与 POSIX 的时间参数和检查次序不是同一接口。

第一次确实需要等待时，读取 now 并建立唯一 deadline=now+ticks。失败得到的 observed 仍可能在调用 WAIT 前变化，这正由 K53 的再次比较处理。返回 -11 后重新 CAS；返回 0 也重新 CAS。仍未取得时再读取 now，先检查 now>=deadline，再做减法形成下次预算，避免无符号下溢或每次通知重新给满 ticks。

成功 CAS 才授予所有权。当前策略每次恢复先尝试取得，仍被占用才判断截止；它使用已交付 PIT 事件，既不是墙钟，也没有严格实时调度承诺。等待返回 -110 时直接交出超时，不把它解释成锁已经可用。

追加 lock：

<!-- append: labs/airios/libc/mutex.c -->
```c
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
```

释放方先查询自己的 TID，再尝试 CAS(tid→0)。若内存保存其他人或已经是 0，失败不修改字段，返回 -1。成功的 release 先发布临界区里的普通写入，然后通知最多一人。释放到通知之间即使有人先运行，等待者内部的比较也会检查最新持有者，不需要把“通知次数”保存成锁的计数。

通知不是把锁直接交给指定的人，扫描顺序也不保证公平。释放以后新来的执行者可能抢先取得，被通知者醒后重新尝试。本章没有递归计数、取消、优先级继承或 robust 恢复。若 wake 意外报错，字段已经变为 0，函数返回原始错误但不会写回旧 TID；一次状态改变不能被错误报告伪装成尚未发生。

追加 unlock：

<!-- append: labs/airios/libc/mutex.c -->
```c
long airi_mutex_unlock(airi_mutex *mutex) { // 释放与通知是两个分别完成的动作。
    if (!valid(mutex)) return -22; // 只检查能在C函数内安全判断的形状。
    uint32_t tid = (uint32_t)airi_thread_id(); // 不允许另一执行者冒充当前持有者。
    if (!__atomic_compare_exchange_n(&mutex->owner, &tid, 0, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) return -1; // -EPERM，失败不改变owner。
    long result = airi_futex_wake(&mutex->owner, 1); // 先发布空闲，再通知；空隙由WAIT内部比较弥补。
    return result < 0 ? result : 0; // 若通知意外失败，所有权已经释放，不能倒退恢复旧TID。
}
```

这版每次 trylock/lock/unlock 仍经 42 号查询真实 TID，unlock 也会调用通知，即使此刻无人等待。无争用只是不进入 WAIT，不是零系统调用快速路径。TID 暂不缓存在普通 TLS 里，因为 fork 会复制旧运行值；后续需要把快速身份与 fork/exec 更新一起设计，再优化无人等待时的通知。

在 errno.h 增加 EDEADLK 的名字，函数仍直接返回原始负错误，不设置 errno：

<!-- replace: labs/airios/include/errno.h -->
<!-- before -->
```c
#define ENOSPC 28 // 有界文件或目录空间不足。
#define ESPIPE 29 // 字节流不能定位。
#define EPIPE 32 // 没有读端；当前内核不附带SIGPIPE。
#define ENOSYS 38 // 当前未实现的操作。
#define ETIMEDOUT 110 // 有界等待预算已经用完。
#define EOVERFLOW 75 // 不能表示请求结果。
```
<!-- after -->
```c
#define ENOSPC 28 // 有界文件或目录空间不足。
#define ESPIPE 29 // 字节流不能定位。
#define EPIPE 32 // 没有读端；当前内核不附带SIGPIPE。
#define EDEADLK 35 // 当前执行者不能重复取得自己的非递归锁。
#define ENOSYS 38 // 当前未实现的操作。
#define ETIMEDOUT 110 // 有界等待预算已经用完。
#define EOVERFLOW 75 // 不能表示请求结果。
```

<a id="mutex-protected-observation"></a>
## 故意在半次更新中让出 CPU

新建 mutex-probe.c。counter 和 checksum 是普通对象，ready 单独用原子发布夹具进度。每位 change 工作者必须先取得 guard，读取 counter 和匹配校验，然后在尚未写回时 yield。另一位此时应只能看到锁的持有者并等待，不能进入同一段普通数据更新。

共享状态和三个工作函数先写完，再接 main。foreign 由另一个 TID 尝试解除主执行者的锁，失败后继续验证 trylock 和三个真实 tick 的超时。abandon 只在后面的独立子进程里使用：故意拿锁后普通返回，将“执行者已 join”和“业务锁已释放”分开观察。

<!-- write: labs/airios/mutex-probe.c -->
```c
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
```

主执行者先核对显式坏形状和预算，再持锁创建两名工作者。ready=2 只代表它们已经开始尝试，sleep 给出进入 WAIT 的机会，实际登记由日志确认。此时主执行者还持锁，所以 foreign 的超时不依赖它们之间谁先被调度。主执行者在 join foreign 时暂停，PIT 仍能结束第三人的等待。

确认 counter 仍是 0 后，主执行者释放。两位工作者各完成 32 次临界区，join 后再次加锁读取 64 和对应 checksum。只检查最终计数不足以说明临界区中间没有半更新，因此工作函数还在每次取得后核对 checksum，并在读写之间故意换执行者。

<!-- append: labs/airios/mutex-probe.c -->
```c
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
    long tester = airi_thread_create(foreign, 0), result; // 在仍持有guard时进行非法释放和超时观察。
    require(tester > 0 && airi_thread_join(tester, &result) == 0 && result == -110, "foreign owner and timeout"); // 主join期间仍持锁，第三人必须到期。
    require(counter == 0 && checksum == 0x5a5a && airi_mutex_unlock(&guard) == 0, "publish initial release"); // 两位工作者此前都没有进入临界区。
    require(airi_thread_join(first, &result) == 0 && result == 32, "join first"); // 主执行者等真实结果，不读取运行中的普通数据。
    require(airi_thread_join(second, &result) == 0 && result == 32, "join second"); // 两人的临界区现已全部结束。
    require(airi_mutex_trylock(&guard) == 0 && counter == 64 && checksum == (64 ^ 0x5a5a), "protected ordinary data"); // 加锁后核对最终状态。
    require(airi_mutex_unlock(&guard) == 0, "final unlock"); // 不把持锁对象复制到下面的fork。
```

<a id="mutex-owner-lifetime"></a>
## join 不会替工作者修复它遗留的锁

所有工作者已 join、guard 已释放后再 fork。孩子里的 abandon 取得 guard 后返回 9；内核安全回收它的执行栈和 FPU，但用户内存里的 owner 仍是那个旧 TID。孩子主执行者的 lock 于是等到 -110，unlock 也因非持有者返回 -1。

这不是 robust 锁的 EOWNERDEAD 交接：没有人检查或修复受保护数据，也没有权限强制接管。观察以孩子退出 41 结束该地址空间；父的 guard 是另一页表中的独立副本，仍可立即取得。整个对象在使用中被复制、释放，或 fork 时复制了一把被别的执行者持有的锁，都不能靠 INITIALIZER 的名字获得安全性。这里特意在锁空闲且没有 worker 时 fork，保持现有生命周期边界。

<!-- append: labs/airios/mutex-probe.c -->
```c
    long child = airi_fork(); require(child >= 0, "owner death fork"); // 本时刻没有worker且锁空闲。
    if (!child) { // 持有者返回而未释放的情况只影响这个独立地址空间。
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

<a id="mutex-build"></a>
## 只给观察 ELF 链入锁库

追加 Makefile 规则和 images.S 范围，再在真实目录注册 mutex。对象使用 mutex-lib.o，观察入口用 mutex-probe.o，避免两份同名源文件被已有模式规则混在一起。其他旧程序不自动链接锁库。

<!-- append: Makefile -->
```makefile

# 锁实现使用四字节原子内建与自有线程/等待接口。
$(OUT)/user/mutex-lib.o: labs/airios/libc/mutex.c Makefile
# 不与观察程序mutex-probe.o共用对象名。
	@mkdir -p $(dir $@)
# 编译器必须在当前目标直接生成原子指令。
	$(CC) $(USER_FLAGS) -Ilabs/airios/include -c $< -o $@
# 该用户程序显式选择锁库，不改变其他早期ELF。
$(OUT)/user/mutex.elf: $(USER_COMMON) $(OUT)/user/threads-lib.o $(OUT)/user/thread-entry.o $(OUT)/user/mutex-lib.o $(OUT)/user/mutex-probe.o labs/airios/program.ld
# 静态链接不借宿主pthread或libatomic。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(filter %.o,$^) -o $@
# 初始目录等待本配置镜像真正完成。
$(OUT)/labs/airios/images.o: $(OUT)/user/mutex.elf
# 路径仅供incbin打包实际字节。
$(OUT)/labs/airios/images.o: CPPFLAGS += -DAIRI_MUTEX_ELF='"$(OUT)/user/mutex.elf"'
```

<!-- append: labs/airios/images.S -->
```asm

.balign 8 /* 互斥观察在真实用户地址空间运行。 */
.global airi_mutex_elf_start, airi_mutex_elf_end /* 初始程序目录使用完整只读文件范围。 */
airi_mutex_elf_start: /* 独立C用户映像的文件首。 */
.incbin AIRI_MUTEX_ELF /* 本配置链接好的锁与工作者程序。 */
airi_mutex_elf_end: /* 文件排他右端。 */
```

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
extern const uint8_t airi_process_elf_start[], airi_process_elf_end[]; // 新的fork/wait和抢占工作负载。
extern const uint8_t airi_vector_elf_start[], airi_vector_elf_end[]; // 显式偏移向量正常与坏范围观察。
extern const uint8_t airi_malloc_elf_start[], airi_malloc_elf_end[]; // 用户对象分配与页面缓存的独立ELF。
extern const uint8_t airi_futex_elf_start[], airi_futex_elf_end[]; // 私有内存等待与通知的真实用户程序。
extern const uint8_t airi_threads_elf_start[], airi_threads_elf_end[]; // 同进程共享内存、FD与异步owner的真实工作执行者。
extern const uint8_t airi_posix_elf_start[], airi_posix_elf_end[]; // 显式链接errno与POSIX风格文件包装。
```
<!-- after -->
```c
extern const uint8_t airi_process_elf_start[], airi_process_elf_end[]; // 新的fork/wait和抢占工作负载。
extern const uint8_t airi_vector_elf_start[], airi_vector_elf_end[]; // 显式偏移向量正常与坏范围观察。
extern const uint8_t airi_malloc_elf_start[], airi_malloc_elf_end[]; // 用户对象分配与页面缓存的独立ELF。
extern const uint8_t airi_mutex_elf_start[], airi_mutex_elf_end[]; // 原子所有权与futex等待组合的真实用户观察。
extern const uint8_t airi_futex_elf_start[], airi_futex_elf_end[]; // 私有内存等待与通知的真实用户程序。
extern const uint8_t airi_threads_elf_start[], airi_threads_elf_end[]; // 同进程共享内存、FD与异步owner的真实工作执行者。
extern const uint8_t airi_posix_elf_start[], airi_posix_elf_end[]; // 显式链接errno与POSIX风格文件包装。
```

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    {"vector", airi_vector_elf_start, airi_vector_elf_end}, // 聚合、分散、短前缀与错误原子性。
    {"heap", airi_malloc_elf_start, airi_malloc_elf_end}, // 单线程用户对象与错误路径。
    {"heap-reset", airi_malloc_elf_start, airi_malloc_elf_end}, // 同一ELF新BSS和按需arena。
    {"futex", airi_futex_elf_start, airi_futex_elf_end}, // 相等比较与登记、超时及进程私有键。
    {"threads", airi_threads_elf_start, airi_threads_elf_end}, // 独立TLS/CPU状态、join与整个进程退出。
    {"libc", airi_posix_elf_start, airi_posix_elf_end}, // 用户库错误与文件结果转换。
```
<!-- after -->
```c
    {"vector", airi_vector_elf_start, airi_vector_elf_end}, // 聚合、分散、短前缀与错误原子性。
    {"heap", airi_malloc_elf_start, airi_malloc_elf_end}, // 单线程用户对象与错误路径。
    {"heap-reset", airi_malloc_elf_start, airi_malloc_elf_end}, // 同一ELF新BSS和按需arena。
    {"mutex", airi_mutex_elf_start, airi_mutex_elf_end}, // 临界区、错误所有者、截止与非robust寿命。
    {"futex", airi_futex_elf_start, airi_futex_elf_end}, // 相等比较与登记、超时及进程私有键。
    {"threads", airi_threads_elf_start, airi_threads_elf_end}, // 独立TLS/CPU状态、join与整个进程退出。
    {"libc", airi_posix_elf_start, airi_posix_elf_end}, // 用户库错误与文件结果转换。
```

已有 shell 自动会话接着运行 mutex，并限定在本条命令以后查找结果。至少四次真实 FUTEX_WAIT 分别涵盖两位争用者、foreign 超时和孩子的死亡所有者观察；临界区轮转还可能增加更多登记，因此不把一次调度轨迹写成固定数量。

<!-- replace: scripts/shell_test.py -->
<!-- before -->
```python
                assert '[FUTEX_COUNT_OK] waiters=3 wake_one=1 wake_rest=2 unchanged=1' in futex_output # 数量与字不被修改分别由guest检查。
                assert '[FUTEX_USER_OK] compare=atomic prewake=checked timeout=pit counts=1,2 private=process,va exit=clean' in futex_output # 私有键、退出清理与真实PIT超时都已完成。
                assert len(re.findall(r'\[FUTEX_WAIT\] pid=',futex_output))==8 # 一次超时、三次同键、父子各一、退出孩子与复用者各一。
                for repetition in range(2): # 同一shell连续完成两次线程命令，上一组退出回收后应能重新创建。
                    offset=len(text); wire.sendall(b'threads\n'); text=wait('[SHELL_COMMAND_DONE] name=threads code=0',offset) # 真正同地址空间执行，不能由fork复制或宿主线程模拟。
                    thread_output=text[offset:] # 所有身份与标记均限定在这条命令之后。
```
<!-- after -->
```python
                assert '[FUTEX_COUNT_OK] waiters=3 wake_one=1 wake_rest=2 unchanged=1' in futex_output # 数量与字不被修改分别由guest检查。
                assert '[FUTEX_USER_OK] compare=atomic prewake=checked timeout=pit counts=1,2 private=process,va exit=clean' in futex_output # 私有键、退出清理与真实PIT超时都已完成。
                assert len(re.findall(r'\[FUTEX_WAIT\] pid=',futex_output))==8 # 一次超时、三次同键、父子各一、退出孩子与复用者各一。
                offset=len(text); wire.sendall(b'mutex\n'); text=wait('[SHELL_COMMAND_DONE] name=mutex code=0',offset) # 真正共享普通数据在带锁yield时也不能半更新。
                mutex_output=text[offset:] # 当前程序而非旧日志提供结果。
                assert '[MUTEX_USER_OK] counter=64 critical_yield=1 foreign=denied self=deadlock timeout=pit owner_death=nonrobust' in mutex_output # 所有权、真实等待和非robust寿命都由guest核对。
                assert len(re.findall(r'\[FUTEX_WAIT\] pid=',mutex_output))>=4 and 'result=-110' in mutex_output # 两位争用者、超时者和死亡所有者观察确实进入等待。
                for repetition in range(2): # 同一shell连续完成两次线程命令，上一组退出回收后应能重新创建。
                    offset=len(text); wire.sendall(b'threads\n'); text=wait('[SHELL_COMMAND_DONE] name=threads code=0',offset) # 真正同地址空间执行，不能由fork复制或宿主线程模拟。
                    thread_output=text[offset:] # 所有身份与标记均限定在这条命令之后。
```

<!-- replace: scripts/shell_test.py -->
<!-- before -->
```python
                offset=len(text); wire.sendall(b'echo after-shell\n'); text=wait('[CONSOLE_UPDATE] view=0 commands=2 page=0',offset) # 回到内核后输入仍完整。
                assert 'after-shell' in text[offset:] # 不只是退出而丢了后续UART。
                result={'passed':True,'ring3_shell':True,'ram_file_and_device_readback':True,'other_user_during_cq_wait':True,'qmp_iops':1,'error_exec_survived':True,'returned_to_monitor':True,'orphan_rounds':24,'memory_reuse_rounds':128,'capacity_rounds':4,'capacity_pages':1024,'simd_preempt_fork_exec':True,'fs_base_preempt_fork_exec':True,'compiler_static_tls':True,'posix_errno_io_subset':True,'positioned_vectors':True} # 只发布已观察事实。
                result.update(private_futex=True,futex_registrations=8,user_heap=True,shared_executions=True,thread_commands=2,worker_capacity=8,worker_reuse_rounds=24) # 关键字把实际已验收字段加入结果字典，不替代上面的逐命令检查。
                result_path.write_text(json.dumps(result,indent=2)+'\n'); print('PASS user shell: alloc/file/async wait/other task/CQ/readback, failed exec, return to monitor') # 阶段演示实际完成。
                return result # 总入口可收集。
            finally: # 包括断言、超时和QEMU错误。
```
<!-- after -->
```python
                offset=len(text); wire.sendall(b'echo after-shell\n'); text=wait('[CONSOLE_UPDATE] view=0 commands=2 page=0',offset) # 回到内核后输入仍完整。
                assert 'after-shell' in text[offset:] # 不只是退出而丢了后续UART。
                result={'passed':True,'ring3_shell':True,'ram_file_and_device_readback':True,'other_user_during_cq_wait':True,'qmp_iops':1,'error_exec_survived':True,'returned_to_monitor':True,'orphan_rounds':24,'memory_reuse_rounds':128,'capacity_rounds':4,'capacity_pages':1024,'simd_preempt_fork_exec':True,'fs_base_preempt_fork_exec':True,'compiler_static_tls':True,'posix_errno_io_subset':True,'positioned_vectors':True} # 只发布已观察事实。
                result.update(owned_mutex=True,mutex_critical_updates=64,private_futex=True,futex_registrations=8,user_heap=True,shared_executions=True,thread_commands=2,worker_capacity=8,worker_reuse_rounds=24) # 关键字把实际已验收字段加入结果字典，不替代上面的逐命令检查。
                result_path.write_text(json.dumps(result,indent=2)+'\n'); print('PASS user shell: alloc/file/async wait/other task/CQ/readback, failed exec, return to monitor') # 阶段演示实际完成。
                return result # 总入口可收集。
            finally: # 包括断言、超时和QEMU错误。
```

<!-- run -->
```sh
./airios build  # 静态链接当前锁库与独立用户程序，未解析符号必须使链接失败
```

<!-- run -->
```sh
./airios test  # 经真实UART运行争用、错误、截止、进程寿命及既有回归
```

<a id="mutex-results"></a>
## 观察所有权、普通数据与目标指令

本轮实际 shell 输出：

```text
[MUTEX_USER_OK] counter=64 critical_yield=1 foreign=denied self=deadlock timeout=pit owner_death=nonrobust
[SHELL_COMMAND_DONE] name=mutex code=0
```

交互运行 `./airios run`，在内核提示符输入 `run sh`，再输入 `mutex`，可以观察同一次命令的 FUTEX_WAIT 和 WAIT_WAKE。看到 result=-110 要结合当前执行者判断：foreign 是被主执行者一直占用；孩子主执行者则面对已经退出而未释放的旧 TID。两者输出相同，持有者寿命和后续恢复方式不同。

当前 LLVM 反汇编显示 CAS 使用 x86 的 lock 前缀和 cmpxchgl。lock 前缀作用于紧随的比较交换，使对目标内存的读、比较、可能写入作为一个原子读改写；它不是调用线程库。cmpxchg 用 EAX 保存期望值，成功时把源寄存器的 TID 写入目标，失败把目标原值交回 EAX。编译器再把该值对应到 observed。反汇编器可能把前缀单独打印成一行，CPU 仍将它和后续指令一起解码。

mutex.elf 没有未解析符号，不借宿主 pthread 或 libatomic。另以 freestanding C++17 编译器检查了两个公共头的声明与聚合初始化；这只证明头可以被 C++ 源码包含，尚未证明 Server-Lib 或 C++ runtime 已能运行。



这项头检查可以用以下完整输入复现。临时文件在 build 下，不进入内核镜像；-fsyntax-only 只解析类型、声明与调用，不链接函数或运行 C++ 程序。tool 仍选择当前工程已验证的目标编译器。

先运行 `mkdir -p build`，新建临时文件 build/mutex-header.cpp，内容如下；它不进入源码清单或镜像：

```cpp
#include <airi_mutex.h> // 检查聚合类型和C链接函数声明
#include <airi_threads.h> // 同一翻译单元也能包含共享执行接口
static airi_mutex value = AIRI_MUTEX_INITIALIZER; // 只在创建工作者以前初始化
long observe() { return airi_mutex_trylock(&value); } // 仅检查调用类型，不要求此处链接运行库
```

执行头检查：

```sh
PYTHONPATH=scripts python3 - <<'PYCODE'  # 复用工具选择规则生成一次编译参数列表
import subprocess  # 启动目标编译器并检查退出码
from tools import tool  # 选择当前LLVM目标工具，不直接借宿主C++运行库
subprocess.run([tool('clang', 'llvm'), '-target', 'x86_64-unknown-none-elf', '-std=c++17', '-ffreestanding', '-fsyntax-only', '-Ilabs/airios/include', 'build/mutex-header.cpp'], check=True)  # 头兼容错误使本次命令失败
PYCODE
```

[编译器原子内建文档](https://gcc.gnu.org/onlinedocs/gcc/_005f_005fatomic-Builtins.html)定义比较交换和内存序参数；本章实现及工作负载由本项目编写。[POSIX timed mutex](https://man7.org/linux/man-pages/man3/pthread_mutex_timedlock.3p.html)对时间参数、错误返回与 robust 有更完整约定，不能把当前负错误和 PIT 预算当成 pthread ABI。Linux 对照仍使用 p5-threads 中真正的 pthread mutex/condition queue；K53 的本轮 SSH255 没有进入执行，当前沿用未验证状态。

下一步才能用这把锁保护共享分配器元数据；现有 K48 malloc 仍是单线程实现，不能因为同一镜像里出现了 mutex.elf 就自动当成线程安全。短期优化还包括减少无争用身份查询与无等待通知，均需单独观察调用路径及 fork/exec 后的身份更新。
