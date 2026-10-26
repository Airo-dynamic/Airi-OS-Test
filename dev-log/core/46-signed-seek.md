# K46 · 在共享打开对象内完成有符号定位

从 K45 继续。文件与显式偏移向量已有真实路径，K44的标准lseek却还只能SEEK_SET，CUR/END返回ENOSYS。现在扩展三种基准，允许合法的负差值，并保留失败后旧位置。原14号绝对定位接口继续存在，新的37号调用承接标准源码语义。

沿同一libc用户程序观察：读取几个字节后向前退一字节，从文件尾退一字节，定位到尾后读EOF，再实际写入扩大文件。定位本身不扩长度，写入产生的空洞要清零；INT64_MIN等差值不能让C代码溢出或改坏共享偏移。

<a id="seek-object"></a>
## 三种基准属于同一个打开对象

[定位手册](https://man7.org/linux/man-pages/man2/lseek.2.html)区分SET、CUR和END：分别以0、当前打开对象的偏移和文件当前内容长度为基准，加有符号差值形成新位置。这里沿用这三种关系，最终位置仍限于当前RAM模型的0至1024，不提供稀疏磁盘文件或SEEK_DATA/HOLE。

打开对象持有offset，文件节点持有size，容量1024又是第三个数值。假设内容300字节、当前位置3：SEEK_CUR的−1得到2，SEEK_END的−1得到299；SEEK_END的10得到310，但size仍为300，此时read返回EOF。只有在302实际写一个Q，size才变303，中间两个未写字节变零。

不能在用户库先查当前位置，再另发绝对定位。dup/fork可以让不同执行者引用同一打开对象，两次调用之间别人可能推进offset。当前单CPU内核将取基准、验证和提交放在一个文件层函数，用户库只发一次调用；以后加入SMP还需给共享对象加相应同步，不能由此直接推定多核安全。

先在file.h增加airi_fd_seek_from，旧airi_fd_seek保持绝对语义。新参数delta是有符号64位，whence0/1/2由文件对象所有者解释。

<details>
<summary>include/airi/file.h 的完整终点</summary>

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
long airi_fd_seek_from(struct airi_fd_table *table, unsigned fd, int64_t delta, unsigned whence); // SET/CUR/END在同一打开对象内取基准、检查并提交。
long airi_fd_read(struct airi_fd_table *table, unsigned fd, void *buffer, size_t bytes, bool positioned, uint64_t offset); // 输出是内核副本。
long airi_fd_write(struct airi_fd_table *table, unsigned fd, const void *buffer, size_t bytes, bool positioned, uint64_t offset); // 输入不被异步保留。
bool airi_fd_console(const struct airi_fd_table *table, unsigned fd); // 终端发送由设备适配完成。
long airi_fd_pipe(struct airi_fd_table *table, unsigned ends[2], bool nonblocking); // 原子创建两个FD和有界共享缓冲，失败不改输出。
long airi_fd_ready(const struct airi_fd_table *table, unsigned fd); // 本阶段只查询管道当前条件，不订阅事件。
bool airi_fd_nonblocking(const struct airi_fd_table *table, unsigned fd); // 后续等待层据此决定是否可阻塞。
unsigned airi_files_open_count(void); // 不含固定终端对象，用于会话回收检查。
bool airi_fd_input(const struct airi_fd_table *table, unsigned fd); // 判断固定终端输入，设备读取在陷入层。
bool airi_fd_attach_input(struct airi_fd_table *table); // 仅空FD0可接入CLOEXEC输入；已有表项时不改变引用。
#endif // 结束保护。
```

</details>

<a id="seek-arithmetic"></a>
## 最小负数不能直接取负，失败先不动对象

计算 `base + delta` 看似只有一行，却不能直接在有符号整数上做任意加法。极端输入可能溢出；`-INT64_MIN`也超出正数范围，在C里不能用它求绝对值。

以刚才当前位置3向前退一字节为例，base为3、delta为−1，需要先得到退回幅度1，确认它不大于base，再用无符号减法得到2。幅度若大于base，最终位置会小于0，此时直接返回EINVAL，原位置不动。

为了让同一条路径也能处理 INT64_MIN，幅度写成 `(uint64_t)(-(delta+1))+1`：先加1得到可取负的−9223372036854775807，取负并转为uint64_t后，再补1得到2^63。全过程没有对最小负数直接取负。非负方向则先检查 delta 不超过1024减base，通过后才做无符号相加；两条路径最后都得到容量内的位置，才允许提交。

所有错误分支都在写file->offset之前。合法结果形成后只提交一次，dup别名和fork引用一起看到新位置；size在此不改。坏FD返回EBADF，UART和管道没有可定位节点，返回ESPIPE。完整更新file.c，新函数位于旧绝对seek之后，原读写及引用逻辑保持。

在原绝对定位之后、transfer 之前加入有符号定位。原 seek 的行为保持，新增函数自己完成基准读取、验证与一次提交。

本章每组替换的第一块只用于定位旧文本，敲入第二块替换它，不把两块同时加入文件。各组按正文顺序完成。

<!-- replace: kernel/fs/file.c -->
<!-- before -->
```c
    file->offset = offset; // 所有dup/fork引用一起观察新偏移。
    return (long)offset; // 返回定位后的偏移。
}
static long transfer(struct airi_fd_table *table, unsigned fd, void *buffer, size_t bytes, bool write, bool positioned, uint64_t offset) { // 这里只操作内核缓冲，不验证用户指针。
    struct airi_open_file *file = get(table, fd); // 当前表里的整数转内部对象。
    if (!file || !(file->flags & (write ? AIRI_OPEN_WRITE : AIRI_OPEN_READ))) // 错误访问模式也返回EBADF。
```
<!-- after -->
```c
    file->offset = offset; // 所有dup/fork引用一起观察新偏移。
    return (long)offset; // 返回定位后的偏移。
}
long airi_fd_seek_from(struct airi_fd_table *table, unsigned fd, int64_t delta, unsigned whence) { // 一次内核操作读取并更新共享打开对象位置。
    struct airi_open_file *file = get(table, fd); // 不把FD整数当文件地址。
    if (!file) // 关闭或越界表项无目标。
        return -9; // -EBADF，旧对象不变。
    if (!file->node) // 管道、UART没有可定位的RAM节点。
        return -29; // -ESPIPE，不伪造流位置。
    if (whence > 2) // 0/1/2分别SET/CUR/END，未知值拒绝。
        return -22; // -EINVAL。
    uint64_t base = whence == 0 ? 0 : whence == 1 ? file->offset : file->node->size; // END使用内容长度，不是预留容量或当前偏移。
    uint64_t next; // 只在完整合法后才写回共享对象。
    if (delta >= 0) { // 非负方向先确认容量差，不做可能回绕的有符号相加。
        if ((uint64_t)delta > FILE_BYTES - base) // 当前base本身始终位于0至1024。
            return -22; // 超出本轮有界文件定位范围。
        next = base + (uint64_t)delta; // 已确认结果在显式上限内。
    } else { // 负偏移不能直接对INT64_MIN取负。
        uint64_t magnitude = (uint64_t)(-(delta + 1)) + 1; // 先加1再取负，提升无符号后补1，完整表达2^63。
        if (magnitude > base) // 最终位置将小于0。
            return -22; // 原共享偏移保持。
        next = base - magnitude; // 只有合法的向前回退。
    }
    file->offset = next; // 单核不可抢占内核中一次提交，dup/fork引用一起观察。
    return (long)next; // 定位不扩文件，只有后续写入才会改变size并清零洞。
}
static long transfer(struct airi_fd_table *table, unsigned fd, void *buffer, size_t bytes, bool write, bool positioned, uint64_t offset) { // 这里只操作内核缓冲，不验证用户指针。
    struct airi_open_file *file = get(table, fd); // 当前表里的整数转内部对象。
    if (!file || !(file->flags & (write ? AIRI_OPEN_WRITE : AIRI_OPEN_READ))) // 错误访问模式也返回EBADF。
```

<a id="seek-boundary"></a>
## 原始位模式到标准有符号差值

user.c增加37号分支，先检查完整FD和whence，防止高位被窄化后变成另一项。RSI保存delta的完整64位二补数位模式，在当前固定x86-64目标转换为int64_t后交文件层。它不需要用户指针，也不在用户库查询size或offset。

旧14号调用仍接无符号绝对偏移，已有airi_seek及旧教程检查点继续成立。标准lseek改用新入口，不把“负参数”一概判错；SEEK_SET负结果自然被文件层拒绝，CUR/END的负差值则可能合法。

接下来按现有 `kernel/process/user.c` 中的位置逐组替换，未列出的前章内容保持。所有映像、用户库与构建接入完成后再运行。

在35/36号向量调用之后、未知调用拒绝之前插入37号分支。完整 FD 和 whence 检查通过以后，才把有符号差值交给文件对象。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
        f->rax = current->fs_base; // 低半区基址可由long无歧义表示，不返回内核地址。
    else if (f->rax == 35 || f->rax == 36) // 自有显式偏移向量操作，R10保存第四参数。
        f->rax = (uint64_t)vector_transfer(current, f->rdi, f->rsi, f->rdx, f->r10, f->rax == 36); // 描述和前缀在调用内完成，不建立异步借用。
    else // 尚未提供的接口都明确拒绝。
        f->rax = (uint64_t)-38ll; // -ENOSYS。
    poll_waiters(); // 文件/流修改完成后立即重查其他等待者，不等用户轮询。
```
<!-- after -->
```c
        f->rax = current->fs_base; // 低半区基址可由long无歧义表示，不返回内核地址。
    else if (f->rax == 35 || f->rax == 36) // 自有显式偏移向量操作，R10保存第四参数。
        f->rax = (uint64_t)vector_transfer(current, f->rdi, f->rsi, f->rdx, f->r10, f->rax == 36); // 描述和前缀在调用内完成，不建立异步借用。
    else if (f->rax == 37) { // 有符号相对定位仍由打开对象的所有者一次完成。
        if (f->rdi >= AIRI_FD_COUNT) // 完整64位编号不能截成另一个合法FD。
            f->rax = (uint64_t)-9ll; // -EBADF。
        else if (f->rdx > 2) // 原始调用也不能靠窄化whence绕过检查。
            f->rax = (uint64_t)-22ll; // -EINVAL。
        else // RSI位模式在当前二补数x86-64目标解释为有符号偏移。
            f->rax = (uint64_t)airi_fd_seek_from(&current->files, (unsigned)f->rdi, (int64_t)f->rsi, (unsigned)f->rdx); // 一次读取基准、验证并提交，旧14号绝对接口保留。
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
#endif // 结束包含保护。
```

</details>

在 `labs/airios/libc/syscall.c` 末尾追加以下内容，前章已有部分保留。

<!-- append: labs/airios/libc/syscall.c -->
```c

long airi_seek_from(unsigned fd, int64_t delta, unsigned whence) { // 有符号差值按完整64位二补数传入RSI。
    return call3(37, fd, (uint64_t)delta, whence); // 基准由内核在同一打开对象上决定，不做用户侧查询加定位。
}
```

更新unistd.h中三种基准与lseek声明的范围说明。posix.c只在本地拒绝未知whence，再将参数交新原始调用，使用已有result转换错误；成功位置仍不清errno。原来的CUR/END返回ENOSYS分支删除。

<details>
<summary>labs/airios/include/unistd.h 的完整终点</summary>

<!-- write: labs/airios/include/unistd.h -->
```c
#ifndef AIRI_UNISTD_H // 有界POSIX风格用户接口，不读取Mac/Linux宿主头。
#define AIRI_UNISTD_H // 包含保护。
#include <sys/types.h> // size_t、ssize_t、off_t和pid_t属于当前目标。
#define STDIN_FILENO 0 // 当前前台UART输入表项。
#define STDOUT_FILENO 1 // UART输出表项。
#define STDERR_FILENO 2 // 同一固定UART输出对象。
#define SEEK_SET 0 // 以文件起点0为基准。
#define SEEK_CUR 1 // 以共享打开对象的当前位置为基准。
#define SEEK_END 2 // 以当前内容长度为基准，定位本身不扩文件。
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
off_t lseek(int fd, off_t offset, int whence); // SET/CUR/END支持有符号差值，最终位置仍限0至1024。
pid_t getpid(void); // 返回真实身份，非槽下标。
void _exit(int code) __attribute__((noreturn)); // 不运行stdio刷新或析构，直接结束当前用户。
#ifdef __cplusplus // 结束函数声明的C链接范围。
} // 类型与宏不需要语言包装。
#endif // 结束语言差异。
#endif // 结束包含保护。
```

</details>

<details>
<summary>labs/airios/libc/posix.c 的完整终点</summary>

<!-- write: labs/airios/libc/posix.c -->
```c
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
```

</details>

<a id="seek-observe"></a>
## 共享引用、极值和真实零洞分别观察

更新file_test.c，使用同一只读/seed在END−1后读到f，再用CUR回退。dup别名推进后原FD查询也应得到新位置。INT64_MIN、INT64_MAX、未知whence和最终负位置都拒绝，旧offset保持；定位1024允许但不扩size，下一步越界仍拒绝，END仍返回原内容长度6。ASan/UBSan同时检查这段有符号运算没有未定义行为。

宿主检查只操作模型中的对象，不替代用户入口。posix-probe另在guest通过标准lseek观察文件300字节尾、向后读EOF、实际写入后size303和零洞，以及极值失败后当前位置303仍在。原来的“CUR未实现”观察改为这些新行为，旧K44正文及快照保留当时的接口。

先在文件检查中接入这组定位观察，再更新下面的用户程序。

在原文件与管道检查之后，使用新表打开同一 /seed，接入共享偏移、极值和内容长度不变的观察。

<!-- replace: tests/unit/file_test.c -->
<!-- before -->
```c
    airi_fd_close_all(&child,false); airi_fd_close_all(&parent,false); // 最后输入引用释放不能清空固定对象配置。
    airi_fd_init(&parent); assert(airi_fd_attach_input(&parent)); // 再次启动shell可重新使用相同输入对象。
    assert(airi_fd_input(&parent,0)); airi_fd_close_all(&parent,false); // 重复会话完整回收。
    puts("PASS RAM files/pipes: offsets, fork/exec references, sparse zero, short I/O, FIFO wrap, endpoint lifetime, exhaustion"); // 全部边界通过才发布。
    return 0; // 宿主测试成功。
}
```
<!-- after -->
```c
    airi_fd_close_all(&child,false); airi_fd_close_all(&parent,false); // 最后输入引用释放不能清空固定对象配置。
    airi_fd_init(&parent); assert(airi_fd_attach_input(&parent)); // 再次启动shell可重新使用相同输入对象。
    assert(airi_fd_input(&parent,0)); airi_fd_close_all(&parent,false); // 重复会话完整回收。
    airi_fd_init(&parent); // 单独验证一次基准读取与共享位置提交。
    assert(airi_fd_open(&parent, "/seed", AIRI_OPEN_READ) == 0); // 内容长度6，偏移0。
    assert(airi_fd_seek_from(&parent, 0, -1, 2) == 5 && airi_fd_read(&parent, 0, data, 1, false, 0) == 1 && data[0] == 'f'); // END使用size而非容量。
    assert(airi_fd_seek_from(&parent, 0, -2, 1) == 4); // 当前6回退2为4。
    int alias = (int)airi_fd_dup(&parent, 0, false); // 同一对象的新引用。
    assert(alias >= 0 && airi_fd_seek_from(&parent, (unsigned)alias, 1, 1) == 5 && airi_fd_seek_from(&parent, 0, 0, 1) == 5); // dup任一表项更新都反映在共享字段。
    assert(airi_fd_seek_from(&parent, 0, INT64_MIN, 1) == -22 && airi_fd_seek_from(&parent, 0, INT64_MAX, 1) == -22); // 极值差不做未定义有符号运算。
    assert(airi_fd_seek_from(&parent, 0, -7, 2) == -22 && airi_fd_seek_from(&parent, 0, 0, 99) == -22 && airi_fd_seek_from(&parent, 0, 0, 1) == 5); // 所有失败保持原5。
    assert(airi_fd_seek_from(&parent, 0, 1024, 0) == 1024 && airi_fd_seek_from(&parent, 0, 1, 1) == -22 && airi_fd_seek_from(&parent, 0, 0, 2) == 6); // 最终位置上限与内容长度分别维护，定位不扩size。
    assert(airi_fd_seek_from(&parent, 1, 0, 0) == -29 && airi_fd_seek_from(&parent, AIRI_FD_COUNT, 0, 0) == -9); // 终端不可定位与坏FD分开。
    airi_fd_close_all(&parent, false); assert(!airi_files_open_count()); // 新观察不泄漏打开对象。
    puts("PASS RAM files/pipes: offsets/signed seek, fork/exec references, sparse zero, short I/O, FIFO wrap, endpoint lifetime, exhaustion"); // 全部边界通过才发布。
    return 0; // 宿主测试成功。
}
```

<details>
<summary>labs/airios/posix-probe.c 的完整终点</summary>

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
    if (pread(fd, output, 1, -1) != -1 || errno != EINVAL || lseek(fd, 0, SEEK_CUR) != 3 || lseek(fd, -1, SEEK_CUR) != 2) // 显式读偏移不能为负，当前位置的合法负差值却应成功。
        return 236; // 参数的意义不同，不能统一拒绝所有负数。
    if (lseek(fd, -1, SEEK_END) != 299 || read(fd, output, 1) != 1 || output[0] != input[299] || lseek(fd, 10, SEEK_END) != 310 || read(fd, output, 1) != 0 || lseek(fd, 0, SEEK_END) != 300) // 文件尾、定位后EOF与不扩文件分别观察。
        return 252; // 容量1024不能当作当前内容尾。
    if (lseek(fd, 302, SEEK_SET) != 302 || write(fd, "Q", 1) != 1 || pread(fd, output, 3, 300) != 3 || output[0] || output[1] || output[2] != 'Q') // 真正写入才扩size并清零两字节洞。
        return 253; // 不允许暴露原来未写的旧内容。
    if (lseek(fd, -304, SEEK_END) != -1 || errno != EINVAL || lseek(fd, INT64_MIN, SEEK_CUR) != -1 || errno != EINVAL || lseek(fd, INT64_MAX, SEEK_CUR) != -1 || errno != EINVAL || lseek(fd, 0, 99) != -1 || errno != EINVAL || lseek(fd, 0, SEEK_CUR) != 303) // 最小/最大差值与非法基准不能修改旧位置。
        return 254; // 不在有符号加法或对最小整数取负中产生UB。
    airi_puts("[LIBC_SEEK_OK] set=1 cur=1 end=1 signed=checked hole=zero failure=unchanged\n"); // 新语义由guest实际核对。
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

</details>

shell_test在真实libc命令的输出中额外要求LIBC_SEEK_OK，再继续原短I/O、错误槽和成功exec检查；其他用户、输入、服务和设备路径保持完整回归。

在本次 libc_output 中额外要求 LIBC_SEEK_OK，其余 I/O、errno 与 exec 检查保留。

<!-- replace: scripts/shell_test.py -->
<!-- before -->
```python
                assert 'ELF TLS size or alignment is unsupported' in ctls_output # 超限真实样本必须在装载器拒绝。
                offset=len(text); wire.sendall(b'libc\n'); text=wait('[SHELL_COMMAND_DONE] name=libc code=0',offset) # 用户库转换真实内核结果，errno来自编译器TLS。
                libc_output=text[offset:] # 不使用旧映像或启动期输出。
                assert '[LIBC_IO_OK] short=256 pipe=64 eof=zero offset=preserved bad_buffer=unchanged' in libc_output # 正常、短操作和坏缓冲区均由guest验证。
                assert '[LIBC_ERRNO_OK] initial=0 fork=copied preempt=private success=preserved exec=reset' in libc_output and '[LIBC_EXEC_RESET_OK] errno=zero cloexec=closed' in libc_output # 同一身份成功exec重置错误并关闭表项。
                offset=len(text); wire.sendall(b'vector\n'); text=wait('[SHELL_COMMAND_DONE] name=vector code=0',offset) # 散布缓冲必须由真实内核边界校验后完成一次文件操作。
```
<!-- after -->
```python
                assert 'ELF TLS size or alignment is unsupported' in ctls_output # 超限真实样本必须在装载器拒绝。
                offset=len(text); wire.sendall(b'libc\n'); text=wait('[SHELL_COMMAND_DONE] name=libc code=0',offset) # 用户库转换真实内核结果，errno来自编译器TLS。
                libc_output=text[offset:] # 不使用旧映像或启动期输出。
                assert '[LIBC_SEEK_OK] set=1 cur=1 end=1 signed=checked hole=zero failure=unchanged' in libc_output # 相对定位、极值失败和真实零洞均由guest观察。
                assert '[LIBC_IO_OK] short=256 pipe=64 eof=zero offset=preserved bad_buffer=unchanged' in libc_output # 正常、短操作和坏缓冲区均由guest验证。
                assert '[LIBC_ERRNO_OK] initial=0 fork=copied preempt=private success=preserved exec=reset' in libc_output and '[LIBC_EXEC_RESET_OK] errno=zero cloexec=closed' in libc_output # 同一身份成功exec重置错误并关闭表项。
                offset=len(text); wire.sendall(b'vector\n'); text=wait('[SHELL_COMMAND_DONE] name=vector code=0',offset) # 散布缓冲必须由真实内核边界校验后完成一次文件操作。
```

全部修改完成后运行：

<!-- run -->
```sh
./airios build # 新37号调用承接有符号差值，旧绝对入口保留。
./airios test # 宿主运算边界与真实用户定位、原有服务和I/O一起运行。
```

Mac默认仍是旧Python时给命令前加 `PATH=/opt/homebrew/bin:$PATH`。交互 `./airios run`，输入 `run sh`，再输入libc，新增观察先于原I/O成功行：

```text
[LIBC_SEEK_OK] set=1 cur=1 end=1 signed=checked hole=zero failure=unchanged
[LIBC_IO_OK] short=256 pipe=64 eof=zero offset=preserved bad_buffer=unchanged
```

SEEK_END此处得到的是RAM节点的内容长度，不是磁盘大小；位置310的EOF也不表示已存在310字节内容。seek只改变打开对象偏移，既不预分配页、不写数据，也不执行fsync。后续真正文件系统还要分别实现容量、稀疏分配与持久化语义。

到此，源程序可以用三种基准定位，错误转换与向量前缀继续保持。线程、futex、完整用户堆和C++运行时仍在后续，Linux实验与真实板状态另行记录。

