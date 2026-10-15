# 06 · 让引导器进入自己的 C 代码，并发出第一行文字

当前主机观察程序能用 printf 打印，但 printf 所依赖的 C 库、操作系统输出接口和启动运行时都由主机提供。把这些程序变成 AiriOS 内核后，下面没有另一个操作系统替它输出。第一次启动需要一条由自己完成的路径：引导器跳到入口，汇编准备 C 函数使用的栈，C 向设备写入文字，主机取得这些文字。

## 从内核的一行文字到主机日志

<a id="serial-path"></a>

第 04 章已用串口说明字节与文字的关系。现在选择 QEMU 模拟的 COM1 串口设备。COM1 是第一路传统 PC 串口的名称；UART 是其中收发字节的控制器，软件写一个字节，控制器负责把它送到串行通路。QEMU 可以把通路另一端接到主机文件或终端。窗口显示文字是整条传递路径末端的动作。

本章准备发送 `[AIRI_LESSON_READY] _start -> C -> UART`。字符串先存在内核内存中，发送循环逐字节读它，UART 接受字节后交给 QEMU 的主机输出。下一章启动脚本用 `-serial file:...` 将字节收集到 serial.log，读取文件后才在 Mac 终端显示。这里的文件是主机上的观察记录，内核尚未实现文件系统。

内核头文件先声明串口、日志、错误报告和停机接口。`_Noreturn` 告诉编译器这个调用不会正常返回，实际停机仍由机器指令执行。`airi_dashboard_draw` 为可选显示实现保留接口，当前没有调用它。函数声明只说明调用形式；没有调用便没有待链接的引用，所以此时可以只有声明。

**新建 `include/airi/kernel.h`：**

<a id="code-include-airi-kernel-h-1"></a>

<!-- write: include/airi/kernel.h -->
```c
#ifndef AIRI_KERNEL_H   // 首次包含内核共用接口时进入，防止重复声明
#define AIRI_KERNEL_H   // 标记本头已处理，末尾 endif 结束保护范围
#include <airi/boot.h>  // 使用双方共同的字段布局和枚举常量
#include <airi/base.h>  // 按公开声明编译自己的字节函数，保证定义与调用使用同一签名

void airi_serial_init(void);  // 配置 COM1 UART，之后数据端口才能按当前格式发送字符
void airi_serial_write(const char *s);  // 同步发送 NUL 之前的字节，不保留调用者指针
void airi_log(const char *fmt, ...);    // 在局部数组形成日志，再通过串口逐字发送
_Noreturn void airi_halt(void);  // 使当前 CPU 停在不返回的循环中，QEMU 主机进程仍存在
_Noreturn void airi_panic(const char *message);      // 先报告具体错误再停机，参数须为有效 NUL 字符串
bool airi_dashboard_draw(const AiriBootInfo *info);  // 预留图形接口，当前入口未调用，因此本章无需提供定义
#endif  // 结束头文件包含保护，后续使用同一公开声明
```

## 用 CPU 指令访问设备寄存器

<a id="port-registers"></a>

CPU 寄存器是处理器内保存数值的小存储位置，例如 AL 是 RAX 的低八位、DX 是 RDX 的低十六位。设备寄存器则属于 UART：写入一个寄存器设置发送数据，读另一个寄存器取得“能否再写”的状态。二者都叫寄存器，所在位置和用途不同。

x86 还为设备提供一套独立的 I/O 端口编号。编号 0x3f8 在当前固定 PC 配置中对应 COM1 的数据寄存器；`outb` 把一个字节写到这个端口，`inb` 从给定端口读一个字节。它不是内存地址，不能写成 `*(char *)0x3f8`；也不是 TCP 的网络端口。这里由正在运行的内核执行有权限的设备访问。

先把两条指令封装为 out8/in8，C 调用方分别传入端口编号与字节。

这里的 `static inline` 函数定义放在 `serial.c` 中。每个 C 文件连同 `#include` 展开的内容形成一个编译单元；`static` 使 out8/in8 只能由这个单元中的代码按名字引用。`inline` 允许编译器在调用位置展开函数体，是否真正展开仍由编译器决定。如果以后把这样的定义放进头文件，每个包含它的单元也各有自己的内部定义。

`__asm__` 把汇编嵌在 C 函数里，volatile 要求保留这次有外部效果的操作。冒号依次分隔输出约束、输入约束和破坏项；约束让编译器知道参数必须在哪些 CPU 寄存器中，不能只靠汇编文字猜。

