# K68 · 释放互斥以后，仍未结束的对象使用

从K67的同一目录继续。变化序号已经能通知等待者，但条件等待还有一次所有权交接：调用者先持有应用互斥，发现条件为假，释放互斥让别人改变条件，回来以后重新取得同一把锁。整个调用没有结束，锁却有一段时间不属于它。

K65的销毁检查只看owner与contenders。两者都为0只能说明此刻没有所有权和锁慢路径，不能证明以后无人还要回来。本节先补这个具体缺口，增加内部存活引用，用一个真正worker经历“持有→登记→释放→重取→归还”。本节尚不实现pthread_cond_wait，也不用夹具的阶段标志冒充条件通知。

<a id="mutex-lifetime-model"></a>
## owner为空，为什么还不能销毁

观察程序的worker在guard下增加wait_users，然后unlock。主看到owner=0、wait_users=1：它能trylock成功，说明引用没有占住普通数据访问权；destroy却必须返回EBUSY，因为原worker尚未完成重取。主再次unlock后，该引用仍是1，不能由主顺手清掉。

主发布resume以后，原worker先lock guard，成功后才归还引用。此时它已经持有core，owner继续阻止销毁；最后unlock、返回、join收拢以后才进入真正静止状态。引用覆盖的是释放到重取的空隙，不替代锁，也不保证任意并发destroy安全。应用仍须停止所有新的访问，不能一边允许别人开始lock，一边靠一次状态检查销毁。

| 时刻 | core.owner | wait_users | 数据访问与销毁 |
| --- | --- | --- | --- |
| worker持有并登记 | worker TID | 1 | worker访问，不能销毁 |
| worker已释放 | 0 | 1 | 别人可以取得，不能销毁 |
| worker重新取得 | worker TID | 1→0 | 先重取再归还，仍不能销毁 |
| worker最后释放并结束 | 0 | 0 | 外部已排除新使用后才可销毁 |

<a id="mutex-lifetime-shape"></a>
## 在对象中保留引用，在内部头中声明配对动作

pthread_mutex_t增加wait_users，静态初始化器和动态init都把它清零。这里修改的是自有源码布局，没有承诺Linux二进制兼容；重新编译本项目所有使用者，不拿旧对象字节与新代码混用。

修改pthread.h的下列窗口。应用仍调用原有pthread接口；新增字段属于运行库内部，不允许应用拿它当计数信号量。

<!-- replace: labs/airios/include/pthread.h -->
<!-- before -->
```c
#define PTHREAD_MUTEX_RECURSIVE 2 // 尚无重入计数，不接受此类型。
#define AIRI_PTHREAD_MUTEX_READY 1u // 仅是用户库初始化标记，不是内核权限或映射有效证明。
typedef struct { int type; unsigned ready; } pthread_mutexattr_t; // 调用者初始化/设置/销毁期间须独占这个属性对象。
typedef struct { airi_mutex core; uint32_t state; } pthread_mutex_t; // core保护普通数据，state只诊断初始化生命周期。
#define PTHREAD_MUTEX_INITIALIZER {AIRI_MUTEX_INITIALIZER, AIRI_PTHREAD_MUTEX_READY} // 在任何并发访问以前建立可用的默认互斥。
#ifdef __cplusplus // C++调用同一C实现。
extern "C" { // 不链接宿主pthread。
#endif // C直接声明。
```
<!-- after -->
```c
#define PTHREAD_MUTEX_RECURSIVE 2 // 尚无重入计数，不接受此类型。
#define AIRI_PTHREAD_MUTEX_READY 1u // 仅是用户库初始化标记，不是内核权限或映射有效证明。
typedef struct { int type; unsigned ready; } pthread_mutexattr_t; // 调用者初始化/设置/销毁期间须独占这个属性对象。
typedef struct { airi_mutex core; uint32_t state, wait_users; } pthread_mutex_t; // core保护普通数据，wait_users保留已释放但还需重取的内部使用者。
#define PTHREAD_MUTEX_INITIALIZER {AIRI_MUTEX_INITIALIZER, AIRI_PTHREAD_MUTEX_READY, 0} // 在任何并发访问以前建立可用的默认互斥。
#ifdef __cplusplus // C++调用同一C实现。
extern "C" { // 不链接宿主pthread。
#endif // C直接声明。
```

