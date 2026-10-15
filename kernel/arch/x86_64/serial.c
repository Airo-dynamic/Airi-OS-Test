#include <airi/arch.h> // 用户终端只通过架构适配读取UART。
#include <airi/kernel.h>  // 取得串口、日志、停机及基础函数的共同声明

static inline void out8(uint16_t port, uint8_t value) {  // 本文件的八位设备写接口，port 为端口号而非内存地址
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));  // 无输出约束；value 放 AL，port 用八位立即数或 DX，写设备一个字节
}
static inline uint8_t in8(uint16_t port) {  // 按给定 I/O 端口读取一个设备状态或数据字节
    uint8_t value;  // 准备接收 AL 输出的 C 局部字节值
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));  // 从端口读到 AL，=a 告诉编译器 value 是本条汇编写出的结果
    return value;  // 将本次从设备读到的字节交回 C 调用方
}
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

void airi_serial_write_bytes(const void *bytes, size_t count) { // 当前串口保持文本换行转换，但逻辑长度可以包含NUL。
    const uint8_t *data = bytes; // 按字节读取已验证的内核副本。
    for (size_t i = 0; i < count; ++i) { // 严格使用调用者长度，不扫描数组外终止符。
        if (data[i] == '\n') // 与原文字输出保持CRLF行为。
            putchar_serial('\r'); // 原始换行之前增加回车。
        putchar_serial((char)data[i]); // 包括零字节也真正交给UART。
    }
}

int airi_arch_serial_read(void) { // 不等待，不解码键盘，也不把NUL误判成无数据。
    return (in8(0x3fd) & 1) ? in8(0x3f8) : -1; // 只在接收就绪时消费真实设备字节。
}
