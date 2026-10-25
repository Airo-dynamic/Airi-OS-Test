#ifndef AIRI_MUTEX_H // 有界私有互斥接口，不使用宿主pthread类型。
#define AIRI_MUTEX_H // 防止重复声明。
#include <stdint.h> // 锁字与TID同为四字节，0保留为空闲。
typedef struct { uint32_t owner; } airi_mutex; // 并发访问只经过运行库原子操作，不直接读写owner。
#define AIRI_MUTEX_INITIALIZER {0} // 在发布给其他执行者以前静态或局部初始化。
#ifdef __cplusplus // C++可以包含同一声明，不在头内暴露_Atomic语法。
extern "C" { // 调用已链接的C实现，保持符号名。
#endif // C编译时跳过链接语言范围。
long airi_mutex_trylock(airi_mutex *mutex); // 0取得，已占用-16；不等待。
long airi_mutex_lock(airi_mutex *mutex, unsigned ticks); // 有限PIT预算，重入-35，到期-110，0只尝试一次。
long airi_mutex_unlock(airi_mutex *mutex); // 只允许持有TID释放；非持有者-1，随后最多通知一人。
#ifdef __cplusplus // 结束函数的C链接声明。
} // 类型仍能由C++按普通聚合初始化。
#endif // 结束语言差异。
#endif // 结束包含保护。
