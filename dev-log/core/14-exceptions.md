# K14 · 让 CPU 故障成为可以定位的异常报告

串口会话已运行在自己的栈上，但地址翻译仍由 Limine 提供。接下来写页表或内存时，一次坏访问可能使 CPU 进入异常；没有自己的入口，就来不及留下是哪条指令出错。先用必定失败的 ud2 建立“故障、报告、恢复已知现场”的过程，再用非法段选择子验证带错误码的另一种栈形状。未登记的故障仍必须停止。

保留现有基础库、适配、会话和输入，先写三份异常文件，再接 Make、main 与启动工具，到章末统一构建。两个恢复点属于启动时自动执行的固定演示，不需要读者破坏其他环境，也不依赖图形路线。

<a id="trap-frame"></a>

## CPU 不是用普通 call 进入异常处理

执行 ud2 时，没有调用者按 C ABI 准备参数。CPU 得到向量 6，查 IDT 的第 6 个门，从中取得入口、代码段和换栈选择；非法段装载则到向量 13。cli 只挡可屏蔽设备中断，不能阻止当前指令自己产生的异常。

门中的 CS 还要指向 GDT 的有效代码段。长模式虽少用分段算地址，CPU 仍检查代码段权限和 64 位属性，所以先建立自己的 GDT，不继续借加载器表。选择子 8 指第一个八字节项，16 指第二项，24 指占两项的 TSS 描述符。这里 TSS 提供硬件切栈地址，不负责自动调度任务。

先跟本章的 ud2：CPU 保存 SS、RSP、RFLAGS、CS、RIP，这个异常没有硬件错误码。栈向低地址增长，因此保存后从当前 RSP 向高地址读取，依次是 RIP、CS、RFLAGS、原 RSP、SS。稍后的 TRAP 6 先补一个零作为 error，再压入 vector=6；从低地址看，便在 RIP 前多了 vector、error 两格。

公共入口还要保存被打断时的通用寄存器。它先压 RAX，最后压 R15，使新的 RSP 指向 r15；头文件便从 r15 开始按这个地址次序声明，直到末尾的 ss。故障可能打断任意指令，原值稍后都要继续使用，因此不能只保存普通 C 调用要求保留的少数寄存器。GP 自带硬件错误码，只压 vector 就能得到相同布局；再补零反而会使所有后续字段错位。

**新建 `include/airi/trap.h`。** 每项 uint64_t 八字节，声明次序须和实际压栈一致，停止接口用 _Noreturn 标记。

<!-- write: include/airi/trap.h -->
```c
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
```

<a id="trap-entry"></a>

## 先统一现场，再借普通 ABI 调用 C

TRAP 宏在汇编阶段为各向量生成入口，不是运行时函数。AT&T 的源/目标次序、寄存器与立即数前缀沿用入口；`\number` 替换宏参数，.if 决定是否生成补零。已有硬件错误码时若再补，整个字段位置都会错，恢复也会取错内容。

公共入口形成的现场现在可以按 airi_trap_frame 读取，但 CPU 尚未按 C ABI 发起调用。入口先清方向标志，把指向 r15 的 RSP 复制到 RDI，作为 airi_trap_dispatch 的第一个实参，再复制到 R12 留住现场起点。随后向下对齐 RSP，普通 call 才能使用符合 ABI 的调用栈。C 必须保留 R12，返回后便能用它找回对齐前那份现场。

恢复分两层：先从原现场逆序 pop 全部通用寄存器，再略过 vector/error 两格，剩下的栈顶就是 CPU 保存的 RIP。iretq 从这里恢复 RIP、CS、RFLAGS、RSP、SS；普通 ret 只取返回地址，无法完成这组恢复。C 若修改了现场中的 rip，iretq 使用的也就是修改后的值。

lgdt/lidt 从虚拟地址读取表描述。DF 门选择 IST1，处理异常时再发生双重故障可先换到独立栈；本章实际故障证据只覆盖 UD/GP，尚未破坏栈验证 DF。独立栈安装可在调试器查看。

**新建 `kernel/arch/x86_64/traps.S`。** 1f 指向后方最近标签，.irp 生成 0–31 的入口地址项；预处理汇编使用合法的 `/* ... */` 旁注。

