# K37 · 扩大用户内存，同时让进程控制块保持较小

从 K36 继续。常驻 init 可以重启 shell，但旧的六十四页预算只有 256 KiB，连一个 384 KiB 的静态缓冲区也装不下。这里沿 `sh → capacity → fork → exec worker → wait → sh` 运行大一些的程序，扩展 ELF、用户栈和动态页容量，并保留失败后的旧地址空间。

本节将每进程用户数据页上限提高到 1024 页，即 4 MiB；静态 ELF 为 64 KiB 初始用户栈预留十六页，单次动态申请与归还最多六十四页，即 256 KiB。页表、内核陷入栈和保存页归属的记录另外占物理页，不计入这 4 MiB。目标机器仍是单 CPU、256 MiB QEMU，进程槽仍为八个。内核小对象堆仍是 64 KiB；这里的扩容先解决用户程序，不引入 malloc、线程或 C++ 运行时。

<a id="capacity-records"></a>
## 页的记录也需要内存

K35 的 `owned[64]` 直接放在每个 `user_process` 中。一项记录保存用户 VA、物理 PA 和权限，当前 x86-64 结构占 24 字节。若直接改成 `owned[1024]`，单个数组就有 24576 字节；而 `exec_current` 还会在当前内核栈上同时保存新控制块 next 与旧控制块 old。每进程的内核陷入栈只有 16 KiB，两份大数组会越过保护页。

数据页变多时，需要增长的是记录空间。控制块只保存七个物理页地址 `records[7]`，真正的记录页按需从物理页池取得。内核已经将受管理 RAM 映射进 HHDM，因而可以用 `airi_memory_direct` 访问记录，不必为它们建立另一组虚拟地址。用户页表没有给这些记录 USER 权限，用户不能改写页归属。记录页本身也不能登记成用户数据页，否则释放记录需要读取另一条记录，会产生没有必要的递归。

每个 4096 字节物理页可以放 `4096 / 24 = 170` 项，剩余十六字节不用。有效项的逻辑下标保持连续，记录页的物理位置可以分散。`owned_at(p, index)` 先用除法找记录页，再用余数找页内项：

| 逻辑下标 | records 槽 | 页内项 | 发生的操作 |
| --- | --- | --- | --- |
| 0 | 0 | 0 | 第一次建立用户页时借第一个记录页 |
| 169 | 0 | 169 | 仍在第一个记录页内 |
| 170 | 1 | 0 | 新增第171个用户页前借第二个记录页 |
| 1023 | 6 | 3 | 满预算时使用第七个记录页的第四项 |

count 继续表示活的数据页记录数量，不把保存这些记录的页算进去。删除一项时，先把最后一项复制到空位，再缩小 count；末项可能在另一个物理记录页上，因此不能先释放那一页。

复制完成以后，`trim_records` 才从尾部收回没有活项的记录页。比如 count 从171变成170：原第170下标的末项已经被搬走，第二个记录页不再有消费者，可以归还；第一页仍保存170条有效记录。清理完全部用户数据页后 count 为0，七个记录槽也都不再持页。

先新建 `include/airi/user_limits.h`，让 ELF 检查和用户分配消费同一份预算。这里的宏只是编译时整数；它们不自动分配 RAM，也不扩展用户地址范围。

<!-- write: include/airi/user_limits.h -->
```c
#ifndef AIRI_USER_LIMITS_H // 装载器与进程页记录使用同一预算。
#define AIRI_USER_LIMITS_H // 避免重复宏定义。
#define AIRI_USER_PAGES 1024u // 每进程最多4MiB用户数据；页表、元数据和内核栈另计。
#define AIRI_USER_STACK_PAGES 16u // 静态ELF初始栈64KiB，下面保持未映射保护页。
#define AIRI_USER_REQUEST_PAGES 64u // 一次页面申请或归还最多256KiB，内核操作保持有界。
#endif // 结束预算声明。
```

更新 `include/airi/elf.h`。ELF 的段预算是总预算减十六个栈页，因此最多接受 1008 个段数据页。检查器仍在分配前完整核对输入，拒绝动态链接、TLS、RWX 与重叠页；这次只扩大资源量。

<!-- write: include/airi/elf.h -->
```c
#ifndef AIRI_ELF_H // 静态ELF解析边界只声明一次。
#define AIRI_ELF_H // 包含保护。
#include <stdbool.h> // 校验器使用布尔范围与入口状态。
#include <airi/base.h> // 固定宽度字段与自有字节复制。
#define AIRI_ELF_SEGMENTS 8u // 教学程序最多八个LOAD段。
#include <airi/user_limits.h> // ELF验证必须为实际初始栈预留容量。
#define AIRI_ELF_PAGES (AIRI_USER_PAGES - AIRI_USER_STACK_PAGES) // 段最多1008页，栈的16页单独建立。
struct airi_elf_segment { // 从外部字节校验后复制出的段事实。
    uint64_t address, offset, file_bytes, memory_bytes; // 用户VA、映像偏移、初始化长度和总内存长度。
    unsigned flags; // 原ELF PF_R/PF_W/PF_X，不能当作x86 PTE直接写入。
};
struct airi_elf_view { // 只借用输入映像，装载完成前映像必须持续有效。
    const uint8_t *image; // 不拥有也不修改这些输入字节。
    size_t bytes; // 输入映像的实际可读取长度。
    uint64_t entry; // 入口必须位于可执行段的文件内容中。
    unsigned count, pages; // 已验证LOAD段数与向外舍入后的总页数。
    struct airi_elf_segment segments[AIRI_ELF_SEGMENTS]; // 每项已通过范围、权限和重叠检查。
};
const char *airi_elf_inspect(const void *image, size_t bytes, struct airi_elf_view *view); // NULL表示通过；失败不修改输出view。
#endif // 结束包含保护。
```

下面在已有 `kernel/process/user.c` 中修改控制块页清单、记录访问与回收、ELF 栈建立、fork复制、动态页申请/归还及 capacity 映像登记。先接入小控制块和两个记录助手，再沿各自消费者替换；原调度、文件和等待函数保留，全部接入以后再编译。

`owned_at` 只借出当前内核操作中的指针，调用者不能跨 `trim_records` 保存它。记录页没有独立映射要解除，归还其物理页以后 HHDM 地址仍存在，但原记录已经失效，不能再访问。控制块新增的静态断言把其大小限制在 1024 字节以内，防止以后再次把容量数组塞回控制块。

调整用户栈说明、页记录布局与控制块大小约束；容量增加到外部记录页，栈上的控制块保持较小。