新建airi_pthread_internal.h。__airi前缀标明自有库内部入口，不是额外标准pthread API。ref与unref必须属于同一次未完成操作；直接操纵计数不能取得内核权限，也没有逐引用的防伪句柄。本节观察程序显式使用内部头，是为了在完整条件变量加入以前把这一段交接单独运行。

<!-- write: labs/airios/include/airi_pthread_internal.h -->
```c
#ifndef AIRI_PTHREAD_INTERNAL_H // 自有运行库内部协议，应用不得直接操纵引用。
#define AIRI_PTHREAD_INTERNAL_H // 包含保护。
#include <pthread.h> // 当前对象布局，不兼容其他系统pthread ABI。
int __airi_pthread_mutex_wait_ref(pthread_mutex_t *mutex); // 持锁登记，随后才可释放并等待，正错误不改errno。
int __airi_pthread_mutex_wait_unref(pthread_mutex_t *mutex); // 同一次操作重取该锁以后配对归还。
#endif // 这两个入口不承诺标准pthread API。
```

<a id="mutex-lifetime-implementation"></a>
## 所有引用修改都在同一个应用互斥下完成

ref首先验证初始化形状，再比较core.owner与当前airi_thread_id。未持有锁返回正EPERM，引用不变。持有者读取旧计数；到UINT32_MAX返回EAGAIN而不回绕，其余加一。它必须在unlock以前成功，不能先释放再尝试保留一个可能已被销毁的对象。

unref也要求当前持有同一把锁。原worker在无所有权期间调用它会得到EPERM，即使计数非零也不能提前缩短寿命；重取以后计数从1减到0。额外归还遇到0返回EINVAL，保持0，不能下溢。

两个函数的计数修改由core串行，因此无需在这两个入口间再造一把锁。原子读写让destroy检查使用同一种访问方式，不能由此推断已支持并发销毁或SMP。真实条件等待将负责保证配对及调用顺序；当前函数只检查局部所有权和计数，不识别“哪一个引用属于谁”。所有错误直接返回正数，不修改errno。

在pthread_mutex.c加入内部声明和身份接口；init清零，destroy把wait_users也纳入忙检查。然后增加配对函数，原lock、trylock、unlock策略不变：

<!-- replace: labs/airios/libc/pthread_mutex.c -->
<!-- before -->
```c
#include <pthread.h> // 默认ERRORCHECK映射、属性与初始化状态。
#include <errno.h> // pthread直接返回正错误号。
static int aligned(const void *pointer, unsigned alignment) { // 只检查显式形状，不能验证任意映射或对象寿命。
    return pointer && !((uintptr_t)pointer % alignment); // 参数保持有效仍由调用者保证。
```
<!-- after -->
```c
#include <pthread.h> // 默认ERRORCHECK映射、属性与初始化状态。
#include <airi_pthread_internal.h> // 条件等待将使用的内部存活引用，不作为应用接口。
#include <airi_threads.h> // 取得当前运行头的真实身份。
#include <errno.h> // pthread直接返回正错误号。
static int aligned(const void *pointer, unsigned alignment) { // 只检查显式形状，不能验证任意映射或对象寿命。
    return pointer && !((uintptr_t)pointer % alignment); // 参数保持有效仍由调用者保证。
```

