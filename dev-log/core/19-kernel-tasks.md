# K19 · 两个任务沿各自的栈暂停和继续

K18 的时钟已经能进入内核，IRQ 目前只计数，不会替正在执行的代码选择下一个任务。本章先让任务自己调用 `airi_task_yield()` 交出执行权：任务 1 输出一次，暂时停下；任务 2 输出一次，再让其他任务继续。以后回来时，两者都应从自己上次暂停的位置接着执行。

重新调用一遍 `worker()` 不能产生这样的继续：它会从入口重新开始，而我们要留下当前轮数、局部值和中间函数的返回位置。两个任务因此各有自己的栈。任务 1 的局部值 107 和任务 2 的 207 分别留在各自现场中，三轮输出应形成 `121212`。启动任务也保留原来的栈，最后从那里回收两个工作任务的栈，使空闲页数恢复。

全部任务仍共享当前内核 CR3，没有引入用户态、不同进程地址空间或 ELF 装载。图形不是前置。先扩展内存区域接口，再写切换汇编和任务控制，最后接入构建、启动与观察脚本；这些新调用到章末才闭合，中途先不构建。

<a id="stack-region-owner"></a>
## 暂停的调用链需要一直保留自己的栈

一个工作任务使用四页，即 16 KiB 栈。它的虚拟地址要连续，push、pop 和局部变量才能按同一个连续范围访问；底下的四个物理页可以分别向页池借出。映射允许内核写入、禁止执行（NX），前面额外留一页不映射，用来暴露越过栈边界的写入。

现在需要记住的不只是栈顶数值，还有这组映射与物理页归谁回收。`airi_kernel_region` 保存虚拟起点、物理页数组和实际持有的 count，任务槽持有这个句柄。初始必须清零；不能按值复制后让两份句柄都释放同一组页面。

创建时一页一页借出并映射，成功才登记到 count。例如要求四页、只完成两页便耗尽，句柄中的两项正好说明需要拆掉哪些映射、归还哪些页。本次借到却未能映射的页先单独归还；已经成功的部分按句柄回滚。四页全部就绪以后才清零整个范围，交给任务使用，不把半成品栈发布出去。

回收之前，调用者必须已经离开该区域。任务仍在自己的栈上执行释放函数时，也还在借用这份内存；所以工作任务结束后先换到其他栈，由启动任务回收。释放函数先核对所有映射是否仍指向句柄记录的物理页，全部匹配才逆序解除映射、失效翻译，再归还数据页。当前单 CPU、IRQ 不操作这些映射，验证与回收之间没有其他修改者；匹配失败保留原状态，内部不变量被破坏则停止，不能假装已经归还。

**局部更新 `include/airi/memory.h`。** 统计快照继续只提供数值；新增 region 句柄供内核自己管理资源。

依次找到以下旧窗口，各用对应新文本完整替换；其他定义保留。

<!-- replace: include/airi/memory.h -->
<!-- before -->
```c
void *airi_kmalloc(size_t bytes); // 小对象来自已映射内核堆，不承诺完整 libc malloc ABI。
bool airi_kfree(void *pointer); // 结束对象借用，backing 页仍由内核堆持有。
#endif // 结束包含保护。
```
<!-- after -->
```c
void *airi_kmalloc(size_t bytes); // 小对象来自已映射内核堆，不承诺完整 libc malloc ABI。
bool airi_kfree(void *pointer); // 结束对象借用，backing 页仍由内核堆持有。
#define AIRI_REGION_PAGES 16u // 当前有界内核区域最多持有十六个数据页。
struct airi_kernel_region { // 所有权句柄必须在使用前清零，不能按值复制后重复释放。
    uint64_t virtual_address; // 所有数据页连续映射的内核虚拟起点。
    uint64_t physical[AIRI_REGION_PAGES]; // 逐页记录实际借出的物理页，便于精确回滚。
    unsigned count; // 只有已经成功映射的页才计入所有权。
};
bool airi_memory_region_create(struct airi_kernel_region *region, uint64_t address, unsigned count); // 建立新的有界RW/NX内核区域，不覆盖已有映射。
bool airi_memory_region_free(struct airi_kernel_region *region); // 所有借用结束后先解除映射，再归还数据页。
#endif // 结束包含保护。
```

`memory.c` 仍持有页池、页表与固定堆。原先的初始化和观察保留，新增的区域函数复用这些对象，不再建立一套分配器。创建前先检查整个目标虚拟范围尚未映射；有冲突就原样拒绝，避免覆盖其他服务的内存。

**局部更新 `kernel/mm/memory.c`。** 重点新增末尾的 `airi_memory_region_create/free`，原页池、页表与堆实现保持。

在 `kernel/mm/memory.c` 末尾追加区域操作，原有定义全部保留：

