# 06 · 让引导器进入自己的 C 代码，并发出第一行文字

前面的观察入口由 Mac 运行时调用 main，再借 printf 显示结果。现在要把同一工程变成独立内核，就需要自己接过这些职责。运行时的顺序是：Limine 装入 ELF 并跳到 `_start`，汇编准备栈并调用 C 入口，C 初始化串口，发送 `[AIRI_LESSON_READY] _start -> C -> UART`，最后停机。下面按这条路径所需的组件逐一实现，到章末再链接完整程序。

## 文字从内核内存走向主机日志

<a id="serial-path"></a>

第 04 章已将整数变成字符。现在选用 QEMU 的 COM1，也就是第一路传统 PC 串口；其中 UART 控制器接受软件写入的字节并送入串行通路。字符串先存在内核内存，发送循环逐字节读取，QEMU 再把设备另一端连接到主机输出。

下一章的 `-serial file:...` 会把这些字节收进主机 serial.log，再由脚本打印到 Mac 终端。内核此时并没有文件系统，写的是设备，创建文件的是主机 QEMU。UART 接受一个字节、文件收到它、终端显示它，是这条路径中的不同动作。

先在共同头文件声明串口、日志、错误报告和停机接口。`_Noreturn` 告诉编译器调用不会正常返回，停机仍由实现中的机器指令完成。可选图形接口 airi_dashboard_draw 当前只有声明且未被调用，没有待链接的引用；它不会让本次入口自动取得图形能力。

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

## 先分清传参数的 CPU 寄存器与保存状态的设备寄存器

<a id="port-registers"></a>

要发送 A，C 代码最终要把数值 65 交给 UART。CPU 的 AL 是 RAX 的低八位，DX 是 RDX 的低十六位，负责承载指令操作数；UART 的数据、配置和状态寄存器属于设备，负责记住设备状态。这两组寄存器需要通过设备访问指令连接。

固定 PC 配置用一套独立的 I/O 端口编号访问 UART，COM1 数据端口为 0x3f8。outb 将 AL 的字节交给指定端口，inb 将端口字节读回 AL。这个编号既不是可解引用的内存地址，也不是 TCP 端口；内核有权限执行这些 x86 设备指令。

out8/in8 把它们包装成 C 调用。`__asm__ volatile` 要求保留具有外部效果的操作，冒号依次分隔输出、输入与破坏约束。out8 的 `"a"(value)` 让编译器安排 AL，`"Nd"(port)` 允许立即端口或 DX；0x3f8 需要 DX。`%0/%1` 引用约束操作数，in8 的 `"=a"(value)` 则声明 AL 是输出，随后作为 C 返回值交回。

这些定义位于 serial.c，static 使名字只在当前编译单元可见；一个 C 文件连同展开的头文件构成该单元。inline 允许在调用处展开，是否展开仍由编译器决定；若同类定义放入头文件，各包含单元也各有内部定义。C 传参和寄存器约束先准备数值，真正访问设备的仍是 outb/inb。

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

## 先配置端口用途，再等待下一字节可以提交

COM1 由一组相邻端口控制。同一端口在不同模式下用途不同，所以还不能立即向 0x3f8 写字符：DLAB 置一时，它改为速率除数的低字节。初始化先关闭 UART 中断，当前由 CPU 主动轮询状态。

| 端口 | 当前访问的用途 |
| --- | --- |
| 0x3f8 | DLAB=0 时写数据；DLAB=1 时写波特率除数低字节 |
| 0x3f9 | DLAB=0 时控制中断；DLAB=1 时写除数高字节 |
| 0x3fb | 设置数据位、校验、停止位和 DLAB |
| 0x3fa | 配置收发 FIFO |
| 0x3fc | 设置控制线路 |
| 0x3fd | 读取状态，0x20 表示发送保持部分可接受下一字节 |

向 0x3fb 写 0x80 选中 DLAB，再向低、高除数字节写 1、0，按传统 UART 时钟得到 115200 波特。随后写 3，采用八数据位、无校验、一停止位，同时清 DLAB；此后写数据端口才是提交字符。0xc7 启用并清空 FIFO，设置接收触发档位；0x0b 设置 DTR、RTS、OUT2，本版不依赖调制解调器状态流控。FIFO 保持字节先后。

<a id="polling"></a>

配置就绪以后，putchar_serial 才能发送 A：反复读 0x3fd，与 0x20 按位与，非零时把 65 写到 0x3f8。这叫轮询，CPU 主动检查，尚未让设备通过中断触发处理。状态就绪只保证可以再交字节，不保证主机终端已经显示。

