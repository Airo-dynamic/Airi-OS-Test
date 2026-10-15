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
