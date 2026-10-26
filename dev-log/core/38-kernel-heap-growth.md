# K38 · 给内核对象堆接入新页面，并区分缓存与活对象

从 K37 继续。用户已有 4 MiB 数据容量，但内核对象仍只能从 K17 的 64 KiB 固定范围中取得。较大的服务缓冲区会让 `airi_kmalloc` 失败。本节保留旧对象的地址与内容，空间不足时向页池申请新页面，映射在堆尾，再把新范围交给对象分配器。

初始和每批新增范围均为 64 KiB，最多十六批，即 1 MiB。对象 free 后空间回到堆，映射页面保留作为缓存。启动演示会实际增长到上限，所以本节启动后的 mem 显示 1 MiB；扩展机制本身只在申请找不到足够空闲块时增加页面。单核、内核不可抢占与 IRQ 不分配的约定保持，不引入并发分配器、自动收缩、slab 或完整 malloc ABI。

<a id="heap-extend-chain"></a>
## 尾部空闲块可以延长，活对象不能被扩容改变

K17 已将堆分成两层：memory.c 持有物理页和内核映射，heap.c 只在已映射范围中维护块头与 payload。现在仍由 memory.c 建立新页面，新增 `airi_heap_extend(heap, tail, bytes)` 只负责接入这些字节。调用者必须保证新范围可写且在整个借用期有效。

例如旧 backing 覆盖起点后的 65536 字节，tail 必须精确等于起点加 65536。若有地址洞，对象分配器把两侧误当连续后可能在洞里写块头；若 tail 落入旧范围，则会覆盖活对象。因此 extend 先检查旧右端、tail 相等、长度与16字节对齐和新右端溢出，全部成立后才读取并修改末块。

末块的两种状态产生不同结果：

- 末块空闲：直接增加它的 payload 容量，原来的块头继续有效，新页面不再占一个额外块头。
- 末块仍被借用：在新范围首部完整写好一个32字节空闲块头，再让旧末块指向它。旧对象的容量与返回地址保持，不能让调用者意外拥有新页面。

下面更新 `include/airi/heap.h`。新增的 used 查询只统计仍借出的 payload 容量，包含对齐后实际占有的字节，不含空闲空间与块头。它用来区分对象借用与已经映射但尚无对象使用的缓存。

<!-- write: include/airi/heap.h -->
```c
#ifndef AIRI_HEAP_H // 字节级分配接口只展开一次。
#define AIRI_HEAP_H // 包含保护。
#include <airi/base.h> // size_t、NULL 与 SIZE_MAX。
#include <stdbool.h> // 分配和归还操作的明确成功状态。
struct airi_heap_block; // 块元数据布局只由 heap.c 维护。
struct airi_heap { // 调用者持有 backing 范围和分配状态。
    uint8_t *base; // 可读写且至少 16 字节对齐的虚拟起点。
    size_t bytes; // backing 的总字节数，包含内部元数据。
    struct airi_heap_block *first; // 第一个块，后续块按地址顺序链接。
};
bool airi_heap_init(struct airi_heap *heap, void *base, size_t bytes); // 不申请页，只在调用者已经拥有的连续范围建堆。
bool airi_heap_extend(struct airi_heap *heap, void *tail, size_t bytes); // 调用者已映射紧邻尾部的新范围，失败不改变旧块链。
size_t airi_heap_used_bytes(const struct airi_heap *heap); // 只统计仍借出的payload容量，不含空闲缓存与块头。
void *airi_heap_alloc(struct airi_heap *heap, size_t bytes); // 返回 16 字节对齐的未初始化小对象，失败或零长度返回 NULL。
bool airi_heap_free(struct airi_heap *heap, void *pointer); // NULL 作为空操作，其他指针必须是当前借出的对象起点。
size_t airi_heap_free_bytes(const struct airi_heap *heap); // 空闲 payload 字节和，不含块头，不保证可以一次分配这么大。
#endif // 结束包含保护。
```

更新 `kernel/mm/heap.c`。在 init 与 alloc 之间加入 extend 和 used 查询，其余分割、首次适配、精确起点归还与合并逻辑保持 K17。所有扩展检查失败都不修改旧链，也不会访问传入的新范围；成功后，新范围已经属于块链，调用者不能再单独解除它的映射。

