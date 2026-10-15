#include <assert.h> // 作者的地址转换与回滚检查。
#include <stdio.h> // 只在宿主输出结果。
#include <airi/vm.h> // 使用真实的四级表算法。
static uint8_t arena[16 * 4096] __attribute__((aligned(4096))); // 模拟十六个物理页的内容，绝不装入宿主 CR3。
static struct airi_page_pool pool; // 分配器所有权状态与 guest 实现相同。
int main(void) { // 宿主仅观察内存中的表和资源变化。
    uint64_t offset = (uintptr_t)arena - 4096; // 模拟 PA=4096 对应 arena 起点的直接映射。
    AiriBootInfo info = {.magic=AIRI_BOOT_MAGIC, .version=AIRI_BOOT_VERSION, .size=sizeof(info), .direct_map_offset=offset, .kernel_physical_base=0x900000, .kernel_virtual_base=0xffffffff80000000ull, .kernel_size=4096, .region_count=1}; // 内核范围不在模拟 arena 中。
    info.regions[0] = (struct airi_memory_region){4096, sizeof(arena), AIRI_MEMORY_USABLE, 0}; // 只允许借出 arena 真正能承载的页。
    assert(airi_pages_init(&pool, &info) && pool.free == 16); // 所有可表示页面初始空闲。
    struct airi_address_space space = {0}; // 尚未拥有根页。
    assert(airi_vm_create(&space, &pool, offset)); // 根页消耗一个物理页。
    uint64_t data = airi_page_alloc(&pool); // 模拟数据页仍由调用者持有。
    assert(data && pool.free == 14); // 根与数据各一页。
    assert(airi_vm_map(&space, AIRI_VM_DEMO_ADDRESS, data, AIRI_VM_WRITE)); // 首次映射需另外三张中间表。
    assert(pool.free == 11); // 共五页被根、数据和三层表持有。
    uint64_t translated = 0; // 接收带页内偏移的翻译结果。
    unsigned flags = 0; // 接收叶子权限。
    assert(airi_vm_lookup(&space, AIRI_VM_DEMO_ADDRESS + 57, &translated, &flags)); // 软件查询允许页内地址。
    assert(translated == data + 57 && flags == AIRI_VM_WRITE); // 保留偏移，默认 NX。
    assert(!airi_vm_map(&space, AIRI_VM_DEMO_ADDRESS, data, 0) && pool.free == 11); // 拒绝覆盖，不消耗额外页。
    assert(!airi_vm_map(&space, AIRI_VM_DEMO_ADDRESS + 1, data, 0)); // 拒绝未对齐 VA。
    assert(!airi_vm_map(&space, 0x0000800000000000ull, data, 0)); // 拒绝不规范 48 位地址。
    assert(!airi_vm_map(&space, AIRI_VM_DEMO_ADDRESS + 4096, data + 1, 0)); // 拒绝未对齐 PA。
    assert(!airi_vm_map(&space, AIRI_VM_DEMO_ADDRESS + 4096, data, 8)); // 拒绝接口未声明的权限。
    assert(airi_vm_map(&space, AIRI_VM_DEMO_ADDRESS + 4096, data, AIRI_VM_EXEC)); // 相邻映射共享所有中间表，演示允许受控物理别名。
    assert(pool.free == 11 && airi_vm_unmap(&space, AIRI_VM_DEMO_ADDRESS)); // 解除第一页不能回收还有第二页的表。
    assert(pool.free == 11 && !airi_vm_lookup(&space, AIRI_VM_DEMO_ADDRESS, NULL, NULL)); // 原叶子必须消失。
    assert(airi_vm_lookup(&space, AIRI_VM_DEMO_ADDRESS + 4096, &translated, &flags)); // 相邻映射仍在。
    assert(translated == data && flags == AIRI_VM_EXEC); // 第二叶子独立拥有自己的权限。
    assert(airi_vm_unmap(&space, AIRI_VM_DEMO_ADDRESS + 4096) && pool.free == 14); // 最后一个叶子移除，三张空表全部归还。
    assert(!airi_vm_unmap(&space, AIRI_VM_DEMO_ADDRESS + 4096)); // 再次解除不存在映射必须失败。
    assert(airi_page_free(&pool, data) && pool.free == 15); // 只有调用者归还数据页，根仍由地址空间持有。
    uint64_t held[13]; // 借走剩余空间，只留两页，构造第三张表分配失败。
    for (unsigned i = 0; i < 13; ++i) // 有界准备明确的耗尽环境。
        held[i] = airi_page_alloc(&pool); // 这些页面暂由测试持有。
    assert(pool.free == 2); // 映射独立分支需要三页，因此必须失败。
    assert(!airi_vm_map(&space, AIRI_VM_DEMO_ADDRESS, held[0], 0)); // 前两层成功、第三层失败后应回滚。
    assert(pool.free == 2 && !airi_vm_lookup(&space, AIRI_VM_DEMO_ADDRESS, NULL, NULL)); // 回滚既不泄漏，也不留下半条路径。
    uint64_t *root = (uint64_t *)(uintptr_t)(offset + space.root); // 模拟直接映射指向真实 arena 内存。
    for (unsigned i = 0; i < 512; ++i) // 本测试最终没有任何叶子，根应恢复全零。
        assert(root[i] == 0); // 不留下指向已释放表的父链接。
    for (unsigned i = 0; i < 13; ++i) // 归还所有测试借用。
        assert(held[i] && airi_page_free(&pool, held[i])); // 每个页面必须仍属于原调用者。
    data = airi_page_alloc(&pool); // 新测试的数据仍由调用者拥有。
    assert(data && airi_vm_map(&space, AIRI_VM_DEMO_ADDRESS, data, AIRI_VM_WRITE)); // 建立要借给用户根的内核高半区路径。
    struct airi_address_space child = {0}; // 用户空间尚无任何所有权。
    assert(airi_vm_clone_kernel(&child, &space)); // 只借高半区中间表，单独拥有根。
    assert(airi_vm_map(&child, 0x400000, data, AIRI_VM_USER | AIRI_VM_EXEC)); // 自有低半区四层允许USER。
    assert(airi_vm_lookup(&child, 0x400000, &translated, &flags) && (flags & AIRI_VM_USER)); // 叶子报告用户权限。
    assert(!airi_vm_map(&child, AIRI_VM_DEMO_ADDRESS + 4096, data, AIRI_VM_WRITE)); // 不允许子根覆盖借用内核分支。
    assert(!airi_vm_unmap(&child, AIRI_VM_DEMO_ADDRESS)); // 也不能解除共享内核映射。
    assert(!airi_vm_map(&space, AIRI_VM_DEMO_ADDRESS + 4096, data, AIRI_VM_USER)); // 高半区不能被标成USER。
    assert(airi_vm_destroy(&child) && !airi_vm_destroy(&child)); // 只回收用户自有表，第二次拒绝。
    assert(airi_vm_lookup(&space, AIRI_VM_DEMO_ADDRESS, &translated, NULL) && translated == data); // 销毁用户根不破坏原内核表。
    assert(airi_vm_unmap(&space, AIRI_VM_DEMO_ADDRESS) && airi_page_free(&pool, data)); // 内核所有者最后自己结束高半区映射和数据页。
    assert(pool.free == 15); // 根仍被内核持有，无其他泄漏。
    assert(!airi_vm_activate(&space)); // 宿主版本明确不执行特权安装，不能记成硬件验证。
    assert(airi_vm_destroy(&space) && pool.free == 16); // 空根不再使用后，测试手动销毁最后一个所有权。
    puts("PASS vm: translation, permissions, shared tables, unmap, exhaustion rollback"); // 全部状态核对通过才打印。
    return 0; // 正常退出宿主观察。
}
