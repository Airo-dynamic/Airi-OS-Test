# K42 · 给当前用户恢复自己的 FS 基址

从 K41 继续。CPU 的浮点数据和控制状态已经随进程切换，但用户运行库还需要一种寻址方式：同一段代码，用相同偏移访问当前执行者自己的小块数据。errno、线程身份和线程库内部指针都会需要这种每线程数据。本节先把 x86-64 的 FS 基址接入真实用户边界，仍让现有每进程只有一个线程。

沿 `sh → tls → fork → 不同本地块 → exec tls-reset → wait` 运行。父子先验证 fork 继承，再将孩子换到另一个虚拟地址，用真实 FS 读写跨抢占验证隔离。成功 exec 清基址，失败保留；故障孩子释放目标页后继续 FS 读取，应只终止自己。

本节新增的是基址设施与手工小块布局。C 的 `_Thread_local`、ELF 的 TLS 描述、线程创建和 errno 包装尚未接入；不会因程序叫 tls 就已经拥有完整线程运行库。所有修改闭合后再运行，图形没有前置。

<a id="fs-address"></a>
## FS:8 加的是基址，得到的仍是用户虚拟地址

普通 `movq 8(%rdi),%rax` 从 RDI 加 8 的地址读八字节。`movq %fs:8,%rax` 则以 CPU 的 FS 基址加 8 形成线性地址，再由当前 CR3 的页表翻译。即使 FS 使用了特殊前缀，USER、WRITE、PRESENT 等页权限检查仍然适用；FS 不能跳过隔离或把用户地址变成物理地址。