<!-- write: kernel/mm/heap.c -->
```c
#include <airi/heap.h> // 独立于页表和设备的字节分配器。
struct airi_heap_block { // 块头紧靠 payload 之前，整个块按地址顺序排列。
    size_t bytes; // payload 容量，包含为 16 字节对齐增加的空间。
    struct airi_heap_block *next; // 紧邻的下一个块，NULL 表示 backing 末尾。
    bool free; // true 表示当前没有对象借用这块 payload。
} __attribute__((aligned(16))); // 块头大小也必须是 16 的倍数，才能保持后续对象对齐。
_Static_assert(sizeof(struct airi_heap_block) == 32, "heap header layout"); // 本项目固定 64 位 C ABI。
bool airi_heap_init(struct airi_heap *heap, void *base, size_t bytes) { // 调用者保证整个 backing 持续可读写。
    if (!heap || !base || (uintptr_t)base % 16 || bytes < sizeof(struct airi_heap_block) + 16 || bytes > UINTPTR_MAX - (uintptr_t)base) // 拒绝未对齐、过小和右端回绕。
        return false; // 不构造不完整的初始块。
    bytes &= ~(size_t)15; // 尾部不足 16 字节的空间不参与对象划分。
    heap->base = base; // 保存当前 backing 虚拟起点。
    heap->bytes = bytes; // 保存包含块头的总容量。
    heap->first = base; // 第一个块头就在 backing 起点。
    *heap->first = (struct airi_heap_block){bytes - sizeof(*heap->first), NULL, true}; // 初始只有一个完整空闲块。
    return true; // 仍未向任何对象借出空间。
}
bool airi_heap_extend(struct airi_heap *heap, void *tail, size_t bytes) { // 只接入已经拥有且连续的新backing，不执行映射。
    if (!heap || !heap->first || !tail || bytes < sizeof(struct airi_heap_block) + 16 || bytes % 16 || heap->bytes > UINTPTR_MAX - (uintptr_t)heap->base) // 先核对对象与旧右端，再读取块链。
        return false; // 失败不修改已存在的对象或块头。
    uintptr_t end = (uintptr_t)heap->base + heap->bytes; // 上面的减法检查保证旧右端不回绕。
    if ((uintptr_t)tail != end || bytes > UINTPTR_MAX - end) // 新范围必须精确紧邻，不能有洞、覆盖旧对象或右端溢出。
        return false; // 此时尚未触碰调用者的新范围。
    struct airi_heap_block *last = heap->first; // 找到唯一一个没有后继的块。
    while (last->next) // 只沿已有受管理块，不读取新页面上的旧字节。
        last = last->next; // 链按地址连续排列，末块到达旧右端。
    if (last->free) // 空末块可直接延长，避免再付一个块头。
        last->bytes += bytes; // 原payload地址与所有活对象位置均不改变。
    else { // 旧末块仍被对象借用，不能把新增容量偷偷并入该对象。
        struct airi_heap_block *added = tail; // 新块头恰在旧backing右端。
        *added = (struct airi_heap_block){bytes - sizeof(*added), NULL, true}; // 完整初始化后才接入旧链。
        last->next = added; // 发布新的空闲末块，旧对象容量保持。
    }
    heap->bytes += bytes; // 块链准备好以后才发布完整backing容量。
    return true; // 新范围此后归堆管理，调用者不能单独解除映射。
}
size_t airi_heap_used_bytes(const struct airi_heap *heap) { // 资源核对只观察活payload，不把空闲缓存算成对象泄漏。
    size_t bytes = 0; // 空堆与无活对象都为零。
    if (heap) // 尚未初始化的查询同样安全。
        for (const struct airi_heap_block *block = heap->first; block; block = block->next) // 不改变块状态。
            if (!block->free) // 已归还的payload不再有借用者。
                bytes += block->bytes; // 包含对象实际取得的对齐容量，不是原请求长度。
    return bytes; // 元数据块头与物理backing另有所有者。
}
void *airi_heap_alloc(struct airi_heap *heap, size_t bytes) { // 首次适配按地址顺序寻找能放下对象的空闲块。
    if (!heap || !heap->first || !bytes || bytes > SIZE_MAX - 15) // 拒绝零长度和对齐加法溢出。
        return NULL; // 失败不改变块链。
    size_t needed = (bytes + 15) & ~(size_t)15; // 向上补齐到 16 字节。
    for (struct airi_heap_block *b = heap->first; b; b = b->next) { // 遍历仍属于这个 backing 的块。
        if (!b->free || b->bytes < needed) // 已借出或容量不足的块不能使用。
            continue; // 检查后面的块。
        if (b->bytes - needed >= sizeof(*b) + 16) { // 剩余空间必须能容纳新块头和最小 payload。
            struct airi_heap_block *tail = (void *)((uint8_t *)(b + 1) + needed); // 新块头位于本次对象容量之后。
            *tail = (struct airi_heap_block){b->bytes - needed - sizeof(*tail), b->next, true}; // 尾块接回原来的后继。
            b->bytes = needed; // 当前块缩到本次实际占用容量。
            b->next = tail; // 再把新尾块接入链，避免丢失原后继。
        }
        b->free = false; // 在交出 payload 指针前记录已借出。
        return b + 1; // C 的结构指针加一恰好跨过一个完整块头。
    }
    return NULL; // 碎片或容量耗尽，不越过 backing 尝试写入。
}
bool airi_heap_free(struct airi_heap *heap, void *pointer) { // 只比较已知块的 payload，不解引用传入的任意指针。
    if (!pointer) // 空借用没有对象需要归还。
        return true; // free(NULL) 作为明确空操作。
    if (!heap || !heap->first) // 没有初始化过的堆不接受对象。
        return false; // 不访问未知块链。
    struct airi_heap_block *found = NULL; // 尚未找到匹配的对象起点。
    for (struct airi_heap_block *b = heap->first; b; b = b->next) // 沿受堆管理的真实块查找。
        if ((void *)(b + 1) == pointer) // 只接受精确 payload 起点，不接受内部地址。
            found = b; // 找到对应的当前块元数据。
    if (!found || found->free) // 未找到或已经归还，不能再次改变容量。
        return false; // 过期块头被合并后也不会作为有效对象找到。
    found->free = true; // 对象从已借出变为空闲，调用者从此不能再访问。
    for (struct airi_heap_block *b = heap->first; b && b->next;) { // 按地址顺序合并相邻空闲块。
        if (b->free && b->next->free) { // 两个相邻 payload 都没有所有者。
            b->bytes += sizeof(*b) + b->next->bytes; // 原后继块头也回到可用 payload。
            b->next = b->next->next; // 跳过已被合并的后继，继续检查新的邻居。
        } else // 中间还有借出的对象时不能跨越它合并。
            b = b->next; // 向后检查下一对块。
    }
    return true; // 只归还对象空间，不触碰 backing 页所有权。
}
size_t airi_heap_free_bytes(const struct airi_heap *heap) { // 统计可供小对象使用的 payload。
    size_t bytes = 0; // 空堆或没有空闲块时为零。
    if (heap) // 允许消费者查询尚未初始化的空状态。
        for (const struct airi_heap_block *b = heap->first; b; b = b->next) // 不修改分配状态。
            if (b->free) // 已借出块不能计入空闲。
                bytes += b->bytes; // 和可能分散在多个块中，不代表最大连续块。
    return bytes; // 不包含任何仍存在的块头。
}
```

<a id="heap-cache-usage"></a>
## 页面从页池转入堆，并不表示多了一个活对象

旧的会话检查要求前后 `pages_free` 和 `heap_free` 都完全相等。现在一次临时大对象可能让堆从64 KiB增长到128 KiB：即使对象已经free，页池仍少十六页，而堆的空闲payload增加。这符合保留缓存的策略，却会被旧检查误报为泄漏。

快照新增 heap_used，单独保存活payload。新的 `airi_memory_same_usage` 在当前固定布局下同时核对两项：

1. `pages_free + heap_bytes / 4096` 保持，表示堆增长所占的数据页恰好从页池转入；其他地方丢失的页不能被忽略。
2. heap_used 保持，表示会话没有留下额外活对象；堆变大不能抵消对象未归还。

这两项比较使用同一次会话的前后快照。

当前堆基址按2 MiB对齐，最多1 MiB仍落在第一批已建立的同一张叶子页表内，所以扩展只增加数据页，不增加页表页。以后若跨到别的叶子表，页表自身也会占物理页，需要另行计入。这份核对只比较净资源数量，仍不能辨认某个对象引用是否指向正确身份。

更新 `include/airi/memory.h`。`static inline` 让各使用者取得同一小函数的定义，不需要额外链接一个实现符号；这里的inline是编译提示，函数语义仍是普通C调用。

