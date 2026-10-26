# K56 · 从当前线程控制块取得身份

K54 的每次加锁和解锁都查询 TID，K55 的每次分配又经过这把锁。当前执行者在一次运行期间身份不变，没必要每次都进入内核重新取同一个值。K43 已经让 FS 指向每条执行者自己的静态 TLS 运行块，TP 后还保留了16字节控制头，现在把其中的身份和版本正式约定下来。

从 K55 的同一目录继续。普通身份读取走用户映射，42号原始查询仍保留；内核负责在启动、fork、exec、新worker和槽复用时写入正确值。下面的观察会直接调用原始fork，而不只经过用户库包装，检验更新发生在真正的生命周期交接处。

<a id="runtime-tcb-layout"></a>
## 同一块用户内存，有两种不同的更新责任

程序的 `_Thread_local local` 初值为42。父进程改为99后fork，孩子的普通TLS变量应继续是99，因为它继承了父的运行状态；但孩子不能继承父的TID。若只是把查询结果缓存成另一个普通TLS变量，逐页复制也会复制旧身份。

因此保留普通TLS数据的复制规则，同时由内核在孩子尚未运行时改写控制头。exec则重新建立声明初值42，并写入仍属于这个孩子的TID。新worker从ELF模板重新建立普通TLS，而头里的身份是它自己新分配的值。程序数据与运行身份虽然位于相邻的用户页中，来源和更新时间仍不同。

新建 include/airi/runtime_abi.h。布局保持原来TCB的16字节大小，既有模板加控制头的页数计算规则不变：

| 相对TP偏移 | 宽度 | 当前内容 |
| --- | --- | --- |
| 0 | 8字节 | self，当前控制头的用户虚拟地址 |
| 8 | 4字节 | 当前执行者的非零TID |
| 12 | 4字节 | 布局版本1 |

self不是物理地址，TID也不是授权令牌。用户可以改自己的可写内存，但内核仍从running/current决定真正身份、FD和进程资源；不会拿这份头里的数值授权。该头是本项目运行库ABI，不是Linux或glibc的TCB布局。

<!-- write: include/airi/runtime_abi.h -->
```c
#ifndef AIRI_RUNTIME_ABI_H // 当前静态TLS运行块头的自有数值布局。
#define AIRI_RUNTIME_ABI_H // 包含保护，不引入内核对象指针。
#include <stdint.h> // 用户和内核都使用相同固定宽度。
#define AIRI_RUNTIME_VERSION 1u // 版本0仍可由用户库退回原始身份查询。
struct airi_runtime_tcb { // 占用既有TP之后的16字节，不扩大TLS运行页数。
    uint64_t self; // 当前执行者TP的用户虚拟地址，不是物理地址或权限令牌。
    uint32_t tid; // 内核在真正发布执行者以前写入其非零身份。
    uint32_t version; // 只描述这16字节布局，不代表完整pthread或Linux TCB。
};
#endif // 结束包含保护。
```

<a id="runtime-tcb-publication"></a>
## 先准备映射，再发布真正身份

内核引入同一个声明，并把写头集中到write_tcb。它接收已验证布局算出的目标VA，经目标进程自己的页表复制16字节，允许该根尚未成为CR3。不能把用户保存的FS或self直接当内核裸指针。write_primary_tcb只根据USER_TLS和校验后的pointer_offset找到主运行块；无PT_TLS时不读写不存在的控制头。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
#include <airi/time_abi.h> // 单调时间只输出固定数值，用户指针由当前根验证。
#include <airi/vector_abi.h> // 向量描述在一次内核调用内完整复制，文件操作前检查本次前缀。
#include <airi/tls.h> // 每次真实用户陷入取回FS，内核处理期间清零。
```
<!-- after -->
```c
#include <airi/runtime_abi.h> // 共享数值TCB布局，身份由内核在生命周期交接时更新。
#include <airi/time_abi.h> // 单调时间只输出固定数值，用户指针由当前根验证。
#include <airi/vector_abi.h> // 向量描述在一次内核调用内完整复制，文件操作前检查本次前缀。
#include <airi/tls.h> // 每次真实用户陷入取回FS，内核处理期间清零。
```

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
        done += part; // 推进下一段。
    }
    return true; // 同步结束，不借出buffer指针。
}
static long file_transfer(struct user_process *p, uint64_t fd, uint64_t address, size_t bytes, bool write, bool positioned, uint64_t offset) { // 允许在另一根的阻塞进程上重试，同步副本属于当前内核栈。
    char buffer[256]; // 一次有界副本，不跨调度保留。
```
<!-- after -->
```c
        done += part; // 推进下一段。
    }
    return true; // 同步结束，不借出buffer指针。
}
_Static_assert(sizeof(struct airi_runtime_tcb) == 16, "runtime header must fit existing TCB reservation"); // 保持ELF TLS页预算与负偏移布局。
static void write_tcb(struct user_process *p, uint64_t address, unsigned tid) { // 只接装载器计算出的已拥有TCB位置，不相信用户保存的FS或self值。
    struct airi_runtime_tcb control = {address, tid, AIRI_RUNTIME_VERSION}; // self是用户VA，身份是当前内核分配的数值。
    if (!copy_user(p, &control, address, sizeof(control), true)) // 通过目标进程页表写自己的映射，允许目标根尚未活动。
        airi_panic("cannot publish prepared runtime TCB"); // 已准备映射不变量错误不能发布半个身份。
}
static void write_primary_tcb(struct user_process *p) { // fork/exec只允许已收拢worker的主执行者。
    if (p->tls.memory_bytes) // 没有PT_TLS的旧程序仍FS=0，不读写不存在的TCB。
        write_tcb(p, USER_TLS + p->tls.pointer_offset, p->execution.tid); // 固定运行块位置来自校验后布局，不跟随任意用户FS修改。
}
static long file_transfer(struct user_process *p, uint64_t fd, uint64_t address, size_t bytes, bool write, bool positioned, uint64_t offset) { // 允许在另一根的阻塞进程上重试，同步副本属于当前内核栈。
    char buffer[256]; // 一次有界副本，不跨调度保留。
```

