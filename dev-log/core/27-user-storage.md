# K27 · 用户提交一条记录，重启后由新内核重新找回来

K26 在同一次启动中写入、扫描并恢复演示扇区。即使结果正确，也还可能依赖这次内核持有的对象。现在让用户 storage 提交记录，结束整个 QEMU，再由全新的内核运行 recover：新内核拿不到旧 count，必须重新读取介质才能知道上一条记录是否存在。本节把正式日志放在16–143号扇区，启动时只scan；用户显式run storage才追加一条固定记录，run recover只读最后一条。两个独立QEMU进程随后使用同一个实验盘，第二个内核没有第一个内核的count或用户页，只能从盘重新恢复。

<a id="storage-service"></a>
## 内核拥有日志，用户只拿序号和内容

用户进程会退出，日志却需要继续供下一次运行读取，因此不能把 journal 放进某个进程控制块。service.c 持有一个静态 journal 对象，它跨用户进程退出保留；不把对象指针交给用户或图形。初始化从块设备容量建立后端，调用scan恢复前缀，完成后记录count、truncated和readonly。没有设备时明确STORAGE_SKIPPED，真实读取错误则返回错误让main停止，不能把坏盘当新空盘。

服务给用户的是记录身份和内容，不是可修改内部状态的指针。append 接受已经复制到内核的 payload，最多480字节；read按0开始的索引读取完整记录，输出容量不够返回错误。count也可能返回−19或−5，应用不能把负错误转成巨大无符号索引。只读设备可以scan和read，append直接返回−30，不会把这种明确拒绝标成提交结果不确定。

**新建 `include/airi/storage.h`。** 用户服务只暴露数值和同步复制边界。

<!-- write: include/airi/storage.h -->
```c
#ifndef AIRI_STORAGE_H // 用户存储服务不暴露日志后端结构。
#define AIRI_STORAGE_H // 包含保护。
#include <airi/base.h> // 字节容量和返回值。
int airi_storage_init(void); // 从正式日志区16起恢复，设备受限时明确跳过。
long airi_storage_count(void); // 已确认前缀或负错误。
long airi_storage_append(const void *data, size_t bytes); // 同步追加，成功返回sequence。
long airi_storage_read(unsigned index, void *data, size_t capacity); // 索引从0，失败不复制输出。
#endif // 结束保护。
```

**新建 `kernel/storage/service.c`。** 正式区与K26的演示区、K25的盘末尾均不重叠。

<!-- write: kernel/storage/service.c -->
```c
#include <airi/storage.h> // 进程和控制台只消费数值结果。
#include <airi/journal.h> // 完整提交与恢复协议。
#include <airi/block.h> // 固定单设备后端。
#include <airi/kernel.h> // 实际恢复记录。
static struct airi_journal store; // 内核拥有，跨用户进程退出保留。
static bool available; // 初始化失败或设备受限不能伪造空日志。
static int read_sector(void *context, uint64_t sector, void *data) { // 窄同步适配不包含用户指针。
    (void)context; // 当前只有一块实验盘。
    return airi_block_read(sector, data); // 真实VirtIO完成。
}
static int write_sector(void *context, uint64_t sector, const void *data) { // 算法写入独立正式区域。
    (void)context; // 单设备无需额外状态。
    return airi_block_write(sector, data); // 错误保持原样。
}
static int flush(void *context) { // 不以普通write完成代替同步。
    (void)context; // 接口形状与日志后端一致。
    return airi_block_flush(); // 虚拟后端FLUSH。
}
int airi_storage_init(void) { // 正常启动只扫描，不偷偷新增记录。
    available = false; // 旧内存状态不可当恢复结果。
    if (!airi_block_sectors()) { // 超时隔离后不能继续读盘。
        airi_log("[STORAGE_SKIPPED] block_unavailable=1"); // 明确本阶段未提供磁盘服务。
        return 0; // 故障配置可继续非存储命令。
    }
    struct airi_journal_io io = {NULL, read_sector, write_sector, flush, airi_block_sectors()}; // 函数与静态设备在整个启动期有效。
    int error = airi_journal_scan(&store, &io, 16, 64); // 正式区占16–143，避开演示扇区与盘末尾。
    if (error) // 真实I/O错误不能当新空盘。
        return error; // main决定报告并停止。
    available = true; // 完整恢复后才接受请求。
    airi_log("[STORAGE_RECOVERED] count=%u truncated=%u readonly=%u", store.count, store.truncated, airi_block_readonly()); // 字段来自扫描与真实设备特征。
    return 0; // 空日志与已有前缀都合法。
}
long airi_storage_count(void) { // 用户取得数值，不持有store指针。
    return !available ? -19 : store.poisoned ? -5 : (long)store.count; // 缺设备、不确定与可用分开。
}
long airi_storage_append(const void *data, size_t bytes) { // 当前同步副本不跨调用保存。
    if (!available) // 超时设备不能继续发布请求。
        return -19; // -ENODEV。
    if (airi_block_readonly()) // 只读状态查询不破坏已恢复前缀。
        return -30; // -EROFS，不把正常只读拒绝标成不确定写。
    return airi_journal_append(&store, data, bytes); // 数据、提交和同步均完成后才成功。
}
long airi_storage_read(unsigned index, void *data, size_t capacity) { // 单条记录不是RAM文件偏移接口。
    return available ? airi_journal_read(&store, index, data, capacity) : -19; // 返回实际长度或明确错误。
}
```

<a id="storage-user-copy"></a>
## 系统调用不能让设备借用用户栈

storage 的字符串位于用户页，日志却需要把内容编码进扇区，再交给 DMA。不能让整个过程一直借用这个用户指针：该地址只在对应进程根中有意义，用户页以后还会随退出回收。因此系统调用边界先取得内核副本，存储服务再消费副本；设备最终接触的是驱动自己的 DMA 缓冲。

追加与读取在这个边界上方向相反。自有编号 17 用 RDI 传用户输入地址、RSI 传字节数；先检查长度并把全部输入复制进最多 480 字节的内核数组，成功后才调用 append。编号 18 用 RDI 传记录索引、RSI 传输出地址、RDX 传容量；先验证完整用户输出范围，再读设备并检查 CRC，最后只复制实际返回长度。坏指针因而不会触发磁盘副作用，也不会使设备借用用户页。编号 19 只查询 count，不传缓冲。

