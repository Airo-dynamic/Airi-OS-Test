#include <airi_threads.h> // 自有共享执行接口，不链接宿主pthread。
#include <airi_libc.h> // 原始进程、页、FD和异步调用保留负错误。
#include <errno.h> // 每条执行链应取得自己新建的静态TLS错误槽。
#include <stdatomic.h> // 共享发布使用明确的C11原子操作，不把volatile当同步。
_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "thread probe needs lock-free int atomics"); // 当前x86-64不借未实现的libatomic。
static _Thread_local volatile unsigned local = 7; // 文件初值，每个新工作者从7而非父当前值99开始。
static _Thread_local volatile unsigned char zero[33]; // 复用工作槽也须重新清零完整tbss。
static _Atomic unsigned gate, capacity_gate, pipe_entered; // 主执行者以release发布，工作者以acquire观察。
static _Atomic unsigned observed[2], opened; // 工作结果不通过无同步的普通全局写入传播。
static long expected_pid; // 创建前设定，发布gate以后只读。
static unsigned shared_fd; // 工作执行者访问的同一进程表项。
static uint64_t shared_handle; // 注册buffer属于进程PID，工作者共享同一owner。
struct job { unsigned index; }; // 参数存活在主执行者栈上，直到两次join都结束。
static int fresh(void) { // 普通C本地变量的初始值不能从主执行者运行块复制。
    if (local != 7 || errno) // 父当前值分别为99与EBUSY。
        return 0; // 不以FS地址不同代替内容正确。
    for (unsigned i = 0; i < sizeof(zero); ++i) // 检查整个新tbss。
        if (zero[i]) // 上一次工作者留下的修改也必须清除。
            return 0; // 未初始化的复用槽不能继续。
    return 1; // 只读当前执行者的真实TLS。
}
static long compute(void *argument) { // 两个工作者共享进程与FD，但CPU状态和TLS分别保存。
    struct job *job = argument; // 主执行者在join之前不会结束此参数对象。
    while (!atomic_load_explicit(&gate, memory_order_acquire)) // 创建后暂等主执行者完成共享输入和忙路径观察。
        airi_yield(); // 此发布等待只是测试夹具，尚不是用户futex。
    if (!fresh() || airi_getpid() != expected_pid || airi_thread_id() == expected_pid) // acquire读到发布以后再读取普通共享输入；同PID、不同TID且新TLS初值。
        return -101; // 通过join返回具体失败，不伪造进程成功。
    unsigned char byte; // 每个工作者有独立用户栈输出。
    if (airi_read(shared_fd, &byte, 1) != 1) // 共享打开对象的偏移应被两个调用各消费一次。
        return -102; // 主执行者随后应读到第三个字节c。
    atomic_store_explicit(&observed[job->index], byte, memory_order_release); // 内容由主执行者在join后读取。
    if (!job->index) { // 一位工作者另建FD，主执行者之后必须能在同一表中使用。
        long fd = airi_open("/seed", AIRI_READ); // 独立open从offset0开始。
        if (fd < 0 || airi_io_put(shared_handle, "T", 1)) // 异步buffer同样使用共享PID授权。
            return -103; // 工作者结束不能关闭这份进程资源。
        atomic_store_explicit(&opened, (unsigned)fd, memory_order_release); // 发布新表项号码。
    }
    local = 10 + job->index; // 两个工作者使用同一编译器符号，运行值分别为10与11。
    zero[32] = (unsigned char)(30 + job->index); // 修改tbss尾部供下次槽复用观察。
    errno = (int)(200 + job->index); // 错误槽也是独立运行副本。
    unsigned mxcsr, configured = 0x1f80u | ((job->index + 1) << 13); // 分别使用不同舍入控制，正常返回前再恢复。
    unsigned short control, configured_control = (unsigned short)(0x037fu | ((job->index + 1) << 10)); // x87控制也要随执行者保持。
    __asm__ volatile("stmxcsr %0; fnstcw %1" : "=m"(mxcsr), "=m"(control)); // 直接观察入场硬件控制位。
    if (mxcsr != 0x7f80 || control != 0x0f7f) // 新工作者继承主执行者在创建前修改的浮点环境，不重新置为新进程默认值。
        return -104; // 不能只给新TLS而遗漏环境继承。
    unsigned inherited_mxcsr = mxcsr; unsigned short inherited_control = control; // 函数返回前仍须恢复自己入场时的控制值。
    __asm__ volatile("ldmxcsr %0; fldcw %1" : : "m"(configured), "m"(configured_control)); // 设定本执行者自己的两个控制值。
    long value = 0x1234567800000000l + job->index; // 正常与失败都走控制位恢复出口。
    uint64_t start = airi_ticks(); // 下一个循环必须跨真实PIT抢占窗口。
    do { // 普通用户计算不以yield冒充切换。
        for (unsigned i = 0; i < 10000; ++i) { // 反复通过FS和真实控制寄存器观察。
            __asm__ volatile("stmxcsr %0; fnstcw %1" : "=m"(mxcsr), "=m"(control)); // 不从静态副本猜硬件状态。
            if (mxcsr != configured || control != configured_control || local != 10 + job->index || zero[32] != 30 + job->index || errno != (int)(200 + job->index)) { // 任何串状态都让实际join结果失败。
                value = -105; // 失败也须遵循普通C调用的控制位保持约定。
                goto restore; // 不从中间分支绕过恢复。
            }
        }
        volatile double input = 3.25, multiplier = 2.0; // 当前用户编译选项真正生成SSE2浮点运算。
        volatile double product = input * multiplier; // 精确6.5不受当前舍入方向影响。
        if (product != 6.5) { // 各自用户栈和寄存器内容必须正确。
            value = -106; // 当前目标不借宿主浮点运行时。
            goto restore; // 仍先恢复再返回错误。
        }
    } while (airi_ticks() - start < 20); // 二十个真实tick，不以宿主sleep代替。
restore: // 无论观察成功或失败，工作函数都按C ABI归还控制状态。
    mxcsr = inherited_mxcsr; control = inherited_control; // 普通C调用返回前恢复本次继承的入场控制状态。
    __asm__ volatile("ldmxcsr %0; fldcw %1" : : "m"(mxcsr), "m"(control)); // 恢复后桥接器提交64位函数返回值。
    return value; // join不得按进程exit的低八位截断。
}
static long short_job(void *argument) { // 用于容量、重复创建和TLS缓存重置。
    (void)argument; // 本工作函数没有额外输入对象。
    while (!atomic_load_explicit(&capacity_gate, memory_order_acquire)) // 容量阶段让全部八槽同时存活。
        airi_yield(); // gate释放之前不能提前结束并影响容量结论。
    if (!fresh() || airi_getpid() != expected_pid) // acquire以后再读创建前的普通进程身份输入，同时核对本地模板初值。
        return -107; // 上一轮tbss或errno不能残留。
    local = 91; zero[32] = 0xab; errno = 201; // 故意留下非初值给下一轮观察。
    return airi_thread_id(); // 主执行者可以把完整结果与创建得到的TID对应。
}
static long pipe_job(void *argument) { // 工作者阻塞read时主执行者还应能继续其他调用。
    unsigned fd = (unsigned)(uintptr_t)argument; // FD号码作为不解引用的数值参数传入。
    atomic_store_explicit(&pipe_entered, 1, memory_order_release); // 标志只说明即将read，串口WAIT_REGISTER才说明内核已阻塞。
    char byte; // 当前工作用户栈在join后继续缓存，不在阻塞期间解除。
    long result = airi_read(fd, &byte, 1); // 无字节时真正登记IO_READ等待。
    return result == 1 ? (unsigned char)byte : -108; // 主执行者join应得到发送的Z。
}
static long process_exit_job(void *argument) { // 与工作函数return不同，原始exit结束整个进程组。
    char byte; // 先等父进程验证外组TID不能join。
    if (airi_read((unsigned)(uintptr_t)argument, &byte, 1) != 1) // 真实管道控制何时触发退出。
        return -109; // 不能提前把目标变成已收取状态。
    airi_exit(77); // 本执行者和它的主执行者一起停止，共享资源只关闭一次。
}
static long fault_job(void *argument) { // 用户故障必须结束目标进程，不能击穿内核或父进程。
    (void)argument; // 明确不借主执行者对象。
    __asm__ volatile("movl $1, (%0)" : : "r"((uintptr_t)0) : "memory"); // 显式CPU写未映射0地址，不让C空指针UB被编译器替换成其他指令。
    return -110; // 正常PF14路径不会执行。
}
int main(void) { // 主执行者负责创建和收拢，不在库中伪造pthread对象。
    expected_pid = airi_getpid(); // 创建任何工作者之前发布固定进程身份。
    if (airi_thread_id() != expected_pid || !fresh()) // 主TID初始等于PID，新ELF TLS为模板值。
        return 70; // 不能由旧进程槽猜执行者身份。
    local = 99; errno = EBUSY; // 后续新工作者不能继承这两个运行值。
    if (airi_thread_create(NULL, NULL) != -14 || airi_thread_join(expected_pid, NULL) != -35 || airi_thread_join(-1, NULL) != -3) // 非RX入口、自等待和无目标分别拒绝。
        return 71; // 参数错误不创建资源。
    void *pages[80]; size_t lengths[80], used = 0; // 保存实际成功的原始页申请，用于恢复预算。
    for (size_t step = 64 * 4096; step; step = step == 64 * 4096 ? 4096 : 0) // 先大批再补单页，填满当前用户数据页预算。
        for (;;) { // 每个步长申请到失败。
            void *page = airi_alloc(step); // 不经过尚未同步的malloc。
            if (!page) // 大批失败后还有可能容纳小批。
                break; // 不记录失败指针。
            if (used == 80) // 保护当前有界记录容量。
                return 72; // 固定1024页预算不应越界。
            pages[used] = page; lengths[used++] = step; // 每项保留真实页面所有权和长度。
        }
    size_t available = 8 * 4096; // 留八页，故意让工作用户栈在已经建立部分页面以后失败。
    while (available) { // 归还最后几批的后缀，不假定尾项一定是单页。
        if (!used) // 已填满的预算应有足够原始页，仍保护记录下标。
            return 72; // 不因异常环境在用户栈上越界。
        --used; // 取最后一个仍持有的范围。
        size_t part = lengths[used] < available ? lengths[used] : available; // 最多归还当前范围，全部长度都按页对齐。
        if (airi_release((unsigned char *)pages[used] + lengths[used] - part, part)) // 只解除该范围的页对齐后缀。
            return 73; // 失败时不假装已经腾出预算。
        lengths[used] -= part; available -= part; // 记录剩余前缀和还需要腾出的字节。
        if (lengths[used]) // 部分归还时保留同一起点的前缀所有权。
            ++used; // 当前项仍是有效记录，下一轮只会在还有剩余目标时继续。
    }
    if (airi_thread_create(short_job, NULL) != -12) // 八页用户栈已接入，第九页触及预算，内核栈/FPU和新增记录页都应回滚。
        return 73; // 不发布半个工作执行者。
    void *restored = airi_alloc(8 * 4096); // 失败后必须能重新取得全部八页，不能只回滚控制块。
    if (!restored || airi_alloc(4096)) // 重新占满以后第九页应失败，核对净数据页数量。
        return 73; // 漏页或错误归还都不能继续发布成功。
    pages[used] = restored; lengths[used++] = 8 * 4096; // 保存这次真实恢复的所有权，后面一起归还。
    while (used) { // 失败创建不得把这些原始页误收走。
        --used; // 只索引真实成功项。
        if (airi_release(pages[used], lengths[used])) // 先恢复用户页预算，再真正创建工作者。
            return 74; // 不继续忽略映射归属错误。
    }
    void *guard_page = airi_alloc(4096); // 后续工作期间的释放应明确返回忙。
    long fd = airi_open("/seed", AIRI_READ), handle = airi_io_alloc(); // 文件和注册buffer都由进程共享。
    if (!guard_page || fd < 0 || handle < 0 || airi_io_put((uint64_t)handle, "Q", 1)) // 真实资源完整准备后才发布输入。
        return 75; // 不在无效FD或句柄上启动工作函数。
    shared_fd = (unsigned)fd; shared_handle = (uint64_t)handle; // gate发布之前设置只读共享输入。
    unsigned saved_mxcsr, observed_mxcsr, parent_mxcsr = 0x7f80; // 父使用第三种舍入方向，与两个工作者的配置都不同。
    unsigned short saved_control, observed_control, parent_control = 0x0f7f; // 记录父原环境，正常与错误出口都应恢复。
    __asm__ volatile("stmxcsr %0; fnstcw %1" : "=m"(saved_mxcsr), "=m"(saved_control)); // 在创建之前取得真实主执行者环境。
    __asm__ volatile("ldmxcsr %0; fldcw %1" : : "m"(parent_mxcsr), "m"(parent_control)); // 工作者应继承这个已修改的环境，而非硬编码默认值。
    struct job jobs[2] = {{0}, {1}}; // 两个参数对象在整个join期间保持存活。
    long first = airi_thread_create(compute, &jobs[0]), second = airi_thread_create(compute, &jobs[1]); // 同进程两个独立执行者。
    if (first < 0 || second < 0 || first == second || first == expected_pid || second == expected_pid) { // 身份不能与PID或彼此混同。
        __asm__ volatile("ldmxcsr %0; fldcw %1" : : "m"(saved_mxcsr), "m"(saved_control)); // 创建失败也先恢复父环境。
        return 76; // 后续按真实返回身份join。
    }
    if (airi_fork() != -16 || airi_exec("broken") != -16 || airi_waitpid(-1, NULL) != -16 || airi_release(guard_page, 4096) != -16 || airi_thread_join(first, (long *)(uintptr_t)0xffff800000000000ull) != -14) { // 并发映像操作拒绝，坏join输出不能消费目标。
        __asm__ volatile("ldmxcsr %0; fldcw %1" : : "m"(saved_mxcsr), "m"(saved_control)); // 失败不把修改后的环境带回_start。
        return 77; // 旧进程内存和两个工作者保持。
    }
    atomic_store_explicit(&gate, 1, memory_order_release); // 发布共享输入，允许两个工作函数进入计算阶段。
    long result = 0; // 内核检查完整8字节用户输出。
    if (airi_thread_join(first, &result) || result != 0x1234567800000000l || airi_thread_join(second, &result) || result != 0x1234567800000001l || airi_thread_join(first, NULL) != -3) { // 真阻塞、64位结果与一次收取均实际观察。
        __asm__ volatile("ldmxcsr %0; fldcw %1" : : "m"(saved_mxcsr), "m"(saved_control)); // 观察失败仍遵循主调用环境恢复约定。
        return 78; // 不接受只看到THREAD_CREATE就算运行。
    }
    __asm__ volatile("stmxcsr %0; fnstcw %1" : "=m"(observed_mxcsr), "=m"(observed_control)); // 两位工作者结束后观察父自己的真实硬件控制值。
    __asm__ volatile("ldmxcsr %0; fldcw %1" : : "m"(saved_mxcsr), "m"(saved_control)); // 先恢复入场环境，再解释观察结果。
    if (observed_mxcsr != parent_mxcsr || observed_control != parent_control) // 工作者各自修改控制不能污染父的第三种环境。
        return 79; // 此时父环境已经恢复，后续代码不依赖观察值。
    unsigned a = atomic_load_explicit(&observed[0], memory_order_acquire), b = atomic_load_explicit(&observed[1], memory_order_acquire); // 配合工作者的release读取各自完成数据。
    char byte; // 共享偏移已经由两个工作者消费a/b。
    if (!((a == 'a' && b == 'b') || (a == 'b' && b == 'a')) || airi_read(shared_fd, &byte, 1) != 1 || byte != 'c' || local != 99 || errno != EBUSY) // 主TLS保持，两个工作者共享文件位置。
        return 79; // 不能把复制进程冒充共享执行。
    unsigned created = atomic_load_explicit(&opened, memory_order_acquire); // 工作者新打开的表项应仍在进程中。
    if (airi_read(created, &byte, 1) != 1 || byte != 'a' || airi_io_get(shared_handle, &byte, 1) || byte != 'T' || airi_close(created) || airi_close(shared_fd) || airi_io_free(shared_handle) || airi_release(guard_page, 4096)) // 工作结束保留共享FD/owner，全部join后可释放原始页。
        return 80; // 共享资源应只由进程持有一次。
    long identities[8]; // 固定全局八工作槽容量。
    for (unsigned i = 0; i < 8; ++i) // gate未开放，八个工作者同时占槽。
        if ((identities[i] = airi_thread_create(short_job, NULL)) < 0) // 原始页失败后的资源也必须恢复可用。
            return 81; // 不让未创建项参与join。
    if (airi_thread_create(short_job, NULL) != -11) // 第九个工作者必须明确容量不足。
        return 82; // 不覆盖仍活着或尚未收取的执行者。
    atomic_store_explicit(&capacity_gate, 1, memory_order_release); // 一次发布，让所有工作者可正常return。
    for (unsigned i = 0; i < 8; ++i) // 按返回身份收取，结果顺序不由执行完成顺序推断。
        if (airi_thread_join(identities[i], &result) || result != identities[i]) // 每个结果是其真实gettid。
            return 83; // 不混用槽号或旧身份。
    for (unsigned i = 0; i < 24; ++i) { // 超过八槽容量的连续复用，不结束进程恢复。
        long tid = airi_thread_create(short_job, NULL); // 重建TLS初值，保留用户页缓存但另建CPU状态与内核栈。
        if (tid < 0 || airi_thread_join(tid, i == 23 ? NULL : &result) || (i != 23 && result != tid)) // 最后一轮同时观察丢弃结果。
            return 84; // 上一位工作者的local/tbss/errno不能进入下一轮。
    }
    unsigned ends[2]; // 工作者阻塞在同一进程管道表上。
    if (airi_pipe(ends, 0)) // 真实阻塞管道，不借宿主注入。
        return 85; // 不在无效端点上继续。
    long reader = airi_thread_create(pipe_job, (void *)(uintptr_t)ends[0]); // 参数只是FD号码。
    if (reader < 0) // 创建必须成功才等待发布。
        return 86; // 避免无限等一个不存在的工作者。
    while (!atomic_load_explicit(&pipe_entered, memory_order_acquire)) // 这只说明用户即将read，不据此假定内核已经登记。
        airi_yield(); // 让工作者先获得CPU。
    if (airi_sleep(3) || airi_close(ends[0]) != -16 || airi_write(ends[1], "Z", 1) != 1 || airi_thread_join(reader, &result) || result != 'Z' || airi_close(ends[0]) || airi_close(ends[1])) // 实际PIT窗口后确认close忙，再写入唤醒并收取。
        return 87; // 阻塞FD不能被关闭后复用到另一个对象。
    unsigned channel[2], trigger[2]; // 父取得外进程工作TID，再控制它触发整进程退出。
    if (airi_pipe(channel, 0) || airi_pipe(trigger, 0)) // 两条真实管道分别传身份和触发字节。
        return 88; // 资源错误不能伪装成权限隔离成功。
    long child = airi_fork(); // 当前全部工作者已join，因此重新允许fork。
    if (child < 0) // 父映像和用户页缓存仍可复制。
        return 89; // 不把负值当孩子身份。
    if (!child) { // 孩子另开自己的工作者，保留复制来的管道FD引用。
        long tid = airi_thread_create(process_exit_job, (void *)(uintptr_t)trigger[0]); // 工作者先等父许可。
        if (tid < 0 || airi_write(channel[1], &tid, sizeof(tid)) != sizeof(tid)) // 完整身份经内核管道交出。
            return 90; // 不能让父永久等不到身份。
        airi_thread_join(tid, NULL); // 主执行者真实等待，工作exit77应连同这条等待一起结束进程。
        return 96; // 只有工作函数意外普通返回时才到达，不能冒充整组退出。
    }
    long foreign; int status = 0; // 外组TID不是本进程可join的能力。
    if (airi_read(channel[0], &foreign, sizeof(foreign)) != sizeof(foreign) || airi_thread_join(foreign, NULL) != -3 || airi_write(trigger[1], "!", 1) != 1 || airi_waitpid(child, &status) != child || status != 77) // 同时验证跨组拒绝与worker exit结束整个孩子组。
        return 91; // 父仍应继续。
    if (airi_close(channel[0]) || airi_close(channel[1]) || airi_close(trigger[0]) || airi_close(trigger[1])) // 归还父持有的管道引用。
        return 92; // 子整组退出已关闭自己的副本。
    child = airi_fork(); // 再验证CPU故障的组退出与父隔离。
    if (child < 0) // 前一组的工作槽必须已经回收。
        return 93; // 不跳过故障演示。
    if (!child) { // 故障只发生在孩子地址空间中。
        long tid = airi_thread_create(fault_job, NULL); // 工作者应真正进入用户函数触发PF14。
        if (tid < 0) // 资源失败不能替代真实用户异常。
            return 94; // 资源失败不能当成页故障成功。
        airi_thread_join(tid, NULL); // 故障应终止整个孩子组，主执行者不从这里继续。
        return 97; // 普通返回说明预期PF14未发生。
    }
    if (airi_waitpid(child, &status) != child || status != -142 || local != 99 || errno != EBUSY) // 故障组回收后父的TLS和执行能力保持。
        return 95; // 不以整个QEMU停止代替进程隔离。
    airi_puts("[THREAD_USER_OK] shared=memory,fd,async tls=fresh cpu=private join=64bit capacity=8 reuse=24 busy=checked group_exit=77 worker_fault=pf14\n"); // 全部真实正常与故障路径结束后发布。
    return 0; // shell继续，整会话最后核对全部进程和工作者资源归还。
}
