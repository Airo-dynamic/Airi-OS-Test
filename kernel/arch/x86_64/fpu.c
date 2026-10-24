#include <airi/fpu.h> // 架构状态不暴露给用户指针或普通设备。
#include <airi/kernel.h> // 内部对齐与启动能力失败明确报告。
static _Alignas(16) uint8_t initial[AIRI_FPU_BYTES]; // 只读模板来自本CPU的真实状态布局，保留到关机。
static bool initialized; // 不能在CPU设施未准备好时创建用户状态。
extern void airi_fpu_seed(void *state); // 独立汇编清除所有x87数据和XMM0至15再保存。
bool airi_fpu_init(void) { // 调用时已安装IDT，尚无用户运行，内核C仍仅用通用寄存器。
    uint32_t a, b, c, d; // CPUID基础叶给处理器能力。
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0), "c"(0)); // 先检查叶1是否存在。
    if (a < 1) // 不能读取不存在的能力叶。
        return false; // 没有修改控制寄存器。
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0)); // 读取FXSR、SSE和SSE2能力。
    uint32_t required = (1u << 24) | (1u << 25) | (1u << 26); // 本轮完整支持集合，不能只接受有x87的旧目标。
    if ((d & required) != required) // 不能执行目标不支持的保存或用户指令。
        return false; // 固定qemu64应满足，其他目标明确拒绝。
    uint64_t cr0, cr4; // 特权控制只留在架构层。
    __asm__ volatile("mov %%cr0,%0" : "=r"(cr0)); // 保留原有分页与写保护等位。
    cr0 = (cr0 & ~((1ull << 2) | (1ull << 3))) | (1ull << 1) | (1ull << 5); // 关闭EM/TS，开启MP/NE；不采用延迟#NM切换。
    __asm__ volatile("mov %0,%%cr0" :: "r"(cr0) : "memory"); // 允许实际浮点指令并走CPU异常路径。
    __asm__ volatile("mov %%cr4,%0" : "=r"(cr4)); // 原有四级页表设置保持。
    cr4 = (cr4 | (1ull << 9) | (1ull << 10)) & ~(1ull << 18); // OSFXSR/OSXMMEXCPT开启，OSXSAVE关闭，不开放未保存的AVX状态。
    __asm__ volatile("mov %0,%%cr4" :: "r"(cr4) : "memory"); // 之后FXSAVE64包含XMM与MXCSR。
    airi_memset(initial, 0, sizeof(initial)); // 指令未写的保留字节也不携带旧内容。
    airi_fpu_seed(initial); // 真实清空硬件，再取得本CPU可恢复的512字节模板。
    airi_memset(initial + 6, 0, 18); // 新进程不继承旧FOP/FIP/FDP指针字段，即使固件曾使用x87。
    initialized = true; // 完整模板建立后才允许用户资源构造。
    airi_log("[CPU_STATE_READY] x87=1 sse2=1 fxsave_bytes=512 avx=off kernel=general-only"); // 能力和范围明确可观察。
    return true; // 尚未装载或运行任何用户程序。
}
static void require_state(const void *state) { // 所有调用都来自内核持有的物理页HHDM别名。
    if (!initialized || !state || (uintptr_t)state % 16) // FXSAVE/FXRSTOR的对齐失败属于内核构造错误。
        airi_panic("user FPU state is missing or misaligned"); // 不把任意用户地址交给特权返回路径。
}
void airi_fpu_new(void *state) { // 构造新状态不改变正在运行者的硬件寄存器。
    require_state(state); // 目标必须已经拥有且可写。
    airi_memcpy(state, initial, sizeof(initial)); // 普通整数复制，不在每次exec中重新FNINIT当前用户。
}
void airi_fpu_save(void *state) { // IRQ与系统调用都已保存完整GPR，当前内核不触碰浮点。
    require_state(state); // 状态页不是用户提供的FXSAVE映像。
    __asm__ volatile("fxsave64 (%0)" :: "r"(state) : "memory"); // 保存x87/MMX、XMM0-15与MXCSR，内存必须保持到下次恢复。
}
void airi_fpu_restore(const void *state) { // 目标身份、CR3与返回现场已确定。
    require_state(state); // 只恢复真实硬件保存或初始模板形成的合法内容。
    __asm__ volatile("fxrstor64 (%0)" :: "r"(state) : "memory"); // 内核C保持general-only，此后直到iretq没有XMM活值被覆盖。
}
