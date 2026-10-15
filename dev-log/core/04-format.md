# 04 · 在容量有限的数组里形成一行日志

字节函数可以搬动现有文字，但内存大小 123 当前还是一个整数。内核要留下 `IO:123` 这样的日志，需要先把数值转成字符，再把字符交给输出设备。日志是一段程序写出的状态记录；这一步只形成记录文字，设备发送稍后接入。

## 日志文字与发送通路

<a id="serial-model"></a>

首次启动时内核还没有图形输出，先使用串口。串口是一种按先后顺序发送与接收数据的设备通路；传统硬件把字节的各位逐位传输，接收端按约定还原字节。这里用 QEMU 模拟这个设备，并把它输出的字节接到主机文件或终端，所以 Mac 不需要真的插串口线。

例如内核给设备发送字节 65，接收端按 ASCII 编码将它显示为 A。ASCII 是字符与整数编号的对应规则，A 为 65、换行为 10；发送整数 123 的原始字节并不会显示字符 1、2、3。必须先得到编号 49、50、51 这三个字节，再依次发送。当前只做这个数值到文字的转换，第 06 章再实现设备访问。

<a id="format-capacity"></a>

以格式 `"IO:%u"` 和数值 123 为输入，完整文字有六个字符。五字节输出数组还要存放一个 NUL，因此只能保留四个字符，结果是 `IO:1\0`。函数既要遵守实际容量，又要告诉调用者完整结果需要六个字符；这两个长度不能混用。

## 给格式化函数传入参数

新增声明中的 `...` 接收可变数量参数，va_list 保存遍历这些参数的状态。格式中的 `%u` 要求读取一个 unsigned，`%s` 要求读取字符指针；调用者给出的类型必须与之匹配。格式化实现不会凭字符串自动知道实参的真实类型。头文件完整替换后仍保留上一章的五个字节函数声明。

**完整替换 `include/airi/base.h`：**

<a id="code-include-airi-base-h-1"></a>

<!-- write: include/airi/base.h -->
```c
#ifndef AIRI_BASE_H  // 第一次包含时进入此范围，后续同一翻译单元跳过重复声明
#define AIRI_BASE_H  // 记录基础头文件已处理，配合末尾 endif 构成包含保护
#include <stddef.h>  // 取得 size_t、ptrdiff_t、NULL 等工具链定义
#include <stdint.h>  // 取得固定宽度整数及 uintptr_t，不会因此链接宿主 libc
#include <stdarg.h>  // 取得 va_list 和可变参数遍历宏，稍后的格式函数会使用

void *airi_memcpy(void *dst, const void *src, size_t n);   // 从 src 读 n 字节写入 dst，双方不得重叠，返回原目标地址
void *airi_memmove(void *dst, const void *src, size_t n);  // 按地址次序选择搬运方向，允许双方范围重叠
void *airi_memset(void *dst, int value, size_t n);  // 将 value 转成一个无符号字节，填入 dst 的 n 个位置
int airi_memcmp(const void *a, const void *b, size_t n);  // 按无符号字节比较 n 项，首个差异决定返回值正负
size_t airi_strlen(const char *s);  // 读到 NUL 才停止，返回前面的字符数，不计 NUL
/* 截断时仍返回完整所需字符数，不含 NUL。
 * 支持 %s/%c/%d/%u/%x/%p/%%、l/ll/z 及整数前导零宽度。 */
int airi_vsnprintf(char *dst, size_t cap, const char *fmt, va_list args);  // 接收已开始遍历的参数状态，按 cap 截断保存并返回完整所需长度
int airi_snprintf(char *dst, size_t cap, const char *fmt, ...);  // 普通调用入口，省略号接收 fmt 所约定的额外实参
#endif  // 结束头文件包含保护，后续使用同一公开声明
```

保留已有五个实现，再把格式化代码接在后面。输出记录 `struct output` 同时保存调用者数组 `dst`、可写容量 `cap` 和已经产生的逻辑长度 `count`。逻辑长度会在数组装不下时继续增加，才能向调用者报告完整文本需要多大空间。

**完整替换 `kernel/lib/base.c`：**

<a id="code-kernel-lib-base-c-1"></a>

