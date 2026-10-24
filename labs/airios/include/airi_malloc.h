#ifndef AIRI_MALLOC_H // 用户分配器的只读观察，不是内核ABI。
#define AIRI_MALLOC_H // 包含保护。
#include <stddef.h> // 各项按当前进程的字节与对象数计。
struct airi_malloc_info { size_t arena_bytes, live_bytes, live_objects; }; // 页面缓存、请求字节和活对象分别统计。
#ifdef __cplusplus // 结构不含内部块指针，函数保留C名字。
extern "C" { // 与stdlib接口使用同一链接约定。
#endif // C源码直接声明。
struct airi_malloc_info airi_malloc_info(void); // 当前单线程快照，不分配、不清理也不改errno。
#ifdef __cplusplus // 结束链接范围。
} // 此观察接口不是线程同步工具。
#endif // 结束语言差异。
#endif // 结束包含保护。
