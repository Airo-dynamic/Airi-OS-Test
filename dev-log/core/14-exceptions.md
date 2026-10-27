# K14 · 让 CPU 故障成为可以定位的异常报告

K13 的串口会话已经能持续处理命令。接下来要分配物理页、建立自己的地址映射，这些操作一旦写错，可能在某条指令处使 CPU 无法继续。串口发送函数已经存在，但 CPU 遇到故障时还需要知道去哪里执行，发送函数才有机会留下报告。

先让内核执行一条确定无效的指令 `ud2`。它的用途就是产生无效指令异常：CPU 不能按正常顺序继续执行这条指令，转而按硬件规定进入异常入口。我们要把这次入口接到 C 报告函数，留下故障指令地址和寄存器，再让这个已知演示从指定标签继续。第二个演示尝试装入无效的段选择子，用另一种硬件现场走同一条路径。一般故障没有预定恢复位置，报告后停止。

本章从 K13 和 L01 的工程继续，基础库、引导适配、会话与输入全部保留。先写现场声明、汇编入口和 C 处理，再把它们接入构建与启动；代码写齐以后统一运行。两个故障由启动代码依次触发，各自的发生位置和恢复位置都写在程序中。

<a id="trap-frame"></a>

<a id="cpu-不是用普通-call-进入异常处理"></a>

## 用故障编号找到报告入口

第 06 章的普通调用把目标函数地址写进 `call`；`ud2` 的指令字节里没有报告函数地址。CPU 先把这类故障归为编号 6，这个编号称为异常向量，再按向量查内存中的中断描述符表 `IDT`。内核负责预先填表，CPU 负责在故障发生时按硬件格式读取它。

CPU 的 IDTR 寄存器保存 IDT 的起点和界限。64 位模式每条入口记录占 16 字节，向量 6 就选择表起点之后第 `6×16` 字节处的 `idt[6]`。该记录保存汇编标签 `airi_trap_6` 的地址，所以“故障是第几类”和“去哪里执行”在这里接上。向量 6 是表下标，入口地址是表项中的另一个值。

这条入口记录称为门。CPU 进入一段代码时还要知道它的权限和执行模式，因此门除地址外还保存代码段选择子。选择子按索引指向全局描述符表 `GDT`，CPU 从中读取代码段属性，并把选择子装进代码段寄存器 CS。本章 GDT 的第零项为空，第一项是内核代码，第二项是内核数据；每项八字节，选择子 8 的索引为 1，选择子 16 的索引为 2，低三位都为零，选择 GDT 和内核权限。于是 `idt[6]` 的“地址 + 选择子 8”共同确定异常入口。

64 位模式主要通过页表安排内存地址，代码段的执行权限与 64 位属性仍要符合硬件规则。我们会安装自己的 GDT、IDT，让后续入口持续使用内核持有的记录。先跟随仍在内核权限中运行的 `ud2`：它沿当前内核栈保存现场；处理故障时换备用栈的情况在后面单独接入。

### 先把 CPU 留下的现场排成可以读取的结构

找到门以后，CPU 还不能直接把普通 C 函数当入口。被打断的调用链以后可能需要恢复，CPU 必须先保存返回所需的现场。RIP 是指令位置；本次故障保存的 RIP 正好指向 `ud2`。RFLAGS 保存处理器标志，CS 和 SS 是代码段与栈段选择子，RSP 是原栈位置。这些值先成为栈上的数据，随后 CPU 才开始执行门指定的汇编指令。`cli` 关闭的是可屏蔽设备中断，当前指令自身产生的无效指令异常仍会到达这里。

在本章的 64 位入口中，CPU 将接收现场的栈位置向下对齐到 16 字节，再保存 SS、原 RSP、RFLAGS、CS、RIP。即使没有跨权限换栈，原 SS/RSP 也在现场中。栈向低地址增长，压入以后从当前 RSP 向高地址看，顺序便是 RIP、CS、RFLAGS、原 RSP、SS。`ud2` 不附带硬件错误码，我们的 `TRAP 6` 再压入值为零的 error，最后压入向量 6。

