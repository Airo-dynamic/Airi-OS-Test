# K33 · 用户shell启动程序，沿一页数据走完文件与异步设备路径

前面一直由 ring0 的 monitor 接收 run NAME，再替用户启动程序。现在把选择与启动也交给用户程序：monitor 只启动 sh，之后由 sh 接受 demo、fork 孩子并 exec，父 shell 等孩子结束再显示提示符。从 K32 继续；Linux L05–L07分别解释线程等待、epoll就绪和io_uring完成，guest构建不要求Linux主机在线。图形支线仍可暂停。本节将已建立的进程、文件和完成路径接到用户输入。

前面的内核monitor运行在ring0，直接调用launch；现在的sh运行在ring3，只能调用自有libc和int80。它没有权限读UART端口、内核进程表或块队列。用户shell与桌面Terminal的窗口控件是不同对象，当前不会为窗口增加一个隐藏用户TTY。

<a id="shell-stdin"></a>
## UART输入只交给一个所有者

同一个输入字节不能既被 monitor 当命令，又被 sh 当程序名。会话边界先确定唯一读者，再给它一个可在用户态调用的 FD：普通程序的新表仍只装 stdout1、stderr2，FD0 空着，旧 files/worker 演示依赖这个约定。直接启动 sh 时，才额外把只读 terminal_input 装到 0 并标 CLOEXEC。

fork 复制的是这项引用。孩子成功 exec 其他程序时关闭自己的 FD0，父 shell 仍保留输入；关闭一个表项不会夺走另一个进程的引用。输入与输出是两个固定对象，最后引用释放也不能把固定配置清零，下次 shell 还要接入。后面的主机单元检查同时覆盖继承、CLOEXEC 和再次打开。

接着跟 sh 的 read(0,&c,1)。它先经 int80 进入文件传输层，用 FD 识别固定输入对象，再由架构读取 UART。arch_serial_read 只有接收就绪才取字节，暂无数据返回 −1；实际字节零仍是数据。传输层先验证完整用户输出，再把当前可读前缀放进最多 256 字节的内核副本，最后复制到目标用户页。

若尚无字节，传输层给出内部 EAGAIN，K29 的 IO_READ 保存原地址和 frame，普通服务栈在 PIT 机会重查；得到字符以后，结果才交回这一次 read。零长度请求立即返回零，不登记等待，pread 则因输入不可定位而返回 ESPIPE。下面先把固定对象和非阻塞架构读取接好，再让已有等待层消费这种内部条件。

launch在用户会话结束前不返回内核monitor，因此monitor不与sh争抢同一UART。当前没有终端复用、行规程、信号、后台任务或多个输入会话；要有多个shell，需要先做终端对象和输入路由，不能仅复制FD0。

**增加架构输入声明。** 原有空闲与用户调度门保持。

<!-- write: include/airi/arch.h -->
```c
#ifndef AIRI_ARCH_H  // 避免重复声明架构操作。
#define AIRI_ARCH_H  // 标记本头文件已展开。
void airi_arch_relax(void);  // 在忙等的一次迭代间提示处理器；不是任务睡眠。
void airi_arch_idle(void); // 固定平台开IRQ并等待一次真实中断，调用者醒后重查条件。
long airi_arch_user_gate(void); // 进入当前用户切换门，返回可为暂回服务或最终结束，由进程层区分。
int airi_arch_serial_read(void); // UART当前一字节或-1无数据，只用于独占串口会话。
#endif  // 结束重复包含保护。
```

**保留发送与错误处理，补接口引用和末尾的非阻塞接收。** 用下面的旧/新窗口定位，相同部分保留。

先引入架构接口，供编译器核对新增接收函数的声明。

<!-- replace: kernel/arch/x86_64/serial.c -->
<!-- before -->
```c
#include <airi/kernel.h>  // 取得串口、日志、停机及基础函数的共同声明

static inline void out8(uint16_t port, uint8_t value) {  // 本文件的八位设备写接口，port 为端口号而非内存地址
```
<!-- after -->
```c
#include <airi/arch.h> // 用户终端只通过架构适配读取UART。
#include <airi/kernel.h>  // 取得串口、日志、停机及基础函数的共同声明

static inline void out8(uint16_t port, uint8_t value) {  // 本文件的八位设备写接口，port 为端口号而非内存地址
```

保留发送和日志实现，在文件末尾追加非阻塞接收。

<!-- replace: kernel/arch/x86_64/serial.c -->
<!-- before -->
```c
        putchar_serial((char)data[i]); // 包括零字节也真正交给UART。
    }
}
```
<!-- after -->
```c
        putchar_serial((char)data[i]); // 包括零字节也真正交给UART。
    }
}

int airi_arch_serial_read(void) { // 不等待，不解码键盘，也不把NUL误判成无数据。
    return (in8(0x3fd) & 1) ? in8(0x3f8) : -1; // 只在接收就绪时消费真实设备字节。
}
```

**增加固定输入身份查询与新表接入声明。** 对外仍是FD，不泄露设备地址。

<!-- write: include/airi/file.h -->
```c
#ifndef AIRI_FILE_H // 内核FD对象与用户整数句柄分开。
#define AIRI_FILE_H // 包含保护。
#include <airi/base.h> // 长度、固定宽度和基础复制。
#include <stdbool.h> // 显式状态。
#define AIRI_FD_COUNT 16u // 每进程最多十六个表项。
#define AIRI_OPEN_READ 1u // 自有ABI标志，不沿用Linux数字。
#define AIRI_OPEN_WRITE 2u // 写访问。
#define AIRI_OPEN_CREATE 4u // 名称不存在时创建。
#define AIRI_OPEN_TRUNCATE 8u // 有写权限才可清空。
#define AIRI_OPEN_CLOEXEC 16u // 成功替换时关闭此表项。
#define AIRI_OPEN_NONBLOCK 32u // 管道可显式选择非阻塞，不支持普通文件open这个标志。
#define AIRI_READY_READ 1u // 管道读不会等待，可能得到数据或EOF。
#define AIRI_READY_WRITE 2u // 当前有读者且有缓冲空间。
#define AIRI_READY_HUP 4u // 读端已无写者，仍须先消费已有数据。
#define AIRI_READY_ERROR 8u // 写端已无读者，下一次写得到EPIPE。
struct airi_open_file; // 全局打开对象由实现拥有。
struct airi_fd_entry { struct airi_open_file *file; bool cloexec; }; // 偏移不放在进程表项中。
struct airi_fd_table { struct airi_fd_entry entries[AIRI_FD_COUNT]; }; // fork逐项增加引用。
void airi_files_init(void); // 仅启动时初始化内存文件系统。
void airi_fd_init(struct airi_fd_table *table); // 初始stdout/stderr指向串口对象。
void airi_fd_clone(struct airi_fd_table *destination, const struct airi_fd_table *source); // 目标必须为空。
void airi_fd_close_all(struct airi_fd_table *table, bool exec_only); // exit关闭全部，exec只关闭标志项。
long airi_fd_open(struct airi_fd_table *table, const char *path, unsigned flags); // 接收已复制的内核路径。
long airi_fd_close(struct airi_fd_table *table, unsigned fd); // 释放表项与其引用。
long airi_fd_dup(struct airi_fd_table *table, unsigned fd, bool cloexec); // 新表项共享偏移。
long airi_fd_seek(struct airi_fd_table *table, unsigned fd, uint64_t offset); // 首版只支持绝对偏移。
long airi_fd_read(struct airi_fd_table *table, unsigned fd, void *buffer, size_t bytes, bool positioned, uint64_t offset); // 输出是内核副本。
long airi_fd_write(struct airi_fd_table *table, unsigned fd, const void *buffer, size_t bytes, bool positioned, uint64_t offset); // 输入不被异步保留。
bool airi_fd_console(const struct airi_fd_table *table, unsigned fd); // 终端发送由设备适配完成。
long airi_fd_pipe(struct airi_fd_table *table, unsigned ends[2], bool nonblocking); // 原子创建两个FD和有界共享缓冲，失败不改输出。
long airi_fd_ready(const struct airi_fd_table *table, unsigned fd); // 本阶段只查询管道当前条件，不订阅事件。
bool airi_fd_nonblocking(const struct airi_fd_table *table, unsigned fd); // 后续等待层据此决定是否可阻塞。
unsigned airi_files_open_count(void); // 不含固定终端对象，用于会话回收检查。
bool airi_fd_input(const struct airi_fd_table *table, unsigned fd); // 判断固定终端输入，设备读取在陷入层。
void airi_fd_attach_input(struct airi_fd_table *table); // 只给全新shell根的空FD0接入CLOEXEC输入。
#endif // 结束保护。
```

**更新固定对象初始化和最后引用释放，并增加输入接入函数。** RAM文件和管道算法保持，按前文版本对照保留。

增加独立固定输入对象。

