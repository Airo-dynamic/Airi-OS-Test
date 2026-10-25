#ifndef AIRI_ERRNO_H // 错误状态由当前用户库拥有，不引用宿主运行库。
#define AIRI_ERRNO_H // 包含保护。
#define EPERM 1 // 当前操作不允许。
#define ENOENT 2 // 名称不存在。
#define ESRCH 3 // 当前线程身份不存在或已经收取。
#define EINTR 4 // 预留中断错误名；当前尚无信号，不会主动制造它。
#define EIO 5 // 设备或I/O错误。
#define ENOEXEC 8 // 映像格式不支持。
#define EBADF 9 // FD无效或访问模式不符。
#define EAGAIN 11 // 暂不可完成，非阻塞调用可重试。
#define EWOULDBLOCK EAGAIN // 当前实现使用同一暂不可完成错误。
#define ENOMEM 12 // 内存资源不足。
#define EACCES 13 // 当前访问被拒绝。
#define EFAULT 14 // 用户缓冲区不可访问。
#define EBUSY 16 // 资源仍被占用。
#define EINVAL 22 // 参数不符合本接口范围。
#define ENFILE 23 // 全局打开对象不足。
#define EMFILE 24 // 当前进程FD表不足。
#define ENOSPC 28 // 有界文件或目录空间不足。
#define ESPIPE 29 // 字节流不能定位。
#define EPIPE 32 // 没有读端；当前内核不附带SIGPIPE。
#define EDEADLK 35 // 当前执行者不能重复取得自己的非递归锁。
#define ENOSYS 38 // 当前未实现的操作。
#define ETIMEDOUT 110 // 有界等待预算已经用完。
#define EOVERFLOW 75 // 不能表示请求结果。
#define ENOTSUP 95 // 当前明确不支持的属性或线程管理方式。
#ifdef __cplusplus // 同一头以后可被C++源码使用，函数仍用C链接名。
extern "C" { // 防止把运行库入口编码成C++重载符号。
#endif // C11跳过链接语言包装。
int *__airi_errno_location(void); // 地址属于当前静态TLS副本，不能作为另一个线程的错误槽。
#ifdef __cplusplus // 结束C链接声明范围。
} // 只包围函数声明，不改变错误宏。
#endif // 结束语言差异。
#define errno (*__airi_errno_location()) // 可修改的int左值；每次按当前执行者定位。
#endif // 结束包含保护。