本章每组替换的第一块只用于定位旧文本，敲入第二块替换它，不把两块同时加入文件。各组按正文顺序完成。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
#include <airi/timer.h> // 用户时间片依赖真实IRQ0。
#include <airi/kernel.h> // 有界串口输出与故障报告。
#define USER_CODE 0x400000ull // 旧受控映像的用户入口。
#define USER_STACK 0x70000000ull // 当前每进程一页初始用户栈。
#define STACK_BASE 0xffffe10000000000ull // 所有进程陷入栈位于同一长期内核分支。
#define USER_HEAP 0x10000000ull // ELF静态范围之外的用户页分配起点。
#define USER_HEAP_END 0x20000000ull // 动态用户页只占这段独立范围，不接近初始用户栈。
#define PROCESS_COUNT 8u // 有界进程表，不做动态扩容。
enum user_state { USER_UNUSED, USER_RUNNABLE, USER_RUNNING, USER_WAITING, USER_IO_WAIT, USER_ZOMBIE, USER_REAP_PENDING }; // 退出与回收是两个不同状态。
enum io_operation { IO_NONE, IO_READ, IO_WRITE, IO_READY, IO_ASYNC, IO_SLEEP }; // 等待保存操作条件，不保留内核局部buffer。
struct user_page { uint64_t address, physical; unsigned flags; }; // fork需要把同一VA映射到另一个物理页。
struct user_process { // 所有权和调度状态由内核持有，用户只能看到PID。
    bool async_owned; // 本控制块是否持有当前PID的异步owner，局部旧exec副本必须清除此所有权。
    struct airi_fd_table files; // exec转移表项，fork增加同一打开对象的引用。
    struct airi_address_space space; // 根和低半区表自有，高半区借用。
    struct airi_kernel_region stack; // 不能在仍使用它的C调用中释放。
    struct user_page owned[64]; // 所有用户数据页，含代码、静态数据、堆页和栈。
    unsigned count, pid, parent, user_ticks; // 资源数量、身份、父PID和本进程实际IRQ数。
    uint64_t fault_error; // 用户故障证据；动态页位置由现有映射寻找，不再使用单向游标。
    int exit_code; // 自有wait接口返回普通退出码或负故障编码。
    enum user_state state; // 只有RUNNABLE可被调度器选中。
    int64_t wait_target; // 正PID表示指定孩子，-1表示任意孩子。
    uint64_t wait_status; // 已验证的父进程用户输出地址，阻塞期间父进程不会修改映射。
    enum io_operation io; // 只有USER_IO_WAIT消费下面的请求副本。
    unsigned io_fd, io_events; // FD属于本进程表，events只用于就绪等待。
    uint64_t io_address, io_bytes, io_deadline; // 用户页在阻塞期间仍由进程拥有，deadline按真实tick。
    struct airi_trap_frame initial; // 尚未运行时的寄存器值，不依赖临时C对象。
    struct airi_trap_frame *frame; // 运行后指向本进程内核栈的实际现场。
};
static struct user_process processes[PROCESS_COUNT]; // 槽可以复用，PID单调增长，不等于槽下标。
static struct user_process *current; // 只在完整陷入边界改变当前身份。
static unsigned next_pid = 1, root_pid, timer_switches, wait_blocks; // 每次运行会话的根身份和实际行为计数。
```
<!-- after -->
```c
#include <airi/timer.h> // 用户时间片依赖真实IRQ0。
#include <airi/kernel.h> // 有界串口输出与故障报告。
#define USER_CODE 0x400000ull // 旧受控映像的用户入口。
#define USER_STACK 0x70000000ull // 初始栈最高一页保持原地址，新增页向下展开。
#define STACK_BASE 0xffffe10000000000ull // 所有进程陷入栈位于同一长期内核分支。
#define USER_HEAP 0x10000000ull // ELF静态范围之外的用户页分配起点。
#define USER_HEAP_END 0x20000000ull // 动态用户页只占这段独立范围，不接近初始用户栈。
#define PROCESS_COUNT 8u // 有界进程表，不做动态扩容。
enum user_state { USER_UNUSED, USER_RUNNABLE, USER_RUNNING, USER_WAITING, USER_IO_WAIT, USER_ZOMBIE, USER_REAP_PENDING }; // 退出与回收是两个不同状态。
enum io_operation { IO_NONE, IO_READ, IO_WRITE, IO_READY, IO_ASYNC, IO_SLEEP }; // 等待保存操作条件，不保留内核局部buffer。
struct user_page { uint64_t address, physical; unsigned flags; }; // fork需要把同一VA映射到另一个物理页。
#define PAGE_RECORDS (4096u / sizeof(struct user_page)) // 一物理页放170项，余下16字节不用。
#define RECORD_CHUNKS ((AIRI_USER_PAGES + PAGE_RECORDS - 1) / PAGE_RECORDS) // 1024项最多借七个记录页。
struct user_process { // 所有权和调度状态由内核持有，用户只能看到PID。
    bool async_owned; // 本控制块是否持有当前PID的异步owner，局部旧exec副本必须清除此所有权。
    struct airi_fd_table files; // exec转移表项，fork增加同一打开对象的引用。
    struct airi_address_space space; // 根和低半区表自有，高半区借用。
    struct airi_kernel_region stack; // 不能在仍使用它的C调用中释放。
    uint64_t records[RECORD_CHUNKS]; // 元数据页的物理地址；各页经HHDM访问，不映射给用户。
    unsigned count, pid, parent, user_ticks; // 资源数量、身份、父PID和本进程实际IRQ数。
    uint64_t fault_error; // 用户故障证据；动态页位置由现有映射寻找，不再使用单向游标。
    int exit_code; // 自有wait接口返回普通退出码或负故障编码。
    enum user_state state; // 只有RUNNABLE可被调度器选中。
    int64_t wait_target; // 正PID表示指定孩子，-1表示任意孩子。
    uint64_t wait_status; // 已验证的父进程用户输出地址，阻塞期间父进程不会修改映射。
    enum io_operation io; // 只有USER_IO_WAIT消费下面的请求副本。
    unsigned io_fd, io_events; // FD属于本进程表，events只用于就绪等待。
    uint64_t io_address, io_bytes, io_deadline; // 用户页在阻塞期间仍由进程拥有，deadline按真实tick。
    struct airi_trap_frame initial; // 尚未运行时的寄存器值，不依赖临时C对象。
    struct airi_trap_frame *frame; // 运行后指向本进程内核栈的实际现场。
};
_Static_assert(sizeof(struct user_process) <= 1024, "exec control block must stay small"); // exec的新旧局部副本不能随用户容量膨胀。
static struct user_process processes[PROCESS_COUNT]; // 槽可以复用，PID单调增长，不等于槽下标。
static struct user_process *current; // 只在完整陷入边界改变当前身份。
static unsigned next_pid = 1, root_pid, timer_switches, wait_blocks; // 每次运行会话的根身份和实际行为计数。
```

声明 capacity 映像边界。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
extern const uint8_t airi_hello_elf_start[], airi_hello_elf_end[]; // 既有独立用户C程序。
extern const uint8_t airi_process_elf_start[], airi_process_elf_end[]; // 新的fork/wait和抢占工作负载。
extern const uint8_t airi_init_elf_start[], airi_init_elf_end[]; // 用户态常驻监督程序。
extern const uint8_t airi_memory_elf_start[], airi_memory_elf_end[]; // 用户页面归还与空洞复用工作负载。
extern const uint8_t airi_orphans_elf_start[], airi_orphans_elf_end[]; // 持续会话中的孤儿回收与槽复用演示。
extern const uint8_t airi_shell_elf_start[], airi_shell_elf_end[]; // 真正用户shell，不是内核命令别名。
```
<!-- after -->
```c
extern const uint8_t airi_hello_elf_start[], airi_hello_elf_end[]; // 既有独立用户C程序。
extern const uint8_t airi_process_elf_start[], airi_process_elf_end[]; // 新的fork/wait和抢占工作负载。
extern const uint8_t airi_init_elf_start[], airi_init_elf_end[]; // 用户态常驻监督程序。
extern const uint8_t airi_capacity_elf_start[], airi_capacity_elf_end[]; // 大BSS、深用户栈与跨记录页的fork/exec。
extern const uint8_t airi_memory_elf_start[], airi_memory_elf_end[]; // 用户页面归还与空洞复用工作负载。
extern const uint8_t airi_orphans_elf_start[], airi_orphans_elf_end[]; // 持续会话中的孤儿回收与槽复用演示。
extern const uint8_t airi_shell_elf_start[], airi_shell_elf_end[]; // 真正用户shell，不是内核命令别名。
```