<!-- replace: labs/airios/libc/pthread_mutex.c -->
<!-- before -->
```c
    if (!aligned(mutex, _Alignof(pthread_mutex_t)) || (attributes && !attribute_valid(attributes))) return EINVAL; // 不读mutex旧状态，不以未初始化字节猜是否活锁。
    if (attributes && attributes->type != PTHREAD_MUTEX_ERRORCHECK) return ENOTSUP; // 不接入手改的其他策略。
    mutex->core = (airi_mutex)AIRI_MUTEX_INITIALIZER; // 当前独占构造，不与正在使用的锁并发。
    __atomic_store_n(&mutex->state, AIRI_PTHREAD_MUTEX_READY, __ATOMIC_RELEASE); // 形成可用标记，属性指针不保存到对象。
    return 0; // 成功为空闲互斥。
}
int pthread_mutex_destroy(pthread_mutex_t *mutex) { // 调用者已经停止所有将来的锁访问，检查不能替代这个寿命约定。
    if (!initialized(mutex)) return EINVAL; // 当前诊断已销毁状态。
    if (__atomic_load_n(&mutex->core.owner, __ATOMIC_SEQ_CST) || __atomic_load_n(&mutex->core.contenders, __ATOMIC_SEQ_CST)) return EBUSY; // 活所有权或慢路径都不能撤销标记。
    __atomic_store_n(&mutex->state, 0, __ATOMIC_RELEASE); // 仅静止对象销毁，底层两字段已为0。
    return 0; // 可在之后无并发地重新init。
}
```
<!-- after -->
```c
    if (!aligned(mutex, _Alignof(pthread_mutex_t)) || (attributes && !attribute_valid(attributes))) return EINVAL; // 不读mutex旧状态，不以未初始化字节猜是否活锁。
    if (attributes && attributes->type != PTHREAD_MUTEX_ERRORCHECK) return ENOTSUP; // 不接入手改的其他策略。
    mutex->core = (airi_mutex)AIRI_MUTEX_INITIALIZER; // 当前独占构造，不与正在使用的锁并发。
    __atomic_store_n(&mutex->wait_users, 0, __ATOMIC_RELAXED); // 独占构造，尚无释放后重取的使用者。
    __atomic_store_n(&mutex->state, AIRI_PTHREAD_MUTEX_READY, __ATOMIC_RELEASE); // 形成可用标记，属性指针不保存到对象。
    return 0; // 成功为空闲互斥。
}
int pthread_mutex_destroy(pthread_mutex_t *mutex) { // 调用者已经停止所有将来的锁访问，检查不能替代这个寿命约定。
    if (!initialized(mutex)) return EINVAL; // 当前诊断已销毁状态。
    if (__atomic_load_n(&mutex->core.owner, __ATOMIC_SEQ_CST) || __atomic_load_n(&mutex->core.contenders, __ATOMIC_SEQ_CST) || __atomic_load_n(&mutex->wait_users, __ATOMIC_SEQ_CST)) return EBUSY; // 包括暂时不持锁、以后仍须重取的等待者。
    __atomic_store_n(&mutex->state, 0, __ATOMIC_RELEASE); // 仅静止对象销毁，底层两字段已为0。
    return 0; // 可在之后无并发地重新init。
}
```

<!-- replace: labs/airios/libc/pthread_mutex.c -->
<!-- before -->
```c
    long result = airi_mutex_unlock(&mutex->core); // 普通写发布及参与者通知沿已有协议。
    return result < 0 ? (int)-result : 0; // 外人或空锁为EPERM，已经释放的通知错误仍如实报告。
}
```
<!-- after -->
```c
    long result = airi_mutex_unlock(&mutex->core); // 普通写发布及参与者通知沿已有协议。
    return result < 0 ? (int)-result : 0; // 外人或空锁为EPERM，已经释放的通知错误仍如实报告。
}
int __airi_pthread_mutex_wait_ref(pthread_mutex_t *mutex) { // 仅由内部等待协议在仍持有应用锁时登记。
    if (!initialized(mutex)) return EINVAL; // 不引用已销毁对象。
    if (__atomic_load_n(&mutex->core.owner, __ATOMIC_SEQ_CST) != airi_thread_id()) return EPERM; // 未持锁时不允许凭空保留。
    uint32_t count = __atomic_load_n(&mutex->wait_users, __ATOMIC_RELAXED); // 修改均由同一core所有者串行，销毁检查仍原子读。
    if (count == UINT32_MAX) return EAGAIN; // 拒绝回绕，失败不改变引用。
    __atomic_store_n(&mutex->wait_users, count + 1, __ATOMIC_SEQ_CST); // 必须先登记再释放core。
    return 0; // 不改变锁的所有权或errno。
}
int __airi_pthread_mutex_wait_unref(pthread_mutex_t *mutex) { // 同一次内部操作重新取得应用锁以后归还。
    if (!initialized(mutex)) return EINVAL; // 配对期间对象必须仍有效。
    if (__atomic_load_n(&mutex->core.owner, __ATOMIC_SEQ_CST) != airi_thread_id()) return EPERM; // 不能先减引用再去重新争取锁。
    uint32_t count = __atomic_load_n(&mutex->wait_users, __ATOMIC_RELAXED); // 同一core排除其他引用修改者。
    if (!count) return EINVAL; // 配对错误不能下溢为巨大引用数。
    __atomic_store_n(&mutex->wait_users, count - 1, __ATOMIC_SEQ_CST); // 当前已重获core，后续普通unlock完成访问。
    return 0; // 引用不是通知信用，也不授予访问普通数据的权利。
}
```

