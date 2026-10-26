# K65 · pthread互斥的默认类型与对象寿命

已有airi_mutex_lock接收有限tick预算，失败返回负错误。现在给pthread源码提供lock、trylock、unlock及静态／动态初始化，继续运行同一个底层锁。公开pthread_mutex_lock没有总超时参数，内部一次等待到期不能直接变成公开失败；默认重入和错误所有者行为也必须明确。

从 K64 的同一目录继续。本节将DEFAULT映到已实现的ERRORCHECK，提供互斥属性类型的初始化、查询、设置和销毁。NORMAL、RECURSIVE等未支持策略明确拒绝，不用相同名字冒充其他行为。线程创建的pthread_attr_t仍与这里的pthread_mutexattr_t不同，create的非NULL属性限制没有因此解除。

<a id="pmutex-default-policy"></a>
## 默认映射决定自锁和错误释放怎样报告

已有AiriMutex保存当前TID，自己重复lock返回-35，外人unlock返回-1。这恰好适合作为ERRORCHECK：公开包装分别报告EDEADLK和EPERM，trylock遇到任意持有者都为EBUSY。它没有递归层数，不能把第二次取得悄悄算成成功。

POSIX的DEFAULT允许实现映到其他标准互斥类型，本项目明确选ERRORCHECK；这一映射可对照 [互斥类型与重入行为](https://man7.org/linux/man-pages/man3/pthread_mutex_lock.3p.html)。可移植代码不能据此推断其他实现默认也一样。NORMAL的自锁死锁语义与RECURSIVE的层数尚未实现，因此设置它们返回ENOTSUP；未知类型返回EINVAL，均保留旧属性。

pthread_mutex_t嵌入AiriMutex和一个初始化标记state，不在堆上另分配锁。PTHREAD_MUTEX_INITIALIZER在发布以前建立可用对象；动态init也只接受尚未发布或已静止销毁的对象，不能通过读随机旧字节判断“这块未初始化内存是不是一把活锁”。state只帮助诊断部分已知误用，不能验证任意映射、对象寿命或内核权限。

更新pthread.h。属性有type与ready两个字段，设置／销毁期间调用者独占；互斥的并发操作只读自己的state，不反复读取原属性对象。

<!-- replace: labs/airios/include/pthread.h -->
<!-- before -->
```c
#ifndef AIRI_PTHREAD_H // 当前线程源码子集，不声明完整POSIX支持。
#define AIRI_PTHREAD_H // 包含保护。
#include <stdint.h> // 身份载体与指针往返保持目标宽度。
typedef uintptr_t pthread_t; // 当前实现用真实TID作不透明身份，应用通过pthread_equal比较。
typedef struct { uint64_t reserved; } pthread_attr_t; // 仅保留类型位置，当前create只接受NULL属性，不解析此对象。
#ifdef __cplusplus // C++调用同一C实现。
extern "C" { // 不链接宿主pthread。
#endif // C直接声明。
```
<!-- after -->
```c
#ifndef AIRI_PTHREAD_H // 当前线程源码子集，不声明完整POSIX支持。
#define AIRI_PTHREAD_H // 包含保护。
#include <stdint.h> // 身份载体与指针往返保持目标宽度。
#include <airi_mutex.h> // 标准形状的互斥仍由自有所有权锁执行。
typedef uintptr_t pthread_t; // 当前实现用真实TID作不透明身份，应用通过pthread_equal比较。
typedef struct { uint64_t reserved; } pthread_attr_t; // 仅保留类型位置，当前create只接受NULL属性，不解析此对象。
#define PTHREAD_MUTEX_NORMAL 0 // 已知类型名，当前属性设置明确不支持。
#define PTHREAD_MUTEX_ERRORCHECK 1 // 当前实现的所有权错误检查类型。
#define PTHREAD_MUTEX_DEFAULT PTHREAD_MUTEX_ERRORCHECK // 本项目将DEFAULT映到ERRORCHECK，不冒充NORMAL语义。
#define PTHREAD_MUTEX_RECURSIVE 2 // 尚无重入计数，不接受此类型。
#define AIRI_PTHREAD_MUTEX_READY 1u // 仅是用户库初始化标记，不是内核权限或映射有效证明。
typedef struct { int type; unsigned ready; } pthread_mutexattr_t; // 调用者初始化/设置/销毁期间须独占这个属性对象。
typedef struct { airi_mutex core; uint32_t state; } pthread_mutex_t; // core保护普通数据，state只诊断初始化生命周期。
#define PTHREAD_MUTEX_INITIALIZER {AIRI_MUTEX_INITIALIZER, AIRI_PTHREAD_MUTEX_READY} // 在任何并发访问以前建立可用的默认互斥。
#ifdef __cplusplus // C++调用同一C实现。
extern "C" { // 不链接宿主pthread。
#endif // C直接声明。
```

<!-- replace: labs/airios/include/pthread.h -->
<!-- before -->
```c
int pthread_join(pthread_t thread, void **result); // 同组worker结果唯一收取，NULL丢弃；主执行者暂不可作目标，成功才写私人输出。
pthread_t pthread_self(void); // 当前有效运行头提供身份，不能跨进程当授权句柄。
int pthread_equal(pthread_t left, pthread_t right); // 相同非零，不同0，不将可移植比较写成数值猜测。
#ifdef __cplusplus // 结束C链接声明。
} // 类型布局保持本项目约定，不兼容Linux二进制。
#endif // 结束语言差异。
```
<!-- after -->
```c
int pthread_join(pthread_t thread, void **result); // 同组worker结果唯一收取，NULL丢弃；主执行者暂不可作目标，成功才写私人输出。
pthread_t pthread_self(void); // 当前有效运行头提供身份，不能跨进程当授权句柄。
int pthread_equal(pthread_t left, pthread_t right); // 相同非零，不同0，不将可移植比较写成数值猜测。
int pthread_mutexattr_init(pthread_mutexattr_t *attributes); // 默认ERRORCHECK映射，不分配。
int pthread_mutexattr_destroy(pthread_mutexattr_t *attributes); // 此属性不再被并发使用时撤销可用标记。
int pthread_mutexattr_gettype(const pthread_mutexattr_t *attributes, int *type); // 成功才写私人输出。
int pthread_mutexattr_settype(pthread_mutexattr_t *attributes, int type); // DEFAULT/ERRORCHECK接受，NORMAL/RECURSIVE不伪造支持。
int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attributes); // 未发布对象初始化，不覆盖活锁。
int pthread_mutex_destroy(pthread_mutex_t *mutex); // 调用者先确保无人使用；检测持有/慢路径返回EBUSY。
int pthread_mutex_lock(pthread_mutex_t *mutex); // 无总超时，内部有限等待重试；重入EDEADLK，不改errno。
int pthread_mutex_trylock(pthread_mutex_t *mutex); // 一次争取，忙EBUSY。
int pthread_mutex_unlock(pthread_mutex_t *mutex); // 错误所有者EPERM，成功0，不改errno。
#ifdef __cplusplus // 结束C链接声明。
} // 类型布局保持本项目约定，不兼容Linux二进制。
#endif // 结束语言差异。
```

<a id="pmutex-attributes"></a>
## 属性先构造，成功后才提交新类型

新建pthread_mutex.c，先放三个形状与状态助手。aligned只检查NULL与对齐，不替代有效对象合同；attribute_valid用于已经初始化的属性，initialized用于已经发布的互斥。并发lock只读state，init/destroy仍须由调用者排除并发。

<!-- write: labs/airios/libc/pthread_mutex.c -->
```c
#include <pthread.h> // 默认ERRORCHECK映射、属性与初始化状态。
#include <errno.h> // pthread直接返回正错误号。
static int aligned(const void *pointer, unsigned alignment) { // 只检查显式形状，不能验证任意映射或对象寿命。
    return pointer && !((uintptr_t)pointer % alignment); // 参数保持有效仍由调用者保证。
}
static int attribute_valid(const pthread_mutexattr_t *attributes) { // 属性变更和销毁必须由调用者排除并发。
    return aligned(attributes, _Alignof(pthread_mutexattr_t)) && attributes->ready == 1; // 不把销毁后的属性当默认。
}
static int initialized(pthread_mutex_t *mutex) { // 并发锁操作只读同一可用标记，不与init/destroy竞赛。
    return aligned(mutex, _Alignof(pthread_mutex_t)) && __atomic_load_n(&mutex->state, __ATOMIC_ACQUIRE) == AIRI_PTHREAD_MUTEX_READY; // 状态不是内核授权。
}
```

属性初始化本身不能调用attribute_valid去读随机旧ready；它直接构造DEFAULT和ready=1。destroy则用于已经初始化的属性，将ready清0，调用者必须先保证没有并发读写。

<!-- append: labs/airios/libc/pthread_mutex.c -->
```c
int pthread_mutexattr_init(pthread_mutexattr_t *attributes) { // 不读取未初始化对象的旧内容。
    if (!aligned(attributes, _Alignof(pthread_mutexattr_t))) return EINVAL; // 显式坏形状先拒绝。
    *attributes = (pthread_mutexattr_t){PTHREAD_MUTEX_DEFAULT, 1}; // 独占构造，类型与可用标记一起形成。
    return 0; // 不申请内存、不改errno。
}
int pthread_mutexattr_destroy(pthread_mutexattr_t *attributes) { // 属性无需跟随已初始化mutex一起存活。
    if (!attribute_valid(attributes)) return EINVAL; // 当前实现检测销毁后的再次使用。
    attributes->ready = 0; // 调用者已确保无人并发读写此属性。
    return 0; // 类型字段不再授权后续操作。
}
```

属性可用以后，gettype才验证私人输出并写入实际type。settype先分清“已知未实现”和“无效值”，全部通过才改type；拒绝NORMAL、RECURSIVE或未知值时，旧属性保持。

<!-- append: labs/airios/libc/pthread_mutex.c -->
```c
int pthread_mutexattr_gettype(const pthread_mutexattr_t *attributes, int *type) { // 输出只在成功以后改动。
    if (!attribute_valid(attributes) || !aligned(type, _Alignof(int))) return EINVAL; // 不把失败写成一个伪造默认类型。
    *type = attributes->type; // 私人有效输出，不能与属性重叠。
    return 0; // 正常返回不触碰errno。
}
int pthread_mutexattr_settype(pthread_mutexattr_t *attributes, int type) { // 当前只实现一种所有权策略。
    if (!attribute_valid(attributes)) return EINVAL; // 坏属性不是未支持类型。
    if (type == PTHREAD_MUTEX_NORMAL || type == PTHREAD_MUTEX_RECURSIVE) return ENOTSUP; // 已知但尚未实现，不改旧属性。
    if (type != PTHREAD_MUTEX_ERRORCHECK) return EINVAL; // DEFAULT与ERRORCHECK是同一个本项目值。
    attributes->type = type; // 全部检查成功以后才提交。
    return 0; // 不修改其他对象。
}
```

<a id="pmutex-object-lifetime"></a>
## init不覆盖活锁，destroy也不能替调用者停止别人

mutex_init检查目标形状和可选属性，只接受当前ERRORCHECK策略，清底层owner／contenders，再发布可用标记。不保存属性指针，所以属性可以随后销毁，互斥仍保持已经确定的行为。当前只有一种策略，不需要为尚未支持的类型预建分派。

destroy先检查可用状态，再原子读取owner与contenders，非零返回EBUSY且不撤销标记；静止空闲时将state清0，之后可以重新init。调用者必须确保没有人还在使用或即将进入这个对象，不能把一次读到零当成允许与另一个lock并发销毁的证明。检查状态与下一次使用之间仍可能出现新调用，排除它属于应用的对象寿命管理。

静止销毁和重新初始化的边界可对照 [互斥对象寿命](https://man7.org/linux/man-pages/man3/pthread_mutex_destroy.3p.html)。本实现对已销毁对象返回EINVAL、对检测到的忙返回EBUSY，属于诊断；可移植正常程序不应依赖先误用已销毁对象再观察返回值。下面的观察会有意演示这类失败并给出完整结果。

<!-- append: labs/airios/libc/pthread_mutex.c -->
```c
int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attributes) { // 调用者必须在发布以前或销毁后静止地初始化。
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

<a id="pmutex-unbounded-lock"></a>
## 底层一次到期，公开lock仍继续等待

pthread_mutex_lock只有互斥参数，没有调用者给出的期限，因此不能把airi_mutex_lock(&core,200)的-110直接翻译成ETIMEDOUT返回。包装重复这次有限等待，仅对-ETIMEDOUT重试；真正取得才返回0，其他明确错误转正返回。self重入-35立即变EDEADLK，不进入无限超时重试。

每次底层尝试仍有自己的固定截止和登记／撤销，不把等待记录永久留在内核。长期被占用时，公开调用可以继续等待，并让持有者获得CPU释放。当前非robust锁若持有者退出而不释放，仍可能永久等待；内部200tick不是自动修复死亡所有者的机制，也不应伪造成公开超时合同。

trylock只做一次争取，unlock保留真实TID所有权、普通数据发布和参与者通知。这些原始操作不设置errno，包装也不调用会修改它的分配函数，因此只需将负错误转正，不额外保存错误槽。

<!-- append: labs/airios/libc/pthread_mutex.c -->
```c
int pthread_mutex_lock(pthread_mutex_t *mutex) { // POSIX形状的lock没有调用者给出的总超时。
    if (!initialized(mutex)) return EINVAL; // 不把销毁后的对象当一把新锁。
    long result; // 内部原始负错误，与公开正错误分开。
    do result = airi_mutex_lock(&mutex->core, 200); // 每次底层等待有界，超时不等于公开lock失败。
    while (result == -ETIMEDOUT); // 持有者仍可继续运行释放，持续争取直到取得或明确其他错误。
    return result < 0 ? (int)-result : 0; // self重入变EDEADLK，成功才持有普通数据访问权。
}
int pthread_mutex_trylock(pthread_mutex_t *mutex) { // 非阻塞只尝试一次。
    if (!initialized(mutex)) return EINVAL; // 未生效对象不作原子争取。
    long result = airi_mutex_trylock(&mutex->core); // 自己已持有也为忙，不递归计数。
    return result < 0 ? (int)-result : 0; // EBUSY或成功0，errno不变。
}
int pthread_mutex_unlock(pthread_mutex_t *mutex) { // 释放仍依赖底层TID所有权。
    if (!initialized(mutex)) return EINVAL; // 不绕过销毁标记。
    long result = airi_mutex_unlock(&mutex->core); // 普通写发布及参与者通知沿已有协议。
    return result < 0 ? (int)-result : 0; // 外人或空锁为EPERM，已经释放的通知错误仍如实报告。
}
```

<a id="pmutex-ordinary-workers"></a>
## 让公开接口保护两份普通共享数据

新建pthread-mutex-probe.c。guard使用静态默认初始化；counter与checksum是普通变量，只在持锁时访问。ready/finished单独用原子发布夹具进度，不借counter的未加锁读取猜worker是否已经运行。

worker先确认自己的errno0，设为91，在主仍持锁时尝试外人释放和trylock，分别要求EPERM、EBUSY。随后发布ready，调用公开lock，取得以后读取旧counter和校验，故意持锁yield，再更新两份数据并释放。每人32次，最后join返回原tokens指针。yield在临界区里是为了让另一位实际面对竞争，不是锁实现的忙等。

<!-- write: labs/airios/pthread-mutex-probe.c -->
```c
#include <pthread.h> // 同时使用真实线程及标准形状的互斥。
#include <airi_threads.h> // 只为实际请求计数，不替代pthread锁操作。
#include <airi_libc.h> // 名义时钟、串口和临界区主动交接。
#include <errno.h> // 所有pthread返回不得覆盖当前错误哨兵。
#include <stdatomic.h> // 夹具开始/结束独立发布，不借受保护普通数据通信。
static pthread_mutex_t guard = PTHREAD_MUTEX_INITIALIZER; // 本项目DEFAULT映为ERRORCHECK，发布前静态构造。
static unsigned counter, checksum = 0x5a5a, tokens[2]; // 普通共享数据由guard保护，参数本体由主持有。
static _Atomic unsigned ready, finished; // 主持锁时确认两位已开始但尚未完成。
static void require(int condition, const char *message) { // 错误终止本真实用户组。
    if (!condition) { airi_puts(message); airi_puts("\n"); airi_exit(90); } // 不在失败后输出成功。
}
static void *worker(void *argument) { // 两位都通过pthread入口取得独立TLS。
    require(errno == 0, "pthread mutex fresh errno"); errno = 91; // 之后所有锁操作都应保持这个值。
    require(pthread_mutex_unlock(&guard) == EPERM && pthread_mutex_trylock(&guard) == EBUSY, "pthread mutex foreign owner"); // 主仍持锁，外人不能释放。
    atomic_fetch_add_explicit(&ready, 1, memory_order_release); // 只表示即将进入真正阻塞lock。
    for (unsigned i = 0; i < 32; ++i) { // 每人32次普通状态更新。
        require(pthread_mutex_lock(&guard) == 0 && errno == 91, "pthread mutex blocking acquisition"); // 内部200tick到期不得泄漏成公开ETIMEDOUT。
        unsigned before = counter; require(checksum == (before ^ 0x5a5a), "pthread mutex protected checksum"); // 同一次临界区看一致数据。
        airi_yield(); // 故意持锁交接，另一位必须竞争而非读半成品。
        counter = before + 1; checksum = counter ^ 0x5a5a; // 先后写两个普通对象，下一持有者一起看到。
        require(pthread_mutex_unlock(&guard) == 0 && errno == 91, "pthread mutex release errno"); // 正常和竞争通知均不改errno。
    }
    atomic_fetch_add_explicit(&finished, 1, memory_order_release); // 不再访问guard以后才发布完成。
    return argument; // main仍持有参数，join取得原指针。
}
```

<a id="pmutex-dynamic-observation"></a>
## 属性寿命结束以后，动态互斥继续有效

main的attributes和dynamic开始尚未初始化，只交给init，不先查询旧标记。默认类型应为ERRORCHECK；设置NORMAL、RECURSIVE、99分别被拒绝，随后gettype仍为原值。用这份属性初始化dynamic，再销毁attributes，dynamic仍可取得和释放。

<!-- append: labs/airios/pthread-mutex-probe.c -->
```c
int main(void) { // 静态与动态对象分别观察，均不并发init/destroy。
    errno = 71; pthread_mutexattr_t attributes; pthread_mutex_t dynamic; int type = 99; // 未初始化对象只交init，不先读取旧标记。
    require(pthread_mutexattr_init(0) == EINVAL && pthread_mutexattr_init(&attributes) == 0, "pthread mutex attribute init"); // 显式坏形状不影响下一次正常构造。
    require(pthread_mutexattr_gettype(&attributes, &type) == 0 && type == PTHREAD_MUTEX_ERRORCHECK, "pthread mutex default mapping"); // 不冒充NORMAL行为。
    require(pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_NORMAL) == ENOTSUP && pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_RECURSIVE) == ENOTSUP && pthread_mutexattr_settype(&attributes, 99) == EINVAL, "pthread mutex unsupported policies"); // 失败必须保持旧类型。
    require(pthread_mutexattr_gettype(&attributes, &type) == 0 && type == PTHREAD_MUTEX_ERRORCHECK && pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_ERRORCHECK) == 0, "pthread mutex type unchanged"); // 只接受真正实现的策略。
    require(pthread_mutex_init(&dynamic, &attributes) == 0 && pthread_mutexattr_destroy(&attributes) == 0, "pthread mutex copies policy"); // 互斥不保存属性指针。
    type = 99; require(pthread_mutexattr_gettype(&attributes, &type) == EINVAL && type == 99, "pthread mutex dead attributes"); // 当前诊断扩展，失败不写输出。
