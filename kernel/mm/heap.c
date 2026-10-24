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
