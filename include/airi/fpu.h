#ifndef AIRI_FPU_H // 当前x86用户浮点/SSE上下文边界。
#define AIRI_FPU_H // 包含保护。
#include <airi/base.h> // 固定宽度、字节复制与长度。
#include <stdbool.h> // 初始化能力检查明确成功或拒绝。
#define AIRI_FPU_BYTES 512u // FXSAVE64固定状态区，至少16字节对齐，不含AVX扩展状态。
bool airi_fpu_init(void); // 仅启动时开启x87/SSE2并建立不含旧数据的初始状态。
void airi_fpu_new(void *state); // 复制初始模板，目标由调用者拥有且对齐。
void airi_fpu_save(void *state); // 完整用户陷入后保存当前硬件状态，内核C不使用这些寄存器。
void airi_fpu_restore(const void *state); // 用户返回前恢复目标进程状态，不接受用户提供的状态指针。
#endif // 结束保护。