out8 的 `"a"(value)` 把数值放进 AL，`"Nd"(port)` 允许能表示的立即端口编号或 DX。传入 0x3f8 时使用 DX，`%0/%1` 指约束中的第零/第一操作数。in8 的 `"=a"(value)` 表示汇编写出 AL 的结果，编译器再把它作为 C 返回值交给调用者。普通函数调用负责传参数，这两条设备指令负责真正访问 UART。

**新建 `kernel/arch/x86_64/serial.c`：**

<a id="code-kernel-arch-x86-64-serial-c-1"></a>

<!-- write: kernel/arch/x86_64/serial.c -->
```c
#include <airi/kernel.h>  // 取得串口、日志、停机及基础函数的共同声明

static inline void out8(uint16_t port, uint8_t value) {  // 本文件的八位设备写接口，port 为端口号而非内存地址
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));  // 无输出约束；value 放 AL，port 用八位立即数或 DX，写设备一个字节
}
static inline uint8_t in8(uint16_t port) {  // 按给定 I/O 端口读取一个设备状态或数据字节
    uint8_t value;  // 准备接收 AL 输出的 C 局部字节值
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));  // 从端口读到 AL，=a 告诉编译器 value 是本条汇编写出的结果
    return value;  // 将本次从设备读到的字节交回 C 调用方
}
```

端口读写现在可从 C 调用，设备还需要明确收发格式。COM1 的几个寄存器按基址后的小偏移编号；同一个地址在不同配置下还可以承担不同用途。DLAB 是“访问除数锁存器”的选择位，置一时数据端口改作速率配置。配置前先关闭 UART 自己产生的中断，当前发送用轮询主动检查状态。

| 端口 | 本次访问的用途 |
| --- | --- |
| 0x3f8 | DLAB=0 时写待发送字节；DLAB=1 时写波特率除数低字节 |
| 0x3f9 | DLAB=0 时控制中断；DLAB=1 时写除数高字节 |
| 0x3fb | 设置字长、校验、停止位以及 DLAB 选择位 |
| 0x3fa | 配置先入先出的收发队列 FIFO |
| 0x3fc | 写控制线路设置 |
| 0x3fd | 读线路状态，其中 0x20 表示发送保持部分可接受下一字节 |

波特率约定线路的传输速率；本配置按传统 UART 的时钟使用除数 1，得到 115200 波特。先向 0x3fb 写 0x80，置 DLAB 选择位，再向 0x3f8/0x3f9 写低字节 1、高字节 0。随后写 3，把 DLAB 清零并采用八数据位、无校验、一停止位。之后再写 0x3f8 才是在发送字符，而非改变速率。FIFO 保持收发字节的先后顺序，0xc7 启用并清空收发队列、设置接收触发档位；0x0b 设置 DTR、RTS 和 OUT2 控制位，本版没有依赖调制解调器状态来流控。

<a id="polling"></a>

发送一个 A 时，putchar_serial 反复读取 0x3fd。按位与 `& 0x20` 只保留第五位；非零才用 out8 写 65 到数据端口。反复由 CPU 读状态的做法叫轮询，与以后设备主动发中断让 CPU 转去处理不同。状态位说明可再提交字节，还不等于主机终端已经画出 A。