<!-- append: kernel/mm/memory.c -->
```c

bool airi_memory_region_create(struct airi_kernel_region *region, uint64_t address, unsigned count) { // 调用者清零句柄并保证没有并发操作这个对象。
    if (!region || region->count || !count || count > AIRI_REGION_PAGES || address % AIRI_PAGE_SIZE || address < 0xffff800000000000ull || address > UINT64_MAX - count * AIRI_PAGE_SIZE) // 只接受新的有界内核范围。
        return false; // 参数错误不改变任何页或映射。
    for (unsigned i = 0; i < count; ++i) // 先验证整个范围没有被其他服务映射。
        if (airi_vm_lookup(&kernel_space, address + i * AIRI_PAGE_SIZE, NULL, NULL)) // 覆盖已有对象会破坏所有权。
            return false; // 尚未借出页面，可以原样拒绝。
    region->virtual_address = address; // 保存本次区域的虚拟位置。
    while (region->count < count) { // 每成功一页才增加持有数量。
        uint64_t physical = airi_page_alloc(&pages); // 数据页的所有权先归本次创建操作。
        if (!physical) // 页池耗尽，没有本次页可归还。
            break; // 统一回滚已经成功的页面。
        if (!airi_vm_map(&kernel_space, address + region->count * AIRI_PAGE_SIZE, physical, AIRI_VM_WRITE)) { // 映射为supervisor读写NX。
            airi_page_free(&pages, physical); // 本次数据页未接入，立即归还。
            break; // 之前已经接入的页面由统一出口解除。
        }
        region->physical[region->count++] = physical; // 记录物理地址后才增加有效数量。
    }
    if (region->count != count) { // 不提供只建成一部分的区域。
        if (region->count && !airi_memory_region_free(region)) // 只有实际持有页时才回滚；内部映射必须仍匹配句柄。
            airi_panic("cannot roll back partial kernel region"); // 不清掉无法归还的所有权记录后假装无泄漏。
        airi_memset(region, 0, sizeof(*region)); // 失败句柄恢复无所有权状态。
        return false; // 调用者没有取得可访问区域。
    }
    airi_memset((void *)(uintptr_t)address, 0, count * AIRI_PAGE_SIZE); // 完整映射后清除旧页面内容。
    return true; // 句柄拥有数据页及对应映射，不能复制出第二个释放者。
}
```

create 成功后，句柄的 count 对应完整栈；失败已经按相同记录回滚。free 先逐项核对映射与物理页一致，再逆序解除和归还；这仍要求调用者已离开目标栈，函数不能替正在执行的调用结束借用。

**继续追加区域归还：**

<!-- append: kernel/mm/memory.c -->
```c
bool airi_memory_region_free(struct airi_kernel_region *region) { // 调用前必须离开该区域，包括不能还在它的任务栈上执行。
    if (!region || !region->count || region->count > AIRI_REGION_PAGES) // 空句柄和损坏容量不能参与回收。
        return false; // 不猜测曾经持有的页面。
    for (unsigned i = 0; i < region->count; ++i) { // 先完整验证所有映射仍对应句柄记录。
        uint64_t physical; // 保存软件翻译结果。
        if (!airi_vm_lookup(&kernel_space, region->virtual_address + i * AIRI_PAGE_SIZE, &physical, NULL) || physical != region->physical[i]) // 不归还已被别处替换的范围。
            return false; // 验证失败不拆任何映射。
    }
    while (region->count) { // 由末尾逆序解除映射。
        --region->count; // 当前槽位仍有真实页面所有权。
        if (!airi_vm_unmap(&kernel_space, region->virtual_address + region->count * AIRI_PAGE_SIZE) || !airi_page_free(&pages, region->physical[region->count])) // 先断链接和失效，之后才归还数据页。
            airi_panic("kernel region ownership changed during release"); // 内部不变量破坏时不继续双重释放。
    }
    airi_memset(region, 0, sizeof(*region)); // 成功后句柄不能再次释放同一组页面。
    return true; // 当前没有遗留的数据页借用。
}
```

<a id="switch-call-abi"></a>
## 保存旧栈位置，再从另一份调用链返回

先看两个都已经运行过的任务。假设任务 2 上次调用 yield 时暂停在 `airi_context_switch()` 中，那次 call 的返回地址和需保留的寄存器仍在任务 2 的栈里。现在任务 1 也调用 yield，调度器选择任务 2，切换函数就需要留下任务 1 的位置，再找回任务 2 的位置。

第一个实参 `&tasks[1].rsp` 指向旧任务控制块里的保存位置；第二个实参 `tasks[2].rsp` 则是目标任务上次留下的栈顶值。普通 call 已把返回地址压到任务 1 的栈上，汇编再压入 RBP、RBX、R12–R15，把此时的 RSP 写进第一个实参指向的字段。此时只是保存好了旧位置，CPU 仍在执行任务 1 的切换函数。

把第二个实参装入处理器 RSP，才真正改变接下来读哪一份栈。后面的六次 pop 和 ret 都读取任务 2 的现场；ret 取得任务 2 那次 call 的返回地址，继续的是任务 2 以前尚未返回的调用。任务 1 本次调用仍留在自己的旧栈中，直到未来重新选中任务 1、换回它的 RSP 才返回。暂停期间的局部值和中间返回地址不能回收。

这里只保存六个通用寄存器，是因为 yield 主动通过普通 C 调用进入。编译器知道调用后 RAX、RCX 等调用者保存寄存器可能改变，需要跨调用存活的值会安排在栈或被调用者保存寄存器中。汇编保存 RBP、RBX、R12–R15 并记录 RSP，就继续履行这份约定；返回地址已经由 call 保存。K14 的异常可以发生在任意指令处，编译器没有提前安排一次普通调用的机会，因此必须保存全部通用寄存器，两个范围不能互换。

**新建 `kernel/arch/x86_64/switch.S`。** 后半段坏栈入口只用于独立故障镜像，正常任务交接不执行它。