将 capacity 加入初始目录。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
static const struct executable executables[] = { // 每份映像字节由内核只读段长期持有。
    {"sh", airi_shell_elf_start, airi_shell_elf_end}, // 独占串口输入的可选用户程序。
    {"init", airi_init_elf_start, airi_init_elf_end}, // 管理前台shell的常驻用户进程。
    {"memory", airi_memory_elf_start, airi_memory_elf_end}, // 同一地址空间内释放与重新申请。
    {"orphans", airi_orphans_elf_start, airi_orphans_elf_end}, // 不结束根会话也能回收退出的孤儿。
    {"demo", airi_demo_elf_start, airi_demo_elf_end}, // 页、任务和设备完成串联。
```
<!-- after -->
```c
static const struct executable executables[] = { // 每份映像字节由内核只读段长期持有。
    {"sh", airi_shell_elf_start, airi_shell_elf_end}, // 独占串口输入的可选用户程序。
    {"init", airi_init_elf_start, airi_init_elf_end}, // 管理前台shell的常驻用户进程。
    {"capacity", airi_capacity_elf_start, airi_capacity_elf_end}, // 预算耗尽后回收并继续运行。
    {"memory", airi_memory_elf_start, airi_memory_elf_end}, // 同一地址空间内释放与重新申请。
    {"orphans", airi_orphans_elf_start, airi_orphans_elf_end}, // 不结束根会话也能回收退出的孤儿。
    {"demo", airi_demo_elf_start, airi_demo_elf_end}, // 页、任务和设备完成串联。
```

在 slot 与 create_stack 之间加入记录访问和裁尾助手。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
static bool create_stack(struct user_process *p) { // 每槽前面留一页保护页，所有栈共享已存在高半区分支。
    uint64_t address = STACK_BASE + slot(p) * 5 * 4096ull + 4096; // 四页数据与一页保护，PID增长不扩散地址。
    return airi_memory_region_create(&p->stack, address, 4); // 完整成功后才借给用户根。
}
static bool user_access(const struct user_process *p, uint64_t address, size_t bytes, bool write) { // 检查完整用户范围，不直接解引用它。
    if (address >= AIRI_USER_LIMIT || bytes > AIRI_USER_LIMIT - address) // 排除内核地址与右端溢出。
```
<!-- after -->
```c
static bool create_stack(struct user_process *p) { // 每槽前面留一页保护页，所有栈共享已存在高半区分支。
    uint64_t address = STACK_BASE + slot(p) * 5 * 4096ull + 4096; // 四页数据与一页保护，PID增长不扩散地址。
    return airi_memory_region_create(&p->stack, address, 4); // 完整成功后才借给用户根。
}
static struct user_page *owned_at(const struct user_process *p, unsigned index) { // 下标可等于count供新增项写入，但必须已有记录页。
    unsigned chunk = index / PAGE_RECORDS; // 整数除法选出记录页，不要求各物理页连续。
    if (index >= AIRI_USER_PAGES || !p->records[chunk]) // 所有调用均来自内核已经验证的有效项。
        airi_panic("user page record is missing"); // 内部越界不能返回任意HHDM指针。
    struct user_page *entries = airi_memory_direct(p->records[chunk]); // 别名只借用，物理所有权仍在records中。
    return &entries[index % PAGE_RECORDS]; // 余数选页内项；单项不会跨物理页。
}
static void trim_records(struct user_process *p) { // count已缩小，释放不再容纳任何活记录的尾部元数据页。
    unsigned needed = (p->count + PAGE_RECORDS - 1) / PAGE_RECORDS; // 零项需要零页，其余向上取整。
    for (unsigned i = needed; i < RECORD_CHUNKS; ++i) { // 固定七槽，只检查完全位于有效项之外的页。
        if (!p->records[i]) // 尚未分配的槽没有物理所有权。
            continue; // 不把0哨兵交给页池。
        if (!airi_memory_return_page(p->records[i])) // 没有独立VA映射，HHDM随内核始终存在。
            airi_panic("cannot reclaim user page records"); // 归属错误不能静默复用页面。
        p->records[i] = 0; // 归还后立即撤掉唯一所有权记录。
    }
}
static bool user_access(const struct user_process *p, uint64_t address, size_t bytes, bool write) { // 检查完整用户范围，不直接解引用它。
    if (address >= AIRI_USER_LIMIT || bytes > AIRI_USER_LIMIT - address) // 排除内核地址与右端溢出。
```

用户指针检查与文件副本仍按目标进程的页表访问，不通过记录页推断权限。这条访问路径、等待重试及初始现场构造不需修改，接下来只在 cleanup 中接入记录页回收。

cleanup 的次序是：切离目标根由调用者完成，随后销毁低半区页表，逐项归还用户数据页，再释放保存这些项的记录页，最后释放已经离开的内核栈。若先释放记录，清理者就失去了还应归还哪些数据页的信息。初始 argc/argv 仍放在最高一页的顶部，原用户 `_start` 无需改变。

cleanup 逐项借 owned_at 归还用户数据，最后再裁掉已经无活项的记录页。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    if (p->space.root && !airi_vm_destroy(&p->space)) // 先解除并归还自有页表，不动借用的内核表。
        airi_panic("cannot destroy inactive user tables"); // 不能归还仍被活动根引用的数据页。
    while (p->count) // 用户代码与用户栈数据页分别归还。
        if (!airi_memory_return_page(p->owned[--p->count].physical)) // 先退到最后一个真正拥有的页。
            airi_panic("cannot return owned user page"); // 归属错误不继续复用内存。
    if (p->stack.count && !airi_memory_region_free(&p->stack)) // 调用者已经离开目标内核栈，才可销毁它。
        airi_panic("cannot reclaim user kernel stack"); // 必须已经离开目标内核栈，不能在退出者仍使用它时提前回收。
    airi_memset(p, 0, sizeof(*p)); // 不留下可重复释放的根或句柄。
```
<!-- after -->
```c
    if (p->space.root && !airi_vm_destroy(&p->space)) // 先解除并归还自有页表，不动借用的内核表。
        airi_panic("cannot destroy inactive user tables"); // 不能归还仍被活动根引用的数据页。
    while (p->count) // 用户代码与用户栈数据页分别归还。
        if (!airi_memory_return_page(owned_at(p, --p->count)->physical)) // 先退到最后一个真正拥有的页。
            airi_panic("cannot return owned user page"); // 归属错误不继续复用内存。
    trim_records(p); // 所有数据页已归还，最后释放保存其VA/PA的记录页。
    if (p->stack.count && !airi_memory_region_free(&p->stack)) // 调用者已经离开目标内核栈，才可销毁它。
        airi_panic("cannot reclaim user kernel stack"); // 必须已经离开目标内核栈，不能在退出者仍使用它时提前回收。
    airi_memset(p, 0, sizeof(*p)); // 不留下可重复释放的根或句柄。
```

<a id="capacity-load"></a>
## 建立数据映射之前，先确保有位置记录它

装载 ELF 或申请动态页最终都经过 `owned_page`。当 count 为 170 时，下一项要写入第二个记录页；函数先借记录页，再借数据页，清零数据并建立用户映射，写入 VA/PA/权限，最后增加 count。新增记录页成功而数据页分配失败时，count 尚未增长，`trim_records` 可以立即收回这个空记录页。映射失败时同样先归还本次数据页，再收回没有活项的记录页。调用者负责回滚本批此前已经成功的用户页。

ELF 的每个段仍先清零完整页，再复制 filesz 指定的文件字节。因此 384 KiB BSS 增长的是 memsz 和实际分配页数，内核输入文件不需要包含 384 KiB 的零。段全部完成后，装载器从 `0x70000000` 这一最高栈页向下建立十六页，得到 `[0x6fff1000, 0x70001000)` 的 64 KiB 栈。`0x6fff0000` 仍没有映射，用于观察越过栈低端的真实故障。这里没有按需缺页扩栈；超出固定范围会结束该用户进程。

接下来替换单页构造和两种映像的装载位置。受控 flat 汇编映像仅用于早期故障回归，仍用原来的一页栈；普通静态 ELF 才走十六页栈。原有进程调度与退出流程保留。

owned_page 先保证记录位置，再借数据页与建立映射；每种失败都收回空记录页。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    p->frame->rflags = 0x202; // 用户IF开启，IOPL为零。
}
static bool owned_page(struct user_process *p, uint64_t address, unsigned flags) { // 一次只建立一页，失败页面仍可由统一cleanup找到。
    if (p->count == 64) // 包括初始栈在内的全部数据页预算。
        return false; // 不越过所有权数组。
    uint64_t physical = airi_memory_take_page(); // 取得真正归本上下文持有的物理页。
    if (!physical) // 耗尽时没有需要登记的页。
        return false; // 调用方回滚已有部分。
    airi_memset(airi_memory_direct(physical), 0, 4096); // 文件尾部与BSS默认都从零开始。
    if (!airi_vm_map(&p->space, address, physical, flags | AIRI_VM_USER)) { // 单页映射失败还没有发布到所有权数组。
        airi_memory_return_page(physical); // 明确归还本次未接入数据页。
        return false; // 调用者回滚此前成功的部分。
    }
    p->owned[p->count++] = (struct user_page){address, physical, flags | AIRI_VM_USER}; // 保存VA/PA/权限供fork复制与精确回收。
    return true; // 每个已登记数据页都有对应映射。
}
static int prepare_elf(struct user_process *p, const uint8_t *image, size_t bytes, const char *name, bool make_stack) { // 空目标只拥有新用户资源；exec不另建正在使用的内核栈。
```
<!-- after -->
```c
    p->frame->rflags = 0x202; // 用户IF开启，IOPL为零。
}
static bool owned_page(struct user_process *p, uint64_t address, unsigned flags) { // 一次只建立一页，失败页面仍可由统一cleanup找到。
    if (p->count == AIRI_USER_PAGES) // 包括ELF、动态页和用户栈，不包括记录页。
        return false; // 不越过固定记录容量。
    unsigned chunk = p->count / PAGE_RECORDS; // 下一项应写入哪一页元数据。
    if (!p->records[chunk]) { // 仅跨过170项边界时才增加一个记录页。
        p->records[chunk] = airi_memory_take_page(); // 记录页与用户数据页分别拥有、分别归还。
        if (!p->records[chunk]) // 元数据耗尽不能继续分配没有记录位置的数据页。
            return false; // 旧有效项和旧映射完全保持。
        airi_memset(airi_memory_direct(p->records[chunk]), 0, 4096); // 清除前一个所有者的地址记录。
    }
    uint64_t physical = airi_memory_take_page(); // 取得真正归本上下文持有的物理页。
    if (!physical) { // 可能刚借出记录页，却还未增加有效数据项。
        trim_records(p); // 收回本次没有承载活项的新记录页。
        return false; // 调用方再回滚本批此前成功的数据页。
    }
    airi_memset(airi_memory_direct(physical), 0, 4096); // 文件尾部与BSS默认都从零开始。
    if (!airi_vm_map(&p->space, address, physical, flags | AIRI_VM_USER)) { // 单页映射失败还没有发布到所有权数组。
        airi_memory_return_page(physical); // 明确归还本次未接入数据页。
        trim_records(p); // 映射失败也不能遗留空元数据页。
        return false; // 调用者回滚此前成功的部分。
    }
    *owned_at(p, p->count) = (struct user_page){address, physical, flags | AIRI_VM_USER}; // 保存VA/PA/权限供fork复制与精确回收。
    ++p->count; // 记录完整后才发布有效项；指针不跨记录页释放保留。
    return true; // 每个已登记数据页都有对应映射。
}
static int prepare_elf(struct user_process *p, const uint8_t *image, size_t bytes, const char *name, bool make_stack) { // 空目标只拥有新用户资源；exec不另建正在使用的内核栈。
```

普通 ELF 从原栈顶向下建立十六页，段与栈共用新预算。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
            done += part; // 下一次继续剩余文件内容。
        }
    }
    if (!owned_page(p, USER_STACK, AIRI_VM_WRITE)) // 栈单独RW/NX，不采用ELF可执行栈请求。
        goto failed; // 失败仍统一回滚。
    initial_frame(p, view.entry, name); // 用户_start接收最小argc/argv协议。
    airi_log("[ELF_LOAD] entry=0x%llx segments=%u data_pages=%u argc=1", (unsigned long long)view.entry, view.count, p->count); // 不含页表与内核栈的用户数据页数量。
    return 0; // 只准备新空间，尚未切CR3或覆盖旧现场。
