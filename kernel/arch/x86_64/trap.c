#include <airi/irq.h> // 设备来源先撤销，再确认PIC级联。
#include <airi/block.h> // 当前固定设备IRQ只做通知，不在中断里收集DMA。
#include <airi/user.h> // 用户陷入可选择另一条完整返回现场。
#include <airi/timer.h> // 唯一开放的设备IRQ交给架构时钟驱动。
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
static uint64_t gdt[7]; // null、ring0代码/数据、ring3代码/数据和两项TSS描述符。
static struct idt_gate idt[256]; // 整张表属于内核静态内存，安装后持续有效。
static struct task_state tss; // TSS 也必须在 ltr 后持续存在。
static uint8_t double_fault_stack[16384] __attribute__((aligned(16))); // 原栈损坏时 DF 换这块栈。
extern const uintptr_t airi_trap_entries[32]; // 汇编产生的异常入口地址表。
extern void airi_trap_128(void), airi_trap_129(void); // 系统调用与内核启动用户上下文的门。
extern void airi_trap_43(void); // 当前q35设备IRQ11的独立向量入口。
extern void airi_trap_32(void); // IRQ0也复用完整通用寄存器保存入口。
extern void airi_trap_255(void); // 未启用的向量转到统一停止路径。
extern void airi_tables_load(const void *, const void *); // 汇编依次加载 GDT/TSS/IDT。
extern void airi_probe_ud(void), airi_probe_gp(void); // 两个由作者明确触发的故障函数。
extern char airi_probe_ud_site[], airi_probe_ud_resume[]; // UD 精确指令与恢复地址。
extern char airi_probe_gp_site[], airi_probe_gp_resume[]; // GP 精确指令与恢复地址。
static uintptr_t expected_site, expected_resume; // 仅同步演示期间非零，不允许一般内核故障被吞掉。
static uint64_t expected_vector, expected_error; // 同时核对向量与硬件错误码。
static uint64_t expected_cr2; // PF 必须同时核对本次访问的线性地址。
static bool recovered; // 处理器真正进入处理函数后才置位。
void airi_trap_init(void) { // 调用时 IF 仍为零，尚不接收普通设备中断。
    gdt[1] = 0x00af9a000000ffffull; // present、ring0、64 位可读代码段。
    gdt[2] = 0x00cf92000000ffffull; // present、ring0、可写数据段。
    uintptr_t base = (uintptr_t)&tss; // TSS 描述符使用内核虚拟地址。
    uint64_t limit = sizeof(tss) - 1; // 描述符界限是最后一个有效字节偏移。
    gdt[3] = 0x00affa000000ffffull; // ring3、64位、只读可执行代码段。
    gdt[4] = 0x00cff2000000ffffull; // ring3、可写数据段。
    gdt[5] = limit | ((base & 0xffffff) << 16) | (0x89ull << 40) | (((base >> 24) & 0xff) << 56); // available 64 位 TSS，地址低 32 位拆入描述符。
    gdt[6] = base >> 32; // TSS 地址高 32 位存到紧随的第二项。
    tss.ist[0] = (uintptr_t)(double_fault_stack + sizeof(double_fault_stack)); // 硬件从栈顶向低地址压入 DF 现场。
    tss.io_offset = sizeof(tss); // 当前 TSS 内没有 I/O 权限位图。
    for (unsigned i = 0; i < 256; ++i) { // 全部项目初始化，避免继承加载器状态。
        uintptr_t address = i < 32 ? airi_trap_entries[i] : (uintptr_t)airi_trap_255; // 未使用向量也有明确停止入口。
        if (i == 32) // 唯一开放的设备向量有自己的编号，不能冒充未知255。
            address = (uintptr_t)airi_trap_32; // 指向TRAP32生成的入口。
        if (i == 32u + AIRI_PCI_IRQ) // 唯一登记的PCI设备有自己的43号现场。
            address = (uintptr_t)airi_trap_43; // 与TRAP宏压入的编号一致。
        if (i == 128 || i == 129) // 两个软件中断各有固定身份。
            address = i == 128 ? (uintptr_t)airi_trap_128 : (uintptr_t)airi_trap_129; // 不能让它们落入未知向量255。
        idt[i] = (struct idt_gate){(uint16_t)address, 8, i == 8 ? 1 : 0, i == 128 ? 0xee : 0x8e, (uint16_t)(address >> 16), (uint32_t)(address >> 32), 0}; // int80门DPL3，其余DPL0；仅DF使用IST1。
    }
    struct table_pointer gdtr = {sizeof(gdt) - 1, (uintptr_t)gdt}; // CPU 复制描述符值，不保留这个局部对象指针。
    struct table_pointer idtr = {sizeof(idt) - 1, (uintptr_t)idt}; // IDT 本体则在静态区持续存在。
    airi_tables_load(&gdtr, &idtr); // 装载表和选择子，函数返回后表已经由本内核拥有。
    airi_log("[TRAP_READY] vectors=32 cs=8 double_fault_ist=1 interrupts=off"); // 该标记只表示异常设施，不宣称调度或 IRQ 完成。
}
struct airi_trap_frame *airi_trap_dispatch(struct airi_trap_frame *f) { // 汇编传入的指针指向当前异常栈。
    if (f->vector == 32u + AIRI_PCI_IRQ) { // 必须先于通用CS3故障判断，设备IRQ不能杀死用户进程。
        bool notified = airi_block_interrupt(); // 读取设备ISR使INTx先撤销，仅保存提示。
        airi_irq_end(AIRI_PCI_IRQ); // 再按从PIC到主PIC确认，避免电平来源持续触发。
        return notified && (f->cs & 3) == 3 ? airi_user_device_irq(f) : f; // 内核不抢占；用户可在完整现场交普通服务栈。
    }
    if (f->vector == 128 || f->vector == 129 || ((f->cs & 3) == 3 && f->vector != 32)) // 用户系统调用和用户故障由用户上下文所有者处理。
        return airi_user_trap(f); // 返回哪条完整现场由明确边界决定。
    if (f->vector == 32) { // PIC重映射后的时钟，不能作为CPU故障停止。
        airi_timer_interrupt(); // 计数并确认IRQ，保持当前任务现场不变。
        if ((f->cs & 3) == 3) // IRQ真正打断了ring3执行。
            return airi_user_timer(f); // 用户时间片结束时可返回另一个完整现场。
        return f; // 内核上下文始终不可抢占。
    }
    uint64_t cr2; // 读取最近页故障的线性地址，非 PF 时仅作为附带状态。
    __asm__ volatile("mov %%cr2,%0" : "=r"(cr2)); // =r 请求输出到编译器分配的通用寄存器。
    airi_log("[EXCEPTION] vector=%llu error=0x%llx rip=0x%llx cs=0x%llx rsp=0x%llx cr2=0x%llx", // 保存实际 CPU 返回现场，便于反汇编定位。
             (unsigned long long)f->vector, (unsigned long long)f->error, (unsigned long long)f->rip, // 向量/错误码/指令位置分别输出。
             (unsigned long long)f->cs, (unsigned long long)f->rsp, (unsigned long long)cr2); // 栈地址来自 CPU，而非 C 局部变量。
    if (expected_site && f->rip == expected_site && f->vector == expected_vector && f->error == expected_error && (f->vector != 14 || cr2 == expected_cr2)) { // 只接受精确登记的作者演示。
        f->rip = expected_resume; // 修改 iretq 将恢复的指令地址，不猜指令长度。
        expected_site = 0; // 一次性登记在返回前消费，不能无限跳过相同错误。
        recovered = true; // 调用者据此检查故障确实发生。
        return f; // 汇编恢复同一个故障演示现场并执行iretq。
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

extern void airi_probe_read(uint64_t), airi_probe_write(uint64_t), airi_probe_execute(uint64_t); // 汇编产生真实访存或取指。
extern char airi_probe_read_site[], airi_probe_read_resume[]; // 未映射读取的指令与恢复标签。
extern char airi_probe_write_site[], airi_probe_write_resume[], airi_probe_execute_resume[]; // 只读写入与 NX 取指的恢复标签。
void airi_trap_memory_probe(uint64_t address, enum airi_memory_fault kind) { // 只用于作者可预测的三种 supervisor 故障。
    expected_vector = 14; // 三种情况都由分页硬件产生 PF。
    expected_cr2 = address; // PF 报告的虚拟地址必须和本次实参一致。
    recovered = false; // 不继承之前演示的成功状态。
    if (kind == AIRI_FAULT_READ) { // 演示未映射地址的读访问。
        expected_site = (uintptr_t)airi_probe_read_site; // RIP 位于已知 load 指令。
        expected_resume = (uintptr_t)airi_probe_read_resume; // 返回标签不再访问目标。
        expected_error = 0; // supervisor、read、not-present。
        airi_probe_read(address); // 真实读指令应立即触发 PF。
    } else if (kind == AIRI_FAULT_WRITE) { // 演示只读页拒绝 supervisor 写。
        expected_site = (uintptr_t)airi_probe_write_site; // RIP 位于已知 store 指令。
        expected_resume = (uintptr_t)airi_probe_write_resume; // 跳过完整 store。
        expected_error = 3; // present=1、write=1、user=0。
        airi_probe_write(address); // CR0.WP 必须已经开启。
    } else { // 演示 NX 叶子拒绝取指。
        expected_site = address; // 取指故障 RIP 是目标，不是 call 本身。
        expected_resume = (uintptr_t)airi_probe_execute_resume; // 特殊恢复标签还会丢弃内部 call 的返回地址。
        expected_error = 17; // present=1、instruction-fetch=1、user=0。
        airi_probe_execute(address); // 目标含 ret 字节，若 NX 不生效也能返回并被下方检查拒绝。
    }
    if (!recovered || expected_site) // 只有处理器进入了正确 PF 入口才算完成。
        airi_panic("page fault probe did not recover"); // 不把普通函数返回误记为权限生效。
}

void airi_trap_set_stack(uint64_t top) { // 用户陷入时CPU从这里取ring0栈，不借用户提供的RSP。
    if (!top || top % 16) // 当前入口要求16字节对齐的有效栈顶。
        airi_panic("invalid ring0 stack top"); // 不装入可导致首次陷入即双重故障的描述。
    tss.rsp[0] = top; // 所有者必须在用户运行期间保留完整映射。
}

uint64_t airi_trap_fault_address(void) { // x86控制寄存器读取只留在架构实现。
    uint64_t address; // CR2保存最近页故障的线性地址。
    __asm__ volatile("mov %%cr2,%0" : "=r"(address)); // 输出约束让编译器分配通用寄存器。
    return address; // 仅向PF消费者交出本次数值。
}
