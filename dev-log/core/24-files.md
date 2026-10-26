# K24 · 一个整数怎样找到文件，偏移为什么会被孩子推进

从K23继续，图形不是前置。此前write把1和2直接解释成串口；现在用户程序先open一个名字，内核返回当前进程表里的编号，后续读写从这个编号找到文件。先把整个路径放在RAM中，避免第一次学习FD就同时处理磁盘协议。文件在本次启动内跨程序会话保留，QEMU重启后全部丢失。

本节沿一个实际操作展开：files打开/seed，孩子读ab，exec后的worker读cd，父亲随后从自己的dup编号读ef。它们都在消费同一个打开对象的偏移。另一个/notes用于实际短读写、显式定位、零洞和容量错误。两条路径均由真正的ring3程序发起int80，宿主测试只另外检查算法的边界。

<a id="fd-three-objects"></a>
## 名字、打开对象和表项各自持有什么

父子要按 ab→cd→ef 继续读取，就不能把偏移各放在自己的 FD 槽里；两次独立 open 又应能各从开头读。这需要区分名字、打开对象和进程表项。

/seed 是目录名字，对应六字节内容 abcdef。open 找到这个节点后，新建打开对象，保存节点、访问模式、偏移 0、引用数 1，再把对象指针放进当前进程的空 FD 槽；用户只拿到槽号。另一次 open 仍找同一内容节点，却建立另一对象，因此内容共享、偏移独立。dup 则只添表项并加引用，两编号继续指向同一 offset。

fork 把表项复制到孩子并逐项加引用，不复制打开对象。用户栈虽已各自复制，内核偏移仍只有一份。close 先清当前表项，再减引用，最后一个引用消失才释放打开槽；节点和名字保留，下一次 open 仍可读内容。

成功 exec 转移整份 FD 表，再按各项 cloexec 关闭指定编号。失败 exec 尚未触表，所以不能提前关闭。exit 则立即关闭全部 FD，僵尸保留等待结果和暂不能释放的栈等内存。这里结构转移与 fork 加引用是不同操作。

接口标志 READ=1、WRITE=2 等属于本项目，不与 Linux O_RDONLY 数值混用。当前最多八个平坦绝对名字，每个可写文件 1024 字节，全局 24 个打开对象，每进程 16 个 FD。/seed 只读，/notes 可创建与截断；终端是固定对象，不占 RAM 打开槽，尚无 stdin。目录遍历、unlink、权限用户组、符号链接、挂载和页缓存都未实现。

