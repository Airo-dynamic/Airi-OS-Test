# K18 · 用真实时钟 IRQ 唤醒停下的 CPU

内存已能分配和映射，命令输入却仍靠主动轮询。现在让 CPU 执行 hlt 暂停，由周期事件把它叫醒：PIT 产生 IRQ0，PIC 提供向量 32，CPU 查 IDT32，汇编保存现场，C 增计数并确认服务结束，iretq 再回到被打断的位置。正常代码累计至少五次 IRQ 后才报告。

从 K17/L02 继续，新增 timer 接口和实现，接入异常分派、汇编、Make/main 与启动观察，内存保持。这里还不选另一个任务，时钟能进入不代表调度已完成。

<a id="pit-pic-route"></a>

## 设备在计数，CPU 是否收到取决于整条通路

PIT 输入约 1.193182 MHz，整数除数 11931 得约 100 Hz，不是精确墙钟。IRQ0 是设备连线编号，IDT32 是 CPU 的入口编号，PIC 以基址 0x20 建立对应；0–31 已归异常，不能重叠。

一次事件需同时满足设备产生、控制器未屏蔽、CPU IF=1。cli/sti 只改最后一项；中断门 0x8e 进入时清 IF，处理函数加 ticks 后发送 EOI，让 PIC 知道该次服务结束。少 EOI 可能只收到第一次，不能靠延长等待解决。

固定 q35 的 UEFI 还可能留下本地 APIC 已启用、LINT0 ExtINT 被屏蔽的状态，PIT/PIC 虽有请求，CPU 仍收不到。首版选择单核兼容 PIC，在 cli 期间仅清 IA32_APIC_BASE 全局使能并保留基址；x2APIC 明确拒绝。不配置 APIC MMIO、IOAPIC 或 SMP，其他平台需要自己的路由实现。

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

## 配置和软件状态全部就绪，最后才允许交付

timer_init 先关 IF、选择兼容路径并屏蔽两片 PIC，再写 ICW1–4。主基址 0x20、从基址 0x28，主片 IRQ2 级联所以位 2 置一，从片身份为 2，最后选 8086/软件 EOI。0x20/0xa0 为控制，0x21/0xa1 为初始化与掩码，均属固定 PC 驱动。

PIT 控制端口 0x43 写 0x34，选择通道 0、低后高字节、模式 2、二进制，再经 0x40 写完整除数。ticks 清零后只开放 IRQ0，配置文字也在 sti 前输出；若后面没收到 IRQ，至少知道已完成哪部分。键盘继续轮询，其余线屏蔽。

IRQ 只更新对齐的 64 位 volatile ticks 并 EOI，不写串口、不分配。单核读不撕裂，volatile 保留实际访问，不是多核同步。与 airi_halt 的 cli/hlt 不同，这里 IF=1 的 hlt 可以被时钟唤醒。

若 tick 恰在检查条件与 hlt 之间到达，下一周期仍会唤醒，elapsed 可以大于 5。本演示依赖周期事件，不声称已解决真正任务等待的丢唤醒；那需要后续阻塞协议。

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

端口助手现在能提交配置并保留 PIC 的 I/O 间隔。初始化先在 IF=0 时选择兼容路线、屏蔽并重映射 PIC，再给 PIT 交完整除数；软件 ticks 和最终掩码准备好以后才 sti。因而中断第一次进入时，硬件入口和计数对象都已有确定状态。

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

配置结束，IRQ0 已可经 IDT32 进入，但 TIMER_CONFIG 仍只证明初始化。下面的读取函数观察 ticks，IRQ 函数每次加一并 EOI；正常上下文用 start 作基准，在 hlt 之后重新读取差值，只有真实五次事件才到完成日志。

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

## 沿既有现场进入，但不要把时钟当错误

TRAP32 无硬件错误码，仍补零并保存通用寄存器，IDT32 指向它。C 分派要在普通异常报告之前识别 32，交给 timer_interrupt 后直接返回，否则每次 tick 都会误报甚至 PANIC。已有 iretq 路径恢复寄存器、原栈和 RFLAGS。