<!-- write: kernel/arch/x86_64/switch.S -->
```asm
.section .text /* 任务交接代码属于内核可执行段。 */
.global airi_context_switch /* C用普通调用约定进入。 */
.type airi_context_switch,@function /* 标记函数便于反汇编。 */
airi_context_switch: /* rdi指向旧任务的RSP槽，rsi是新任务已保存的RSP。 */
    pushq %rbp /* 普通C调用需要保留的寄存器之一。 */
    pushq %rbx /* 保存被调用者负责保留的RBX。 */
    pushq %r12 /* 保存R12。 */
    pushq %r13 /* 保存R13。 */
    pushq %r14 /* 保存R14。 */
    pushq %r15 /* 最后保存的R15位于最低地址。 */
    movq %rsp, (%rdi) /* 保存的是暂停的调用链，不是刚执行完函数后的栈。 */
    movq %rsi, %rsp /* 此后所有pop和ret都从另一个任务栈取值。 */
    popq %r15 /* 恢复目标任务的被调用者保存寄存器。 */
    popq %r14 /* 恢复R14。 */
    popq %r13 /* 恢复R13。 */
    popq %r12 /* 恢复R12。 */
    popq %rbx /* 恢复RBX。 */
    popq %rbp /* 恢复目标调用链的帧指针。 */
    ret /* 弹出目标任务之前的返回地址，首次则进入trampoline。 */
.size airi_context_switch,.-airi_context_switch /* 记录符号长度。 */
.global airi_stack_guard_fault /* 作者故障镜像用坏栈触发双重故障。 */
.type airi_stack_guard_fault,@function /* 这是不返回的特定演示函数。 */
airi_stack_guard_fault: /* rdi为映射栈下方保护页的上边界。 */
    movq %rdi, %rsp /* 把栈顶置于边界，下一次压栈会写进未映射页。 */
    pushq $0 /* PF也无法在坏栈上压现场，CPU改用DF的IST1。 */
    ud2 /* 若错误地没有触发保护，仍明确失败而不继续用坏栈运行。 */
.size airi_stack_guard_fault,.-airi_stack_guard_fault /* 记录演示范围。 */
.section .note.GNU-stack,"",@progbits /* 任务栈不需要执行权限。 */
```

已有调用链能够恢复，但新任务尚未运行，栈里还没有那次 call 的返回地址。下一步需要让同一组六次 pop 和 ret 也能进入第一次执行的任务。

<a id="task-first-stack"></a>
## 给首次任务准备恢复步骤能够使用的现场

`spawn()` 从栈顶预制六个初始寄存器槽和一个 `trampoline` 入口。第一次切入时，pop 取得六个初始值，ret 取得 trampoline 地址，执行便进入一个新的 C 函数。初始寄存器值设零，因为此前没有旧值需要恢复；trampoline 再根据当前任务槽调用真正的 entry。

C 入口还必须满足 SysV 的 `RSP % 16 == 8`。这里初始栈顶按 16 字节对齐，额外预留一个模拟调用者返回槽，再放 trampoline 地址与六个寄存器槽，总计 64 字节。恢复消耗六次 pop 的 48 字节和 ret 的 8 字节，进入 trampoline 时还留下八字节，正好满足入口对齐。额外槽设零，trampoline 承诺不返回，所以它不会作为正常返回地址使用。

首次进入与再次恢复共用切换代码，任务控制还要决定哪一份现场可以运行。接口只提供本章主动交接与演示，不向其他模块承诺完整的进程或线程创建 API。

**新建 `include/airi/task.h`：**

<!-- write: include/airi/task.h -->
```c
#ifndef AIRI_TASK_H // 教学任务接口只声明一次。
#define AIRI_TASK_H // 包含保护。
void airi_task_yield(void); // 当前内核任务主动交接CPU，尚不是时钟抢占。
void airi_task_demo(void); // 两个独立栈任务轮流执行并回收其映射。
#endif // 结束包含保护。
```

## 沿 0→1→2→0 看全局编号与各自的局部值

演示有三个静态任务槽。0 是当前启动任务，借用原启动栈；1、2 是工作任务，各自持有新建区域和相同的 worker 入口。创建前工作槽为 UNUSED，栈与 entry 全部准备好以后才设 RUNNABLE，调度器才能选它。

0 第一次调用 yield，局部 `old` 保存 0，自己的 RUNNING 改回 RUNNABLE。调度器从下一个编号开始查一圈，只选 RUNNABLE；找到 1 后，先把目标状态改成 RUNNING、将全局 `current` 写成 1，再切换栈。任务 1 第一次经预制现场进入 trampoline，任务 2 之后也按同样方式开始。

任务 1 让出给 2，任务 2 再让出给 0，最早属于 0 的 switch 调用才返回。这时 current 已由后来的调度重新写成 0，而 old 随 0 自己的栈或保留寄存器一直保存，因此返回处应满足 `current == old`。不是改完 current 后旧调用立即运行下一行；切栈已经把它暂停了。每个任务恢复时都检查这项对应，避免把某任务的栈当成另一槽的现场。

`worker` 的 id、round 和 local 也随各自调用链保留。local 用 volatile 放在真实栈上，任务 1 的 107 不应被任务 2 的 207 覆盖。全局 trace 只记录工作任务输出，不记录启动任务的内部轮转，所以最终仍是六项 `121212`。

entry 返回时，trampoline 先把本槽设为 DONE，再 yield 离开这份栈。DONE 不会重新变成 RUNNABLE，也不会被再次选中；启动任务等两个工作槽都 DONE，才在自己的原栈上回收它们。状态生命周期因而是 UNUSED→RUNNABLE→RUNNING，普通 yield 回到 RUNNABLE，结束则转 DONE。回收后控制块里残留的旧 RSP 已过期，不能再恢复这些调用链。

**新建 `kernel/task/task.c`。** 任务槽保存现场和资源，启动任务统一核对轨迹与回收数量。

