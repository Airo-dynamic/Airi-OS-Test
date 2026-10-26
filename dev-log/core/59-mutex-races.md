# K59 · 在四个真实交错位置继续同一次加锁

K58 的竞争更新和超时已经实际运行，但一次普通运行不一定恰好停在登记前后。现在仍使用同一份 mutex.c，在观察版本的三个位置暂停，让主执行者先释放，再恢复 worker 原来尚未完成的 lock 调用。这样可以区分二次CAS直接取得、WAIT比较失败，以及已经睡下后收到通知。

从 K58 的同一目录继续。普通锁的字段和算法不变，新增一个真实用户程序 mutex-races。暂停只编进专用 mutex-observe.o，普通 mutex-lib.o、分配器和 identity 不引用观察回调。全部操作仍在固定单CPU guest 中，宿主仅沿 UART 驱动并读取结果。

<a id="races-four-positions"></a>
## 主执行者持锁，同一个worker从暂停点继续

每轮主执行者先取得 guard，再创建一位 worker。worker 的初次 CAS 必须失败，因为 owner 仍是主TID。前三轮分别停在下面的位置，主确认到达后释放并放行；第四轮不暂停，让 worker 真正进入内核等待。各轮都要 join 收取，确认 owner 和 contenders 均归零，才开始下一轮。

| 轮次 | worker停止处 | 释放时参与数 | 主的通知 | worker随后发生的事 |
| --- | --- | --- | --- | --- |
| 1 | 初次失败、尚未登记 | 0 | 省去44号 | 登记后CAS直接取得，无43号 |
| 2 | 已登记、尚未复查 | 1 | 44号返回唤醒0 | CAS直接取得，无43号 |
| 3 | 已复查失败、尚未WAIT | 1 | 44号返回唤醒0 | 43号比较到owner=0，立即返回-11，再CAS取得 |
| 4 | 已在内核登记睡眠 | 1 | 44号唤醒1 | 原43号返回0，再CAS取得 |

第2轮说明参与数不是精确睡眠数，空通知允许出现。第3轮说明请求了 WAIT 也不等于真的睡下：它确实进入43号，但没有 FUTEX_WAIT 登记日志。第4轮才同时有等待登记和真正通知返回。四轮都从原来的调用继续，不用重新执行一次 lock 掩盖中间状态。

<a id="races-observer-object"></a>
## 在用户库里留观察位置，普通编译不产生回调

给 mutex.c 加一份只在 AIRI_MUTEX_OBSERVE 定义时可见的函数声明。条件编译在产生机器指令前选择源码；普通目标没有这份定义时，声明与调用都会被排除。观察回调由新的用户程序提供，接收当前锁、位置编号和返回值，不替算法写 owner、参与数或系统调用结果。

位置1在固定截止建立以后、fetch_add以前，位置2在fetch_add以后、二次CAS以前，位置3在CAS失败以后、重读当前tick以前。暂停会消耗真实时间，因此恢复后才读now，不能沿用暂停前的now把WAIT预算意外延长。位置4在实际WAIT返回以后，只统计result，不再暂停。

修改 mutex.c 的两个窗口。未启用宏时仍是K58相同的语句顺序；暂停版本也保留原deadline，不为每次恢复重新借预算。

<!-- replace: labs/airios/libc/mutex.c -->
<!-- before -->
```c
#include <airi_mutex.h> // owner布局与对外返回合同。
#include <airi_threads.h> // 真实执行身份与比较并等待。
#include <airi_libc.h> // 已交付PIT事件计数，不使用墙钟。
_Static_assert(sizeof(airi_mutex) == 8 && _Alignof(airi_mutex) >= 4, "mutex needs two aligned four-byte words"); // 两个四字节原子字段，WAIT仍只使用owner地址。
_Static_assert(__atomic_always_lock_free(4, 0), "mutex needs native four-byte atomics"); // 当前目标不能导入未实现的libatomic。
static int valid(airi_mutex *mutex) { // C库仍要求调用者提供有效存活对象。
```
<!-- after -->
```c
#include <airi_mutex.h> // owner布局与对外返回合同。
#include <airi_threads.h> // 真实执行身份与比较并等待。
#include <airi_libc.h> // 已交付PIT事件计数，不使用墙钟。
#ifdef AIRI_MUTEX_OBSERVE // 只在单独观察对象中启用，不进入普通互斥库。
void airi_mutex_observe(airi_mutex *mutex, unsigned stage, long result); // 用户夹具暂停1登记前/2复查前/3WAIT前，4报告真实返回。
#endif // 普通编译没有回调依赖。
_Static_assert(sizeof(airi_mutex) == 8 && _Alignof(airi_mutex) >= 4, "mutex needs two aligned four-byte words"); // 两个四字节原子字段，WAIT仍只使用owner地址。
_Static_assert(__atomic_always_lock_free(4, 0), "mutex needs native four-byte atomics"); // 当前目标不能导入未实现的libatomic。
static int valid(airi_mutex *mutex) { // C库仍要求调用者提供有效存活对象。
```

