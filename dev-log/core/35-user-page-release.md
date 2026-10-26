# K35 · 归还用户页，并让下一次申请复用空洞

从 K34 继续。孤儿退出后已经能及时释放资源，但一个一直不退出的服务器仍只能用 `airi_alloc` 增加用户页；六十四页预算很快就会耗尽。这里先建立完整页的释放与地址复用，让同一个进程反复申请、使用、归还内存。对象级 malloc、C++ 分配器和更大的内存预算留在后面接入。

本节增加 `airi_release(address, bytes)`。它只解除动态范围中的一到四个完整页，允许释放原申请范围中的部分页，也允许释放相邻申请形成的连续已映射页。内核记录页的 VA、PA 和权限，不记录 malloc 对象，所以调用者给的是页面范围，不是任意对象地址。地址与长度都必须按 4096 对齐；失败时整段原映射保持。已有程序不调用它时，exit 仍会清理剩余页面。

<a id="release-order"></a>
## 先检查整段，再断开映射和归还物理页

动态页限定在 `[0x10000000, 0x20000000)`，与静态 ELF 范围及 `0x70000000` 的初始用户栈分开。这个虚拟范围有 256 MiB，但当前每进程仍最多拥有六十四个用户数据页，代码、静态数据和栈也占预算。地址范围大小不等于已经分配的物理内存。

例如一次申请得到两页 A，程序现在只归还第二页。`release_user` 接到的是这一页的虚拟起点和4096字节长度；它先在 `owned` 中确认页面属于当前进程，且权限符合动态用户页的要求。检查地址时先限制它位于动态范围内，再计算 `USER_HEAP_END - address`，避免无符号减法下溢。`owned_index` 找不到页时返回 `current->count`，这个值在有效记录的右边界，不能再用来读取数组。

一次请求也可能覆盖四页。如果前三页存在、第四页是洞，返回错误以后前三页仍应属于调用者。因此第一遍只检查整段的地址、长度、右端以及每页的所有权和权限，不解除任何映射；全部通过以后，第二遍才逐页提交。普通坏输入停在第一遍，提交中再失败表示内核不变量已经被破坏，此时明确 panic。

提交每一页时，先用 `airi_vm_unmap` 断开当前用户根的映射并失效本 CPU 的旧翻译，再把物理页归还页池。如果顺序反过来，物理页可能已经被另一份内核或用户数据复用，旧映射却仍能读写它。K16 的 unmap 和 K22 的共享映射失效提供底层动作，这里按用户页的所有权调用它们。

物理页归还后，用 `owned` 的最后一个有效项补上空位，再减少 count。记录的位置可能随之改变，所以第二遍每处理下一页都重新查索引；列表不再按虚拟地址排序。这个内部顺序不影响 fork 按各项 VA/PA 复制，也不影响 exit 按所有权回收。

<a id="release-first-fit"></a>
## 不再让单向游标越过已经归还的地址

K34 的 `heap_next` 只向高地址移动。即使增加释放操作，它仍不会复用低地址空洞，长期运行后会耗尽虚拟范围。本节去掉这个游标，从动态低端找能容纳整次申请的第一段连续虚拟地址。

一次两页申请从候选起点开始，检查 owned 中是否有页落在这两页内。若撞上某页，就把候选移到该页右端，并重新检查全部记录；若没有冲突，才分配物理页。记录可以无序：任何落在当前候选区间内的占用页，都阻挡了从当前起点直到该页起点的所有候选，所以越过它不会漏掉更早可用的完整区间。每次移动都越过一个已有页，固定六十四项的表足够完成当前教学范围的搜索。

下面是用户工作负载按页号观察的同一轮操作，H 表示动态范围起点：

| 操作 | 仍持有的页与新返回地址 |
| --- | --- |
| 申请 A 两页、B 一页、C 一页 | A=H+0 页，B=H+2 页，C=H+3 页 |
| 释放 A 的第二页 | H+1 页成为单页洞，A 第一页、B、C 保持 |
| 申请 tail 两页 | 单页洞容不下；返回 H+4 页 |
| 释放 B | H+1、H+2 页连成两页洞 |
| 申请 joined 两页 | 返回 H+1 页，不能覆盖 H+3 页的 C |