<!-- write: kernel/arch/x86_64/traps.S -->
```asm
.section .text /* 异常入口属于可执行段。 */
.global airi_tables_load /* 由 C 初始化函数调用。 */
.type airi_tables_load,@function /* 标记为函数供 ELF 调试器识别。 */
airi_tables_load: /* rdi 指向 GDT 描述符，rsi 指向 IDT 描述符。 */
    lgdt (%rdi) /* 读取 10 字节的 GDT 界限与基地址。 */
    pushq $8 /* 远返回将选择 GDT 中的内核代码段。 */
    leaq 1f(%rip), %rax /* 1f 表示往后最近的数字标签 1。 */
    pushq %rax /* 压入远返回的目标指令地址。 */
    lretq /* 同时装入 RIP 和 CS，切到自己的代码段。 */
1: /* 已经使用自己的 CS=8。 */
    movw $16, %ax /* GDT 第二项是内核数据段。 */
    movw %ax, %ds /* 设置数据段选择子。 */
    movw %ax, %es /* 设置额外数据段选择子。 */
    movw %ax, %ss /* 设置内核栈段选择子。 */
    movw $24, %ax /* TSS 描述符从 GDT 第三项开始，占两项。 */
    ltr %ax /* 为双重故障加载独立栈信息。 */
    lidt (%rsi) /* 后续异常从自己的 IDT 取入口。 */
    ret /* 返回 C 初始化函数。 */
.size airi_tables_load, .-airi_tables_load /* 记录函数长度。 */

```

自己的表加载入口已写齐，接下来生成 CPU 查到的异常入口。UD 的 TRAP 6 会补零，GP 的 TRAP 13,1 则保留硬件错误码；两条路径随后汇到同一公共入口。

**继续追加向量入口与公共现场保存/恢复：**

