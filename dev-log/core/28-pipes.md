# K28 · 管道先把有限容量、背压和关闭说清楚

从L04和K27继续，不需要图形。文件可以按偏移反复读写，管道则把一次写入的字节交给另一端，读过的字节被消费，不能seek回去。本节先实现四个64字节FIFO，提供显式非阻塞read/write和当前就绪查询；下一节再把暂时不能完成的请求转成真正等待，避免同时引入数据结构和调度两套状态。

<a id="pipe-endpoint-owner"></a>
## 环里有多少字节，端点还由谁持有

现在把父子之间的 child 五字节当作一股只能向前消费的数据。环要记住尚未读的范围，也要知道将来是否还可能有人写：空并不足以决定 EOF。memory_pipe 保存 64 字节数组、head、size、readers、writers。head是下一个读位置，size是未读数量，写位置为(head+size)%64；每次按实际完成数推进，不用零字节当“空”标记，因此任何字节值都能传输。读和写各有一个打开对象，两个对象引用同一环。

有了字节环，还不能按某一次 close 就决定没有写者；fork 后父子可能持有同一个端点。FD 仍按 K24 引用打开对象。dup/fork只加打开对象references，不直接加环的readers/writers。最后一个表项引用消失时才关闭该端点对象，再减相应端点数；两端都结束才把缓冲清零复用。因此父亲关闭写FD时，若孩子还持同一个写对象，读者不能提前EOF。exit和成功exec的既有关闭规则会沿同一引用路径更新端点。

创建pipe必须同时取得两个空FD、两个全局打开槽和一个空环，全部成功后才发布。进程FD不足返回−24，全局对象不足−23，四个环耗尽−28，输出数组和已有表项都保持不变。不能先交出读端，写端失败后留下半个无法使用的对象。

**更新内核文件声明。** 新增NONBLOCK和自有就绪位，值不宣称兼容Linux poll/epoll。

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
#endif // 结束保护。
```

<a id="pipe-conditions"></a>
## 暂时空、永久结束与发生错误不是同一个结果

先看同一个空读端。若孩子仍持写端，它以后可能写入，当前返回 −11；若最后一个写端已经关闭，等待也不会产生新字节，空读才返回 EOF。读端有数据时返回当前前缀，最多64字节；没有数据但仍有写者时返回−11，表示暂时不能完成。无写者时仍先把已有数据读完，空后才返回0表示EOF。写端无读者返回−32，本项目不发送SIGPIPE；有读者但满返回−11，有空位时只写当前能放下的前缀。这种短写策略不实现Linux PIPE_BUF的小写入原子性。

零长度在访问模式核对后立即返回0，不消费字节或等待。pread/pwrite和seek对管道返回−29，因为FIFO没有可定位的文件位置。RAM文件、固定终端和流分别走明确分支，不能仅凭node为空就把所有非文件对象当串口。

用户不一定立刻读写，也可以先问“现在操作会怎样”。ready 只查询当前条件，不保存事件。读端有数据或将读到EOF时给READ；最后写者关闭还给HUP，HUP不代表可以丢弃未读数据。写端有读者且有空位时给WRITE，无读者给ERROR。返回负数时是接口错误，不能当成四个事件位全置位。另一个进程可能在查询后先读走数据，因此就绪不保证下一次操作仍有进展；调用者仍须检查实际返回。

**从 K27 的 `kernel/fs/file.c` 改六处。** 先加入环与端点寿命，再接传输、创建与查询；每处都给出完整旧/新窗口，RAM 文件已有算法保留。

先为打开对象加入可选 pipe 指针，并建立四个环。一个端点对象只引用其中一个环；FD 表项仍引用打开对象，不直接管理 readers/writers。

<!-- replace: kernel/fs/file.c -->
<!-- before -->
```c
#define OPEN_COUNT 24u // 全局打开对象容量与每进程FD容量不同。
#define FILE_BYTES 1024u // 每个可写文件的固定字节上限。
struct memory_file { char name[32]; uint8_t data[FILE_BYTES]; size_t size; bool readonly; }; // 名字和内容跨程序会话存活，重启丢失。
struct airi_open_file { struct memory_file *node; uint64_t offset; unsigned flags, references; }; // 多个表项可共享同一个偏移。
static struct memory_file nodes[NODE_COUNT]; // 初值零表示空目录槽。
static struct airi_open_file opened[OPEN_COUNT]; // 引用数零表示空打开槽。
static struct airi_open_file terminal; // 固定串口对象，不含RAM文件内容。
static struct airi_open_file *get(const struct airi_fd_table *table, unsigned fd) { // 用户整数必须先做范围检查。
    return fd < AIRI_FD_COUNT ? table->entries[fd].file : NULL; // 不把FD转成内核指针。
```
<!-- after -->
```c
#define OPEN_COUNT 24u // 全局打开对象容量与每进程FD容量不同。
#define FILE_BYTES 1024u // 每个可写文件的固定字节上限。
struct memory_file { char name[32]; uint8_t data[FILE_BYTES]; size_t size; bool readonly; }; // 名字和内容跨程序会话存活，重启丢失。
struct memory_pipe { uint8_t data[64]; unsigned head, size, readers, writers; }; // 四个固定环，端点最终引用消失才回收。
struct airi_open_file { struct memory_file *node; struct memory_pipe *pipe; uint64_t offset; unsigned flags, references; }; // 多个表项可共享同一个偏移。
static struct memory_file nodes[NODE_COUNT]; // 初值零表示空目录槽。
static struct airi_open_file opened[OPEN_COUNT]; // 引用数零表示空打开槽。
static struct memory_pipe pipes[4]; // readers与writers都零表示未使用的管道。
static struct airi_open_file terminal; // 固定串口对象，不含RAM文件内容。
static struct airi_open_file *get(const struct airi_fd_table *table, unsigned fd) { // 用户整数必须先做范围检查。
    return fd < AIRI_FD_COUNT ? table->entries[fd].file : NULL; // 不把FD转成内核指针。
```

在启动初始化里清空环。它和 RAM 节点一样只在无活动 FD 的启动阶段重置，不在每次 read 时清空。

<!-- replace: kernel/fs/file.c -->
<!-- before -->
```c
}
void airi_files_init(void) { // 启动一次，不能在仍有FD时重置。
    airi_memset(nodes, 0, sizeof(nodes)); // 清空名字、内容和长度。
    airi_memset(opened, 0, sizeof(opened)); // 清空全局打开对象。
    airi_memset(&terminal, 0, sizeof(terminal)); // 固定对象没有自有节点。
    terminal.flags = AIRI_OPEN_WRITE; // 当前只提供stdout/stderr。
```
<!-- after -->
```c
}
void airi_files_init(void) { // 启动一次，不能在仍有FD时重置。
    airi_memset(nodes, 0, sizeof(nodes)); // 清空名字、内容和长度。
    airi_memset(pipes, 0, sizeof(pipes)); // 启动前没有端点或在途字节。
    airi_memset(opened, 0, sizeof(opened)); // 清空全局打开对象。
    airi_memset(&terminal, 0, sizeof(terminal)); // 固定对象没有自有节点。
    terminal.flags = AIRI_OPEN_WRITE; // 当前只提供stdout/stderr。
```

扩展最后引用关闭的位置。一个 FD 关闭尚不等于端点结束，只有 references 降为零才减 readers 或 writers；两端都结束以后才允许复用环。

<!-- replace: kernel/fs/file.c -->
<!-- before -->
```c
    if (!file) // 无效或已关闭。
        return -9; // -EBADF。
    table->entries[fd] = (struct airi_fd_entry){0}; // 先撤掉进程引用。
    if (--file->references == 0 && file != &terminal) // 最后引用消失才归还全局槽。
        airi_memset(file, 0, sizeof(*file)); // 下一次open取得独立偏移。
    return 0; // 节点内容保留到重启或truncate。
}
void airi_fd_close_all(struct airi_fd_table *table, bool exec_only) { // 多次清理空表安全。
```
<!-- after -->
```c
    if (!file) // 无效或已关闭。
        return -9; // -EBADF。
    table->entries[fd] = (struct airi_fd_entry){0}; // 先撤掉进程引用。
    if (--file->references == 0 && file != &terminal) { // 最后表项引用消失才关闭真实端点。
        if (file->pipe) { // 普通RAM文件没有管道对端状态。
            if (file->flags & AIRI_OPEN_READ) // 当前是只读端点打开对象。
                --file->pipe->readers; // dup/fork共享对象，不能每关一个FD都减端点数。
            else // pipe创建的另一个打开对象是写端。
                --file->pipe->writers; // 最后写端消失后，读者排空再EOF。
            if (!file->pipe->readers && !file->pipe->writers) // 所有端点引用真正结束。
                airi_memset(file->pipe, 0, sizeof(*file->pipe)); // 未读内容丢弃，槽可供新管道使用。
        }
        airi_memset(file, 0, sizeof(*file)); // 回收全局打开对象，不误清别人的共享偏移。
    }
    return 0; // 节点内容保留到重启或truncate。
}
void airi_fd_close_all(struct airi_fd_table *table, bool exec_only) { // 多次清理空表安全。
```

普通 open 改用指定字段初始化，新增 pipe 字段自然为零；RAM 文件继续走 node 路径。

<!-- replace: kernel/fs/file.c -->
<!-- before -->
```c
    }
    if (flags & AIRI_OPEN_TRUNCATE) // 有写权限且不会因表满再失败。
        node->size = 0; // 后续扩洞时再显式清零，旧尾不对读者可见。
    *file = (struct airi_open_file){node, 0, flags & 3u, 1}; // 独立open得到新偏移0。
    table->entries[fd] = (struct airi_fd_entry){file, (flags & AIRI_OPEN_CLOEXEC) != 0}; // 标志按表项保存。
    return fd; // 用户只拿整数。
}
```
<!-- after -->
```c
    }
    if (flags & AIRI_OPEN_TRUNCATE) // 有写权限且不会因表满再失败。
        node->size = 0; // 后续扩洞时再显式清零，旧尾不对读者可见。
    *file = (struct airi_open_file){.node = node, .flags = flags & 3u, .references = 1}; // 独立open得到新偏移0。
    table->entries[fd] = (struct airi_fd_entry){file, (flags & AIRI_OPEN_CLOEXEC) != 0}; // 标志按表项保存。
    return fd; // 用户只拿整数。
}
```

在 transfer 的模式检查之后加入 FIFO 分支。已有字节、仍有写者与仍有读者分别决定本次可返回的前缀或错误；分支结束返回，不会再进入 RAM 文件偏移逻辑。

<!-- replace: kernel/fs/file.c -->
<!-- before -->
```c
    struct airi_open_file *file = get(table, fd); // 当前表里的整数转内部对象。
    if (!file || !(file->flags & (write ? AIRI_OPEN_WRITE : AIRI_OPEN_READ))) // 错误访问模式也返回EBADF。
        return -9; // 无副作用。
    struct memory_file *node = file->node; // 终端不在此处直接访问端口。
    if (!node) // 不支持的设备操作。
        return -29; // -ESPIPE。
