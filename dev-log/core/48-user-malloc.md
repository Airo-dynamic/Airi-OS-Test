# K48 · 在用户页中分配和归还 C 对象

从 K47 继续。一个服务器请求可能只需要33字节记录和97字节状态；直接调用 `airi_alloc` 会让它们各自占用完整页面。这里为用户程序提供 `malloc/free/calloc/realloc`：第一次需要对象时申请一段用户页，在其中划分小对象，归还以后继续复用。串口程序 `heap` 完成正常与失败路径，成功替换到 `heap-reset` 后再观察新的分配器。

本轮固定一个256 KiB内存范围，称为 arena。它属于当前进程，只有一个线程能调用分配器，不支持并发调用、自动增长、自动收缩或大于16字节的扩展对齐。当前调用上限为262096字节，实际是否成功还取决于碎片和活对象。这个容量用于建立完整对象生命周期；后续接运行库时再按真实需要扩展。

<a id="user-arena"></a>
## 同一块链算法，两个独立运行实例

K17 的 `heap.c` 在已经可写的连续范围内维护32字节块头、空闲标记和后继指针，K38 为它增加了连续范围扩展。该文件只使用目标整数类型和普通内存读写，不发系统调用，也不访问页表、串口或内核全局状态。本节保留这份已写过的源码，用用户编译参数再生成 `heap-algorithm.o`。它被静态链接到新的用户ELF，内核中的那份代码与堆描述符保持独立。

例如用户页接口返回起点 `0x10000000`，范围长度262144。第一个块头占32字节，块链交出的 payload 从 `0x10000020` 开始。标准分配包装还要记录原请求长度，于是再占一个16字节请求头，最终把 `0x10000030` 交给调用者。33字节请求连同请求头共49字节，底层向上补齐到64字节；调用者只能使用其中属于自己的33字节。

这时底层块链记住的容量是64字节，用户实际请求却只有33字节。块链要用容量判断能否分裂、合并和再次分配；以后 realloc 搬对象时，则只能按请求头的 `requested=33` 复制旧内容，不能把补齐空间也当作用户数据。活请求统计同样累加33，页面缓存另算。

请求头的另8字节保留为0，使它恰好占16字节，头后的对象继续满足当前 x86-64 基本类型的对齐。`max_align_t` 是工具链定义的基本类型最大对齐类型，静态断言会拒绝换目标后仍沿用不足对齐的情况。

页面归内核的当前进程记录持有，arena归用户分配器管理，对象只借用其中一段。`free` 结束对象生命周期并合并空块，页面仍然映射。不能把 malloc 返回值交给 `airi_release`，也不能手动解除arena中的页面；那会让块头和其他活对象失去有效存储。退出或成功exec仍由既有内核回收全部旧用户页。

新建 `stdlib.h` 和 `airi_malloc.h`。前者只声明本轮真正支持的四个标准名字，后者用于当前程序观察三项数值，不属于系统调用ABI。`live_bytes` 是请求长度之和，`live_objects` 是活对象个数，`arena_bytes` 是持有的页面范围；它们不互相替代。