```
<!-- after -->
```c
            done += part; // 下一次继续剩余文件内容。
        }
    }
    for (unsigned i = 0; i < AIRI_USER_STACK_PAGES; ++i) // 最高页地址不变，向低地址增加十五页。
        if (!owned_page(p, USER_STACK - i * 4096ull, AIRI_VM_WRITE)) // 栈RW/NX，最低页之下仍无映射。
            goto failed; // 任意一页失败均回收整份新映像。
    initial_frame(p, view.entry, name); // 用户_start接收最小argc/argv协议。
    airi_log("[ELF_LOAD] entry=0x%llx segments=%u data_pages=%u argc=1", (unsigned long long)view.entry, view.count, p->count); // 不含页表与内核栈的用户数据页数量。
    return 0; // 只准备新空间，尚未切CR3或覆盖旧现场。
```

旧 flat 故障映像仍是一页，用 owned_at 取得该页的物理记录。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
        return false; // 栈创建失败已回滚。
    if (!airi_memory_user_space(&p->space) || !owned_page(p, USER_CODE, AIRI_VM_EXEC) || !owned_page(p, USER_STACK, AIRI_VM_WRITE)) // 分别拥有根、代码和栈。
        goto failed; // 所有权列表可清理全部已成功部分。
    airi_memcpy(airi_memory_direct(p->owned[0].physical), image, bytes); // 映像内引用保持旧固定布局。
    initial_frame(p, USER_CODE, NULL); // 受控汇编不读取argc/argv。
    return true; // 尚未设置可运行状态。
failed: // 目标从未被调度，当前根和栈均不属于它。
```
<!-- after -->
```c
        return false; // 栈创建失败已回滚。
    if (!airi_memory_user_space(&p->space) || !owned_page(p, USER_CODE, AIRI_VM_EXEC) || !owned_page(p, USER_STACK, AIRI_VM_WRITE)) // 分别拥有根、代码和栈。
        goto failed; // 所有权列表可清理全部已成功部分。
    airi_memcpy(airi_memory_direct(owned_at(p, 0)->physical), image, bytes); // 映像内引用保持旧固定布局。
    initial_frame(p, USER_CODE, NULL); // 受控汇编不读取argc/argv。
    return true; // 尚未设置可运行状态。
failed: // 目标从未被调度，当前根和栈均不属于它。
```

<a id="capacity-transfer"></a>
## fork 复制数据，exec 转移记录的所有权

fork 不能复制父 records 中的七个地址当作孩子的记录页，否则任一方退出都会让另一方继续读取已经归还的记录。它逐项借父记录，只消费其中的 VA、PA 和权限，通过 `owned_page(child, ...)` 建立孩子自己的数据页和记录页，再复制 4096 字节内容。父进程原记录不会被改变，失败只清理尚未发布的孩子。

动态申请先检查整批是否超过 `1024 - current->count`，再按 K35 的首次适配寻找连续虚拟洞。单次请求上限从四页扩大为六十四页；其余地址布局不变。分配中途失败时逆序解除本批新增映射、归还数据页，最后裁掉空记录页，旧页保持。释放则先完整验证用户请求，再逐页解除，末项补洞后统一裁尾；不能在复制末项之前释放末项所在记录页。

下面在 fork、动态页申请和归还中替换旧数组下标，wait 继续调用统一 cleanup。`owned_at` 返回指针只在本轮内核操作有效；count 仍是未找到项的哨兵，不能直接作为有效元素读取。

fork 从父记录页读取每个 VA/PA/权限，孩子逐页取得自己的数据与记录，不共享记录页。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    if (!create_stack(child) || !airi_memory_user_space(&child->space)) // 先创建独立内核栈和空低半区。
        goto failed; // 父状态从未改变。
    for (unsigned i = 0; i < parent->count; ++i) { // 包括用户栈和此前申请的堆页。
        const struct user_page *page = &parent->owned[i]; // 每页原VA、权限和物理地址。
        if (!owned_page(child, page->address, page->flags)) // 子页在相同VA，但物理所有权独立。
            goto failed; // 回滚全部子资源，不改变父页面。
        airi_memcpy(airi_memory_direct(child->owned[child->count - 1].physical), airi_memory_direct(page->physical), 4096); // 复制完整页内容，父子以后写入互不影响。
    }
    airi_fd_clone(&child->files, &parent->files); // 页复制全部成功后再取得FD引用，失败cleanup仍能撤销。
    child->initial = *parent_frame; // 用户RIP/RSP和寄存器从同一次fork返回点继续。
```
<!-- after -->
```c
    if (!create_stack(child) || !airi_memory_user_space(&child->space)) // 先创建独立内核栈和空低半区。
        goto failed; // 父状态从未改变。
    for (unsigned i = 0; i < parent->count; ++i) { // 包括用户栈和此前申请的堆页。
        const struct user_page *page = owned_at(parent, i); // 每页原VA、权限和物理地址。
        if (!owned_page(child, page->address, page->flags)) // 子页在相同VA，但物理所有权独立。
            goto failed; // 回滚全部子资源，不改变父页面。
        airi_memcpy(airi_memory_direct(owned_at(child, child->count - 1)->physical), airi_memory_direct(page->physical), 4096); // 复制完整页内容，父子以后写入互不影响。
    }
    airi_fd_clone(&child->files, &parent->files); // 页复制全部成功后再取得FD引用，失败cleanup仍能撤销。
    child->initial = *parent_frame; // 用户RIP/RSP和寄存器从同一次fork返回点继续。
```

动态申请采用新预算和单次上限，并通过 owned_at 搜索现有地址。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    return schedule(REASON_WAIT); // 孩子结束时填写此f->rax并唤醒父。
}
static long allocate_user(size_t bytes) { // 按页提供有界用户内存，不是完整malloc。
    if (!bytes || bytes > 16384) // 一次最多四页，防止无界请求。
        return -22; // -EINVAL。
    unsigned count = (unsigned)((bytes + 4095) / 4096); // 请求向上取整为完整页。
    if (count > 64 - current->count) // 保留所有权数组容量。
        return -12; // -ENOMEM。
    uint64_t address = USER_HEAP; // 从低端找能容纳本次整段的空洞，不依赖旧分配顺序。
    for (;;) { // 每次冲突越过至少一个已拥有页，最多被六十四页阻挡。
        bool conflict = false; // 本候选范围是否撞上现有用户数据页。
        for (unsigned i = 0; i < current->count; ++i) { // owned可能因释放而换序，不要求按VA排序。
            uint64_t used = current->owned[i].address; // 当前页仍由此进程拥有。
            if (used >= address && used < address + count * 4096ull) { // 页对齐后，页起点落入候选即发生重叠。
                address = used + 4096; // 此页之前的起点都不能容纳本次整段，跳到该页右端。
                conflict = true; // 新候选须重新从整个owned数组检查。
```
<!-- after -->
```c
    return schedule(REASON_WAIT); // 孩子结束时填写此f->rax并唤醒父。
}
static long allocate_user(size_t bytes) { // 按页提供有界用户内存，不是完整malloc。
    if (!bytes || bytes > AIRI_USER_REQUEST_PAGES * 4096ull) // 一次最多六十四页，取整加法保持有界。
        return -22; // -EINVAL。
    unsigned count = (unsigned)((bytes + 4095) / 4096); // 请求向上取整为完整页。
    if (count > AIRI_USER_PAGES - current->count) // 先检查整批有效项容量。
        return -12; // -ENOMEM。
    uint64_t address = USER_HEAP; // 从低端找能容纳本次整段的空洞，不依赖旧分配顺序。
    for (;;) { // 每次冲突越过至少一个已拥有页，搜索被1024项预算限制。
        bool conflict = false; // 本候选范围是否撞上现有用户数据页。
        for (unsigned i = 0; i < current->count; ++i) { // owned可能因释放而换序，不要求按VA排序。
            uint64_t used = owned_at(current, i)->address; // 当前页仍由此进程拥有。
            if (used >= address && used < address + count * 4096ull) { // 页对齐后，页起点落入候选即发生重叠。
                address = used + 4096; // 此页之前的起点都不能容纳本次整段，跳到该页右端。
                conflict = true; // 新候选须重新从整个owned数组检查。
```

