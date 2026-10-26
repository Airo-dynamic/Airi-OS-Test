# K17 · 在持有的页面上分配小对象

页池每次借出 4096 字节，任务或文件记录却可能只要几十字节。让每个小对象独占一页会浪费，因此先借十六个物理页，用自己的页表接成连续 64 KiB 虚拟范围，再在其中划块。物理页可以不相邻，连续的虚拟地址让对象算法按普通字节处理。

本章先写 heap 接口和算法，再建立 memory 服务并接入现有 mem；K16 页表和异常保留。新增主机观察与构建输入，最后才运行。初始化依赖始终是页池、页表、堆，不让底层先依赖尚未存在的 kmalloc。

<a id="heap-backing"></a>

## 归还对象，不等于归还承载它的页

十六页的长期所有者是内核堆，页池将它们记为已借出，页表映到 `0xffffd00000000000` 起的 supervisor 读写 NX 区。heap.c 只在这片已可访问空间中写元数据和内容，不再借页或执行特权指令。

每块有 32 字节头，保存 payload 容量、后继和 free。payload 按 16 对齐，适合当前普通整数、指针和结构，更高对齐或 DMA 需另有接口。开始只有一个空块，可分配容量为 65536−32=65504。free 对象只让这片空间回堆，backing 页仍占用，页池 free 不增加；分散空块总和也不保证能装下一份同样大的连续请求。

**新建 `include/airi/heap.h`。** 接口用不完全类型保存块指针，具体布局留实现。

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
void *airi_heap_alloc(struct airi_heap *heap, size_t bytes); // 返回 16 字节对齐的未初始化小对象，失败或零长度返回 NULL。
bool airi_heap_free(struct airi_heap *heap, void *pointer); // NULL 作为空操作，其他指针必须是当前借出的对象起点。
size_t airi_heap_free_bytes(const struct airi_heap *heap); // 空闲 payload 字节和，不含块头，不保证可以一次分配这么大。
#endif // 结束包含保护。
```

<a id="heap-split"></a>

## 一次请求怎样拆开并重新连接块链

先沿稍后 guest 的第一条 `airi_kmalloc(80)` 看分割。它把 80 交给 airi_heap_alloc 的 bytes；零与 bytes+15 溢出先拒绝，随后向上对齐得到 needed=80。首次适配从头找空块，初始 payload 为 65504，取出 80 后还足够放 32 字节新头和至少 16 字节 payload，因此创建 tail。

`b+1` 先跨过整个 32 字节头，转成 uint8_t* 再加 needed，才落在本次 80 字节之后的新头位置。tail 的 payload 为 65504−80−32=65392，它先接住旧 next；当前块再缩为 80、把 next 指向 tail。先接住旧后继，后面的活对象才不会从链上丢失。返回前把当前 free 置 false，交出的 b+1 就是本次对象起点。

头大小与 needed 都是 16 的倍数，后续 payload 继续对齐。一般请求的余量若不足新头加 16，则整块借出，不产生不能使用的尾块；分散空闲容量仍不保证满足连续请求。

free 不从任意传入地址往前读头，而沿已有链比较精确 payload 起点，拒绝内部指针与重复释放。找到活对象才改 free，再合并相邻空块，旧后继头的 32 字节也回到容量。例如 80 与 128 两块分别不足 208，合并后可用 80+32+128=240，便能重新从首地址容纳该请求。

归还前要结束借用，之后停止用旧指针。地址复用后的过期引用、越界破坏头部仍无法自动识别；当前内核所有者必须守约。输入 backing 已由调用者拥有并映射，不借宿主 malloc。

**新建 `kernel/mm/heap.c`：**

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
```

初始化只建立第一块，尚未借出 payload。下面让 80 字节请求进入刚才的分割过程，先准备 tail 的后继，再缩短当前块；失败仍不改变已有块链。

**继续追加首次适配分配：**

<!-- append: kernel/mm/heap.c -->
```c
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
```

三个对象依次借出后，a/b/c 的头已相邻链接。归还 b 再归还 a，使前两项合并为 80+32+128=240；申请 208 时只余 32，不足新头加最小 payload 的 48，因此整块复用 a。c 仍为活块，合并不能跨过它。下面沿块链查精确起点，再改变状态和合并。

**继续追加归还与空闲 payload 统计：**