<!-- replace: labs/airios/libc/mutex.c -->
<!-- before -->
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
```
<!-- after -->
```c
    uint64_t now = airi_ticks(); // 从首次确需等待的位置计算名义时间。
    if (now > UINT64_MAX - ticks) return -75; // 截止不能回绕。
    uint64_t deadline = now + ticks; // 重试或通知都不能延长这次总预算。
#ifdef AIRI_MUTEX_OBSERVE // 首次失败已发生，当前仍没有参与登记。
    airi_mutex_observe(mutex, 1, 0); // 允许主执行者在人数0时释放。
#endif // 观察之外没有主动交接。
    __atomic_fetch_add(&mutex->contenders, 1, __ATOMIC_SEQ_CST); // 先登记可能等待的参与者，再重新检查owner，不能反过来留下漏通知窗口。
#ifdef AIRI_MUTEX_OBSERVE // 现在已经参加，但还没有二次CAS。
    airi_mutex_observe(mutex, 2, 0); // 允许一次找不到内核等待者的通知。
#endif // 之后恢复同一个锁调用。
    long result; // 所有登记后的成功/失败共用一个撤销出口。
    for (;;) { // 初次也先重新CAS；醒来只说明可重试，不代表已经获锁。
        if (acquire(mutex, tid, &observed)) { result = 0; break; } // 登记后锁可能早已空闲，先争取避免带旧owner睡下。
        if (observed == tid) { result = -35; break; } // 错误重入仍须撤销本次参与计数。
#ifdef AIRI_MUTEX_OBSERVE // 本次复查仍失败，故意留出实际WAIT以前的窗口。
        airi_mutex_observe(mutex, 3, 0); // 释放后仍带本次observed进入内核比较。
#endif // 仅观察版本暂停，不修改owner或计数。
        now = airi_ticks(); // 所有重试共用同一次固定截止，不续借预算。
        if (now >= deadline) { result = -110; break; } // 相减前先拒绝到期，防止无符号回绕。
        result = airi_futex_wait(&mutex->owner, observed, (unsigned)(deadline - now)); // 登记到内核前再次比较实际owner，解决复查以后的释放。
#ifdef AIRI_MUTEX_OBSERVE // 记录已经返回的真实43号结果，不伪造通知/比较失败。
        airi_mutex_observe(mutex, 4, result); // 夹具只统计，不在返回点暂停。
#endif // 普通路径保留原来的分支。
        if (result && result != -11) break; // 超时/错误统一撤销，值改变-11则回循环争取。
    }
    __atomic_fetch_sub(&mutex->contenders, 1, __ATOMIC_SEQ_CST); // 取得、超时及错误各减一次；重试期间一直保留登记。