```

取得dynamic以后，自锁应报EDEADLK，trylock为EBUSY，destroy也为EBUSY并保留对象可用。释放后静止destroy成功，故意再次lock得到本实现EINVAL诊断；坏属性不能重新构造，而NULL默认属性可以。属性gettype失败时，私人type哨兵99保持，所有pthread操作都不能改变主errno71。

<!-- append: labs/airios/pthread-mutex-probe.c -->
```c
    require(pthread_mutex_lock(&dynamic) == 0 && pthread_mutex_lock(&dynamic) == EDEADLK && pthread_mutex_trylock(&dynamic) == EBUSY && pthread_mutex_destroy(&dynamic) == EBUSY, "pthread mutex self and busy destroy"); // 忙时不撤销可用标记。
    require(pthread_mutex_unlock(&dynamic) == 0 && pthread_mutex_destroy(&dynamic) == 0 && pthread_mutex_lock(&dynamic) == EINVAL, "pthread mutex destroy diagnostics"); // 销毁后使用是当前明确诊断，不能当可移植正常用法。
    require(pthread_mutex_init(&dynamic, &attributes) == EINVAL && pthread_mutex_init(&dynamic, 0) == 0, "pthread mutex quiescent reinit"); // 坏属性不构造，NULL默认可重新构造静止对象。