**新建 `include/airi/file.h`。** 内核表项保存内部指针，用户头只提供整数接口，不共享这个结构布局。

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
unsigned airi_files_open_count(void); // 不含固定终端对象，用于会话回收检查。
#endif // 结束保护。
```

<a id="file-transfer"></a>
## 创建与读写先决定边界，再修改内容

open(/notes, WRITE|TRUNCATE) 看起来是在取得编号，却还可能清空内容。如果当前 FD 表已满，这次请求应失败而不是先清空再告诉用户无可用编号。open 先验证名字与模式，再找节点、空 FD 和全局打开槽。只有这些资源都齐全才创建名字或truncate。否则一次失败的open可能已经清空旧文件，这会让资源错误产生意外数据破坏。全局槽满返回−23，当前进程表满返回−24，文件容量不足返回−28；返回值直接采用本项目的负错误形式，不写errno。

普通read/write从打开对象取偏移，完成后只加实际字节数。pread/pwrite使用显式偏移，完全不改那个共享字段。假设/notes有6字节，请求8字节读取只能返回6；再从偏移6读返回0表示EOF。写到1022时容量只剩2，请求4只能返回2；接下来写返回−28，而不是不断返回零让调用者误以为还可重试。

允许seek到文件末尾之外，首次在那里写入时把旧末尾到新位置之间清零。truncate只把长度改0，后续扩洞仍要清零，不能把隐藏的旧尾重新暴露。零长度请求不会扩大文件或移动偏移。这里所有传输同步复制，返回后文件层不保留buffer；未来异步I/O会另建明确所有权，不能直接复用这个借用约定。

**新建 `kernel/fs/file.c`。** 先完成有界对象模型，再接入进程和用户指针。

<!-- write: kernel/fs/file.c -->
```c
#include <airi/file.h> // 自有路径、打开对象与进程FD表。
#define NODE_COUNT 8u // 固定名字数量，不实现目录树和unlink。
#define OPEN_COUNT 24u // 全局打开对象容量与每进程FD容量不同。
#define FILE_BYTES 1024u // 每个可写文件的固定字节上限。
struct memory_file { char name[32]; uint8_t data[FILE_BYTES]; size_t size; bool readonly; }; // 名字和内容跨程序会话存活，重启丢失。
struct airi_open_file { struct memory_file *node; uint64_t offset; unsigned flags, references; }; // 多个表项可共享同一个偏移。
static struct memory_file nodes[NODE_COUNT]; // 初值零表示空目录槽。
static struct airi_open_file opened[OPEN_COUNT]; // 引用数零表示空打开槽。
static struct airi_open_file terminal; // 固定串口对象，不含RAM文件内容。
static struct airi_open_file *get(const struct airi_fd_table *table, unsigned fd) { // 用户整数必须先做范围检查。
    return fd < AIRI_FD_COUNT ? table->entries[fd].file : NULL; // 不把FD转成内核指针。
}
static int empty_fd(const struct airi_fd_table *table) { // 低编号优先，关闭后允许复用。
    for (unsigned i = 0; i < AIRI_FD_COUNT; ++i) // 有界扫描。
        if (!table->entries[i].file) // 无引用就是空表项。
            return (int)i; // 编号只在当前进程有意义。
    return -24; // -EMFILE，当前进程表满。
}
void airi_files_init(void) { // 启动一次，不能在仍有FD时重置。
    airi_memset(nodes, 0, sizeof(nodes)); // 清空名字、内容和长度。
    airi_memset(opened, 0, sizeof(opened)); // 清空全局打开对象。
    airi_memset(&terminal, 0, sizeof(terminal)); // 固定对象没有自有节点。
    terminal.flags = AIRI_OPEN_WRITE; // 当前只提供stdout/stderr。
    airi_memcpy(nodes[0].name, "/seed", 6); // 只读初始文件，字符串含终止零。
    airi_memcpy(nodes[0].data, "abcdef", 6); // 文件字节无需NUL。
    nodes[0].size = 6; // 读到末尾返回0。
    nodes[0].readonly = true; // 模式请求不能让只读节点可写。
}
```

名字与打开对象的存储已经固定。接着实现进程表的初始化、复制、关闭和 dup：表项决定持有引用，只有最后一个引用消失才归还打开槽。此处尚未解析文件名。

<!-- append: kernel/fs/file.c -->
```c
void airi_fd_init(struct airi_fd_table *table) { // 新会话的独立表，不复制旧进程编号。
    airi_memset(table, 0, sizeof(*table)); // stdin暂未接入，槽0可用于普通文件。
    table->entries[1].file = &terminal; // stdout引用固定设备对象。
    table->entries[2].file = &terminal; // stderr是另一个表项。
    terminal.references += 2; // 两个表项各持有一次引用。
}
void airi_fd_clone(struct airi_fd_table *destination, const struct airi_fd_table *source) { // 内核不可抢占，复制和加引用作为同一次操作。
    *destination = *source; // 复制表项，不复制打开对象和偏移。
    for (unsigned i = 0; i < AIRI_FD_COUNT; ++i) // 每个表项代表独立引用。
        if (destination->entries[i].file) // 空项无资源。
            ++destination->entries[i].file->references; // 父关闭不会提前销毁孩子仍在用的对象。
}
long airi_fd_close(struct airi_fd_table *table, unsigned fd) { // 关闭表项，不删除文件名字。
    struct airi_open_file *file = get(table, fd); // 安全查找。
    if (!file) // 无效或已关闭。
        return -9; // -EBADF。
    table->entries[fd] = (struct airi_fd_entry){0}; // 先撤掉进程引用。
    if (--file->references == 0 && file != &terminal) // 最后引用消失才归还全局槽。
        airi_memset(file, 0, sizeof(*file)); // 下一次open取得独立偏移。
    return 0; // 节点内容保留到重启或truncate。
}
void airi_fd_close_all(struct airi_fd_table *table, bool exec_only) { // 多次清理空表安全。
    for (unsigned i = 0; i < AIRI_FD_COUNT; ++i) // 不依赖用户提供清单。
        if (table->entries[i].file && (!exec_only || table->entries[i].cloexec)) // exec保留未标记项。
            airi_fd_close(table, i); // 复用统一引用释放路径。
}
```

表项的引用规则齐全后，再实现 open 和 seek。open 同时检查名字、进程表与全局槽，资源齐全才创建或截断；seek 改的是打开对象的 offset，因而会被共享该对象的表项看到。

<!-- append: kernel/fs/file.c -->
```c
long airi_fd_open(struct airi_fd_table *table, const char *path, unsigned flags) { // 路径已由陷入层复制到内核。
    size_t length = airi_strlen(path); // 调用者保证有界NUL。
    if (length < 2 || length >= sizeof(nodes[0].name) || path[0] != '/' || (flags & ~31u) || !(flags & 3u) || ((flags & AIRI_OPEN_TRUNCATE) && !(flags & AIRI_OPEN_WRITE))) // 名字和标志全部先校验。
        return -22; // -EINVAL，不修改名字或长度。
    for (size_t i = 1; i < length; ++i) // 首版只允许平坦绝对名字。
        if (path[i] == '/' || path[i] == '.') // 不解释目录、相对路径和点段。
            return -22; // 范围限制明确拒绝。
    struct memory_file *node = NULL, *free_node = NULL; // 命中节点与潜在空槽分开。
    for (unsigned i = 0; i < NODE_COUNT; ++i) { // 同一节点的不同open应共享内容。
        if (!nodes[i].name[0]) // 空槽可用于创建。
            free_node = &nodes[i]; // 尚未发布名字。
        else if (!airi_memcmp(path, nodes[i].name, length + 1)) // 长度包含NUL，排除前缀匹配。
            node = &nodes[i]; // 目录持有真实内容。
    }
    if (!node && !(flags & AIRI_OPEN_CREATE)) // 缺名字且不允许创建。
        return -2; // -ENOENT。
    if (!node && !free_node) // 节点容量耗尽。
        return -28; // -ENOSPC。
    if (node && node->readonly && (flags & AIRI_OPEN_WRITE)) // 文件自身只读。
        return -13; // -EACCES。
    int fd = empty_fd(table); // 名字尚未创建或截断，先保证表项。
    if (fd < 0) // 进程表满。
        return fd; // 原文件不变。
    struct airi_open_file *file = NULL; // 再找系统级打开对象。
    for (unsigned i = 0; i < OPEN_COUNT; ++i) // 引用零代表未使用。
        if (!opened[i].references) { // 选择第一个空槽。
            file = &opened[i]; // 尚未发布。
            break; // 不重复分配。
        }
    if (!file) // 全局对象不足不同于进程表满。
        return -23; // -ENFILE，文件仍不变。
    if (!node) { // 所有资源就绪后才创建名字。
        node = free_node; // 目标是确定的空目录槽。
        airi_memset(node, 0, sizeof(*node)); // 不保留旧字节。
        airi_memcpy(node->name, path, length + 1); // 名字复制归文件系统所有。
    }
    if (flags & AIRI_OPEN_TRUNCATE) // 有写权限且不会因表满再失败。
        node->size = 0; // 后续扩洞时再显式清零，旧尾不对读者可见。
    *file = (struct airi_open_file){node, 0, flags & 3u, 1}; // 独立open得到新偏移0。
    table->entries[fd] = (struct airi_fd_entry){file, (flags & AIRI_OPEN_CLOEXEC) != 0}; // 标志按表项保存。
    return fd; // 用户只拿整数。
}
long airi_fd_dup(struct airi_fd_table *table, unsigned fd, bool cloexec) { // 不创建新的打开对象。
    struct airi_open_file *file = get(table, fd); // 验证源FD。
    if (!file) // 源已关闭。
        return -9; // -EBADF。
    int next = empty_fd(table); // 新表项仍可能不足。
    if (next < 0) // 不改变源引用。
        return next; // -EMFILE。
    ++file->references; // 新表项正式取得共享对象。
    table->entries[next] = (struct airi_fd_entry){file, cloexec}; // 偏移不复制，标志独立。
    return next; // 新整数与旧整数指向同一对象。
}
bool airi_fd_console(const struct airi_fd_table *table, unsigned fd) { // 终端特殊后端由陷入层发送。
    return get(table, fd) == &terminal; // 无效FD得到NULL，不会命中。
}
long airi_fd_seek(struct airi_fd_table *table, unsigned fd, uint64_t offset) { // 本项目只接受SEEK_SET语义。
    struct airi_open_file *file = get(table, fd); // 先验证表项。
    if (!file) // 无效句柄。
        return -9; // -EBADF。
    if (!file->node) // 固定终端不是可定位文件。
        return -29; // -ESPIPE。
    if (offset > FILE_BYTES) // 限定可表达的文件范围。
        return -22; // -EINVAL。
    file->offset = offset; // 所有dup/fork引用一起观察新偏移。
    return (long)offset; // 返回定位后的偏移。
}
```

现在一个 FD 已经能找到打开对象。继续追加 transfer 与包装：普通读写以该对象的 offset 为起点并按实际字节数推进，显式定位则只使用本次参数；同一内容因此支持两种不互相混淆的位置来源。

<!-- append: kernel/fs/file.c -->
```c
static long transfer(struct airi_fd_table *table, unsigned fd, void *buffer, size_t bytes, bool write, bool positioned, uint64_t offset) { // 这里只操作内核缓冲，不验证用户指针。
    struct airi_open_file *file = get(table, fd); // 当前表里的整数转内部对象。
    if (!file || !(file->flags & (write ? AIRI_OPEN_WRITE : AIRI_OPEN_READ))) // 错误访问模式也返回EBADF。
        return -9; // 无副作用。
    struct memory_file *node = file->node; // 终端不在此处直接访问端口。
    if (!node) // 不支持的设备操作。
        return -29; // -ESPIPE。
    if (!positioned) // 普通read/write消费共享偏移。
        offset = file->offset; // pread/pwrite只消费显式偏移。
    if (offset > FILE_BYTES) // 在加法之前限制右端。
        return -22; // -EINVAL。
    if (!bytes) // 零长度不扩文件，不改变偏移。
        return 0; // buffer可为空，因为不会解引用。
    size_t limit = write ? FILE_BYTES : node->size; // 读受EOF限制，写受容量限制。
    if (offset >= limit) // 到末尾读返回EOF，满容量写返回错误。
        return write ? -28 : 0; // 区分无进展写与正常EOF。
    if (bytes > limit - offset) // 单次可能短于请求，调用者必须消费返回值。
        bytes = limit - offset; // 不读写数组之外。
    if (write) { // 所有模式检查已经完成。
        if (offset > node->size) // 定位到末尾之外形成的洞必须读为零。
            airi_memset(node->data + node->size, 0, (size_t)offset - node->size); // 不泄漏truncate前的尾字节。
        airi_memcpy(node->data + offset, buffer, bytes); // 同步复制，返回后不再借buffer。
        if (offset + bytes > node->size) // 覆盖中间不缩短文件。
            node->size = offset + bytes; // 发布实际长度。
    } else // 普通读和pread复制相同文件内容。
        airi_memcpy(buffer, node->data + offset, bytes); // 请求已缩到EOF之内。
    if (!positioned) // 显式偏移操作不能移动共享位置。
        file->offset = offset + bytes; // 只推进实际完成数。
    return (long)bytes; // 可小于请求，零只代表读EOF或零长度。
}
long airi_fd_read(struct airi_fd_table *table, unsigned fd, void *buffer, size_t bytes, bool positioned, uint64_t offset) { // 所有读取共用一个偏移实现。
    return transfer(table, fd, buffer, bytes, false, positioned, offset); // 内核缓冲由调用者持有。
}
long airi_fd_write(struct airi_fd_table *table, unsigned fd, const void *buffer, size_t bytes, bool positioned, uint64_t offset) { // 同步写不修改输入。
    return transfer(table, fd, (void *)buffer, bytes, true, positioned, offset); // 内部仅在read分支写buffer，当前固定为write。
}
unsigned airi_files_open_count(void) { // 验证退出与exec没有遗留打开对象。
    unsigned count = 0; // 不统计引用为零的槽。
    for (unsigned i = 0; i < OPEN_COUNT; ++i) // 固定全局容量。
        count += opened[i].references != 0; // 返回对象数量，不是表项数量。
    return count; // 终端固定对象单独管理，不占此表。
}
```

<a id="file-user-boundary"></a>
## 先检查用户输出，才允许消费偏移

read 不只产生字节，还会推进共享 offset。若文件层先消费，再发现用户输出只读，调用者得到 −14，文件位置却已前移。因此陷入层先验证整个请求范围有 USER 和 WRITE，再读进 256 字节内核副本，最后只复制实际完成前缀。write 的方向相反：先验证并复制用户输入，再修改文件或发送串口。

这一检查与消费顺序依赖单核不可抢占内核：同一次调用里，没有同进程另一线程拆掉映射。它不是可以直接搬到多线程内核的锁方案，返回以后文件层也不保留用户 buffer。

FD 寄存器有 64 位，必须先确认完整值小于 16，再转 unsigned，否则 0x100000001 可能被截成 stdout。路径同样先在边界取副本：open 逐字节复制最多 32 字节，见 NUL 才交给文件层，不对原用户指针 strlen。过长或不可读路径统一 −14，尚无 Linux 完整路径错误分类。

pread/pwrite 多一个 offset，放在 R10。用户 register 变量用 __asm__("r10") 固定寄存器，再经扩展汇编输入约束交给 int80；已有完整 CPU 现场会保存 R10，不另加栈参数协议。这仍是本项目 ABI。

资源交接也进入进程控制块的 files。fork 在页复制全部成功后加 FD 引用；exec 将 old.files 转给 next，再清 old.files，防止 cleanup 旧空间时重复关闭。当前内核栈仍按 K23 转移。C 结构赋值本身不会自动加引用，动作必须由这些顺序明确完成。

**沿 K23 的 `kernel/process/user.c` 更新。** 先接表与映像，再补清理、fork/exec 交接和系统调用，最后接演示与资源核对。下面每组都给出唯一旧窗口，其余函数保留；这些编辑完成前，文件调用还未全部接通。

先在 include 区接入文件接口。这里只取得声明，还没有给任何进程分配 FD。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
#include <airi/console.h> // 内核命令消费服务结果，不依赖主题和绘图。
#include <airi/elf.h> // 独立用户ELF检查器。
#include <airi/user.h> // 完整陷入与安全用户返回。
```
<!-- after -->
```c
#include <airi/file.h> // 每进程FD表共享打开文件对象。
#include <airi/console.h> // 内核命令消费服务结果，不依赖主题和绘图。
#include <airi/elf.h> // 独立用户ELF检查器。
#include <airi/user.h> // 完整陷入与安全用户返回。
```