```

现在可以编译普通库，但观察对象还缺最终回调定义。新建 mutex-races.c，先写夹具字段和 await。target选择本轮位置，armed只允许命中一次，parked表示worker已到达，gate表示主已释放、允许继续。它们是独立原子握手，不需要取得正在被观察的 guard，避免“等别人释放guard时先拿guard”造成循环等待。

await用acquire读取对方release写入的值，期限从一次start起算；每次yield给真正的另一条用户执行链运行，不用循环次数代替时间。握手超过100个名义tick直接结束进程组，外层宿主也有超时，因此错误暂停点不会被当成成功。

<!-- write: labs/airios/mutex-races.c -->
```c
#include <airi_mutex.h> // 与普通程序相同的8字节锁和算法。
#include <airi_threads.h> // 真实工作执行者及请求计数。
#include <airi_libc.h> // 有限名义tick、主动交接和串口。
#include <stdatomic.h> // 观察夹具的握手不借被观察锁。
static airi_mutex guard = AIRI_MUTEX_INITIALIZER; // 四轮之间只在完全收拢以后复用。
static _Atomic unsigned target, armed, parked, gate, started; // 选定一个暂停位置，发布到达与继续许可。
static _Atomic unsigned calls, mismatches, notifications; // 记录真实WAIT返回，不把未登记误认为睡眠。
static void require(int condition, const char *message) { // 任一错误结束本真实进程组。
    if (!condition) { airi_puts(message); airi_puts("\n"); airi_exit(85); } // 不继续发布成功标记。
}
static void await(_Atomic unsigned *word, unsigned value) { // 所有观察握手都有总期限。
    uint64_t start = airi_ticks(); // 使用真实交付的100Hz事件，不把yield次数当时间。
    while (atomic_load_explicit(word, memory_order_acquire) != value) { // 对方release发布以后才继续当前操作。
        require(airi_ticks() - start < 100, "mutex race handshake timeout"); // 错位的暂停点不能无限挂住guest。
        airi_yield(); // 让真正的另一条用户执行链运行。
    }
}
```

<a id="races-handshake"></a>
## 发布到达后，等主释放，再回到原调用

回调先确认收到的就是guard。位置4拿到的result来自真实43号：增加calls，-11再增加mismatches，0增加notifications，然后返回。立即比较失败也算一次calls，不能用内核有没有登记日志来代替请求数。

其他位置只在stage等于target时考虑暂停。atomic_exchange把armed改成0并取旧值；只有第一次读到旧1才继续，避免第三轮的-11重试再次停到同一位置。先release写parked=1，让主能确认此前的登记状态，再await等待gate=1。gate打开后从这个C函数返回，继续mutex.c原来那条调用栈，没有重启工作函数。

<!-- append: labs/airios/mutex-races.c -->
```c
void airi_mutex_observe(airi_mutex *mutex, unsigned stage, long result) { // 只有观察编译的锁对象调用，本文件提供最终链接定义。
    require(mutex == &guard, "unexpected observed mutex"); // 不把其他库内部锁纳入握手。
    if (stage == 4) { // 真实WAIT返回以后，result已经来自43号。
        atomic_fetch_add_explicit(&calls, 1, memory_order_relaxed); // 记录每次实际发出的等待请求，包括立即-11。
        if (result == -11) atomic_fetch_add_explicit(&mismatches, 1, memory_order_relaxed); // 比较失败没有真正睡下。
        if (result == 0) atomic_fetch_add_explicit(&notifications, 1, memory_order_relaxed); // 返回0表示真实通知，仍不表示授锁。
        return; // 返回观察点不能再次暂停同一个请求。
    }
    if (stage != atomic_load_explicit(&target, memory_order_relaxed) || !atomic_exchange_explicit(&armed, 0, memory_order_acq_rel)) return; // 仅命中选定点的第一次，后续重试保持算法运行。
    atomic_store_explicit(&parked, 1, memory_order_release); // 发布暂停点以前所有用户字段状态。
    await(&gate, 1); // 主执行者释放以后再继续原来的锁调用。
}
```

worker先发布started，再以200tick预算调用普通算法。前三轮在指定回调里暂停，第四轮没有匹配的暂停点，会一直走到真实WAIT。取得以后，本轮只有它一位竞争者，慢路径应已撤销，所以contenders必须为0。最后unlock归还owner；此时也无人等待，不应多发通知。

<!-- append: labs/airios/mutex-races.c -->
```c
static long worker(void *argument) { // 每轮真实创建一位竞争者。
    (void)argument; // 观察配置在创建前通过原子字段发布。
    atomic_store_explicit(&started, 1, memory_order_release); // 第四轮不用暂停，但主执行者仍需知道worker已运行。
    if (airi_mutex_lock(&guard, 200)) return -220; // 共用普通实现的固定截止、登记、复查和内核WAIT。
    if (__atomic_load_n(&guard.contenders, __ATOMIC_SEQ_CST) != 0) return -221; // 本轮只有自己，取得以后必须已撤销。
    return airi_mutex_unlock(&guard); // 没有其他参与者，归还时不应通知。
}
```

<a id="races-primary-release"></a>
## 主的释放成本与worker的等待结果分开测

main使用两组固定字符串标记每轮边界。begin和done都是指向静态只读字符串的指针数组，stage从1开始，取stage-1下标；这里不增加printf或可变参数库。BEGIN在持锁以前输出，DONE只在join和状态断言之后输出，宿主据此切出每轮实际事件。

<!-- append: labs/airios/mutex-races.c -->
```c
int main(void) { // 暂停只属于本观察ELF，普通锁没有这些回调。
    static const char *begin[] = { // 固定字符串便于宿主划定每轮输出，不额外引入格式化库。
        "[MUTEX_RACE_BEGIN] stage=1\n", // 第一次失败以后、登记以前。
        "[MUTEX_RACE_BEGIN] stage=2\n", // 登记以后、复查以前。
        "[MUTEX_RACE_BEGIN] stage=3\n", // 复查失败以后、WAIT以前。
        "[MUTEX_RACE_BEGIN] stage=4\n" // 不暂停，让worker真正登记到内核。
    };
    static const char *done[] = { // 每个标记都在实际数值核对后输出。
        "[MUTEX_RACE_DONE] stage=1 participants=0 wake=skipped wait=none\n", // 无参与者释放，二次CAS直接取得。
        "[MUTEX_RACE_DONE] stage=2 participants=1 wake=empty wait=none\n", // 空通知允许存在，复查避免WAIT。
        "[MUTEX_RACE_DONE] stage=3 participants=1 wake=empty wait=mismatch\n", // 最后比较返回-11而不是睡错。
        "[MUTEX_RACE_DONE] stage=4 participants=1 wake=one wait=notified\n" // 已睡者确实被一次通知唤醒。
    };
```

每轮先在没有活worker时清握手和统计，再取得guard并创建worker。前三轮await parked，第四轮await started后主睡三tick：主仍持有guard，但离开CPU，让worker实际登记到内核。随后核对参与数，第1轮为0，其余为1，不能仅凭位置标签假定字段已改变。

下面三块连续接在main内，for到最后一块才闭合，本章写完再构建。先写到参与数核对为止，主在这一块结束时仍持有guard。

<!-- append: labs/airios/mutex-races.c -->
```c
    for (unsigned stage = 1; stage <= 4; ++stage) { // 每轮join完再重置握手，不能覆盖活观察状态。
        atomic_store_explicit(&target, stage, memory_order_relaxed); // 1至3暂停，4只观察真实等待返回。
        atomic_store_explicit(&armed, 1, memory_order_relaxed); // 重新允许选定点命中一次。
        atomic_store_explicit(&parked, 0, memory_order_relaxed); atomic_store_explicit(&gate, 0, memory_order_relaxed); // 清除上一轮到达与放行。
        atomic_store_explicit(&started, 0, memory_order_relaxed); atomic_store_explicit(&calls, 0, memory_order_relaxed); // 此时没有worker访问这些对象。
        atomic_store_explicit(&mismatches, 0, memory_order_relaxed); atomic_store_explicit(&notifications, 0, memory_order_relaxed); // 真实返回统计重新开始。
        airi_puts(begin[stage - 1]); // 在加锁前建立本轮日志边界。
        require(airi_mutex_lock(&guard, 0) == 0, "race primary owns guard"); // worker首次争取必定面对主所有权。
        long tid = airi_thread_create(worker, 0), result; // 不用宿主模拟或fork复制代替共享地址空间。
        require(tid > 0, "race worker create"); // 创建失败不继续等待不存在的人。
        if (stage < 4) await(&parked, 1); // 三轮分别停在不同真实语句之间。
        else { await(&started, 1); require(airi_sleep(3) == 0, "race actual waiter"); } // 主持锁睡三tick，worker有机会真正登记WAIT。
        require(__atomic_load_n(&guard.contenders, __ATOMIC_SEQ_CST) == (stage == 1 ? 0u : 1u), "race participant position"); // 握手位置必须与算法字段相符。
```

现在主仍持有guard，参与数已经核对。两次45号只包围主自己的unlock：第1轮差值1，只有结束查询；其他三轮差值2，分别是44号和结束查询。worker即使此时被PIT调度，也有自己的计数，不会污染主的测量。

<!-- append: labs/airios/mutex-races.c -->
```c
        uint64_t before = airi_thread_syscalls(); // 只测主的这次unlock，不混入另一执行者的观察回调。
        require(airi_mutex_unlock(&guard) == 0, "race primary release"); // 根据真实参与数省略、空通知或通知一位。
        uint64_t after = airi_thread_syscalls(); // 结束查询自身仍计一次。
        require(after - before == (stage == 1 ? 1u : 2u), "race release syscall count"); // 1只查询，2为44号加查询。
```

主已释放且测量结束，再release打开gate。前三轮等gate的worker从回调返回；第四轮没有gate暂停，由前面的通知恢复，可能已经在主结束测量前运行。它们最终都要回到原lock调用中的CAS，取得并释放guard。join以后才读它的统计：前两轮0次WAIT，第3、4轮各1次；只有第3轮返回-11，只有第4轮返回0。owner和contenders均归零才打印DONE、进入下一轮。

<!-- append: labs/airios/mutex-races.c -->
```c
        atomic_store_explicit(&gate, 1, memory_order_release); // 前三轮从原暂停点继续；第四轮无暂停。
        require(airi_thread_join(tid, &result) == 0 && result == 0, "race worker result"); // 继续原锁调用，不能重开一次调用绕过窗口。
        require(atomic_load_explicit(&calls, memory_order_relaxed) == (stage >= 3 ? 1u : 0u), "race wait call count"); // 第三轮立即失败也是一次实际43号。
        require(atomic_load_explicit(&mismatches, memory_order_relaxed) == (stage == 3 ? 1u : 0u), "race compare mismatch"); // 只有复查以后释放走-11。
        require(atomic_load_explicit(&notifications, memory_order_relaxed) == (stage == 4 ? 1u : 0u), "race notification return"); // 已睡轮必须由真实通知返回0。
        require(__atomic_load_n(&guard.owner, __ATOMIC_SEQ_CST) == 0 && __atomic_load_n(&guard.contenders, __ATOMIC_SEQ_CST) == 0, "race final cleanup"); // 回收工作者后两字段均归零再进入下一轮。
        airi_puts(done[stage - 1]); // 所有状态和调用数均符合才发布本轮结束。
    }
    airi_puts("[MUTEX_RACES_OK] stages=4 actual_wait=1 compare_mismatch=1 normal_library=unchanged\n"); // 四个窗口由同一实际锁实现完成。
    return 0; // shell收取真实程序结果。
}
```

<a id="races-build-boundary"></a>
## 一份源码，两种对象，只有观察ELF选择暂停版

Makefile另建mutex-observe.o，编译时显式加-DAIRI_MUTEX_OBSERVE。mutex-races.elf只链接这份锁对象，不同时链接mutex-lib.o，否则相同函数会重复定义。其余USER_COMMON、线程库、入口和静态链接脚本都与普通程序一致。images.S只打包这个实际ELF，目录注册也只借它的只读字节。

追加构建规则和镜像范围，再将名字注册进现有目录；默认主线仍不读取前端、字体或Cocoa。当前目录容量512足以容纳新增名称，不缩减原来的分段输出。

<!-- append: Makefile -->
```makefile

