#include <airi/runtime_abi.h> // 同内核约定的16字节运行头，不复制内部控制块。
#include <airi_threads.h> // 不借pthread或Linux clone包装。
#include <stdint.h> // 指针以目标64位数值进入内核。
static _Thread_local unsigned char runtime_anchor __attribute__((used)); // 选用该库的ELF必须保留PT_TLS，即使没有别的本地变量。
extern void airi_thread_entry(void); // 汇编桥接器被静态链接到当前用户可执行段。
static long thread_call(long number, uint64_t first, uint64_t second, uint64_t third) { // 与既有int80前三参数约定相同。
    __asm__ volatile("int $0x80" : "+a"(number) : "D"(first), "S"(second), "d"(third) : "memory", "cc"); // 内存clobber让调用边界不隐瞒共享读写。
    return number; // 保持原始负错误，正错误翻译由上层pthread适配器负责。
}
long airi_thread_create(airi_thread_function function, void *argument) { // 创建者不复制自己的用户栈或TLS运行值，新worker从映像模板建立。
    return thread_call(39, (uintptr_t)airi_thread_entry, (uintptr_t)function, (uintptr_t)argument); // 两个代码地址均由内核检查为用户RX。
}
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
}
long airi_futex_wake(uint32_t *word, unsigned count) { // 用户先发布新条件，再通知已登记者。
    return thread_call(44, (uintptr_t)word, count, 0); // 返回数可能为0，不代表内存条件没有改变。
}
uint64_t airi_thread_syscalls(void) { // 此观察本身是一次真实45号系统调用。
    return (uint64_t)thread_call(45, 0, 0, 0); // 保留完整64位无符号计数，上限饱和不回绕。
}
