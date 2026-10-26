# K62 · 正确的线程函数、指针结果与错误返回

K61 已用自有线程运行真实队列。接下来让一小段使用pthread_create、pthread_join的C源码也能运行，继续复用这些执行者。接口名字相同还不够：目标函数返回void *，底层工作入口返回long；创建函数成功返回0并另写线程身份，失败返回正错误码，不能照搬负错误或errno规则。

从 K61 的同一目录继续。本节只实现create、join、self、equal子集：NULL属性、可join线程和普通函数返回。当前仍由主执行者创建／收取，worker尝试对等管理返回ENOTSUP；不声明完整POSIX支持，不提供尚未实现的pthread_exit、detach、取消、属性操作或互斥API。后续逐项扩充，不通过占位成功绕过真实机制。

<a id="pthread-contract"></a>
## 标准函数形状与当前内核能力分别写清

pthread_create接收void *(*function)(void *)和一个参数，成功后把身份写到thread输出并返回0；新执行者可以在创建者返回前就开始运行。pthread_join等待指定执行者结束，把它返回的void *写入私人输出，NULL可以丢弃结果。这些形状可对照 [创建手册](https://man7.org/linux/man-pages/man3/pthread_create.3.html) 与 [join手册](https://man7.org/linux/man-pages/man3/pthread_join.3.html)。

多数pthread函数失败直接返回错误号，不设置errno；不能写成普通POSIX I/O的-1加errno。这项区别见 [pthread返回约定](https://man7.org/linux/man-pages/man7/pthreads.7.html)。内部会用malloc准备启动参数，malloc失败却会设置ENOMEM，因此适配器需要恢复调用者原errno，再通过正EAGAIN报告资源不足。

新建pthread.h。pthread_t在本实现中用uintptr_t承载真实TID，但应用用self/equal取得和比较，不根据数字猜身份。pthread_attr_t只保留声明位置，非NULL一律ENOTSUP，不能声称栈、调度、detach属性已经生效。本项目类型不是glibc布局，目标是逐步源码兼容，不是Linux二进制兼容。

<!-- write: labs/airios/include/pthread.h -->
```c
#ifndef AIRI_PTHREAD_H // 当前线程源码子集，不声明完整POSIX支持。
#define AIRI_PTHREAD_H // 包含保护。
#include <stdint.h> // 身份载体与指针往返保持目标宽度。
typedef uintptr_t pthread_t; // 当前实现用真实TID作不透明身份，应用通过pthread_equal比较。
typedef struct { uint64_t reserved; } pthread_attr_t; // 仅保留类型位置，当前create只接受NULL属性，不解析此对象。
#ifdef __cplusplus // C++调用同一C实现。
extern "C" { // 不链接宿主pthread。
#endif // C直接声明。
int pthread_create(pthread_t *thread, const pthread_attr_t *attributes, void *(*function)(void *), void *argument); // 当前仅主执行者创建，成功0并写身份，失败正错误且不改errno。
int pthread_join(pthread_t thread, void **result); // 当前仅主执行者收取，NULL丢弃结果；成功才写私人输出。
pthread_t pthread_self(void); // 当前有效运行头提供身份，不能跨进程当授权句柄。
int pthread_equal(pthread_t left, pthread_t right); // 相同非零，不同0，不将可移植比较写成数值猜测。
#ifdef __cplusplus // 结束C链接声明。
} // 类型布局保持本项目约定，不兼容Linux二进制。
#endif // 结束语言差异。
#endif // 结束包含保护。
```

在errno.h增加ESRCH与ENOTSUP名字，分别表示身份不存在和当前不支持的能力。仍是正错误常量；底层自有调用的负错误如何翻译，留在适配层处理。

<!-- replace: labs/airios/include/errno.h -->
<!-- before -->
```c
#define AIRI_ERRNO_H // 包含保护。
#define EPERM 1 // 当前操作不允许。
#define ENOENT 2 // 名称不存在。
#define EINTR 4 // 预留中断错误名；当前尚无信号，不会主动制造它。
#define EIO 5 // 设备或I/O错误。
#define ENOEXEC 8 // 映像格式不支持。
```
<!-- after -->
```c
#define AIRI_ERRNO_H // 包含保护。
#define EPERM 1 // 当前操作不允许。
#define ENOENT 2 // 名称不存在。
#define ESRCH 3 // 当前线程身份不存在或已经收取。
#define EINTR 4 // 预留中断错误名；当前尚无信号，不会主动制造它。
#define EIO 5 // 设备或I/O错误。
#define ENOEXEC 8 // 映像格式不支持。
```

<!-- replace: labs/airios/include/errno.h -->
<!-- before -->
```c
#define ENOSYS 38 // 当前未实现的操作。
#define ETIMEDOUT 110 // 有界等待预算已经用完。
#define EOVERFLOW 75 // 不能表示请求结果。
#ifdef __cplusplus // 同一头以后可被C++源码使用，函数仍用C链接名。
extern "C" { // 防止把运行库入口编码成C++重载符号。
#endif // C11跳过链接语言包装。
```
<!-- after -->
```c
#define ENOSYS 38 // 当前未实现的操作。
#define ETIMEDOUT 110 // 有界等待预算已经用完。
#define EOVERFLOW 75 // 不能表示请求结果。
#define ENOTSUP 95 // 当前明确不支持的属性或线程管理方式。
#ifdef __cplusplus // 同一头以后可被C++源码使用，函数仍用C链接名。
extern "C" { // 防止把运行库入口编码成C++重载符号。
#endif // C11跳过链接语言包装。
```

<a id="pthread-typed-packet"></a>
## 不强转函数指针，使用一个类型正确的适配入口

已有airi_thread_create要求long (*)(void *)，pthread目标却是void *(*)(void *)。当前机器虽然都用RAX返回64位，C函数类型仍不同；把指针强转后直接以错误签名调用，不是正确的语言级适配。让一个真正返回long的start入口接手，再由它按原签名调用用户函数。

start需要知道两个值：真实function和argument。不能把创建者栈上的临时结构交给新worker，因为pthread_create返回后那份局部对象可能已经失效。用已同步malloc分配start_packet，创建者先完整写入两个字段，成功后只交给worker；失败则创建者自己free。

worker入口先把两个字段复制到自己局部变量，再free启动包，然后才调用业务function。释放的是包装参数，不是用户argument；argument的本体仍由应用保证活到最后一次使用。观察程序把参数放在main持有的静态tokens中，join以后才重新使用，不返回某条已经结束的工作栈内地址。

新建pthread.c，先写类型正确的start及self/equal。本目标void *、uintptr_t、long都是64位，目标函数先按正确签名返回指针，再经uintptr_t和当前long载体交给既有40号返回；join反向恢复。这个载体约定依赖当前目标，不是靠sizeof就证明任意新架构都有相同指针表示。

<!-- write: labs/airios/libc/pthread.c -->
```c
#include <pthread.h> // 标准形状的线程函数与指针结果。
#include <airi_threads.h> // 内核真实TID、共享执行者与long结果。
#include <airi_libc.h> // 主执行者TID与PID的当前关系。
#include <stdlib.h> // 已同步的共享用户分配器。
#include <errno.h> // 返回正错误，不把pthread错误写进errno。
_Static_assert(sizeof(void *) == sizeof(long) && sizeof(long) == 8, "pthread result requires current 64-bit carrier"); // 本目标指针可完整经过已有long结果槽。
struct start_packet { void *(*function)(void *); void *argument; }; // 单独持有正确函数类型，不能把void*返回函数强转成long返回函数调用。
static long start(void *argument) { // 这个适配器才符合airi_thread_function的long返回类型。
    struct start_packet *packet = argument; // 创建成功以后只由这个新worker拥有。
    void *(*function)(void *) = packet->function; // 先复制真正目标，不在free以后再读包。
    void *value = packet->argument; // 用户参数本体仍归调用者，寿命须覆盖目标函数使用。
    free(packet); // worker在调用业务函数以前交还启动包，创建者成功后不再访问它。
    return (long)(uintptr_t)function(value); // 按原签名调用，再以当前目标64位载体交回指针位型。
}
pthread_t pthread_self(void) { // 无需每次进入内核查询。
    return (pthread_t)airi_thread_id(); // 有效TCB直接读，旧版本仍沿已有后备。
}
int pthread_equal(pthread_t left, pthread_t right) { // 本实现身份载体是整数，但应用依赖这个接口。
    return left == right; // 不设置errno。
}
```

<a id="pthread-create-ownership"></a>
## 失败包留在创建者，成功包只归worker

create在分配前拒绝显式坏输出、NULL函数、非NULL属性以及worker递归创建。任意坏映射或无效目标函数仍属于用户调用合同，不由这个C包装替内核完成指针验证；内核验证的是它实际要执行的适配器入口，不能把这等同于用户业务函数已被预先运行检查。

随后保存errno并分配包。malloc失败恢复旧errno，返回正EAGAIN；包写完后调用底层39号。返回负数表示没有公开worker，创建者free并恢复errno；成功以后只写独立的thread输出，不能再读packet，因为worker可能已经复制、释放，甚至让这块内存被别人复用。

| 交接位置 | 启动包归属 | 之后允许的操作 |
| --- | --- | --- |
| 调用39号以前 | 创建者 | 写function/argument，失败时free |
| 39号创建失败 | 创建者 | free一次，不写thread输出 |
| 39号创建成功 | 新worker | 复制字段、free一次，再调用业务函数 |

库返回0只表示执行者已经创建，不保证回调尚未开始。errno恢复的是创建者自己的TLS槽，不会覆盖worker后来设置的errno。

<!-- append: labs/airios/libc/pthread.c -->
```c
int pthread_create(pthread_t *thread, const pthread_attr_t *attributes, void *(*function)(void *), void *argument) { // 默认可join线程的最小源码路径。
    if (!thread || (uintptr_t)thread % _Alignof(pthread_t) || !function) return EINVAL; // 显式坏输出/函数在分配前拒绝，其他无效用户地址仍可故障。
    if (attributes) return ENOTSUP; // 不把未实现的栈、调度或detach属性当作生效。
    if (pthread_self() != (pthread_t)airi_getpid()) return ENOTSUP; // 当前生命周期仍由主执行者管理，worker递归创建明确拒绝。
    int saved = errno; // malloc可能设置ENOMEM，但pthread返回规则不允许改调用者errno。
    struct start_packet *packet = malloc(sizeof(*packet)); // 启动包跨创建者栈寿命，不借一个很快失效的局部对象。
    if (!packet) { errno = saved; return EAGAIN; } // 资源不足通过正返回值报告。
    packet->function = function; packet->argument = argument; // 在公开新执行者以前完整准备参数。
    long tid = airi_thread_create(start, packet); // 内核验证适配器的可执行地址，目标函数仍须是调用者提供的有效函数。
    if (tid < 0) { // 新worker未发布，包仍只归创建者。
        free(packet); errno = saved; // 不泄漏失败包，恢复原错误槽。
        return tid == -ENOMEM ? EAGAIN : (int)-tid; // 内核页不足也归资源不足，其余原始负错误映成正值。
    }
    *thread = (pthread_t)tid; // 只有成功才提交身份；worker可能已运行，不能再读packet。
    errno = saved; // 创建过程的内部操作不污染调用者错误状态。
    return 0; // 表示执行者已建立，不保证函数尚未开始或已经结束。
}
```

<a id="pthread-join-result"></a>
## 内核写long私人槽，适配器再提交void *结果

join先拒绝零或过宽身份，避免窄化命中其他人；自己join自己返回EDEADLK。当前worker之间的join图尚未实现，非主执行者返回ENOTSUP，而不是假装已经收取。标准的对等线程语义仍是后续工作。

正常路径用本调用栈上的long value承接内核结果，成功后才转成void *写给调用者。不能直接把void **强转成long *交给内核，绕过两层类型和失败输出的责任。结果输出为NULL时仍做真实join及资源收拢，只是不复制用户结果；失败保留原输出，也不修改errno。

本实现TID在一次启动内不复用，已join的旧身份会返回ESRCH，作为当前诊断扩展。可移植程序不应重复join一个寿命已经结束的身份；也不能把旧TID数值当永久句柄。

<!-- append: labs/airios/libc/pthread.c -->
```c
int pthread_join(pthread_t thread, void **result) { // 当前不支持worker之间的join图。
    if (!thread || thread > UINT32_MAX) return ESRCH; // 不截断过宽身份命中别人。
    if (pthread_equal(thread, pthread_self())) return EDEADLK; // 自己不能等到自己结束再继续，先于主执行者限制报告。
    if (pthread_self() != (pthread_t)airi_getpid()) return ENOTSUP; // 不伪装完整POSIX的对等线程join能力。
    long value; // 内核只写这个已经存在的私人long槽。
    long error = airi_thread_join((long)thread, result ? &value : 0); // NULL仍执行真实join并回收，不创建虚假结果。
    if (error) return (int)-error; // 失败保持调用者输出，原始接口不改errno。
    if (result) *result = (void *)(uintptr_t)value; // 成功后恢复当前目标的指针表示，不返回启动包地址。
    return 0; // 目标已经结束且本次结果只能收取一次。
}
```

<a id="pthread-pointer-observation"></a>
## 参数不是启动包，结果回到同一个存活对象

新建pthread-probe.c。tokens是main一直持有的对象数组，每项包含index及worker写回的identity。主在创建前写index，join以后才读identity，没有把pthread_create返回的时机当成worker已经写完。primary在任何worker运行前写入，之后只读。

<!-- write: labs/airios/pthread-probe.c -->
```c
#include <pthread.h> // 本节实现的create/join/self/equal子集。
#include <airi_event.h> // 容量观察用一个独立的版本门。
#include <airi_malloc.h> // 观察启动包是否在成功/失败路径归还。
#include <stdlib.h> // 容量失败观察占满当前有界arena，再归还。
#include <airi_libc.h> // 串口、真实PIT与主动交接。
#include <errno.h> // 各线程错误槽独立，pthread失败不覆盖它。
#include <stdatomic.h> // 夹具准备位置，不借宿主同步。
struct token { unsigned index; pthread_t identity; }; // 参数和结果都指向主仍持有的对象，不指向适配器启动包。
static struct token tokens[8]; // main写index以后才创建，identity只在join以后读取。
static pthread_t primary; // 创建任何worker以前写入，之后只读。
static airi_event gate = AIRI_EVENT_INITIALIZER; // 八容量worker等待主统一发布。
static _Atomic unsigned ready; // 回调开始时启动包已经被适配器释放。
static void require(int condition, const char *message) { // 失败结束本真实进程组。
    if (!condition) { airi_puts(message); airi_puts("\n"); airi_exit(88); } // 不继续输出成功标记。
}
```

echo保持正确void *函数签名，收到的是原token，不是start_packet。它确认新TLS的errno为0，再写自己的101+index，yield后仍应读到自己的值。第一位还实际尝试worker递归create、对等join和self join，分别观察ENOTSUP、ENOTSUP和EDEADLK，输出哨兵保持；这些失败都不能改它的errno。

<!-- append: labs/airios/pthread-probe.c -->
```c
static void *echo(void *argument) { // 真正的POSIX形状函数，不强转函数指针。
    struct token *token = argument; // 用户参数保持到主join以后。
    require(errno == 0 && !pthread_equal(pthread_self(), primary), "pthread fresh errno and identity"); // 新TLS不继承主71。
    token->identity = pthread_self(); // 只写自己那一份输出，主尚未读取。
    errno = 101 + (int)token->index; // 每位执行者保存不同错误哨兵。
    if (!token->index) { // 第一位观察未支持的worker管理能力。
        pthread_t untouched = 777; void *output = (void *)(uintptr_t)99; // 失败输出哨兵。
        require(pthread_create(&untouched, 0, echo, token) == ENOTSUP && untouched == 777, "pthread nested create unsupported"); // 不能悄悄创建后返回失败。
        require(pthread_join(primary, &output) == ENOTSUP && output == (void *)(uintptr_t)99, "pthread peer join unsupported"); // 保留当前主管理限制。
        require(pthread_join(pthread_self(), 0) == EDEADLK, "pthread worker self join"); // 自己等待自己明确报告。
    }
    airi_yield(); // 在真正共享执行者之间交替，errno仍属于自己。
    require(errno == 101 + (int)token->index, "pthread worker errno private"); // pthread调用和别人的TLS不污染它。
    return token; // 目标返回值不是启动包，而是仍存活的用户对象。
}
```

held用于容量观察。它一进入业务回调，就说明适配器已经free了启动包；ready发布到8以后，主才能检查所有八个包均已归还。它们仍被版本门挡住，工作槽不会提前释放。门使用同一200tick预算，主发布后各自返回自己的原参数。

<!-- append: labs/airios/pthread-probe.c -->
```c
static void *held(void *argument) { // 容量worker从回调开始就不再占启动包。
    atomic_fetch_add_explicit(&ready, 1, memory_order_release); // 主可据此检查所有包已归还。
    uint64_t start = airi_ticks(); // 固定200tick，不被无变化通知续借。
    while (airi_event_sequence(&gate) == 0) { // 主发布以前不能结束并释放工作槽。
        uint64_t elapsed = airi_ticks() - start; // 切换也消耗本次预算。
        require(elapsed < 200 && airi_event_wait(&gate, 0, (unsigned)(200 - elapsed)) == 0, "pthread capacity gate"); // 真实等待，不用宿主线程阻塞。
    }
    return argument; // 八个结果分别指回自己的token。
}
```

main把自己的errno设为71，先验证无效参数、非NULL属性、self join、零／过宽身份等正错误与输出保持。此时还没有创建echo，也没有可收取的worker。

<!-- append: labs/airios/pthread-probe.c -->
```c
int main(void) { // 所有创建/收取都从主执行者开始。
    primary = pthread_self(); errno = 71; // 不把pthread_t的具体整数当可移植接口使用。
    pthread_t unchanged = 777; pthread_attr_t attributes = {0}; // 非NULL属性当前不支持，即使保留字为0。
    require(pthread_equal(primary, pthread_self()), "pthread equality"); // 只比较有效身份，不靠猜测下一个TID。
    require(pthread_create(0, 0, echo, 0) == EINVAL && pthread_create(&unchanged, 0, 0, 0) == EINVAL, "pthread invalid arguments"); // 不创建实际worker。
    require(pthread_create(&unchanged, &attributes, echo, 0) == ENOTSUP && unchanged == 777 && errno == 71, "pthread attributes unsupported"); // 属性未生效不能返回成功。
    void *output = (void *)(uintptr_t)99; // join失败保持私人输出。
    require(pthread_join(primary, &output) == EDEADLK && pthread_join(0, &output) == ESRCH && pthread_join((pthread_t)UINT32_MAX + 1, &output) == ESRCH, "pthread invalid joins"); // 不窄化过宽值。
    require(output == (void *)(uintptr_t)99 && errno == 71, "pthread join failure preserves state"); // 正错误不写errno。
```

继续在main中创建两位echo，比较这两个已创建尚未join的有效身份。第一位join完整返回原token指针，第二位用NULL丢弃结果也仍收拢；结果对象来自main持有的tokens，启动包早已在业务函数调用前释放。再重复join旧身份，只检验本实现的ESRCH扩展，不作为标准用法。

下面两条成功 join 的断言还在 join 返回后比较了旧身份。此时目标身份的标准寿命已经结束；这里能继续比较，仅来自本实现整数 TID 不复用、`pthread_equal` 比较数值的限定。可移植代码应在 join 前比较有效身份，不能把这两条断言移植为一般 pthread 用法。[pthread_equal 的有效身份要求](https://man7.org/linux/man-pages/man3/pthread_equal.3p.html)也适用于这里。K63 将把观察改为创建者、槽位复用与仍存活的 leaf，并删除这两处结束后的比较。

<!-- append: labs/airios/pthread-probe.c -->
```c
    pthread_t threads[8]; // 只在create成功以后使用每个槽。
    for (unsigned i = 0; i < 2; ++i) { tokens[i].index = i; require(pthread_create(&threads[i], 0, echo, &tokens[i]) == 0 && errno == 71, "pthread initial create"); } // 新worker可以在下一次创建以前已经返回。
    require(!pthread_equal(threads[0], threads[1]), "pthread distinct live identities"); // 两个已创建尚未join的身份都仍有效。
    require(pthread_join(threads[0], &output) == 0 && output == &tokens[0] && pthread_equal(tokens[0].identity, threads[0]), "pthread typed result"); // 指针完整往返，身份属于真实worker。
    require(pthread_join(threads[1], 0) == 0 && pthread_equal(tokens[1].identity, threads[1]), "pthread discard result"); // NULL不省略收取。
    output = (void *)(uintptr_t)99; require(pthread_join(threads[0], &output) == ESRCH && output == (void *)(uintptr_t)99 && errno == 71, "pthread already joined extension"); // 本实现不复用TID，旧身份明确拒绝；不可移植代码不依赖此扩展。
```

<a id="pthread-capacity-and-allocation"></a>
## 创建失败有两个不同的资源位置

第一种失败发生在包分配成功以后：先让八位held进入回调，所有包已free，八工作槽仍全被占用。主第九次create会先分配一个包，再被内核以容量不足拒绝。它必须free这个失败包，返回EAGAIN，thread哨兵777和errno71保持。前后malloc_info快照的活对象、活字节应相同，不能把失败包泄漏成一项隐藏状态。

<!-- append: labs/airios/pthread-probe.c -->
```c
    for (unsigned i = 0; i < 8; ++i) require(pthread_create(&threads[i], 0, held, &tokens[i]) == 0 && errno == 71, "pthread capacity fill"); // 八个真实工作槽全部占用。
    uint64_t start = airi_ticks(); // 只等回调实际开始，不从创建成功推断包已释放。
    while (atomic_load_explicit(&ready, memory_order_acquire) != 8) { require(airi_ticks() - start < 100, "pthread capacity ready"); airi_yield(); } // 有限握手。
    struct airi_malloc_info before = airi_malloc_info(); // 所有已运行worker的启动包都应归还。
    require(before.live_objects == 0, "pthread packets freed before callbacks"); // 不把包保留到join才清理。
    unchanged = 777; require(pthread_create(&unchanged, 0, held, 0) == EAGAIN && unchanged == 777 && errno == 71, "pthread capacity error"); // 第九次malloc包后创建失败，必须归还。
    struct airi_malloc_info after = airi_malloc_info(); // 一致快照与失败以前比较。
    require(after.live_objects == before.live_objects && after.live_bytes == before.live_bytes, "pthread failed packet cleanup"); // 失败路径不泄漏包。
```

发布门以后收取八人，再做24轮顺序create/join，超过槽数但从不超过同时八人的容量。所有回调返回自己的原参数。这里不要求每一次join都真的阻塞：目标可能早已结束，仍应正常收取，不能把某种调度顺序当正确性的前提。

<!-- append: labs/airios/pthread-probe.c -->
```c
    require(airi_event_publish(&gate) >= 0, "pthread capacity release"); // 版本前进让尚未登记的人也不睡错。
    for (unsigned i = 0; i < 8; ++i) require(pthread_join(threads[i], &output) == 0 && output == &tokens[i], "pthread capacity join"); // 八个原参数分别返回。
    for (unsigned i = 0; i < 24; ++i) { // 超过八槽总量，验证重复创建/收取与包复用。
        tokens[1].index = 1; // 上一位已经join，可以再次借出同一参数对象。
        require(pthread_create(&threads[0], 0, echo, &tokens[1]) == 0 && pthread_join(threads[0], &output) == 0 && output == &tokens[1], "pthread sequential reuse"); // 保留正确函数类型与真实生命周期。
    }
```

第二种失败发生在内核创建以前：当前arena为262144字节，纯块头32字节、请求头16字节，申请262144-32-16即262096字节，正好占满当前已合并的空闲块。适配器随后malloc启动包会失败并设置ENOMEM，但pthread_create必须恢复调用者71并返回正EAGAIN，没有新的worker或包。

这几个大小来自本项目固定分配器，不是一般malloc都保证可以这样申请。free观察大对象后，活对象与活字节均为0，页面缓存仍262144；缓存页面和泄漏启动包是两回事。34位真实worker包括最初2位、容量8位和顺序24位；两次失败创建及worker内拒绝不计入成功数量。

<!-- append: labs/airios/pthread-probe.c -->
```c
    void *reserve = malloc(262144 - 32 - 16); // 当前唯一arena减32字节块头和16字节请求头，正好占满可分配空间。
    require(reserve != 0, "pthread allocator reserve"); // 此时全部启动包均已释放，不能有阻碍整块分配的活对象。
    before = airi_malloc_info(); errno = 71; // pthread_create内部malloc将失败，但应恢复这个错误哨兵。
    unchanged = 777; require(pthread_create(&unchanged, 0, echo, &tokens[1]) == EAGAIN && unchanged == 777 && errno == 71, "pthread allocation failure preserves errno"); // 失败发生在内核创建以前。
    after = airi_malloc_info(); require(after.live_objects == before.live_objects && after.live_bytes == before.live_bytes, "pthread allocation failure ownership"); // 不伪造启动包或消耗新的worker。
    free(reserve); // 所有借用都结束以后归还唯一观察大对象。
    after = airi_malloc_info(); // 页面可缓存，启动对象不能残留。
    require(after.live_objects == 0 && after.live_bytes == 0 && after.arena_bytes == 262144 && errno == 71, "pthread final allocator and errno"); // 不把缓存页面误判成包泄漏。
    airi_puts("[PTHREAD_USER_OK] functions=typed result=pointer errno=preserved workers=34 capacity=8 failed_packet=freed alloc_failure=preserved reuse=24 attributes=unsupported\n"); // 正常、失败与资源边界实际完成后才输出。
    return 0; // shell收取真实程序结果。
}
```

<a id="pthread-build-and-evidence"></a>
## 把适配器与所需运行库完整链接进guest

pthread-lib.o另编译正确类型的适配器，pthread.elf选择已同步malloc、errno TLS、普通互斥、原始线程库和入口。event仅供观察门使用，不成为pthread_create自身的依赖。没有链接宿主libpthread、宿主malloc或现成内核。再打包真实ELF并注册pthread名称。

<!-- append: Makefile -->
```makefile

# POSIX形状的线程入口适配单独选择，不改变早期用户程序。
$(OUT)/user/pthread-lib.o: labs/airios/libc/pthread.c Makefile
# 与观察main的pthread-probe.o分别保存。
	@mkdir -p $(dir $@)
# 只使用自有头与相同目标，不链接宿主pthread。
	$(CC) $(USER_FLAGS) -Ilabs/airios/include -c $< -o $@
# 启动包使用已同步malloc，观察门额外选择event库。
$(OUT)/user/pthread.elf: $(USER_COMMON) $(OUT)/user/posix.o $(OUT)/user/heap-algorithm.o $(OUT)/user/malloc.o $(OUT)/user/mutex-lib.o $(OUT)/user/threads-lib.o $(OUT)/user/thread-entry.o $(OUT)/user/event-lib.o $(OUT)/user/pthread-lib.o $(OUT)/user/pthread-probe.o labs/airios/program.ld
# 全部函数与线程运行在真实guest用户地址空间。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(filter %.o,$^) -o $@
# 初始目录等待最终静态ELF。
$(OUT)/labs/airios/images.o: $(OUT)/user/pthread.elf
# 路径仅由incbin读取，不运行主机线程示例。
$(OUT)/labs/airios/images.o: CPPFLAGS += -DAIRI_PTHREAD_ELF='"$(OUT)/user/pthread.elf"'
```

<!-- append: labs/airios/images.S -->
```asm
.balign 16 /* 线程源码兼容入口的独立观察ELF。 */
.global airi_pthread_elf_start, airi_pthread_elf_end /* 内核只借完整文件范围。 */
airi_pthread_elf_start: /* 从ELF头开始装载。 */
.incbin AIRI_PTHREAD_ELF /* 自有适配器与真实用户观察。 */
airi_pthread_elf_end: /* 范围右端不包含字节。 */
```

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
extern const uint8_t airi_mutex_races_elf_start[], airi_mutex_races_elf_end[]; // 与普通锁独立链接的真实交错观察ELF。
extern const uint8_t airi_events_elf_start[], airi_events_elf_end[]; // 用户变化通知通过同一真实futex运行。
extern const uint8_t airi_queue_elf_start[], airi_queue_elf_end[]; // 队列算法位于用户态，内核继续提供既有线程/等待。
extern const uint8_t airi_identity_elf_start[], airi_identity_elf_end[]; // 原始fork、exec与工作槽复用中的本地TID观察。
extern const uint8_t airi_heap_threads_elf_start[], airi_heap_threads_elf_end[]; // 同一arena并发分配、跨执行者归还与快照。
extern const uint8_t airi_mutex_elf_start[], airi_mutex_elf_end[]; // 原子所有权与futex等待组合的真实用户观察。
```
<!-- after -->
```c
extern const uint8_t airi_mutex_races_elf_start[], airi_mutex_races_elf_end[]; // 与普通锁独立链接的真实交错观察ELF。
extern const uint8_t airi_events_elf_start[], airi_events_elf_end[]; // 用户变化通知通过同一真实futex运行。
extern const uint8_t airi_queue_elf_start[], airi_queue_elf_end[]; // 队列算法位于用户态，内核继续提供既有线程/等待。
extern const uint8_t airi_pthread_elf_start[], airi_pthread_elf_end[]; // POSIX形状的用户包装不替代真实共享执行者。
extern const uint8_t airi_identity_elf_start[], airi_identity_elf_end[]; // 原始fork、exec与工作槽复用中的本地TID观察。
extern const uint8_t airi_heap_threads_elf_start[], airi_heap_threads_elf_end[]; // 同一arena并发分配、跨执行者归还与快照。
extern const uint8_t airi_mutex_elf_start[], airi_mutex_elf_end[]; // 原子所有权与futex等待组合的真实用户观察。
```

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    {"mutex-races", airi_mutex_races_elf_start, airi_mutex_races_elf_end}, // 暂停点只存在于此观察映像。
    {"events", airi_events_elf_start, airi_events_elf_end}, // 版本快照、无变化通知与广播观察。
    {"queue", airi_queue_elf_start, airi_queue_elf_end}, // 有界生产消费、关闭排空与背压。
    {"identity", airi_identity_elf_start, airi_identity_elf_end}, // 版本化运行头和原始查询对照。
    {"identity-reset", airi_identity_elf_start, airi_identity_elf_end}, // 成功exec后沿用PID并重建运行头。
    {"heap-threads", airi_heap_threads_elf_start, airi_heap_threads_elf_end}, // 显式持内部锁yield的用户观察程序。
```
<!-- after -->
```c
    {"mutex-races", airi_mutex_races_elf_start, airi_mutex_races_elf_end}, // 暂停点只存在于此观察映像。
    {"events", airi_events_elf_start, airi_events_elf_end}, // 版本快照、无变化通知与广播观察。
    {"queue", airi_queue_elf_start, airi_queue_elf_end}, // 有界生产消费、关闭排空与背压。
    {"pthread", airi_pthread_elf_start, airi_pthread_elf_end}, // 正错误、启动包和指针返回观察。
    {"identity", airi_identity_elf_start, airi_identity_elf_end}, // 版本化运行头和原始查询对照。
    {"identity-reset", airi_identity_elf_start, airi_identity_elf_end}, // 成功exec后沿用PID并重建运行头。
    {"heap-threads", airi_heap_threads_elf_start, airi_heap_threads_elf_end}, // 显式持内部锁yield的用户观察程序。
```

宿主在queue以后运行pthread。成功标记必须来自guest的所有状态断言，此外按真实命令PID核对34次THREAD_CREATE和34次THREAD_RETURN，并要求容量门有实际FUTEX_WAIT。join可以收取已结束者，不能为增加日志而强迫它在每种调度下都先阻塞。

<!-- replace: scripts/shell_test.py -->
<!-- before -->
```python
                queue_owner=re.search(r'\[EXEC_COMMIT\] pid=(\d+) program=queue ',queue_output) # 只数本次队列进程的执行者。
                assert queue_owner # 工作链共享同一PID。
                assert len(re.findall(r'\[THREAD_CREATE\] pid='+queue_owner.group(1)+r' tid=',queue_output))==8 and len(re.findall(r'\[THREAD_RETURN\] pid='+queue_owner.group(1)+r' tid=',queue_output))==8 # 一背压、二关闭消费者、一关闭生产者及四负载均收拢。
                offset=len(text); wire.sendall(b'heap-threads\n'); text=wait('[SHELL_COMMAND_DONE] name=heap-threads code=0',offset) # 四个真实执行者共享唯一arena，观察对象交接。
                heap_threads=text[offset:] # 不用旧heap命令代替并发结果。
                assert '[MALLOC_THREADS_OK] workers=4 rounds=96 arena=262144 cross_free=4 snapshots=consistent errno=private critical_yield=1' in heap_threads # 每个工作者和主快照均由guest核对。
```
<!-- after -->
```python
                queue_owner=re.search(r'\[EXEC_COMMIT\] pid=(\d+) program=queue ',queue_output) # 只数本次队列进程的执行者。
                assert queue_owner # 工作链共享同一PID。
                assert len(re.findall(r'\[THREAD_CREATE\] pid='+queue_owner.group(1)+r' tid=',queue_output))==8 and len(re.findall(r'\[THREAD_RETURN\] pid='+queue_owner.group(1)+r' tid=',queue_output))==8 # 一背压、二关闭消费者、一关闭生产者及四负载均收拢。
                offset=len(text); wire.sendall(b'pthread\n'); text=wait('[SHELL_COMMAND_DONE] name=pthread code=0',offset) # 正确函数类型的线程入口与指针结果，不强转回调。
                pthread_output=text[offset:] # 只核对本条命令的真实worker与结果。
                assert '[PTHREAD_USER_OK] functions=typed result=pointer errno=preserved workers=34 capacity=8 failed_packet=freed alloc_failure=preserved reuse=24 attributes=unsupported' in pthread_output # guest检查正错误、errno、包归还与每个原参数返回。
                pthread_owner=re.search(r'\[EXEC_COMMIT\] pid=(\d+) program=pthread ',pthread_output) # 不固定实际PID。
                assert pthread_owner # 静态ELF确实被真实shell执行。
                assert len(re.findall(r'\[THREAD_CREATE\] pid='+pthread_owner.group(1)+r' tid=',pthread_output))==34 and len(re.findall(r'\[THREAD_RETURN\] pid='+pthread_owner.group(1)+r' tid=',pthread_output))==34 # 两初始、八容量和二十四复用均真正结束。
                assert '[FUTEX_WAIT]' in pthread_output # 容量门真实阻塞；join也可收取已结束者，不能强求某种调度时序。
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
                result.update(bounded_queue=True,queue_capacity=4,queue_jobs=48,queue_close_drains=True,sequence_event=True,event_waits=6,event_broadcast_workers=2,event_false_wake_rechecked=True,mutex_race_stages=4,mutex_race_actual_waits=1,mutex_race_mismatches=1,per_execution_syscall_count=True,uncontended_mutex_pairs=8,uncontended_mutex_syscalls=0,runtime_tcb_identity=True,identity_reference_queries=7,shared_malloc=True,malloc_workers=4,malloc_rounds=96,owned_mutex=True,mutex_critical_updates=64,private_futex=True,futex_registrations=8,user_heap=True,shared_executions=True,thread_commands=2,worker_capacity=8,worker_reuse_rounds=24) # 关键字把实际已验收字段加入结果字典，不替代上面的逐命令检查。
                result_path.write_text(json.dumps(result,indent=2)+'\n'); print('PASS user shell: alloc/file/async wait/other task/CQ/readback, failed exec, return to monitor') # 阶段演示实际完成。
                return result # 总入口可收集。
            finally: # 包括断言、超时和QEMU错误。
```
<!-- after -->
```python
                offset=len(text); wire.sendall(b'echo after-shell\n'); text=wait('[CONSOLE_UPDATE] view=0 commands=2 page=0',offset) # 回到内核后输入仍完整。
                assert 'after-shell' in text[offset:] # 不只是退出而丢了后续UART。
                result={'passed':True,'ring3_shell':True,'ram_file_and_device_readback':True,'other_user_during_cq_wait':True,'qmp_iops':1,'error_exec_survived':True,'returned_to_monitor':True,'orphan_rounds':24,'memory_reuse_rounds':128,'capacity_rounds':4,'capacity_pages':1024,'simd_preempt_fork_exec':True,'fs_base_preempt_fork_exec':True,'compiler_static_tls':True,'posix_errno_io_subset':True,'positioned_vectors':True} # 只发布已观察事实。
                result.update(pthread_source_subset=True,pthread_workers=34,pthread_packets_reclaimed=True,bounded_queue=True,queue_capacity=4,queue_jobs=48,queue_close_drains=True,sequence_event=True,event_waits=6,event_broadcast_workers=2,event_false_wake_rechecked=True,mutex_race_stages=4,mutex_race_actual_waits=1,mutex_race_mismatches=1,per_execution_syscall_count=True,uncontended_mutex_pairs=8,uncontended_mutex_syscalls=0,runtime_tcb_identity=True,identity_reference_queries=7,shared_malloc=True,malloc_workers=4,malloc_rounds=96,owned_mutex=True,mutex_critical_updates=64,private_futex=True,futex_registrations=8,user_heap=True,shared_executions=True,thread_commands=2,worker_capacity=8,worker_reuse_rounds=24) # 关键字把实际已验收字段加入结果字典，不替代上面的逐命令检查。
                result_path.write_text(json.dumps(result,indent=2)+'\n'); print('PASS user shell: alloc/file/async wait/other task/CQ/readback, failed exec, return to monitor') # 阶段演示实际完成。
                return result # 总入口可收集。
            finally: # 包括断言、超时和QEMU错误。
```

完整实现闭合，运行：

<!-- run -->
```sh
./airios build  # 静态链接类型正确的线程适配器和共享malloc
```

<!-- run -->
```sh
./airios test  # 指针结果、errno、两种资源失败及34次真实工作生命周期
```

<a id="pthread-results"></a>
## 相同函数名后面，仍然是自己的执行者

交互执行`./airios run`、`run sh`、`pthread`，本轮实际输出：

```text
[PTHREAD_USER_OK] functions=typed result=pointer errno=preserved workers=34 capacity=8 failed_packet=freed alloc_failure=preserved reuse=24 attributes=unsupported
[SHELL_COMMAND_DONE] name=pthread code=0
```

目标函数按void *签名执行，返回原token；适配器只负责正确类型调用、启动包交接、结果载体和错误翻译。真实TID、TLS、CPU现场、等待及收拢继续由自己的内核管理。失败时谁拥有包，成功后谁可以free，均沿同一实际调用路径核对。

当前主执行者管理限制尚不同于完整POSIX对等线程，属性、线程独立exit、detach、取消、清理处理器、线程特定数据和pthread互斥／条件变量仍未提供。main返回或进程exit仍结束整个组。不能因为这段源码使用了pthread名字，就宣称C++标准线程库、Server-Lib或现代Linux全部行为已经兼容；后续逐项接入并保留明确失败范围。