<!-- write: include/airi/memory.h -->
```c
#ifndef AIRI_MEMORY_H // 通用内存服务与只读数据快照。
#define AIRI_MEMORY_H // 包含保护。
#include <airi/boot.h> // 启动合同和固定宽整数。
#include <stddef.h> // 小对象请求按字节计数。
struct airi_memory_stats { // 消费者只得到数值，不持有分配器内部块指针。
    uint32_t pages_total, pages_free; // 实际受管理物理页与当前未借出页数。
    size_t heap_bytes, heap_free, heap_used; // 已映射backing、空闲payload与活对象payload分别统计。
};
void airi_memory_init(const AiriBootInfo *info); // 顺序初始化页池、页表和可扩展内核堆。
void airi_memory_snapshot(struct airi_memory_stats *stats); // 同步复制当前数值到调用者对象。
static inline bool airi_memory_same_usage(const struct airi_memory_stats *before, const struct airi_memory_stats *after) { // 每个包含者使用这段小函数，无独立链接符号。
    uint64_t old_available = before->pages_free + before->heap_bytes / 4096; // 堆的缓存数据页从页池转出，但没有外部借用。
    uint64_t new_available = after->pages_free + after->heap_bytes / 4096; // 当前1MiB堆始终使用初始已建的同一张叶子表。
    return old_available == new_available && before->heap_used == after->heap_used; // 新增活对象或堆以外的页泄漏都不能被缓存抵消。
}
void *airi_kmalloc(size_t bytes); // 对象空间不足时按64KiB批次扩展，最大1MiB，不承诺完整libc malloc ABI。
bool airi_kfree(void *pointer); // 结束对象借用，backing 页仍由内核堆持有。
#define AIRI_REGION_PAGES 16u // 当前有界内核区域最多持有十六个数据页。
struct airi_kernel_region { // 所有权句柄必须在使用前清零，不能按值复制后重复释放。
    uint64_t virtual_address; // 所有数据页连续映射的内核虚拟起点。
    uint64_t physical[AIRI_REGION_PAGES]; // 逐页记录实际借出的物理页，便于精确回滚。
    unsigned count; // 只有已经成功映射的页才计入所有权。
};
bool airi_memory_region_create(struct airi_kernel_region *region, uint64_t address, unsigned count); // 建立新的有界RW/NX内核区域，不覆盖已有映射。
bool airi_memory_region_free(struct airi_kernel_region *region); // 所有借用结束后先解除映射，再归还数据页。
struct airi_address_space; // 内核内部可持有地址空间，不向UI交出页表。
bool airi_memory_user_space(struct airi_address_space *space); // 从长期内核根借用高半区。
void airi_memory_use_kernel(void); // 返回启动内核地址空间后才能销毁用户空间。
uint64_t airi_memory_take_page(void); // 内核对象明确持有一个物理数据页。
bool airi_memory_return_page(uint64_t physical); // 映射和借用结束后归还。
void *airi_memory_direct(uint64_t physical); // 当前受支持RAM的HHDM别名，不创建新映射。
#endif // 结束包含保护。
```

<a id="heap-grow-mapping"></a>
## 完整映射新批次以后，才接入块链

memory.c 使用已经存在的 `airi_kernel_region` 保存每批十六个物理页。十六个区域句柄放在静态BSS中，heap_chunks只记录已经完成接入的批次；为堆申请元数据时不反过来调用尚在扩展的堆。新批次的地址由固定基址加已提交字节数算出，正好接在旧尾部。

`heap_grow` 先向区域接口取得下一批十六页。区域接口预检整个虚拟范围，再逐页申请、映射和登记；任一页失败，都先解除已经建立的映射，再归还本批数据页并清空句柄，堆的块链还没有改变。

整批成功时，新范围已经可写、不可执行并清零，但还没有成为对象分配器的一部分。此时才将它交给 `airi_heap_extend`：空末块可以向后延长，活末块后面则建立新的空块头。若对象层拒绝，它保证旧链保持；新批尚未借给对象，heap_grow就能解除该区域并回滚。只有页面和块链两层都接入成功，才增加heap_chunks、发布HEAP_GROW。

`airi_kmalloc` 首先尝试旧空闲块。找不到时增长一批再试，最多十六批；上限、页池耗尽或映射失败返回NULL，旧对象仍有效。零长度、SIZE_MAX和大于整个堆最大payload的请求在增长前拒绝。用户数据页及其记录页仍按K37的独立所有权管理，不从这个对象堆取得。

更新 `kernel/mm/memory.c`：替换固定堆构造与kmalloc，快照增加heap_used；页池、自有页表与区域接口的原逻辑保留。小对象演示的“本批容不下”检查直接调用底层heap_alloc，因为它只验证旧64 KiB范围，不能触发自动扩展。随后增加完整的大对象演示。

包含区说明对象块链与映射的分别管理。

本章每组替换的第一块只用于定位旧文本，敲入第二块替换它，不把两块同时加入文件。各组按正文顺序完成。

<!-- replace: kernel/mm/memory.c -->
<!-- before -->
```c
#include <airi/memory.h> // 内存服务对消费者只交出快照和对象。
#include <airi/heap.h> // 固定backing上的字节分配器。
#include <airi/vm.h> // 使用自有四级页表建立内核映射。
#include <airi/trap.h> // 真实页权限故障由异常入口报告。
#include <airi/pages.h> // 物理页分配与启动信息。
```
<!-- after -->
```c
#include <airi/memory.h> // 内存服务对消费者只交出快照和对象。
#include <airi/heap.h> // 页面映射与对象块链分别维护，扩展后保留活对象。
#include <airi/vm.h> // 使用自有四级页表建立内核映射。
#include <airi/trap.h> // 真实页权限故障由异常入口报告。
#include <airi/pages.h> // 物理页分配与启动信息。
```

将原固定堆区改为批次句柄、heap_grow 和新的初始构造；大对象观察与有界 kmalloc 也在同一区间接齐。

