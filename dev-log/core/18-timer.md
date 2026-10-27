# K18 · 用真实时钟 IRQ 唤醒停下的 CPU

K17 已经能够分配小对象、查询内存，命令循环却仍不断检查设备。让 CPU 执行 `hlt` 可以暂停取指，省去暂停期间的主动检查；以后要继续执行，就需要一件不依赖当前代码循环的事来唤醒它。本章先接入周期计时设备，让它定期发出请求。请求到达 CPU 后进入中断处理，返回时继续执行 `hlt` 后面的指令。

我们用内存中的 `ticks` 记录实际处理过多少次计时请求。普通代码保存一个起始值，暂停并等待差值达到五，再输出结果。计时设备独立产生周期，CPU 执行处理函数，软件变量保存累计次数：这三个动作要逐一连接，等待循环才有理由最终结束。

本章从 K17 和 L02 继续，沿用已安装的异常表、寄存器保存和返回路径。新增时钟驱动，再把一种设备事件交给它处理。事件到达后仍返回原来的执行位置；下一章才让多个任务各自保留栈，并主动交出执行权。

<a id="pit-pic-route"></a>

<a id="设备在计数cpu-是否收到取决于整条通路"></a>
<a id="设备在计数，cpu-是否收到取决于整条通路"></a>

## 让计时设备的事件找到 CPU 入口

先从 CPU 以外的设备看这条通路。固定 PC 中的可编程间隔计时器 PIT 接收持续的输入时钟，按软件写入的除数计数，周期性改变输出信号。即使 CPU 正停在 `hlt`，PIT 仍继续计数。QEMU 在虚拟机器中模拟这个设备及其连接，内核通过设备端口配置它。

PIT 通道 0 的输出接到中断请求线 IRQ0，IRQ 是设备请求中断的英文简称。这条线把“有事件需要处理”的信号送到可编程中断控制器 PIC，没有传递一份 C 结构，也没有直接调用内核函数。PIC 汇集设备请求，在当前掩码和服务状态允许时向 CPU 请求交付，并在 CPU 应答时提供入口编号，也就是 K14 使用的向量。CPU 再用这个编号查 IDT，保存现场并进入汇编。

设备线编号与入口编号各有用途。这里将主 PIC 的向量基址配置为 0x20，即十进制 32；IRQ0 因而对应向量 32，IRQ1 对应 33。CPU 查 `idt[32]` 后进入 `airi_trap_32`，再由公共汇编调用 C。向量 0–31 已留给 CPU 异常，重映射使设备请求避开它们。整条路径是 PIT 周期输出、IRQ0、PIC 给出 32、CPU 查 IDT32，最后才到软件计数。

三处状态共同决定请求能否沿路到达。PIT 要完成周期配置，PIC 的 IRQ0 掩码位要允许交付，CPU 标志寄存器中的 IF 也要为 1。`cli/sti` 只改变 CPU 这一层，不会启动计时器或解除 PIC 掩码。中断门进入时 CPU 清 IF，避免当前处理被普通设备 IRQ 再次打断；原 RFLAGS 已在现场中，`iretq` 最后恢复它。PIC 也记住当前请求正在接受服务，处理函数要发送服务结束确认 EOI，后续 IRQ0 才能继续正常交付。恢复 IF 与确认 PIC 分别完成 CPU 和控制器这两侧的工作。

### 为固定单核机器选择 PIC 路线

这台 q35 还提供另一套中断控制器 APIC。每个 CPU 的本地 APIC 管理交付给该 CPU 的中断；设备也可以通过 I/O APIC 等路线接入。旧 PIC 的请求可经本地 APIC 的 LINT0 输入，以 ExtINT 模式转交。因而“配置了 PIT 和 PIC”还要接上机器此刻采用的路线：固件可能启用了本地 APIC，却屏蔽 LINT0，这时旧 PIC 的请求仍到不了 CPU。

本章固定单 CPU，选择兼容 PIC 路线。驱动在 IF=0 时读取 `IA32_APIC_BASE`，关闭本地 APIC 的全局使能，使旧 PIC 走这台机器的兼容交付通路。`IA32_APIC_BASE` 是 CPU 的型号专用寄存器 MSR，编号为 0x1b；`rdmsr/wrmsr` 专门按这个编号读取、写入，和 UART 的 I/O 端口属于不同接口。寄存器同时保存 APIC 物理基址与模式标志，代码分 low/high 接住 64 位值，只清 low 的位 11，保留基址和其他标志。

若位 10 表示已处于 x2APIC 模式，当前实现先报告不支持并停止，没有继续改写该模式。本章的这次选择只适用于固定 q35、单 CPU 环境；没有配置 APIC 内存映射寄存器、I/O APIC 路由或多 CPU 分配。

