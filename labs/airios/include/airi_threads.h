#ifndef AIRI_THREADS_H // 自有有界共享执行接口，尚不是pthread。
#define AIRI_THREADS_H // 包含保护。
#include <stdint.h> // 等待字明确为四字节无符号值，不随指针宽度改变。
typedef long (*airi_thread_function)(void *argument); // 工作函数返回完整64位数值，由主执行者join取得。
#ifdef __cplusplus // 后续C++调用使用相同C链接名。
extern "C" { // 不要求宿主线程库。
#endif // C直接声明。
long airi_thread_create(airi_thread_function function, void *argument); // 仅主执行者创建，正TID或原始负错误，不修改errno。
long airi_thread_join(long tid, long *result); // 仅主执行者收取同组一次结果，NULL可丢弃，0成功或负错误。
long airi_thread_id(void); // 有效静态TLS头直接提供当前TID，旧版本退回42号；同进程PID仍共享。
long airi_futex_wait(uint32_t *word, uint32_t expected, unsigned ticks); // 私有键、四字节对齐，有限PIT预算；0通知、-11值不符、-110到期。
long airi_futex_wake(uint32_t *word, unsigned count); // 至多唤醒十六个同进程已登记者，不修改字值，不累计通知。
#ifdef __cplusplus // 结束链接语言范围。
} // 工作函数ABI仍为普通当前目标C调用。
#endif // 结束语言差异。
#endif // 结束包含保护。