<!-- append: kernel/arch/x86_64/traps.S -->
```asm
.macro TRAP number, error=0 /* 为每个异常生成同形的栈；默认没有硬件错误码。 */
.global airi_trap_\number /* C 入口表引用这个符号。 */
airi_trap_\number: /* 宏参数拼成唯一标签。 */
.if !\error /* 只有 CPU 没有压错误码时才补零。 */
    pushq $0 /* 补足统一的 error 槽位。 */
.endif /* 结束错误码差异。 */
    pushq $\number /* 每个入口固定自己的 vector。 */
    jmp airi_trap_common /* 共享寄存器保存和 C 调用。 */
.endm /* 结束宏定义。 */
TRAP 0 /* 除零，无硬件错误码。 */
TRAP 1 /* 调试异常。 */
TRAP 2 /* 不可屏蔽中断，当前不主动启用设备源。 */
TRAP 3 /* 断点。 */
TRAP 4 /* 溢出。 */
TRAP 5 /* 边界异常。 */
TRAP 6 /* 无效指令 UD。 */
TRAP 7 /* 设备不可用。 */
TRAP 8, 1 /* 双重故障，自带零错误码。 */
TRAP 9 /* 保留的旧协处理器向量。 */
TRAP 10, 1 /* 无效 TSS。 */
TRAP 11, 1 /* 段不存在。 */
TRAP 12, 1 /* 栈段故障。 */
TRAP 13, 1 /* 一般保护 GP。 */
TRAP 14, 1 /* 页故障 PF。 */
TRAP 15 /* 保留向量。 */
TRAP 16 /* x87 浮点异常。 */
TRAP 17, 1 /* 对齐检查。 */
TRAP 18 /* 机器检查。 */
TRAP 19 /* SIMD 浮点异常。 */
TRAP 20 /* 虚拟化异常。 */
TRAP 21, 1 /* 控制保护异常。 */
TRAP 22 /* 保留向量。 */
TRAP 23 /* 保留向量。 */
TRAP 24 /* 保留向量。 */
TRAP 25 /* 保留向量。 */
TRAP 26 /* 保留向量。 */
TRAP 27 /* 保留向量。 */
TRAP 28 /* Hypervisor 注入异常，不带错误码。 */
TRAP 29, 1 /* VMM 通信异常。 */
TRAP 30, 1 /* 安全异常。 */
TRAP 31 /* 保留向量。 */
TRAP 255 /* 未使用的 IDT 项统一停止；可屏蔽中断仍关闭。 */
airi_trap_common: /* CPU 现场和 vector/error 已形成。 */
    cld /* C ABI 要求字符串方向标志为零；iretq 会恢复原标志。 */
    pushq %rax /* 先保存最后在 C 结构中出现的通用寄存器。 */
    pushq %rbx /* 保存被打断代码的 RBX。 */
    pushq %rcx /* 保存被打断代码的 RCX。 */
    pushq %rdx /* 保存被打断代码的 RDX。 */
    pushq %rbp /* 保存被打断代码的帧指针。 */
    pushq %rdi /* 保存被打断代码的第一参数寄存器。 */
    pushq %rsi /* 保存被打断代码的第二参数寄存器。 */
    pushq %r8 /* 保存通用寄存器 R8。 */
    pushq %r9 /* 保存通用寄存器 R9。 */
    pushq %r10 /* 保存通用寄存器 R10。 */
    pushq %r11 /* 保存通用寄存器 R11。 */
    pushq %r12 /* 保存原 R12，随后可借它保留现场指针。 */
    pushq %r13 /* 保存通用寄存器 R13。 */
    pushq %r14 /* 保存通用寄存器 R14。 */
    pushq %r15 /* 最后保存的 R15 位于结构起点。 */
    movq %rsp, %rdi /* C 的第一个实参是现场指针。 */
    movq %rsp, %r12 /* C ABI 保证被调用者保留 R12。 */
    andq $-16, %rsp /* call 之前对齐到 16 字节，不假设异常发生点的栈对齐。 */
    call airi_trap_dispatch /* C 负责报告与明确登记的演示恢复。 */
    movq %r12, %rsp /* 丢弃调用前可能增加的对齐空间。 */
    popq %r15 /* 从保存区域恢复通用寄存器。 */
    popq %r14 /* 顺序与压栈相反。 */
    popq %r13 /* 恢复 R13。 */
    popq %r12 /* 恢复被打断代码的原 R12。 */
    popq %r11 /* 恢复 R11。 */
    popq %r10 /* 恢复 R10。 */
    popq %r9 /* 恢复 R9。 */
    popq %r8 /* 恢复 R8。 */
    popq %rsi /* 恢复 RSI。 */
    popq %rdi /* 恢复 RDI。 */
    popq %rbp /* 恢复 RBP。 */
    popq %rdx /* 恢复 RDX。 */
    popq %rcx /* 恢复 RCX。 */
    popq %rbx /* 恢复 RBX。 */
    popq %rax /* 恢复 RAX。 */
    addq $16, %rsp /* 移除软件 vector 与统一 error 两个槽位。 */
    iretq /* 恢复 RIP、CS、RFLAGS、RSP、SS，继续被打断代码。 */

```

公共入口现在能把同形现场交给 C 并取回。演示还需要两个精确故障地址和各自的恢复地址：ud2 后直接 ret；GP 在装载 DS 的指令处失败，原 DS 未变，恢复后同样 ret 回到普通 C 调用者。

**继续追加两份探针及只读入口表，完成汇编文件：**

<!-- append: kernel/arch/x86_64/traps.S -->
```asm
.global airi_probe_ud, airi_probe_ud_site, airi_probe_ud_resume /* C 用精确地址登记预期故障。 */
airi_probe_ud: /* 普通 C 调用进入演示函数。 */
airi_probe_ud_site: /* 仅这个地址的 UD 可以在演示时恢复。 */
    ud2 /* 必然触发向量 6，不是普通函数调用。 */
airi_probe_ud_resume: /* 处理器保存的 RIP 被明确改为此处。 */
    ret /* 回到调用演示的 C 函数。 */
.global airi_probe_gp, airi_probe_gp_site, airi_probe_gp_resume /* GP 演示地址供 C 比对。 */
airi_probe_gp: /* 原 AX 会由 C 调用约定视为可变。 */
    movw $0xffff, %ax /* 构造超出当前表界限的选择子。 */
airi_probe_gp_site: /* 故障报告的 RIP 应指向这条装载。 */
    movw %ax, %ds /* 无效选择子触发带硬件错误码的 GP。 */
airi_probe_gp_resume: /* 故障装载未成功，原 DS 仍有效。 */
    ret /* 返回 C 检查点。 */
.section .rodata /* 入口地址表只读。 */
.global airi_trap_entries /* C 用这张表填 IDT。 */
airi_trap_entries: /* 连续 32 个 64 位入口地址。 */
.irp n,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31 /* irp 将下面一项按 n 展开。 */
    .quad airi_trap_\n /* 地址必须与所属向量一致。 */
.endr /* 结束重复展开。 */
.section .note.GNU-stack,"",@progbits /* 声明不需要可执行栈。 */
```