<!-- replace: kernel/mm/memory.c -->
<!-- before -->
```c
}

#define HEAP_ADDRESS 0xffffd00000000000ull // 固定堆虚拟范围不与内核、HHDM或演示页重叠。
#define HEAP_PAGES 16u // 首版堆固定 64 KiB，不自动扩容。
static struct airi_heap heap; // backing持有者和对象块链在整个内核期存在。
static void heap_init(void) { // 所需页与页表都由当前内核拥有。
    uint64_t backing[HEAP_PAGES]; // 保存每个已借出数据页，失败时精确回滚。
    unsigned count = 0; // 只统计已经成功映射的backing页。
    for (; count < HEAP_PAGES; ++count) { // 虚拟连续不要求物理连续。
        uint64_t physical = airi_page_alloc(&pages); // 每页独立取得所有权。
        if (!physical) // 页池耗尽时还没有本次页面可归还。
            break; // 转到统一回滚路径。
        if (!airi_vm_map(&kernel_space, HEAP_ADDRESS + count * AIRI_PAGE_SIZE, physical, AIRI_VM_WRITE)) { // 只开放supervisor读写，不执行。
            airi_page_free(&pages, physical); // 未接入的本次数据页立即归还。
            break; // 之前成功映射的页面稍后拆除。
        }
        backing[count] = physical; // 在映射成功后登记当前拥有的数据页。
    }
    if (count != HEAP_PAGES) { // 不允许半个堆被提供给调用者。
        while (count) { // 逆序拆除已成功映射的所有backing。
            --count; // 退到最后一个真实映射。
            airi_vm_unmap(&kernel_space, HEAP_ADDRESS + count * AIRI_PAGE_SIZE); // 先断开映射并使TLB失效。
            airi_page_free(&pages, backing[count]); // 然后才能归还数据页。
        }
        airi_panic("cannot map fixed kernel heap"); // 失败保持明确，不回退到未拥有的地址。
    }
    void *base = (void *)(uintptr_t)HEAP_ADDRESS; // 完整映射之后才形成可供堆访问的指针。
    airi_memset(base, 0, HEAP_PAGES * AIRI_PAGE_SIZE); // 清除页面旧内容，块链不会泄漏旧所有者数据。
    if (!airi_heap_init(&heap, base, HEAP_PAGES * AIRI_PAGE_SIZE)) // 把已持有范围交给小对象分配器。
        airi_panic("cannot initialize kernel heap"); // 参数不符合对齐或容量约定就停止。
    size_t before = airi_heap_free_bytes(&heap); // 结束后对象级空闲应恢复。
    uint8_t *a = airi_kmalloc(80); // 第一个对象拥有独立payload。
    uint8_t *b = airi_kmalloc(128); // 中间对象用于演示相邻归还合并。
    uint8_t *c = airi_kmalloc(32); // 后面仍有活对象时不能越过它合并。
    if (!a || !b || !c || (uintptr_t)a % 16) // 不能把NULL作为可写对象。
        airi_panic("kernel heap allocation failed"); // 明确拒绝后再写payload。
    a[79] = 0xa5; // 写在第一个对象的最后一个请求字节。
    c[31] = 0x5a; // 写在第三个对象的最后一个请求字节。
    if (airi_kfree(b + 1) || !airi_kfree(b) || !airi_kfree(a) || airi_kfree(a)) // 内部指针和重复释放必须被拒绝。
        airi_panic("kernel heap release validation failed"); // 不允许错误释放破坏空闲块。
    uint8_t *joined = airi_kmalloc(208); // 原80和128两块合并后才能在a的位置容纳208字节。
    if (joined != a || c[31] != 0x5a || airi_kmalloc(SIZE_MAX) || airi_kmalloc(0) || airi_kmalloc(HEAP_PAGES * AIRI_PAGE_SIZE)) // 检查合并、不覆盖活对象、溢出、零长度和耗尽。
        airi_panic("kernel heap split/coalesce failed"); // 不把分配失败伪装成有效指针。
    if (!airi_kfree(joined) || !airi_kfree(c) || airi_heap_free_bytes(&heap) != before) // 所有对象归还后应恢复单个空闲块容量。
        airi_panic("kernel heap accounting did not recover"); // backing仍持有，只有对象借用结束。
    airi_log("[HEAP_DEMO_OK] mapped_pages=16 bytes=%zu free_payload=%zu aligned=16 coalesced=1", heap.bytes, before); // 实际分配、归还和失败演示结束。
}
void *airi_kmalloc(size_t bytes) { // 内核小对象入口，不引入宿主libc。
    return airi_heap_alloc(&heap, bytes); // 保留有界堆的NULL失败语义。
}
bool airi_kfree(void *pointer) { // 对象所有者结束最后一个借用后调用。
    return airi_heap_free(&heap, pointer); // 对象空间回堆，物理页不因此归还。
```
<!-- after -->
```c
}

#define HEAP_ADDRESS 0xffffd00000000000ull // 固定堆虚拟范围不与内核、HHDM或演示页重叠。
#define HEAP_STEP_PAGES 16u // 初始与每次扩展都为64KiB。
#define HEAP_MAX_CHUNKS 16u // 总backing最多1MiB，仍位于同一2MiB叶子表覆盖内。
static struct airi_heap heap; // backing持有者和对象块链在整个内核期存在。
static struct airi_kernel_region heap_regions[HEAP_MAX_CHUNKS]; // 静态句柄记录每批物理页，不向尚未建立的堆递归申请元数据。
static unsigned heap_chunks; // 只有完整映射并接入块链的批次才计入。
static bool heap_grow(void) { // 单核不可抢占普通上下文调用，IRQ不进入分配器。
    if (heap_chunks == HEAP_MAX_CHUNKS) // 明确容量上限，不能越界写句柄或扩展到别的页表。
        return false; // 当前活对象与空闲块完全保持。
    struct airi_kernel_region *region = &heap_regions[heap_chunks]; // 未发布槽必须仍是空句柄。
    uint64_t address = HEAP_ADDRESS + heap_chunks * HEAP_STEP_PAGES * 4096ull; // 精确紧邻旧backing右端。
    if (!airi_memory_region_create(region, address, HEAP_STEP_PAGES)) // 整批映射失败由区域接口回滚，不提供半个新范围。
        return false; // heap的块链与字节容量还没有改变。
    void *tail = (void *)(uintptr_t)address; // 完整RW/NX映射已建立并清零，才能构造块头。
    bool accepted = heap_chunks ? airi_heap_extend(&heap, tail, HEAP_STEP_PAGES * 4096u) : airi_heap_init(&heap, tail, HEAP_STEP_PAGES * 4096u); // 第一批初始化，其余批只接在尾部。
    if (!accepted) { // 对象分配器拒绝接入时，它保证旧链未修改。
        if (!airi_memory_region_free(region)) // 新批还没交给任何对象，可完整解除和归还。
            airi_panic("cannot roll back kernel heap extension"); // 内部所有权错误不能吞掉。
        return false; // 原堆仍可继续分配和归还。
    }
    ++heap_chunks; // 从这里起句柄与块链共同拥有此批，直到内核生命周期结束。
    airi_log("[HEAP_GROW] mapped_pages=%u bytes=%zu", heap_chunks * HEAP_STEP_PAGES, heap.bytes); // 只有完整提交才报告容量。
    return true; // backing缓存不会随单个对象free自动缩小。
}
static void heap_init(void) { // 所需页与页表都由当前内核拥有。
    if (!heap_grow()) // 第一批也遵守映射成功后才接入的顺序。
        airi_panic("cannot initialize kernel heap"); // 启动必须有最小对象容量。
    size_t before = airi_heap_free_bytes(&heap); // 结束后对象级空闲应恢复。
    uint8_t *a = airi_kmalloc(80); // 第一个对象拥有独立payload。
    uint8_t *b = airi_kmalloc(128); // 中间对象用于演示相邻归还合并。
    uint8_t *c = airi_kmalloc(32); // 后面仍有活对象时不能越过它合并。
    if (!a || !b || !c || (uintptr_t)a % 16) // 不能把NULL作为可写对象。
        airi_panic("kernel heap allocation failed"); // 明确拒绝后再写payload。
    a[79] = 0xa5; // 写在第一个对象的最后一个请求字节。
    c[31] = 0x5a; // 写在第三个对象的最后一个请求字节。
    if (airi_kfree(b + 1) || !airi_kfree(b) || !airi_kfree(a) || airi_kfree(a)) // 内部指针和重复释放必须被拒绝。
        airi_panic("kernel heap release validation failed"); // 不允许错误释放破坏空闲块。
    uint8_t *joined = airi_kmalloc(208); // 原80和128两块合并后才能在a的位置容纳208字节。
    if (joined != a || c[31] != 0x5a || airi_kmalloc(SIZE_MAX) || airi_kmalloc(0) || airi_heap_alloc(&heap, HEAP_STEP_PAGES * AIRI_PAGE_SIZE)) // 检查合并、不覆盖活对象、溢出、零长度和耗尽。
        airi_panic("kernel heap split/coalesce failed"); // 不把分配失败伪装成有效指针。
    if (!airi_kfree(joined) || !airi_kfree(c) || airi_heap_free_bytes(&heap) != before) // 所有对象归还后应恢复单个空闲块容量。
        airi_panic("kernel heap accounting did not recover"); // backing仍持有，只有对象借用结束。
    airi_log("[HEAP_DEMO_OK] mapped_pages=16 bytes=%zu free_payload=%zu aligned=16 coalesced=1", heap.bytes, before); // 实际分配、归还和失败演示结束。
    struct airi_memory_stats baseline, grown; // 允许缓存增长，但结束时活payload与堆外页面必须恢复。
    airi_memory_snapshot(&baseline); // 初始堆仅64KiB且所有演示对象已归还。
    uint8_t *guard = airi_kmalloc(32); // 跨扩展保留一个活对象，不能移动或覆盖。
    if (!guard) // 尚未取得活对象时不能写入。
        airi_panic("cannot allocate heap growth guard"); // 普通耗尽保持明确。
    airi_memset(guard, 0x5a, 32); // 扩展前留下旧对象内容，随后仍须保持。
    uint8_t *large = airi_kmalloc(1024u * 1024 - 256); // 迫使按批扩展直到1MiB，而非返回首批外的无映射指针。
    if (!guard || !large) // 已有页资源不足就明确停止演示。
        airi_panic("cannot grow bounded kernel heap"); // 不访问失败指针。
    airi_memset(large, 0xa6, 1024u * 1024 - 256); // 实际写遍新映射，证明它们可访问。
    void *last = airi_kmalloc(128); // 吃掉最后空闲payload，堆达到明确边界。
    if (!last || airi_kmalloc(16) || airi_kmalloc(SIZE_MAX) || guard[31] != 0x5a || large[1024u * 1024 - 257] != 0xa6) // 超限不扩展到相邻未拥有范围，活内容保持。
        airi_panic("kernel heap growth limit or live data failed"); // 不把NULL以外的越界结果当成功。
    if (!airi_kfree(last) || !airi_kfree(large) || !airi_kfree(guard) || airi_heap_used_bytes(&heap)) // 所有临时对象归还，backing仍缓存1MiB。
        airi_panic("kernel heap growth leaked live payload"); // 不把持有页面与活对象混算。
    airi_memory_snapshot(&grown); // 新堆空闲payload比原来大，但外部借用没有增加。
    if (!airi_memory_same_usage(&baseline, &grown) || grown.heap_bytes != 1024u * 1024 || grown.heap_free != grown.heap_bytes - 32) // 缓存增长精确对应新增数据页，尾块完全合并。
        airi_panic("kernel heap cache accounting failed"); // 页或对象泄漏仍需报告。
    airi_log("[HEAP_CAPACITY_OK] initial=65536 bytes=%zu live=%zu chunks=%u preserved=1 limit=null", grown.heap_bytes, grown.heap_used, heap_chunks); // 真实映射、写入、耗尽和归还完成。

}
void *airi_kmalloc(size_t bytes) { // 内核小对象入口，不引入宿主libc。
    if (!heap.first || !bytes || bytes > HEAP_MAX_CHUNKS * HEAP_STEP_PAGES * 4096u - 32) // 未初始化、空请求或绝不可能容纳的对象不触发增长。
        return NULL; // 也在向上对齐前排除SIZE_MAX等溢出输入。
    for (;;) { // 每次失败最多增加一个64KiB批次，总共只有十六批。
        void *object = airi_heap_alloc(&heap, bytes); // 先利用已有空闲块，不为可重用空间增加页面。
        if (object) // 成功返回的对象可能来自旧空洞或新尾部。
            return object; // 已有对象从未移动。
        if (!heap_grow()) // 上限或物理/映射失败都只影响本次新请求。
            return NULL; // 调用者保留所有旧借用及其内容。
    }
}
bool airi_kfree(void *pointer) { // 对象所有者结束最后一个借用后调用。
    return airi_heap_free(&heap, pointer); // 对象空间回堆，物理页不因此归还。
```

