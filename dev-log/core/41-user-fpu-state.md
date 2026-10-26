# K41 · 让浮点与向量寄存器也属于当前进程

从 K40 继续。时钟已经可以在任意用户指令间切换进程，但目前返回现场只包含通用寄存器。普通 C 的 `double result = input * scale` 在 x86-64 上就会使用另一组寄存器；如果父子只恢复 RAX、RSP 和 RIP，恢复执行时可能继续使用另一个进程留下的浮点值。单 CPU 也需要隔离这部分状态。

本节沿 `sh → simd → fork → 各自计算 → exec simd-reset → wait → sh` 补齐这条边界。用户 C 开放 SSE2，内核 C 保持只用通用寄存器。每进程取得独立状态页，进入内核先保存，返回选定用户前恢复；fork 复制，成功 exec 重新初始化，失败 exec 保留，退出按所有权归还。

本节仍为单核、八进程、静态 ELF；不引入线程、TLS、AVX 或动态链接。下面的内核、探针和构建修改全部完成后再运行，图形支线没有前置。

<a id="fpu-state-shape"></a>
## 一次乘法除了 RAX，还需要保存什么

XMM0 至 XMM15 是十六个 128 位寄存器。SSE2 的标量 double 乘法 `mulsd` 使用其中的低 64 位，其他指令可以一次处理多个值，所以它们也叫向量寄存器。MXCSR 另外记录 SSE 的舍入控制、异常掩码和异常标志。异常掩码为 1 时，相关浮点异常通常只留下标志而不产生 CPU 故障；它与 PIC 屏蔽设备 IRQ 是两套无关的控制。

更早的 x87 单元还有八个 80 位数据寄存器，按 ST(0) 栈顶访问。压栈会改变栈顶位置，标签记录哪些位置为空；控制字指定精度、舍入和异常掩码，状态字记录结果与栈顶。x87 和 MMX 复用这组数据寄存器，不能只保存 XMM 后就认为所有旧浮点代码都安全。这里用 x87 留一个非空值，并给父子不同的舍入模式，实际检查数据与控制均属于正确身份。

PIT 打断父的计算时，XMM、x87 和控制字仍在 CPU 中。`FXSAVE64` 把这组实际值写入父自己的512字节内存，不会清空硬件寄存器；因此仅保存以后，CPU 还没有获得孩子的状态。调度器先确定下一个身份，再由 `FXRSTOR64` 从它的快照恢复，随后才返回用户执行。