<a id="trap-owner"></a>

## 有报告，不等于任意故障都能恢复

GDT、IDT、TSS 与 DF 栈在安装后仍会被硬件读取，放在静态区。局部 table_pointer 只提供两字节 limit 与八字节 base，packed 禁止填充；CPU 在 lgdt/lidt 时取得这十字节，之后继续使用的是静态表本体，不借局部对象。

IDT 将入口地址拆为三段，0x8e 指存在、ring0 的 64 位中断门。TSS 的 0x89 指存在且可用，ltr 后由硬件标忙；拆位只是符合硬件记录布局，仍指向同一地址。

报告先保存 vector/error/RIP/CS/原 RSP/CR2。CR2 是最近页故障地址，仅 vector=14 才可当成本次访问地址。演示同时登记 site、vector、error，完全匹配才把保存 RIP 改为 resume；没有匹配就 panic。一般错误的指令长度与后续状态未知，不能随意跳过几个字节。

第二项用选择子 0xffff，GP 错误码应为 0xfffc：索引和 TI 保留，低位的 EXT/IDT 意义不同于选择子 RPL，因此最低两位清零。错误装载未改变 DS，恢复标签仍可用原数据段。recovered 还要由处理函数置位，区分“真的经过异常”和“普通调用碰巧返回”。

**新建 `kernel/arch/x86_64/trap.c`。** 编译期结构大小断言先拒绝硬件与 C 布局不符。

<!-- write: kernel/arch/x86_64/trap.c -->
```c
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
```

静态对象为硬件表和演示登记提供了持续存储，接下来才将它们填好并装入 CPU。idt[6] 取汇编表下标 6 的入口，代码段选择子为 8；只有 idt[8] 选择 IST1，它的栈顶来自 tss.ist[0]。局部 gdtr/idtr 把这些静态地址交给加载函数后即可结束寿命。

**继续追加表初始化：**

<!-- append: kernel/arch/x86_64/trap.c -->
```c
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
```

表安装完成后，CPU 的向量入口经公共汇编把现场指针交到 f。处理函数先报告现场，再判断这是不是正在演示的同一条指令：site、vector、error 三项必须同时符合。成功时只改保存的 rip，并消费一次登记；它返回汇编之后，iretq 才真正进入恢复标签。

**继续追加报告与精确恢复：**

<!-- append: kernel/arch/x86_64/trap.c -->
```c
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
```

处理器已经有了核对登记的出口，普通调用侧还要在 probe 之前写好登记。第一项实际参数是 ud 探针、ud site/resume、向量 6 和错误码 0；probe 返回后再检查 recovered，防止没有经历故障却误判成功。第二项沿相同过程验证 GP 的 13/0xfffc。fatal 入口先撤掉 site，再执行同一 ud2，因而必须走 panic。

**最后追加演示调用与未登记路径：**

<!-- append: kernel/arch/x86_64/trap.c -->
```c
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
```

<a id="trap-build"></a>

## 在后续内存操作之前安装诊断路径

Make 增加 trap.c 与 traps.o。原 no-red-zone 仍必要：异常可能向 RSP 下方压现场，不能让编译器把临时值偷偷放在那里。general-regs-only 也继续保留，因为入口还未保存浮点/SIMD；这只是限制内核生成的指令，并不是完整任务上下文已经实现。

main 在启动描述验证后安装表，自动运行两个恢复点。trap-fatal 另在独立产物目录定义宏，再触发没有登记的 ud2，应该 PANIC 而无 READY；缺必需启动信息仍由前面的适配先拒绝。

**局部更新 `Makefile` 的源码/对象清单和故障宏条件。** 默认串口编入异常实现，desktop 条件才读取图形依赖。

在文件中依次找到下列 before 窗口，各用对应 after 完整替换；窗口以外保留。

