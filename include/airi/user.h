#ifndef AIRI_USER_H // 用户态边界声明一次。
#define AIRI_USER_H // 包含保护。
#include <airi/trap.h> // 系统调用和用户故障使用完整CPU现场。
void airi_user_demo(void); // 顺序运行正常、非法指令、内核读取和坏栈四个ring3演示并回收。
struct airi_trap_frame *airi_user_trap(struct airi_trap_frame *frame); // 允许返回另一条完整中断调用链。
void airi_user_tick(void); // 当前单用户上下文的真实时钟计数，尚不调度多个进程。
struct airi_trap_frame *airi_user_resume(struct airi_trap_frame *frame); // 检查用户返回地址并清理不能交给iretq的标志。
#endif // 结束包含保护。
