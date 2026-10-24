#include <airi_libc.h> // 进程、时间与输出仍用自有系统调用。
extern long airi_simd_fork_probe(void); // 汇编在fork前后直接核对活XMM、x87和控制环境。
extern long airi_simd_bad_exec(void); // 坏ELF返回旧状态，不能借C ABI假定XMM跨普通函数调用保持。
extern int airi_simd_spin(uint64_t pattern, unsigned loops); // 纯用户循环内保留活寄存器，PIT负责真实抢占。
int main(void) { // 普通用户C现在可以生成SSE2指令。
    long child = airi_simd_fork_probe(); // 子不只是复制GPR和内存，还应继承已保存的CPU状态。
    if (child < 0) // 内核资源错误与探针错误都不能作为有效身份继续。
        return 171; // shell保留普通程序失败结果。
    uint64_t pattern = child ? 0x3ff4000000000000ull : 0x4004000000000001ull; // 父子采用不同有限double模式，低位还决定不同RC环境。
    uint64_t start = airi_ticks(); // 观察真实全局PIT窗口，不把空循环步数冒充时间。
    do { // 每轮在汇编中跨任意中断保持XMM15、x87与两种控制环境。
        if (airi_simd_spin(pattern, 5000000)) // 循环没有yield或系统调用来制造切换。
            return 172; // 任一实际状态串扰均失败。
    } while (airi_ticks() - start < 20); // 至少二十个真实tick，父子都在可运行集合中竞争。
    if (airi_simd_bad_exec() != -8) // 失败exec必须保留旧非默认环境和原返回结果。
        return 173; // 不把坏状态当成功恢复。
    volatile double input = 3.25, scale = 2.0; // volatile使C编译器真正加载操作数，不能全常量折叠。
    double result = input * scale; // 当前x86-64用户编译器实际生成SSE2标量mulsd。
    if (result != 6.5) // 精确可表示的结果不受当前两种舍入模式差异影响。
        return 174; // 不只是汇编探针能运行，普通C浮点也要正确。
    if (!child) { // 子成功后替换为新ELF，观察默认状态是否重新建立。
        airi_puts("[SIMD_CHILD_OK] fork=inherited preempt=preserved failed_exec=preserved\n"); // 来自真实用户检查。
        airi_exec("simd-reset"); // 成功不返回，旧状态页应回收而非转给新映像。
        return 175; // 装载失败保留明确位置。
    }
    int status = -1; // 父收取同一孩子PID在新ELF中的退出结果。
    if (airi_waitpid(child, &status) != child || status) // 新程序必须已检查默认环境并返回0。
        return 176; // 不用旧程序的成功输出替代exec结果。
    long invalid = airi_fork(); // 独立孩子验证FXSAVE未对齐只触发当前用户GP，不让内核接受任意状态页。
    if (invalid < 0) // 没有创建身份不能当成故障已隔离。
        return 177; // 保留明确资源失败。
    if (!invalid) { // 该孩子故意把FXSAVE目标错开一个字节。
        _Alignas(16) unsigned char state[528]; // 整个目标都在合法用户栈，故障只来自16字节对齐要求。
        __asm__ volatile("fxsave64 (%0)" :: "r"(state + 1) : "memory"); // 真实未对齐用户指令产生GP13，不是跨越未映射页。
        return 178; // 若走到这里说明故障没有发生。
    }
    if (airi_waitpid(invalid, &status) != invalid || status != -141) // 自有编码-(128+13)，与Linux信号无关。
        return 179; // 父进程应继续活着并取得这个孩子的真实故障。
    airi_puts("[SIMD_FAULT_ISOLATED] fxsave=unaligned vector=13 parent=alive\n"); // 故障孩子已经收拢后才报告。
    airi_puts("[SIMD_USER_OK] fork=inherited timer_window=20 x87=private xmm15=private exec=reset double=6.5\n"); // 完整路径结束后才发布。
    return 0; // shell继续输入；launch最终核对独立CPU状态页也已归还。
}