<!-- write: kernel/task/task.c -->
```c
#include <airi/task.h> // 主动切换和完整演示入口。
#include <airi/memory.h> // 每个任务栈拥有自己的映射区域。
#include <airi/kernel.h> // 日志与停止接口。
#define TASKS 3u // 一个启动任务，加两个演示任务。
#define STACK_BASE 0xffffe00000000000ull // 任务栈使用独立高地址分支。
#define STACK_PAGES 4u // 每个任务16KiB，前面额外留一页不映射。
enum task_state { UNUSED, RUNNABLE, RUNNING, DONE }; // 每个槽的生命周期状态。
struct task { // 全部状态在内核静态数组，任务不会持有局部控制块。
    uint64_t rsp; // 普通C调用被暂停时保存的栈指针。
    struct airi_kernel_region stack; // 只由此任务槽持有的backing与映射。
    void (*entry)(void); // 第一次被调度时执行的普通C函数。
    enum task_state state; // 只有RUNNABLE槽可以被调度器选中。
};
static struct task tasks[TASKS]; // 槽0借用原启动栈，不创建或释放它。
static unsigned current, trace_count, trace[6]; // 当前任务编号和实际交错顺序。
extern void airi_context_switch(uint64_t *old_rsp, uint64_t new_rsp); // 汇编只保存普通C ABI规定的被调用者寄存器。
extern _Noreturn void airi_stack_guard_fault(uint64_t guard_top); // 作者故障入口不能返回正常栈。
void airi_task_yield(void) { // 只有正常内核调用主动进入，不从时钟IRQ调用。
    unsigned old = current; // 这个局部值随旧任务自己的栈/保留寄存器一起暂停。
    if (tasks[old].state == RUNNING) // 正在结束的DONE任务不能再成为可运行。
        tasks[old].state = RUNNABLE; // 普通yield仍允许未来被选中。
    unsigned next = old; // 暂未找到其他候选。
    for (unsigned n = 1; n <= TASKS; ++n) { // 最多查一圈，包含必要时选择原任务。
        unsigned candidate = (old + n) % TASKS; // 轮流选择下一个槽。
        if (tasks[candidate].state == RUNNABLE) { // UNUSED和DONE不能被错误唤醒。
            next = candidate; // 保存找到的可运行任务。
            break; // 首个候选实现本节round-robin顺序。
        }
    }
    if (tasks[next].state != RUNNABLE) // 当前已DONE且其他任务也不可运行时，不存在合法返回栈。
        airi_panic("task scheduler has no runnable continuation"); // 不用已结束的调用链继续执行。
    tasks[next].state = RUNNING; // 在转交栈前记录目标状态。
    current = next; // 目标trampoline或恢复点会读取这个编号。
    if (next != old) // 没有其他任务时无需保存并恢复同一个栈。
        airi_context_switch(&tasks[old].rsp, tasks[next].rsp); // ret将在目标栈的调用链中继续。
    if (current != old) // 只有旧任务真正被重新选中才可能从它的switch调用返回。
        airi_panic("task resumed with mismatched owner"); // 明确拒绝混用任务栈与控制块。
}
```

yield 已能保存旧调用并从目标调用继续。首次任务还没有旧返回地址，下面的 trampoline 与 spawn 共同准备它：spawn 先建完整四页栈，再按六次 pop/ret 的消费顺序摆槽；状态最后才变成 RUNNABLE。

**继续追加首次入口和创建：**

<!-- append: kernel/task/task.c -->
```c
static _Noreturn void trampoline(void) { // 首次任务不是从普通call进入，而由预制栈的ret进入。
    tasks[current].entry(); // 运行这个槽持有的普通C函数。
    tasks[current].state = DONE; // 函数返回表示任务结束，不能再参与候选。
    airi_task_yield(); // 切到另一个仍存活的栈，启动任务稍后负责回收。
    airi_panic("completed task unexpectedly resumed"); // DONE任务若再次被选中，明确失败。
}
static bool spawn(unsigned id, void (*entry)(void)) { // 由启动任务在首次调度前建立全部状态。
    uint64_t address = STACK_BASE + id * (STACK_PAGES + 1) * 4096ull + 4096; // 每个槽前面一页保持未映射保护。
    if (id == 0 || id >= TASKS || !entry || tasks[id].state != UNUSED) // 不覆盖启动任务或仍被占用的槽。
        return false; // 不创建任何新栈。
    if (!airi_memory_region_create(&tasks[id].stack, address, STACK_PAGES)) // 四页虚拟连续，物理可以分散。
        return false; // 区域接口已经回滚半成品。
    uint64_t *sp = (uint64_t *)(uintptr_t)(address + STACK_PAGES * 4096ull); // 栈从已映射范围的高端向下生长。
    *--sp = 0; // 模拟C函数的调用者返回槽；trampoline承诺不返回。
    *--sp = (uintptr_t)trampoline; // 六次pop之后的ret会取出这个首次入口。
    for (unsigned i = 0; i < 6; ++i) // RBP、RBX、R12–R15首次没有旧值。
        *--sp = 0; // 预制出与switch保存部分相同的六个槽。
    tasks[id].rsp = (uintptr_t)sp; // 指向第一个即将pop的R15槽。
    tasks[id].entry = entry; // 控制块拥有函数身份，参数本节由current编号区分。
    tasks[id].state = RUNNABLE; // 全部栈内容完成后才允许被调度器发现。
    return true; // 此时尚未运行任务任何一条指令。
}
```

两个槽都可以持有同一个 worker 函数地址，身份却来自各自首次进入时的 current。worker 把它保存到本次 id，local 和 round 随独立栈保留；每次 yield 真正返回后，再用这些旧值检查恢复到的是同一个任务。

**继续追加工作任务：**

<!-- append: kernel/task/task.c -->
```c
static void worker(void) { // 两个槽使用同一段代码，但各自有独立调用链和局部变量。
    unsigned id = current; // 本函数暂停后应仍保持这个身份。
    volatile uint64_t local = id * 100 + 7; // 强制局部值留在当前任务栈，证明不是共享一个局部对象。
    for (unsigned round = 0; round < 3; ++round) { // 每个任务输出三次并主动让出。
        if (trace_count >= 6) // 防止错误调度使轨迹数组越界。
            airi_panic("task trace overflow"); // 具体错误停止而不是覆盖控制块。
        trace[trace_count++] = id; // 保存实际执行身份后增加有效轨迹数。
        airi_log("[TASK_STEP] id=%u round=%u stack_local=%llu", id, round, (unsigned long long)local); // 输出当前独立栈的局部值。
        airi_task_yield(); // 保存当前调用链，再从别的任务栈返回。
        if (current != id || local != id * 100 + 7) // 恢复必须保持身份与栈上内容。
            airi_panic("task stack local changed across yield"); // 不能只凭日志交错宣布切换成功。
    }
}
```

