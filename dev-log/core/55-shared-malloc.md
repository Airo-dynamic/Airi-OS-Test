# K55 · 共享分配器，分别持有对象

K54 的互斥可以交接普通数据。现在让四位工作者共用 K48 的一个 arena，而不是各自准备一套堆。一次 malloc 可能分裂空块、增加活请求；一次 free 可能合并相邻块、减少计数。这些变化必须一起完成，其他分配者和快照才能看到同一份完整元数据。

从 K54 的同一目录继续。页面缓存仍固定 256KiB，零长度、calloc 溢出、realloc 失败保留旧对象和 fork/exec 语义继续保持。新增的是同进程分配操作的同步；应用对象里的普通读写仍由应用自己安排，不能因 malloc 能并发就同时释放或修改同一对象。

<a id="malloc-lock-composition"></a>
## calloc 不能再调用会加锁的公开 malloc

设工作者 A 调用 calloc(3,33)。如果只在原有 calloc 入口加锁，它内部再次调用公开 malloc，就会重复取得自己持有的非递归锁。realloc 扩容也会经过 malloc 和 free，同样不能简单给每个旧入口各套一层锁。

公开入口与内部操作因此分开。calloc 只在最外层取得一次私有锁，再调用 zero_unlocked；它检查 3×33=99，交给 allocate_unlocked 取得对象并清零。realloc 在同一次内部所有权中读取旧 requested、申请替代、复制旧有效字节、归还旧块。内部函数的 unlocked 表示“自身不重复加锁”，不是允许其他执行者无保护地直接调用。

这些函数限定为本文件 static，不作为用户运行库的外部符号。heap_mutex 同 objects、live_bytes、live_objects 一起在进程 BSS 中保存，所有 worker 通过同一页表访问它们。errno 则继续由每条执行者自己的 TLS 持有；锁调用返回原始结果，不把另一人的 ENOMEM 放进当前错误槽。

<a id="malloc-unlocked-core"></a>
## 保留块链行为，先建立内部操作

将 labs/airios/libc/malloc.c 替换为以下五块连续源码。先保留请求头、唯一堆描述和统计，把快照改为内部按值读取；后续再加入锁与公开入口，五块写完以后才构建。统计中的缓存字节与活请求字节仍是不同数量，free 不向内核归还缓存页。

<!-- write: labs/airios/libc/malloc.c -->
```c
#include <airi_mutex.h> // 全进程共享块链的私有互斥，不保护用户自己的对象读写。
#include <stdlib.h> // 对象接口与原始页分配分开。
#include <airi_malloc.h> // 只读观察页面缓存和活请求。
#include <airi_libc.h> // 页来源和用户字节函数，不链接内核memory.c。
#include <airi/heap.h> // 同一纯块链算法另编译进当前用户ELF。
#include <errno.h> // 失败使用当前执行者的TLS错误槽。
#define ARENA_BYTES (64u * 4096u) // 单次原始申请最多六十四页，本轮固定一个256KiB arena。
struct allocation { size_t requested, reserved; }; // 两个8字节字段，在块payload中保留16字节请求头。
_Static_assert(sizeof(struct allocation) == 16 && _Alignof(max_align_t) <= 16, "malloc alignment"); // 头后指针仍满足当前目标的基本类型对齐。
static struct airi_heap objects; // 零BSS表示尚未申请；与内核堆或其他进程的实例互不相干。
static size_t live_bytes, live_objects; // 统计请求大小，不把补齐、块头和空闲缓存混入活字节。
static struct airi_malloc_info info_unlocked(void) { // 不暴露内部块链或页面句柄。
    return (struct airi_malloc_info){objects.bytes, live_bytes, live_objects}; // 调用者已持有内部锁，三项来自同一元数据状态。
}
```

allocate_unlocked 保持按需申请 arena、分裂和请求头记录。第一个有效申请才调用原始页分配；初始化失败若只是没有页面，返回 NULL 与 ENOMEM，外层仍会解锁。已取得对齐页面后的 heap_init 对当前固定输入必须成立；若这个内部不变量被破坏，结束进程组，让内核收拢全部页，而不是在 worker 存在时调用已经禁止的 release 掩盖错误。

<!-- append: labs/airios/libc/malloc.c -->
```c
static void *allocate_unlocked(size_t bytes) { // 对象从当前进程的arena划分，不能把返回值交airi_release。
    if (!bytes) // 本实现的零请求是明确的无对象操作。
        return NULL; // 不分配页面，也不改旧errno。
    if (bytes > ARENA_BYTES - 32 - sizeof(struct allocation)) { // 32为既有堆块头；先拒绝不可能放下的请求，避免大小相加溢出。
        errno = ENOMEM; // 有界容量失败与乘法溢出统一为内存不足。
        return NULL; // 尚未改动旧对象或页面缓存。
    }
    if (!objects.first) { // 第一个有效请求才真正申请用户页。
        void *arena = airi_alloc(ARENA_BYTES); // 页被当前进程拥有，内核记录完整映射。
        if (!arena) { // 页预算不足时原始接口只返回NULL。
            errno = ENOMEM; // 对外使用标准源码错误形式。
            return NULL; // 静态堆描述仍为空，下一次可以重试。
        }
        if (!airi_heap_init(&objects, arena, ARENA_BYTES)) { // 已知页对齐与容量应满足纯算法合同。
            airi_exit(125); // 已知对齐和容量必须可接入；worker存在时不能靠禁止的release掩盖内部错误。
        }
    }
    struct allocation *header = airi_heap_alloc(&objects, bytes + sizeof(*header)); // 块链负责补齐与分裂，返回16字节对齐payload。
    if (!header) { // 碎片或仍存活对象占满arena。
        errno = ENOMEM; // 失败不自动解除任何已有对象。
        return NULL; // 旧对象仍可访问和归还。
    }
    *header = (struct allocation){bytes, 0}; // 记录请求而非对齐容量，realloc只能复制真实旧范围。
    live_bytes += bytes; // arena有界，所有存活请求之和不会使size_t溢出。
    ++live_objects; // 新对象从此由调用方借用。
    return header + 1; // 跨过16字节请求头，交付仍对齐的对象首地址。
}
```