长模式仍保留 FS/GS 的基址作用；Linux 常用 FS 访问线程本地存储，见[Linux 的 FS/GS 文档](https://docs.kernel.org/arch/x86/x86_64/fsgs.html)。本实现暂只支持 FS，并使用自己的调用号，不提供 Linux arch_prctl ABI 或 GS/SWAPGS 机制。

本节用一个用户页，从页内偏移 64 放两项 uint64_t：offset0 是该小块自己的用户地址，offset8 是值。父的页位于 0x10000000，FS.base 为 0x10000040，FS:8 访问 0x10000048。孩子继承后另申请页，FS.base 改为 0x10001040。调度既要切 CR3，也要恢复基址；只切页表时，孩子仍可能错误访问自己空间里继承的旧块。

| 对象 | 谁拥有或解释它 | 本节的变化 |
| --- | --- | --- |
| 用户数据页 | 用户进程，原页分配与归还接口管理 | fork 复制物理内容，release 解除映射 |
| FS 基址数值 | CPU 当前活动值，内核按身份保存 | setter 改数值，不分配或固定页面 |
| offset0/8 布局 | 当前用户程序 | 手工约定，不是正式 libc TCB |
| 未来 C 本地变量偏移 | 编译器与运行库共同约定 | 本节尚不处理 ELF TLS 或自动初始化 |

基址可以是不按页对齐的用户地址，也可以暂时未映射。设置时只确认它落在 48 位低半区，不读取未来 offset 对应的内存。真正读取 FS:8 才会检查基址加偏移得到的页面；目标已释放便产生用户 PF。用户须先停用或换基址，再释放还要继续访问的小块，setter 不代替内存所有权管理。

<a id="fs-msr"></a>
## 将段选择子和完整基址分开

K20 的 GDT 已有用户数据项，选择子是偏移 32 加 RPL3，即 35。选择子是索引及权限信息；FS 的完整基址则可以通过模型专用寄存器 IA32_FS_BASE 设置。MSR 这里表示 CPU 内部编号访问的寄存器，0xc0000100 是编号，不是一个可用 memcpy 读写的内存地址。

RDMSR 使用 ECX 选择编号，把结果的低/高 32 位放入 EAX/EDX。WRMSR 反向从 EDX:EAX 写入。它们只能在当前 ring0 中执行。用户接口必须在这条特权指令之前拒绝非法值，否则用户提交一个非规范地址，可能让内核自己产生 GP，而非得到可处理的参数错误。

CPU 还有 WRFSBASE 等直接指令，由 CR4.FSGSBASE 选择是否开放。本实现保持它关闭，让设置接口的范围策略成立；不会因为某 CPU 的 CPUID 报告有指令就自动开放。[Intel 的 FSGSBASE 说明](https://www.intel.com/content/www/us/en/developer/articles/technical/software-security-guidance/best-practices/guidance-enabling-fsgsbase.html)也区分了能力和操作系统允许位。

用户仍可装载合法段选择子，改变描述符对应的基址。例如后面的程序先设置自己的本地块，再装入用户数据选择子35；硬件基址便成为描述符给出的0，进程控制块中先前保存的地址已经过时。因此每次 CS3 陷入都先用 RDMSR 取回实际值，再清零供内核处理，查询接口才能看见这次变化。

恢复另一个用户时，先将 FS 选择子设为空，再写它保存的完整基址。若顺序反过来，段装载可能覆盖刚写入的值，返回后的 FS:8 又会用错起点。本 ABI 返回前统一空选择子，不承诺保留用户任意 FS 选择子或完整 Linux 段状态。

先新建 `include/airi/tls.h`。take 读取并清硬件状态，restore 只接受已验证低半区数值；两者都不接触用户指向的内存。

<!-- write: include/airi/tls.h -->
```c
#ifndef AIRI_TLS_H // 用户FS状态只通过架构接口交接。
#define AIRI_TLS_H // 防止重复声明。
#include <stdbool.h> // 初始化能力检查返回布尔值。
#include <airi/base.h> // 交接的基址使用固定宽度整数。
bool airi_tls_init(void); // 关闭FSGSBASE直写并将内核FS基址清零，尚未创建用户。
uint64_t airi_tls_take(void); // 读取被打断用户的真实FS基址，再清零活动硬件状态。
void airi_tls_restore(uint64_t base); // 只恢复已验证的低半区数值，不映射或固定任何用户页。
#endif // 结束包含保护。
```

新建 `kernel/arch/x86_64/tls.c`。初始化检查 CPUID 的 MSR 位，再只清 CR4 第 16 位，不覆盖 K41 的 SSE 允许位。set_base 先以 AX=0 装空 FS，再将地址按 32 位拆给 WRMSR；读取时须先将 high 提升为 uint64_t 再左移 32 位，不能在 32 位整数里移出整个宽度。

内嵌汇编的 `"c"`、`"a"`、`"d"` 将实参约束到指定寄存器，不是任由编译器挑选的普通 r 约束。清选择子的汇编显式声明 RAX 被覆盖，编译器才不会把一个仍活的 C 值藏在那里；memory 约束阻止相关内存访问跨过基址交接。

<!-- write: kernel/arch/x86_64/tls.c -->
```c
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
```

在 main.c 的 FPU 初始化之后调用 `airi_tls_init`，仍在建立用户前准备。K41 已确认 CPUID 叶 1 存在，本节接着检查 MSR；失败报告具体原因并停止。normal 与 service 都走这一步，尚未使用页池或运行用户。

包含区接入 FS 架构接口。

本章每组替换的第一块只用于定位旧文本，敲入第二块替换它，不把两块同时加入文件。各组按正文顺序完成。

<!-- replace: kernel/main.c -->
<!-- before -->
```c
#include <airi/fpu.h> // 用户浮点状态在首次用户程序以前建立。
#include <airi/async.h> // 有界SQ/CQ先在真实设备演示，再接用户。
#include <airi/storage.h> // 正式日志区跨进程和QEMU重启保留。
```
<!-- after -->
```c
#include <airi/tls.h> // FS基址准备与用户CPU状态分开，服务启动也需要。
#include <airi/fpu.h> // 用户浮点状态在首次用户程序以前建立。
#include <airi/async.h> // 有界SQ/CQ先在真实设备演示，再接用户。
#include <airi/storage.h> // 正式日志区跨进程和QEMU重启保留。
```

接在 FPU 初始化之后完成 FS 初始化，两种启动模式都执行。

<!-- replace: kernel/main.c -->
<!-- before -->
```c
    airi_trap_init(); // 通过启动校验后先接管异常，不启用设备中断。
    if (!airi_fpu_init()) // 诊断和常驻启动都必须拥有完整用户CPU状态。
        airi_panic("CPU requires FXSR, SSE and SSE2 user state support"); // 不把不支持目标带入用户ELF。
    airi_log("[BOOT_MODE] name=%s", diagnostics ? "diagnostic" : "service"); // 当前镜像的启动策略明确可见。
    if (diagnostics) // 服务启动只安装异常入口，不主动触发错误。
        airi_trap_demo(); // 诊断配置验证两种异常栈形状。
```
<!-- after -->
```c
    airi_trap_init(); // 通过启动校验后先接管异常，不启用设备中断。
    if (!airi_fpu_init()) // 诊断和常驻启动都必须拥有完整用户CPU状态。
        airi_panic("CPU requires FXSR, SSE and SSE2 user state support"); // 不把不支持目标带入用户ELF。
    if (!airi_tls_init()) // 用户FS设置只能由已初始化的架构接口完成。
        airi_panic("CPU lacks user FS base support"); // 不进入无法隔离本地状态的用户程序。
    airi_log("[BOOT_MODE] name=%s", diagnostics ? "diagnostic" : "service"); // 当前镜像的启动策略明确可见。
    if (diagnostics) // 服务启动只安装异常入口，不主动触发错误。
        airi_trap_demo(); // 诊断配置验证两种异常栈形状。
```

<a id="fs-entry"></a>
## 所有用户来源先保存，设备没有新通知也要恢复

新增 `airi_user_enter(frame)`，在 trap_dispatch 识别 CS3 后立即调用。先确认当前是活用户，take 实际 FS 并清零，再保存 x87/XMM。K41 分散在 timer、device_irq 和 user_trap 的三次浮点保存移到这个入口，避免未来增加来源时漏掉其中一个。内核 int129 的 CS 是 8，不经过用户保存，继续承担启动门。

中断门已关闭可屏蔽 IRQ，当前内核也不可抢占；入口处理不会在保存一半时切另一个用户。C 入口到清 FS 的这段代码不使用线程本地存储，内核编译无栈保护且不生成 FS 访问。若将来内核本身加入 TLS 或通过 FS 取栈保护值，就必须将交接提前到适当的汇编边界，不能直接保留本节的 C 假设。

新增保存改变了一个容易遗漏的设备返回分支：IRQ43 到来时，ISR 可能已没有新队列通知。即使不需要服务工作，用户 FS 也已在共同入口清零，所以不能直接返回原 frame；必须走 airi_user_resume 恢复。CS0 的同一分支不曾保存用户状态，可以直接恢复原内核现场。

先在 user.h 声明共同入口，再更新 trap.c。两种返回行为分别依据现场 CS 和实际通知决定，不能用 notified 同时代表两者。

<!-- write: include/airi/user.h -->
```c
#ifndef AIRI_USER_H // 用户态边界声明一次。
#define AIRI_USER_H // 包含保护。
#include <stdbool.h> // 系统命令回调明确报告是否处理。
#include <airi/trap.h> // 系统调用和用户故障使用完整CPU现场。
void airi_user_init(void); // 初始化用户文件资源，教学演示与服务启动共用。
int airi_user_boot(const char *name); // 内核启动固定目录中的根用户，返回仅表示该持续会话已经停止。
void airi_user_demo(void); // 运行受控隔离演示与独立C用户ELF，结束后回收。
void airi_user_enter(struct airi_trap_frame *frame); // 所有CS3陷入先保存额外CPU状态并清内核FS。
struct airi_trap_frame *airi_user_trap(struct airi_trap_frame *frame); // 允许返回另一条完整中断调用链。
struct airi_trap_frame *airi_user_device_irq(struct airi_trap_frame *frame); // 真实设备提示可把用户现场交给普通服务栈，不在IRQ复制结果。
struct airi_trap_frame *airi_user_timer(struct airi_trap_frame *frame); // 用户时间片在完整现场边界轮转，内核仍不可抢占。
struct airi_trap_frame *airi_user_resume(struct airi_trap_frame *frame); // 检查用户返回地址并清理不能交给iretq的标志。
struct airi_console; // 只借用文本会话，不依赖图形状态。
bool airi_user_command(struct airi_console *console, const char *command); // programs与run命令使用实际用户服务。
#endif // 结束包含保护。
```

在 trap_dispatch 最前面保存所有 CS3 来源；设备无新通知时也必须按 CS 决定恢复用户状态。

<!-- replace: kernel/arch/x86_64/trap.c -->
<!-- before -->
```c
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
```
<!-- after -->
```c
    airi_log("[TRAP_READY] vectors=32 cs=8 double_fault_ist=1 interrupts=off"); // 该标记只表示异常设施，不宣称调度或 IRQ 完成。
}
struct airi_trap_frame *airi_trap_dispatch(struct airi_trap_frame *f) { // 汇编传入的指针指向当前异常栈。
    if ((f->cs & 3) == 3) // 只有真正打断用户才取回该身份的额外CPU状态。
        airi_user_enter(f); // 清FS后再进入设备、时钟、系统调用或故障的普通处理。
    if (f->vector == 32u + AIRI_PCI_IRQ) { // 必须先于通用CS3故障判断，设备IRQ不能杀死用户进程。
        bool notified = airi_block_interrupt(); // 读取设备ISR使INTx先撤销，仅保存提示。
        airi_irq_end(AIRI_PCI_IRQ); // 再按从PIC到主PIC确认，避免电平来源持续触发。
        if ((f->cs & 3) == 3) // 即使ISR已无新来源，也必须恢复入口保存并清掉的用户状态。
            return notified ? airi_user_device_irq(f) : airi_user_resume(f); // 设备提示与完整恢复是两个独立条件。
        return f; // CS0继续原内核现场，不切用户身份。
    }
    if (f->vector == 128 || f->vector == 129 || ((f->cs & 3) == 3 && f->vector != 32)) // 用户系统调用和用户故障由用户上下文所有者处理。
        return airi_user_trap(f); // 返回哪条完整现场由明确边界决定。
```

<a id="fs-owner"></a>
## 保存的是数值，页的归属仍沿原路径

user_process 增加 fs_base，默认清零。fork 在父的真实硬件状态已保存后，复制该数值，并按原有规则复制用户页。exec 的 next 从零开始，因此成功提交获得 FS=0；准备失败只清 next，不覆盖旧控制块里的值。这个字段没有需要单独归还的内存，旧页仍由 owned 记录清理。

新增调用号 33 将 RDI 的低半区数值写入保存值，返回 0；越界返回 −22，旧值保持。此时硬件仍为内核的 0，airi_user_resume 在确定身份、检查 RIP/RSP 后才恢复用户基址。调用号 34 返回本次入口取到的实际值。低半区小于 2^47，能由非负 long 表示，与负错误范围分开。

当用户等待 I/O、退出或暂回普通服务栈时，共同入口已经清 FS，不依靠当前用户页面供内核运行。恢复另一个进程时，它先恢复自己的 FS，再恢复自己的浮点状态，最后公共汇编取 GPR 并 iretq。其余页记录、FD、异步资源和输入所有权保持。

共同入口已经接到 trap_dispatch，下面在 user.c 提供它的实现，再接 fork、统一返回和两个基址调用。timer、device_irq 与 user_trap 只消费这份共同保存的状态。

接下来按现有 `kernel/process/user.c` 中的位置逐组替换，未列出的前章内容保持。所有映像、用户库与构建接入完成后再运行。

在包含区接入 FS 架构接口。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
#include <airi/fpu.h> // 每进程拥有一份非用户映射的浮点/SSE状态。
#include <airi/async.h> // 进程轮次与固定缓冲资源的生命周期。
#include <airi/arch.h> // CPU空闲与用户调度门留在架构适配。
```
<!-- after -->
```c
#include <airi/tls.h> // 每次真实用户陷入取回FS，内核处理期间清零。
#include <airi/fpu.h> // 每进程拥有一份非用户映射的浮点/SSE状态。
#include <airi/async.h> // 进程轮次与固定缓冲资源的生命周期。
#include <airi/arch.h> // CPU空闲与用户调度门留在架构适配。
```

控制块记录基址数值；它不额外持有物理页面。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    bool async_owned; // 本控制块是否持有当前PID的异步owner，局部旧exec副本必须清除此所有权。
    struct airi_fd_table files; // exec转移表项，fork增加同一打开对象的引用。
    struct airi_address_space space; // 根和低半区表自有，高半区借用。
    uint64_t fpu_physical; // 独立状态页不塞进exec局部控制块，也不算用户数据页。
    struct airi_kernel_region stack; // 不能在仍使用它的C调用中释放。
    uint64_t records[RECORD_CHUNKS]; // 元数据页的物理地址；各页经HHDM访问，不映射给用户。
```
<!-- after -->
```c
    bool async_owned; // 本控制块是否持有当前PID的异步owner，局部旧exec副本必须清除此所有权。
    struct airi_fd_table files; // exec转移表项，fork增加同一打开对象的引用。
    struct airi_address_space space; // 根和低半区表自有，高半区借用。
    uint64_t fs_base; // 用户FS基址只保存数值，映射由原用户页所有权管理。
    uint64_t fpu_physical; // 独立状态页不塞进exec局部控制块，也不算用户数据页。
    struct airi_kernel_region stack; // 不能在仍使用它的C调用中释放。
    uint64_t records[RECORD_CHUNKS]; // 元数据页的物理地址；各页经HHDM访问，不映射给用户。
```

加入两份手工 FS 观察映像的声明。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
extern const uint8_t airi_user_rsp_bad_start[], airi_user_rsp_bad_end[]; // 不规范用户返回栈。
extern const uint8_t airi_hello_elf_start[], airi_hello_elf_end[]; // 既有独立用户C程序。
extern const uint8_t airi_process_elf_start[], airi_process_elf_end[]; // 新的fork/wait和抢占工作负载。
extern const uint8_t airi_simd_elf_start[], airi_simd_elf_end[]; // 浮点、抢占和继承探针。
extern const uint8_t airi_simd_reset_elf_start[], airi_simd_reset_elf_end[]; // exec新状态观察。
extern const uint8_t airi_init_elf_start[], airi_init_elf_end[]; // 用户态常驻监督程序。
```
<!-- after -->
```c
extern const uint8_t airi_user_rsp_bad_start[], airi_user_rsp_bad_end[]; // 不规范用户返回栈。
extern const uint8_t airi_hello_elf_start[], airi_hello_elf_end[]; // 既有独立用户C程序。
extern const uint8_t airi_process_elf_start[], airi_process_elf_end[]; // 新的fork/wait和抢占工作负载。
extern const uint8_t airi_tls_elf_start[], airi_tls_elf_end[]; // FS本地数据与切换探针。
extern const uint8_t airi_tls_reset_elf_start[], airi_tls_reset_elf_end[]; // 成功exec清基址观察。
extern const uint8_t airi_simd_elf_start[], airi_simd_elf_end[]; // 浮点、抢占和继承探针。
extern const uint8_t airi_simd_reset_elf_start[], airi_simd_reset_elf_end[]; // exec新状态观察。
extern const uint8_t airi_init_elf_start[], airi_init_elf_end[]; // 用户态常驻监督程序。
```

将 tls 与 tls-reset 接入同一个初始目录。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
static const struct executable executables[] = { // 每份映像字节由内核只读段长期持有。
    {"sh", airi_shell_elf_start, airi_shell_elf_end}, // 独占串口输入的可选用户程序。
    {"init", airi_init_elf_start, airi_init_elf_end}, // 管理前台shell的常驻用户进程。
    {"simd", airi_simd_elf_start, airi_simd_elf_end}, // 真实SSE/x87隔离与fork/exec。
    {"simd-reset", airi_simd_reset_elf_start, airi_simd_reset_elf_end}, // 新ELF入口的默认状态。
    {"capacity", airi_capacity_elf_start, airi_capacity_elf_end}, // 预算耗尽后回收并继续运行。
```
<!-- after -->
```c
static const struct executable executables[] = { // 每份映像字节由内核只读段长期持有。
    {"sh", airi_shell_elf_start, airi_shell_elf_end}, // 独占串口输入的可选用户程序。
    {"init", airi_init_elf_start, airi_init_elf_end}, // 管理前台shell的常驻用户进程。
    {"tls", airi_tls_elf_start, airi_tls_elf_end}, // 每个用户身份独立FS基址。
    {"tls-reset", airi_tls_reset_elf_start, airi_tls_reset_elf_end}, // 新ELF不沿用旧FS。
    {"simd", airi_simd_elf_start, airi_simd_elf_end}, // 真实SSE/x87隔离与fork/exec。
    {"simd-reset", airi_simd_reset_elf_start, airi_simd_reset_elf_end}, // 新ELF入口的默认状态。
    {"capacity", airi_capacity_elf_start, airi_capacity_elf_end}, // 预算耗尽后回收并继续运行。
```

fork 已经复制用户页，这里再复制父入口取回的基址数值。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
            goto failed; // 回滚全部子资源，不改变父页面。
        airi_memcpy(airi_memory_direct(owned_at(child, child->count - 1)->physical), airi_memory_direct(page->physical), 4096); // 复制完整页内容，父子以后写入互不影响。
    }
    airi_memcpy(airi_memory_direct(child->fpu_physical), airi_memory_direct(parent->fpu_physical), AIRI_FPU_BYTES); // 父陷入时已保存，子复制真实状态而非共享状态页。
    airi_fd_clone(&child->files, &parent->files); // 页复制全部成功后再取得FD引用，失败cleanup仍能撤销。
    child->initial = *parent_frame; // 用户RIP/RSP和寄存器从同一次fork返回点继续。
```
<!-- after -->
```c
            goto failed; // 回滚全部子资源，不改变父页面。
        airi_memcpy(airi_memory_direct(owned_at(child, child->count - 1)->physical), airi_memory_direct(page->physical), 4096); // 复制完整页内容，父子以后写入互不影响。
    }
    child->fs_base = parent->fs_base; // fork继承同一用户VA，指向复制后的子页面，不共享物理内容。
    airi_memcpy(airi_memory_direct(child->fpu_physical), airi_memory_direct(parent->fpu_physical), AIRI_FPU_BYTES); // 父陷入时已保存，子复制真实状态而非共享状态页。
    airi_fd_clone(&child->files, &parent->files); // 页复制全部成功后再取得FD引用，失败cleanup仍能撤销。
    child->initial = *parent_frame; // 用户RIP/RSP和寄存器从同一次fork返回点继续。