worker 入口已有了，启动任务现在创建槽 1、2，并留住槽 0 的原调用链。循环主动交接到两个工作任务；它们 DONE 后不再被选中，只有回到槽 0 才核对轨迹并回收栈。stack-guard 配置在真正交接前进入独立故障路径，普通配置继续三轮。

**最后追加创建、交错和离栈回收的演示入口：**

<!-- append: kernel/task/task.c -->
```c
void airi_task_demo(void) { // 启动任务收拢运行和回收，后续仍回串口循环。
    struct airi_memory_stats before, after; // 用页池与堆快照检查任务资源是否回收。
    airi_memory_snapshot(&before); // 创建任务栈之前的基准。
    tasks[0].state = RUNNING; // 当前C调用链属于槽0的原始启动栈。
    if (!spawn(1, worker) || !spawn(2, worker)) // 两个任务都必须拥有完整独立栈。
        airi_panic("cannot create kernel task stacks"); // 不运行部分准备的演示。
#ifdef AIRI_TEST_STACK_GUARD // 独立故障镜像完整演示坏栈上的DF接管。
    airi_log("[STACK_GUARD_PROBE] top=0x%llx ist=1", (unsigned long long)tasks[1].stack.virtual_address); // 保护页上边界即栈映射起点。
    airi_stack_guard_fault(tasks[1].stack.virtual_address); // 正常栈被故意丢弃，必须由DF的独立栈报告。
#endif // 普通镜像继续真正的任务交错。
    while (tasks[1].state != DONE || tasks[2].state != DONE) // 有任务尚未结束就继续主动交接。
        airi_task_yield(); // 启动任务本身也参与轮转，为最终回收保留活调用链。
    for (unsigned i = 0; i < 6; ++i) // 检查实际轨迹，不只要求输出六行。
        if (trace_count != 6 || trace[i] != 1 + i % 2) // 期望两个独立栈按1、2、1、2、1、2继续。
            airi_panic("kernel tasks did not alternate"); // 状态机或上下文错位时拒绝完成。
    for (unsigned id = 1; id < TASKS; ++id) // 当前已回槽0，两个目标栈均不再执行。
        if (!airi_memory_region_free(&tasks[id].stack)) // 先解除映射和空页表，再归还数据页。
            airi_panic("cannot reclaim completed task stack"); // 不保留隐藏泄漏。
    airi_memory_snapshot(&after); // 回收后的当前实际统计。
    if (after.pages_free != before.pages_free || after.heap_free != before.heap_free) // 任务栈和临时页表必须全部恢复。
        airi_panic("completed kernel tasks leaked memory"); // 内核演示不应吃掉后续主线的容量。
    airi_log("[TASK_DEMO_OK] order=121212 separate_stacks=2 reclaimed=1"); // 只有切换、局部值、顺序与回收全部成立才输出。
}
```

<a id="guard-double-fault"></a>
## 栈已越界时，故障报告为什么也要换栈

正常任务交接已经有了独立栈。保护页用于发现超出栈范围的写入，但发现故障本身也需要栈：普通页故障 PF 到来时，CPU 还要压入入口现场。若 RSP 已指向未映射处，这一步也失败，就会形成双重故障 DF。

K14 已为向量 8 配置 IST1。DF 通过这项配置换到内核静态独立栈，才能进入 C、报告并停止。栈保护并不保证原调用还能继续，本章故障演示就是验证失去原栈后报告通路仍可使用。

`stack-guard` 镜像在两组栈已建立后，把 RSP 设到栈映射起点，再 push 八字节；栈向低地址增长，写入落到前面的未映射保护页。它故意放弃旧调用栈，不尝试恢复；串口必须出现向量 8 与 PANIC，不能继续 READY。正常镜像不执行故障入口，故障宏只用于独立产物目录。

**局部更新 `Makefile`。** guest 加入任务 C 文件和 switch 汇编，stack-guard 配置定义故障宏。

依次找到以下旧窗口，各用对应新文本完整替换；其他定义保留。

<!-- replace: Makefile -->
<!-- before -->
```make
SOURCES := kernel/main.c kernel/boot/limine.c kernel/boot/info.c \
    kernel/lib/base.c kernel/lib/runtime.c kernel/arch/x86_64/serial.c \
    kernel/console/console.c kernel/arch/x86_64/input.c kernel/arch/x86_64/cpu.c kernel/arch/x86_64/trap.c kernel/mm/pages.c kernel/mm/memory.c kernel/arch/x86_64/paging.c kernel/mm/heap.c kernel/arch/x86_64/timer.c
# 只有 desktop- 前缀目标才读取图形支线自己的构建片段。
ifneq ($(filter desktop-%,$(PROFILE)),)
```
<!-- after -->
```make
SOURCES := kernel/main.c kernel/boot/limine.c kernel/boot/info.c \
    kernel/lib/base.c kernel/lib/runtime.c kernel/arch/x86_64/serial.c \
    kernel/console/console.c kernel/arch/x86_64/input.c kernel/arch/x86_64/cpu.c kernel/arch/x86_64/trap.c kernel/mm/pages.c kernel/mm/memory.c kernel/arch/x86_64/paging.c kernel/mm/heap.c kernel/arch/x86_64/timer.c kernel/task/task.c
# 只有 desktop- 前缀目标才读取图形支线自己的构建片段。
ifneq ($(filter desktop-%,$(PROFILE)),)
```