<!-- write: kernel/lib/base.c -->
```c
#include <airi/base.h>  // 按公开声明编译自己的字节函数，保证定义与调用使用同一签名
#include <limits.h>     // 取得 INT_MAX，为格式化返回值的饱和处理提供上限

void *airi_memcpy(void *dst, const void *src, size_t n) {  // 本函数只处理互不重叠的两个已准备范围，不分配内存
    unsigned char *d = dst;  // 目标转成字节指针，每个下标对应一个可写字节
    const unsigned char *s = src;   // 源转成只读字节指针，此函数不通过它写入
    for (size_t i = 0; i < n; ++i)  // 从低下标搬运前 n 个字节，n 为零时不访问源和目标
        d[i] = s[i];  // 先读取当前源字节，再写同一偏移的目标位置
    return dst;  // 交回调用者最初提供的目标起点，便于继续处理该范围
}
void *airi_memmove(void *dst, const void *src, size_t n) {  // 在调用者提供的范围中搬字节，重叠时保护尚未读取的源内容
    unsigned char *d = dst;  // 目标转成字节指针，每个下标对应一个可写字节
    const unsigned char *s = src;  // 源转成只读字节指针，此函数不通过它写入
    /* 地址转换为整数后比较；本实现依赖 x86-64 和主机实验的平坦地址表示。 */
    if ((uintptr_t)d <= (uintptr_t)s) {  // 在本目标的平坦地址表示下比较地址整数，目标不高于源时向前搬
        for (size_t i = 0; i < n; ++i)  // 从低下标搬运前 n 个字节，n 为零时不访问源和目标
            d[i] = s[i];  // 先读取当前源字节，再写同一偏移的目标位置
    } else {  // 目标高于源时改为逆序，先保存靠近末尾的源字节
        for (size_t i = n; i != 0; --i)  // 先从数量 n 开始，每轮使用 i-1；零长度不会下溢
            d[i - 1] = s[i - 1];  // 依次写最后一个到第一个字节，避免覆盖后续待读源字节
    }
    return dst;  // 交回调用者最初提供的目标起点，便于继续处理该范围
}
void *airi_memset(void *dst, int value, size_t n) {  // 把调用者已有范围填成相同字节，不创建新对象
    unsigned char *d = dst;  // 目标转成字节指针，每个下标对应一个可写字节
    for (size_t i = 0; i < n; ++i)  // 依次填充目标前 n 个字节，n 为零时不访问目标
        d[i] = (unsigned char)value;  // 转换后的低八位写入当前位置，例如 value=0 清零
    return dst;  // 交回调用者最初提供的目标起点，便于继续处理该范围
}
int airi_memcmp(const void *a, const void *b, size_t n) {  // 首个不同字节确定次序；n=0 时两指针都不解引用
    const unsigned char *x = a, *y = b;  // 把两个输入都按无符号字节查看，避免高位字符产生负值
    for (size_t i = 0; i < n; ++i)  // 依次比较前 n 个字节，n 为零时不读取任一输入
        if (x[i] != y[i])  // 只有当前字节不相等才结束比较，前面各项均已相等
            return (int)x[i] - (int)y[i];  // 先提升成 int 再相减，返回负、零、正值表达字节次序
    return 0;  // 前 n 项全部相同，返回相等结果
}
size_t airi_strlen(const char *s) {  // 输入必须在可读范围内存在 NUL，本函数没有额外容量参数
    size_t n = 0;  // 从字符串第零字节开始累计可见字符长度
    while (s[n])   // 当前字节非零时继续，NUL 使条件为假
        ++n;  // 计入一个非零字节，下一轮检查紧随其后的字节
    return n;  // 交回 NUL 前的字节数量，终止零本身未被计入
}
```

先写所有输出共用的 `emit`。它只在 `cap>0` 且 `count<cap-1` 时写字符，为 NUL 留出最后一格；每次调用都增加 count。容量为零时仍可统计长度，完全不访问 dst。

在这次 IO:123 输入中，依次 emit I、O、冒号后，count 为 3；写字符 1 后 count 为 4。继续产生 2、3 时，`count<cap-1` 已经为假，所以不再写数组，但 count 仍走到 6。最后用位置 4 放 NUL，函数返回 6。容量为零时同样能完成统计，调用方却没有提供任何可写字符位置。

整数转换函数 `number` 将数值除以进制，暂存每次余数。123 依次得到 3、2、1，最后倒着取出即可恢复字符顺序。`do/while` 至少执行一次，使数值零也形成字符 `0`。width 控制当前子集的前导零数量。

**追加到 `kernel/lib/base.c` 末尾：**

<a id="code-kernel-lib-base-c-2"></a>