入口还要保住被打断代码的通用寄存器。异常可能发生在任意指令处，编译器没有像调用普通函数那样预先安置这些值。公共汇编先压 RAX，最后压 R15，新的 RSP 因而指向保存的 r15；C 结构也从 r15 开始，依次覆盖这片连续栈内存。15 个通用寄存器、vector/error 两格和 CPU 的五格现场，一共占 `22×8=176` 字节。

另一种演示会触发一般保护异常 GP，向量为 13。CPU 在它的 RIP 前面另压硬件错误码，入口只需再压 vector 就能取得相同布局。是否补零由各入口决定；公共保存和 C 读取都只面对下面这一种布局。表中偏移以最终交给 C 的现场指针为零，每格八字节：

| 相对现场起点 | 保存内容 | 写入者 |
| --- | --- | --- |
| 0–112 | r15 到 rax，共 15 个通用寄存器 | 公共汇编，按压栈的反向排列 |
| 120 | vector，UD 为 6、GP 为 13 | 各向量入口 |
| 128 | error，UD 补 0、GP 使用硬件值 | UD 入口或 CPU |
| 136、144、152 | RIP、CS、RFLAGS | CPU |
| 160、168 | 故障前的 RSP、SS | CPU |

C 声明不会再复制一份现场。稍后传给处理函数的指针 f 指向这片栈内存，`f->rip` 直接读取偏移 136 的八字节，改写它也会改变异常返回所用的地址。

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

现场结构说明了入口需要交出什么。现在写汇编完成两项连接：初始化时把表的位置交给 CPU；异常到来时把栈上的现场交给 C。下面先写装表函数，它由稍后的 C 初始化代码调用，所需表对象也在后面建立。

### 把内核表的位置交给处理器

`lgdt` 与 `lidt` 各读取一份十字节描述：两字节界限和八字节表地址。界限是最后一个有效字节的偏移。CPU 记住这些值，后续查表访问的仍是那些地址所指的内存，因此表本体要在初始化返回以后继续存在。

加载 GDT 只更新表的位置，当前 CS 的属性还需要重新装入。代码先压选择子 8，再压同一文件内标签 `1` 的地址，由远返回 `lretq` 依次取出 RIP 和 CS，继续执行标签处的指令。它借用一次远返回来重新选择代码段；普通 `ret` 只取返回地址，做不到这项更新。DS、ES、SS 随后采用数据段选择子 16，仍使用第 06 章建立的内核栈。

装表函数还用 `ltr` 选择一份 TSS 记录，供后面的双重故障备用栈使用；它在 GDT 中占两项，选择子为 24。先把装表动作写齐，再继续 UD 的保存与 C 调用；备用栈的地址和硬件取用方式在创建静态表时展开。调用本函数的 C 初始化尚未写出，完成本章全部编辑以后再运行。

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

### 向量入口把两种硬件现场交给同一个 C 函数

装表函数已经知道如何接收 GDT、IDT 的描述，接下来准备要填进 IDT 的入口地址。`TRAP` 是汇编宏：汇编器将它展开成各向量的机器码。AT&T 源/目标次序、寄存器和立即数前缀沿用第 06 章；`\number` 以本次编号拼出标签，`.if` 决定是否生成补零。`TRAP 6` 补 error=0，`TRAP 13,1` 沿用 CPU 的错误码，两者再跳到 `airi_trap_common`。

公共入口先用 `cld` 清方向标志，使随后的 C 满足 ABI；故障前的 RFLAGS 已由 CPU 留在现场中。15 次压栈以后，RSP 指向 r15。`movq %rsp,%rdi` 将这个地址作为 `airi_trap_dispatch` 的第一个实参，`movq %rsp,%r12` 再留一份现场起点。此刻 RDI、R12 和 RSP 三个寄存器中的地址相同，原来被打断代码的 RDI、R12 已经另存在栈中。