继续替换下一处：

<!-- replace: Makefile -->
<!-- before -->
```make
CASE := $(patsubst desktop-%,%,$(PROFILE))
# patsubst把每个.c相对路径改为当前OUT下的.o，汇编入口对象另加入。
OBJECTS := $(patsubst %.c,$(OUT)/%.o,$(SOURCES)) $(OUT)/kernel/arch/x86_64/entry.o $(OUT)/kernel/arch/x86_64/traps.o
# 交叉目标为无宿主ELF；C11/O2/调试信息/警告作为错误。freestanding与fno-builtin关闭宿主假设；禁栈保护和PIC/PIE；不使用red zone与浮点寄存器；kernel地址模型；MMD/MP生成头文件依赖。
CFLAGS := --target=x86_64-unknown-none-elf -std=c11 -O2 -g -Wall -Wextra -Werror \
```
<!-- after -->
```make
CASE := $(patsubst desktop-%,%,$(PROFILE))
# patsubst把每个.c相对路径改为当前OUT下的.o，汇编入口对象另加入。
OBJECTS := $(patsubst %.c,$(OUT)/%.o,$(SOURCES)) $(OUT)/kernel/arch/x86_64/entry.o $(OUT)/kernel/arch/x86_64/traps.o $(OUT)/kernel/arch/x86_64/switch.o
# 交叉目标为无宿主ELF；C11/O2/调试信息/警告作为错误。freestanding与fno-builtin关闭宿主假设；禁栈保护和PIC/PIE；不使用red zone与浮点寄存器；kernel地址模型；MMD/MP生成头文件依赖。
CFLAGS := --target=x86_64-unknown-none-elf -std=c11 -O2 -g -Wall -Wextra -Werror \
```

继续替换下一处：

<!-- replace: Makefile -->
<!-- before -->
```make
CPPFLAGS += -DAIRI_TEST_NO_MEMMAP
endif # 结束 missing-memory 的编译条件
# 时钟故障镜像仅供K18作者演示超时，不进入正常启动配置集合。
ifeq ($(CASE),timer-masked)
```
<!-- after -->
```make
CPPFLAGS += -DAIRI_TEST_NO_MEMMAP
endif # 结束 missing-memory 的编译条件
# 栈保护故障镜像让未映射栈触发DF，由IST1报告。
ifeq ($(CASE),stack-guard)
CPPFLAGS += -DAIRI_TEST_STACK_GUARD # 此镜像不能发布READY。
endif # 结束栈故障配置。
# 时钟故障镜像仅供K18作者演示超时，不进入正常启动配置集合。
ifeq ($(CASE),timer-masked)
```

## 把任务接在内存与时钟就绪之后

任务需要现成的页池和映射，保护故障也需要已安装的异常入口。main 保持已有初始化，在时钟就绪后执行任务演示。演示返回表示已经重新回到启动栈；随后才进入 console，而不是在某个工作栈上继续整个内核。

**局部更新 `kernel/main.c`：**

依次找到以下旧窗口，各用对应新文本完整替换；其他定义保留。

<!-- replace: kernel/main.c -->
<!-- before -->
```c
#include <airi/timer.h> // 内存建立后开始真实设备中断。
#include <airi/memory.h> // 通过内存服务建立页池、页表、堆并查询快照。
```
<!-- after -->
```c
#include <airi/task.h> // 两个独立栈内核任务主动交接并回收。
#include <airi/timer.h> // 内存建立后开始真实设备中断。
#include <airi/memory.h> // 通过内存服务建立页池、页表、堆并查询快照。
```

继续替换下一处：

<!-- replace: kernel/main.c -->
<!-- before -->
```c
    airi_memory_init(info); // 顺序建立页池、自有映射与固定内核堆，完成本阶段演示。
    airi_timer_demo(); // 从PIT/PIC收到真实IRQ0，hlt后恢复到正常上下文。
    airi_console_init(&console, info);  // 初始化内核静态控制台，借用整个启动期间有效的 info。
    console.read_memory = airi_memory_snapshot; // 文本查询只读取数值，不持有分配器内部对象。
```
<!-- after -->
```c
    airi_memory_init(info); // 顺序建立页池、自有映射与固定内核堆，完成本阶段演示。
    airi_timer_demo(); // 从PIT/PIC收到真实IRQ0，hlt后恢复到正常上下文。
    airi_task_demo(); // 当前只在正常内核上下文主动切换，IRQ不抢占内核任务。
    airi_console_init(&console, info);  // 初始化内核静态控制台，借用整个启动期间有效的 info。
    console.read_memory = airi_memory_snapshot; // 文本查询只读取数值，不持有分配器内部对象。
```

启动观察分别处理两种结果。正常镜像必须核对任务交错与回收标记，再等待 READY；stack-guard 是预期停止的独立镜像，必须看到真实 DF 记录。不能把“进程仍运行”或只有预先打印的探测文字当作故障已经正确交付。

**局部更新 `scripts/boot.py`。** 正常路径核对 TASK_DEMO_OK，保护页路径核对向量 8 和 PANIC。

依次找到以下旧窗口，各用对应新文本完整替换；其他定义保留。