副本解决了内存归属，却还没有改变等待方式。目前这些调用同步等设备，等待期间内核临时打开IRQ接收PIT，但不会切换用户。日志明确写wait=kernel，不能把它当作P5将实现的用户等待队列。用户栈一直由当前进程持有，内核副本和驱动静态DMA缓冲依次复制，返回后都不再借用户输入。

**更新进程服务的五个窗口。** 从 K26 的 user.c 继续，先登记映像，再补 17–19 的分支，最后更新命令列表。fork、exec 和 FD 的已有路径保持；每组旧文本都用于唯一定位。

先接入存储声明。文件 FD 与日志序号继续走不同接口。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
#include <airi/file.h> // 每进程FD表共享打开文件对象。
#include <airi/console.h> // 内核命令消费服务结果，不依赖主题和绘图。
#include <airi/elf.h> // 独立用户ELF检查器。
```
<!-- after -->
```c
#include <airi/storage.h> // 用户存储请求经过有界副本进入日志服务。
#include <airi/file.h> // 每进程FD表共享打开文件对象。
#include <airi/console.h> // 内核命令消费服务结果，不依赖主题和绘图。
#include <airi/elf.h> // 独立用户ELF检查器。
```

在映像声明中接入唯一的 storage ELF。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
extern const uint8_t airi_user_rsp_bad_start[], airi_user_rsp_bad_end[]; // 不规范用户返回栈。
extern const uint8_t airi_hello_elf_start[], airi_hello_elf_end[]; // 既有独立用户C程序。
extern const uint8_t airi_process_elf_start[], airi_process_elf_end[]; // 新的fork/wait和抢占工作负载。
extern const uint8_t airi_files_elf_start[], airi_files_elf_end[]; // 内存文件用户工作负载。
extern const uint8_t airi_worker_elf_start[], airi_worker_elf_end[]; // exec目标是另一个独立静态ELF。
extern const uint8_t airi_broken_elf_start[], airi_broken_elf_end[]; // 受控损坏输入，验证旧进程保持。
```
<!-- after -->
```c
extern const uint8_t airi_user_rsp_bad_start[], airi_user_rsp_bad_end[]; // 不规范用户返回栈。
extern const uint8_t airi_hello_elf_start[], airi_hello_elf_end[]; // 既有独立用户C程序。
extern const uint8_t airi_process_elf_start[], airi_process_elf_end[]; // 新的fork/wait和抢占工作负载。
extern const uint8_t airi_storage_elf_start[], airi_storage_elf_end[]; // 写入和恢复共用一份明确分支的用户ELF。
extern const uint8_t airi_files_elf_start[], airi_files_elf_end[]; // 内存文件用户工作负载。
extern const uint8_t airi_worker_elf_start[], airi_worker_elf_end[]; // exec目标是另一个独立静态ELF。
extern const uint8_t airi_broken_elf_start[], airi_broken_elf_end[]; // 受控损坏输入，验证旧进程保持。
```

在目录中给同一 ELF 两个名字。装载后的 argv[0] 决定追加还是只读恢复，不再复制另一份实现。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    {"hello", airi_hello_elf_start, airi_hello_elf_end}, // 独立C运行库与BSS观察。
    {"processes", airi_process_elf_start, airi_process_elf_end}, // fork/exec/wait实际工作负载。
    {"worker", airi_worker_elf_start, airi_worker_elf_end},
    {"files", airi_files_elf_start, airi_files_elf_end}, // 真实用户FD演示。
    {"broken", airi_broken_elf_start, airi_broken_elf_end}, // 用于完整的坏ELF失败演示。
};
```
<!-- after -->
```c
    {"hello", airi_hello_elf_start, airi_hello_elf_end}, // 独立C运行库与BSS观察。
    {"processes", airi_process_elf_start, airi_process_elf_end}, // fork/exec/wait实际工作负载。
    {"worker", airi_worker_elf_start, airi_worker_elf_end},
    {"storage", airi_storage_elf_start, airi_storage_elf_end}, // 显式提交记录。
    {"recover", airi_storage_elf_start, airi_storage_elf_end}, // 只读取已恢复记录。
    {"files", airi_files_elf_start, airi_files_elf_end}, // 真实用户FD演示。
    {"broken", airi_broken_elf_start, airi_broken_elf_end}, // 用于完整的坏ELF失败演示。
};
```

在 seek 分支之后加入 17–19。append 的内核输入副本与 read 的用户输出验证在这里分别完成；只有前置条件成立才进入存储服务。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
        f->rax = (f->rdi < AIRI_FD_COUNT ? (uint64_t)airi_fd_dup(&current->files, (unsigned)f->rdi, f->rsi != 0) : (uint64_t)-9ll); // 单CPU内核中同步加引用。
    else if (f->rax == 14) // 仅绝对定位，不实现Linux全部whence。
        f->rax = (f->rdi < AIRI_FD_COUNT ? (uint64_t)airi_fd_seek(&current->files, (unsigned)f->rdi, f->rsi) : (uint64_t)-9ll); // 共享打开对象的偏移随之变化。
    else // 尚未提供的接口都明确拒绝。
        f->rax = (uint64_t)-38ll; // -ENOSYS。
    return airi_user_resume(f); // 普通调用继续同一用户，仍检查RSP与标志。
```
<!-- after -->
```c
        f->rax = (f->rdi < AIRI_FD_COUNT ? (uint64_t)airi_fd_dup(&current->files, (unsigned)f->rdi, f->rsi != 0) : (uint64_t)-9ll); // 单CPU内核中同步加引用。
    else if (f->rax == 14) // 仅绝对定位，不实现Linux全部whence。
        f->rax = (f->rdi < AIRI_FD_COUNT ? (uint64_t)airi_fd_seek(&current->files, (unsigned)f->rdi, f->rsi) : (uint64_t)-9ll); // 共享打开对象的偏移随之变化。
    else if (f->rax == 17 || f->rax == 18) { // 日志append与read不是通用文件ABI。
        uint8_t buffer[480]; // payload上限与磁盘格式一致，设备不直接访问用户页。
        long result; // 实际长度、sequence或负错误。
        if (f->rax == 17) { // RDI输入地址，RSI长度。
            if (!f->rsi || f->rsi > sizeof(buffer)) // 不创建空记录或无界内核副本。
                result = -22; // -EINVAL。
            else if (!copy_user(current, buffer, f->rdi, (size_t)f->rsi, false)) // 完整复制后才可能产生磁盘副作用。
                result = -14; // -EFAULT。
            else { // 设备同步等待临时开IRQ，但不会在CS0切用户。
                airi_log("[STORAGE_REQUEST] pid=%u operation=append bytes=%llu wait=kernel", current->pid, (unsigned long long)f->rsi); // 诚实区分同步内核等待与后续用户阻塞。
                result = airi_storage_append(buffer, (size_t)f->rsi); // 成功已完成数据和提交同步。
            }
        } else { // RDI索引，RSI用户输出，RDX容量。
            if (f->rdi >= 64 || f->rdx > sizeof(buffer)) // 完整64位索引不能截断成其他记录。
                result = -22; // -EINVAL。
            else if (!user_access(current, f->rsi, (size_t)f->rdx, true)) // 先验证完整用户输出范围。
                result = -14; // -EFAULT，不进行设备I/O。
            else { // 仅将校验通过的实际长度复制到用户。
                result = airi_storage_read((unsigned)f->rdi, buffer, (size_t)f->rdx); // 设备数据和commit重新核对。
                if (result > 0 && !copy_user(current, buffer, f->rsi, (size_t)result, true)) // 当前映射不应变化。
                    airi_panic("validated storage output mapping changed"); // 内部不变量破坏明确停止。
            }
        }
        f->rax = (uint64_t)result; // 同一次用户调用取得精确结果。
    } else if (f->rax == 19) // 已恢复前缀数值查询。
        f->rax = (uint64_t)airi_storage_count(); // 缺设备或不确定状态返回负数。
    else // 尚未提供的接口都明确拒绝。
        f->rax = (uint64_t)-38ll; // -ENOSYS。
    return airi_user_resume(f); // 普通调用继续同一用户，仍检查RSP与标志。
```