循环最多尝试一百万次，这限制尝试次数而非固定毫秒数。`pause` 是忙等提示，不会把任务交给调度器。尝试耗尽时函数返回并丢弃当前字符，当前接口没有向上报告发送错误。寄存器的配置和状态含义可对照 [UART 数据手册](https://www.ti.com/lit/ds/symlink/tl16c550c.pdf)。

**追加到 `kernel/arch/x86_64/serial.c` 末尾：**

<a id="code-kernel-arch-x86-64-serial-c-2"></a>

<!-- append: kernel/arch/x86_64/serial.c -->
```c
void airi_serial_init(void) {  // 按固定 COM1 基址依次配置中断、速率和收发格式
    out8(0x3f9, 0);  /* Disable UART interrupts; P1 uses polling. */  // DLAB 还为零时关闭 UART 自身中断，P1 通过轮询发送
    out8(0x3fb, 0x80);  // 置 DLAB，使下两次基址读写选择波特率除数寄存器
    out8(0x3f8, 1);  /* Divisor 1: 115200 baud. */  // 除数低字节写 1，配合高字节 0 得到 115200 波特
    out8(0x3f9, 0);  // DLAB 为一时这是除数高字节，和低字节 1 组成完整除数
    out8(0x3fb, 3);  /* 8 data bits, no parity, 1 stop bit. */  // 清 DLAB 并选择八数据位、无校验、一停止位，恢复发送数据用途
    out8(0x3fa, 0xc7);  // 启用并清空 FIFO，接收触发档位设置为 14 字节
    out8(0x3fc, 0x0b);  // 设置 DTR、RTS、OUT2 控制位，当前发送不按调制解调器状态流控
}
static void putchar_serial(char c) {  // 尝试发送一个字节，轮询用次数上限避免设备缺失时永久等待
    /* UART 不就绪时仍有尝试次数上限，耗尽后丢弃该字符。 */
    for (unsigned n = 0; n < 1000000; ++n) {  // 最多读取状态一百万次，这是尝试次数而非固定持续时间
        if (in8(0x3fd) & 0x20) {  // 只保留线路状态第五位，为一表示发送保持部分可接受下一字节
            out8(0x3f8, (uint8_t)c);  // 把字符转成八位数值提交给 UART 数据寄存器
            return;  // 此字节已提交 UART，返回字符串循环处理下一字节
        }
        __asm__ volatile("pause");  // 未就绪时给 CPU 忙等提示，未进行任务切换
    }
}
```

单字符发送已经能把一个数值交给设备，字符串发送再逐字节调用它。当前字符串以 NUL 结束，NUL 只让循环停止，不会被发送。换行 `\n` 表示移到下一行，回车 `\r` 表示回到行首；函数遇换行先发送回车，接收终端就能从下一行左侧继续显示。

日志函数 airi_log 接收格式与实参，在自己的 512 字节局部数组 line 中调用第 04 章的格式化，再同步发送 line 与换行。同步在这里表示函数逐字尝试发送结束后才返回；line 的内容不会留给一个稍后运行的异步发送者。函数返回时局部数组失效，但本次发送已不再读取它。

错误报告 airi_panic 接收一个有效错误字符串，先用相同日志通路输出 `[AIRI_PANIC]` 和原因，然后停机。硬件中断是设备或控制器向 CPU 发出的事件，CPU 按事先安装的处理入口响应；当前内核还没有这些入口。cli 关闭可屏蔽中断，hlt 暂停当前 CPU 指令执行，循环使意外恢复后再次停机。它结束的是内核继续执行的路径；QEMU 作为主机进程仍要由观察脚本终止。

**追加到 `kernel/arch/x86_64/serial.c` 末尾：**

<a id="code-kernel-arch-x86-64-serial-c-3"></a>

<!-- append: kernel/arch/x86_64/serial.c -->
```c
void airi_serial_write(const char *s) {  // 同步遍历调用者字符串，所有读取在返回之前完成
    while (*s) {  // 当前字节不为 NUL 时继续发送，字符串结束标记本身不写设备
        if (*s == '\n')  // 当前字符是换行时，先补一个回车让终端回到行首
            putchar_serial('\r');  // 先发送回车，下一次发送原换行
        putchar_serial(*s++);  // 发送当前字符后前移指针；终止零只结束循环，不发送
    }
}
void airi_log(const char *fmt, ...) {  // 格式化和发送在同一个调用内完成，不借出局部数组
    char line[512];  // 栈上固定容量包含终止零，最多保存 511 个日志字符
    va_list args;    // 保存本次可变参数遍历状态，不是实际参数数组
    va_start(args, fmt);  // 用最后一个具名参数 fmt 作为起点初始化遍历
    airi_vsnprintf(line, sizeof(line), fmt, args);  // 用完整数组容量形成本次日志，过长时按已有规则截断
    va_end(args);  // 结束本次遍历，和 va_start 成对使用
    airi_serial_write(line);  // 同步发送已形成的字符，返回后不再持有 line
    airi_serial_write("\n");  // 在本次格式化日志末尾发送换行，使观察器取得一条完整记录
}
_Noreturn void airi_halt(void) {  // 函数声明不返回，实际通过反复停止 CPU 实现
    for (;;)  // 省略三个条件的循环永不自然结束，恢复执行后再次停机
        __asm__ volatile("cli; hlt");  // 先关闭可屏蔽中断，再暂停当前 CPU；这不会结束主机 QEMU
}
_Noreturn void airi_panic(const char *message) {  // 收到有效错误文字后先写日志，再阻止内核继续
    airi_log("[AIRI_PANIC] %s", message);  // 将当前错误拼进统一失败标记，供主机定位
    airi_halt();  // 当前入口到此停止，不再向调用者返回
}
```

## 汇编入口先给 C 函数一块栈

<a id="entry-stack"></a>

第 02 章准备的 Limine 将内核文件装入内存，启用 64 位模式并建立映射，然后跳到 ELF 记录的入口。ELF 是保存机器码、数据、符号与装载信息的文件格式；稍后的链接脚本把它的 e_entry 字段设为 `_start` 对应地址。这里的入口先由汇编实现，C 的 main 还没有被自动调用。

C 函数会保存局部变量、暂存寄存器并记录返回位置，这些操作需要栈。栈是一块按压入、取出顺序使用的内存；x86-64 用 RSP 寄存器保存当前栈顶。当前入口在 BSS 这种零初始化空间中保留 65536 字节，airi_stack_bottom 是低地址，airi_stack_top 是这段范围的右端。栈向低地址增长，所以第一次使用前把 RSP 放到 top，之后压入才进入这段内存。

AT&T 汇编写源在前、目标在后，寄存器前有 `%`，立即数前有 `$`。`lea airi_stack_top(%rip), %rsp` 计算标签地址并写 RSP。RIP 是指令指针，这种相对寻址用本条指令之后的地址加上位移得到目标；汇编器和链接器根据标签计算位移，lea 只取得地址，不读取栈顶内容。[Intel 指令手册的 RIP 相对寻址说明](https://www.intel.com/content/dam/www/public/us/en/documents/manuals/64-ia-32-architectures-software-developer-vol-2a-manual.pdf)给出了这项规则。

`and $-16, %rsp` 清低四位，让地址能被 16 整除。

<a id="call-abi"></a>

调用约定规定参数、返回值、寄存器保存与栈对齐。我们使用 x86-64 System V ABI 的 C 调用规则。call 先把八字节返回地址压到栈，再跳进函数，所以调用前 RSP 对齐 16 字节，进入 C 后 `(rsp+8)%16==0`。例如调用前 RSP=0x10000，进入后变为 0xfff8；函数按这个前提安排自己的栈空间。`xor %ebp,%ebp` 清 RBP 的低三十二位并使高位归零，为栈帧链留空起点；cld 清方向标志，使可能使用的串操作按 ABI 前提向前处理。

`.section` 选择代码或数据归属，`.global` 让链接器看见名字，`.type/.size` 标注符号种类与长度。`.size _start, .-_start` 中的点是当前汇编位置，减去 `_start` 地址得到这一段机器码的字节数，供 ELF 符号记录使用。`.bss` 记录所需零初始化空间，装载器清零，而文件不用保存 64 KiB 零字节；`.balign 16` 对齐，`.skip 65536` 留出大小。若 C 意外返回，`1b` 跳回最近的数字标签 1，hlt 循环保持停机。

**新建 `kernel/arch/x86_64/entry.S`：**

<a id="code-kernel-arch-x86-64-entry-s-1"></a>

<!-- write: kernel/arch/x86_64/entry.S -->
```asm
.section .text  /* 选择代码节，下面的入口指令会被链接进可执行段 */
.global _start  /* 让链接器可引用入口符号，ENTRY 将选择这个地址 */
.type _start, @function  /* 将 _start 的符号种类标为函数，便于检查和调试 */
_start:  /* 此标签地址成为 ELF 入口，Limine 从这里交出执行权 */
    cli  /* 关闭可屏蔽中断，当前还未设置内核异常/中断入口 */
    cld  /* 清方向标志，满足 C 调用约定要求的向前串操作状态 */
    /* 切换到本内核预留的栈，C 局部变量不再依赖引导器栈。 */
    lea airi_stack_top(%rip), %rsp  /* 按 RIP 相对位置计算栈右端地址，写 RSP 而不读该地址内容 */
    and $-16, %rsp  /* 与 -16 的二进制掩码相与，清低四位以满足调用前 16 字节对齐 */
    xor %ebp, %ebp  /* 将 EBP 清零并使 RBP 高半归零，栈帧链没有更早的内核帧 */
    call airi_limine_start  /* 压入八字节返回地址后进入 C，新的 RSP 满足 ABI 入口对齐 */
1:  hlt  /* 数字标签 1 标记意外返回后的停机位置，暂停当前 CPU */
    jmp 1b  /* b 表示向后找最近标签 1，恢复执行后再次停机 */
.size _start, .-_start  /* 当前位置 . 减入口标签地址，给出入口机器码的字节长度 */

.section .bss  /* 选择零初始化节，装载时有内存但 ELF 不保存每个零字节 */
.balign 16  /* 将下一对象地址向上补齐到 16 字节边界 */
airi_stack_bottom:  /* 标记自有栈的低地址，压栈不可越过此边界 */
    .skip 65536  /* 为栈保留 64 KiB 零初始化空间，当前没有溢出保护页 */
airi_stack_top:  /* 标记保留空间的右端，栈第一次压入后才进入已分配范围 */
.section .note.GNU-stack,"",@progbits  /* 声明此对象不要求可执行栈，该元数据随后由链接策略处理 */
```

汇编入口现在有了栈与一个待连接的 C 函数引用。链接还必须解决编译器自己产生的基础函数引用：结构赋值在语言上是一次复制，但生成的机器码可能调用名字为 memcpy 的辅助函数。我们已有 airi_memcpy，名字却不同，链接器不会自行把两者视为同一符号。

runtime.c 为这些标准名字提供定义，并转交给已有 airi 字节函数。链接器在项目内解决引用，内核仍使用自己的实现。

**新建 `kernel/lib/runtime.c`：**

<a id="code-kernel-lib-runtime-c-1"></a>

<!-- write: kernel/lib/runtime.c -->
```c
/* 独立 C 编译仍可能生成这些标准符号；内核在此提供定义。
 * 主机实验使用 airi_* 名字，避免替换主机自己的库函数。 */
#include <airi/base.h>  // 按公开声明编译自己的字节函数，保证定义与调用使用同一签名
void *memcpy(void *d, const void *s, size_t n) {  // 提供编译器可能引用的标准符号名，实际使用自有复制算法
    return airi_memcpy(d, s, n);  // 把同一目标、源和长度转交给自有非重叠复制
}
void *memmove(void *d, const void *s, size_t n) {  // 标准名的重叠复制入口，与 airi_memmove 使用同一语义
    return airi_memmove(d, s, n);  // 不另建缓存，将方向选择交给已有实现
}
void *memset(void *d, int c, size_t n) {  // 标准名的字节填充入口，使编译器生成的清零引用可链接
    return airi_memset(d, c, n);  // 将同一目标、字节值与数量转交给自有填充
}
int memcmp(const void *a, const void *b, size_t n) {  // 标准名的比较入口，保留已有返回值正负规则
    return airi_memcmp(a, b, n);  // 读取比较仍由自有实现完成，不加入宿主 libc
}
```

汇编调用的 `airi_limine_start` 先初始化串口，再检查 Limine 基础协议版本；版本不支持时，错误也能通过已经可用的设备报告。通过后才输出成功标记并停机。

协议规定的三个数组是引导器扫描的字节标识。`used` 要求编译器保留这些对象，`section` 把它们放入指定节。引导器在交接前写回支持状态，`volatile` 要求 C 代码读取对象时从实际存储取值，避免把状态当成编译时常量。链接器保留这些节则由后面的 KEEP 规则负责。

协议头由第 02 章取得。串口初始化后先输出换行，把固件可能留下的 ANSI 光标控制序列与内核记录分开。内存图响应将在入口可运行后再加入。

**新建 `kernel/boot/limine.c`：**

<a id="code-kernel-boot-limine-c-1"></a>

<!-- write: kernel/boot/limine.c -->
```c
#include <airi/kernel.h>  // 取得串口、日志、停机及基础函数的共同声明
#include <limine.h>  // 读取锁定协议的类型与标识常量，头文件本身不执行引导
__attribute__((used, section(".limine_requests_start")))  // 要求保留下一对象并放进扫描起始标记节
static volatile uint64_t start[] = LIMINE_REQUESTS_START_MARKER;  // 协议规定的起始字节数组，构建时不可丢弃
__attribute__((used, section(".limine_requests")))  // 将下一对象放入引导器扫描的请求节
static volatile uint64_t revision[] = LIMINE_BASE_REVISION(6);  // 请求基础协议修订 6，Limine 会在交接前更新支持状态
__attribute__((used, section(".limine_requests_end")))  // 将下一对象放到扫描范围末尾标记节
static volatile uint64_t end[] = LIMINE_REQUESTS_END_MARKER;  // 结束标记界定本次请求记录范围
_Noreturn void airi_limine_start(void) {  // 汇编 call 进入的 C 入口，当前栈已满足调用约定
    airi_serial_init();  // 先配置串口，后续协议错误也能通过同一设备报告
    airi_serial_write("\n");  /* End any firmware terminal-control line. */  // 先结束固件可能残留的控制序列所在行，再输出内核标记
    if (!LIMINE_BASE_REVISION_SUPPORTED(revision))  // 读取引导器写回的支持状态，修订不符则停止
        airi_panic("Limine revision 6 is unsupported");  // 具体报告所需修订，错误路径不继续发送成功标记
    airi_log("[AIRI_LESSON_READY] _start -> C -> UART");  // 当前栈与设备路径都已走通，发出观察器期待的完整日志
    airi_halt();  // 当前入口到此停止，不再向调用者返回
}
```

## 将对象文件链接成内核

现在源码已经能描述整条路径，文件之间还有尚未确定的地址。汇编 call 使用 airi_limine_start 这个符号名，limine.c 提供它的定义；serial.c 中的日志又引用基础库。符号是目标文件里用于记录这些定义和引用的名字，链接器把引用对应到实际定义地址。

<a id="elf-layout"></a>

编译器把每个源文件变成 `.o` 目标文件，其中最终地址尚未确定；链接器再安排地址、连接引用。ENTRY 选择入口符号，OUTPUT_FORMAT 选择 ELF 格式。

节与段服务不同阶段：.text 保存代码，.rodata 保存只读常量，.data 保存有初值的可写数据，.bss 描述零初始化空间，它们是链接器整理内容的节。PT_LOAD 段给装载器一份“从文件取多少、在内存留多少、放到哪里”的要求；本脚本将几类节归入四个装载段。装载器按段装入，CPU 随后才按入口地址开始执行。

段 FLAGS 的 4、2、1 分别为读、写、执行，text 的 5 表示读加执行，data 的 6 表示读加写。requests 段声明为只读，但引导器在交接前仍按协议填入响应。位置计数器 `.` 从虚拟地址 0xffffffff80000000 开始；各节按 4096 字节对齐。KEEP 保留请求标记，`*(.text .text.*)` 收集输入对象中的代码节。链接器定义 `__kernel_start/end`，之后 C 取它们的地址以计算跨度。

**新建 `kernel/linker.ld`：**

<a id="code-kernel-linker-ld-1"></a>

<!-- write: kernel/linker.ld -->
```ld
/* 请求节布局采用 Limine 官方 0BSD C 模板，许可来源见 THIRD_PARTY。 */
OUTPUT_FORMAT(elf64-x86-64)  /* 输出采用 x86-64 的 64 位 ELF 格式 */
ENTRY(_start)  /* 把符号 _start 的最终虚拟地址写入 ELF 入口字段 */
PHDRS {  /* 声明装载段表，下面的各节会显式归入这些段 */
    requests PT_LOAD FLAGS(4);  /* 引导请求段标为可读，固定协议仍允许引导器在交接前填写响应 */
    text PT_LOAD FLAGS(5);  /* 代码装载段可读可执行，4|1 得到权限数值 5 */
    rodata PT_LOAD FLAGS(4);  /* 只读常量装载段仅允许读，权限数值 4 */
    data PT_LOAD FLAGS(6);  /* 可写数据装载段允许读写，4|2 得到 6 */
}
SECTIONS {  /* 给输入节安排虚拟地址及其归属装载段 */
    . = 0xffffffff80000000;  /* 位置计数器 . 从固定的内核高地址起点开始 */
    __kernel_start = .;  /* 导出此处地址标签，适配层用它计算虚拟地址跨度 */
    .limine_requests : {  /* 建立一个输出请求节，内部按起点、请求、终点顺序收集 */
        KEEP(*(.limine_requests_start))  /* 保留所有输入对象中的请求起始标记节 */
        KEEP(*(.limine_requests))  /* 保留所有输入对象中的实际协议请求节 */
        KEEP(*(.limine_requests_end))  /* 保留所有请求结束标记，防止构建时删除 */
    } :requests  /* 将刚构成的输出请求节放入 requests 装载段 */
    . = ALIGN(4096);  /* 将下一位置向上补齐至 4096 字节边界，分开不同权限页 */
    .text : { *(.text .text.*) } :text  /* 收集 .text 及其子节，全部放入可执行 text 段 */
    . = ALIGN(4096);  /* 将下一位置向上补齐至 4096 字节边界，分开不同权限页 */
    .rodata : { *(.rodata .rodata.*) } :rodata  /* 收集只读数据及子节，放入 rodata 段 */
    . = ALIGN(4096);  /* 将下一位置向上补齐至 4096 字节边界，分开不同权限页 */
    .data : { *(.data .data.*) } :data  /* 收集有初值的可写数据，放入 data 段 */
    .bss : { *(.bss .bss.*) *(COMMON) } :data  /* 收集零初始化与 COMMON 符号，在 data 段保留内存空间 */
    . = ALIGN(4096);  /* 将下一位置向上补齐至 4096 字节边界，分开不同权限页 */
    __kernel_end = .;  /* 导出包含 BSS 及尾部页对齐的右端地址标签 */
    /DISCARD/ : { *(.eh_frame*) *(.note*) }  /* 丢弃此内核不消费的展开/说明元数据，不参与入口执行 */
}
```

<a id="make-rules"></a>

构建还需保证每份源文件用同一目标编译。Make 从依赖规则确定先后：源文件生成对象，对象共同生成 ELF。`:=` 立即展开赋值，`?=` 在变量尚未设置时给默认值；命令行可指定实际工具。

Makefile 是构建规则文件，目标是需要取得的文件或具名动作，依赖是它产生之前需要存在或更新的输入。比如 airios.elf 依赖全部 .o；某个 .o 依赖对应 .c。Make 比较依赖和目标的修改时间，决定这次哪些命令需要执行。

以 kernel/lib/base.c 为例，路径变换生成 `build/normal/kernel/lib/base.o`，模式规则中的 `%` 对应 `kernel/lib/base`。于是 `$@` 是这份 .o 路径，`$<` 是源文件路径，父目录先由 mkdir 建立，编译器再写目标文件。profile=normal 是一组构建选择的名字，之后故障版本用不同目录，避免错误复用对象。

`patsubst` 完成这项路径变换，模式规则 `$(OUT)/%.o: %.c` 为每项提供命令；`$(dir $@)` 给出父目录，规则下方命令必须用 Tab 开头。

内核编译选项各解决一项运行前提：

- `--target` 选择 x86-64 ELF；`-ffreestanding` 声明独立运行环境，`-fno-builtin` 保留自己的基础函数语义。
- `-fno-stack-protector` 避免引用尚未实现的栈保护运行时；`-mno-red-zone` 禁止使用 RSP 下方的隐含 128 字节；`-mgeneral-regs-only` 避免使用尚未管理的 SIMD 状态。
- `-mcmodel=kernel` 配合高地址布局，链接参数 `-nostdlib -static` 排除自动加入 CRT 与 libc。

freestanding 指 C 程序在独立环境中执行，不假定操作系统已提供标准运行时。CRT 是普通 C 程序进入 main 前后的启动与退出代码，当前由自己的 `_start` 替代。red zone 是 System V 用户程序可使用的栈顶下方 128 字节隐含空间；内核不用这个约定，显式关闭。SIMD 是并行处理一组数值的指令与寄存器状态，当前尚未初始化和管理它们，因此限制生成普通寄存器指令。

`-MMD -MP` 的 .d 文件记录此次源文件读了哪些头文件，以后改 base.h 就能触发相关 .o 重编译。`.PHONY` 将 all/kernel/image 等声明为动作名，防止工程里同名文件让 Make 误以为动作已经完成；它本身不提供命令，host-test 和 clean 的实现后面才加入。末尾 `-include` 在文件存在时读入，首次构建缺少 .d 仍可继续；runtime.c 提供当前确实需要的基础函数名字。

本次仅执行 kernel 目标。image 规则还需要下一章的打包脚本。

Makefile 还为后续观察保留三种 profile。`missing-memory` 会让第 08 章的适配层模拟缺地图，`no-framebuffer` 会略过可选显示描述，`edit-experiment` 则替换一行日志文字。当前只构建 normal，这些条件都不成立；其中的面板标题宏要到图形支线才有使用者。

**新建 `Makefile`：**

<a id="code-makefile-1"></a>

<!-- write: Makefile -->
```makefile
# C 编译器默认名称；命令行 CC=路径可以覆盖此赋值
CC := clang
# ELF 链接器默认名称，工具脚本会传入实际路径
LD := ld.lld
# 主机观察程序的编译器名称，与内核目标工具分开
HOST_CC := cc
# 变量尚未设置时采用 normal，命令行可选择独立故障目录
PROFILE ?= normal
# 立即将本次 profile 展开为产物目录，防止不同版本共用对象
OUT := build/$(PROFILE)
# 列出当前入口路径依赖的四份 C 输入，不提前编入后续模块
SOURCES := kernel/boot/limine.c kernel/lib/base.c kernel/lib/runtime.c kernel/arch/x86_64/serial.c
# 将每个 .c 路径换成构建目录中的 .o，再加汇编入口对象
OBJECTS := $(patsubst %.c,$(OUT)/%.o,$(SOURCES)) $(OUT)/kernel/arch/x86_64/entry.o
# 编译 x86-64 ELF，选择 C11、优化及调试信息，并把常见警告作为错误。
# 续行中的 freestanding/builtin/stack-protector 选项选择自有运行环境；pic/pie 关闭地址无关产物。
# 最后一行禁用 red zone/SIMD，采用高地址代码模型，并生成 .d 头文件依赖。
CFLAGS := --target=x86_64-unknown-none-elf -std=c11 -O2 -g -Wall -Wextra -Werror \
          -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-pie \
          -mno-red-zone -mgeneral-regs-only -mcmodel=kernel -MMD -MP
# 为预处理器指定自有头和锁定协议头的搜索路径
CPPFLAGS := -Iinclude -Ithird_party
ifeq ($(PROFILE),no-framebuffer) # 当 profile 名称是 no-framebuffer 时加入缺画布故障宏
# 在预处理阶段定义故障宏，让适配层略过可选图形响应
CPPFLAGS += -DAIRI_TEST_NO_FRAMEBUFFER
endif # 结束当前 Make 条件范围，下面规则恢复对全部 profile 生效
ifeq ($(PROFILE),missing-memory) # 缺地图 profile 采用下一项预处理定义
# 使入口局部地图指针变空，验证必需信息错误路径
CPPFLAGS += -DAIRI_TEST_NO_MEMMAP
endif # 结束当前 Make 条件范围，下面规则恢复对全部 profile 生效
ifeq ($(PROFILE),edit-experiment) # 改动展示 profile 采用下面两条文字覆盖定义
# 向编译器传入两个字符串宏；单引号保护里面的双引号，使它们成为 C 字面量。
CPPFLAGS += -DAIRI_PANEL_TITLE='"LOCAL / MAINTENANCE CONSOLE"' \
            -DAIRI_DEMO_MESSAGE='"I changed the output and rebooted"'
endif # 结束当前 Make 条件范围，下面规则恢复对全部 profile 生效

.PHONY: all kernel image host-test clean # 这些名字表示动作而非同名文件，避免同名文件使动作被误判已完成
all: image # 默认目标先取得 image，也就需要完成其 ELF 前置依赖
kernel: $(OUT)/airios.elf # 请求 kernel 动作时只生成本 profile 的 ELF
$(OUT)/%.o: %.c # 模式规则将任意匹配的 C 源文件编译为同路径对象文件
	@mkdir -p $(dir $@) # 自动变量 $@ 是对象路径，dir 取父目录；@ 隐藏命令回显
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@ # $< 为第一个依赖源码，-c 只编译，-o 指定对象目标
$(OUT)/%.o: %.S # 预处理汇编使用同一对象目录和编译驱动
	@mkdir -p $(dir $@) # 自动变量 $@ 是对象路径，dir 取父目录；@ 隐藏命令回显
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@ # $< 为第一个依赖源码，-c 只编译，-o 指定对象目标
$(OUT)/airios.elf: $(OBJECTS) kernel/linker.ld # 全部对象和链接脚本变化都会使 ELF 需要重新生成
	# 按 x86-64 ELF 静态链接，不加入宿主启动库；最大页大小采用 4096 字节。
	# 续行再禁用可执行栈，使用当前链接脚本，把全部对象写成此目标 ELF。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 \
	    -z noexecstack -T kernel/linker.ld $(OBJECTS) -o $@
image: $(OUT)/airios.elf # 先有当前 ELF，才执行下一行镜像打包
	python3 scripts/image.py $(PROFILE) # 把 profile 作为独立脚本参数交给打包流程
-include $(OBJECTS:.o=.d) # 替换对象后缀读取头依赖文件；开头 - 允许首次尚无 .d
```

通过 tools 模块取得构建参数，将已有 C、汇编和链接脚本生成内核。随后用 readelf 查看格式、目标机器和入口，用 `nm -u` 检查还有没有未解决的函数引用。

**在学习目录根目录运行：**

<!-- run -->
```sh
PYTHONPATH=scripts python3 - <<'PY'  # 标准输入中到下一独立 PY 行之前的内容交给 Python，不由 shell 展开
import subprocess  # 启动主机工具，取得退出状态或保存进程对象
from tools import make_command  # 取得指向当前工具和目标的 Make 参数列表
subprocess.run(make_command(target='kernel'), check=True)  # 选定工具后执行 kernel 目标；构建非零退出即失败
PY
PYTHONPATH=scripts python3 - <<'PY'  # 标准输入中到下一独立 PY 行之前的内容交给 Python，不由 shell 展开
import subprocess  # 启动主机工具，取得退出状态或保存进程对象
from tools import tool  # 按显式覆盖、Homebrew、PATH 的顺序选择工具
subprocess.run([tool('llvm-readelf','llvm'), '-h', '-l', 'build/normal/airios.elf'], check=True)  # 打印 ELF 文件头和装载段，核对入口、机器架构及各段布局
assert not subprocess.check_output([tool('llvm-nm','llvm'), '-u', 'build/normal/airios.elf']).strip()  # 捕获未解决符号列表；去空白后仍有字节就断言失败
PY
```

readelf 应显示 ELF64、x86-64、EXEC 和高地址入口；`nm -u` 没有输出，说明所有链接引用已解决。这份 ELF 保存内核载荷，由 Limine 装载，当前 macOS 终端不能将它作为本机程序运行。