<a id="mutex-lifetime-worker"></a>
## 原worker回来以前，主只能借用锁，不能撤销对象

新建mutex-lifetime.c。released与resume是观察夹具的独立原子阶段字，await使用100tick总截止和yield让另一执行者运行。它们不保存guard业务数据，也不替代将来的条件队列。

worker设置自己的errno=81，lock并ref，然后unlock。它立即尝试一次过早unref，必须EPERM；之后release发布released=1。主acquire看到这个阶段以后，worker已经释放core且不会在resume前回来争取，因此owner=0、引用=1是同一条真实路径的状态。

<!-- write: labs/airios/mutex-lifetime.c -->
```c
#include <airi_pthread_internal.h> // 显式内部协议观察，尚不冒称条件变量。
#include <airi_threads.h> // 实际身份和单执行者请求计数。
#include <airi_libc.h> // UART、名义tick和调度交接。
#include <errno.h> // 正错误与当前执行者私有errno。
#include <stdatomic.h> // 夹具阶段独立于被观察互斥。
static pthread_mutex_t guard = PTHREAD_MUTEX_INITIALIZER; // 初始引用为0。
static _Atomic unsigned released, resume; // 工作者释放以后，主再验证空闲但不可销毁。
static void require(int condition, const char *message) { // 不把错误路径继续当成功。
    if (!condition) { airi_puts(message); airi_puts("\n"); airi_exit(93); } // 整组终止以免遗留等待者。
}
static void await(_Atomic unsigned *word) { // 夹具本身有界，不拿无限等待掩盖失败。
    uint64_t start = airi_ticks(); // 一次总起点。
    while (!atomic_load_explicit(word, memory_order_acquire)) { // 对方release发布以后才能继续。
        require(airi_ticks() - start < 100, "mutex lifetime handshake timeout"); airi_yield(); // 真正允许另一执行者运行。
    }
}
static void *worker(void *argument) { // 演示一次未来条件等待所需的锁寿命，不模拟通知。
    (void)argument; errno = 81; // 私有错误槽不应被内部协议改动。
    require(pthread_mutex_lock(&guard) == 0 && __airi_pthread_mutex_wait_ref(&guard) == 0, "mutex lifetime acquire reference"); // 先持锁保留。
    require(pthread_mutex_unlock(&guard) == 0 && __airi_pthread_mutex_wait_unref(&guard) == EPERM, "mutex lifetime no early release"); // 引用必须跨越整个无所有权区间。
    atomic_store_explicit(&released, 1, memory_order_release); await(&resume); // 主可以取得这把锁，但不能销毁。
    require(pthread_mutex_lock(&guard) == 0 && __airi_pthread_mutex_wait_unref(&guard) == 0 && __airi_pthread_mutex_wait_unref(&guard) == EINVAL, "mutex lifetime paired return"); // 先重取再归还，额外归还不下溢。
    require(pthread_mutex_unlock(&guard) == 0 && errno == 81, "mutex lifetime worker errno"); // 全部访问结束。
    return (void *)71; // join收取同一真实工作者结果。
}
```

<a id="mutex-lifetime-main"></a>
## 从可用到可销毁，中间还差一次重新取得

main先检查NULL为EINVAL、空闲对象上未经lock的ref为EPERM。创建一位真实worker，等released，再验证owner0且引用1。第一次destroy必须EBUSY，trylock却成功；主unlock以后再次destroy仍EBUSY。引用没有阻塞其他普通使用者，也没有因一次别人借用就消失。

主随后发布resume，原worker完成重取、归还和unlock。join返回71，主核对引用0，此时所有使用者已结束，才成功destroy。动态init再把引用清零，并再次静止销毁。主errno仍71，worker已独立验证81。追加main：

