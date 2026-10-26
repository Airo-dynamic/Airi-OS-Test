# K44 · 在用户库将负错误变成 errno，保留实际短操作

从 K43 继续。编译器生成的本地变量已有真实运行环境，可以给用户库保存当前执行者的错误状态。已有 airi_read 等原始封装直接返回−9、−14；普通 POSIX 风格源码则通常检查返回−1，再读 errno。本节把转换放进用户库，内核 ABI 和既有原始封装保持。

沿 `sh → libc → 文件与管道 → fork → 各自错误 → exec libc-reset → wait` 实际运行。新增 errno、open/read/write/pread/pwrite/close/dup/pipe、有限 lseek、getpid 和 _exit。它是明确列出的源码接口子集，没有完整 POSIX、stdio、malloc、信号、线程或持久文件系统。

这份库显式链接到需要它的用户程序。早期手工FS程序不带errno，4KiB极限TLS实验也不额外混入错误槽；内核仍不链接用户库或宿主 libc。所有文件完成后再运行，图形无前置。

<a id="errno-result"></a>
## 先判断返回值，再读取当前错误

原始调用 `airi_read(bad_fd,...)` 返回−9。新 `read` 收到这个值，在用户自己的本地槽写9，再向 C 调用方返回−1。实际传输数大于等于0时原样返回：例如3是短读，0是EOF，不能因前一次留下的 errno=9 就说本次失败。

