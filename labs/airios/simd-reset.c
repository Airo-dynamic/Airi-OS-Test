#include <airi_libc.h> // 此main保持general-only，先观察入口再调用任何用户库函数。
int main(void) { // 来自成功exec，也可单独从目录启动。
    _Alignas(16) unsigned char state[512]; // 用户自己的快照数组，不是内核持有的状态页。
    __asm__ volatile("fxsave64 %0" : "=m"(state) :: "memory"); // 第一项实际状态操作，捕获新ELF的默认硬件环境。
    if (state[0] != 0x7f || state[1] != 3 || state[2] || state[3] || state[4]) // x87控制0x037f、状态和空标签应为默认值。
        return 181; // 不继承孩子旧的非空x87栈或向下舍入控制。
    if (state[24] != 0x80 || state[25] != 0x1f || state[26] || state[27]) // MXCSR默认0x1f80。
        return 182; // 旧0x3f80环境不能泄漏到新映像。
    for (unsigned i = 6; i < 24; ++i) // 新程序不继承旧FOP或指令/数据指针。
        if (state[i]) // 这些字段也可能含旧映像地址。
            return 183; // 默认模板需要明确清理。
    for (unsigned slot = 0; slot < 8; ++slot) // 空标签不代表寄存器数据自动清零。
        for (unsigned byte = 0; byte < 10; ++byte) // 只检查实际80位值，六个保留字节不作硬件承诺。
            if (state[32 + slot * 16 + byte]) // 八个x87数据寄存器应全部为零。
                return 184; // 不能让旧值藏在空标签后面。
    for (unsigned i = 160; i < 416; ++i) // 64位模式完整XMM0至15，共256字节。
        if (state[i]) // 当前main尚未有编译器生成的SSE指令覆盖它们。
            return 185; // 高组与低组都不能沿用旧用户模式。
    airi_puts("[SIMD_EXEC_RESET_OK] x87=empty-zero xmm=zero mxcsr=default pointers=zero\n"); // 快照之后才允许普通用户库运行。
    return 0; // 原父仍wait同一PID。
}
