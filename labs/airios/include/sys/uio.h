#ifndef AIRI_SYS_UIO_H // 当前仅声明不建立流等待的显式偏移向量操作。
#define AIRI_SYS_UIO_H // 包含保护。
#include <sys/types.h> // 用户指针、容量与有符号文件偏移。
#define IOV_MAX 8 // 本源码接口至多八项，低于完整POSIX平台要求，明确为有界子集。
struct iovec { void *iov_base; size_t iov_len; }; // 数组顺序决定串联字节顺序，不代表物理连续。
#ifdef __cplusplus // 标准名字由同一C实现提供给未来C++源。
extern "C" { // 保持链接符号一致。
#endif // C11直接声明。
ssize_t preadv(int fd, const struct iovec *vectors, int count, off_t offset); // 一次至多256字节，显式偏移不移动共享位置。
ssize_t pwritev(int fd, const struct iovec *vectors, int count, off_t offset); // 前缀可在一项中结束；流返回ESPIPE。
#ifdef __cplusplus // 结束C链接范围。
} // 这里只声明两个已经实现的入口。
#endif // 结束语言差异。
#endif // 结束包含保护。
