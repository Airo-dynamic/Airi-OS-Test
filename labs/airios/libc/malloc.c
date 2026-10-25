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