物理页可以不连续，用户虚拟地址必须连续。新页仍由 `owned_page` 清零；页表建立失败时，`allocate_user` 只回滚本次新增项，旧映射和旧内容保持。

**更新 `kernel/process/user.c`。** 增加动态范围右端，替换 `allocate_user`，加入 `owned_index/release_user` 与调用号31，并删除 fork、exec 和 launch 中已无用途的 heap_next 复制/初始化。同时登记下面将加入的 memory ELF。以下旧窗口直接定位这些连接，其余 K34 内容保留；本节文件全部接入后再运行。

加入动态页右端，释放检查和首次适配共用这个范围。

本章每组替换的第一块只用于定位旧文本，敲入第二块替换它，不把两块同时加入文件。各组按正文顺序完成。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
#define USER_STACK 0x70000000ull // 当前每进程一页初始用户栈。
#define STACK_BASE 0xffffe10000000000ull // 所有进程陷入栈位于同一长期内核分支。
#define USER_HEAP 0x10000000ull // ELF静态范围之外的用户页分配起点。
#define PROCESS_COUNT 8u // 有界进程表，不做动态扩容。
enum user_state { USER_UNUSED, USER_RUNNABLE, USER_RUNNING, USER_WAITING, USER_IO_WAIT, USER_ZOMBIE, USER_REAP_PENDING }; // 退出与回收是两个不同状态。
enum io_operation { IO_NONE, IO_READ, IO_WRITE, IO_READY, IO_ASYNC }; // 等待保存操作条件，不保留内核局部buffer。
```
<!-- after -->
```c
#define USER_STACK 0x70000000ull // 当前每进程一页初始用户栈。
#define STACK_BASE 0xffffe10000000000ull // 所有进程陷入栈位于同一长期内核分支。
#define USER_HEAP 0x10000000ull // ELF静态范围之外的用户页分配起点。
#define USER_HEAP_END 0x20000000ull // 动态用户页只占这段独立范围，不接近初始用户栈。
#define PROCESS_COUNT 8u // 有界进程表，不做动态扩容。
enum user_state { USER_UNUSED, USER_RUNNABLE, USER_RUNNING, USER_WAITING, USER_IO_WAIT, USER_ZOMBIE, USER_REAP_PENDING }; // 退出与回收是两个不同状态。
enum io_operation { IO_NONE, IO_READ, IO_WRITE, IO_READY, IO_ASYNC }; // 等待保存操作条件，不保留内核局部buffer。
```

控制块不再保存单向 heap_next，只保留故障证据。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    struct airi_kernel_region stack; // 不能在仍使用它的C调用中释放。
    struct user_page owned[64]; // 所有用户数据页，含代码、静态数据、堆页和栈。
    unsigned count, pid, parent, user_ticks; // 资源数量、身份、父PID和本进程实际IRQ数。
    uint64_t fault_error, heap_next; // 用户故障证据与下一次有界用户页地址。
    int exit_code; // 自有wait接口返回普通退出码或负故障编码。
    enum user_state state; // 只有RUNNABLE可被调度器选中。
    int64_t wait_target; // 正PID表示指定孩子，-1表示任意孩子。
```
<!-- after -->
```c
    struct airi_kernel_region stack; // 不能在仍使用它的C调用中释放。
    struct user_page owned[64]; // 所有用户数据页，含代码、静态数据、堆页和栈。
    unsigned count, pid, parent, user_ticks; // 资源数量、身份、父PID和本进程实际IRQ数。
    uint64_t fault_error; // 用户故障证据；动态页位置由现有映射寻找，不再使用单向游标。
    int exit_code; // 自有wait接口返回普通退出码或负故障编码。
    enum user_state state; // 只有RUNNABLE可被调度器选中。
    int64_t wait_target; // 正PID表示指定孩子，-1表示任意孩子。
```

