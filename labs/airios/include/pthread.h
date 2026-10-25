#ifndef AIRI_PTHREAD_H // 当前线程源码子集，不声明完整POSIX支持。
#define AIRI_PTHREAD_H // 包含保护。
#include <stdint.h> // 身份载体与指针往返保持目标宽度。
#include <airi_mutex.h> // 标准形状的互斥仍由自有所有权锁执行。
typedef uintptr_t pthread_t; // 当前实现用真实TID作不透明身份，应用通过pthread_equal比较。
typedef struct { uint64_t reserved; } pthread_attr_t; // 仅保留类型位置，当前create只接受NULL属性，不解析此对象。
#define PTHREAD_MUTEX_NORMAL 0 // 已知类型名，当前属性设置明确不支持。
#define PTHREAD_MUTEX_ERRORCHECK 1 // 当前实现的所有权错误检查类型。
#define PTHREAD_MUTEX_DEFAULT PTHREAD_MUTEX_ERRORCHECK // 本项目将DEFAULT映到ERRORCHECK，不冒充NORMAL语义。
#define PTHREAD_MUTEX_RECURSIVE 2 // 尚无重入计数，不接受此类型。
#define AIRI_PTHREAD_MUTEX_READY 1u // 仅是用户库初始化标记，不是内核权限或映射有效证明。
typedef struct { int type; unsigned ready; } pthread_mutexattr_t; // 调用者初始化/设置/销毁期间须独占这个属性对象。
typedef struct { airi_mutex core; uint32_t state; } pthread_mutex_t; // core保护普通数据，state只诊断初始化生命周期。
#define PTHREAD_MUTEX_INITIALIZER {AIRI_MUTEX_INITIALIZER, AIRI_PTHREAD_MUTEX_READY} // 在任何并发访问以前建立可用的默认互斥。
#ifdef __cplusplus // C++调用同一C实现。
extern "C" { // 不链接宿主pthread。
#endif // C直接声明。
int pthread_create(pthread_t *thread, const pthread_attr_t *attributes, void *(*function)(void *), void *argument); // 同进程执行者可创建，成功0并写身份，失败正错误且不改errno。
int pthread_join(pthread_t thread, void **result); // 同组worker结果唯一收取，NULL丢弃；主执行者暂不可作目标，成功才写私人输出。
pthread_t pthread_self(void); // 当前有效运行头提供身份，不能跨进程当授权句柄。
int pthread_equal(pthread_t left, pthread_t right); // 相同非零，不同0，不将可移植比较写成数值猜测。
int pthread_mutexattr_init(pthread_mutexattr_t *attributes); // 默认ERRORCHECK映射，不分配。
int pthread_mutexattr_destroy(pthread_mutexattr_t *attributes); // 此属性不再被并发使用时撤销可用标记。
int pthread_mutexattr_gettype(const pthread_mutexattr_t *attributes, int *type); // 成功才写私人输出。
int pthread_mutexattr_settype(pthread_mutexattr_t *attributes, int type); // DEFAULT/ERRORCHECK接受，NORMAL/RECURSIVE不伪造支持。
int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attributes); // 未发布对象初始化，不覆盖活锁。
int pthread_mutex_destroy(pthread_mutex_t *mutex); // 调用者先确保无人使用；检测持有/慢路径返回EBUSY。
int pthread_mutex_lock(pthread_mutex_t *mutex); // 无总超时，内部有限等待重试；重入EDEADLK，不改errno。
int pthread_mutex_trylock(pthread_mutex_t *mutex); // 一次争取，忙EBUSY。
int pthread_mutex_unlock(pthread_mutex_t *mutex); // 错误所有者EPERM，成功0，不改errno。
#ifdef __cplusplus // 结束C链接声明。
} // 类型布局保持本项目约定，不兼容Linux二进制。
#endif // 结束语言差异。
#endif // 结束包含保护。
