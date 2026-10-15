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
