#ifndef AIRI_TRAP_H // 异常接口只展开一次。
#define AIRI_TRAP_H // 标记已经声明。
#include <airi/base.h> // 固定宽度整数和布尔值来自基础库。
struct airi_trap_frame { // 顺序严格对应 traps.S 的压栈次序。
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8; // 最后压入的寄存器位于最低地址。
    uint64_t rsi, rdi, rbp, rdx, rcx, rbx, rax; // C 调用前保存全部通用寄存器。
    uint64_t vector, error; // 汇编为无硬件错误码的异常补零，统一这两个位置。
    uint64_t rip, cs, rflags, rsp, ss; // 64 位模式由 CPU 压入的返回现场。
};
void airi_trap_init(void); // 安装自己的 GDT、TSS 和 IDT，保持可屏蔽中断关闭。
void airi_trap_dispatch(struct airi_trap_frame *frame); // 仅供汇编入口调用。
void airi_trap_demo(void); // 完整演示可恢复的 UD2 与带错误码的 GP。
_Noreturn void airi_trap_fatal_demo(void); // 演示未登记故障的停止路径。
#endif // 结束包含保护。