release_unlocked 合并空块，zero_unlocked 调用内部申请后清零，resize_unlocked 的增长先申请后复制再归还。下面的调用名全部指向不重复加锁的版本。一次 realloc 暂时可以同时拥有旧新对象，但其他快照要等整个公开调用解锁，不能读到复合变化中间的统计。

<!-- append: labs/airios/libc/malloc.c -->
```c
static void release_unlocked(void *pointer) { // 调用者必须交回本库仍存活的精确对象起点，错误free属于未定义行为。
    if (!pointer) // NULL不代表一个块。
        return; // 保留errno和所有计数。
    struct allocation *header = (struct allocation *)pointer - 1; // 从有效对象回到请求头，不把任意指针当作安全输入。
    size_t bytes = header->requested; // 合并空块可能覆盖元数据，归还前先保存真实请求长度。
    if (airi_heap_free(&objects, header)) { // 纯算法只回收对象与合并相邻空块，不解除映射。
        live_bytes -= bytes; // 只扣除这个已归还对象的请求。
        --live_objects; // 页面缓存继续属于当前arena。
    }
}
static void *zero_unlocked(size_t count, size_t bytes) { // 不能先乘后检查，否则回绕可能得到很小的错误对象。
    if (bytes && count > SIZE_MAX / bytes) { // 先除后比，零元素大小也不发生除零。
        errno = ENOMEM; // 真实请求无法表示，不能仅分配回绕后的前缀。
        return NULL; // arena、旧对象与计数保持。
    }
    size_t total = count * bytes; // 上面的条件证明本次乘法可表示。
    void *pointer = allocate_unlocked(total); // 零总量沿用NULL且保留errno的策略。
    if (pointer) // 分配失败或零请求都不能解引用NULL。
        memset(pointer, 0, total); // 即使复用含旧内容的块，也清零完整请求。
    return pointer; // calloc只承诺字节全零，不等于任意类型的语义构造。
}
static void *resize_unlocked(void *pointer, size_t bytes) { // 正常扩容先取得新对象，失败不得释放旧对象。
    if (!pointer) // NULL没有旧生命周期。
        return allocate_unlocked(bytes); // 包括本实现明确的零请求规则。
    if (!bytes) { // 本C11实现特定选择，不宣称所有标准版本都保证这一行为。
        release_unlocked(pointer); // 旧对象生命周期结束。
        return NULL; // errno保持，不代表内存不足。
    }
    struct allocation *header = (struct allocation *)pointer - 1; // 非NULL必须来自仍存活的本库对象。
    size_t old_bytes = header->requested; // 只复制原请求，不读取对齐填充或别的对象。
    if (bytes <= old_bytes) { // 缩小或相同大小时保留同一块，暂不回收块内尾部。
        header->requested = bytes; // 后续扩容最多复制当前真实长度。
        live_bytes -= old_bytes - bytes; // 活字节缩小，底层借用容量暂时不变。
        return pointer; // 当前实现不搬动缩小对象。
    }
    void *replacement = allocate_unlocked(bytes); // 暂时同时持有旧新对象，峰值容量可能导致失败。
    if (!replacement) // 旧块仍被借出，原指针及内容保持。
        return NULL; // 调用者应先保存返回值，再决定是否替换旧变量。
    memcpy(replacement, pointer, old_bytes); // 新旧块互不重叠，仅复制仍属旧对象的字节。
    release_unlocked(pointer); // 新内容准备好后才结束旧生命周期。
    return replacement; // 成功后调用者只能继续使用新指针。
}
```

<a id="malloc-private-guard"></a>
## 等到私有元数据可用，再完成整次调用

enter_heap 使用 K54 的同一把私有锁。单次等待仍是有限的 200 tick；到期可以重试，不能让 free 因一次调度延迟就无声丢掉本应归还的对象。只有真正取得后，才访问块链。当前内部临界区没有业务回调、取消或信号处理，不跨用户 I/O 等待，正常路径都会解锁；用户 CPU 故障或 exit 则结束整个进程组。

其他内部锁错误表示元数据同步不再可信，不能假装分配失败以后继续操作损坏状态，因此以进程退出 125 结束。这里没有提供独立线程取消或持有者死亡后的 allocator 恢复协议。锁也不适合在中断、信号处理器或其他重入回调中调用。

AIRI_MALLOC_OBSERVE_YIELD 只给后面的并发观察版本使用：取得私有锁后故意 yield，让其他工作者实际阻塞在同一个 owner 字。普通 malloc.o 不定义这个宏，不插入主动调度。两份对象来自同一源码，差别由明确的编译参数控制，不把观察中的主动换执行者冒称日常调用都必然发生的抢占。

