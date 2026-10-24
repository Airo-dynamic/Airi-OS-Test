#ifndef AIRI_FCNTL_H // 当前只声明已提供的open子集。
#define AIRI_FCNTL_H // 包含保护。
#include <sys/types.h> // mode_t与目标用户类型。
#define O_RDONLY 0 // 访问模式是低两位枚举，不是独立可叠加位。
#define O_WRONLY 1 // 只写。
#define O_RDWR 2 // 读写。
#define O_ACCMODE 3 // 提取访问模式；值3无效。
#define O_CREAT 0x100 // 本运行库源码常量，进入内核前转换，不是Linux ABI。
#define O_TRUNC 0x200 // 已有文件清空需要写权限。
#define O_CLOEXEC 0x400 // 成功exec关闭当前FD表项。
#ifdef __cplusplus // C++调用相同静态运行库。
extern "C" { // 使用C函数名。
#endif // C11无需额外包装。
int open(const char *path, int flags, ...); // O_CREAT允许mode参数；当前RAM模型不保存权限位。
#ifdef __cplusplus // 结束声明语言范围。
} // 仅open入口使用C链接。
#endif // 结束语言差异。
#endif // 结束包含保护。