<!-- append: kernel/lib/base.c -->
```c
struct output {  // 把一次格式化的保存位置与统计状态放进同一个记录
    char *dst;  // 调用者已经准备的输出字符数组，本记录只借用它
    size_t cap, count;  // cap 为数组总字节数，count 为产生的字符数，包含被截断部分
};
static void emit(struct output *o, char c) {  // 仅在本文件使用的单字符出口，所有转换共享同一容量规则
    if (o->cap && o->count < o->cap - 1)  // 容量非零才求 cap-1；只写 NUL 预留格之前的位置
        o->dst[o->count] = c;  // 将本次字符放到逻辑位置，该位置已由上面的条件证明可写
    ++o->count;  // 无论字符是否存得下，完整所需长度都增加一项
}
static void number(struct output *o, uint64_t value, unsigned radix, unsigned width) {  // 把无符号量按 10 或 16 进制转换，输出由 o 统一限容
    char digits[64];  // 临时保存倒序数位，64 格足以容纳 uint64_t 的二进制位数
    unsigned n = 0;   // n 记录临时数组中已保存的数位数量
    do {  // 先生成一位再检查，数值零也必须产生字符 0
        digits[n++] = "0123456789abcdef"[value % radix];  // 余数选择数位字符，写旧下标 n 后再增加有效数位数
        value /= radix;  // 丢掉已保存的最低位，下一轮处理更高位
    } while (value);     // 仍有更高位就继续，数值耗尽时结束逆序存放
    while (width > n) {  // 最小零填充宽度大于实际数位数时，先输出补足的零
        emit(o, '0');  // 将一个前导零交给统一限容输出，仍计入完整长度
        --width;  // 本次已补一格，缩小剩余宽度直到不超过实际数位数
    }
    while (n)  // 临时数组还有数位时继续，从末尾取出恢复正常顺序
        emit(o, digits[--n]);  // 先将 n 减一，再读取该数位，按高位到低位输出
}
```

字符和数字都有了输出方法，`airi_vsnprintf` 开始扫描格式文本。普通字符交给 emit；遇 `%` 时先读取零填充宽度和长度修饰，再读取转换字母。`*fmt++` 取出当前字符，然后让指针前进一步。输入 `%08x` 时得到 width=8、spec=x，按十六进制输出整数，0xab 因此形成 `000000ab`。

格式扫描指针与可变参数遍历状态各走自己的序列：读到 I、O、冒号不消耗实参，遇到 `%u` 才从 args 取一次 unsigned。格式中有两个转换项时，需要取两项；格式写错类型时，取值操作便不符合调用约定，不能把出现的乱码当成有定义的转换结果。

`va_arg(args, T)` 按类型 T 取得下一项。可变参数调用会将较小整数类型提升，`%c` 因此取 `int`；`long long` 与 `unsigned long long` 则各按匹配类型读取。`%zu/%zx` 读取 size_t；`%zd` 读取对应的有符号类型。当前 x86-64 ELF 和 macOS/Linux 主机都采用 LP64：long 与指针为 64 位，size_t 是 unsigned long，所以 `%zd` 读取 long。这一选择依赖固定目标，不宣称适合所有 C 实现。

有符号负数先转成 `uint64_t`，再以无符号 `0-value` 取得量级，使最小有符号数也能转换。

未知 `%q` 保留两个原字符，末尾孤立 `%` 也保留；浮点和精度尚不支持。扫描结束后，容量非零就在 `min(count, cap-1)` 写 NUL，返回完整所需长度，超过 INT_MAX 时饱和。对于五字节数组中的 `IO:123`，实际字节为 `I O : 1 \0`，返回值仍是 6。

**追加到 `kernel/lib/base.c` 末尾：**

<a id="code-kernel-lib-base-c-3"></a>

