# K63 · 工作者也能创建，创建者结束不带走子执行者

K62 还把线程创建限制在主执行者。现在让worker调用同一入口，创建同进程的另一个worker。新执行者的内存和FD归进程，不归创建它的那条调用栈；创建者普通返回并被join以后，子执行者仍应有自己的内核栈、TLS和身份。

从 K62 的同一目录继续。本节放宽创建，join仍由主执行者统一收取。观察会先让创建者结束，复用它原来的槽及TLS位置，再让仍存活的子执行者恢复，验证它没有借用已经失效的创建者状态。对等join认领和死锁检测留到下一节，当前仍明确返回ENOTSUP。

<a id="nested-resource-owner"></a>
## current不变，running可以是任一创建者

K52 已把共享页表、FD和ELF模板放进process，把独立CPU现场、内核栈、TLS基址放进execution。主执行者创建时，current是所属进程，running是它自己的主execution；worker创建时，current仍是同一个进程，只是running换成这个worker。创建一个新execution不需要再复制process，也不需要借用创建者自己的栈作为新栈。

create_worker原本就从全局空闲工作槽挑选目标，使用目标槽号准备独立内核栈与用户栈，映射资源登记在current->memory。TLS从current保存的ELF模板重新建立，而FPU环境从当前running已经保存的状态复制，继承的是实际创建者。二者来源不同：普通TLS声明初值不能变成创建者改过的运行值。