```
<!-- after -->
```c
    struct airi_open_file *file = get(table, fd); // 当前表里的整数转内部对象。
    if (!file || !(file->flags & (write ? AIRI_OPEN_WRITE : AIRI_OPEN_READ))) // 错误访问模式也返回EBADF。
        return -9; // 无副作用。
    if (file->pipe) { // 端点使用FIFO字节流，不使用文件偏移。
        struct memory_pipe *pipe = file->pipe; // 内核共享缓冲，不直接借用户地址。
        if (positioned) // pread/pwrite不能定位流。
            return -29; // -ESPIPE。
        if (!bytes) // 零长度不消费或等待，不检查对端存在。
            return 0; // 访问模式已经核对。
        if (write && !pipe->readers) // 没有任何读端，数据永远无人接收。
            return -32; // 自有EPIPE，不发Linux SIGPIPE。
        unsigned available = write ? (unsigned)sizeof(pipe->data) - pipe->size : pipe->size; // 写看空位，读看已占字节。
        if (!available) // 暂无进展不等于永久结束。
            return !write && !pipe->writers ? 0 : -11; // 读端无写者为EOF，其余EAGAIN。
        if (bytes > available) // 本项目允许有界短写，不宣称Linux PIPE_BUF原子性。
            bytes = available; // 只处理当前可完成前缀。
        for (size_t i = 0; i < bytes; ++i) // 最多64字节，直接按环下标复制。
            if (write) // 写入当前尾部，不覆盖未读数据。
                pipe->data[(pipe->head + pipe->size + i) % sizeof(pipe->data)] = ((const uint8_t *)buffer)[i]; // 尾部越界时回到0。
            else // 读取最老的未消费字节。
                ((uint8_t *)buffer)[i] = pipe->data[(pipe->head + i) % sizeof(pipe->data)]; // 按FIFO顺序交回。
        if (write) // 完成数据复制后再发布可读数量。
            pipe->size += (unsigned)bytes; // 后续就绪查询才能看到新字节。
        else { // 读后释放环空间。
            pipe->head = (pipe->head + (unsigned)bytes) % (unsigned)sizeof(pipe->data); // 推进读头。
            pipe->size -= (unsigned)bytes; // 写者随后可继续。
        }
        return (long)bytes; // 实际前缀长度，不保留本次buffer。
    }
    struct memory_file *node = file->node; // 终端不在此处直接访问端口。
    if (!node) // 不支持的设备操作。
        return -29; // -ESPIPE。
```

最后加入原子创建和当前条件查询。创建先找齐两个 FD、两个打开槽和一个环，再一起发布；ready 只读这些状态，不替未来调用预留容量。

<!-- replace: kernel/fs/file.c -->
<!-- before -->
```c
        count += opened[i].references != 0; // 返回对象数量，不是表项数量。
    return count; // 终端固定对象单独管理，不占此表。
}
```
<!-- after -->
```c
        count += opened[i].references != 0; // 返回对象数量，不是表项数量。
    return count; // 终端固定对象单独管理，不占此表。
}

