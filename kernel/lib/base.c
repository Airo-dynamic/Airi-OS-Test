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
int airi_snprintf(char *dst, size_t cap, const char *fmt, ...) {  // 普通调用方通过省略号提供额外参数，此入口建立遍历状态
    va_list args;  // 保存本次可变参数遍历状态，不是实际参数数组
    va_start(args, fmt);  // 用最后一个具名参数 fmt 作为起点初始化遍历
    int count = airi_vsnprintf(dst, cap, fmt, args);  // 转交同一数组与参数序列，取得完整所需字符数
    va_end(args);  // 结束本次遍历，和 va_start 成对使用
    return count;  // 把完整所需长度交回调用者，不将截断长度误作结果
}
