# K23 · exec 先准备新映像，再保留 PID 提交替换

fork 已让孩子继续旧程序中的一条分支，但控制台以后要运行的是用户选定的另一份程序。重新 fork 并不能完成这项变化：孩子仍有旧代码、旧堆和旧返回位置。exec 因而保留这个孩子的身份，换掉它将执行的用户映像。这里最重要的是替换顺序：找不到程序、名称指针坏、ELF损坏或资源不够时，旧进程必须还能继续；只有新空间完整后，才切根并放弃旧用户页。成功之后PID和父子关系保持，用户RIP/RSP却从新_start和新栈开始。

本节的孩子先把堆值改99，调用缺失名称得到−2，再调用八字节坏ELF得到−8，两次都核对旧值与PID不变。随后exec worker，成功不返回旧调用；worker核对新argv、初值、BSS和零填充堆，再退出23，父仍用原孩子PID取得状态。串口同时接入programs和run NAME，坏文件或普通非零退出不能停止控制台。

保留 K22 的用户访问、fork、wait 和调度函数，本章只按下列旧新窗口更新实际连接：映像目录、exec 事务、launch 结果与系统命令服务。再写 worker 及构建输入。prepare_elf 的返回类型、make_stack 参数和调用点要一起更新，全部完成后运行。

<a id="exec-transaction"></a>
## 失败与提交分别发生在哪个空间

孩子传入 worker 时，名称字符串还在即将被替换的旧用户页中。若清掉旧页后才查名称，读取的已经是失效借用。exec 先把用户名称逐字节复制到 64 字节内核数组，检查每页可读和终止零，后面不再借用旧用户字符串。目录当前只有hello、processes、worker和明确损坏的broken，不是完整文件系统。未找到名称返回−2，未终止返回−36，坏指针返回−14；旧控制块和返回现场尚未改动。

名称已取得内核副本，旧程序仍在运行。此时需要试着建好新映像，同时保留失败后回到原调用点的可能。prepare_elf 现在返回 0、−8 或 −12，分别表示成功、格式不支持、资源失败。exec给它一个局部next所有者，并要求不另建内核栈；构造器只创建新的用户根、段、用户栈和initial寄存器值，不覆盖正在运行的旧f。所有失败都只清理next，返回旧用户系统调用的下一条指令。

next 已有完整段页、用户栈和初始寄存器以后，才进入提交。先把旧用户资源记在 old，让 next 接住原 PID、父 PID 和已运行计数，用户堆游标则重置为空。这个构造阶段没有另建内核栈，当前 exec 的 C 调用仍在旧栈上。

提交因而明确转移这组栈：`next.stack=old.stack` 后清掉 old.stack，旧资源清理者便不再拥有它。再把 next.initial 的值复制到实际 f，让 next.frame 指向 f，避免当前控制块最后借着局部 next.initial。发布 next 到原槽之后，身份未变，待恢复的用户 RIP/RSP 已指向新入口和新用户栈。

此时装入新 CR3，原内核栈仍由共享高半区映射支持，这次 C 调用可以继续。旧根已不活动，才 cleanup(old) 归还旧用户表和数据页；转移过的内核栈不在清理范围。最后公共返回消费同一 f，从新 _start 执行，旧 airi_exec 调用不再返回。硬件换根不变量若失败则停止内核，不能当普通装载错误回旧用户。

| 时点 | 活动用户根 | 当前内核栈 | 允许失败的结果 |
| --- | --- | --- | --- |
| 名称与ELF检查 | 旧根 | 原栈 | 返回错误，旧用户继续 |
| next完整准备 | 旧根 | 原栈 | 只回滚next |
| 提交与切CR3 | 新根 | 原栈转移给新所有者 | 只剩必须成立的内核不变量 |
| 清old、iretq | 新根 | 同一内核栈 | 从新_start执行，旧调用不返回 |

程序替换并不要求另起一个PID，也不要求重启CPU；正在完成exec的内核C调用仍可沿原内核栈结束，因为新根借用了同一高半区映射。改变的是用户映像和用户调用链。

<a id="launch-result"></a>
## 程序退出码与内核失败分开

另一个区别发生在外层调用者：worker 正常退出 23，并不意味着内核启动它失败。原 run_one 把固定演示与启动放在一起。现在launch负责完整准备、运行和回收，只返回“能否启动”的错误，并把程序退出码、tick和调度计数复制到user_result；普通程序返回23也合法。run_one只用于作者的固定启动演示，在这些演示不符合预期时才PANIC。串口命令使用launch，坏ELF得到−8，旧命令循环仍能处理下一条输入。

launch在成功和构造失败两条路径都核对页池与堆恢复。运行结束先复制必要数值，再清控制块，避免命令消费者拿着已回收进程指针。整个用户会话仍同步收拢，不提供并发命令客户端或通用远程协议。

**按以下确定窗口更新 `kernel/process/user.c`：** 保留K22的访问、fork、wait和调度函数；prepare_elf新增make_stack和状态返回，系统调用7调用exec_current，文件末尾新增目录命令消费者。

每块先列出唯一旧窗口，再给完整新文本；两块不能同时留在文件中。

依次找到以下旧窗口，各用对应新文本完整替换；窗口以外保留。

先让用户所有者能够把结果送回文本会话。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
#include <airi/elf.h> // 独立用户ELF检查器。
#include <airi/user.h> // 完整陷入与安全用户返回。
```
<!-- after -->
```c
#include <airi/console.h> // 内核命令消费服务结果，不依赖主题和绘图。
#include <airi/elf.h> // 独立用户ELF检查器。
#include <airi/user.h> // 完整陷入与安全用户返回。
```

继续处理下一处：

目录保存长期映像边界，find_program 只借用匹配项；worker 名称已经是内核副本，后面不再读取旧用户字符串。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
extern const uint8_t airi_hello_elf_start[], airi_hello_elf_end[]; // 既有独立用户C程序。
extern const uint8_t airi_process_elf_start[], airi_process_elf_end[]; // 新的fork/wait和抢占工作负载。
static unsigned slot(const struct user_process *p) { // 只接受processes中的真实槽，不接受临时复制对象。
    return (unsigned)(p - processes); // 同一数组内指针差得到稳定槽号。
```
<!-- after -->
```c
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
    size_t bytes = airi_strlen(name); // 只读当前有效字符串，不保存其指针。
    for (unsigned i = 0; i < sizeof(executables) / sizeof(executables[0]); ++i) // 遍历固定目录。
        if (airi_strlen(executables[i].name) == bytes && !airi_memcmp(executables[i].name, name, bytes)) // 长度与全部名称字节都相同才匹配。
            return &executables[i]; // 只借长期静态目录项。
    return NULL; // 未知名称不触碰任何用户地址空间。
}
static unsigned slot(const struct user_process *p) { // 只接受processes中的真实槽，不接受临时复制对象。
    return (unsigned)(p - processes); // 同一数组内指针差得到稳定槽号。
```

