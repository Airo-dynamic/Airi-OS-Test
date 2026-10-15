#include <assert.h> // 失败状态交给 Make，不依赖输出文字猜测成功。
#include <stdio.h> // 主机检查可使用宿主输出，guest 实现不链接它。
#include <airi/pages.h> // 使用同一页分配器实现。
static struct airi_page_pool pool; // 大位图不放进小型测试栈。
int main(void) { // 主机只验证地址与所有权算法，不把物理地址当宿主指针。
    AiriBootInfo info = {.magic=AIRI_BOOT_MAGIC, .version=AIRI_BOOT_VERSION, .size=sizeof(info), .direct_map_offset=0xffff800000000000ull, .kernel_physical_base=0x2000, .kernel_virtual_base=0xffffffff80000000ull, .kernel_size=4096, .region_count=4}; // 构造可验证的启动描述。
    info.regions[0] = (struct airi_memory_region){0, 0x9000, AIRI_MEMORY_USABLE, 0}; // 内含页零和故意误标的内核页，均须额外排除。
    info.regions[1] = (struct airi_memory_region){0x9000, 0x7000, AIRI_MEMORY_RESERVED, 0}; // 中间保留范围不能分配。
    info.regions[2] = (struct airi_memory_region){0x10003, 0x3ffd, AIRI_MEMORY_USABLE, 0}; // 向内取整后只有 0x11000、0x12000、0x13000 三页。
    info.regions[3] = (struct airi_memory_region){AIRI_PHYSICAL_LIMIT, 4096, AIRI_MEMORY_USABLE, 0}; // 超过固定管理上限的页不纳入位图。
    assert(airi_pages_init(&pool, &info)); // 正常地图应形成非空页池。
    assert(pool.total == 10 && pool.free == 10); // 第一段七页，第三段三页。
    uint64_t values[10]; // 保留本轮全部借出地址，以核对唯一性和最终回收。
    for (unsigned i = 0; i < 10; ++i) { // 借出直到耗尽，而不是只试一次成功。
        values[i] = airi_page_alloc(&pool); // 调用真实分配器，不用替身。
        assert(values[i] && values[i] != 0x2000 && values[i] % 4096 == 0); // 页零、内核和未对齐地址均不能返回。
        assert(values[i] < 0x9000 || (values[i] >= 0x11000 && values[i] < 0x14000)); // 不得借出保留或不完整页面。
        for (unsigned j = 0; j < i; ++j) // 与所有仍借出的页比较。
            assert(values[i] != values[j]); // 不能同时把同一页交给两个所有者。
    }
    assert(pool.free == 0 && airi_page_alloc(&pool) == 0); // 耗尽必须返回失败且保持统计。
    assert(!airi_page_free(&pool, 0) && !airi_page_free(&pool, 0x2000)); // 页零和内核不能伪装成有效释放。
    assert(!airi_page_free(&pool, 0x9000) && !airi_page_free(&pool, AIRI_PHYSICAL_LIMIT)); // 保留范围和上界均被拒绝。
    assert(!airi_page_free(&pool, values[0] + 1) && pool.free == 0); // 未对齐释放不改变原页状态。
    for (unsigned i = 0; i < 10; ++i) // 归还每一个真实借出的页。
        assert(airi_page_free(&pool, values[i])); // 正确释放必须成功。
    assert(pool.free == pool.total && !airi_page_free(&pool, values[0])); // 全部收回后重复释放仍失败。
    info.regions[1].base = 0x8000; // 作者完整演示重叠地图的拒绝。
    assert(!airi_pages_init(&pool, &info)); // 无效地图不能重新建立可分配范围。
    puts("PASS pages: inward rounding, ownership, exhaustion, reuse, invalid maps"); // 实际全部断言通过后输出。
    return 0; // 正常返回主机。
}