# 与普通mutex-lib.o来自同一源码，观察点只用于受控交错ELF。
$(OUT)/user/mutex-observe.o: labs/airios/libc/mutex.c Makefile
# 不覆盖普通对象，也不把回调加进USER_COMMON。
	@mkdir -p $(dir $@)
# 只有此对象可引用观察程序定义的暂停函数。
	$(CC) $(USER_FLAGS) -DAIRI_MUTEX_OBSERVE -Ilabs/airios/include -c $< -o $@
# 最终ELF只选观察锁版本，避免同名函数重复定义。
$(OUT)/user/mutex-races.elf: $(USER_COMMON) $(OUT)/user/threads-lib.o $(OUT)/user/thread-entry.o $(OUT)/user/mutex-observe.o $(OUT)/user/mutex-races.o labs/airios/program.ld
# 共用真实用户入口与线程运行库，不借宿主并发。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(filter %.o,$^) -o $@
# 镜像只打包已链接的观察程序。
$(OUT)/labs/airios/images.o: $(OUT)/user/mutex-races.elf
# 本配置路径只供incbin消费。
$(OUT)/labs/airios/images.o: CPPFLAGS += -DAIRI_MUTEX_RACES_ELF='"$(OUT)/user/mutex-races.elf"'
```

<!-- append: labs/airios/images.S -->
```asm
.balign 16 /* 新的观察ELF保持独立只读范围。 */
.global airi_mutex_races_elf_start, airi_mutex_races_elf_end /* 内核目录只借实际文件字节。 */
airi_mutex_races_elf_start: /* 包含完整ELF头与段。 */
.incbin AIRI_MUTEX_RACES_ELF /* 不调用宿主程序冒充观察。 */
airi_mutex_races_elf_end: /* 范围右端不含字节。 */
```

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
extern const uint8_t airi_process_elf_start[], airi_process_elf_end[]; // 新的fork/wait和抢占工作负载。
extern const uint8_t airi_vector_elf_start[], airi_vector_elf_end[]; // 显式偏移向量正常与坏范围观察。
extern const uint8_t airi_malloc_elf_start[], airi_malloc_elf_end[]; // 用户对象分配与页面缓存的独立ELF。
extern const uint8_t airi_identity_elf_start[], airi_identity_elf_end[]; // 原始fork、exec与工作槽复用中的本地TID观察。
extern const uint8_t airi_heap_threads_elf_start[], airi_heap_threads_elf_end[]; // 同一arena并发分配、跨执行者归还与快照。
extern const uint8_t airi_mutex_elf_start[], airi_mutex_elf_end[]; // 原子所有权与futex等待组合的真实用户观察。
```
<!-- after -->
```c
extern const uint8_t airi_process_elf_start[], airi_process_elf_end[]; // 新的fork/wait和抢占工作负载。
extern const uint8_t airi_vector_elf_start[], airi_vector_elf_end[]; // 显式偏移向量正常与坏范围观察。
extern const uint8_t airi_malloc_elf_start[], airi_malloc_elf_end[]; // 用户对象分配与页面缓存的独立ELF。
extern const uint8_t airi_mutex_races_elf_start[], airi_mutex_races_elf_end[]; // 与普通锁独立链接的真实交错观察ELF。
extern const uint8_t airi_identity_elf_start[], airi_identity_elf_end[]; // 原始fork、exec与工作槽复用中的本地TID观察。
extern const uint8_t airi_heap_threads_elf_start[], airi_heap_threads_elf_end[]; // 同一arena并发分配、跨执行者归还与快照。
extern const uint8_t airi_mutex_elf_start[], airi_mutex_elf_end[]; // 原子所有权与futex等待组合的真实用户观察。
```

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    {"vector", airi_vector_elf_start, airi_vector_elf_end}, // 聚合、分散、短前缀与错误原子性。
    {"heap", airi_malloc_elf_start, airi_malloc_elf_end}, // 单线程用户对象与错误路径。
    {"heap-reset", airi_malloc_elf_start, airi_malloc_elf_end}, // 同一ELF新BSS和按需arena。
    {"identity", airi_identity_elf_start, airi_identity_elf_end}, // 版本化运行头和原始查询对照。
    {"identity-reset", airi_identity_elf_start, airi_identity_elf_end}, // 成功exec后沿用PID并重建运行头。
    {"heap-threads", airi_heap_threads_elf_start, airi_heap_threads_elf_end}, // 显式持内部锁yield的用户观察程序。