<!-- append: labs/airios/libc/malloc.c -->
```c
static airi_mutex heap_mutex = AIRI_MUTEX_INITIALIZER; // 同一进程的全部对象与统计共用这一把私有锁。
static void enter_heap(void) { // 临界区只调用本文件内部操作，不跨业务回调或不受控等待。
    long result; // 内部串行化不能把free请求因一次等待到期而静默丢弃。
    do { // 抢占或竞争可以重试有限等待，未取得时不访问任何元数据。
        result = airi_mutex_lock(&heap_mutex, 200); // 每次等待有真实截止，持有者仍可被调度继续。
    } while (result == -ETIMEDOUT); // 普通内部临界区会解锁；当前无取消或信号回调。
    if (result) airi_exit(125); // 私有锁不变量损坏不能伪造分配成功、忽略free或返回撕裂快照。
#ifdef AIRI_MALLOC_OBSERVE_YIELD // 只在明确的并发观察ELF中启用，不改变普通malloc.o。
    airi_yield(); // 故意持锁换执行者，让其他工作者真正进入futex竞争。
#endif // 正常对象不插入这次观察用主动调度。
}
static void leave_heap(void) { // 所有正常与容量失败路径都经过同一释放出口。
    if (airi_mutex_unlock(&heap_mutex)) airi_exit(125); // 此时旧所有权已释放；内部故障终止所属进程组。
}
struct airi_malloc_info airi_malloc_info(void) { // 快照自身不分配对象，不修改errno。
    enter_heap(); // 读取三项之前先取得同一元数据锁。
    struct airi_malloc_info value = info_unlocked(); // 按值复制，不向调用方暴露块链或锁。
    leave_heap(); // 返回后其他工作者可以继续，快照不是持续冻结状态。
    return value; // 三项内部一致，但可能已不是当前最新值。
}
```

每个公开入口都只调用一次 enter_heap 和 leave_heap。内部函数返回 NULL、溢出或容量失败以后，控制流回到同一个释放出口；零请求和 free(NULL) 保留原来的空操作策略。airi_malloc_info 也持相同锁，将三项复制到调用者独有的栈后释放：返回值内部一致，但不持续冻结未来分配。

<!-- append: labs/airios/libc/malloc.c -->
```c
void *malloc(size_t bytes) { // 每次公开调用只取得一次锁。
    if (!bytes) return NULL; // 零请求不分配也不修改errno，保持既有策略。
    enter_heap(); // 保护按需arena初始化、块链和活请求统计。
    void *value = allocate_unlocked(bytes); // 失败也会走释放出口。
    leave_heap(); // 发布元数据以后再把对象交给调用者。
    return value; // 成功对象内容仍由应用自行协调访问。
}
void free(void *pointer) { // 仍要求精确的本库活对象起点；重复释放或并发双重所有权不合法。
    if (!pointer) return; // NULL不访问元数据，也不取得锁。
    enter_heap(); // 其他工作者不能同时分裂、合并或改统计。
    release_unlocked(pointer); // 只归还对象，继续缓存页面到exit/exec。
    leave_heap(); // 元数据一致以后允许另一执行者使用空块。
}
void *calloc(size_t count, size_t bytes) { // 清零过程也在同一次已取得的内部所有权中完成。
    if (!count || !bytes) return NULL; // 零总量保持无对象且保留errno。
    enter_heap(); // 不能再从zero_unlocked调用公开malloc重入锁。
    void *value = zero_unlocked(count, bytes); // 先查乘法，再分配和清零。
    leave_heap(); // 包括溢出或容量失败都释放锁。
    return value; // 新对象全零，其他线程不能凭地址猜测其所有权。
}
void *realloc(void *pointer, size_t bytes) { // 复合变化在同一锁范围内完成，仍保持失败旧对象不变。
    if (!pointer && !bytes) return NULL; // 与既有零请求约定相同。
    enter_heap(); // 旧长度读取、申请替代和归还不能被另一个分配操作穿插。
    void *value = resize_unlocked(pointer, bytes); // 内部只调用不重复加锁的allocate/release。
    leave_heap(); // 失败不会把锁遗留给未来调用。
    return value; // 应用仍不得同时在多个执行者重分配或释放同一对象。
}
```

更新公共头的能力与所有权说明。free 接收的仍必须是本库有效精确起点；同步分配元数据并不能让两个人同时拥有同一指针的归还权。

<!-- replace: labs/airios/include/airi_malloc.h -->
<!-- before -->
```c
#ifdef __cplusplus // 结构不含内部块指针，函数保留C名字。
extern "C" { // 与stdlib接口使用同一链接约定。
#endif // C源码直接声明。
struct airi_malloc_info airi_malloc_info(void); // 当前单线程快照，不分配、不清理也不改errno。
#ifdef __cplusplus // 结束链接范围。
} // 此观察接口不是线程同步工具。
#endif // 结束语言差异。
#endif // 结束包含保护。
```
<!-- after -->
```c
#ifdef __cplusplus // 结构不含内部块指针，函数保留C名字。
extern "C" { // 与stdlib接口使用同一链接约定。
#endif // C源码直接声明。
struct airi_malloc_info airi_malloc_info(void); // 内部锁保护的一致快照，不分配、不清理也不改errno。
#ifdef __cplusplus // 结束链接范围。
} // 返回后不冻结对象或元数据，应用仍要管理自己的数据所有权。
#endif // 结束语言差异。
#endif // 结束包含保护。
```