两条指令要求内存起点16字节对齐，并使用同一种保存格式；64后缀选择长模式的指针字段。它们只搬运这部分 CPU 状态，不替调度器选择进程或切换 CR3。格式和对齐要求依据 [Intel 指令手册的 FXSAVE/FXRSTOR 条目](https://cdrdv2-public.intel.com/812383/253666-sdm-vol-2a.pdf)。本节使用的布局为：

| 字节偏移 | 保存的实际状态 | 当前消费者 |
| --- | --- | --- |
| 0–5 | x87 控制字、状态字、压缩标签 | 检查默认控制和空栈 |
| 6–23 | 操作码、指令指针、数据指针 | 新程序清零，避免沿用旧映像地址 |
| 24–31 | MXCSR 与可用位掩码 | 保存当前 SSE 环境，恢复使用合法值 |
| 32–159 | 八个 x87 槽，每槽实际值 10 字节、跨度 16 字节 | 检查空标签后也没有旧数据 |
| 160–415 | 十六个 XMM，每个 16 字节 | 包括高组 XMM8 至 XMM15 |
| 其余 | 保留或指令不写的区域 | 创建时先清整页，不赋予额外语义 |

状态页通过内核 HHDM 访问，不映射为 USER，也不接受用户提交任意恢复映像。用户只能执行 CPU 允许的浮点指令；系统调用号没有增加。4 KiB 页满足 16 字节对齐，实际只复制前 512 字节，剩余部分不用。此页与用户数据页、页记录和陷入栈分别持有，不占 1024 页用户数据预算。

先新建 `include/airi/fpu.h`，将 CPU 状态操作限制在架构适配层。

<!-- write: include/airi/fpu.h -->
```c
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
```

<a id="fpu-enable-template"></a>
## 先允许指令执行，再建立可恢复的初始状态

CPU 支持某条指令，不代表当前控制寄存器允许它执行。CPUID 叶 1 的 EDX 第 24、25、26 位分别报告 FXSR、SSE、SSE2；先查询叶 0 的最大编号，再确认本节需要的整组能力。它们都是 CPU 查询结果，不是磁盘或内存里的配置字段。

CR0.EM 用于禁止硬件浮点路径，TS 可令浮点指令触发“设备不可用”异常；本实现清两者，选择每次用户陷入都保存的直接方式，不建立延迟 #NM 分配。MP、NE 则设为 1。写入前先读取旧 CR0，只改这些位，不能意外关闭分页或写保护。CR4.OSFXSR 允许保存与恢复 SSE 状态，OSXMMEXCPT 开启相应异常路径。OSXSAVE 保持关闭，用户也用 `-mno-avx` 编译，避免使用当前 512 字节格式未保存的 AVX 高半状态。

新进程的初始状态不是随手填一个全零数组。控制字应为 0x037f，MXCSR 应为 0x1f80，数据才是零。`fninit` 会将标签设为空，却不保证旧 x87 数据值消失，所以初始化汇编先压八次零覆盖数据，再重新标空；十六个 XMM 逐一清零，最后用真实 FXSAVE64 形成合法模板。

新建 `kernel/arch/x86_64/fpu_seed.S`。`.rept 8` 与 `.endr` 是汇编器的重复展开指令，生成八条 fldz，并非运行时循环；pxor 对同一寄存器异或得到 128 位零。这个文件是内核中明确使用浮点和 XMM 的专用初始化段，其他内核 C 仍禁止编译器使用它们。

<!-- write: kernel/arch/x86_64/fpu_seed.S -->
```asm
.section .text /* 启动CPU状态清理只在独立汇编中使用浮点与XMM。 */
.global airi_fpu_seed /* rdi指向内核16字节对齐的512字节模板。 */
.type airi_fpu_seed,@function /* 调试器可定位这段实际状态初始化。 */
airi_fpu_seed: /* 能力和CR0/CR4已由C初始化检查。 */
    fninit /* 先建立空x87栈与默认控制字，避免旧异常干扰。 */
    .rept 8 /* 八个物理x87数据寄存器都需要清除，空标签本身不清数据。 */
    fldz /* 每次压一个真正的正零，八次覆盖全部数据寄存器。 */
    .endr /* 结束八次压栈展开。 */
    fninit /* 数据仍为零，标签重新标空，控制字恢复0x037f。 */
    ldmxcsr airi_fpu_default_mxcsr(%rip) /* SSE异常掩码和舍入采用0x1f80，状态标志为零。 */
    pxor %xmm0, %xmm0 /* 逐个清除用户未来可读到的128位寄存器。 */
    pxor %xmm1, %xmm1 /* 不依赖固件退出时留下的值。 */
    pxor %xmm2, %xmm2 /* 新进程不继承旧向量数据。 */
    pxor %xmm3, %xmm3 /* 保留统一零模板。 */
    pxor %xmm4, %xmm4 /* 同样覆盖低组寄存器。 */
    pxor %xmm5, %xmm5 /* 所有128位都清零。 */
    pxor %xmm6, %xmm6 /* 不只清低64位。 */
    pxor %xmm7, %xmm7 /* 低八个寄存器处理完毕。 */
    pxor %xmm8, %xmm8 /* 64位用户还可访问高八个寄存器。 */
    pxor %xmm9, %xmm9 /* 同一模板覆盖完整64位模式集合。 */
    pxor %xmm10, %xmm10 /* 不将高组遗漏在上下文之外。 */
    pxor %xmm11, %xmm11 /* 初始向量全部为零。 */
    pxor %xmm12, %xmm12 /* 后续进程分别复制此模板。 */
    pxor %xmm13, %xmm13 /* 不能只处理普通C常用的前几个寄存器。 */
    pxor %xmm14, %xmm14 /* 清除高组中的倒数第二项。 */
    pxor %xmm15, %xmm15 /* 新用户探针会明确检查这一高寄存器。 */
    fxsave64 (%rdi) /* 只保存，不再次清空；得到可用FXRSTOR64恢复的映像。 */
    ret /* 返回仍只使用通用寄存器的内核C。 */
.size airi_fpu_seed,.-airi_fpu_seed /* 固定汇编函数边界。 */
.section .rodata /* 默认控制值由内核只读区持有。 */
.balign 4 /* MXCSR输入按四字节对齐。 */
airi_fpu_default_mxcsr: /* 没有用户指针参与。 */
.long 0x1f80 /* 默认舍入到最近、所有SSE浮点异常掩码开启。 */
.section .note.GNU-stack,"",@progbits /* 不需要可执行栈。 */
```

新建 `kernel/arch/x86_64/fpu.c`。模板数组的 `_Alignas(16)` 要求编译器将起点放在 16 字节边界，不能用任意局部 uint8_t 数组代替。初始化先清数组，保存后再清操作码与两项指针字段；这些字段不决定当前数据的数值，却可能留下固件或旧程序地址。

内嵌汇编的 `%0` 引用约束列表的第一个操作数，`"r"` 要求编译器用通用寄存器传地址，括号表示访问该地址的内存；`"memory"` 告诉编译器不能把相关内存操作跨过保存或恢复。它没有替内核自动保存 C 的活 XMM 值。本实现依赖内核 C 的 general-only 限制，因此恢复用户 XMM 后，C 返回和公共陷入汇编都不会再覆盖它们。

<!-- write: kernel/arch/x86_64/fpu.c -->
```c
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
```

在 main.c 安装异常入口后、建立任何用户之前调用 `airi_fpu_init`，能力不足就明确 panic 并停止。服务模式也需要这一步，因为 init、shell 和后续 C 程序都使用同一个用户运行边界。此处只创建静态模板，不依赖尚未初始化的物理页池。下面接入包含与初始化两处，原有诊断与服务选择保持。

包含区接入 CPU 状态接口。

本章每组替换的第一块只用于定位旧文本，敲入第二块替换它，不把两块同时加入文件。各组按正文顺序完成。

<!-- replace: kernel/main.c -->
<!-- before -->
```c
#include <airi/async.h> // 有界SQ/CQ先在真实设备演示，再接用户。
#include <airi/storage.h> // 正式日志区跨进程和QEMU重启保留。
#include <airi/journal.h> // 追加记录恢复演示独立于RAM文件。
```
<!-- after -->
```c
#include <airi/fpu.h> // 用户浮点状态在首次用户程序以前建立。
#include <airi/async.h> // 有界SQ/CQ先在真实设备演示，再接用户。
#include <airi/storage.h> // 正式日志区跨进程和QEMU重启保留。
#include <airi/journal.h> // 追加记录恢复演示独立于RAM文件。
```

在异常入口安装成功以后、读取启动信息以前初始化状态模板；此时还不需要物理页池。

<!-- replace: kernel/main.c -->
<!-- before -->
```c
    if (error)  // 有具体原因则不能继续读取未经验证的数据。
        airi_panic(error);  // 输出失败原因并停止当前 CPU 执行。
    airi_trap_init(); // 通过启动校验后先接管异常，不启用设备中断。
    airi_log("[BOOT_MODE] name=%s", diagnostics ? "diagnostic" : "service"); // 当前镜像的启动策略明确可见。
    if (diagnostics) // 服务启动只安装异常入口，不主动触发错误。
        airi_trap_demo(); // 诊断配置验证两种异常栈形状。
```
<!-- after -->
```c
    if (error)  // 有具体原因则不能继续读取未经验证的数据。
        airi_panic(error);  // 输出失败原因并停止当前 CPU 执行。
    airi_trap_init(); // 通过启动校验后先接管异常，不启用设备中断。
    if (!airi_fpu_init()) // 诊断和常驻启动都必须拥有完整用户CPU状态。
        airi_panic("CPU requires FXSR, SSE and SSE2 user state support"); // 不把不支持目标带入用户ELF。
    airi_log("[BOOT_MODE] name=%s", diagnostics ? "diagnostic" : "service"); // 当前镜像的启动策略明确可见。
    if (diagnostics) // 服务启动只安装异常入口，不主动触发错误。
        airi_trap_demo(); // 诊断配置验证两种异常栈形状。
```

<a id="fpu-process-owner"></a>
## 状态页随进程创建，硬件状态随当前身份交接

在 user_process 增加一个物理地址字段 fpu_physical；512 字节数组不直接放进控制块，避免 exec 同时保留 next、old 时扩大陷入栈占用。create_fpu 取得一页，清整页，再复制模板。普通 ELF 与固定汇编映像都必须先构造这个所有权，才能成为可运行进程；中途失败沿 cleanup 归还已完成部分。

一次父进程 fork 的具体顺序是：

1. 父在用户态留下 XMM 和 x87 值，int80 先保存通用寄存器，再进入 airi_user_trap。
2. 仍以父为 current，把真实浮点状态保存到父的状态页；之后才处理 fork。
3. 子取得独立状态页，512 字节从父快照复制。子和父可以暂时字节相同，但物理页地址不同。
4. 调度选择目标身份、TSS 和 CR3；所有返回用户的路径经 airi_user_resume，在检查返回 RIP/RSP 和标志后恢复该身份的状态。
5. iretq 返回，原来的乘法或探针从自己的寄存器继续。下一次 PIT 打断它时，先保存这个实际身份，才能选别人。

PIT 入口、设备 IRQ 的 CS3 入口和普通用户系统调用/故障入口都保存。CS0 仍不抢占内核；这时没有新的用户所有者，内核又不使用浮点，所以不把某个内核中间状态写进进程。设备 IRQ 可以把用户交给普通服务栈，恢复时也必须走同一入口，不能只在 timer 调度函数里补保存。

exec 的事务顺序沿 K23 保留：next 独立准备新根、栈、数据与默认状态页，失败只清 next，旧页和已保存的浮点状态仍在；成功提交 current 的新资源后，才清 old。新 ELF 恢复模板，而不是复制旧用户寄存器。CPU 寄存器保存的是数值，不保存状态页指针；离开旧现场和地址空间后可以归还该页，不需要让它一直活到下一次 FXSAVE。

这些动作需要同时接到状态页的构造与清理、三个用户陷入入口，以及统一返回处。FD、等待、异步和页记录继续沿原路径；下面的局部替换把这几处连接逐个接齐。

接下来按现有 `kernel/process/user.c` 中的位置逐组替换，未列出的前章内容保持。所有映像、用户库与构建接入完成后再运行。

先在文件的包含区接入架构状态接口。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
#include <airi/async.h> // 进程轮次与固定缓冲资源的生命周期。
#include <airi/arch.h> // CPU空闲与用户调度门留在架构适配。
#include <airi/storage.h> // 用户存储请求经过有界副本进入日志服务。
```
<!-- after -->
```c
#include <airi/fpu.h> // 每进程拥有一份非用户映射的浮点/SSE状态。
#include <airi/async.h> // 进程轮次与固定缓冲资源的生命周期。
#include <airi/arch.h> // CPU空闲与用户调度门留在架构适配。
#include <airi/storage.h> // 用户存储请求经过有界副本进入日志服务。
```

控制块只新增状态页的物理地址，512字节状态仍放在页中。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    bool async_owned; // 本控制块是否持有当前PID的异步owner，局部旧exec副本必须清除此所有权。
    struct airi_fd_table files; // exec转移表项，fork增加同一打开对象的引用。
    struct airi_address_space space; // 根和低半区表自有，高半区借用。
    struct airi_kernel_region stack; // 不能在仍使用它的C调用中释放。
    uint64_t records[RECORD_CHUNKS]; // 元数据页的物理地址；各页经HHDM访问，不映射给用户。
    unsigned count, pid, parent, user_ticks; // 资源数量、身份、父PID和本进程实际IRQ数。
```
<!-- after -->
```c
    bool async_owned; // 本控制块是否持有当前PID的异步owner，局部旧exec副本必须清除此所有权。
    struct airi_fd_table files; // exec转移表项，fork增加同一打开对象的引用。
    struct airi_address_space space; // 根和低半区表自有，高半区借用。
    uint64_t fpu_physical; // 独立状态页不塞进exec局部控制块，也不算用户数据页。
    struct airi_kernel_region stack; // 不能在仍使用它的C调用中释放。
    uint64_t records[RECORD_CHUNKS]; // 元数据页的物理地址；各页经HHDM访问，不映射给用户。
    unsigned count, pid, parent, user_ticks; // 资源数量、身份、父PID和本进程实际IRQ数。
```

登记两份探针映像的边界；后面的构建与包装完成以后才会定义它们。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
extern const uint8_t airi_user_rsp_bad_start[], airi_user_rsp_bad_end[]; // 不规范用户返回栈。
extern const uint8_t airi_hello_elf_start[], airi_hello_elf_end[]; // 既有独立用户C程序。
extern const uint8_t airi_process_elf_start[], airi_process_elf_end[]; // 新的fork/wait和抢占工作负载。
extern const uint8_t airi_init_elf_start[], airi_init_elf_end[]; // 用户态常驻监督程序。
extern const uint8_t airi_capacity_elf_start[], airi_capacity_elf_end[]; // 大BSS、深用户栈与跨记录页的fork/exec。
extern const uint8_t airi_memory_elf_start[], airi_memory_elf_end[]; // 用户页面归还与空洞复用工作负载。
```
<!-- after -->
```c
extern const uint8_t airi_user_rsp_bad_start[], airi_user_rsp_bad_end[]; // 不规范用户返回栈。
extern const uint8_t airi_hello_elf_start[], airi_hello_elf_end[]; // 既有独立用户C程序。
extern const uint8_t airi_process_elf_start[], airi_process_elf_end[]; // 新的fork/wait和抢占工作负载。
extern const uint8_t airi_simd_elf_start[], airi_simd_elf_end[]; // 浮点、抢占和继承探针。
extern const uint8_t airi_simd_reset_elf_start[], airi_simd_reset_elf_end[]; // exec新状态观察。
extern const uint8_t airi_init_elf_start[], airi_init_elf_end[]; // 用户态常驻监督程序。
extern const uint8_t airi_capacity_elf_start[], airi_capacity_elf_end[]; // 大BSS、深用户栈与跨记录页的fork/exec。
extern const uint8_t airi_memory_elf_start[], airi_memory_elf_end[]; // 用户页面归还与空洞复用工作负载。
```

在初始目录中加入 simd 与 simd-reset 两个名字。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
static const struct executable executables[] = { // 每份映像字节由内核只读段长期持有。
    {"sh", airi_shell_elf_start, airi_shell_elf_end}, // 独占串口输入的可选用户程序。
    {"init", airi_init_elf_start, airi_init_elf_end}, // 管理前台shell的常驻用户进程。
    {"capacity", airi_capacity_elf_start, airi_capacity_elf_end}, // 预算耗尽后回收并继续运行。
    {"memory", airi_memory_elf_start, airi_memory_elf_end}, // 同一地址空间内释放与重新申请。
    {"orphans", airi_orphans_elf_start, airi_orphans_elf_end}, // 不结束根会话也能回收退出的孤儿。
```
<!-- after -->
```c
static const struct executable executables[] = { // 每份映像字节由内核只读段长期持有。
    {"sh", airi_shell_elf_start, airi_shell_elf_end}, // 独占串口输入的可选用户程序。
    {"init", airi_init_elf_start, airi_init_elf_end}, // 管理前台shell的常驻用户进程。
    {"simd", airi_simd_elf_start, airi_simd_elf_end}, // 真实SSE/x87隔离与fork/exec。
    {"simd-reset", airi_simd_reset_elf_start, airi_simd_reset_elf_end}, // 新ELF入口的默认状态。
    {"capacity", airi_capacity_elf_start, airi_capacity_elf_end}, // 预算耗尽后回收并继续运行。
    {"memory", airi_memory_elf_start, airi_memory_elf_end}, // 同一地址空间内释放与重新申请。
    {"orphans", airi_orphans_elf_start, airi_orphans_elf_end}, // 不结束根会话也能回收退出的孤儿。
```

在 trim_records 结束与 create_stack 之间加入 create_fpu。它只建立新身份的状态页，不把模板恢复到当前 CPU。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
            airi_panic("cannot reclaim user page records"); // 归属错误不能静默复用页面。
        p->records[i] = 0; // 归还后立即撤掉唯一所有权记录。
    }
}
static bool user_access(const struct user_process *p, uint64_t address, size_t bytes, bool write) { // 检查完整用户范围，不直接解引用它。
    if (address >= AIRI_USER_LIMIT || bytes > AIRI_USER_LIMIT - address) // 排除内核地址与右端溢出。
```
<!-- after -->
```c
            airi_panic("cannot reclaim user page records"); // 归属错误不能静默复用页面。
        p->records[i] = 0; // 归还后立即撤掉唯一所有权记录。
    }
}
static bool create_fpu(struct user_process *p) { // 构造器只给空控制块建立独立页所有权。
    p->fpu_physical = airi_memory_take_page(); // 一页足够容纳对齐512字节状态，与用户记录页分别归还。
    if (!p->fpu_physical) // 没有内存就不准备可运行身份。
        return false; // cleanup可回收此前的栈等资源。
    void *state = airi_memory_direct(p->fpu_physical); // 内核HHDM别名，不给用户USER映射。
    airi_memset(state, 0, 4096); // 剩余页字节也不带旧内容。
    airi_fpu_new(state); // 只复制已验证模板，不改变当前用户硬件状态。
    return true; // 页句柄此后由进程清理者持有。
}
static bool user_access(const struct user_process *p, uint64_t address, size_t bytes, bool write) { // 检查完整用户范围，不直接解引用它。
    if (address >= AIRI_USER_LIMIT || bytes > AIRI_USER_LIMIT - address) // 排除内核地址与右端溢出。
```

cleanup 在归还用户数据以后，也收回这张独立状态页；调用者仍须先离开目标根与栈。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
        if (!airi_memory_return_page(owned_at(p, --p->count)->physical)) // 先退到最后一个真正拥有的页。
            airi_panic("cannot return owned user page"); // 归属错误不继续复用内存。
    trim_records(p); // 所有数据页已归还，最后释放保存其VA/PA的记录页。
    if (p->stack.count && !airi_memory_region_free(&p->stack)) // 调用者已经离开目标内核栈，才可销毁它。
        airi_panic("cannot reclaim user kernel stack"); // 必须已经离开目标内核栈，不能在退出者仍使用它时提前回收。
    airi_memset(p, 0, sizeof(*p)); // 不留下可重复释放的根或句柄。
```
<!-- after -->
```c
        if (!airi_memory_return_page(owned_at(p, --p->count)->physical)) // 先退到最后一个真正拥有的页。
            airi_panic("cannot return owned user page"); // 归属错误不继续复用内存。
    trim_records(p); // 所有数据页已归还，最后释放保存其VA/PA的记录页。
    if (p->fpu_physical && !airi_memory_return_page(p->fpu_physical)) // CPU寄存器不保留状态页指针，切离后可归还。
        airi_panic("cannot reclaim user FPU state"); // 不能漏掉独立于用户数据的内核页面。
    if (p->stack.count && !airi_memory_region_free(&p->stack)) // 调用者已经离开目标内核栈，才可销毁它。
        airi_panic("cannot reclaim user kernel stack"); // 必须已经离开目标内核栈，不能在退出者仍使用它时提前回收。
    airi_memset(p, 0, sizeof(*p)); // 不留下可重复释放的根或句柄。
```

普通 ELF 准备时，先完成状态页和空用户根；失败仍去同一回滚出口。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    }
    if (make_stack && !create_stack(p)) // 内核陷入栈先完成，再借高半区。
        return -12; // -ENOMEM，区域接口已回滚失败。
    if (!airi_memory_user_space(&p->space)) // 创建独立低半区。
        goto failed; // 后续失败均收回所有已持有资源。
    for (unsigned i = 0; i < view.count; ++i) { // 每段独立赋予对应权限。
        const struct airi_elf_segment *segment = &view.segments[i]; // 元数据已经过统一校验。
```
<!-- after -->
```c
    }
    if (make_stack && !create_stack(p)) // 内核陷入栈先完成，再借高半区。
        return -12; // -ENOMEM，区域接口已回滚失败。
    if (!create_fpu(p) || !airi_memory_user_space(&p->space)) // 浮点页与独立低半区都必须完整建立。
        goto failed; // 后续失败均收回所有已持有资源。
    for (unsigned i = 0; i < view.count; ++i) { // 每段独立赋予对应权限。
        const struct airi_elf_segment *segment = &view.segments[i]; // 元数据已经过统一校验。
```

受控 flat 映像也走相同状态页准备，早期故障程序不能绕过这项所有权。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
static bool prepare_flat(struct user_process *p, const uint8_t *image, size_t bytes) { // 保留受控指令故障回归，不作为ELF加载接口。
    if (!bytes || bytes > 4096 || !create_stack(p)) // 本节旧映像最多一页。
        return false; // 栈创建失败已回滚。
    if (!airi_memory_user_space(&p->space) || !owned_page(p, USER_CODE, AIRI_VM_EXEC) || !owned_page(p, USER_STACK, AIRI_VM_WRITE)) // 分别拥有根、代码和栈。
        goto failed; // 所有权列表可清理全部已成功部分。
    airi_memcpy(airi_memory_direct(owned_at(p, 0)->physical), image, bytes); // 映像内引用保持旧固定布局。
    initial_frame(p, USER_CODE, NULL); // 受控汇编不读取argc/argv。
```
<!-- after -->
```c
static bool prepare_flat(struct user_process *p, const uint8_t *image, size_t bytes) { // 保留受控指令故障回归，不作为ELF加载接口。
    if (!bytes || bytes > 4096 || !create_stack(p)) // 本节旧映像最多一页。
        return false; // 栈创建失败已回滚。
    if (!create_fpu(p) || !airi_memory_user_space(&p->space) || !owned_page(p, USER_CODE, AIRI_VM_EXEC) || !owned_page(p, USER_STACK, AIRI_VM_WRITE)) // 分别拥有根、代码和栈。
        goto failed; // 所有权列表可清理全部已成功部分。
    airi_memcpy(airi_memory_direct(owned_at(p, 0)->physical), image, bytes); // 映像内引用保持旧固定布局。
    initial_frame(p, USER_CODE, NULL); // 受控汇编不读取argc/argv。
```

fork 先给孩子创建自己的状态页和用户根，再复制用户页与父的512字节入口快照。父子状态可以相同，孩子的状态页却是独立所有权；整组准备完成后才继续取得 FD 引用与运行资格。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    struct user_process *child = unused(); // 未发布的新槽。
    if (!child) // 表满没有可分配身份。
        return -11; // -EAGAIN。
    if (!create_stack(child) || !airi_memory_user_space(&child->space)) // 先创建独立内核栈和空低半区。
        goto failed; // 父状态从未改变。
    for (unsigned i = 0; i < parent->count; ++i) { // 包括用户栈和此前申请的堆页。
        const struct user_page *page = owned_at(parent, i); // 每页原VA、权限和物理地址。
        if (!owned_page(child, page->address, page->flags)) // 子页在相同VA，但物理所有权独立。
            goto failed; // 回滚全部子资源，不改变父页面。
        airi_memcpy(airi_memory_direct(owned_at(child, child->count - 1)->physical), airi_memory_direct(page->physical), 4096); // 复制完整页内容，父子以后写入互不影响。
    }
    airi_fd_clone(&child->files, &parent->files); // 页复制全部成功后再取得FD引用，失败cleanup仍能撤销。
    child->initial = *parent_frame; // 用户RIP/RSP和寄存器从同一次fork返回点继续。
    child->initial.rax = 0; // 子分支看到fork返回0。
```
<!-- after -->
```c
    struct user_process *child = unused(); // 未发布的新槽。
    if (!child) // 表满没有可分配身份。
        return -11; // -EAGAIN。
    if (!create_stack(child) || !create_fpu(child) || !airi_memory_user_space(&child->space)) // 先创建独立内核栈和空低半区。
        goto failed; // 父状态从未改变。
    for (unsigned i = 0; i < parent->count; ++i) { // 包括用户栈和此前申请的堆页。
        const struct user_page *page = owned_at(parent, i); // 每页原VA、权限和物理地址。
        if (!owned_page(child, page->address, page->flags)) // 子页在相同VA，但物理所有权独立。
            goto failed; // 回滚全部子资源，不改变父页面。
        airi_memcpy(airi_memory_direct(owned_at(child, child->count - 1)->physical), airi_memory_direct(page->physical), 4096); // 复制完整页内容，父子以后写入互不影响。
    }
    airi_memcpy(airi_memory_direct(child->fpu_physical), airi_memory_direct(parent->fpu_physical), AIRI_FPU_BYTES); // 父陷入时已保存，子复制真实状态而非共享状态页。
    airi_fd_clone(&child->files, &parent->files); // 页复制全部成功后再取得FD引用，失败cleanup仍能撤销。
    child->initial = *parent_frame; // 用户RIP/RSP和寄存器从同一次fork返回点继续。
    child->initial.rax = 0; // 子分支看到fork返回0。
```

统一用户返回恢复所选身份；设备打断用户时则先保存原身份，两个方向分别接入下面位置。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    }
    f->rflags &= ~((3ull << 12) | (1ull << 14) | (1ull << 17)); // 清IOPL、NT和VM。
    f->rflags |= 0x202; // 保留必需bit1并保持用户IF。
    return f; // 返回选中进程的完整现场。
}
struct airi_trap_frame *airi_user_device_irq(struct airi_trap_frame *f) { // 本函数只在真实设备IRQ打断ring3后调用。
    if (!session_active || !current || current->state != USER_RUNNING) // 用户返回必须有明确现场所有者。
        airi_panic("device IRQ without a running user"); // 不把未知栈交给启动继续点。
    if (!airi_async_work()) // 同步启动请求或已收集的旧通知无需切用户身份。
        return airi_user_resume(f); // 用户返回策略仍统一核对。
    current->frame = f; // 保存完整GPR与用户RIP/RSP，稍后从同一指令继续。
```
<!-- after -->
```c
    }
    f->rflags &= ~((3ull << 12) | (1ull << 14) | (1ull << 17)); // 清IOPL、NT和VM。
    f->rflags |= 0x202; // 保留必需bit1并保持用户IF。
    airi_fpu_restore(airi_memory_direct(current->fpu_physical)); // 当前身份已确定，恢复后内核和公共汇编不再使用浮点/XMM。
    return f; // 返回选中进程的完整现场。
}
struct airi_trap_frame *airi_user_device_irq(struct airi_trap_frame *f) { // 本函数只在真实设备IRQ打断ring3后调用。
    if (!session_active || !current || current->state != USER_RUNNING) // 用户返回必须有明确现场所有者。
        airi_panic("device IRQ without a running user"); // 不把未知栈交给启动继续点。
    airi_fpu_save(airi_memory_direct(current->fpu_physical)); // IRQ可打断活XMM/x87值，先保存再决定是否交普通服务。
    if (!airi_async_work()) // 同步启动请求或已收集的旧通知无需切用户身份。
        return airi_user_resume(f); // 用户返回策略仍统一核对。
    current->frame = f; // 保存完整GPR与用户RIP/RSP，稍后从同一指令继续。
```

PIT 入口同样先保存实际被打断身份的状态，再决定轮转。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
struct airi_trap_frame *airi_user_timer(struct airi_trap_frame *f) { // IRQ路径不分配、不回收、不打印，只有状态与根选择。
    if (!session_active || !current || current->state != USER_RUNNING) // CS3的IRQ必须有明确用户所有者。
        airi_panic("user IRQ without a running process"); // 不把未知内核栈交给调度器。
    current->frame = f; // 保存真正被打断的位置，不只保存C ABI寄存器。
    ++current->user_ticks; // 实际CPU用户时间片事件数。
    if (current->user_ticks % 2 == 0) // 每两个实际IRQ尝试一次round-robin。
```
<!-- after -->
```c
struct airi_trap_frame *airi_user_timer(struct airi_trap_frame *f) { // IRQ路径不分配、不回收、不打印，只有状态与根选择。
    if (!session_active || !current || current->state != USER_RUNNING) // CS3的IRQ必须有明确用户所有者。
        airi_panic("user IRQ without a running process"); // 不把未知内核栈交给调度器。
    airi_fpu_save(airi_memory_direct(current->fpu_physical)); // 每个用户时钟入口先保存额外CPU状态，不能只保存GPR。
    current->frame = f; // 保存真正被打断的位置，不只保存C ABI寄存器。
    ++current->user_ticks; // 实际CPU用户时间片事件数。
    if (current->user_ticks % 2 == 0) // 每两个实际IRQ尝试一次round-robin。
```

普通系统调用与用户故障入口也在处理 fork、exec 或退出以前保存。所有局部修改完成后，再接后面的探针与构建。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    }
    if (!session_active || !current || current->state != USER_RUNNING || (f->cs & 3) != 3) // 内核错误不能伪装为用户故障。
        airi_panic("user trap without a running process"); // 维持故障边界。
    collect_pending(); // 现在确实在另一条内核栈上，回收已经离开的孩子。
    current->frame = f; // 系统调用与故障都以当前实际现场为继续点。
    if (f->vector != 128) { // 用户CPU故障只结束拥有者。
```
<!-- after -->
```c
    }
    if (!session_active || !current || current->state != USER_RUNNING || (f->cs & 3) != 3) // 内核错误不能伪装为用户故障。
        airi_panic("user trap without a running process"); // 维持故障边界。
    airi_fpu_save(airi_memory_direct(current->fpu_physical)); // syscall、fork、exec与用户故障都从本次真实硬件状态承接。
    collect_pending(); // 现在确实在另一条内核栈上，回收已经离开的孩子。
    current->frame = f; // 系统调用与故障都以当前实际现场为继续点。
    if (f->vector != 128) { // 用户CPU故障只结束拥有者。
```

<a id="fpu-probe-abi"></a>
## 用活寄存器观察抢占，避免把普通调用约定误当进程语义

普通 C 函数调用允许被调用者覆盖 XMM 寄存器。若 C 先设 XMM15，再调用一串库函数后比较，就无法区分内核丢状态和普通函数合法覆盖。fork、exec 作为进程操作又有各自语义：fork 的孩子从同一执行点继承，失败 exec 沿旧执行点返回，成功 exec 从新入口开始。因此探针把准备值、int80 和返回后比较写在同一汇编函数中，在回 C 之前完成检查。

[x86-64 psABI寄存器约定](https://gitlab.com/x86-psABIs/x86-64-ABI/-/blob/master/x86-64-ABI/low-level-sys-info.tex)对控制字另有要求：普通函数返回时要保留调用者的 x87 控制字和 MXCSR 控制位。探针在局部栈另存原环境，内部临时改舍入，核对完成后恢复，再返回 C。XMM 数据和 x87 数据不作为本函数返回值；返回值仍在 RAX。这样测到的是系统调用与抢占期间的保持，不会让测试函数本身破坏 C 调用者的环境。

新建 `labs/airios/simd-probe.S`。SET_PROBE/CHECK_PROBE 是汇编宏，形参 failed 由每个调用位置提供跳转标签；`\failed` 在展开时替换成该标签。fork 与坏 exec 探针在 x87 留 1，并让 XMM15 的两半都为 0x123456789abcdef0。fucomip 比较 x87 栈值后弹一个，jp 拒绝空栈或 NaN 引起的无序结果。

spin 探针使用父子不同的 double 字节模式，同时保留不同的 x87 与 MXCSR 舍入控制，在五百万次整数比较中持续读实际 XMM15，没有 yield 或 syscall。PIT 可以在任意一轮打断它。局部栈分别放模式、预期环境、读回环境和原调用环境；40 字节局部空间保持调用处的对齐。数字标签 1b 表示向前找最近的标签 1，并非某个绝对地址。

<!-- write: labs/airios/simd-probe.S -->
```asm
.section .text /* 真实用户浮点状态观察，不链接到内核。 */
.macro SET_PROBE /* 给fork或失败exec建立可辨识的非默认状态。 */
    fninit /* 不沿用上一次探针残留的x87栈。 */
    fld1 /* x87栈留下一个真正的1，保存时不能只处理空栈。 */
    fldcw probe_cw(%rip) /* 改成0x077f的向下舍入控制环境。 */
    ldmxcsr probe_mxcsr(%rip) /* SSE使用对应的0x3f80环境。 */
    movabsq $0x123456789abcdef0, %rcx /* 该模式不等于新进程的零值。 */
    movq %rcx, %xmm15 /* 使用高组寄存器，不能遗漏XMM8至15。 */
    punpcklqdq %xmm15, %xmm15 /* 同一64位模式填满128位，不只检查低半。 */
.endm /* 结束环境准备。 */
.macro CHECK_PROBE failed /* failed由调用点提供，所有失败走同一返回位置。 */
    movabsq $0x123456789abcdef0, %rcx /* 内核返回后重新建立独立的期望GPR值。 */
    movq %xmm15, %rdx /* 读取实际低64位，不能用内存副本替代。 */
    cmpq %rcx, %rdx /* 与探针模式比较。 */
    jne \failed /* 低半丢失或串到其他进程。 */
    movhlps %xmm15, %xmm0 /* 将高64位移到一个临时寄存器低半。 */
    movq %xmm0, %rdx /* 再读取高半模式。 */
    cmpq %rcx, %rdx /* 高寄存器的整个128位都应保留。 */
    jne \failed /* 不能只保存部分向量。 */
    fnstcw 0(%rsp) /* 观察本次调用后的实际x87控制字。 */
    cmpw $0x077f, 0(%rsp) /* 不能从新进程默认值冒充fork继承。 */
    jne \failed /* 环境不符。 */
    stmxcsr 4(%rsp) /* 观察实际SSE控制与状态。 */
    cmpl $0x3f80, 4(%rsp) /* 这段探针没有做会设置异常标志的运算。 */
    jne \failed /* SSE环境也应保持。 */
    fld1 /* 另压一个1，与原来保存的值比较。 */
    fucomip %st(1), %st /* 比较后弹出新值；原值若未保留会得到unordered。 */
    jp \failed /* 拒绝空栈或NaN造成的无序比较。 */
    jne \failed /* 原非空x87数据必须仍为1。 */
    fstp %st(0) /* 弹出原值，让探针返回时不留下栈元素。 */
.endm /* 结束状态核对。 */
.global airi_simd_fork_probe /* 同一次int80后父子都先检查，再返回C。 */
.type airi_simd_fork_probe,@function /* 普通用户C调用约定。 */
airi_simd_fork_probe: /* 无参数，返回真正fork的PID/0或探针错误。 */
    subq $24, %rsp /* 为控制字快照保留独立栈空间，仍按16字节对齐。 */
    fnstcw 8(%rsp) /* 普通C调用要求被调用者保留x87控制字，先记住调用者环境。 */
    stmxcsr 12(%rsp) /* MXCSR控制位也是callee-saved，本探针结束后恢复。 */
    SET_PROBE /* 两分支都应继承这里的完整状态。 */
    movq $6, %rax /* AiriOS自有fork编号，不是Linux编号。 */
    int $0x80 /* 内核保存状态并构造孩子。 */
    movq %rax, %r8 /* 保留原fork结果，后面的比较借用其他GPR。 */
    CHECK_PROBE fork_failed /* 父子在任何C代码覆盖寄存器以前验证。 */
    movq %r8, %rax /* 真正PID或0交回C。 */
    fldcw 8(%rsp) /* 原控制字在fork的父子用户栈中分别存在，返回C前恢复。 */
    ldmxcsr 12(%rsp) /* 恢复调用者的控制位与原状态，不改变已完成的探针判断。 */
    addq $24, %rsp /* 结束本函数局部栈范围。 */
    ret /* 父子沿各自复制的用户栈返回。 */
fork_failed: /* 失败不会在内核中伪造状态。 */
    fninit /* 清理可能损坏的探针x87栈后返回普通错误。 */
    movq $-201, %rax /* 与内核fork资源错误分开。 */
    fldcw 8(%rsp) /* 原控制字在fork的父子用户栈中分别存在，返回C前恢复。 */
    ldmxcsr 12(%rsp) /* 恢复调用者的控制位与原状态，不改变已完成的探针判断。 */
    addq $24, %rsp /* 恢复调用者栈。 */
    ret /* C工作负载报告失败。 */
.size airi_simd_fork_probe,.-airi_simd_fork_probe /* 函数边界。 */
.global airi_simd_bad_exec /* 完整环境跨失败exec继续存在。 */
.type airi_simd_bad_exec,@function /* 返回原exec错误或探针错误。 */
airi_simd_bad_exec: /* 直接系统调用避免普通C函数的caller-saved XMM规则干扰。 */
    subq $24, %rsp /* 快照空间与fork探针相同。 */
    fnstcw 8(%rsp) /* 保存普通C调用者原有x87控制字。 */
    stmxcsr 12(%rsp) /* 保存原SSE环境，探针结束不改变调用者的舍入模式。 */
    SET_PROBE /* 旧映像留下非默认CPU状态。 */
    leaq probe_broken(%rip), %rdi /* 有效用户名称，指向已有截断ELF目录项。 */
    movq $7, %rax /* AiriOS exec调用号。 */
    int $0x80 /* 应拒绝为-8，并返回旧用户现场。 */
    movq %rax, %r8 /* 在核对CPU状态以前保存原结果。 */
    CHECK_PROBE exec_failed /* 失败装载不能换成新模板或别人的状态。 */
    movq %r8, %rax /* 原-8由C继续核对。 */
    fldcw 8(%rsp) /* 原控制字在fork的父子用户栈中分别存在，返回C前恢复。 */
    ldmxcsr 12(%rsp) /* 恢复调用者的控制位与原状态，不改变已完成的探针判断。 */
    addq $24, %rsp /* 返回时不借局部数组。 */
    ret /* 仍是原ELF调用链。 */
exec_failed: /* 只有状态不符合才走这里。 */
    fninit /* 不保留错误的x87栈。 */
    movq $-202, %rax /* 与正常ENOEXEC分开报告。 */
    fldcw 8(%rsp) /* 原控制字在fork的父子用户栈中分别存在，返回C前恢复。 */
    ldmxcsr 12(%rsp) /* 恢复调用者的控制位与原状态，不改变已完成的探针判断。 */
    addq $24, %rsp /* 恢复栈。 */
    ret /* C报告精确路径。 */
.size airi_simd_bad_exec,.-airi_simd_bad_exec /* 函数边界。 */
.global airi_simd_spin /* rdi是模式，esi是有限循环次数，返回0或错误。 */
.type airi_simd_spin,@function /* 普通C调用，但内部有跨真实抢占的活寄存器。 */
airi_simd_spin: /* 调用者为父子选择不同的有限double位模式。 */
    subq $40, %rsp /* 模式、期望/实际环境和原调用者环境分开，仍保持16字节对齐。 */
    fnstcw 24(%rsp) /* 本函数会改变RC，先保存callee-saved的原x87控制字。 */
    stmxcsr 28(%rsp) /* 返回C前必须恢复原MXCSR，不能把探针环境泄漏到调用者。 */
    movq %rdi, 0(%rsp) /* x87随后从这个IEEE double字节值装载。 */
    fninit /* 本轮只有一个活x87元素。 */
    fldl 0(%rsp) /* 将本进程模式对应的double留在x87栈中。 */
    movl %edi, %r8d /* 低一位让父子采用不同控制环境。 */
    andl $1, %r8d /* 只选择当前支持的两种舍入模式。 */
    shll $10, %r8d /* x87 RC位从第10位开始。 */
    orl $0x037f, %r8d /* 异常保持掩码，其余默认控制保持。 */
    movw %r8w, 8(%rsp) /* 本进程期望的控制字。 */
    fldcw 8(%rsp) /* 将其写入真实x87控制寄存器。 */
    movl %edi, %r8d /* SSE也独立采用相同的模式选择。 */
    andl $1, %r8d /* 清除模式以外的位。 */
    shll $13, %r8d /* MXCSR RC位从第13位开始。 */
    orl $0x1f80, %r8d /* 默认异常掩码与零状态标志。 */
    movl %r8d, 12(%rsp) /* 本进程期望的MXCSR。 */
    ldmxcsr 12(%rsp) /* 真实写入用户SSE环境。 */
    movq %rdi, %xmm15 /* 向量与x87都保持本进程特有模式。 */
    punpcklqdq %xmm15, %xmm15 /* 高半也保持同一模式。 */
    movl %esi, %ecx /* 有界纯计算，不用yield制造主动切换。 */
1: /* 真实PIT可在循环内任意指令打断并切另一个用户。 */
    movq %xmm15, %rax /* 每轮直接观察活寄存器。 */
    cmpq %rdi, %rax /* 与当前用户GPR保存的模式比较。 */
    jne spin_failed /* 串进另一个进程或被清成零都要失败。 */
    subl $1, %ecx /* 原请求次数由C固定为正数。 */
    jne 1b /* 回到前面的数字标签1，期间没有系统调用。 */
    movhlps %xmm15, %xmm0 /* 核对128位向量的高半。 */
    movq %xmm0, %rax /* 读取高半数值。 */
    cmpq %rdi, %rax /* 不能遗漏高64位。 */
    jne spin_failed /* 状态不符。 */
    fnstcw 16(%rsp) /* 与本进程栈中期望值分别保存。 */
    movw 16(%rsp), %ax /* cmp不能直接比较两个内存操作数，先读一个。 */
    cmpw %ax, 8(%rsp) /* 核对x87控制环境。 */
    jne spin_failed /* 父子的RC位不同，交换会暴露。 */
    stmxcsr 20(%rsp) /* SSE环境也须属于当前身份。 */
    movl 20(%rsp), %eax /* 读回控制与状态。 */
    cmpl %eax, 12(%rsp) /* 纯比较没有设置浮点异常标志。 */
    jne spin_failed /* 不接受另一个用户的MXCSR。 */
    fldl 0(%rsp) /* 再装入同一double作为期望值。 */
    fucomip %st(1), %st /* 与抢占前留在x87里的值比较并弹掉期望。 */
    jp spin_failed /* 无序不算相等。 */
    jne spin_failed /* 活x87数据也必须保持。 */
    fstp %st(0) /* 清掉本轮原值，回C时栈为空。 */
    xorl %eax, %eax /* 0表示本轮状态保持。 */
    fldcw 24(%rsp) /* 恢复调用者x87控制，空栈数据不作为返回值。 */
    ldmxcsr 28(%rsp) /* 恢复原SSE控制与状态，符合普通C调用约定。 */
    addq $40, %rsp /* 恢复用户栈。 */
    ret /* 普通调用者可读取tick并开始下一轮。 */
spin_failed: /* 错误只结束当前用户工作负载。 */
    fninit /* 不将坏栈传给后续C调用。 */
    movl $1, %eax /* 与本轮正常0区分。 */
    fldcw 24(%rsp) /* 恢复调用者x87控制，空栈数据不作为返回值。 */
    ldmxcsr 28(%rsp) /* 恢复原SSE控制与状态，符合普通C调用约定。 */
    addq $40, %rsp /* 回到原调用者帧。 */
    ret /* 用户返回码交给shell。 */
.size airi_simd_spin,.-airi_simd_spin /* 完整函数边界。 */
.section .rodata /* 控制常量属于用户ELF只读段。 */
.balign 4 /* MXCSR按四字节排列。 */
probe_mxcsr: /* 期望SSE环境。 */
.long 0x3f80 /* 所有异常掩码，向下舍入。 */
probe_cw: /* 期望x87环境。 */
.word 0x077f /* 默认精度和异常掩码，向下舍入。 */
probe_broken: /* 用户虚拟地址中的名称。 */
.asciz "broken" /* 复用既有坏ELF，不破坏正常程序。 */
.section .note.GNU-stack,"",@progbits /* 不需要可执行栈。 */
```

新建 `labs/airios/simd-reset.c`，作为成功 exec 的新 ELF。它的 main 特别保留 general-only：第一件事将真实状态保存到自己的对齐数组，避免编译器在观察前用 SSE 初始化局部对象。入口 start.S 只整理栈并调用 main；用户库函数要等快照完成后才调用。

检查空标签之外，还检查八个 x87 槽的实际 80 位和所有十六个 XMM 为零，操作码/指针为零，控制为默认。每个 x87 槽剩余六个保留字节没有数值承诺，不把它们当作寄存器数据比较。状态数组属于用户自己的栈；它从未成为内核的恢复输入。

<!-- write: labs/airios/simd-reset.c -->
```c
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
```

新建 `labs/airios/simd.c`。父子至少经过二十个真实 tick 的窗口，各自重复纯计算探针，之后验证失败 exec；volatile double 让输入必须真实加载，实际反汇编可看到 mulsd，结果为精确可表示的 6.5。孩子成功 exec 到 reset，父按同一 PID 等待它的默认状态检查结果。

最后再 fork 一个故障孩子，在有效用户栈内把 FXSAVE 目标错开一个字节。整个 512 字节跨度都已映射，失败应来自 16 字节对齐而非缺页；CPU 产生 GP13，已有用户故障路径只结束它，父 wait 收到自有编码 −141 后仍可输出成功。本实现没有把状态页开放给用户，也没有恢复用户指定的任意字节。

本轮另尝试过设置 MXCSR 保留位来验证 GP，但固定 QEMU TCG 实际允许指令继续，测试失败。这里采用真实观察到的对齐故障；这项模拟器差异与失败日志保留在 K41 验收记录，不能将保留位路径写成已验证成功。

<!-- write: labs/airios/simd.c -->
```c
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
```

<a id="fpu-build"></a>
## 用户允许 SSE2，内核仍保持通用寄存器

更新 images.S，为 simd 和 simd-reset 分别嵌入真实静态 ELF。user.c 的目录项只引用这些字节，执行时仍经过完整 ELF 检查与独立映射，不直接跳到宿主函数。

在 `labs/airios/images.S` 末尾追加以下内容，原有部分保留。

<!-- append: labs/airios/images.S -->
```asm

.section .rodata /* 浮点工作负载仍经过同一个静态ELF装载器。 */
.global airi_simd_elf_start, airi_simd_elf_end /* 主程序输入边界。 */
airi_simd_elf_start: /* 文件首。 */
.incbin AIRI_SIMD_ELF /* 当前配置带真实SSE/x87指令的ELF。 */
airi_simd_elf_end: /* 文件右端。 */
.global airi_simd_reset_elf_start, airi_simd_reset_elf_end /* exec新入口观察映像。 */
airi_simd_reset_elf_start: /* 文件首。 */
.incbin AIRI_SIMD_RESET_ELF /* 当前配置的状态快照程序。 */
airi_simd_reset_elf_end: /* 半开右端。 */
```

更新 Makefile。内核 CFLAGS 的 -mgeneral-regs-only 保持；USER_FLAGS 去掉它，改为 -msse2 -mno-avx。simd-reset.o 的专用规则在后面再加 general-only，只限制这一份入口观察 C，明确写出的内嵌 FXSAVE64 不受禁止。fpu.c 和 fpu_seed.S 分别生成不同名称的对象，避免同名 C/S 源争用一个目标。

构建规则也必须重新编译已有用户对象。Make 默认只比较声明依赖的时间，不会因为 USER_FLAGS 文字变化就知道旧 .o 不适用；因此对象目标将 Makefile 也列为前置，源文件仍排第一，保证 $< 继续取得源文件。用户编译另加 -MMD -MP，编译器生成 .d 记录传递头文件依赖，末尾以 -include 读取这些已生成的文件；首次还没有 .d 时不会报错。这样切换策略与改用户头文件都不会悄悄沿用旧对象。

simd.elf 额外链接寄存器探针对象，reset 使用自己的 main；两者都复用用户入口、字节库和系统调用封装，不借宿主 libc 或浮点运行库。

在内核 SOURCES 中登记 fpu.c。

<!-- replace: Makefile -->
<!-- before -->
```make
# 基础目标只编译文本会话与设备；不读取 frontend 的目录或字体。
SOURCES := kernel/main.c kernel/boot/limine.c kernel/boot/info.c \
    kernel/lib/base.c kernel/lib/runtime.c kernel/arch/x86_64/serial.c \
    kernel/console/console.c kernel/arch/x86_64/input.c kernel/arch/x86_64/cpu.c kernel/arch/x86_64/trap.c kernel/mm/pages.c kernel/mm/memory.c kernel/arch/x86_64/paging.c kernel/mm/heap.c kernel/arch/x86_64/timer.c kernel/task/task.c kernel/process/user.c kernel/process/elf.c kernel/fs/file.c kernel/arch/x86_64/irq.c kernel/drivers/virtio_block.c kernel/storage/journal.c kernel/storage/demo.c kernel/storage/service.c kernel/io/engine.c kernel/io/service.c
# 只有 desktop- 前缀目标才读取图形支线自己的构建片段。
ifneq ($(filter desktop-%,$(PROFILE)),)
include frontend/sources.mk # 本分支取得图形自己的源码清单。
```
<!-- after -->
```make
# 基础目标只编译文本会话与设备；不读取 frontend 的目录或字体。
SOURCES := kernel/main.c kernel/boot/limine.c kernel/boot/info.c \
    kernel/lib/base.c kernel/lib/runtime.c kernel/arch/x86_64/serial.c \
    kernel/console/console.c kernel/arch/x86_64/input.c kernel/arch/x86_64/cpu.c kernel/arch/x86_64/trap.c kernel/mm/pages.c kernel/mm/memory.c kernel/arch/x86_64/paging.c kernel/mm/heap.c kernel/arch/x86_64/timer.c kernel/task/task.c kernel/process/user.c kernel/process/elf.c kernel/fs/file.c kernel/arch/x86_64/fpu.c kernel/arch/x86_64/irq.c kernel/drivers/virtio_block.c kernel/storage/journal.c kernel/storage/demo.c kernel/storage/service.c kernel/io/engine.c kernel/io/service.c
# 只有 desktop- 前缀目标才读取图形支线自己的构建片段。
ifneq ($(filter desktop-%,$(PROFILE)),)
include frontend/sources.mk # 本分支取得图形自己的源码清单。
```

把 fpu_seed.S 的独立对象加入 OBJECTS，保持 C 与汇编目标不重名。

<!-- replace: Makefile -->
<!-- before -->
```make
# 去掉图形前缀后，两种呈现方式使用同一故障配置含义。
CASE := $(patsubst desktop-%,%,$(PROFILE))
# patsubst把每个.c相对路径改为当前OUT下的.o，汇编入口对象另加入。
OBJECTS := $(patsubst %.c,$(OUT)/%.o,$(SOURCES)) $(OUT)/kernel/arch/x86_64/entry.o $(OUT)/kernel/arch/x86_64/traps.o $(OUT)/kernel/arch/x86_64/switch.o $(OUT)/labs/airios/probe.o $(OUT)/labs/airios/images.o
# 交叉目标为无宿主ELF；C11/O2/调试信息/警告作为错误。freestanding与fno-builtin关闭宿主假设；禁栈保护和PIC/PIE；不使用red zone与浮点寄存器；kernel地址模型；MMD/MP生成头文件依赖。
CFLAGS := --target=x86_64-unknown-none-elf -std=c11 -O2 -g -Wall -Wextra -Werror \
          -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-pie \
```
<!-- after -->
```make
# 去掉图形前缀后，两种呈现方式使用同一故障配置含义。
CASE := $(patsubst desktop-%,%,$(PROFILE))
# patsubst把每个.c相对路径改为当前OUT下的.o，汇编入口对象另加入。
OBJECTS := $(patsubst %.c,$(OUT)/%.o,$(SOURCES)) $(OUT)/kernel/arch/x86_64/fpu_seed.o $(OUT)/kernel/arch/x86_64/entry.o $(OUT)/kernel/arch/x86_64/traps.o $(OUT)/kernel/arch/x86_64/switch.o $(OUT)/labs/airios/probe.o $(OUT)/labs/airios/images.o
# 交叉目标为无宿主ELF；C11/O2/调试信息/警告作为错误。freestanding与fno-builtin关闭宿主假设；禁栈保护和PIC/PIE；不使用red zone与浮点寄存器；kernel地址模型；MMD/MP生成头文件依赖。
CFLAGS := --target=x86_64-unknown-none-elf -std=c11 -O2 -g -Wall -Wextra -Werror \
          -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-pie \
```

C 与汇编的通用编译规则都依赖 Makefile，切换本节策略时会重新编译。

<!-- replace: Makefile -->
<!-- before -->
```make
# 只构建独立内核ELF，不制作启动镜像。
kernel: $(OUT)/airios.elf
# 模式规则将相同相对路径的C源文件编成当前配置对象；%匹配路径主体。
$(OUT)/%.o: %.c
# @隐藏命令回显；$@是目标路径，dir取得其目录并确保存在。
	@mkdir -p $(dir $@)
# $<取第一项依赖即当前源文件，$@取输出对象；-c只编译不链接。
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
# 同一对象布局下编译带预处理的汇编入口。
$(OUT)/%.o: %.S
# @隐藏命令回显；$@是目标路径，dir取得其目录并确保存在。
	@mkdir -p $(dir $@)
# $<取第一项依赖即当前源文件，$@取输出对象；-c只编译不链接。
```
<!-- after -->
```make
# 只构建独立内核ELF，不制作启动镜像。
kernel: $(OUT)/airios.elf
# 模式规则将相同相对路径的C源文件编成当前配置对象；%匹配路径主体。
$(OUT)/%.o: %.c Makefile
# @隐藏命令回显；$@是目标路径，dir取得其目录并确保存在。
	@mkdir -p $(dir $@)
# $<取第一项依赖即当前源文件，$@取输出对象；-c只编译不链接。
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
# 同一对象布局下编译带预处理的汇编入口。
$(OUT)/%.o: %.S Makefile
# @隐藏命令回显；$@是目标路径，dir取得其目录并确保存在。
	@mkdir -p $(dir $@)
# $<取第一项依赖即当前源文件，$@取输出对象；-c只编译不链接。
```

更新用户参数并为入口、hello 与库对象登记 Makefile 依赖。

<!-- replace: Makefile -->
<!-- before -->
```make
	./build/labs/boot-info

# 用户C使用自己的低地址模型、入口和库；绝不沿用内核高地址链接脚本。
USER_FLAGS := --target=x86_64-unknown-none-elf -std=c11 -O1 -g -Wall -Wextra -Werror -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -mgeneral-regs-only -mcmodel=small -Iinclude
# 每个配置拥有独立用户对象，避免跨配置复用旧二进制。
USER_OBJECTS := $(OUT)/user/start.o $(OUT)/user/hello.o $(OUT)/user/bytes.o $(OUT)/user/syscall.o
# 入口汇编与内核汇编分别形成对象，符号_start不在同一个链接目标中冲突。
$(OUT)/user/start.o: labs/airios/start.S
# 先建立独立用户产物目录。
	@mkdir -p $(dir $@)
# 当前交叉编译器不链接宿主运行时。
	$(CC) $(USER_FLAGS) -c $< -o $@
# 用户C程序只读取自有用户库声明。
$(OUT)/user/hello.o: labs/airios/hello.c labs/airios/include/airi_libc.h include/airi/io_abi.h
# 先建立输出目录。
	@mkdir -p $(dir $@)
# 编译为用户模型对象。
	$(CC) $(USER_FLAGS) -Ilabs/airios/include -c $< -o $@
# 用户库的字节实现与系统调用实现共用这一规则。
$(OUT)/user/%.o: labs/airios/libc/%.c labs/airios/include/airi_libc.h include/airi/io_abi.h
# 输出目录与当前配置绑定。
	@mkdir -p $(dir $@)
# 所有用户库均使用同样的freestanding约束。
```
<!-- after -->
```make
	./build/labs/boot-info

# 用户C使用自己的低地址模型、入口和库；绝不沿用内核高地址链接脚本。
USER_FLAGS := --target=x86_64-unknown-none-elf -std=c11 -O1 -g -Wall -Wextra -Werror -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -msse2 -mno-avx -MMD -MP -mcmodel=small -Iinclude
# 每个配置拥有独立用户对象，避免跨配置复用旧二进制。
USER_OBJECTS := $(OUT)/user/start.o $(OUT)/user/hello.o $(OUT)/user/bytes.o $(OUT)/user/syscall.o
# 入口汇编与内核汇编分别形成对象，符号_start不在同一个链接目标中冲突。
$(OUT)/user/start.o: labs/airios/start.S Makefile
# 先建立独立用户产物目录。
	@mkdir -p $(dir $@)
# 当前交叉编译器不链接宿主运行时。
	$(CC) $(USER_FLAGS) -c $< -o $@
# 用户C程序只读取自有用户库声明。
$(OUT)/user/hello.o: labs/airios/hello.c labs/airios/include/airi_libc.h include/airi/io_abi.h Makefile
# 先建立输出目录。
	@mkdir -p $(dir $@)
# 编译为用户模型对象。
	$(CC) $(USER_FLAGS) -Ilabs/airios/include -c $< -o $@
# 用户库的字节实现与系统调用实现共用这一规则。
$(OUT)/user/%.o: labs/airios/libc/%.c labs/airios/include/airi_libc.h include/airi/io_abi.h Makefile
# 输出目录与当前配置绑定。
	@mkdir -p $(dir $@)
# 所有用户库均使用同样的freestanding约束。
```

包装对象也要随 Makefile 改变重编，避免继续嵌入旧用户 ELF。

<!-- replace: Makefile -->
<!-- before -->
```make
# 用户只链接自己的_start、main和libc子集，无宿主依赖。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_OBJECTS) -o $@
# 包装汇编明确依赖本配置用户ELF，文件变动就重新嵌入。
$(OUT)/labs/airios/images.o: labs/airios/images.S $(OUT)/user/hello.elf $(OUT)/user/processes.elf $(OUT)/user/worker.elf $(OUT)/user/files.elf $(OUT)/user/storage.elf $(OUT)/user/pipes.elf $(OUT)/user/waits.elf $(OUT)/user/async.elf
# 建立包装对象目录。
	@mkdir -p $(dir $@)
# 单引号保护路径宏内部双引号，.incbin只读本配置生成文件。
```
<!-- after -->
```make
# 用户只链接自己的_start、main和libc子集，无宿主依赖。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_OBJECTS) -o $@
# 包装汇编明确依赖本配置用户ELF，文件变动就重新嵌入。
$(OUT)/labs/airios/images.o: labs/airios/images.S $(OUT)/user/hello.elf $(OUT)/user/processes.elf $(OUT)/user/worker.elf $(OUT)/user/files.elf $(OUT)/user/storage.elf $(OUT)/user/pipes.elf $(OUT)/user/waits.elf $(OUT)/user/async.elf Makefile
# 建立包装对象目录。
	@mkdir -p $(dir $@)
# 单引号保护路径宏内部双引号，.incbin只读本配置生成文件。
```

processes 的对象依赖补上 Makefile。

<!-- replace: Makefile -->
<!-- before -->
```make
# 第二个用户程序只复用用户入口与库，不把hello的main再次链接进去。
USER_COMMON := $(filter-out $(OUT)/user/hello.o,$(USER_OBJECTS))
# 普通用户C工作负载仍使用相同低地址编译约束。
$(OUT)/user/processes.o: labs/airios/processes.c labs/airios/include/airi_libc.h include/airi/io_abi.h
# 创建当前配置目录。
	@mkdir -p $(dir $@)
# 只读取用户库头，不引入内核实现。
```
<!-- after -->
```make
# 第二个用户程序只复用用户入口与库，不把hello的main再次链接进去。
USER_COMMON := $(filter-out $(OUT)/user/hello.o,$(USER_OBJECTS))
# 普通用户C工作负载仍使用相同低地址编译约束。
$(OUT)/user/processes.o: labs/airios/processes.c labs/airios/include/airi_libc.h include/airi/io_abi.h Makefile
# 创建当前配置目录。
	@mkdir -p $(dir $@)
# 只读取用户库头，不引入内核实现。
```

worker 的对象依赖补上 Makefile。

<!-- replace: Makefile -->
<!-- before -->
```make
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/processes.o -o $@

# exec目标独立编译，保持同一用户ABI与链接布局。
$(OUT)/user/worker.o: labs/airios/worker.c labs/airios/include/airi_libc.h include/airi/io_abi.h
# 建立当前配置输出目录。
	@mkdir -p $(dir $@)
# 普通用户C仅链接自己的运行库。
```
<!-- after -->
```make
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/processes.o -o $@

# exec目标独立编译，保持同一用户ABI与链接布局。
$(OUT)/user/worker.o: labs/airios/worker.c labs/airios/include/airi_libc.h include/airi/io_abi.h Makefile
# 建立当前配置输出目录。
	@mkdir -p $(dir $@)
# 普通用户C仅链接自己的运行库。
```

files 的对象依赖补上 Makefile。

<!-- replace: Makefile -->
<!-- before -->
```make
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/worker.o -o $@

# 文件演示有独立用户main，复用自有入口与库。
$(OUT)/user/files.o: labs/airios/files.c labs/airios/include/airi_libc.h include/airi/io_abi.h
# 按配置建立目录。
	@mkdir -p $(dir $@)
# 同样禁止宿主库和浮点寄存器。
```
<!-- after -->
```make
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/worker.o -o $@

# 文件演示有独立用户main，复用自有入口与库。
$(OUT)/user/files.o: labs/airios/files.c labs/airios/include/airi_libc.h include/airi/io_abi.h Makefile
# 按配置建立目录。
	@mkdir -p $(dir $@)
# 沿用当前允许 SSE2 的用户编译策略，不链接宿主运行库。
```

storage 的对象依赖补上 Makefile。

<!-- replace: Makefile -->
<!-- before -->
```make
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/files.o -o $@

# 存储工作负载有独立用户main，普通启动不自动追加正式记录。
$(OUT)/user/storage.o: labs/airios/storage.c labs/airios/include/airi_libc.h include/airi/io_abi.h
# 建立配置专属目录。
	@mkdir -p $(dir $@)
# 保持相同用户整数编译约束。
```
<!-- after -->
```make
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/files.o -o $@

# 存储工作负载有独立用户main，普通启动不自动追加正式记录。
$(OUT)/user/storage.o: labs/airios/storage.c labs/airios/include/airi_libc.h include/airi/io_abi.h Makefile
# 建立配置专属目录。
	@mkdir -p $(dir $@)
# 沿用允许 SSE2 的 USER_FLAGS 与独立配置目录。
```

pipes 的对象依赖补上 Makefile。

<!-- replace: Makefile -->
<!-- before -->
```make
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/storage.o -o $@

# 非阻塞管道有独立用户main，保持前面程序各自可运行。
$(OUT)/user/pipes.o: labs/airios/pipes.c labs/airios/include/airi_libc.h include/airi/io_abi.h
# 当前配置目录单独建立。
	@mkdir -p $(dir $@)
# 不链接内核头或宿主运行时。
```
<!-- after -->
```make
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/storage.o -o $@

# 非阻塞管道有独立用户main，保持前面程序各自可运行。
$(OUT)/user/pipes.o: labs/airios/pipes.c labs/airios/include/airi_libc.h include/airi/io_abi.h Makefile
# 当前配置目录单独建立。
	@mkdir -p $(dir $@)
# 不链接内核头或宿主运行时。
```

waits 的对象依赖补上 Makefile。

<!-- replace: Makefile -->
<!-- before -->
```make
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/pipes.o -o $@

# 阻塞和就绪等待工作负载独立于非阻塞管道程序。
$(OUT)/user/waits.o: labs/airios/waits.c labs/airios/include/airi_libc.h include/airi/io_abi.h
# 仍按配置隔离用户对象。
	@mkdir -p $(dir $@)
# 只使用自有用户库与整数寄存器。
```
<!-- after -->
```make
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/pipes.o -o $@

# 阻塞和就绪等待工作负载独立于非阻塞管道程序。
$(OUT)/user/waits.o: labs/airios/waits.c labs/airios/include/airi_libc.h include/airi/io_abi.h Makefile
# 仍按配置隔离用户对象。
	@mkdir -p $(dir $@)
# 只链接自有用户库，寄存器策略沿用允许 SSE2 的 USER_FLAGS。
```

async 的对象依赖补上 Makefile。

<!-- replace: Makefile -->
<!-- before -->
```make
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/waits.o -o $@

# 异步用户程序与重置别名共用独立main和低地址入口。
$(OUT)/user/async.o: labs/airios/async.c labs/airios/include/airi_libc.h include/airi/io_abi.h
# 当前配置的对象目录。
	@mkdir -p $(dir $@)
# USER_FLAGS含公开ABI头，不能依赖内核内部结构。
```
<!-- after -->
```make
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/waits.o -o $@

# 异步用户程序与重置别名共用独立main和低地址入口。
$(OUT)/user/async.o: labs/airios/async.c labs/airios/include/airi_libc.h include/airi/io_abi.h Makefile
# 当前配置的对象目录。
	@mkdir -p $(dir $@)
# USER_FLAGS含公开ABI头，不能依赖内核内部结构。
```

普通用户程序的模式规则也依赖 Makefile，后续新增的 main 自动遵循。

<!-- replace: Makefile -->
<!-- before -->
```make
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/async.o -o $@

# shell与贯通演示是独立的低地址用户ELF，不链接彼此main。
$(OUT)/user/%.o: labs/airios/%.c labs/airios/include/airi_libc.h include/airi/io_abi.h
# 与既有libc模式规则路径不同，Make按存在的源选择。
	@mkdir -p $(dir $@)
# 复用整数用户ABI，不借宿主运行时。
```
<!-- after -->
```make
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/async.o -o $@

# shell与贯通演示是独立的低地址用户ELF，不链接彼此main。
$(OUT)/user/%.o: labs/airios/%.c labs/airios/include/airi_libc.h include/airi/io_abi.h Makefile
# 与既有libc模式规则路径不同，Make按存在的源选择。
	@mkdir -p $(dir $@)
# 复用整数用户ABI，不借宿主运行时。
```

在 Makefile 末尾追加探针、reset 与两份映像规则，最后读取用户 .d 依赖。

<!-- append: Makefile -->
```make

# 用户SSE/x87观察由独立汇编保持活寄存器，不能用普通函数调用假定XMM保留。
$(OUT)/user/simd-probe.o: labs/airios/simd-probe.S Makefile
# 这个对象只进入用户ELF，与内核状态清理汇编分开。
	@mkdir -p $(dir $@)
# USER_FLAGS开放SSE2但不开放未保存的AVX。
	$(CC) $(USER_FLAGS) -c $< -o $@
# simd的main有真实double表达式；另链接寄存器观察汇编。
$(OUT)/user/simd.elf: $(USER_COMMON) $(OUT)/user/simd.o $(OUT)/user/simd-probe.o labs/airios/program.ld
# 静态链接仍只有一份main，不借宿主浮点运行库。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/simd.o $(OUT)/user/simd-probe.o -o $@
# reset探针在任何编译器SSE指令之前快照入口状态，因此此main保持general-only。
$(OUT)/user/simd-reset.o: labs/airios/simd-reset.c labs/airios/include/airi_libc.h include/airi/io_abi.h Makefile
# 与普通用户源使用同一配置目录。
	@mkdir -p $(dir $@)
# 后面的general-only只限制此观察C文件，内嵌FXSAVE64仍明确执行。
	$(CC) $(USER_FLAGS) -mgeneral-regs-only -Ilabs/airios/include -c $< -o $@
# 新ELF用于观察exec重新建立默认CPU状态。
$(OUT)/user/simd-reset.elf: $(USER_COMMON) $(OUT)/user/simd-reset.o labs/airios/program.ld
# 用户入口尚不触碰XMM，main第一件事保存真实状态。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/simd-reset.o -o $@
# 包装只等待本配置两份真实用户程序。
$(OUT)/labs/airios/images.o: $(OUT)/user/simd.elf $(OUT)/user/simd-reset.elf
# 文件名以C字符串宏交给预处理汇编。
$(OUT)/labs/airios/images.o: CPPFLAGS += -DAIRI_SIMD_ELF='"$(OUT)/user/simd.elf"' -DAIRI_SIMD_RESET_ELF='"$(OUT)/user/simd-reset.elf"'

# 用户C与汇编也生成传递头文件依赖，Makefile变化会重编各对象，避免保留旧SSE编译策略。
-include $(wildcard $(OUT)/user/*.d)
```

更新 boot.py，在普通成功启动中要求 CPU_STATE_READY。它只证明能力初始化完成，不代表切换保持正确。shell_test 随后在真实 ring3 shell 中输入 simd，检查父子输出、新 ELF 默认状态、故障孩子和最后退出。它从本次 EXEC_COMMIT/FORK 找 PID，再要求这两个用户的真实 user_ticks 至少为 2。

在 normal 成功分支加入 CPU_STATE_READY 检查，位置与原有时钟、陷入设施标记相邻。

<!-- replace: scripts/boot.py -->
<!-- before -->
```python
                        assert '[FILES_USER_OK]' in text and '[FD_EXEC_OK]' in text # RAM文件和成功exec继承均完成。
                        assert '[PROCESS_FORK_WAIT_OK]' in text and '[PROCESS_DEMO_OK]' in text # 复制、真实阻塞、timer换身份和回收都须完成。
                        assert '[ELF_LOAD]' in text and '[ELF_USER_OK]' in text # 独立ELF段、BSS与用户libc必须真正运行。
                        assert '[USER_DEMO_OK]' in text # 用户输出、指针拒绝和用户故障后内核必须继续。
                        assert '[HEAP_CAPACITY_OK] initial=65536 bytes=1048576 live=0 chunks=16 preserved=1 limit=null' in text # 新映射、活对象保持、上限和缓存归属均实际演示。
                        assert '[TASK_DEMO_OK]' in text # 两个独立栈必须交错并全部回收。
```
<!-- after -->
```python
                        assert '[FILES_USER_OK]' in text and '[FD_EXEC_OK]' in text # RAM文件和成功exec继承均完成。
                        assert '[PROCESS_FORK_WAIT_OK]' in text and '[PROCESS_DEMO_OK]' in text # 复制、真实阻塞、timer换身份和回收都须完成。
                        assert '[ELF_LOAD]' in text and '[ELF_USER_OK]' in text # 独立ELF段、BSS与用户libc必须真正运行。
                        assert '[CPU_STATE_READY] x87=1 sse2=1 fxsave_bytes=512 avx=off kernel=general-only' in text # 用户SSE编译前必须真正准备CPU保存边界。
                        assert '[USER_DEMO_OK]' in text # 用户输出、指针拒绝和用户故障后内核必须继续。
                        assert '[HEAP_CAPACITY_OK] initial=65536 bytes=1048576 live=0 chunks=16 preserved=1 limit=null' in text # 新映射、活对象保持、上限和缓存归属均实际演示。
                        assert '[TASK_DEMO_OK]' in text # 两个独立栈必须交错并全部回收。
```

在 capacity 命令完成以后运行 simd，并从本次输出关联父子 PID 与实际 user_ticks。

<!-- replace: scripts/shell_test.py -->
<!-- before -->
```python
                assert len(re.findall(r'\[FORK\].*copied_pages=1024',capacity_output))==4 # 四轮都触及最后一个元数据页。
                assert len(re.findall(r'\[EXEC_COMMIT\].*program=worker old_pages=1024',capacity_output))==4 # 大旧空间在成功exec提交后完整清理。
                assert 'cr2=0x6fff0000' in capacity_output # 用户栈下方保护页真正触发硬件异常。
                offset=len(text); wire.sendall(b'orphans\n'); text=wait('[SHELL_COMMAND_DONE] name=orphans code=0',offset) # 同一用户shell持续服务，不靠返回monitor收拢孙进程。
                orphan_output=text[offset:] # 只核对本条命令的身份记录。
                assert '[ORPHAN_USER_OK] rounds=24 live=12 zombie=12 session=alive' in orphan_output # 超过八槽容量仍能连续fork。
```
<!-- after -->
```python
                assert len(re.findall(r'\[FORK\].*copied_pages=1024',capacity_output))==4 # 四轮都触及最后一个元数据页。
                assert len(re.findall(r'\[EXEC_COMMIT\].*program=worker old_pages=1024',capacity_output))==4 # 大旧空间在成功exec提交后完整清理。
                assert 'cr2=0x6fff0000' in capacity_output # 用户栈下方保护页真正触发硬件异常。
                offset=len(text); wire.sendall(b'simd\n'); text=wait('[SHELL_COMMAND_DONE] name=simd code=0',offset) # 真实C浮点与跨抢占的活寄存器不能靠宿主模拟。
                simd_output=text[offset:] # 只核对本次命令的实际用户身份。
                assert '[SIMD_USER_OK] fork=inherited timer_window=20 x87=private xmm15=private exec=reset double=6.5' in simd_output # 父子与exec路径均由guest核对。
                assert '[SIMD_FAULT_ISOLATED] fxsave=unaligned vector=13 parent=alive' in simd_output and 'vector=13 error=0x0' in simd_output # 未对齐状态保存只结束自己的真实子身份。
                assert '[SIMD_CHILD_OK]' in simd_output and '[SIMD_EXEC_RESET_OK]' in simd_output # 子必须进入新的真实ELF检查默认状态。
                parent=re.search(r'\[EXEC_COMMIT\] pid=(\d+) program=simd ',simd_output) # shell创建的目标用户PID。
                assert parent # 不能用其他启动程序的输出代替。
                fork=re.search(r'\[FORK\] parent='+parent.group(1)+r' child=(\d+) ',simd_output) # 探针fork产生另一真实身份。
                assert fork # 本次C程序并非仅在一个进程里比较两个数组。
                for identity in (parent.group(1),fork.group(1)): # 两个用户都必须实际经过PIT中断窗口。
                    ended=re.search(r'\[USER_EXIT\] pid='+identity+r' code=0 user_ticks=(\d+)',simd_output) # exec保留同一PID的用户tick统计。
                    assert ended and int(ended.group(1))>=2 # 活寄存器检查跨真实时间片，而非只有主动yield。
                offset=len(text); wire.sendall(b'orphans\n'); text=wait('[SHELL_COMMAND_DONE] name=orphans code=0',offset) # 同一用户shell持续服务，不靠返回monitor收拢孙进程。
                orphan_output=text[offset:] # 只核对本条命令的身份记录。
                assert '[ORPHAN_USER_OK] rounds=24 live=12 zombie=12 session=alive' in orphan_output # 超过八槽容量仍能连续fork。
```

所有命令完成后，结果记录再加入 simd_preempt_fork_exec。

<!-- replace: scripts/shell_test.py -->
<!-- before -->
```python
                assert '[USER_SHELL_EXIT]' in text[offset:] and 'program sh exited 0' in text[offset:] # 真实收拢页/FD/静态池由launch核对。
                offset=len(text); wire.sendall(b'echo after-shell\n'); text=wait('[CONSOLE_UPDATE] view=0 commands=2 page=0',offset) # 回到内核后输入仍完整。
                assert 'after-shell' in text[offset:] # 不只是退出而丢了后续UART。
                result={'passed':True,'ring3_shell':True,'ram_file_and_device_readback':True,'other_user_during_cq_wait':True,'qmp_iops':1,'error_exec_survived':True,'returned_to_monitor':True,'orphan_rounds':24,'memory_reuse_rounds':128,'capacity_rounds':4,'capacity_pages':1024} # 只发布已观察事实。
                result_path.write_text(json.dumps(result,indent=2)+'\n'); print('PASS user shell: alloc/file/async wait/other task/CQ/readback, failed exec, return to monitor') # 阶段演示实际完成。
                return result # 总入口可收集。
            finally: # 包括断言、超时和QEMU错误。
```
<!-- after -->
```python
                assert '[USER_SHELL_EXIT]' in text[offset:] and 'program sh exited 0' in text[offset:] # 真实收拢页/FD/静态池由launch核对。
                offset=len(text); wire.sendall(b'echo after-shell\n'); text=wait('[CONSOLE_UPDATE] view=0 commands=2 page=0',offset) # 回到内核后输入仍完整。
                assert 'after-shell' in text[offset:] # 不只是退出而丢了后续UART。
                result={'passed':True,'ring3_shell':True,'ram_file_and_device_readback':True,'other_user_during_cq_wait':True,'qmp_iops':1,'error_exec_survived':True,'returned_to_monitor':True,'orphan_rounds':24,'memory_reuse_rounds':128,'capacity_rounds':4,'capacity_pages':1024,'simd_preempt_fork_exec':True} # 只发布已观察事实。
                result_path.write_text(json.dumps(result,indent=2)+'\n'); print('PASS user shell: alloc/file/async wait/other task/CQ/readback, failed exec, return to monitor') # 阶段演示实际完成。
                return result # 总入口可收集。
            finally: # 包括断言、超时和QEMU错误。
```

<a id="fpu-observe"></a>
## 先看实际指令，再看两个身份跨抢占继续

全部修改完成后运行：

<!-- run -->
```sh
./airios build # 编译新的CPU状态适配和两份真实用户ELF。
./airios test # 同时重跑旧用户、故障、服务监督、输入和存储路径。
```

Mac 若默认仍是旧 Python，前加 `PATH=/opt/homebrew/bin:$PATH`。启动会出现：

```text
[CPU_STATE_READY] x87=1 sse2=1 fxsave_bytes=512 avx=off kernel=general-only
```

交互运行 `./airios run`，在 airi> 输入 `run sh`，在用户提示符输入 `simd`。父子输出会交错，PID 由当前运行分配；成功路径应包括：

```text
[SIMD_CHILD_OK] fork=inherited preempt=preserved failed_exec=preserved
[SIMD_EXEC_RESET_OK] x87=empty-zero xmm=zero mxcsr=default pointers=zero
[USER_FAULT] pid=... vector=13 error=0x0 rip=... cr2=0x0
[SIMD_FAULT_ISOLATED] fxsave=unaligned vector=13 parent=alive
[SIMD_USER_OK] fork=inherited timer_window=20 x87=private xmm15=private exec=reset double=6.5
[SHELL_COMMAND_DONE] name=simd code=0
```

故障行的 PID 是第二次 fork 的孩子；第一次孩子成功 exec 后仍是同一 PID。父子退出的 user_ticks 来自真实 PIT，不承诺固定数值。测试结束退出 shell，原 launch 继续核对页、活堆对象与 FD 恢复，额外状态页不能因用户命令完成而泄漏。

可以用本机 LLVM 的 llvm-objdump -d 查看 build/normal/user/simd.elf：main 中实际有 mulsd，探针中有 XMM15 操作。相同工具查看内核 ELF 时，浮点指令只应在 fpu_seed 的明确初始化段；保存/恢复指令在对应架构函数，不把“无宿主 libc”误解为“不会生成 SSE”。库的整数循环也可能被编译器向量化，因此不能只给含 double 的单个文件补状态处理。

Linux 的普通程序也需要操作系统保存这类每线程状态；调用约定与系统调度职责仍要分开。本节没有新跑个人 Linux 主机，也没有完成 pthread、TLS 或 AVX/XSAVE。P8 接下来先给用户线程本地状态建立可隔离的基址，再接线程、同步与运行库。