```
<!-- after -->
```c
    {"vector", airi_vector_elf_start, airi_vector_elf_end}, // 聚合、分散、短前缀与错误原子性。
    {"heap", airi_malloc_elf_start, airi_malloc_elf_end}, // 单线程用户对象与错误路径。
    {"heap-reset", airi_malloc_elf_start, airi_malloc_elf_end}, // 同一ELF新BSS和按需arena。
    {"mutex-races", airi_mutex_races_elf_start, airi_mutex_races_elf_end}, // 暂停点只存在于此观察映像。
    {"identity", airi_identity_elf_start, airi_identity_elf_end}, // 版本化运行头和原始查询对照。
    {"identity-reset", airi_identity_elf_start, airi_identity_elf_end}, // 成功exec后沿用PID并重建运行头。
    {"heap-threads", airi_heap_threads_elf_start, airi_heap_threads_elf_end}, // 显式持内部锁yield的用户观察程序。
```

<a id="races-host-records"></a>
## 按每轮边界核对真正登记和通知

宿主先执行mutex-races，再继续原来的heap-threads和identity。把BEGIN到DONE之间的内容单独切出来：split(begin,1)只切第一次，取右侧，再split(end,1)取左侧，所以每轮不能借其他轮的日志满足检查。先要求每个边界恰一次，再数该范围内真实FUTEX_WAIT和FUTEX_WAKE。

WAIT只允许第4轮一条；WAKE第1轮没有，第2、3轮恰一条woken=0，第4轮恰一条woken=1。第3轮实际请求但不登记的差别，由观察回调对真实返回-11的断言补充。宿主不因为看到RACES_OK就省去这些独立检查，也不把回调中的yield当内核睡眠。

<!-- replace: scripts/shell_test.py -->
<!-- before -->
```python
                assert '[MUTEX_USER_OK] counter=64 critical_yield=1 foreign=denied self=deadlock timeout=pit owner_death=nonrobust' in mutex_output # 所有权、真实等待和非robust寿命都由guest核对。
                assert '[MUTEX_PARTICIPANTS_OK] blocked=2 timeout=withdrawn acquired=withdrawn dead_owner=retained' in mutex_output # 真实竞争和到期均撤销参与者，死亡owner仍保留。
                assert len(re.findall(r'\[FUTEX_WAIT\] pid=',mutex_output))>=4 and 'result=-110' in mutex_output # 两位争用者、超时者和死亡所有者观察确实进入等待。
                offset=len(text); wire.sendall(b'heap-threads\n'); text=wait('[SHELL_COMMAND_DONE] name=heap-threads code=0',offset) # 四个真实执行者共享唯一arena，观察对象交接。
                heap_threads=text[offset:] # 不用旧heap命令代替并发结果。
                assert '[MALLOC_THREADS_OK] workers=4 rounds=96 arena=262144 cross_free=4 snapshots=consistent errno=private critical_yield=1' in heap_threads # 每个工作者和主快照均由guest核对。