```

在 exec_current 结束、airi_user_resume 开始之前加入共同用户入口。先取真实 FS 并清零，再保存浮点状态。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    airi_log("[EXEC_COMMIT] pid=%u program=%s old_pages=%u new_pages=%u", current->pid, name, old_pages, current->count); // 名称是内核副本，不引用刚释放用户页。
    return 0; // 调用者直接恢复新f，不把这个0写回旧用户调用。
}
struct airi_trap_frame *airi_user_resume(struct airi_trap_frame *f) { // 所有用户iretq路径使用同一规范地址和标志策略。
    if (f->cs != 27 || f->ss != 35 || f->rip >= AIRI_USER_LIMIT || f->rsp >= AIRI_USER_LIMIT) { // 坏返回不能变成内核GP。
        airi_log("[USER_RETURN_REJECTED] pid=%u rip=0x%llx rsp=0x%llx", current->pid, (unsigned long long)f->rip, (unsigned long long)f->rsp); // 不解引用错误地址。
```
<!-- after -->
```c
    airi_log("[EXEC_COMMIT] pid=%u program=%s old_pages=%u new_pages=%u", current->pid, name, old_pages, current->count); // 名称是内核副本，不引用刚释放用户页。
    return 0; // 调用者直接恢复新f，不把这个0写回旧用户调用。
}
void airi_user_enter(struct airi_trap_frame *f) { // 所有CS3来源在分派设备、timer或syscall之前共同保存。
    if (!session_active || !current || current->state != USER_RUNNING || (f->cs & 3) != 3) // 当前实际栈必须有运行所有者。
        airi_panic("CPU state without a running user"); // 内核int129不经过这个入口。
    current->fs_base = airi_tls_take(); // 先取真实用户基址，再把硬件FS清零供内核处理。
    airi_fpu_save(airi_memory_direct(current->fpu_physical)); // x87/XMM也在此唯一入口保存，不能因设备没有新ISR而跳过。
}
struct airi_trap_frame *airi_user_resume(struct airi_trap_frame *f) { // 所有用户iretq路径使用同一规范地址和标志策略。
    if (f->cs != 27 || f->ss != 35 || f->rip >= AIRI_USER_LIMIT || f->rsp >= AIRI_USER_LIMIT) { // 坏返回不能变成内核GP。
        airi_log("[USER_RETURN_REJECTED] pid=%u rip=0x%llx rsp=0x%llx", current->pid, (unsigned long long)f->rip, (unsigned long long)f->rsp); // 不解引用错误地址。
```