<!-- append: kernel/mm/heap.c -->
```c
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

<a id="memory-service"></a>

## 控制台需要结果数值，不需要修改分配器内部对象

后续内核通过 kmalloc/kfree 取得小对象，mem 则只想显示资源状态。把当前 pages_total/free 与 heap_bytes/free 复制到一个小快照：前两项以页计，heap_bytes 是包含块头的 backing 字节数，heap_free 才是空闲 payload 字节和，不能合为同一个“剩余内存”。

消费者不拿位图、块头或页表内部指针，文本与未来图形都可使用相同数值。memory.h 因而承担服务接口，页算法头只保留自己职责；NULL 和布尔失败继续明确区分。

**新建 `include/airi/memory.h`：**

<!-- write: include/airi/memory.h -->
```c
#ifndef AIRI_MEMORY_H // 通用内存服务与只读数据快照。
#define AIRI_MEMORY_H // 包含保护。
#include <airi/boot.h> // 启动合同和固定宽整数。
#include <stddef.h> // 小对象请求按字节计数。
struct airi_memory_stats { // 消费者只得到数值，不持有分配器内部块指针。
    uint32_t pages_total, pages_free; // 实际受管理物理页与当前未借出页数。
    size_t heap_bytes, heap_free; // 固定堆 backing 字节与空闲 payload 字节。
};
void airi_memory_init(const AiriBootInfo *info); // 顺序初始化页池、页表和固定内核堆。
void airi_memory_snapshot(struct airi_memory_stats *stats); // 同步复制当前数值到调用者对象。
void *airi_kmalloc(size_t bytes); // 小对象来自已映射内核堆，不承诺完整 libc malloc ABI。
bool airi_kfree(void *pointer); // 结束对象借用，backing 页仍由内核堆持有。
#endif // 结束包含保护。
```

将 memory_init 声明移到 memory.h，不改变页池算法。这样只使用页面的调用者不会仅因包含 pages.h 就依赖堆服务。

**在 `include/airi/pages.h` 删除原 memory_init 声明，保留页面接口：**

在该文件依次找到以下旧窗口，各用对应新文本完整替换；其他内容保留。

<!-- replace: include/airi/pages.h -->
<!-- before -->
```c
uint64_t airi_page_alloc(struct airi_page_pool *pool); // 返回物理页起点，耗尽返回 0；物理页 0 永不借出。
bool airi_page_free(struct airi_page_pool *pool, uint64_t physical); // 对齐、归属和借出状态均有效才释放。
void airi_memory_init(const AiriBootInfo *info); // 在真实 guest 上建立页池并演示分配、写入和回收。
#endif // 结束包含保护。
```
<!-- after -->
```c
uint64_t airi_page_alloc(struct airi_page_pool *pool); // 返回物理页起点，耗尽返回 0；物理页 0 永不借出。
bool airi_page_free(struct airi_page_pool *pool, uint64_t physical); // 对齐、归属和借出状态均有效才释放。
#endif // 结束包含保护。
```

## backing 全部就绪，才创建第一个可分配块

memory.c 逐页借出并映射，成功才登记；中途失败先逆序解除已建映射、失效翻译，再还数据页。本轮已借出却映射失败的那页也单独归还，不能只回滚数组里已经登记的部分。十六页齐备后才清零并初始化堆。

演示分 80、128、32 三块，保存第三块尾字节。归还前两项后申请 208，应回到第一个地址，第三项仍不变。最后还完所有对象，payload 恢复 65504，而 backing 仍由堆持有。零请求、SIZE_MAX、超过 backing、内部指针与重复释放也都在对应作用处拒绝。

**局部更新 `kernel/mm/memory.c` 并追加堆服务，保持页池→页表→堆的初始化顺序：**

在该文件依次找到以下旧窗口，各用对应新文本完整替换；其他内容保留。

<!-- replace: kernel/mm/memory.c -->
<!-- before -->
```c
#include <airi/vm.h> // 使用自有四级页表建立内核映射。
#include <airi/trap.h> // 真实页权限故障由异常入口报告。
```
<!-- after -->
```c
#include <airi/memory.h> // 内存服务对消费者只交出快照和对象。
#include <airi/heap.h> // 固定backing上的字节分配器。
#include <airi/vm.h> // 使用自有四级页表建立内核映射。
#include <airi/trap.h> // 真实页权限故障由异常入口报告。
```

继续处理下一处：

<!-- replace: kernel/mm/memory.c -->
<!-- before -->
```c
#include <airi/kernel.h> // 日志和具体失败出口。
static struct airi_page_pool pages; // 页池元数据放在内核 BSS，不能自己被分配。
static void virtual_memory_init(const AiriBootInfo *info); // 页池就绪后才建立并激活页表。
void airi_memory_init(const AiriBootInfo *info) { // 在异常设施安装后调用，仍使用加载器页表。
```
<!-- after -->
```c
#include <airi/kernel.h> // 日志和具体失败出口。
static struct airi_page_pool pages; // 页池元数据放在内核 BSS，不能自己被分配。
static void heap_init(void); // 自有页表建立后才映射堆backing。
static void virtual_memory_init(const AiriBootInfo *info); // 页池就绪后才建立并激活页表。
void airi_memory_init(const AiriBootInfo *info) { // 在异常设施安装后调用，仍使用加载器页表。
```

继续处理下一处：

<!-- replace: kernel/mm/memory.c -->
<!-- before -->
```c
             (unsigned long long)first, (unsigned long long)second, (unsigned long long)again, pages.free); // 记录数值，不在归还后解引用旧指针。
    virtual_memory_init(info); // 页表页也从同一所有权池取得。
}