<!-- append: kernel/lib/base.c -->
```c
int airi_vsnprintf(char *dst, size_t cap, const char *fmt, va_list args) {  // 扫描格式与实参，在借用数组中保存截断文本并统计完整长度
    struct output o = {dst, cap, 0};  // 按成员声明顺序初始化目标、容量和已产生长度
    while (*fmt) {  // 格式指针尚未遇 NUL 时处理当前字符
        if (*fmt != '%') {  // 普通文字不取实参，直接作为一个输出字符处理
            emit(&o, *fmt++);  // 取当前字符交给 emit，再将格式指针前移一字节
            continue;  // 当前普通字符已输出，直接处理下一格式字符而不误当转换项
        }
        ++fmt;  // 越过百分号，开始读取宽度、长度修饰或转换字母
        if (!*fmt) {  // 百分号之后已到 NUL，避免继续向字符串外读取
            emit(&o, '%');  // 保留一个字面百分号；完整百分号转换和残缺转换都使用此出口
            break;  // 格式已结束，退出扫描后统一写终止零和返回长度
        }
        unsigned width = 0;  // 本次转换默认不补前导零，宽度只影响当前整数
        if (*fmt == '0') {   // 只有以 0 开始的宽度属于当前支持的零填充格式
            ++fmt;  // 越过零填充标记 0，开始收集后面的宽度数字
            while (*fmt >= '0' && *fmt <= '9') {  // 逐个消费宽度十进制数字，停在非数字的长度或转换字母
                /* 宽度限制到 64，畸形格式也不能产生无限补零工作。 */
                width = width < 64 ? width * 10 + (unsigned)(*fmt - '0') : 64;  // 旧宽度乘十加当前数字；已有 64 时不再继续扩大
                if (width > 64)  // 本次累加也可能超过 64，统一限制最多补到 64 位
                    width = 64;  // 使畸形巨大宽度不能让格式化无限补零
                ++fmt;  // 本次宽度数字已加入统计，继续读取下一字符
            }
        }
        enum { NORMAL, LONG, LLONG, SIZE } length = NORMAL;  // 局部枚举记录实参应按哪一种整数类型读取，初始为普通 int/unsigned
        if (*fmt == 'l') {  // 第一字母 l 将实参类型从普通整数改成 long
            length = LONG;  // 后面的 va_arg 将根据此值读取 long 或 unsigned long
            ++fmt;  // 越过第一个 l，下一步判断是否还有第二个 l
            if (*fmt == 'l') {  // 已越过第一个 l，此处识别第二个连续 l，改用 long long 类型
                length = LLONG;  // 第二个连续 l 改为 long long 对应类型
                ++fmt;  // 越过第二个 l，让格式指针到达转换字母
            }
        } else if (*fmt == 'z') {  // z 修饰大小类型，和上面的 l/ll 分支互斥
            length = SIZE;  // 无符号读取 size_t；固定 LP64 目标的有符号分支读取 long
            ++fmt;  // 越过 z 长度修饰，下一步读取转换字母
        }
        if (!*fmt) {  // 百分号之后已到 NUL，避免继续向字符串外读取
            emit(&o, '%');  // 保留一个字面百分号；完整百分号转换和残缺转换都使用此出口
            break;  // 格式已结束，退出扫描后统一写终止零和返回长度
        }
        char spec = *fmt++;  // 保存转换字母，然后让格式指针指向下一个普通字符
        if (spec == '%')     // 双百分号形成一个字面字符，不消耗额外实参
            emit(&o, '%');  // 保留一个字面百分号；完整百分号转换和残缺转换都使用此出口
        else if (spec == 's') {  // 字符串转换读取一项字符指针，逐字节输出到 NUL
            const char *s = va_arg(args, const char *);  // 按调用者约定的字符指针类型取得下一实参
            if (!s)  // 空字符串指针无法解引用，改用固定提示文字
                s = "(null)";  // 把 NULL 参数显示成六个字符，不尝试访问零地址
            while (*s)  // 只输出终止零前的字符，NUL 不计入格式长度
                emit(&o, *s++);  // 读取当前源字符，输出后前移源指针
        } else if (spec == 'c')  // 单字符实参经默认提升成为 int，随后再转换成 char
            emit(&o, (char)va_arg(args, int));  // 读取提升后的 int，并把字符值交给限容输出
        else if (spec == 'p') {  // 指针显示采用固定 0x 前缀及地址的十六进制数字
            emit(&o, '0');  // 将十六进制地址前缀的第一个字符 0 写入输出
            emit(&o, 'x');  // 完成指针的 0x 前缀，后面跟地址数字
            number(&o, (uintptr_t)va_arg(args, void *), 16, width);  // 读取 void* 实参，按本目标的整数地址表示转换成十六进制
        } else if (spec == 'd' || spec == 'u' || spec == 'x') {  // 其余支持的整数分别为有符号十进制、无符号十进制和十六进制
            uint64_t value;     // 统一保存待转换的无符号量，负号另行输出
            if (spec == 'd') {  // 有符号转换先按长度读取，再分离符号和绝对量
                int64_t signed_value = length == LLONG  ? va_arg(args, long long)  // 长度为 ll 时读取 long long；其余分支继续选择匹配类型
                                       : length == LONG ? va_arg(args, long)  // 长度为 l 时读取 long，不把参数字节误当另一种类型
                                       : length == SIZE ? va_arg(args, long)  // 固定 LP64 目标上 size_t 是 unsigned long，zd 对应的有符号类型为 long
                                                        : va_arg(args, int);  // 没有长度修饰时按默认提升后的 int 读取
                value = (uint64_t)signed_value;  // 先转为无符号表示，负数按模 2^64 规则转换
                if (signed_value < 0) {  // 只有负数需要额外输出负号并求其无符号量级
                    emit(&o, '-');      // 负号独立占一个输出字符，不计入数位的零填充宽度
                    value = 0 - value;  // 无符号减法取得负数的量级，避免对最小有符号值取负溢出
                }
            } else {  // u/x 没有负号，按无符号实参类型读取
                value = length == LLONG  ? va_arg(args, unsigned long long)  // 无符号 ll 按 unsigned long long 读取，不能用有符号类型替代
                        : length == LONG ? va_arg(args, unsigned long)  // 无符号 l 按 unsigned long 读取
                        : length == SIZE ? va_arg(args, size_t)  // 无符号 z 按 size_t 读取，例如 sizeof 的结果
                                         : va_arg(args, unsigned);  // 无修饰时读取 unsigned，调用者须传入匹配类型
            }
            number(&o, value, spec == 'x' ? 16 : 10, width);  // x 采用 16 进制，d/u 采用 10 进制，均传入当前零填充宽度
        } else {  // 未识别的转换保留原来的百分号与字母，不消耗实参
            emit(&o, '%');   // 保留一个字面百分号；完整百分号转换和残缺转换都使用此出口
            emit(&o, spec);  // 不认识的转换保留该字母，与前面的百分号一起显示
        }
    }
    if (cap)  // 调用者提供非零容量时才允许放终止零
        dst[o.count < cap ? o.count : cap - 1] = '\0';  // 完整结果较短时跟随内容放 NUL，截断时用数组最后一格
    return o.count > INT_MAX ? INT_MAX : (int)o.count;  // 返回完整字符数，超过 int 可表示范围时饱和为 INT_MAX
}
```

