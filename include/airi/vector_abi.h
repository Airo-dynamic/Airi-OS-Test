#ifndef AIRI_VECTOR_ABI_H // 显式偏移向量I/O的固定字段，不含内核指针。
#define AIRI_VECTOR_ABI_H // 包含保护。
#include <stdint.h> // 两端都以64位数字解释地址和长度。
#define AIRI_VECTOR_COUNT 8u // 一次最多八项，描述数组可完整复制到有界内核栈。
#define AIRI_VECTOR_BYTES 256u // 一次只处理串联数据的前256字节，不隐式循环。
struct airi_iovec { uint64_t address, bytes; }; // 地址是当前用户VA，描述先复制，不跨返回借用。
_Static_assert(sizeof(struct airi_iovec) == 16, "vector ABI size"); // 固定每项两个8字节字段。
#endif // 结束包含保护。
