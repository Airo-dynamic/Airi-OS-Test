#ifndef AIRI_VM_H // 地址空间接口只展开一次。
#define AIRI_VM_H // 包含保护标记。
#include <airi/pages.h> // 页表页从物理页池借出。
#define AIRI_VM_WRITE 1u // 显式可写；不设置则只读。
#define AIRI_VM_EXEC 2u // 显式可执行；不设置则 NX。
#define AIRI_VM_DEVICE 4u // 设备映射禁用普通缓存，不用于 RAM。
#define AIRI_VM_USER 8u // 用户映射必须位于低半区，四层均允许用户访问。
#define AIRI_USER_LIMIT 0x0000800000000000ull // 48位规范地址的低半区右端。
#define AIRI_VM_DEMO_ADDRESS 0xffffc00000000000ull // 演示使用的独立高半区地址。
struct airi_address_space { // 根页物理地址与访问页表页所需的现有直接映射。
    uint64_t root, direct_offset; // CR3 需要 root，C 遍历页表需要 direct_offset。
    bool borrowed_kernel; // true时根的高半区只借用内核表，不能映射、解除或回收它。
    struct airi_page_pool *pages; // 页表拥有者借用的页池，必须比地址空间存活更久。
};
bool airi_vm_create(struct airi_address_space *space, struct airi_page_pool *pages, uint64_t direct_offset); // 借出并清零根页。
bool airi_vm_map(struct airi_address_space *space, uint64_t virtual_address, uint64_t physical, unsigned flags); // 创建一个 4 KiB 映射，拒绝覆盖与无效地址。
bool airi_vm_unmap(struct airi_address_space *space, uint64_t virtual_address); // 解除一个映射，回收空页表，不归还数据页。
bool airi_vm_lookup(const struct airi_address_space *space, uint64_t virtual_address, uint64_t *physical, unsigned *flags); // 查询页内地址，不存在返回 false。
bool airi_vm_activate(const struct airi_address_space *space); // x86 启用 NX/WP 并装 CR3；主机算法观察不执行此动作。
bool airi_vm_clone_kernel(struct airi_address_space *space, const struct airi_address_space *kernel); // 新根只复制高半区链接，低半区从空开始。
bool airi_vm_destroy(struct airi_address_space *space); // 仅销毁非活动空间拥有的表，数据页仍归调用者。
#endif // 结束包含保护。