long airi_fd_pipe(struct airi_fd_table *table, unsigned ends[2], bool nonblocking) { // 所有容量检查完成前不发布任何端点。
    unsigned fds[2], count = 0; // 两个最低空编号，不先占一个再回滚。
    for (unsigned i = 0; i < AIRI_FD_COUNT && count < 2; ++i) // 有界寻找两个空表项。
        if (!table->entries[i].file) // 不碰已有文件和终端。
            fds[count++] = i; // 只保存在内核局部数组。
    if (count != 2) // 不能留下半个管道。
        return -24; // -EMFILE。
    struct airi_open_file *files[2]; // 两个独立端点打开对象。
    count = 0; // 复用计数只找本类资源。
    for (unsigned i = 0; i < OPEN_COUNT && count < 2; ++i) // 全局容量独立检查。
        if (!opened[i].references) // 未发布槽。
            files[count++] = &opened[i]; // 尚未加引用。
    if (count != 2) // 有FD不代表有内核打开槽。
        return -23; // -ENFILE。
    struct memory_pipe *pipe = NULL; // 只有两个端点都消失才可复用缓冲。
    for (unsigned i = 0; i < 4; ++i) // 固定四个环。
        if (!pipes[i].readers && !pipes[i].writers) { // 未使用，不会匹配仍有一端的流。
            pipe = &pipes[i]; // 取得整个环的所有权。
            break; // 不重复占用。
        }
    if (!pipe) // 全局流数量达到上限。
        return -28; // -ENOSPC，输出与表都仍未改。
    airi_memset(pipe, 0, sizeof(*pipe)); // 新流不含上一流未读字节。
    pipe->readers = pipe->writers = 1; // 每端一个打开对象，引用数由FD再管理。
    for (unsigned i = 0; i < 2; ++i) { // 分别发布只读端和只写端。
        *files[i] = (struct airi_open_file){.pipe = pipe, .flags = (i ? AIRI_OPEN_WRITE : AIRI_OPEN_READ) | (nonblocking ? AIRI_OPEN_NONBLOCK : 0), .references = 1}; // 两端共享同一个环。
        table->entries[fds[i]] = (struct airi_fd_entry){.file = files[i]}; // 初始不设CLOEXEC，dup可另选。
        ends[i] = fds[i]; // 所有资源都齐后才写输出编号。
    }
    return 0; // 不把内部指针交给用户。
}
long airi_fd_ready(const struct airi_fd_table *table, unsigned fd) { // 查询当前条件，不保存边沿事件。
    struct airi_open_file *file = get(table, fd); // 完整编号由陷入层校验。
    if (!file) // 已关闭不该继续使用旧就绪状态。
        return -9; // -EBADF。
    if (!file->pipe) // 不把RAM文件或终端冒充已支持的事件源。
        return -95; // -EOPNOTSUPP。
    const struct memory_pipe *pipe = file->pipe; // 同一不可抢占调用内状态稳定。
    if (file->flags & AIRI_OPEN_READ) // 读端可读包括EOF。
        return (pipe->size || !pipe->writers ? AIRI_READY_READ : 0) | (!pipe->writers ? AIRI_READY_HUP : 0); // HUP不丢弃仍可读取数据。
    return (pipe->readers && pipe->size < sizeof(pipe->data) ? AIRI_READY_WRITE : 0) | (!pipe->readers ? AIRI_READY_ERROR : 0); // 就绪描述条件，不保证下次操作一定成功。
}
bool airi_fd_nonblocking(const struct airi_fd_table *table, unsigned fd) { // 后续系统调用层决定遇EAGAIN如何处理。
    struct airi_open_file *file = get(table, fd); // 不直接解用户值。
    return file && (file->flags & AIRI_OPEN_NONBLOCK); // 不支持或无效对象不会被当成非阻塞流。
}
```

<a id="pipe-user-boundary"></a>
## 两个FD通过一份已验证的用户输出交回

pipe 创建必须一次交回一对可用端点。若先改内核表，最后才发现用户输出不可写，调用者会丢失刚创建端点的编号。因此先验证输出，再原子创建。编号 20 接受 RDI 指向两个 unsigned 的输出数组、RSI为nonblocking。当前只接受1，0先返回−22，阻塞模式接下一节；不会偷偷让用户循环轮询冒充内核睡眠。先验证8字节输出区可写，再调用原子创建，最后复制两个编号。编号21查询一个FD当前条件，完整64位值先限制在表范围内。

用户read/write继续使用K24的256字节副本边界。请求100字节在接口预算内，但管道容量只允许64，实际返回64；这使短成功与−11可以连续观察。设备或管道都不在返回后借用户buffer。

**更新用户服务的四个窗口。** 加入 pipes 映像、20/21 调用及启动演示；这一轮没有保存等待请求，也没有阻塞管道状态。

在映像声明中加入 pipes ELF。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
extern const uint8_t airi_user_rsp_bad_start[], airi_user_rsp_bad_end[]; // 不规范用户返回栈。
extern const uint8_t airi_hello_elf_start[], airi_hello_elf_end[]; // 既有独立用户C程序。
extern const uint8_t airi_process_elf_start[], airi_process_elf_end[]; // 新的fork/wait和抢占工作负载。
extern const uint8_t airi_storage_elf_start[], airi_storage_elf_end[]; // 写入和恢复共用一份明确分支的用户ELF。
extern const uint8_t airi_files_elf_start[], airi_files_elf_end[]; // 内存文件用户工作负载。
extern const uint8_t airi_worker_elf_start[], airi_worker_elf_end[]; // exec目标是另一个独立静态ELF。
```
<!-- after -->
```c
extern const uint8_t airi_user_rsp_bad_start[], airi_user_rsp_bad_end[]; // 不规范用户返回栈。
extern const uint8_t airi_hello_elf_start[], airi_hello_elf_end[]; // 既有独立用户C程序。
extern const uint8_t airi_process_elf_start[], airi_process_elf_end[]; // 新的fork/wait和抢占工作负载。
extern const uint8_t airi_pipes_elf_start[], airi_pipes_elf_end[]; // 非阻塞管道用户程序。
extern const uint8_t airi_storage_elf_start[], airi_storage_elf_end[]; // 写入和恢复共用一份明确分支的用户ELF。
extern const uint8_t airi_files_elf_start[], airi_files_elf_end[]; // 内存文件用户工作负载。
extern const uint8_t airi_worker_elf_start[], airi_worker_elf_end[]; // exec目标是另一个独立静态ELF。
```

给新映像登记 pipes 名字；程序仍经已有装载路径运行。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    {"hello", airi_hello_elf_start, airi_hello_elf_end}, // 独立C运行库与BSS观察。
    {"processes", airi_process_elf_start, airi_process_elf_end}, // fork/exec/wait实际工作负载。
    {"worker", airi_worker_elf_start, airi_worker_elf_end},
    {"storage", airi_storage_elf_start, airi_storage_elf_end}, // 显式提交记录。
    {"recover", airi_storage_elf_start, airi_storage_elf_end}, // 只读取已恢复记录。
    {"files", airi_files_elf_start, airi_files_elf_end}, // 真实用户FD演示。