<!-- replace: kernel/fs/file.c -->
<!-- before -->
```c
static struct memory_file nodes[NODE_COUNT]; // 初值零表示空目录槽。
static struct airi_open_file opened[OPEN_COUNT]; // 引用数零表示空打开槽。
static struct memory_pipe pipes[4]; // readers与writers都零表示未使用的管道。
static struct airi_open_file terminal; // 固定串口对象，不含RAM文件内容。
static struct airi_open_file *get(const struct airi_fd_table *table, unsigned fd) { // 用户整数必须先做范围检查。
    return fd < AIRI_FD_COUNT ? table->entries[fd].file : NULL; // 不把FD转成内核指针。
```
<!-- after -->
```c
static struct memory_file nodes[NODE_COUNT]; // 初值零表示空目录槽。
static struct airi_open_file opened[OPEN_COUNT]; // 引用数零表示空打开槽。
static struct memory_pipe pipes[4]; // readers与writers都零表示未使用的管道。
static struct airi_open_file terminal_input; // 与输出不同的固定对象，最后关闭也不清对象配置。
static struct airi_open_file terminal; // 固定串口对象，不含RAM文件内容。
static struct airi_open_file *get(const struct airi_fd_table *table, unsigned fd) { // 用户整数必须先做范围检查。
    return fd < AIRI_FD_COUNT ? table->entries[fd].file : NULL; // 不把FD转成内核指针。
```

启动时初始化其只读配置，RAM 与输出配置保留。

<!-- replace: kernel/fs/file.c -->
<!-- before -->
```c
    airi_memset(pipes, 0, sizeof(pipes)); // 启动前没有端点或在途字节。
    airi_memset(opened, 0, sizeof(opened)); // 清空全局打开对象。
    airi_memset(&terminal, 0, sizeof(terminal)); // 固定对象没有自有节点。
    terminal.flags = AIRI_OPEN_WRITE; // 当前只提供stdout/stderr。
    airi_memcpy(nodes[0].name, "/seed", 6); // 只读初始文件，字符串含终止零。
    airi_memcpy(nodes[0].data, "abcdef", 6); // 文件字节无需NUL。
```
<!-- after -->
```c
    airi_memset(pipes, 0, sizeof(pipes)); // 启动前没有端点或在途字节。
    airi_memset(opened, 0, sizeof(opened)); // 清空全局打开对象。
    airi_memset(&terminal, 0, sizeof(terminal)); // 固定对象没有自有节点。
    airi_memset(&terminal_input, 0, sizeof(terminal_input)); // 启动时无输入引用。
    terminal_input.flags = AIRI_OPEN_READ; // 只读，不能被当成stdout。
    terminal.flags = AIRI_OPEN_WRITE; // 当前只提供stdout/stderr。
    airi_memcpy(nodes[0].name, "/seed", 6); // 只读初始文件，字符串含终止零。
    airi_memcpy(nodes[0].data, "abcdef", 6); // 文件字节无需NUL。
```

普通根 FD0 仍为空；只有 shell 根另外接入输入。

<!-- replace: kernel/fs/file.c -->
<!-- before -->
```c
    nodes[0].readonly = true; // 模式请求不能让只读节点可写。
}
void airi_fd_init(struct airi_fd_table *table) { // 新会话的独立表，不复制旧进程编号。
    airi_memset(table, 0, sizeof(*table)); // stdin暂未接入，槽0可用于普通文件。
    table->entries[1].file = &terminal; // stdout引用固定设备对象。
    table->entries[2].file = &terminal; // stderr是另一个表项。
    terminal.references += 2; // 两个表项各持有一次引用。
```
<!-- after -->
```c
    nodes[0].readonly = true; // 模式请求不能让只读节点可写。
}
void airi_fd_init(struct airi_fd_table *table) { // 新会话的独立表，不复制旧进程编号。
    airi_memset(table, 0, sizeof(*table)); // 普通根的槽0默认空，shell随后显式接入输入。
    table->entries[1].file = &terminal; // stdout引用固定设备对象。
    table->entries[2].file = &terminal; // stderr是另一个表项。
    terminal.references += 2; // 两个表项各持有一次引用。
```

最后引用关闭时不清空固定输入对象。

<!-- replace: kernel/fs/file.c -->
<!-- before -->
```c
    if (!file) // 无效或已关闭。
        return -9; // -EBADF。
    table->entries[fd] = (struct airi_fd_entry){0}; // 先撤掉进程引用。
    if (--file->references == 0 && file != &terminal) { // 最后表项引用消失才关闭真实端点。
        if (file->pipe) { // 普通RAM文件没有管道对端状态。
            if (file->flags & AIRI_OPEN_READ) // 当前是只读端点打开对象。
                --file->pipe->readers; // dup/fork共享对象，不能每关一个FD都减端点数。
```
<!-- after -->
```c
    if (!file) // 无效或已关闭。
        return -9; // -EBADF。
    table->entries[fd] = (struct airi_fd_entry){0}; // 先撤掉进程引用。
    if (--file->references == 0 && file != &terminal && file != &terminal_input) { // 最后表项引用消失才关闭真实端点。
        if (file->pipe) { // 普通RAM文件没有管道对端状态。
            if (file->flags & AIRI_OPEN_READ) // 当前是只读端点打开对象。
                --file->pipe->readers; // dup/fork共享对象，不能每关一个FD都减端点数。
```

在末尾加入 shell 输入接入与身份查询，不暴露设备指针。

<!-- replace: kernel/fs/file.c -->
<!-- before -->
```c
    struct airi_open_file *file = get(table, fd); // 不直接解用户值。
    return file && (file->flags & AIRI_OPEN_NONBLOCK); // 不支持或无效对象不会被当成非阻塞流。
}
```
<!-- after -->
```c
    struct airi_open_file *file = get(table, fd); // 不直接解用户值。
    return file && (file->flags & AIRI_OPEN_NONBLOCK); // 不支持或无效对象不会被当成非阻塞流。
}

void airi_fd_attach_input(struct airi_fd_table *table) { // 调用者保证是刚初始化且槽零为空的新表。
    table->entries[0] = (struct airi_fd_entry){.file=&terminal_input, .cloexec=true}; // exec子程序不能继承shell独占的UART输入。
    ++terminal_input.references; // fork/close使用已有共享引用协议。
}
bool airi_fd_input(const struct airi_fd_table *table, unsigned fd) { // 只比较内核已验证表项身份。
    return get(table, fd) == &terminal_input; // 不把用户整数当硬件地址。
}
```

**在文件/管道检查末尾补输入fork、CLOEXEC和重复接入。** 不是用宿主stdin替代guest设备。

在现有观察后验证输入继承、CLOEXEC、最后引用与再次接入。

<!-- replace: tests/unit/file_test.c -->
<!-- before -->
```c
    assert(airi_fd_pipe(&parent, ends, true) == -28 && ends[0] == 77 && ends[1] == 88 && airi_files_open_count() == 8); // 第五条失败不泄漏半个FD或对象。
    airi_fd_close_all(&parent, false); // 扫描全部表项，端点池也全部回收。
    assert(!airi_files_open_count()); // 后续程序可继续创建流。
    puts("PASS RAM files/pipes: offsets, fork/exec references, sparse zero, short I/O, FIFO wrap, endpoint lifetime, exhaustion"); // 全部边界通过才发布。
    return 0; // 宿主测试成功。
}
```
<!-- after -->
```c
    assert(airi_fd_pipe(&parent, ends, true) == -28 && ends[0] == 77 && ends[1] == 88 && airi_files_open_count() == 8); // 第五条失败不泄漏半个FD或对象。
    airi_fd_close_all(&parent, false); // 扫描全部表项，端点池也全部回收。
    assert(!airi_files_open_count()); // 后续程序可继续创建流。
    airi_fd_init(&parent); airi_fd_attach_input(&parent); // 只给新shell根接入固定输入。
    assert(airi_fd_input(&parent,0) && parent.entries[0].cloexec); // FD0与输出对象身份不同，exec时必须撤销。
    airi_fd_clone(&child,&parent); // fork增加输入对象引用，但不复制设备。
    airi_fd_close_all(&child,true); // 成功exec只关闭CLOEXEC输入，保留stdout/stderr。
    assert(!airi_fd_input(&child,0) && airi_fd_input(&parent,0)); // 子撤销不影响父shell继续读。
    airi_fd_close_all(&child,false); airi_fd_close_all(&parent,false); // 最后输入引用释放不能清空固定对象配置。
    airi_fd_init(&parent); airi_fd_attach_input(&parent); // 再次启动shell可重新使用相同输入对象。
    assert(airi_fd_input(&parent,0)); airi_fd_close_all(&parent,false); // 重复会话完整回收。
    puts("PASS RAM files/pipes: offsets, fork/exec references, sparse zero, short I/O, FIFO wrap, endpoint lifetime, exhaustion"); // 全部边界通过才发布。
    return 0; // 宿主测试成功。
}
```

<a id="shell-user-parser"></a>
## shell保留自己的映像，孩子成功exec才替换

用户读到 d、e、m、o 后，仍只是在自己的 line 数组中累积名称；回车才决定启动。先保证这个输入和名称查询边界能处理失败：sh 先验证目录输出的 ENOSPC 与 EFAULT、stdin 空读和坏输出指针。通过后发布USER_SHELL_READY。行缓冲64字节包含末尾NUL，退格只撤已收字符，CR后紧随LF只执行一次；一旦溢出，剩余整行全部丢弃，不能把被截短前缀当可执行名称。