通路确定后，才给 PIT 选择周期。输入约为 1.193182 MHz，整数除数 `1193182/100=11931` 得到约 100 Hz，即每秒约一百个周期。内存中的 ticks 则在 C 处理函数每进入一次时加一，记录 CPU 实际处理次数。请求被屏蔽、合并或延迟时，设备周期数与这个软件计数可能不同；ticks 还不是日期、墙上时间或精确经过秒数。

**新建 `include/airi/timer.h`。** tick 是实际处理事件数，不直接当时间戳。

<!-- write: include/airi/timer.h -->
```c
#ifndef AIRI_TIMER_H // 时钟接口只声明一次。
#define AIRI_TIMER_H // 包含保护。
#include <stdint.h> // tick计数使用64位无符号值。
void airi_timer_init(void); // 固定PC平台：重映射PIC、配置PIT并只开放IRQ0。
uint64_t airi_timer_ticks(void); // 自启动以来实际处理过的IRQ0次数。
void airi_timer_interrupt(void); // 只供架构异常分派调用，先计数再确认中断。
void airi_timer_demo(void); // 等待真实时钟中断，证明halt后能继续执行。
#endif // 结束包含保护。
```

<a id="timer-config"></a>

<a id="配置和软件状态全部就绪最后才允许交付"></a>
<a id="配置和软件状态全部就绪，最后才允许交付"></a>

## 配置完整通路以后，再开放 CPU 的 IF

`airi_timer_init` 需要连续配置控制器、计时器和软件计数。在此期间先关闭 IF；即使设备此时产生请求，CPU 也不会进入尚未准备完整的时钟处理路径。函数最后一次 `sti` 才开放 CPU 这一层。K14 的 IDT 要先安装好，本章稍后还会把向量 32 指向专用入口；新增实现和接入都写完以后才能运行。

先准备驱动使用的端口写入函数和静态 ticks。`out8` 沿用 UART 驱动的指令约束，PIC 的初始化写入之间则经 0x80 端口保留 PC 兼容 I/O 间隔。ticks 由中断处理修改、普通执行路径读取，放在整个内核运行期间都存在的静态区。

**新建 `kernel/arch/x86_64/timer.c`：**

<!-- write: kernel/arch/x86_64/timer.c -->
```c
#include <airi/timer.h> // 共享IRQ0的初始化、读取与处理接口。
#include <airi/kernel.h> // 在正常上下文发布时钟观察结果。
static volatile uint64_t ticks; // 当前单CPU下，IRQ写、内核读取，volatile保留每次观察。
static void out8(uint16_t port, uint8_t value) { // 端口I/O只留在PC架构驱动中。
    __asm__ volatile("outb %0,%1" :: "a"(value), "Nd"(port)); // Nd允许8位立即端口或DX，a传入AL数据。
}
static void pic_write(uint16_t port, uint8_t value) { // 旧式PIC配置操作之间保留I/O间隔。
    out8(port, value); // 先提交本次配置字节。
    out8(0x80, 0); // PC兼容延迟端口；不是向UART输出文字。
}
```

### 屏蔽请求，重映射编号，再交给 PIT 一个完整除数

PC 的两片 PIC 各处理八条线，从片接在主片的 IRQ2，组成十六路设备编号。先把两片全部屏蔽，再写初始化命令字 ICW1–4。主、从控制端口分别为 0x20、0xa0，数据端口为 0x21、0xa1。同一个数据端口在初始化期间按顺序接收不同配置字节，初始化完成后才用来写中断掩码。

<a id="timer-control-words"></a>

配置的第一步向两片命令端口写 0x11。PIC 按 ICW1 的字段解释这个字节：位 4 宣告开始初始化，位 0 宣告后面还会提供 ICW4，其余位取零，完整值为二进制 `00010001 = 0x10 | 0x01`。

| ICW1 的位 | 字段 | 本章取值与后续动作 |
| --- | --- | --- |
| 7–5 | 旧 MCS-80/85 模式的地址位 | 000，后面采用 8086 模式，不使用这组位 |
| 4 | 初始化选择位 | 1，PIC 将本次写入解释为 ICW1，开始接收初始化序列 |
| 3 | LTIM，触发方式 | 0，采用边沿触发；本次 IRQ0 使用这条方式 |
| 2 | ADI，旧模式调用地址间隔 | 0，8086 模式不使用该字段 |
| 1 | SNGL，单片或级联 | 0，使用级联，因此后面需要 ICW3 描述主从连接 |
| 0 | IC4，是否还要 ICW4 | 1，因此 ICW3 之后还要发送 ICW4 |