<!-- replace: scripts/boot.py -->
<!-- before -->
```python
    graphics = profile.startswith('desktop-') and case != 'no-framebuffer'  # 有绘图实现且设备存在才应生成画面。
    expected='[AIRI_PANIC] required Limine memory map is missing' if case=='missing-memory' else '[AIRI_P2_READY]'  # 正常配置等完整内存阶段，P1标记仍供早期输入观察。
    if case == 'trap-fatal': # 未处理异常镜像必须进入具体停止路径。
        expected = '[AIRI_PANIC] unhandled CPU exception' # 不以 QEMU 存活推断故障已处理。
    started=time.monotonic()  # 单调秒数作为经过时间起点，不受系统时钟调整影响。
```
<!-- after -->
```python
    graphics = profile.startswith('desktop-') and case != 'no-framebuffer'  # 有绘图实现且设备存在才应生成画面。
    expected='[AIRI_PANIC] required Limine memory map is missing' if case=='missing-memory' else '[AIRI_P2_READY]'  # 正常配置等完整内存阶段，P1标记仍供早期输入观察。
    if case in ('trap-fatal', 'stack-guard'): # 未处理异常镜像必须进入具体停止路径。
        expected = '[AIRI_PANIC] unhandled CPU exception' # 不以 QEMU 存活推断故障已处理。
    started=time.monotonic()  # 单调秒数作为经过时间起点，不受系统时钟调整影响。
```

继续替换下一处：

<!-- replace: scripts/boot.py -->
<!-- before -->
```python
                complete_lines = text.splitlines(keepends=True)  # 保留行结束符，完整记录与写到一半的标记可区分。
                if any(line.endswith('\n') and line.startswith(expected) for line in complete_lines):  # 生成器逐行检查，至少一个完整且以预期标记开头才成立。
                    if case in ('missing-memory', 'trap-fatal'):  # 失败配置只允许具体拒绝记录，不能同时宣布启动完成。
                        assert '[AIRI_P1_READY]' not in text  # 错误路径不得出现成功标记。
                    else:  # 按当前profile的另一条完整验证路径处理。
                        assert '[TIMER_DEMO_OK]' in text # 时钟必须在真实IRQ唤醒后完成。
                        assert '[AIRI_P2_READY]' in text # 当前主线还必须完成页池、自有页表和堆。
```
<!-- after -->
```python
                complete_lines = text.splitlines(keepends=True)  # 保留行结束符，完整记录与写到一半的标记可区分。
                if any(line.endswith('\n') and line.startswith(expected) for line in complete_lines):  # 生成器逐行检查，至少一个完整且以预期标记开头才成立。
                    if case in ('missing-memory', 'trap-fatal', 'stack-guard'):  # 失败配置只允许具体拒绝记录，不能同时宣布启动完成。
                        if case == 'stack-guard': # 坏栈必须真的转到双重故障的独立栈。
                            assert '[STACK_GUARD_PROBE]' in text and '[EXCEPTION] vector=8 error=0x0 ' in text # 不能把任意PANIC冒充保护页演示。
                        assert '[AIRI_P1_READY]' not in text  # 错误路径不得出现成功标记。
                    else:  # 按当前profile的另一条完整验证路径处理。
                        assert '[TASK_DEMO_OK]' in text # 两个独立栈必须交错并全部回收。
                        assert '[TIMER_DEMO_OK]' in text # 时钟必须在真实IRQ唤醒后完成。
                        assert '[AIRI_P2_READY]' in text # 当前主线还必须完成页池、自有页表和堆。
```

统一入口继续使用各配置自己的构建与启动检查。日常完整回归现在有六个配置，新增 stack-guard 的预期故障；它的停止不会被当成正常镜像成功进入交互循环。

**局部更新 `scripts/manage.py`：**

依次找到以下旧窗口，各用对应新文本完整替换；其他定义保留。

<!-- replace: scripts/manage.py -->
<!-- before -->
```python
from console_test import check_console  # 复用真实键盘和UART输入验证。

PROFILES=('normal','no-framebuffer','missing-memory','edit-experiment','trap-fatal')  # 元组固定五种构建配置，CLI choices 与 test 遍历共用。
def run(cmd):  # 所有统一入口外部命令都从同一根目录执行。
    subprocess.run(cmd,cwd=ROOT,check=True)  # 非零状态转成异常传至入口。
```
<!-- after -->
```python
from console_test import check_console  # 复用真实键盘和UART输入验证。

PROFILES=('normal','no-framebuffer','missing-memory','edit-experiment','trap-fatal','stack-guard')  # 元组固定六种构建配置，CLI choices 与 test 遍历共用。
def run(cmd):  # 所有统一入口外部命令都从同一根目录执行。
    subprocess.run(cmd,cwd=ROOT,check=True)  # 非零状态转成异常传至入口。
```

继续替换下一处：

<!-- replace: scripts/manage.py -->
<!-- before -->
```python
    parser.add_argument('--app',choices=('terminal','files','editor','tasks','system','settings','viewer'),default='editor')  # 主机窗口默认Editor，只接受已知应用名。
    parser.add_argument('--state',choices=['normal','empty','loading','error','offline'],default='normal')  # 状态名限定到共享视图已有的五种状态。
    parser.add_argument('--profile',choices=PROFILES,default='normal')  # 镜像默认普通配置，只接受固定五项。
    parser.add_argument('--phase',choices=('p1','p2'),default='p1') # Linux阶段显式选择，默认保持P1。
    parser.add_argument('--host',default='airi@ssh.airi-dynamic.top')   # 个人Linux主机可以显式替换，不涉及公司机器。
```
<!-- after -->
```python
    parser.add_argument('--app',choices=('terminal','files','editor','tasks','system','settings','viewer'),default='editor')  # 主机窗口默认Editor，只接受已知应用名。
    parser.add_argument('--state',choices=['normal','empty','loading','error','offline'],default='normal')  # 状态名限定到共享视图已有的五种状态。
    parser.add_argument('--profile',choices=PROFILES,default='normal')  # 镜像默认普通配置，只接受固定六项。
    parser.add_argument('--phase',choices=('p1','p2'),default='p1') # Linux阶段显式选择，默认保持P1。
    parser.add_argument('--host',default='airi@ssh.airi-dynamic.top')   # 个人Linux主机可以显式替换，不涉及公司机器。
```

继续替换下一处：