```

接着沿K57同样的两次45号观察，运行八对公共pthread_mutex_lock／unlock。有效state检查是用户读，底层首次CAS成功不读时钟，释放时没有参与者也不通知，所以差值仍只为结束查询的一次。打印FAST在测量以后，静止destroy也在结束快照以后执行。

<!-- append: labs/airios/pthread-mutex-probe.c -->
```c
    uint64_t before = airi_thread_syscalls(); // 只包围公共互斥的无竞争操作。
    for (unsigned i = 0; i < 8; ++i) require(pthread_mutex_lock(&dynamic) == 0 && pthread_mutex_unlock(&dynamic) == 0, "pthread mutex fast pairs"); // 初始化标记不需要额外陷入。
    uint64_t after = airi_thread_syscalls(); // 结束查询本身计一次。
    require(after - before == 1 && pthread_mutex_destroy(&dynamic) == 0 && errno == 71, "pthread mutex fast counter"); // 八对锁没有额外请求。
    airi_puts("[PTHREAD_MUTEX_FAST] pairs=8 delta=1\n"); // 打印发生在测量窗口外。
```

<a id="pmutex-internal-timeout"></a>
## 持有205tick，让内部200tick到期真的发生

最后主持有静态guard再创建两位worker。等ready=2以后，主睡205个名义tick但不释放guard；两位都不能更新数据，底层200tick等待会实际到期并撤销，再由公开包装重新等待。主恢复时finished仍应为0，证明没有worker把一次内部到期误当取得。

205只是当前已交付PIT事件的名义计数，不是精确墙钟保证。

<!-- append: labs/airios/pthread-mutex-probe.c -->
```c
    require(pthread_mutex_lock(&guard) == 0 && pthread_mutex_destroy(&guard) == EBUSY, "pthread mutex primary ownership"); // 主持有时创建两竞争者。
    pthread_t threads[2]; void *result; // 两位实际线程分别收取。
    for (unsigned i = 0; i < 2; ++i) require(pthread_create(&threads[i], 0, worker, &tokens[i]) == 0, "pthread mutex workers"); // 启动包仍由现有适配器交接。
    uint64_t start = airi_ticks(); // 夹具准备也有总期限。
    while (atomic_load_explicit(&ready, memory_order_acquire) != 2) { require(airi_ticks() - start < 100, "pthread mutex contenders ready"); airi_yield(); } // 给两位实际执行机会。
    require(airi_sleep(205) == 0 && atomic_load_explicit(&finished, memory_order_acquire) == 0, "pthread mutex internal timeout retry"); // 超过底层200tick仍未取得，公开调用应继续等待。