<!-- append: labs/airios/mutex-lifetime.c -->
```c
int main(void) { // 主与worker同时使用同一个静态互斥。
    errno = 71; pthread_t thread; void *result = 0; // 私有结果槽。
    require(__airi_pthread_mutex_wait_ref(0) == EINVAL && __airi_pthread_mutex_wait_ref(&guard) == EPERM, "mutex lifetime invalid registration"); // 显式坏对象和未持有都不登记。
    require(pthread_create(&thread, 0, worker, 0) == 0, "mutex lifetime worker create"); await(&released); // worker此时确已释放core。
    require(__atomic_load_n(&guard.core.owner, __ATOMIC_SEQ_CST) == 0 && __atomic_load_n(&guard.wait_users, __ATOMIC_SEQ_CST) == 1, "mutex lifetime idle owner live reference"); // 空闲不能代表无人再用。
    require(pthread_mutex_destroy(&guard) == EBUSY && pthread_mutex_trylock(&guard) == 0, "mutex lifetime retained but usable"); // 引用阻止撤销对象，但不锁住数据。
    require(pthread_mutex_unlock(&guard) == 0 && pthread_mutex_destroy(&guard) == EBUSY, "mutex lifetime still retained"); // 主释放以后引用仍属于未完成操作。
    atomic_store_explicit(&resume, 1, memory_order_release); // 允许原worker回来重取。
    require(pthread_join(thread, &result) == 0 && result == (void *)71 && !__atomic_load_n(&guard.wait_users, __ATOMIC_SEQ_CST), "mutex lifetime join reclaimed"); // 已配对，当前再无使用者。
    require(pthread_mutex_destroy(&guard) == 0 && pthread_mutex_init(&guard, 0) == 0 && !guard.wait_users && pthread_mutex_destroy(&guard) == 0 && errno == 71, "mutex lifetime quiescent reinit"); // 静止后销毁/重建才合法。
    airi_puts("[MUTEX_LIFETIME_OK] owner=idle reference=live usable=1 destroy=busy reacquire=before_unref errno=preserved\n"); // 只报告本节实际机制。
    return 0; // 不声称已经实现pthread_cond_wait。
}
```

<a id="mutex-lifetime-build"></a>
## 打包同一份用户ELF并关联真实worker

新增对象复用既有pthread创建、互斥、分配器和原始线程入口；没有新增内核系统调用。Makefile只选择当前用户产物，汇编嵌入完整ELF，初始目录注册mutex-lifetime：

<!-- append: Makefile -->
```makefile

# 释放锁期间仍须保留对象的真实用户观察。
$(OUT)/user/mutex-lifetime.o: labs/airios/mutex-lifetime.c
# 目标目录与其他用户对象一致。
	@mkdir -p $(dir $@)
# 同一freestanding ABI，不导入宿主线程库。
	$(CC) $(USER_FLAGS) -Ilabs/airios/include -c $< -o $@
# 引用属于现有互斥实现，复用真正pthread创建与回收。
$(OUT)/user/mutex-lifetime.elf: $(USER_COMMON) $(OUT)/user/threads-lib.o $(OUT)/user/thread-entry.o $(OUT)/user/mutex-lib.o $(OUT)/user/pthread-lib.o $(OUT)/user/pthread-mutex-lib.o $(OUT)/user/posix.o $(OUT)/user/heap-algorithm.o $(OUT)/user/malloc.o $(OUT)/user/mutex-lifetime.o labs/airios/program.ld
# 只选择实际guest对象。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(filter %.o,$^) -o $@
# 完整ELF进入初始目录。
$(OUT)/labs/airios/images.o: $(OUT)/user/mutex-lifetime.elf
# 与汇编incbin保持同一个产物路径。
$(OUT)/labs/airios/images.o: CPPFLAGS += -DAIRI_MUTEX_LIFETIME_ELF='"$(OUT)/user/mutex-lifetime.elf"'
```

