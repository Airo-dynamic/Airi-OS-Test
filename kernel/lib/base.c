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
