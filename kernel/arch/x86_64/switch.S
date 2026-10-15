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