<!-- append: labs/airios/images.S -->
```asm
.balign 16 /* 互斥对象引用的实际用户程序。 */
.global airi_mutex_lifetime_elf_start, airi_mutex_lifetime_elf_end /* 只读ELF范围。 */
airi_mutex_lifetime_elf_start: /* 文件头起点。 */
.incbin AIRI_MUTEX_LIFETIME_ELF /* 构建的同一静态产物。 */
airi_mutex_lifetime_elf_end: /* 右端不含。 */
```

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
extern const uint8_t airi_join_elf_start[], airi_join_elf_end[]; // 对等worker收取与有界环检测观察。
extern const uint8_t airi_pthread_mutex_elf_start[], airi_pthread_mutex_elf_end[]; // 默认类型与对象生命周期的用户观察。
extern const uint8_t airi_futex64_elf_start[], airi_futex64_elf_end[]; // 同地址宽度隔离和完整64位比较。
extern const uint8_t airi_events64_elf_start[], airi_events64_elf_end[]; // 完整数值快照与独立推进/通知。
extern const uint8_t airi_identity_elf_start[], airi_identity_elf_end[]; // 原始fork、exec与工作槽复用中的本地TID观察。
extern const uint8_t airi_heap_threads_elf_start[], airi_heap_threads_elf_end[]; // 同一arena并发分配、跨执行者归还与快照。
```
<!-- after -->
```c
extern const uint8_t airi_join_elf_start[], airi_join_elf_end[]; // 对等worker收取与有界环检测观察。
extern const uint8_t airi_pthread_mutex_elf_start[], airi_pthread_mutex_elf_end[]; // 默认类型与对象生命周期的用户观察。
extern const uint8_t airi_futex64_elf_start[], airi_futex64_elf_end[]; // 同地址宽度隔离和完整64位比较。
extern const uint8_t airi_mutex_lifetime_elf_start[], airi_mutex_lifetime_elf_end[]; // 暂释互斥期间的存活引用。
extern const uint8_t airi_events64_elf_start[], airi_events64_elf_end[]; // 完整数值快照与独立推进/通知。
extern const uint8_t airi_identity_elf_start[], airi_identity_elf_end[]; // 原始fork、exec与工作槽复用中的本地TID观察。
extern const uint8_t airi_heap_threads_elf_start[], airi_heap_threads_elf_end[]; // 同一arena并发分配、跨执行者归还与快照。
```

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    {"join", airi_join_elf_start, airi_join_elf_end}, // 同组认领、坏输出及循环等待拒绝。
    {"pthread-mutex", airi_pthread_mutex_elf_start, airi_pthread_mutex_elf_end}, // 公开互斥接口与真实内部等待重试。
    {"futex64", airi_futex64_elf_start, airi_futex64_elf_end}, // 宽键与原四字节键共存。
    {"events64", airi_events64_elf_start, airi_events64_elf_end}, // 高位数据、输出错误和宽通知观察。
    {"identity", airi_identity_elf_start, airi_identity_elf_end}, // 版本化运行头和原始查询对照。
    {"identity-reset", airi_identity_elf_start, airi_identity_elf_end}, // 成功exec后沿用PID并重建运行头。
```
<!-- after -->
```c
    {"join", airi_join_elf_start, airi_join_elf_end}, // 同组认领、坏输出及循环等待拒绝。
    {"pthread-mutex", airi_pthread_mutex_elf_start, airi_pthread_mutex_elf_end}, // 公开互斥接口与真实内部等待重试。
    {"futex64", airi_futex64_elf_start, airi_futex64_elf_end}, // 宽键与原四字节键共存。
    {"mutex-lifetime", airi_mutex_lifetime_elf_start, airi_mutex_lifetime_elf_end}, // 对象仍被操作引用时不能销毁。
    {"events64", airi_events64_elf_start, airi_events64_elf_end}, // 高位数据、输出错误和宽通知观察。
    {"identity", airi_identity_elf_start, airi_identity_elf_end}, // 版本化运行头和原始查询对照。
    {"identity-reset", airi_identity_elf_start, airi_identity_elf_end}, // 成功exec后沿用PID并重建运行头。
```

shell驱动从当前命令的EXEC_COMMIT取得PID，只认这一进程的一次THREAD_CREATE和一次THREAD_RETURN。标记来自guest完成的检查；宿主不直接写guard或阶段字。