```

主释放以后，两人的公开lock取得成功，完成合计64次普通更新并返回原参数；主join取得各自参数。再持锁检查counter64和对应checksum，释放并在所有使用者结束后destroy。自动会话还会要求实际result=-110及sleep ticks=205记录，不能只根据源码中写了200就宣称测试了重试。

<!-- append: labs/airios/pthread-mutex-probe.c -->
```c
    require(pthread_mutex_unlock(&guard) == 0, "pthread mutex opens critical section"); // 之后两人才能更新普通数据。
    for (unsigned i = 0; i < 2; ++i) require(pthread_join(threads[i], &result) == 0 && result == &tokens[i], "pthread mutex worker results"); // 正常收拢以后才能销毁。
    require(pthread_mutex_lock(&guard) == 0 && counter == 64 && checksum == (64 ^ 0x5a5a) && pthread_mutex_unlock(&guard) == 0, "pthread mutex final ordinary state"); // 所有更新完整且未丢失。
    require(pthread_mutex_destroy(&guard) == 0 && errno == 71, "pthread mutex final lifecycle"); // 所有使用者已结束，不把检测当并发销毁许可。
    airi_puts("[PTHREAD_MUTEX_USER_OK] default=errorcheck counter=64 critical_yield=1 timeout=internal init=dynamic destroy=checked errno=preserved\n"); // 正常、错误与生命周期完整结束。
    return 0; // shell取得真实成功结果。
}
```

<a id="pmutex-build-evidence"></a>
## 同一底层锁，新公开包装只在选用时链接

pthread-mutex-lib.o编译新包装，pthread-mutex.elf同时链接原有线程、malloc、普通mutex和观察main。旧airi_mutex有界接口保持不变，原K54超时演示仍应返回-110；只有新的pthread无总超时入口做内部重试。不能修改底层把所有超时都吞掉，破坏原有合同。

<!-- append: Makefile -->
```makefile