声明 memory 用户映像的边界。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
extern const uint8_t airi_user_rsp_bad_start[], airi_user_rsp_bad_end[]; // 不规范用户返回栈。
extern const uint8_t airi_hello_elf_start[], airi_hello_elf_end[]; // 既有独立用户C程序。
extern const uint8_t airi_process_elf_start[], airi_process_elf_end[]; // 新的fork/wait和抢占工作负载。
extern const uint8_t airi_orphans_elf_start[], airi_orphans_elf_end[]; // 持续会话中的孤儿回收与槽复用演示。
extern const uint8_t airi_shell_elf_start[], airi_shell_elf_end[]; // 真正用户shell，不是内核命令别名。
extern const uint8_t airi_demo_elf_start[], airi_demo_elf_end[]; // 用户页分配与异步存储贯通演示。
```
<!-- after -->
```c
extern const uint8_t airi_user_rsp_bad_start[], airi_user_rsp_bad_end[]; // 不规范用户返回栈。
extern const uint8_t airi_hello_elf_start[], airi_hello_elf_end[]; // 既有独立用户C程序。
extern const uint8_t airi_process_elf_start[], airi_process_elf_end[]; // 新的fork/wait和抢占工作负载。
extern const uint8_t airi_memory_elf_start[], airi_memory_elf_end[]; // 用户页面归还与空洞复用工作负载。
extern const uint8_t airi_orphans_elf_start[], airi_orphans_elf_end[]; // 持续会话中的孤儿回收与槽复用演示。
extern const uint8_t airi_shell_elf_start[], airi_shell_elf_end[]; // 真正用户shell，不是内核命令别名。
extern const uint8_t airi_demo_elf_start[], airi_demo_elf_end[]; // 用户页分配与异步存储贯通演示。
```

将 memory 接入既有初始目录。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
struct executable { const char *name; const uint8_t *start, *end; }; // 初始映像目录尚不是通用文件系统。
static const struct executable executables[] = { // 每份映像字节由内核只读段长期持有。
    {"sh", airi_shell_elf_start, airi_shell_elf_end}, // 独占串口输入的可选用户程序。
    {"orphans", airi_orphans_elf_start, airi_orphans_elf_end}, // 不结束根会话也能回收退出的孤儿。
    {"demo", airi_demo_elf_start, airi_demo_elf_end}, // 页、任务和设备完成串联。
    {"hello", airi_hello_elf_start, airi_hello_elf_end}, // 独立C运行库与BSS观察。
```
<!-- after -->
```c
struct executable { const char *name; const uint8_t *start, *end; }; // 初始映像目录尚不是通用文件系统。
static const struct executable executables[] = { // 每份映像字节由内核只读段长期持有。
    {"sh", airi_shell_elf_start, airi_shell_elf_end}, // 独占串口输入的可选用户程序。
    {"memory", airi_memory_elf_start, airi_memory_elf_end}, // 同一地址空间内释放与重新申请。
    {"orphans", airi_orphans_elf_start, airi_orphans_elf_end}, // 不结束根会话也能回收退出的孤儿。
    {"demo", airi_demo_elf_start, airi_demo_elf_end}, // 页、任务和设备完成串联。
    {"hello", airi_hello_elf_start, airi_hello_elf_end}, // 独立C运行库与BSS观察。
```

fork 不再复制已删除的游标，用户页仍按所有权复制。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    child->initial.rax = 0; // 子分支看到fork返回0。
    child->pid = next_pid++; // 全部内存成功后才分配PID。
    child->parent = parent->pid; // 关系用身份而不是可能复用的槽指针表示。
    child->heap_next = parent->heap_next; // 复制后的用户页分配游标也属于子空间。
    if (airi_async_open(child->pid)) // fork创建空上下文，用户内存里的父handle整数并不授权子访问。
        goto failed; // 其余子资源统一回滚，父不变。
    child->async_owned = true; // 成功后才记资源所有权。
```
<!-- after -->
```c
    child->initial.rax = 0; // 子分支看到fork返回0。
    child->pid = next_pid++; // 全部内存成功后才分配PID。
    child->parent = parent->pid; // 关系用身份而不是可能复用的槽指针表示。
    if (airi_async_open(child->pid)) // fork创建空上下文，用户内存里的父handle整数并不授权子访问。
        goto failed; // 其余子资源统一回滚，父不变。
    child->async_owned = true; // 成功后才记资源所有权。