<!-- replace: labs/airios/include/stdlib.h -->
<!-- before -->
```c
#ifndef AIRI_STDLIB_H // 当前只提供单线程有界对象分配，不引入宿主stdlib。
#define AIRI_STDLIB_H // 包含保护。
#include <stddef.h> // 对象大小沿用目标工具链的size_t。
#ifdef __cplusplus // 为未来C++调用保留C链接名字。
extern "C" { // 声明本身不启动C++运行库。
#endif // C11无需链接包装。
void *malloc(size_t bytes); // 非零失败NULL并设置ENOMEM；零长度NULL且不改errno。
void free(void *pointer); // NULL空操作；其他参数必须是本库仍存活的对象起点。
void *calloc(size_t count, size_t bytes); // 先检查乘法溢出，再分配并清零请求范围。
void *realloc(void *pointer, size_t bytes); // 失败保持旧对象；本C11实现将非NULL零长度视为free。
#ifdef __cplusplus // 结束函数的C链接范围。
```
<!-- after -->
```c
#ifndef AIRI_STDLIB_H // 有界对象分配共享内部元数据锁，不引入宿主stdlib。
#define AIRI_STDLIB_H // 包含保护。
#include <stddef.h> // 对象大小沿用目标工具链的size_t。
#ifdef __cplusplus // 为未来C++调用保留C链接名字。
extern "C" { // 声明本身不启动C++运行库。
#endif // C11无需链接包装。
void *malloc(size_t bytes); // 非零失败NULL并设置ENOMEM；零长度NULL且不改errno。
void free(void *pointer); // NULL空操作；其他参数必须是本库仍存活且由调用者独占交还权的对象起点。
void *calloc(size_t count, size_t bytes); // 先检查乘法溢出，再分配并清零请求范围。
void *realloc(void *pointer, size_t bytes); // 失败保持旧对象；本C11实现将非NULL零长度视为free。
#ifdef __cplusplus // 结束函数的C链接范围。
```

<a id="malloc-shared-observation"></a>
## 四个人一起开始，只创建一个 arena

新建 malloc-threads.c。主执行者先取快照，确认尚无 arena，再创建四位真正共享地址空间的 worker，最后用 gate 的 release/acquire 放行。每人共24轮：申请变长对象、取得全零对象、在自己的对象仍活着时让别人分配，再增长并核对旧内容，最后归还两个对象。

以 round=1 为例，bytes=33，calloc 需要99字节，realloc 将第一份33字节扩成66字节。增长以后只核对旧33字节的图案，不把新增尾部当成已经初始化。各工作者的图案含自己的身份和轮数，所以别人的复用或重叠会被内容比较发现。

每人的 errno 先设为300+身份，成功操作必须保持。循环之后各自故意请求 SIZE_MAX×2，返回 NULL 和 ENOMEM；接着恢复自己的标记，其他工作者仍继续。这同时观察溢出出口没有遗留私有锁，错误槽也没有变成共享状态。

<!-- write: labs/airios/malloc-threads.c -->
```c
#include <stdlib.h> // 公开对象接口，现在共用内部元数据锁。
#include <airi_malloc.h> // 同一时刻三项一致的统计快照。
#include <airi_threads.h> // 真实共享执行者，不使用fork模拟共享堆。
#include <airi_libc.h> // PIT、yield和串口观察。
#include <errno.h> // 每个执行者保留独立错误槽。
#include <stdatomic.h> // 对象交接用release/acquire明确移交所有权。
static _Atomic unsigned gate, done; // 全部工作者创建完后才一起争取第一次arena初始化。
static _Atomic uintptr_t handed[4]; // 每槽只发布一次活对象指针，交给下一位工作者归还。
static long work(void *argument) { // 四位工作者共享同一BSS、块链和页面缓存。
    unsigned identity = (unsigned)(uintptr_t)argument; // 参数是0至3的小数值，不是临时对象地址。
    int tag = (int)(300 + identity); // 成功分配与锁等待都应保留本执行者的错误状态。
    errno = tag; // 工作者TLS由内核从声明模板建立。
    while (!atomic_load_explicit(&gate, memory_order_acquire)) airi_yield(); // 主执行者还在创建其他工作者，不提前开始申请。
    for (unsigned round = 0; round < 24; ++round) { // 每轮只拥有两个对象，内存峰值有明确上界。
        size_t bytes = 16 + (round % 8) * 17; // 不同请求长度形成补齐、分裂和合并。
        unsigned char *first = malloc(bytes), *zero = calloc(3, bytes); // 所有线程共用唯一按需arena。
        if (!first || !zero || ((uintptr_t)first & 15) || ((uintptr_t)zero & 15) || errno != tag) return -201; // 成功不得借另一个执行者的errno。
        for (size_t i = 0; i < bytes; ++i) first[i] = (unsigned char)(identity * 31 + round + i); // 该对象当前只由本工作者使用。
        for (size_t i = 0; i < 3 * bytes; ++i) if (zero[i]) return -202; // 复用过的块也必须清零完整请求。
        airi_yield(); // 对象可以跨其他执行者分配操作保持内容，不持有内部分配锁。
        unsigned char *grown = realloc(first, bytes * 2); // 复合操作只在内部取得一次锁。
        if (!grown || errno != tag) return -203; // 失败由进程组退出收拢，不伪造继续成功。
        for (size_t i = 0; i < bytes; ++i) if (grown[i] != (unsigned char)(identity * 31 + round + i)) return -204; // 只检查原有效范围。
        free(grown); free(zero); // 正常释放不能因一次内部等待到期而静默丢失请求。
    }
    volatile size_t overflow = SIZE_MAX; // 保留运行时乘法溢出分支，不由优化器折叠观察。
    if (calloc(overflow, 2) || errno != ENOMEM) return -205; // 错误出口必须释放内部锁，其他执行者继续分配。
    errno = tag; // 后续成功仍保留本执行者明确设定的错误。
```

