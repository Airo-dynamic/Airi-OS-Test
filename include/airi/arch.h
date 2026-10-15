#ifndef AIRI_ARCH_H  // 避免重复声明架构操作。
#define AIRI_ARCH_H  // 标记本头文件已展开。
void airi_arch_relax(void);  // 在忙等的一次迭代间提示处理器；不是任务睡眠。
#endif  // 结束重复包含保护。