快照补上活payload，原有页池、缓存字节与空闲payload继续分别保存。

<!-- replace: kernel/mm/memory.c -->
<!-- before -->
```c
    stats->pages_total = pages.total; // 固定受管理物理页容量。
    stats->pages_free = pages.free; // 减去页表和堆backing等当前所有者。
    stats->heap_bytes = heap.bytes; // 已映射的总backing字节。
    stats->heap_free = airi_heap_free_bytes(&heap); // 空闲对象payload，不包含块头。
}

```
<!-- after -->
```c
    stats->pages_total = pages.total; // 固定受管理物理页容量。
    stats->pages_free = pages.free; // 减去页表和堆backing等当前所有者。
    stats->heap_bytes = heap.bytes; // 已映射的总backing字节。
    stats->heap_used = airi_heap_used_bytes(&heap); // 真实活payload与缓存页面分别报告。
    stats->heap_free = airi_heap_free_bytes(&heap); // 空闲对象payload，不包含块头。
}

```

大对象演示先在旧堆保留一个32字节guard并写0x5a，再申请1048320字节，即1 MiB减256字节。分配器必须真实映射新页，才能逐字节写入0xa6。此时已有两个活块头及一个末尾空闲块头，最后128字节payload仍可申请；再申请16字节必须失败。旧guard与大对象末字节继续保持。

三个对象归还后，相邻块跨批次合并，只剩一个32字节初始块头，空闲payload为1048544字节，heap_used为零。新的资源核对允许缓存从十六页增长到256页，但不允许丢失其他页面。这里没有人为制造整机物理页耗尽；部分映射失败的回滚沿用已有区域接口，实际演示的失败是堆容量边界。

<a id="heap-grow-callers"></a>
## 会话收尾采用同一套资源规则

在 `kernel/task/task.c` 收尾处，将两项旧字段比较替换为 `!airi_memory_same_usage(&before, &after)`。任务仍必须离开目标栈后再归还栈与页表；新核对不放宽这些页面的释放条件。

在 `kernel/process/user.c` 的 launch 中也有三处需要替换：初始映像失败、异步owner创建失败，以及完整用户会话收尾。每处保留原有FD计数条件。它们共用同一规则，避免某条错误路径仍把缓存增长当作泄漏。除此以外，K37的页记录、ELF、fork/exec/wait和输入监督保持。下面直接给出这些条件在旧文件中的位置。

在任务演示回收后的快照处替换旧比较，目标栈已经归还，活对象与缓存继续分别核对。

