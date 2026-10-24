#include <airi/tls.h> // 当前只提供FS，尚无ELF TLS装载或线程。
#include <airi/kernel.h> // 错误边界明确停止，启动范围可观察。
#define FS_BASE 0xc0000100u // 长模式的IA32_FS_BASE模型专用寄存器编号，不是内存地址。
static void set_base(uint64_t base) { // 调用者已验证该值是48位低半区规范地址或0。
    __asm__ volatile("xor %%eax,%%eax; mov %%ax,%%fs" ::: "rax", "memory"); // 先设空选择子，不能在WRMSR后用段装载覆盖基址。
    __asm__ volatile("wrmsr" :: "c"(FS_BASE), "a"((uint32_t)base), "d"((uint32_t)(base >> 32)) : "memory"); // ECX选MSR，EDX:EAX拼完整64位基址。
}
bool airi_tls_init(void) { // 固定x86-64仍明确确认CPU支持特权MSR操作。
    uint32_t a, b, c, d; // CPUID基本能力叶的四个结果。
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0)); // FPU初始化已经确认叶1存在。
    if (!(d & (1u << 5))) // EDX.MSR为0就不能安全执行RDMSR/WRMSR。
        return false; // 不修改控制或状态。
    uint64_t cr4; // 只修改用户直接访问基址指令的允许位。
    __asm__ volatile("mov %%cr4,%0" : "=r"(cr4)); // 保存现有分页、SSE与其他控制。
    cr4 &= ~(1ull << 16); // FSGSBASE关闭，用户通过受检查接口改基址，不允许WRFSBASE绕过。
    __asm__ volatile("mov %0,%%cr4" :: "r"(cr4) : "memory"); // 不改变K41的OSFXSR/OSXMMEXCPT设置。
    set_base(0); // 内核没有TLS，FS不指向任何用户数据。
    airi_log("[TLS_BASE_READY] register=FS direct_write=off kernel_base=0"); // 这里只承诺基址设施，不冒充完整__thread运行库。
    return true; // 之后所有用户返回都恢复自己的已保存值。
}
uint64_t airi_tls_take(void) { // 只在明确CS3陷入且通用寄存器已保存后调用。
    uint32_t low, high; // RDMSR将结果拆在两个32位寄存器。
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(FS_BASE) : "memory"); // 读真实硬件，不假定用户段选择子从未变化。
    uint64_t base = ((uint64_t)high << 32) | low; // 先提升宽度再左移，避免32位移位错误。
    set_base(0); // 内核普通处理、设备服务和返回监控台都不沿用用户基址。
    return base; // 所有者将此数值保存在进程控制块，不能解引用它。
}
void airi_tls_restore(uint64_t base) { // 选择目标身份后恢复；没有共享的内核FS变量。
    if (base >= (1ull << 47)) // 当前只允许48位低半区，坏值不能令特权WRMSR产生GP。
        airi_panic("invalid saved user FS base"); // 用户接口应在修改保存值之前拒绝非法输入。
    set_base(base); // 写入不创建映射；之后用户FS访问仍要通过正常USER页权限。
}