在 user_process 的开头加入 files。它是本进程的表项容器，表内指针仍可与别的进程指向同一打开对象。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
enum user_state { USER_UNUSED, USER_RUNNABLE, USER_RUNNING, USER_WAITING, USER_ZOMBIE, USER_REAP_PENDING }; // 退出与回收是两个不同状态。
struct user_page { uint64_t address, physical; unsigned flags; }; // fork需要把同一VA映射到另一个物理页。
struct user_process { // 所有权和调度状态由内核持有，用户只能看到PID。
    struct airi_address_space space; // 根和低半区表自有，高半区借用。
    struct airi_kernel_region stack; // 不能在仍使用它的C调用中释放。
    struct user_page owned[64]; // 所有用户数据页，含代码、静态数据、堆页和栈。
```
<!-- after -->
```c
enum user_state { USER_UNUSED, USER_RUNNABLE, USER_RUNNING, USER_WAITING, USER_ZOMBIE, USER_REAP_PENDING }; // 退出与回收是两个不同状态。
struct user_page { uint64_t address, physical; unsigned flags; }; // fork需要把同一VA映射到另一个物理页。
struct user_process { // 所有权和调度状态由内核持有，用户只能看到PID。
    struct airi_fd_table files; // exec转移表项，fork增加同一打开对象的引用。
    struct airi_address_space space; // 根和低半区表自有，高半区借用。
    struct airi_kernel_region stack; // 不能在仍使用它的C调用中释放。
    struct user_page owned[64]; // 所有用户数据页，含代码、静态数据、堆页和栈。
```

在映像声明和目录中登记 files；装载过程仍走既有 ELF 路径。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
extern const uint8_t airi_user_rsp_bad_start[], airi_user_rsp_bad_end[]; // 不规范用户返回栈。
extern const uint8_t airi_hello_elf_start[], airi_hello_elf_end[]; // 既有独立用户C程序。
extern const uint8_t airi_process_elf_start[], airi_process_elf_end[]; // 新的fork/wait和抢占工作负载。
extern const uint8_t airi_worker_elf_start[], airi_worker_elf_end[]; // exec目标是另一个独立静态ELF。
extern const uint8_t airi_broken_elf_start[], airi_broken_elf_end[]; // 受控损坏输入，验证旧进程保持。
struct executable { const char *name; const uint8_t *start, *end; }; // 初始映像目录尚不是通用文件系统。
static const struct executable executables[] = { // 每份映像字节由内核只读段长期持有。
    {"hello", airi_hello_elf_start, airi_hello_elf_end}, // 独立C运行库与BSS观察。
    {"processes", airi_process_elf_start, airi_process_elf_end}, // fork/exec/wait实际工作负载。
    {"worker", airi_worker_elf_start, airi_worker_elf_end}, // 替换成功后从自己的_start开始。
    {"broken", airi_broken_elf_start, airi_broken_elf_end}, // 用于完整的坏ELF失败演示。
};
static const struct executable *find_program(const char *name) { // 输入是内核已复制的短NUL字符串。
```
<!-- after -->
```c
extern const uint8_t airi_user_rsp_bad_start[], airi_user_rsp_bad_end[]; // 不规范用户返回栈。
extern const uint8_t airi_hello_elf_start[], airi_hello_elf_end[]; // 既有独立用户C程序。
extern const uint8_t airi_process_elf_start[], airi_process_elf_end[]; // 新的fork/wait和抢占工作负载。
extern const uint8_t airi_files_elf_start[], airi_files_elf_end[]; // 内存文件用户工作负载。
extern const uint8_t airi_worker_elf_start[], airi_worker_elf_end[]; // exec目标是另一个独立静态ELF。
extern const uint8_t airi_broken_elf_start[], airi_broken_elf_end[]; // 受控损坏输入，验证旧进程保持。
struct executable { const char *name; const uint8_t *start, *end; }; // 初始映像目录尚不是通用文件系统。
static const struct executable executables[] = { // 每份映像字节由内核只读段长期持有。
    {"hello", airi_hello_elf_start, airi_hello_elf_end}, // 独立C运行库与BSS观察。
    {"processes", airi_process_elf_start, airi_process_elf_end}, // fork/exec/wait实际工作负载。
    {"worker", airi_worker_elf_start, airi_worker_elf_end},
    {"files", airi_files_elf_start, airi_files_elf_end}, // 真实用户FD演示。
    {"broken", airi_broken_elf_start, airi_broken_elf_end}, // 用于完整的坏ELF失败演示。
};
static const struct executable *find_program(const char *name) { // 输入是内核已复制的短NUL字符串。
```

在 cleanup 入口关闭仍由目标持有的表项。失败构造与已经清空的表都可走这里，避免页资源回滚以后遗留全局打开槽。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    return true; // 同步结束，不借出buffer指针。
}
static void cleanup(struct user_process *p) { // 目标根必须不活动，而且当前不能使用目标内核栈。
    if (p->space.root && !airi_vm_destroy(&p->space)) // 先解除并归还自有页表，不动借用的内核表。
        airi_panic("cannot destroy inactive user tables"); // 不能归还仍被活动根引用的数据页。
    while (p->count) // 用户代码与用户栈数据页分别归还。
```
<!-- after -->
```c
    return true; // 同步结束，不借出buffer指针。
}
static void cleanup(struct user_process *p) { // 目标根必须不活动，而且当前不能使用目标内核栈。
    airi_fd_close_all(&p->files, false); // 回滚或已退出空表均安全，不能遗留全局引用。
    if (p->space.root && !airi_vm_destroy(&p->space)) // 先解除并归还自有页表，不动借用的内核表。
        airi_panic("cannot destroy inactive user tables"); // 不能归还仍被活动根引用的数据页。
    while (p->count) // 用户代码与用户栈数据页分别归还。
```

在 exit_current 中先关闭退出者的 FD。后面的僵尸仍有返回状态和待回收栈，但已经不再持有这些文件引用。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
}
static struct airi_trap_frame *exit_current(int code) { // CPU还在当前进程内核栈上，先结束身份而不释放栈。
    struct user_process *p = current; // 当前退出者的稳定控制块。
    p->exit_code = code; // 保存给父进程wait读取。
    p->state = USER_ZOMBIE; // 不再运行，但资源和状态尚存在。
    if (p->pid == root_pid) // 整个会话的结果以最初根用户为准。
```
<!-- after -->
```c
}
static struct airi_trap_frame *exit_current(int code) { // CPU还在当前进程内核栈上，先结束身份而不释放栈。
    struct user_process *p = current; // 当前退出者的稳定控制块。
    airi_fd_close_all(&p->files, false); // exit立即释放文件，僵尸只保留等待状态与待回收内存。
    p->exit_code = code; // 保存给父进程wait读取。
    p->state = USER_ZOMBIE; // 不再运行，但资源和状态尚存在。
    if (p->pid == root_pid) // 整个会话的结果以最初根用户为准。
```

在 fork 的页复制循环之后取得孩子的 FD 引用。到这里内存副本已经齐全，孩子与父亲才开始共享打开对象。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
            goto failed; // 回滚全部子资源，不改变父页面。
        airi_memcpy(airi_memory_direct(child->owned[child->count - 1].physical), airi_memory_direct(page->physical), 4096); // 复制完整页内容，父子以后写入互不影响。
    }
    child->initial = *parent_frame; // 用户RIP/RSP和寄存器从同一次fork返回点继续。
    child->initial.rax = 0; // 子分支看到fork返回0。
    child->pid = next_pid++; // 全部内存成功后才分配PID。
```
<!-- after -->
```c
            goto failed; // 回滚全部子资源，不改变父页面。
        airi_memcpy(airi_memory_direct(child->owned[child->count - 1].physical), airi_memory_direct(page->physical), 4096); // 复制完整页内容，父子以后写入互不影响。
    }
    airi_fd_clone(&child->files, &parent->files); // 页复制全部成功后再取得FD引用，失败cleanup仍能撤销。
    child->initial = *parent_frame; // 用户RIP/RSP和寄存器从同一次fork返回点继续。
    child->initial.rax = 0; // 子分支看到fork返回0。
    child->pid = next_pid++; // 全部内存成功后才分配PID。