```
<!-- after -->
```c
             (unsigned long long)first, (unsigned long long)second, (unsigned long long)again, pages.free); // 记录数值，不在归还后解引用旧指针。
    virtual_memory_init(info); // 页表页也从同一所有权池取得。
    heap_init(); // 小对象分配不能反过来成为页表构建的前置。
}

```

继续处理下一处：

已有页池与虚拟内存定义保留。现在在文件末尾追加固定堆的 backing 建立与服务接口：

<!-- append: kernel/mm/memory.c -->
```c

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
```

十六次循环只登记已映射的数据页，失败先拆映射再归还这些页；本次映射失败的页在登记前就已单独归还。走到下面 count=16，整个虚拟 backing 才可写，清零后交给 heap_init，再运行三对象演示。

**继续追加堆初始化与对象演示：**

<!-- append: kernel/mm/memory.c -->
```c
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
```

所有对象归还后，heap 仍长期持有十六页，块链恢复一块 65504 字节 payload。服务包装从这份静态 heap 分配对象，snapshot 则只复制数字；后续 mem 消费这些值，不取得块头或页表地址。

**最后追加对象包装与数值快照：**

<!-- append: kernel/mm/memory.c -->
```c
void *airi_kmalloc(size_t bytes) { // 内核小对象入口，不引入宿主libc。
    return airi_heap_alloc(&heap, bytes); // 保留有界堆的NULL失败语义。
}
bool airi_kfree(void *pointer) { // 对象所有者结束最后一个借用后调用。
    return airi_heap_free(&heap, pointer); // 对象空间回堆，物理页不因此归还。
}
void airi_memory_snapshot(struct airi_memory_stats *stats) { // 目前单核不可抢占，可同步复制一致数值。
    stats->pages_total = pages.total; // 固定受管理物理页容量。
    stats->pages_free = pages.free; // 减去页表和堆backing等当前所有者。
    stats->heap_bytes = heap.bytes; // 已映射的总backing字节。
    stats->heap_free = airi_heap_free_bytes(&heap); // 空闲对象payload，不包含块头。
}
```

<a id="memory-consumer"></a>

## 同一次 mem 如何拿到当前状态

console 新增可选 read_memory 回调，参数指向调用者局部快照。main 必须在 memory_init 与 console_init 以后绑定，否则初始化清零会覆盖回调。执行 mem 时同步填数字，返回后再格式化，不保留局部快照地址。

未绑定的主机纯文本模型和早期检查点仍报告没有分配统计，不编造页池数。头文件只引入快照声明，不让文本模型直接链接分配器。

**局部更新 `include/airi/console.h` 的以下窗口：**

在该文件依次找到以下旧窗口，各用对应新文本完整替换；其他内容保留。

<!-- replace: include/airi/console.h -->
<!-- before -->
```c
#ifndef AIRI_CONSOLE_H  // 第一次包含控制台接口时保留以下声明，重复包含时跳过
#define AIRI_CONSOLE_H  // 标记本编译单元已经读取控制台接口
#include <airi/boot.h>  // 引入启动结构、固定宽整数和内存范围接口
#define AIRI_CONSOLE_LINES 256  // 输出环最多保存256条文字记录
```
<!-- after -->
```c
#ifndef AIRI_CONSOLE_H  // 第一次包含控制台接口时保留以下声明，重复包含时跳过
#define AIRI_CONSOLE_H  // 标记本编译单元已经读取控制台接口
#include <airi/memory.h> // 消费稳定内存数值快照，不链接分配器实现。
#include <airi/boot.h>  // 引入启动结构、固定宽整数和内存范围接口
#define AIRI_CONSOLE_LINES 256  // 输出环最多保存256条文字记录
```

继续处理下一处：

<!-- replace: include/airi/console.h -->
<!-- before -->
```c
struct airi_console {  // 独立保存文本输入、历史与输出环。
    bool (*extension)(struct airi_console *, const char *, void *);  // 可选命令消费者；true 表示已经处理，NULL 表示没有扩展。
    void *extension_context;   // 借用扩展拥有者的数据，必须活过回调调用。
    const AiriBootInfo *boot;  // 借用启动信息，描述与内存地图须持续有效
```
<!-- after -->
```c
struct airi_console {  // 独立保存文本输入、历史与输出环。
    bool (*extension)(struct airi_console *, const char *, void *);  // 可选命令消费者；true 表示已经处理，NULL 表示没有扩展。
    void (*read_memory)(struct airi_memory_stats *); // 可选同步查询；宿主纯文本观察可保持NULL。
    void *extension_context;   // 借用扩展拥有者的数据，必须活过回调调用。
    const AiriBootInfo *boot;  // 借用启动信息，描述与内存地图须持续有效
```

原启动分类依然可查，新快照另输出物理池与堆运行状态。mem 不修改块链，显示值也不取代分配器自己的所有权判断。

**局部更新 `kernel/console/console.c` 的以下窗口：**

在该文件依次找到以下旧窗口，各用对应新文本完整替换；其他内容保留。

<!-- replace: kernel/console/console.c -->
<!-- before -->
```c
        airi_console_print(c, "kernel / modules      %llu",  // 把本次查询字段按kernel / modules      %llu的格式保存到输出环
             (unsigned long long)(airi_memory_bytes(c->boot, AIRI_MEMORY_KERNEL) / 1024));  // 字节除1024转KiB，再按%llu约定提供unsigned long long
        airi_console_print(c, "%u firmware regions. No allocator accounting yet.", c->boot->region_count);  // 把本次查询字段按%u firmware regions. No allocator accounting yet.的格式保存到输出环
    } else if (equal(start, "mem map")) {  // 完整匹配mem map时逐条输出实际物理范围
        airi_console_print(c, "ID  PHYSICAL START   PHYSICAL END     TYPE");  // 将ID  PHYSICAL START   PHYSICAL END     TYPE保存为一条命令说明或身份记录
```
<!-- after -->
```c
        airi_console_print(c, "kernel / modules      %llu",  // 把本次查询字段按kernel / modules      %llu的格式保存到输出环
             (unsigned long long)(airi_memory_bytes(c->boot, AIRI_MEMORY_KERNEL) / 1024));  // 字节除1024转KiB，再按%llu约定提供unsigned long long
        if (c->read_memory) { // guest 内存服务初始化后提供真实分配状态。
            struct airi_memory_stats stats; // 本次查询只借用同步填好的数值。
            c->read_memory(&stats); // 回调返回前复制，不借出页池或堆块指针。
            airi_console_print(c, "page pool: total=%u free=%u held=%u", stats.pages_total, stats.pages_free, stats.pages_total - stats.pages_free); // 页表和堆backing都计入held。
            airi_console_print(c, "kernel heap: bytes=%zu free_payload=%zu", stats.heap_bytes, stats.heap_free); // 区分页级backing与对象级空闲。
        } else // 较早检查点与宿主纯模型尚未绑定分配器。
            airi_console_print(c, "%u firmware regions. No allocator accounting yet.", c->boot->region_count); // 未接入就明确不提供伪造统计。
    } else if (equal(start, "mem map")) {  // 完整匹配mem map时逐条输出实际物理范围
        airi_console_print(c, "ID  PHYSICAL START   PHYSICAL END     TYPE");  // 将ID  PHYSICAL START   PHYSICAL END     TYPE保存为一条命令说明或身份记录
```

入口完成所有初始化、绑定快照后发布 AIRI_P2_READY。原 P1 标记继续供既有输入工具使用，但 P2 等待者不能只看到较早那行就判断新增设施已就绪。

**局部更新 `kernel/main.c` 的以下窗口：**

在该文件依次找到以下旧窗口，各用对应新文本完整替换；其他内容保留。

<!-- replace: kernel/main.c -->
<!-- before -->
```c
#include <airi/pages.h> // 在稳定内存地图上建立物理页所有权。
#include <airi/trap.h> // 安装异常表并演示处理器现场。
#include <airi/arch.h>     // 通过架构边界使用忙等提示。
```
<!-- after -->
```c
#include <airi/memory.h> // 通过内存服务建立页池、页表、堆并查询快照。
#include <airi/trap.h> // 安装异常表并演示处理器现场。
#include <airi/arch.h>     // 通过架构边界使用忙等提示。
```

继续处理下一处：

<!-- replace: kernel/main.c -->
<!-- before -->
```c
    airi_memory_init(info); // 只借出完整 usable 页，并在真实 RAM 演示写入与归还。
    airi_console_init(&console, info);  // 初始化内核静态控制台，借用整个启动期间有效的 info。
    bool graphics = airi_console_draw(&console);  // 实际完成首帧后取得布尔结果，不能仅凭描述存在认定成功。
    if (graphics) {  // 有效图形路径才输出图形参数。
```
<!-- after -->
```c
    airi_memory_init(info); // 顺序建立页池、自有映射与固定内核堆，完成本阶段演示。
    airi_console_init(&console, info);  // 初始化内核静态控制台，借用整个启动期间有效的 info。
    console.read_memory = airi_memory_snapshot; // 文本查询只读取数值，不持有分配器内部对象。
    bool graphics = airi_console_draw(&console);  // 实际完成首帧后取得布尔结果，不能仅凭描述存在认定成功。
    if (graphics) {  // 有效图形路径才输出图形参数。
```

继续处理下一处：

<!-- replace: kernel/main.c -->
<!-- before -->
```c
    airi_log("[AIRI_P1_READY] graphics=%u usable_bytes=%llu", graphics ? 1u : 0u,  // 首帧调用结束后才发布完成标记，布尔转换为明确的0/1无符号值。
             (unsigned long long)usable);  // 完成记录附同一实际可用字节数。
    airi_serial_write("airi> ");  // 输出首个串口提示符。
    for (;;) {  // 没有退出条件，内核持续轮询输入。
```
<!-- after -->
```c
    airi_log("[AIRI_P1_READY] graphics=%u usable_bytes=%llu", graphics ? 1u : 0u,  // 首帧调用结束后才发布完成标记，布尔转换为明确的0/1无符号值。
             (unsigned long long)usable);  // 完成记录附同一实际可用字节数。
    airi_log("[AIRI_P2_READY] exceptions=1 pages=1 own_cr3=1 heap=1"); // 所有内存演示和控制台初始化结束后才发布阶段完成。
    airi_serial_write("airi> ");  // 输出首个串口提示符。
    for (;;) {  // 没有退出条件，内核持续轮询输入。
```

<a id="heap-lifetimes"></a>

## 继续使用的第三项，能揭示错误合并

主机先执行相同的三对象合并，再以固定种子交错两千步、维护十六个对象槽。每步核对全部活对象的每个请求字节：重复借出或合并覆盖邻居都会留下变化，不只看当前分配是否成功。最后全部归还，容量恢复；种子使同样顺序可重复。

arena 是主机实际拥有的数组，与 guest 高地址无关，算法在两种已映射空间上共用，不把内核地址交给宿主。完整观察已由代码提供。

**新建 `tests/unit/heap_test.c`：**

<!-- write: tests/unit/heap_test.c -->
```c
#include <assert.h> // 失败直接返回非零，不依赖文字判断。
#include <stdio.h> // 宿主输出测试结果。
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
```

三对象序列已经证明相邻合并能容纳 208，且 c 的尾字节未变。下面用同一主机 arena 交错两千步：每次只改变一个槽，却核对全部活槽的请求字节，才能发现错误分割或合并影响了其他对象。

**继续追加交错生命周期观察与最终回收：**

<!-- append: tests/unit/heap_test.c -->
```c
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
    puts("PASS heap: alignment, overflow, exhaustion, invalid free, coalescing, 2000 lifetime steps"); // 全部实际断言通过。
    return 0; // 正常退出宿主。
}
```

guest 增加纯字节 heap.c，host-test 对同一算法运行 sanitizer。主机没有安装内核映射，所以 backing 建立仍需 guest 路径验证。

**局部更新 `Makefile` 的以下窗口：**

在该文件依次找到以下旧窗口，各用对应新文本完整替换；其他内容保留。

<!-- replace: Makefile -->
<!-- before -->
```make
SOURCES := kernel/main.c kernel/boot/limine.c kernel/boot/info.c \
    kernel/lib/base.c kernel/lib/runtime.c kernel/arch/x86_64/serial.c \
    kernel/console/console.c kernel/arch/x86_64/input.c kernel/arch/x86_64/cpu.c kernel/arch/x86_64/trap.c kernel/mm/pages.c kernel/mm/memory.c kernel/arch/x86_64/paging.c
# 只有 desktop- 前缀目标才读取图形支线自己的构建片段。
ifneq ($(filter desktop-%,$(PROFILE)),)
```
<!-- after -->
```make
SOURCES := kernel/main.c kernel/boot/limine.c kernel/boot/info.c \
    kernel/lib/base.c kernel/lib/runtime.c kernel/arch/x86_64/serial.c \
    kernel/console/console.c kernel/arch/x86_64/input.c kernel/arch/x86_64/cpu.c kernel/arch/x86_64/trap.c kernel/mm/pages.c kernel/mm/memory.c kernel/arch/x86_64/paging.c kernel/mm/heap.c
# 只有 desktop- 前缀目标才读取图形支线自己的构建片段。
ifneq ($(filter desktop-%,$(PROFILE)),)
```

继续处理下一处：

<!-- replace: Makefile -->
<!-- before -->
```make
# 覆盖翻译、共享分支、空表回收和中途耗尽的所有权回滚。
	./build/tests/vm_test
# 清理仅删除可重建产物，保留运行证据artifacts。
clean:
```
<!-- after -->
```make
# 覆盖翻译、共享分支、空表回收和中途耗尽的所有权回滚。
	./build/tests/vm_test
# 在宿主受控backing上验证同一小对象实现，不借用guest地址。
	$(HOST_CC) -std=c11 -g -O1 -Wall -Wextra -Werror -fno-builtin \
	    -fsanitize=address,undefined -Iinclude tests/unit/heap_test.c \
	    kernel/mm/heap.c kernel/lib/base.c -o build/tests/heap_test
# 交错分配与归还时检查所有活对象内容和最终容量。
	./build/tests/heap_test
# 清理仅删除可重建产物，保留运行证据artifacts。
clean:
```

启动检查改等完整 AIRI_P2_READY，避免较早 P1 行已到、P2 行尚未写完就读取字段。故障仍等对应 PANIC，无显示描述也继续保留串口观察；截止与回收不变。

**局部更新 `scripts/boot.py` 的以下窗口：**

在该文件依次找到以下旧窗口，各用对应新文本完整替换；其他内容保留。

<!-- replace: scripts/boot.py -->
<!-- before -->
```python
    case = profile.removeprefix('desktop-')  # 图形前缀不改变故障注入含义。
    graphics = profile.startswith('desktop-') and case != 'no-framebuffer'  # 有绘图实现且设备存在才应生成画面。
    expected='[AIRI_PANIC] required Limine memory map is missing' if case=='missing-memory' else '[AIRI_P1_READY]'  # 缺内存响应要看具体 PANIC，其他配置要看 READY。
    if case == 'trap-fatal': # 未处理异常镜像必须进入具体停止路径。
        expected = '[AIRI_PANIC] unhandled CPU exception' # 不以 QEMU 存活推断故障已处理。
```
<!-- after -->
```python
    case = profile.removeprefix('desktop-')  # 图形前缀不改变故障注入含义。
    graphics = profile.startswith('desktop-') and case != 'no-framebuffer'  # 有绘图实现且设备存在才应生成画面。
    expected='[AIRI_PANIC] required Limine memory map is missing' if case=='missing-memory' else '[AIRI_P2_READY]'  # 正常配置等完整内存阶段，P1标记仍供早期输入观察。
    if case == 'trap-fatal': # 未处理异常镜像必须进入具体停止路径。
        expected = '[AIRI_PANIC] unhandled CPU exception' # 不以 QEMU 存活推断故障已处理。
```

继续处理下一处：

<!-- replace: scripts/boot.py -->
<!-- before -->
```python
                        assert '[AIRI_P1_READY]' not in text  # 错误路径不得出现成功标记。
                    else:  # 按当前profile的另一条完整验证路径处理。
                        assert '[TRAP_DEMO_OK] ud=6 gp=13 resumed=2' in text # 正常启动必须真的从两种异常栈返回。
                        assert '[AIRI_PANIC]' not in text  # 正常完成路径不得混入内核停止记录。
```
<!-- after -->
```python
                        assert '[AIRI_P1_READY]' not in text  # 错误路径不得出现成功标记。
                    else:  # 按当前profile的另一条完整验证路径处理。
                        assert '[AIRI_P2_READY]' in text # 当前主线还必须完成页池、自有页表和堆。
                        assert '[TRAP_DEMO_OK] ud=6 gp=13 resumed=2' in text # 正常启动必须真的从两种异常栈返回。
                        assert '[AIRI_PANIC]' not in text  # 正常完成路径不得混入内核停止记录。
```

UART 增加 mem 的真实统计，PS/2 旧 mem 预期同步为页池输出。查询必须在当前命令的新尾部，不能用启动时打印过的相同数字通过。

**局部更新 `scripts/console_test.py` 的以下窗口：**

在该文件依次找到以下旧窗口，各用对应新文本完整替换；其他内容保留。

<!-- replace: scripts/console_test.py -->
<!-- before -->
```python
                wait(update(0,3),offset)      # 等待第三次真实提交。
                assert 'unknown command' in output()[offset:]  # 不把无消费者的主题设置伪装成功。
                cases=['UART receive / command dispatch','serial-only device report']  # 结果只记录此路径实际运行的两项观察。
            else:  # 按当前profile的另一条完整验证路径处理。
                capture(work/'terminal.ppm')  # READY已到达后保存初始终端画面。
                run('help','mem map    Complete physical address inventory')  # 验证帮助输出包含完整物理地图命令说明。
                run('mem','No allocator accounting yet.')     # 验证实际内存描述，不假装已有分配器占用统计。
                run('unknown','unknown command. Type help.')  # 未知命令必须得到具体错误文本。
                run('echo ready','ready')  # 检查文字参数到输出的正常命令通路。
```
<!-- after -->
```python
                wait(update(0,3),offset)      # 等待第三次真实提交。
                assert 'unknown command' in output()[offset:]  # 不把无消费者的主题设置伪装成功。
                offset=len(output()) # 内存查询使用本次命令之后的新记录。
                wire.sendall(b'mem\n') # 同一串口主线查询真实页池与堆。
                tail=wait(update(0,4),offset) # 等第四次提交的完整状态。
                assert 'page pool: total=' in tail and 'kernel heap: bytes=65536' in tail # 不能只返回启动地图。
                cases=['UART receive / command dispatch','serial-only device report']  # 结果只记录此路径实际运行的两项观察。
            else:  # 按当前profile的另一条完整验证路径处理。
                capture(work/'terminal.ppm')  # READY已到达后保存初始终端画面。
                run('help','mem map    Complete physical address inventory')  # 验证帮助输出包含完整物理地图命令说明。
                run('mem','page pool: total=')     # 验证实际内存描述，不假装已有分配器占用统计。
                run('unknown','unknown command. Type help.')  # 未知命令必须得到具体错误文本。
                run('echo ready','ready')  # 检查文字参数到输出的正常命令通路。
```

用统一入口执行本章主机与 guest 检查，再在交互模式查看同一 mem 快照。

<!-- run -->
```sh
./airios test  # 真正构建全部配置，运行主机边界、五次启动和三种设备输入。
```

应新增 HEAP_DEMO_OK 的 mapped_pages=16、bytes=65536、free_payload=65504、aligned=16、coalesced=1，随后 AIRI_P2_READY。交互 mem 分开显示 page pool 与 kernel heap，held 包括长期页表/backing，不等于用户对象大小。

三对象序列说明相邻空闲合并后可重新容纳 208，同时活着的 32 字节项不变；主机另覆盖内部地址、重复释放、溢出和耗尽。调试分割时看 b/tail/next，合并后看头数量减少，注意 guest 指针与宿主 arena 属于不同进程。

Linux malloc 与 mmap 也在对象和映射的不同层，free 对象不保证 RSS 立刻下降。这里还不是完整 libc malloc，没有动态扩大/收缩、用户堆、并发分配和 backing 回收策略。接下来的 Linux 对照将分开观察虚拟范围与实际触碰，再继续时钟和任务。