装载候选映像时，TLS页已经完整映射，普通初值也已复制，但候选还没有运行身份。先写self、版本及临时tid=0；此时只属于内核准备过程，尚未公开USER_RUNNABLE。真正启动时分配root PID/TID，再覆盖为非零值，最后才进入可运行集合。不能把“ELF已准备”当成“用户已经能读这个头”。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
        if (view.tls.file_bytes && !copy_user(p, (void *)(view.image + view.tls.offset), data, (size_t)view.tls.file_bytes, true)) // 按目标页查物理别名，初值可能跨页。
            goto failed; // 只复制filesz，memsz剩余保持owned_page的零填充。
        p->execution.fs_base = USER_TLS + view.tls.pointer_offset; // TP位于TLS数据之后，不指向模板p_vaddr。
        uint64_t control[2] = {p->execution.fs_base, 0}; // FS:0为self，第二字保留给后续自有运行库。
        if (!copy_user(p, control, p->execution.fs_base, sizeof(control), true)) // TCB属于当前新用户根，不能直接写未活动用户VA。
            goto failed; // 未完整建立TP不发布可运行状态。
        airi_log("[ELF_TLS] bytes=%llu initialized=%llu alignment=%llu pages=%u data=0x%llx tp=0x%llx", (unsigned long long)view.tls.memory_bytes, (unsigned long long)view.tls.file_bytes, (unsigned long long)view.tls.alignment, view.tls.pages, (unsigned long long)data, (unsigned long long)p->execution.fs_base); // 模板与运行位置分别可观察。
    }
    p->tls = view.tls; // 保存已校验模板布局，不能从后来修改过的主TLS复制初值。
```
<!-- after -->
```c
        if (view.tls.file_bytes && !copy_user(p, (void *)(view.image + view.tls.offset), data, (size_t)view.tls.file_bytes, true)) // 按目标页查物理别名，初值可能跨页。
            goto failed; // 只复制filesz，memsz剩余保持owned_page的零填充。
        p->execution.fs_base = USER_TLS + view.tls.pointer_offset; // TP位于TLS数据之后，不指向模板p_vaddr。
        write_tcb(p, p->execution.fs_base, 0); // 私有候选尚无身份；发布之前再写真实TID，不让用户观察0。
        airi_log("[ELF_TLS] bytes=%llu initialized=%llu alignment=%llu pages=%u data=0x%llx tp=0x%llx", (unsigned long long)view.tls.memory_bytes, (unsigned long long)view.tls.file_bytes, (unsigned long long)view.tls.alignment, view.tls.pages, (unsigned long long)data, (unsigned long long)p->execution.fs_base); // 模板与运行位置分别可观察。
    }
    p->tls = view.tls; // 保存已校验模板布局，不能从后来修改过的主TLS复制初值。
```

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    root->pid = next_pid++; // 完整准备后才取得运行身份。
    root->execution.process = root; // 会话主执行者显式借所属进程。
    root->execution.tid = root->pid; // 主TID初始与PID相同，额外执行者另取身份。
    int async_error = airi_async_open(root->pid); // 完整映像准备后才登记新进程资源上下文。
    if (async_error) { // 未向用户发布，此时根与栈不活动。
        cleanup(root); // 回滚页面、FD与内核栈，尚未拥有异步owner。
```
<!-- after -->
```c
    root->pid = next_pid++; // 完整准备后才取得运行身份。
    root->execution.process = root; // 会话主执行者显式借所属进程。
    root->execution.tid = root->pid; // 主TID初始与PID相同，额外执行者另取身份。
    write_primary_tcb(root); // 初次发布时补上真实TID，flat/noTLS映像不写任何运行块。
    int async_error = airi_async_open(root->pid); // 完整映像准备后才登记新进程资源上下文。
    if (async_error) { // 未向用户发布，此时根与栈不活动。
        cleanup(root); // 回滚页面、FD与内核栈，尚未拥有异步owner。
```

fork逐页复制全部用户内容以后，孩子先获得新PID/TID，再保存父的TLS布局并重写孩子自己的头。普通变量仍是复制来的99，TP数值仍可与父相同，物理页却分别持有。更新位于内核fork_current，所以直接6号调用同样获得新身份，不依赖用户回调补救。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    child->execution.tid = child->pid; // 初始执行者的TID与PID数值相同。
    child->tls = parent->tls; // fork复制用户TLS当前值，后续新工作者仍从原ELF模板建立。
    child->tls_image = parent->tls_image; // 只读输入字节由内核目录长期持有。
    child->parent = parent->pid; // 关系用身份而不是可能复用的槽指针表示。
    if (airi_async_open(child->pid)) // fork创建空上下文，用户内存里的父handle整数并不授权子访问。
        goto failed; // 其余子资源统一回滚，父不变。
```
<!-- after -->
```c
    child->execution.tid = child->pid; // 初始执行者的TID与PID数值相同。
    child->tls = parent->tls; // fork复制用户TLS当前值，后续新工作者仍从原ELF模板建立。
    child->tls_image = parent->tls_image; // 只读输入字节由内核目录长期持有。
    write_primary_tcb(child); // 用户TLS值继续复制，TCB身份必须改为孩子，直接raw fork也成立。
    child->parent = parent->pid; // 关系用身份而不是可能复用的槽指针表示。
    if (airi_async_open(child->pid)) // fork创建空上下文，用户内存里的父handle整数并不授权子访问。
        goto failed; // 其余子资源统一回滚，父不变。
```

exec先完整准备新映像，原调用者仍保留旧空间。成功提交后，新memory和tls布局进入current，FS采用新TP；这时写入原主执行者的TID，不能把候选中的0带到新main。失败的ELF或资源申请仍不会提交这些修改，旧控制头继续可用。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    running->fpu_physical = next.execution.fpu_physical; // 替换为准备好的默认x87/SSE状态。
    next.execution.fpu_physical = 0; // 候选从此不能再归还这页。
    running->fs_base = next.execution.fs_base; // 新映像的静态TLS指针，或无TLS时的0。
    running->initial = next.execution.initial; // 保存新入口数值，不保存指向候选栈局部对象的指针。
    running->frame = f; // 原内核陷入栈及TSS继续属于同一执行者。
    *f = running->initial; // 全部可失败准备完成之后才覆盖旧RIP/RSP和寄存器。
```
<!-- after -->
```c
    running->fpu_physical = next.execution.fpu_physical; // 替换为准备好的默认x87/SSE状态。
    next.execution.fpu_physical = 0; // 候选从此不能再归还这页。
    running->fs_base = next.execution.fs_base; // 新映像的静态TLS指针，或无TLS时的0。
    write_primary_tcb(current); // 新TLS初值已经接入，继续使用原进程主TID，而不是候选的0。
    running->initial = next.execution.initial; // 保存新入口数值，不保存指向候选栈局部对象的指针。
    running->frame = f; // 原内核陷入栈及TSS继续属于同一执行者。
    *f = running->initial; // 全部可失败准备完成之后才覆盖旧RIP/RSP和寄存器。
```

