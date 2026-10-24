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
    if (!airi_memory_same_usage(&before, &after)) // 任务栈和临时页表必须全部恢复。
        airi_panic("completed kernel tasks leaked memory"); // 内核演示不应吃掉后续主线的容量。
    airi_log("[TASK_DEMO_OK] order=121212 separate_stacks=2 reclaimed=1"); // 只有切换、局部值、顺序与回收全部成立才输出。
}