<!-- replace: scripts/shell_test.py -->
<!-- before -->
```python
                assert len(wide_waits)==3 and len(narrow_waits)==2 and len({address for address,expected in wide_waits+narrow_waits})==1 # 真正相同VA，不是用不同变量逃开隔离要求。
                assert all(expected==str(0x1122334400000001) for address,expected in wide_waits) and all(expected=='1' for address,expected in narrow_waits) # 高32位保留，窄键只看自己的四字节。
                assert re.findall(r'\[FUTEX64_WAKE\].*count=16 woken=(\d+)',wide_output)==['0','1','1','0'] and re.findall(r'\[FUTEX_WAKE\].*count=16 woken=(\d+)',wide_output)==['1','1'] # 两种先后顺序都各唤醒自己一人。
                offset=len(text); wire.sendall(b'events64\n'); text=wait('[SHELL_COMMAND_DONE] name=events64 code=0',offset) # 完整uint64数据不与负错误混合，推进与通知单独观察。
                wide_events=text[offset:] # 不借前一个宽futex命令的记录。
                assert '[EVENT64_USER_OK] snapshot=full64 errors=separate advance=not_wake false_wake=rechecked broadcast=2 saturation=refused' in wide_events # guest已经检查全1输出与保持、无变化返回、仅推进不唤醒。
```
<!-- after -->
```python
                assert len(wide_waits)==3 and len(narrow_waits)==2 and len({address for address,expected in wide_waits+narrow_waits})==1 # 真正相同VA，不是用不同变量逃开隔离要求。
                assert all(expected==str(0x1122334400000001) for address,expected in wide_waits) and all(expected=='1' for address,expected in narrow_waits) # 高32位保留，窄键只看自己的四字节。
                assert re.findall(r'\[FUTEX64_WAKE\].*count=16 woken=(\d+)',wide_output)==['0','1','1','0'] and re.findall(r'\[FUTEX_WAKE\].*count=16 woken=(\d+)',wide_output)==['1','1'] # 两种先后顺序都各唤醒自己一人。
                offset=len(text); wire.sendall(b'mutex-lifetime\n'); text=wait('[SHELL_COMMAND_DONE] name=mutex-lifetime code=0',offset) # 真实释放core以后仍有存活引用。
                lifetime=text[offset:] # 只检查当前观察程序。
                assert '[MUTEX_LIFETIME_OK] owner=idle reference=live usable=1 destroy=busy reacquire=before_unref errno=preserved' in lifetime # guest验证空闲、重取、配对及errno。
                owner=re.search(r'\[EXEC_COMMIT\] pid=(\d+) program=mutex-lifetime ',lifetime) # 按真实所属进程关联。
                assert owner and len(re.findall(r'\[THREAD_CREATE\] pid='+owner.group(1)+r' tid=',lifetime))==1 and len(re.findall(r'\[THREAD_RETURN\] pid='+owner.group(1)+r' tid=',lifetime))==1 # 不用主执行者代替生命周期交接。
                offset=len(text); wire.sendall(b'events64\n'); text=wait('[SHELL_COMMAND_DONE] name=events64 code=0',offset) # 完整uint64数据不与负错误混合，推进与通知单独观察。
                wide_events=text[offset:] # 不借前一个宽futex命令的记录。
                assert '[EVENT64_USER_OK] snapshot=full64 errors=separate advance=not_wake false_wake=rechecked broadcast=2 saturation=refused' in wide_events # guest已经检查全1输出与保持、无变化返回、仅推进不唤醒。
```