```

在 exec 提交处把 old.files 转给 next，再清空旧持有位置并按 cloexec 关闭新表指定项。旧空间随后 cleanup 时不会再次关闭已转移的引用。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    airi_memset(&old.stack, 0, sizeof(old.stack)); // old清理者不再拥有这组栈，不能重复释放。
    next.frame = f; // 真实返回现场仍在同一内核栈上，不能留下指向next.initial局部对象的指针。
    *f = next.initial; // 事务提交时才覆盖旧用户RIP/RSP和寄存器。
    next.state = USER_RUNNING; // 此身份继续运行，但从新_start开始。
    *current = next; // 把新所有权发布到原进程槽，PID不变。
    if (!airi_vm_activate(&current->space)) // 新根已借到当前内核栈及内核映射。
```
<!-- after -->
```c
    airi_memset(&old.stack, 0, sizeof(old.stack)); // old清理者不再拥有这组栈，不能重复释放。
    next.frame = f; // 真实返回现场仍在同一内核栈上，不能留下指向next.initial局部对象的指针。
    *f = next.initial; // 事务提交时才覆盖旧用户RIP/RSP和寄存器。
    next.files = old.files; // 文件表所有权从旧映像转到新映像，偏移和普通FD继续存在。
    airi_memset(&old.files, 0, sizeof(old.files)); // cleanup旧映像不能重复关闭已转移引用。
    airi_fd_close_all(&next.files, true); // 只有成功提交才关闭CLOEXEC，失败exec不影响原表。
    next.state = USER_RUNNING; // 此身份继续运行，但从新_start开始。
    *current = next; // 把新所有权发布到原进程槽，PID不变。
    if (!airi_vm_activate(&current->space)) // 新根已借到当前内核栈及内核映射。
```

将原来的串口 write 分支扩展成文件传输分支。先检查完整 FD 和用户范围，再用内核副本完成实际前缀，最后写回返回值；读输出失败不会消费 offset。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
        airi_log("[USER_FAULT] pid=%u vector=%llu error=0x%llx rip=0x%llx cr2=0x%llx", current->pid, (unsigned long long)f->vector, (unsigned long long)f->error, (unsigned long long)f->rip, (unsigned long long)(f->vector == 14 ? airi_trap_fault_address() : 0)); // PF才读取本次CR2。
        return exit_current(-(128 + (int)f->vector)); // 自有故障编码，不宣称Linux信号ABI。
    }
    if (f->rax == 1) { // 有界同步write，仍只支持stdout/stderr。
        char buffer[256]; // 同步内核副本不保存用户借用。
        if (f->rdi != 1 && f->rdi != 2) // 通用FD表在文件阶段接入。
            f->rax = (uint64_t)-9ll; // -EBADF。
        else if (f->rdx > sizeof(buffer)) // 拒绝覆盖内核栈的巨大长度。
            f->rax = (uint64_t)-22ll; // -EINVAL。
        else if (!copy_user(current, buffer, f->rsi, (size_t)f->rdx, false)) // 完整检查所有用户输入页。
            f->rax = (uint64_t)-14ll; // -EFAULT。
        else { // 用户内容已复制进本内核调用。
            airi_serial_write_bytes(buffer, (size_t)f->rdx); // 按逻辑长度发送。
            f->rax = f->rdx; // 返回已完成长度。
        }
    } else if (f->rax == 2) // 退出后不再恢复此用户指令。
        return exit_current((int)(f->rdi & 255)); // 普通退出码0–255。
    else if (f->rax == 3) // getpid查询当前身份。
