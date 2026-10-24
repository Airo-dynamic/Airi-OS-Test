#ifndef AIRI_IRQ_H // 当前q35的PCI中断接线与PIC确认边界。
#define AIRI_IRQ_H // 包含保护。
#include <stdbool.h> // 路由校验明确成功或拒绝。
#define AIRI_PCI_IRQ 11u // 当前固定平台只给一个VirtIO设备分配IRQ11，向量为43。
bool airi_irq_route_pci(unsigned slot, unsigned pin); // bus0普通根槽、INTA-D，配置q35 PIRQ到IRQ11，不在此开放PIC。
void airi_irq_mask(unsigned irq, bool masked); // 修改本CPU平台PIC屏蔽位，开放从片时同时开放级联IRQ2。
void airi_irq_end(unsigned irq); // 设备先撤销来源，再按从片、主片顺序确认。
#endif // 结束包含保护。