```
<!-- after -->
```c
    {"hello", airi_hello_elf_start, airi_hello_elf_end}, // 独立C运行库与BSS观察。
    {"processes", airi_process_elf_start, airi_process_elf_end}, // fork/exec/wait实际工作负载。
    {"worker", airi_worker_elf_start, airi_worker_elf_end},
    {"pipes", airi_pipes_elf_start, airi_pipes_elf_end}, // 有界FIFO与关闭语义。
    {"storage", airi_storage_elf_start, airi_storage_elf_end}, // 显式提交记录。
    {"recover", airi_storage_elf_start, airi_storage_elf_end}, // 只读取已恢复记录。
    {"files", airi_files_elf_start, airi_files_elf_end}, // 真实用户FD演示。
```

在 count 分支后接入 20/21。20 先验证整份输出，再创建并复制两个 FD；21 只把完整范围内的编号交给 ready。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
        f->rax = (uint64_t)result; // 同一次用户调用取得精确结果。
    } else if (f->rax == 19) // 已恢复前缀数值查询。
        f->rax = (uint64_t)airi_storage_count(); // 缺设备或不确定状态返回负数。
    else // 尚未提供的接口都明确拒绝。
        f->rax = (uint64_t)-38ll; // -ENOSYS。
    return airi_user_resume(f); // 普通调用继续同一用户，仍检查RSP与标志。
```
<!-- after -->
```c
        f->rax = (uint64_t)result; // 同一次用户调用取得精确结果。
    } else if (f->rax == 19) // 已恢复前缀数值查询。
        f->rax = (uint64_t)airi_storage_count(); // 缺设备或不确定状态返回负数。
    else if (f->rax == 20) { // pipe原子创建并返回两个整数，不暴露内部环。
        unsigned ends[2]; // 内核副本，发布前用户输出已验证。
        long result; // 0或负错误。
        if (f->rsi != 1) // 本节只发布非阻塞模式，阻塞接下一阶段。
            result = -22; // -EINVAL。
        else if (!user_access(current, f->rdi, sizeof(ends), true)) // 坏输出不能创建无法交给用户的FD。
            result = -14; // -EFAULT。
        else { // 没有半次FD分配的用户可见状态。
            result = airi_fd_pipe(&current->files, ends, true); // 两端和缓冲都齐后才成功。
            if (!result && !copy_user(current, ends, f->rdi, sizeof(ends), true)) // 映射在本次调用中不应变化。
                airi_panic("validated pipe output mapping changed"); // 明确内核不变量。
        }
        f->rax = (uint64_t)result; // 失败输出保持原值。
    } else if (f->rax == 21) // 当前就绪条件查询，不保存通知事件。
        f->rax = f->rdi < AIRI_FD_COUNT ? (uint64_t)airi_fd_ready(&current->files, (unsigned)f->rdi) : (uint64_t)-9ll; // 完整64位编号先限制。
    else // 尚未提供的接口都明确拒绝。
        f->rax = (uint64_t)-38ll; // -ENOSYS。
    return airi_user_resume(f); // 普通调用继续同一用户，仍检查RSP与标志。
```

最后把 pipes 接进启动演示与 programs 提示，原有 files 和 storage 仍各自运行。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    run_one(airi_hello_elf_start, (size_t)((uintptr_t)airi_hello_elf_end - (uintptr_t)airi_hello_elf_start), "hello", 0, true, false, false); // 独立C和自有用户库。
    run_one(airi_process_elf_start, (size_t)((uintptr_t)airi_process_elf_end - (uintptr_t)airi_process_elf_start), "processes", 0, true, false, true); // 真实多进程复制、抢占、阻塞与回收。
    run_one(airi_files_elf_start, (size_t)((uintptr_t)airi_files_elf_end - (uintptr_t)airi_files_elf_start), "files", 0, true, false, false); // 文件内容、共享偏移与exec引用回收。
    airi_log("[USER_DEMO_OK] ring=3 syscall=int80 bad_pointers=2 user_gp=13 user_pf=14 bad_rsp=rejected reclaimed=1"); // 完整会话结束，仍回串口命令。
}

bool airi_user_command(struct airi_console *console, const char *command) { // 系统命令消费者独立于图形主题扩展。
    size_t bytes = airi_strlen(command); // 先取得有效长度，固定前缀比较不能越过短字符串。
    if (bytes == 8 && !airi_memcmp(command, "programs", 8)) { // 显示当前初始ELF目录。
        airi_console_print(console, "programs: hello processes worker broken files storage recover"); // broken是明确损坏的演示输入。
        return true; // 已处理，不再交给图形扩展。
    }
    if (bytes < 4 || airi_memcmp(command, "run ", 4)) // 只接受明确的run空格前缀。
```
<!-- after -->
```c
    run_one(airi_hello_elf_start, (size_t)((uintptr_t)airi_hello_elf_end - (uintptr_t)airi_hello_elf_start), "hello", 0, true, false, false); // 独立C和自有用户库。
    run_one(airi_process_elf_start, (size_t)((uintptr_t)airi_process_elf_end - (uintptr_t)airi_process_elf_start), "processes", 0, true, false, true); // 真实多进程复制、抢占、阻塞与回收。
    run_one(airi_files_elf_start, (size_t)((uintptr_t)airi_files_elf_end - (uintptr_t)airi_files_elf_start), "files", 0, true, false, false); // 文件内容、共享偏移与exec引用回收。
    run_one(airi_pipes_elf_start, (size_t)((uintptr_t)airi_pipes_elf_end - (uintptr_t)airi_pipes_elf_start), "pipes", 0, true, false, false); // 真实FIFO、背压、关闭和fork共享。
    airi_log("[USER_DEMO_OK] ring=3 syscall=int80 bad_pointers=2 user_gp=13 user_pf=14 bad_rsp=rejected reclaimed=1"); // 完整会话结束，仍回串口命令。
}