<!-- replace: Makefile -->
<!-- before -->
```make
SOURCES := kernel/main.c kernel/boot/limine.c kernel/boot/info.c \
    kernel/lib/base.c kernel/lib/runtime.c kernel/arch/x86_64/serial.c \
    kernel/console/console.c kernel/arch/x86_64/input.c kernel/arch/x86_64/cpu.c
# 只有 desktop- 前缀目标才读取图形支线自己的构建片段。
ifneq ($(filter desktop-%,$(PROFILE)),)
```
<!-- after -->
```make
SOURCES := kernel/main.c kernel/boot/limine.c kernel/boot/info.c \
    kernel/lib/base.c kernel/lib/runtime.c kernel/arch/x86_64/serial.c \
    kernel/console/console.c kernel/arch/x86_64/input.c kernel/arch/x86_64/cpu.c kernel/arch/x86_64/trap.c
# 只有 desktop- 前缀目标才读取图形支线自己的构建片段。
ifneq ($(filter desktop-%,$(PROFILE)),)
```

继续找到下一处旧窗口，用对应新文本替换：

<!-- replace: Makefile -->
<!-- before -->
```make
CASE := $(patsubst desktop-%,%,$(PROFILE))
# patsubst把每个.c相对路径改为当前OUT下的.o，汇编入口对象另加入。
OBJECTS := $(patsubst %.c,$(OUT)/%.o,$(SOURCES)) $(OUT)/kernel/arch/x86_64/entry.o
# 交叉目标为无宿主ELF；C11/O2/调试信息/警告作为错误。freestanding与fno-builtin关闭宿主假设；禁栈保护和PIC/PIE；不使用red zone与浮点寄存器；kernel地址模型；MMD/MP生成头文件依赖。
CFLAGS := --target=x86_64-unknown-none-elf -std=c11 -O2 -g -Wall -Wextra -Werror \
```
<!-- after -->
```make
CASE := $(patsubst desktop-%,%,$(PROFILE))
# patsubst把每个.c相对路径改为当前OUT下的.o，汇编入口对象另加入。
OBJECTS := $(patsubst %.c,$(OUT)/%.o,$(SOURCES)) $(OUT)/kernel/arch/x86_64/entry.o $(OUT)/kernel/arch/x86_64/traps.o
# 交叉目标为无宿主ELF；C11/O2/调试信息/警告作为错误。freestanding与fno-builtin关闭宿主假设；禁栈保护和PIC/PIE；不使用red zone与浮点寄存器；kernel地址模型；MMD/MP生成头文件依赖。
CFLAGS := --target=x86_64-unknown-none-elf -std=c11 -O2 -g -Wall -Wextra -Werror \
```

继续找到下一处旧窗口，用对应新文本替换：

<!-- replace: Makefile -->
<!-- before -->
```make
CPPFLAGS += -DAIRI_TEST_NO_MEMMAP
endif # 结束 missing-memory 的编译条件
# 修改实验仅改变标题与输出文本，普通配置不继承这两个宏。
ifeq ($(CASE),edit-experiment)
```
<!-- after -->
```make
CPPFLAGS += -DAIRI_TEST_NO_MEMMAP
endif # 结束 missing-memory 的编译条件
# 作者故障配置不登记恢复，应报告真实异常后停止。
ifeq ($(CASE),trap-fatal)
CPPFLAGS += -DAIRI_TEST_FATAL_TRAP # 仅此构建目录触发未处理 UD2。
endif # 结束故障配置。
# 修改实验仅改变标题与输出文本，普通配置不继承这两个宏。
ifeq ($(CASE),edit-experiment)
```

入口先让异常表可用，再执行演示，回到普通调用链后才继续已有会话初始化与输入循环。两个演示恢复不改变串口、文字环和可选绘图的所有者。

**在 `kernel/main.c` 加入异常声明，并在启动验证成功后接入安装和演示：**

继续找到下一处旧窗口，用对应新文本替换：

<!-- replace: kernel/main.c -->
<!-- before -->
```c
#include <airi/arch.h>     // 通过架构边界使用忙等提示。
#include <airi/kernel.h>   // 取得启动信息、基础库与串口日志接口。
```
<!-- after -->
```c
#include <airi/trap.h> // 安装异常表并演示处理器现场。
#include <airi/arch.h>     // 通过架构边界使用忙等提示。
#include <airi/kernel.h>   // 取得启动信息、基础库与串口日志接口。
```

继续找到下一处旧窗口，用对应新文本替换：