<!-- replace: kernel/task/task.c -->
<!-- before -->
```c
        if (!airi_memory_region_free(&tasks[id].stack)) // 先解除映射和空页表，再归还数据页。
            airi_panic("cannot reclaim completed task stack"); // 不保留隐藏泄漏。
    airi_memory_snapshot(&after); // 回收后的当前实际统计。
    if (after.pages_free != before.pages_free || after.heap_free != before.heap_free) // 任务栈和临时页表必须全部恢复。
        airi_panic("completed kernel tasks leaked memory"); // 内核演示不应吃掉后续主线的容量。
    airi_log("[TASK_DEMO_OK] order=121212 separate_stacks=2 reclaimed=1"); // 只有切换、局部值、顺序与回收全部成立才输出。
}
```
<!-- after -->
```c
        if (!airi_memory_region_free(&tasks[id].stack)) // 先解除映射和空页表，再归还数据页。
            airi_panic("cannot reclaim completed task stack"); // 不保留隐藏泄漏。
    airi_memory_snapshot(&after); // 回收后的当前实际统计。
    if (!airi_memory_same_usage(&before, &after)) // 任务栈和临时页表必须全部恢复。
        airi_panic("completed kernel tasks leaked memory"); // 内核演示不应吃掉后续主线的容量。
    airi_log("[TASK_DEMO_OK] order=121212 separate_stacks=2 reclaimed=1"); // 只有切换、局部值、顺序与回收全部成立才输出。
}
```

launch 的初始 ELF 准备失败后，用同一次前后快照的缓存规则核对资源。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    int error = elf ? prepare_elf(root, image, bytes, name, true) : (prepare_flat(root, image, bytes) ? 0 : -12); // ELF保留格式与资源错误之别。
    if (error) { // 失败构造也必须完整恢复资源。
        airi_memory_snapshot(&after); // 不能只看错误码忽略泄漏。
        if (before.pages_free != after.pages_free || before.heap_free != after.heap_free || airi_files_open_count()) // 半份新映像不应改变空闲数量。
            airi_panic("failed user launch leaked resources"); // 内部错误与普通坏文件分开。
        return error; // 调用者可以继续命令，不停机。
    }
```
<!-- after -->
```c
    int error = elf ? prepare_elf(root, image, bytes, name, true) : (prepare_flat(root, image, bytes) ? 0 : -12); // ELF保留格式与资源错误之别。
    if (error) { // 失败构造也必须完整恢复资源。
        airi_memory_snapshot(&after); // 不能只看错误码忽略泄漏。
        if (!airi_memory_same_usage(&before, &after) || airi_files_open_count()) // 半份新映像不应改变空闲数量。
            airi_panic("failed user launch leaked resources"); // 内部错误与普通坏文件分开。
        return error; // 调用者可以继续命令，不停机。
    }
```

异步身份准备失败时同样核对缓存与活对象，FD条件保持。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    if (async_error) { // 未向用户发布，此时根与栈不活动。
        cleanup(root); // 回滚页面、FD与内核栈，尚未拥有异步owner。
        airi_memory_snapshot(&after); // 失败不能留下普通内存泄漏。
        if (before.pages_free != after.pages_free || before.heap_free != after.heap_free) // 静态池外的资源同样核对。
            airi_panic("failed async owner launch leaked memory"); // 内部不变量明确停止。
        return async_error; // 应用可见普通资源失败。
    }
```
<!-- after -->
```c
    if (async_error) { // 未向用户发布，此时根与栈不活动。
        cleanup(root); // 回滚页面、FD与内核栈，尚未拥有异步owner。
        airi_memory_snapshot(&after); // 失败不能留下普通内存泄漏。
        if (!airi_memory_same_usage(&before, &after)) // 静态池外的资源同样核对。
            airi_panic("failed async owner launch leaked memory"); // 内部不变量明确停止。
        return async_error; // 应用可见普通资源失败。
    }
```