[errno手册](https://man7.org/linux/man-pages/man3/errno.3.html)说明它可以是一个可修改的 int 左值宏，并具有线程局部语义；成功调用的 errno 不用来判断本次失败。本库选择成功时完全不改它，这便于观察旧值为何可能仍在；不能将这种实现选择扩展成所有标准库成功函数都承诺保留 errno。

刚才的9需要留到 read 返回以后，供调用者再次读取；它不能放在包装函数的局部变量里，也不能由所有执行者共用一个 int。这里将 error_number 声明为 `_Thread_local`，K43 已在进入 main 前建立它的 .tbss 零初值副本。

errno 宏展开为 `(*__airi_errno_location())`。定位函数先取得当前本地槽地址，外层解引用才成为可读、可赋值的 int；因此 `errno=EBUSY` 和错误包装写入的是同一个当前槽。现在每进程只有一个线程，观察的是不同进程副本；以后同一地址空间的多个线程仍须各自分配 TLS，不能共用一份 FS 基址。

错误名是源码符号。头文件数值对应本内核已有负错误约定，不承诺别的系统或架构相同。EINTR 名字先提供，但当前无信号，不会为了演示而伪造中断错误；EPIPE 也不会自动产生当前尚未实现的 SIGPIPE。

先新建 sys/types.h，定义本目标的有符号字节数、64位文件偏移、PID及mode类型。ssize_t 允许返回−1，size_t 则继续表示非负容量；off_t 先保持有符号，库必须在转成原始无符号参数前拒绝负偏移。

<!-- write: labs/airios/include/sys/types.h -->
```c
#ifndef AIRI_SYS_TYPES_H // 本用户运行库的类型，不读取宿主sys/types.h。
#define AIRI_SYS_TYPES_H // 只声明一次。
#include <stddef.h> // size_t来自目标编译器的freestanding头。
#include <stdint.h> // 固定宽度整数仍由目标工具链定义。
typedef long ssize_t; // 当前x86-64有符号字节数，负数用于错误。
typedef int64_t off_t; // 当前文件定位使用有符号64位偏移。
typedef long pid_t; // 能容纳内核完整32位正PID，不把槽号当身份。
typedef unsigned mode_t; // open的源码参数类型；当前RAM文件没有权限位模型。
#endif // 结束包含保护。
```

新建 errno.h，列出当前会遇到的错误符号与定位函数。`__cplusplus` 由 C++ 编译器预定义；extern "C" 让 C++ 源以后以同一 C 函数链接名调用运行库，当前 C11 编译跳过这层包装。它只处理链接名，不代表已提供 C++ runtime 或异常支持。

<!-- write: labs/airios/include/errno.h -->
```c
#ifndef AIRI_ERRNO_H // 错误状态由当前用户库拥有，不引用宿主运行库。
#define AIRI_ERRNO_H // 包含保护。
#define EPERM 1 // 当前操作不允许。
#define ENOENT 2 // 名称不存在。
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
#define ENOSYS 38 // 当前未实现的操作。
#define EOVERFLOW 75 // 不能表示请求结果。
#ifdef __cplusplus // 同一头以后可被C++源码使用，函数仍用C链接名。
extern "C" { // 防止把运行库入口编码成C++重载符号。
#endif // C11跳过链接语言包装。
int *__airi_errno_location(void); // 地址属于当前静态TLS副本，不能作为另一个线程的错误槽。
#ifdef __cplusplus // 结束C链接声明范围。
} // 只包围函数声明，不改变错误宏。
#endif // 结束语言差异。
#define errno (*__airi_errno_location()) // 可修改的int左值；每次按当前执行者定位。
#endif // 结束包含保护。
```

<a id="posix-flags"></a>
## 源码常量和内核标志分别解释

O_RDONLY、O_WRONLY、O_RDWR 是访问模式的三个枚举值，其中只读是0。不能用 `if (flags & O_RDONLY)` 判断可读；先以 O_ACCMODE 提取低两位，再映射到原来的 AIRI_READ/AIRI_WRITE。O_CREAT、O_TRUNC 和 O_CLOEXEC 分别转换到原始 ABI 位，未知位拒绝为 EINVAL，不静默忽略。

fcntl.h 的常量只供编译到本目标的源码使用，既不传给 Linux，也不让 Linux 二进制直接运行。open 保留可选 mode 参数的函数形状，但当前 RAM 文件没有权限位模型，0600 不产生访问控制；节点是否只读仍由内核既有规则决定。这里未声明 append、目录、非阻塞控制或通用fcntl功能。

新建 fcntl.h。其余描述符、定位和进程查询声明放在 unistd.h，支持范围在声明处保留。SEEK_CUR/END 常量虽存在，当前调用会明确返回 ENOSYS；_exit 只直接退出，不运行stdio刷新或析构。

<!-- write: labs/airios/include/fcntl.h -->
```c
#ifndef AIRI_FCNTL_H // 当前只声明已提供的open子集。
#define AIRI_FCNTL_H // 包含保护。
#include <sys/types.h> // mode_t与目标用户类型。
#define O_RDONLY 0 // 访问模式是低两位枚举，不是独立可叠加位。
#define O_WRONLY 1 // 只写。
#define O_RDWR 2 // 读写。
#define O_ACCMODE 3 // 提取访问模式；值3无效。
#define O_CREAT 0x100 // 本运行库源码常量，进入内核前转换，不是Linux ABI。
#define O_TRUNC 0x200 // 已有文件清空需要写权限。
#define O_CLOEXEC 0x400 // 成功exec关闭当前FD表项。
#ifdef __cplusplus // C++调用相同静态运行库。
extern "C" { // 使用C函数名。
#endif // C11无需额外包装。
int open(const char *path, int flags, ...); // O_CREAT允许mode参数；当前RAM模型不保存权限位。
#ifdef __cplusplus // 结束声明语言范围。
} // 仅open入口使用C链接。
#endif // 结束语言差异。
#endif // 结束包含保护。
```

<!-- write: labs/airios/include/unistd.h -->
```c
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
```

<a id="posix-prefix"></a>
## 包装一次操作，不替调用方隐藏部分进展

当前内核一次最多复制256字节，原始接口对更长请求返回−22。用户库可以把一个可表示的长请求缩到256后提交一次，把真实结果交回调用方；[read手册](https://man7.org/linux/man-pages/man2/read.2.html)也区分实际完成数、EOF和错误。请求大于 LONG_MAX 时，本实现先拒绝 EINVAL，避免返回类型无法表达请求范围。

例如300字节输入先返回256，调用方自己计算余下44字节。库不能偷偷循环写满：如果中途阻塞、EAGAIN或出现错误，调用者还需要知道已处理前缀、剩余缓冲区和下一步等待条件。管道只有64字节时，write(100)仍可返回64，不能把它误改成−1或100。

pread/pwrite同样只尝试一次，并保留明确偏移；它们不改共享文件位置，不用seek加read模拟。pipe直接把调用方的两个整数输出地址交内核检查，不能先在库局部数组创建端点，再写坏用户地址造成程序故障和资源泄漏。int/unsigned当前宽度一致，由静态断言固定。

新建 libc/posix.c。result负责负错误转换，invalid处理库本地参数错误，prefix只决定一次提交的长度；成功分支均不改错误槽。原始 airi_ 包装仍不设置errno，应用可以明确选择其中一层。

<!-- write: labs/airios/libc/posix.c -->
```c
#include <airi_libc.h> // 原始调用仍返回内核负错误，不自行修改errno。
#include <errno.h> // 可修改错误左值由本地槽提供。
#include <fcntl.h> // 源码标志进入原始ABI前显式转换。
#include <unistd.h> // 导出当前已提供的POSIX风格类型与函数。
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
off_t lseek(int fd, off_t offset, int whence) { // 尚无文件长度/当前位置查询，不虚构完整定位能力。
    if (offset < 0 || (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END)) // 先拒绝明显非法输入。
        return invalid(EINVAL); // 未修改共享位置。
    if (whence != SEEK_SET) // 常量存在不表示操作已经实现。
        return invalid(ENOSYS); // 调用方可明确识别当前缺口。
    return result(airi_seek((unsigned)fd, (uint64_t)offset)); // 返回实际绝对位置。
}
pid_t getpid(void) { // 当前原始getpid不会失败，仍复用一致转换。
    return result(airi_getpid()); // 使用可容纳完整内核PID的有符号类型。
}
void _exit(int code) { // 直接退出，不提供完整C标准exit或析构流程。
    airi_exit((unsigned)code); // 原内核按低八位编码普通退出值。
}
```

<a id="posix-workload"></a>
## 在同一程序中比较短读、EOF、EAGAIN与错误

新建 posix-probe.c。先创建RAM文件/abi，留下旧 errno=EBUSY，分两次写300字节，再分两次读回全部内容，检查EOF0没有被旧错误值掩盖。显式偏移读写之后再做普通read，确认共享位置没移动；坏输出指针返回EFAULT后，下一次read仍从原位置继续。

随后验证负偏移、未实现相对定位、关闭后EBADF、缺名称ENOENT、访问枚举3拒绝。dup别名应共享偏移，关闭原FD后别名继续；只读/seed仍不能用写模式打开。管道保留64字节短操作与EOF；非阻塞端点暂用原始接口建立，再由标准名字read观察−1/EAGAIN。坏pipe输出必须在内核创建资源前拒绝。

最后把errno设为EBUSY再fork，父子先核对继承，随后分别改为ENOENT和EBADF，跨真实时钟保持不同值。孩子打开CLOEXEC文件后成功exec同一ELF的新名字，先观察errno重建为0，再确认该表项已经关闭。

本会话进入libc时，shell独占的输入FD0已因CLOEXEC撤销，只保留1和2，所以后一次open复用最低空槽0。FD0并不在所有程序中永远代表标准输入；它是当前进程表中的整数。reset据此检查0已关闭。父仍在自己的本地错误槽看到ENOENT，成功getpid也不清它。

<!-- write: labs/airios/posix-probe.c -->
```c
#include <airi_libc.h> // 原始进程控制暂与POSIX风格文件接口共存，原始调用不设置errno。
#include <errno.h> // 错误变量是当前TLS的可修改左值。
#include <fcntl.h> // open源码标志由用户库转换。
#include <unistd.h> // 有符号字节数、显式偏移与标准文件调用名。
int main(int argc, char **argv) { // 同一ELF另以libc-reset观察exec后的错误初值。
    if (errno) // .tbss中的错误槽必须在main前初始化为0。
        return 227; // 不能继承上一个映像的错误值。
    if (argc == 1 && strlen(argv[0]) > 4 && argv[0][4] == '-') { // 孩子成功替换到libc-reset。
        char byte; // 测试成功exec关闭之前的CLOEXEC表项。
        if (read(STDIN_FILENO, &byte, 1) != -1 || errno != EBADF) // 默认错误为0已先核对，失败后才应变为9。
            return 228; // 本会话复用的FD0不得遗留。
        airi_puts("[LIBC_EXEC_RESET_OK] errno=zero cloexec=closed\n"); // 只在两项实际观察完成后报告。
        _exit(0); // 标准名字调用用户库，仍由原内核退出当前身份。
    }
    unsigned char input[300], output[300]; // 请求故意超过当前256字节系统调用副本。
    for (unsigned i = 0; i < sizeof(input); ++i) // 逐位置可辨识，不能只比较固定重复值。
        input[i] = (unsigned char)(i * 7u + 3u); // 字节内容独立于错误返回和偏移。
    int fd = open("/abi", O_RDWR | O_CREAT | O_TRUNC, (mode_t)0600); // 当前只使用mode的源码形状，RAM尚无权限模型。
    if (fd < 0) // 必须先看返回值，不能只看errno。
        return 229; // 明确资源或路径错误。
    errno = EBUSY; // 先留一项旧错误，后续成功不应该靠清零来表示成功。
    if (write(fd, input, sizeof(input)) != 256 || errno != EBUSY || write(fd, input + 256, 44) != 44) // 库只提交一次前缀，调用方明确消费剩余。
        return 230; // 不接受偷偷写满并伪装返回300。
    if (lseek(fd, 0, SEEK_SET) || read(fd, output, sizeof(output)) != 256 || read(fd, output + 256, 44) != 44 || memcmp(input, output, sizeof(input))) // 实际短读次数与内容都符合已写前缀。
        return 231; // 不以返回值正确代替数据正确。
    if (read(fd, output, 1) != 0 || errno != EBUSY) // EOF是成功0，旧errno仍无本次错误含义。
        return 232; // 不能把旧EBUSY误判为EOF失败。
    if (lseek(fd, 0, SEEK_SET) || pread(fd, output, 5, 10) != 5 || memcmp(output, input + 10, 5) || read(fd, output, 1) != 1 || output[0] != input[0]) // pread不移动共享位置。
        return 233; // 后一次read应仍从offset0开始。
    if (pwrite(fd, "XYZ", 3, 5) != 3 || read(fd, output, 1) != 1 || output[0] != input[1] || pread(fd, output, 3, 5) != 3 || memcmp(output, "XYZ", 3)) // pwrite同样保持位置且内容真实可见。
        return 234; // 显式偏移不是先seek再write的别名。
    if (read(fd, (void *)(uintptr_t)0xffff800000000000ull, 1) != -1 || errno != EFAULT || read(fd, output, 1) != 1 || output[0] != input[2] || errno != EFAULT) // 坏输出在消费偏移前拒绝，后一次成功保留旧错误。
        return 235; // 不让包装破坏原内核错误原子性。
    if (pread(fd, output, 1, -1) != -1 || errno != EINVAL || lseek(fd, 0, SEEK_CUR) != -1 || errno != ENOSYS) // 本地负参数和当前未实现操作明确区分。
        return 236; // 不虚构完整POSIX定位。
    if (close(fd) || close(fd) != -1 || errno != EBADF || open("/missing", O_RDONLY) != -1 || errno != ENOENT || open("/seed", O_ACCMODE) != -1 || errno != EINVAL) // 真实内核错误和库参数错误都转换为-1+正errno。
        return 237; // 成功结果不能被旧errno遮蔽。
    fd = open("/seed", O_RDONLY); // 独立open从offset0开始。
    if (fd < 0) // 先检查返回值，不读旧errno猜结果。
        return 249; // 保留具体失败位置。
    int alias = dup(fd); // 新表项共享打开对象及偏移。
    if (alias < 0 || read(fd, output, 1) != 1 || output[0] != 'a' || read(alias, output, 1) != 1 || output[0] != 'b' || close(fd) || read(alias, output, 1) != 1 || output[0] != 'c' || close(alias)) // 关原FD不能破坏别名持有的对象。
        return 250; // 标准包装不应复制偏移或重复关闭对象。
    if (open("/seed", O_WRONLY) != -1 || errno != EACCES) // 只读初始节点仍由内核拒绝写模式。
        return 251; // 包装不能因标志映射错误而提升权限。
    int ends[2]; // 同一用户地址直接给内核验证，库不借局部输出副本。
    if (pipe(ends) || write(ends[1], input, 100) != 64 || read(ends[0], output, 100) != 64 || memcmp(input, output, 64)) // 当前管道只有64字节，短操作要保持。
        return 238; // 不把短流操作自动扩成循环等待。
    if (close(ends[1]) || read(ends[0], output, 1) != 0 || close(ends[0])) // 最后写端关闭后才是EOF。
        return 239; // 原FD引用和流结束语义仍在内核。
    unsigned raw[2]; // 非阻塞模式尚无标准fcntl，明确借原始实验接口建立。
    if (airi_pipe(raw, 1) || read((int)raw[0], output, 1) != -1 || errno != EAGAIN || close((int)raw[0]) || close((int)raw[1])) // 暂无字节是错误-1/EAGAIN，不是成功EOF0。
        return 240; // 不混淆两个结果。
    if (pipe((int *)(uintptr_t)0xffff800000000000ull) != -1 || errno != EFAULT) // 坏输出必须在创建FD前拒绝。
        return 241; // 不能在用户库里先创建局部管道再因写坏指针而崩溃。
    airi_puts("[LIBC_IO_OK] short=256 pipe=64 eof=zero offset=preserved bad_buffer=unchanged\n"); // 有界文件与错误路径都已实际运行。
    errno = EBUSY; // fork应复制当前错误值，并在此后各自改变。
    long child = airi_fork(); // 原始进程接口仍保留负错误，不自动改errno。
    if (child < 0 || errno != EBUSY) // 父子都须先继承16。
        return 242; // 不重新从模板初始化fork。
    if (child) { // 父制造一个名称错误。
        if (open("/missing", O_RDONLY) != -1 || errno != ENOENT) // 当前本地槽改为2。
            return 243; // 另一进程错误不能混入。
    } else if (read(-1, output, 1) != -1 || errno != EBADF) // 子将同一符号改为9。
        return 244; // 完整有符号FD转换仍应得到正确错误。
    int expected = child ? ENOENT : EBADF; // 两个真实身份的不同值。
    uint64_t start = airi_ticks(); // 原始查询不会改errno。
    do { // 普通用户纯计算，让PIT实际切换。
        for (unsigned i = 0; i < 1000000; ++i) // errno定位函数在独立对象，不由本main内联成常量。
            if (errno != expected) // 真实本地错误槽不能串到另一个进程。
                return 245; // 任一次错误即结束。
    } while (airi_ticks() - start < 20); // 二十个真实tick，非宿主注入。
    if (!child) { // shell的输入已在进入libc时CLOEXEC，当前最低空FD是0。
        if (open("/seed", O_RDONLY | O_CLOEXEC) != STDIN_FILENO || errno != EBADF) // 成功open也不会清旧错误。
            return 246; // 后续reset可以准确观察此表项。
        airi_exec("libc-reset"); // 错误槽来自新ELF模板0，PID仍不变。
        return 247; // 成功不返回。
    }
    int status = 1; // 父按实际孩子身份取得新程序结果。
    if (airi_waitpid(child, &status) != child || status || errno != ENOENT || getpid() != airi_getpid() || errno != ENOENT) // 原始wait与成功标准getpid均不清父错误。
        return 248; // 控制路径不能覆盖本地错误环境。
    airi_puts("[LIBC_ERRNO_OK] initial=0 fork=copied preempt=private success=preserved exec=reset\n"); // 全路径结束后才发布。
    return 0; // shell持续服务，会话最终核对页/FD/堆恢复。
}
```

<a id="posix-link"></a>
## 显式选择运行库，不把它偷偷带进原始实验

为 user.c 目录添加 libc/libc-reset，并在 images.S 嵌入同一份真实ELF。两者按 argv[0] 选择首次运行或默认错误槽检查，成功exec仍从新入口开始，PID保持。内核服务、系统调用号和文件对象机制均不因标准名字包装改变。

接下来按现有 `kernel/process/user.c` 中的位置逐组替换，未列出的前章内容保持。所有映像、用户库与构建接入完成后再运行。

在已有 ctls 映像声明旁加入用户库观察 ELF 的边界。

本章每组替换的第一块只用于定位旧文本，敲入第二块替换它，不把两块同时加入文件。各组按正文顺序完成。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
extern const uint8_t airi_user_rsp_bad_start[], airi_user_rsp_bad_end[]; // 不规范用户返回栈。
extern const uint8_t airi_hello_elf_start[], airi_hello_elf_end[]; // 既有独立用户C程序。
extern const uint8_t airi_process_elf_start[], airi_process_elf_end[]; // 新的fork/wait和抢占工作负载。
extern const uint8_t airi_ctls_elf_start[], airi_ctls_elf_end[]; // 编译器_Thread_local与静态模板。
extern const uint8_t airi_ctls_char_elf_start[], airi_ctls_char_elf_end[]; // 单字节小对齐模板。
extern const uint8_t airi_ctls_page_elf_start[], airi_ctls_page_elf_end[]; // 最大纯零模板与独立TCB页。
```
<!-- after -->
```c
extern const uint8_t airi_user_rsp_bad_start[], airi_user_rsp_bad_end[]; // 不规范用户返回栈。
extern const uint8_t airi_hello_elf_start[], airi_hello_elf_end[]; // 既有独立用户C程序。
extern const uint8_t airi_process_elf_start[], airi_process_elf_end[]; // 新的fork/wait和抢占工作负载。
extern const uint8_t airi_posix_elf_start[], airi_posix_elf_end[]; // 显式链接errno与POSIX风格文件包装。
extern const uint8_t airi_ctls_elf_start[], airi_ctls_elf_end[]; // 编译器_Thread_local与静态模板。
extern const uint8_t airi_ctls_char_elf_start[], airi_ctls_char_elf_end[]; // 单字节小对齐模板。
extern const uint8_t airi_ctls_page_elf_start[], airi_ctls_page_elf_end[]; // 最大纯零模板与独立TCB页。
```

在同一目录加入 libc 与 libc-reset，二者的映像范围相同，argv[0] 区分两条用户分支。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
static const struct executable executables[] = { // 每份映像字节由内核只读段长期持有。
    {"sh", airi_shell_elf_start, airi_shell_elf_end}, // 独占串口输入的可选用户程序。
    {"init", airi_init_elf_start, airi_init_elf_end}, // 管理前台shell的常驻用户进程。
    {"ctls", airi_ctls_elf_start, airi_ctls_elf_end}, // 自动建立静态TLS后运行普通C。
    {"ctls-reset", airi_ctls_elf_start, airi_ctls_elf_end}, // 同一映像成功exec后观察新模板。
    {"ctls-char", airi_ctls_char_elf_start, airi_ctls_char_elf_end}, // 小模板的负偏移与TP对齐。
```
<!-- after -->
```c
static const struct executable executables[] = { // 每份映像字节由内核只读段长期持有。
    {"sh", airi_shell_elf_start, airi_shell_elf_end}, // 独占串口输入的可选用户程序。
    {"init", airi_init_elf_start, airi_init_elf_end}, // 管理前台shell的常驻用户进程。
    {"libc", airi_posix_elf_start, airi_posix_elf_end}, // 用户库错误与文件结果转换。
    {"libc-reset", airi_posix_elf_start, airi_posix_elf_end}, // 同一ELF新错误槽和CLOEXEC观察。
    {"ctls", airi_ctls_elf_start, airi_ctls_elf_end}, // 自动建立静态TLS后运行普通C。
    {"ctls-reset", airi_ctls_elf_start, airi_ctls_elf_end}, // 同一映像成功exec后观察新模板。
    {"ctls-char", airi_ctls_char_elf_start, airi_ctls_char_elf_end}, // 小模板的负偏移与TP对齐。
```

在 `labs/airios/images.S` 末尾追加以下内容，前章已有部分保留。

<!-- append: labs/airios/images.S -->
```asm

.balign 8 /* POSIX风格包装仍在用户ELF中，不进内核ABI。 */
.global airi_posix_elf_start, airi_posix_elf_end /* 普通和reset目录项共用映像字节。 */
airi_posix_elf_start: /* 含errno静态TLS的新ELF。 */
.incbin AIRI_POSIX_ELF /* 只有该显式目标链接posix.o。 */
airi_posix_elf_end: /* 完整用户文件边界。 */
```

更新 Makefile，单独编译posix.o，仅链接进posix-probe.elf。USER_COMMON仍只含原始入口、字节函数和系统调用封装。因此需要errno的程序会有PT_TLS并在main前建立错误槽；没有链接这份库的手工FS程序仍从0开始，ctls-page的4096字节极限模板也不被额外4字节推到超限。

这些头文件位于用户include搜索目录，内核和宿主检查没有加入该路径。标准函数名由本项目源码提供，不由Mac或Linux libc补齐；缺少实现时应出现明确未定义符号，不能因为名字标准就默认可用。

在 `Makefile` 末尾追加以下内容，前章已有部分保留。

<!-- append: Makefile -->
```make

# POSIX风格包装是显式用户库，不让早期原始FS和4KiB模板实验强行加入errno。
$(OUT)/user/posix.o: labs/airios/libc/posix.c Makefile
# 使用同一用户目标与自动头依赖，拒绝宿主头文件实现混入。
	@mkdir -p $(dir $@)
# posix.c的_Thread_local错误槽由静态模板装载，库不自己设置FS。
	$(CC) $(USER_FLAGS) -Ilabs/airios/include -c $< -o $@
# 此程序单独选择posix.o，USER_COMMON仍只包含原始入口和字节/系统调用库。
$(OUT)/user/posix-probe.elf: $(USER_COMMON) $(OUT)/user/posix.o $(OUT)/user/posix-probe.o labs/airios/program.ld
# 无宿主libc，所有标准名字由本项目用户库提供。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/posix.o $(OUT)/user/posix-probe.o -o $@
# 包装等待当前配置的完整用户ELF。
$(OUT)/labs/airios/images.o: $(OUT)/user/posix-probe.elf
# 该路径宏不改变内核调用号或符号。
$(OUT)/labs/airios/images.o: CPPFLAGS += -DAIRI_POSIX_ELF='"$(OUT)/user/posix-probe.elf"'
```

shell_test在同一用户shell中输入libc，要求实际短I/O、错误槽生命周期和CLOEXEC标记来自本次命令。原ctls、手工FS和浮点程序仍继续运行，避免可选运行库破坏旧接口的边界。

在 ctls 完成后运行 libc，分别核对短I/O与 errno/exec 记录，仍只消费本次命令后的输出。

<!-- replace: scripts/shell_test.py -->
<!-- before -->
```python
                assert '[CTLS_CHILD_OK]' in ctls_output and '[CTLS_EXEC_RESET_OK]' in ctls_output and '[CTLS_CHAR_OK] bytes=1 alignment=1 tp=16 value=9' in ctls_output # 大小对齐与成功exec均实际运行。
                assert '[CTLS_PAGE_OK] bytes=4096 initialized=0 pages=2 tcb=second zeroed=all' in ctls_output # 最大纯零模板实际访问第二页TCB。
                assert 'ELF TLS size or alignment is unsupported' in ctls_output # 超限真实样本必须在装载器拒绝。
                offset=len(text); wire.sendall(b'orphans\n'); text=wait('[SHELL_COMMAND_DONE] name=orphans code=0',offset) # 同一用户shell持续服务，不靠返回monitor收拢孙进程。
                orphan_output=text[offset:] # 只核对本条命令的身份记录。
                assert '[ORPHAN_USER_OK] rounds=24 live=12 zombie=12 session=alive' in orphan_output # 超过八槽容量仍能连续fork。
```
<!-- after -->
```python
                assert '[CTLS_CHILD_OK]' in ctls_output and '[CTLS_EXEC_RESET_OK]' in ctls_output and '[CTLS_CHAR_OK] bytes=1 alignment=1 tp=16 value=9' in ctls_output # 大小对齐与成功exec均实际运行。
                assert '[CTLS_PAGE_OK] bytes=4096 initialized=0 pages=2 tcb=second zeroed=all' in ctls_output # 最大纯零模板实际访问第二页TCB。
                assert 'ELF TLS size or alignment is unsupported' in ctls_output # 超限真实样本必须在装载器拒绝。
                offset=len(text); wire.sendall(b'libc\n'); text=wait('[SHELL_COMMAND_DONE] name=libc code=0',offset) # 用户库转换真实内核结果，errno来自编译器TLS。
                libc_output=text[offset:] # 不使用旧映像或启动期输出。
                assert '[LIBC_IO_OK] short=256 pipe=64 eof=zero offset=preserved bad_buffer=unchanged' in libc_output # 正常、短操作和坏缓冲区均由guest验证。
                assert '[LIBC_ERRNO_OK] initial=0 fork=copied preempt=private success=preserved exec=reset' in libc_output and '[LIBC_EXEC_RESET_OK] errno=zero cloexec=closed' in libc_output # 同一身份成功exec重置错误并关闭表项。
                offset=len(text); wire.sendall(b'orphans\n'); text=wait('[SHELL_COMMAND_DONE] name=orphans code=0',offset) # 同一用户shell持续服务，不靠返回monitor收拢孙进程。
                orphan_output=text[offset:] # 只核对本条命令的身份记录。
                assert '[ORPHAN_USER_OK] rounds=24 live=12 zombie=12 session=alive' in orphan_output # 超过八槽容量仍能连续fork。
```

完整 shell 序列结束后，结果记录加入 posix_errno_io_subset。

<!-- replace: scripts/shell_test.py -->
<!-- before -->
```python
                assert '[USER_SHELL_EXIT]' in text[offset:] and 'program sh exited 0' in text[offset:] # 真实收拢页/FD/静态池由launch核对。
                offset=len(text); wire.sendall(b'echo after-shell\n'); text=wait('[CONSOLE_UPDATE] view=0 commands=2 page=0',offset) # 回到内核后输入仍完整。
                assert 'after-shell' in text[offset:] # 不只是退出而丢了后续UART。
                result={'passed':True,'ring3_shell':True,'ram_file_and_device_readback':True,'other_user_during_cq_wait':True,'qmp_iops':1,'error_exec_survived':True,'returned_to_monitor':True,'orphan_rounds':24,'memory_reuse_rounds':128,'capacity_rounds':4,'capacity_pages':1024,'simd_preempt_fork_exec':True,'fs_base_preempt_fork_exec':True,'compiler_static_tls':True} # 只发布已观察事实。
                result_path.write_text(json.dumps(result,indent=2)+'\n'); print('PASS user shell: alloc/file/async wait/other task/CQ/readback, failed exec, return to monitor') # 阶段演示实际完成。
                return result # 总入口可收集。
            finally: # 包括断言、超时和QEMU错误。
```
<!-- after -->
```python
                assert '[USER_SHELL_EXIT]' in text[offset:] and 'program sh exited 0' in text[offset:] # 真实收拢页/FD/静态池由launch核对。
                offset=len(text); wire.sendall(b'echo after-shell\n'); text=wait('[CONSOLE_UPDATE] view=0 commands=2 page=0',offset) # 回到内核后输入仍完整。
                assert 'after-shell' in text[offset:] # 不只是退出而丢了后续UART。
                result={'passed':True,'ring3_shell':True,'ram_file_and_device_readback':True,'other_user_during_cq_wait':True,'qmp_iops':1,'error_exec_survived':True,'returned_to_monitor':True,'orphan_rounds':24,'memory_reuse_rounds':128,'capacity_rounds':4,'capacity_pages':1024,'simd_preempt_fork_exec':True,'fs_base_preempt_fork_exec':True,'compiler_static_tls':True,'posix_errno_io_subset':True} # 只发布已观察事实。
                result_path.write_text(json.dumps(result,indent=2)+'\n'); print('PASS user shell: alloc/file/async wait/other task/CQ/readback, failed exec, return to monitor') # 阶段演示实际完成。
                return result # 总入口可收集。
            finally: # 包括断言、超时和QEMU错误。
```

<a id="posix-observe"></a>
## 看见−1之前，内核已经给出了哪一个错误

全部修改完成后运行：

<!-- run -->
```sh
./airios build # 只有libc观察程序显式链接POSIX风格包装。
./airios test # 新用户库与原始ABI、TLS边界、输入、服务和存储路径一同运行。
```

Mac默认仍为旧Python时，给命令前加 `PATH=/opt/homebrew/bin:$PATH`。交互 `./airios run`，输入 `run sh`，再输入 `libc`，成功路径包含：

```text
[LIBC_IO_OK] short=256 pipe=64 eof=zero offset=preserved bad_buffer=unchanged
[LIBC_EXEC_RESET_OK] errno=zero cloexec=closed
[LIBC_ERRNO_OK] initial=0 fork=copied preempt=private success=preserved exec=reset
[SHELL_COMMAND_DONE] name=libc code=0
```

这份ELF的errno模板是4字节纯零数据，TP至少16字节对齐，所以实际错误槽位于TP−4，不是共享的内核全局数组。用LLVM的llvm-readelf -lW检查posix-probe.elf可见这项PT_TLS；llvm-objdump -d定位__airi_errno_location则能看到实际本地地址计算。

当前错误槽可以为后续运行库使用，文件包装也已允许普通源码处理短返回与errno；RAM文件仍最多1024字节，单次内核副本256字节，管道64字节，无完整权限、定位、信号、套接字或持久文件系统。下一步继续扩展需要的内存与线程机制，不能把尚未实现的fsync、pthread或socket简单写成返回成功的占位函数。Linux实验仍需在个人主机另行验证。