```

在 allocate_user 的旧范围里接入首次适配，并在它之后加入 owned_index 与 release_user。整段预检、unmap/归还和末项补洞在这组完整函数中接齐。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    unsigned count = (unsigned)((bytes + 4095) / 4096); // 请求向上取整为完整页。
    if (count > 64 - current->count) // 保留所有权数组容量。
        return -12; // -ENOMEM。
    uint64_t address = current->heap_next; // 首次从ELF范围之外开始。
    unsigned before = current->count; // 失败只回滚本次新增映射。
    for (unsigned i = 0; i < count; ++i) // 页物理可以不连续，用户VA连续。
        if (!owned_page(current, address + i * 4096ull, AIRI_VM_WRITE)) // 成功页面已在owned中登记。
            goto rollback; // 游标尚未前移。
    current->heap_next += count * 4096ull; // 全部成功后再发布新的末端。
    airi_log("[USER_ALLOC] pid=%u address=0x%llx pages=%u zeroed=1", current->pid, (unsigned long long)address, count); // 只记录已完整提交的用户映射。
    return (long)address; // 当前没有逐对象free，进程结束统一回收。
rollback: // 已有用户页面不受本次耗尽影响。
    while (current->count > before) { // 只处理新增的成功项。
        const struct user_page *page = &current->owned[--current->count]; // 先退到最后一项。
        if (!airi_vm_unmap(&current->space, page->address) || !airi_memory_return_page(page->physical)) // 活动根必须先断映射并失效，再归还。
            airi_panic("cannot roll back user page allocation"); // 内部所有权错误不能被吞掉。
    }
    return -12; // 不返回半段用户范围。
}
static long exec_current(struct airi_trap_frame *f) { // 成功不返回旧用户调用，失败必须保持旧PID、空间和现场。
    char name[64]; // 先把用户名称复制到当前内核栈，后续不再借旧用户页。
```
<!-- after -->
```c
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
                break; // 不沿用旧范围检查结果。
            }
        }
        if (address > USER_HEAP_END - count * 4096ull) // 先减有界长度，不允许候选越过动态范围。
            return -12; // -ENOMEM；尚未分配页面，也没有修改原映射。
        if (!conflict) // 所有现有页都不与此候选相交。
            break; // 后续只为这段空洞创建映射。
    }
    unsigned before = current->count; // 失败只回滚本次新增映射。
    for (unsigned i = 0; i < count; ++i) // 页物理可以不连续，用户VA连续。
        if (!owned_page(current, address + i * 4096ull, AIRI_VM_WRITE)) // 成功页面已在owned中登记。
            goto rollback; // 本批映射尚未完整提交，已有用户页保持。
    airi_log("[USER_ALLOC] pid=%u address=0x%llx pages=%u zeroed=1", current->pid, (unsigned long long)address, count); // 只记录已完整提交的用户映射。
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
static long exec_current(struct airi_trap_frame *f) { // 成功不返回旧用户调用，失败必须保持旧PID、空间和现场。
    char name[64]; // 先把用户名称复制到当前内核栈，后续不再借旧用户页。
```

exec 提交不再转移游标，其余身份、栈、FD与根的转移保持。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    next.pid = old.pid; // exec保持进程身份。
    next.parent = old.parent; // 父仍等待同一个孩子。
    next.user_ticks = old.user_ticks; // 同一身份的已运行计数不丢失。
    next.heap_next = USER_HEAP; // 新映像的动态用户页从空开始。
    next.stack = old.stack; // 明确转移正在使用的内核栈所有权，VA和物理内容都不变。
    airi_memset(&old.stack, 0, sizeof(old.stack)); // old清理者不再拥有这组栈，不能重复释放。
    next.frame = f; // 真实返回现场仍在同一内核栈上，不能留下指向next.initial局部对象的指针。