<!-- replace: kernel/main.c -->
<!-- before -->
```c
    if (error)  // 有具体原因则不能继续读取未经验证的数据。
        airi_panic(error);  // 输出失败原因并停止当前 CPU 执行。
    airi_log("[BOOT_INFO] version=%u regions=%u loader=%s", info->version, info->region_count,  // 打印协议版本和有效内存区域数，%u 与无符号参数匹配。
             info->loader);  // 输出适配层复制且有 NUL 终止的加载器名称。
```
<!-- after -->
```c
    if (error)  // 有具体原因则不能继续读取未经验证的数据。
        airi_panic(error);  // 输出失败原因并停止当前 CPU 执行。
    airi_trap_init(); // 通过启动校验后先接管异常，不启用设备中断。
    airi_trap_demo(); // 正常启动验证带与不带硬件错误码的返回路径。
#ifdef AIRI_TEST_FATAL_TRAP // 只在作者故障镜像中执行未处理异常。
    airi_trap_fatal_demo(); // 必须报告并停止，不能发布 READY。
#endif // 普通镜像继续初始化。
    airi_log("[BOOT_INFO] version=%u regions=%u loader=%s", info->version, info->region_count,  // 打印协议版本和有效内存区域数，%u 与无符号参数匹配。
             info->loader);  // 输出适配层复制且有 NUL 终止的加载器名称。
```

<a id="trap-check"></a>

## 根据现场与结果判断，不能只等进程存活

启动工具仍删旧日志、有截止时间并在 finally 回收自己进程。正常版除了 READY，还必须有两次恢复标记；trap-fatal 必须出现 unhandled CPU exception 且禁止 READY。两条路径从相同设施进入，区别是当前故障有没有精确登记。

**局部更新 `scripts/boot.py` 的预期标记和成功条件。** 只扩展配置预期和标记核对，QMP、超时与清理沿用 K12。

在文件中依次找到下列 before 窗口，各用对应 after 完整替换；窗口以外保留。

<!-- replace: scripts/boot.py -->
<!-- before -->
```python
    graphics = profile.startswith('desktop-') and case != 'no-framebuffer'  # 有绘图实现且设备存在才应生成画面。
    expected='[AIRI_PANIC] required Limine memory map is missing' if case=='missing-memory' else '[AIRI_P1_READY]'  # 缺内存响应要看具体 PANIC，其他配置要看 READY。
    started=time.monotonic()  # 单调秒数作为经过时间起点，不受系统时钟调整影响。
    with (work/'qemu.log').open('w') as errors:  # 主机 QEMU 输出单独存档，with 保证文件最终关闭。
```
<!-- after -->
```python
    graphics = profile.startswith('desktop-') and case != 'no-framebuffer'  # 有绘图实现且设备存在才应生成画面。
    expected='[AIRI_PANIC] required Limine memory map is missing' if case=='missing-memory' else '[AIRI_P1_READY]'  # 缺内存响应要看具体 PANIC，其他配置要看 READY。
    if case == 'trap-fatal': # 未处理异常镜像必须进入具体停止路径。
        expected = '[AIRI_PANIC] unhandled CPU exception' # 不以 QEMU 存活推断故障已处理。
    started=time.monotonic()  # 单调秒数作为经过时间起点，不受系统时钟调整影响。
    with (work/'qemu.log').open('w') as errors:  # 主机 QEMU 输出单独存档，with 保证文件最终关闭。
```

继续找到下一处旧窗口，用对应新文本替换：