```
<!-- after -->
```c
        airi_log("[USER_FAULT] pid=%u vector=%llu error=0x%llx rip=0x%llx cr2=0x%llx", current->pid, (unsigned long long)f->vector, (unsigned long long)f->error, (unsigned long long)f->rip, (unsigned long long)(f->vector == 14 ? airi_trap_fault_address() : 0)); // PF才读取本次CR2。
        return exit_current(-(128 + (int)f->vector)); // 自有故障编码，不宣称Linux信号ABI。
    }
    if (f->rax == 1 || f->rax == 10 || f->rax == 15 || f->rax == 16) { // write/read/pread/pwrite共享检查与副本边界。
        char buffer[256]; // 内核只在本次同步调用持有副本。
        bool write = f->rax == 1 || f->rax == 16; // 用户到文件为写，文件到用户为读。
        bool positioned = f->rax == 15 || f->rax == 16; // 显式偏移来自第四参数R10。
        size_t bytes = (size_t)f->rdx; // 请求长度先有界再校验跨度。
        long result; // 自有ABI直接返回负错误或实际字节数。
        if (f->rdi >= AIRI_FD_COUNT) // 全64位FD必须先验证，不能截断高位后访问另一个编号。
            result = -9; // -EBADF。
        else if (bytes > sizeof(buffer)) // 不允许覆盖内核栈。
            result = -22; // -EINVAL。
        else if (!user_access(current, f->rsi, bytes, !write)) // read必须先验证整个输出区，失败不能消费偏移。
            result = -14; // -EFAULT。
        else if (write) { // 输入完整有效后才能发生文件或串口副作用。
            copy_user(current, buffer, f->rsi, bytes, false); // 复制当前进程的输入字节。
            if (!positioned && airi_fd_console(&current->files, (unsigned)f->rdi)) { // 被dup的终端FD也可发送。
                airi_serial_write_bytes(buffer, bytes); // 同步设备适配，不借出用户指针。
                result = (long)bytes; // 返回逻辑字节数。
            } else // RAM文件或不支持的定位设备操作。
                result = airi_fd_write(&current->files, (unsigned)f->rdi, buffer, bytes, positioned, f->r10); // 文件层维护共享偏移。
        } else { // 内核副本先承接实际读取结果。
            result = airi_fd_read(&current->files, (unsigned)f->rdi, buffer, bytes, positioned, f->r10); // EOF为0，错误为负。
            if (result > 0 && !copy_user(current, buffer, f->rsi, (size_t)result, true)) // 前置完整检查后映射不能变化。
                airi_panic("validated file output mapping changed"); // 不吞内部错误。
        }
        f->rax = (uint64_t)result; // 调用者必须按实际完成数推进。
    } else if (f->rax == 2) // 退出后不再恢复此用户指令。
        return exit_current((int)(f->rdi & 255)); // 普通退出码0–255。
    else if (f->rax == 3) // getpid查询当前身份。
```

在分配用户页的分支后接上 open、close、dup 和 seek。路径复制与完整整数检查均留在陷入边界，文件层只消费已经验证的参数。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
        return wait_current(f); // 结果恢复到父同一次调用之后。
    else if (f->rax == 9) // 申请零填充用户页。
        f->rax = (uint64_t)allocate_user((size_t)f->rdi); // 不成功不前移堆游标。
    else // 尚未提供的接口都明确拒绝。
        f->rax = (uint64_t)-38ll; // -ENOSYS。
    return airi_user_resume(f); // 普通调用继续同一用户，仍检查RSP与标志。
```
<!-- after -->
```c
        return wait_current(f); // 结果恢复到父同一次调用之后。
    else if (f->rax == 9) // 申请零填充用户页。
        f->rax = (uint64_t)allocate_user((size_t)f->rdi); // 不成功不前移堆游标。
    else if (f->rax == 11) { // open只消费内核持有的短路径副本。
        char path[32]; // 与平坦目录名容量一致。
        bool copied = false; // 必须看到终止零才可交给文件层。
        for (size_t i = 0; i < sizeof(path); ++i) { // 逐字节检查，不能strlen用户指针。
            if (!copy_user(current, &path[i], f->rdi + i, 1, false)) // 未映射或内核地址拒绝。
                break; // 不发生创建或截断。
            if (!path[i]) { // 有界NUL字符串完成。
                copied = true; // 副本可在当前内核调用中使用。
                break; // 不额外读取终止零后面的用户页。
            }
        }
        f->rax = copied && f->rsi <= 31 ? (uint64_t)airi_fd_open(&current->files, path, (unsigned)f->rsi) : (uint64_t)-14ll; // 错误路径不变更文件。
    } else if (f->rax == 12) // close撤销当前表项引用。
        f->rax = (f->rdi < AIRI_FD_COUNT ? (uint64_t)airi_fd_close(&current->files, (unsigned)f->rdi) : (uint64_t)-9ll); // 不删除文件内容。
    else if (f->rax == 13) // dup共享打开对象，标志放在新表项。
        f->rax = (f->rdi < AIRI_FD_COUNT ? (uint64_t)airi_fd_dup(&current->files, (unsigned)f->rdi, f->rsi != 0) : (uint64_t)-9ll); // 单CPU内核中同步加引用。
    else if (f->rax == 14) // 仅绝对定位，不实现Linux全部whence。
        f->rax = (f->rdi < AIRI_FD_COUNT ? (uint64_t)airi_fd_seek(&current->files, (unsigned)f->rdi, f->rsi) : (uint64_t)-9ll); // 共享打开对象的偏移随之变化。
    else // 尚未提供的接口都明确拒绝。
        f->rax = (uint64_t)-38ll; // -ENOSYS。
    return airi_user_resume(f); // 普通调用继续同一用户，仍检查RSP与标志。
```

失败启动的资源核对增加打开对象数量。坏 ELF 的回滚除了归还页面，也必须撤回新取得的文件引用。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    int error = elf ? prepare_elf(root, image, bytes, name, true) : (prepare_flat(root, image, bytes) ? 0 : -12); // ELF保留格式与资源错误之别。
    if (error) { // 失败构造也必须完整恢复资源。
        airi_memory_snapshot(&after); // 不能只看错误码忽略泄漏。
        if (before.pages_free != after.pages_free || before.heap_free != after.heap_free) // 半份新映像不应改变空闲数量。
            airi_panic("failed user launch leaked resources"); // 内部错误与普通坏文件分开。
        return error; // 调用者可以继续命令，不停机。
    }
    root->pid = next_pid++; // 完整准备后才取得运行身份。
    root->heap_next = USER_HEAP; // 动态用户页初始为空。
    place_frame(root); // 寄存器值进入本进程真实内核栈。
```
<!-- after -->
```c
    int error = elf ? prepare_elf(root, image, bytes, name, true) : (prepare_flat(root, image, bytes) ? 0 : -12); // ELF保留格式与资源错误之别。
    if (error) { // 失败构造也必须完整恢复资源。
        airi_memory_snapshot(&after); // 不能只看错误码忽略泄漏。
        if (before.pages_free != after.pages_free || before.heap_free != after.heap_free || airi_files_open_count()) // 半份新映像不应改变空闲数量。
            airi_panic("failed user launch leaked resources"); // 内部错误与普通坏文件分开。
        return error; // 调用者可以继续命令，不停机。
    }
    airi_fd_init(&root->files); // 初始stdout/stderr归本次根进程持有。
    root->pid = next_pid++; // 完整准备后才取得运行身份。
    root->heap_next = USER_HEAP; // 动态用户页初始为空。
    place_frame(root); // 寄存器值进入本进程真实内核栈。
```

成功会话结束时做同样核对。用户返回码可以不同，退出后的内核资源仍应恢复。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    current = NULL; // 不保留已清掉的身份指针。
    kernel_return = NULL; // 下次会话重新登记。
    airi_memory_snapshot(&after); // 全部页和栈回收后验证。
    if (before.pages_free != after.pages_free || before.heap_free != after.heap_free) // 用户程序退出码可以任意，但内核资源必须恢复。
        airi_panic("user process session leaked memory"); // 不把泄漏当成应用错误跳过。
    return 0; // 表示已完整启动与收拢，程序结果另在result.code。
}
```
<!-- after -->
```c
    current = NULL; // 不保留已清掉的身份指针。
    kernel_return = NULL; // 下次会话重新登记。
    airi_memory_snapshot(&after); // 全部页和栈回收后验证。
    if (before.pages_free != after.pages_free || before.heap_free != after.heap_free || airi_files_open_count()) // 用户程序退出码可以任意，但内核资源必须恢复。
        airi_panic("user process session leaked memory"); // 不把泄漏当成应用错误跳过。
    return 0; // 表示已完整启动与收拢，程序结果另在result.code。
}
```

