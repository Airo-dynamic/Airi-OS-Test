#ifndef AIRI_TIME_H // 本运行库的单调时钟子集，不读取宿主time.h。
#define AIRI_TIME_H // 包含保护。
#include <sys/types.h> // 目标的整数与时间类型。
typedef int clockid_t; // 源码选择时钟，内核仍独立验证完整参数。
struct timespec { int64_t tv_sec; long tv_nsec; }; // 当前x86-64两个8字节字段，纳秒不是实际硬件精度。
#define CLOCK_REALTIME 0 // 名称存在但本轮未提供墙钟，明确EINVAL。
#define CLOCK_MONOTONIC 1 // 从自有PIT初始化起的名义单调时间。
#ifdef __cplusplus // 未来C++源码以同一C函数名调用。
extern "C" { // 不引入C++运行库依赖。
#endif // C11直接声明。
int clock_gettime(clockid_t clock, struct timespec *value); // 成功0，坏时钟/输出返回-1并设置errno。
#ifdef __cplusplus // 结束函数链接范围。
} // 结构本身不需要语言包装。
#endif // 结束语言差异。
#endif // 结束包含保护。
