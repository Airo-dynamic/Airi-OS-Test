/* x86-64 Linux only. No libc, no main, no CRT startup. */
.section .rodata  /* 文字字节放在只读数据节，由ELF段映射供write读取。 */
message:  /* 标签绑定下一字节地址，本身不输出字节。 */
    .ascii "raw: _start -> syscall(write) -> syscall(exit)\n"  /* 原样存字节，含末尾换行但不自动加NUL。 */
.set message_length, .-message  /* .为当前汇编位置，减起点得到消息字节数；.set绑定这个常量。 */

.section .text  /* 后续机器指令放入可执行代码节。 */
.global _start, airi_before_write, airi_after_write  /* 入口与观察标签导出，链接器/GDB可按名称找到。 */
.type _start, @function  /* 符号表把_start标为函数，供反汇编和调试工具识别。 */
_start:  /* ELF入口从此开始；RSP仍指向Linux准备的argc和指针数组。 */
    mov $1, %eax  /* $表示立即数，写eax会清零rax高32位；调用号1选择write。 */
    mov $1, %edi  /* 第一整数参数rdi取fd=1，通常是当前进程标准输出。 */
    lea message(%rip), %rsi  /* 以RIP相对地址形成message指针，lea不读取消息内容。 */
    mov $message_length, %edx  /* 第三个参数为需写出的字节数，不靠NUL找末尾。 */
airi_before_write:  /* GDB在这里停下，寄存器已经准备好而请求尚未进入内核。 */
    syscall  /* 转入Linux服务入口；rax变为返回值，rcx/r11被机制改写。 */
airi_after_write:  /* 返回后下一指令地址，用于核对GDB单步确实跨过syscall。 */
    cmp $message_length, %rax  /* 比较实际成功字节数与要求长度，短写或负错误都不相等。 */
    jne failed  /* 不相等跳到失败退出，此小演示拒绝短写而不实现重试循环。 */
    xor %edi, %edi  /* 同一寄存器异或得到零，第一退出参数为成功状态0。 */
    jmp exit_now  /* 成功也进入统一退出调用，不返回未知调用者。 */
failed:  /* write没完整完成时从这里建立失败状态。 */
    mov $1, %edi  /* 退出状态1交给Shell，不能因打印部分文字就算成功。 */
exit_now:  /* 两种结果在此共享Linux退出请求。 */
    mov $60, %eax  /* x86-64 Linux调用号60为exit。 */
    syscall  /* 内核结束当前进程，此调用正常情况下不返回。 */
.size _start, .-_start  /* 当前位置减入口得到函数机器字节长度，保存为符号元数据。 */
.section .note.GNU-stack,"",@progbits  /* 空标记节声明不需要可执行栈，供链接器形成栈权限。 */