统一返回先恢复 FS；设备路径改用共同入口已经保存的快照，不再重复保存浮点状态。

<!-- replace: kernel/process/user.c -->
<!-- before -->
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
<!-- after -->
```c
    }
    f->rflags &= ~((3ull << 12) | (1ull << 14) | (1ull << 17)); // 清IOPL、NT和VM。
    f->rflags |= 0x202; // 保留必需bit1并保持用户IF。
    airi_tls_restore(current->fs_base); // 与所选用户身份一起恢复，随后内核C不会通过FS访问数据。
    airi_fpu_restore(airi_memory_direct(current->fpu_physical)); // 当前身份已确定，恢复后内核和公共汇编不再使用浮点/XMM。
    return f; // 返回选中进程的完整现场。
}
struct airi_trap_frame *airi_user_device_irq(struct airi_trap_frame *f) { // 本函数只在真实设备IRQ打断ring3后调用。
    if (!session_active || !current || current->state != USER_RUNNING) // 用户返回必须有明确现场所有者。
        airi_panic("device IRQ without a running user"); // 不把未知栈交给启动继续点。
    if (!airi_async_work()) // 同步启动请求或已收集的旧通知无需切用户身份。
        return airi_user_resume(f); // 用户返回策略仍统一核对。
    current->frame = f; // 保存完整GPR与用户RIP/RSP，稍后从同一指令继续。
```

timer 入口也删除已经移到共同入口的保存动作，其调度行为保持。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
struct airi_trap_frame *airi_user_timer(struct airi_trap_frame *f) { // IRQ路径不分配、不回收、不打印，只有状态与根选择。
    if (!session_active || !current || current->state != USER_RUNNING) // CS3的IRQ必须有明确用户所有者。
        airi_panic("user IRQ without a running process"); // 不把未知内核栈交给调度器。
    airi_fpu_save(airi_memory_direct(current->fpu_physical)); // 每个用户时钟入口先保存额外CPU状态，不能只保存GPR。
    current->frame = f; // 保存真正被打断的位置，不只保存C ABI寄存器。
    ++current->user_ticks; // 实际CPU用户时间片事件数。
    if (current->user_ticks % 2 == 0) // 每两个实际IRQ尝试一次round-robin。
```
<!-- after -->
```c
struct airi_trap_frame *airi_user_timer(struct airi_trap_frame *f) { // IRQ路径不分配、不回收、不打印，只有状态与根选择。
    if (!session_active || !current || current->state != USER_RUNNING) // CS3的IRQ必须有明确用户所有者。
        airi_panic("user IRQ without a running process"); // 不把未知内核栈交给调度器。
    current->frame = f; // 保存真正被打断的位置，不只保存C ABI寄存器。
    ++current->user_ticks; // 实际CPU用户时间片事件数。
    if (current->user_ticks % 2 == 0) // 每两个实际IRQ尝试一次round-robin。
```

user_trap 同样只消费共同入口保存的状态。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    }
    if (!session_active || !current || current->state != USER_RUNNING || (f->cs & 3) != 3) // 内核错误不能伪装为用户故障。
        airi_panic("user trap without a running process"); // 维持故障边界。
    airi_fpu_save(airi_memory_direct(current->fpu_physical)); // syscall、fork、exec与用户故障都从本次真实硬件状态承接。
    collect_pending(); // 现在确实在另一条内核栈上，回收已经离开的孩子。
    current->frame = f; // 系统调用与故障都以当前实际现场为继续点。
    if (f->vector != 128) { // 用户CPU故障只结束拥有者。
```
<!-- after -->
```c
    }
    if (!session_active || !current || current->state != USER_RUNNING || (f->cs & 3) != 3) // 内核错误不能伪装为用户故障。
        airi_panic("user trap without a running process"); // 维持故障边界。
    collect_pending(); // 现在确实在另一条内核栈上，回收已经离开的孩子。
    current->frame = f; // 系统调用与故障都以当前实际现场为继续点。
    if (f->vector != 128) { // 用户CPU故障只结束拥有者。
```

最后在 sleep 分支之后、未知调用的拒绝分支之前接入33/34。setter只改变保存值，统一返回才写硬件。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
            return schedule(REASON_IO); // 其他用户继续；全阻塞时启动栈hlt等PIT。
        }
    }
    else // 尚未提供的接口都明确拒绝。
        f->rax = (uint64_t)-38ll; // -ENOSYS。
    poll_waiters(); // 文件/流修改完成后立即重查其他等待者，不等用户轮询。
```
<!-- after -->
```c
            return schedule(REASON_IO); // 其他用户继续；全阻塞时启动栈hlt等PIT。
        }
    }
    else if (f->rax == 33) { // 自有FS基址设置，只验证数值，不固定或分配其目标页。
        if (f->rdi >= AIRI_USER_LIMIT) // 非规范中间地址与所有内核高半区都在特权WRMSR之前拒绝。
            f->rax = (uint64_t)-22ll; // -EINVAL，旧保存值不变。
        else { // 0表示停用，其余低半区值即使尚未映射也可登记。
            current->fs_base = f->rdi; // 当前硬件仍为内核0，统一返回入口才写用户值。
            f->rax = 0; // 不解引用、不读取该用户地址。
        }
    }
    else if (f->rax == 34) // 查询的是陷入时从硬件读取的真实当前值。
        f->rax = current->fs_base; // 低半区基址可由long无歧义表示，不返回内核地址。
    else // 尚未提供的接口都明确拒绝。
        f->rax = (uint64_t)-38ll; // -ENOSYS。
    poll_waiters(); // 文件/流修改完成后立即重查其他等待者，不等用户轮询。