# 标准形状的互斥包装只在选用时链接，不替换底层有界AiriMutex。
$(OUT)/user/pthread-mutex-lib.o: labs/airios/libc/pthread_mutex.c Makefile
# 与完整观察main的对象独立命名。
	@mkdir -p $(dir $@)
# 仍使用自有pthread头及当前静态用户目标。
	$(CC) $(USER_FLAGS) -Ilabs/airios/include -c $< -o $@
# 线程启动包、普通锁与公开包装全部真实运行于guest。
$(OUT)/user/pthread-mutex.elf: $(USER_COMMON) $(OUT)/user/posix.o $(OUT)/user/heap-algorithm.o $(OUT)/user/malloc.o $(OUT)/user/mutex-lib.o $(OUT)/user/threads-lib.o $(OUT)/user/thread-entry.o $(OUT)/user/pthread-lib.o $(OUT)/user/pthread-mutex-lib.o $(OUT)/user/pthread-mutex-probe.o labs/airios/program.ld
# 不链接宿主同步库或观察暂停版锁。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(filter %.o,$^) -o $@
# 只借链接完成的只读映像。
$(OUT)/labs/airios/images.o: $(OUT)/user/pthread-mutex.elf
# 本配置路径交给incbin。
$(OUT)/labs/airios/images.o: CPPFLAGS += -DAIRI_PTHREAD_MUTEX_ELF='"$(OUT)/user/pthread-mutex.elf"'
```

<!-- append: labs/airios/images.S -->
```asm
.balign 16 /* 公开互斥源码接口的真实观察ELF。 */
.global airi_pthread_mutex_elf_start, airi_pthread_mutex_elf_end /* 初始目录借用完整边界。 */
airi_pthread_mutex_elf_start: /* 从ELF文件头开始。 */
.incbin AIRI_PTHREAD_MUTEX_ELF /* 自有线程、锁与包装，不执行主机例子。 */
airi_pthread_mutex_elf_end: /* 范围右端不含字节。 */
```

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
extern const uint8_t airi_queue_elf_start[], airi_queue_elf_end[]; // 队列算法位于用户态，内核继续提供既有线程/等待。
extern const uint8_t airi_pthread_elf_start[], airi_pthread_elf_end[]; // POSIX形状的用户包装不替代真实共享执行者。
extern const uint8_t airi_join_elf_start[], airi_join_elf_end[]; // 对等worker收取与有界环检测观察。
extern const uint8_t airi_identity_elf_start[], airi_identity_elf_end[]; // 原始fork、exec与工作槽复用中的本地TID观察。
extern const uint8_t airi_heap_threads_elf_start[], airi_heap_threads_elf_end[]; // 同一arena并发分配、跨执行者归还与快照。
extern const uint8_t airi_mutex_elf_start[], airi_mutex_elf_end[]; // 原子所有权与futex等待组合的真实用户观察。
```
<!-- after -->
```c
extern const uint8_t airi_queue_elf_start[], airi_queue_elf_end[]; // 队列算法位于用户态，内核继续提供既有线程/等待。
extern const uint8_t airi_pthread_elf_start[], airi_pthread_elf_end[]; // POSIX形状的用户包装不替代真实共享执行者。
extern const uint8_t airi_join_elf_start[], airi_join_elf_end[]; // 对等worker收取与有界环检测观察。
extern const uint8_t airi_pthread_mutex_elf_start[], airi_pthread_mutex_elf_end[]; // 默认类型与对象生命周期的用户观察。
extern const uint8_t airi_identity_elf_start[], airi_identity_elf_end[]; // 原始fork、exec与工作槽复用中的本地TID观察。
extern const uint8_t airi_heap_threads_elf_start[], airi_heap_threads_elf_end[]; // 同一arena并发分配、跨执行者归还与快照。
extern const uint8_t airi_mutex_elf_start[], airi_mutex_elf_end[]; // 原子所有权与futex等待组合的真实用户观察。
```

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    {"queue", airi_queue_elf_start, airi_queue_elf_end}, // 有界生产消费、关闭排空与背压。
    {"pthread", airi_pthread_elf_start, airi_pthread_elf_end}, // 正错误、启动包和指针返回观察。
    {"join", airi_join_elf_start, airi_join_elf_end}, // 同组认领、坏输出及循环等待拒绝。
    {"identity", airi_identity_elf_start, airi_identity_elf_end}, // 版本化运行头和原始查询对照。
    {"identity-reset", airi_identity_elf_start, airi_identity_elf_end}, // 成功exec后沿用PID并重建运行头。
    {"heap-threads", airi_heap_threads_elf_start, airi_heap_threads_elf_end}, // 显式持内部锁yield的用户观察程序。
