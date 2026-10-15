#include <airi/arch.h>  // 通用循环只通过此声明使用处理器提示。
void airi_arch_relax(void) {  // 本实现只用于当前 x86-64 目标。
    __asm__ volatile("pause");  // 告知 x86 处理器正在忙等，不开启中断或切换任务。
}