bool airi_user_command(struct airi_console *console, const char *command) { // 系统命令消费者独立于图形主题扩展。
    size_t bytes = airi_strlen(command); // 先取得有效长度，固定前缀比较不能越过短字符串。
    if (bytes == 8 && !airi_memcmp(command, "programs", 8)) { // 显示当前初始ELF目录。
        airi_console_print(console, "programs: hello processes worker broken files storage recover pipes"); // broken是明确损坏的演示输入。
        return true; // 已处理，不再交给图形扩展。
    }
    if (bytes < 4 || airi_memcmp(command, "run ", 4)) // 只接受明确的run空格前缀。
```

**更新用户头。** 就绪位与read/write负错误分开，pipe当前明确要求nonblocking=1。

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
#define AIRI_READABLE 1u // 当前read可得到数据或EOF。
#define AIRI_WRITABLE 2u // 当前有读者和空位。
#define AIRI_HANGUP 4u // 最后写者关闭，仍应排空已有数据。
#define AIRI_ERROR 8u // 最后读者关闭，写将EPIPE。
long airi_pipe(unsigned ends[2], unsigned nonblocking); // 本节仅支持nonblocking=1。
long airi_ready(unsigned fd); // 管道当前条件查询，不是Linux epoll ABI。
#endif // 结束包含保护。
```

**更新用户包装。** 编号和三个参数约定仍为自己的int80 ABI。

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

long airi_pipe(unsigned ends[2], unsigned nonblocking) { // 内核先验证输出再原子创建两个FD。
    return call3(20, (uintptr_t)ends, nonblocking, 0); // 成功后ends[0]读、ends[1]写。
}
long airi_ready(unsigned fd) { // 仅查询当前管道条件。
    return call3(21, fd, 0, 0); // 返回掩码或负错误，不能把负数当事件位。
}
```

<a id="pipe-user-walk"></a>
## 沿100字节请求观察一轮回绕

用同一段数据走过容量与回绕：pipes 生成 0–99 的字节序列。空流读返回−11，写100只完成64，下一字节写返回−11。先读10个字节0–9，再写输入开头10字节，尾部回到数组开头。接着读100实际返回64，顺序必须是原10–63再接新0–9，不能覆盖尚未读出的原内容。

读完这轮内容后再关闭写端，才能把“暂时空”和“永远结束”的结果作直接对照。关闭写端后，空读端同时给 READ 与 HUP，read返回0；另一条新管道关闭读端后，写返回−32并有ERROR。最后创建父子流：孩子关闭读端、写child并关闭写端；父亲关闭自己的写端并wait，随后读到五字节，再读EOF。父子共享的是内核环，用户缓冲页仍各自复制。

**新建 `labs/airios/pipes.c`。** 每种结果都由真实用户调用验证后才打印PIPE_USER_OK。

<!-- write: labs/airios/pipes.c -->
```c
#include <airi_libc.h> // 真正ring3管道工作负载。
int main(void) { // 本节全部显式非阻塞，暂不让内核睡眠。
    unsigned ends[2]; // 由内核原子填写读/写FD。
    if (airi_pipe(ends, 1)) // 1表示本节已实现的非阻塞模式。
        return 70; // 创建失败不能用未初始化编号。
    char data[100], output[100]; // 两个用户缓冲，设备不持有其地址。
    for (unsigned i = 0; i < sizeof(data); ++i) // 使用不同字节检验FIFO和回绕。
        data[i] = (char)i; // 可复现序列0–99。
    if (airi_ready(ends[0]) != 0 || airi_ready(ends[1]) != AIRI_WRITABLE || airi_read(ends[0], output, 1) != -11) // 空流是暂不可读，不是EOF。
        return 71; // 保留写端时read不能提前返回0。
    if (airi_write(ends[1], data, 100) != 64 || airi_write(ends[1], data, 1) != -11 || airi_ready(ends[1]) != 0) // 固定64字节容量产生真实短写和背压。
        return 72; // 不把满环当已完成100字节。
    if (airi_read(ends[0], output, 10) != 10 || memcmp(output, data, 10) || airi_write(ends[1], data, 10) != 10) // 先释放十个空位，再回绕写十字节。
        return 73; // 只写当前有空间的前缀。
    if (airi_read(ends[0], output, 100) != 64 || memcmp(output, data + 10, 54) || memcmp(output + 54, data, 10)) // FIFO次序必须跨环尾保持。
        return 74; // 未读数据不能被覆盖。
    if (airi_close(ends[1]) || airi_ready(ends[0]) != (AIRI_READABLE | AIRI_HANGUP) || airi_read(ends[0], output, 1) != 0 || airi_close(ends[0])) // 最后写者关闭才变EOF。
        return 75; // HUP和可读EOF可以同时存在。
    if (airi_pipe(ends, 1) || airi_close(ends[0]) || airi_write(ends[1], data, 1) != -32 || airi_ready(ends[1]) != AIRI_ERROR || airi_close(ends[1])) // 无读者写为EPIPE，不发Linux SIGPIPE。
        return 76; // 错误也是可查询状态。
    if (airi_pipe(ends, 1)) // 新管道必须不含上次字节。
        return 77; // 不能复用仍被持有的环。
    long child = airi_fork(); // 父子复制表项，端点打开对象的引用增加。
    if (child < 0) // 明确进程容量错误。
        return 78; // 不等待不存在孩子。
    if (!child) { // 孩子保留写端，父保留读端。
        if (airi_close(ends[0]) || airi_write(ends[1], "child", 5) != 5 || airi_close(ends[1])) // 真正用户间传字节。
            return 79; // 父wait取得具体失败。
        return 0; // exit清余下FD引用。
    }
    int status = -1; // 自有普通退出码。
    if (airi_close(ends[1]) || airi_waitpid(child, &status) != child || status || airi_read(ends[0], output, 10) != 5 || memcmp(output, "child", 5) || airi_read(ends[0], output, 1) != 0 || airi_close(ends[0])) // 父关闭自己写端，孩子最后写端也关闭后才能EOF。
        return 80; // 不能因一个FD关闭就提前消灭整个管道。
    airi_puts("[PIPE_USER_OK] short_write=64 backpressure=-11 wrap=fifo eof=0 broken=-32 fork=shared\n"); // 所有真实用户状态成立后发布。
    return 0; // 整个会话还会核对全局对象回收。
}
```

**更新映像包装。** 管道程序单独链接，不把main直接当内核函数调用。

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
```

**更新文件主机检查。** 额外验证回绕、父子端点最后引用、零长度和第五条管道创建失败不泄漏；ASan/UBSan与guest结果分别记录。

<!-- write: tests/unit/file_test.c -->
```c
#include <airi/file.h> // 与guest相同的文件算法，宿主不访问UART。
#include <assert.h> // 每条断言验证具体所有权或边界。
#include <stdio.h> // 宿主只输出最终结果。
int main(void) { // 独立进程，每次建立干净RAM目录。
    airi_files_init(); // 与启动一致的只读seed。
    struct airi_fd_table parent, child; // 两份进程表引用同一全局对象。
    airi_fd_init(&parent); // stdout/stderr占1和2，槽0可用。
    assert(airi_fd_open(&parent, "/seed", AIRI_OPEN_WRITE) == -13); // 只读节点不能绕过。
    assert(airi_fd_open(&parent, "/bad/path", AIRI_OPEN_READ) == -22); // 不假装有目录遍历。
    long fd = airi_fd_open(&parent, "/seed", AIRI_OPEN_READ); // 第一份独立打开对象。
    assert(fd == 0); // 最小空编号。
    long dupped = airi_fd_dup(&parent, (unsigned)fd, true); // 同一对象的第二表项。
    assert(dupped == 3 && airi_files_open_count() == 1); // dup不新建全局对象。
    airi_fd_clone(&child, &parent); // fork复制表项、增加引用。
    char data[16] = {0}; // 核对实际返回的文件字节。
    assert(airi_fd_read(&child, 0, data, 2, false, 0) == 2 && !airi_memcmp(data, "ab", 2)); // 子先消费ab。
    airi_fd_close_all(&child, true); // 成功exec只关其CLOEXEC项。
    assert(airi_fd_read(&child, 3, data, 1, false, 0) == -9); // 关闭不影响父同号。
    assert(airi_fd_read(&parent, 3, data, 2, false, 0) == 2 && !airi_memcmp(data, "cd", 2)); // 父dup看到共享偏移。
    assert(airi_fd_read(&child, 0, data, 8, true, 0) == 6); // pread读6字节，不改变偏移4。
    assert(airi_fd_read(&child, 0, data, 2, false, 0) == 2 && !airi_memcmp(data, "ef", 2)); // 普通读继续4。
    airi_fd_close_all(&child, false); // 子退出释放其余引用。
    airi_fd_close_all(&parent, false); // 最后引用归还全局对象。
    assert(!airi_files_open_count()); // 不遗留只增不减的引用。
    airi_fd_init(&parent); // 新会话不清除文件内容。
    fd = airi_fd_open(&parent, "/notes", 15); // 读写+创建+清空。
    assert(fd == 0); // 编号复用。
    assert(airi_fd_write(&parent, 0, "abcdef", 6, false, 0) == 6); // 初始内容。
    assert(airi_fd_write(&parent, 0, "X", 1, true, 1023) == 1); // 显式定位使中间成为洞。
    assert(airi_fd_write(&parent, 0, "Z", 1, true, 1024) == -28); // 容量满错误。
    assert(airi_fd_read(&parent, 0, data, 16, true, 10) == 16); // 洞可见为零。
    for (unsigned i = 0; i < sizeof(data); ++i) // 所有字节必须被明确初始化。
        assert(data[i] == 0); // 不泄漏旧内容。
    while (airi_fd_dup(&parent, 0, false) >= 0) {} // 填满进程表，多个表项仍只用一个对象。
    assert(airi_fd_open(&parent, "/notes", 11) == -24); // 表满时TRUNCATE必须尚未执行。
    assert(airi_fd_read(&parent, 0, data, 6, true, 0) == 6 && !airi_memcmp(data, "abcdef", 6)); // 失败open未清旧内容。
    airi_fd_close_all(&parent, false); // 释放所有共享引用。
    assert(!airi_files_open_count()); // 同一对象不会被过早或重复回收。
    struct airi_fd_table tables[2]; // 两个进程可耗尽全局24个对象。
    airi_fd_init(&tables[0]); // 每个进程还有十四个空FD。
    airi_fd_init(&tables[1]); // 全局容量与单进程上限不同。
    for (unsigned i = 0; i < 24; ++i) // 第一表开14份，第二表开10份。
        assert(airi_fd_open(&tables[i < 14 ? 0 : 1], "/seed", AIRI_OPEN_READ) >= 0); // 每次open得到独立对象。
    assert(airi_fd_open(&tables[1], "/notes", 11) == -23); // 还有FD但没有打开对象，不能truncate。
    airi_fd_close_all(&tables[0], false); // 清全局引用的一部分。
    airi_fd_close_all(&tables[1], false); // 清剩余部分。
    assert(!airi_files_open_count()); // 所有槽均可再次复用。
    airi_fd_init(&parent); // 继续用同一文件系统测试管道资源，不重置已有状态。
    unsigned ends[2] = {77, 88}; // 创建失败必须保留输出身份。
    assert(!airi_fd_pipe(&parent, ends, true) && ends[0] == 0 && ends[1] == 3); // 两个最低空FD。
    assert(airi_fd_ready(&parent, 0) == 0 && airi_fd_ready(&parent, 3) == AIRI_READY_WRITE); // 空管道的双端条件不同。
    assert(airi_fd_read(&parent, 0, data, 1, false, 0) == -11); // 有写者时空读不是EOF。
    char input[100], output[100]; // 模式覆盖整个环并越过容量。
    for (unsigned i = 0; i < sizeof(input); ++i) // 每个位置有明确值。
        input[i] = (char)i; // 可检查FIFO和回绕。
    assert(airi_fd_write(&parent, 3, input, 100, false, 0) == 64); // 容量边界产生短写。
    assert(airi_fd_write(&parent, 3, input, 1, false, 0) == -11 && airi_fd_ready(&parent, 3) == 0); // 满流形成背压。
    assert(airi_fd_read(&parent, 0, output, 10, false, 0) == 10 && !airi_memcmp(output, input, 10)); // 先释放一段空间。
    assert(airi_fd_write(&parent, 3, input, 10, false, 0) == 10); // 新数据跨环尾。
    assert(airi_fd_read(&parent, 0, output, 100, false, 0) == 64 && !airi_memcmp(output, input + 10, 54) && !airi_memcmp(output + 54, input, 10)); // 完整FIFO次序不被回绕改变。
    airi_fd_clone(&child, &parent); // fork后端点存在直到最后共享引用消失。
    assert(!airi_fd_close(&parent, 3) && airi_fd_read(&parent, 0, data, 1, false, 0) == -11); // 子仍持写端，不能提前EOF。
    assert(!airi_fd_close(&child, 3) && airi_fd_ready(&parent, 0) == (AIRI_READY_READ | AIRI_READY_HUP)); // 最后写引用消失才HUP。
    assert(airi_fd_read(&parent, 0, data, 1, false, 0) == 0); // 排空后真正EOF。
    airi_fd_close_all(&child, false); // 关闭子读端引用，不破坏父。
    airi_fd_close_all(&parent, false); // 最后读端结束，缓冲可复用。
    assert(!airi_files_open_count()); // 两端对象全部归还。
    airi_fd_init(&parent); // 新会话再建新管道。
    assert(!airi_fd_pipe(&parent, ends, true)); // 旧流字节必须清零丢弃。
    assert(!airi_fd_close(&parent, ends[0])); // 不再存在任何读者。
    assert(airi_fd_write(&parent, ends[1], input, 1, false, 0) == -32 && airi_fd_ready(&parent, ends[1]) == AIRI_READY_ERROR); // 写端收到EPIPE和错误就绪。
    assert(airi_fd_write(&parent, ends[1], NULL, 0, false, 0) == 0); // 零长度不访问buffer，也不等待对端。
    airi_fd_close_all(&parent, false); // 回收断流。
    airi_fd_init(&parent); // 四个流总共八个端点，小于本进程FD容量。
    for (unsigned i = 0; i < 4; ++i) // 填满独立管道池。
        assert(!airi_fd_pipe(&parent, ends, true)); // 每次两个端点完整成功。
    ends[0] = 77; ends[1] = 88; // 失败输出哨兵。
    assert(airi_fd_pipe(&parent, ends, true) == -28 && ends[0] == 77 && ends[1] == 88 && airi_files_open_count() == 8); // 第五条失败不泄漏半个FD或对象。
    airi_fd_close_all(&parent, false); // 扫描全部表项，端点池也全部回收。
    assert(!airi_files_open_count()); // 后续程序可继续创建流。
    puts("PASS RAM files/pipes: offsets, fork/exec references, sparse zero, short I/O, FIFO wrap, endpoint lifetime, exhaustion"); // 全部边界通过才发布。
    return 0; // 宿主测试成功。
}
```

**更新用户目标。** pipes复用自有入口和库，既有文件与日志主机测试继续运行。

包装依赖加入 pipes.elf。

<!-- replace: Makefile -->
<!-- before -->
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
<!-- after -->
```make
# 用户只链接自己的_start、main和libc子集，无宿主依赖。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_OBJECTS) -o $@
# 包装汇编明确依赖本配置用户ELF，文件变动就重新嵌入。
$(OUT)/labs/airios/images.o: labs/airios/images.S $(OUT)/user/hello.elf $(OUT)/user/processes.elf $(OUT)/user/worker.elf $(OUT)/user/files.elf $(OUT)/user/storage.elf $(OUT)/user/pipes.elf
# 建立包装对象目录。
	@mkdir -p $(dir $@)
# 单引号保护路径宏内部双引号，.incbin只读本配置生成文件。
	$(CC) $(CPPFLAGS) $(CFLAGS) -DAIRI_HELLO_ELF='"$(OUT)/user/hello.elf"' -DAIRI_PROCESS_ELF='"$(OUT)/user/processes.elf"' -DAIRI_WORKER_ELF='"$(OUT)/user/worker.elf"' -DAIRI_FILES_ELF='"$(OUT)/user/files.elf"' -DAIRI_STORAGE_ELF='"$(OUT)/user/storage.elf"' -DAIRI_PIPES_ELF='"$(OUT)/user/pipes.elf"' -c $< -o $@

# 第二个用户程序只复用用户入口与库，不把hello的main再次链接进去。
USER_COMMON := $(filter-out $(OUT)/user/hello.o,$(USER_OBJECTS))
```

在尾部加入 pipes 独立构建目标，已有程序保持。

<!-- replace: Makefile -->
<!-- before -->
```make
$(OUT)/user/storage.elf: $(USER_COMMON) $(OUT)/user/storage.o labs/airios/program.ld
# 用户库没有宿主libc依赖。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/storage.o -o $@
```
<!-- after -->
```make
$(OUT)/user/storage.elf: $(USER_COMMON) $(OUT)/user/storage.o labs/airios/program.ld
# 用户库没有宿主libc依赖。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/storage.o -o $@

# 非阻塞管道有独立用户main，保持前面程序各自可运行。
$(OUT)/user/pipes.o: labs/airios/pipes.c labs/airios/include/airi_libc.h
# 当前配置目录单独建立。
	@mkdir -p $(dir $@)
# 不链接内核头或宿主运行时。
	$(CC) $(USER_FLAGS) -Ilabs/airios/include -c $< -o $@
# 管道程序只复用自有用户入口与库。
$(OUT)/user/pipes.elf: $(USER_COMMON) $(OUT)/user/pipes.o labs/airios/program.ld
# 保持相同低地址ELF布局。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/pipes.o -o $@
```

<a id="p4-ready"></a>
## P4标记只在真实存储服务可用后发布

main在原P1/P2/P3标记之后，正式日志count可用时才发布AIRI_P4_READY；超时隔离设备只保留P3可运行标记，不宣称存储可用。只读设备可以读与恢复，writable=0准确说明写能力。正常自动验收、控制台输入和双启动恢复都等当前阶段标记；旧标记保留用于对照，不再提前判定正常存储启动成功。

P4内核已有FD、RAM文件、非阻塞管道、单队列VirtIO、有超时同步完成、校验追加日志和用户跨QEMU恢复。Linux L04代码已提供，此前记录 SSH255，Linux编译、strace与FS能力未验证，不能由guest READY代替。用户程序仍同步等磁盘，没有异步等待、完整持久文件系统或通用TTY。

**更新阶段标记。** 错误镜像不伪装完整磁盘阶段。

正式日志 count 可用才发布 P4；隔离盘只保留较早阶段的能力。

<!-- replace: kernel/main.c -->
<!-- before -->
```c
             (unsigned long long)usable);  // 完成记录附同一实际可用字节数。
    airi_log("[AIRI_P2_READY] exceptions=1 pages=1 own_cr3=1 heap=1"); // 所有内存演示和控制台初始化结束后才发布阶段完成。
    airi_log("[AIRI_P3_READY] user=ring3 elf=static fork=copy exec=transaction wait=blocked"); // 完整内核进程路径结束并恢复后才发布。
    airi_serial_write("airi> ");  // 输出首个串口提示符。
    for (;;) {  // 没有退出条件，内核持续轮询输入。
        int key = airi_input_poll();  // 从虚拟键盘或 UART 取得一个共享键值，0 表示暂无输入。
```
<!-- after -->
```c
             (unsigned long long)usable);  // 完成记录附同一实际可用字节数。
    airi_log("[AIRI_P2_READY] exceptions=1 pages=1 own_cr3=1 heap=1"); // 所有内存演示和控制台初始化结束后才发布阶段完成。
    airi_log("[AIRI_P3_READY] user=ring3 elf=static fork=copy exec=transaction wait=blocked"); // 完整内核进程路径结束并恢复后才发布。
    if (airi_storage_count() >= 0) // 超时隔离的故障镜像不宣称磁盘阶段可用。
        airi_log("[AIRI_P4_READY] files=ram pipes=nonblocking block=virtio journal=crc writable=%u", !airi_block_readonly()); // Linux实测状态另记，不由guest标记证明。
    airi_serial_write("airi> ");  // 输出首个串口提示符。
    for (;;) {  // 没有退出条件，内核持续轮询输入。
        int key = airi_input_poll();  // 从虚拟键盘或 UART 取得一个共享键值，0 表示暂无输入。
```

**更新正常启动预期。** 正常与只读等待P4，隔离盘只等P3，并继续核对具体故障。

正常及只读改等 P4，超时隔离仍等 P3。

<!-- replace: scripts/boot.py -->
<!-- before -->
```python
    (work/'command.json').write_text(json.dumps(cmd,indent=2))  # 保存实际参数列表供本轮复现。
    case = profile.removeprefix('desktop-')  # 图形前缀不改变故障注入含义。
    graphics = profile.startswith('desktop-') and case != 'no-framebuffer'  # 有绘图实现且设备存在才应生成画面。
    expected='[AIRI_PANIC] required Limine memory map is missing' if case=='missing-memory' else '[AIRI_P3_READY]'  # 正常配置等完整进程阶段，旧阶段标记只保留兼容。
    if case == 'block-missing': # 缺盘是明确设备初始化失败，不能继续宣称存储可用。
        expected = '[AIRI_PANIC] virtio block initialization failed' # 错误码另外核对。
    if case in ('trap-fatal', 'stack-guard'): # 未处理异常镜像必须进入具体停止路径。
```
<!-- after -->
```python
    (work/'command.json').write_text(json.dumps(cmd,indent=2))  # 保存实际参数列表供本轮复现。
    case = profile.removeprefix('desktop-')  # 图形前缀不改变故障注入含义。
    graphics = profile.startswith('desktop-') and case != 'no-framebuffer'  # 有绘图实现且设备存在才应生成画面。
    expected='[AIRI_PANIC] required Limine memory map is missing' if case=='missing-memory' else '[AIRI_P3_READY]' if case == 'block-timeout' else '[AIRI_P4_READY]' # 正常配置等完整存储阶段，隔离盘只保留进程可用标记。
    if case == 'block-missing': # 缺盘是明确设备初始化失败，不能继续宣称存储可用。
        expected = '[AIRI_PANIC] virtio block initialization failed' # 错误码另外核对。
    if case in ('trap-fatal', 'stack-guard'): # 未处理异常镜像必须进入具体停止路径。
```

正常完成路径还需用户管道标记，不用旧阶段代替。

<!-- replace: scripts/boot.py -->
<!-- before -->
```python
                        assert journal_marker in text # 正常配置必须真实恢复并替换坏尾。
                        storage_marker = '[STORAGE_SKIPPED]' if case == 'block-timeout' else '[STORAGE_RECOVERED]' # 只读仍可恢复，隔离盘不能提供空日志。
                        assert storage_marker in text # 正式区必须实际扫描或明确未提供。
                        assert '[FILES_USER_OK]' in text and '[FD_EXEC_OK]' in text # RAM文件和成功exec继承均完成。
                        assert '[PROCESS_FORK_WAIT_OK]' in text and '[PROCESS_DEMO_OK]' in text # 复制、真实阻塞、timer换身份和回收都须完成。
                        assert '[ELF_LOAD]' in text and '[ELF_USER_OK]' in text # 独立ELF段、BSS与用户libc必须真正运行。
```
<!-- after -->
```python
                        assert journal_marker in text # 正常配置必须真实恢复并替换坏尾。
                        storage_marker = '[STORAGE_SKIPPED]' if case == 'block-timeout' else '[STORAGE_RECOVERED]' # 只读仍可恢复，隔离盘不能提供空日志。
                        assert storage_marker in text # 正式区必须实际扫描或明确未提供。
                        assert '[PIPE_USER_OK]' in text # 有界流、背压、关闭与fork共享均已实际运行。
                        assert '[FILES_USER_OK]' in text and '[FD_EXEC_OK]' in text # RAM文件和成功exec继承均完成。
                        assert '[PROCESS_FORK_WAIT_OK]' in text and '[PROCESS_DEMO_OK]' in text # 复制、真实阻塞、timer换身份和回收都须完成。
                        assert '[ELF_LOAD]' in text and '[ELF_USER_OK]' in text # 独立ELF段、BSS与用户libc必须真正运行。
```

**更新输入等待。** 正常UART和PS/2先等完整P4，不改变原输入序列。

让 READY 匹配认识 P4 的字段边界。

<!-- replace: scripts/console_test.py -->
<!-- before -->
```python
                            continue  # 等后续读取取得完整换行再检查这条记录。
                        record=line.removeprefix('airi> ').removesuffix('\n').removesuffix('\r')  # 只移除已知提示符与CRLF，保留字段本身。
                        matches=record==marker  # CONSOLE_UPDATE要求完整等值，page=1不会匹配page=10。
                        if marker in ('[AIRI_P1_READY]', '[AIRI_P3_READY]'):  # 阶段READY后还有机制字段，按标记后的空格确定边界。
                            matches=record.startswith(marker+' ')  # 接受带字段的完整READY行，不接受相似标记名称。
                        if matches:  # 本次输出中已取得完整且匹配的状态记录。
                            return text[after:]  # 把本次动作的新结果交回调用者继续核对正文。
```
<!-- after -->
```python
                            continue  # 等后续读取取得完整换行再检查这条记录。
                        record=line.removeprefix('airi> ').removesuffix('\n').removesuffix('\r')  # 只移除已知提示符与CRLF，保留字段本身。
                        matches=record==marker  # CONSOLE_UPDATE要求完整等值，page=1不会匹配page=10。
                        if marker in ('[AIRI_P1_READY]', '[AIRI_P3_READY]', '[AIRI_P4_READY]'):  # 阶段READY后还有机制字段，按标记后的空格确定边界。
                            matches=record.startswith(marker+' ')  # 接受带字段的完整READY行，不接受相似标记名称。
                        if matches:  # 本次输出中已取得完整且匹配的状态记录。
                            return text[after:]  # 把本次动作的新结果交回调用者继续核对正文。
```

发送输入前等待 P4，原设备输入序列继续保持。

<!-- replace: scripts/console_test.py -->
<!-- before -->
```python
                        raise RuntimeError('Guest exited during console test')  # 报告真实guest生命周期失败。
                    time.sleep(.05)  # 暂停50毫秒给设备或日志处理时间，再重复本次检查。
                raise RuntimeError(f'Console timeout waiting for {marker}; see {serial}')  # 明确给出缺少的标记和已有日志路径。
            wait('[AIRI_P3_READY]',timeout=35)  # 输入前先等内核初始化与首帧结束，最多35秒。
            def key(*codes):  # 形参*把多个键名收集成元组，可同时发送ctrl或shift组合。
                qmp_execute(qmp,'send-key',{'keys':[{'type':'qcode','data':c} for c in codes],'hold-time':20})  # 每个键名转qcode字典，列表作为keys，按住时间单位毫秒。
                time.sleep(.12)  # 两次虚拟键事件间隔120毫秒，轮询内核有时间逐次消费。
```
<!-- after -->
```python
                        raise RuntimeError('Guest exited during console test')  # 报告真实guest生命周期失败。
                    time.sleep(.05)  # 暂停50毫秒给设备或日志处理时间，再重复本次检查。
                raise RuntimeError(f'Console timeout waiting for {marker}; see {serial}')  # 明确给出缺少的标记和已有日志路径。
            wait('[AIRI_P4_READY]',timeout=35)  # 输入前先等内核初始化与首帧结束，最多35秒。
            def key(*codes):  # 形参*把多个键名收集成元组，可同时发送ctrl或shift组合。
                qmp_execute(qmp,'send-key',{'keys':[{'type':'qcode','data':c} for c in codes],'hold-time':20})  # 每个键名转qcode字典，列表作为keys，按住时间单位毫秒。
                time.sleep(.12)  # 两次虚拟键事件间隔120毫秒，轮询内核有时间逐次消费。
```

**更新双启动等待。** 两个全新内核都先完成P4，才运行用户写入或恢复。

两个启动各自等 P4，再检查本次扫描和用户结果。

<!-- replace: scripts/storage_test.py -->
<!-- before -->
```python
                                raise RuntimeError(f'Guest exited before {marker}') # 保留目标身份。
                            time.sleep(.05) # 等设备与CPU推进。
                        raise RuntimeError(f'Restart test timed out: {marker}; see {serial}') # 明确失败文件。
                    text = wait('[AIRI_P3_READY] ') # 全部初始化和用户演示结束后才输入。
                    assert kernel_marker in text and '[AIRI_PANIC]' not in text # 当前新CPU必须扫描出指定前缀。
                    offset = len(text) # 用户命令只消费之后的新输出。
                    wire.sendall((request + '\n').encode()) # 通过实际UART提交控制台命令。
```
<!-- after -->
```python
                                raise RuntimeError(f'Guest exited before {marker}') # 保留目标身份。
                            time.sleep(.05) # 等设备与CPU推进。
                        raise RuntimeError(f'Restart test timed out: {marker}; see {serial}') # 明确失败文件。
                    text = wait('[AIRI_P4_READY] ') # 全部初始化和用户演示结束后才输入。
                    assert kernel_marker in text and '[AIRI_PANIC]' not in text # 当前新CPU必须扫描出指定前缀。
                    offset = len(text) # 用户命令只消费之后的新输出。
                    wire.sendall((request + '\n').encode()) # 通过实际UART提交控制台命令。
```


```sh
./airios test # 七组主机观察、九种启动、三条输入和双启动存储恢复。
./airios run # 输入run pipes，再输入run storage或run recover。
```

PIPE_USER_OK应记录short_write=64、backpressure=−11、wrap=fifo、eof=0、broken=−32、fork=shared，run pipes退出0后仍可继续命令。没有frontend、字体和Cocoa的默认目录完成同一路径。下一节把−11的可恢复条件登记为等待者，在状态变化后重查并唤醒，而不是把ready查询本身叫成事件循环。