worker的TLS初值和栈先准备，身份在所有可失败资源申请完成后才分配。把旧的两字保留头写入移到tid赋值以后，统一使用write_tcb，再发布可运行状态。旧槽清零和模板重建继续保持，不能因为self地址可以复用，就连上一次TID也一起复用。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
        if (current->tls.file_bytes && !copy_user(current, (void *)(current->tls_image + current->tls.offset), tls + current->tls.data_offset, (size_t)current->tls.file_bytes, true)) // 布局已在装载时校验。
            airi_panic("cannot initialize prepared worker TLS"); // 映射不应在不可抢占准备中改变。
        execution->fs_base = tls + current->tls.pointer_offset; // TP仍按既有local-exec负偏移布局。
        uint64_t control[2] = {execution->fs_base, 0}; // 当前工作者自己的self指针与保留槽。
        if (!copy_user(current, control, execution->fs_base, sizeof(control), true)) // 只向已准备的TCB范围写入。
            airi_panic("cannot initialize worker TCB"); // 不发布半初始化FS。
    }
    execution->initial = (struct airi_trap_frame){0}; // 首次工作入口没有继承主执行者的GPR。
    execution->initial.rip = entry; // 用户库汇编桥接器负责调用真正工作函数和提交返回值。
```
<!-- after -->
```c
        if (current->tls.file_bytes && !copy_user(current, (void *)(current->tls_image + current->tls.offset), tls + current->tls.data_offset, (size_t)current->tls.file_bytes, true)) // 布局已在装载时校验。
            airi_panic("cannot initialize prepared worker TLS"); // 映射不应在不可抢占准备中改变。
        execution->fs_base = tls + current->tls.pointer_offset; // TP仍按既有local-exec负偏移布局。

    }
    execution->initial = (struct airi_trap_frame){0}; // 首次工作入口没有继承主执行者的GPR。
    execution->initial.rip = entry; // 用户库汇编桥接器负责调用真正工作函数和提交返回值。
```

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    execution->initial.rflags = 0x202; // 用户中断开启，无I/O特权。
    execution->process = current; // 共享既有内存、FD和异步owner，不打开第二份资源表。
    execution->tid = next_pid++; // 全部准备完成以后才发布新的执行身份。
    place_frame(execution); // 现场进入本执行者自己的内核栈。
    execution->state = USER_RUNNABLE; // 最后发布运行资格，IRQ此前只能看见空槽。
    airi_log("[THREAD_CREATE] pid=%u tid=%u slot=%u tp=0x%llx", current->pid, execution->tid, index, (unsigned long long)execution->fs_base); // 记录真实共享身份与本地寻址起点。
```
<!-- after -->
```c
    execution->initial.rflags = 0x202; // 用户中断开启，无I/O特权。
    execution->process = current; // 共享既有内存、FD和异步owner，不打开第二份资源表。
    execution->tid = next_pid++; // 全部准备完成以后才发布新的执行身份。
    if (current->tls.memory_bytes) write_tcb(current, execution->fs_base, execution->tid); // 新槽和复用槽都在可运行前取得自己的身份。
    place_frame(execution); // 现场进入本执行者自己的内核栈。
    execution->state = USER_RUNNABLE; // 最后发布运行资格，IRQ此前只能看见空槽。
    airi_log("[THREAD_CREATE] pid=%u tid=%u slot=%u tp=0x%llx", current->pid, execution->tid, index, (unsigned long long)execution->fs_base); // 记录真实共享身份与本地寻址起点。
```

调度继续恢复所选execution保存的FS。切换到另一位worker时，CPU开始从另一份头读取self和TID；不需要每个tick重建或重新分配这16字节。

<a id="runtime-fast-query"></a>
## FS找到控制头，普通加载取得TID

选用线程库的程序可能没有其他TLS变量。新增runtime_anchor，并以used属性要求编译器保留这个静态TLS定义，使这些ELF也具有PT_TLS。它不是身份缓存，不需要在fork包装里清零。原来没有TLS、现在选择这个库的程序会新增本地运行映射；保持控制头为16字节，不等于每份程序的总页数都不变。当前链接不使用节垃圾回收；在ELF上used本身不阻止--gc-sections，未来改变该链接策略必须一并保护这个运行前提。

后面的inspect先以原始42号查询校准身份，再连续调用128次普通airi_thread_id。每次普通调用先从当前FS:0取得self的用户VA，再在这份头里读取偏移12的版本和偏移8的TID。版本为1且身份非零，就把这个值交回调用方，CPU只完成普通用户加载，不执行int80。

旧保留头或零身份才退回42号查询。这个后备判断需要先能读取合法控制头；有效FS布局仍是运行库前提，不能把FS改成任意自定义块以后继续调用这些TLS接口。切换工作者时由调度恢复它自己的FS，同一段读取代码才会访问另一份头。

<!-- replace: labs/airios/libc/threads.c -->
<!-- before -->
```c
#include <airi_threads.h> // 不借pthread或Linux clone包装。
#include <stdint.h> // 指针以目标64位数值进入内核。
extern void airi_thread_entry(void); // 汇编桥接器被静态链接到当前用户可执行段。
static long thread_call(long number, uint64_t first, uint64_t second, uint64_t third) { // 与既有int80前三参数约定相同。
    __asm__ volatile("int $0x80" : "+a"(number) : "D"(first), "S"(second), "d"(third) : "memory", "cc"); // 内存clobber让调用边界不隐瞒共享读写。
```
<!-- after -->
```c
#include <airi/runtime_abi.h> // 同内核约定的16字节运行头，不复制内部控制块。
#include <airi_threads.h> // 不借pthread或Linux clone包装。
#include <stdint.h> // 指针以目标64位数值进入内核。
static _Thread_local unsigned char runtime_anchor __attribute__((used)); // 选用该库的ELF必须保留PT_TLS，即使没有别的本地变量。
extern void airi_thread_entry(void); // 汇编桥接器被静态链接到当前用户可执行段。
static long thread_call(long number, uint64_t first, uint64_t second, uint64_t third) { // 与既有int80前三参数约定相同。
    __asm__ volatile("int $0x80" : "+a"(number) : "D"(first), "S"(second), "d"(third) : "memory", "cc"); // 内存clobber让调用边界不隐瞒共享读写。
```

