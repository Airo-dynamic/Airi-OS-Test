# K45 · 将分散缓冲区交给同一次显式偏移 I/O

从 K44 继续。read/pwrite 已保留错误与实际字节数，但数据常分在几个对象里：一个请求头、一段内容、一个校验尾。为了写一次文件，调用方不必先把所有内容拼进自己的一块连续数组。BOSS 的向量写路径也会遇到这种描述数组与缓冲区寿命问题。

本节沿 `sh → vector → pwritev → preadv → 坏后项 → sh` 提供显式偏移的向量操作。用户传最多八个地址/长度记录，内核先复制描述，验证本次前缀，之后聚合或分散；文件层只调用一次。仍是同步、单CPU、RAM文件，实际一次最多256字节，不是零拷贝或异步注册。

当前不提供阻塞流的 readv/writev。它们需要在阻塞期间持有描述与输入快照，不能借本节局部栈跨调度。已有普通read/write和管道等待保持，显式偏移向量对流明确返回ESPIPE。

<a id="vector-prefix"></a>
## 向量描述缓冲区，返回值描述串联前缀

iovec 每项有一个用户地址和字节长度，数组顺序定义逻辑串联顺序。它既不要求各段地址相邻，也不描述物理页。pwritev将各段按序聚合写入，preadv从同一连续文件范围读出后按序分散；显式offset不改变共享文件位置，见[向量I/O手册](https://man7.org/linux/man-pages/man2/readv.2.html)。本实现保留这些关系，缩小项数和单次字节预算。

例如输入为 `{ "ab",2 }, { "CDE",3 }, { "f",1 }`，一次pwritev在offset0写出abCDEf。随后在offset1读取到容量2、4、1的三个目标，只剩5字节：第一段得到bC，第二段前三字节得到DEf，第二段最后一字节及第三段保持原值。返回的是总数5，不是每段长度，也不是完成项数。

当逻辑总长400而本轮预算256时，当前只访问串联前256字节。前200字节位于第一项，余下56位于第二项；调用方据返回值调整后续描述继续，内核不自动循环写满。所有描述记录仍须完整可读，总长不得超出有符号返回类型；256以外未触及的数据地址不会检查或访问。这是当前有界接口的明确前缀策略。

<a id="vector-copy-check"></a>
## 先复制全部描述，再检查本次数据前缀

沿刚才的三项目标读取看两层内存：iov 指向地址/长度记录，各条记录才指向接收 bC、DEf 的数据范围。即使 iov 非空，也不能据此认定整份描述可读，更不能认定三个目标可写。内核先把最多八项、至多128字节描述完整复制到自己的局部数组，再解释这些地址数值。

这份快照在本次同步调用中保持。若某个目标恰好覆盖用户的描述数组，分散文件内容会改掉那份用户字节，但不会改掉内核正在使用的地址和长度。接下来还必须检查本次实际会触及的数据范围；复制描述本身没有完成这些检查。

总长使用“本项长度不超过INT64_MAX减已有总量”在加法前检查，避免溢出成一个小请求。零长项没有要访问的字节，不检查其无意义基址；count0不读取描述，但文件层仍检查FD和定位能力。

在调用文件层之前，先检查本次至多256字节覆盖的全部用户范围。写文件时要求来源可读，读文件时要求目标可写；每段都逐页验证。不允许前一项已经写文件，才发现后一项是内核高地址。本节相关失败策略是整次前缀无副作用，不能用逐项调用pwrite模拟。

新建 vector_abi.h，原始ABI每项固定两个uint64_t。这里地址字段只是当前用户VA的数值，外部协议不进入文件对象或设备结构。

<!-- write: include/airi/vector_abi.h -->
```c
#ifndef AIRI_VECTOR_ABI_H // 显式偏移向量I/O的固定字段，不含内核指针。
#define AIRI_VECTOR_ABI_H // 包含保护。
#include <stdint.h> // 两端都以64位数字解释地址和长度。
#define AIRI_VECTOR_COUNT 8u // 一次最多八项，描述数组可完整复制到有界内核栈。
#define AIRI_VECTOR_BYTES 256u // 一次只处理串联数据的前256字节，不隐式循环。
struct airi_iovec { uint64_t address, bytes; }; // 地址是当前用户VA，描述先复制，不跨返回借用。
_Static_assert(sizeof(struct airi_iovec) == 16, "vector ABI size"); // 固定每项两个8字节字段。
#endif // 结束包含保护。
```

<a id="vector-kernel"></a>
## 一份局部缓冲，只有一次文件层操作

在user.c的普通文件传输之后加入vector_transfer。它持有最多128字节描述和256字节数据，生命周期只到同步调用返回。完成全部前缀验证后，pwritev才按序复制输入到这份内核缓冲，调用一次airi_fd_write；preadv调用一次airi_fd_read，再仅分散实际返回数。

实际文件层已检查FD访问模式、文件容量和流不可定位语义。它可能在EOF或容量上限返回比请求更短的前缀；分散时再次用剩余实际数量限制每项，不用零覆盖未完成尾部。内核当前不可抢占、用户进程单线程，已验证映射不会被另一线程同时解除；以后共享地址空间和可抢占I/O需要额外固定页或保存快照机制。

加入调用35/36，RDI为FD、RSI为描述数组地址、RDX为数量、R10为显式offset。64位值都先限制后窄化，不能把超大数量截断成八项以内。该路径不登记等待，流返回ESPIPE；没有把用户地址直接送给VirtIO，也没有在返回后保留局部buffer。

这条同步路径还要接入35/36号分派，稍后的 vector 程序再通过同一个初始目录启动。下面修改这些连接，原进程、TLS、FD和异步机制保持。

接下来按现有 `kernel/process/user.c` 中的位置逐组替换，未列出的前章内容保持。所有映像、用户库与构建接入完成后再运行。

先接入已经写好的固定描述结构。

本章每组替换的第一块只用于定位旧文本，敲入第二块替换它，不把两块同时加入文件。各组按正文顺序完成。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
#include <airi/tls.h> // 每次真实用户陷入取回FS，内核处理期间清零。
#include <airi/fpu.h> // 每进程拥有一份非用户映射的浮点/SSE状态。
#include <airi/async.h> // 进程轮次与固定缓冲资源的生命周期。
```
<!-- after -->
```c
#include <airi/vector_abi.h> // 向量描述在一次内核调用内完整复制，文件操作前检查本次前缀。
#include <airi/tls.h> // 每次真实用户陷入取回FS，内核处理期间清零。
#include <airi/fpu.h> // 每进程拥有一份非用户映射的浮点/SSE状态。
#include <airi/async.h> // 进程轮次与固定缓冲资源的生命周期。
```

声明 vector 观察程序的映像边界。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
extern const uint8_t airi_user_rsp_bad_start[], airi_user_rsp_bad_end[]; // 不规范用户返回栈。
extern const uint8_t airi_hello_elf_start[], airi_hello_elf_end[]; // 既有独立用户C程序。
extern const uint8_t airi_process_elf_start[], airi_process_elf_end[]; // 新的fork/wait和抢占工作负载。
extern const uint8_t airi_posix_elf_start[], airi_posix_elf_end[]; // 显式链接errno与POSIX风格文件包装。
extern const uint8_t airi_ctls_elf_start[], airi_ctls_elf_end[]; // 编译器_Thread_local与静态模板。
extern const uint8_t airi_ctls_char_elf_start[], airi_ctls_char_elf_end[]; // 单字节小对齐模板。
```
<!-- after -->
```c
extern const uint8_t airi_user_rsp_bad_start[], airi_user_rsp_bad_end[]; // 不规范用户返回栈。
extern const uint8_t airi_hello_elf_start[], airi_hello_elf_end[]; // 既有独立用户C程序。
extern const uint8_t airi_process_elf_start[], airi_process_elf_end[]; // 新的fork/wait和抢占工作负载。
extern const uint8_t airi_vector_elf_start[], airi_vector_elf_end[]; // 显式偏移向量正常与坏范围观察。
extern const uint8_t airi_posix_elf_start[], airi_posix_elf_end[]; // 显式链接errno与POSIX风格文件包装。
extern const uint8_t airi_ctls_elf_start[], airi_ctls_elf_end[]; // 编译器_Thread_local与静态模板。
extern const uint8_t airi_ctls_char_elf_start[], airi_ctls_char_elf_end[]; // 单字节小对齐模板。
```

将 vector 名称接入初始目录，稍后的用户程序仍通过真实 ELF 装载。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
static const struct executable executables[] = { // 每份映像字节由内核只读段长期持有。
    {"sh", airi_shell_elf_start, airi_shell_elf_end}, // 独占串口输入的可选用户程序。
    {"init", airi_init_elf_start, airi_init_elf_end}, // 管理前台shell的常驻用户进程。
    {"libc", airi_posix_elf_start, airi_posix_elf_end}, // 用户库错误与文件结果转换。
    {"libc-reset", airi_posix_elf_start, airi_posix_elf_end}, // 同一ELF新错误槽和CLOEXEC观察。
    {"ctls", airi_ctls_elf_start, airi_ctls_elf_end}, // 自动建立静态TLS后运行普通C。
```
<!-- after -->
```c
static const struct executable executables[] = { // 每份映像字节由内核只读段长期持有。
    {"sh", airi_shell_elf_start, airi_shell_elf_end}, // 独占串口输入的可选用户程序。
    {"init", airi_init_elf_start, airi_init_elf_end}, // 管理前台shell的常驻用户进程。
    {"vector", airi_vector_elf_start, airi_vector_elf_end}, // 聚合、分散、短前缀与错误原子性。
    {"libc", airi_posix_elf_start, airi_posix_elf_end}, // 用户库错误与文件结果转换。
    {"libc-reset", airi_posix_elf_start, airi_posix_elf_end}, // 同一ELF新错误槽和CLOEXEC观察。
    {"ctls", airi_ctls_elf_start, airi_ctls_elf_end}, // 自动建立静态TLS后运行普通C。
```

在普通 file_transfer 结束与 poll_waiters 开始之间加入 vector_transfer。描述、数据和文件结果都在这次同步调用内消费。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    if (result > 0 && !copy_user(p, buffer, address, (size_t)result, true)) // 目标阻塞且单线程，已检查映射不应变化。
        airi_panic("validated file output mapping changed"); // 内部错误不丢字节后假装成功。
    return result; // 完成前缀或明确负错误。
}
static void poll_waiters(void) { // 仅普通内核上下文运行，IRQ本身不复制、分配或打印。
    for (unsigned i = 0; i < PROCESS_COUNT; ++i) { // 本阶段有界等待队列由进程槽直接表示。
```
<!-- after -->
```c
    if (result > 0 && !copy_user(p, buffer, address, (size_t)result, true)) // 目标阻塞且单线程，已检查映射不应变化。
        airi_panic("validated file output mapping changed"); // 内部错误不丢字节后假装成功。
    return result; // 完成前缀或明确负错误。
}
static long vector_transfer(struct user_process *p, uint64_t fd, uint64_t descriptors, uint64_t count, uint64_t offset, bool write) { // 当前只支持可定位文件，一次有界副本。
    if (fd >= AIRI_FD_COUNT) // 在窄化整数前检查完整FD。
        return -9; // -EBADF。
    if (count > AIRI_VECTOR_COUNT || offset > INT64_MAX) // 负count/offset转为无符号后也会在这里拒绝。
        return -22; // -EINVAL，不读取描述或改变文件。
    struct airi_iovec vectors[AIRI_VECTOR_COUNT]; // 当前调用持有的128字节描述快照。
    if (count && !copy_user(p, vectors, descriptors, (size_t)count * sizeof(vectors[0]), false)) // 整个数组必须可读，不在用户库中直接解引用。
        return -14; // -EFAULT，文件尚未操作。
    uint64_t total = 0; // 所有项串联的逻辑长度，不把溢出截成小请求。
    for (unsigned i = 0; i < count; ++i) { // 固定最多八项。
        if (vectors[i].bytes > (uint64_t)INT64_MAX - total) // 返回类型可表达范围在做加法前检查。
            return -22; // -EINVAL，拒绝负数式回绕或不可表示总量。
        total += vectors[i].bytes; // 总长仅决定本次有限前缀。
    }
    size_t bytes = total > AIRI_VECTOR_BYTES ? AIRI_VECTOR_BYTES : (size_t)total; // 长向量也只发起一次至多256字节操作。
    size_t planned = 0; // 本次会触及的前缀，越过256后的地址不访问。
    for (unsigned i = 0; i < count && planned < bytes; ++i) { // 先验证全部实际前缀，再进行任何I/O或输出。
        size_t part = vectors[i].bytes > bytes - planned ? bytes - planned : (size_t)vectors[i].bytes; // 一项可能只使用前半。
        if (part && !user_access(p, vectors[i].address, part, !write)) // 写文件读用户，读文件写用户；零长不检查无意义基址。
            return -14; // -EFAULT，前面合法项也未产生文件或输出副作用。
        planned += part; // 不用地址加长度形成可能回绕的右端。
    }
    uint8_t buffer[AIRI_VECTOR_BYTES]; // 同步调用内聚合/分散，不给设备DMA，不跨返回保留。
    size_t copied = 0; // gather按数组顺序形成同一文件操作的连续输入。
    if (write) // preadv不读取目标用户缓冲内容。
        for (unsigned i = 0; i < count && copied < bytes; ++i) { // 只聚合本次已验证前缀。
            size_t part = vectors[i].bytes > bytes - copied ? bytes - copied : (size_t)vectors[i].bytes; // 当前项实际长度。
            if (part && !copy_user(p, buffer + copied, vectors[i].address, part, false)) // 映射在不可抢占、单线程内核中保持。
                airi_panic("validated vector input changed"); // 不变量破坏不能交出半份文件操作。
            copied += part; // 下一项紧随上一个实际字节。
        }
    long result = write ? airi_fd_write(&p->files, (unsigned)fd, buffer, bytes, true, offset) : airi_fd_read(&p->files, (unsigned)fd, buffer, bytes, true, offset); // 仅调用一次文件层，显式偏移不改共享位置。
    if (!write && result > 0) { // 只分散实际读到的字节，不用零覆盖尚未完成尾部。
        copied = 0; // 从第一个目标重新开始。
        for (unsigned i = 0; i < count && copied < (size_t)result; ++i) { // 本轮返回数不超过已验证256字节。
            size_t part = vectors[i].bytes > (size_t)result - copied ? (size_t)result - copied : (size_t)vectors[i].bytes; // EOF可能在项中间结束。
            if (part && !copy_user(p, buffer + copied, vectors[i].address, part, true)) // 描述快照不受输出覆盖用户数组影响。
                airi_panic("validated vector output changed"); // 不能消费文件后掩盖复制失败。
            copied += part; // 未触及目标保留原有内容。
        }
    }
    return result; // 实际短数量、EOF0或原始负错误，不伪造全部完成。
}
static void poll_waiters(void) { // 仅普通内核上下文运行，IRQ本身不复制、分配或打印。
    for (unsigned i = 0; i < PROCESS_COUNT; ++i) { // 本阶段有界等待队列由进程槽直接表示。
```

在33/34号 FS 分支之后接入35/36，按 R10 取得显式偏移。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    }
    else if (f->rax == 34) // 查询的是陷入时从硬件读取的真实当前值。
        f->rax = current->fs_base; // 低半区基址可由long无歧义表示，不返回内核地址。
    else // 尚未提供的接口都明确拒绝。
        f->rax = (uint64_t)-38ll; // -ENOSYS。
    poll_waiters(); // 文件/流修改完成后立即重查其他等待者，不等用户轮询。
```
<!-- after -->
```c
    }
    else if (f->rax == 34) // 查询的是陷入时从硬件读取的真实当前值。
        f->rax = current->fs_base; // 低半区基址可由long无歧义表示，不返回内核地址。
    else if (f->rax == 35 || f->rax == 36) // 自有显式偏移向量操作，R10保存第四参数。
        f->rax = (uint64_t)vector_transfer(current, f->rdi, f->rsi, f->rdx, f->r10, f->rax == 36); // 描述和前缀在调用内完成，不建立异步借用。
    else // 尚未提供的接口都明确拒绝。
        f->rax = (uint64_t)-38ll; // -ENOSYS。
    poll_waiters(); // 文件/流修改完成后立即重查其他等待者，不等用户轮询。
```

用户原始封装只传描述地址及数量，不自行遍历数组；这样坏描述仍得到内核−14，用户程序能继续处理。更新用户头与syscall.c，call4沿已有R10规则传第四参数。

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
#endif // 结束包含保护。
```

</details>

在 `labs/airios/libc/syscall.c` 末尾追加以下内容，前章已有部分保留。

<!-- append: labs/airios/libc/syscall.c -->
```c

long airi_preadv(unsigned fd, const struct airi_iovec *vectors, unsigned count, uint64_t offset) { // 不在用户库解引用未知描述数组。
    return call4(35, fd, (uintptr_t)vectors, count, offset); // 第四参数沿既有R10约定，不移动共享文件偏移。
}
long airi_pwritev(unsigned fd, const struct airi_iovec *vectors, unsigned count, uint64_t offset) { // 描述只借用到同步调用返回。
    return call4(36, fd, (uintptr_t)vectors, count, offset); // 实际短数量或原始负错误。
}
```

<a id="vector-source-abi"></a>
## 标准指针字段到原始数字字段只在接口处对应

新建sys/uio.h。源码使用struct iovec的指针和size_t字段，内核使用固定16字节数字记录。当前目标指针与size_t都是64位；posix.c通过sizeof与offsetof分别断言整个长度、地址字段和长度字段偏移一致，才将其地址交原始封装。库没有通过强转直接读取结构对象。

用户count与offset是有符号值，负数在包装中拒绝为−1/EINVAL；其余结果继续用K44的errno转换。内核还独立验证原始64位参数，不能只依赖调用方使用这一份库。IOV_MAX=8低于完整POSIX平台要求，所以仍称有界源码子集，不据标准函数名宣称完全兼容。

<!-- write: labs/airios/include/sys/uio.h -->
```c
#ifndef AIRI_SYS_UIO_H // 当前仅声明不建立流等待的显式偏移向量操作。
#define AIRI_SYS_UIO_H // 包含保护。
#include <sys/types.h> // 用户指针、容量与有符号文件偏移。
#define IOV_MAX 8 // 本源码接口至多八项，低于完整POSIX平台要求，明确为有界子集。
struct iovec { void *iov_base; size_t iov_len; }; // 数组顺序决定串联字节顺序，不代表物理连续。
#ifdef __cplusplus // 标准名字由同一C实现提供给未来C++源。
extern "C" { // 保持链接符号一致。
#endif // C11直接声明。
ssize_t preadv(int fd, const struct iovec *vectors, int count, off_t offset); // 一次至多256字节，显式偏移不移动共享位置。
ssize_t pwritev(int fd, const struct iovec *vectors, int count, off_t offset); // 前缀可在一项中结束；流返回ESPIPE。
#ifdef __cplusplus // 结束C链接范围。
} // 这里只声明两个已经实现的入口。
#endif // 结束语言差异。
#endif // 结束包含保护。
```

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

<a id="vector-workload"></a>
## 让坏后项与半个描述都在真实用户边界失败

新建vector-probe.c。先观察三段聚合、EOF落在第二目标中间、共享offset保持，再让1024字节RAM容量产生短写。两个200字节来源测试统一的256前缀，输出300字节中最后44字节必须保留哨兵。

随后把第二项改为内核高地址：pwritev不得把第一项Z写入文件，preadv不得先覆盖第一个合法目标。坏数组地址、负/超限数量、总长溢出和负offset分别返回明确错误。零长项即使给UINT64_MAX基址也没有数据要访问，不能被当成一次坏读取。

最后申请一个真实用户页，只在页尾放描述的第一个8字节，长度字段落到未映射下一页；再让有效数据范围跨同一页边界。两次都应返回EFAULT并继续，不能只检查首地址。缓冲区仍在同步调用期间由进程持有，调用返回并确认失败后才归还。

管道上的pwritev应返回ESPIPE，已有阻塞read/write仍用原接口。结束前关闭FD，原launch继续核对整次会话页、活堆和FD资源恢复。

<!-- write: labs/airios/vector-probe.c -->
```c
#include <airi_libc.h> // 原始页申请用于构造跨页坏描述，文件调用走用户包装。
#include <errno.h> // 向量错误同样返回-1和当前本地正错误号。
#include <fcntl.h> // 平坦RAM文件与访问模式。
#include <unistd.h> // 普通读回与共享偏移观察。
#include <sys/uio.h> // 本节实际提供preadv/pwritev，不宣称readv/writev已支持阻塞流。
int main(void) { // 所有缓冲区都在真实用户页内，未由宿主注入数据。
    int fd = open("/vector", O_RDWR | O_CREAT | O_TRUNC, (mode_t)0600); // 新打开对象偏移0。
    if (fd < 0) // 保留普通错误结果。
        return 1; // 不进行后续比较。
    struct iovec input[3] = {{"ab", 2}, {"CDE", 3}, {"f", 1}}; // 分散用户地址按数组顺序聚合成abCDEf。
    if (pwritev(fd, input, 3, 0) != 6) // 一次显式偏移调用，不是三次文件写。
        return 2; // 不接受只处理第一段的伪向量。
    unsigned char a[2], b[4], c[1]; // EOF将落在第二个目标内部。
    memset(a, '?', sizeof(a)); memset(b, '?', sizeof(b)); c[0] = '?'; // 尚未读到的尾部必须保持。
    struct iovec output[3] = {{a, sizeof(a)}, {b, sizeof(b)}, {c, sizeof(c)}}; // 总容量7，offset1之后只有5字节。
    if (preadv(fd, output, 3, 1) != 5 || memcmp(a, "bC", 2) || memcmp(b, "DEf", 3) || b[3] != '?' || c[0] != '?') // 按顺序填满a，只填b前三字节，不碰尾部。
        return 3; // 短返回总数不能变成每段都填满。
    if (read(fd, c, 1) != 1 || c[0] != 'a') // 前面的显式读写都不能改变原offset0。
        return 4; // preadv不是先seek再read。
    if (pwritev(fd, input, 2, 1022) != 2) // RAM上限1024导致短写恰好结束在第一项。
        return 5; // 不将文件容量限制伪装为全部完成。
    if (preadv(fd, output, 3, 1022) != 2 || memcmp(a, "ab", 2)) // 实际读回最后两字节。
        return 6; // 不能仅靠返回2作证。
    unsigned char first[200], second[200], data[300]; // 大于一次内核前缀预算的逻辑向量。
    memset(first, 0x31, sizeof(first)); memset(second, 0x79, sizeof(second)); memset(data, '?', sizeof(data)); // 两个来源可独立辨识。
    struct iovec large[2] = {{first, sizeof(first)}, {second, sizeof(second)}}; // 总400，当前一次最多256。
    if (pwritev(fd, large, 2, 0) != 256) // 第一项200加第二项56，返回前缀交调用者继续。
        return 7; // 不是每项各允许256。
    struct iovec scatter[2] = {{data, 100}, {data + 100, 200}}; // 总300，只分散实际256。
    if (preadv(fd, scatter, 2, 0) != 256) // 一次读取的连续文件范围。
        return 8; // 不隐式循环读取剩下44字节。
    for (unsigned i = 0; i < 300; ++i) // 比较完整输出，含未触及尾部。
        if (data[i] != (i < 200 ? 0x31 : i < 256 ? 0x79 : '?')) // 200之后进入第二个来源，256以后保持哨兵。
            return 9; // 不让scatter覆盖额外字节。
    struct iovec bad[2] = {{"Z", 1}, {(void *)(uintptr_t)0xffff800000000000ull, 1}}; // 第一项合法，第二项在本次实际前缀内非法。
    if (pwritev(fd, bad, 2, 0) != -1 || errno != EFAULT || pread(fd, c, 1, 0) != 1 || c[0] != 0x31) // 写前完整检查，不能留下第一项Z。
        return 10; // 逐项发普通write会在这里暴露半次副作用。
    c[0] = '?'; bad[0] = (struct iovec){c, 1}; // 改为读，第一项是有效输出。
    if (preadv(fd, bad, 2, 0) != -1 || errno != EFAULT || c[0] != '?') // 坏后项不能让前一个输出先被消费填入。
        return 11; // 原哨兵必须保持。
    if (preadv(fd, (void *)(uintptr_t)0xffff800000000000ull, 1, 0) != -1 || errno != EFAULT) // 坏描述数组由内核检查，用户库不得先解引用崩溃。
        return 12; // 必须仍在此用户中继续运行。
    if (pwritev(fd, NULL, -1, 0) != -1 || errno != EINVAL || pwritev(fd, NULL, IOV_MAX + 1, 0) != -1 || errno != EINVAL) // 负数量与超过八项都在读描述前拒绝。
        return 13; // 不因NULL指针掩盖数量校验。
    struct iovec overflow[2] = {{c, (size_t)INT64_MAX}, {c, 1}}; // 两项总量超出ssize_t，即使前256原本可能可读也不能接受。
    if (pwritev(fd, overflow, 2, 0) != -1 || errno != EINVAL || pwritev(fd, input, 1, -1) != -1 || errno != EINVAL) // 长度溢出与负显式偏移都无副作用。
        return 14; // 不把回绕后的短值当合法请求。
    struct iovec empty[2] = {{(void *)(uintptr_t)UINT64_MAX, 0}, {"ok", 2}}; // 零长项没有可访问字节，基址不参与本次检查。
    if (pwritev(fd, empty, 2, 10) != 2 || pwritev(fd, NULL, 0, 0) != 0 || preadv(-1, NULL, 0, 0) != -1 || errno != EBADF) // 空向量不读取描述，但仍验证FD。
        return 15; // 不把全部零长度误当任意FD都成功。
    unsigned char *page = airi_alloc(4096); // 下一页未映射，构造真实跨页描述而非宿主数组。
    if (!page) // 明确处理资源不足。
        return 16; // 不访问NULL。
    *(uint64_t *)(page + 4088) = (uintptr_t)c; // 描述只有第一个字段可读，长度字段落到下一页。
    if (preadv(fd, (const struct iovec *)(page + 4088), 1, 0) != -1 || errno != EFAULT) // 整个16字节数组必须在操作前复制。
        return 17; // 不将有效半项当完整描述。
    struct iovec crosses = {page + 4094, 4}; // 数据缓冲前两字节合法，后两字节跨未映射边界。
    page[4094] = 0x55; page[4095] = 0x55; // 验证坏范围不改有效前缀。
    if (preadv(fd, &crosses, 1, 0) != -1 || errno != EFAULT || page[4094] != 0x55 || page[4095] != 0x55 || airi_release(page, 4096)) // 数据跨度完整校验后才可读文件。
        return 18; // 不是只检查指针起点。
    int ends[2]; // 当前显式偏移向量不进入阻塞流等待。
    if (pipe(ends) || pwritev(ends[1], input, 1, 0) != -1 || errno != ESPIPE || close(ends[0]) || close(ends[1]) || close(fd)) // 一次文件层操作保留流的不可定位语义。
        return 19; // 不对管道伪造偏移。
    airi_puts("[VECTOR_USER_OK] gather=ordered scatter=short prefix=256 positioned=1 invalid=unchanged descriptors=checked\n"); // 所有正常与故障路径实际完成。
    return 0; // 用户shell继续服务，整次会话核对资源恢复。
}
```

在images.S嵌入真实vector-probe.elf，Makefile将它与USER_COMMON和posix.o显式链接。标准向量函数由本项目用户库提供，内核新入口处理完整用户边界，没有借宿主readv实现完成guest工作。

在 `labs/airios/images.S` 末尾追加以下内容，前章已有部分保留。

<!-- append: labs/airios/images.S -->
```asm

.balign 8 /* 向量用户程序同样经过独立ELF装载。 */
.global airi_vector_elf_start, airi_vector_elf_end /* 真实用户映像范围。 */
airi_vector_elf_start: /* 含POSIX包装的观察程序。 */
.incbin AIRI_VECTOR_ELF /* 本配置交叉链接产物。 */
airi_vector_elf_end: /* 只读输入的右端。 */
```

在 `Makefile` 末尾追加以下内容，前章已有部分保留。

<!-- append: Makefile -->
```make

# 向量程序选择相同POSIX错误转换，内核仍使用独立固定宽度描述。
$(OUT)/user/vector-probe.elf: $(USER_COMMON) $(OUT)/user/posix.o $(OUT)/user/vector-probe.o labs/airios/program.ld
# 一份用户main与自有库，不调用宿主readv实现。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/posix.o $(OUT)/user/vector-probe.o -o $@
# 包装等待当前配置的实际向量程序。
$(OUT)/labs/airios/images.o: $(OUT)/user/vector-probe.elf
# 文件路径以C字符串宏交给预处理汇编。
$(OUT)/labs/airios/images.o: CPPFLAGS += -DAIRI_VECTOR_ELF='"$(OUT)/user/vector-probe.elf"'
```

shell_test在同一实际用户shell输入vector，要求本次目标完整退出并发布正常与错误路径标记。旧libc、编译器TLS、手工FS、浮点、容量、孤儿和存储路径继续跑，不用单条新输出替代原回归。

在 libc 命令之后接入 vector，等待同一目标的结束记录，再检查向量前缀与坏范围标记。

<!-- replace: scripts/shell_test.py -->
<!-- before -->
```python
                libc_output=text[offset:] # 不使用旧映像或启动期输出。
                assert '[LIBC_IO_OK] short=256 pipe=64 eof=zero offset=preserved bad_buffer=unchanged' in libc_output # 正常、短操作和坏缓冲区均由guest验证。
                assert '[LIBC_ERRNO_OK] initial=0 fork=copied preempt=private success=preserved exec=reset' in libc_output and '[LIBC_EXEC_RESET_OK] errno=zero cloexec=closed' in libc_output # 同一身份成功exec重置错误并关闭表项。
                offset=len(text); wire.sendall(b'orphans\n'); text=wait('[SHELL_COMMAND_DONE] name=orphans code=0',offset) # 同一用户shell持续服务，不靠返回monitor收拢孙进程。
                orphan_output=text[offset:] # 只核对本条命令的身份记录。
                assert '[ORPHAN_USER_OK] rounds=24 live=12 zombie=12 session=alive' in orphan_output # 超过八槽容量仍能连续fork。
```
<!-- after -->
```python
                libc_output=text[offset:] # 不使用旧映像或启动期输出。
                assert '[LIBC_IO_OK] short=256 pipe=64 eof=zero offset=preserved bad_buffer=unchanged' in libc_output # 正常、短操作和坏缓冲区均由guest验证。
                assert '[LIBC_ERRNO_OK] initial=0 fork=copied preempt=private success=preserved exec=reset' in libc_output and '[LIBC_EXEC_RESET_OK] errno=zero cloexec=closed' in libc_output # 同一身份成功exec重置错误并关闭表项。
                offset=len(text); wire.sendall(b'vector\n'); text=wait('[SHELL_COMMAND_DONE] name=vector code=0',offset) # 散布缓冲必须由真实内核边界校验后完成一次文件操作。
                assert '[VECTOR_USER_OK] gather=ordered scatter=short prefix=256 positioned=1 invalid=unchanged descriptors=checked' in text[offset:] # 坏后项、跨页描述和实际前缀均由guest检查。
                offset=len(text); wire.sendall(b'orphans\n'); text=wait('[SHELL_COMMAND_DONE] name=orphans code=0',offset) # 同一用户shell持续服务，不靠返回monitor收拢孙进程。
                orphan_output=text[offset:] # 只核对本条命令的身份记录。
                assert '[ORPHAN_USER_OK] rounds=24 live=12 zombie=12 session=alive' in orphan_output # 超过八槽容量仍能连续fork。
```

完整序列结束后，结果记录加入 positioned_vectors。

<!-- replace: scripts/shell_test.py -->
<!-- before -->
```python
                assert '[USER_SHELL_EXIT]' in text[offset:] and 'program sh exited 0' in text[offset:] # 真实收拢页/FD/静态池由launch核对。
                offset=len(text); wire.sendall(b'echo after-shell\n'); text=wait('[CONSOLE_UPDATE] view=0 commands=2 page=0',offset) # 回到内核后输入仍完整。
                assert 'after-shell' in text[offset:] # 不只是退出而丢了后续UART。
                result={'passed':True,'ring3_shell':True,'ram_file_and_device_readback':True,'other_user_during_cq_wait':True,'qmp_iops':1,'error_exec_survived':True,'returned_to_monitor':True,'orphan_rounds':24,'memory_reuse_rounds':128,'capacity_rounds':4,'capacity_pages':1024,'simd_preempt_fork_exec':True,'fs_base_preempt_fork_exec':True,'compiler_static_tls':True,'posix_errno_io_subset':True} # 只发布已观察事实。
                result_path.write_text(json.dumps(result,indent=2)+'\n'); print('PASS user shell: alloc/file/async wait/other task/CQ/readback, failed exec, return to monitor') # 阶段演示实际完成。
                return result # 总入口可收集。
            finally: # 包括断言、超时和QEMU错误。
```
<!-- after -->
```python
                assert '[USER_SHELL_EXIT]' in text[offset:] and 'program sh exited 0' in text[offset:] # 真实收拢页/FD/静态池由launch核对。
                offset=len(text); wire.sendall(b'echo after-shell\n'); text=wait('[CONSOLE_UPDATE] view=0 commands=2 page=0',offset) # 回到内核后输入仍完整。
                assert 'after-shell' in text[offset:] # 不只是退出而丢了后续UART。
                result={'passed':True,'ring3_shell':True,'ram_file_and_device_readback':True,'other_user_during_cq_wait':True,'qmp_iops':1,'error_exec_survived':True,'returned_to_monitor':True,'orphan_rounds':24,'memory_reuse_rounds':128,'capacity_rounds':4,'capacity_pages':1024,'simd_preempt_fork_exec':True,'fs_base_preempt_fork_exec':True,'compiler_static_tls':True,'posix_errno_io_subset':True,'positioned_vectors':True} # 只发布已观察事实。
                result_path.write_text(json.dumps(result,indent=2)+'\n'); print('PASS user shell: alloc/file/async wait/other task/CQ/readback, failed exec, return to monitor') # 阶段演示实际完成。
                return result # 总入口可收集。
            finally: # 包括断言、超时和QEMU错误。
```

<a id="vector-observe"></a>
## 从描述数组追到文件前缀，再回到存储项目

全部修改完成后运行：

<!-- run -->
```sh
./airios build # 编译两个原始向量入口、用户包装和真实观察程序。
./airios test # 旧流程与向量正常/故障路径一起实际运行。
```

Mac默认仍是旧Python时给命令前加 `PATH=/opt/homebrew/bin:$PATH`。交互 `./airios run`，输入 `run sh`，再输入vector，应看到：

```text
[VECTOR_USER_OK] gather=ordered scatter=short prefix=256 positioned=1 invalid=unchanged descriptors=checked
[SHELL_COMMAND_DONE] name=vector code=0
```

坏后项和半个描述不会产生用户CPU故障，因为这是系统调用必须验证的输入；当前程序收到−1/EFAULT后仍继续并最终成功。与此前主动执行未映射FS读取导致PF的实验相比，差别在谁负责检查和执行访问。

在BOSS的pwritev路径里，描述数组和每块来源数据同样是不同对象；同步返回后可复用此次已不再借用的缓冲，但返回正数可能只表示逻辑前缀。现代异步提交还要把描述、注册缓冲和完成身份的寿命延长到明确CQ，不能从本节同步复制推出提交返回就能释放。现有异步实验仍是固定512字节注册副本，不自动获得任意iovec支持。

一次文件层操作也不等于持久提交。当前写入RAM，未实现磁盘文件系统、fsync或掉电原子性；内核串行处理期间的有界操作关系，不给物理存储额外保证。Linux的向量与真实项目对照继续按L04及系统实践地图进行，本节没有补写远端未运行的结果。