```
<!-- after -->
```c
    {"queue", airi_queue_elf_start, airi_queue_elf_end}, // 有界生产消费、关闭排空与背压。
    {"pthread", airi_pthread_elf_start, airi_pthread_elf_end}, // 正错误、启动包和指针返回观察。
    {"join", airi_join_elf_start, airi_join_elf_end}, // 同组认领、坏输出及循环等待拒绝。
    {"pthread-mutex", airi_pthread_mutex_elf_start, airi_pthread_mutex_elf_end}, // 公开互斥接口与真实内部等待重试。
    {"identity", airi_identity_elf_start, airi_identity_elf_end}, // 版本化运行头和原始查询对照。
    {"identity-reset", airi_identity_elf_start, airi_identity_elf_end}, // 成功exec后沿用PID并重建运行头。
    {"heap-threads", airi_heap_threads_elf_start, airi_heap_threads_elf_end}, // 显式持内部锁yield的用户观察程序。
```

宿主在join以后运行pthread-mutex，核对FAST差值1、实际内部超时、205tick睡眠和两位真实创建／返回。普通counter/checksum、动态生命周期与errno由guest自己断言，不由主机代算结果。

<!-- replace: scripts/shell_test.py -->
<!-- before -->
```python
                cycles=re.findall(r'\[THREAD_JOIN_CYCLE\] pid='+join_owner.group(1)+r' joiner=(\d+) target=(\d+)',join_output) # B->A会回到B。
                assert len(busy)==1 and len(cycles)==1 and busy[0][1]==join_owner.group(1) and cycles[0]==(busy[0][0],busy[0][2]) # 使用真实内核关系，不硬编码本轮数字。
                assert '[THREAD_JOIN_WAIT] pid='+join_owner.group(1)+' tid='+busy[0][0]+' joiner='+busy[0][2] in join_output # A已经真的等待B，环不是用户自报的虚拟图。
                offset=len(text); wire.sendall(b'heap-threads\n'); text=wait('[SHELL_COMMAND_DONE] name=heap-threads code=0',offset) # 四个真实执行者共享唯一arena，观察对象交接。
                heap_threads=text[offset:] # 不用旧heap命令代替并发结果。
                assert '[MALLOC_THREADS_OK] workers=4 rounds=96 arena=262144 cross_free=4 snapshots=consistent errno=private critical_yield=1' in heap_threads # 每个工作者和主快照均由guest核对。
