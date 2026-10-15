#include <airi/arch.h>  // 通用循环只通过此声明使用处理器提示。
void airi_arch_relax(void) {  // 本实现只用于当前 x86-64 目标。
    __asm__ volatile("pause");  // 告知 x86 处理器正在忙等，不开启中断或切换任务。
}

void airi_arch_idle(void) { // x86等待细节留在架构层，通用服务不直接散布sti/hlt。
    __asm__ volatile("sti; hlt" ::: "memory"); // 开IRQ后等待PIT，返回仍允许IRQ，CS0不被用户抢占。
}
long airi_arch_user_gate(void) { // 调用期间本函数的启动栈现场保持，暂回后由调用者判状态。
    long result; // RAX接门返回数值。
    __asm__ volatile("int $0x81" : "=a"(result) :: "memory", "cc"); // 自有DPL0调度门，非用户系统调用ABI。
    return result; // 下一次恢复用户前会重新调用，登记新的继续现场。
}