**局部更新 `kernel/arch/x86_64/traps.S`。** 原保存布局不变，只增加设备向量入口。

依次找到以下旧窗口，用对应新文本完整替换；其余实现保留。

<!-- replace: kernel/arch/x86_64/traps.S -->
<!-- before -->
```asm
TRAP 30, 1 /* 安全异常。 */
TRAP 31 /* 保留向量。 */
TRAP 255 /* 未使用的 IDT 项统一停止；可屏蔽中断仍关闭。 */
airi_trap_common: /* CPU 现场和 vector/error 已形成。 */
```
<!-- after -->
```asm
TRAP 30, 1 /* 安全异常。 */
TRAP 31 /* 保留向量。 */
TRAP 32 /* 主PIC IRQ0对应的时钟入口，无硬件错误码。 */
TRAP 255 /* 未使用的 IDT 项统一停止；可屏蔽中断仍关闭。 */
airi_trap_common: /* CPU 现场和 vector/error 已形成。 */
```

安装 IDT32 时给它明确入口，分派根据向量选择时钟；UD/GP/PF 仍执行自己的精确恢复或停止条件。设备成功处理与故障恢复不共用一套判定。

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

默认内核加入 timer.c；timer-masked 只在独立 profile 定义测试宏，不把故意屏蔽写入普通镜像。

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

main 在异常和内存就绪后初始化时钟，先完成五次唤醒演示再进入命令循环。之后 IRQ 仍持续计数，但不会在中断里调用任务切换。

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

启动观察除 READY 外增加真实 TIMER_DEMO_OK，要求本次已经经历设备入口；只打印配置或 CPU 仍运行都不足以通过。其余截止和清理沿用原工具。

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

运行正常回归，先检查设备配置，再检查累计事件后的完成记录。

<!-- run -->
```sh
./airios build  # 构建普通时钟镜像。
PYTHONPATH=scripts python3 - <<'PYTIMER'  # 原样交出Python源码。
from boot import check  # 同一有超时和清理的启动观察。
check('normal')  # 真正收到IRQ0、hlt唤醒后才能READY。
PYTIMER
```

既有正常记录先为 `[TIMER_CONFIG] lapic=disabled pic_mask=0xfe pit_divisor=11931`，再有 elapsed_ticks=5 的 TIMER_DEMO_OK，随后 READY 和命令循环。它证明实际事件经过 IDT32 返回，不是固定次数空循环。

<a id="masked-timer"></a>

## 同一设备仍运行，只切断 PIC 交付

timer-masked 将掩码改 0xff，PIT 仍计数、IF 仍打开，但 IRQ0 不能交到 CPU。它是预期超时的独立镜像，不在日常五配置列表，下面用已有 build 显式构建。观察仍有 35 秒期限，并要求先看到 TIMER_CONFIG，才将后续无完成解释为这项故障。

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

既有故障记录在 pic_mask=0xff 后超时，没有 TIMER_DEMO_OK/READY，检查器失败并回收 QEMU；普通构建不会继承该宏。

正常版若无 IRQ，QMP human-monitor-command 可查 info registers/pic/irq/lapic：IF=1 且 HLT=1 表示等待，IRR 位 0=1、IMR=0xfe 表示请求到达未屏蔽；若本地 LVT0 仍 masked，就核对兼容路径。首版曾遇到这组状态，修正交付才得到 tick，不以扩大超时掩盖它。

Linux 程序调用 clock_gettime/poll，不直接配置 PIT/PIC，内核也可选其他时钟与 tickless。本章只建立事件、掩码、入口、EOI、返回的关系，不代表现代 Linux 时钟实现。下一章让两个保留各自栈的任务主动交接，之后才有用户态抢占。

依据 [8254 手册](https://www.scs.stanford.edu/25sp-cs212/pintos/specs/8254.pdf)、[8259 兼容控制器章节](https://cdrdv2-public.intel.com/332995/332995-skl-io-platform-datasheet-vol1_rev004.pdf)、[Intel 系统编程手册](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html)。