因此先删除create_worker开头的主管理拒绝。准备失败仍在当前创建者的内核栈上回滚尚未运行的目标资源，不销毁调用者现场。最后的THREAD_CREATE增加creator=running->tid作观察；它不改变资源归属，也不产生“创建者退出就自动结束后代”的规则。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
        airi_memset(airi_memory_direct(physical), 0, 4096); // 每页起点已4096对齐，清完整缓存内容。
    }
}
static long create_worker(uint64_t entry, uint64_t function, uint64_t argument) { // 首版仅主执行者管理额外执行链。
    if (running != &current->execution) // 工作者不能递归创建，生命周期由主执行者统一收拢。
        return -16; // -EBUSY。
    if (!worker_code(entry) || !worker_code(function)) // 先验证两个真实代码地址，再分配任何内存。
        return -14; // -EFAULT，数据页或内核地址不能成为返回入口。
    if (next_pid == UINT32_MAX) // 与进程身份共享单调空间，不回绕重用旧TID。
```
<!-- after -->
```c
        airi_memset(airi_memory_direct(physical), 0, 4096); // 每页起点已4096对齐，清完整缓存内容。
    }
}
static long create_worker(uint64_t entry, uint64_t function, uint64_t argument) { // 同进程任一执行者可创建，新增资源仍由process统一持有。
    if (!worker_code(entry) || !worker_code(function)) // 先验证两个真实代码地址，再分配任何内存。
        return -14; // -EFAULT，数据页或内核地址不能成为返回入口。
    if (next_pid == UINT32_MAX) // 与进程身份共享单调空间，不回绕重用旧TID。
```

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
        execution->fs_base = tls + current->tls.pointer_offset; // TP仍按既有local-exec负偏移布局。

    }
    execution->initial = (struct airi_trap_frame){0}; // 首次工作入口没有继承主执行者的GPR。
    execution->initial.rip = entry; // 用户库汇编桥接器负责调用真正工作函数和提交返回值。
    execution->initial.rdi = function; // 桥接器的第一个寄存器参数是函数地址。
    execution->initial.rsi = argument; // 第二个参数是不由内核解引用的用户数值。
```
<!-- after -->
```c
        execution->fs_base = tls + current->tls.pointer_offset; // TP仍按既有local-exec负偏移布局。

    }
    execution->initial = (struct airi_trap_frame){0}; // 首次工作入口没有复制创建者GPR或用户调用栈。
    execution->initial.rip = entry; // 用户库汇编桥接器负责调用真正工作函数和提交返回值。
    execution->initial.rdi = function; // 桥接器的第一个寄存器参数是函数地址。
    execution->initial.rsi = argument; // 第二个参数是不由内核解引用的用户数值。
```

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    if (current->tls.memory_bytes) write_tcb(current, execution->fs_base, execution->tid); // 新槽和复用槽都在可运行前取得自己的身份。
    place_frame(execution); // 现场进入本执行者自己的内核栈。
    execution->state = USER_RUNNABLE; // 最后发布运行资格，IRQ此前只能看见空槽。
    airi_log("[THREAD_CREATE] pid=%u tid=%u slot=%u tp=0x%llx", current->pid, execution->tid, index, (unsigned long long)execution->fs_base); // 记录真实共享身份与本地寻址起点。
    return execution->tid; // 返回TID，仍不是pthread_t或Linux clone ABI。
failed: // 当前仍在主执行者栈上，可以立即收回尚未运行的工作者资源。
    while (current->memory.count > before) { // 只处理本次新增项，不能释放旧缓存页面。
        const struct user_page *page = owned_at(&current->memory, --current->memory.count); // 先退到最后一项有效所有权。
        if (!airi_vm_unmap(&current->memory.space, page->address) || !airi_memory_return_page(page->physical)) // 活动根先断映射，再归还数据页。
```
<!-- after -->
```c
    if (current->tls.memory_bytes) write_tcb(current, execution->fs_base, execution->tid); // 新槽和复用槽都在可运行前取得自己的身份。
    place_frame(execution); // 现场进入本执行者自己的内核栈。
    execution->state = USER_RUNNABLE; // 最后发布运行资格，IRQ此前只能看见空槽。
    airi_log("[THREAD_CREATE] pid=%u tid=%u slot=%u tp=0x%llx creator=%u", current->pid, execution->tid, index, (unsigned long long)execution->fs_base, running->tid); // 创建者身份只作观察，不把新执行者资源绑定到创建者寿命。
    return execution->tid; // 返回原始TID，不是pthread_create的0/正错误合同，也不是Linux clone ABI。
failed: // 当前仍在创建者的内核栈上，可以立即收回尚未运行的目标资源。
    while (current->memory.count > before) { // 只处理本次新增项，不能释放旧缓存页面。
        const struct user_page *page = owned_at(&current->memory, --current->memory.count); // 先退到最后一项有效所有权。
        if (!airi_vm_unmap(&current->memory.space, page->address) || !airi_memory_return_page(page->physical)) // 活动根先断映射，再归还数据页。
```

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    }
    trim_records(&current->memory); // 新增但未使用的尾部记录页一并收回。
    destroy_execution(execution); // 栈/FPU的成功部分可独立归还，不关闭进程共享资源。
    return -12; // -ENOMEM，主执行者和其他已运行工作者保持。
}
static struct airi_trap_frame *join_worker(struct airi_trap_frame *frame) { // 本轮只有主执行者能收取同组工作结果。
    if (running != &current->execution) // 不支持工作者之间的join图或自建管理树。
```
<!-- after -->
```c
    }
    trim_records(&current->memory); // 新增但未使用的尾部记录页一并收回。
    destroy_execution(execution); // 栈/FPU的成功部分可独立归还，不关闭进程共享资源。
    return -12; // -ENOMEM，调用者和其他已运行执行者保持。
}
static struct airi_trap_frame *join_worker(struct airi_trap_frame *frame) { // 本轮只有主执行者能收取同组工作结果。
    if (running != &current->execution) // 不支持工作者之间的join图或自建管理树。
```

<a id="nested-public-entry"></a>
## 包交接不变，去掉用户包装中的同一限制

原始airi_thread_create仍只把桥接器、long工作函数和参数交给39号，不复制创建者用户调用栈。pthread_create也仍用类型正确的start_packet：哪个执行者调用，它就先拥有这个包；创建失败自己free，成功后交给新worker。共享malloc已经支持多个执行者，不能为嵌套创建另写一份不受锁保护的分配器。

更新两个公开声明与原始包装的边界说明，再删除pthread_create中检查“self必须等于PID”的那一行。NULL属性、坏参数、资源失败和errno恢复都保持；pthread_join的主管理检查暂时保留。内核先允许，再让用户入口使用，不能只删包装返回而让底层仍报忙。

<!-- replace: labs/airios/include/airi_threads.h -->
<!-- before -->
```c
#ifdef __cplusplus // 后续C++调用使用相同C链接名。
extern "C" { // 不要求宿主线程库。
#endif // C直接声明。
long airi_thread_create(airi_thread_function function, void *argument); // 仅主执行者创建，正TID或原始负错误，不修改errno。
long airi_thread_join(long tid, long *result); // 仅主执行者收取同组一次结果，NULL可丢弃，0成功或负错误。
long airi_thread_id(void); // 有效静态TLS头直接提供当前TID，旧版本退回42号；同进程PID仍共享。
uint64_t airi_thread_syscalls(void); // 当前执行者累计用户请求数，含本次查询，fork/worker重新开始，exec保留。
```
<!-- after -->
```c
#ifdef __cplusplus // 后续C++调用使用相同C链接名。
extern "C" { // 不要求宿主线程库。
#endif // C直接声明。
long airi_thread_create(airi_thread_function function, void *argument); // 同进程任一执行者可创建，正TID或原始负错误，不修改errno。
long airi_thread_join(long tid, long *result); // 仅主执行者收取同组一次结果，NULL可丢弃，0成功或负错误。
long airi_thread_id(void); // 有效静态TLS头直接提供当前TID，旧版本退回42号；同进程PID仍共享。
uint64_t airi_thread_syscalls(void); // 当前执行者累计用户请求数，含本次查询，fork/worker重新开始，exec保留。
```

<!-- replace: labs/airios/libc/threads.c -->
<!-- before -->
```c
extern void airi_thread_entry(void); // 汇编桥接器被静态链接到当前用户可执行段。
static long thread_call(long number, uint64_t first, uint64_t second, uint64_t third) { // 与既有int80前三参数约定相同。
    __asm__ volatile("int $0x80" : "+a"(number) : "D"(first), "S"(second), "d"(third) : "memory", "cc"); // 内存clobber让调用边界不隐瞒共享读写。
    return number; // 负错误保持，不映射成尚未实现的pthread返回规则。
}
long airi_thread_create(airi_thread_function function, void *argument) { // 主执行者不复制自己的用户栈或TLS运行值。
    return thread_call(39, (uintptr_t)airi_thread_entry, (uintptr_t)function, (uintptr_t)argument); // 两个代码地址均由内核检查为用户RX。
}
long airi_thread_join(long tid, long *result) { // 内核在输出前核对完整long跨度。
```
<!-- after -->
```c
extern void airi_thread_entry(void); // 汇编桥接器被静态链接到当前用户可执行段。
static long thread_call(long number, uint64_t first, uint64_t second, uint64_t third) { // 与既有int80前三参数约定相同。
    __asm__ volatile("int $0x80" : "+a"(number) : "D"(first), "S"(second), "d"(third) : "memory", "cc"); // 内存clobber让调用边界不隐瞒共享读写。
    return number; // 保持原始负错误，正错误翻译由上层pthread适配器负责。
}
long airi_thread_create(airi_thread_function function, void *argument) { // 创建者不复制自己的用户栈或TLS运行值，新worker从映像模板建立。
    return thread_call(39, (uintptr_t)airi_thread_entry, (uintptr_t)function, (uintptr_t)argument); // 两个代码地址均由内核检查为用户RX。
}
long airi_thread_join(long tid, long *result) { // 内核在输出前核对完整long跨度。
```

<!-- replace: labs/airios/include/pthread.h -->
<!-- before -->
```c
#ifdef __cplusplus // C++调用同一C实现。
extern "C" { // 不链接宿主pthread。
#endif // C直接声明。
int pthread_create(pthread_t *thread, const pthread_attr_t *attributes, void *(*function)(void *), void *argument); // 当前仅主执行者创建，成功0并写身份，失败正错误且不改errno。
int pthread_join(pthread_t thread, void **result); // 当前仅主执行者收取，NULL丢弃结果；成功才写私人输出。
pthread_t pthread_self(void); // 当前有效运行头提供身份，不能跨进程当授权句柄。
int pthread_equal(pthread_t left, pthread_t right); // 相同非零，不同0，不将可移植比较写成数值猜测。
```
<!-- after -->
```c
#ifdef __cplusplus // C++调用同一C实现。
extern "C" { // 不链接宿主pthread。
#endif // C直接声明。
int pthread_create(pthread_t *thread, const pthread_attr_t *attributes, void *(*function)(void *), void *argument); // 同进程执行者可创建，成功0并写身份，失败正错误且不改errno。
int pthread_join(pthread_t thread, void **result); // 当前仅主执行者收取，NULL丢弃结果；成功才写私人输出。
pthread_t pthread_self(void); // 当前有效运行头提供身份，不能跨进程当授权句柄。
int pthread_equal(pthread_t left, pthread_t right); // 相同非零，不同0，不将可移植比较写成数值猜测。
```

<!-- replace: labs/airios/libc/pthread.c -->
<!-- before -->
```c
int pthread_create(pthread_t *thread, const pthread_attr_t *attributes, void *(*function)(void *), void *argument) { // 默认可join线程的最小源码路径。
    if (!thread || (uintptr_t)thread % _Alignof(pthread_t) || !function) return EINVAL; // 显式坏输出/函数在分配前拒绝，其他无效用户地址仍可故障。
    if (attributes) return ENOTSUP; // 不把未实现的栈、调度或detach属性当作生效。
    if (pthread_self() != (pthread_t)airi_getpid()) return ENOTSUP; // 当前生命周期仍由主执行者管理，worker递归创建明确拒绝。
    int saved = errno; // malloc可能设置ENOMEM，但pthread返回规则不允许改调用者errno。
    struct start_packet *packet = malloc(sizeof(*packet)); // 启动包跨创建者栈寿命，不借一个很快失效的局部对象。
    if (!packet) { errno = saved; return EAGAIN; } // 资源不足通过正返回值报告。
```
<!-- after -->
```c
int pthread_create(pthread_t *thread, const pthread_attr_t *attributes, void *(*function)(void *), void *argument) { // 默认可join线程的最小源码路径。
    if (!thread || (uintptr_t)thread % _Alignof(pthread_t) || !function) return EINVAL; // 显式坏输出/函数在分配前拒绝，其他无效用户地址仍可故障。
    if (attributes) return ENOTSUP; // 不把未实现的栈、调度或detach属性当作生效。
    int saved = errno; // malloc可能设置ENOMEM，但pthread返回规则不允许改调用者errno。
    struct start_packet *packet = malloc(sizeof(*packet)); // 启动包跨创建者栈寿命，不借一个很快失效的局部对象。
    if (!packet) { errno = saved; return EAGAIN; } // 资源不足通过正返回值报告。
```

<a id="nested-lifetime-observation"></a>
## 先让创建者返回，再改用它旧的槽

pthread-probe增加TLS变量local_marker，声明初值42。echo0将它改成100、errno改成101，再创建leaf。leaf应从自己的模板初值42和errno0开始，不能继承100／101；它把自己的值改成321／201，并保存pthread_self，然后等待descendant_gate。

descendant是echo0写出的新身份，主在join echo0以后才读，不把create成功当成用户写回已经完成。descendant_self由leaf在运行以后写，再release发布descendant_ready。用户参数仍指向主持有的静态tokens[0]，不指向echo0将要结束的工作栈。

下面分两块更新pthread-probe.c从primary到held签名之间的内容。第一块替换primary到require结束的旧窗口，新增TLS、身份和门，再加入leaf；第二块才更新echo。容量八人、二十四轮复用和两个资源失败路径都保留。普通结束以后不再拿已经结束的身份调用equal；有效身份的比较放在自身仍运行或两个已创建尚未join的时候。

<!-- replace: labs/airios/pthread-probe.c -->
<!-- before -->
```c
static pthread_t primary; // 创建任何worker以前写入，之后只读。
static airi_event gate = AIRI_EVENT_INITIALIZER; // 八容量worker等待主统一发布。
static _Atomic unsigned ready; // 回调开始时启动包已经被适配器释放。
static void require(int condition, const char *message) { // 失败结束本真实进程组。
    if (!condition) { airi_puts(message); airi_puts("\n"); airi_exit(88); } // 不继续输出成功标记。
}
```
<!-- after -->
```c
static pthread_t primary; // 创建任何worker以前写入，之后只读。
static airi_event gate = AIRI_EVENT_INITIALIZER; // 八容量worker等待主统一发布。
static _Atomic unsigned ready; // 回调开始时启动包已经被适配器释放。
static _Thread_local unsigned local_marker = 42; // 嵌套创建仍从模板开始，不复制创建者运行值。
static pthread_t descendant, descendant_self; // 第一位creator写descendant，root在join creator以后读；leaf写自身参考。
static airi_event descendant_gate = AIRI_EVENT_INITIALIZER; // 保持子执行者活到创建者槽被重用以后。
static _Atomic unsigned descendant_ready; // 发布leaf已经设置自己的TLS和身份。
static void require(int condition, const char *message) { // 失败结束本真实进程组。
    if (!condition) { airi_puts(message); airi_puts("\n"); airi_exit(88); } // 不继续输出成功标记。
}
static void *leaf(void *argument) { // 创建者是worker而不是主执行者。
    require(errno == 0 && local_marker == 42, "nested worker fresh TLS"); // 不继承creator的101和100。
    descendant_self = pthread_self(); errno = 201; local_marker = 321; // 本次执行者独有的运行值。
    atomic_store_explicit(&descendant_ready, 1, memory_order_release); // root可确认初始化已实际发生。
    uint64_t start = airi_ticks(); // 等待仍有固定预算。
    while (airi_event_sequence(&descendant_gate) == 0) { // creator先结束，其槽随后另作他用。
        uint64_t elapsed = airi_ticks() - start; // 共享时钟经过时间，不续借预算。
        require(elapsed < 200 && airi_event_wait(&descendant_gate, 0, (unsigned)(200 - elapsed)) == 0, "nested worker gate"); // 原执行现场保留。
    }
    require(errno == 201 && local_marker == 321 && pthread_equal(pthread_self(), descendant_self), "nested worker survives creator reuse"); // 创建者槽复用不污染仍存活的子TLS/TCB。
    return argument; // 原main持有的token仍有效，不借creator已经结束的栈。
}
```

接着替换echo函数至held签名以前的窗口。现在echo0实际创建leaf，把main持有的token传下去；echo自身的100／101与leaf的321／201属于不同TLS。对等join仍返回ENOTSUP，不能把创建能力放开等同于收取能力也已放开。

<!-- replace: labs/airios/pthread-probe.c -->
<!-- before -->
```c
static void *echo(void *argument) { // 真正的POSIX形状函数，不强转函数指针。
    struct token *token = argument; // 用户参数保持到主join以后。
    require(errno == 0 && !pthread_equal(pthread_self(), primary), "pthread fresh errno and identity"); // 新TLS不继承主71。
    token->identity = pthread_self(); // 只写自己那一份输出，主尚未读取。
    errno = 101 + (int)token->index; // 每位执行者保存不同错误哨兵。
    if (!token->index) { // 第一位观察未支持的worker管理能力。
        pthread_t untouched = 777; void *output = (void *)(uintptr_t)99; // 失败输出哨兵。
        require(pthread_create(&untouched, 0, echo, token) == ENOTSUP && untouched == 777, "pthread nested create unsupported"); // 不能悄悄创建后返回失败。
        require(pthread_join(primary, &output) == ENOTSUP && output == (void *)(uintptr_t)99, "pthread peer join unsupported"); // 保留当前主管理限制。
        require(pthread_join(pthread_self(), 0) == EDEADLK, "pthread worker self join"); // 自己等待自己明确报告。
    }
    airi_yield(); // 在真正共享执行者之间交替，errno仍属于自己。
    require(errno == 101 + (int)token->index, "pthread worker errno private"); // pthread调用和别人的TLS不污染它。
    return token; // 目标返回值不是启动包，而是仍存活的用户对象。
}
static void *held(void *argument) { // 容量worker从回调开始就不再占启动包。
```
<!-- after -->
```c
static void *echo(void *argument) { // 真正的POSIX形状函数，不强转函数指针。
    struct token *token = argument; // 用户参数保持到主join以后。
    require(errno == 0 && local_marker == 42 && !pthread_equal(pthread_self(), primary), "pthread fresh errno and identity"); // 新TLS不继承主71。
    token->identity = pthread_self(); // 只写自己那一份输出，主尚未读取。
    errno = 101 + (int)token->index; local_marker = 100 + token->index; // 每位执行者保存不同运行值。
    if (!token->index) { // 第一位创建子执行者，并保留尚未支持的对等join观察。
        void *output = (void *)(uintptr_t)99; // 尚未支持的对等join仍保留失败输出哨兵。
        require(pthread_create(&descendant, 0, leaf, token) == 0, "pthread nested create succeeds"); // 包由这个creator准备，资源归同一process。
        require(pthread_join(primary, &output) == ENOTSUP && output == (void *)(uintptr_t)99, "pthread peer join unsupported"); // 保留当前主管理限制。
        require(pthread_join(pthread_self(), 0) == EDEADLK, "pthread worker self join"); // 自己等待自己明确报告。
    }
    airi_yield(); // 在真正共享执行者之间交替，errno仍属于自己。
    require(errno == 101 + (int)token->index && local_marker == 100 + token->index, "pthread worker errno private"); // pthread调用和别人的TLS不污染它。
    return token; // 目标返回值不是启动包，而是仍存活的用户对象。
}
static void *held(void *argument) { // 容量worker从回调开始就不再占启动包。
```

<!-- replace: labs/airios/pthread-probe.c -->
<!-- before -->
```c
    }
    return argument; // 八个结果分别指回自己的token。
}
int main(void) { // 所有创建/收取都从主执行者开始。
    primary = pthread_self(); errno = 71; // 不把pthread_t的具体整数当可移植接口使用。
    pthread_t unchanged = 777; pthread_attr_t attributes = {0}; // 非NULL属性当前不支持，即使保留字为0。
    require(pthread_equal(primary, pthread_self()), "pthread equality"); // 只比较有效身份，不靠猜测下一个TID。
```
<!-- after -->
```c
    }
    return argument; // 八个结果分别指回自己的token。
}
int main(void) { // 主安排观察并统一收取，实际创建也可发生在worker内。
    primary = pthread_self(); errno = 71; // 不把pthread_t的具体整数当可移植接口使用。
    pthread_t unchanged = 777; pthread_attr_t attributes = {0}; // 非NULL属性当前不支持，即使保留字为0。
    require(pthread_equal(primary, pthread_self()), "pthread equality"); // 只比较有效身份，不靠猜测下一个TID。
```

主收取echo0后，让leaf实际在gate上等待。接着创建replacement，使用另一份tokens[2]，避免两个仍在运行者同时写同一对象。内核会复用最早空闲的echo0槽：用户TLS基址数值可以相同，但普通初值和TCB身份必须重新建立。replacement返回并被join以后，主才发布leaf的gate。

leaf恢复后检查321、201和自己的活身份仍正确，再返回原tokens[0]。若它借用了创建者的TLS或现场，创建者槽被清零、写入replacement初值及身份时就会暴露错误。主最后join leaf取得原指针，证明创建者结束没有提前结束仍活着的同进程执行链。

下面替换main从失败输出哨兵到容量start之间的旧窗口，按创建echo→收取创建者→重用其槽→放行leaf的顺序写入。八人容量观察仍接在这一段之后。

<!-- replace: labs/airios/pthread-probe.c -->
<!-- before -->
```c
    void *output = (void *)(uintptr_t)99; // join失败保持私人输出。
    require(pthread_join(primary, &output) == EDEADLK && pthread_join(0, &output) == ESRCH && pthread_join((pthread_t)UINT32_MAX + 1, &output) == ESRCH, "pthread invalid joins"); // 不窄化过宽值。
    require(output == (void *)(uintptr_t)99 && errno == 71, "pthread join failure preserves state"); // 正错误不写errno。
    pthread_t threads[8]; // 只在create成功以后使用每个槽。
    for (unsigned i = 0; i < 2; ++i) { tokens[i].index = i; require(pthread_create(&threads[i], 0, echo, &tokens[i]) == 0 && errno == 71, "pthread initial create"); } // 新worker可以在下一次创建以前已经返回。
    require(!pthread_equal(threads[0], threads[1]), "pthread distinct live identities"); // 两个已创建尚未join的身份都仍有效。
    require(pthread_join(threads[0], &output) == 0 && output == &tokens[0] && pthread_equal(tokens[0].identity, threads[0]), "pthread typed result"); // 指针完整往返，身份属于真实worker。
    require(pthread_join(threads[1], 0) == 0 && pthread_equal(tokens[1].identity, threads[1]), "pthread discard result"); // NULL不省略收取。
    output = (void *)(uintptr_t)99; require(pthread_join(threads[0], &output) == ESRCH && output == (void *)(uintptr_t)99 && errno == 71, "pthread already joined extension"); // 本实现不复用TID，旧身份明确拒绝；不可移植代码不依赖此扩展。
    for (unsigned i = 0; i < 8; ++i) require(pthread_create(&threads[i], 0, held, &tokens[i]) == 0 && errno == 71, "pthread capacity fill"); // 八个真实工作槽全部占用。
    uint64_t start = airi_ticks(); // 只等回调实际开始，不从创建成功推断包已释放。
```
<!-- after -->
```c
    void *output = (void *)(uintptr_t)99; // join失败保持私人输出。
    require(pthread_join(primary, &output) == EDEADLK && pthread_join(0, &output) == ESRCH && pthread_join((pthread_t)UINT32_MAX + 1, &output) == ESRCH, "pthread invalid joins"); // 不窄化过宽值。
    require(output == (void *)(uintptr_t)99 && errno == 71, "pthread join failure preserves state"); // 正错误不写errno。
    airi_puts("[PTHREAD_NESTED_BEGIN]\n"); // 宿主记录创建者、子执行者和原槽复用顺序。
    pthread_t threads[8]; // 只在create成功以后使用每个槽。
    for (unsigned i = 0; i < 2; ++i) { tokens[i].index = i; require(pthread_create(&threads[i], 0, echo, &tokens[i]) == 0 && errno == 71, "pthread initial create"); } // 新worker可以在下一次创建以前已经返回。
    require(!pthread_equal(threads[0], threads[1]), "pthread distinct live identities"); // 两个已创建尚未join的身份都仍有效。
    require(pthread_join(threads[0], &output) == 0 && output == &tokens[0], "pthread typed result"); // creator已结束，之后不再用它的死身份调用equal。
    uint64_t nested_start = airi_ticks(); // 有限等待leaf已进入自己的运行环境。
    while (!atomic_load_explicit(&descendant_ready, memory_order_acquire)) { require(airi_ticks() - nested_start < 100, "nested worker ready"); airi_yield(); } // 不从创建成功推断已经运行。
    require(airi_sleep(3) == 0, "nested worker actual wait"); // leaf仍在gate上，creator槽可先重用。
    pthread_t replacement; tokens[2].index = 2; // 另一份长期参数，不与仍活着的leaf或echo1重叠写。
    require(pthread_create(&replacement, 0, echo, &tokens[2]) == 0 && pthread_join(replacement, &output) == 0 && output == &tokens[2], "creator slot replacement"); // 最早空槽应复用creator，而非破坏leaf。
    require(airi_event_publish(&descendant_gate) >= 0 && pthread_join(descendant, &output) == 0 && output == &tokens[0], "nested result after creator join"); // 真正子执行者从原调用继续并正常结束。
    airi_puts("[PTHREAD_NESTED_OK] creator=joined slot=reused child=alive tls=fresh result=pointer\n"); // 宿主再用实际slot/TP/creator记录独立核对。
    require(pthread_join(threads[1], 0) == 0, "pthread discard result"); // NULL不省略收取。
    output = (void *)(uintptr_t)99; require(pthread_join(threads[0], &output) == ESRCH && output == (void *)(uintptr_t)99 && errno == 71, "pthread already joined extension"); // 本实现不复用TID，旧身份明确拒绝；不可移植代码不依赖此扩展。
    for (unsigned i = 0; i < 8; ++i) require(pthread_create(&threads[i], 0, held, &tokens[i]) == 0 && errno == 71, "pthread capacity fill"); // 八个真实工作槽全部占用。
    uint64_t start = airi_ticks(); // 只等回调实际开始，不从创建成功推断包已释放。
```

<!-- replace: labs/airios/pthread-probe.c -->
<!-- before -->
```c
    free(reserve); // 所有借用都结束以后归还唯一观察大对象。
    after = airi_malloc_info(); // 页面可缓存，启动对象不能残留。
    require(after.live_objects == 0 && after.live_bytes == 0 && after.arena_bytes == 262144 && errno == 71, "pthread final allocator and errno"); // 不把缓存页面误判成包泄漏。
    airi_puts("[PTHREAD_USER_OK] functions=typed result=pointer errno=preserved workers=34 capacity=8 failed_packet=freed alloc_failure=preserved reuse=24 attributes=unsupported\n"); // 正常、失败与资源边界实际完成后才输出。
    return 0; // shell收取真实程序结果。
}
```
<!-- after -->
```c
    free(reserve); // 所有借用都结束以后归还唯一观察大对象。
    after = airi_malloc_info(); // 页面可缓存，启动对象不能残留。
    require(after.live_objects == 0 && after.live_bytes == 0 && after.arena_bytes == 262144 && errno == 71, "pthread final allocator and errno"); // 不把缓存页面误判成包泄漏。
    airi_puts("[PTHREAD_USER_OK] functions=typed result=pointer errno=preserved workers=36 capacity=8 failed_packet=freed alloc_failure=preserved reuse=24 attributes=unsupported\n"); // 正常、失败与资源边界实际完成后才输出。
    return 0; // shell收取真实程序结果。
}
```

<a id="nested-host-relationship"></a>
## 日志顺序可以交错，创建关系不能靠位置猜

本轮pthread命令有36次实际创建／返回：原先34位，加一位leaf与一位replacement。只靠“前两条THREAD_CREATE是主创建”会出错：主创建第一个echo以后，PIT可能先让它创建leaf，再回主创建第二个echo。宿主应根据creator字段分组，再在同一个创建者内部看顺序。

取NESTED_BEGIN到NESTED_OK之间的四条记录。root_created筛选creator等于本次PID的三条，分别是echo0、echo1和replacement；children筛选creator等于echo0 TID的唯一一条。叶子活着时槽号不能与创建者相同，replacement的slot与TP则应和已经收拢的创建者一致。再检查replacement返回先于leaf，且leaf确有FUTEX_WAIT，不能用“leaf早就结束”冒充跨寿命演示。

元组中的字段顺序是(tid,slot,tp,creator)，replacement[1:3]和parent[1:3]分别取slot、tp两个元素，右端3不包含在切片中。比较的是同一进程内的槽和用户虚拟地址，既不是物理地址，也不是说两个执行者同时拥有同一活TLS。

<!-- replace: scripts/shell_test.py -->
<!-- before -->
```python
                assert len(re.findall(r'\[THREAD_CREATE\] pid='+queue_owner.group(1)+r' tid=',queue_output))==8 and len(re.findall(r'\[THREAD_RETURN\] pid='+queue_owner.group(1)+r' tid=',queue_output))==8 # 一背压、二关闭消费者、一关闭生产者及四负载均收拢。
                offset=len(text); wire.sendall(b'pthread\n'); text=wait('[SHELL_COMMAND_DONE] name=pthread code=0',offset) # 正确函数类型的线程入口与指针结果，不强转回调。
                pthread_output=text[offset:] # 只核对本条命令的真实worker与结果。
                assert '[PTHREAD_USER_OK] functions=typed result=pointer errno=preserved workers=34 capacity=8 failed_packet=freed alloc_failure=preserved reuse=24 attributes=unsupported' in pthread_output # guest检查正错误、errno、包归还与每个原参数返回。
                pthread_owner=re.search(r'\[EXEC_COMMIT\] pid=(\d+) program=pthread ',pthread_output) # 不固定实际PID。
                assert pthread_owner # 静态ELF确实被真实shell执行。
                assert len(re.findall(r'\[THREAD_CREATE\] pid='+pthread_owner.group(1)+r' tid=',pthread_output))==34 and len(re.findall(r'\[THREAD_RETURN\] pid='+pthread_owner.group(1)+r' tid=',pthread_output))==34 # 两初始、八容量和二十四复用均真正结束。
                assert '[FUTEX_WAIT]' in pthread_output # 容量门真实阻塞；join也可收取已结束者，不能强求某种调度时序。
                offset=len(text); wire.sendall(b'heap-threads\n'); text=wait('[SHELL_COMMAND_DONE] name=heap-threads code=0',offset) # 四个真实执行者共享唯一arena，观察对象交接。
                heap_threads=text[offset:] # 不用旧heap命令代替并发结果。
                assert '[MALLOC_THREADS_OK] workers=4 rounds=96 arena=262144 cross_free=4 snapshots=consistent errno=private critical_yield=1' in heap_threads # 每个工作者和主快照均由guest核对。
```
<!-- after -->
```python
                assert len(re.findall(r'\[THREAD_CREATE\] pid='+queue_owner.group(1)+r' tid=',queue_output))==8 and len(re.findall(r'\[THREAD_RETURN\] pid='+queue_owner.group(1)+r' tid=',queue_output))==8 # 一背压、二关闭消费者、一关闭生产者及四负载均收拢。
                offset=len(text); wire.sendall(b'pthread\n'); text=wait('[SHELL_COMMAND_DONE] name=pthread code=0',offset) # 正确函数类型的线程入口与指针结果，不强转回调。
                pthread_output=text[offset:] # 只核对本条命令的真实worker与结果。
                assert '[PTHREAD_USER_OK] functions=typed result=pointer errno=preserved workers=36 capacity=8 failed_packet=freed alloc_failure=preserved reuse=24 attributes=unsupported' in pthread_output # guest检查正错误、errno、包归还与每个原参数返回。
                pthread_owner=re.search(r'\[EXEC_COMMIT\] pid=(\d+) program=pthread ',pthread_output) # 不固定实际PID。
                assert pthread_owner # 静态ELF确实被真实shell执行。
                assert len(re.findall(r'\[THREAD_CREATE\] pid='+pthread_owner.group(1)+r' tid=',pthread_output))==36 and len(re.findall(r'\[THREAD_RETURN\] pid='+pthread_owner.group(1)+r' tid=',pthread_output))==36 # 两初始、一嵌套、一creator槽复用、八容量和二十四复用均结束。
                assert '[FUTEX_WAIT]' in pthread_output # 容量门真实阻塞；join也可收取已结束者，不能强求某种调度时序。
                assert '[PTHREAD_NESTED_OK] creator=joined slot=reused child=alive tls=fresh result=pointer' in pthread_output # leaf跨creator结束与槽复用仍保持自己的TCB/TLS。
                nested=pthread_output.split('[PTHREAD_NESTED_BEGIN]',1)[1].split('[PTHREAD_NESTED_OK]',1)[0] # 只取三代交接范围。
                created=re.findall(r'\[THREAD_CREATE\] pid='+pthread_owner.group(1)+r' tid=(\d+) slot=(\d+) tp=(0x[0-9a-f]+) creator=(\d+)',nested) # creator来自内核当前running，不由用户自报。
                root_created=[item for item in created if item[3]==pthread_owner.group(1)] # 主创建两echo及一replacement，leaf由第一个echo创建。
                assert len(created)==4 and len(root_created)==3 # 不依赖跨PIT时嵌套与第二次主创建的全局日志先后。
                parent,replacement=root_created[0],root_created[2] # 按同一创建者内部顺序取原执行者和复用者。
                children=[item for item in created if item[3]==parent[0]] # 实际祖先关系由creator字段确定。
                assert len(children)==1 and children[0][1]!=parent[1] and replacement[1:3]==parent[1:3] # 活leaf有独立槽，creator槽/TP可被替换。
                replacement_return=nested.index('[THREAD_RETURN] pid='+pthread_owner.group(1)+' tid='+replacement[0]+' ') # 完成replacement以前leaf仍不得返回。
                child_return=nested.index('[THREAD_RETURN] pid='+pthread_owner.group(1)+' tid='+children[0][0]+' ') # gate在replacement join以后才发布。
                assert replacement_return < child_return and re.search(r'\[FUTEX_WAIT\] pid='+pthread_owner.group(1)+r' tid='+children[0][0]+r' ',nested) # 不是leaf早已结束后的虚假寿命演示。

                offset=len(text); wire.sendall(b'heap-threads\n'); text=wait('[SHELL_COMMAND_DONE] name=heap-threads code=0',offset) # 四个真实执行者共享唯一arena，观察对象交接。
                heap_threads=text[offset:] # 不用旧heap命令代替并发结果。
                assert '[MALLOC_THREADS_OK] workers=4 rounds=96 arena=262144 cross_free=4 snapshots=consistent errno=private critical_yield=1' in heap_threads # 每个工作者和主快照均由guest核对。
```

<!-- replace: scripts/shell_test.py -->
<!-- before -->
```python
                offset=len(text); wire.sendall(b'echo after-shell\n'); text=wait('[CONSOLE_UPDATE] view=0 commands=2 page=0',offset) # 回到内核后输入仍完整。
                assert 'after-shell' in text[offset:] # 不只是退出而丢了后续UART。
                result={'passed':True,'ring3_shell':True,'ram_file_and_device_readback':True,'other_user_during_cq_wait':True,'qmp_iops':1,'error_exec_survived':True,'returned_to_monitor':True,'orphan_rounds':24,'memory_reuse_rounds':128,'capacity_rounds':4,'capacity_pages':1024,'simd_preempt_fork_exec':True,'fs_base_preempt_fork_exec':True,'compiler_static_tls':True,'posix_errno_io_subset':True,'positioned_vectors':True} # 只发布已观察事实。
                result.update(pthread_source_subset=True,pthread_workers=34,pthread_packets_reclaimed=True,bounded_queue=True,queue_capacity=4,queue_jobs=48,queue_close_drains=True,sequence_event=True,event_waits=6,event_broadcast_workers=2,event_false_wake_rechecked=True,mutex_race_stages=4,mutex_race_actual_waits=1,mutex_race_mismatches=1,per_execution_syscall_count=True,uncontended_mutex_pairs=8,uncontended_mutex_syscalls=0,runtime_tcb_identity=True,identity_reference_queries=7,shared_malloc=True,malloc_workers=4,malloc_rounds=96,owned_mutex=True,mutex_critical_updates=64,private_futex=True,futex_registrations=8,user_heap=True,shared_executions=True,thread_commands=2,worker_capacity=8,worker_reuse_rounds=24) # 关键字把实际已验收字段加入结果字典，不替代上面的逐命令检查。
                result_path.write_text(json.dumps(result,indent=2)+'\n'); print('PASS user shell: alloc/file/async wait/other task/CQ/readback, failed exec, return to monitor') # 阶段演示实际完成。
                return result # 总入口可收集。
            finally: # 包括断言、超时和QEMU错误。
```
<!-- after -->
```python
                offset=len(text); wire.sendall(b'echo after-shell\n'); text=wait('[CONSOLE_UPDATE] view=0 commands=2 page=0',offset) # 回到内核后输入仍完整。
                assert 'after-shell' in text[offset:] # 不只是退出而丢了后续UART。
                result={'passed':True,'ring3_shell':True,'ram_file_and_device_readback':True,'other_user_during_cq_wait':True,'qmp_iops':1,'error_exec_survived':True,'returned_to_monitor':True,'orphan_rounds':24,'memory_reuse_rounds':128,'capacity_rounds':4,'capacity_pages':1024,'simd_preempt_fork_exec':True,'fs_base_preempt_fork_exec':True,'compiler_static_tls':True,'posix_errno_io_subset':True,'positioned_vectors':True} # 只发布已观察事实。
                result.update(pthread_source_subset=True,pthread_workers=36,pthread_packets_reclaimed=True,bounded_queue=True,queue_capacity=4,queue_jobs=48,queue_close_drains=True,sequence_event=True,event_waits=6,event_broadcast_workers=2,event_false_wake_rechecked=True,mutex_race_stages=4,mutex_race_actual_waits=1,mutex_race_mismatches=1,per_execution_syscall_count=True,uncontended_mutex_pairs=8,uncontended_mutex_syscalls=0,runtime_tcb_identity=True,identity_reference_queries=7,shared_malloc=True,malloc_workers=4,malloc_rounds=96,owned_mutex=True,mutex_critical_updates=64,private_futex=True,futex_registrations=8,user_heap=True,shared_executions=True,thread_commands=2,worker_capacity=8,worker_reuse_rounds=24) # 关键字把实际已验收字段加入结果字典，不替代上面的逐命令检查。
                result_path.write_text(json.dumps(result,indent=2)+'\n'); print('PASS user shell: alloc/file/async wait/other task/CQ/readback, failed exec, return to monitor') # 阶段演示实际完成。
                return result # 总入口可收集。
            finally: # 包括断言、超时和QEMU错误。
```

没有新增文件或镜像名称，已有pthread.elf会按传递头依赖重新编译。运行完整入口：

<!-- run -->
```sh
./airios build  # 内核允许任一同进程创建者，用户包装保持原类型和包交接
```

<!-- run -->
```sh
./airios test  # 创建者结束、槽/TP复用、活leaf恢复及所有旧资源失败
```

<a id="nested-results"></a>
## 同一TP再次出现时，谁还活着

交互执行`./airios run`、`run sh`、`pthread`。本轮实际日志中主PID95创建TID96与97；TID96创建TID98。96在slot0、TP=0x60002010，98在slot2、TP=0x60006010。96返回并被主收取以后，主创建99复用slot0和0x60002010；99先结束，98才从自己的gate等待恢复并正常返回。

```text
[PTHREAD_NESTED_OK] creator=joined slot=reused child=alive tls=fresh result=pointer
[PTHREAD_USER_OK] functions=typed result=pointer errno=preserved workers=36 capacity=8 failed_packet=freed alloc_failure=preserved reuse=24 attributes=unsupported
```

这些数字只记录本轮实际值，自动检查始终从PID和creator关系取身份。slot/TP可以复用，TID仍不复用；同一个数值地址再次出现，不等于原执行者还活着。leaf归进程共享资源管理，有独立执行现场，不以创建者栈的寿命作自己的寿命。

这里创建者结束指工作函数普通return；原始进程exit或用户故障仍结束整个进程组。当前主仍统一join，用户参数和返回指针的寿命仍由应用负责，不能因为允许嵌套创建就传一个即将失效的局部地址。对等join还要解决同一目标被多人收取以及循环等待，不能只删除原有拒绝。下一节会给目标加唯一认领，在登记等待前检查有界join链，再放开这一侧。
