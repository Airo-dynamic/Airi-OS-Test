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
