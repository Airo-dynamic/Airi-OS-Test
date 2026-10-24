#include <airi/irq.h> // 固定q35路由留在平台层，块接口不暴露芯片寄存器。
#include <airi/x86_io.h> // 端口访问与原IF保存恢复。
static uint32_t lpc_read(unsigned offset) { // 当前ICH9 LPC固定在bus0:31.0。
    airi_out32(0xcf8, 0x80000000u | 31u << 11 | (offset & 0xfcu)); // 选择芯片PCI配置双字。
    return airi_in32(0xcfc); // 只读取配置，不解释成普通内存地址。
}
bool airi_irq_route_pci(unsigned slot, unsigned pin) { // 针对锁定QEMU q35普通根槽的实际连线，不冒充通用ACPI路由。
    if (slot >= 25 || pin < 1 || pin > 4) // 特殊片上设备与桥后设备的路由公式不同，本阶段拒绝。
        return false; // 不修改其他平台配置。
    uint64_t flags = airi_irq_save(); // PCI选择/数据与ELCR修改在本CPU不可打断。
    if (lpc_read(0) != 0x29188086u) { // 确认是QEMU ICH9 LPC，而非把其他桥寄存器当作同一布局。
        airi_irq_restore(flags); // 拒绝也保持原IF。
        return false; // 真实开发板需按其芯片和ACPI单独接入。
    }
    unsigned pirq = (slot + pin - 1) % 4 + 4; // QEMU普通根槽的INTA-D接到PIRQ E-H，pin从1转成0基数。
    unsigned offset = 0x68 + pirq - 4; // ICH9的PIRQ E-H路由字节位于0x68至0x6b。
    (void)lpc_read(offset); // 选择包含该路由字节的双字，其他字节保持原值。
    airi_out8((uint16_t)(0xcfc + (offset & 3)), AIRI_PCI_IRQ); // 清禁用位并只将本来源接到IRQ11。
    bool routed = ((lpc_read(offset) >> ((offset & 3) * 8)) & 0x8fu) == AIRI_PCI_IRQ; // 读回路由和禁用位，不能只假定写入生效。
    if (routed) // 只有已确认的PCI电平来源才修改ELCR。
        airi_out8(0x4d1, airi_in8(0x4d1) | (1u << (AIRI_PCI_IRQ - 8))); // IRQ11改为电平触发，其他IRQ类型保持。
    airi_irq_restore(flags); // PIC仍屏蔽此设备，调用者准备完成后才开放。
    return routed; // 块驱动决定路由失败是否拒绝启动。
}
void airi_irq_mask(unsigned irq, bool masked) { // 当前驱动只传0至15的合法PIC输入。
    if (irq >= 16) // 不将未知编号截断成另一路设备。
        return; // 无状态改变。
    uint64_t flags = airi_irq_save(); // 屏蔽位的读改写不能丢掉另一调用的位。
    uint16_t port = irq < 8 ? 0x21 : 0xa1; // IRQ8-15属于从PIC。
    uint8_t bit = (uint8_t)(1u << (irq & 7)); // 片内输入编号。
    uint8_t mask = airi_in8(port); // 保留时钟和其他设备当前屏蔽策略。
    airi_out8(port, masked ? mask | bit : mask & (uint8_t)~bit); // 只改变目标输入。
    if (!masked && irq >= 8) // 从PIC的中断必须还能通过主PIC IRQ2。
        airi_out8(0x21, airi_in8(0x21) & (uint8_t)~4u); // 级联开放不改变PIT IRQ0。
    airi_irq_restore(flags); // 系统调用原IF0不能被无条件打开。
}
void airi_irq_end(unsigned irq) { // 调用时设备ISR已读取，来源电平先撤销。
    if (irq >= 8) // 从片发来的IRQ先结束从片服务状态。
        airi_out8(0xa0, 0x20); // 非特定EOI，当前没有嵌套的从片处理。
    airi_out8(0x20, 0x20); // 最后结束主片的目标或级联服务状态。
}