收到 demo 后，如果 shell 自己直接 exec，成功就失去再次显示提示符的旧映像。因此它保留自己，只让孩子替换。help、programs 和 exit 是 shell 自己的内建操作。其他整行作为一个无参数程序名：父fork得到孩子PID，子调用exec，成功从新_start开始，失败仍在子旧映像里打印负错误并exit127；父waitpid阻塞，拿到这个孩子的真实退出码后打印SHELL_COMMAND_DONE，再读下一行。父的line在父地址空间里，孩子替换页面不会破坏它。当前不支持参数、重定向、管线语法、作业控制与嵌套sh。

programs系统调用30从exec使用的同一注册表形成有界换行文本。容量不足不复制半份，实际输出包含NUL，返回长度不含NUL；用户获得名字，不获得ELF地址或内部指针。kernel monitor的programs也遍历相同注册表。目录里保留broken等故障样本，列出名称不等于保证它是合法ELF。

**加入用户目录查询。** 已有读写、进程与异步ABI保持。

<!-- write: labs/airios/include/airi_libc.h -->
```c
#ifndef AIRI_USER_LIBC_H // 用户库头与内核基础库分开。
#define AIRI_USER_LIBC_H // 包含保护。
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
void *airi_alloc(size_t bytes); // 零填充页申请，失败NULL，进程结束统一回收。
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
long airi_programs(char *names, size_t capacity); // 真实初始ELF目录，每行名称，容量不足不截断。
#endif // 结束包含保护。
```

**在末尾加入系统调用30包装。** 名称结果由内核复制到用户输出。

在末尾加入 30 号目录复制包装。

<!-- replace: labs/airios/libc/syscall.c -->
<!-- before -->
```c
long airi_io_state_get(struct airi_io_state *state) { // 状态快照不会消耗CQ或解锁buffer。
    return call3(29, (uintptr_t)state, 0, 0); // 四十字节，不含内核地址。
}
```
<!-- after -->
```c
long airi_io_state_get(struct airi_io_state *state) { // 状态快照不会消耗CQ或解锁buffer。
    return call3(29, (uintptr_t)state, 0, 0); // 四十字节，不含内核地址。
}

long airi_programs(char *names, size_t capacity) { // 完整目录复制到用户输出，不借内核指针。
    return call3(30, (uintptr_t)names, capacity, 0); // 返回不含NUL长度或负错误。
}
```

**新建用户shell。** 从数字输出、执行孩子到行输入按顺序敲入完整代码。

<!-- write: labs/airios/sh.c -->
```c
#include <airi_libc.h> // shell只通过用户库访问内核，不链接console内部对象。
static int equal(const char *a, const char *b) { // 有界行已经NUL终止。
    size_t n=strlen(a); return n==strlen(b) && !memcmp(a,b,n); // 先比长度，避免读过短字符串。
}
static void number(long value) { // 最小十进制输出，不引入完整printf。
    char digits[24]; unsigned used=0; unsigned long n; // 数值最大PID与错误码均可容纳。
    if (value<0) { airi_puts("-"); n=(unsigned long)(-(value+1))+1; } // 即使最小有符号数也不溢出。
    else n=(unsigned long)value; // 正数直接转换。
    do { digits[used++]=(char)('0'+n%10); n/=10; } while (n); // 先产生逆序数字。
    while (used) airi_write(1,&digits[--used],1); // 从高位逐字输出。
}
static void execute(const char *name) { // 一条无参数程序名对应一个孩子。
    long child=airi_fork(); // shell保留原页，子得到独立用户副本与共享FD引用。
    if (child<0) { airi_puts("fork failed: "); number(child); airi_puts("\n"); return; } // 资源满仍回到提示符。
    if (!child) { // 只有孩子执行替换，父shell不能被覆盖。
        long error=airi_exec(name); // 成功不返回，CLOEXEC关闭复制来的输入FD0。
        airi_puts("exec failed: "); number(error); airi_puts("\n"); airi_exit(127); // 失败旧孩子仍存在，明确退出供父收取。
    }
    int status=0; long waited=airi_waitpid(child,&status); // 父阻塞等待指定孩子，不以yield轮询。
    if (waited!=child) { airi_puts("wait failed\n"); return; } // 不把缺状态当成功。
    airi_puts("[SHELL_COMMAND_DONE] name="); airi_puts(name); // 名称是父自己的行缓冲，孩子exec不影响它。
    airi_puts(" code="); number(status); airi_puts("\n"); // 这是孩子真实退出结果。
}
int main(void) { // 内核监控台run sh后从此用户入口继续。
    char guard[1]={'Q'}; // 小容量失败不能覆盖用户哨兵。
    if (airi_programs(guard,1)!=-28 || guard[0]!='Q' || airi_programs((char *)0,256)!=-14 || airi_read(0,guard,0)!=0 || airi_read(0,(void *)0,1)!=-14) // 目录与UART输出先完整验证，空读不阻塞。
        return 2; // 未通过不能发布shell就绪。
    char line[64]; size_t used=0; int overflow=0,skip_lf=0; // 固定输入预算，过长行不截断执行。
    airi_puts("[USER_SHELL_READY]\nCommands: help programs exit, or one program name.\n"); // 交互与内核monitor的run语法不同。
    for (;;) { // 用户shell保持运行，exit才回到内核监控台。
        airi_puts("[USER_SHELL_PROMPT]\nsh> "); used=0; overflow=0; // 每条新命令有明确观察边界。
        for (;;) { // 行编辑留在用户，不把桌面控件引进内核。
            char c; long n=airi_read(0,&c,1); // 没字节时真正IO_READ阻塞，由PIT驱动UART重查。
            if (n!=1) { airi_puts("shell stdin unavailable\n"); return 1; } // 嵌套exec sh没有stdin时安全退出。
            if (skip_lf && c=='\n') { skip_lf=0; continue; } // CRLF只执行一次，不产生空命令。
            skip_lf=0; // 非LF字节仍正常解释。
            if (c=='\r' || c=='\n') { skip_lf=c=='\r'; airi_puts("\n"); break; } // 提交整行。
            if ((c==8 || c==127) && !overflow) { // 支持Backspace和Delete退格。
                if (used) { --used; airi_puts("\b \b"); } // 只撤已输入字符。
                continue; // 退格不是程序名的一部分。
            }
            if ((unsigned char)c<32 || (unsigned char)c>126) continue; // 首轮命令限定可见ASCII。
            if (used+1<sizeof line && !overflow) { line[used++]=c; airi_write(1,&c,1); } // 始终保留NUL位置。
            else overflow=1; // 后续字节一直消费到换行，不能执行被截短的前缀。
        }
        line[used]=0; // 所有strlen与exec都有终止界限。
        if (overflow) { airi_puts("line too long; discarded\n"); continue; } // 放弃整条过长输入。
        if (!used) continue; // 空行只重画提示符。
        if (equal(line,"exit")) { airi_puts("[USER_SHELL_EXIT]\n"); return 0; } // 退出与普通孩子结束分开。
        if (equal(line,"help")) { airi_puts("Use programs, one name, or exit. No arguments, redirects, jobs or nested sh.\n"); continue; } // 准确说明当前语法。
        if (equal(line,"programs")) { // 目录从内核真实注册表查询，避免维护第二套名单。
            char names[256]; long n=airi_programs(names,sizeof names); // 完整结果或错误，不显示半目录。
            if (n>=0) airi_puts(names); else airi_puts("catalog unavailable\n"); // 名称文本没有内部指针。
            continue; // 内建查询不用fork。
        }
        if (equal(line,"sh")) { airi_puts("nested sh has no separate input terminal\n"); continue; } // 不与父竞争单UART。
        execute(line); // 其他名称交给真实exec，未知名字同样有明确失败结果。
    }
}
```

<a id="shell-pipeline"></a>
## demo：用户页与注册副本的两个寿命

shell 为 demo 创建孩子并 exec 后，新程序先 airi_alloc 申请 4096 字节。内核分配、清零、建立可写映射，全部成功才前移 heap_next 并报告 USER_ALLOC。程序检查整页为零，再在首 512 字节填模式。这里的用户页会活到进程退出，没有单独的对象 free。

先让同一模式经过 RAM 文件：/demo 按 256 字节系统调用副本上限分两次写，再用 pread 分两段读到用户页偏移 1024，比较内容后关闭 FD。文件能跨程序保留，却不会跨 QEMU 重启持久化；这一步还没接设备。

接下来把用户页前 512 字节 put 到注册写 buffer，另申请读 buffer。注册对象是内核池副本，设备不直接借用户页。fork 后孩子得到用户页副本，但 owner 为空，不继承两个 handle。父提交扇区 257 的 WRITE 并等 CQ，孩子用真实 tick 工作、报告 OTHER_PROGRESS。快设备可能在 next 前已完成，交错证据另由 QMP 真实限速取得，guest 不伪造延迟。