<a id="malloc-transfer-object"></a>
## 对象可以换持有者，元数据锁不会替应用交接

每人另申请64字节 gift，写完图案后，把地址以 release 存到自己的 handed 槽，从此不再访问或释放这份对象。下一人以 acquire 取出地址并读图案，随后 free 精确起点。四个槽每个只发布一次，每份对象恰有一个接收者；这不是可以无序反复覆盖的队列，数组仍保留旧地址也不代表它还能再次消费。

内部分配锁只保护块链。把指针写进共享数组、保证内容可见和决定谁负责 free，是应用层的单独协议。done 仅在当前工作者不再拥有对象以后发布，主执行者仍须 join 才回收执行资源。

<!-- append: labs/airios/malloc-threads.c -->
```c
    unsigned char *gift = malloc(64); // 新对象即将交给另一个执行者，不再由自己归还。
    if (!gift) return -206; // 不能发布无效指针。
    for (unsigned i = 0; i < 64; ++i) gift[i] = (unsigned char)(identity + i); // 发布以前写完完整内容。
    atomic_store_explicit(&handed[identity], (uintptr_t)gift, memory_order_release); // 从此放弃对gift的访问和释放权。
    unsigned previous = (identity + 3) % 4; // 每人只接收上一人的对象，恰好一位接收者。
    uint64_t start = airi_ticks(); // 交接夹具也必须在有限时间内推进。
    uintptr_t received; // acquire取得的不只是地址，还包含对应发布的数据顺序。
    while (!(received = atomic_load_explicit(&handed[previous], memory_order_acquire))) { // 等前一位发布，不在分配器锁内等待。
        if (airi_ticks() - start >= 300) return -207; // 失败不能无限卡住shell观察。
        airi_yield(); // 让其他生产者继续完成自己的分配。
    }
    unsigned char *owned = (unsigned char *)received; // 现在由本执行者独占这个活对象。
    for (unsigned i = 0; i < 64; ++i) if (owned[i] != (unsigned char)(previous + i)) return -208; // 读取只发生在接收所有权以后。
    free(owned); // 可由不同TID向同一个分配器归还精确起点。
    if (errno != tag) return -209; // 另一个线程的ENOMEM不能污染当前TLS。
    atomic_fetch_add_explicit(&done, 1, memory_order_release); // 当前工作者不再持有任何对象。
    return 24; // 主执行者通过真实join取得完成轮数。
}
```

主执行者在工作期间反复取快照。它检查缓存范围、活字节范围和“零活对象对应零活字节”，不要求快照返回后其他执行者停止变化。四位完成并 join 后，最终缓存仍为262144，活字节和活对象均为0。进程退出才由内核归还这些页；跨线程 free 也没有改变唯一地址空间所有者。

<!-- append: labs/airios/malloc-threads.c -->
```c
static void require(int condition, const char *message) { // 单独保留主执行者的具体错误原因。
    if (!condition) { airi_puts(message); airi_puts("\n"); airi_exit(83); } // 进程组失败时由内核统一回收页面和执行栈。
}
int main(void) { // 初始堆描述和内部锁均来自本ELF的新BSS。
    struct airi_malloc_info initial = airi_malloc_info(); // 观察快照不能替自己申请arena。
    require(!initial.arena_bytes && !initial.live_bytes && !initial.live_objects, "lazy shared heap"); // 四人放行前确实尚未创建arena。
    errno = 600; // 主执行者也有独立TLS错误状态。
    long ids[4]; // 所有TID的生命周期都持续到join。
    for (unsigned i = 0; i < 4; ++i) { // 同一进程中四位真实工作者。
        ids[i] = airi_thread_create(work, (void *)(uintptr_t)i); // 不复制进程地址空间或FD。
        require(ids[i] > 0, "create heap workers"); // 资源不足应明确停止观察。
    }
    atomic_store_explicit(&gate, 1, memory_order_release); // 四人都存在后才开始争取首次初始化。
    uint64_t start = airi_ticks(); unsigned snapshots = 0; // 主执行者并发观察，不读取内部块指针。
    while (atomic_load_explicit(&done, memory_order_acquire) != 4) { // 快照和其他线程的分配操作互斥。
        struct airi_malloc_info view = airi_malloc_info(); // 取回按值一致的三项，随后其他工作者仍可推进。
        require((!view.arena_bytes || view.arena_bytes == 262144) && view.live_bytes <= view.arena_bytes && (!view.live_objects == !view.live_bytes), "consistent heap snapshot"); // 空闲请求与缓存页面分别统计。
        require(airi_ticks() - start < 1000 && errno == 600, "heap progress and parent errno"); // 不把卡住或错误状态串扰当完成。
        ++snapshots; airi_yield(); // 不在主循环持续抢占私有锁，让工作者完成。
    }
    for (unsigned i = 0; i < 4; ++i) { // done只说明完成发布，join才收拢执行者。
        long result; // 每个工作者的结果独立复制到当前栈。
        require(airi_thread_join(ids[i], &result) == 0 && result == 24, "join heap worker"); // 每人都完成24轮及跨线程对象归还。
    }
    struct airi_malloc_info final = airi_malloc_info(); // 全部工作者已结束，此刻没有活对象。
    require(final.arena_bytes == 262144 && !final.live_bytes && !final.live_objects && snapshots, "shared heap fully returned"); // 页面缓存仍在，不能称为已归还物理页。
    airi_puts("[MALLOC_THREADS_OK] workers=4 rounds=96 arena=262144 cross_free=4 snapshots=consistent errno=private critical_yield=1\n"); // 所有真实状态与生命周期检查完成后才输出。
    return 0; // 用户shell收取成功，进程退出才归还页面缓存。
}
```

