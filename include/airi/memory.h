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