父恢复执行不等于写已完成，必须核对自己的 cookie1、handle、WRITE 和 result=512。之后才提交 cookie2 的 FLUSH，等结果 0，再提交 cookie3 的 READ，等结果 512。每次都分别处理接受、等待、CQ 身份和操作结果；airi_io_next 返回 0 只表示取得记录，实际字节数在 cqe.result。

读 CQ 已收取，buffer 才解除 BUSY。get 将它复制到用户页偏移 512，与原模式比较，然后 free 两个注册对象并 wait 孩子。扇区 257 属于独立异步实验区，不是正式日志；WRITE→FLUSH→READ 说明请求同步顺序，没有经历主机断电。demo 返回后 shell 收退出码，用户页由进程退出统一回收，不能把页回收与注册 buffer 的 free 当成一个协议。

**新建贯通演示。** 应用错误只exit1，shell能够继续处理下一条命令。

<!-- write: labs/airios/demo.c -->
```c
#include <airi_libc.h> // 页、FD、进程与异步请求都走自有用户系统调用。
static void require(int yes, const char *message) { // 应用失败只退出当前程序，shell继续存在。
    if (!yes) { airi_puts("[PIPELINE_FAIL] "); airi_puts(message); airi_puts("\n"); airi_exit(1); } // 不要求内核PANIC。
}
static void operation(unsigned op, uint64_t handle, uint64_t cookie, long expected) { // 每次接受、等待、验证是三个不同动作。
    struct airi_io_submission submission={.sector=257,.cookie=cookie}; // 独立实验扇区，不覆盖正式追加日志。
    require(airi_io_submit(op,handle,&submission)==0,"submit"); // 只表示资源与CQ信用已接受。
    struct airi_io_completion cqe; // 用户栈一直存活到该调用返回。
    require(airi_io_next(&cqe,1)==0,"wait CQ"); // 空队列登记等待，期间别的进程可执行。
    require(cqe.cookie==cookie && cqe.operation==op && cqe.buffer==handle && cqe.result==expected,"CQ identity/result"); // 不把接口返回0误读成读到0字节。
    airi_puts("[PIPELINE_CONTINUE] CQ consumed; buffer reusable\n"); // 真实收取后才继续。
}
int main(void) { // shell fork后exec，独立页表和异步owner。
    unsigned char *page=airi_alloc(4096); require(page!=NULL,"allocate page"); // 页退出时统一回收，没有单对象free。
    for (unsigned i=0;i<4096;++i) require(page[i]==0,"zero page"); // 新映射不能泄露旧进程数据。
    for (unsigned i=0;i<512;++i) page[i]=(unsigned char)(i*17+3); // 端到端读回的真实数据。
    long file=airi_open("/demo",AIRI_READ|AIRI_WRITE|AIRI_CREATE|AIRI_TRUNCATE); require(file>=0,"RAM file"); // 同时覆盖真实FD路径。
    require(airi_write((unsigned)file,page,256)==256 && airi_write((unsigned)file,page+256,256)==256,"file writes"); // 单次用户副本上限256，显式分两段。
    require(airi_pread((unsigned)file,page+1024,256,0)==256 && airi_pread((unsigned)file,page+1280,256,256)==256,"file reads"); // 显式偏移不改写位置。
    require(!memcmp(page,page+1024,512) && airi_close((unsigned)file)==0,"file roundtrip"); // 路径内容由RAMFS拥有，FD按引用释放。
    long write_buffer=airi_io_alloc(),read_buffer=airi_io_alloc(); require(write_buffer>0 && read_buffer>0,"register copies"); // 代数句柄不是用户VA。
    require(airi_io_put((uint64_t)write_buffer,page,512)==0,"copy payload"); // 请求借用的是内核注册副本，不直接DMA用户页。
    long child=airi_fork(); require(child>=0,"fork worker"); // 子异步owner为空，不继承两个handle的所有权。
    if (!child) { // 另一个真实用户做少量工作，展示父等待期间CPU仍能推进。
        uint64_t start=airi_ticks(); while (airi_ticks()-start<3) { } // 等真实三tick，使父先登记CQ等待。
        airi_puts("[PIPELINE_OTHER_PROGRESS]\n"); // 测试核对它是否位于父登记与唤醒之间。
        while (airi_ticks()-start<30) { } // 仅演示用户计算，不伪造设备延迟。
        airi_exit(0); // 父随后wait收取身份和页资源。
    }
    operation(AIRI_IO_WRITE,(uint64_t)write_buffer,1,512); // sector257写入完成。
    operation(AIRI_IO_FLUSH,0,2,0); // 再同步设备，不能以WRITE完成代替FLUSH。
    operation(AIRI_IO_READ,(uint64_t)read_buffer,3,512); // 最后从同一设备读回。
    require(airi_io_get((uint64_t)read_buffer,page+512,512)==0 && !memcmp(page,page+512,512),"device readback"); // CQ之后才允许获取注册副本。
    require(airi_io_free((uint64_t)write_buffer)==0 && airi_io_free((uint64_t)read_buffer)==0,"release buffers"); // 不在完成前释放。
    int status=-1; require(airi_waitpid(child,&status)==child && status==0,"reap worker"); // 孩子也必须正常结束。
    airi_puts("[PIPELINE_USER_OK] page=4096 ram_file=512 device=512 flush=0 child=reaped\n"); return 0; // shell收取这个程序的退出码。
}
```

<a id="shell-kernel-entry"></a>
## 内核注册两个ELF，并保持图形入口可返回

shell 的控制逻辑留在用户 ELF，内核只提供已经解释过的连接处。user.c 只增加 sh/demo 映像目录、UART读分支、目录调用30、根shell输入接入、成功页分配记录，以及monitor的图形限制。已有 fork/exec/等待调度的机制不变，下面只给这些连接处的旧/新窗口。图形构建定义AIRI_GRAPHICAL_CONSOLE时，monitor的run sh明确提示从./airios run串口进入，立即返回；不能让窗口等待一个未接线的输入通道。其他用户程序仍可运行，stdout仍写UART。

**更新进程与系统调用连接处。** 保留此前相同函数，逐项加入本节已解释的接线。

登记 sh 与 demo ELF 声明。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
extern const uint8_t airi_user_rsp_bad_start[], airi_user_rsp_bad_end[]; // 不规范用户返回栈。
extern const uint8_t airi_hello_elf_start[], airi_hello_elf_end[]; // 既有独立用户C程序。
extern const uint8_t airi_process_elf_start[], airi_process_elf_end[]; // 新的fork/wait和抢占工作负载。
extern const uint8_t airi_async_elf_start[], airi_async_elf_end[]; // 用户注册、CQ等待与exec资源轮次。
extern const uint8_t airi_waits_elf_start[], airi_waits_elf_end[]; // 真正阻塞与超时工作负载。
extern const uint8_t airi_pipes_elf_start[], airi_pipes_elf_end[]; // 非阻塞管道用户程序。
```
<!-- after -->
```c
extern const uint8_t airi_user_rsp_bad_start[], airi_user_rsp_bad_end[]; // 不规范用户返回栈。
extern const uint8_t airi_hello_elf_start[], airi_hello_elf_end[]; // 既有独立用户C程序。
extern const uint8_t airi_process_elf_start[], airi_process_elf_end[]; // 新的fork/wait和抢占工作负载。
extern const uint8_t airi_shell_elf_start[], airi_shell_elf_end[]; // 真正用户shell，不是内核命令别名。
extern const uint8_t airi_demo_elf_start[], airi_demo_elf_end[]; // 用户页分配与异步存储贯通演示。
extern const uint8_t airi_async_elf_start[], airi_async_elf_end[]; // 用户注册、CQ等待与exec资源轮次。
extern const uint8_t airi_waits_elf_start[], airi_waits_elf_end[]; // 真正阻塞与超时工作负载。
extern const uint8_t airi_pipes_elf_start[], airi_pipes_elf_end[]; // 非阻塞管道用户程序。
```

同一装载目录接入两个名字。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
extern const uint8_t airi_broken_elf_start[], airi_broken_elf_end[]; // 受控损坏输入，验证旧进程保持。
struct executable { const char *name; const uint8_t *start, *end; }; // 初始映像目录尚不是通用文件系统。
static const struct executable executables[] = { // 每份映像字节由内核只读段长期持有。
    {"hello", airi_hello_elf_start, airi_hello_elf_end}, // 独立C运行库与BSS观察。
    {"processes", airi_process_elf_start, airi_process_elf_end}, // fork/exec/wait实际工作负载。
    {"worker", airi_worker_elf_start, airi_worker_elf_end},
```
<!-- after -->
```c
extern const uint8_t airi_broken_elf_start[], airi_broken_elf_end[]; // 受控损坏输入，验证旧进程保持。
struct executable { const char *name; const uint8_t *start, *end; }; // 初始映像目录尚不是通用文件系统。
static const struct executable executables[] = { // 每份映像字节由内核只读段长期持有。
    {"sh", airi_shell_elf_start, airi_shell_elf_end}, // 独占串口输入的可选用户程序。
    {"demo", airi_demo_elf_start, airi_demo_elf_end}, // 页、任务和设备完成串联。
    {"hello", airi_hello_elf_start, airi_hello_elf_end}, // 独立C运行库与BSS观察。
    {"processes", airi_process_elf_start, airi_process_elf_end}, // fork/exec/wait实际工作负载。
    {"worker", airi_worker_elf_start, airi_worker_elf_end},
```