<a id="malloc-shared-build"></a>
## 普通实现与明确的观察编译分开

Makefile 给原 malloc-probe 增加互斥和共享执行库依赖，旧的容量、fork/exec 与失败保留观察继续运行。新的 malloc-observe.o 来自同一个 malloc.c，单独定义持锁 yield 宏；heap-threads.elf 只选它，不同时链接普通 malloc.o。宏在预处理时决定是否保留那条 airi_yield，不生成另一份隐藏算法。

<!-- replace: Makefile -->
<!-- before -->
```makefile
# 与其他用户对象使用同一输出目录和传递头依赖。
	@mkdir -p $(dir $@)
# 所有状态位于当前用户ELF的BSS，编译器不访问内核memory.c。
	$(CC) $(USER_FLAGS) -Ilabs/airios/include -c $< -o $@
# 分配器观察是独立main，不把标准malloc塞进USER_COMMON。
$(OUT)/user/malloc-probe.elf: $(USER_COMMON) $(OUT)/user/posix.o $(OUT)/user/heap-algorithm.o $(OUT)/user/malloc.o $(OUT)/user/malloc-probe.o labs/airios/program.ld
# 原始库、POSIX错误槽、纯算法与用户对象包装都在这个静态ELF内。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(filter %.o,$^) -o $@
# 包装等待真实用户分配程序。
$(OUT)/labs/airios/images.o: $(OUT)/user/malloc-probe.elf
```
<!-- after -->
```makefile
# 与其他用户对象使用同一输出目录和传递头依赖。
	@mkdir -p $(dir $@)
# 所有状态位于当前用户ELF的BSS，编译器不访问内核memory.c。
	$(CC) $(USER_FLAGS) -Ilabs/airios/include -c $< -o $@
# 分配器观察是独立main，不把标准malloc塞进USER_COMMON。
$(OUT)/user/malloc-probe.elf: $(USER_COMMON) $(OUT)/user/posix.o $(OUT)/user/heap-algorithm.o $(OUT)/user/malloc.o $(OUT)/user/malloc-probe.o $(OUT)/user/mutex-lib.o $(OUT)/user/threads-lib.o $(OUT)/user/thread-entry.o labs/airios/program.ld
# 原始库、POSIX错误槽、纯算法与用户对象包装都在这个静态ELF内。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(filter %.o,$^) -o $@
# 包装等待真实用户分配程序。
$(OUT)/labs/airios/images.o: $(OUT)/user/malloc-probe.elf
```

<!-- replace: Makefile -->
<!-- before -->
```makefile
# 初始目录等待本配置镜像真正完成。
$(OUT)/labs/airios/images.o: $(OUT)/user/mutex.elf
# 路径仅供incbin打包实际字节。
$(OUT)/labs/airios/images.o: CPPFLAGS += -DAIRI_MUTEX_ELF='"$(OUT)/user/mutex.elf"'
```
<!-- after -->
```makefile
# 初始目录等待本配置镜像真正完成。
$(OUT)/labs/airios/images.o: $(OUT)/user/mutex.elf
# 路径仅供incbin打包实际字节。
$(OUT)/labs/airios/images.o: CPPFLAGS += -DAIRI_MUTEX_ELF='"$(OUT)/user/mutex.elf"'

# 与普通malloc.o使用同一实现，仅这份观察对象在持内部锁时主动yield。
$(OUT)/user/malloc-observe.o: labs/airios/libc/malloc.c Makefile
# 观察专用对象不覆盖普通分配器。
	@mkdir -p $(dir $@)
# 宏只启用已经写在源码内的主动换执行者位置，不借宿主线程模拟。
	$(CC) $(USER_FLAGS) -DAIRI_MALLOC_OBSERVE_YIELD -Ilabs/airios/include -c $< -o $@
# 并发程序需要errno TLS、纯块链、同步包装和共享执行库。
$(OUT)/user/heap-threads.elf: $(USER_COMMON) $(OUT)/user/posix.o $(OUT)/user/heap-algorithm.o $(OUT)/user/malloc-observe.o $(OUT)/user/mutex-lib.o $(OUT)/user/threads-lib.o $(OUT)/user/thread-entry.o $(OUT)/user/malloc-threads.o labs/airios/program.ld
# 一份静态ELF只链接一个malloc版本，不混入宿主分配器。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(filter %.o,$^) -o $@
# 初始目录只消费真正生成的新观察ELF。
$(OUT)/labs/airios/images.o: $(OUT)/user/heap-threads.elf
# 路径宏只交给只读incbin包装。
$(OUT)/labs/airios/images.o: CPPFLAGS += -DAIRI_HEAP_THREADS_ELF='"$(OUT)/user/heap-threads.elf"'
```