<!-- replace: scripts/shell_test.py -->
<!-- before -->
```python
                offset=len(text); wire.sendall(b'echo after-shell\n'); text=wait('[CONSOLE_UPDATE] view=0 commands=2 page=0',offset) # 回到内核后输入仍完整。
                assert 'after-shell' in text[offset:] # 不只是退出而丢了后续UART。
                result={'passed':True,'ring3_shell':True,'ram_file_and_device_readback':True,'other_user_during_cq_wait':True,'qmp_iops':1,'error_exec_survived':True,'returned_to_monitor':True,'orphan_rounds':24,'memory_reuse_rounds':128,'capacity_rounds':4,'capacity_pages':1024,'simd_preempt_fork_exec':True,'fs_base_preempt_fork_exec':True,'compiler_static_tls':True,'posix_errno_io_subset':True,'positioned_vectors':True} # 只发布已观察事实。
                result.update(wide_event64=True,event64_value_error_separate=True,event64_advance_notify_separate=True,private_futex64=True,futex_width_isolation_rounds=2,pthread_mutex_source=True,pthread_mutex_internal_timeout_retried=True,peer_worker_join=True,join_claim_unique=True,join_cycle_rejected=True,pthread_source_subset=True,pthread_workers=36,pthread_packets_reclaimed=True,bounded_queue=True,queue_capacity=4,queue_jobs=48,queue_close_drains=True,sequence_event=True,event_waits=6,event_broadcast_workers=2,event_false_wake_rechecked=True,mutex_race_stages=4,mutex_race_actual_waits=1,mutex_race_mismatches=1,per_execution_syscall_count=True,uncontended_mutex_pairs=8,uncontended_mutex_syscalls=0,runtime_tcb_identity=True,identity_reference_queries=7,shared_malloc=True,malloc_workers=4,malloc_rounds=96,owned_mutex=True,mutex_critical_updates=64,private_futex=True,futex_registrations=8,user_heap=True,shared_executions=True,thread_commands=2,worker_capacity=8,worker_reuse_rounds=24) # 关键字把实际已验收字段加入结果字典，不替代上面的逐命令检查。
                result_path.write_text(json.dumps(result,indent=2)+'\n'); print('PASS user shell: alloc/file/async wait/other task/CQ/readback, failed exec, return to monitor') # 阶段演示实际完成。
                return result # 总入口可收集。
            finally: # 包括断言、超时和QEMU错误。
```
<!-- after -->
```python
                offset=len(text); wire.sendall(b'echo after-shell\n'); text=wait('[CONSOLE_UPDATE] view=0 commands=2 page=0',offset) # 回到内核后输入仍完整。
                assert 'after-shell' in text[offset:] # 不只是退出而丢了后续UART。
                result={'passed':True,'ring3_shell':True,'ram_file_and_device_readback':True,'other_user_during_cq_wait':True,'qmp_iops':1,'error_exec_survived':True,'returned_to_monitor':True,'orphan_rounds':24,'memory_reuse_rounds':128,'capacity_rounds':4,'capacity_pages':1024,'simd_preempt_fork_exec':True,'fs_base_preempt_fork_exec':True,'compiler_static_tls':True,'posix_errno_io_subset':True,'positioned_vectors':True} # 只发布已观察事实。
                result.update(mutex_wait_lifetime=True,wide_event64=True,event64_value_error_separate=True,event64_advance_notify_separate=True,private_futex64=True,futex_width_isolation_rounds=2,pthread_mutex_source=True,pthread_mutex_internal_timeout_retried=True,peer_worker_join=True,join_claim_unique=True,join_cycle_rejected=True,pthread_source_subset=True,pthread_workers=36,pthread_packets_reclaimed=True,bounded_queue=True,queue_capacity=4,queue_jobs=48,queue_close_drains=True,sequence_event=True,event_waits=6,event_broadcast_workers=2,event_false_wake_rechecked=True,mutex_race_stages=4,mutex_race_actual_waits=1,mutex_race_mismatches=1,per_execution_syscall_count=True,uncontended_mutex_pairs=8,uncontended_mutex_syscalls=0,runtime_tcb_identity=True,identity_reference_queries=7,shared_malloc=True,malloc_workers=4,malloc_rounds=96,owned_mutex=True,mutex_critical_updates=64,private_futex=True,futex_registrations=8,user_heap=True,shared_executions=True,thread_commands=2,worker_capacity=8,worker_reuse_rounds=24) # 关键字把实际已验收字段加入结果字典，不替代上面的逐命令检查。
                result_path.write_text(json.dumps(result,indent=2)+'\n'); print('PASS user shell: alloc/file/async wait/other task/CQ/readback, failed exec, return to monitor') # 阶段演示实际完成。
                return result # 总入口可收集。
            finally: # 包括断言、超时和QEMU错误。
```

完成以上修改后运行：

<!-- run -->
```sh
./airios build  # 构建存活引用及真实交接观察程序
```

<!-- run -->
```sh
./airios test  # 同时回归旧线程互斥和新增释放期间的寿命检查
```

<a id="mutex-lifetime-results"></a>
## 引用保护的是对象继续存在，不是当前访问权限

交互执行`./airios run`、`run sh`、`mutex-lifetime`，实际检查通过后输出：

```text
[MUTEX_LIFETIME_OK] owner=idle reference=live usable=1 destroy=busy reacquire=before_unref errno=preserved
[SHELL_COMMAND_DONE] name=mutex-lifetime code=0
```

本节只建立条件等待所需的对象寿命部分。后续还要在应用锁下取得变化快照、登记条件使用者、释放并真正等待、通知后重新取得应用锁；不能把这里的resume夹具当已完成这些动作。

释放与阻塞、成功返回前重取锁的接口语义参照[POSIX条件等待](https://man7.org/linux/man-pages/man3/pthread_cond_wait.3p.html)。本节内部引用不属于该标准的公开类型；初始化/销毁仍要求外部控制寿命，未增加取消、robust互斥、SMP或Linux ABI承诺。