最后更新 programs 列表，让 monitor 能发现刚登记的 storage 与 recover。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
bool airi_user_command(struct airi_console *console, const char *command) { // 系统命令消费者独立于图形主题扩展。
    size_t bytes = airi_strlen(command); // 先取得有效长度，固定前缀比较不能越过短字符串。
    if (bytes == 8 && !airi_memcmp(command, "programs", 8)) { // 显示当前初始ELF目录。
        airi_console_print(console, "programs: hello processes worker broken files"); // broken是明确损坏的演示输入。
        return true; // 已处理，不再交给图形扩展。
    }
    if (bytes < 4 || airi_memcmp(command, "run ", 4)) // 只接受明确的run空格前缀。
```
<!-- after -->
```c
bool airi_user_command(struct airi_console *console, const char *command) { // 系统命令消费者独立于图形主题扩展。
    size_t bytes = airi_strlen(command); // 先取得有效长度，固定前缀比较不能越过短字符串。
    if (bytes == 8 && !airi_memcmp(command, "programs", 8)) { // 显示当前初始ELF目录。
        airi_console_print(console, "programs: hello processes worker broken files storage recover"); // broken是明确损坏的演示输入。
        return true; // 已处理，不再交给图形扩展。
    }
    if (bytes < 4 || airi_memcmp(command, "run ", 4)) // 只接受明确的run空格前缀。
```

**更新用户声明。** 日志序号从1开始，读取索引从0开始，不和FD混用。

<!-- write: labs/airios/include/airi_libc.h -->
```c
#ifndef AIRI_USER_LIBC_H // 用户库头与内核基础库分开。
#define AIRI_USER_LIBC_H // 包含保护。
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
#endif // 结束包含保护。
```

**更新用户包装。** 沿既有int80三个参数约定进入新服务。

<!-- write: labs/airios/libc/syscall.c -->
```c
#include <airi_libc.h> // 自有用户接口，没有Linux系统调用编号。
static long call3(long number, uint64_t first, uint64_t second, uint64_t third) { // 统一约定RAX/RDI/RSI/RDX。
    __asm__ volatile("int $0x80" : "+a"(number) : "D"(first), "S"(second), "d"(third) : "memory", "cc"); // 内核恢复其他通用寄存器，RAX改为结果。
    return number; // +a表示同一个C对象既提供输入又接收输出。
}
long airi_write(unsigned fd, const void *buffer, size_t bytes) { // 用户指针由内核完整检查，库不伪造成功。
    return call3(1, fd, (uintptr_t)buffer, bytes); // 负值直接交给本节调用者，尚无errno变量。
}
long airi_getpid(void) { // 不需要参数的系统调用仍使用同一寄存器入口。
    return call3(3, 0, 0, 0); // 返回当前用户身份。
}
_Noreturn void airi_exit(unsigned code) { // 本函数成功时不返回。
    call3(2, code, 0, 0); // 内核选择启动任务现场继续。
    for (;;) // 若内核错误地返回这里，产生明确用户异常。
        __asm__ volatile("ud2"); // 不在未知退出状态下继续执行。
}
long airi_puts(const char *text) { // 文本长度先在当前用户地址空间计算。
    return airi_write(1, text, strlen(text)); // 当前内核最大256字节，过长会返回-EINVAL。
}
long airi_fork(void) { // 用户库不复制内存，交给内核建立子根和返回现场。
    return call3(6, 0, 0, 0); // 正PID/0/负错误保留原样。
}
long airi_waitpid(long pid, int *status) { // status为自有普通int退出码，不是Linux wait编码。
    return call3(8, (uint64_t)pid, (uintptr_t)status, 0); // 父进程可阻塞到这个调用的返回点。
}
void *airi_alloc(size_t bytes) { // 本节只按页申请，不提供逐对象free。
    long result = call3(9, bytes, 0, 0); // 内核完整成功后才前移用户VA游标。
    return result < 0 ? NULL : (void *)(uintptr_t)result; // 有界页接口以NULL表示申请失败，尚无errno。
}
uint64_t airi_ticks(void) { // 用来观察真实用户时间片。
    return (uint64_t)call3(5, 0, 0, 0); // 不把固定空循环步数当作时钟。
}
void airi_yield(void) { // 可主动让出，但抢占工作负载不使用它。
    call3(4, 0, 0, 0); // 由内核在完整陷入现场选择其他用户。
}
long airi_exec(const char *name) { // 成功进入新_start，只有失败才返回这条旧调用链。
    return call3(7, (uintptr_t)name, 0, 0); // 当前只传程序名，argv由内核建立单项。
}

static long call4(long number, uint64_t first, uint64_t second, uint64_t third, uint64_t fourth) { // 前三参数与call3相同，显式偏移需要第四槽。
    register uint64_t r10 __asm__("r10") = fourth; // C寄存器变量固定到自有ABI的R10。
    __asm__ volatile("int $0x80" : "+a"(number) : "D"(first), "S"(second), "d"(third), "r"(r10) : "memory", "cc"); // 陷入现场完整保存R10。
    return number; // 负错误保持原样。
}
long airi_open(const char *path, unsigned flags) { // 路径由内核有界复制。
    return call3(11, (uintptr_t)path, flags, 0); // FD是当前进程的编号。
}
long airi_close(unsigned fd) { // 释放一个表项引用。
    return call3(12, fd, 0, 0); // 已关闭再关闭为-9。
}
long airi_dup(unsigned fd, unsigned cloexec) { // 不重新open文件。
    return call3(13, fd, cloexec, 0); // 新表项的标志独立。
}
long airi_seek(unsigned fd, uint64_t offset) { // 自有接口没有whence参数。
    return call3(14, fd, offset, 0); // 返回绝对偏移。
}
long airi_read(unsigned fd, void *buffer, size_t bytes) { // 同步读至当前用户可写范围。
    return call3(10, fd, (uintptr_t)buffer, bytes); // 返回数不能直接假定等于bytes。
}
long airi_pread(unsigned fd, void *buffer, size_t bytes, uint64_t offset) { // 显式偏移不修改共享打开位置。
    return call4(15, fd, (uintptr_t)buffer, bytes, offset); // R10传偏移，不借用用户栈参数布局。
}
long airi_pwrite(unsigned fd, const void *buffer, size_t bytes, uint64_t offset) { // 与pread使用同一第四参数约定。
    return call4(16, fd, (uintptr_t)buffer, bytes, offset); // 写也可能短于请求。
}

long airi_log_append(const void *data, size_t bytes) { // 同步记录提交，不等同普通FD写。
    return call3(17, (uintptr_t)data, bytes, 0); // 数据和commit同步后才返回sequence。
}
long airi_log_read(unsigned index, void *data, size_t capacity) { // 内核验证完整输出范围。
    return call3(18, index, (uintptr_t)data, capacity); // 小容量返回错误，不部分截断记录。
}
long airi_log_count(void) { // 只返回数值，不借内核对象。
    return call3(19, 0, 0, 0); // 真实恢复失败保持负错误。
}
```

<a id="storage-user-program"></a>
## 同一ELF明确选择追加或只读恢复

第二次启动只应验证恢复，不能又写一条新记录后声称读到了旧结果。storage 与 recover 使用同一 ELF，通过内核提供的argv[0]选择路径。storage先读count，再追加固定文本，成功sequence必须等于旧count+1；随后从设备读最后一条并核对长度和每字节内容。recover不写日志，count为0时报告STORAGE_EMPTY，否则读同一条固定格式内容。它是课程专用工作负载，不是能读取任意用户文本的通用日志工具。

正式记录可能跨多次运行保留，非法请求尤其不能悄悄改变 count。程序还实际传内核地址、481 字节超长请求和只读用户输出，确认分别返回−14、−22、−14，count保持不变。容量64条满后打印STORAGE_FULL并退出28，监控器仍能输入；不会为了继续演示而覆盖旧记录。正常启动不自动运行storage，所以反复启动不会把正式区写满。

**新建 `labs/airios/storage.c`。** 完整用户程序验证结果，内核不直接替它打印成功标记。

<!-- write: labs/airios/storage.c -->
```c
#include <airi_libc.h> // 用户只通过int80进入存储服务。
static const char message[] = "airios: durable request"; // 固定可复现记录，payload不包含NUL。
int main(int argc, char **argv) { // 同一ELF通过明确名字选择写入或只读恢复。
    if (argc != 1) // 当前自有启动只传一个名字。
        return 60; // 不猜测参数布局。
    long count = airi_log_count(); // 当前启动从设备恢复的已确认前缀。
    if (count < 0) { // 缺设备或不确定日志不当成空盘。
        airi_puts("[STORAGE_UNAVAILABLE]\n"); // 说明普通应用失败，内核仍可继续。
        return 61; // 应用退出值不等于内核PANIC。
    }
    if (airi_log_append((const void *)0xffffffff80000000ull, 1) != -14 || airi_log_append(message, 481) != -22 || airi_log_read(0, (void *)message, 1) != -14 || airi_log_count() != count) // 坏指针和长度必须在任何写入或读取副作用之前拒绝。
        return 65; // 原前缀数量不能因错误请求改变。
    airi_puts("[STORAGE_USER_ERRORS_OK] bad_pointer=-14 length=-22 prefix=unchanged\n"); // 真实用户边界结果。
    if (!memcmp(argv[0], "storage", 8)) { // 显式追加一次，普通启动不会自动写正式日志。
        long sequence = airi_log_append(message, sizeof(message) - 1); // 复制字节后等待数据与提交同步。
        if (sequence == -28) { // 有界容量满，不覆盖旧记录。
            airi_puts("[STORAGE_FULL] records=64\n"); // 供用户决定后续处理。
            return 28; // 普通应用错误可从监控器继续。
        }
        if (sequence != count + 1) // 成功必须承认正确新记录身份。
            return 62; // 不假装短写或设备错误已完成。
        ++count; // 本程序接下来读取刚确认的最后一条。
    } else if (memcmp(argv[0], "recover", 8)) // 只读名字来自内核初始程序目录。
        return 63; // 不接受隐式其他模式。
    if (!count) { // 干净盘尚无记录。
        airi_puts("[STORAGE_EMPTY]\n"); // 空是合法状态，恢复演示另要求已写记录。
        return 0; // 不误读索引-1。
    }
    char buffer[64]; // 用户输出缓冲比当前payload长，内核仍按长度复制。
    long bytes = airi_log_read((unsigned)(count - 1), buffer, sizeof(buffer)); // 从实际设备重新验证最后记录。
    if (bytes != sizeof(message) - 1 || memcmp(buffer, message, (size_t)bytes)) // 不只看缓存计数。
        return 64; // 旧映像或损坏数据不能通过。
    airi_puts(!memcmp(argv[0], "storage", 8) ? "[STORAGE_USER_OK] append=committed read=verified\n" : "[RECOVERY_USER_OK] last_record=verified\n"); // 标记来自真正ring3用户代码。
    return 0; // 同步设备请求结束后正常退出。
}
```

**更新只读映像包装。** storage有独立链接结果，recover别名不复制另一份ELF。

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
```

**更新构建。** 内核加入服务，用户工作负载仍独立链接自有入口与libc。

把存储服务加入内核源码。

<!-- replace: Makefile -->
<!-- before -->
```make
# 基础目标只编译文本会话与设备；不读取 frontend 的目录或字体。
SOURCES := kernel/main.c kernel/boot/limine.c kernel/boot/info.c \
    kernel/lib/base.c kernel/lib/runtime.c kernel/arch/x86_64/serial.c \
    kernel/console/console.c kernel/arch/x86_64/input.c kernel/arch/x86_64/cpu.c kernel/arch/x86_64/trap.c kernel/mm/pages.c kernel/mm/memory.c kernel/arch/x86_64/paging.c kernel/mm/heap.c kernel/arch/x86_64/timer.c kernel/task/task.c kernel/process/user.c kernel/process/elf.c kernel/fs/file.c kernel/drivers/virtio_block.c kernel/storage/journal.c kernel/storage/demo.c
# 只有 desktop- 前缀目标才读取图形支线自己的构建片段。
ifneq ($(filter desktop-%,$(PROFILE)),)
include frontend/sources.mk # 本分支取得图形自己的源码清单。
```
<!-- after -->
```make
# 基础目标只编译文本会话与设备；不读取 frontend 的目录或字体。
SOURCES := kernel/main.c kernel/boot/limine.c kernel/boot/info.c \
    kernel/lib/base.c kernel/lib/runtime.c kernel/arch/x86_64/serial.c \
    kernel/console/console.c kernel/arch/x86_64/input.c kernel/arch/x86_64/cpu.c kernel/arch/x86_64/trap.c kernel/mm/pages.c kernel/mm/memory.c kernel/arch/x86_64/paging.c kernel/mm/heap.c kernel/arch/x86_64/timer.c kernel/task/task.c kernel/process/user.c kernel/process/elf.c kernel/fs/file.c kernel/drivers/virtio_block.c kernel/storage/journal.c kernel/storage/demo.c kernel/storage/service.c
# 只有 desktop- 前缀目标才读取图形支线自己的构建片段。
ifneq ($(filter desktop-%,$(PROFILE)),)
include frontend/sources.mk # 本分支取得图形自己的源码清单。
```

包装对象增加 storage.elf 依赖；recover 仍复用同一产物。

<!-- replace: Makefile -->
<!-- before -->
```make
# 用户只链接自己的_start、main和libc子集，无宿主依赖。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_OBJECTS) -o $@
# 包装汇编明确依赖本配置用户ELF，文件变动就重新嵌入。
$(OUT)/labs/airios/images.o: labs/airios/images.S $(OUT)/user/hello.elf $(OUT)/user/processes.elf $(OUT)/user/worker.elf $(OUT)/user/files.elf
# 建立包装对象目录。
	@mkdir -p $(dir $@)
# 单引号保护路径宏内部双引号，.incbin只读本配置生成文件。
	$(CC) $(CPPFLAGS) $(CFLAGS) -DAIRI_HELLO_ELF='"$(OUT)/user/hello.elf"' -DAIRI_PROCESS_ELF='"$(OUT)/user/processes.elf"' -DAIRI_WORKER_ELF='"$(OUT)/user/worker.elf"' -DAIRI_FILES_ELF='"$(OUT)/user/files.elf"' -c $< -o $@

# 第二个用户程序只复用用户入口与库，不把hello的main再次链接进去。
USER_COMMON := $(filter-out $(OUT)/user/hello.o,$(USER_OBJECTS))
```
<!-- after -->
```make
# 用户只链接自己的_start、main和libc子集，无宿主依赖。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_OBJECTS) -o $@
# 包装汇编明确依赖本配置用户ELF，文件变动就重新嵌入。
$(OUT)/labs/airios/images.o: labs/airios/images.S $(OUT)/user/hello.elf $(OUT)/user/processes.elf $(OUT)/user/worker.elf $(OUT)/user/files.elf $(OUT)/user/storage.elf
# 建立包装对象目录。
	@mkdir -p $(dir $@)
# 单引号保护路径宏内部双引号，.incbin只读本配置生成文件。
	$(CC) $(CPPFLAGS) $(CFLAGS) -DAIRI_HELLO_ELF='"$(OUT)/user/hello.elf"' -DAIRI_PROCESS_ELF='"$(OUT)/user/processes.elf"' -DAIRI_WORKER_ELF='"$(OUT)/user/worker.elf"' -DAIRI_FILES_ELF='"$(OUT)/user/files.elf"' -DAIRI_STORAGE_ELF='"$(OUT)/user/storage.elf"' -c $< -o $@

# 第二个用户程序只复用用户入口与库，不把hello的main再次链接进去。
USER_COMMON := $(filter-out $(OUT)/user/hello.o,$(USER_OBJECTS))
```

新增 storage 的编译链接目标，使用同一用户入口和自有库。

<!-- replace: Makefile -->
<!-- before -->
```make
$(OUT)/user/files.elf: $(USER_COMMON) $(OUT)/user/files.o labs/airios/program.ld
# 独立低地址ELF由内核包装后装载。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/files.o -o $@
```
<!-- after -->
```make
$(OUT)/user/files.elf: $(USER_COMMON) $(OUT)/user/files.o labs/airios/program.ld
# 独立低地址ELF由内核包装后装载。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/files.o -o $@

# 存储工作负载有独立用户main，普通启动不自动追加正式记录。
$(OUT)/user/storage.o: labs/airios/storage.c labs/airios/include/airi_libc.h
# 建立配置专属目录。
	@mkdir -p $(dir $@)
# 保持相同用户整数编译约束。
	$(CC) $(USER_FLAGS) -Ilabs/airios/include -c $< -o $@
# storage与recover由同一ELF按明确argv分支。
$(OUT)/user/storage.elf: $(USER_COMMON) $(OUT)/user/storage.o labs/airios/program.ld
# 用户库没有宿主libc依赖。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/storage.o -o $@
```

**更新启动顺序。** scratch恢复演示后扫描正式区，再启动既有用户演示与串口，不偷偷追加正式记录。

先引入正式存储服务。

<!-- replace: kernel/main.c -->
<!-- before -->
```c
#include <airi/journal.h> // 追加记录恢复演示独立于RAM文件。
#include <airi/block.h> // 设备适配在内存、时钟之后初始化。
#include <airi/user.h> // 验证ring3系统调用和故障隔离。
```
<!-- after -->
```c
#include <airi/storage.h> // 正式日志区跨进程和QEMU重启保留。
#include <airi/journal.h> // 追加记录恢复演示独立于RAM文件。
#include <airi/block.h> // 设备适配在内存、时钟之后初始化。
#include <airi/user.h> // 验证ring3系统调用和故障隔离。
```

scratch 演示之后扫描正式区；真实错误停止，不能当空盘继续。

<!-- replace: kernel/main.c -->
<!-- before -->
```c
    }
    airi_block_demo(); // 真正DMA读写、FLUSH与范围拒绝。
    airi_journal_demo(); // 固定实验扇区上提交、校验、坏尾截断与恢复。
    airi_user_demo(); // 正常与非法用户程序结束后仍回到启动栈和串口。
    airi_console_init(&console, info);  // 初始化内核静态控制台，借用整个启动期间有效的 info。
    console.service = airi_user_command; // 系统程序服务独立于可选主题/窗口扩展。
```
<!-- after -->
```c
    }
    airi_block_demo(); // 真正DMA读写、FLUSH与范围拒绝。
    airi_journal_demo(); // 固定实验扇区上提交、校验、坏尾截断与恢复。
    int storage_error = airi_storage_init(); // 启动扫描，绝不自动追加正式记录。
    if (storage_error) { // I/O错误不能当作空盘。
        airi_log("[STORAGE_INIT_ERROR] error=%d", storage_error); // 保留具体失败。
        airi_panic("storage recovery failed"); // 不继续提供错误前缀。
    }
    airi_user_demo(); // 正常与非法用户程序结束后仍回到启动栈和串口。
    airi_console_init(&console, info);  // 初始化内核静态控制台，借用整个启动期间有效的 info。
    console.service = airi_user_command; // 系统程序服务独立于可选主题/窗口扩展。
```

<a id="storage-two-boots"></a>
## 两个 QEMU 先后使用同一块盘，结果分别来自各自启动

这项观察需要保留介质，同时丢弃 CPU、RAM 和串口缓存。两个 QEMU 按先后运行，不能并发操作同一个盘，也不能拿旧 READY 满足新启动。storage_test 为本测试建立独立artifacts/storage-restart-PROFILE目录，每次完整测试显式删除该目录内上次生成的8MiB盘；普通run目录及学习者源码不受影响。异常文件类型、大小或符号链接都会拒绝替换。第一次command排他创建零盘，之后两次boot都传同一路径，第二次不能再创建或截断它。

每次QEMU先-S暂停，UART接收端连接后才QMP cont。每次有新的字节数组和first.log/second.log，旧READY无法满足当前等待。第一次必须scan出count0，再经真实UART发送run storage，等用户成功与本次CONSOLE_UPDATE；停止该QEMU并回收后，第二次从新CPU和新RAM启动，必须scan出count1，run recover再次验证内容。

这里调用terminate属于受控QEMU进程结束，宿主仍运行，后端也可能在结束期间处理缓存；因此它证明跨VM进程恢复，并不独立证明没有FLUSH时会丢哪些数据。写入顺序的故障边界仍由K26明确的live/durable模型验证，真实主机掉电没有执行。

**新建 `scripts/storage_test.py`。** 进程、端点、串口与结果都按本轮生命周期清理，成功必须来自两个真实guest。

<!-- write: scripts/storage_test.py -->
```python
"""Run two actual QEMU boots against one dedicated, freshly created test disk.""" # 跨VM启动与模型crash分别验证。
import json # 记录本轮事实，不执行JSON内容。
from pathlib import Path # 构造项目内独立证据路径。
import socket # 实际UART收发。
import subprocess # 启动和回收真实QEMU。
import tempfile # 短端点路径避免macOS UNIX长度上限。
import time # 单调截止时间不受墙钟变化影响。
from boot import command, qmp_execute # 固定硬件与实际QMP继续。
from tools import ROOT # 不依赖调用者当前目录。

def check_storage_restart(profile='normal'): # 对当前构建正常或图形正常配置运行两次。
    work = ROOT / 'artifacts' / ('storage-restart-' + profile) # 仅此测试拥有的实验盘。
    work.mkdir(parents=True, exist_ok=True) # 证据父目录必须存在。
    result_path = work / 'result.json' # 本轮状态路径。
    result_path.write_text(json.dumps({'passed':False, 'status':'running'}) + '\n') # 旧成功不能覆盖新失败。
    disk = work / 'disk.raw' # 此测试每次从空盘开始，普通run目录不受影响。
    if disk.is_symlink() or (disk.exists() and (not disk.is_file() or disk.stat().st_size != 8 * 1024 * 1024)): # 未知对象不能当测试盘删除。
        raise RuntimeError('Invalid restart-test disk; refusing to replace') # 明确保留异常路径。
    disk.unlink(missing_ok=True) # 只删除本测试上次生成的8MiB盘，下一command排他创建新盘。
    started = time.monotonic() # 整组测试耗时。
    def boot_once(label, kernel_marker, request, user_marker): # 同一work保存盘，端点和CPU每次新建。
        with tempfile.TemporaryDirectory(prefix='airios-store-', dir='/tmp') as directory: # 短临时路径仅放通信端点。
            qmp = Path(directory) / 'qmp.sock' # QMP不是设备输出。
            uart = Path(directory) / 'uart.sock' # UART在CPU继续前连接。
            cmd = command(profile, work) # 复制本轮固件变量，保持同一个disk.raw。
            cmd[cmd.index('-serial') + 1] = f'unix:{uart},server=on,wait=off' # 串口改成真实收发端点。
            cmd += ['-S', '-qmp', f'unix:{qmp},server=on,wait=off'] # 先暂停，接收端准备好才继续。
            serial = work / (label + '.log') # 每个新CPU单独记录，不能读取上次READY。
            data = bytearray() # 只累积本次设备字节。
            with (work / (label + '-qemu.log')).open('w') as errors: # QEMU自身诊断与内核输出分开。
                process = subprocess.Popen(cmd, stdout=errors, stderr=errors) # 启动真实独立进程。
                wire = None # 连接失败也进入finally清理。
                try: # 每个失败都必须收回QEMU。
                    deadline = time.monotonic() + 10 # 端点等待另有总时间上限。
                    while not uart.exists(): # 等待服务端创建端点。
                        if process.poll() is not None or time.monotonic() > deadline: # 已退出或耗时过长。
                            raise RuntimeError('Restart-test UART unavailable') # 不能无限等。
                        time.sleep(.05) # 给QEMU建立端点的时间。
                    wire = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) # 本机UART流，不走网络服务。
                    wire.connect(str(uart)) # CPU仍暂停，早期输出不会丢失。
                    wire.setblocking(False) # 暂无字节不阻塞整个控制循环。
                    qmp_execute(qmp, 'cont') # 接收端存在后继续CPU。
                    def output(): # 累积本次所有已到设备字节。
                        while True: # 取到当前无更多数据为止。
                            try: # 非阻塞接收暂时无数据是正常情况。
                                chunk = wire.recv(4096) # 分块读，不假定一条消息一次到齐。
                            except BlockingIOError: # 稍后再试。
                                break # 本轮读完。
                            if not chunk: # 设备流关闭。
                                break # 后续进程状态给出失败。
                            data.extend(chunk) # 保留原字节顺序。
                        serial.write_bytes(data) # 失败前也保存已收到证据。
                        return data.decode(errors='replace') # 不因单个解码问题丢整份日志。
                    def wait(marker, offset=0): # 只在本次请求后的完整行内找标记。
                        deadline = time.monotonic() + 35 # 真实目标标记最多等待35秒。
                        while time.monotonic() < deadline: # 使用单调截止时间。
                            text = output() # 读取本次设备输出。
                            for line in text[offset:].splitlines(keepends=True): # 保留结束符以拒绝半行。
                                record = line.removeprefix('airi> ').removesuffix('\n').removesuffix('\r') # 只移除已知提示符与CRLF。
                                if line.endswith('\n') and (record == marker or (marker.endswith(' ') and record.startswith(marker))): # 完整等值或明确带空格字段边界，拒绝半行和相似命令计数。
                                    return text # 交回实际累计输出。
                            if process.poll() is not None: # 提前退出不能通过。
                                raise RuntimeError(f'Guest exited before {marker}') # 保留目标身份。
                            time.sleep(.05) # 等设备与CPU推进。
                        raise RuntimeError(f'Restart test timed out: {marker}; see {serial}') # 明确失败文件。
                    text = wait('[AIRI_P3_READY] ') # 全部初始化和用户演示结束后才输入。
                    assert kernel_marker in text and '[AIRI_PANIC]' not in text # 当前新CPU必须扫描出指定前缀。
                    offset = len(text) # 用户命令只消费之后的新输出。
                    wire.sendall((request + '\n').encode()) # 通过实际UART提交控制台命令。
                    text = wait('[CONSOLE_UPDATE] view=0 commands=1 page=0', offset) # 等命令与用户会话全部收拢。
                    assert user_marker in text[offset:] and '[AIRI_PANIC]' not in text[offset:] # 标记来自本次ring3程序。
                finally: # 正常和错误均收回设备与CPU。
                    if wire is not None: # 建立过才关闭。
                        wire.close() # 结束本次UART连接。
                    process.terminate() # 这是受控进程结束，不冒充真实主机掉电。
                    try: # 请求结束还需确认进程已回收。
                        process.wait(timeout=5) # 清理也有截止时间。
                    except subprocess.TimeoutExpired: # QEMU未及时退出。
                        process.kill() # 强制结束本测试子进程。
                        process.wait() # 不遗留僵尸。
    boot_once('first', '[STORAGE_RECOVERED] count=0 truncated=0', 'run storage', '[STORAGE_USER_OK]') # 空盘上由用户实际追加并同步。
    boot_once('second', '[STORAGE_RECOVERED] count=1 truncated=0', 'run recover', '[RECOVERY_USER_OK]') # 新CPU和新内存从同一盘恢复。
    result = {'passed':True, 'profile':profile, 'boots':2, 'recovered_records':1, 'seconds':round(time.monotonic() - started, 2), 'model':'QEMU process restart, not physical power loss'} # 只记录实际完成的模型。
    result_path.write_text(json.dumps(result, indent=2) + '\n') # 两次均完成才覆盖成功。
    print('PASS storage restart: ring3 append/flush -> new QEMU -> scan/read') # 不用宿主读取文件猜客体行为。
    return result # 统一入口可收集当前结果。
```

**更新普通启动验收。** 正式区必须完成scan，隔离设备明确跳过，不能只匹配旧阶段标记。

等待正式区完成扫描或明确跳过；只读盘仍要恢复，不能和超时隔离混为一类。

<!-- replace: scripts/boot.py -->
<!-- before -->
```python
                        assert '[BLOCK_READY]' in text and block_marker in text # 完成、拒绝或超时必须符合本配置。
                        journal_marker = '[JOURNAL_SKIPPED]' if case in ('block-timeout', 'block-readonly') else '[JOURNAL_DEMO_OK]' # 没有可写设备时不冒充日志完成。
                        assert journal_marker in text # 正常配置必须真实恢复并替换坏尾。
                        assert '[FILES_USER_OK]' in text and '[FD_EXEC_OK]' in text # RAM文件和成功exec继承均完成。
                        assert '[PROCESS_FORK_WAIT_OK]' in text and '[PROCESS_DEMO_OK]' in text # 复制、真实阻塞、timer换身份和回收都须完成。
                        assert '[ELF_LOAD]' in text and '[ELF_USER_OK]' in text # 独立ELF段、BSS与用户libc必须真正运行。
```
<!-- after -->
```python
                        assert '[BLOCK_READY]' in text and block_marker in text # 完成、拒绝或超时必须符合本配置。
                        journal_marker = '[JOURNAL_SKIPPED]' if case in ('block-timeout', 'block-readonly') else '[JOURNAL_DEMO_OK]' # 没有可写设备时不冒充日志完成。
                        assert journal_marker in text # 正常配置必须真实恢复并替换坏尾。
                        storage_marker = '[STORAGE_SKIPPED]' if case == 'block-timeout' else '[STORAGE_RECOVERED]' # 只读仍可恢复，隔离盘不能提供空日志。
                        assert storage_marker in text # 正式区必须实际扫描或明确未提供。
                        assert '[FILES_USER_OK]' in text and '[FD_EXEC_OK]' in text # RAM文件和成功exec继承均完成。
                        assert '[PROCESS_FORK_WAIT_OK]' in text and '[PROCESS_DEMO_OK]' in text # 复制、真实阻塞、timer换身份和回收都须完成。
                        assert '[ELF_LOAD]' in text and '[ELF_USER_OK]' in text # 独立ELF段、BSS与用户libc必须真正运行。
```

**更新完整test。** 九种启动和三条输入后，再运行同一构建的双启动恢复检查。

引入双启动恢复观察。

<!-- replace: scripts/manage.py -->
<!-- before -->
```python
from tools import ROOT, tool, make_command  # 复用路径、工具查找与构建参数，不另建工具选择规则。
from deps import prepare  # 复用外部依赖下载与校验，统一入口先调用它。
from boot import check, command  # check 等待启动完成，command 只组装交互QEMU参数。
from console_test import check_console  # 复用真实键盘和UART输入验证。

PROFILES=('normal','no-framebuffer','missing-memory','edit-experiment','trap-fatal','stack-guard','block-readonly','block-timeout','block-missing')  # 元组固定九种构建配置，CLI choices 与 test 遍历共用。
```
<!-- after -->
```python
from tools import ROOT, tool, make_command  # 复用路径、工具查找与构建参数，不另建工具选择规则。
from deps import prepare  # 复用外部依赖下载与校验，统一入口先调用它。
from boot import check, command  # check 等待启动完成，command 只组装交互QEMU参数。
from storage_test import check_storage_restart # 两个独立CPU从同一实验盘恢复。
from console_test import check_console  # 复用真实键盘和UART输入验证。

PROFILES=('normal','no-framebuffer','missing-memory','edit-experiment','trap-fatal','stack-guard','block-readonly','block-timeout','block-missing')  # 元组固定九种构建配置，CLI choices 与 test 遍历共用。
```

完整输入检查之后再运行双启动恢复，使用相同呈现配置。

<!-- replace: scripts/manage.py -->
<!-- before -->
```python
        prefix = 'desktop-' if args.graphics else ''  # 本轮输入验证选择同一呈现方式。
        console_results=[check_console(prefix+'normal'),check_console(prefix+'no-framebuffer'),check_console(prefix+'normal',uart_input=False)]  # 顺序验证normal UART、无帧缓冲UART与normal PS/2三条真实输入路径。
        (ROOT/'artifacts'/('desktop-console-tests.json' if args.graphics else 'console-tests.json')).write_text(json.dumps(console_results,indent=2)+'\n')  # 保存三次实际输入验证结果。
    else:  # 普通build/run/debug共用指定配置构建路径。
        build(args.profile)  # build/run/debug先构建调用者选择的配置。
        if args.action!='build':  # 单独构建到此结束，run和debug继续启动虚拟机。
```
<!-- after -->
```python
        prefix = 'desktop-' if args.graphics else ''  # 本轮输入验证选择同一呈现方式。
        console_results=[check_console(prefix+'normal'),check_console(prefix+'no-framebuffer'),check_console(prefix+'normal',uart_input=False)]  # 顺序验证normal UART、无帧缓冲UART与normal PS/2三条真实输入路径。
        (ROOT/'artifacts'/('desktop-console-tests.json' if args.graphics else 'console-tests.json')).write_text(json.dumps(console_results,indent=2)+'\n')  # 保存三次实际输入验证结果。
        check_storage_restart('desktop-normal' if args.graphics else 'normal') # 独立正式日志区的真实跨QEMU恢复。
    else:  # 普通build/run/debug共用指定配置构建路径。
        build(args.profile)  # build/run/debug先构建调用者选择的配置。
        if args.action!='build':  # 单独构建到此结束，run和debug继续启动虚拟机。
```

<a id="storage-run"></a>
## 从串口实际运行

```sh
./airios test # 包含两个独立QEMU使用同一专用测试盘的验证。
./airios run # 输入run storage，完成后输入run recover。
```

正常run storage显示STORAGE_REQUEST、STORAGE_USER_ERRORS_OK、STORAGE_USER_OK，并返回0；run recover显示RECOVERY_USER_OK。Ctrl-A后X退出，再运行同一默认run配置（其盘为artifacts/interactive-normal/disk.raw），STORAGE_RECOVERED应从该目录的disk.raw扫描出已有count。不要删除对应artifacts/interactive-normal目录，否则也会删除实验盘。

自动恢复证据在artifacts/storage-restart-normal，first.log和second.log分别保存全新内核的输出，result.json只在两次都成功时标passed。当前链已贯通“用户库→系统调用→校验日志→块请求→设备完成→用户继续”，同步等待尚未让其他用户任务继续；后续管道与异步阶段将改变这个等待位置。