<!-- replace: labs/airios/libc/threads.c -->
<!-- before -->
```c
long airi_thread_join(long tid, long *result) { // 内核在输出前核对完整long跨度。
    return thread_call(41, (uint64_t)tid, (uintptr_t)result, 0); // 未完成时调用链真正阻塞，不由库忙轮询。
}
long airi_thread_id(void) { // 只返回当前执行身份，不产生资源所有权。
    return thread_call(42, 0, 0, 0); // PID仍使用原始airi_getpid查询。
}
long airi_futex_wait(uint32_t *word, uint32_t expected, unsigned ticks) { // 不改errno，返回自有内核原始结果。
    return thread_call(43, (uintptr_t)word, expected, ticks); // 条件字由调用者以原子操作读写，内核只负责比较和阻塞。
```
<!-- after -->
```c
long airi_thread_join(long tid, long *result) { // 内核在输出前核对完整long跨度。
    return thread_call(41, (uint64_t)tid, (uintptr_t)result, 0); // 未完成时调用链真正阻塞，不由库忙轮询。
}
long airi_thread_id(void) { // 当前x86-64适配直接读取内核建立的TCB，不缓存fork会复制的旧TID。
    const struct airi_runtime_tcb *control; // 此指针只在当前有效FS布局下借用，不跨执行者保存。
    __asm__ volatile("movq %%fs:0,%0" : "=r"(control) :: "memory"); // 读取self用户VA，继续由当前用户页表翻译。
    if (control->version != AIRI_RUNTIME_VERSION || !control->tid) // 旧版保留头可退回内核查询，不把0当真实身份。
        return thread_call(42, 0, 0, 0); // 后备仍返回内核真实值，不更新或信任用户头进行授权。
    return control->tid; // 有效头只做用户态读，fork/exec/worker更新由内核负责。
}
long airi_futex_wait(uint32_t *word, uint32_t expected, unsigned ticks) { // 不改errno，返回自有内核原始结果。
    return thread_call(43, (uintptr_t)word, expected, ticks); // 条件字由调用者以原子操作读写，内核只负责比较和阻塞。
```

<!-- replace: labs/airios/include/airi_threads.h -->
<!-- before -->
```c
#endif // C直接声明。
long airi_thread_create(airi_thread_function function, void *argument); // 仅主执行者创建，正TID或原始负错误，不修改errno。
long airi_thread_join(long tid, long *result); // 仅主执行者收取同组一次结果，NULL可丢弃，0成功或负错误。
long airi_thread_id(void); // 同进程PID相同，每个执行者的TID不同。
long airi_futex_wait(uint32_t *word, uint32_t expected, unsigned ticks); // 私有键、四字节对齐，有限PIT预算；0通知、-11值不符、-110到期。
long airi_futex_wake(uint32_t *word, unsigned count); // 至多唤醒十六个同进程已登记者，不修改字值，不累计通知。
#ifdef __cplusplus // 结束链接语言范围。
```
<!-- after -->
```c
#endif // C直接声明。
long airi_thread_create(airi_thread_function function, void *argument); // 仅主执行者创建，正TID或原始负错误，不修改errno。
long airi_thread_join(long tid, long *result); // 仅主执行者收取同组一次结果，NULL可丢弃，0成功或负错误。
long airi_thread_id(void); // 有效静态TLS头直接提供当前TID，旧版本退回42号；同进程PID仍共享。
long airi_futex_wait(uint32_t *word, uint32_t expected, unsigned ticks); // 私有键、四字节对齐，有限PIT预算；0通知、-11值不符、-110到期。
long airi_futex_wake(uint32_t *word, unsigned count); // 至多唤醒十六个同进程已登记者，不修改字值，不累计通知。
#ifdef __cplusplus // 结束链接语言范围。
```