<!-- replace: scripts/boot.py -->
<!-- before -->
```python
                complete_lines = text.splitlines(keepends=True)  # 保留行结束符，完整记录与写到一半的标记可区分。
                if any(line.endswith('\n') and line.startswith(expected) for line in complete_lines):  # 生成器逐行检查，至少一个完整且以预期标记开头才成立。
                    if case=='missing-memory':  # 失败配置只允许具体拒绝记录，不能同时宣布启动完成。
                        assert '[AIRI_P1_READY]' not in text  # 错误路径不得出现成功标记。
                    else:  # 按当前profile的另一条完整验证路径处理。
                        assert '[AIRI_PANIC]' not in text  # 正常完成路径不得混入内核停止记录。
                        assert f'graphics={int(graphics)}' in text  # 报告值须与本次选择的呈现实现一致。
```
<!-- after -->
```python
                complete_lines = text.splitlines(keepends=True)  # 保留行结束符，完整记录与写到一半的标记可区分。
                if any(line.endswith('\n') and line.startswith(expected) for line in complete_lines):  # 生成器逐行检查，至少一个完整且以预期标记开头才成立。
                    if case in ('missing-memory', 'trap-fatal'):  # 失败配置只允许具体拒绝记录，不能同时宣布启动完成。
                        assert '[AIRI_P1_READY]' not in text  # 错误路径不得出现成功标记。
                    else:  # 按当前profile的另一条完整验证路径处理。
                        assert '[TRAP_DEMO_OK] ud=6 gp=13 resumed=2' in text # 正常启动必须真的从两种异常栈返回。
                        assert '[AIRI_PANIC]' not in text  # 正常完成路径不得混入内核停止记录。
                        assert f'graphics={int(graphics)}' in text  # 报告值须与本次选择的呈现实现一致。
```

把 trap-fatal 加入 PROFILES，它同时约束 CLI 的合法配置和完整回归会启动哪些镜像。只定义宏而不实际加入运行列表，无法取得停止路径的证据。

**在 `scripts/manage.py` 将 trap-fatal 加入原 PROFILES；其他入口规则保留：**

继续找到下一处旧窗口，用对应新文本替换：

<!-- replace: scripts/manage.py -->
<!-- before -->
```python
from console_test import check_console  # 复用真实键盘和UART输入验证。

PROFILES=('normal','no-framebuffer','missing-memory','edit-experiment')  # 元组固定四种构建配置，CLI choices 与 test 遍历共用。
def run(cmd):  # 所有统一入口外部命令都从同一根目录执行。
    subprocess.run(cmd,cwd=ROOT,check=True)  # 非零状态转成异常传至入口。
```
<!-- after -->
```python
from console_test import check_console  # 复用真实键盘和UART输入验证。

PROFILES=('normal','no-framebuffer','missing-memory','edit-experiment','trap-fatal')  # 元组固定5种构建配置，CLI choices 与 test 遍历共用。
def run(cmd):  # 所有统一入口外部命令都从同一根目录执行。
    subprocess.run(cmd,cwd=ROOT,check=True)  # 非零状态转成异常传至入口。
```

在 `scripts/manage.py` 中同步本阶段已变化的条件说明；执行语句保留。旧块用于定位，只写回对应新块。