循环最多一百万次，限制的是尝试数量而非毫秒。pause 是忙等提示，不交出任务；耗尽后当前字符丢弃，接口没有向上报告错误。具体寄存器含义见 [UART 数据手册](https://www.ti.com/lit/ds/symlink/tl16c550c.pdf)。

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

## 在同一次调用里形成并发送完整字符串

单字节接口已经完成，airi_serial_write 逐个读取 NUL 前的字符并调用它。NUL 只结束遍历，不发送；遇到换行 `\n` 先补回车 `\r`，让终端移到下一行并回到行首。

airi_log 则在自己的 512 字节局部数组 line 中调用格式化，再发送 line 与换行。格式化最多保存 511 个字符和 NUL，所有读取均在本次同步调用返回前结束，没有异步发送者继续借用这块栈空间。函数返回后 line 失效，不影响已提交的字节。

错误路径复用同一设备：airi_panic 接受有效错误字符串，输出 `[AIRI_PANIC]` 与原因，再进入 airi_halt。当前没有安装中断入口，cli 关闭可屏蔽中断，hlt 停止当前 CPU 执行，循环保证意外恢复后再次停机。它不会终止主机上的 QEMU 进程，后者还需由观察脚本回收。硬件中断本来可以把 CPU 转到预设处理入口，这条处理路径将在后续建立。

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

## C 能调用发送函数之前，先需要自己的栈

<a id="entry-stack"></a>

发送实现已有了，现在接回真正的运行起点。Limine 将内核 ELF 装入内存，启用 64 位模式、建立早期映射，再跳到 ELF 的 e_entry。ELF 保存机器码、数据、符号与装载要求，链接脚本会把 e_entry 指向 `_start`；此时没有主机运行时自动调用 main。

C 函数需要保存返回位置、寄存器和局部变量，汇编入口先为这些操作准备栈。RSP 保存当前栈顶，x86-64 栈向低地址增长：预留区低端是 airi_stack_bottom，右端是 airi_stack_top，先把 RSP 设到 top，第一次压入才进入范围。64 KiB 放在 BSS，装载器提供零初始化空间，文件不必保存同样多的零字节。

AT&T 汇编源在前、目标在后，寄存器前写 `%`、立即数前写 `$`。lea 用 RIP 相对位置算出 airi_stack_top 地址再写 RSP，只取得地址，不读取顶端内容。RIP 相对寻址以本指令之后的位置加位移求目标，汇编器与链接器根据标签确定差值，见 [Intel 指令说明](https://www.intel.com/content/dam/www/public/us/en/documents/manuals/64-ia-32-architectures-software-developer-vol-2a-manual.pdf)。and 的 −16 掩码清低四位，使调用前 RSP 可被 16 整除。

<a id="call-abi"></a>

这里遵守 x86-64 System V C ABI：它约定参数、返回值、寄存器保存和对齐。call 压入八字节返回地址再跳转，因此调用前 RSP=0x10000，进入 C 后为 0xfff8，满足 `(rsp+8)%16==0`。清 EBP 同时清 RBP 高半，为栈帧链设空起点；cld 清方向标志，满足 C 串操作前提。

汇编伪指令负责把这些字节交给链接器。section 选择节，global 导出名字，type/size 标注符号种类与长度；`.size _start, .-_start` 用当前位置减入口得到机器码跨度。balign 对齐，skip 在 BSS 留空间。若 C 意外返回，`1b` 找到最近的数字标签 1，继续 hlt 循环。

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

栈和 C 调用已连接，还要补编译器可能产生的调用。结构赋值在 C 中是一条语句，编译后却可能引用标准名 memcpy；自己的实现叫 airi_memcpy，链接器不会因用途相同就自动替换名字。

runtime.c 提供 memcpy/memmove/memset/memcmp 这些标准符号，并转交已有算法，仍不加入主机 libc。主机观察继续使用 airi_* 名字，避免替换观察进程自身的库函数。

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

## 让 C 入口消费引导器留下的支持状态

汇编调用 airi_limine_start，入口先初始化串口并输出换行，再检查协议修订。这样修订不符也能报告错误；换行将固件可能留下的 ANSI 光标控制序列与内核记录分开。通过后发送本章标记并停机，真实内存响应稍后接入。

修订数组和起止数组先作为协议字节编进 ELF。Limine 装入它们，在进入 `_start` 之前按协议写回修订支持状态，C 入口随后读取。used 让编译器保留对象，section 选择输入节，volatile 要求读取实际存储，链接器的 KEEP 则保留这些节；这几个机制分别作用于构建和运行阶段。协议头仍使用第 02 章锁定的声明。

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

## 链接确定地址，装载器再按段放入内存

每个源文件先成为 .o，包含定义和仍待解决的符号引用：entry.S 调用 airi_limine_start，后者在 limine.c 定义，又调用 serial.c 和基础库。链接器需要把这些引用指到定义的最终地址，并给 Limine 一个可装载的 ELF。

<a id="elf-layout"></a>

脚本以 OUTPUT_FORMAT 指定 ELF64 x86-64，以 ENTRY 选 `_start`。`.text/.rodata/.data/.bss` 是链接整理内容的节，分别承载代码、只读常量、可写初值和零初始化空间；PT_LOAD 段则给装载器“从文件读多少、内存留多少、放哪里”的要求。本脚本将内容分为 requests、text、rodata、data 四个装载段，装入之后才从入口执行。

FLAGS 的读/写/执行分别是 4/2/1，因此 text=5、data=6。requests 标为只读，固定引导协议仍允许 Limine 在交接前写回状态。位置计数器从 0xffffffff80000000 起，各类节按 4096 对齐；KEEP 依次保留请求起点、记录和终点，通配 `.text.*` 收集各输入子节。导出的 `__kernel_start/end` 标签记录包含 BSS 的跨度，之后 C 只取地址即可计算大小。

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

## Make 把同一组源码变成同一目标环境的对象

现在回到 01 章的工具参数。CC 用于各 C 与汇编源的编译，LD 链接它们，HOST_CC 留给主机观察。Makefile 将源码、对象与 ELF 写成依赖：某个源或头更新就重编相应对象，任一对象或链接脚本更新再重新链接。Make 比较目标和依赖的修改时间，而不是每次盲目执行所有命令。

例如 kernel/lib/base.c 经 patsubst 变为 `build/normal/kernel/lib/base.o`。模式 `$(OUT)/%.o: %.c` 中 `%` 匹配 kernel/lib/base，命令中的 `$<` 是输入源码、`$@` 是对象路径，`$(dir $@)` 给出需先建立的父目录。规则下命令用 Tab 开头。`:=` 立即展开赋值，`?=` 只给未设置变量默认值，命令行工具赋值覆盖默认值。normal 及后续 profile 各自使用产物目录，避免故障版和正常版共用旧对象。

各组内核选项对应实际运行前提：

- `--target=x86_64-unknown-none-elf` 生成供虚拟 CPU 使用的对象，`-ffreestanding -fno-builtin` 选择自有运行环境与基础函数语义；`-fno-pic -fno-pie` 配合固定地址产物。
- `-fno-stack-protector` 避免尚未提供的保护运行时，`-mno-red-zone` 禁用 RSP 下方隐含 128 字节，`-mgeneral-regs-only` 避免尚未管理的 SIMD 状态。
- `-mcmodel=kernel` 配合高地址代码；链接的 `-nostdlib -static` 排除主机 CRT/libc，自己的 `_start` 承担进入 C 的职责。

CRT 是普通应用 main 前后的启动退出代码；red zone 是 System V 用户程序可借用的栈下隐含空间，内核不采用；SIMD 同时处理多项数值并有额外寄存器状态，当前尚未初始化。它们都不能因 C 能编译就视为已有。

`-MMD -MP` 生成记录头文件依赖的 .d，末尾 `-include` 读它们，首次缺失仍继续。`.PHONY` 表示动作名，避免同名文件掩盖动作，但不凭空提供命令；host-test、clean 稍后实现。当前只执行 kernel，image 仍等待下一章脚本。

已有 Make 条件为后续 missing-memory、no-framebuffer、edit-experiment 保留宏，分别模拟缺必需地图、略过可选显示、改变展示文字。normal 不选这些分支，面板标题宏也暂时没有消费者。当前只构建已经闭合的入口。

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

代码与构建规则都已写齐，下面才真正调用 Make。第一段借 tools 选择工具并执行 kernel；第二段让 readelf 读取产物头与装载段，再用 nm 的 `-u` 列出未解决引用。check_output 捕获输出，去掉空白后仍有内容就失败。

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

应看到 ELF64、x86-64、EXEC、高地址入口和对应装载段，nm 未解决列表为空。这证明生成与链接已经闭合，没有证明虚拟 CPU 实际运行过它。

这份文件是 Limine 将装载的内核载荷，不能作为普通 Mac 程序在当前终端执行。下一章把它与引导程序、配置组合成启动介质，第一次沿 `_start → C → UART` 取得运行结果。
