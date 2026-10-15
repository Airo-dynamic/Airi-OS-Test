#include <airi/base.h> // 使用自己的字节清零实现。
#include <airi/pages.h> // 物理页状态与固定管理范围。
static bool bit(const uint8_t *map, uint64_t index) { // 读取某个页帧对应的一位。
    return (map[index / 8] & (1u << (index % 8))) != 0; // 商定位字节，余数定位字节中的位。
}
static void change(uint8_t *map, uint64_t index, bool value) { // 只修改当前页帧的一位。
    uint8_t mask = (uint8_t)(1u << (index % 8)); // 页号的低三位选择一个位掩码。
    if (value) // 借出或纳入管理时置位。
        map[index / 8] |= mask; // 按位或保留同字节其他页面的状态。
    else // 释放或排除范围时清位。
        map[index / 8] &= (uint8_t)~mask; // 取反掩码只清目标位。
}
bool airi_pages_init(struct airi_page_pool *pool, const AiriBootInfo *info) { // 调用者持有 pool 的全部寿命。
    if (!pool || airi_boot_validate(info)) // 无效地图不参与任何地址计算。
        return false; // 不把损坏地图解释成可分配物理内存。
    airi_memset(pool, 0, sizeof(*pool)); // 初始所有页都不可管理且未借出。
    for (uint32_t i = 0; i < info->region_count; ++i) { // 按已验证的有序非重叠地图检查区域。
        const struct airi_memory_region *r = &info->regions[i]; // 只借用描述，不修改 BootInfo。
        if (r->kind != AIRI_MEMORY_USABLE || r->base >= AIRI_PHYSICAL_LIMIT) // ACPI、内核、设备和加载器区域均保持保留。
            continue; // 高于管理上限的 RAM 当前也不纳入页池。
        uint64_t end = r->base + r->length; // boot_validate 已排除加法溢出。
        if (end > AIRI_PHYSICAL_LIMIT) // 固定容量不能被高地址范围越界索引。
            end = AIRI_PHYSICAL_LIMIT; // 截到位图可表示的半开右端。
        uint64_t first = (r->base + AIRI_PAGE_SIZE - 1) / AIRI_PAGE_SIZE; // 起点向上取整，放弃不完整的第一页。
        uint64_t last = end / AIRI_PAGE_SIZE; // 右端向下取整，放弃不完整的最后一页。
        for (uint64_t page = first; page < last; ++page) // 只标记完全落入可用区的页。
            change(pool->managed, page, true); // 此时还没有任何页面借出。
    }
    uint64_t kernel_end = info->kernel_physical_base + info->kernel_size; // 已验证的内核物理半开区间。
    for (uint64_t page = 0; page < AIRI_PAGE_COUNT; ++page) { // 一次扫描建立准确统计，同时额外保护内核范围。
        uint64_t start = page * AIRI_PAGE_SIZE; // 页号转换回物理起点。
        if (!page || (start < kernel_end && start + AIRI_PAGE_SIZE > info->kernel_physical_base)) // 页零作失败哨兵；任何与内核相交的页都保留。
            change(pool->managed, page, false); // 即使错误地把内核标成 usable，也不交给调用者。
        if (bit(pool->managed, page)) // 只有实际可管理页进入统计。
            ++pool->total; // 初始全部可管理页都还空闲。
    }
    pool->free = pool->total; // total 是容量，free 随借出和归还变化。
    pool->hint = 1; // 从页一开始查找，页零永不分配。
    return pool->total != 0; // 无完整可用页时明确拒绝启动后续机制。
}
uint64_t airi_page_alloc(struct airi_page_pool *pool) { // 单核、不可抢占内核下串行修改位图。
    if (!pool || !pool->free) // 空池或耗尽不扫描、不修改统计。
        return 0; // 0 明确表示未取得任何页面。
    for (uint64_t n = 0; n < AIRI_PAGE_COUNT; ++n) { // 最多一圈，避免失配状态产生无限循环。
        uint64_t page = (pool->hint + n) % AIRI_PAGE_COUNT; // 越过上界后从零继续，位图索引总在范围内。
        if (!bit(pool->managed, page) || bit(pool->allocated, page)) // 保留或已借出的页面都不能再次借出。
            continue; // 检查下一个候选页。
        change(pool->allocated, page, true); // 在交出地址前先记录所有权。
        --pool->free; // 一次成功恰好减少一个空闲页。
        pool->hint = (uint32_t)((page + 1) % AIRI_PAGE_COUNT); // 下一次优先从当前页之后扫描。
        return page * AIRI_PAGE_SIZE; // 返回物理地址，不是可直接解引用的 C 指针。
    }
    return 0; // 统计失配也不得返回未拥有的页面。
}
bool airi_page_free(struct airi_page_pool *pool, uint64_t physical) { // 调用者必须先结束所有借用与映射，再归还页。
    if (!pool || !physical || physical >= AIRI_PHYSICAL_LIMIT || physical % AIRI_PAGE_SIZE) // 拒绝空哨兵、越界和页内地址。
        return false; // 不让无效释放改变位图。
    uint64_t page = physical / AIRI_PAGE_SIZE; // 对齐地址换成唯一页号。
    if (!bit(pool->managed, page) || !bit(pool->allocated, page)) // 拒绝保留页和重复释放。
        return false; // 不增加空闲统计，也不让别人的页面进入空闲池。
    change(pool->allocated, page, false); // 页从已借出回到可借出。
    ++pool->free; // 恰好归还一个页面。
    if (page < pool->hint) // 下次可以尽早复用刚归还的低地址页。
        pool->hint = (uint32_t)page; // 缩短下一次扫描，不代表页面内容已清零。
    return true; // 状态改变后才报告归还成功。
}
