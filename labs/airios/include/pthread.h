#ifndef AIRI_PTHREAD_H // 当前线程源码子集，不声明完整POSIX支持。
#define AIRI_PTHREAD_H // 包含保护。
#include <stdint.h> // 身份载体与指针往返保持目标宽度。
typedef uintptr_t pthread_t; // 当前实现用真实TID作不透明身份，应用通过pthread_equal比较。
typedef struct { uint64_t reserved; } pthread_attr_t; // 仅保留类型位置，当前create只接受NULL属性，不解析此对象。
#ifdef __cplusplus // C++调用同一C实现。
extern "C" { // 不链接宿主pthread。
#endif // C直接声明。
int pthread_create(pthread_t *thread, const pthread_attr_t *attributes, void *(*function)(void *), void *argument); // 当前仅主执行者创建，成功0并写身份，失败正错误且不改errno。
int pthread_join(pthread_t thread, void **result); // 当前仅主执行者收取，NULL丢弃结果；成功才写私人输出。
pthread_t pthread_self(void); // 当前有效运行头提供身份，不能跨进程当授权句柄。
int pthread_equal(pthread_t left, pthread_t right); // 相同非零，不同0，不将可移植比较写成数值猜测。
#ifdef __cplusplus // 结束C链接声明。
} // 类型布局保持本项目约定，不兼容Linux二进制。
#endif // 结束语言差异。
#endif // 结束包含保护。