<!-- replace: scripts/manage.py -->
<!-- before -->
```python
        run([sys.executable,str(ROOT/'scripts/kernel_debug.py'),'--host',args.host])  # 远端GDB连接本地QEMU。
        return  # 调试记录完成后结束本动作。
    if args.action=='test':  # 完整验证串联主机算法、可选前端、五种启动和三条设备输入。
        run(make_command(target='host-test'))  # 编译同一纯C实现并运行带ASan/UBSan的主机边界检查。
        if args.graphics:  # 普通 test 不导入或验证前端。
```
<!-- after -->
```python
        run([sys.executable,str(ROOT/'scripts/kernel_debug.py'),'--host',args.host])  # 远端GDB连接本地QEMU。
        return  # 调试记录完成后结束本动作。
    if args.action=='test':  # 完整验证串联主机算法、可选前端、六种启动和三条设备输入。
        run(make_command(target='host-test'))  # 编译同一纯C实现并运行带ASan/UBSan的主机边界检查。
        if args.graphics:  # 普通 test 不导入或验证前端。
```

继续替换下一处：

<!-- replace: scripts/manage.py -->
<!-- before -->
```python
        results=[]  # 列表依次保存各配置的实际启动结果。
        profiles = [('desktop-' + p) if args.graphics else p for p in PROFILES]  # 呈现方式和故障场景正交。
        for profile in profiles:  # 按固定五种配置独立编译与启动。
            build(profile)  # 生成对应配置的ELF和镜像，产物目录彼此隔离。
            if profile.endswith('normal'):  # 普通目标额外检查ELF结构。
                inspect_elf(profile)  # 保存机器、类型、依赖与符号报告。
            results.append(check(profile))  # 等待该配置完成标记并保存结果，任一失败异常中止。
        (ROOT/'artifacts'/('desktop-boot-tests.json' if args.graphics else 'boot-tests.json')).write_text(json.dumps(results,indent=2)+'\n')  # 五种启动结果编码为JSON数组。
        prefix = 'desktop-' if args.graphics else ''  # 本轮输入验证选择同一呈现方式。
        console_results=[check_console(prefix+'normal'),check_console(prefix+'no-framebuffer'),check_console(prefix+'normal',uart_input=False)]  # 顺序验证normal UART、无帧缓冲UART与normal PS/2三条真实输入路径。
```
<!-- after -->
```python
        results=[]  # 列表依次保存各配置的实际启动结果。
        profiles = [('desktop-' + p) if args.graphics else p for p in PROFILES]  # 呈现方式和故障场景正交。
        for profile in profiles:  # 按固定六种配置独立编译与启动。
            build(profile)  # 生成对应配置的ELF和镜像，产物目录彼此隔离。
            if profile.endswith('normal'):  # 普通目标额外检查ELF结构。
                inspect_elf(profile)  # 保存机器、类型、依赖与符号报告。
            results.append(check(profile))  # 等待该配置完成标记并保存结果，任一失败异常中止。
        (ROOT/'artifacts'/('desktop-boot-tests.json' if args.graphics else 'boot-tests.json')).write_text(json.dumps(results,indent=2)+'\n')  # 六种启动结果编码为JSON数组。
        prefix = 'desktop-' if args.graphics else ''  # 本轮输入验证选择同一呈现方式。
        console_results=[check_console(prefix+'normal'),check_console(prefix+'no-framebuffer'),check_console(prefix+'normal',uart_input=False)]  # 顺序验证normal UART、无帧缓冲UART与normal PS/2三条真实输入路径。
```

## 运行后对照交错、局部值和资源回收

**在学习目录根目录运行：**

<!-- run -->
```sh
./airios test  # 实际运行主机边界、六个启动配置、三条设备输入。
```

正常结果应有六条 TASK_STEP：id 在 1、2 间交替，每个任务 round 从 0 到 2，local 分别保持 107、207。既有检查点记录中，随后出现 `[TASK_DEMO_OK] order=121212 separate_stacks=2 reclaimed=1` 并继续 READY。free 与 heap_free 回到创建之前，包含新分支空页表的回收；只有日志交错而资源未恢复，仍不能算本次生命周期完成。

stack-guard 的既有记录中，栈映射起点为 `0xffffe00000006000`，push 写到下面八字节 `0xffffe00000005ff8`，CR2 记录该地址，最终报告 `vector=8 error=0x0` 并 PANIC。这组地址来自该检查点固定布局，故障 RIP 则会随源码布局移动，应配合同次 ELF 定位。记录证明 DF 的 IST 接管，没有证明普通 PF 能在坏栈上返回。

在 `airi_context_switch` 断点处，`x/8gx $rsp` 先显示 call 留下的返回位置；单步六次 push 后，处理器 RSP 已指向压好的现场；再执行 `movq %rsp,(%rdi)`，旧任务的 rsp 字段才更新为这个位置。换入新 RSP 后，同一个 `$rsp` 表达式读取的已经是另一块映射。第一次 ret 进入 trampoline，后续 ret 回到目标任务上次的 switch 返回位置。暂停时分别查看 `tasks[1].rsp` 与 `tasks[2].rsp`，可沿独立栈找到不同 local。

本次切换共享 CR3，只换执行现场，没有切换进程地址空间。它与用户空间 fiber 都包含保留调用链、换栈继续，但还不是 Linux 线程或 BOSS bthread 的完整实现。时钟仍在计数，IRQ 只改 ticks 并发送 EOI，不调用 yield；后续用户态抢占会在完整中断现场边界选任务，内核仍不可抢占。

后续等待机制还需要区分能运行与正在等待：谁保存继续位置、谁在事件发生后唤醒、谁在借用结束后回收资源。BOSS 的条件变量和 bthread 等待也需要这些关系；一个回调函数本身并没有描述执行栈与资源寿命。