<!-- append: labs/airios/images.S -->
```asm

.balign 8 /* 四个执行者共享分配器的真实用户ELF。 */
.global airi_heap_threads_elf_start, airi_heap_threads_elf_end /* 初始目录不保存宿主进程指针。 */
airi_heap_threads_elf_start: /* 本配置完整静态程序文件。 */
.incbin AIRI_HEAP_THREADS_ELF /* 明确选择带持锁yield的观察版本。 */
airi_heap_threads_elf_end: /* 输入的排他右端。 */
```

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
extern const uint8_t airi_process_elf_start[], airi_process_elf_end[]; // 新的fork/wait和抢占工作负载。
extern const uint8_t airi_vector_elf_start[], airi_vector_elf_end[]; // 显式偏移向量正常与坏范围观察。
extern const uint8_t airi_malloc_elf_start[], airi_malloc_elf_end[]; // 用户对象分配与页面缓存的独立ELF。
extern const uint8_t airi_mutex_elf_start[], airi_mutex_elf_end[]; // 原子所有权与futex等待组合的真实用户观察。
extern const uint8_t airi_futex_elf_start[], airi_futex_elf_end[]; // 私有内存等待与通知的真实用户程序。
extern const uint8_t airi_threads_elf_start[], airi_threads_elf_end[]; // 同进程共享内存、FD与异步owner的真实工作执行者。
```
<!-- after -->
```c
extern const uint8_t airi_process_elf_start[], airi_process_elf_end[]; // 新的fork/wait和抢占工作负载。
extern const uint8_t airi_vector_elf_start[], airi_vector_elf_end[]; // 显式偏移向量正常与坏范围观察。
extern const uint8_t airi_malloc_elf_start[], airi_malloc_elf_end[]; // 用户对象分配与页面缓存的独立ELF。
extern const uint8_t airi_heap_threads_elf_start[], airi_heap_threads_elf_end[]; // 同一arena并发分配、跨执行者归还与快照。
extern const uint8_t airi_mutex_elf_start[], airi_mutex_elf_end[]; // 原子所有权与futex等待组合的真实用户观察。
extern const uint8_t airi_futex_elf_start[], airi_futex_elf_end[]; // 私有内存等待与通知的真实用户程序。
extern const uint8_t airi_threads_elf_start[], airi_threads_elf_end[]; // 同进程共享内存、FD与异步owner的真实工作执行者。
```

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    {"vector", airi_vector_elf_start, airi_vector_elf_end}, // 聚合、分散、短前缀与错误原子性。
    {"heap", airi_malloc_elf_start, airi_malloc_elf_end}, // 单线程用户对象与错误路径。
    {"heap-reset", airi_malloc_elf_start, airi_malloc_elf_end}, // 同一ELF新BSS和按需arena。
    {"mutex", airi_mutex_elf_start, airi_mutex_elf_end}, // 临界区、错误所有者、截止与非robust寿命。
    {"futex", airi_futex_elf_start, airi_futex_elf_end}, // 相等比较与登记、超时及进程私有键。
    {"threads", airi_threads_elf_start, airi_threads_elf_end}, // 独立TLS/CPU状态、join与整个进程退出。
```
<!-- after -->
```c
    {"vector", airi_vector_elf_start, airi_vector_elf_end}, // 聚合、分散、短前缀与错误原子性。
    {"heap", airi_malloc_elf_start, airi_malloc_elf_end}, // 单线程用户对象与错误路径。
    {"heap-reset", airi_malloc_elf_start, airi_malloc_elf_end}, // 同一ELF新BSS和按需arena。
    {"heap-threads", airi_heap_threads_elf_start, airi_heap_threads_elf_end}, // 显式持内部锁yield的用户观察程序。
    {"mutex", airi_mutex_elf_start, airi_mutex_elf_end}, // 临界区、错误所有者、截止与非robust寿命。
    {"futex", airi_futex_elf_start, airi_futex_elf_end}, // 相等比较与登记、超时及进程私有键。
    {"threads", airi_threads_elf_start, airi_threads_elf_end}, // 独立TLS/CPU状态、join与整个进程退出。
```

自动 shell 会话必须看到 heap-threads 自己的成功标记、真正的 futex 等待，以及同一进程创建并返回的四位工作者。join 可能已经能立即取得结果，因此不强求出现 THREAD_JOIN_WAIT；具体调度先后不能成为“共享堆正确”的额外前提。