```

用户包装只传基址数值，不直接执行 WRMSR，也不把它当输出缓冲区。更新用户头文件与 libc/syscall.c，保持原 call3 的 int80 寄存器规则。两个新函数仍直接返回负错误；errno 包装尚未加入。

<details>
<summary>labs/airios/include/airi_libc.h 的完整终点</summary>

<!-- write: labs/airios/include/airi_libc.h -->
```c
#ifndef AIRI_USER_LIBC_H // 用户库头与内核基础库分开。
#define AIRI_USER_LIBC_H // 包含保护。
#include <airi/io_abi.h> // 自有固定宽度提交、完成和只读状态，不包含内核结构。
#include <stddef.h> // 用户对象字节长度。
#include <stdint.h> // 用户ABI的64位寄存器值。
void *memcpy(void *destination, const void *source, size_t bytes); // 双方不重叠的用户空间复制。
void *memmove(void *destination, const void *source, size_t bytes); // 允许同一用户数组内重叠移动。
void *memset(void *destination, int value, size_t bytes); // 按字节填充用户对象。
int memcmp(const void *left, const void *right, size_t bytes); // 首个不同无符号字节决定结果。
size_t strlen(const char *text); // 用户有效NUL字符串长度。
long airi_write(unsigned fd, const void *buffer, size_t bytes); // 自有int80 ABI，失败直接返回负数。
long airi_getpid(void); // 获取当前用户身份。
_Noreturn void airi_exit(unsigned code); // 退出不返回用户调用方。
long airi_puts(const char *text); // 有界 FD 写入，不自动补换行。
long airi_fork(void); // 首版内核直接复制用户页，子返回0，父返回子PID。
long airi_waitpid(long pid, int *status); // pid=-1或指定孩子；status是普通退出值。
void *airi_alloc(size_t bytes); // 零填充页申请，失败NULL；显式release整页或进程结束回收。
uint64_t airi_ticks(void); // 实际时钟事件计数。
void airi_yield(void); // 主动交接，不等同于时钟抢占。
long airi_exec(const char *name); // 成功替换映像不返回；失败保留旧PID和调用链，返回负错误。
#define AIRI_READ 1u // 以下标志只属于自有open ABI。
#define AIRI_WRITE 2u // 写访问。
#define AIRI_CREATE 4u // 名字不存在时创建。
#define AIRI_TRUNCATE 8u // 清空可写文件。
#define AIRI_CLOEXEC 16u // 成功exec关闭此表项。
long airi_open(const char *path, unsigned flags); // 平坦绝对名字，返回FD或负错误。
long airi_close(unsigned fd); // 关闭不删除名字。
long airi_dup(unsigned fd, unsigned cloexec); // 与源共享偏移。
long airi_seek(unsigned fd, uint64_t offset); // 只支持绝对定位。
long airi_read(unsigned fd, void *buffer, size_t bytes); // EOF为0，短读是实际完成数。
long airi_pread(unsigned fd, void *buffer, size_t bytes, uint64_t offset); // 不移动共享偏移。
long airi_pwrite(unsigned fd, const void *buffer, size_t bytes, uint64_t offset); // 第四参数R10。
long airi_log_append(const void *data, size_t bytes); // 最多480字节，成功返回从1开始的sequence。
long airi_log_read(unsigned index, void *data, size_t capacity); // 索引从0，返回实际长度。
long airi_log_count(void); // 已恢复有效前缀，错误为负。
#define AIRI_READABLE 1u // 当前read可得到数据或EOF。
#define AIRI_WRITABLE 2u // 当前有读者和空位。
#define AIRI_HANGUP 4u // 最后写者关闭，仍应排空已有数据。
#define AIRI_ERROR 8u // 最后读者关闭，写将EPIPE。
long airi_pipe(unsigned ends[2], unsigned nonblocking); // nonblocking=0阻塞，1暂不可完成时返回-11。
long airi_ready(unsigned fd); // 管道当前条件查询，不是Linux epoll ABI。
long airi_wait_ready(unsigned fd, unsigned events, unsigned ticks); // READ/WRITE条件，0纯查询，最多500tick，超时返回0。
long airi_io_alloc(void); // 注册一个512字节内核buffer，返回句柄而不是指针。
long airi_io_put(uint64_t handle, const void *data, size_t bytes); // IDLE可修改，BUSY返回-16。
long airi_io_get(uint64_t handle, void *data, size_t bytes); // CQ收集后才可读取。
long airi_io_free(uint64_t handle); // 旧句柄-9，BUSY-16。
long airi_io_submit(unsigned operation, uint64_t handle, const struct airi_io_submission *submission); // 0仅接收，描述在返回前已复制。
long airi_io_next(struct airi_io_completion *completion, unsigned wait); // 0不等，1阻塞；返回0仅表示交出一条CQ。
long airi_io_state_get(struct airi_io_state *state); // 当前轮次与同PID退役ACTIVE的只读数值。
long airi_sleep(unsigned ticks); // 0立即成功，1至6000tick真阻塞；此时钟100Hz，不是墙钟。
long airi_release(void *address, size_t bytes); // 解除一至六十四个完整动态页；允许部分范围，失败不改整段。
long airi_programs(char *names, size_t capacity); // 真实初始ELF目录，每行名称，容量不足不截断。
long airi_fs_set(uintptr_t base); // 自有ABI设置低半区FS基址，0停用；不创建或固定映射。
long airi_fs_base(void); // 返回当前用户FS基址，尚不是通用arch_prctl或完整TLS运行库。
#endif // 结束包含保护。
```

</details>

在 `labs/airios/libc/syscall.c` 末尾追加以下内容，前章已有部分保留。

<!-- append: labs/airios/libc/syscall.c -->
```c

