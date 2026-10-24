#ifndef AIRI_UNISTD_H // 有界POSIX风格用户接口，不读取Mac/Linux宿主头。
#define AIRI_UNISTD_H // 包含保护。
#include <sys/types.h> // size_t、ssize_t、off_t和pid_t属于当前目标。
#define STDIN_FILENO 0 // 当前前台UART输入表项。
#define STDOUT_FILENO 1 // UART输出表项。
#define STDERR_FILENO 2 // 同一固定UART输出对象。
#define SEEK_SET 0 // 当前仅支持绝对定位。
#define SEEK_CUR 1 // 常量保留，当前lseek明确返回ENOSYS。
#define SEEK_END 2 // 当前没有查询长度接口，明确拒绝。
#ifdef __cplusplus // 允许后续C++源使用同一C实现。
extern "C" { // 不使用C++名字改编。
#endif // C语言直接声明。
ssize_t read(int fd, void *buffer, size_t bytes); // 单次至多256字节，可能短读，EOF为0，失败-1并置errno。
ssize_t write(int fd, const void *buffer, size_t bytes); // 返回实际前缀，不偷偷循环写满。
ssize_t pread(int fd, void *buffer, size_t bytes, off_t offset); // 显式非负偏移，不改共享位置。
ssize_t pwrite(int fd, const void *buffer, size_t bytes, off_t offset); // 同上，只写实际前缀。
int close(int fd); // 成功0，错误-1；不能以旧errno判断成功调用失败。
int dup(int fd); // 共享打开对象与偏移，新FD默认非CLOEXEC。
int pipe(int ends[2]); // 阻塞管道；当前64字节容量，不承诺完整POSIX PIPE_BUF原子性。
off_t lseek(int fd, off_t offset, int whence); // 当前只接受SEEK_SET及原内核有界文件范围。
pid_t getpid(void); // 返回真实身份，非槽下标。
void _exit(int code) __attribute__((noreturn)); // 不运行stdio刷新或析构，直接结束当前用户。
#ifdef __cplusplus // 结束函数声明的C链接范围。
} // 类型与宏不需要语言包装。
#endif // 结束语言差异。
#endif // 结束包含保护。
