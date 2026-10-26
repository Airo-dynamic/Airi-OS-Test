# K47 · 将名义单调时间通过受检查的接口交给用户

从K46继续。等待、调度和用户sleep已经用真实PIT tick，普通源码还需要一个明确的时间结构来比较间隔。本节提供clock_gettime(CLOCK_MONOTONIC)，从自有PIT初始化起计时；不会把它当作日期，也不提供CLOCK_REALTIME。

当前时钟来自已交付的100Hz名义tick。每次输出量化到10ms，连续查询可能相同；PIT整数分频有误差，长时间关闭中断也可能漏计，所以它不是准确墙钟或现代高精度clocksource。后续部署还需独立计数时钟源、校准、RTC和休眠语义；纳秒字段只规定单位，不提高硬件精度。

<a id="clock-value"></a>
## 秒和纳秒来自同一次 tick 快照

单调时间用于比较先后和间隔，[时钟手册](https://man7.org/linux/man-pages/man2/clock_gettime.2.html)区分它与可校时的日期时间。这里保留单调查询的形式，缩小为当前自有计数器。设快照ticks为123，则seconds=1，nanoseconds=230000000。取模100保证纳秒小于十亿，一份16字节记录一次形成，不能先查秒再另查纳秒而跨过一个tick。

调用者给出的 timespec 地址只是输出目的地。包装库先把这个地址数值交给38号调用，内核读取一次计数并形成两字段记录，再由 copy_user 检查完整16字节可写跨度、复制到用户页。这样坏地址在系统调用边界得到−14，用户程序仍能处理错误，不由包装库先解引用而触发自身故障。

时钟编号先于输出检查：不支持的请求返回−22，原输出不动；合法请求的坏指针才返回−14。用户库继续把负错误转换为−1和errno。这个计数从内核 PIT 初始化开始，不含固件启动时间，也不是本进程获得的 CPU 用量。

先新建time_abi.h，再在用户侧新建time.h。固定目标为两个8字节字段，公开timespec保留有符号秒和long纳秒；当前计数除100后的秒数不会超出有符号64位范围。C++声明只处理链接名，不提供完整时间库。

<!-- write: include/airi/time_abi.h -->
```c
#ifndef AIRI_TIME_ABI_H // 单调时间数值记录，不携带内核对象或墙钟。
#define AIRI_TIME_ABI_H // 包含保护。
#include <stdint.h> // 两端按固定64位字段复制。
#define AIRI_CLOCK_MONOTONIC 1u // 自有调用目前唯一支持的时钟。
struct airi_time_value { uint64_t seconds, nanoseconds; }; // 纳秒字段始终小于十亿，分辨率仍是10ms。
_Static_assert(sizeof(struct airi_time_value) == 16, "time ABI size"); // 固定输出跨度供用户验证。
#endif // 结束包含保护。
```

<!-- write: labs/airios/include/time.h -->
```c
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
```

<a id="clock-boundary"></a>
## 输出只在内核边界复制

user.c增加38号分支，完整比较时钟编号1，不先窄化参数；在本CPU的中断门内读取一次ticks，再计算秒和纳秒。其余时间值、用户进程状态与等待队列保持。

原始包装只传编号与输出地址，不在用户空间先写一个临时对象再盲目复制。posix.c用结构长度及纳秒字段偏移断言固定布局后传递指针，沿result处理返回。用户time.h里的CLOCK_REALTIME只是明确可拒绝的请求名，不能因常量存在就把自有单调计数解释成1970年以来的秒。

接下来按现有 `kernel/process/user.c` 中的位置逐组替换，未列出的前章内容保持。所有映像、用户库与构建接入完成后再运行。

在包含区接入固定时间记录。

本章每组替换的第一块只用于定位旧文本，敲入第二块替换它，不把两块同时加入文件。各组按正文顺序完成。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
#include <airi/vector_abi.h> // 向量描述在一次内核调用内完整复制，文件操作前检查本次前缀。
#include <airi/tls.h> // 每次真实用户陷入取回FS，内核处理期间清零。
#include <airi/fpu.h> // 每进程拥有一份非用户映射的浮点/SSE状态。
```
<!-- after -->
```c
#include <airi/time_abi.h> // 单调时间只输出固定数值，用户指针由当前根验证。
#include <airi/vector_abi.h> // 向量描述在一次内核调用内完整复制，文件操作前检查本次前缀。
#include <airi/tls.h> // 每次真实用户陷入取回FS，内核处理期间清零。
#include <airi/fpu.h> // 每进程拥有一份非用户映射的浮点/SSE状态。
```

在37号定位分支之后、未知调用拒绝之前接入38号。只读一次 tick，形成两字段记录后，通过当前用户根复制输出。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
        else // RSI位模式在当前二补数x86-64目标解释为有符号偏移。
            f->rax = (uint64_t)airi_fd_seek_from(&current->files, (unsigned)f->rdi, (int64_t)f->rsi, (unsigned)f->rdx); // 一次读取基准、验证并提交，旧14号绝对接口保留。
    }
    else // 尚未提供的接口都明确拒绝。
        f->rax = (uint64_t)-38ll; // -ENOSYS。
    poll_waiters(); // 文件/流修改完成后立即重查其他等待者，不等用户轮询。
```
<!-- after -->
```c
        else // RSI位模式在当前二补数x86-64目标解释为有符号偏移。
            f->rax = (uint64_t)airi_fd_seek_from(&current->files, (unsigned)f->rdi, (int64_t)f->rsi, (unsigned)f->rdx); // 一次读取基准、验证并提交，旧14号绝对接口保留。
    }
    else if (f->rax == 38) { // 只提供PIT单调时钟，不伪造RTC墙钟。
        if (f->rdi != AIRI_CLOCK_MONOTONIC) // 完整参数不截成另一个合法编号。
            f->rax = (uint64_t)-22ll; // -EINVAL，输出保持。
        else { // tick仅由真实IRQ增加，同一CPU读出一次完整快照。
            uint64_t ticks = airi_timer_ticks(); // 基准是本内核PIT初始化，不含固件启动时间。
            struct airi_time_value value = {ticks / 100, (ticks % 100) * 10000000ull}; // 名义100Hz换算；纳秒单位不提高实际分辨率。
            f->rax = copy_user(current, &value, f->rsi, sizeof(value), true) ? 0 : (uint64_t)-14ll; // 完整输出先验证，坏指针不产生用户CPU故障。
        }
    }
    else // 尚未提供的接口都明确拒绝。
        f->rax = (uint64_t)-38ll; // -ENOSYS。
    poll_waiters(); // 文件/流修改完成后立即重查其他等待者，不等用户轮询。
```

<details>
<summary>labs/airios/include/airi_libc.h 的完整终点</summary>

<!-- write: labs/airios/include/airi_libc.h -->
```c
#ifndef AIRI_USER_LIBC_H // 用户库头与内核基础库分开。
#define AIRI_USER_LIBC_H // 包含保护。
#include <airi/time_abi.h> // 单调时钟输出的固定字段。
#include <airi/vector_abi.h> // 显式偏移向量描述采用固定16字节原始记录。
#include <airi/io_abi.h> // 自有固定宽度提交、完成和只读状态，不包含内核结构。
#include <stddef.h> // 用户对象字节长度。
#include <stdint.h> // 用户ABI的64位寄存器值。
void *memcpy(void *destination, const void *source, size_t bytes); // 双方不重叠的用户空间复制。
void *memmove(void *destination, const void *source, size_t bytes); // 允许同一用户数组内重叠移动。
void *memset(void *destination, int value, size_t bytes); // 按字节填充用户对象。
int memcmp(const void *left, const void *right, size_t bytes); // 首个不同无符号字节决定结果。
size_t strlen(const char *text); // 用户有效NUL字符串长度。
long airi_write(unsigned fd, const void *buffer, size_t bytes); // 自有int80 ABI，失败直接返回负数。
long airi_getpid(void); // 获取当前用户身份。
_Noreturn void airi_exit(unsigned code); // 退出不返回用户调用方。
long airi_puts(const char *text); // 有界 FD 写入，不自动补换行。
long airi_fork(void); // 首版内核直接复制用户页，子返回0，父返回子PID。
long airi_waitpid(long pid, int *status); // pid=-1或指定孩子；status是普通退出值。
void *airi_alloc(size_t bytes); // 零填充页申请，失败NULL；显式release整页或进程结束回收。
uint64_t airi_ticks(void); // 实际时钟事件计数。
void airi_yield(void); // 主动交接，不等同于时钟抢占。
long airi_exec(const char *name); // 成功替换映像不返回；失败保留旧PID和调用链，返回负错误。
#define AIRI_READ 1u // 以下标志只属于自有open ABI。
#define AIRI_WRITE 2u // 写访问。
#define AIRI_CREATE 4u // 名字不存在时创建。
#define AIRI_TRUNCATE 8u // 清空可写文件。
#define AIRI_CLOEXEC 16u // 成功exec关闭此表项。
long airi_open(const char *path, unsigned flags); // 平坦绝对名字，返回FD或负错误。
long airi_close(unsigned fd); // 关闭不删除名字。
long airi_dup(unsigned fd, unsigned cloexec); // 与源共享偏移。
long airi_seek(unsigned fd, uint64_t offset); // 只支持绝对定位。
long airi_read(unsigned fd, void *buffer, size_t bytes); // EOF为0，短读是实际完成数。
long airi_pread(unsigned fd, void *buffer, size_t bytes, uint64_t offset); // 不移动共享偏移。
long airi_pwrite(unsigned fd, const void *buffer, size_t bytes, uint64_t offset); // 第四参数R10。
long airi_log_append(const void *data, size_t bytes); // 最多480字节，成功返回从1开始的sequence。
long airi_log_read(unsigned index, void *data, size_t capacity); // 索引从0，返回实际长度。
long airi_log_count(void); // 已恢复有效前缀，错误为负。
#define AIRI_READABLE 1u // 当前read可得到数据或EOF。
#define AIRI_WRITABLE 2u // 当前有读者和空位。
#define AIRI_HANGUP 4u // 最后写者关闭，仍应排空已有数据。
#define AIRI_ERROR 8u // 最后读者关闭，写将EPIPE。
long airi_pipe(unsigned ends[2], unsigned nonblocking); // nonblocking=0阻塞，1暂不可完成时返回-11。
long airi_ready(unsigned fd); // 管道当前条件查询，不是Linux epoll ABI。
long airi_wait_ready(unsigned fd, unsigned events, unsigned ticks); // READ/WRITE条件，0纯查询，最多500tick，超时返回0。
long airi_io_alloc(void); // 注册一个512字节内核buffer，返回句柄而不是指针。
long airi_io_put(uint64_t handle, const void *data, size_t bytes); // IDLE可修改，BUSY返回-16。
long airi_io_get(uint64_t handle, void *data, size_t bytes); // CQ收集后才可读取。
long airi_io_free(uint64_t handle); // 旧句柄-9，BUSY-16。
long airi_io_submit(unsigned operation, uint64_t handle, const struct airi_io_submission *submission); // 0仅接收，描述在返回前已复制。
long airi_io_next(struct airi_io_completion *completion, unsigned wait); // 0不等，1阻塞；返回0仅表示交出一条CQ。
long airi_io_state_get(struct airi_io_state *state); // 当前轮次与同PID退役ACTIVE的只读数值。
long airi_sleep(unsigned ticks); // 0立即成功，1至6000tick真阻塞；此时钟100Hz，不是墙钟。
long airi_release(void *address, size_t bytes); // 解除一至六十四个完整动态页；允许部分范围，失败不改整段。
long airi_programs(char *names, size_t capacity); // 真实初始ELF目录，每行名称，容量不足不截断。
long airi_fs_set(uintptr_t base); // 自有ABI设置低半区FS基址，0停用；不创建或固定映射。
long airi_fs_base(void); // 返回当前用户FS基址，尚不是通用arch_prctl或完整TLS运行库。
long airi_preadv(unsigned fd, const struct airi_iovec *vectors, unsigned count, uint64_t offset); // 35号调用，负错误原样返回。
long airi_pwritev(unsigned fd, const struct airi_iovec *vectors, unsigned count, uint64_t offset); // 36号调用，一次至多256字节。
long airi_seek_from(unsigned fd, int64_t delta, unsigned whence); // 37号调用，0/1/2按SET/CUR/END选择基准，可有符号回退。
long airi_clock_get(unsigned clock, struct airi_time_value *value); // 38号调用，当前只支持单调时钟1。
#endif // 结束包含保护。
```

</details>

在 `labs/airios/libc/syscall.c` 末尾追加以下内容，前章已有部分保留。

<!-- append: labs/airios/libc/syscall.c -->
```c

long airi_clock_get(unsigned clock, struct airi_time_value *value) { // 用户库不先解引用可能非法的输出。
    return call3(38, clock, (uintptr_t)value, 0); // 内核一次形成并复制16字节值，负错误原样交回。
}
```

<details>
<summary>labs/airios/libc/posix.c 的完整终点</summary>

<!-- write: labs/airios/libc/posix.c -->
```c
#include <airi_libc.h> // 原始调用仍返回内核负错误，不自行修改errno。
#include <errno.h> // 可修改错误左值由本地槽提供。
#include <fcntl.h> // 源码标志进入原始ABI前显式转换。
#include <unistd.h> // 导出当前已提供的POSIX风格类型与函数。
#include <time.h> // 标准源码结构对应固定时间输出记录。
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

_Static_assert(sizeof(struct timespec) == sizeof(struct airi_time_value), "timespec ABI size"); // 固定目标两个8字节字段。
_Static_assert(offsetof(struct timespec, tv_nsec) == offsetof(struct airi_time_value, nanoseconds), "timespec nanosecond offset"); // 正数秒与纳秒的布局一致。
int clock_gettime(clockid_t clock, struct timespec *value) { // 结果仍由原有errno转换，不写用户局部替代坏输出。
    return (int)result(airi_clock_get((unsigned)clock, (struct airi_time_value *)value)); // 当前两个字段始终为可表示的非负数。
}
```

</details>

<a id="clock-observe"></a>
## 在真正阻塞期间比较前后时间

posix-probe在普通文件操作前查询单调时间，使用既有airi_sleep(3)真正阻塞，再查询。两次应非倒退，名义差值至少30ms，纳秒字段为10ms的整数倍；不会用宿主sleep或打印预期数字代替计数。

差值计算将秒差换成纳秒，再加结束纳秒、减开始纳秒；纳秒部分跨秒时使用无符号模运算，最终仍得到当前有界观察中的正间隔。未实现的REALTIME查询使用独立哨兵，失败后两个字段保持71和83；坏高地址由内核拒绝后同一用户继续。成功exec的reset分支仍先检查errno初值并退出，不重复该睡眠观察。

更新用户程序和shell_test，后者要求新增成功标记来自真实libc命令，再继续定位、I/O和错误槽回归。

在已有用户头中接入 time.h。

<!-- replace: labs/airios/posix-probe.c -->
<!-- before -->
```c
#include <airi_libc.h> // 原始进程控制暂与POSIX风格文件接口共存，原始调用不设置errno。
#include <errno.h> // 错误变量是当前TLS的可修改左值。
#include <fcntl.h> // open源码标志由用户库转换。
#include <unistd.h> // 有符号字节数、显式偏移与标准文件调用名。
int main(int argc, char **argv) { // 同一ELF另以libc-reset观察exec后的错误初值。
    if (errno) // .tbss中的错误槽必须在main前初始化为0。
```
<!-- after -->
```c
#include <airi_libc.h> // 原始进程控制暂与POSIX风格文件接口共存，原始调用不设置errno。
#include <errno.h> // 错误变量是当前TLS的可修改左值。
#include <fcntl.h> // open源码标志由用户库转换。
#include <time.h> // 单调时间不是日历日期，超时以此比较。
#include <unistd.h> // 有符号字节数、显式偏移与标准文件调用名。
int main(int argc, char **argv) { // 同一ELF另以libc-reset观察exec后的错误初值。
    if (errno) // .tbss中的错误槽必须在main前初始化为0。
```

reset 分支仍先检查新错误槽；只有正常分支继续到这组时钟查询、三 tick 睡眠和哨兵检查。

<!-- replace: labs/airios/posix-probe.c -->
<!-- before -->
```c
        airi_puts("[LIBC_EXEC_RESET_OK] errno=zero cloexec=closed\n"); // 只在两项实际观察完成后报告。
        _exit(0); // 标准名字调用用户库，仍由原内核退出当前身份。
    }
    unsigned char input[300], output[300]; // 请求故意超过当前256字节系统调用副本。
    for (unsigned i = 0; i < sizeof(input); ++i) // 逐位置可辨识，不能只比较固定重复值。
        input[i] = (unsigned char)(i * 7u + 3u); // 字节内容独立于错误返回和偏移。
```
<!-- after -->
```c
        airi_puts("[LIBC_EXEC_RESET_OK] errno=zero cloexec=closed\n"); // 只在两项实际观察完成后报告。
        _exit(0); // 标准名字调用用户库，仍由原内核退出当前身份。
    }
    struct timespec before, after, guard = {71, 83}; // 真实时间与失败输出哨兵分别持有。
    if (clock_gettime(CLOCK_REALTIME, &guard) != -1 || errno != EINVAL || guard.tv_sec != 71 || guard.tv_nsec != 83 || clock_gettime(CLOCK_MONOTONIC, (void *)(uintptr_t)0xffff800000000000ull) != -1 || errno != EFAULT) // 未实现墙钟和坏输出都不能改有效对象。
        return 20; // 用户程序应继续而非因库解引用坏指针退出。
    if (clock_gettime(CLOCK_MONOTONIC, &before) || before.tv_sec < 0 || before.tv_nsec < 0 || before.tv_nsec >= 1000000000 || before.tv_nsec % 10000000 || airi_sleep(3) || clock_gettime(CLOCK_MONOTONIC, &after)) // 原始sleep真实阻塞至少三个PIT tick。
        return 21; // 不把宿主等待当作guest时钟推进。
    uint64_t elapsed = (uint64_t)(after.tv_sec - before.tv_sec) * 1000000000ull + (uint64_t)after.tv_nsec - (uint64_t)before.tv_nsec; // 无符号算术处理跨秒的纳秒回绕，当前观察差距有界。
    if (after.tv_sec < before.tv_sec || (after.tv_sec == before.tv_sec && after.tv_nsec < before.tv_nsec) || elapsed < 30000000ull) // 不能倒退且真实阻塞窗口应至少30ms名义时间。
        return 22; // 实际PIT分辨率不等于真实墙钟精度。
    airi_puts("[LIBC_CLOCK_OK] monotonic=1 quantum_ns=10000000 sleep=blocked invalid=unchanged\n"); // 当前只承诺名义100Hz单调时间。
    unsigned char input[300], output[300]; // 请求故意超过当前256字节系统调用副本。
    for (unsigned i = 0; i < sizeof(input); ++i) // 逐位置可辨识，不能只比较固定重复值。
        input[i] = (unsigned char)(i * 7u + 3u); // 字节内容独立于错误返回和偏移。
```

在本次 libc_output 中加入 LIBC_CLOCK_OK，沿同一命令继续原定位、I/O 和错误槽检查。

<!-- replace: scripts/shell_test.py -->
<!-- before -->
```python
                assert 'ELF TLS size or alignment is unsupported' in ctls_output # 超限真实样本必须在装载器拒绝。
                offset=len(text); wire.sendall(b'libc\n'); text=wait('[SHELL_COMMAND_DONE] name=libc code=0',offset) # 用户库转换真实内核结果，errno来自编译器TLS。
                libc_output=text[offset:] # 不使用旧映像或启动期输出。
                assert '[LIBC_SEEK_OK] set=1 cur=1 end=1 signed=checked hole=zero failure=unchanged' in libc_output # 相对定位、极值失败和真实零洞均由guest观察。
                assert '[LIBC_IO_OK] short=256 pipe=64 eof=zero offset=preserved bad_buffer=unchanged' in libc_output # 正常、短操作和坏缓冲区均由guest验证。
                assert '[LIBC_ERRNO_OK] initial=0 fork=copied preempt=private success=preserved exec=reset' in libc_output and '[LIBC_EXEC_RESET_OK] errno=zero cloexec=closed' in libc_output # 同一身份成功exec重置错误并关闭表项。
```
<!-- after -->
```python
                assert 'ELF TLS size or alignment is unsupported' in ctls_output # 超限真实样本必须在装载器拒绝。
                offset=len(text); wire.sendall(b'libc\n'); text=wait('[SHELL_COMMAND_DONE] name=libc code=0',offset) # 用户库转换真实内核结果，errno来自编译器TLS。
                libc_output=text[offset:] # 不使用旧映像或启动期输出。
                assert '[LIBC_CLOCK_OK] monotonic=1 quantum_ns=10000000 sleep=blocked invalid=unchanged' in libc_output # 真实PIT窗口、输出权限和墙钟拒绝均由guest核对。
                assert '[LIBC_SEEK_OK] set=1 cur=1 end=1 signed=checked hole=zero failure=unchanged' in libc_output # 相对定位、极值失败和真实零洞均由guest观察。
                assert '[LIBC_IO_OK] short=256 pipe=64 eof=zero offset=preserved bad_buffer=unchanged' in libc_output # 正常、短操作和坏缓冲区均由guest验证。
                assert '[LIBC_ERRNO_OK] initial=0 fork=copied preempt=private success=preserved exec=reset' in libc_output and '[LIBC_EXEC_RESET_OK] errno=zero cloexec=closed' in libc_output # 同一身份成功exec重置错误并关闭表项。
```

完成全部修改后运行：

<!-- run -->
```sh
./airios build # 新38号调用只提供受检查的名义单调时间。
./airios test # 实际tick、sleep、错误输出与已有所有内核/服务路径一同运行。
```

Mac默认仍为旧Python时给命令加PATH=/opt/homebrew/bin:$PATH。交互./airios run，输入run sh后输入libc，应看到：

```text
[LIBC_CLOCK_OK] monotonic=1 quantum_ns=10000000 sleep=blocked invalid=unchanged
```

本节没有clock_settime、clock_getres、nanosleep、RTC日期或NTP；现有sleep仍以tick为参数。后续线程的超时等待要明确使用哪个时钟，网络证书校验还需要可信墙钟，不能直接使用这里的开机计数。Linux时钟与真实板精度仍需各自实测。

