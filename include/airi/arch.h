#ifndef AIRI_ARCH_H  // 避免重复声明架构操作。
#define AIRI_ARCH_H  // 标记本头文件已展开。
void airi_arch_relax(void);  // 在忙等的一次迭代间提示处理器；不是任务睡眠。
void airi_arch_idle(void); // 固定平台开IRQ并等待一次真实中断，调用者醒后重查条件。
long airi_arch_user_gate(void); // 进入当前用户切换门，返回可为暂回服务或最终结束，由进程层区分。
int airi_arch_serial_read(void); // UART当前一字节或-1无数据，只用于独占串口会话。
#endif  // 结束重复包含保护。