```
<!-- after -->
```python
                cycles=re.findall(r'\[THREAD_JOIN_CYCLE\] pid='+join_owner.group(1)+r' joiner=(\d+) target=(\d+)',join_output) # B->A会回到B。
                assert len(busy)==1 and len(cycles)==1 and busy[0][1]==join_owner.group(1) and cycles[0]==(busy[0][0],busy[0][2]) # 使用真实内核关系，不硬编码本轮数字。
                assert '[THREAD_JOIN_WAIT] pid='+join_owner.group(1)+' tid='+busy[0][0]+' joiner='+busy[0][2] in join_output # A已经真的等待B，环不是用户自报的虚拟图。
                offset=len(text); wire.sendall(b'pthread-mutex\n'); text=wait('[SHELL_COMMAND_DONE] name=pthread-mutex code=0',offset) # 公开无总超时的lock不能泄漏内部200tick到期。
                pmutex_output=text[offset:] # 固定本条命令的实际默认类型、结果和错误。
                assert '[PTHREAD_MUTEX_FAST] pairs=8 delta=1' in pmutex_output and '[PTHREAD_MUTEX_USER_OK] default=errorcheck counter=64 critical_yield=1 timeout=internal init=dynamic destroy=checked errno=preserved' in pmutex_output # guest分别核对计数、普通状态与生命周期。
                assert '[FUTEX_WAIT]' in pmutex_output and 'result=-110' in pmutex_output and 'operation=sleep ticks=205' in pmutex_output # 底层确实到期后重试，公开两位仍完成。
                pmutex_owner=re.search(r'\[EXEC_COMMIT\] pid=(\d+) program=pthread-mutex ',pmutex_output) # 不固定本轮PID。
                assert pmutex_owner and len(re.findall(r'\[THREAD_CREATE\] pid='+pmutex_owner.group(1)+r' tid=',pmutex_output))==2 and len(re.findall(r'\[THREAD_RETURN\] pid='+pmutex_owner.group(1)+r' tid=',pmutex_output))==2 # 不是主或宿主代算64次更新。
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
                result.update(peer_worker_join=True,join_claim_unique=True,join_cycle_rejected=True,pthread_source_subset=True,pthread_workers=36,pthread_packets_reclaimed=True,bounded_queue=True,queue_capacity=4,queue_jobs=48,queue_close_drains=True,sequence_event=True,event_waits=6,event_broadcast_workers=2,event_false_wake_rechecked=True,mutex_race_stages=4,mutex_race_actual_waits=1,mutex_race_mismatches=1,per_execution_syscall_count=True,uncontended_mutex_pairs=8,uncontended_mutex_syscalls=0,runtime_tcb_identity=True,identity_reference_queries=7,shared_malloc=True,malloc_workers=4,malloc_rounds=96,owned_mutex=True,mutex_critical_updates=64,private_futex=True,futex_registrations=8,user_heap=True,shared_executions=True,thread_commands=2,worker_capacity=8,worker_reuse_rounds=24) # 关键字把实际已验收字段加入结果字典，不替代上面的逐命令检查。
                result_path.write_text(json.dumps(result,indent=2)+'\n'); print('PASS user shell: alloc/file/async wait/other task/CQ/readback, failed exec, return to monitor') # 阶段演示实际完成。
                return result # 总入口可收集。
            finally: # 包括断言、超时和QEMU错误。