最后在启动演示中初始化一次 RAM 目录并运行 files；programs 列表也接入同一映像名字。不要在每次用户会话重建 /seed 或 /notes。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
        airi_log("[PROCESS_DEMO_OK] fork=copy exec=transaction wait=blocked timer_switches=%u wait_blocks=%u reclaimed=1", result.switches, result.blocks); // 计数来自真实IRQ与状态转换。
}
void airi_user_demo(void) { // 串口主线的真实用户边界与多进程回归。
    run_one(airi_user_probe_start, (size_t)((uintptr_t)airi_user_probe_end - (uintptr_t)airi_user_probe_start), NULL, 0, false, true, false); // 原有非法指针与用户IRQ验证。
    run_one(airi_user_bad_start, (size_t)((uintptr_t)airi_user_bad_end - (uintptr_t)airi_user_bad_start), NULL, -141, false, false, false); // 用户cli只杀当前身份。
    run_one(airi_user_load_bad_start, (size_t)((uintptr_t)airi_user_load_bad_end - (uintptr_t)airi_user_load_bad_start), NULL, -142, false, false, false); // USER保护PF。
    run_one(airi_user_rsp_bad_start, (size_t)((uintptr_t)airi_user_rsp_bad_end - (uintptr_t)airi_user_rsp_bad_start), NULL, -22, false, false, false); // 坏返回栈不击穿内核。
    run_one(airi_hello_elf_start, (size_t)((uintptr_t)airi_hello_elf_end - (uintptr_t)airi_hello_elf_start), "hello", 0, true, false, false); // 独立C和自有用户库。
    run_one(airi_process_elf_start, (size_t)((uintptr_t)airi_process_elf_end - (uintptr_t)airi_process_elf_start), "processes", 0, true, false, true); // 真实多进程复制、抢占、阻塞与回收。
    airi_log("[USER_DEMO_OK] ring=3 syscall=int80 bad_pointers=2 user_gp=13 user_pf=14 bad_rsp=rejected reclaimed=1"); // 完整会话结束，仍回串口命令。
}

bool airi_user_command(struct airi_console *console, const char *command) { // 系统命令消费者独立于图形主题扩展。
    size_t bytes = airi_strlen(command); // 先取得有效长度，固定前缀比较不能越过短字符串。
    if (bytes == 8 && !airi_memcmp(command, "programs", 8)) { // 显示当前初始ELF目录。
        airi_console_print(console, "programs: hello processes worker broken"); // broken是明确损坏的演示输入。
        return true; // 已处理，不再交给图形扩展。
    }
    if (bytes < 4 || airi_memcmp(command, "run ", 4)) // 只接受明确的run空格前缀。
```
<!-- after -->
```c
        airi_log("[PROCESS_DEMO_OK] fork=copy exec=transaction wait=blocked timer_switches=%u wait_blocks=%u reclaimed=1", result.switches, result.blocks); // 计数来自真实IRQ与状态转换。
}
void airi_user_demo(void) { // 串口主线的真实用户边界与多进程回归。
    airi_files_init(); // 用户会话开始前创建只读seed与空RAM目录，仅启动一次。
    run_one(airi_user_probe_start, (size_t)((uintptr_t)airi_user_probe_end - (uintptr_t)airi_user_probe_start), NULL, 0, false, true, false); // 原有非法指针与用户IRQ验证。
    run_one(airi_user_bad_start, (size_t)((uintptr_t)airi_user_bad_end - (uintptr_t)airi_user_bad_start), NULL, -141, false, false, false); // 用户cli只杀当前身份。
    run_one(airi_user_load_bad_start, (size_t)((uintptr_t)airi_user_load_bad_end - (uintptr_t)airi_user_load_bad_start), NULL, -142, false, false, false); // USER保护PF。
    run_one(airi_user_rsp_bad_start, (size_t)((uintptr_t)airi_user_rsp_bad_end - (uintptr_t)airi_user_rsp_bad_start), NULL, -22, false, false, false); // 坏返回栈不击穿内核。
    run_one(airi_hello_elf_start, (size_t)((uintptr_t)airi_hello_elf_end - (uintptr_t)airi_hello_elf_start), "hello", 0, true, false, false); // 独立C和自有用户库。
    run_one(airi_process_elf_start, (size_t)((uintptr_t)airi_process_elf_end - (uintptr_t)airi_process_elf_start), "processes", 0, true, false, true); // 真实多进程复制、抢占、阻塞与回收。
    run_one(airi_files_elf_start, (size_t)((uintptr_t)airi_files_elf_end - (uintptr_t)airi_files_elf_start), "files", 0, true, false, false); // 文件内容、共享偏移与exec引用回收。
    airi_log("[USER_DEMO_OK] ring=3 syscall=int80 bad_pointers=2 user_gp=13 user_pf=14 bad_rsp=rejected reclaimed=1"); // 完整会话结束，仍回串口命令。
}