```
<!-- after -->
```python
                assert '[MUTEX_USER_OK] counter=64 critical_yield=1 foreign=denied self=deadlock timeout=pit owner_death=nonrobust' in mutex_output # 所有权、真实等待和非robust寿命都由guest核对。
                assert '[MUTEX_PARTICIPANTS_OK] blocked=2 timeout=withdrawn acquired=withdrawn dead_owner=retained' in mutex_output # 真实竞争和到期均撤销参与者，死亡owner仍保留。
                assert len(re.findall(r'\[FUTEX_WAIT\] pid=',mutex_output))>=4 and 'result=-110' in mutex_output # 两位争用者、超时者和死亡所有者观察确实进入等待。
                offset=len(text); wire.sendall(b'mutex-races\n'); text=wait('[SHELL_COMMAND_DONE] name=mutex-races code=0',offset) # 同一用户锁在三个受控窗口及一个真实睡眠位置继续。
                races_output=text[offset:] # 只使用当前命令的实际回调和内核日志。
                assert '[MUTEX_RACES_OK] stages=4 actual_wait=1 compare_mismatch=1 normal_library=unchanged' in races_output # guest已核对真实返回及参与者撤销。
                for stage in range(1,5): # 每轮有独立串口边界，不能互相借日志。
                    begin='[MUTEX_RACE_BEGIN] stage='+str(stage) # str把本轮整数放进固定标记。
                    end='[MUTEX_RACE_DONE] stage='+str(stage) # 只切当前轮结束以前的记录。
                    assert races_output.count(begin)==1 and races_output.count(end)==1 # 不接受缺轮或重复成功。
                    section=races_output.split(begin,1)[1].split(end,1)[0] # 两次最多切一刀，保留这轮真实事件。
                    waits=re.findall(r'\[FUTEX_WAIT\] pid=',section) # -11不登记，因此第三轮也没有WAIT记录。
                    wakes=re.findall(r'\[FUTEX_WAKE\] pid=\d+ address=0x[0-9a-f]+ count=1 woken=(\d+)',section) # 实际通知数量来自内核，不从用户标记猜测。
                    assert len(waits)==(1 if stage==4 else 0) # 只有第四轮真正进入睡眠集合。
                    assert wakes==([] if stage==1 else ['1' if stage==4 else '0']) # 第一轮省略，第二/三轮空通知，第四轮真实唤醒一人。
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
                result.update(per_execution_syscall_count=True,uncontended_mutex_pairs=8,uncontended_mutex_syscalls=0,runtime_tcb_identity=True,identity_reference_queries=7,shared_malloc=True,malloc_workers=4,malloc_rounds=96,owned_mutex=True,mutex_critical_updates=64,private_futex=True,futex_registrations=8,user_heap=True,shared_executions=True,thread_commands=2,worker_capacity=8,worker_reuse_rounds=24) # 关键字把实际已验收字段加入结果字典，不替代上面的逐命令检查。
                result_path.write_text(json.dumps(result,indent=2)+'\n'); print('PASS user shell: alloc/file/async wait/other task/CQ/readback, failed exec, return to monitor') # 阶段演示实际完成。
                return result # 总入口可收集。
            finally: # 包括断言、超时和QEMU错误。
