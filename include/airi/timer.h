#ifndef AIRI_TIMER_H // 时钟接口只声明一次。
#define AIRI_TIMER_H // 包含保护。
#include <stdint.h> // tick计数使用64位无符号值。
void airi_timer_init(void); // 固定PC平台：重映射PIC、配置PIT并只开放IRQ0。
uint64_t airi_timer_ticks(void); // 自启动以来实际处理过的IRQ0次数。
void airi_timer_interrupt(void); // 只供架构异常分派调用，先计数再确认中断。
void airi_timer_demo(void); // 等待真实时钟中断，证明halt后能继续执行。
#endif // 结束包含保护。