42号接口继续从running->tid返回，新增真实查询日志。这样观察不依赖“缓存命中次数”一类用户自报计数：每条THREAD_ID_QUERY确实经过内核分派。TCB版本有效的数百次身份读取不应增加该记录。普通互斥的unlock仍会执行44号通知，这次只消除身份查询，尚未让整个锁完全没有系统调用。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
        return return_worker(f); // 不关闭进程共享FD或异步owner。
    else if (f->rax == 41) // 主执行者收取一次同组工作结果。
        return join_worker(f); // 未完成时真正阻塞于原frame。
    else if (f->rax == 42) // 执行身份与共享PID分别查询。
        f->rax = running->tid; // 同组工作者的TID各自不同。
    else if (f->rax == 43) { // 自有私有futex等待，超时单位为名义PIT tick，不是Linux ABI。
        uint32_t value; // 本次比较取得四字节数值副本，不跨阻塞保留HHDM指针。
        uint64_t now = airi_timer_ticks(); // 截止只计算一次，不因轮询延长。
```
<!-- after -->
```c
        return return_worker(f); // 不关闭进程共享FD或异步owner。
    else if (f->rax == 41) // 主执行者收取一次同组工作结果。
        return join_worker(f); // 未完成时真正阻塞于原frame。
    else if (f->rax == 42) { // 原始查询保留为参考与版本后备，不读取用户TCB授权。
        f->rax = running->tid; // 内核身份始终来自真实正在运行的执行者。
        airi_log("[THREAD_ID_QUERY] pid=%u tid=%u", current->pid, running->tid); // 普通系统调用中记录真实陷入，不能用用户缓存计数冒充。
    }
    else if (f->rax == 43) { // 自有私有futex等待，超时单位为名义PIT tick，不是Linux ABI。
        uint32_t value; // 本次比较取得四字节数值副本，不跨阻塞保留HHDM指针。
        uint64_t now = airi_timer_ticks(); // 截止只计算一次，不因轮询延长。
```

<a id="runtime-identity-observation"></a>
## 先用一次原始查询，校准后面的用户读取

新建identity.c。inspect读取当前头，并恰好调用一次原始42号作参考；随后128次普通身份读取和八次局部不争用互斥，应都复用这份已发布的身份。不能只比较两次同一个快速函数，因为同一份旧值可以连续错两次。

先写声明、控制头读取、原始调用和inspect。原始调用也用于6号fork，不经过airi_fork包装。局部互斥放在当前调用栈上，没有其他执行者竞争，单独检验它没有隐藏的身份查询。

<!-- write: labs/airios/identity.c -->
```c
#include <airi/runtime_abi.h> // 内核和用户约定的16字节运行头。
#include <airi_threads.h> // 快速身份与工作者生命周期。
#include <airi_mutex.h> // 不争用锁也应复用本地身份读取。
#include <airi_libc.h> // 真实进程、PIT和串口调用。
static _Thread_local volatile unsigned local = 42; // fork复制运行值，exec和新worker恢复声明初值。
static void require(int condition, const char *message) { // 失败终止所属真实进程组。
    if (!condition) { airi_puts(message); airi_puts("\n"); airi_exit(84); } // 不让错误路径输出成功标记。
}
static struct airi_runtime_tcb *control(void) { // 只在内核建立的静态TLS布局下取头。
    struct airi_runtime_tcb *value; // 指向当前执行者的用户映射，不跨执行者保存。
    __asm__ volatile("movq %%fs:0,%0" : "=r"(value) :: "memory"); // 已有self槽提供TCB自身用户VA。
    return value; // 后续字段仍通过当前用户页表访问。
}
static long raw(long number) { // 绕开库包装，用实际内核查询校准身份和fork。
    __asm__ volatile("int $0x80" : "+a"(number) : "D"(0ull), "S"(0ull), "d"(0ull) : "memory", "cc"); // 参数为0，编号42查询或6复制进程。
    return number; // 参考值由内核真实运行者产生。
}
static long inspect(void) { // 每次恰做一次原始42号，其他身份读取应留在用户态。
    struct airi_runtime_tcb *value = control(); // 当前有效TP由内核在发布执行者以前建立。
    long actual = raw(42); // 内核日志记录真实查询，不能只用用户计数推断没陷入。
    require(value->self == (uintptr_t)value && value->version == AIRI_RUNTIME_VERSION && value->tid == (uint32_t)actual, "published TCB identity"); // 未更新的fork/exec候选0会在这里失败。
    for (unsigned i = 0; i < 128; ++i) require(airi_thread_id() == actual, "fast identity stable"); // 应只有TCB读，不重复42号查询。
    airi_mutex own = AIRI_MUTEX_INITIALIZER; // 当前调用栈独有，不制造另一项竞争。
    for (unsigned i = 0; i < 8; ++i) require(airi_mutex_lock(&own, 0) == 0 && airi_mutex_unlock(&own) == 0, "mutex uses local identity"); // unlock仍调用44，不能把整个锁宣称为零陷入。
    return actual; // 后续用独立原始参考比较，不自证同一缓存。
}
```

worker从声明值42开始，经inspect取得自己的TID，然后把local改为200加自己的序号。在纯用户循环中反复读快速身份和真实TLS，至少跨四个全局PIT事件；函数不以yield替代抢占。最终宿主还要检查每位worker自己的用户tick至少为2。

<!-- append: labs/airios/identity.c -->
```c
static long worker(void *argument) { // 原始声明值与本次内核TID都须属于新执行者。
    unsigned index = (unsigned)(uintptr_t)argument; // 由主执行者提供0、1或2。
    require(local == 42, "worker fresh TLS"); // 不继承主执行者改成99的运行值。
    long identity = inspect(); // 该worker恰有一次真实参考查询。
    require(identity != airi_getpid(), "worker TID differs from shared PID"); // 同进程身份与执行者身份分别判断。
    local = 200 + index; // 每个实际TLS副本保留不同观察值。
    uint64_t start = airi_ticks(); // 后面的循环必须跨真实PIT窗口。
    do { // 不用主动yield代替身份隔离验证。
        for (unsigned i = 0; i < 20000; ++i) // 在用户态反复读运行头和真实TLS。
            if (airi_thread_id() != identity || local != 200 + index) return -211; // 换执行者后不能读到另一人的头或变量。
    } while (airi_ticks() - start < 4); // 至少四个全局事件，每位worker的用户tick另由宿主日志核对。
    return identity; // join保留完整long，便于对应实际创建TID。
}
static int named(const char *value, const char *expected) { // 启动参数只包含初始目录名称。
    size_t bytes = strlen(expected); // 先取得固定短名称的长度。
    return strlen(value) == bytes && !memcmp(value, expected, bytes); // 不借宿主字符串库。
}
```

主入口区分普通目录名与identity-reset。重置入口应看见local=42、头版本1和仍等于孩子PID的主TID；普通入口先校准主执行者，再把自己的版本临时改成0，迫使一次真实后备查询，随后恢复版本1。此时还没有worker，不与别人并发修改同一份头。

<!-- append: labs/airios/identity.c -->
```c
int main(int argc, char **argv) { // exec前后使用同一ELF的不同目录名。
    require(argc == 1, "identity argv"); // 最小启动协议仍是单项程序名。
    if (named(argv[0], "identity-reset")) { // 成功exec重新建立TLS与运行头，PID仍为原孩子。
        require(local == 42 && inspect() == airi_getpid(), "exec fresh TLS and retained PID"); // 不能把候选的0或旧运行值123带入新main。
        airi_puts("[IDENTITY_EXEC_OK] tls=42 tcb=current version=1\n"); // 原始参考查询与字段均已经检查。
        return 0; // 父wait取得成功结果。
    }
    long parent = inspect(); // 主执行者版本头在首次进入用户前即有真实TID。
    require(parent == airi_getpid(), "primary TID"); // 主执行者初始TID等于PID。
    struct airi_runtime_tcb *value = control(); // 本阶段只在无worker时修改自己的版本，观察后立即恢复。
    value->version = 0; // 模拟旧版保留头，不破坏self或映射。
    require(airi_thread_id() == parent, "old-version fallback"); // 这次必须真的进入42号，而不是返回0或伪造值。
    value->version = AIRI_RUNTIME_VERSION; // 后续再次使用用户态快路径。
```

接着把local改为99，直接原始fork。孩子看到普通值99，但inspect的原始参考和头TID都必须是孩子身份，不能是父值。孩子再写123并exec到同一ELF的reset名称，重新取得42。父wait以后仍看到99和原来的主身份，不能因相同TP数值就误以为共享了物理内容。

<!-- append: labs/airios/identity.c -->
```c
    local = 99; // fork应继承这份运行值，但运行头TID必须改变。
    long child = raw(6); // 直接系统调用，证明身份更新不依赖库的fork回调。
    require(child >= 0, "raw fork"); // 失败不进入孩子逻辑。
    if (!child) { // 新地址空间保留调用继续点和普通TLS运行值。
        require(local == 99 && inspect() == airi_getpid() && airi_thread_id() != parent, "raw fork child identity"); // 只有TCB身份被内核改为孩子。
        local = 123; // 与随后exec的声明初值区别。
        airi_exec("identity-reset"); // 成功进入新main，保留同一孩子PID。
        require(0, "identity exec returned"); // 返回只可能是失败。
    }
    int status; // 与工作函数long返回值分开。
    require(airi_waitpid(child, &status) == child && !status && local == 99 && airi_thread_id() == parent, "parent TLS unchanged"); // 子写入不污染父头和变量。
```

最后两位worker交替跨PIT运行，join返回值分别等于它们真实TID。收拢后再创建第三位，让同一个槽及用户页缓存被复用；新头和普通TLS必须重新初始化。三个TID不复用，槽地址允许复用。

<!-- append: labs/airios/identity.c -->
```c
    long first = airi_thread_create(worker, (void *)(uintptr_t)0), second = airi_thread_create(worker, (void *)(uintptr_t)1), result; // 两条计算执行者交替取得PIT。
    require(first > 0 && second > 0, "identity workers"); // 不用单执行者代替隔离。
    require(airi_thread_join(first, &result) == 0 && result == first && airi_thread_join(second, &result) == 0 && result == second, "identity worker results"); // 返回值对应真实TID。
    long reused = airi_thread_create(worker, (void *)(uintptr_t)2); // 相同槽重用时必须重新写新身份。
    require(reused > 0 && reused != first && reused != second && airi_thread_join(reused, &result) == 0 && result == reused, "identity reused slot"); // 不能沿用原槽TCB中的旧TID。
    airi_puts("[IDENTITY_USER_OK] tcb=version1 fast=local fork=raw exec=fresh workers=3 reused=fresh fallback=queried\n"); // 所有真实身份路径完成后才输出。
    return 0; // 用户shell收取本条命令成功。
}
```

<a id="runtime-identity-build"></a>
## 将同一ELF分别用于普通入口和exec重建

追加Makefile与images.S，再注册identity、identity-reset。两份名称指向同一静态ELF，main按当前argv[0]选择观察分支；没有借宿主进程替代fork。TCB仍占16字节，原来最大TLS模板另占一页控制头的边界保持不变。

<!-- append: Makefile -->
```makefile

# 身份观察链接自有线程与锁库，PT_TLS由本地运行库anchor显式保留。
$(OUT)/user/identity.elf: $(USER_COMMON) $(OUT)/user/threads-lib.o $(OUT)/user/thread-entry.o $(OUT)/user/mutex-lib.o $(OUT)/user/identity.o labs/airios/program.ld
# 同一ELF供普通名称和exec重置名称，运行头仍由内核逐次发布。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(filter %.o,$^) -o $@
# 打包等待实际链接完成。
$(OUT)/labs/airios/images.o: $(OUT)/user/identity.elf
# 本配置文件路径交给只读incbin。
$(OUT)/labs/airios/images.o: CPPFLAGS += -DAIRI_IDENTITY_ELF='"$(OUT)/user/identity.elf"'
```

<!-- append: labs/airios/images.S -->
```asm

.balign 8 /* TCB身份和生命周期观察的真实用户ELF。 */
.global airi_identity_elf_start, airi_identity_elf_end /* 文件范围由初始程序目录借用。 */
airi_identity_elf_start: /* 同一文件支持普通入口与exec重建观察。 */
.incbin AIRI_IDENTITY_ELF /* 当前配置的静态程序，不是宿主模拟查询。 */
airi_identity_elf_end: /* 输入的排他右端。 */
```

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
extern const uint8_t airi_process_elf_start[], airi_process_elf_end[]; // 新的fork/wait和抢占工作负载。
extern const uint8_t airi_vector_elf_start[], airi_vector_elf_end[]; // 显式偏移向量正常与坏范围观察。
extern const uint8_t airi_malloc_elf_start[], airi_malloc_elf_end[]; // 用户对象分配与页面缓存的独立ELF。
extern const uint8_t airi_heap_threads_elf_start[], airi_heap_threads_elf_end[]; // 同一arena并发分配、跨执行者归还与快照。
extern const uint8_t airi_mutex_elf_start[], airi_mutex_elf_end[]; // 原子所有权与futex等待组合的真实用户观察。
extern const uint8_t airi_futex_elf_start[], airi_futex_elf_end[]; // 私有内存等待与通知的真实用户程序。
```
<!-- after -->
```c
extern const uint8_t airi_process_elf_start[], airi_process_elf_end[]; // 新的fork/wait和抢占工作负载。
extern const uint8_t airi_vector_elf_start[], airi_vector_elf_end[]; // 显式偏移向量正常与坏范围观察。
extern const uint8_t airi_malloc_elf_start[], airi_malloc_elf_end[]; // 用户对象分配与页面缓存的独立ELF。
extern const uint8_t airi_identity_elf_start[], airi_identity_elf_end[]; // 原始fork、exec与工作槽复用中的本地TID观察。
extern const uint8_t airi_heap_threads_elf_start[], airi_heap_threads_elf_end[]; // 同一arena并发分配、跨执行者归还与快照。
extern const uint8_t airi_mutex_elf_start[], airi_mutex_elf_end[]; // 原子所有权与futex等待组合的真实用户观察。
extern const uint8_t airi_futex_elf_start[], airi_futex_elf_end[]; // 私有内存等待与通知的真实用户程序。
```

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    {"vector", airi_vector_elf_start, airi_vector_elf_end}, // 聚合、分散、短前缀与错误原子性。
    {"heap", airi_malloc_elf_start, airi_malloc_elf_end}, // 单线程用户对象与错误路径。
    {"heap-reset", airi_malloc_elf_start, airi_malloc_elf_end}, // 同一ELF新BSS和按需arena。
    {"heap-threads", airi_heap_threads_elf_start, airi_heap_threads_elf_end}, // 显式持内部锁yield的用户观察程序。
    {"mutex", airi_mutex_elf_start, airi_mutex_elf_end}, // 临界区、错误所有者、截止与非robust寿命。
    {"futex", airi_futex_elf_start, airi_futex_elf_end}, // 相等比较与登记、超时及进程私有键。
```
<!-- after -->
```c
    {"vector", airi_vector_elf_start, airi_vector_elf_end}, // 聚合、分散、短前缀与错误原子性。
    {"heap", airi_malloc_elf_start, airi_malloc_elf_end}, // 单线程用户对象与错误路径。
    {"heap-reset", airi_malloc_elf_start, airi_malloc_elf_end}, // 同一ELF新BSS和按需arena。
    {"identity", airi_identity_elf_start, airi_identity_elf_end}, // 版本化运行头和原始查询对照。
    {"identity-reset", airi_identity_elf_start, airi_identity_elf_end}, // 成功exec后沿用PID并重建运行头。
    {"heap-threads", airi_heap_threads_elf_start, airi_heap_threads_elf_end}, // 显式持内部锁yield的用户观察程序。
    {"mutex", airi_mutex_elf_start, airi_mutex_elf_end}, // 临界区、错误所有者、截止与非robust寿命。
    {"futex", airi_futex_elf_start, airi_futex_elf_end}, // 相等比较与登记、超时及进程私有键。
```

自动会话限定本条命令以后恰好七次实际42号查询：主执行者一次参考加一次版本后备，孩子exec前后各一次，三位worker各一次。再按真实pid/tid对应，不固定本轮恰好出现的数值。大量快速调用及互斥调用不应增加这些数量。

<!-- replace: scripts/shell_test.py -->
<!-- before -->
```python
                assert '[FUTEX_WAIT]' in heap_threads # 观察编译明确在内部持锁yield，必须实际进入竞争等待。
                heap_owner=re.search(r'\[EXEC_COMMIT\] pid=(\d+) program=heap-threads ',heap_threads) # 固定本条命令的真实进程身份。
                assert heap_owner and len(re.findall(r'\[THREAD_CREATE\] pid='+heap_owner.group(1)+r' tid=',heap_threads))==4 and len(re.findall(r'\[THREAD_RETURN\] pid='+heap_owner.group(1)+r' tid=',heap_threads))==4 # join可以立即成功，不能强求特定阻塞时序。
                for repetition in range(2): # 同一shell连续完成两次线程命令，上一组退出回收后应能重新创建。
                    offset=len(text); wire.sendall(b'threads\n'); text=wait('[SHELL_COMMAND_DONE] name=threads code=0',offset) # 真正同地址空间执行，不能由fork复制或宿主线程模拟。
                    thread_output=text[offset:] # 所有身份与标记均限定在这条命令之后。
```
<!-- after -->
```python
                assert '[FUTEX_WAIT]' in heap_threads # 观察编译明确在内部持锁yield，必须实际进入竞争等待。
                heap_owner=re.search(r'\[EXEC_COMMIT\] pid=(\d+) program=heap-threads ',heap_threads) # 固定本条命令的真实进程身份。
                assert heap_owner and len(re.findall(r'\[THREAD_CREATE\] pid='+heap_owner.group(1)+r' tid=',heap_threads))==4 and len(re.findall(r'\[THREAD_RETURN\] pid='+heap_owner.group(1)+r' tid=',heap_threads))==4 # join可以立即成功，不能强求特定阻塞时序。
                offset=len(text); wire.sendall(b'identity\n'); text=wait('[SHELL_COMMAND_DONE] name=identity code=0',offset) # 运行头必须在raw fork、exec和工作者复用时更新。
                identity_output=text[offset:] # 只核对这一条命令的真实查询。
                assert '[IDENTITY_USER_OK] tcb=version1 fast=local fork=raw exec=fresh workers=3 reused=fresh fallback=queried' in identity_output and '[IDENTITY_EXEC_OK] tls=42 tcb=current version=1' in identity_output # 数据字段与原始查询均由guest比较。
                identity_owner=re.search(r'\[EXEC_COMMIT\] pid=(\d+) program=identity ',identity_output) # 主执行者身份是本次命令真实PID。
                assert identity_owner # 不能拿其他程序的查询日志校准。
                queries=re.findall(r'\[THREAD_ID_QUERY\] pid=(\d+) tid=(\d+)',identity_output) # 原始42号入口记录实际陷入，不依赖用户计数。
                assert len(queries)==7 and queries.count((identity_owner.group(1),identity_owner.group(1)))==2 # 主参考加版本后备两次、孩子前后两次、三位worker各一次。
                identity_workers=re.findall(r'\[THREAD_CREATE\] pid='+identity_owner.group(1)+r' tid=(\d+) ',identity_output) # 三次创建中最后一次复用已join槽。
                assert len(identity_workers)==3 and len(set(identity_workers))==3 # 身份不复用，槽可以复用。
                for tid in identity_workers: # 每位跨PIT的计算执行者都只做一次参考42号查询。
                    assert queries.count((identity_owner.group(1),tid))==1 # 大量快速读取和互斥调用不增加查询陷入。
                    returned=re.search(r'\[THREAD_RETURN\] pid='+identity_owner.group(1)+r' tid='+tid+r' user_ticks=(\d+)',identity_output) # 独立用户tick来自真实IRQ。
                    assert returned and int(returned.group(1))>=2 # 不把主动yield当抢占隔离证据。
                for repetition in range(2): # 同一shell连续完成两次线程命令，上一组退出回收后应能重新创建。
                    offset=len(text); wire.sendall(b'threads\n'); text=wait('[SHELL_COMMAND_DONE] name=threads code=0',offset) # 真正同地址空间执行，不能由fork复制或宿主线程模拟。
                    thread_output=text[offset:] # 所有身份与标记均限定在这条命令之后。
```

<!-- replace: scripts/shell_test.py -->
<!-- before -->
```python
                offset=len(text); wire.sendall(b'echo after-shell\n'); text=wait('[CONSOLE_UPDATE] view=0 commands=2 page=0',offset) # 回到内核后输入仍完整。
                assert 'after-shell' in text[offset:] # 不只是退出而丢了后续UART。
                result={'passed':True,'ring3_shell':True,'ram_file_and_device_readback':True,'other_user_during_cq_wait':True,'qmp_iops':1,'error_exec_survived':True,'returned_to_monitor':True,'orphan_rounds':24,'memory_reuse_rounds':128,'capacity_rounds':4,'capacity_pages':1024,'simd_preempt_fork_exec':True,'fs_base_preempt_fork_exec':True,'compiler_static_tls':True,'posix_errno_io_subset':True,'positioned_vectors':True} # 只发布已观察事实。
                result.update(shared_malloc=True,malloc_workers=4,malloc_rounds=96,owned_mutex=True,mutex_critical_updates=64,private_futex=True,futex_registrations=8,user_heap=True,shared_executions=True,thread_commands=2,worker_capacity=8,worker_reuse_rounds=24) # 关键字把实际已验收字段加入结果字典，不替代上面的逐命令检查。
                result_path.write_text(json.dumps(result,indent=2)+'\n'); print('PASS user shell: alloc/file/async wait/other task/CQ/readback, failed exec, return to monitor') # 阶段演示实际完成。
                return result # 总入口可收集。
            finally: # 包括断言、超时和QEMU错误。
```
<!-- after -->
```python
                offset=len(text); wire.sendall(b'echo after-shell\n'); text=wait('[CONSOLE_UPDATE] view=0 commands=2 page=0',offset) # 回到内核后输入仍完整。
                assert 'after-shell' in text[offset:] # 不只是退出而丢了后续UART。
                result={'passed':True,'ring3_shell':True,'ram_file_and_device_readback':True,'other_user_during_cq_wait':True,'qmp_iops':1,'error_exec_survived':True,'returned_to_monitor':True,'orphan_rounds':24,'memory_reuse_rounds':128,'capacity_rounds':4,'capacity_pages':1024,'simd_preempt_fork_exec':True,'fs_base_preempt_fork_exec':True,'compiler_static_tls':True,'posix_errno_io_subset':True,'positioned_vectors':True} # 只发布已观察事实。
                result.update(runtime_tcb_identity=True,identity_reference_queries=7,shared_malloc=True,malloc_workers=4,malloc_rounds=96,owned_mutex=True,mutex_critical_updates=64,private_futex=True,futex_registrations=8,user_heap=True,shared_executions=True,thread_commands=2,worker_capacity=8,worker_reuse_rounds=24) # 关键字把实际已验收字段加入结果字典，不替代上面的逐命令检查。
                result_path.write_text(json.dumps(result,indent=2)+'\n'); print('PASS user shell: alloc/file/async wait/other task/CQ/readback, failed exec, return to monitor') # 阶段演示实际完成。
                return result # 总入口可收集。
            finally: # 包括断言、超时和QEMU错误。
```

<!-- run -->
```sh
./airios build  # 链接静态TLS运行库和身份观察，保持16字节TCB预算
```

<!-- run -->
```sh
./airios test  # 观察原始fork、exec、工作槽复用及既有容量和TLS回归
```

<a id="runtime-identity-results"></a>
## 身份改变的地方，与不必陷入的地方

真实guest输出：

```text
[IDENTITY_EXEC_OK] tls=42 tcb=current version=1
[IDENTITY_USER_OK] tcb=version1 fast=local fork=raw exec=fresh workers=3 reused=fresh fallback=queried
[SHELL_COMMAND_DONE] name=identity code=0
```

交互运行`./airios run`，在内核提示符输入`run sh`，再输入`identity`。沿FORK的父子关系区分七次THREAD_ID_QUERY：孩子exec前后PID不变，三个worker与父共享PID却分别有自己的TID。看到新PID或新槽，回到对应发布位置核对头值；不能只看最终成功字符串。

本轮实际反汇编中，airi_thread_id先读FS:0，比较0xc偏移的版本，再从0x8偏移加载TID并返回。只有版本不符或零身份才跳到装入42并执行int80的分支。可用当前LLVM目录的llvm-objdump对build/normal/user/threads-lib.o反汇编核对；普通mov加载和特权陷入是不同操作，不因都发生在一个C函数中就具有相同成本。

下面命令完整复现这次对象观察。with_name只更换工具路径末尾的文件名，str将Path转成单个参数；没有把程序文本拼成Shell命令。

<!-- run -->
```sh
PYTHONPATH=scripts python3 - <<'PYCODE'  # 使用当前工程选择的LLVM安装目录
from pathlib import Path  # 保留父目录，只替换可执行文件名
import subprocess  # 按参数数组运行反汇编器并检查退出码
from tools import tool  # 与构建使用同一份LLVM选择规则
objdump = Path(tool('clang', 'llvm')).with_name('llvm-objdump')  # with_name不改变编译器所在目录，得到相邻工具路径
subprocess.run([str(objdump), '-d', 'build/normal/user/threads-lib.o'], check=True)  # -d反汇编目标对象，不运行其中的用户代码
PYCODE
```

库选择带来了新的PT_TLS，内核仍使用已验证的模板、指针偏移和页面归属。旧无TLS程序不选线程库时继续FS=0；版本化头不是让任意FS地址都能安全解引用。头值是用户可读副本，原始查询和资源授权仍依据内核控制块，二者职责不交换。

[Clang used属性](https://clang.llvm.org/docs/AttributeReference.html#used)说明定义保留及ELF链接回收的边界。[Linux gettid](https://man7.org/linux/man-pages/man2/gettid.2.html)返回内核线程身份，[pthread_self](https://man7.org/linux/man-pages/man3/pthread_self.3.html)返回线程库的pthread_t，后者不能按数值直接当内核TID。AiriOS这里是自有运行库接口和固定TCB布局，不用这项观察推断Linux接口的实现成本。远端Linux对照仍未验证，最近尝试为K53的SSH255。

下一步需要判断是否真的有人正在争取锁，再减少无人等待时的通知。身份读取已经留在用户态，并不自动改变unlock仍执行44号的事实；等待数量、到期撤销与释放之间还需要完整的无丢通知协议。
