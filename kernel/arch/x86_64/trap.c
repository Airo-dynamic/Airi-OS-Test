#include <airi/trap.h> // 异常现场的 C 布局。
#include <airi/kernel.h> // 日志、清零和停止接口。
struct table_pointer { uint16_t limit; uint64_t base; } __attribute__((packed)); // CPU 要求 2+8 字节，中间不能有填充。
struct idt_gate { // 一个 64 位中断门占 16 字节。
    uint16_t low, selector; // 入口最低 16 位和目标代码段。
    uint8_t ist, flags; // 独立栈编号和存在/权限/门类型。
    uint16_t middle; // 入口中间 16 位。
    uint32_t high, zero; // 入口最高 32 位及必须清零的保留项。
} __attribute__((packed)); // 位布局必须符合处理器读取格式。
struct task_state { // 64 位 TSS 只保存硬件切栈所需地址，不承担任务调度。
    uint32_t reserved0; // 固定保留字段。
    uint64_t rsp[3]; // 从用户态进入内核时未来使用的栈。
    uint64_t reserved1, ist[7], reserved2; // IST1 放双重故障栈，其他保持零。
    uint16_t reserved3, io_offset; // I/O 权限位图起点设到结构之后，当前不提供位图。
} __attribute__((packed)); // TSS 的硬件偏移不能被 C 对齐改变。
_Static_assert(sizeof(struct task_state) == 104, "TSS layout"); // 编译时拒绝错误结构长度。
_Static_assert(sizeof(struct idt_gate) == 16, "IDT layout"); // 每个门必须恰好 16 字节。
_Static_assert(sizeof(struct airi_trap_frame) == 176, "trap layout"); // 栈含 15+2+5 个 8 字节槽。
static uint64_t gdt[5]; // null、代码、数据和两项 TSS 描述符。
static struct idt_gate idt[256]; // 整张表属于内核静态内存，安装后持续有效。
static struct task_state tss; // TSS 也必须在 ltr 后持续存在。
static uint8_t double_fault_stack[16384] __attribute__((aligned(16))); // 原栈损坏时 DF 换这块栈。
extern const uintptr_t airi_trap_entries[32]; // 汇编产生的异常入口地址表。
extern void airi_trap_255(void); // 未启用的向量转到统一停止路径。
extern void airi_tables_load(const void *, const void *); // 汇编依次加载 GDT/TSS/IDT。
extern void airi_probe_ud(void), airi_probe_gp(void); // 两个由作者明确触发的故障函数。
extern char airi_probe_ud_site[], airi_probe_ud_resume[]; // UD 精确指令与恢复地址。
extern char airi_probe_gp_site[], airi_probe_gp_resume[]; // GP 精确指令与恢复地址。
static uintptr_t expected_site, expected_resume; // 仅同步演示期间非零，不允许一般内核故障被吞掉。
static uint64_t expected_vector, expected_error; // 同时核对向量与硬件错误码。
static bool recovered; // 处理器真正进入处理函数后才置位。
void airi_trap_init(void) { // 调用时 IF 仍为零，尚不接收普通设备中断。
    gdt[1] = 0x00af9a000000ffffull; // present、ring0、64 位可读代码段。
    gdt[2] = 0x00cf92000000ffffull; // present、ring0、可写数据段。
    uintptr_t base = (uintptr_t)&tss; // TSS 描述符使用内核虚拟地址。
    uint64_t limit = sizeof(tss) - 1; // 描述符界限是最后一个有效字节偏移。
    gdt[3] = limit | ((base & 0xffffff) << 16) | (0x89ull << 40) | (((base >> 24) & 0xff) << 56); // available 64 位 TSS，地址低 32 位拆入描述符。
    gdt[4] = base >> 32; // TSS 地址高 32 位存到紧随的第二项。
    tss.ist[0] = (uintptr_t)(double_fault_stack + sizeof(double_fault_stack)); // 硬件从栈顶向低地址压入 DF 现场。
    tss.io_offset = sizeof(tss); // 当前 TSS 内没有 I/O 权限位图。
    for (unsigned i = 0; i < 256; ++i) { // 全部项目初始化，避免继承加载器状态。
        uintptr_t address = i < 32 ? airi_trap_entries[i] : (uintptr_t)airi_trap_255; // 未使用向量也有明确停止入口。
        idt[i] = (struct idt_gate){(uint16_t)address, 8, i == 8 ? 1 : 0, 0x8e, (uint16_t)(address >> 16), (uint32_t)(address >> 32), 0}; // DPL0 中断门，只有 DF 使用 IST1。
    }
    struct table_pointer gdtr = {sizeof(gdt) - 1, (uintptr_t)gdt}; // CPU 复制描述符值，不保留这个局部对象指针。
    struct table_pointer idtr = {sizeof(idt) - 1, (uintptr_t)idt}; // IDT 本体则在静态区持续存在。
    airi_tables_load(&gdtr, &idtr); // 装载表和选择子，函数返回后表已经由本内核拥有。
    airi_log("[TRAP_READY] vectors=32 cs=8 double_fault_ist=1 interrupts=off"); // 该标记只表示异常设施，不宣称调度或 IRQ 完成。
}
void airi_trap_dispatch(struct airi_trap_frame *f) { // 汇编传入的指针指向当前异常栈。
    uint64_t cr2; // 读取最近页故障的线性地址，非 PF 时仅作为附带状态。
    __asm__ volatile("mov %%cr2,%0" : "=r"(cr2)); // =r 请求输出到编译器分配的通用寄存器。
    airi_log("[EXCEPTION] vector=%llu error=0x%llx rip=0x%llx cs=0x%llx rsp=0x%llx cr2=0x%llx", // 保存实际 CPU 返回现场，便于反汇编定位。
             (unsigned long long)f->vector, (unsigned long long)f->error, (unsigned long long)f->rip, // 向量/错误码/指令位置分别输出。
             (unsigned long long)f->cs, (unsigned long long)f->rsp, (unsigned long long)cr2); // 栈地址来自 CPU，而非 C 局部变量。
    if (expected_site && f->rip == expected_site && f->vector == expected_vector && f->error == expected_error) { // 只接受精确登记的作者演示。
        f->rip = expected_resume; // 修改 iretq 将恢复的指令地址，不猜指令长度。
        expected_site = 0; // 一次性登记在返回前消费，不能无限跳过相同错误。
        recovered = true; // 调用者据此检查故障确实发生。
        return; // 汇编恢复寄存器并执行 iretq。
    }
    airi_panic("unhandled CPU exception"); // 未预期异常不返回有问题的指令。
}
static void demonstrate(void (*probe)(void), char *site, char *resume, uint64_t vector, uint64_t error) { // 普通函数指针调用可触发不同演示。
    expected_site = (uintptr_t)site; // 保存故障指令的精确地址。
    expected_resume = (uintptr_t)resume; // 保存对应的恢复位置。
    expected_vector = vector; // UD=6，GP=13。
    expected_error = error; // UD 补零；GP 是非法选择子的索引与表标记。
    recovered = false; // 防止前一次演示的结果被重用。
    probe(); // 同一 CPU 同步触发、报告并恢复。
    if (!recovered || expected_site) // 函数返回还不能证明 CPU 真的经历故障。
        airi_panic("exception probe did not recover"); // 明确拒绝意外绕过故障的实现。
}
void airi_trap_demo(void) { // 每次正常启动都验证两种栈形状。
    demonstrate(airi_probe_ud, airi_probe_ud_site, airi_probe_ud_resume, 6, 0); // 验证无硬件错误码的入口。
    demonstrate(airi_probe_gp, airi_probe_gp_site, airi_probe_gp_resume, 13, 0xfffc); // 验证有硬件错误码的入口与 iretq。
    airi_log("[TRAP_DEMO_OK] ud=6 gp=13 resumed=2"); // 两个 probe 都返回后才宣布通过。
}
_Noreturn void airi_trap_fatal_demo(void) { // 命令演示真正的未处理异常。
    expected_site = 0; // 不登记恢复，故障必须走停止路径。
    airi_probe_ud(); // 执行真实 UD2，处理器进入向量 6。
    airi_panic("fatal probe unexpectedly returned"); // 若错误返回也必须停止。
}