bool airi_user_command(struct airi_console *console, const char *command) { // 系统命令消费者独立于图形主题扩展。
    size_t bytes = airi_strlen(command); // 先取得有效长度，固定前缀比较不能越过短字符串。
    if (bytes == 8 && !airi_memcmp(command, "programs", 8)) { // 显示当前初始ELF目录。
        airi_console_print(console, "programs: hello processes worker broken files"); // broken是明确损坏的演示输入。
        return true; // 已处理，不再交给图形扩展。
    }
    if (bytes < 4 || airi_memcmp(command, "run ", 4)) // 只接受明确的run空格前缀。
```

**按完整版本更新用户头。** 用户整数FD不暴露内核对象，接口暂未兼容POSIX。

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
#endif // 结束包含保护。
```

**按完整版本更新用户系统调用包装。** 原call3保留，显式定位用call4。

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
```

<a id="file-user-walk"></a>
## 在新用户映像里继续读同一个文件

files先检查/notes的六字节初值和pwrite覆盖，再从显式偏移0读8字节。实际返回6，普通偏移仍为6，接着普通read返回EOF；这说明pread没有偷偷移动共享位置。随后定位1022短写两字节，再读中间洞，逐字节确认为零。

现在跟回章首的 abcdef。关闭 /notes 后，最低空 FD 重新为 0，/seed 占 0；stdout/stderr 仍占 1 和 2，所以带 CLOEXEC 的 duplicate 占 3。fork 以后父子都有 0 和 3，这四个表项指向同一个打开对象。

孩子先从 FD0 读 ab，将唯一的 offset 推到 2，再 exec worker。worker 的新用户缓冲没有继承旧孩子栈里的 ab，FD0 却仍引用刚才的打开对象，因此接着读 cd，把 offset 推到 4。成功 exec 关闭的是孩子的 FD3；父亲自己的 FD3 仍在。

父亲 wait 后从自己的 FD3 读 ef，将同一 offset 推到 6，最后关闭 0 和 3。孩子的 FD3 已消失而父亲仍可读，说明 cloexec 关闭一个表项引用，不会强制销毁仍被别人引用的打开对象。

worker仍支持K23的独立运行：如果FD0返回−9就没有文件继承场景，沿旧路径退出23。本节以明确的FD0/3布置作为演示协议，不把它当作通用应用猜测文件用途的推荐方法。以后shell通过显式参数或标准输入重定向安排接口。

**新建 `labs/airios/files.c`。** 只有全部真实用户调用成立才输出FILES_USER_OK。

<!-- write: labs/airios/files.c -->
```c
#include <airi_libc.h> // 真实用户程序通过int80操作FD，不调用内核函数。
int main(int argc, char **argv) { // 用户入口仍由自己的_start提供。
    if (argc != 1 || memcmp(argv[0], "files", 6)) // 验证装载的是这份ELF。
        return 40; // 不发布文件完成标记。
    char buffer[16] = {0}; // 同步读写缓冲在用户栈。
    if (airi_open("/missing", AIRI_READ) != -2 || airi_open("/seed", AIRI_WRITE) != -13) // 名称缺失和权限错误分开。
        return 41; // 文件层错误不能伪装成功。
    long fd = airi_open("/notes", AIRI_READ | AIRI_WRITE | AIRI_CREATE | AIRI_TRUNCATE); // 独立打开对象，内容在RAM。
    if (fd < 0 || airi_write((unsigned)fd, "abcdef", 6) != 6) // 只推进实际完成数量。
        return 42; // 后续偏移演示需要完整初值。
    if (airi_pwrite((unsigned)fd, "XY", 2, 1) != 2 || airi_pread((unsigned)fd, buffer, 8, 0) != 6 || memcmp(buffer, "aXYdef", 6)) // 显式定位覆盖与EOF短读。
        return 43; // 请求8只能返回实际6。
    if (airi_read((unsigned)fd, buffer, 1) != 0) // 前面的pread不能把原共享偏移6移回0。
        return 44; // EOF证明位置没有被pread移动。
    if (airi_seek((unsigned)fd, 1022) != 1022 || airi_write((unsigned)fd, "1234", 4) != 2 || airi_write((unsigned)fd, "x", 1) != -28) // 仅剩两字节产生短写，随后明确容量错误。
        return 45; // 不允许数组越界或假完成4。
    memset(buffer, 1, sizeof(buffer)); // 洞读取应覆盖为零。
    if (airi_pread((unsigned)fd, buffer, sizeof(buffer), 10) != 16) // 写到1022使10到1021成为零洞。
        return 46; // 长度应覆盖完整缓冲。
    for (unsigned i = 0; i < sizeof(buffer); ++i) // 不把初始化1误当文件内容。
        if (buffer[i]) // 每个洞字节都必须为零。
            return 47; // 防止truncate后的旧内容泄漏。
    if (airi_close((unsigned)fd) || airi_close((unsigned)fd) != -9) // 表项只能释放一次。
        return 48; // 内容仍在目录，打开对象已回收。
    fd = airi_open("/seed", AIRI_READ); // 槽0应被复用，供exec后的worker辨认本次FD演示。
    long duplicate = airi_dup((unsigned)fd, 1); // 成功exec关闭此表项，父亲的对应表项不受影响。
    if (fd != 0 || duplicate != 3 || airi_read((unsigned)fd, (void *)"readonly", 2) != -14) // 只读用户输出缓冲必须在消费偏移前拒绝。
        return 49; // 下一次读仍应从a开始。
    long pid = airi_fork(); // 子表项引用相同打开对象，用户缓冲页独立复制。
    if (pid < 0) // 容量问题显式处理。
        return 50; // 不继续等待不存在孩子。
    if (!pid) { // 孩子先读ab，再exec让新程序读cd。
        if (airi_read((unsigned)fd, buffer, 2) != 2 || memcmp(buffer, "ab", 2)) // 真正消费共享偏移。
            return 51; // 父亲会收到失败状态。
        airi_exec("worker"); // 普通FD保留，CLOEXEC关闭，PID仍是本孩子。
        return 52; // 成功不能返回原映像。
    }
    int status = -1; // 自有wait返回普通退出值。
    if (airi_waitpid(pid, &status) != pid || status != 23) // worker完成继承检查后返回23。
        return 53; // 不能只观察孩子创建成功。
    if (airi_read((unsigned)duplicate, buffer, 2) != 2 || memcmp(buffer, "ef", 2)) // 子exec后读cd已前移同一打开对象，父dup仍开放。
        return 54; // fork+exec的共享偏移真实生效。
    if (airi_close((unsigned)fd) || airi_read((unsigned)duplicate, buffer, 1) != 0 || airi_close((unsigned)duplicate)) // 最后引用才回收，不因父关闭一个编号破坏另一个。
        return 55; // 必须正确到达EOF。
    airi_puts("[FILES_USER_OK] short_io=1 holes=zero pread_offset=stable fork_offset=shared cloexec=per_fd\n"); // 仅所有实际检查成立后发布。
    return 0; // launch随后核对内存和全局打开对象恢复。
}
```

**按完整版本更新 `worker.c`。** 新ELF内验证继承和CLOEXEC，普通worker调用保持可用。

<!-- write: labs/airios/worker.c -->
```c
#include <airi_libc.h> // 新映像使用自己的入口、用户库和地址空间。
static volatile unsigned char initialized = 0x53; // 新ELF的数据初值不能继承旧用户映像。
static volatile unsigned char zeroed[4096]; // 新BSS由装载器重新清零。
int main(int argc, char **argv) { // exec成功应从_start重建argc/argv，而不是返回旧调用。
    if (argc != 1 || !argv || !argv[0] || strlen(argv[0]) != 6 || memcmp(argv[0], "worker", 7) || initialized != 0x53) // 新参数和新数据都必须存在。
        return 71; // 旧映像残留则失败。
    for (size_t i = 0; i < sizeof(zeroed); ++i) // 检查整个BSS，不只首字节。
        if (zeroed[i]) // 不允许泄漏旧地址空间或页池内容。
            return 72; // 新入口不发布成功。
    unsigned char *page = airi_alloc(4096); // exec应重置用户页游标，旧孩子曾在同一VA写99。
    if ((uintptr_t)page != 0x10000000 || page[0]) // 新页必须重新拥有且清零。
        return 73; // 不能保留旧heap_next或旧数据。
    page[0] = 7; // 新映像拥有自己的可写页。
    zeroed[sizeof(zeroed) - 1] = 9; // BSS尾页也可写。
    airi_puts("[EXEC_WORKER_OK] argv=worker heap=zeroed bss=new\n"); // 真实来自新ELF的用户输出。
    char inherited[2]; // files工作负载把只读seed保留在FD0，普通worker没有FD0。
    long inherited_bytes = airi_read(0, inherited, 2); // 成功exec后的新程序消费同一打开对象。
    if (inherited_bytes != -9) { // 只有明确继承文件的调用才做这组检查。
        if (inherited_bytes != 2 || memcmp(inherited, "cd", 2) || airi_read(3, inherited, 1) != -9) // CLOEXEC表项已关闭，普通偏移继续。
            return 24; // 父亲期望23，因此错误不能被吞掉。
        airi_puts("[FD_EXEC_OK] inherited=0 closed=3 offset=4\n"); // 来自新ELF中的真实读结果。
    }
    return 23; // 父进程应仍通过原孩子PID取得这个状态。
}
```

**更新映像包装。** 新文件演示单独链接成ELF，不把main直接放进内核。

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
```

**新建 `tests/unit/file_test.c`。** 主机检查另外填满进程表与全局打开槽，验证失败truncate保持原内容；这些不是磁盘或Linux结果。

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
    puts("PASS RAM files: offsets, fork/exec references, sparse zero, short I/O, transactional exhaustion"); // 全部边界通过才发布。
    return 0; // 宿主测试成功。
}
```

**按旧/新窗口更新构建规则。** 内核加入file.c，用户新增files.elf，host-test加入同一文件算法的ASan/UBSan检查。

把 file.c 加入内核源码清单。

<!-- replace: Makefile -->
<!-- before -->
```make
# 基础目标只编译文本会话与设备；不读取 frontend 的目录或字体。
SOURCES := kernel/main.c kernel/boot/limine.c kernel/boot/info.c \
    kernel/lib/base.c kernel/lib/runtime.c kernel/arch/x86_64/serial.c \
    kernel/console/console.c kernel/arch/x86_64/input.c kernel/arch/x86_64/cpu.c kernel/arch/x86_64/trap.c kernel/mm/pages.c kernel/mm/memory.c kernel/arch/x86_64/paging.c kernel/mm/heap.c kernel/arch/x86_64/timer.c kernel/task/task.c kernel/process/user.c kernel/process/elf.c
