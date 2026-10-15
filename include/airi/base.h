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
#endif  // 结束头文件包含保护，后续使用同一公开声明