随后 `andq $-16,%rsp` 只把供 C 调用使用的栈位置向低地址对齐，RDI 中的指针没有跟着改变。`call` 继续在它下面压普通返回地址，C 的局部变量也使用下面的空间；上方那份 176 字节现场保持原位。System V ABI 要求被调用的 C 保留 R12，所以 C 返回后，`movq %r12,%rsp` 能丢弃对齐空隙，把当前栈重新定位到 r15。这里始终在同一份内核栈上，只是当前栈顶的位置发生了变化。

接着汇编逆序 `pop` 通用寄存器，恢复原来的 R12 和 RDI，略过 vector/error 两格，使 RSP 再次指向 CPU 保存的 RIP。`iretq` 才从这里取回 RIP、CS、RFLAGS、原 RSP、SS，恢复故障前的执行环境。如果 C 已经改写现场中的 rip，恢复的是新地址；若原样返回 `ud2`，同一条无效指令还会再次产生异常。

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
TRAP 255 /* 未单独登记的 IDT 项进入统一停止入口。 */
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

公共入口已经接好保存、调用和恢复，接下来让同一次普通调用确实经过它。`airi_probe_ud` 由 C 调用，在 `airi_probe_ud_site` 执行 `ud2`；CPU 保存的 RIP 应当就是这个地址。另一个标签 `airi_probe_ud_resume` 位于它后面，那里只有 `ret`。处理函数稍后会把现场中的 RIP 改到这个精确标签，使异常返回以后再返回原来的 C 调用者。

GP 探针按同样方式保存 site/resume。它将 0xffff 放入 AX，再尝试装入数据段寄存器 DS。这个选择子的 TI 位选择局部描述符表，当前没有可接受它的记录，因而装载产生一般保护异常；失败后原 DS 仍有效。恢复标签同样用 `ret` 结束普通调用。进入探针、发生故障和恢复执行是三处位置，只有 site 才应与硬件保存的 RIP 相等。

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

<a id="有报告不等于任意故障都能恢复"></a>
<a id="有报告，不等于任意故障都能恢复"></a>

## 让表持续存在，让恢复只消费一次登记

汇编已经能把当前栈上的现场交给 C，也能按 C 留下的值返回。现在为这条路径建立真正的表对象，让初始化把入口地址交给 CPU。

<a id="trap-stack"></a>

### 原栈无法承接故障时，由硬件先换备用栈

UD、GP 演示继续使用当前内核栈。如果交付某些异常时又发生了无法正常交付的异常，CPU 会报告双重故障 DF，向量为 8。这发生在进入普通 C 处理之前，当前栈也可能已经不能承接压栈；因此 DF 入口需要让 CPU 在写现场之前就取得另一个栈顶。

任务状态段 `TSS` 是硬件规定布局的一份内存记录，其中的中断栈表 IST 有七个备用栈顶。这里另留 16 KiB 数组 `double_fault_stack`，把数组右端地址写进 `tss.ist[0]`，再令 `idt[8]` 的 ist 字段为 1。CPU 查 DF 门时读到这个非零编号，就从 TSS 的 IST1 取新 RSP，转到备用栈上压入旧 SS/RSP 及其余硬件现场，再进入 `airi_trap_8`。从公共汇编开始，仍按刚才的一种布局处理；现场中的 rsp 保存原来那份栈的位置。

门中的编号 1 对应 C 数组下标 0；编号 0 表示不指定 IST，UD、GP 在当前同权限情形下继续沿原栈进入。CPU 通过 `ltr` 选中的 TSS 描述符找到这份记录，故而 GDT 的两项 TSS 描述符、TSS 本体和备用栈缺一不可。本章只准备这条硬件切栈路径，没有创建任务或调度器，也未通过破坏原栈实测 DF。

### 静态表持续存在，短描述只负责装表

GDT、IDT、TSS 和独立栈都放在静态区，在内核运行期间持续存在；`table_pointer` 只是装表时传递界限与地址的短记录。`packed` 禁止编译器在字段之间插入填充，使 `lgdt/lidt` 读到的十字节符合硬件格式。局部描述结束寿命以后，CPU 保存的表地址仍指向静态表。