# 只有 desktop- 前缀目标才读取图形支线自己的构建片段。
ifneq ($(filter desktop-%,$(PROFILE)),)
include frontend/sources.mk # 本分支取得图形自己的源码清单。
```
<!-- after -->
```make
# 基础目标只编译文本会话与设备；不读取 frontend 的目录或字体。
SOURCES := kernel/main.c kernel/boot/limine.c kernel/boot/info.c \
    kernel/lib/base.c kernel/lib/runtime.c kernel/arch/x86_64/serial.c \
    kernel/console/console.c kernel/arch/x86_64/input.c kernel/arch/x86_64/cpu.c kernel/arch/x86_64/trap.c kernel/mm/pages.c kernel/mm/memory.c kernel/arch/x86_64/paging.c kernel/mm/heap.c kernel/arch/x86_64/timer.c kernel/task/task.c kernel/process/user.c kernel/process/elf.c kernel/fs/file.c
# 只有 desktop- 前缀目标才读取图形支线自己的构建片段。
ifneq ($(filter desktop-%,$(PROFILE)),)
include frontend/sources.mk # 本分支取得图形自己的源码清单。
```

在 host-test 接上第六组文件算法观察。

<!-- replace: Makefile -->
<!-- before -->
```make
	    kernel/process/elf.c kernel/lib/base.c -o build/tests/elf_test
# 失败不得写出半份view，也不得越界读取映像。
	./build/tests/elf_test
# 清理仅删除可重建产物，保留运行证据artifacts。
clean:
# 调用shutil删除build；ignore_errors允许尚不存在目录。
```
<!-- after -->
```make
	    kernel/process/elf.c kernel/lib/base.c -o build/tests/elf_test
# 失败不得写出半份view，也不得越界读取映像。
	./build/tests/elf_test
# 文件算法在主机使用同一实现，验证引用、偏移和容量回滚。
	$(HOST_CC) -std=c11 -g -O1 -Wall -Wextra -Werror -fno-builtin \
	    -fsanitize=address,undefined -Iinclude tests/unit/file_test.c \
	    kernel/fs/file.c kernel/lib/base.c -o build/tests/file_test
# 仅全部文件边界成立才继续启动检查。
	./build/tests/file_test
# 清理仅删除可重建产物，保留运行证据artifacts。
clean:
# 调用shutil删除build；ignore_errors允许尚不存在目录。
```

把 files.elf 加入包装对象的依赖，输入变化才会重新嵌入。

<!-- replace: Makefile -->
<!-- before -->
```make
# 用户只链接自己的_start、main和libc子集，无宿主依赖。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_OBJECTS) -o $@
# 包装汇编明确依赖本配置用户ELF，文件变动就重新嵌入。
$(OUT)/labs/airios/images.o: labs/airios/images.S $(OUT)/user/hello.elf $(OUT)/user/processes.elf $(OUT)/user/worker.elf
# 建立包装对象目录。
	@mkdir -p $(dir $@)
# 单引号保护路径宏内部双引号，.incbin只读本配置生成文件。
	$(CC) $(CPPFLAGS) $(CFLAGS) -DAIRI_HELLO_ELF='"$(OUT)/user/hello.elf"' -DAIRI_PROCESS_ELF='"$(OUT)/user/processes.elf"' -DAIRI_WORKER_ELF='"$(OUT)/user/worker.elf"' -c $< -o $@

# 第二个用户程序只复用用户入口与库，不把hello的main再次链接进去。
USER_COMMON := $(filter-out $(OUT)/user/hello.o,$(USER_OBJECTS))
```
<!-- after -->
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

在尾部加入 files 的独立编译与链接目标，继续复用用户入口和库。

<!-- replace: Makefile -->
<!-- before -->
```make
$(OUT)/user/worker.elf: $(USER_COMMON) $(OUT)/user/worker.o labs/airios/program.ld
# 复用用户入口与库，不混合其他程序main。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/worker.o -o $@
```
<!-- after -->
```make
$(OUT)/user/worker.elf: $(USER_COMMON) $(OUT)/user/worker.o labs/airios/program.ld
# 复用用户入口与库，不混合其他程序main。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/worker.o -o $@

# 文件演示有独立用户main，复用自有入口与库。
$(OUT)/user/files.o: labs/airios/files.c labs/airios/include/airi_libc.h
# 按配置建立目录。
	@mkdir -p $(dir $@)
# 同样禁止宿主库和浮点寄存器。
	$(CC) $(USER_FLAGS) -Ilabs/airios/include -c $< -o $@
# 只链接这份工作负载的main。
$(OUT)/user/files.elf: $(USER_COMMON) $(OUT)/user/files.o labs/airios/program.ld
# 独立低地址ELF由内核包装后装载。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/files.o -o $@
```

**更新启动验收的对应窗口。** READY仍需所有早期路径结束，并新增文件与继承标记，不能只看QEMU仍在运行。

在正常启动核对中加入两项文件结果；QEMU 存活本身不能满足条件。

<!-- replace: scripts/boot.py -->
<!-- before -->
```python
                        assert '[AIRI_P1_READY]' not in text  # 错误路径不得出现成功标记。
                    else:  # 按当前profile的另一条完整验证路径处理。
                        assert '[EXEC_FAILURE_OK]' in text and '[EXEC_WORKER_OK]' in text and '[EXEC_COMMIT]' in text # 坏映像保持与成功替换都须实际运行。
                        assert '[PROCESS_FORK_WAIT_OK]' in text and '[PROCESS_DEMO_OK]' in text # 复制、真实阻塞、timer换身份和回收都须完成。
                        assert '[ELF_LOAD]' in text and '[ELF_USER_OK]' in text # 独立ELF段、BSS与用户libc必须真正运行。
                        assert '[USER_DEMO_OK]' in text # 用户输出、指针拒绝和用户故障后内核必须继续。
```
<!-- after -->
```python
                        assert '[AIRI_P1_READY]' not in text  # 错误路径不得出现成功标记。
                    else:  # 按当前profile的另一条完整验证路径处理。
                        assert '[EXEC_FAILURE_OK]' in text and '[EXEC_WORKER_OK]' in text and '[EXEC_COMMIT]' in text # 坏映像保持与成功替换都须实际运行。
                        assert '[FILES_USER_OK]' in text and '[FD_EXEC_OK]' in text # RAM文件和成功exec继承均完成。
                        assert '[PROCESS_FORK_WAIT_OK]' in text and '[PROCESS_DEMO_OK]' in text # 复制、真实阻塞、timer换身份和回收都须完成。
                        assert '[ELF_LOAD]' in text and '[ELF_USER_OK]' in text # 独立ELF段、BSS与用户libc必须真正运行。
                        assert '[USER_DEMO_OK]' in text # 用户输出、指针拒绝和用户故障后内核必须继续。
```

<a id="file-observe"></a>
## 编译、运行和正常错误后的继续

```sh
./airios test # 六组主机检查、各启动故障与真实输入均使用当前源码。
./airios run # 在QEMU串口输入programs，再输入run files。
```

按既有观察条件，正常启动和 run files 应出现FD_EXEC_OK与FILES_USER_OK，程序退出0。每次用户会话还检查内存恢复与全局打开对象为0。/notes由files自己truncate，因此同一启动内多次运行不会依赖上次内容。/seed的写open返回−13，缺名字读open返回−2，只读用户输出拒绝为−14，第二次close返回−9；这些普通错误都不停止串口命令。

调试时在airi_fd_read前比较parent.files.entries[0].file与child同项，它们应相同；这个对象的offset依次0→2→4→6。用户buffer却由各自页表翻译到不同物理页。GDB需要个人主机可连，尚未连接的停点保留为调试操作，不列为已观察。

Linux接口对照见[open](https://man7.org/linux/man-pages/man2/open.2.html)与[pread](https://man7.org/linux/man-pages/man2/pread.2.html)。L03提供真实Linux父子程序，但当前SSH255，Linux结果仍未验证。下一步把块设备接到独立磁盘实验，RAM文件完成不表示内容已经落盘。
