#ifndef AIRI_THREADS_H // 自有有界共享执行接口，尚不是pthread。
#define AIRI_THREADS_H // 包含保护。
typedef long (*airi_thread_function)(void *argument); // 工作函数返回完整64位数值，由主执行者join取得。
#ifdef __cplusplus // 后续C++调用使用相同C链接名。
extern "C" { // 不要求宿主线程库。
#endif // C直接声明。
long airi_thread_create(airi_thread_function function, void *argument); // 仅主执行者创建，正TID或原始负错误，不修改errno。
long airi_thread_join(long tid, long *result); // 仅主执行者收取同组一次结果，NULL可丢弃，0成功或负错误。
long airi_thread_id(void); // 同进程PID相同，每个执行者的TID不同。
#ifdef __cplusplus // 结束链接语言范围。
} // 工作函数ABI仍为普通当前目标C调用。
#endif // 结束语言差异。
#endif // 结束包含保护。