long airi_fs_set(uintptr_t base) { // 用户代码只提交数值，内核先排除高半区与非规范值。
    return call3(33, base, 0, 0); // 0成功，-22失败且保持原基址；不设置POSIX errno。
}
long airi_fs_base(void) { // 通过受检查入口观察本次陷入时真实硬件状态。
    return call3(34, 0, 0, 0); // 当前只支持低半区，可用非负long表示。
}
```

<a id="fs-workload"></a>
## 相同偏移跨两个基址继续读写

新建 tls-reset.c。新 ELF 先查询 FS=0，之后才输出；它不尝试用 FS:0 访问一个尚未建立的新本地块。成功 exec 保留 PID，却不保留旧运行库的线程本地对象。

<!-- write: labs/airios/tls-reset.c -->
```c
#include <airi_libc.h> // 新ELF先查询基址，不解引用尚未建立的TLS。
int main(void) { // 成功exec必须从停用的FS环境开始。
    if (airi_fs_base() != 0) // PID可保留，旧用户本地块不能作为新映像的默认环境。
        return 191; // 父仍能收到具体失败位置。
    airi_puts("[TLS_EXEC_RESET_OK] fs_base=0 old_block=detached\n"); // 只在真实查询完成后报告。
    return 0; // 原父wait同一孩子身份。
}
```

新建 tls.c。三个短函数以明确的内嵌汇编读 FS:0、FS:8 和写 FS:8，memory 约束保证每次观察实际内存。纯计算循环里没有 syscall 或 yield，父子不同地址和值必须靠时钟抢占正确恢复；循环外的基址查询又检查系统调用返回路径。

父在 fork 前先演示三个非法基址均拒绝且旧值不变，再装载用户数据选择子 35，把真实基址变为描述符的 0。下一次查询必须读到 0，而非旧软件字段。重新设置后，另将一个尚未映射的合法低地址登记并查询，再恢复有效本地块；整个过程中不访问那个缺页地址。

第一个孩子改为另一个 VA，验证抢占和失败 exec 后成功替换为 reset。父 wait 完成后仍读取自己的 43，再创建第二个孩子；第二个孩子解除 FS 指向的页，真实 FS:8 产生 PF14。父 wait 得到 −142 后仍读取 43，最后先将 FS 设为 0 再归还页。原 launch 最终核对整个会话资源恢复。

<!-- write: labs/airios/tls.c -->
```c
#include <airi_libc.h> // FS基址由自有系统调用设置；内存仍由用户页接口持有。
static uint64_t read_self(void) { // offset0存本地块自己的用户VA。
    uint64_t value; // 结果进入普通寄存器，不返回内核指针。
    __asm__ volatile("movq %%fs:0,%0" : "=r"(value) :: "memory"); // 地址是FS.base加0，再走当前用户页表。
    return value; // 这是本节自定的小块布局，尚非C运行库的TCB ABI。
}
static uint64_t read_value(void) { // 同一条指令在不同基址读取不同用户对象。
    uint64_t value; // 实际FS访问的值。
    __asm__ volatile("movq %%fs:8,%0" : "=r"(value) :: "memory"); // offset8保存当前身份的可辨识数据。
    return value; // 不用C数组下标冒充FS硬件寻址。
}
static void write_value(uint64_t value) { // 仍受当前用户映射的可写权限检查。
    __asm__ volatile("movq %0,%%fs:8" :: "r"(value) : "memory"); // 基址不变，数据写入本进程页面。
}
static int spin(uint64_t self, uint64_t expected) { // 不在循环中主动yield或进入内核。
    for (unsigned i = 0; i < 5000000; ++i) // 有界纯计算，PIT可在任意一轮抢占。
        if (read_self() != self || read_value() != expected) // 两个实际FS偏移都必须来自本进程。
            return 1; // 捕获另一个进程基址或遗漏恢复。
    return 0; // 本轮每次实际访问均符合当前身份。
}
int main(void) { // 每个普通静态ELF仍从FS=0开始，本节手工建立本地块。
    if (airi_fs_base()) // 前一个命令或shell不得把旧基址传给新ELF。
        return 192; // 不在未知环境中继续。
    uint64_t *page = airi_alloc(4096); // 页面所有权属于本进程，基址设置不会另行分配它。
    if (!page) // 明确处理资源失败。
        return 193; // 尚未改变FS。
    uint64_t *local = page + 8; // 从页内偏移64开始，FS基址不要求按页对齐。
    local[0] = (uintptr_t)local; local[1] = 42; // 0为self，8为value，其他字段尚不使用。
    if (airi_fs_set((uintptr_t)local) || airi_fs_base() != (long)(uintptr_t)local || read_self() != (uintptr_t)local || read_value() != 42) // 设置、查询和硬件访问分别核对。
        return 194; // 数值相同不足以替代实际寻址。
    for (unsigned i = 0; i < 3; ++i) { // 中间非规范地址、内核高半区与最大数值分别拒绝。
        uint64_t invalid[] = {0x0000800000000000ull, 0xffff800000000000ull, UINT64_MAX}; // 不让特权WRMSR替用户触发内核GP。
        if (airi_fs_set(invalid[i]) != -22 || airi_fs_base() != (long)(uintptr_t)local || read_value() != 42) // 失败必须保持旧基址和数据。
            return 195; // 参数失败不能污染后续返回路径。
    }
    __asm__ volatile("movw $35,%%ax; movw %%ax,%%fs" ::: "rax", "memory"); // 用户可装载合法数据段选择子，这会把实际FS基址改为该描述符的0。
    if (airi_fs_base() != 0 || airi_fs_set((uintptr_t)local) || read_value() != 42) // 入口必须读真实硬件，不能只返回旧软件缓存。
        return 208; // 当前ABI返回前统一设空选择子，再写完整基址。
    if (airi_fs_set(0x12345000) || airi_fs_base() != 0x12345000 || airi_fs_set((uintptr_t)local)) // 合法低半区数值可尚未映射；未访问就不应产生PF。
        return 209; // 设置基址不是分配页或检查未来全部offset。
    long child = airi_fork(); // 基址数值与用户页面都继承，页面物理内容独立复制。
    if (child < 0) // 不能将资源失败当父分支继续。
        return 196; // 保留可诊断错误。
    if (airi_fs_base() != (long)(uintptr_t)local || read_value() != 42) // 父子都在改变自己的环境前核对继承。
        return 197; // 子必须先得到旧本地块的42。
    if (!child) { // 子换成另一个VA，以暴露只切CR3而遗漏FS恢复的实现。
        page = airi_alloc(4096); // 子另持一页，原页仍留给退出统一回收。
        if (!page) // 尚未修改当前继承值。
            return 198; // 分配失败可正常退出。
        local = page + 8; local[0] = (uintptr_t)local; local[1] = 99; // 与父的基址和数据都不同。
        if (airi_fs_set((uintptr_t)local)) // 完整低半区值只在返回用户前提交硬件。
            return 199; // 明确拒绝设置失败。
    }
    uint64_t expected = child ? 42 : 99, start = airi_ticks(); // 两个进程在真实时间窗口内竞争运行。
    do { // 循环内部不主动交出CPU，由PIT打断两种不同基址。
        if (spin((uintptr_t)local, expected) || airi_fs_base() != (long)(uintptr_t)local) // 活访问与系统调用返回后都要正确。
            return 200; // 不能只在某一种陷入路径恢复。
    } while (airi_ticks() - start < 20); // 至少二十个真实tick，非宿主墙钟猜测。
    write_value(expected + 1); // FS写也必须落在当前身份自己的页面。
    if (local[1] != expected + 1 || airi_exec("broken") != -8 || read_value() != expected + 1) // 失败exec保留FS与旧映像对象。
        return 201; // 不将默认FS=0错误套给失败事务。
    if (!child) { // 孩子用成功exec观察重新初始化。
        airi_puts("[TLS_CHILD_OK] fork=inherited base=private preempt=preserved failed_exec=preserved\n"); // 来自真实FS读写检查。
        airi_exec("tls-reset"); // 成功不回旧函数；旧页面与基址归属一起撤销。
        return 202; // 不用这条失败返回冒充新程序完成。
    }
    int status = 1; // 保存同一孩子在新ELF中的退出状态。
    if (airi_waitpid(child, &status) != child || status || read_value() != 43) // 父仍使用原本地块，子写100不能影响父。
        return 203; // 等待和切换都不能串状态。
    child = airi_fork(); // 独立孩子验证基址不固定其页面。
    if (child < 0) // 没有身份就不能执行预期故障。
        return 204; // 正常错误返回。
    if (!child) { // 基址仍指向自己的page+64。
        if (airi_release(page, 4096)) // 完整解除该页USER映射，基址数值仍保持。
            return 205; // 未释放就不能声称发生缺页。
        return (int)read_value(); // 实际FS:8触发PF14，不能进入内核访问旧物理页。
    }
    if (airi_waitpid(child, &status) != child || status != -142 || read_value() != 43) // 父的页和基址仍在。
        return 206; // 非当前故障不能击穿父或内核。
    if (airi_fs_set(0) || airi_fs_base() || airi_release(page, 4096)) // 先停用，再由原所有者释放本地块。
        return 207; // FS设置本身不做内存归还。
    airi_puts("[TLS_USER_OK] fork=inherited timer_window=20 fs=private invalid=unchanged exec=reset released=pf14\n"); // 完整生命周期结束后才发布。
    return 0; // shell继续接收命令，会话清理核对资源恢复。
}
```

将两份真实用户 ELF 加入 images.S，各自的 main 分开链接。此前更新的 user.c 目录项使用这些起止范围；它们仍经过装载器，不是将用户函数直接链接成内核调用。

在 `labs/airios/images.S` 末尾追加以下内容，前章已有部分保留。

<!-- append: labs/airios/images.S -->
```asm