```
<!-- after -->
```python
                offset=len(text); wire.sendall(b'echo after-shell\n'); text=wait('[CONSOLE_UPDATE] view=0 commands=2 page=0',offset) # 回到内核后输入仍完整。
                assert 'after-shell' in text[offset:] # 不只是退出而丢了后续UART。
                result={'passed':True,'ring3_shell':True,'ram_file_and_device_readback':True,'other_user_during_cq_wait':True,'qmp_iops':1,'error_exec_survived':True,'returned_to_monitor':True,'orphan_rounds':24,'memory_reuse_rounds':128,'capacity_rounds':4,'capacity_pages':1024,'simd_preempt_fork_exec':True,'fs_base_preempt_fork_exec':True,'compiler_static_tls':True,'posix_errno_io_subset':True,'positioned_vectors':True} # 只发布已观察事实。
                result.update(mutex_race_stages=4,mutex_race_actual_waits=1,mutex_race_mismatches=1,per_execution_syscall_count=True,uncontended_mutex_pairs=8,uncontended_mutex_syscalls=0,runtime_tcb_identity=True,identity_reference_queries=7,shared_malloc=True,malloc_workers=4,malloc_rounds=96,owned_mutex=True,mutex_critical_updates=64,private_futex=True,futex_registrations=8,user_heap=True,shared_executions=True,thread_commands=2,worker_capacity=8,worker_reuse_rounds=24) # 关键字把实际已验收字段加入结果字典，不替代上面的逐命令检查。
                result_path.write_text(json.dumps(result,indent=2)+'\n'); print('PASS user shell: alloc/file/async wait/other task/CQ/readback, failed exec, return to monitor') # 阶段演示实际完成。
                return result # 总入口可收集。
            finally: # 包括断言、超时和QEMU错误。