```
<!-- after -->
```python
                offset=len(text); wire.sendall(b'echo after-shell\n'); text=wait('[CONSOLE_UPDATE] view=0 commands=2 page=0',offset) # 回到内核后输入仍完整。
                assert 'after-shell' in text[offset:] # 不只是退出而丢了后续UART。
                result={'passed':True,'ring3_shell':True,'ram_file_and_device_readback':True,'other_user_during_cq_wait':True,'qmp_iops':1,'error_exec_survived':True,'returned_to_monitor':True,'orphan_rounds':24,'memory_reuse_rounds':128,'capacity_rounds':4,'capacity_pages':1024,'simd_preempt_fork_exec':True,'fs_base_preempt_fork_exec':True,'compiler_static_tls':True,'posix_errno_io_subset':True,'positioned_vectors':True} # 只发布已观察事实。
                result.update(pthread_mutex_source=True,pthread_mutex_internal_timeout_retried=True,peer_worker_join=True,join_claim_unique=True,join_cycle_rejected=True,pthread_source_subset=True,pthread_workers=36,pthread_packets_reclaimed=True,bounded_queue=True,queue_capacity=4,queue_jobs=48,queue_close_drains=True,sequence_event=True,event_waits=6,event_broadcast_workers=2,event_false_wake_rechecked=True,mutex_race_stages=4,mutex_race_actual_waits=1,mutex_race_mismatches=1,per_execution_syscall_count=True,uncontended_mutex_pairs=8,uncontended_mutex_syscalls=0,runtime_tcb_identity=True,identity_reference_queries=7,shared_malloc=True,malloc_workers=4,malloc_rounds=96,owned_mutex=True,mutex_critical_updates=64,private_futex=True,futex_registrations=8,user_heap=True,shared_executions=True,thread_commands=2,worker_capacity=8,worker_reuse_rounds=24) # 关键字把实际已验收字段加入结果字典，不替代上面的逐命令检查。
                result_path.write_text(json.dumps(result,indent=2)+'\n'); print('PASS user shell: alloc/file/async wait/other task/CQ/readback, failed exec, return to monitor') # 阶段演示实际完成。
                return result # 总入口可收集。
            finally: # 包括断言、超时和QEMU错误。
```

完整实现闭合，运行：

<!-- run -->
```sh
./airios build  # 链接默认ERRORCHECK映射、属性与对象生命周期
```

<!-- run -->
```sh
./airios test  # 内部200tick重试、205tick持有、64次真实竞争更新
```

<a id="pmutex-results"></a>
## 无竞争不陷入，竞争等待也不偷换超时合同

交互执行`./airios run`、`run sh`、`pthread-mutex`。本轮实际输出包括：

```text
[PTHREAD_MUTEX_FAST] pairs=8 delta=1
[PTHREAD_MUTEX_USER_OK] default=errorcheck counter=64 critical_yield=1 timeout=internal init=dynamic destroy=checked errno=preserved
[SHELL_COMMAND_DONE] name=pthread-mutex code=0
```

两位worker实际遇到内部到期以后继续等待，公开lock最终取得；64次持锁yield没有泄露半更新数据。空闲对象的静态／动态初始化、属性销毁后独立使用、忙销毁拒绝与静止重初始化分别经过。初始化标记不授锁，notify不授锁，返回0取得才允许访问普通数据。

本节没有提供NORMAL、RECURSIVE、robust、进程共享、PI或pthread_mutex_timedlock，也未实现完整C++运行时。销毁检查仍要求调用者先排除所有使用者，未来条件等待会在释放互斥期间仍引用它，需要把这段使用寿命继续接入，不能只看owner暂时为0。