file_transfer 先识别固定输入，再验证并读取 UART 前缀；暂无字节交给原等待层。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
            return (long)bytes; // 返回逻辑进展。
        }
        return airi_fd_write(&p->files, (unsigned)fd, buffer, bytes, positioned, offset); // 满管道可返回EAGAIN。
    }
    long result = airi_fd_read(&p->files, (unsigned)fd, buffer, bytes, positioned, offset); // EOF和暂不可读分开。
    if (result > 0 && !copy_user(p, buffer, address, (size_t)result, true)) // 目标阻塞且单线程，已检查映射不应变化。
```
<!-- after -->
```c
            return (long)bytes; // 返回逻辑进展。
        }
        return airi_fd_write(&p->files, (unsigned)fd, buffer, bytes, positioned, offset); // 满管道可返回EAGAIN。
    }
    if (airi_fd_input(&p->files, (unsigned)fd)) { // 只有shell拥有，内核监控台在launch返回前不再轮询UART。
        if (positioned) // 字节流没有文件偏移。
            return -29; // -ESPIPE，不消费字节。
        if (!bytes) // 零长度读取不能登记永久等待，也不消费UART。
            return 0; // 有效输入FD的空操作。
        size_t received = 0; // 返回当前可取得的前缀，不等待填满用户请求。
        while (received < bytes) { // bytes与整个用户输出范围已在上方验证。
            int c = airi_arch_serial_read(); // 设备寄存器只在架构层。
            if (c < 0) // 暂无字节和字符NUL不同。
                break; // 让已有前缀先交给用户。
            buffer[received++] = (uint8_t)c; // 固定内核副本不会越过256字节。
        }
        if (!received) // 空输入走同一个IO_READ持久等待机制。
            return -11; // 由陷入分派登记等待，不让shell忙轮询。
        if (!copy_user(p, buffer, address, received, true)) // 消费UART前已经完整验证用户范围。
            airi_panic("validated UART output mapping changed"); // 内部错误不能静默丢字节。
        return (long)received; // 实际取得的字节数。
    }
    long result = airi_fd_read(&p->files, (unsigned)fd, buffer, bytes, positioned, offset); // EOF和暂不可读分开。
    if (result > 0 && !copy_user(p, buffer, address, (size_t)result, true)) // 目标阻塞且单线程，已检查映射不应变化。
```

只有用户页完整分配提交后才报告地址。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
        if (!owned_page(current, address + i * 4096ull, AIRI_VM_WRITE)) // 成功页面已在owned中登记。
            goto rollback; // 游标尚未前移。
    current->heap_next += count * 4096ull; // 全部成功后再发布新的末端。
    return (long)address; // 当前没有逐对象free，进程结束统一回收。
rollback: // 已有用户页面不受本次耗尽影响。
    while (current->count > before) { // 只处理新增的成功项。
```
<!-- after -->
```c
        if (!owned_page(current, address + i * 4096ull, AIRI_VM_WRITE)) // 成功页面已在owned中登记。
            goto rollback; // 游标尚未前移。
    current->heap_next += count * 4096ull; // 全部成功后再发布新的末端。
    airi_log("[USER_ALLOC] pid=%u address=0x%llx pages=%u zeroed=1", current->pid, (unsigned long long)address, count); // 只记录已完整提交的用户映射。
    return (long)address; // 当前没有逐对象free，进程结束统一回收。
rollback: // 已有用户页面不受本次耗尽影响。
    while (current->count > before) { // 只处理新增的成功项。
```

新增目录调用 30，从同一 exec 注册表构造完整名称输出。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
        }
        f->rax = (uint64_t)result; // 用户看到的只是本次时刻数值。
    }
    else // 尚未提供的接口都明确拒绝。
        f->rax = (uint64_t)-38ll; // -ENOSYS。
    poll_waiters(); // 文件/流修改完成后立即重查其他等待者，不等用户轮询。
```
<!-- after -->
```c
        }
        f->rax = (uint64_t)result; // 用户看到的只是本次时刻数值。
    }
    else if (f->rax == 30) { // 目录只返回有界文本，不暴露ELF或内核对象地址。
        char names[256]; size_t used = 0; // 当前固定目录完整小于此上限。
        for (unsigned i=0; i<sizeof(executables)/sizeof(executables[0]); ++i) { // 结果与exec使用同一真实注册表。
            size_t n=airi_strlen(executables[i].name); // 名称包含内部演示目标，用户仍可选择尝试。
            if (used+n+2>sizeof(names)) // 防止以后目录增长造成栈覆盖。
                airi_panic("program catalog exceeds bounded ABI"); // 维护者必须扩大显式上限。
            airi_memcpy(names+used,executables[i].name,n); used+=n; // 复制有效名称前缀。
            names[used++]='\n'; // 每行一个名字，界面自己呈现。
        }
        names[used]=0; // 查询返回含NUL的完整文本，不返回截断目录。
        if (f->rsi < used+1) // 小容量不能写一半。
            f->rax=(uint64_t)-28ll; // -ENOSPC。
        else if (!copy_user(current,names,f->rdi,used+1,true)) // 验证实际输出跨度。
            f->rax=(uint64_t)-14ll; // -EFAULT。
        else // 完整复制后才发布有效长度。
            f->rax=used; // 不含末尾NUL，与strlen一致。
    }
    else // 尚未提供的接口都明确拒绝。
        f->rax = (uint64_t)-38ll; // -ENOSYS。
    poll_waiters(); // 文件/流修改完成后立即重查其他等待者，不等用户轮询。
```

直接启动 sh 的根 FD 表才额外装入 stdin，exec 的 CLOEXEC 保持旧程序 FD0 约定。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
        return error; // 调用者可以继续命令，不停机。
    }
    airi_fd_init(&root->files); // 初始stdout/stderr归本次根进程持有。
    root->pid = next_pid++; // 完整准备后才取得运行身份。
    root->heap_next = USER_HEAP; // 动态用户页初始为空。
    int async_error = airi_async_open(root->pid); // 完整映像准备后才登记新进程资源上下文。
```
<!-- after -->
```c
        return error; // 调用者可以继续命令，不停机。
    }
    airi_fd_init(&root->files); // 初始stdout/stderr归本次根进程持有。
    if (name && airi_strlen(name)==2 && !airi_memcmp(name,"sh",2)) // 只有直接启动shell时接入UART输入。
        airi_fd_attach_input(&root->files); // CLOEXEC使既有子程序FD0继续可用于普通文件。
    root->pid = next_pid++; // 完整准备后才取得运行身份。
    root->heap_next = USER_HEAP; // 动态用户页初始为空。
    int async_error = airi_async_open(root->pid); // 完整映像准备后才登记新进程资源上下文。
```