`idt_gate` 占 16 字节，把入口地址的低 16 位、中 16 位、高 32 位分别放入 low、middle、high；CPU 按硬件格式组合出同一个 64 位地址。selector 取代码段选择子 8，ist 则按刚才的栈选择填写。剩下的 flags 是一个八位属性字节，CPU 按固定位位置解释它。

<a id="trap-gate-encoding"></a>

本章要一个存在、DPL0 的中断门。下表的位号相对 flags 字节，最低位为 0；把各字段移到位置以后按位或，就得到代码中的 `0x8e = 0x80 | 0x0e`：

| flags 的位 | 字段 | 本章取值与作用 | 对字节的贡献 |
| --- | --- | --- | --- |
| 7 | P，存在位 | 1，允许 CPU 使用这个门 | `1<<7 = 0x80` |
| 6–5 | DPL，门的权限级别 | 00，采用内核的级别 0 | `0<<5 = 0` |
| 4 | 固定零位 | 0，符合门描述符格式 | `0` |
| 3–0 | Type，门类型 | 二进制 1110，在当前模式中表示 64 位中断门 | `0x0e` |

DPL 约束软件主动用中断指令进入门的权限，本章由 CPU 产生的故障按异常规则交付。选择中断门还使 CPU 在进入时清 IF，故障前的标志已保留在栈中，之后由 `iretq` 恢复。门类型和字段位置见 [Intel SDM 3A，6.14.1 节及图 6-8，253668-082US](https://cdrdv2-public.intel.com/812386/253668-sdm-vol-3a.pdf#page=218)。

`task_state` 对应 104 字节 TSS，GDT 描述字节 0x89 表示存在的可用 64 位 TSS，`ltr` 加载后硬件将其标为忙。三个 `_Static_assert` 分别核对 TSS、门和 176 字节异常现场的大小，布局不符就先拒绝编译。

除了表，C 还保存当前演示的登记：故障地址 site、恢复地址 resume、预期向量和错误码。登记仅在一次同步探针调用期间有效。处理函数要求故障地址、向量和错误码完全匹配，才会改写保存的 RIP，并撤销这次登记。一般错误没有这样的恢复位置和状态约定，报告后就进入 panic。

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

### 把入口地址、代码段和栈选择写入硬件表

先填 GDT：`gdt[1]`、`gdt[2]` 分别描述代码和数据，选择子仍为 8、16。两项都是八字节描述符，代码将各字段预先组成一个 uint64_t 常量。先看字段位置，再把实际取值装回去，便能从属性得到 `0x00af9a000000ffff` 和 `0x00cf92000000ffff`。

<a id="trap-descriptor-encoding"></a>

| 八字节描述符的位 | 保存的字段 | 代码段 gdt[1] | 数据段 gdt[2] |
| --- | --- | --- | --- |
| 15–0 | 段界限的低 16 位 | `0xffff` | `0xffff` |
| 39–16 | 段基址的低 24 位 | `0` | `0` |
| 47–40 | access，存在、权限和类型字节 | `0x9a` | `0x92` |
| 51–48 | 段界限的高 4 位 | `0xf` | `0xf` |
| 55–52 | flags，粒度和执行属性四位 | `0xa` | `0xc` |
| 63–56 | 段基址的高 8 位 | `0` | `0` |

access 内部仍从最低位 0 编号。两项的位 7 都是 P=1，位 6–5 是 DPL=00，位 4 是 S=1，表示普通代码或数据描述符。低四位 Type 再区分用途：代码段的 1010 表示可执行、非一致、可读、初始访问位为零；数据段的 0010 表示数据、普通向上扩展、可写、初始访问位为零。向上扩展描述段的合法偏移范围，压栈仍按 RSP 向低地址增长。于是代码 access 为 `0x80 | 0x10 | 0x0a = 0x9a`，数据 access 为 `0x80 | 0x10 | 0x02 = 0x92`。CPU 使用段时可以把 Type 最低位的访问位 A 设为 1。

flags 四位按从高到低排列为 G、D/B、L、AVL。代码段取 1010：G=1 使用传统的 4 KiB 界限粒度，L=1 选择 64 位代码，此时 D/B 必须为 0，AVL=0 不保存额外软件状态。数据段取 1100：G=1、D/B=1、AVL=0，对应代码 L 的位置在数据描述符中保留为零；D/B 保留常用的 32 位数据/栈属性，不会把正在执行的 64 位代码改成 32 位。两段编码的界限均为 `0xfffff`，按传统粒度换算为 `0xffffffff`；本章已经在 64 位模式，CS、DS、ES、SS 的基址和界限不承担当前地址范围的隔离，内存访问继续使用现有页表。属性位置和含义见 [Intel SDM 3A，3.4.5 节、图 3-8 与表 3-1](https://cdrdv2-public.intel.com/812386/253668-sdm-vol-3a.pdf#page=102)。

将表中的非零项左移到目标位，可以重新组成源码常量：代码段为 `0xffffull | (0x9aull<<40) | (0xfull<<48) | (0xaull<<52) = 0x00af9a000000ffff`，数据段只把 access 换成 0x92、flags 换成 0xc，就得到 `0x00cf92000000ffff`。演算中的 ull 后缀保证移位使用 64 位无符号数，源码再将组合结果直接写成一个常量。

接着为 TSS 填 `gdt[3]` 和 `gdt[4]`。它们共同组成一个 16 字节的 64 位 TSS 描述符，选择子为 24。base 取 `&tss` 的内核虚拟地址，CPU 将沿这个地址读取 IST；limit 是最后一个有效字节的偏移，`104−1=103=0x67`。TSS 与普通代码段的用途不同：描述符要保留完整 64 位基址，界限按字节计，属性中的 S 必须为零。

| 写入位置 | 字段及本章取值 | 对应的 C 表达式 |
| --- | --- | --- |
| gdt[3] 的 15–0 位 | TSS 界限低 16 位，0x67 | `limit` |
| gdt[3] 的 39–16 位 | base 的 23–0 位 | `(base & 0xffffff) << 16` |
| gdt[3] 的 47–40 位 | P=1、DPL=00、S=0、Type=1001，合成 0x89 | `0x89ull << 40` |
| gdt[3] 的 51–48 位 | 界限高四位，0 | 本章 limit 很小，这些位保持零 |
| gdt[3] 的 55–52 位 | G=0 按字节计界限，其余属性及保留位为零 | 不加入非零项 |
| gdt[3] 的 63–56 位 | base 的 31–24 位 | `((base >> 24) & 0xff) << 56` |
| gdt[4] 的 31–0 位 | base 的 63–32 位 | `base >> 32` |
| gdt[4] 的 63–32 位 | 保留零位 | 右移后只剩 32 位有效数值，高位自然为零 |

因此 `gdt[3]` 的四项按位或，分别送入界限、地址低 24 位、属性字节、地址下一字节，字段彼此不重叠；`gdt[4]` 再放地址高 32 位。`limit` 只有 0x67，可以直接并入低 16 位，无须另搬高四位。属性字节为 `0x80 | 0x09 = 0x89`，Type=9 表示可用的 64 位 TSS，`ltr` 加载后硬件把类型标成忙。这个 16 字节布局见 [Intel SDM 3A，8.2.3 节及图 8-4](https://cdrdv2-public.intel.com/812386/253668-sdm-vol-3a.pdf#page=271)。

描述符使 CPU 找得到 TSS，`tss.ist[0]` 再让它找得到独立栈顶。TSS 中的 rsp 数组留给后续权限切换，本章没有用户态；I/O 权限位图也没有建立，io_offset 放到记录末尾。

初始化随后遍历全部 256 个 IDT 槽位。前 32 项从 `airi_trap_entries` 取得对应异常入口，其他项先指向统一停止入口 255；门中的软件编号由那个入口固定，供 C 报告。`idt[6]` 因而指向 UD 探针要经过的入口，只有 `idt[8]` 的 ist 为 1。局部 gdtr/idtr 把静态表地址交给刚写好的 `airi_tables_load`，完成 GDT、段寄存器、TSS 和 IDT 的装载。

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

### 从现场报告到已登记的恢复地址

表安装完成后，`ud2` 可以经 `idt[6]` 和公共汇编将现场地址交给参数 f。处理函数从 f 读取 vector、error、RIP、CS 和原 RSP，串口记录对应的是这一次被打断的执行。另读的 CR2 保存 CPU 最近一次页故障的访问地址，只有向量 14 的报告才把它解释为本次访问地址；UD、GP 中它只是附带的旧状态。

随后比较 f 的 RIP、vector、error 与当前登记。匹配就直接把栈中那格 rip 改为 expected_resume，清掉 expected_site 并置 recovered。这里只修改返回数据，CPU 此刻仍在处理函数里；C 的普通 `return` 回到公共汇编，汇编恢复寄存器并执行 `iretq` 后，才真正进入恢复标签。没有匹配时，`airi_panic` 先报告再停止，汇编不会继续执行异常返回。

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

### 一次普通调用怎样经过故障、报告和恢复

调用方先把 UD 探针、site/resume、向量 6 和错误码 0 交给 `demonstrate`。它发布这次登记、清 recovered，再调用 probe。`ud2` 触发后，处理函数改写 RIP，`iretq` 进入 resume，那里执行 `ret`，于是同一次 probe 调用终于返回。此时 recovered 必须为真，expected_site 必须已被消费；随后才进行第二项。

GP 使用向量 13 和错误码 0xfffc。段选择子的高位索引和 TI 表标志保留到错误码；最低两位另按外部来源 EXT 和 IDT 标志编码，不沿用选择子的请求权限 RPL。本次错误来自当前 DS 装载，两位均为零，所以从 0xffff 得到 0xfffc。失败的装载没有改变原 DS，恢复标签可以直接执行 `ret`。处理函数核对完整错误码，因而不会把同地址的其他保护故障也当作这次演示。

`airi_trap_demo` 等两项都返回以后，才发送 resumed=2。独立的 fatal 入口先清 expected_site，再执行相同 `ud2`；处理函数仍会报告向量 6，但这次没有恢复登记，会走停止路径。

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

现在这条故障路径有了全部组成：C 定义表和恢复条件，汇编连接 CPU 现场与 C，探针给出确定的故障位置。工程还需要把两个实现文件链接进去，再由主函数先安装表、后触发探针，才能实际观察它们。

### 构建同时包含 C 表实现与汇编入口

Make 的 C 源码清单加入 trap.c，对象清单另加入 traps.o。原 `-mno-red-zone` 继续生效：异常会向当前 RSP 下方写现场，这个区域必须留给栈，不能同时被编译器当作隐含临时空间。`-mgeneral-regs-only` 将内核生成的代码限制在普通寄存器内；当前入口只保存这组寄存器，还没有浮点与 SIMD 状态的保存路径。

正常构建自动演示两个恢复点。另一个 `trap-fatal` profile 在自己的产物目录定义故障宏，正常演示以后再触发一次未登记 `ud2`；这次应报告 PANIC，并在发送 READY 以前停止。缺少必需启动信息的路径则仍在引导适配阶段拒绝，尚未安装这些表。

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

### 主函数先安装入口，再调用两个探针

主函数从引导适配取得启动描述，验证通过后调用 `airi_trap_init`。此后 CPU 已使用内核自己的异常表，`airi_trap_demo` 才能触发两次故障。两次恢复都回到同一普通调用链，随后继续已有会话初始化与输入循环；串口设备、文字环和可选绘图仍由原来的模块持有。

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

<a id="根据现场与结果判断不能只等进程存活"></a>
<a id="根据现场与结果判断，不能只等进程存活"></a>

## 收到两份故障现场以后，再确认恢复或停止

主机启动工具收取的仍是本轮串口日志，它先删旧记录，等待有截止时间，结束时由 finally 回收自己的 QEMU。正常镜像先输出 UD、GP 两条 EXCEPTION：这是处理函数已取得现场。之后 `TRAP_DEMO_OK` 的 resumed=2 表示两次探针都经过 `iretq` 和恢复标签中的 `ret`，回到了调用者；最后 READY 表示启动继续完成。`trap-fatal` 镜像还会输出第三次故障和 `unhandled CPU exception`，随后停在内核中，主机工具负责结束仍然存在的 QEMU 进程。

这使同一条入口的两种结果都可观察：登记的故障修改现场后返回，未登记的故障报告后停止。下面给已有启动观察增加这些条件，QMP 通路、超时和清理仍沿用 K12。

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

### 让统一入口也运行未登记故障镜像

`PROFILES` 同时提供 CLI 的合法配置和完整回归要启动的镜像。将 trap-fatal 放入其中以后，日常回归才会构建它、收取停止结果；前面的 Make 宏只负责改变这个镜像中的代码。

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

### 分开运行正常恢复与未登记停止

全部编辑完成后，从统一入口分别构建 normal 和 trap-fatal。下面整个运行块交给一个 `sh -eu` 执行：任一构建失败就结束，不会继续拿原目录中的旧镜像做观察。两个 check 各自启动一台 QEMU，并使用对应目录中的新日志；第一次运行的 READY 不会成为第二次运行的结果。

<!-- run -->
```sh
sh -eu <<'SH' # 本组任一构建或观察失败即停止，结束词前的内容原样交给sh。
./airios build  # 构建普通串口镜像。
./airios build --profile trap-fatal  # 独立构建未处理故障镜像。
PYTHONPATH=scripts python3 - <<'PYTRAP'  # 引用结束词使Shell原样交出其中的Python源码。
from boot import check  # 使用 K12 已解释的有界启动观察。
check('normal')  # 应先出现两个 EXCEPTION，再出现 TRAP_DEMO_OK 和 READY。
check('trap-fatal')  # 应第三次出现向量 6，再出现 PANIC，绝不能出现 READY。
PYTRAP
SH
```

2026-10-15 的检查点记录中，正常镜像先报告向量 6/error=0、向量 13/error=0xfffc，CS 都为 8，随后输出 `[TRAP_DEMO_OK] ud=6 gp=13 resumed=2` 和 P1 READY。fatal 镜像则在第三次 UD 后停止。地址随本次链接布局变化，报告中的 RIP 应当用同一次构建的 ELF 定位。两组日志分别保存在 `artifacts/boot-normal`、`artifacts/boot-trap-fatal`。

需要观察现场时，在 `airi_trap_dispatch` 设置断点，`p *f` 读取其参数指向的结构。尚未执行 C 序言时，第一实参仍在 RDI，可用 `p *(struct airi_trap_frame *)$rdi`，或先用 `stepi` 进入已建立局部符号的位置。再以 `x/i` 反汇编现场中的 RIP，UD 应指向 `ud2`，GP 应指向装载 DS 的指令。`idt[8].ist` 和 `tss.ist[0]` 则分别显示 DF 的栈选择和栈顶。

至此，后续页分配和页表代码发生错误时，内核已有一条取得现场、输出故障位置并停止的路径。只有本章登记的两个探针具备恢复约定；它们不改变一般故障的停止规则。L01 的 Linux 用户程序由已有内核接收故障，再按系统规则处理，例如向进程发信号；当前 AiriOS 尚无用户进程，未处理异常会停止整个单核内核。

异常交付、64 位栈和 TSS 的规则见 [Intel 系统编程手册](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html)。本章仍关闭普通设备 IRQ，NMI、机器检查恢复和损坏栈上的日志输出均未验证。接下来先用这条诊断路径支撑物理页与页表实现。