```

完整实现现在闭合，运行：

<!-- run -->
```sh
./airios build  # 普通和观察互斥对象分别编译，不混用版本
```

<!-- run -->
```sh
./airios test  # 四个受控交错及既有普通锁/分配器路径一起运行
```

还可以直接核对对象的符号表。沿K56查工具的方法，用当前Clang旁边的llvm-nm读取两个对象；-u只列未定义符号，即这个对象仍需要最终链接者提供的函数。普通对象不应引用观察函数，观察对象则必须引用它，最终由mutex-races.c定义。

<!-- run -->
```sh
python3 - <<'PY' # 引号固定结束词，以下完整Python只读取对象符号
from pathlib import Path  # 替换当前编译器文件名，得到同一LLVM目录的工具
import subprocess  # 以参数列表运行检查程序
import sys  # 加入本项目已有脚本目录
sys.path.insert(0, 'scripts')  # 导入已有工具选择函数，不依赖作者工具
from tools import tool  # 与实际编译使用相同Clang来源
nm = Path(tool('clang', 'llvm')).with_name('llvm-nm')  # 不改变父目录
ordinary = subprocess.check_output([str(nm), '-u', 'build/normal/user/mutex-lib.o'], text=True)  # 普通对象所需的外部符号
observed = subprocess.check_output([str(nm), '-u', 'build/normal/user/mutex-observe.o'], text=True)  # 观察对象所需的外部符号
assert 'airi_mutex_observe' not in ordinary and 'airi_mutex_observe' in observed  # 两种编译边界必须成立
print('PASS ordinary mutex has no observation callback')  # 只在实际符号满足时输出
PY
```

<a id="races-results"></a>
## 同是“等待前释放”，三种结果仍有差别

交互运行`./airios run`、`run sh`，输入`mutex-races`。本轮实际得到：

```text
[MUTEX_RACE_DONE] stage=1 participants=0 wake=skipped wait=none
[MUTEX_RACE_DONE] stage=2 participants=1 wake=empty wait=none
[MUTEX_RACE_DONE] stage=3 participants=1 wake=empty wait=mismatch
[MUTEX_RACE_DONE] stage=4 participants=1 wake=one wait=notified
[MUTEX_RACES_OK] stages=4 actual_wait=1 compare_mismatch=1 normal_library=unchanged
```

第1轮从未登记到内核，释放方也未通知，二次CAS直接取得。第2轮参与数为1却没有内核等待者，空通知随后被复查承接；这不是计数泄漏。第3轮保留原observed进入43号，内核看到owner=0而返回-11，calls为1但没有FUTEX_WAIT。第4轮才有真实等待登记和woken=1，原调用恢复后仍要CAS取得。四个结果分别对应表中的真实位置，不能把“正在竞争”“发出请求”“已经睡下”“获得所有权”混成一个状态。

观察编译故意改变调度时机，不能拿它的yield和握手开销测普通锁延迟。普通对象不含回调，K58的identity仍检查八对无竞争操作没有额外请求，有意争用的mutex/heap仍真实等待。SC顺序是用户协议的推理依据，本节运行只覆盖列出的固定单CPU交错，不声称枚举全部并发情况或完成SMP移植。下一步将这份等待基础用于条件变化通知和有界工作队列。
