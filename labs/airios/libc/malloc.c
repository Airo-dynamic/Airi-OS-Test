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