供普通调用使用的 `airi_snprintf` 用 `va_start` 建立参数遍历状态，再调用刚完成的 `airi_vsnprintf`，最后用 `va_end` 结束遍历。它转交同一组参数和输出数组，格式只解析一次。

**追加到 `kernel/lib/base.c` 末尾：**

<a id="code-kernel-lib-base-c-4"></a>

<!-- append: kernel/lib/base.c -->
```c
int airi_snprintf(char *dst, size_t cap, const char *fmt, ...) {  // 普通调用方通过省略号提供额外参数，此入口建立遍历状态
    va_list args;  // 保存本次可变参数遍历状态，不是实际参数数组
    va_start(args, fmt);  // 用最后一个具名参数 fmt 作为起点初始化遍历
    int count = airi_vsnprintf(dst, cap, fmt, args);  // 转交同一数组与参数序列，取得完整所需字符数
    va_end(args);  // 结束本次遍历，和 va_start 成对使用
    return count;  // 把完整所需长度交回调用者，不将截断长度误作结果
}
```

观察程序实际执行容量为 5 的 `IO:123`。它同时检查返回长度和五个保存字节，`memcmp` 的范围包含终止零。接着把重叠复制和截断放进一个完整跟踪程序，显示错误正向循环怎样破坏原数组，以及正确实现如何产生不同结果。

**新建 `labs/tutorial/format.c`：**

<a id="code-labs-tutorial-format-c-1"></a>

<!-- write: labs/tutorial/format.c -->
```c
#include <airi/base.h>  // 取得自有字节或格式函数声明，主机观察程序按这些签名调用
#include <stdio.h>      // 主机观察程序借用 printf 和 puts 显示结果；内核函数仍使用自己的实现
int main(void) {  // 主机 C 运行时调用此入口，返回值成为观察进程退出状态
    char text[5];  // 观察数组只有五字节，最多保存四个字符和一个 NUL
    int required = airi_snprintf(text, sizeof(text), "%s:%u", "IO", 123u);  // 传字符串与 unsigned 实参，保存 IO:1 并取得所需长度 6
    printf("stored=%s required=%d capacity=%zu\n", text, required, sizeof(text));  // 并排显示实际文字、完整长度和数组容量，分别使用匹配格式
    return required != 6 || airi_memcmp(text, "IO:1", 5) != 0;  // 长度不符或保存的五字节含 NUL 不符时，观察程序失败
}
```

