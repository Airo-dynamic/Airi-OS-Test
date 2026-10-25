#ifndef AIRI_STDLIB_H // 有界对象分配共享内部元数据锁，不引入宿主stdlib。
#define AIRI_STDLIB_H // 包含保护。
#include <stddef.h> // 对象大小沿用目标工具链的size_t。
#ifdef __cplusplus // 为未来C++调用保留C链接名字。
extern "C" { // 声明本身不启动C++运行库。
#endif // C11无需链接包装。
void *malloc(size_t bytes); // 非零失败NULL并设置ENOMEM；零长度NULL且不改errno。
void free(void *pointer); // NULL空操作；其他参数必须是本库仍存活且由调用者独占交还权的对象起点。
void *calloc(size_t count, size_t bytes); // 先检查乘法溢出，再分配并清零请求范围。
void *realloc(void *pointer, size_t bytes); // 失败保持旧对象；本C11实现将非NULL零长度视为free。
#ifdef __cplusplus // 结束函数的C链接范围。
} // 不改变对象布局。
#endif // 结束语言差异。
#endif // 结束包含保护。
