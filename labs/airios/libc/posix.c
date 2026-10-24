#include <airi_libc.h> // 原始调用仍返回内核负错误，不自行修改errno。
#include <errno.h> // 可修改错误左值由本地槽提供。
#include <fcntl.h> // 源码标志进入原始ABI前显式转换。
#include <unistd.h> // 导出当前已提供的POSIX风格类型与函数。
#include <sys/uio.h> // 标准源码iovec布局对应固定宽度原始描述。
#include <limits.h> // LONG_MAX限制可表示的单次字节数。
static _Thread_local int error_number; // 零初值来自PT_TLS.tbss，每个进程/未来线程应各自持有。
int *__airi_errno_location(void) { // 不把进程号当数组下标，直接使用编译器本地地址。
    return &error_number; // 宏解引用后成为可赋值int左值。
}
static long result(long value) { // 只转换原始接口返回，不解释buffer或文件内容。
    if (value < 0) { // 当前内核这些接口以负数表示明确错误。
        errno = (int)-value; // 正错误号保存在当前执行者的本地槽。
        return -1; // C调用方先看返回值，再读取errno。
    }
    return value; // 成功不清errno，也不把短字节数扩成请求长度。
}
static long invalid(int error) { // 用户库本地拒绝与内核错误使用同一结果约定。
    errno = error; // 参数验证没有发起内核副作用。
    return -1; // 调用方仍看到统一失败返回。
}
static size_t prefix(size_t bytes) { // 单次内核副本上限，不在库中变成长时间填满循环。
    return bytes > 256 ? 256 : bytes; // 短操作由调用者决定下一步与缓冲区寿命。
}
ssize_t read(int fd, void *buffer, size_t bytes) { // 对有界内核接口进行一次实际尝试。
    if (bytes > LONG_MAX) // 超出ssize_t范围采用本实现明确的参数错误策略。
        return invalid(EINVAL); // 不先截短一个不可表示的请求。
    return result(airi_read((unsigned)fd, buffer, prefix(bytes))); // 负fd转换后由内核完整范围检查为EBADF。
}
ssize_t write(int fd, const void *buffer, size_t bytes) { // 成功可能只处理可完成前缀。
    if (bytes > LONG_MAX) // 不让返回值容量与输入不一致。
        return invalid(EINVAL); // 无系统调用副作用。
    return result(airi_write((unsigned)fd, buffer, prefix(bytes))); // 只提交一次，不掩盖部分进展。
}
ssize_t pread(int fd, void *buffer, size_t bytes, off_t offset) { // 显式偏移使用当前有符号接口。
    if (offset < 0 || bytes > LONG_MAX) // 不将负数转换成巨大的合法无符号偏移。
        return invalid(EINVAL); // 共享位置保持。
    return result(airi_pread((unsigned)fd, buffer, prefix(bytes), (uint64_t)offset)); // 内核仍检查FD、跨度和文件范围。
}
ssize_t pwrite(int fd, const void *buffer, size_t bytes, off_t offset) { // 位置和实际前缀分别传递。
    if (offset < 0 || bytes > LONG_MAX) // 与pread使用相同参数策略。
        return invalid(EINVAL); // 不写任何字节。
    return result(airi_pwrite((unsigned)fd, buffer, prefix(bytes), (uint64_t)offset)); // 不移动共享文件位置。
}
int open(const char *path, int flags, ...) { // 兼容带mode参数的源码调用，RAM模型当前没有权限字段。
    if ((flags & ~(O_ACCMODE | O_CREAT | O_TRUNC | O_CLOEXEC)) || (flags & O_ACCMODE) == 3) // 未实现标志不能静默忽略，访问枚举3无效。
        return (int)invalid(EINVAL); // mode本轮不参与权限判定，不读取可选参数。
    unsigned raw = (flags & O_ACCMODE) == O_RDONLY ? AIRI_READ : (flags & O_ACCMODE) == O_WRONLY ? AIRI_WRITE : AIRI_READ | AIRI_WRITE; // O_RDONLY为0，不能按非零位判断可读。
    if (flags & O_CREAT) // 源码常量与原始内核常量分别解释。
        raw |= AIRI_CREATE; // 尚未进行创建。
    if (flags & O_TRUNC) // 具体写权限仍由内核原子检查。
        raw |= AIRI_TRUNCATE; // 不在库中先清文件。
    if (flags & O_CLOEXEC) // 描述符表项标志。
        raw |= AIRI_CLOEXEC; // 成功exec才关闭。
    return (int)result(airi_open(path, raw)); // 失败路径、FD分配与名称所有权仍在内核。
}
int close(int fd) { // 不自行记一份可能过期的FD表。
    return (int)result(airi_close((unsigned)fd)); // 0成功，-1加当前errno失败。
}
int dup(int fd) { // 新表项共享对象及偏移，但不是复制文件内容。
    return (int)result(airi_dup((unsigned)fd, 0)); // 新表项不继承CLOEXEC。
}
int pipe(int ends[2]) { // 直接交用户输出，内核在创建资源前验证完整范围。
    _Static_assert(sizeof(int) == sizeof(unsigned), "pipe ABI integer width"); // 两个整数与原始ABI保持字节布局一致。
    return (int)result(airi_pipe((unsigned *)ends, 0)); // 阻塞模式；不借局部数组后再冒险写坏用户指针。
}
off_t lseek(int fd, off_t offset, int whence) { // 负差值可用于CUR/END，最终位置由内核有界检查。
    if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END) // 源码未知基准先拒绝。
        return invalid(EINVAL); // 不查询或修改对象。
    return result(airi_seek_from((unsigned)fd, offset, (unsigned)whence)); // 原子地读取共享基准并提交新位置，成功不清errno。
}
pid_t getpid(void) { // 当前原始getpid不会失败，仍复用一致转换。
    return result(airi_getpid()); // 使用可容纳完整内核PID的有符号类型。
}
void _exit(int code) { // 直接退出，不提供完整C标准exit或析构流程。
    airi_exit((unsigned)code); // 原内核按低八位编码普通退出值。
}

_Static_assert(sizeof(struct iovec) == sizeof(struct airi_iovec), "iovec ABI size"); // 当前目标指针与size_t都是64位。
_Static_assert(offsetof(struct iovec, iov_base) == offsetof(struct airi_iovec, address), "iovec base offset"); // 第一项按地址数字读取。
_Static_assert(offsetof(struct iovec, iov_len) == offsetof(struct airi_iovec, bytes), "iovec length offset"); // 第二项为完整64位长度。
ssize_t preadv(int fd, const struct iovec *vectors, int count, off_t offset) { // 只转换结果，不在用户库预读指针数组。
    if (count < 0 || offset < 0) // 保留源码有符号参数语义。
        return invalid(EINVAL); // 负数不转换成大正索引。
    return result(airi_preadv((unsigned)fd, (const struct airi_iovec *)vectors, (unsigned)count, (uint64_t)offset)); // 布局由上面断言约束，所有描述复制在内核。
}
ssize_t pwritev(int fd, const struct iovec *vectors, int count, off_t offset) { // 多段写仍是一个文件层操作。
    if (count < 0 || offset < 0) // 同一参数策略。
        return invalid(EINVAL); // 没有文件副作用。
    return result(airi_pwritev((unsigned)fd, (const struct airi_iovec *)vectors, (unsigned)count, (uint64_t)offset)); // 实际前缀数量交给调用者继续。
}