```
<!-- after -->
```c
    next.pid = old.pid; // exec保持进程身份。
    next.parent = old.parent; // 父仍等待同一个孩子。
    next.user_ticks = old.user_ticks; // 同一身份的已运行计数不丢失。
    next.stack = old.stack; // 明确转移正在使用的内核栈所有权，VA和物理内容都不变。
    airi_memset(&old.stack, 0, sizeof(old.stack)); // old清理者不再拥有这组栈，不能重复释放。
    next.frame = f; // 真实返回现场仍在同一内核栈上，不能留下指向next.initial局部对象的指针。
```

在目录查询之后接入31号归还调用，仍直接使用用户传来的地址数值与长度。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
        else // 完整复制后才发布有效长度。
            f->rax=used; // 不含末尾NUL，与strlen一致。
    }
    else // 尚未提供的接口都明确拒绝。
        f->rax = (uint64_t)-38ll; // -ENOSYS。
    poll_waiters(); // 文件/流修改完成后立即重查其他等待者，不等用户轮询。
```
<!-- after -->
```c
        else // 完整复制后才发布有效长度。
            f->rax=used; // 不含末尾NUL，与strlen一致。
    }
    else if (f->rax == 31) // 只归还调用者自己的动态页面，不能释放ELF或初始栈。
        f->rax = (uint64_t)release_user(f->rdi, f->rsi); // 负参数错误保持全部原映射。
    else // 尚未提供的接口都明确拒绝。
        f->rax = (uint64_t)-38ll; // -ENOSYS。
    poll_waiters(); // 文件/流修改完成后立即重查其他等待者，不等用户轮询。
```

