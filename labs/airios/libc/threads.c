#include <airi_threads.h> // 不借pthread或Linux clone包装。
#include <stdint.h> // 指针以目标64位数值进入内核。
extern void airi_thread_entry(void); // 汇编桥接器被静态链接到当前用户可执行段。
static long thread_call(long number, uint64_t first, uint64_t second, uint64_t third) { // 与既有int80前三参数约定相同。
    __asm__ volatile("int $0x80" : "+a"(number) : "D"(first), "S"(second), "d"(third) : "memory", "cc"); // 内存clobber让调用边界不隐瞒共享读写。
    return number; // 负错误保持，不映射成尚未实现的pthread返回规则。
}
long airi_thread_create(airi_thread_function function, void *argument) { // 主执行者不复制自己的用户栈或TLS运行值。
    return thread_call(39, (uintptr_t)airi_thread_entry, (uintptr_t)function, (uintptr_t)argument); // 两个代码地址均由内核检查为用户RX。
}
long airi_thread_join(long tid, long *result) { // 内核在输出前核对完整long跨度。
    return thread_call(41, (uint64_t)tid, (uintptr_t)result, 0); // 未完成时调用链真正阻塞，不由库忙轮询。
}
long airi_thread_id(void) { // 只返回当前执行身份，不产生资源所有权。
    return thread_call(42, 0, 0, 0); // PID仍使用原始airi_getpid查询。
}