<!-- write: labs/airios/include/stdlib.h -->
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
} // 不改变对象布局。
#endif // 结束语言差异。
#endif // 结束包含保护。
```

<!-- write: labs/airios/include/airi_malloc.h -->
```c
#ifndef AIRI_MALLOC_H // 用户分配器的只读观察，不是内核ABI。
#define AIRI_MALLOC_H // 包含保护。
#include <stddef.h> // 各项按当前进程的字节与对象数计。
struct airi_malloc_info { size_t arena_bytes, live_bytes, live_objects; }; // 页面缓存、请求字节和活对象分别统计。
#ifdef __cplusplus // 结构不含内部块指针，函数保留C名字。
extern "C" { // 与stdlib接口使用同一链接约定。
#endif // C源码直接声明。
struct airi_malloc_info airi_malloc_info(void); // 当前单线程快照，不分配、不清理也不改errno。
#ifdef __cplusplus // 结束链接范围。
} // 此观察接口不是线程同步工具。
#endif // 结束语言差异。
#endif // 结束包含保护。
```

<a id="malloc-request"></a>
## 第一次非零请求才向内核取得页面

新建 `labs/airios/libc/malloc.c`。全局 `objects`、`live_bytes` 和 `live_objects` 在用户ELF的BSS中，装载时为零。`airi_malloc_info()` 只读这些值，查询本身不会申请内存。下面先写到 `malloc` 的结尾，随后接上其余三个函数；当前先不构建。

零长度请求在本C11实现中返回NULL并保留errno，不表示内存不足。非零请求若超过arena能容纳的最大对象，则先设置ENOMEM并退出，大小相加尚未发生。其余首次请求经 `airi_alloc(262144)` 真正进入内核；页预算不足会返回NULL，静态描述仍为空，释放其他原始页面后可以再次尝试。只有页申请和块链初始化都完成，才能把对象交给调用方。

底层 `airi_heap_alloc` 的返回值是请求头位置。包装填写请求长度后更新活字节和对象数，再以 `header+1` 跨过一个完整结构，交出对象。普通malloc内容未指定；页最初清零也不能保证复用块中的旧内容为空。成功不清旧errno，判断失败仍应先看返回值。

<!-- write: labs/airios/libc/malloc.c -->
```c
#include <stdlib.h> // 对象接口与原始页分配分开。
#include <airi_malloc.h> // 只读观察页面缓存和活请求。
#include <airi_libc.h> // 页来源和用户字节函数，不链接内核memory.c。
#include <airi/heap.h> // 同一纯块链算法另编译进当前用户ELF。
#include <errno.h> // 失败使用本进程的TLS错误槽。
#define ARENA_BYTES (64u * 4096u) // 单次原始申请最多六十四页，本轮固定一个256KiB arena。
struct allocation { size_t requested, reserved; }; // 两个8字节字段，在块payload中保留16字节请求头。
_Static_assert(sizeof(struct allocation) == 16 && _Alignof(max_align_t) <= 16, "malloc alignment"); // 头后指针仍满足当前目标的基本类型对齐。
static struct airi_heap objects; // 零BSS表示尚未申请；与内核堆或其他进程的实例互不相干。
static size_t live_bytes, live_objects; // 统计请求大小，不把补齐、块头和空闲缓存混入活字节。
struct airi_malloc_info airi_malloc_info(void) { // 不暴露内部块链或页面句柄。
    return (struct airi_malloc_info){objects.bytes, live_bytes, live_objects}; // 单线程观察，不构成并发原子快照。
}
void *malloc(size_t bytes) { // 对象从当前进程的arena划分，不能把返回值交airi_release。
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
            airi_release(arena, ARENA_BYTES); // 不把尚未接入块链的页面留作隐形所有权。
            errno = ENOMEM; // 本轮不向调用者交出部分初始化对象。
            return NULL; // 正常内核页接口不会到达此分支。
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

<a id="malloc-release"></a>
## 归还对象与清零新对象

把下面两段接到同一文件。`free(NULL)` 什么也不做；非NULL参数必须是这个分配器当前仍借出的精确对象起点。包装从该指针前退一个请求头，先保存请求长度，再交给块链归还，因为相邻空块合并时旧元数据可能成为空闲内容。归还成功后再扣活请求计数，页面缓存保持。

这里不提供对任意指针的安全释放。内部块链能拒绝部分错误起点，但包装为读取请求长度已经访问了指针前的内存；重复释放、内部地址或过期指针均属于接口外的未定义行为。用户进程若访问未映射页，仍由已有用户异常路径处理，不能把这种检查当成内存安全库。

`calloc(count, bytes)` 先用除法检查乘法能否表示。例如 `SIZE_MAX*2` 若先相乘会回绕，分配出来的小块无法容纳真实请求。检查通过才计算总量、调用malloc，并清零整个请求范围。清零发生在每次成功calloc上，不能只依赖页第一次进入进程时的零填充。

<!-- append: labs/airios/libc/malloc.c -->
```c
void free(void *pointer) { // 调用者必须交回本库仍存活的精确对象起点，错误free属于未定义行为。
    if (!pointer) // NULL不代表一个块。
        return; // 保留errno和所有计数。
    struct allocation *header = (struct allocation *)pointer - 1; // 从有效对象回到请求头，不把任意指针当作安全输入。
    size_t bytes = header->requested; // 合并空块可能覆盖元数据，归还前先保存真实请求长度。
    if (airi_heap_free(&objects, header)) { // 纯算法只回收对象与合并相邻空块，不解除映射。
        live_bytes -= bytes; // 只扣除这个已归还对象的请求。
        --live_objects; // 页面缓存继续属于当前arena。
    }
}
void *calloc(size_t count, size_t bytes) { // 不能先乘后检查，否则回绕可能得到很小的错误对象。
    if (bytes && count > SIZE_MAX / bytes) { // 先除后比，零元素大小也不发生除零。
        errno = ENOMEM; // 真实请求无法表示，不能仅分配回绕后的前缀。
        return NULL; // arena、旧对象与计数保持。
    }
    size_t total = count * bytes; // 上面的条件证明本次乘法可表示。
    void *pointer = malloc(total); // 零总量沿用NULL且保留errno的策略。
    if (pointer) // 分配失败或零请求都不能解引用NULL。
        memset(pointer, 0, total); // 即使复用含旧内容的块，也清零完整请求。
    return pointer; // calloc只承诺字节全零，不等于任意类型的语义构造。
}
```

<a id="malloc-reallocate"></a>
## 扩容失败时旧对象继续属于调用者

33字节对象扩容到8193字节时，先申请8193字节的新对象，复制旧33字节，再归还旧块。申请失败会直接返回NULL，旧请求头、旧内容和旧指针仍有效。因此调用方要先将返回值存入临时变量，成功后再替换原指针；直接写 `p=realloc(p,n)` 会在失败时丢失仍需归还的旧指针。

当前实现缩小时保留原块，只把requested和活请求字节减小。例如8193缩为17以后，17字节之外已不属于调用者，底层较大容量暂时仍被该块占用。后续增长仍按新的真实请求长度复制。增长时同时持有新旧对象会提高瞬时占用，即使释放旧对象后总量放得下，也可能因没有新块而失败。

非NULL对象重分配为0时，本C11实现明确执行free并返回NULL，errno保持。不同C/POSIX版本对零长度重分配的规定不同，移植代码应明确调用free，不把这项选择当成跨版本保证。NULL指针则直接走malloc，包括零请求。将最后一个函数接入，完成文件。

<!-- append: labs/airios/libc/malloc.c -->
```c
void *realloc(void *pointer, size_t bytes) { // 正常扩容先取得新对象，失败不得释放旧对象。
    if (!pointer) // NULL没有旧生命周期。
        return malloc(bytes); // 包括本实现明确的零请求规则。
    if (!bytes) { // 本C11实现特定选择，不宣称所有标准版本都保证这一行为。
        free(pointer); // 旧对象生命周期结束。
        return NULL; // errno保持，不代表内存不足。
    }
    struct allocation *header = (struct allocation *)pointer - 1; // 非NULL必须来自仍存活的本库对象。
    size_t old_bytes = header->requested; // 只复制原请求，不读取对齐填充或别的对象。
    if (bytes <= old_bytes) { // 缩小或相同大小时保留同一块，暂不回收块内尾部。
        header->requested = bytes; // 后续扩容最多复制当前真实长度。
        live_bytes -= old_bytes - bytes; // 活字节缩小，底层借用容量暂时不变。
        return pointer; // 当前实现不搬动缩小对象。
    }
    void *replacement = malloc(bytes); // 暂时同时持有旧新对象，峰值容量可能导致失败。
    if (!replacement) // 旧块仍被借出，原指针及内容保持。
        return NULL; // 调用者应先保存返回值，再决定是否替换旧变量。
    memcpy(replacement, pointer, old_bytes); // 新旧块互不重叠，仅复制仍属旧对象的字节。
    free(pointer); // 新内容准备好后才结束旧生命周期。
    return replacement; // 成功后调用者只能继续使用新指针。
}
```

<a id="malloc-observation"></a>
## 先耗尽页面，再观察对象的复用与复制

新建 `malloc-probe.c`。同一ELF使用两个目录名：`heap` 完成全流程，孩子成功exec到 `heap-reset` 时只检查全新BSS、errno和首次calloc。程序最初的快照必须是0，连查询与零长度操作都不应提前申请arena。

首次真实失败由原始页接口制造：先每批申请64页，批量申请失败后继续逐页申请，直到1024页的当前进程预算耗尽。记录数组保存每批实际地址和长度，此时malloc(1)无法取得64页arena，应报告ENOMEM并保持空描述。按原长度全部release后，再次malloc可以成功。数据页预算还包含ELF、TLS和用户栈，因此不能硬编码一定成功申请多少批。

该循环中的 `step` 依次取262144、4096、0，0结束外层循环；`lengths[count++]` 先用旧下标记录成功批次，再增加有效条数。80项足以记录当前预算下的十五批64页和最多六十三个单页。数组只是用户栈上的所有权记录，不能代替内核页校验。

<!-- write: labs/airios/malloc-probe.c -->
```c
#include <stdlib.h> // 本ELF显式选择对象分配实现。
#include <airi_malloc.h> // 观察请求对象与页面缓存，不猜物理页状态。
#include <airi_libc.h> // 进程、原始页接口与串口输出。
#include <errno.h> // 用户库失败使用真实TLS错误槽。
static int info(size_t arena, size_t bytes, size_t count) { // 对照三种独立数量。
    struct airi_malloc_info value = airi_malloc_info(); // 查询不会首次申请arena。
    return value.arena_bytes == arena && value.live_bytes == bytes && value.live_objects == count; // 不把缓存误记为活对象。
}
int main(int argc, char **argv) { // 同一ELF另以heap-reset验证成功exec的全新BSS。
    const size_t arena = 64 * 4096; // 当前用户分配器的明确容量。
    if (!info(0, 0, 0) || errno) // 进入main前不能已经分配arena或继承旧错误槽。
        return 31; // exec重建失败不能由新分配掩盖。
    if (argc == 1 && strlen(argv[0]) == 10 && !memcmp(argv[0], "heap-reset", 10)) { // 孩子成功替换到同一ELF的新实例。
        unsigned char *value = calloc(97, 1); // 新arena应从真实页接口取得。
        if (!value || value[0] || value[96] || !info(arena, 97, 1)) // 新对象和计数都来自本次执行。
            return 32; // 不能沿用旧块链地址而只把计数清零。
        free(value); // 页面留作缓存，退出时由内核归还。
        if (!info(arena, 0, 0)) // 无活对象不等于未持有arena。
            return 33; // 不能把free假装成release。
        airi_puts("[MALLOC_EXEC_RESET_OK] arena=lazy objects=zero errno=zero\n"); // 真正exec后才发布。
        return 0; // 父等待这个实际身份。
    }
    errno = EBUSY; // 零操作都应保留旧错误。
    free(NULL); // 不得初始化arena或修改错误。
    if (malloc(0) || calloc(0, SIZE_MAX) || calloc(SIZE_MAX, 0) || realloc(NULL, 0) || errno != EBUSY || !info(0, 0, 0)) // 零请求采用本实现明确的NULL规则。
        return 34; // 不用NULL本身推断ENOMEM。
    if (calloc(SIZE_MAX, 2) || errno != ENOMEM || malloc(SIZE_MAX) || errno != ENOMEM || !info(0, 0, 0)) // 先拒绝无法表示的乘法和明显超限大小。
        return 35; // 不能为回绕后的小请求建立arena。
    void *pages[80]; size_t lengths[80], count = 0; // 保存每批原始页面的独立归还长度，最多十五大批和六十三单页。
    for (size_t step = arena; step; step = step == arena ? 4096 : 0) { // 先填64页批次，再填最后不足64页的预算。
        for (;;) { // 直到当前批次再也无法取得。
            void *page = airi_alloc(step); // 原始页接口没有调用对象分配器。
            if (!page) // 当前步长耗尽；大批失败后还应尝试单页。
                break; // 不把失败指针写进归还列表。
            if (count == 80) // 防止以后扩大预算时悄悄覆盖用户栈。
                return 36; // 当前固定1024页预算不应到达。
            pages[count] = page; lengths[count++] = step; // 保存实际成功的整批所有权。
        }
    }
    if (malloc(1) || errno != ENOMEM || !info(0, 0, 0)) // 没有数据页时首次arena申请真实失败，描述仍为空。
        return 37; // 不用纯大小检查代替真实内核失败。
    while (count) { // 完整归还自己持有的原始页。
        --count; // 从最后一个有效表项开始。
        if (airi_release(pages[count], lengths[count])) // 每段保持原始页对齐与长度。
            return 38; // 失败不能继续伪装已经恢复预算。
    }
```

页面重新可用后，继续同一main。1至65字节的申请覆盖各种16字节补齐余数；每次写0xa5再free，之后calloc必须把复用范围真正清零。两个70000字节对象归还以后，再申请180000字节；任一旧块都不够，只有相邻空块和尾部合并才会成功。

随后保留33字节对象与97字节邻居，扩容前者、缩小，再检查邻居内容。`grown` 保存扩容结果，成功以后不再访问旧指针；新尾部没有初始化保证，内容比较只覆盖原来的33字节。追加到main当前开口处。

<!-- append: labs/airios/malloc-probe.c -->
```c
    for (size_t bytes = 1; bytes <= 65; ++bytes) { // 覆盖不同补齐余数，并重复复用同一arena。
        unsigned char *value = malloc(bytes); // 内核失败恢复后此处必须可以重试。
        if (!value || (uintptr_t)value % 16 || !info(arena, bytes, 1)) // 当前目标基本对齐至少16字节。
            return 39; // 不把请求头大小遗漏在对齐中。
        memset(value, 0xa5, bytes); free(value); // 下一次calloc必须清除这类复用内容。
    }
    unsigned char *zero = calloc(65, 1); // 首次适配应复用之前写过的空块。
    if (!zero) // 后续字节检查只能针对成功对象。
        return 40; // 不能假定calloc必然成功。
    for (unsigned i = 0; i < 65; ++i) // 检查完整请求，而非只看第一页或首字节。
        if (zero[i]) // 复用内容也须为零。
            return 41; // 页最初清零不能代替每次calloc清零。
    free(zero); // 恢复无活对象状态。
    void *left = malloc(70000), *right = malloc(70000); // 两个相邻活块后仍有空闲尾块。
    if (!left || !right) // 先确认真实分裂出两块。
        return 42; // 不拿NULL参与复用判断。
    free(left); free(right); // 连续空块及它们的内部块头应合并。
    void *merged = malloc(180000); // 单一旧块均不够，合并后才有足够连续空间。
    if (!merged || !info(arena, 180000, 1)) // 不用空闲容量之和冒充连续可分配范围。
        return 43; // 相邻空块合并必须实际发生。
    free(merged); // 为正常realloc恢复空间。
    unsigned char *old = malloc(33), *guard = malloc(97); // 同时活着的邻居用于观察复制范围。
    if (!old || !guard) // 不在失败指针上填充。
        return 44; // 留存具体故障位置。
    for (unsigned i = 0; i < 33; ++i) // 每个旧字节可辨认。
        old[i] = (unsigned char)(i ^ 0x59); // 扩容必须保留完整前缀。
    memset(guard, 0x6d, 97); // 另一对象不能被复制或free影响。
    uintptr_t old_address = (uintptr_t)old; // 先记录地址数字，成功搬运后不再读取已结束生命周期的旧指针。
    unsigned char *grown = realloc(old, 8193); // 先拿临时返回值；失败时old仍有效。
    if (!grown || (uintptr_t)grown == old_address || !info(arena, 8193 + 97, 2)) // 本实现增长总是搬运到新块。
        return 45; // 对照请求字节，不能按对齐容量统计。
    for (unsigned i = 0; i < 33; ++i) // 只比较旧请求范围，新尾部没有初始化保证。
        if (grown[i] != (unsigned char)(i ^ 0x59)) // 顺序与内容必须保留。
            return 46; // 不读取已结束生命周期的old。
    if (realloc(grown, 17) != grown || !info(arena, 17 + 97, 2)) // 缩小不搬运，但改变请求计数。
        return 47; // 底层容量暂不缩小，不得在此读旧尾部。
    for (unsigned i = 0; i < 97; ++i) // 邻居仍存活。
        if (guard[i] != 0x6d) // 扩容/缩小都不得污染它。
            return 48; // 不只检查分配器自身计数。
    errno = EBUSY; // free与零长度realloc不清旧错误。
    if (realloc(grown, 0) || errno != EBUSY) // 本实现明确结束旧对象并返回NULL。
        return 49; // 不把此NULL写成内存不足。
    free(guard); // 恢复整个arena可复用。
```

接着让180000字节对象与60000字节对象同时存活。扩容前者到190000时，arena里没有空间同时放下新块，应失败；逐字节核对旧180000字节，并核对两个对象仍计为240000个活请求字节。归还后再连续128轮分配250000字节并free，观察合并没有逐轮遗失块头容量。继续追加。

<!-- append: labs/airios/malloc-probe.c -->
```c
    unsigned char *large = malloc(180000); // 稍后扩容需要同时持有新旧大块。
    void *blocker = malloc(60000); // 剩余空间不足以先放下190000字节的新对象。
    if (!large || !blocker) // 两项实际成功后才开始失败观察。
        return 50; // 不用超大无效请求替代容量耗尽。
    memset(large, 0x37, 180000); // 整个旧范围都须在失败后可访问。
    if (realloc(large, 190000) || errno != ENOMEM || !info(arena, 240000, 2)) // 峰值容量真实不足，旧对象不能被释放或缩短。
        return 51; // 调用者仍持有large。
    for (size_t i = 0; i < 180000; ++i) // 检查完整旧对象。
        if (large[i] != 0x37) // 失败不能破坏旧内容。
            return 52; // 此后仍能正常free。
    free(blocker); free(large); // 相邻空块重新合并。
    for (unsigned round = 0; round < 128; ++round) { // 反复使用全部大空间，不依赖进程退出恢复。
        void *value = malloc(250000); // 在同一arena内接近上限，但仍容得下元数据。
        if (!value) // 历史块头不能逐轮吃掉容量。
            return 53; // 检查反复释放与合并。
        free(value); // 每轮结束活字节均回到零。
    }
```

<a id="malloc-fork-exec"></a>
## fork 复制块链，exec 重新开始

最后保留一个97字节对象再fork。既有fork复制全部用户页，包括BSS堆描述、arena块头和对象内容，父子中的指针数值相同，却属于不同地址空间中的物理副本。孩子修改首字节并尝试坏exec，失败后仍能访问和free自己的对象。随后成功exec到heap-reset，旧arena页由内核回收，新BSS将描述符重新置零。

父等待孩子退出后，再检查自己的97字节内容与活请求计数。孩子的free、exec和退出都不能改变父对象。父最终free以后，快照应为arena=262144、live_bytes=0、live_objects=0；等父退出，内核才归还这一份缓存页。将剩余代码接入并闭合main。

<!-- append: labs/airios/malloc-probe.c -->
```c
    unsigned char *inherited = malloc(97); // fork前保留一个真实活对象与块链。
    if (!inherited) // 后续只操作成功借出的对象。
        return 54; // 不在NULL上写入。
    memset(inherited, 0x61, 97); // 同虚拟地址在父子中应指向各自物理页。
    long child = airi_fork(); // 当前直接复制用户BSS、arena页和块头，指针数值保持。
    if (child < 0 || !info(arena, 97, 1)) // 父子均应继承同样的请求状态。
        return 55; // fork不是从零模板建立分配器。
    if (!child) { // 孩子只改变自己的对象及分配器元数据。
        inherited[0] = 0x72; // 父的相同VA内容不得变化。
        if (airi_exec("broken") != -8 || inherited[0] != 0x72 || !info(arena, 97, 1)) // 失败exec完整保留原arena与块链。
            return 56; // 不能只保存返回码而丢失页所有权。
        free(inherited); // 子归还同VA对象不影响父的借用。
        if (!info(arena, 0, 0)) // 缓存仍在旧孩子映像内。
            return 57; // 成功exec应由内核回收它。
        airi_exec("heap-reset"); // 同一PID进入新BSS与TLS，旧arena映射结束。
        return 58; // 成功不会回到旧调用点。
    }
    int status = 1; // 父实际等待孩子的新映像退出。
    if (airi_waitpid(child, &status) != child || status || !info(arena, 97, 1)) // 子free/exec/exit不能改变父对象计数。
        return 59; // wait的真实结果必须成功。
    for (unsigned i = 0; i < 97; ++i) // 子写过的首字节以及所有其他内容仍由父持有。
        if (inherited[i] != 0x61) // 父子物理页保持隔离。
            return 60; // 不把指针数值相同误认为共享对象。
    free(inherited); // 父最后一个对象归还。
    if (!info(arena, 0, 0)) // 活对象清零，arena等待进程退出由内核回收。
        return 61; // 串口成功标记不应掩盖剩余借用。
    airi_puts("[MALLOC_USER_OK] arena=262144 align=16 calloc=zero overflow=checked realloc=failure_preserved reuse=128 fork=private exec=reset\n"); // 所有正常与失败路径实际完成。
    return 0; // shell继续运行，整会话收尾另核对用户页和内核资源。
}
```

<a id="malloc-build"></a>
## 把纯算法编译进用户 ELF，再接入真实目录

Makefile末尾新增三个对象/映像规则。`heap-algorithm.o` 的输入仍是已经写过的 `kernel/mm/heap.c`，但使用USER_FLAGS和用户目标头，输出位于user目录。编译器可能产生的字节复制由用户bytes.o解析，不需要内核base.c或宿主libc。`malloc.o` 只包含本节包装。该ELF还选择posix.o，以便使用真实TLS errno；USER_COMMON不变，早期无TLS与原始FS探针保持原有前提。

链接输入的 `$(filter %.o,$^)` 从当前规则所有前置中保留.o结尾的项，排除链接脚本；`$^` 已含完整先决目标列表，不需要再手抄两份对象顺序。下面将这三组规则追加到已有 Makefile，其他规则保持。

在 `Makefile` 末尾追加以下内容，前章已有部分保留。

<!-- append: Makefile -->
```make

# 纯块链算法再次以用户选项编译，内核heap.o不进入用户ELF。
$(OUT)/user/heap-algorithm.o: kernel/mm/heap.c Makefile
# 独立输出路径隔离两种编译策略。
	@mkdir -p $(dir $@)
# 算法只需要目标类型，隐式字节复制由用户bytes.o提供。
	$(CC) $(USER_FLAGS) -Ilabs/airios/include -c $< -o $@
# 对象包装单独选择errno与原始页来源，不改变早期无TLS程序。
$(OUT)/user/malloc.o: labs/airios/libc/malloc.c Makefile
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
# 本配置产物路径仅作为.incbin输入。
$(OUT)/labs/airios/images.o: CPPFLAGS += -DAIRI_MALLOC_ELF='"$(OUT)/user/malloc-probe.elf"'
```

images.S增加一段只读incbin，把本配置的独立用户ELF包进内核初始目录；不把用户malloc函数链接成内核服务。user.c只新增映像范围声明、heap/heap-reset两个目录项，目录查询继续使用原有30号调用。进程创建、内存页申请、fork、exec和退出实现保持。

新增程序名后的完整目录共237字节，含末尾NUL，仍容得下既有256字节缓冲区。用户shell保持原样，目录仍完整返回；小容量ENOSPC与坏输出地址EFAULT的既有路径保持。

在 `labs/airios/images.S` 末尾追加以下内容，前章已有部分保留。

<!-- append: labs/airios/images.S -->
```asm

.balign 8 /* 对象分配器只进入选择它的用户ELF。 */
.global airi_malloc_elf_start, airi_malloc_elf_end /* 两个目录名共用这一完整映像。 */
airi_malloc_elf_start: /* 用户页与对象借用的真实观察程序。 */
.incbin AIRI_MALLOC_ELF /* 由本配置静态链接，不借宿主malloc。 */
airi_malloc_elf_end: /* 输入范围的排他右端。 */
```

接下来按现有 `kernel/process/user.c` 中的位置逐组替换，未列出的前章内容保持。所有映像、用户库与构建接入完成后再运行。

在已有用户映像声明区加入 malloc 观察 ELF 的边界。

本章每组替换的第一块只用于定位旧文本，敲入第二块替换它，不把两块同时加入文件。各组按正文顺序完成。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
extern const uint8_t airi_hello_elf_start[], airi_hello_elf_end[]; // 既有独立用户C程序。
extern const uint8_t airi_process_elf_start[], airi_process_elf_end[]; // 新的fork/wait和抢占工作负载。
extern const uint8_t airi_vector_elf_start[], airi_vector_elf_end[]; // 显式偏移向量正常与坏范围观察。
extern const uint8_t airi_posix_elf_start[], airi_posix_elf_end[]; // 显式链接errno与POSIX风格文件包装。
extern const uint8_t airi_ctls_elf_start[], airi_ctls_elf_end[]; // 编译器_Thread_local与静态模板。
extern const uint8_t airi_ctls_char_elf_start[], airi_ctls_char_elf_end[]; // 单字节小对齐模板。
```
<!-- after -->
```c
extern const uint8_t airi_hello_elf_start[], airi_hello_elf_end[]; // 既有独立用户C程序。
extern const uint8_t airi_process_elf_start[], airi_process_elf_end[]; // 新的fork/wait和抢占工作负载。
extern const uint8_t airi_vector_elf_start[], airi_vector_elf_end[]; // 显式偏移向量正常与坏范围观察。
extern const uint8_t airi_malloc_elf_start[], airi_malloc_elf_end[]; // 用户对象分配与页面缓存的独立ELF。
extern const uint8_t airi_posix_elf_start[], airi_posix_elf_end[]; // 显式链接errno与POSIX风格文件包装。
extern const uint8_t airi_ctls_elf_start[], airi_ctls_elf_end[]; // 编译器_Thread_local与静态模板。
extern const uint8_t airi_ctls_char_elf_start[], airi_ctls_char_elf_end[]; // 单字节小对齐模板。
```

将 heap 与 heap-reset 接入目录。它们使用相同文件，成功 exec 后仍从新 BSS 和入口开始。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    {"sh", airi_shell_elf_start, airi_shell_elf_end}, // 独占串口输入的可选用户程序。
    {"init", airi_init_elf_start, airi_init_elf_end}, // 管理前台shell的常驻用户进程。
    {"vector", airi_vector_elf_start, airi_vector_elf_end}, // 聚合、分散、短前缀与错误原子性。
    {"libc", airi_posix_elf_start, airi_posix_elf_end}, // 用户库错误与文件结果转换。
    {"libc-reset", airi_posix_elf_start, airi_posix_elf_end}, // 同一ELF新错误槽和CLOEXEC观察。
    {"ctls", airi_ctls_elf_start, airi_ctls_elf_end}, // 自动建立静态TLS后运行普通C。
```
<!-- after -->
```c
    {"sh", airi_shell_elf_start, airi_shell_elf_end}, // 独占串口输入的可选用户程序。
    {"init", airi_init_elf_start, airi_init_elf_end}, // 管理前台shell的常驻用户进程。
    {"vector", airi_vector_elf_start, airi_vector_elf_end}, // 聚合、分散、短前缀与错误原子性。
    {"heap", airi_malloc_elf_start, airi_malloc_elf_end}, // 单线程用户对象与错误路径。
    {"heap-reset", airi_malloc_elf_start, airi_malloc_elf_end}, // 同一ELF新BSS和按需arena。
    {"libc", airi_posix_elf_start, airi_posix_elf_end}, // 用户库错误与文件结果转换。
    {"libc-reset", airi_posix_elf_start, airi_posix_elf_end}, // 同一ELF新错误槽和CLOEXEC观察。
    {"ctls", airi_ctls_elf_start, airi_ctls_elf_end}, // 自动建立静态TLS后运行普通C。
```

<a id="malloc-run"></a>
## 在持续运行的用户 shell 中观察

全部编辑闭合后构建。启动后在内核提示符输入run sh，进入用户提示符再输入heap；等待命令结束后可以再次heap，两次均从新ELF状态开始，shell自身不退出。最后exit返回内核监控台。下面自动运行块只负责构建，交互输入按后面的逐行文本完成。

<!-- run -->
```sh
./airios build  # 构建独立用户ELF和当前串口镜像，不读取图形源码。
```

```sh
./airios run  # 打开当前固定q35/TCG/OVMF环境，接收真实UART输入。
```

```text
run sh
heap
heap
exit
```

每次heap应出现以下完整记录，PID和页分配日志随会话变化。第二条来自父，第一条来自真正成功exec的孩子；不能只观察QEMU仍在运行。

```text
[MALLOC_EXEC_RESET_OK] arena=lazy objects=zero errno=zero
[MALLOC_USER_OK] arena=262144 align=16 calloc=zero overflow=checked realloc=failure_preserved reuse=128 fork=private exec=reset
[SHELL_COMMAND_DONE] name=heap code=0
```

故障都包含在这条操作中：零请求无对象、乘法回绕被拒绝、原始页预算不足、增长时瞬时容量不足，以及坏ELF保持旧堆。每项失败后继续使用或归还原资源，不需要人为破坏块头。非法free没有安全合同，本节不把它放进可恢复错误演示。

shell_test在vector完成以后发送heap，并按本次命令输出核对父子标记和退出码。该入口已有单步超时与finally回收QEMU，失败不能落成成功结果。下面在既有命令序列接入 heap，随后运行既有完整验证。

在 vector 完成以后发送 heap，从本次命令与其成功 exec 的孩子取得两条分配器完成记录。

<!-- replace: scripts/shell_test.py -->
<!-- before -->
```python
                assert '[LIBC_ERRNO_OK] initial=0 fork=copied preempt=private success=preserved exec=reset' in libc_output and '[LIBC_EXEC_RESET_OK] errno=zero cloexec=closed' in libc_output # 同一身份成功exec重置错误并关闭表项。
                offset=len(text); wire.sendall(b'vector\n'); text=wait('[SHELL_COMMAND_DONE] name=vector code=0',offset) # 散布缓冲必须由真实内核边界校验后完成一次文件操作。
                assert '[VECTOR_USER_OK] gather=ordered scatter=short prefix=256 positioned=1 invalid=unchanged descriptors=checked' in text[offset:] # 坏后项、跨页描述和实际前缀均由guest检查。
                offset=len(text); wire.sendall(b'orphans\n'); text=wait('[SHELL_COMMAND_DONE] name=orphans code=0',offset) # 同一用户shell持续服务，不靠返回monitor收拢孙进程。
                orphan_output=text[offset:] # 只核对本条命令的身份记录。
                assert '[ORPHAN_USER_OK] rounds=24 live=12 zombie=12 session=alive' in orphan_output # 超过八槽容量仍能连续fork。
```
<!-- after -->
```python
                assert '[LIBC_ERRNO_OK] initial=0 fork=copied preempt=private success=preserved exec=reset' in libc_output and '[LIBC_EXEC_RESET_OK] errno=zero cloexec=closed' in libc_output # 同一身份成功exec重置错误并关闭表项。
                offset=len(text); wire.sendall(b'vector\n'); text=wait('[SHELL_COMMAND_DONE] name=vector code=0',offset) # 散布缓冲必须由真实内核边界校验后完成一次文件操作。
                assert '[VECTOR_USER_OK] gather=ordered scatter=short prefix=256 positioned=1 invalid=unchanged descriptors=checked' in text[offset:] # 坏后项、跨页描述和实际前缀均由guest检查。
                offset=len(text); wire.sendall(b'heap\n'); text=wait('[SHELL_COMMAND_DONE] name=heap code=0',offset) # 对象库运行在真实用户地址空间，先耗尽原始页再验证重试。
                heap_output=text[offset:] # 只接受本条命令与它实际exec的孩子记录。
                assert '[MALLOC_USER_OK] arena=262144 align=16 calloc=zero overflow=checked realloc=failure_preserved reuse=128 fork=private exec=reset' in heap_output # 对齐、复用、失败保留与父子隔离均由guest观察。
                assert '[MALLOC_EXEC_RESET_OK] arena=lazy objects=zero errno=zero' in heap_output # 成功exec必须重新建立BSS和TLS，不能继承旧arena。
                offset=len(text); wire.sendall(b'orphans\n'); text=wait('[SHELL_COMMAND_DONE] name=orphans code=0',offset) # 同一用户shell持续服务，不靠返回monitor收拢孙进程。
                orphan_output=text[offset:] # 只核对本条命令的身份记录。
                assert '[ORPHAN_USER_OK] rounds=24 live=12 zombie=12 session=alive' in orphan_output # 超过八槽容量仍能连续fork。
```

<!-- run -->
```sh
./airios test  # 运行主机算法、启动故障、真实用户shell、init监督和设备存储检查。
```

本轮实际用户heap结束为0；成功exec记录显示旧孩子85页替换为新ELF的21页，随后新映像按需再申请64页arena。该数量只属于当前编译结果，判断正确性依据是生命周期与回收，不是固定页数。静态ELF无未解析符号；对象库不借宿主malloc。完整自动运行结果保存在项目验收记录，个人Linux环境没有因这些guest结果而变成已验证。

<a id="malloc-linux"></a>
## 接回 Linux 的对象、映射与内存占用

同一个请求在C++网络服务里常表现为容器扩容或请求对象分配。对象归还以后，分配器可能保留缓存，因此进程RSS未下降不等于活对象未释放；反过来，活对象泄漏也不能用缓存策略解释。AiriOS的三项快照先把这两种情况分开，但没有提供Linux分配器的线程缓存、arena策略或性能保证。

Linux必修对照继续使用既有L02的页映射、缺页与RSS实验。源码接口合同见 [Open Group malloc](https://pubs.opengroup.org/onlinepubs/9799919799/functions/malloc.html) 和 [realloc](https://pubs.opengroup.org/onlinepubs/009696899/functions/realloc.html)。malloc返回对齐空间，非零realloc失败应保留旧对象。零长度选择和有界容量以本实现说明为准，四个名字不代表完整C/POSIX兼容。

当前分配器仍是每进程单线程状态。进程内增加线程之前，必须决定共享arena的互斥、每线程errno与TLS归属、退出清理和fork/exec范围。这里没有用关用户中断或静默成功的pthread接口遮住这些工作。