回滚、查找与释放也改用记录助手。搬走末项以后才裁尾，不能跨裁尾保留记录指针。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    return (long)address; // 用户可显式归还完整页，未归还页仍在exit统一清理。
rollback: // 已有用户页面不受本次耗尽影响。
    while (current->count > before) { // 只处理新增的成功项。
        const struct user_page *page = &current->owned[--current->count]; // 先退到最后一项。
        if (!airi_vm_unmap(&current->space, page->address) || !airi_memory_return_page(page->physical)) // 活动根必须先断映射并失效，再归还。
            airi_panic("cannot roll back user page allocation"); // 内部所有权错误不能被吞掉。
    }
    return -12; // 不返回半段用户范围。
}
static unsigned owned_index(uint64_t address) { // 只在当前单线程进程的不可抢占系统调用中查询。
    for (unsigned i = 0; i < current->count; ++i) // 有效项之外可能有旧字节，不参与查找。
        if (current->owned[i].address == address) // 精确页起点，不接受页内指针。
            return i; // 下标只借给本次操作，不跨状态修改保存。
    return current->count; // count本身是未找到哨兵，不能作为数组下标解引用。
}
static long release_user(uint64_t address, uint64_t bytes) { // 按页解除动态用户映射，不是C malloc对象free。
    if (!bytes || bytes > 16384 || (bytes & 4095) || (address & 4095) || address < USER_HEAP || address >= USER_HEAP_END || bytes > USER_HEAP_END - address) // 短路先限地址再做右端减法。
        return -22; // -EINVAL；不舍入无效长度，不动任何页。
    unsigned count = (unsigned)(bytes / 4096); // 参数已限制为一到四个完整页。
    for (unsigned i = 0; i < count; ++i) { // 第一遍先验证整段，避免中途坏页留下部分解除。
        unsigned index = owned_index(address + i * 4096ull); // 当前目标VA属于动态页范围。
        if (index == current->count || current->owned[index].flags != (AIRI_VM_USER | AIRI_VM_WRITE)) // 既需真实所有权，也只接受RW/NX用户动态页。
            return -22; // 任何一页不存在，整段保持原样。
    }
    for (unsigned i = 0; i < count; ++i) { // 第二遍提交，当前没有其他线程改变本地址空间。
        unsigned index = owned_index(address + i * 4096ull); // 每次重新查，上一轮可能移动了owned末项。
        struct user_page page = current->owned[index]; // 在压缩数组前保留当前VA与PA数值。
        if (!airi_vm_unmap(&current->space, page.address) || !airi_memory_return_page(page.physical)) // 先断开活动根映射并invlpg，才归还物理页。
            airi_panic("cannot release owned dynamic user page"); // 内部不变量失败不能返回假成功。
        current->owned[index] = current->owned[--current->count]; // 用最后一项补空位，再缩小有效数量；顺序没有ABI含义。
    }
    airi_log("[USER_RELEASE] pid=%u address=0x%llx pages=%u", current->pid, (unsigned long long)address, count); // 只记录整段真正归还后的结果。
    return 0; // 后续分配可复用空洞，新物理页按原owned_page逻辑清零。
}
```
<!-- after -->
```c
    return (long)address; // 用户可显式归还完整页，未归还页仍在exit统一清理。
rollback: // 已有用户页面不受本次耗尽影响。
    while (current->count > before) { // 只处理新增的成功项。
        const struct user_page *page = owned_at(current, --current->count); // 先退到最后一项。
        if (!airi_vm_unmap(&current->space, page->address) || !airi_memory_return_page(page->physical)) // 活动根必须先断映射并失效，再归还。
            airi_panic("cannot roll back user page allocation"); // 内部所有权错误不能被吞掉。
    }
    trim_records(current); // 本批回滚后，尾部没有活项的记录页也一起归还。
    return -12; // 不返回半段用户范围。
}
static unsigned owned_index(uint64_t address) { // 只在当前单线程进程的不可抢占系统调用中查询。
    for (unsigned i = 0; i < current->count; ++i) // 有效项之外可能有旧字节，不参与查找。
        if (owned_at(current, i)->address == address) // 精确页起点，不接受页内指针。
            return i; // 下标只借给本次操作，不跨状态修改保存。
    return current->count; // count本身是未找到哨兵，不能作为数组下标解引用。
}
static long release_user(uint64_t address, uint64_t bytes) { // 按页解除动态用户映射，不是C malloc对象free。
    if (!bytes || bytes > AIRI_USER_REQUEST_PAGES * 4096ull || (bytes & 4095) || (address & 4095) || address < USER_HEAP || address >= USER_HEAP_END || bytes > USER_HEAP_END - address) // 短路先限地址再做右端减法。
        return -22; // -EINVAL；不舍入无效长度，不动任何页。
    unsigned count = (unsigned)(bytes / 4096); // 参数已限制为一到六十四个完整页。
    for (unsigned i = 0; i < count; ++i) { // 第一遍先验证整段，避免中途坏页留下部分解除。
        unsigned index = owned_index(address + i * 4096ull); // 当前目标VA属于动态页范围。
        if (index == current->count || owned_at(current, index)->flags != (AIRI_VM_USER | AIRI_VM_WRITE)) // 既需真实所有权，也只接受RW/NX用户动态页。
            return -22; // 任何一页不存在，整段保持原样。
    }
    for (unsigned i = 0; i < count; ++i) { // 第二遍提交，当前没有其他线程改变本地址空间。
        unsigned index = owned_index(address + i * 4096ull); // 每次重新查，上一轮可能移动了owned末项。
        struct user_page page = *owned_at(current, index); // 在压缩数组前保留当前VA与PA数值。
        if (!airi_vm_unmap(&current->space, page.address) || !airi_memory_return_page(page.physical)) // 先断开活动根映射并invlpg，才归还物理页。
            airi_panic("cannot release owned dynamic user page"); // 内部不变量失败不能返回假成功。
        *owned_at(current, index) = *owned_at(current, --current->count); // 末项可位于另一记录页，复制后才允许回收尾页。
    }
    trim_records(current); // 全段压缩结束，任何有效项都不再借用空尾页。
    airi_log("[USER_RELEASE] pid=%u address=0x%llx pages=%u", current->pid, (unsigned long long)address, count); // 只记录整段真正归还后的结果。
    return 0; // 后续分配可复用空洞，新物理页按原owned_page逻辑清零。
}
```

exec 的 next 与 old 都只保存小控制块。新 ELF 在 next 中独立准备，它的记录页与旧映像不同；若构造失败，cleanup(next) 回收新资源，当前 PID、地址空间、用户现场、FD 和旧 records 保持。

成功提交要让新旧两批页始终都有明确的清理者。`old = *current` 先保存旧控制块中的句柄与数值，当前进程槽暂时仍描述旧映像；候选 next 则持有准备好的新根、新数据页和新记录页。记录数组只有七个地址，复制这个小控制块不会复制用户数据。

进入提交后，next 接住仍在使用的内核陷入栈和普通 FD 表，old 中的对应句柄随即清空；旧异步轮次已经 reset，新的轮次交给提交后的 current。然后 `*current = next` 将新资源交入正式进程槽，切换 CR3，最后才 cleanup(old)，归还旧用户根、旧数据和旧记录页。清空已转移句柄，使这次清理不会带走正在执行的内核栈或继续使用的 FD。

局部 next 退出作用域不会自动销毁资源。提交后它的数值仍可能指向已交给 current 的同一批页，因此不能再对 next 调用 cleanup；新资源的后续清理由正式进程槽负责。

这段 exec 的执行语句无需修改：原来的事务提交继续转移小控制块，新的 cleanup 负责归还旧记录页。

已有用户返回检查、时钟调度、系统调用、launch 与命令入口也不需重敲。申请和归还仍用原调用号与参数，新容量不改变 ABI。launch 在整个有限会话结束后比较页池和内核堆空闲数；这次新增的记录页也必须回到页池，不能因为用户数据已经归还就忽略元数据泄漏。`run init` 的持续监督和前台 UART 策略保持 K36 行为。

更新用户库头 `labs/airios/include/airi_libc.h` 的归还范围说明。声明、调用号和包装实现不变；所有旧的一至四页请求继续合法。

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
#endif // 结束包含保护。
```

