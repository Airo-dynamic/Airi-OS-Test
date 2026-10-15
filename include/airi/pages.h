#ifndef AIRI_PAGES_H // 物理页接口只声明一次。
#define AIRI_PAGES_H // 包含保护标记。
#include <airi/boot.h> // 分配器只消费稳定启动地图，不包含 Limine 协议。
#define AIRI_PAGE_SIZE 4096ull // 一个页帧的字节数。
#define AIRI_PHYSICAL_LIMIT (1024ull * 1024 * 1024) // 首版只管理低 1 GiB，固定目标内存为 256 MiB。
#define AIRI_PAGE_COUNT (AIRI_PHYSICAL_LIMIT / AIRI_PAGE_SIZE) // 位图可表示的页帧总数。
struct airi_page_pool { // 两张位图区分“允许管理”和“本分配器当前已借出”。
    uint8_t managed[AIRI_PAGE_COUNT / 8]; // 一位对应一个完整可用页帧。
    uint8_t allocated[AIRI_PAGE_COUNT / 8]; // 只对 managed 中的页面记借出状态。
    uint32_t total, free, hint; // 管理总页数、空闲页数和下一次扫描起点。
};
bool airi_pages_init(struct airi_page_pool *pool, const AiriBootInfo *info); // 初始化只接收有效地图，不回收加载器区域。
uint64_t airi_page_alloc(struct airi_page_pool *pool); // 返回物理页起点，耗尽返回 0；物理页 0 永不借出。
bool airi_page_free(struct airi_page_pool *pool, uint64_t physical); // 对齐、归属和借出状态均有效才释放。
void airi_memory_init(const AiriBootInfo *info); // 在真实 guest 上建立页池并演示分配、写入和回收。
#endif // 结束包含保护。
