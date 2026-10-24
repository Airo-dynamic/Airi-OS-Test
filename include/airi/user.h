#ifndef AIRI_USER_H // 用户态边界声明一次。
#define AIRI_USER_H // 包含保护。
#include <stdbool.h> // 系统命令回调明确报告是否处理。
#include <airi/trap.h> // 系统调用和用户故障使用完整CPU现场。
void airi_user_init(void); // 初始化用户文件资源，教学演示与服务启动共用。
int airi_user_boot(const char *name); // 内核启动固定目录中的根用户，返回仅表示该持续会话已经停止。
void airi_user_demo(void); // 运行受控隔离演示与独立C用户ELF，结束后回收。
struct airi_trap_frame *airi_user_trap(struct airi_trap_frame *frame); // 允许返回另一条完整中断调用链。
struct airi_trap_frame *airi_user_device_irq(struct airi_trap_frame *frame); // 真实设备提示可把用户现场交给普通服务栈，不在IRQ复制结果。
struct airi_trap_frame *airi_user_timer(struct airi_trap_frame *frame); // 用户时间片在完整现场边界轮转，内核仍不可抢占。
struct airi_trap_frame *airi_user_resume(struct airi_trap_frame *frame); // 检查用户返回地址并清理不能交给iretq的标志。
struct airi_console; // 只借用文本会话，不依赖图形状态。
bool airi_user_command(struct airi_console *console, const char *command); // programs与run命令使用实际用户服务。
#endif // 结束包含保护。