<a id="capacity-workload"></a>
## 同一个大程序填满、复制、替换，再归还

新建 `labs/airios/capacity.c`。这个用户程序先逐字节检查 384 KiB BSS 为零，再写入 0x31。随后先申请六十四页块，放不下时改为单页，把不足一块的余量也用完。它只记录实际返回的块地址和长度，不知道 ELF 实际占了几页；最后连一页也申请不到时，旧 BSS 与初始化字节仍须保持。

满预算后，它进入一个带 32768 字节 volatile 局部数组的函数。`volatile` 要求真实读写这些字节，`noinline` 保留独立函数帧，避免编译器将这个观察优化掉；它不保证并发同步。本函数和上层的块指针/长度数组都位于 64 KiB 用户栈内，合计仍有余量。写满局部数组后 fork，孩子先验证失败 exec 保留旧大映像，再 exec 成既有 worker 并返回 23；父 wait 后核对自己的 BSS 和整个局部数组。

每轮归还全部动态页，重复四次。最后再 fork 一个孩子，实际读取栈下保护地址，让 PF14 只结束孩子，父取得 −142 后继续运行。父进程仍保留自己的栈与数据，收到状态后才发布完成标记。

<!-- write: labs/airios/capacity.c -->
```c
#include <airi_libc.h> // 大映像仍只调用自有用户ABI，不借宿主运行时。
static volatile unsigned char data = 0x53; // 文件中的初始化字节与BSS分别检查。
static volatile unsigned char bss[384 * 1024]; // 超过旧六十四页总预算，ELF文件不保存这些零字节。
__attribute__((noinline)) static int stack_and_fork(void) { // 保留真实函数帧，不能把大数组优化掉。
    volatile unsigned char local[32768]; // 使用八页以上用户栈，仍小于固定64KiB范围。
    for (unsigned i = 0; i < sizeof(local); ++i) // 每个字节实际触发用户写入。
        local[i] = (unsigned char)(i ^ 0x5a); // 页内位置变化使内容检查不只看首字节。
    long child = airi_fork(); // 此时父拥有1024数据页，孩子复制全部七页元数据所描述的映射。
    if (child < 0) // 普通资源失败不能继续按孩子身份处理。
        return 151; // 交给shell保留精确错误。
    if (!child) { // 子与父同VA，但物理数据和记录页各自拥有。
        bss[0] = 0x99; // 改变子BSS不应影响父的0x31。
        if (airi_exec("broken") != -8 || bss[0] != 0x99 || local[12345] != (unsigned char)(12345 ^ 0x5a)) // 失败exec保持旧大映像与跨页栈。
            return 152; // 不把拒绝坏ELF误认为成功替换。
        airi_exec("worker"); // 成功时同PID进入worker，旧1024页和七个记录页归还。
        return 153; // 成功exec不能返回这里。
    }
    int status = 0; // worker的普通退出值仍由父wait取得。
    if (airi_waitpid(child, &status) != child || status != 23 || bss[0] != 0x31) // 子exec和exit不能释放父页面。
        return 154; // 身份、状态、父数据必须同时保持。
    for (unsigned i = 0; i < sizeof(local); ++i) // wait后仍沿原用户栈继续执行。
        if (local[i] != (unsigned char)(i ^ 0x5a)) // fork/exec内核操作不能覆盖用户栈内容。
            return 155; // 捕获栈映射或复制错误。
    return 0; // local的C对象寿命结束，页面仍属于初始用户栈。
}
static int fill_and_release(void) { // 一次完整耗尽与恢复，后面在同一进程重复。
    unsigned char *blocks[1024]; // 记录用户动态范围，不读取内核页预算或内部指针。
    unsigned sizes[1024]; // 每项保存本次实际申请长度，归还时保持原范围。
    unsigned count = 0, pages = 0; // 按成功返回累计，不假定ELF段页数。
    for (unsigned step = 64; step; step = step == 64 ? 1 : 0) { // 先用256KiB块，再用单页填满剩余不足一块的容量。
        while (count < 1024) { // 明确保护本地记录数组，内核耗尽通常更早到达。
            unsigned char *block = airi_alloc(step * 4096u); // 失败NULL不交付半个范围。
            if (!block) // 当前块大小放不下，转成下一种更小请求或结束。
                break; // 不把失败地址记录为活块。
            for (unsigned i = 0; i < step * 4096u; ++i) // 检查所有新字节，包括跨页部分。
                if (block[i]) // 重复轮次旧填充值必须被清零。
                    return 156; // 只在完整清零后使用。
            memset(block, 0xa6, step * 4096u); // 留下跨页可辨识内容。
            blocks[count] = block; // 有效用户VA仍由本进程拥有。
            sizes[count++] = step * 4096u; // 同一下标记录长度，再发布一项。
            pages += step; // 栈和ELF另占预算，不计入这里。
        }
    }
    if (pages < 512 || count == 1024 || airi_alloc(4096) || airi_alloc(0) || airi_alloc(262145)) // 至少2MiB动态页，最终单页也耗尽；零长和过大请求拒绝。
        return 157; // 不拿旧六十四页限制冒充扩容完成。
    if (data != 0x53 || bss[0] != 0x31 || bss[sizeof(bss) - 1] != 0x31) // 耗尽失败不能改变已有ELF数据。
        return 158; // 分配请求失败后旧进程应继续可用。
    int error = stack_and_fork(); // 在满预算与真实大用户栈上进入系统调用。
    if (error) // 保存具体失败位置，不继续掩盖。
        return error; // 由内核退出清理尚持有的块。
    while (count) { // 逆序释放全部动态页，期间会越过多个元数据页边界。
        --count; // 退到最后一项仍持有的范围。
        if (blocks[count][0] != 0xa6 || blocks[count][sizes[count] - 1] != 0xa6 || airi_release(blocks[count], sizes[count])) // 邻居内容与整段归还都须成立。
            return 159; // 不将有损回收记录为成功。
    }
    return 0; // ELF和栈仍活着，下一轮再次填满同一地址空间。
}
int main(void) { // shell正常fork/exec进入真实大BSS映像。
    if (data != 0x53) // 初始化段必须来自文件内容。
        return 160; // 不继续掩盖装载错误。
    for (unsigned i = 0; i < sizeof(bss); ++i) // 验证全部384KiB零填充。
        if (bss[i]) // 大BSS与文件长度不同，不能越界复制文件。
            return 161; // 不只检查第一个页面。
    for (unsigned i = 0; i < sizeof(bss); ++i) // 写入父映像长期保留的内容。
        bss[i] = 0x31; // 子写0x99之后父仍须看到0x31。
    for (unsigned round = 0; round < 4; ++round) { // 不退出进程，反复耗尽、fork、exec、归还。
        int error = fill_and_release(); // 记录页也随尾部有效项减少而归还。
        if (error) // 任一轮失败保留普通退出码。
            return error; // shell可继续报告，而非触发内核PANIC。
    }
    long child = airi_fork(); // 单独验证固定用户栈下面没有被顺手映射。
    if (child < 0) // 身份创建失败不能冒充页故障。
        return 162; // 明确返回失败位置。
    if (!child) // 故障局限于这个孩子，父继续wait。
        return *(volatile unsigned char *)(uintptr_t)0x6fff0000; // 64KiB栈低端之下的未映射保护页，真实用户PF。
    int status = 0; // 故障编码沿已有用户wait ABI交付。
    if (airi_waitpid(child, &status) != child || status != -142) // 真实PF14，而非读出某个宿主预期值。
        return 163; // 错误映射或缺少隔离都会暴露。
    airi_puts("[USER_CAPACITY_OK] bss=393216 stack=32768 rounds=4 full_pages=1024 exec=worker guard=pf14\n"); // 所有路径实际完成后才发布。
    return 0; // shell继续接收其他命令，整次launch结束还会核对物理页回收。
}
```

把新程序接入构建。在 Makefile 末尾追加以下规则；编译 .o 仍使用 K33 的普通用户 C 模式规则，链接只选择 capacity 的 main。包装对象必须等待本配置的 ELF，并用 target-specific CPPFLAGS 将路径交给 `.incbin`。