<!-- replace: scripts/shell_test.py -->
<!-- before -->
```python
                mutex_output=text[offset:] # 当前程序而非旧日志提供结果。
                assert '[MUTEX_USER_OK] counter=64 critical_yield=1 foreign=denied self=deadlock timeout=pit owner_death=nonrobust' in mutex_output # 所有权、真实等待和非robust寿命都由guest核对。
                assert len(re.findall(r'\[FUTEX_WAIT\] pid=',mutex_output))>=4 and 'result=-110' in mutex_output # 两位争用者、超时者和死亡所有者观察确实进入等待。
                for repetition in range(2): # 同一shell连续完成两次线程命令，上一组退出回收后应能重新创建。
                    offset=len(text); wire.sendall(b'threads\n'); text=wait('[SHELL_COMMAND_DONE] name=threads code=0',offset) # 真正同地址空间执行，不能由fork复制或宿主线程模拟。
                    thread_output=text[offset:] # 所有身份与标记均限定在这条命令之后。
```
<!-- after -->
```python
                mutex_output=text[offset:] # 当前程序而非旧日志提供结果。
                assert '[MUTEX_USER_OK] counter=64 critical_yield=1 foreign=denied self=deadlock timeout=pit owner_death=nonrobust' in mutex_output # 所有权、真实等待和非robust寿命都由guest核对。
                assert len(re.findall(r'\[FUTEX_WAIT\] pid=',mutex_output))>=4 and 'result=-110' in mutex_output # 两位争用者、超时者和死亡所有者观察确实进入等待。
                offset=len(text); wire.sendall(b'heap-threads\n'); text=wait('[SHELL_COMMAND_DONE] name=heap-threads code=0',offset) # 四个真实执行者共享唯一arena，观察对象交接。
                heap_threads=text[offset:] # 不用旧heap命令代替并发结果。
                assert '[MALLOC_THREADS_OK] workers=4 rounds=96 arena=262144 cross_free=4 snapshots=consistent errno=private critical_yield=1' in heap_threads # 每个工作者和主快照均由guest核对。
                assert '[FUTEX_WAIT]' in heap_threads # 观察编译明确在内部持锁yield，必须实际进入竞争等待。
                heap_owner=re.search(r'\[EXEC_COMMIT\] pid=(\d+) program=heap-threads ',heap_threads) # 固定本条命令的真实进程身份。
                assert heap_owner and len(re.findall(r'\[THREAD_CREATE\] pid='+heap_owner.group(1)+r' tid=',heap_threads))==4 and len(re.findall(r'\[THREAD_RETURN\] pid='+heap_owner.group(1)+r' tid=',heap_threads))==4 # join可以立即成功，不能强求特定阻塞时序。
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
                result.update(owned_mutex=True,mutex_critical_updates=64,private_futex=True,futex_registrations=8,user_heap=True,shared_executions=True,thread_commands=2,worker_capacity=8,worker_reuse_rounds=24) # 关键字把实际已验收字段加入结果字典，不替代上面的逐命令检查。
                result_path.write_text(json.dumps(result,indent=2)+'\n'); print('PASS user shell: alloc/file/async wait/other task/CQ/readback, failed exec, return to monitor') # 阶段演示实际完成。
                return result # 总入口可收集。
            finally: # 包括断言、超时和QEMU错误。
```
<!-- after -->
```python
                offset=len(text); wire.sendall(b'echo after-shell\n'); text=wait('[CONSOLE_UPDATE] view=0 commands=2 page=0',offset) # 回到内核后输入仍完整。
                assert 'after-shell' in text[offset:] # 不只是退出而丢了后续UART。
                result={'passed':True,'ring3_shell':True,'ram_file_and_device_readback':True,'other_user_during_cq_wait':True,'qmp_iops':1,'error_exec_survived':True,'returned_to_monitor':True,'orphan_rounds':24,'memory_reuse_rounds':128,'capacity_rounds':4,'capacity_pages':1024,'simd_preempt_fork_exec':True,'fs_base_preempt_fork_exec':True,'compiler_static_tls':True,'posix_errno_io_subset':True,'positioned_vectors':True} # 只发布已观察事实。
                result.update(shared_malloc=True,malloc_workers=4,malloc_rounds=96,owned_mutex=True,mutex_critical_updates=64,private_futex=True,futex_registrations=8,user_heap=True,shared_executions=True,thread_commands=2,worker_capacity=8,worker_reuse_rounds=24) # 关键字把实际已验收字段加入结果字典，不替代上面的逐命令检查。
                result_path.write_text(json.dumps(result,indent=2)+'\n'); print('PASS user shell: alloc/file/async wait/other task/CQ/readback, failed exec, return to monitor') # 阶段演示实际完成。
                return result # 总入口可收集。
            finally: # 包括断言、超时和QEMU错误。
```

<!-- run -->
```sh
./airios build  # 普通malloc和观察版本分别静态链接，缺少互斥依赖必须报错
```

<!-- run -->
```sh
./airios test  # 运行旧单线程容量/继承用例与新四执行者共享arena观察
```

<a id="malloc-shared-result"></a>
## 看活对象归零，再看进程释放页面

本轮 heap-threads 在真实 guest 输出：

```text
[MALLOC_THREADS_OK] workers=4 rounds=96 arena=262144 cross_free=4 snapshots=consistent errno=private critical_yield=1
[SHELL_COMMAND_DONE] name=heap-threads code=0
```

交互操作为 `./airios run`，再在内核提示符输入 `run sh`，依次运行 `heap`、`heap-threads`、`programs`、`exit`。heap 验证原先页面不足后的申请重试、calloc溢出、realloc峰值失败保持、128轮复用及fork/exec隔离；heap-threads 验证共享块链与对象交还。两者不是同一段程序，只是分别选择了普通或观察编译的同一分配实现。

观察版本的 FUTEX_WAIT 来自真实用户元数据锁竞争。主执行者在首个申请之前取快照时，arena仍不存在；放行后四人共用一份初始化。最后快照为“缓存仍在、活对象为零”，shell收取进程以后，原有会话资源检查再核对页和FD已经回到基准，不能把 free 等同于返回内核页面。

当前仍是固定256KiB、单锁串行化的分配器，没有线程缓存、多个arena或取消恢复。互斥仍包含TID查询和通知系统调用。下一步可先减少身份查询，再考虑分配竞争与POSIX/C++运行库接口；不能仅靠增加worker数就把这份实现称为完整现代运行库。Linux的pthread/条件变量对照继续保留未验证状态，最近实际连接为K53的SSH255。
