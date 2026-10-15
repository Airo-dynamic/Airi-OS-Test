#include <airi_libc.h> // 用户库自己提供标准字节函数，不链接内核实现。
void *memcpy(void *destination, const void *source, size_t bytes) { // 复制完成后返回原目的指针。
    unsigned char *out = destination; // 写方按字节访问。
    const unsigned char *in = source; // 读方不修改内容。
    for (size_t i = 0; i < bytes; ++i) // 零长度不读取任何字节。
        out[i] = in[i]; // 不处理重叠，调用者应使用memmove。
    return destination; // 符合本用户库的标准函数签名。
}
void *memmove(void *destination, const void *source, size_t bytes) { // 重叠时先保护尚未读取的源。
    unsigned char *out = destination; // 目标字节跨度。
    const unsigned char *in = source; // 源字节跨度。
    if ((uintptr_t)out < (uintptr_t)in) // 目标在源前，从低到高不会覆盖未来源。
        for (size_t i = 0; i < bytes; ++i) // 正向移动。
            out[i] = in[i]; // 每次读取之后再覆盖目标。
    else // 目标在源后或相同，从高到低避免覆盖后续源。
        while (bytes) { // 不用无符号i>=0造成下溢死循环。
            --bytes; // 先退到最后一个实际字节。
            out[bytes] = in[bytes]; // 逆序复制仍保留源内容。
        }
    return destination; // 整体完成后返回目的起点。
}
void *memset(void *destination, int value, size_t bytes) { // 填充值只保留低8位。
    unsigned char *out = destination; // 用户可写对象的起点。
    for (size_t i = 0; i < bytes; ++i) // 精确写指定跨度。
        out[i] = (unsigned char)value; // 不把int宽度误当每个元素宽度。
    return destination; // 返回原目的地址。
}
int memcmp(const void *left, const void *right, size_t bytes) { // 只比较调用者声明的有效范围。
    const unsigned char *a = left, *b = right; // 无符号比较避免char符号性改变顺序。
    for (size_t i = 0; i < bytes; ++i) // 相等前缀继续向后。
        if (a[i] != b[i]) // 第一个不同字节就是结果依据。
            return (int)a[i] - (int)b[i]; // 结果正负分别表示左右大小。
    return 0; // 整个跨度相等。
}
size_t strlen(const char *text) { // 有效字符串必须在可访问范围内终止。
    size_t bytes = 0; // 终止零不计入长度。
    while (text[bytes]) // 非零字节继续。
        ++bytes; // 前移到下一个字节。
    return bytes; // 调用方仍负责指针和生命周期。
}