<!-- append: Makefile -->
```make

# 大映像容量观察复用用户库，但保持独立main。
$(OUT)/user/capacity.elf: $(USER_COMMON) $(OUT)/user/capacity.o labs/airios/program.ld
# BSS只占memsz，不能把零填充强塞进内核输入文件。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/capacity.o -o $@
# 包装必须等到本配置容量程序链接完成。
$(OUT)/labs/airios/images.o: $(OUT)/user/capacity.elf
# 原始ELF路径只由此对象的预处理变量消费。
$(OUT)/labs/airios/images.o: CPPFLAGS += -DAIRI_CAPACITY_ELF='"$(OUT)/user/capacity.elf"'
```

在 `labs/airios/images.S` 末尾追加输入边界。`.incbin` 包含完整 ELF 文件；BSS 由装载器根据程序头建立，不是包装汇编中的大零数组。内核目录中的 capacity 项已经在前面的 user.c 登记，文件与目录到这里才全部接通。

<!-- append: labs/airios/images.S -->
```asm

.section .rodata /* 大BSS程序仍只嵌入实际文件，不嵌入运行时零填充页。 */
.global airi_capacity_elf_start, airi_capacity_elf_end /* 新初始映像的输入边界。 */
airi_capacity_elf_start: /* ELF文件首。 */
.incbin AIRI_CAPACITY_ELF /* 当前配置刚链接完成的独立用户程序。 */
airi_capacity_elf_end: /* 文件半开右端。 */
```

<a id="capacity-observe"></a>
## 在串口里看同一个 PID 的大映像替换

从自己的 K36 目录完成上述修改后运行。Apple Silicon Mac 若默认 python3 仍是系统旧版本，可临时使用 `PATH=/opt/homebrew/bin:$PATH ./airios build` 与相同前缀的 run；这只影响当前命令的工具查找，不修改全局 shell 配置。

<!-- run -->
```sh
./airios build # 链接独立用户ELF与内核，再制作当前镜像。
```

```sh
./airios run # 在UART中输入下面的guest命令，Ctrl-A后X结束QEMU。
```

```text
run sh
capacity
hello
exit
```

本轮在 QEMU 中观察到 capacity 的初始装载有 115 个用户数据页，包含 ELF 的 99 页及十六个栈页；具体数量会随编译器和后续源码变化。每轮动态申请后，fork 日志必须给出 `copied_pages=1024`，worker 的成功提交给出 `old_pages=1024`。PID 来自实际运行，不固定为下面某个数：

```text
[FORK] parent=41 child=42 copied_pages=1024
[ELF_REJECTED] ELF header is missing or truncated
[EXEC_COMMIT] pid=42 program=worker old_pages=1024 new_pages=20
[EXEC_WORKER_OK] argv=worker heap=zeroed bss=new
```

失败 exec 没有产生 EXEC_COMMIT，孩子仍在原栈函数继续检查；成功 exec 后则从 worker 的 `_start` 进入新 main。父 wait 仍按原孩子 PID 取得 23。四轮完成后，栈保护观察给出实际 `vector=14 error=0x4 cr2=0x6fff0000`，表示用户读取未映射页；内核将它转成 −142 交给父程序。

```text
[USER_CAPACITY_OK] bss=393216 stack=32768 rounds=4 full_pages=1024 exec=worker guard=pf14
[SHELL_COMMAND_DONE] name=capacity code=0
```

随后 hello 正常执行，exit 返回内核监控台；launch 的前后资源核对才完整结束。正常执行中若缺少记录页，owned_at 会报告内核不变量错误并停机；用户预算耗尽只返回 NULL，不停止系统。物理页分配或映射的部分失败仍走回滚，本节没有人为耗尽整机 RAM，相关代码核对与本次实际耗尽的每进程预算应区分。

需要查看控制块的实际大小时，在构建机用本轮 LLVM 路径执行以下只读命令：

```sh
/opt/homebrew/opt/llvm/bin/llvm-dwarfdump --name=user_process build/normal/airios.elf # DWARF调试信息中查看DW_AT_byte_size。
```

本轮得到 `0x0300`，即 768 字节。这个值描述内核结构布局，不含它借出的记录页、用户数据页和内核栈。Linux 主机若使用其他 LLVM 安装路径，运行该安装中的同名工具即可；guest 本身不依赖这个调试工具。

<a id="capacity-author"></a>
## 把容量边界接入已有验证入口

主机 ELF 夹具增加两个相邻输入：一代码页加 1007 个数据页应接受，再增加一个数据页应拒绝且保持旧 view。夹具的文件只有少量字节，数据段的巨大零尾部只由 memsz 描述。更新 `tests/unit/elf_test.c`；原来各项损坏输入继续保留。

<!-- write: tests/unit/elf_test.c -->
```c
#include <assert.h> // 作者的正常与损坏ELF观察。
#include <stdio.h> // 宿主只输出结果，不运行用户指令。
#include <airi/elf.h> // 同一段检查算法。
static uint8_t image[0x3000]; // 完整受控输入，不借真实可执行文件的偶然布局。
static void put(unsigned offset, uint64_t value, unsigned bytes) { // 按ELF小端编码原始字段，与解析器结构读取独立。
    for (unsigned i = 0; i < bytes; ++i) // 只写该字段实际宽度。
        image[offset + i] = (uint8_t)(value >> (i * 8)); // 第i个字节取数值的相应低位。
}
static void fixture(void) { // 每个失败只改变一个关键事实。
    airi_memset(image, 0, sizeof(image)); // 清掉上一轮破坏。
    image[0] = 0x7f; // ELF魔数的首字节不是可打印ASCII。
    image[1] = 'E'; // 魔数第二字节。
    image[2] = 'L'; // 魔数第三字节。
    image[3] = 'F'; // 魔数第四字节。
    image[4] = 2; // ELFCLASS64。
    image[5] = 1; // ELFDATA2LSB小端。
    image[6] = 1; // 标识版本1。
    put(16, 2, 2); // e_type=ET_EXEC占两字节。
    put(18, 62, 2); // e_machine=EM_X86_64。
    put(20, 1, 4); // e_version=1占四字节。
    put(24, 0x400000, 8); // e_entry位于第一段的文件内容。
    put(32, 64, 8); // e_phoff紧随文件头。
    put(52, 64, 2); // e_ehsize为64。
    put(54, 56, 2); // e_phentsize为56。
    put(56, 2, 2); // e_phnum只含两个LOAD。
    put(64, 1, 4); // 第一项p_type=PT_LOAD。
    put(68, 5, 4); // 第一项PF_R|PF_X。
    put(72, 0x1000, 8); // 第一段文件字节从4096开始。
    put(80, 0x400000, 8); // 第一段用户虚拟地址。
    put(88, UINT64_MAX, 8); // p_paddr故意无意义，装载必须忽略它。
    put(96, 8, 8); // 第一段文件内容八字节。
    put(104, 4096, 8); // 第一段内存占一页。
    put(112, 4096, 8); // p_align为4096。
    put(120, 1, 4); // 第二项从64+56开始，也是LOAD。
    put(124, 6, 4); // 数据段PF_R|PF_W且没有X。
    put(128, 0x2000, 8); // 数据文件偏移8192。
    put(136, 0x401000, 8); // 数据段紧随代码页，边缘不共享。
    put(152, 4, 8); // 数据段filesz四字节。
    put(160, 8192, 8); // memsz两页，剩余跨度应清零。
    put(168, 4096, 8); // 数据段同样页对齐。
    image[0x1000] = 0xc3; // 代码字节仅作输入，不在宿主执行。
    image[0x2000] = 0x41; // 数据初值不等于零。
}
static void rejected(size_t bytes) { // 失败不能把局部接受结果写到调用者输出。
    struct airi_elf_view view; // 原输出故意包含可识别旧状态。
    uint8_t before[sizeof(view)]; // 字节快照包含对象表示，用于核对完全未修改。
    airi_memset(&view, 0xa5, sizeof(view)); // 不解引用其中的哨兵指针。
    airi_memcpy(before, &view, sizeof(view)); // 按字节记录真实原状态。
    assert(airi_elf_inspect(image, bytes, &view)); // 必须给出具体静态错误原因。
    assert(!airi_memcmp(before, &view, sizeof(view))); // 不能发布半份解析结果。
}
static void corrupt(unsigned offset, uint64_t value, unsigned bytes) { // 每种破坏都从同一个有效输入重新开始。
    fixture(); // 清掉其他破坏，避免前一次错误掩盖当前分支。
    put(offset, value, bytes); // 只修改这一种字段事实。
    rejected(sizeof(image)); // 要求拒绝并保持旧view不变。
}
int main(void) { // 本程序只验证解析器，不替代guest的BSS和权限运行。
    fixture(); // 两段、三数据页、有效入口。
    struct airi_elf_view view; // 接收完整已验证事实。
    assert(!airi_elf_inspect(image, sizeof(image), &view)); // 正常输入应通过。
    assert(view.count == 2 && view.pages == 3 && view.entry == 0x400000); // 页预算与段数量来自实际向外舍入。
    assert(view.segments[1].file_bytes == 4 && view.segments[1].memory_bytes == 8192); // 文件和内存长度明确区分。
    rejected(63); // 不完整文件头。
    corrupt(5, 2, 1); // 大端输入当前不支持。
    corrupt(18, 183, 2); // AArch64不是当前用户CPU目标。
    corrupt(32, sizeof(image)-8, 8); // 程序头表截断。
    corrupt(56, 17, 2); // 程序头数量超过固定界限。
    corrupt(96, 4097, 8); // filesz大于memsz。
    corrupt(72, UINT64_MAX-3, 8); // 文件范围加法回绕不能绕过边界。
    corrupt(80, 0xffffffff80000000ull, 8); // 内核高地址不能作为用户段。
    corrupt(68, 7, 4); // RWX不被本装载策略接受。
    corrupt(112, 3, 8); // 对齐不是2的幂。
    corrupt(136, 0x400000, 8); // 两段共享同一页必须拒绝。
    corrupt(24, 0x401000, 8); // 入口落入可写NX数据段。
    corrupt(64, 3, 4); // PT_INTERP不能被静默忽略。
    corrupt(64, 7, 4); // 未实现TLS也不能当普通静态程序。
    fixture(); // 大BSS只有memsz增长，文件本身不需要装下零填充字节。
    put(160, (AIRI_ELF_PAGES - 1ull) * 4096, 8); // 一代码页加其余数据页，刚好达到ELF预算。
    assert(!airi_elf_inspect(image, sizeof(image), &view) && view.pages == AIRI_ELF_PAGES); // 栈预留之后的精确上界仍可接受。
    put(160, AIRI_ELF_PAGES * 4096ull, 8); // 总段页数比预算多一页。
    rejected(sizeof(image)); // 失败保持上一份输出，不开始物理分配。
    puts("PASS elf: target, bounds, overflow, permissions, overlap, entry, budget, unchanged output"); // 所有失败与正常路径均成立。
    return 0; // 正常退出宿主观察。
}
```