.balign 8 /* 两份FS观察程序仍是独立ELF字节，不是内核函数。 */
.global airi_tls_elf_start, airi_tls_elf_end /* 注册目录使用真实起止范围。 */
airi_tls_elf_start: /* 本配置的用户程序。 */
.incbin AIRI_TLS_ELF /* 路径由Makefile传入，不借宿主ABI。 */
airi_tls_elf_end: /* 当前ELF结束位置。 */
.balign 8 /* reset映像单独对齐。 */
.global airi_tls_reset_elf_start, airi_tls_reset_elf_end /* 成功exec的新目标。 */
airi_tls_reset_elf_start: /* 新映像独立入口。 */
.incbin AIRI_TLS_RESET_ELF /* main只查询新基址并输出。 */
airi_tls_reset_elf_end: /* 新ELF的右端。 */
```

更新 Makefile：内核 SOURCES 增加 tls.c，两个用户目标各自复用 USER_COMMON，images.o 等待它们并传入路径宏。K41 已建立对象对 Makefile 的依赖与用户 .d 文件，此处修改后会重编所需对象，无须把学习目录整体清空。

在 SOURCES 中加入架构 tls.c，已有 FPU 源保持。

<!-- replace: Makefile -->
<!-- before -->
```make
# 基础目标只编译文本会话与设备；不读取 frontend 的目录或字体。
SOURCES := kernel/main.c kernel/boot/limine.c kernel/boot/info.c \
    kernel/lib/base.c kernel/lib/runtime.c kernel/arch/x86_64/serial.c \
    kernel/console/console.c kernel/arch/x86_64/input.c kernel/arch/x86_64/cpu.c kernel/arch/x86_64/trap.c kernel/mm/pages.c kernel/mm/memory.c kernel/arch/x86_64/paging.c kernel/mm/heap.c kernel/arch/x86_64/timer.c kernel/task/task.c kernel/process/user.c kernel/process/elf.c kernel/fs/file.c kernel/arch/x86_64/fpu.c kernel/arch/x86_64/irq.c kernel/drivers/virtio_block.c kernel/storage/journal.c kernel/storage/demo.c kernel/storage/service.c kernel/io/engine.c kernel/io/service.c
# 只有 desktop- 前缀目标才读取图形支线自己的构建片段。
ifneq ($(filter desktop-%,$(PROFILE)),)
include frontend/sources.mk # 本分支取得图形自己的源码清单。
```
<!-- after -->
```make
# 基础目标只编译文本会话与设备；不读取 frontend 的目录或字体。
SOURCES := kernel/main.c kernel/boot/limine.c kernel/boot/info.c \
    kernel/lib/base.c kernel/lib/runtime.c kernel/arch/x86_64/serial.c \
    kernel/console/console.c kernel/arch/x86_64/input.c kernel/arch/x86_64/cpu.c kernel/arch/x86_64/trap.c kernel/mm/pages.c kernel/mm/memory.c kernel/arch/x86_64/paging.c kernel/mm/heap.c kernel/arch/x86_64/timer.c kernel/task/task.c kernel/process/user.c kernel/process/elf.c kernel/fs/file.c kernel/arch/x86_64/fpu.c kernel/arch/x86_64/tls.c kernel/arch/x86_64/irq.c kernel/drivers/virtio_block.c kernel/storage/journal.c kernel/storage/demo.c kernel/storage/service.c kernel/io/engine.c kernel/io/service.c
# 只有 desktop- 前缀目标才读取图形支线自己的构建片段。
ifneq ($(filter desktop-%,$(PROFILE)),)
include frontend/sources.mk # 本分支取得图形自己的源码清单。
```

在 Makefile 末尾追加两份 FS 观察 ELF 的构建与包装规则。

<!-- append: Makefile -->
```make

# 两份FS观察用户程序分别保留自己的main，仍共用静态用户运行入口。
$(OUT)/user/tls.elf: $(USER_COMMON) $(OUT)/user/tls.o labs/airios/program.ld
# 普通用户C已允许SSE2，本探针的FS访问由明确内嵌汇编执行。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/tls.o -o $@
# reset只观察exec后FS默认值，不借用旧本地块。
$(OUT)/user/tls-reset.elf: $(USER_COMMON) $(OUT)/user/tls-reset.o labs/airios/program.ld
# 新ELF不与tls的main同时链接。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/tls-reset.o -o $@
# 包装只等待本配置的两份用户ELF。
$(OUT)/labs/airios/images.o: $(OUT)/user/tls.elf $(OUT)/user/tls-reset.elf
# 文件路径通过预处理宏进入.incbin。
$(OUT)/labs/airios/images.o: CPPFLAGS += -DAIRI_TLS_ELF='"$(OUT)/user/tls.elf"' -DAIRI_TLS_RESET_ELF='"$(OUT)/user/tls-reset.elf"'
```

<a id="fs-observe"></a>
## 将地址检查与真实抢占放进日常入口

boot.py 的 normal 成功路径要求 TLS_BASE_READY。shell_test 在同一真实用户 shell 输入 tls，按本次 EXEC_COMMIT/FORK 关联父子 PID，要求两者实际 user_ticks 至少为 2。它核对父子检查、exec 清零、释放后故障和最终命令成功，不以一条启动设施标记代替全部行为。

在 CPU_STATE_READY 之后检查 FS 设施标记；这仍只是启动准备检查。

<!-- replace: scripts/boot.py -->
<!-- before -->
```python
                        assert '[FILES_USER_OK]' in text and '[FD_EXEC_OK]' in text # RAM文件和成功exec继承均完成。
                        assert '[PROCESS_FORK_WAIT_OK]' in text and '[PROCESS_DEMO_OK]' in text # 复制、真实阻塞、timer换身份和回收都须完成。
                        assert '[ELF_LOAD]' in text and '[ELF_USER_OK]' in text # 独立ELF段、BSS与用户libc必须真正运行。
                        assert '[CPU_STATE_READY] x87=1 sse2=1 fxsave_bytes=512 avx=off kernel=general-only' in text # 用户SSE编译前必须真正准备CPU保存边界。
                        assert '[USER_DEMO_OK]' in text # 用户输出、指针拒绝和用户故障后内核必须继续。
                        assert '[HEAP_CAPACITY_OK] initial=65536 bytes=1048576 live=0 chunks=16 preserved=1 limit=null' in text # 新映射、活对象保持、上限和缓存归属均实际演示。
```
<!-- after -->
```python
                        assert '[FILES_USER_OK]' in text and '[FD_EXEC_OK]' in text # RAM文件和成功exec继承均完成。
                        assert '[PROCESS_FORK_WAIT_OK]' in text and '[PROCESS_DEMO_OK]' in text # 复制、真实阻塞、timer换身份和回收都须完成。
                        assert '[ELF_LOAD]' in text and '[ELF_USER_OK]' in text # 独立ELF段、BSS与用户libc必须真正运行。
                        assert '[TLS_BASE_READY] register=FS direct_write=off kernel_base=0' in text # CPU基址设施不依赖完整用户TLS运行库。
                        assert '[CPU_STATE_READY] x87=1 sse2=1 fxsave_bytes=512 avx=off kernel=general-only' in text # 用户SSE编译前必须真正准备CPU保存边界。
                        assert '[USER_DEMO_OK]' in text # 用户输出、指针拒绝和用户故障后内核必须继续。
                        assert '[HEAP_CAPACITY_OK] initial=65536 bytes=1048576 live=0 chunks=16 preserved=1 limit=null' in text # 新映射、活对象保持、上限和缓存归属均实际演示。