monitor 的 programs 也遍历同一注册表，不另硬编码程序名。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
bool airi_user_command(struct airi_console *console, const char *command) { // 系统命令消费者独立于图形主题扩展。
    size_t bytes = airi_strlen(command); // 先取得有效长度，固定前缀比较不能越过短字符串。
    if (bytes == 8 && !airi_memcmp(command, "programs", 8)) { // 显示当前初始ELF目录。
        airi_console_print(console, "programs: hello processes worker broken files storage recover pipes waits async async-reset"); // broken是明确损坏的演示输入。
        return true; // 已处理，不再交给图形扩展。
    }
    if (bytes < 4 || airi_memcmp(command, "run ", 4)) // 只接受明确的run空格前缀。
```
<!-- after -->
```c
bool airi_user_command(struct airi_console *console, const char *command) { // 系统命令消费者独立于图形主题扩展。
    size_t bytes = airi_strlen(command); // 先取得有效长度，固定前缀比较不能越过短字符串。
    if (bytes == 8 && !airi_memcmp(command, "programs", 8)) { // 显示当前初始ELF目录。
        airi_console_print(console,"programs: registered ELF images"); // 表头不硬编码另一套程序名。
        for (unsigned i=0; i<sizeof(executables)/sizeof(executables[0]); ++i) // 与exec共享真实目录，不维护第二份静态名称。
            airi_console_print(console, "%s", executables[i].name); // broken是明确损坏的演示输入。
        return true; // 已处理，不再交给图形扩展。
    }
    if (bytes < 4 || airi_memcmp(command, "run ", 4)) // 只接受明确的run空格前缀。
```

图形构建拒绝未接用户 TTY 的 run sh，其他程序继续沿既有路径。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
        airi_console_print(console, "program not found: %s", name); // 给出当前名称。
        return true; // 请求已得到明确结果。
    }
    struct user_result result; // 子进程都结束后取得数值退出状态。
    int error = launch(image->start, (size_t)((uintptr_t)image->end - (uintptr_t)image->start), name, true, &result); // 同步执行并完整回收，不借回调局部对象给用户。
    if (error) // 例如坏ELF返回-8，不是PANIC。
```
<!-- after -->
```c
        airi_console_print(console, "program not found: %s", name); // 给出当前名称。
        return true; // 请求已得到明确结果。
    }
#ifdef AIRI_GRAPHICAL_CONSOLE // 当前图形终端尚未接入用户TTY，不能让窗口命令陷入不可见输入等待。
    if (bytes==6 && !airi_memcmp(name,"sh",2)) { // 只限制需要交互输入的shell，其他程序仍可运行。
        airi_console_print(console,"User shell uses UART: start ./airios run, then run sh."); // 给出实际可操作入口。
        return true; // 图形桌面立即继续接收输入。
    }
#endif // 纯串口主线没有此限制。
    struct user_result result; // 子进程都结束后取得数值退出状态。
    int error = launch(image->start, (size_t)((uintptr_t)image->end - (uintptr_t)image->start), name, true, &result); // 同步执行并完整回收，不借回调局部对象给用户。
    if (error) // 例如坏ELF返回-8，不是PANIC。
```

**追加sh/demo的.incbin边界。** 它们是内核只读装载输入，用户运行时另建页面。

<!-- write: labs/airios/images.S -->
```asm
.section .rodata.user_elf,"a" /* 原始ELF作为内核只读输入，不在内核直接执行。 */
.balign 8 /* 输入开头至少按8字节对齐，解析仍使用memcpy。 */
.global airi_hello_elf_start, airi_hello_elf_end /* 编译期固定映像边界。 */
airi_hello_elf_start: /* 文件头也包含在输入字节中。 */
.incbin AIRI_HELLO_ELF /* Make传入当前配置自己的ELF路径，不复制另一配置旧产物。 */
airi_hello_elf_end: /* 解析器只允许读取边界之内。 */
.section .note.GNU-stack,"",@progbits /* 包装对象不需要可执行栈。 */
.section .rodata.user_elf,"a" /* 第二个独立ELF同样只是内核装载输入。 */
.balign 8 /* 边界按八字节对齐，解析器仍安全按字节复制。 */
.global airi_process_elf_start, airi_process_elf_end /* 多进程用户工作负载的完整文件边界。 */
airi_process_elf_start: /* 包含独立文件头与程序表。 */
.incbin AIRI_PROCESS_ELF /* 路径由本配置的Make依赖提供。 */
airi_process_elf_end: /* 内核不能读过此边界。 */
.section .rodata.user_elf,"a" /* 第三个独立ELF作为exec目标。 */
.balign 8 /* 原始输入边界固定。 */
.global airi_worker_elf_start, airi_worker_elf_end /* 新用户映像范围。 */
airi_worker_elf_start: /* 解析从文件头开始。 */
.incbin AIRI_WORKER_ELF /* Make提供同一配置刚生成的文件。 */
airi_worker_elf_end: /* 输入长度不包括后续损坏夹具。 */
.global airi_broken_elf_start, airi_broken_elf_end /* 受控截断ELF不需要外部文件。 */
airi_broken_elf_start: /* 只有魔数与少量标识，不足64字节头。 */
.byte 0x7f, 0x45, 0x4c, 0x46, 2, 1, 1, 0 /* 解析器必须拒绝，不能改掉旧进程。 */
airi_broken_elf_end: /* 失败输入恰好八字节。 */

.section .rodata /* 文件工作负载只在内核中保留映像字节。 */
.global airi_files_elf_start, airi_files_elf_end /* 用户空间由装载器另建。 */
airi_files_elf_start: /* 映像首地址。 */
.incbin AIRI_FILES_ELF /* 使用本配置实际链接的用户ELF。 */
airi_files_elf_end: /* 映像右端，不把后续字节算入文件。 */

.section .rodata /* 存储用户映像属于只读内核初始目录。 */
.global airi_storage_elf_start, airi_storage_elf_end /* 同一映像按argv选择storage或recover。 */
airi_storage_elf_start: /* 文件首。 */
.incbin AIRI_STORAGE_ELF /* 本配置真实链接的用户ELF。 */
airi_storage_elf_end: /* 文件半开右端。 */

.section .rodata /* 独立管道用户映像。 */
.global airi_pipes_elf_start, airi_pipes_elf_end /* 只发布文件边界。 */
airi_pipes_elf_start: /* 文件起点。 */
.incbin AIRI_PIPES_ELF /* 当前配置的真实ELF。 */
airi_pipes_elf_end: /* 文件右端。 */

.section .rodata /* 阻塞用户程序映像。 */
.global airi_waits_elf_start, airi_waits_elf_end /* 边界只供内核装载。 */
airi_waits_elf_start: /* 文件开始。 */
.incbin AIRI_WAITS_ELF /* 本配置独立ELF。 */
airi_waits_elf_end: /* 半开右端。 */

.section .rodata /* 用户异步工作负载与exec别名共用同一ELF。 */
.global airi_async_elf_start, airi_async_elf_end /* 初始映像边界。 */
airi_async_elf_start: /* 文件首字节。 */
.incbin AIRI_ASYNC_ELF /* 本配置真实链接的用户程序。 */
airi_async_elf_end: /* 文件半开右端。 */

.section .rodata /* 新程序仍只是内核只读段中的输入文件。 */
.global airi_shell_elf_start, airi_shell_elf_end /* shell装载边界。 */
airi_shell_elf_start: /* 完整文件头起点。 */
.incbin AIRI_SHELL_ELF /* 当前配置的真实用户shell。 */
airi_shell_elf_end: /* 半开右端。 */
.global airi_demo_elf_start, airi_demo_elf_end /* 贯通演示装载边界。 */
airi_demo_elf_start: /* 完整ELF起点。 */
.incbin AIRI_DEMO_ELF /* 当前配置的真实异步用户程序。 */
airi_demo_elf_end: /* 半开右端。 */
```

**增加两个独立用户ELF和包装依赖。** 末尾target-specific CPPFLAGS只给包装对象追加本配置ELF路径；默认串口仍不读取frontend。

图形目标才定义串口 shell 限制宏。

<!-- replace: Makefile -->
<!-- before -->
```make
CPPFLAGS := -Iinclude -Ithird_party
# 只有显式图形配置需要前端头文件。
ifneq ($(filter desktop-%,$(PROFILE)),)
CPPFLAGS += -Ifrontend/include # 为已选择的图形实现提供头文件搜索位置。
endif # 结束当前可选图形分支。
# Make条件只在no-framebuffer配置加入下方测试宏。
ifeq ($(CASE),no-framebuffer)
```
<!-- after -->
```make
CPPFLAGS := -Iinclude -Ithird_party
# 只有显式图形配置需要前端头文件。
ifneq ($(filter desktop-%,$(PROFILE)),)
CPPFLAGS += -DAIRI_GRAPHICAL_CONSOLE -Ifrontend/include # 为已选择的图形实现提供头文件搜索位置。
endif # 结束当前可选图形分支。
# Make条件只在no-framebuffer配置加入下方测试宏。
ifeq ($(CASE),no-framebuffer)
```

加入 sh/demo 独立目标与包装路径，默认串口不读前端。

<!-- replace: Makefile -->
<!-- before -->
```make
$(OUT)/user/async.elf: $(USER_COMMON) $(OUT)/user/async.o labs/airios/program.ld
# 仍为无宿主libc的静态ELF。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/async.o -o $@
```
<!-- after -->
```make
$(OUT)/user/async.elf: $(USER_COMMON) $(OUT)/user/async.o labs/airios/program.ld
# 仍为无宿主libc的静态ELF。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/async.o -o $@

# shell与贯通演示是独立的低地址用户ELF，不链接彼此main。
$(OUT)/user/%.o: labs/airios/%.c labs/airios/include/airi_libc.h include/airi/io_abi.h
# 与既有libc模式规则路径不同，Make按存在的源选择。
	@mkdir -p $(dir $@)
# 复用整数用户ABI，不借宿主运行时。
	$(CC) $(USER_FLAGS) -Ilabs/airios/include -c $< -o $@
# 两个新程序各自链接自有入口与库。
$(OUT)/user/sh.elf: $(USER_COMMON) $(OUT)/user/sh.o labs/airios/program.ld
# 每个目标包含且只包含自己的main。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/sh.o -o $@
# demo同样形成真正ELF，再作为初始装载输入。
$(OUT)/user/demo.elf: $(USER_COMMON) $(OUT)/user/demo.o labs/airios/program.ld
# 段权限与BSS由既有用户链接脚本定义。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/demo.o -o $@
# 目标额外依赖保证.incbin永远读取本配置已完成ELF。
$(OUT)/labs/airios/images.o: $(OUT)/user/sh.elf $(OUT)/user/demo.elf
# target-specific变量只对这个包装对象追加路径宏，单引号保护C字符串。
$(OUT)/labs/airios/images.o: CPPFLAGS += -DAIRI_SHELL_ELF='"$(OUT)/user/sh.elf"' -DAIRI_DEMO_ELF='"$(OUT)/user/demo.elf"'
```

**增加P6_AVAILABLE入口标记。** 它只说明shell/demo已注册，不能单独证明整个演示已完成。

增加 P6_AVAILABLE，只承诺入口存在。

<!-- replace: kernel/main.c -->
<!-- before -->
```c
    if (airi_storage_count() >= 0 && !airi_block_readonly()) // 可写正常设备已完成用户异步生命周期。
        airi_log("[AIRI_P5_READY] wait=blocked sq=4 cq=4 registered=1 lifecycle=reclaimed"); // Linux实验实测另记，不由guest标记代替。
    airi_serial_write("airi> ");  // 输出首个串口提示符。
    for (;;) {  // 没有退出条件，内核持续轮询输入。
        int key = airi_input_poll();  // 从虚拟键盘或 UART 取得一个共享键值，0 表示暂无输入。
        if (!key) {  // 没有新键时不处理命令和重画。
```
<!-- after -->
```c
    if (airi_storage_count() >= 0 && !airi_block_readonly()) // 可写正常设备已完成用户异步生命周期。
        airi_log("[AIRI_P5_READY] wait=blocked sq=4 cq=4 registered=1 lifecycle=reclaimed"); // Linux实验实测另记，不由guest标记代替。
    airi_serial_write("airi> ");  // 输出首个串口提示符。
    airi_log("[AIRI_P6_AVAILABLE] shell=sh demo=demo input=UART"); // 只声明入口可用，完整演示由实际shell测试验证。
    for (;;) {  // 没有退出条件，内核持续轮询输入。
        int key = airi_input_poll();  // 从虚拟键盘或 UART 取得一个共享键值，0 表示暂无输入。
        if (!key) {  // 没有新键时不处理命令和重画。
```

<a id="shell-verification"></a>
## 用UART输入与真实限速完成作者演示

这条“贯通路径”不是 shell 的管线语法；证据要同时落到用户输入、孩子身份、设备操作和父 shell 恢复。shell_test 先暂停 QEMU，连接UART后继续，等待P5完整就绪，再发送run sh。进入用户提示符后才对airi-disk设置1 IOPS，demo的USER_ALLOC给出真实PID；脚本要求OTHER_PROGRESS位于此PID一次CQ登记与result0唤醒之间，同时存在设备start/complete和PIPELINE_USER_OK。只有VM仍在运行或只有SUBMIT日志均不能通过。

随后撤掉限速，运行storage、recover、files，故意运行missing和broken，再运行hello证明失败后仍可继续。programs查询验证真实目录；80字符行必须整行丢弃，退格改正后的hellx加Delete再o和CRLF应运行hello。exit后等待内核monitor真正收拢会话，再echo确认UART仍工作。每次等待35秒，失败也保留串口日志，finally回收本测试QEMU；测试只重置自己8MiB盘，不碰交互目录。

**新建真实shell检查。** 不注入内核状态，不用宿主程序输出冒充ring3执行。

<!-- write: scripts/shell_test.py -->
```python
"""Drive the actual ring-3 shell through UART and observe a complete I/O path.""" # 不把宿主脚本当用户shell。
import json # 记录本轮真实结果。
from pathlib import Path # 短控制端点。
import re # 按实际PID关联等待和进展。
import socket # QEMU的UART双向字节流。
import subprocess # 只管理本测试子进程。
import tempfile # 避免UNIX路径长度上限。
import time # 单调总截止。
from boot import command, qmp_execute # 固定硬件与真实限速。
from tools import ROOT # 不依赖调用目录。
def check_shell(): # 默认串口主线，不读取frontend或字体。
    work=ROOT/'artifacts/shell-normal'; work.mkdir(parents=True,exist_ok=True) # 本测试独占目录。
    result_path=work/'result.json'; result_path.write_text('{"passed":false}\n') # 旧成功立即撤销。
    disk=work/'disk.raw' # 仅测试盘可重置，不动交互run的正式实验盘。
    if disk.is_symlink() or (disk.exists() and (not disk.is_file() or disk.stat().st_size!=8*1024*1024)): # 未知对象不替换。
        raise RuntimeError('Invalid shell-test disk') # 保留异常路径供检查。
    disk.unlink(missing_ok=True) # 下一command排他创建空盘。
    with tempfile.TemporaryDirectory(prefix='airios-sh-',dir='/tmp') as directory: # 本次端点随测试销毁。
        qmp=Path(directory)/'qmp.sock'; uart=Path(directory)/'uart.sock' # 控制与设备分开。
        cmd=command('normal',work); cmd[cmd.index('-serial')+1]=f'unix:{uart},server=on,wait=off' # 实际UART而非内存注入。
        cmd+=['-S','-qmp',f'unix:{qmp},server=on,wait=off'] # 连接UART后才继续CPU。
        (work/'command.json').write_text(json.dumps(cmd,indent=2)+'\n') # 固定本次QEMU参数。
        with (work/'qemu.log').open('w') as errors: # QEMU诊断单独保存。
            process=subprocess.Popen(cmd,stdout=errors,stderr=errors); wire=None; data=bytearray() # 保留真实累积字节。
            try: # 任何失败都回收自己的QEMU。
                deadline=time.monotonic()+10 # 等待端点不能无限。
                while not uart.exists(): # CPU还未开始启动。
                    if process.poll() is not None or time.monotonic()>deadline: raise RuntimeError('Shell UART unavailable') # 明确失败。
                    time.sleep(.05) # 给QEMU建立端点的机会。
                wire=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM); wire.connect(str(uart)); wire.setblocking(False) # 接收端在CPU之前准备好。
                qmp_execute(qmp,'cont') # 真实继续CPU。
                def output(): # 即使失败也保留已到达的日志。
                    while True: # 取到本轮暂时没有更多字节。
                        try: chunk=wire.recv(4096) # 流不保证一行一次到齐。
                        except BlockingIOError: break # 没字节不是EOF。
                        if not chunk: break # 进程状态由wait检查。
                        data.extend(chunk) # 保持串口真实顺序。
                    (work/'serial.log').write_bytes(data) # 及时落证据。
                    return data.decode(errors='replace') # 诊断不因单个字节丢失。
                def wait(marker,offset=0): # 只接受当前请求后已完整结束的记录。
                    end=time.monotonic()+35 # 每个操作有界。
                    while time.monotonic()<end: # 不重置超时预算。
                        text=output(); tail=text[offset:] # 旧启动输出不能满足新请求。
                        for line in tail.splitlines(keepends=True): # 半行不能被当成功。
                            record=line.removeprefix('airi> ').removesuffix('\n').removesuffix('\r') # 只移除已知提示符。
                            if line.endswith('\n') and (record==marker or (marker.endswith(' ') and record.startswith(marker))): return text # 明确完整记录。
                        if '[AIRI_PANIC]' in tail or process.poll() is not None: raise RuntimeError(f'Shell failed before {marker}') # 不把QEMU存活当成功。
                        time.sleep(.02) # 给真实设备与CPU推进。
                    raise RuntimeError(f'Shell timeout: {marker}; see {work}/serial.log') # 可定位失败证据。
                text=wait('[AIRI_P5_READY] ') # 完整内核启动后再进入用户shell。
                wire.sendall(b'run sh\n'); text=wait('[USER_SHELL_PROMPT]',len(text)) # 真正进入用户态shell。
                limits={'device':'airi-disk','bps':0,'bps_rd':0,'bps_wr':0,'iops':1,'iops_rd':0,'iops_wr':0} # 让物理完成晚于父等待。
                qmp_execute(qmp,'block_set_io_throttle',limits); (work/'throttle.json').write_text(json.dumps(limits)+'\n') # QMP接受后才记录。
                offset=len(text); wire.sendall(b'demo\n'); text=wait('[SHELL_COMMAND_DONE] name=demo code=0',offset) # 命令由shell解析、fork/exec/wait。
                tail=text[offset:]; match=re.search(r'\[USER_ALLOC\] pid=(\d+) ',tail) # 从实际新程序分配记录取得PID。
                assert match and '[PIPELINE_USER_OK]' in tail and '[EXEC_COMMIT]' in tail # 不用宿主复制或预期字符串替代运行。
                pid=match.group(1); progress=tail.index('[PIPELINE_OTHER_PROGRESS]'); blocked=False # 验证另一个用户在同一次CQ等待期间推进。
                for registered in re.finditer(r'\[WAIT_REGISTER\] pid='+pid+r' operation=cq\r?\n',tail): # 只匹配演示父进程。
                    wake=tail.find('[WAIT_WAKE] pid='+pid+' result=0\r\n',registered.end()) # 同PID真实结果交付。
                    blocked|=registered.start()<progress<wake # 不固定猜用户PID或时序。
                assert blocked and 'phase=start' in tail and 'phase=complete' in tail # 实际设备请求和完成均可见。
                qmp_execute(qmp,'block_set_io_throttle',dict(limits,iops=0)) # 后续同步日志不继承慢设备测试预算。
                for request,marker in [('storage','[STORAGE_USER_OK]'),('recover','[RECOVERY_USER_OK]'),('files','[FILES_USER_OK]'),('missing','exec failed: -2'),('broken','exec failed: -8'),('hello','[ELF_USER_OK]')]: # 正常、坏名字、坏ELF后仍可运行。
                    offset=len(text); wire.sendall((request+'\n').encode()); text=wait('[SHELL_COMMAND_DONE] name='+request+' ',offset) # 完整孩子退出才开始下一条。
                    assert marker in text[offset:] # 必须来自本条命令。
                offset=len(text); wire.sendall(b'programs\n'); text=wait('[USER_SHELL_PROMPT]',offset) # 用户查询来自真实注册目录。
                assert 'demo\r\n' in text[offset:] and 'async-reset\r\n' in text[offset:] # 不是内核监控台的静态帮助。
                offset=len(text); wire.sendall(b'x'*80+b'\n'); text=wait('[USER_SHELL_PROMPT]',offset) # 过长行必须整体丢弃。
                assert 'line too long; discarded' in text[offset:] and '[FORK]' not in text[offset:] # 不执行截断前缀。
                offset=len(text); wire.sendall(b'hellx\x7fo\r\n'); text=wait('[SHELL_COMMAND_DONE] name=hello code=0',offset) # 退格修正与CRLF真正走用户输入。
                offset=len(text); wire.sendall(b'exit\n'); text=wait('[CONSOLE_UPDATE] view=0 commands=1 page=0',offset) # shell退出后内核监控台才返回。
                assert '[USER_SHELL_EXIT]' in text[offset:] and 'program sh exited 0' in text[offset:] # 真实收拢页/FD/静态池由launch核对。
                offset=len(text); wire.sendall(b'echo after-shell\n'); text=wait('[CONSOLE_UPDATE] view=0 commands=2 page=0',offset) # 回到内核后输入仍完整。
                assert 'after-shell' in text[offset:] # 不只是退出而丢了后续UART。
                result={'passed':True,'ring3_shell':True,'ram_file_and_device_readback':True,'other_user_during_cq_wait':True,'qmp_iops':1,'error_exec_survived':True,'returned_to_monitor':True} # 只发布已观察事实。
                result_path.write_text(json.dumps(result,indent=2)+'\n'); print('PASS user shell: alloc/file/async wait/other task/CQ/readback, failed exec, return to monitor') # 阶段演示实际完成。
                return result # 总入口可收集。
            finally: # 包括断言、超时和QEMU错误。
                if wire is not None: wire.close() # 只关闭自己的设备连接。
                process.terminate() # 正常终止测试CPU，不称掉电。
                try: process.wait(timeout=5) # 清理也有期限。
                except subprocess.TimeoutExpired: process.kill(); process.wait() # 不遗留测试进程。
```

**目录检查改用真实注册表的表头。** 其他输入与GUI回归保持。

目录匹配改用实际注册表表头，后续输入不变。

<!-- replace: scripts/console_test.py -->
<!-- before -->
```python
                wire.sendall(b'mem\n') # 同一串口主线查询真实页池与堆。
                tail=wait(update(0,4),offset) # 等第四次提交的完整状态。
                assert 'page pool: total=' in tail and 'kernel heap: bytes=65536' in tail # 不能只返回启动地图。
                for number, text, expected in [(5, 'programs', 'programs: hello processes worker broken'), (6, 'run hello', 'program hello exited 0'), (7, 'run broken', 'cannot load broken: error=-8'), (8, 'run worker', 'program worker exited 23'), (9, 'run processes', 'program processes exited 0'), (10, 'run missing', 'program not found: missing'), (11, 'echo after-programs', 'after-programs')]: # 正常、坏ELF、非零退出和未知名称都不破坏后续会话。
                    offset=len(output()) # 当前命令只消费新日志。
                    wire.sendall((text + '\n').encode()) # 发送真正UART输入，而不是直接调用内核函数。
                    tail=wait(update(0,number),offset) # 程序会话全部收拢后才完成控制台命令。
```
<!-- after -->
```python
                wire.sendall(b'mem\n') # 同一串口主线查询真实页池与堆。
                tail=wait(update(0,4),offset) # 等第四次提交的完整状态。
                assert 'page pool: total=' in tail and 'kernel heap: bytes=65536' in tail # 不能只返回启动地图。
                for number, text, expected in [(5, 'programs', 'programs: registered ELF images'), (6, 'run hello', 'program hello exited 0'), (7, 'run broken', 'cannot load broken: error=-8'), (8, 'run worker', 'program worker exited 23'), (9, 'run processes', 'program processes exited 0'), (10, 'run missing', 'program not found: missing'), (11, 'echo after-programs', 'after-programs')]: # 正常、坏ELF、非零退出和未知名称都不破坏后续会话。
                    offset=len(output()) # 当前命令只消费新日志。
                    wire.sendall((text + '\n').encode()) # 发送真正UART输入，而不是直接调用内核函数。
                    tail=wait(update(0,number),offset) # 程序会话全部收拢后才完成控制台命令。
```

**默认test加入用户shell测试。** 图形test继续验证现有桌面，不把当前串口shell冒充图形TTY。

引入真实 shell 观察。

<!-- replace: scripts/manage.py -->
<!-- before -->
```python
from tools import ROOT, tool, make_command  # 复用路径、工具查找与构建参数，不另建工具选择规则。
from deps import prepare  # 复用外部依赖下载与校验，统一入口先调用它。
from boot import check, command  # check 等待启动完成，command 只组装交互QEMU参数。
from storage_test import check_storage_restart # 跨启动恢复与真实QMP节流下的CQ等待。
from console_test import check_console  # 复用真实键盘和UART输入验证。

```
<!-- after -->
```python
from tools import ROOT, tool, make_command  # 复用路径、工具查找与构建参数，不另建工具选择规则。
from deps import prepare  # 复用外部依赖下载与校验，统一入口先调用它。
from boot import check, command  # check 等待启动完成，command 只组装交互QEMU参数。
from shell_test import check_shell # 真正用户shell与端到端存储演示。
from storage_test import check_storage_restart # 跨启动恢复与真实QMP节流下的CQ等待。
from console_test import check_console  # 复用真实键盘和UART输入验证。

```

普通串口 test 才运行 shell，图形尚无用户 TTY。

<!-- replace: scripts/manage.py -->
<!-- before -->
```python
        prefix = 'desktop-' if args.graphics else ''  # 本轮输入验证选择同一呈现方式。
        console_results=[check_console(prefix+'normal'),check_console(prefix+'no-framebuffer'),check_console(prefix+'normal',uart_input=False)]  # 顺序验证normal UART、无帧缓冲UART与normal PS/2三条真实输入路径。
        (ROOT/'artifacts'/('desktop-console-tests.json' if args.graphics else 'console-tests.json')).write_text(json.dumps(console_results,indent=2)+'\n')  # 保存三次实际输入验证结果。
        check_storage_restart('desktop-normal' if args.graphics else 'normal') # 正式日志双启动恢复，另以第三个guest验证慢设备CQ等待。
    else:  # 普通build/run/debug共用指定配置构建路径。
        build(args.profile)  # build/run/debug先构建调用者选择的配置。
```
<!-- after -->
```python
        prefix = 'desktop-' if args.graphics else ''  # 本轮输入验证选择同一呈现方式。
        console_results=[check_console(prefix+'normal'),check_console(prefix+'no-framebuffer'),check_console(prefix+'normal',uart_input=False)]  # 顺序验证normal UART、无帧缓冲UART与normal PS/2三条真实输入路径。
        (ROOT/'artifacts'/('desktop-console-tests.json' if args.graphics else 'console-tests.json')).write_text(json.dumps(console_results,indent=2)+'\n')  # 保存三次实际输入验证结果。
        if not args.graphics: # 当前用户shell只独占串口，图形终端尚无用户TTY接入。
            check_shell() # 同一核心构建实际执行用户解析、fork/exec/wait和慢设备I/O。
        check_storage_restart('desktop-normal' if args.graphics else 'normal') # 正式日志双启动恢复，另以第三个guest验证慢设备CQ等待。
    else:  # 普通build/run/debug共用指定配置构建路径。
        build(args.profile)  # build/run/debug先构建调用者选择的配置。
```

<!-- run -->
```sh
./airios test # 主机边界、九种启动、输入、真实shell与三次I/O会话都有实际失败上界。
```

手动运行：

```sh
./airios run # 当前终端连接UART；Ctrl-A X退出QEMU。
```

依次输入run sh、programs、demo、storage、recover、missing、hello、exit，再在内核提示符输入echo after-shell。demo应有USER_ALLOC、IO_TRACE、PIPELINE_CONTINUE和PIPELINE_USER_OK，shell按真实退出码给出code=0。普通未限速设备的等待/切换数量允许变化，不要求人为制造每次阻塞。

2026-10-16 的既有运行中，作者实际观察到了父CQ等待区间内的另一个用户进展，设备与RAM文件512字节读回正确，失败exec后可继续，shell退出后monitor仍可输入。完整回归与独立重建结果绑定在.codex/teaching/validation/K33.json，详细串口在artifacts/shell-normal/serial.log。Linux P6 的固定负载对照在 L08，不把guest成功代替Linux运行。

现在主线已连接启动、用户程序、分配、文件、等待、设备完成和继续运行。后续可独立选择图形TTY、WebUI宿主网关、设备中断完成或开发板；当前没有网络栈、POSIX shell、多核或通用桌面进程服务。