整次用户会话收尾也使用同一比较规则，已释放的状态仍需恢复。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    kernel_return = NULL; // 下次会话重新登记。
    airi_async_drain(); // 所有owner已关闭，内核仍须结束无主ACTIVE并核对静态池完全回收。
    airi_memory_snapshot(&after); // 全部页和栈回收后验证。
    if (before.pages_free != after.pages_free || before.heap_free != after.heap_free || airi_files_open_count()) // 用户程序退出码可以任意，但内核资源必须恢复。
        airi_panic("user process session leaked memory"); // 不把泄漏当成应用错误跳过。
    return 0; // 表示已完整启动与收拢，程序结果另在result.code。
}
```
<!-- after -->
```c
    kernel_return = NULL; // 下次会话重新登记。
    airi_async_drain(); // 所有owner已关闭，内核仍须结束无主ACTIVE并核对静态池完全回收。
    airi_memory_snapshot(&after); // 全部页和栈回收后验证。
    if (!airi_memory_same_usage(&before, &after) || airi_files_open_count()) // 用户程序退出码可以任意，但内核资源必须恢复。
        airi_panic("user process session leaked memory"); // 不把泄漏当成应用错误跳过。
    return 0; // 表示已完整启动与收拢，程序结果另在result.code。
}
```

<a id="heap-grow-tests"></a>
## 在已有对象还活着时验证两种扩展

主机检查继续用同一份heap.c，arena模拟已经映射的连续内存。原先2000步交错分配/归还保留；它们结束并归还全部对象后，检查从arena前512字节重新开始：占满480字节payload，扩展第二段，再从新段申请400字节。原对象与新对象都写入不同字节，以核对拒绝有洞、过短、未对齐和溢出范围后内容未变。

两对象归还后扩展空末块，应该只有一个块头；再次占满并扩展，则应出现独立新末块。随后归还，整个arena仍能合并成一块。资源快照夹具还分别演示正常缓存增长、堆外页面未归还、活对象未归还三种情况。更新 `tests/unit/heap_test.c`。这些宿主检查验证对象算法，真实映射与访问另由guest演示完成。

<!-- write: tests/unit/heap_test.c -->
```c
#include <assert.h> // 失败直接返回非零，不依赖文字判断。
#include <stdio.h> // 宿主输出测试结果。
#include <airi/memory.h> // 对象借用与backing缓存使用不同的资源核对。
#include <airi/heap.h> // 同一小对象算法不依赖宿主页表。
static uint8_t arena[4096] __attribute__((aligned(16))); // 宿主只提供一个受控的已拥有范围。
struct item { uint8_t *pointer; size_t bytes; uint8_t pattern; }; // 记录每个活对象的内容预期。
int main(void) { // 独立验证对象生命周期和碎片回收。
    struct airi_heap heap = {0}; // 初始没有backing。
    assert(!airi_heap_init(&heap, arena + 1, sizeof(arena) - 1)); // 未对齐起点不能偷偷接受。
    assert(!airi_heap_init(&heap, arena, 32)); // 只有块头没有payload时必须失败。
    assert(airi_heap_init(&heap, arena, sizeof(arena))); // 正常创建单块空闲堆。
    size_t initial = airi_heap_free_bytes(&heap); // 记录元数据之外的真实payload容量。
    assert(initial == sizeof(arena) - 32); // 当前块头32字节。
    assert(!airi_heap_alloc(&heap, 0) && !airi_heap_alloc(&heap, SIZE_MAX)); // 零长度和对齐溢出均失败。
    assert(!airi_heap_alloc(&heap, sizeof(arena)) && airi_heap_free_bytes(&heap) == initial); // 耗尽不修改空闲状态。
    void *a = airi_heap_alloc(&heap, 80); // 创建相邻的三块，以控制碎片位置。
    void *b = airi_heap_alloc(&heap, 128); // 后续先归还中间块。
    uint8_t *c = airi_heap_alloc(&heap, 32); // 活对象阻止跨越它合并。
    assert(a && b && c && (uintptr_t)a % 16 == 0); // 所有对象非空且对齐。
    c[31] = 0x5a; // 保留活对象尾字节，确认合并不覆盖它。
    assert(!airi_heap_free(&heap, c + 1)); // 内部指针不能作为对象起点。
    assert(airi_heap_free(&heap, b) && airi_heap_free(&heap, a)); // 两个相邻空闲块应合并。
    assert(!airi_heap_free(&heap, a)); // 重复释放不得改变状态。
    void *joined = airi_heap_alloc(&heap, 208); // 请求大于80和128，必须利用合并结果。
    assert(joined == a && c[31] == 0x5a); // first-fit复用合并块，第三对象内容不变。
    assert(airi_heap_free(&heap, joined) && airi_heap_free(&heap, c)); // 全部归还会继续与尾块合并。
    assert(airi_heap_free_bytes(&heap) == initial && airi_heap_free(&heap, NULL)); // 合并恢复最初容量，NULL为空操作。
    struct item items[16] = {0}; // 十六个同时存活对象，空槽pointer为NULL。
    uint32_t random = 0x12345678; // 固定种子使失败可以逐次复现。
    for (unsigned step = 0; step < 2000; ++step) { // 完整演示交错大小、归还和碎片重用。
        random = random * 1664525u + 1013904223u; // 无符号运算按32位回绕，生成确定序列。
        unsigned slot = (random >> 16) % 16; // 从高位选择对象槽，避免只重复低位周期。
        struct item *item = &items[slot]; // 本次只改变一个活对象或空槽。
        if (item->pointer) { // 活槽先验证再归还。
            for (size_t i = 0; i < item->bytes; ++i) // 检查整个请求范围的内容。
                assert(item->pointer[i] == item->pattern); // 其他分配和归还不能覆盖活对象。
            assert(airi_heap_free(&heap, item->pointer)); // 结束该对象生命周期。
            item->pointer = NULL; // 槽不再持有过期指针。
        } else { // 空槽尝试不同大小分配。
            item->bytes = 1 + ((random >> 8) % 240); // 覆盖不足16字节和多个对齐跨度。
            item->pointer = airi_heap_alloc(&heap, item->bytes); // 容量不足允许返回NULL。
            item->pattern = (uint8_t)(slot + 1); // 各槽使用不同内容以发现重叠。
            if (item->pointer) { // 只有取得所有权后才写入。
                assert((uintptr_t)item->pointer % 16 == 0); // 每种大小都保持16字节对齐。
                airi_memset(item->pointer, item->pattern, item->bytes); // 填满请求范围，不写对齐填充区。
            }
        }
        for (unsigned n = 0; n < 16; ++n) // 每次操作后检查所有仍活着的对象。
            if (items[n].pointer) // 空槽没有可访问内容。
                for (size_t i = 0; i < items[n].bytes; ++i) // 不只检查被选槽。
                    assert(items[n].pointer[i] == items[n].pattern); // 检测分割、合并或重复借出导致的破坏。
    }
    for (unsigned n = 0; n < 16; ++n) // 释放最后留下的活对象。
        assert(airi_heap_free(&heap, items[n].pointer)); // NULL槽也明确允许。
    assert(airi_heap_free_bytes(&heap) == initial); // 不留下隐藏块头碎片或借用泄漏。
    assert(airi_heap_init(&heap, arena, 512)); // 所有旧对象已归还，重新用同一arena前512字节建立小堆。
    uint8_t *held = airi_heap_alloc(&heap, 480); // 占满末块，使扩展走新增块头分支。
    assert(held && airi_heap_used_bytes(&heap) == 480); // 活payload不含32字节块头。
    airi_memset(held, 0x6b, 480); // 新backing接入不能移动或覆盖此对象。
    assert(airi_heap_extend(&heap, arena + 512, 512)); // 第二段虚拟上精确紧邻旧右端。
    uint8_t *added = airi_heap_alloc(&heap, 400); // 从新增空块取得另一个对象。
    assert(added && added != held && airi_heap_used_bytes(&heap) == 880); // 两个对象各自拥有payload。
    airi_memset(added, 0xa3, 400); // 后续失败与合并不能改变活内容。
    assert(!airi_heap_extend(&heap, arena + 1536, 512)); // 有洞的新范围不能进入块链。
    assert(!airi_heap_extend(&heap, arena + 1024, 32)); // 容不下块头加最小payload。
    assert(!airi_heap_extend(&heap, arena + 1024, 511)); // 长度必须保持16字节对齐。
    assert(!airi_heap_extend(&heap, arena + 1024, SIZE_MAX & ~(size_t)15)); // 巨大右端不能回绕。
    assert(heap.bytes == 1024 && airi_heap_used_bytes(&heap) == 880); // 拒绝没有悄悄发布容量。
    for (unsigned i = 0; i < 480; ++i) // 扩展前的对象所有字节仍在原地址。
        assert(held[i] == 0x6b); // 不只检查边界字节。
    for (unsigned i = 0; i < 400; ++i) // 新对象在失败扩展后也继续有效。
        assert(added[i] == 0xa3); // 不能部分覆盖已有链。
    assert(airi_heap_free(&heap, held) && airi_heap_free(&heap, added)); // 跨旧新backing边界的块可合并。
    assert(airi_heap_extend(&heap, arena + 1024, 1024)); // 空末块直接延长，不增加另一个块头。
    assert(airi_heap_free_bytes(&heap) == 2048 - 32 && !airi_heap_used_bytes(&heap)); // 只有一个初始块头，活对象为零。
    held = airi_heap_alloc(&heap, 2016); // 用跨扩展边界的大对象占满当前范围。
    assert(held && airi_heap_extend(&heap, arena + 2048, 2048)); // 已借出尾块仍走独立新块分支。
    assert(airi_heap_used_bytes(&heap) == 2016 && airi_heap_free_bytes(&heap) == 2016); // 旧对象容量不会随backing增加。
    assert(airi_heap_free(&heap, held) && airi_heap_free_bytes(&heap) == sizeof(arena) - 32); // 最后恢复一个完整空闲块。
    struct airi_memory_stats before = {.pages_free=100, .heap_bytes=65536, .heap_used=16}; // 已有一个活对象的资源快照。
    struct airi_memory_stats after = {.pages_free=84, .heap_bytes=131072, .heap_used=16}; // 十六个页仅从页池转入堆缓存。
    assert(airi_memory_same_usage(&before, &after)); // 空闲缓存增长不会冒充对象泄漏。
    --after.pages_free; // 另有一个不属于堆缓存的页面没有归还。
    assert(!airi_memory_same_usage(&before, &after)); // 真实页泄漏仍须被拒绝。
    ++after.pages_free; // 还原页面数，单独改变活对象容量。
    after.heap_used = 32; // 新活payload未归还，不能被缓存容量掩盖。
    assert(!airi_memory_same_usage(&before, &after)); // 对象泄漏与backing缓存分开判断。
    puts("PASS heap: alignment, overflow, exhaustion, invalid free, coalescing, 2000 lifetime steps, contiguous extension, cache vs live usage"); // 全部实际断言通过。
    return 0; // 正常退出宿主。
}
```

启动检查需在当前READY之前看见新的HEAP_CAPACITY_OK，保证不是只链接了扩展函数。更新 `scripts/boot.py` 的正常启动分支，故障配置仍按原有停止位置核对；缺内存、未处理异常等早期失败不要求经过堆扩展。

在正常启动的堆完成条件中加入 HEAP_CAPACITY_OK；早期故障分支保持原停止位置。

<!-- replace: scripts/boot.py -->
<!-- before -->
```python
                        assert '[PROCESS_FORK_WAIT_OK]' in text and '[PROCESS_DEMO_OK]' in text # 复制、真实阻塞、timer换身份和回收都须完成。
                        assert '[ELF_LOAD]' in text and '[ELF_USER_OK]' in text # 独立ELF段、BSS与用户libc必须真正运行。
                        assert '[USER_DEMO_OK]' in text # 用户输出、指针拒绝和用户故障后内核必须继续。
                        assert '[TASK_DEMO_OK]' in text # 两个独立栈必须交错并全部回收。
                        assert '[TIMER_DEMO_OK]' in text # 时钟必须在真实IRQ唤醒后完成。
                        assert '[AIRI_P2_READY]' in text # 当前主线还必须完成页池、自有页表和堆。