```

在 simd 后运行 tls，从它自己的 EXEC_COMMIT/FORK 关联身份，再核对真实 tick 和释放页后的 PF。

<!-- replace: scripts/shell_test.py -->
<!-- before -->
```python
                for identity in (parent.group(1),fork.group(1)): # 两个用户都必须实际经过PIT中断窗口。
                    ended=re.search(r'\[USER_EXIT\] pid='+identity+r' code=0 user_ticks=(\d+)',simd_output) # exec保留同一PID的用户tick统计。
                    assert ended and int(ended.group(1))>=2 # 活寄存器检查跨真实时间片，而非只有主动yield。
                offset=len(text); wire.sendall(b'orphans\n'); text=wait('[SHELL_COMMAND_DONE] name=orphans code=0',offset) # 同一用户shell持续服务，不靠返回monitor收拢孙进程。
                orphan_output=text[offset:] # 只核对本条命令的身份记录。
                assert '[ORPHAN_USER_OK] rounds=24 live=12 zombie=12 session=alive' in orphan_output # 超过八槽容量仍能连续fork。
```
<!-- after -->
```python
                for identity in (parent.group(1),fork.group(1)): # 两个用户都必须实际经过PIT中断窗口。
                    ended=re.search(r'\[USER_EXIT\] pid='+identity+r' code=0 user_ticks=(\d+)',simd_output) # exec保留同一PID的用户tick统计。
                    assert ended and int(ended.group(1))>=2 # 活寄存器检查跨真实时间片，而非只有主动yield。
                offset=len(text); wire.sendall(b'tls\n'); text=wait('[SHELL_COMMAND_DONE] name=tls code=0',offset) # FS的读写必须跨真实父子抢占保持。
                tls_output=text[offset:] # 不复用启动或前一个用户程序的成功标记。
                assert '[TLS_USER_OK] fork=inherited timer_window=20 fs=private invalid=unchanged exec=reset released=pf14' in tls_output # 完整正常与释放后故障由guest检查。
                assert '[TLS_CHILD_OK]' in tls_output and '[TLS_EXEC_RESET_OK] fs_base=0 old_block=detached' in tls_output # 同一孩子成功换成新ELF。
                tls_parent=re.search(r'\[EXEC_COMMIT\] pid=(\d+) program=tls ',tls_output) # 本次命令的真实根PID。
                assert tls_parent # 不能用别的命令的fork满足条件。
                tls_fork=re.search(r'\[FORK\] parent='+tls_parent.group(1)+r' child=(\d+) ',tls_output) # 首个孩子参与不同基址的纯计算。
                assert tls_fork # 本次必须出现真实的两个可运行身份。
                for identity in (tls_parent.group(1),tls_fork.group(1)): # 二者都实际经过PIT窗口。
                    ended=re.search(r'\[USER_EXIT\] pid='+identity+r' code=0 user_ticks=(\d+)',tls_output) # exec仍使用同一身份累计值。
                    assert ended and int(ended.group(1))>=2 # 主动yield或单身份内存比较不算抢占证据。
                offset=len(text); wire.sendall(b'orphans\n'); text=wait('[SHELL_COMMAND_DONE] name=orphans code=0',offset) # 同一用户shell持续服务，不靠返回monitor收拢孙进程。
                orphan_output=text[offset:] # 只核对本条命令的身份记录。
                assert '[ORPHAN_USER_OK] rounds=24 live=12 zombie=12 session=alive' in orphan_output # 超过八槽容量仍能连续fork。
```

完整 shell 序列结束后，结果记录加入 fs_base_preempt_fork_exec。

<!-- replace: scripts/shell_test.py -->
<!-- before -->
```python
                assert '[USER_SHELL_EXIT]' in text[offset:] and 'program sh exited 0' in text[offset:] # 真实收拢页/FD/静态池由launch核对。
                offset=len(text); wire.sendall(b'echo after-shell\n'); text=wait('[CONSOLE_UPDATE] view=0 commands=2 page=0',offset) # 回到内核后输入仍完整。
                assert 'after-shell' in text[offset:] # 不只是退出而丢了后续UART。
                result={'passed':True,'ring3_shell':True,'ram_file_and_device_readback':True,'other_user_during_cq_wait':True,'qmp_iops':1,'error_exec_survived':True,'returned_to_monitor':True,'orphan_rounds':24,'memory_reuse_rounds':128,'capacity_rounds':4,'capacity_pages':1024,'simd_preempt_fork_exec':True} # 只发布已观察事实。
                result_path.write_text(json.dumps(result,indent=2)+'\n'); print('PASS user shell: alloc/file/async wait/other task/CQ/readback, failed exec, return to monitor') # 阶段演示实际完成。
                return result # 总入口可收集。
            finally: # 包括断言、超时和QEMU错误。
```
<!-- after -->
```python
                assert '[USER_SHELL_EXIT]' in text[offset:] and 'program sh exited 0' in text[offset:] # 真实收拢页/FD/静态池由launch核对。
                offset=len(text); wire.sendall(b'echo after-shell\n'); text=wait('[CONSOLE_UPDATE] view=0 commands=2 page=0',offset) # 回到内核后输入仍完整。
                assert 'after-shell' in text[offset:] # 不只是退出而丢了后续UART。
                result={'passed':True,'ring3_shell':True,'ram_file_and_device_readback':True,'other_user_during_cq_wait':True,'qmp_iops':1,'error_exec_survived':True,'returned_to_monitor':True,'orphan_rounds':24,'memory_reuse_rounds':128,'capacity_rounds':4,'capacity_pages':1024,'simd_preempt_fork_exec':True,'fs_base_preempt_fork_exec':True} # 只发布已观察事实。
                result_path.write_text(json.dumps(result,indent=2)+'\n'); print('PASS user shell: alloc/file/async wait/other task/CQ/readback, failed exec, return to monitor') # 阶段演示实际完成。
                return result # 总入口可收集。
            finally: # 包括断言、超时和QEMU错误。
```

全部修改完成后运行：

<!-- run -->
```sh
./airios build # 建立FS架构接口与两份用户ELF。
./airios test # 保留K41浮点、旧内存/进程/设备及服务路径的完整回归。
```

Mac 默认仍是旧 Python 时，在命令前加 `PATH=/opt/homebrew/bin:$PATH`。正常启动包含：

```text
[TLS_BASE_READY] register=FS direct_write=off kernel_base=0
```

交互 `./airios run`，输入 `run sh`，再输入用户命令 `tls`。实际成功路径包含：

```text
[TLS_CHILD_OK] fork=inherited base=private preempt=preserved failed_exec=preserved
[TLS_EXEC_RESET_OK] fs_base=0 old_block=detached
[USER_FAULT] pid=... vector=14 error=0x4 rip=... cr2=0x10000048
[TLS_USER_OK] fork=inherited timer_window=20 fs=private invalid=unchanged exec=reset released=pf14
[SHELL_COMMAND_DONE] name=tls code=0
```

CR2 是实际线性地址，即本轮释放页起点 0x10000000 加 64 再加 8。它不是 MSR 编号，也不是页的物理位置。PID 和 tick 由本次运行分配；父子退出后用户 shell 仍在，可以继续 hello、simd 或 exit 回内核监控台。

用 LLVM 的 llvm-objdump -d 查看 build/normal/user/tls.elf，可看到实际 `%fs:0x0`、`%fs:0x8` 读写。查看内核 tls 对象则可找到 rdmsr/wrmsr；前者读取用户值，后者清零或恢复数值，不复制用户页内容。

到这里有了自有基址接口与实际硬件观察，尚未让编译器生成的 `_Thread_local` 自动工作。后续要解释 ELF TLS 模板、每执行者的本地块和编译器偏移，再接 errno 与线程创建。Linux 实验仍按个人主机状态另记，本节没有用 Mac 的 FS/TLS 行为替代 Linux 实测。