<!-- replace: scripts/manage.py -->
<!-- before -->
```python
        build('normal')  # 调试默认仍使用串口目标，测试宏不进入调试对象。
        run([sys.executable,str(ROOT/'scripts/kernel_debug.py'),'--host',args.host])  # 远端GDB连接本地QEMU。
        return  # 调试记录完成后结束本动作。
    if args.action=='test':  # 完整验证串联主机算法、前端、四种启动和两种真实输入。
        run(make_command(target='host-test'))  # 编译同一纯C实现并运行带ASan/UBSan的主机边界检查。
        if args.graphics:  # 普通 test 不导入或验证前端。
            check_frontend(screenshots=False)  # 图形模型检查仍有自己的一组断言。
        results=[]  # 列表依次保存各配置的实际启动结果。
        profiles = [('desktop-' + p) if args.graphics else p for p in PROFILES]  # 呈现方式和故障场景正交。
        for profile in profiles:  # 按固定四种配置独立编译与启动。
            build(profile)  # 生成对应配置的ELF和镜像，产物目录彼此隔离。
            if profile.endswith('normal'):  # 普通目标额外检查ELF结构。
                inspect_elf(profile)  # 保存机器、类型、依赖与符号报告。
            results.append(check(profile))  # 等待该配置完成标记并保存结果，任一失败异常中止。
        (ROOT/'artifacts'/('desktop-boot-tests.json' if args.graphics else 'boot-tests.json')).write_text(json.dumps(results,indent=2)+'\n')  # 四种启动结果编码为JSON数组。
        prefix = 'desktop-' if args.graphics else ''  # 本轮输入验证选择同一呈现方式。
        console_results=[check_console(prefix+'normal'),check_console(prefix+'no-framebuffer'),check_console(prefix+'normal',uart_input=False)]  # 顺序验证normal UART、无帧缓冲UART与normal PS/2三条真实输入路径。
        (ROOT/'artifacts'/('desktop-console-tests.json' if args.graphics else 'console-tests.json')).write_text(json.dumps(console_results,indent=2)+'\n')  # 保存三次实际输入验证结果。
```
<!-- after -->
```python
        build('normal')  # 调试默认仍使用串口目标，测试宏不进入调试对象。
        run([sys.executable,str(ROOT/'scripts/kernel_debug.py'),'--host',args.host])  # 远端GDB连接本地QEMU。
        return  # 调试记录完成后结束本动作。
    if args.action=='test':  # 完整验证串联主机算法、前端、5种启动和三次真实输入观察。
        run(make_command(target='host-test'))  # 编译同一纯C实现并运行带ASan/UBSan的主机边界检查。
        if args.graphics:  # 普通 test 不导入或验证前端。
            check_frontend(screenshots=False)  # 图形模型检查仍有自己的一组断言。
        results=[]  # 列表依次保存各配置的实际启动结果。
        profiles = [('desktop-' + p) if args.graphics else p for p in PROFILES]  # 呈现方式和故障场景正交。
        for profile in profiles:  # 按固定5种配置独立编译与启动。
            build(profile)  # 生成对应配置的ELF和镜像，产物目录彼此隔离。
            if profile.endswith('normal'):  # 普通目标额外检查ELF结构。
                inspect_elf(profile)  # 保存机器、类型、依赖与符号报告。
            results.append(check(profile))  # 等待该配置完成标记并保存结果，任一失败异常中止。
        (ROOT/'artifacts'/('desktop-boot-tests.json' if args.graphics else 'boot-tests.json')).write_text(json.dumps(results,indent=2)+'\n')  # 5种启动结果编码为JSON数组。
        prefix = 'desktop-' if args.graphics else ''  # 本轮输入验证选择同一呈现方式。
        console_results=[check_console(prefix+'normal'),check_console(prefix+'no-framebuffer'),check_console(prefix+'normal',uart_input=False)]  # 顺序验证normal UART、无帧缓冲UART与normal PS/2三条真实输入路径。
        (ROOT/'artifacts'/('desktop-console-tests.json' if args.graphics else 'console-tests.json')).write_text(json.dumps(console_results,indent=2)+'\n')  # 保存三次实际输入验证结果。
```

全部改动就绪后，从统一入口运行本章回归。正常与 fatal 各用自己的镜像和日志。

<!-- run -->
```sh
./airios build  # 构建普通串口镜像。
./airios build --profile trap-fatal  # 独立构建未处理故障镜像。
PYTHONPATH=scripts python3 - <<'PYTRAP'  # 引用结束词使Shell原样交出其中的Python源码。
from boot import check  # 使用 K12 已解释的有界启动观察。
check('normal')  # 应先出现两个 EXCEPTION，再出现 TRAP_DEMO_OK 和 READY。
check('trap-fatal')  # 应第三次出现向量 6，再出现 PANIC，绝不能出现 READY。
PYTRAP
```

既有检查点记录中，向量 6/error=0、向量 13/error=0xfffc，CS 都为 8，随后是 `[TRAP_DEMO_OK] ud=6 gp=13 resumed=2` 和 P1 READY。RIP 随链接变化，以同次 ELF 定位；正常与 fatal 日志分别在 artifacts/boot-normal、boot-trap-fatal。

调试可在 airi_trap_dispatch 停下，用 p *f 读现场；若尚未执行 C 序言，用 `p *(struct airi_trap_frame *)$rdi` 或先 stepi。x/i 查看 RIP，分别应是 ud2 与装载 ds；idt[8].ist、tss.ist[0] 则显示 DF 栈配置。Linux 用户程序只能请求内核服务，不能像这里安装 IDT 或执行这些特权指令。

切换 trap-fatal 会完整演示同样 ud2 从“登记恢复”变为“未登记停止”，无需另破坏源码。Linux 通常把用户非法指令转成信号，当前自制系统还没有用户进程，意外异常停止整个单核内核。下一步用它定位物理页与页表操作错误。

依据 [Intel 系统编程手册](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html) 的异常、64 位栈和 TSS 规则。设备 IRQ 尚未开启，未验证 NMI/机器检查恢复，也不承诺任意故障后都还能输出日志。
