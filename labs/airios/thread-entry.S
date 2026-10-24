.section .text /* 桥接器与工作函数都在用户RX段，不进入内核代码。 */
.global airi_thread_entry /* 用户库把该地址交给自有创建调用。 */
.type airi_thread_entry, @function /* 链接和调试工具知道这是一个函数入口。 */
airi_thread_entry: /* 内核给RDI函数、RSI参数，RSP在call前按16字节对齐。 */
    mov %rdi, %rax /* 先保存函数地址，下一步要复用RDI传真正参数。 */
    mov %rsi, %rdi /* 普通C工作函数的第一个参数使用RDI。 */
    call *%rax /* 间接调用压入8字节返回地址，满足C入口RSP余8的ABI。 */
    mov %rax, %rdi /* 工作函数返回的long完整交到线程返回调用参数。 */
    mov $40, %rax /* 自有工作者结束号，不是整个进程exit。 */
    int $0x80 /* 保留结果等主执行者join，不释放仍在使用的内核栈。 */
    ud2 /* 正常结束不会返回，异常返回形成明确用户故障。 */
.size airi_thread_entry, .-airi_thread_entry /* 调试符号范围不包含后面的非代码节。 */
.section .note.GNU-stack,"",@progbits /* 不申请可执行栈。 */