这个字节先确定后续配置的格式，尚未给出 IRQ 对应哪个向量。ICW2 才填向量基址，ICW3 填级联关系；最后的 ICW4=1 将位 0 的微处理器模式置为 8086，位 1 的 AEOI 保持零，让服务结束由软件发送 EOI。字段格式见 [Intel 8259A 原始数据手册，231468-003，图 7](https://pdos.csail.mit.edu/6.828/2010/readings/hardware/8259A.pdf#page=11)。初始化序列会重新建立控制器状态，CPU 的 IF 在整个过程中仍为零，最终掩码会在序列以后重写。

向主片数据端口写基址 0x20，将 IRQ0–7 放到向量 32–39；向从片写 0x28，将 IRQ8–15 放到 40–47。端口号 0x20 与向量基址 0x20 恰好数值相同，前者选择设备接口，后者是写入设备的配置值。随后主片接收 4，表示位 2 对应的输入连接从片；从片接收 2，标明自己的级联位置。两片都采用软件 EOI，处理结束时由内核明确确认。

PIC 已知道怎样交付 IRQ0，接着配置产生这个请求的 PIT。向控制端口 0x43 写的 0x34 是四个字段的组合，二进制按字段分开为 `00 | 11 | 010 | 0`：

| PIT 控制字的位 | 字段 | 本章取值与作用 | 移到目标位后的值 |
| --- | --- | --- | --- |
| 7–6 | SC，选择计数通道 | 00，选择接 IRQ0 的通道 0 | `0<<6 = 0` |
| 5–4 | RW，计数值的读写格式 | 11，先低八位、再高八位 | `3<<4 = 0x30` |
| 3–1 | Mode，计数模式 | 010，模式 2 周期速率发生器 | `2<<1 = 0x04` |
| 0 | BCD，数值表示 | 0，采用 16 位二进制计数 | `0` |

按位或后得到 `(0<<6) | (3<<4) | (2<<1) | 0 = 0x34`，与控制端口的写入值对应。字段决定后续两个字节怎样组成同一个计数值；模式 2 则使设备持续产生周期输出。格式见 [Intel 8254 原始数据手册，231164-005，图 7](https://www.scs.stanford.edu/25sp-cs212/pintos/specs/8254.pdf#page=6)。

除数 `11931 = 0x2e9b` 通过八位端口 0x40 分两次写入：`(uint8_t)divisor` 取低字节 0x9b，`(uint8_t)(divisor>>8)` 取高字节 0x2e。PIT 按 RW=11 将它们接回 `0x9b | (0x2e<<8) = 0x2e9b`，完整除数才到达设备。ticks 随后清零，再写最终 PIC 掩码 0xfe。PIC 用位 1 表示屏蔽，0xfe 的最低位为零，只开放 IRQ0；从片保持 0xff。键盘仍由原来的轮询路径读取。

正常代码在 `sti` 前发送 TIMER_CONFIG，留下本次路线、掩码和除数。故障 profile 只将主片掩码改成 0xff，其他准备照常完成；后面的观察可以据这条配置记录判断执行已到哪里。

**继续追加时钟配置：**

<!-- append: kernel/arch/x86_64/timer.c -->
```c
void airi_timer_init(void) { // 调用前已安装自己的IDT，内核目前仍关闭IF。
    __asm__ volatile("cli" ::: "memory"); // 配置PIC的过程中不接收未完成重映射的IRQ。
    uint32_t low, high; // IA32_APIC_BASE同时包含物理基址和APIC模式标志。
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(0x1bu)); // 固定qemu64具备本地APIC，读取其全局使能状态。
    if (low & (1u << 10)) // 当前不实现x2APIC状态迁移。
        airi_panic("legacy PIC timer does not support x2APIC mode"); // 拒绝未支持配置而不是永远等不到IRQ。
    low &= ~(1u << 11); // 关闭本地APIC全局使能，选择单核PC兼容PIC直接交付路径。
    __asm__ volatile("wrmsr" :: "a"(low), "d"(high), "c"(0x1bu) : "memory"); // 保留APIC物理基址和其他标志。
    pic_write(0x21, 0xff); // 首先屏蔽主PIC全部八路。
    pic_write(0xa1, 0xff); // 屏蔽从PIC，键盘继续使用轮询而不是IRQ。
    pic_write(0x20, 0x11); // 主PIC初始化：级联、边沿、后续提供ICW4。
    pic_write(0xa0, 0x11); // 从PIC使用同一模式。
    pic_write(0x21, 0x20); // 主PIC IRQ0–7映射到IDT32–39，避开CPU异常0–31。
    pic_write(0xa1, 0x28); // 从PIC IRQ8–15映射到IDT40–47。
    pic_write(0x21, 4); // 主PIC的IRQ2连接从PIC，位2置一。
    pic_write(0xa1, 2); // 从PIC的级联身份为2。
    pic_write(0x21, 1); // 主PIC使用8086模式，软件发送EOI。
    pic_write(0xa1, 1); // 从PIC同样不自动EOI。
    uint16_t divisor = 1193182u / 100u; // PIT输入频率近似1.193182MHz，整数分频得到约100Hz。
    out8(0x43, 0x34); // 通道0、先低后高两字节、模式2周期计数、二进制。
    out8(0x40, (uint8_t)divisor); // 先写分频器低八位。
    out8(0x40, (uint8_t)(divisor >> 8)); // 再写高八位，完整周期配置生效。
    ticks = 0; // 在开放IRQ之前初始化软件计数。
    uint8_t mask = 0xfe; // 普通配置只开放IRQ0。
#ifdef AIRI_TEST_TIMER_MASKED // 作者故障镜像故意屏蔽所有设备IRQ。
    mask = 0xff; // CPU可以开启IF，但没有任何PIC输入获准交付。
#endif // 普通镜像保留IRQ0。
    pic_write(0x21, mask); // 其他设备继续屏蔽。
    pic_write(0xa1, 0xff); // 从PIC仍全部屏蔽，没有未实现的设备IRQ入口。
    airi_log("[TIMER_CONFIG] lapic=disabled pic_mask=0x%x pit_divisor=%u", (unsigned)mask, (unsigned)divisor); // 尚未开放IF时记录硬件配置。
    __asm__ volatile("sti" ::: "memory"); // 允许CPU接收已经安装入口的时钟IRQ。
}
```

### 中断只增加计数，普通代码等待计数再继续

TIMER_CONFIG 在 `sti` 以前输出，此时只完成配置。下一次获准的设备请求经 IDT32 和公共汇编进入 C，`airi_trap_dispatch` 再调用 `airi_timer_interrupt`，ticks 才增加。处理函数随后向主 PIC 的命令端口 0x20 写 EOI 命令值 0x20，结束本次服务；IRQ0 位于主片，不需要从片确认。EOI 只确认控制器，不替 CPU 恢复现场。这段处理不分配内存、不写串口，返回后的公共汇编负责恢复，日志由普通代码发送。

普通代码通过读取函数每次访问同一个 `volatile` ticks。当前单核 x86-64 上，对齐的 64 位读取不会取得撕裂的两个半值，普通代码也不会与另一个 CPU 同时更新它；`volatile` 要求保留实际访问，不提供多核互斥。演示先取 start：`sti` 以后可能已经处理过某次 IRQ，所以起始值无需为零。以后只看 `ticks-start`，等待从这个基准起又处理至少五次。

若差值尚小于五，普通代码执行 `hlt`。PIT 不依赖这段代码继续产生周期，IF=1 允许下一次获准请求唤醒 CPU。CPU 保存的返回位置在 `hlt` 后面，处理函数计数、EOI，公共汇编 `iretq` 恢复后继续检查循环条件。同一普通执行流因此多次暂停又继续，没有切换任务。

如果一次 tick 恰在条件读取与 `hlt` 之间到达，CPU 仍可能继续执行 `hlt`，下一周期才再唤醒它，所以 elapsed 可以超过 5。这里依赖周期事件继续到来；只有一次通知的任务等待还需要登记和重查条件，后续再建立那套协议。原 `airi_halt` 则先执行 `cli`，不采用这条周期唤醒路径。

**最后追加读取、IRQ 处理与唤醒演示：**

<!-- append: kernel/arch/x86_64/timer.c -->
```c
uint64_t airi_timer_ticks(void) { // x86-64对齐64位读取不撕裂，当前无多核并发。
    return ticks; // 每次真正读取IRQ更新过的对象。
}
void airi_timer_interrupt(void) { // IDT32中断门进入时CPU已经清IF。
    ++ticks; // 先记录本次实际完成入口的时钟事件。
    out8(0x20, 0x20); // 向主PIC发送EOI，允许后续IRQ0继续到达。
}
void airi_timer_demo(void) { // 此时只演示中断唤醒，不存在任务调度。
    airi_timer_init(); // 开启固定平台的周期时钟。
    uint64_t start = airi_timer_ticks(); // 记录当前基准，不假定初始化后尚未经过第一个tick。
    while (airi_timer_ticks() - start < 5) // 至少处理五个真实IRQ，而不是忙等某个常数。
        __asm__ volatile("hlt" ::: "memory"); // IF为一，下一次IRQ唤醒后从这里继续。
    airi_log("[TIMER_DEMO_OK] source=pit-pic vector=32 target_hz=100 elapsed_ticks=%llu", // 正常上下文输出，IRQ里不调用串口或分配器。
             (unsigned long long)(airi_timer_ticks() - start)); // 计数可以超过5，不把整数分频当精确墙钟。
}
```

<a id="timer-entry"></a>

<a id="沿既有现场进入但不要把时钟当错误"></a>
<a id="沿既有现场进入，但不要把时钟当错误"></a>

## 把向量 32 接进已有的现场保存与返回路径

驱动已经能够配置通路并处理计数，现在补上 PIC 与它之间的软件入口。PIC 给出的向量 32 先用于 CPU 查 IDT；CPU 不会自动把这个编号写进 K14 的 vector 槽，仍由对应入口 `TRAP 32` 压入。时钟 IRQ 没有硬件错误码，宏先补零，再走原有通用寄存器保存，交给 C 的布局保持不变。

分派函数在一般异常报告之前识别 32，调用 `airi_timer_interrupt` 并返回。随后公共汇编恢复寄存器、原栈和 RFLAGS；被打断时 IF=1，返回后又允许后续时钟交付。设备请求只是暂时打断正常执行，恢复位置已经由 CPU 保存，无须像 UD 演示那样登记故障指令并跳过它。

**局部更新 `kernel/arch/x86_64/traps.S`。** 原保存布局不变，只增加设备向量入口。

下方旧窗口继承 K14 的 `TRAP 255` 停止入口。本章仍保留这个入口，但“可屏蔽中断仍关闭”的原旁注只适用于时钟初始化以前：前面函数末尾的 `sti` 已允许 IRQ0。其他设备线继续由 PIC 掩码屏蔽；未使用的 IDT 项若被进入，仍走统一停止路径。

依次找到以下旧窗口，用对应新文本完整替换；其余实现保留。

<!-- replace: kernel/arch/x86_64/traps.S -->
<!-- before -->
```asm
TRAP 30, 1 /* 安全异常。 */
TRAP 31 /* 保留向量。 */
TRAP 255 /* 未单独登记的 IDT 项进入统一停止入口。 */
airi_trap_common: /* CPU 现场和 vector/error 已形成。 */
```
<!-- after -->
```asm
TRAP 30, 1 /* 安全异常。 */
TRAP 31 /* 保留向量。 */
TRAP 32 /* 主PIC IRQ0对应的时钟入口，无硬件错误码。 */
TRAP 255 /* 未单独登记的 IDT 项进入统一停止入口。 */
airi_trap_common: /* CPU 现场和 vector/error 已形成。 */
```

### IDT 选择时钟入口，C 分派再交给计数函数

只生成 `airi_trap_32` 还没有使 CPU 使用它。K14 将 32 以上的 IDT 槽位都指向停止入口，初始化现在要专门令 `idt[32]` 指向新入口。C 文件同时声明这个汇编符号，并引入时钟接口；向量 6、13、14 等原有异常继续执行各自的登记恢复或停止条件。

**局部更新 `kernel/arch/x86_64/trap.c`：**

依次找到以下旧窗口，用对应新文本完整替换；其余实现保留。

<!-- replace: kernel/arch/x86_64/trap.c -->
<!-- before -->
```c
#include <airi/trap.h> // 异常现场的 C 布局。
#include <airi/kernel.h> // 日志、清零和停止接口。
```
<!-- after -->
```c
#include <airi/timer.h> // 唯一开放的设备IRQ交给架构时钟驱动。
#include <airi/trap.h> // 异常现场的 C 布局。
#include <airi/kernel.h> // 日志、清零和停止接口。
```

继续替换下一处：

<!-- replace: kernel/arch/x86_64/trap.c -->
<!-- before -->
```c
static uint8_t double_fault_stack[16384] __attribute__((aligned(16))); // 原栈损坏时 DF 换这块栈。
extern const uintptr_t airi_trap_entries[32]; // 汇编产生的异常入口地址表。
extern void airi_trap_255(void); // 未启用的向量转到统一停止路径。
extern void airi_tables_load(const void *, const void *); // 汇编依次加载 GDT/TSS/IDT。
```
<!-- after -->
```c
static uint8_t double_fault_stack[16384] __attribute__((aligned(16))); // 原栈损坏时 DF 换这块栈。
extern const uintptr_t airi_trap_entries[32]; // 汇编产生的异常入口地址表。
extern void airi_trap_32(void); // IRQ0也复用完整通用寄存器保存入口。
extern void airi_trap_255(void); // 未启用的向量转到统一停止路径。
extern void airi_tables_load(const void *, const void *); // 汇编依次加载 GDT/TSS/IDT。
```

继续替换下一处：

<!-- replace: kernel/arch/x86_64/trap.c -->
<!-- before -->
```c
    for (unsigned i = 0; i < 256; ++i) { // 全部项目初始化，避免继承加载器状态。
        uintptr_t address = i < 32 ? airi_trap_entries[i] : (uintptr_t)airi_trap_255; // 未使用向量也有明确停止入口。
        idt[i] = (struct idt_gate){(uint16_t)address, 8, i == 8 ? 1 : 0, 0x8e, (uint16_t)(address >> 16), (uint32_t)(address >> 32), 0}; // DPL0 中断门，只有 DF 使用 IST1。
    }
```
<!-- after -->
```c
    for (unsigned i = 0; i < 256; ++i) { // 全部项目初始化，避免继承加载器状态。
        uintptr_t address = i < 32 ? airi_trap_entries[i] : (uintptr_t)airi_trap_255; // 未使用向量也有明确停止入口。
        if (i == 32) // 唯一开放的设备向量有自己的编号，不能冒充未知255。
            address = (uintptr_t)airi_trap_32; // 指向TRAP32生成的入口。
        idt[i] = (struct idt_gate){(uint16_t)address, 8, i == 8 ? 1 : 0, 0x8e, (uint16_t)(address >> 16), (uint32_t)(address >> 32), 0}; // DPL0 中断门，只有 DF 使用 IST1。
    }
```

继续替换下一处：

<!-- replace: kernel/arch/x86_64/trap.c -->
<!-- before -->
```c
}
void airi_trap_dispatch(struct airi_trap_frame *f) { // 汇编传入的指针指向当前异常栈。
    uint64_t cr2; // 读取最近页故障的线性地址，非 PF 时仅作为附带状态。
    __asm__ volatile("mov %%cr2,%0" : "=r"(cr2)); // =r 请求输出到编译器分配的通用寄存器。
```
<!-- after -->
```c
}
void airi_trap_dispatch(struct airi_trap_frame *f) { // 汇编传入的指针指向当前异常栈。
    if (f->vector == 32) { // PIC重映射后的时钟，不能作为CPU故障停止。
        airi_timer_interrupt(); // 计数并确认IRQ，保持当前任务现场不变。
        return; // 仍由同一汇编入口恢复全部寄存器和RFLAGS。
    }
    uint64_t cr2; // 读取最近页故障的线性地址，非 PF 时仅作为附带状态。
    __asm__ volatile("mov %%cr2,%0" : "=r"(cr2)); // =r 请求输出到编译器分配的通用寄存器。
```

## 让正常启动先经过五次设备唤醒

入口与分派接好后，将 timer.c 加入默认内核。独立的 timer-masked profile 只改变本次构建的掩码宏，普通镜像仍只开放 IRQ0；两份对象和镜像按既有 profile 目录分开。

**局部更新 `Makefile`：**

依次找到以下旧窗口，用对应新文本完整替换；其余实现保留。

<!-- replace: Makefile -->
<!-- before -->
```make
SOURCES := kernel/main.c kernel/boot/limine.c kernel/boot/info.c \
    kernel/lib/base.c kernel/lib/runtime.c kernel/arch/x86_64/serial.c \
    kernel/console/console.c kernel/arch/x86_64/input.c kernel/arch/x86_64/cpu.c kernel/arch/x86_64/trap.c kernel/mm/pages.c kernel/mm/memory.c kernel/arch/x86_64/paging.c kernel/mm/heap.c
# 只有 desktop- 前缀目标才读取图形支线自己的构建片段。
ifneq ($(filter desktop-%,$(PROFILE)),)
```
<!-- after -->
```make
SOURCES := kernel/main.c kernel/boot/limine.c kernel/boot/info.c \
    kernel/lib/base.c kernel/lib/runtime.c kernel/arch/x86_64/serial.c \
    kernel/console/console.c kernel/arch/x86_64/input.c kernel/arch/x86_64/cpu.c kernel/arch/x86_64/trap.c kernel/mm/pages.c kernel/mm/memory.c kernel/arch/x86_64/paging.c kernel/mm/heap.c kernel/arch/x86_64/timer.c
# 只有 desktop- 前缀目标才读取图形支线自己的构建片段。
ifneq ($(filter desktop-%,$(PROFILE)),)
```

继续替换下一处：

<!-- replace: Makefile -->
<!-- before -->
```make
CPPFLAGS += -DAIRI_TEST_NO_MEMMAP
endif # 结束 missing-memory 的编译条件
# 作者故障配置不登记恢复，应报告真实异常后停止。
ifeq ($(CASE),trap-fatal)
```
<!-- after -->
```make
CPPFLAGS += -DAIRI_TEST_NO_MEMMAP
endif # 结束 missing-memory 的编译条件
# 时钟故障镜像仅供K18作者演示超时，不进入正常启动配置集合。
ifeq ($(CASE),timer-masked)
CPPFLAGS += -DAIRI_TEST_TIMER_MASKED # PIT仍计数，但PIC阻止IRQ0到达CPU。
endif # 结束时钟故障条件。
# 作者故障配置不登记恢复，应报告真实异常后停止。
ifeq ($(CASE),trap-fatal)
```

### 主函数在异常与内存就绪以后调用演示

`airi_memory_init` 返回时，异常报告和内存服务已经可用。主函数接着调用 `airi_timer_demo`，由它初始化 PIT/PIC、开启 IF、取得 start，并等待真实计数差值达到五，再初始化原有控制台。进入命令循环以后，周期 IRQ 仍继续增加 ticks，处理函数每次都返回被打断的位置；本章尚未给这份计数安排任务切换工作。

**局部更新 `kernel/main.c`：**

依次找到以下旧窗口，用对应新文本完整替换；其余实现保留。

<!-- replace: kernel/main.c -->
<!-- before -->
```c
#include <airi/memory.h> // 通过内存服务建立页池、页表、堆并查询快照。
#include <airi/trap.h> // 安装异常表并演示处理器现场。
```
<!-- after -->
```c
#include <airi/timer.h> // 内存建立后开始真实设备中断。
#include <airi/memory.h> // 通过内存服务建立页池、页表、堆并查询快照。
#include <airi/trap.h> // 安装异常表并演示处理器现场。
```

继续替换下一处：

<!-- replace: kernel/main.c -->
<!-- before -->
```c
    airi_log("[LESSON] %s", AIRI_DEMO_MESSAGE);  // 发送当前配置的学习文本，修改实验可据此核对版本。
    airi_memory_init(info); // 顺序建立页池、自有映射与固定内核堆，完成本阶段演示。
    airi_console_init(&console, info);  // 初始化内核静态控制台，借用整个启动期间有效的 info。
    console.read_memory = airi_memory_snapshot; // 文本查询只读取数值，不持有分配器内部对象。
```
<!-- after -->
```c
    airi_log("[LESSON] %s", AIRI_DEMO_MESSAGE);  // 发送当前配置的学习文本，修改实验可据此核对版本。
    airi_memory_init(info); // 顺序建立页池、自有映射与固定内核堆，完成本阶段演示。
    airi_timer_demo(); // 从PIT/PIC收到真实IRQ0，hlt后恢复到正常上下文。
    airi_console_init(&console, info);  // 初始化内核静态控制台，借用整个启动期间有效的 info。
    console.read_memory = airi_memory_snapshot; // 文本查询只读取数值，不持有分配器内部对象。
```

### 主机观察先等设备完成标记，再接受 READY

TIMER_DEMO_OK 只在等待循环结束以后发送，启动观察因此把它加入正常配置的成功条件。原来的异常演示、内存演示和 READY 仍要全部出现。观察器继续使用新日志、截止时间和 finally 清理，不改变其他启动条件。

**局部更新 `scripts/boot.py`：**

依次找到以下旧窗口，用对应新文本完整替换；其余实现保留。

<!-- replace: scripts/boot.py -->
<!-- before -->
```python
                        assert '[AIRI_P1_READY]' not in text  # 错误路径不得出现成功标记。
                    else:  # 按当前profile的另一条完整验证路径处理。
                        assert '[AIRI_P2_READY]' in text # 当前主线还必须完成页池、自有页表和堆。
                        assert '[TRAP_DEMO_OK] ud=6 gp=13 resumed=2' in text # 正常启动必须真的从两种异常栈返回。
```
<!-- after -->
```python
                        assert '[AIRI_P1_READY]' not in text  # 错误路径不得出现成功标记。
                    else:  # 按当前profile的另一条完整验证路径处理。
                        assert '[TIMER_DEMO_OK]' in text # 时钟必须在真实IRQ唤醒后完成。
                        assert '[AIRI_P2_READY]' in text # 当前主线还必须完成页池、自有页表和堆。
                        assert '[TRAP_DEMO_OK] ud=6 gp=13 resumed=2' in text # 正常启动必须真的从两种异常栈返回。
```

全部编辑写齐后，先构建本次普通镜像，再让已有 check 启动并收取它的输出。整个运行块由同一个 `sh -eu` 执行，构建非零退出就停止，不再观察旧镜像。正常输出顺序应为设备配置、计数达到目标、随后继续启动。

<!-- run -->
```sh
sh -eu <<'SH' # 本组任一构建或观察失败即停止，结束词前的内容原样交给sh。
./airios build  # 构建普通时钟镜像。
PYTHONPATH=scripts python3 - <<'PYTIMER'  # 原样交出Python源码。
from boot import check  # 同一有超时和清理的启动观察。
check('normal')  # 真正收到IRQ0、hlt唤醒后才能READY。
PYTIMER
SH
```

2026-10-15 的正常记录先出现 `[TIMER_CONFIG] lapic=disabled pic_mask=0xfe pit_divisor=11931`，再出现 elapsed_ticks=5 的 TIMER_DEMO_OK，随后进入 READY 和命令循环。配置记录给出选择的通路与周期，完成记录才说明 IDT32 处理实际增加了计数；等待循环自身只读取它。其他次运行的差值可以大于五，这份历史值不固定每次唤醒次数。

<a id="masked-timer"></a>

<a id="同一设备仍运行只切断-pic-交付"></a>
<a id="同一设备仍运行，只切断-pic-交付"></a>

## 保留计时设备，只屏蔽它到 CPU 的请求

再用相同路径定位一次等待不结束的原因。timer-masked 将掩码改成 0xff，PIT 仍计数、CPU 的 IF 仍打开，PIC 却不再交付 IRQ0。ticks 因而不增加，演示停在 `hlt`，也到不了后面的 READY。

这份故障镜像不在日常五配置列表中，下面用已有 build 显式构建。check 仍有 35 秒期限，并在结束时回收 QEMU。只有日志已经出现 pic_mask=0xff 的 TIMER_CONFIG，再加上后续超时，才符合这次故障的结果；若在固件阶段就退出或等待，日志中还没有驱动配置，属于另一处启动问题。

<!-- run -->
```sh
PYTHONPATH=scripts python3 - <<'PYMASK'  # 引用结束词，保持源码原字节。
from manage import build  # 使用统一入口已有构建函数，故障目录独立。
from boot import check  # 超时仍会回收本轮QEMU。
from tools import ROOT  # 读取当前镜像的证据目录。
build('timer-masked')  # Make为此场景定义屏蔽IRQ0的宏。
try:  # 预期检查器拒绝把等待中的guest当成就绪。
    check('timer-masked')  # 仍使用正常35秒上界，不改成永不超时。
except RuntimeError as error:  # 只接受预期超时和具体guest配置同时成立。
    text = (ROOT / 'artifacts/boot-timer-masked/serial.log').read_text()  # 本轮日志已由check清旧。
    assert 'timeout' in str(error) and 'pic_mask=0xff' in text  # 必须实际进入驱动配置，不能在UEFI阶段就失败。
    assert '[TIMER_DEMO_OK]' not in text and '[AIRI_P2_READY]' not in text  # 禁止没有IRQ却宣布完成。
    print('PASS negative timer: masked IRQ never became ready')  # 只说明失败路径按预期被检查器拒绝。
else:  # 没有抛错意味着被屏蔽的设备竟完成等待。
    raise RuntimeError('masked timer unexpectedly became ready')  # 明确拒绝假阳性。
PYMASK
```

同一天的故障记录在 pic_mask=0xff 后超时，没有 TIMER_DEMO_OK/READY；检查器拒绝这次启动并结束 QEMU。普通构建的对象目录与宏独立，重新运行 normal 时仍采用 0xfe。

正常版收不到 IRQ 时，可以沿三层通路分别看状态。K12 已建立的 QMP 控制连接可通过 `human-monitor-command` 调用 QEMU 的 `info irq` 看设备请求，`info pic` 看控制器，`info registers` 看 CPU，再用 `info lapic` 核对路由。CPU 的 IF=1、HLT=1 表示它正在允许中断的暂停状态；PIC 的 IRR 保存待处理请求，位 0=1 表示 IRQ0 已到达控制器；IMR 为 0xfe 才允许交付这一路。若本地 APIC 仍然启用，且 LVT0 为 masked ExtINT，要继续核对是否实际执行了本章关闭本地 APIC 的初始化。历史首版记录正是在这处路由阻断，修正后才开始增加 ticks。

现在同一 CPU 可以暂停，经过设备入口处理以后继续，这为后续等待和调度提供了时间事件。K19 先让任务在自己的栈上主动交接，用户态抢占再接入周期计数。Linux 应用则通过 `clock_gettime`、`poll` 等接口取得时间和等待服务，由 Linux 内核选择具体计时器及交付方式；这里的 PIT/PIC 配置限定于当前单核 q35。

依据 [8254 手册](https://www.scs.stanford.edu/25sp-cs212/pintos/specs/8254.pdf)、[8259 兼容控制器章节](https://cdrdv2-public.intel.com/332995/332995-skl-io-platform-datasheet-vol1_rev004.pdf)、[Intel 系统编程手册](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html)。