更新 `scripts/shell_test.py`，在 memory 之后运行 capacity，再继续原来的二十四轮孤儿与输入回归。只匹配本次请求偏移之后的日志，要求四个满预算 fork、四个大旧映像 exec 与真实保护页故障；整条命令有原来的 35 秒截止，不能仅靠 QEMU 仍存活宣告成功。现有宿主驱动负责发送 UART 字节，所有内存内容检查仍在 guest 用户程序中执行。

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
                offset=len(text); wire.sendall(b'memory\n'); text=wait('[SHELL_COMMAND_DONE] name=memory code=0',offset) # 同一地址空间内归还并复用，不靠exit恢复内存。
                assert '[USER_MEMORY_REUSE_OK] rounds=128 holes=first-fit zeroed=1 invalid=unchanged' in text[offset:] # 新旧对象内容和失败原子性都由用户实际核对。
                offset=len(text); wire.sendall(b'capacity\n'); text=wait('[SHELL_COMMAND_DONE] name=capacity code=0',offset) # 大BSS、跨页用户栈、耗尽恢复与exec都来自guest。
                capacity_output=text[offset:] # 只使用本次请求后的完整输出。
                assert '[USER_CAPACITY_OK] bss=393216 stack=32768 rounds=4 full_pages=1024 exec=worker guard=pf14' in capacity_output # 用户实际逐字节核对后才发布。
                assert len(re.findall(r'\[FORK\].*copied_pages=1024',capacity_output))==4 # 四轮都触及最后一个元数据页。
                assert len(re.findall(r'\[EXEC_COMMIT\].*program=worker old_pages=1024',capacity_output))==4 # 大旧空间在成功exec提交后完整清理。
                assert 'cr2=0x6fff0000' in capacity_output # 用户栈下方保护页真正触发硬件异常。
                offset=len(text); wire.sendall(b'orphans\n'); text=wait('[SHELL_COMMAND_DONE] name=orphans code=0',offset) # 同一用户shell持续服务，不靠返回monitor收拢孙进程。
                orphan_output=text[offset:] # 只核对本条命令的身份记录。
                assert '[ORPHAN_USER_OK] rounds=24 live=12 zombie=12 session=alive' in orphan_output # 超过八槽容量仍能连续fork。
                assert len(re.findall(r'\[ORPHAN_REAP\] pid=\d+ released=1',orphan_output))==24 # 不是launch收尾清空表冒充托管。
                assert len(re.findall(r'\[ORPHAN_ADOPT\].*finished=1',orphan_output))==12 # 先成为僵尸再失去父的路径实际运行。
                assert len(re.findall(r'\[ORPHAN_EXIT\]',orphan_output))==12 # 活孤儿放行后正常退出。
                offset=len(text); wire.sendall(b'programs\n'); text=wait('[USER_SHELL_PROMPT]',offset) # 用户查询来自真实注册目录。
                assert 'demo\r\n' in text[offset:] and 'async-reset\r\n' in text[offset:] # 不是内核监控台的静态帮助。
                offset=len(text); wire.sendall(b'x'*80+b'\n'); text=wait('[USER_SHELL_PROMPT]',offset) # 过长行必须整体丢弃。
                assert 'line too long; discarded' in text[offset:] and '[FORK]' not in text[offset:] # 不执行截断前缀。
                offset=len(text); wire.sendall(b'hellx\x7fo\r\n'); text=wait('[SHELL_COMMAND_DONE] name=hello code=0',offset) # 退格修正与CRLF真正走用户输入。
                offset=len(text); wire.sendall(b'exit\n'); text=wait('[CONSOLE_UPDATE] view=0 commands=1 page=0',offset) # shell退出后内核监控台才返回。
                assert '[USER_SHELL_EXIT]' in text[offset:] and 'program sh exited 0' in text[offset:] # 真实收拢页/FD/静态池由launch核对。
                offset=len(text); wire.sendall(b'echo after-shell\n'); text=wait('[CONSOLE_UPDATE] view=0 commands=2 page=0',offset) # 回到内核后输入仍完整。
                assert 'after-shell' in text[offset:] # 不只是退出而丢了后续UART。
                result={'passed':True,'ring3_shell':True,'ram_file_and_device_readback':True,'other_user_during_cq_wait':True,'qmp_iops':1,'error_exec_survived':True,'returned_to_monitor':True,'orphan_rounds':24,'memory_reuse_rounds':128,'capacity_rounds':4,'capacity_pages':1024} # 只发布已观察事实。
                result_path.write_text(json.dumps(result,indent=2)+'\n'); print('PASS user shell: alloc/file/async wait/other task/CQ/readback, failed exec, return to monitor') # 阶段演示实际完成。
                return result # 总入口可收集。
            finally: # 包括断言、超时和QEMU错误。
                if wire is not None: wire.close() # 只关闭自己的设备连接。
                process.terminate() # 正常终止测试CPU，不称掉电。
                try: process.wait(timeout=5) # 清理也有期限。
                except subprocess.TimeoutExpired: process.kill(); process.wait() # 不遗留测试进程。
```

<!-- run -->
```sh
./airios test # 主机算法、启动与故障配置、输入、用户程序、init和存储均实际运行。
```

本轮完整 test 通过后，capacity 的页故障属于预期用户错误，内核不得产生 AIRI_PANIC；shell 退出后页池空闲数、堆空闲数和文件引用必须恢复。自动测试最后由宿主结束自己的虚拟机，这不属于 guest 掉电恢复实验。

<a id="capacity-linux"></a>
## 接回 Linux 中看到的内存现象

L02 已提供 mmap、触页、RSS 与缺页观察，L03 提供 fork/COW 与 exec 对照。它们在个人 Linux 主机上的编译运行仍未验证，本节不新增已经实测 Linux 的结论。AiriOS 这里会立即分配并清零申请的全部物理页，fork 也立即复制 4 MiB 数据；Linux 的虚拟映射、按需分配与 COW 不应由这些日志类推。后续内存压力与性能结论继续通过各自真实环境观察。

现在较大的静态缓冲区、跨页用户调用栈和可回收动态页已经能运行。4 MiB 仍是受控预算，尚不能据此宣称能够运行完整 Server-Lib、TLS 或 Cap’n Proto；那些组件还需要线程、TLS、浮点现场、libc 与 C++ 接口。P7 继续处理内核堆容量、正式启动与设备服务，图形支线仍不成为前置。