`wrong` 数组专门使用错误算法；其余输入调用已经实现的正确函数。故障观察与正常实现分开，运行后内核基础库仍保持正确版本。

**新建 `labs/host/base/memmove_trace.c`：**

<a id="code-labs-host-base-memmove-trace-c-1"></a>

<!-- write: labs/host/base/memmove_trace.c -->
```c
#include <airi/base.h>  // 取得自有字节或格式函数声明，主机观察程序按这些签名调用
#include <stdio.h>      // 主机观察程序借用 printf 和 puts 显示结果；内核函数仍使用自己的实现
#include <string.h>     // 主机观察用 strcmp/strcpy 检查或准备文字，不加入内核链接

int main(void) {  // 主机 C 运行时调用此入口，返回值成为观察进程退出状态
    char right[] = "ABCDE", left[] = "ABCDE", wrong[] = "ABCDE";  // 三个独立数组分别观察两个正确方向及错误正向算法
    airi_memmove(right + 1, right, 4);  // 把前四字节向高地址挪一格，逆序搬运后得到 AABCD
    airi_memmove(left, left + 1, 4);    // 把偏移 1 的四字节向低地址挪一格，得到 BCDEE
    for (unsigned i = 0; i < 4; ++i)    // 故意从低到高搬四字节，用于展示错误，不改变基础库
        wrong[i + 1] = wrong[i];  // 目标覆盖下一轮待读源字节，B/C/D 会先后丢失
    printf("right: ABCDE -> %s (expected AABCD)\n", right);  // 显示正确逆序搬运的实际结果和期望值
    printf("left:  ABCDE -> %s (expected BCDEE)\n", left);   // 显示正确正序搬运的实际结果和期望值
    printf("wrong forward loop: %s (source bytes overwritten)\n", wrong);  // 显示错误算法所得 AAAAA，将故障与正确实现并排观察
    char output[5];  // 另准备五字节数组，用于观察较长文字的截断
    int count = airi_snprintf(output, sizeof(output), "%s", "abcdef");  // 仅保存 abcd 和 NUL，但返回完整所需长度 6
    printf("format: cap=5 stored=%s required=%d\n", output, count);     // 同时显示截断结果与完整长度，防止混淆两种数量
    if (strcmp(right, "AABCD") || strcmp(left, "BCDEE") || strcmp(wrong, "AAAAA") ||  // 检查三个搬运结果，任一不符便进入失败出口
        strcmp(output, "abcd") || count != 6)  // 再检查截断文字与所需长度，两项都必须符合当前容量模型
        return 1;  // 本轮实际结果与期望不符，以非零状态结束主机观察
    puts("PASS lab/memmove: direction and truncation observed");  // 全部观察符合才打印本次成功标记
    return 0;  // 本轮所有结果均符合，向 shell 交回成功状态
}
```

将两个观察程序分别与基础库编译链接，再依次运行。它们只使用主机 libc 的 `printf` 展示结果，格式化函数本身仍只写调用者提供的数组。

**在学习目录根目录运行：**

<!-- run -->
```sh
mkdir -p build  # 建立主机观察产物目录，已有目录也可继续
cc -std=c11 -O0 -g -Wall -Wextra -Werror -Iinclude labs/tutorial/format.c kernel/lib/base.c  -o build/probe  # 把本轮观察源码和实现编译链接成指定主机可执行文件
./build/probe  # 运行刚生成的主机观察，非零退出表示实际结果不符
mkdir -p build  # 建立主机观察产物目录，已有目录也可继续
cc -std=c11 -O0 -g -Wall -Wextra -Werror -Iinclude labs/host/base/memmove_trace.c kernel/lib/base.c  -o build/memmove  # 把本轮观察源码和实现编译链接成指定主机可执行文件
./build/memmove  # 显示两个正确方向、错误循环和截断的实际结果
```

第一段显示 `stored=IO:1 required=6 capacity=5`。第二段的错误循环得到 AAAAA，容量为 5 的格式化保存 abcd，所需字符数仍为 6。

所需长度和实际保存长度现在有了各自含义。后面串口日志可以先在固定栈数组中形成文字，再逐字发送；在此之前，要确定内核将接收怎样的启动数据。