继续处理下一处：

先调整构造器合同：普通 launch 令 make_stack=true，exec 的 next 令它为 false。格式和资源错误分别返回负值。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    return true; // 每个已登记数据页都有对应映射。
}
static bool prepare_elf(struct user_process *p, const uint8_t *image, size_t bytes, const char *name) { // 当前还没有exec，先在空上下文中完成事务装载。
    struct airi_elf_view view; // 只借输入ELF直到复制结束。
    const char *error = airi_elf_inspect(image, bytes, &view); // 在任何资源分配前校验整个输入。
    if (error) { // 损坏格式不能触发后续内存写入。
        airi_log("[ELF_REJECTED] %s", error); // 给出精确范围/权限原因。
        return false; // 旧输出和当前空上下文都未改变。
    }
    if (!create_stack(p)) // 内核陷入栈先完成，再借高半区。
        return false; // 区域接口已回滚失败。
    if (!airi_memory_user_space(&p->space)) // 创建独立低半区。
        goto failed; // 后续失败均收回所有已持有资源。
```
<!-- after -->
```c
    return true; // 每个已登记数据页都有对应映射。
}
static int prepare_elf(struct user_process *p, const uint8_t *image, size_t bytes, const char *name, bool make_stack) { // 空目标只拥有新用户资源；exec不另建正在使用的内核栈。
    struct airi_elf_view view; // 只借输入ELF直到复制结束。
    const char *error = airi_elf_inspect(image, bytes, &view); // 在任何资源分配前校验整个输入。
    if (error) { // 损坏格式不能触发后续内存写入。
        airi_log("[ELF_REJECTED] %s", error); // 给出精确范围/权限原因。
        return -8; // -ENOEXEC，旧输出和调用者原上下文都未改变。
    }
    if (make_stack && !create_stack(p)) // 内核陷入栈先完成，再借高半区。
        return -12; // -ENOMEM，区域接口已回滚失败。
    if (!airi_memory_user_space(&p->space)) // 创建独立低半区。
        goto failed; // 后续失败均收回所有已持有资源。
```

继续处理下一处：

构造器的成功、回滚返回也同步采用0和负错误，调用方才能区分失败。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    initial_frame(p, view.entry, name); // 用户_start接收最小argc/argv协议。
    airi_log("[ELF_LOAD] entry=0x%llx segments=%u data_pages=%u argc=1", (unsigned long long)view.entry, view.count, p->count); // 不含页表与内核栈的用户数据页数量。
    return true; // 还未切CR3，运行入口统一由int129交接。
failed: // 长期根仍活动，用户空间从未开始执行。
    cleanup(p); // 页表、用户数据和内核栈全部回收。
    return false; // 不提供部分映像。
}
static void place_frame(struct user_process *p) { // 初始寄存器值完成后复制到自己拥有的内核栈。
```
<!-- after -->
```c
    initial_frame(p, view.entry, name); // 用户_start接收最小argc/argv协议。
    airi_log("[ELF_LOAD] entry=0x%llx segments=%u data_pages=%u argc=1", (unsigned long long)view.entry, view.count, p->count); // 不含页表与内核栈的用户数据页数量。
    return 0; // 只准备新空间，尚未切CR3或覆盖旧现场。
failed: // 候选根尚未激活，调用者原根与当前内核栈仍有效；候选用户映像尚未开始执行。
    cleanup(p); // 页表、用户数据和内核栈全部回收。
    return -12; // -ENOMEM，不提供部分映像。
}
static void place_frame(struct user_process *p) { // 初始寄存器值完成后复制到自己拥有的内核栈。
```

继续处理下一处：

构造边界已成立，在用户返回检查前加入 exec_current：先取得短名称，再准备 next，最后才执行上述栈转移与切根。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    return -12; // 不返回半段用户范围。
}
struct airi_trap_frame *airi_user_resume(struct airi_trap_frame *f) { // 所有用户iretq路径使用同一规范地址和标志策略。
    if (f->cs != 27 || f->ss != 35 || f->rip >= AIRI_USER_LIMIT || f->rsp >= AIRI_USER_LIMIT) { // 坏返回不能变成内核GP。
```
<!-- after -->
```c
    return -12; // 不返回半段用户范围。
}
static long exec_current(struct airi_trap_frame *f) { // 成功不返回旧用户调用，失败必须保持旧PID、空间和现场。
    char name[64]; // 先把用户名称复制到当前内核栈，后续不再借旧用户页。
    bool terminated = false; // 不能把未终止输入交给strlen。
    for (unsigned i = 0; i < sizeof(name); ++i) { // 有界逐字节读取，允许跨用户页但不越过上限。
        if (f->rdi > UINT64_MAX - i || !copy_user(current, &name[i], f->rdi + i, 1, false)) // 名称本身也可能是坏指针。
            return -14; // -EFAULT，旧用户继续。
        if (!name[i]) { // 取得完整NUL字符串后才查目录。
            terminated = true; // 后续strlen有明确终止界限。
            break; // 不读取字符串后无关字节。
        }
    }
    if (!terminated) // 超过本项目名称预算。
        return -36; // -ENAMETOOLONG。
    const struct executable *image = find_program(name); // 不存在目标不用分配新根。
    if (!image) // 和损坏格式是两种不同失败。
        return -2; // -ENOENT。
    struct user_process next = {0}; // 临时所有者不在进程表中，尚不能被调度。
    int error = prepare_elf(&next, image->start, (size_t)((uintptr_t)image->end - (uintptr_t)image->start), name, false); // 不创建第二条内核栈，先完整准备新用户空间。
    if (error) // 构造器已回滚next，current所有字段和旧f不变。
        return error; // -ENOEXEC或-ENOMEM。
    struct user_process old = *current; // 暂存旧用户资源，稍后在切离旧根之后归还。
    unsigned old_pages = old.count; // 归还前保留实际替换规模用于记录。
    next.pid = old.pid; // exec保持进程身份。
    next.parent = old.parent; // 父仍等待同一个孩子。
    next.user_ticks = old.user_ticks; // 同一身份的已运行计数不丢失。
    next.heap_next = USER_HEAP; // 新映像的动态用户页从空开始。
    next.stack = old.stack; // 明确转移正在使用的内核栈所有权，VA和物理内容都不变。
    airi_memset(&old.stack, 0, sizeof(old.stack)); // old清理者不再拥有这组栈，不能重复释放。
    next.frame = f; // 真实返回现场仍在同一内核栈上，不能留下指向next.initial局部对象的指针。
    *f = next.initial; // 事务提交时才覆盖旧用户RIP/RSP和寄存器。
    next.state = USER_RUNNING; // 此身份继续运行，但从新_start开始。
    *current = next; // 把新所有权发布到原进程槽，PID不变。
    if (!airi_vm_activate(&current->space)) // 新根已借到当前内核栈及内核映射。
        airi_panic("cannot activate committed exec image"); // 硬件不变量失败不能回旧用户继续。
    cleanup(&old); // 旧根已经不活动，旧数据可回收；当前内核栈已转移所以不释放。
    airi_log("[EXEC_COMMIT] pid=%u program=%s old_pages=%u new_pages=%u", current->pid, name, old_pages, current->count); // 名称是内核副本，不引用刚释放用户页。
    return 0; // 调用者直接恢复新f，不把这个0写回旧用户调用。
}
struct airi_trap_frame *airi_user_resume(struct airi_trap_frame *f) { // 所有用户iretq路径使用同一规范地址和标志策略。
    if (f->cs != 27 || f->ss != 35 || f->rip >= AIRI_USER_LIMIT || f->rsp >= AIRI_USER_LIMIT) { // 坏返回不能变成内核GP。
```

继续处理下一处：

系统调用7现在接到 exec_current；成功返回已改写的新 f，失败才把负错误交回旧 f。文件末尾的 launch 则独立收拢会话并复制结果，run_one继续只核对固定演示。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    else if (f->rax == 6) // fork复制所有用户页与返回现场。
        f->rax = (uint64_t)fork_current(f); // 父得到子PID，孩子自己的frame已经设为0。
    else if (f->rax == 8) // wait可直接回收或真的阻塞。
        return wait_current(f); // 结果恢复到父同一次调用之后。
    else if (f->rax == 9) // 申请零填充用户页。
        f->rax = (uint64_t)allocate_user((size_t)f->rdi); // 不成功不前移堆游标。
    else // 其他接口包括尚未实现的exec都明确拒绝。
        f->rax = (uint64_t)-38ll; // -ENOSYS。
    return airi_user_resume(f); // 普通调用继续同一用户，仍检查RSP与标志。
}
static void run_one(const uint8_t *image, size_t bytes, const char *name, int expected, bool elf, bool require_ticks, bool require_scheduler) { // 每次会话从一个根用户开始，可以产生孩子。
    struct airi_memory_stats before, after; // 包括所有子根、数据页和两种栈。
    airi_memory_snapshot(&before); // 会话前真实空闲基准。
    struct user_process *root = unused(); // 上次会话必须已经全部回收。
    if (!root) // 没有空槽就不覆盖旧身份。
        airi_panic("no slot for root user"); // 教学演示明确失败。
    bool prepared = elf ? prepare_elf(root, image, bytes, name) : prepare_flat(root, image, bytes); // 输入路径各有明确格式边界。
    if (!prepared) // 失败已经回滚本根资源。
        airi_panic("cannot prepare root user"); // 不启动半份映像。
    root->pid = next_pid++; // 完整资源之后分配身份。
    root->heap_next = USER_HEAP; // 动态用户页不与ELF静态段重叠。
    place_frame(root); // 初始值复制进实际内核返回栈。
    root->state = USER_RUNNABLE; // 直到此处才发布可运行者。
    root_pid = root->pid; // 会话结果只跟踪这个根身份。
    session_result = 0; // 不继承上一故障演示的负结果。
    timer_switches = 0; // 本次实际timer换身份次数。
    wait_blocks = 0; // 本次真实阻塞wait次数。
    current = root; // 内核int129将选择这个已准备根。
    long code; // 由启动现场RAX交回整个会话结果。
    __asm__ volatile("int $0x81" : "=a"(code) :: "memory", "cc"); // 所有用户结束后才回这条启动调用链。
    if (code != expected || (require_ticks && root->user_ticks < 2) || (expected == -142 && root->fault_error != 5) || (require_scheduler && (!timer_switches || !wait_blocks))) // 验证行为而非只要求QEMU存活。
        airi_panic("user session ended without expected process semantics"); // 抢占、阻塞或退出任意不符都失败。
    for (unsigned i = 0; i < PROCESS_COUNT; ++i) // 当前已回启动栈和长期根，可回收全部僵尸或孤儿。
        if (processes[i].state != USER_UNUSED) // 已在wait中回收的槽不重复释放。
            cleanup(&processes[i]); // 目标根不活动，目标内核栈也不再执行。
    current = NULL; // 不保留刚清掉的当前槽身份。
    kernel_return = NULL; // 下个会话重新登记启动现场。
    airi_memory_snapshot(&after); // 包括孩子资源的全部回收后取统计。
    if (before.pages_free != after.pages_free || before.heap_free != after.heap_free) // 临时页和共享路径空表都必须恢复。
        airi_panic("user process session leaked memory"); // 不把僵尸资源泄漏藏到下一阶段。
    if (require_scheduler) // 只为真正多用户工作负载报告抢占与阻塞结果。
        airi_log("[PROCESS_DEMO_OK] fork=copy wait=blocked timer_switches=%u wait_blocks=%u reclaimed=1", timer_switches, wait_blocks); // 计数来自实际IRQ和阻塞分支。
}
void airi_user_demo(void) { // 串口主线的真实用户边界与多进程回归。
```
<!-- after -->
```c
    else if (f->rax == 6) // fork复制所有用户页与返回现场。
        f->rax = (uint64_t)fork_current(f); // 父得到子PID，孩子自己的frame已经设为0。
    else if (f->rax == 7) { // exec失败保留旧现场，成功已把f替换成新入口。
        long result = exec_current(f); // 名称复制与新空间事务在不可抢占内核完成。
        if (result < 0) // 只有失败才向旧用户调用返回错误。
            f->rax = (uint64_t)result; // PID和旧地址空间仍保持。
        return airi_user_resume(f); // 成功恢复新_start，失败恢复旧调用下一条指令。
    } else if (f->rax == 8) // wait可直接回收或真的阻塞。
        return wait_current(f); // 结果恢复到父同一次调用之后。
    else if (f->rax == 9) // 申请零填充用户页。
        f->rax = (uint64_t)allocate_user((size_t)f->rdi); // 不成功不前移堆游标。
    else // 尚未提供的接口都明确拒绝。
        f->rax = (uint64_t)-38ll; // -ENOSYS。
    return airi_user_resume(f); // 普通调用继续同一用户，仍检查RSP与标志。
}
struct user_result { int code; unsigned ticks, switches, blocks; uint64_t fault_error; }; // 启动任务只取得数值，不借已回收控制块。
static int launch(const uint8_t *image, size_t bytes, const char *name, bool elf, struct user_result *result) { // 可用于启动演示或串口命令，程序失败不等于内核失败。
    if (session_active) // 当前只有一个由启动栈收拢的用户会话。
        return -16; // -EBUSY，不能覆盖已有kernel_return。
    struct airi_memory_stats before, after; // 包含根、所有孩子和内核栈。
    airi_memory_snapshot(&before); // 在任何资源分配前取基准。
    struct user_process *root = unused(); // 上次会话已回收的空槽。
    if (!root) // 仍有未回收身份就明确失败。
        return -11; // -EAGAIN。
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
    root->state = USER_RUNNABLE; // 发布首次运行资格。
    root_pid = root->pid; // 会话根身份不会被孩子exit覆盖。
    session_result = 0; // 不继承上次结果。
    timer_switches = 0; // 这次会话实际抢占计数。
    wait_blocks = 0; // 这次会话实际阻塞计数。
    current = root; // 内核启动门选择此根。
    long code; // 根结束结果由启动现场RAX交回。
    __asm__ volatile("int $0x81" : "=a"(code) :: "memory", "cc"); // 全部用户结束后才真正回启动栈。
    *result = (struct user_result){(int)code, root->user_ticks, timer_switches, wait_blocks, root->fault_error}; // 回收前复制必要数值。
    for (unsigned i = 0; i < PROCESS_COUNT; ++i) // 当前不在任何用户内核栈上。
        if (processes[i].state != USER_UNUSED) // wait已回收槽不重复处理。
            cleanup(&processes[i]); // 收拢剩余僵尸和未被父wait的资源。
    current = NULL; // 不保留已清掉的身份指针。
    kernel_return = NULL; // 下次会话重新登记。
    airi_memory_snapshot(&after); // 全部页和栈回收后验证。
    if (before.pages_free != after.pages_free || before.heap_free != after.heap_free) // 用户程序退出码可以任意，但内核资源必须恢复。
        airi_panic("user process session leaked memory"); // 不把泄漏当成应用错误跳过。
    return 0; // 表示已完整启动与收拢，程序结果另在result.code。
}
static void run_one(const uint8_t *image, size_t bytes, const char *name, int expected, bool elf, bool require_ticks, bool require_scheduler) { // 作者启动演示使用具体预期，不控制普通命令的合法退出码。
    struct user_result result; // 会话结束只消费数值副本。
    if (launch(image, bytes, name, elf, &result) || result.code != expected || (require_ticks && result.ticks < 2) || (expected == -142 && result.fault_error != 5) || (require_scheduler && (!result.switches || !result.blocks))) // 同时验证真正执行、故障、抢占和等待。
        airi_panic("user session ended without expected semantics"); // 固定演示有误才拒绝发布READY。
    if (require_scheduler) // 实际多用户会话单独报告行为证据。
        airi_log("[PROCESS_DEMO_OK] fork=copy exec=transaction wait=blocked timer_switches=%u wait_blocks=%u reclaimed=1", result.switches, result.blocks); // 计数来自真实IRQ与状态转换。
}
void airi_user_demo(void) { // 串口主线的真实用户边界与多进程回归。
```

继续处理下一处：

最后在 user.c 末尾接命令消费者。它借本次命令副本，launch结束后只把退出数值写回console，返回true结束这次分派。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    airi_log("[USER_DEMO_OK] ring=3 syscall=int80 bad_pointers=2 user_gp=13 user_pf=14 bad_rsp=rejected reclaimed=1"); // 完整会话结束，仍回串口命令。
}
```
<!-- after -->
```c
    airi_log("[USER_DEMO_OK] ring=3 syscall=int80 bad_pointers=2 user_gp=13 user_pf=14 bad_rsp=rejected reclaimed=1"); // 完整会话结束，仍回串口命令。
}

bool airi_user_command(struct airi_console *console, const char *command) { // 系统命令消费者独立于图形主题扩展。
    size_t bytes = airi_strlen(command); // 先取得有效长度，固定前缀比较不能越过短字符串。
    if (bytes == 8 && !airi_memcmp(command, "programs", 8)) { // 显示当前初始ELF目录。
        airi_console_print(console, "programs: hello processes worker broken"); // broken是明确损坏的演示输入。
        return true; // 已处理，不再交给图形扩展。
    }
    if (bytes < 4 || airi_memcmp(command, "run ", 4)) // 只接受明确的run空格前缀。
        return false; // 其他命令继续原分派链。
    const char *name = command + 4; // 只在本次调用期借控制台的命令副本。
    const struct executable *image = find_program(name); // 目前目录名不是通用文件系统路径。
    if (!image) { // 错别字不影响内核继续输入。
        airi_console_print(console, "program not found: %s", name); // 给出当前名称。
        return true; // 请求已得到明确结果。
    }
    struct user_result result; // 子进程都结束后取得数值退出状态。
    int error = launch(image->start, (size_t)((uintptr_t)image->end - (uintptr_t)image->start), name, true, &result); // 同步执行并完整回收，不借回调局部对象给用户。
    if (error) // 例如坏ELF返回-8，不是PANIC。
        airi_console_print(console, "cannot load %s: error=%d", name, error); // 串口详细日志还给出解析原因。
    else // 程序可以合法返回非零，例如worker的23。
        airi_console_print(console, "program %s exited %d", name, result.code); // 不把应用结果当成内核停止。
    return true; // GUI是否存在都不影响服务执行。
}
```

<a id="exec-worker"></a>
## 新程序验证用户状态真的替换了

提交成功后，不应继续访问旧孩子的堆，也不应回到 airi_exec 后一行。worker 是单独链接的 ELF，恢复的是它自己的 _start 与新用户栈。它要求argv[0]=worker，读取自己的初始化数据与4096字节BSS，再申请第一个用户页：地址要重新从0x10000000开始，内容必须为零，不能继承旧孩子在同一VA写的99。退出23仍交给等待同一PID的父进程。

旧孩子的两次失败检查完成后才输出EXEC_FAILURE_OK，说明旧C代码和堆仍可访问。成功调用后紧跟的return77是完整的错误出口，若真的回到旧用户代码，父收到77就会使启动演示失败。

**新建 `labs/airios/worker.c`。** 新ELF初值和BSS独立于被替换程序。

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
    return 23; // 父进程应仍通过原孩子PID取得这个状态。
}
```

**局部更新 `labs/airios/include/airi_libc.h`。** 新增airi_exec，成功不返回，失败可返回负数，所以它不声明_Noreturn。

依次找到以下旧窗口，各用对应新文本完整替换；窗口以外保留。

<!-- replace: labs/airios/include/airi_libc.h -->
<!-- before -->
```c
uint64_t airi_ticks(void); // 实际时钟事件计数。
void airi_yield(void); // 主动交接，不等同于时钟抢占。
#endif // 结束包含保护。
```
<!-- after -->
```c
uint64_t airi_ticks(void); // 实际时钟事件计数。
void airi_yield(void); // 主动交接，不等同于时钟抢占。
long airi_exec(const char *name); // 成功替换映像不返回；失败保留旧PID和调用链，返回负错误。
#endif // 结束包含保护。
```

**局部更新 `labs/airios/libc/syscall.c`。** 只新增编号7的用户包装，其余包装沿K22。

在 `labs/airios/libc/syscall.c` 末尾追加以下内容，已有定义保持：

<!-- append: labs/airios/libc/syscall.c -->
```c
long airi_exec(const char *name) { // 成功进入新_start，只有失败才返回这条旧调用链。
    return call3(7, (uintptr_t)name, 0, 0); // 当前只传程序名，argv由内核建立单项。
}
```

**局部更新 `labs/airios/processes.c`。** 孩子改为验证两个失败后真正exec worker，其余fork/抢占/表满回归保持。

依次找到以下旧窗口，各用对应新文本完整替换；窗口以外保留。

<!-- replace: labs/airios/processes.c -->
<!-- before -->
```c
        value[0] = 99; // 不能影响父页面。
        busy_ticks(5); // 给父进程机会进入真正阻塞的wait。
        airi_puts("[FORK_CHILD_OK] private_value=99\n"); // 真实用户输出。
        return 23; // 用户_start通过exit交回普通退出码。
    }
    const char *readonly = "read-only wait output"; // ELF只读段不能作为内核写结果的目标。
```
<!-- after -->
```c
        value[0] = 99; // 不能影响父页面。
        busy_ticks(5); // 给父进程机会进入真正阻塞的wait。
        long pid = airi_getpid(); // exec前保存孩子身份，不使用父分支返回的0冒充PID。
        if (airi_exec("missing") != -2 || value[0] != 99 || airi_getpid() != pid) // 未知名称不能改变旧映像或身份。
            return 75; // 失败原子性不符。
        if (airi_exec("broken") != -8 || value[0] != 99 || airi_getpid() != pid) // 截断ELF也必须保持旧用户内存和调用链。
            return 76; // 不能先销毁旧空间再发现格式错误。
        airi_puts("[EXEC_FAILURE_OK] old_image=alive pid=preserved\n"); // 两种失败后仍由旧ELF输出。
        airi_exec("worker"); // 成功只会从新worker_start继续，父仍等待同一个孩子PID。
        return 77; // 若回到这里，成功替换未发生。
    }
    const char *readonly = "read-only wait output"; // ELF只读段不能作为内核写结果的目标。
```

**局部更新 `labs/airios/images.S`。** worker有独立边界，broken是明确八字节截断输入。

在 `labs/airios/images.S` 末尾追加以下内容，已有定义保持：

<!-- append: labs/airios/images.S -->
```asm
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
```

**局部更新 `Makefile`。** 包装对象依赖三份本配置ELF，worker复用用户入口与库，自己的main单独链接。

依次找到以下旧窗口，各用对应新文本完整替换；窗口以外保留。

<!-- replace: Makefile -->
<!-- before -->
```make
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_OBJECTS) -o $@
# 包装汇编明确依赖本配置用户ELF，文件变动就重新嵌入。
$(OUT)/labs/airios/images.o: labs/airios/images.S $(OUT)/user/hello.elf $(OUT)/user/processes.elf
# 建立包装对象目录。
	@mkdir -p $(dir $@)
# 单引号保护路径宏内部双引号，.incbin只读本配置生成文件。
	$(CC) $(CPPFLAGS) $(CFLAGS) -DAIRI_HELLO_ELF='"$(OUT)/user/hello.elf"' -DAIRI_PROCESS_ELF='"$(OUT)/user/processes.elf"' -c $< -o $@

# 第二个用户程序只复用用户入口与库，不把hello的main再次链接进去。
```
<!-- after -->
```make
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_OBJECTS) -o $@
# 包装汇编明确依赖本配置用户ELF，文件变动就重新嵌入。
$(OUT)/labs/airios/images.o: labs/airios/images.S $(OUT)/user/hello.elf $(OUT)/user/processes.elf $(OUT)/user/worker.elf
# 建立包装对象目录。
	@mkdir -p $(dir $@)
# 单引号保护路径宏内部双引号，.incbin只读本配置生成文件。
	$(CC) $(CPPFLAGS) $(CFLAGS) -DAIRI_HELLO_ELF='"$(OUT)/user/hello.elf"' -DAIRI_PROCESS_ELF='"$(OUT)/user/processes.elf"' -DAIRI_WORKER_ELF='"$(OUT)/user/worker.elf"' -c $< -o $@

# 第二个用户程序只复用用户入口与库，不把hello的main再次链接进去。
```

继续处理下一处：

<!-- replace: Makefile -->
<!-- before -->
```make
# 入口与库复用，main只选择processes.o。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/processes.o -o $@
```
<!-- after -->
```make
# 入口与库复用，main只选择processes.o。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/processes.o -o $@

# exec目标独立编译，保持同一用户ABI与链接布局。
$(OUT)/user/worker.o: labs/airios/worker.c labs/airios/include/airi_libc.h
# 建立当前配置输出目录。
	@mkdir -p $(dir $@)
# 普通用户C仅链接自己的运行库。
	$(CC) $(USER_FLAGS) -Ilabs/airios/include -c $< -o $@
# worker也有自己的_start和main，原始ELF只在包装时进入内核。
$(OUT)/user/worker.elf: $(USER_COMMON) $(OUT)/user/worker.o labs/airios/program.ld
# 复用用户入口与库，不混合其他程序main。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/worker.o -o $@
```

<a id="system-command-consumer"></a>
## 文本服务先分派，图形扩展继续独立

这条运行路径接回 K09 的文本模型：输入 run worker 后，模型已经识别命令，但它不能自己持有进程页表。console 新增 service 函数指针，负责内核程序服务；原extension继续属于可选图形命令，例如theme。内置mem等查询之后先问service，未处理才问extension。main在console_init后绑定airi_user_command，图形初始化不能替换这条绑定。没有服务的宿主参考模型也不宣传programs/run，避免把模拟界面当成真实进程。

programs 列出初始目录。实际输入 `run worker` 时，service 收到本次整理后的命令副本，去掉四字节 `run ` 前缀得到 worker；目录项提供同一份 worker ELF 的起止边界，launch 建立新的根用户会话。它与孩子中的 `airi_exec("worker")` 分工不同：前者分配本次根 PID，后者沿原孩子 PID 替换映像。

launch 同步等完整会话结束，先把必要值复制到 result 再清控制块。worker 退出23时，launch 返回0表示启动与回收已完成，result.code=23才是应用结果；service 因而把 `program worker exited 23` 写进原文字环，再返回 true。坏 ELF 则让 launch 返回−8，service 写装载错误后同样结束本次命令，控制台可继续下一条输入。

文本模型只消费结果数值，不借进程对象。用户 stdout 当前直接走 UART，图形 Terminal 显示命令与结束摘要；完整图形 TTY 输出适配尚未接入。串口主线不需要 frontend、字体或 Cocoa。

**局部更新 `include/airi/user.h`。** 系统命令只借用文本会话，不暴露进程内部对象。

依次找到以下旧窗口，各用对应新文本完整替换；窗口以外保留。

<!-- replace: include/airi/user.h -->
<!-- before -->
```c
#ifndef AIRI_USER_H // 用户态边界声明一次。
#define AIRI_USER_H // 包含保护。
#include <airi/trap.h> // 系统调用和用户故障使用完整CPU现场。
void airi_user_demo(void); // 运行受控隔离演示与独立C用户ELF，结束后回收。
```
<!-- after -->
```c
#ifndef AIRI_USER_H // 用户态边界声明一次。
#define AIRI_USER_H // 包含保护。
#include <stdbool.h> // 系统命令回调明确报告是否处理。
#include <airi/trap.h> // 系统调用和用户故障使用完整CPU现场。
void airi_user_demo(void); // 运行受控隔离演示与独立C用户ELF，结束后回收。
```

继续处理下一处：

<!-- replace: include/airi/user.h -->
<!-- before -->
```c
struct airi_trap_frame *airi_user_timer(struct airi_trap_frame *frame); // 用户时间片在完整现场边界轮转，内核仍不可抢占。
struct airi_trap_frame *airi_user_resume(struct airi_trap_frame *frame); // 检查用户返回地址并清理不能交给iretq的标志。
#endif // 结束包含保护。
```
<!-- after -->
```c
struct airi_trap_frame *airi_user_timer(struct airi_trap_frame *frame); // 用户时间片在完整现场边界轮转，内核仍不可抢占。
struct airi_trap_frame *airi_user_resume(struct airi_trap_frame *frame); // 检查用户返回地址并清理不能交给iretq的标志。
struct airi_console; // 只借用文本会话，不依赖图形状态。
bool airi_user_command(struct airi_console *console, const char *command); // programs与run命令使用实际用户服务。
#endif // 结束包含保护。
```

**局部更新 `include/airi/console.h`。** service和图形extension分别保存。

依次找到以下旧窗口，各用对应新文本完整替换；窗口以外保留。

<!-- replace: include/airi/console.h -->
<!-- before -->
```c
/* Fixed storage owned by the kernel. No heap, process or filesystem dependency. */
struct airi_console {  // 独立保存文本输入、历史与输出环。
    bool (*extension)(struct airi_console *, const char *, void *);  // 可选命令消费者；true 表示已经处理，NULL 表示没有扩展。
    void (*read_memory)(struct airi_memory_stats *); // 可选同步查询；宿主纯文本观察可保持NULL。
```
<!-- after -->
```c
/* Fixed storage owned by the kernel. No heap, process or filesystem dependency. */
struct airi_console {  // 独立保存文本输入、历史与输出环。
    bool (*service)(struct airi_console *, const char *); // 内核服务先分派，GUI扩展不能替换它。
    bool (*extension)(struct airi_console *, const char *, void *);  // 可选命令消费者；true 表示已经处理，NULL 表示没有扩展。
    void (*read_memory)(struct airi_memory_stats *); // 可选同步查询；宿主纯文本观察可保持NULL。
```

**局部更新 `kernel/console/console.c`。** help按实际绑定显示程序服务，其余文本模型保持。

依次找到以下旧窗口，各用对应新文本完整替换；窗口以外保留。

<!-- replace: kernel/console/console.c -->
<!-- before -->
```c
}
static void system_info(struct airi_console *c) {  // 将启动身份和内核尺寸加入命令输出
    airi_console_print(c, "AiriOS 0.1 / x86_64 / bootstrap kernel");  // 将AiriOS 0.1 / x86_64 / bootstrap kernel保存为一条命令说明或身份记录
    airi_console_print(c, "Console: local    CPU: single    Boot: UEFI / %s", c->boot->loader);  // 把本次查询字段按Console: local    CPU: single    Boot: UEFI / %s的格式保存到输出环
    airi_console_print(c, "Kernel image: %llu KiB    Boot ABI: v%u",  // 把本次查询字段按Kernel image: %llu KiB    Boot ABI: v%u的格式保存到输出环
```
<!-- after -->
```c
}
static void system_info(struct airi_console *c) {  // 将启动身份和内核尺寸加入命令输出
    airi_console_print(c, "AiriOS 0.1 / x86_64 / learning kernel");  // 将AiriOS 0.1 / x86_64 / learning kernel保存为一条命令说明或身份记录
    airi_console_print(c, "Console: local    CPU: single    Boot: UEFI / %s", c->boot->loader);  // 把本次查询字段按Console: local    CPU: single    Boot: UEFI / %s的格式保存到输出环
    airi_console_print(c, "Kernel image: %llu KiB    Boot ABI: v%u",  // 把本次查询字段按Kernel image: %llu KiB    Boot ABI: v%u的格式保存到输出环
```

继续处理下一处：

<!-- replace: kernel/console/console.c -->
<!-- before -->
```c
        airi_console_print(c, "sys        Kernel and boot identity");  // 将sys        Kernel and boot identity保存为一条命令说明或身份记录
        airi_console_print(c, "mem        Usable / held physical memory summary");  // 将mem        Usable / held physical memory summary保存为一条命令说明或身份记录
        airi_console_print(c, "mem map    Complete physical address inventory");  // 将mem map    Complete physical address inventory保存为一条命令说明或身份记录
        airi_console_print(c, "devices    Registered console hardware endpoints");  // 将devices    Registered console hardware endpoints保存为一条命令说明或身份记录
```
<!-- after -->
```c
        airi_console_print(c, "sys        Kernel and boot identity");  // 将sys        Kernel and boot identity保存为一条命令说明或身份记录
        airi_console_print(c, "mem        Usable / held physical memory summary");  // 将mem        Usable / held physical memory summary保存为一条命令说明或身份记录
        if (c->service) // 只有绑定真实内核服务时才宣传可运行程序。
            airi_console_print(c, "programs / run NAME  Run a bundled user program"); // 主题与宿主参考模型不冒充进程服务。
        airi_console_print(c, "mem map    Complete physical address inventory");  // 将mem map    Complete physical address inventory保存为一条命令说明或身份记录
        airi_console_print(c, "devices    Registered console hardware endpoints");  // 将devices    Registered console hardware endpoints保存为一条命令说明或身份记录
```

继续处理下一处：

<!-- replace: kernel/console/console.c -->
<!-- before -->
```c
    else if (!airi_memcmp(start, "echo ", length < 5 ? length : 5) && length >= 5)  // 最多比较当前长度与5的较小值，再要求至少5字节，确认echo加空格前缀
        airi_console_print(c, "%s", start + 5);  // 略过echo和空格这5字节，将余部作为本条输出
    else if (c->extension && c->extension(c, start, c->extension_context)) {  // 同步调用可选消费者；它自己形成输出，不能保留局部命令指针。
        /* 扩展已处理本条命令，不再生成未知命令错误。 */
    } else  // 没有扩展或扩展不认识本命令时，保留原来的具体错误。
```
<!-- after -->
```c
    else if (!airi_memcmp(start, "echo ", length < 5 ? length : 5) && length >= 5)  // 最多比较当前长度与5的较小值，再要求至少5字节，确认echo加空格前缀
        airi_console_print(c, "%s", start + 5);  // 略过echo和空格这5字节，将余部作为本条输出
    else if (c->service && c->service(c, start)) { // 系统命令同步完成，结果写入同一文字环。
        /* Service owns this command; the optional graphical extension is separate. */
    } else if (c->extension && c->extension(c, start, c->extension_context)) {  // 同步调用可选消费者；它自己形成输出，不能保留局部命令指针。
        /* 扩展已处理本条命令，不再生成未知命令错误。 */
    } else  // 没有扩展或扩展不认识本命令时，保留原来的具体错误。
```

**局部更新 `kernel/main.c`。** 绑定真实服务，并在所有演示与console完成后发布AIRI_P3_READY。

依次找到以下旧窗口，各用对应新文本完整替换；窗口以外保留。

<!-- replace: kernel/main.c -->
<!-- before -->
```c
    airi_user_demo(); // 正常与非法用户程序结束后仍回到启动栈和串口。
    airi_console_init(&console, info);  // 初始化内核静态控制台，借用整个启动期间有效的 info。
    console.read_memory = airi_memory_snapshot; // 文本查询只读取数值，不持有分配器内部对象。
    bool graphics = airi_console_draw(&console);  // 实际完成首帧后取得布尔结果，不能仅凭描述存在认定成功。
```
<!-- after -->
```c
    airi_user_demo(); // 正常与非法用户程序结束后仍回到启动栈和串口。
    airi_console_init(&console, info);  // 初始化内核静态控制台，借用整个启动期间有效的 info。
    console.service = airi_user_command; // 系统程序服务独立于可选主题/窗口扩展。
    console.read_memory = airi_memory_snapshot; // 文本查询只读取数值，不持有分配器内部对象。
    bool graphics = airi_console_draw(&console);  // 实际完成首帧后取得布尔结果，不能仅凭描述存在认定成功。
```

继续处理下一处：

<!-- replace: kernel/main.c -->
<!-- before -->
```c
             (unsigned long long)usable);  // 完成记录附同一实际可用字节数。
    airi_log("[AIRI_P2_READY] exceptions=1 pages=1 own_cr3=1 heap=1"); // 所有内存演示和控制台初始化结束后才发布阶段完成。
    airi_serial_write("airi> ");  // 输出首个串口提示符。
    for (;;) {  // 没有退出条件，内核持续轮询输入。
```
<!-- after -->
```c
             (unsigned long long)usable);  // 完成记录附同一实际可用字节数。
    airi_log("[AIRI_P2_READY] exceptions=1 pages=1 own_cr3=1 heap=1"); // 所有内存演示和控制台初始化结束后才发布阶段完成。
    airi_log("[AIRI_P3_READY] user=ring3 elf=static fork=copy exec=transaction wait=blocked"); // 完整内核进程路径结束并恢复后才发布。
    airi_serial_write("airi> ");  // 输出首个串口提示符。
    for (;;) {  // 没有退出条件，内核持续轮询输入。
```

<a id="program-command-evidence"></a>
## 错误之后还要运行下一条命令

错误处理是否保住旧状态，要靠错误后的动作说明。坏 ELF 后仍能执行 echo，才证明控制台调用链没有被误当成用户失败一起终止。启动观察现在等待完整 P3 标记，并要求EXEC_FAILURE_OK、EXEC_COMMIT和EXEC_WORKER_OK都存在。输入观察也等P3完成后才发送UART字节，再实际运行hello、坏ELF、返回23的worker、完整processes以及不存在名称，最后echo证明输入循环仍正常。不能只检查首个程序能跑，忽略失败后的全局状态。

**局部更新 `scripts/boot.py`。** 旧P1/P2标记保留兼容，正常完成以P3为准。

依次找到以下旧窗口，各用对应新文本完整替换；窗口以外保留。

<!-- replace: scripts/boot.py -->
<!-- before -->
```python
    case = profile.removeprefix('desktop-')  # 图形前缀不改变故障注入含义。
    graphics = profile.startswith('desktop-') and case != 'no-framebuffer'  # 有绘图实现且设备存在才应生成画面。
    expected='[AIRI_PANIC] required Limine memory map is missing' if case=='missing-memory' else '[AIRI_P2_READY]'  # 正常配置等完整内存阶段，P1标记仍供早期输入观察。
    if case in ('trap-fatal', 'stack-guard'): # 未处理异常镜像必须进入具体停止路径。
        expected = '[AIRI_PANIC] unhandled CPU exception' # 不以 QEMU 存活推断故障已处理。
```
<!-- after -->
```python
    case = profile.removeprefix('desktop-')  # 图形前缀不改变故障注入含义。
    graphics = profile.startswith('desktop-') and case != 'no-framebuffer'  # 有绘图实现且设备存在才应生成画面。
    expected='[AIRI_PANIC] required Limine memory map is missing' if case=='missing-memory' else '[AIRI_P3_READY]'  # 正常配置等完整进程阶段，旧阶段标记只保留兼容。
    if case in ('trap-fatal', 'stack-guard'): # 未处理异常镜像必须进入具体停止路径。
        expected = '[AIRI_PANIC] unhandled CPU exception' # 不以 QEMU 存活推断故障已处理。
```

继续处理下一处：

<!-- replace: scripts/boot.py -->
<!-- before -->
```python
                        assert '[AIRI_P1_READY]' not in text  # 错误路径不得出现成功标记。
                    else:  # 按当前profile的另一条完整验证路径处理。
                        assert '[PROCESS_FORK_WAIT_OK]' in text and '[PROCESS_DEMO_OK]' in text # 复制、真实阻塞、timer换身份和回收都须完成。
                        assert '[ELF_LOAD]' in text and '[ELF_USER_OK]' in text # 独立ELF段、BSS与用户libc必须真正运行。
```
<!-- after -->
```python
                        assert '[AIRI_P1_READY]' not in text  # 错误路径不得出现成功标记。
                    else:  # 按当前profile的另一条完整验证路径处理。
                        assert '[EXEC_FAILURE_OK]' in text and '[EXEC_WORKER_OK]' in text and '[EXEC_COMMIT]' in text # 坏映像保持与成功替换都须实际运行。
                        assert '[PROCESS_FORK_WAIT_OK]' in text and '[PROCESS_DEMO_OK]' in text # 复制、真实阻塞、timer换身份和回收都须完成。
                        assert '[ELF_LOAD]' in text and '[ELF_USER_OK]' in text # 独立ELF段、BSS与用户libc必须真正运行。
```

**局部更新 `scripts/console_test.py`。** 结果必须出现在当前UART命令之后，坏应用不能出现AIRI_PANIC。

依次找到以下旧窗口，各用对应新文本完整替换；窗口以外保留。

<!-- replace: scripts/console_test.py -->
<!-- before -->
```python
                        record=line.removeprefix('airi> ').removesuffix('\n').removesuffix('\r')  # 只移除已知提示符与CRLF，保留字段本身。
                        matches=record==marker  # CONSOLE_UPDATE要求完整等值，page=1不会匹配page=10。
                        if marker=='[AIRI_P1_READY]':  # READY后还有graphics与内存字段，按标记后的空格确定边界。
                            matches=record.startswith(marker+' ')  # 接受带字段的完整READY行，不接受相似标记名称。
                        if matches:  # 本次输出中已取得完整且匹配的状态记录。
```
<!-- after -->
```python
                        record=line.removeprefix('airi> ').removesuffix('\n').removesuffix('\r')  # 只移除已知提示符与CRLF，保留字段本身。
                        matches=record==marker  # CONSOLE_UPDATE要求完整等值，page=1不会匹配page=10。
                        if marker in ('[AIRI_P1_READY]', '[AIRI_P3_READY]'):  # 阶段READY后还有机制字段，按标记后的空格确定边界。
                            matches=record.startswith(marker+' ')  # 接受带字段的完整READY行，不接受相似标记名称。
                        if matches:  # 本次输出中已取得完整且匹配的状态记录。
```

继续处理下一处：

<!-- replace: scripts/console_test.py -->
<!-- before -->
```python
                    time.sleep(.05)  # 暂停50毫秒给设备或日志处理时间，再重复本次检查。
                raise RuntimeError(f'Console timeout waiting for {marker}; see {serial}')  # 明确给出缺少的标记和已有日志路径。
            wait('[AIRI_P1_READY]',timeout=35)  # 输入前先等内核初始化与首帧结束，最多35秒。
            def key(*codes):  # 形参*把多个键名收集成元组，可同时发送ctrl或shift组合。
                qmp_execute(qmp,'send-key',{'keys':[{'type':'qcode','data':c} for c in codes],'hold-time':20})  # 每个键名转qcode字典，列表作为keys，按住时间单位毫秒。
```
<!-- after -->
```python
                    time.sleep(.05)  # 暂停50毫秒给设备或日志处理时间，再重复本次检查。
                raise RuntimeError(f'Console timeout waiting for {marker}; see {serial}')  # 明确给出缺少的标记和已有日志路径。
            wait('[AIRI_P3_READY]',timeout=35)  # 输入前先等内核初始化与首帧结束，最多35秒。
            def key(*codes):  # 形参*把多个键名收集成元组，可同时发送ctrl或shift组合。
                qmp_execute(qmp,'send-key',{'keys':[{'type':'qcode','data':c} for c in codes],'hold-time':20})  # 每个键名转qcode字典，列表作为keys，按住时间单位毫秒。
```

继续处理下一处：

<!-- replace: scripts/console_test.py -->
<!-- before -->
```python
                tail=wait(update(0,4),offset) # 等第四次提交的完整状态。
                assert 'page pool: total=' in tail and 'kernel heap: bytes=65536' in tail # 不能只返回启动地图。
                cases=['UART receive / command dispatch','serial-only device report']  # 结果只记录此路径实际运行的两项观察。
            else:  # 按当前profile的另一条完整验证路径处理。
                capture(work/'terminal.ppm')  # READY已到达后保存初始终端画面。
```
<!-- after -->
```python
                tail=wait(update(0,4),offset) # 等第四次提交的完整状态。
                assert 'page pool: total=' in tail and 'kernel heap: bytes=65536' in tail # 不能只返回启动地图。
                for number, text, expected in [(5, 'programs', 'programs: hello processes worker broken'), (6, 'run hello', 'program hello exited 0'), (7, 'run broken', 'cannot load broken: error=-8'), (8, 'run worker', 'program worker exited 23'), (9, 'run processes', 'program processes exited 0'), (10, 'run missing', 'program not found: missing'), (11, 'echo after-programs', 'after-programs')]: # 正常、坏ELF、非零退出和未知名称都不破坏后续会话。
                    offset=len(output()) # 当前命令只消费新日志。
                    wire.sendall((text + '\n').encode()) # 发送真正UART输入，而不是直接调用内核函数。
                    tail=wait(update(0,number),offset) # 程序会话全部收拢后才完成控制台命令。
                    assert expected in tail # 核对当前请求结果，不能复用旧标记。
                    assert '[AIRI_PANIC]' not in tail # 应用格式或退出错误不能停掉内核。
                cases=['UART receive / command dispatch' ,'serial-only device report']  # 本列表仍用 UART 与纯串口两类标签；上方还核对七条程序相关命令。
            else:  # 按当前profile的另一条完整验证路径处理。
                capture(work/'terminal.ppm')  # READY已到达后保存初始终端画面。
```

继续处理下一处：

<!-- replace: scripts/console_test.py -->
<!-- before -->
```python
                key('ret')   # 提交修复后的命令。
                commands+=1  # 本次实际提交也计入预期数。
                assert 'bootstrap kernel' in wait(update(0,commands),offset)  # 修复后的sys命令应报告当前内核身份。
                offset=len(output())  # 先记录旧文本长度，后续只消费它之后的完成与结果。
                for c in 'echo ':     # 逐个字符准备echo前缀。
```
<!-- after -->
```python
                key('ret')   # 提交修复后的命令。
                commands+=1  # 本次实际提交也计入预期数。
                assert 'learning kernel' in wait(update(0,commands),offset)  # 修复后的sys命令应报告当前内核身份。
                offset=len(output())  # 先记录旧文本长度，后续只消费它之后的完成与结果。
                for c in 'echo ':     # 逐个字符准备echo前缀。
```

<!-- run -->
```sh
./airios test  # 六个启动配置与真实输入，包含程序成功、坏ELF和非零退出后继续输入。
```

既有正常运行记录中，孩子 7先输出EXEC_FAILURE_OK，随后`[EXEC_COMMIT] pid=7 program=worker old_pages=4 new_pages=5`，新worker输出EXEC_WORKER_OK并退出23；父仍wait同一个7并保留值42。之后的抢占、进程表满和回收继续通过，最后P3_READY。PID受前置演示影响，保持身份的关系不依赖固定数字。

运行`./airios run`，输入programs、run hello、run processes、run broken、run worker、echo still-alive。对应结果为正常0、完整多进程0、装载错误−8、合法退出23以及后续echo；错误文件不停止内核。Ctrl-A后X退出QEMU。图形支线合入后也可在Terminal输入同样命令，已有观察主要以 UART 记录完整 stdout。

调试可以在exec_current开始、提交前和cleanup old前分别比较current->pid、space.root、frame->rip/rsp和stack句柄。失败时这些旧字段不变；成功后PID与内核栈不变，root和用户RIP/RSP已换，old.stack必须为空。不要把C结构赋值理解成自动引用计数，所有权转移由这段不可抢占的提交顺序明确完成。

P3内核路径已经覆盖静态ELF、用户入口、系统调用、用户抢占、直接复制fork、事务exec和阻塞wait，且串口能再次启动程序。Linux对照接着观察真正的fork/execve/waitpid、COW和FD继承；其wait状态、errno与完整进程语义仍和本项目子集区分。当前不支持多核、信号、线程、FPU/SIMD上下文、完整argv/env/auxv或动态链接，用户程序使用课程的整数编译配置。