launch 不再初始化已经不存在的 heap_next，新页面从实际空洞选择地址。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    if (name && airi_strlen(name)==2 && !airi_memcmp(name,"sh",2)) // 只有直接启动shell时接入UART输入。
        airi_fd_attach_input(&root->files); // CLOEXEC使既有子程序FD0继续可用于普通文件。
    root->pid = next_pid++; // 完整准备后才取得运行身份。
    root->heap_next = USER_HEAP; // 动态用户页初始为空。
    int async_error = airi_async_open(root->pid); // 完整映像准备后才登记新进程资源上下文。
    if (async_error) { // 未向用户发布，此时根与栈不活动。
        cleanup(root); // 回滚页面、FD与内核栈，尚未拥有异步owner。
```
<!-- after -->
```c
    if (name && airi_strlen(name)==2 && !airi_memcmp(name,"sh",2)) // 只有直接启动shell时接入UART输入。
        airi_fd_attach_input(&root->files); // CLOEXEC使既有子程序FD0继续可用于普通文件。
    root->pid = next_pid++; // 完整准备后才取得运行身份。
    int async_error = airi_async_open(root->pid); // 完整映像准备后才登记新进程资源上下文。
    if (async_error) { // 未向用户发布，此时根与栈不活动。
        cleanup(root); // 回滚页面、FD与内核栈，尚未拥有异步owner。
```

在 `kernel/process/user.c` 中同步本阶段已变化的条件说明；执行语句保留。旧块用于定位，只写回对应新块。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    } else if (f->rax == 8) // wait可直接回收或真的阻塞。
        return wait_current(f); // 结果恢复到父同一次调用之后。
    else if (f->rax == 9) // 申请零填充用户页。
        f->rax = (uint64_t)allocate_user((size_t)f->rdi); // 不成功不前移堆游标。
    else if (f->rax == 11) { // open只消费内核持有的短路径副本。
        char path[32]; // 与平坦目录名容量一致。
        bool copied = false; // 必须看到终止零才可交给文件层。
```
<!-- after -->
```c
    } else if (f->rax == 8) // wait可直接回收或真的阻塞。
        return wait_current(f); // 结果恢复到父同一次调用之后。
    else if (f->rax == 9) // 申请零填充用户页。
        f->rax = (uint64_t)allocate_user((size_t)f->rdi); // 失败保持已有用户页与映射。
    else if (f->rax == 11) { // open只消费内核持有的短路径副本。
        char path[32]; // 与平坦目录名容量一致。
        bool copied = false; // 必须看到终止零才可交给文件层。
```

当前系统每进程只有一个线程，内核不可抢占，所以验证到提交之间没有另一个线程改变同一根页表。以后加入共享地址空间的线程时，这里需要相应同步。现有异步 I/O 注册的是内核副本，设备不直接持有这次释放的用户页；不能把这个条件推广到以后钉住用户页的零拷贝 DMA。

<a id="release-abi"></a>
## 用户库把页范围送入同一次系统调用

用户传入的地址通过整数寄存器进入内核，内核不会解引用该地址读取释放参数。`airi_release` 返回 0 或负错误；`airi_alloc` 仍把失败转换为 NULL。这里是自有 int80 页接口，尚未增加 POSIX errno，也没有宣称兼容 mmap/munmap 的所有参数和错误规则。

更新 `labs/airios/include/airi_libc.h`：补释放声明，并将原来的“只在退出回收”说明改成显式释放或退出均可回收。其他声明保持。

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
long airi_release(void *address, size_t bytes); // 解除一至四个完整动态页；允许部分范围，失败不改整段。
long airi_programs(char *names, size_t capacity); // 真实初始ELF目录，每行名称，容量不足不截断。
#endif // 结束包含保护。
```

在 `labs/airios/libc/syscall.c` 末尾追加这个包装。复用已有 `call3`：31 是本内核调用号，RDI 是虚拟地址数值，RSI 是字节长度，第三参数留零。memory clobber 继续阻止编译器把相关内存操作跨过系统调用；CPU 的地址翻译失效由内核 unmap 完成。

<!-- append: labs/airios/libc/syscall.c -->
```c

long airi_release(void *address, size_t bytes) { // 调用后用户不能继续解引用已归还范围。
    return call3(31, (uintptr_t)address, bytes, 0); // 自有页接口返回0或负错误，不设置POSIX errno。
}
```

<a id="release-workload"></a>
## 反复复用地址，并故意访问已解除的页

新建 `labs/airios/memory.c`。每轮按前面的 A/B/C/tail/joined 顺序运行，用 `uintptr_t` 对虚拟地址数值作布局比较。程序给不同范围填入不同字节，检查邻居保持、新申请页全部清零。中间特意请求一个末页未映射的四页范围，确认失败后前面的有效页仍能读取；再检查未对齐、零长度、超大长度、初始栈和重复释放都被拒绝。

连续一百二十八轮累计申请远超过六十四页，但每轮都在当前进程内归还。若只更新记录而没有归还物理页，或留下了无法复用的所有权项，后续申请就会失败。

循环结束后再 fork。孩子释放的是自己的物理副本，随后通过 volatile 指针真实读取已解除的用户地址，产生用户缺页异常。父 wait 取得自有故障退出码 −142 后，仍能从自己的页读到42：相同虚拟地址在父子两根页表中有各自的映射，孩子的释放不应撤掉父的内容。

volatile 要求编译器保留这次读取，避免把故障演示消掉。内核只结束子进程，父随后归还自己的页并报告成功；这个故障操作由正文和完整代码直接演示，不留给读者自行猜测操作。

<!-- write: labs/airios/memory.c -->
```c
#include <airi_libc.h> // 页生命周期仍通过自有ABI，不使用宿主malloc。
static int reuse_round(void) { // 每轮保持同一个进程地址空间，只替换动态页。
    unsigned char *a = airi_alloc(8192); // 两页对象，分配器从动态范围低端寻找空洞。
    unsigned char *b = airi_alloc(4096); // 紧随a的一页，之后用于扩大空洞。
    unsigned char *c = airi_alloc(4096); // 留住它，阻止两页请求跨过此页。
    if (!a || !b || !c || (uintptr_t)b != (uintptr_t)a + 8192 || (uintptr_t)c != (uintptr_t)a + 12288) // 固定工作负载核对最初布局。
        return 121; // 失败不能沿无效指针读写。
    for (unsigned i = 0; i < 8192; ++i) // 每一轮都检查复用页的全部字节。
        if (a[i]) // 旧所有者的填充值必须已经清零。
            return 122; // 不只检查第一个字节。
    memset(a, 0xa1, 8192); // 两页都留下可辨识内容。
    memset(b, 0xb2, 4096); // 独立邻居的保护字节。
    memset(c, 0xc3, 4096); // 后面的请求不能覆盖仍持有的页。
    if (airi_release(a + 1, 4096) != -22 || airi_release(a, 0) != -22 || airi_release(a, 1) != -22 || airi_release(a, SIZE_MAX) != -22) // 地址、长度与溢出输入均拒绝。
        return 123; // 无效请求不能被偷偷舍入。
    if (airi_release((void *)(uintptr_t)0x70000000, 4096) != -22) // 初始用户栈不是动态分配页。
        return 124; // 内核不能接受任意自有RW页的释放。
    if (airi_release(a + 4096, 16384) != -22 || a[4096] != 0xa1 || b[0] != 0xb2 || c[0] != 0xc3) // 范围末页未映射，前面三页也必须保持。
        return 125; // 验证整段先检查再提交。
    if (airi_release(a + 4096, 4096)) // 只释放a的第二页，第一页面向用户仍有效。
        return 126; // 页面接口允许完整页子范围。
    unsigned char *tail = airi_alloc(8192); // 单页洞容不下两页，须越过仍持有的b和c。
    if (!tail || (uintptr_t)tail != (uintptr_t)c + 4096 || c[0] != 0xc3) // 不把非连续物理页误当连续VA洞。
        return 127; // 活页被覆盖或选择范围错误。
    if (airi_release(b, 4096)) // 与刚才空洞连成两个连续用户虚拟页。
        return 128; // b之后不能再解引用。
    unsigned char *joined = airi_alloc(8192); // owned换序仍须找到低地址完整洞。
    if ((uintptr_t)joined != (uintptr_t)a + 4096) // 页面列表不是有序表，也能复用邻接洞。
        return 129; // 单向游标会在这里暴露。
    for (unsigned i = 0; i < 8192; ++i) // 这两页分别曾属于a和b。
        if (joined[i]) // 不允许保留任一旧填充值。
            return 130; // 页面归还与再次分配的清零都必须真实发生。
    if (airi_release(joined, 8192) || airi_release(tail, 8192)) // 归还两个新范围。
        return 131; // 不让本轮新对象留到进程exit。
    if (airi_release(a, 8192) != -22 || a[0] != 0xa1) // 第二页已经是洞，不能先释放第一页再报告失败。
        return 132; // 第一页仍须可访问。
    if (airi_release(a, 4096) || airi_release(c, 4096) || airi_release(a, 4096) != -22) // 清最后活页，并验证重复释放拒绝。
        return 133; // 下一轮仍从同一低地址空洞开始。
    return 0; // 当前进程还活着，页资源已在本轮归还。
}
int main(void) { // 测试的返回码由用户shell的wait报告。
    for (unsigned i = 0; i < 128; ++i) { // 累计分配远超每进程六十四页上限。
        int error = reuse_round(); // 多次页面创建与解除都在同一地址空间。
        if (error) // 第一次失败保留精确位置。
            return error; // 不重新启动规避资源泄漏。
    }
    volatile unsigned char *page = airi_alloc(4096); // 单独检查真实解除映射与fork所有权。
    if (!page) // 普通耗尽不能当作已准备好故障页。
        return 134; // shell收到明确错误。
    *page = 42; // 父子复制前的独立数据。
    long child = airi_fork(); // 子有同VA不同物理页，释放不应影响父。
    if (child < 0) // 不对失败身份执行wait。
        return 135; // 明确进程资源失败。
    if (!child) { // 故障只发生在拥有被解除页的孩子。
        if (airi_release((void *)page, 4096)) // 解除当前活动用户根的真实映射。
            return 136; // 失败不执行预期故障操作。
        return *page; // volatile强制真实读取，必须触发用户PF而不能命中旧TLB。
    }
    int status = 0; // 子用户故障由内核转成自有退出码。
    if (airi_waitpid(child, &status) != child || status != -142 || *page != 42) // 父地址空间与内容仍有效。
        return 137; // 错误解除共享页或未执行invlpg都会暴露。
    if (airi_release((void *)page, 4096)) // 父自己的页也正常归还。
        return 138; // 最后由launch再核对会话总资源。
    airi_puts("[USER_MEMORY_REUSE_OK] rounds=128 holes=first-fit zeroed=1 invalid=unchanged\n"); // 只在所有路径完成后发布。
    return 0; // shell仍活着，后续可继续运行orphans或其他程序。
}
```

在 `Makefile` 末尾追加 memory ELF 规则，再在包装汇编末尾追加其边界。通用用户编译规则、USER_COMMON、静态链接脚本和 `.incbin` 的关系沿 K33/K34；新程序拥有自己的 main，不与 orphans 或 shell 的 main 混合链接。

<!-- append: Makefile -->
```make

# 动态页工作负载与其他用户main分别链接。
$(OUT)/user/memory.elf: $(USER_COMMON) $(OUT)/user/memory.o labs/airios/program.ld
# 静态ELF只使用自有用户入口与库。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/memory.o -o $@
# 当前包装对象要等本配置的新映像完成。
$(OUT)/labs/airios/images.o: $(OUT)/user/memory.elf
# 文件名作为C字符串宏传入预处理汇编。
$(OUT)/labs/airios/images.o: CPPFLAGS += -DAIRI_MEMORY_ELF='"$(OUT)/user/memory.elf"'
```
<!-- append: labs/airios/images.S -->
```asm

.section .rodata /* 页释放程序由同一静态ELF装载器处理。 */
.global airi_memory_elf_start, airi_memory_elf_end /* 内核只保留输入字节边界。 */
airi_memory_elf_start: /* 文件首。 */
.incbin AIRI_MEMORY_ELF /* 本配置的新用户映像。 */
airi_memory_elf_end: /* 文件右端。 */
```

<a id="release-observe"></a>
## 在真实用户 shell 中继续运行

更新 `scripts/shell_test.py`，在 orphans 前运行 memory，并核对新的用户成功标记。原来的二十四轮孤儿回收、慢设备、坏 exec、长行和返回监控台检查继续保留；整个脚本仍只对本次请求之后的串口记录作判断。

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
                result={'passed':True,'ring3_shell':True,'ram_file_and_device_readback':True,'other_user_during_cq_wait':True,'qmp_iops':1,'error_exec_survived':True,'returned_to_monitor':True,'orphan_rounds':24,'memory_reuse_rounds':128} # 只发布已观察事实。
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
./airios test # 八组主机检查、九种启动、真实输入、用户页复用/故障与孤儿回收，继续核对存储路径。
```

交互时运行 `./airios run`，在 `airi>` 输入 `run sh`，再输入 `memory`。正常结果包含子进程的 `[USER_FAULT] vector=14` 记录、父用户成功标记，以及 shell 收到的零退出码；PID、RIP 等地址按本次实际运行记录读取。

```text
[USER_MEMORY_REUSE_OK] rounds=128 holes=first-fit zeroed=1 invalid=unchanged
[SHELL_COMMAND_DONE] name=memory code=0
```

继续输入 `memory` 或 `orphans` 仍会成功。归还页面后若程序保留旧指针，再次申请可能复用同一个虚拟地址；旧指针的数值没有自动失效标记。这是用户程序需要遵守的对象生命周期，后面的 malloc/C++ 层要在此页机制之上管理对象，不能靠内核自动辨认每个 C 指针。

Linux 对照继续 L02 的匿名映射与缺页观察；解除映射后访问相应地址属于其 munmap 行为的一部分。Linux 对未映射范围、长度舍入和错误的完整规则与本教学接口有区别，具体见 [mmap/munmap 手册](https://man7.org/linux/man-pages/man2/munmap.2.html)。本节增加的是 AiriOS 真实页生命周期，Linux 主机本轮是否可连接另行记录。

下一步将让用户 init 管理常驻服务，继续拆开“启动一组演示并统一结束”与“内核长期服务”的关系。当前仍是八进程、单线程、静态 ELF、单核和每进程六十四页，本节没有扩大这些容量。