```
<!-- after -->
```python
                        assert '[PROCESS_FORK_WAIT_OK]' in text and '[PROCESS_DEMO_OK]' in text # 复制、真实阻塞、timer换身份和回收都须完成。
                        assert '[ELF_LOAD]' in text and '[ELF_USER_OK]' in text # 独立ELF段、BSS与用户libc必须真正运行。
                        assert '[USER_DEMO_OK]' in text # 用户输出、指针拒绝和用户故障后内核必须继续。
                        assert '[HEAP_CAPACITY_OK] initial=65536 bytes=1048576 live=0 chunks=16 preserved=1 limit=null' in text # 新映射、活对象保持、上限和缓存归属均实际演示。
                        assert '[TASK_DEMO_OK]' in text # 两个独立栈必须交错并全部回收。
                        assert '[TIMER_DEMO_OK]' in text # 时钟必须在真实IRQ唤醒后完成。
                        assert '[AIRI_P2_READY]' in text # 当前主线还必须完成页池、自有页表和堆。
```

串口mem查询现在要看到实际扩展后的1048576字节。更新 `scripts/console_test.py` 中这一期望，输入数量、后续程序与错误恢复保持。可选图形读取同一数值快照；这里不添加前端路径或新的图形前置。

将本次 mem 查询的期望改为实际扩展后的1MiB，命令顺序和输入通路保持。

<!-- replace: scripts/console_test.py -->
<!-- before -->
```python
                offset=len(output()) # 内存查询使用本次命令之后的新记录。
                wire.sendall(b'mem\n') # 同一串口主线查询真实页池与堆。
                tail=wait(update(0,4),offset) # 等第四次提交的完整状态。
                assert 'page pool: total=' in tail and 'kernel heap: bytes=65536' in tail # 不能只返回启动地图。
                for number, text, expected in [(5, 'programs', 'programs: registered ELF images'), (6, 'run hello', 'program hello exited 0'), (7, 'run broken', 'cannot load broken: error=-8'), (8, 'run worker', 'program worker exited 23'), (9, 'run processes', 'program processes exited 0'), (10, 'run missing', 'program not found: missing'), (11, 'echo after-programs', 'after-programs')]: # 正常、坏ELF、非零退出和未知名称都不破坏后续会话。
                    offset=len(output()) # 当前命令只消费新日志。
                    wire.sendall((text + '\n').encode()) # 发送真正UART输入，而不是直接调用内核函数。
```
<!-- after -->
```python
                offset=len(output()) # 内存查询使用本次命令之后的新记录。
                wire.sendall(b'mem\n') # 同一串口主线查询真实页池与堆。
                tail=wait(update(0,4),offset) # 等第四次提交的完整状态。
                assert 'page pool: total=' in tail and 'kernel heap: bytes=1048576' in tail # 不能只返回启动地图。
                for number, text, expected in [(5, 'programs', 'programs: registered ELF images'), (6, 'run hello', 'program hello exited 0'), (7, 'run broken', 'cannot load broken: error=-8'), (8, 'run worker', 'program worker exited 23'), (9, 'run processes', 'program processes exited 0'), (10, 'run missing', 'program not found: missing'), (11, 'echo after-programs', 'after-programs')]: # 正常、坏ELF、非零退出和未知名称都不破坏后续会话。
                    offset=len(output()) # 当前命令只消费新日志。
                    wire.sendall((text + '\n').encode()) # 发送真正UART输入，而不是直接调用内核函数。
```

<a id="heap-grow-observe"></a>
## 构建、查询，再运行原有用户程序

从K37完成上述修改后，在同一学习目录运行：

<!-- run -->
```sh
./airios build # 用新的对象算法、页映射与快照核对重建当前镜像。
./airios test # 实际运行主机检查、正常与故障启动、串口、用户和存储回归。
```

Mac默认python3若仍指向旧系统版本，在命令前使用`PATH=/opt/homebrew/bin:$PATH`。交互入口与guest输入如下：

```sh
./airios run # 观察本轮启动日志，Ctrl-A后X由宿主结束QEMU。
```

```text
mem
run sh
capacity
exit
```

启动先发布旧小对象演示，再出现逐批HEAP_GROW；最后一批与完成记录为：

```text
[HEAP_GROW] mapped_pages=256 bytes=1048576
[HEAP_CAPACITY_OK] initial=65536 bytes=1048576 live=0 chunks=16 preserved=1 limit=null
```

mem查询应显示`kernel heap: bytes=1048576 free_payload=1048544`。页池空闲页少于K37同环境启动时，是因为堆缓存增加了240个数据页，内核BSS与映像本身也可能影响启动可用页数，不能只拿两个版本的总数相减推断泄漏。同一会话内的前后核对使用当前真实基准。随后capacity仍完成四轮满1024页的用户fork/exec，shell退出返回监控台。

超出堆上限的申请只返回NULL，已经借出的guard与large不会移动。指针不是当前活对象的起点、对象已经归还或输入长度无效时，接口仍按K17拒绝。内核调用者仍负责结束全部借用后再free；本节没有为旧指针增加代数或引用计数。

Linux对照继续使用L02的映射、RSS和触页观察。AiriOS这里按批立即映射并保留backing，用户层的Linux malloc、线程缓存与内核slab也各有不同策略；不能把这套计数直接当作Linux的RSS含义。个人Linux实验仍未验证。下一步继续把正式启动策略与设备服务从当前教学启动路径中分开。

