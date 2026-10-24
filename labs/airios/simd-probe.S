.section .text /* 真实用户浮点状态观察，不链接到内核。 */
.macro SET_PROBE /* 给fork或失败exec建立可辨识的非默认状态。 */
    fninit /* 不沿用上一次探针残留的x87栈。 */
    fld1 /* x87栈留下一个真正的1，保存时不能只处理空栈。 */
    fldcw probe_cw(%rip) /* 改成0x077f的向下舍入控制环境。 */
    ldmxcsr probe_mxcsr(%rip) /* SSE使用对应的0x3f80环境。 */
    movabsq $0x123456789abcdef0, %rcx /* 该模式不等于新进程的零值。 */
    movq %rcx, %xmm15 /* 使用高组寄存器，不能遗漏XMM8至15。 */
    punpcklqdq %xmm15, %xmm15 /* 同一64位模式填满128位，不只检查低半。 */
.endm /* 结束环境准备。 */
.macro CHECK_PROBE failed /* failed由调用点提供，所有失败走同一返回位置。 */
    movabsq $0x123456789abcdef0, %rcx /* 内核返回后重新建立独立的期望GPR值。 */
    movq %xmm15, %rdx /* 读取实际低64位，不能用内存副本替代。 */
    cmpq %rcx, %rdx /* 与探针模式比较。 */
    jne \failed /* 低半丢失或串到其他进程。 */
    movhlps %xmm15, %xmm0 /* 将高64位移到一个临时寄存器低半。 */
    movq %xmm0, %rdx /* 再读取高半模式。 */
    cmpq %rcx, %rdx /* 高寄存器的整个128位都应保留。 */
    jne \failed /* 不能只保存部分向量。 */
    fnstcw 0(%rsp) /* 观察本次调用后的实际x87控制字。 */
    cmpw $0x077f, 0(%rsp) /* 不能从新进程默认值冒充fork继承。 */
    jne \failed /* 环境不符。 */
    stmxcsr 4(%rsp) /* 观察实际SSE控制与状态。 */
    cmpl $0x3f80, 4(%rsp) /* 这段探针没有做会设置异常标志的运算。 */
    jne \failed /* SSE环境也应保持。 */
    fld1 /* 另压一个1，与原来保存的值比较。 */
    fucomip %st(1), %st /* 比较后弹出新值；原值若未保留会得到unordered。 */
    jp \failed /* 拒绝空栈或NaN造成的无序比较。 */
    jne \failed /* 原非空x87数据必须仍为1。 */
    fstp %st(0) /* 弹出原值，让探针返回时不留下栈元素。 */
.endm /* 结束状态核对。 */
.global airi_simd_fork_probe /* 同一次int80后父子都先检查，再返回C。 */
.type airi_simd_fork_probe,@function /* 普通用户C调用约定。 */
airi_simd_fork_probe: /* 无参数，返回真正fork的PID/0或探针错误。 */
    subq $24, %rsp /* 为控制字快照保留独立栈空间，仍按16字节对齐。 */
    fnstcw 8(%rsp) /* 普通C调用要求被调用者保留x87控制字，先记住调用者环境。 */
    stmxcsr 12(%rsp) /* MXCSR控制位也是callee-saved，本探针结束后恢复。 */
    SET_PROBE /* 两分支都应继承这里的完整状态。 */
    movq $6, %rax /* AiriOS自有fork编号，不是Linux编号。 */
    int $0x80 /* 内核保存状态并构造孩子。 */
    movq %rax, %r8 /* 保留原fork结果，后面的比较借用其他GPR。 */
    CHECK_PROBE fork_failed /* 父子在任何C代码覆盖寄存器以前验证。 */
    movq %r8, %rax /* 真正PID或0交回C。 */
    fldcw 8(%rsp) /* 原控制字在fork的父子用户栈中分别存在，返回C前恢复。 */
    ldmxcsr 12(%rsp) /* 恢复调用者的控制位与原状态，不改变已完成的探针判断。 */
    addq $24, %rsp /* 结束本函数局部栈范围。 */
    ret /* 父子沿各自复制的用户栈返回。 */
fork_failed: /* 失败不会在内核中伪造状态。 */
    fninit /* 清理可能损坏的探针x87栈后返回普通错误。 */
    movq $-201, %rax /* 与内核fork资源错误分开。 */
    fldcw 8(%rsp) /* 原控制字在fork的父子用户栈中分别存在，返回C前恢复。 */
    ldmxcsr 12(%rsp) /* 恢复调用者的控制位与原状态，不改变已完成的探针判断。 */
    addq $24, %rsp /* 恢复调用者栈。 */
    ret /* C工作负载报告失败。 */
.size airi_simd_fork_probe,.-airi_simd_fork_probe /* 函数边界。 */
.global airi_simd_bad_exec /* 完整环境跨失败exec继续存在。 */
.type airi_simd_bad_exec,@function /* 返回原exec错误或探针错误。 */
airi_simd_bad_exec: /* 直接系统调用避免普通C函数的caller-saved XMM规则干扰。 */
    subq $24, %rsp /* 快照空间与fork探针相同。 */
    fnstcw 8(%rsp) /* 保存普通C调用者原有x87控制字。 */
    stmxcsr 12(%rsp) /* 保存原SSE环境，探针结束不改变调用者的舍入模式。 */
    SET_PROBE /* 旧映像留下非默认CPU状态。 */
    leaq probe_broken(%rip), %rdi /* 有效用户名称，指向已有截断ELF目录项。 */
    movq $7, %rax /* AiriOS exec调用号。 */
    int $0x80 /* 应拒绝为-8，并返回旧用户现场。 */
    movq %rax, %r8 /* 在核对CPU状态以前保存原结果。 */
    CHECK_PROBE exec_failed /* 失败装载不能换成新模板或别人的状态。 */
    movq %r8, %rax /* 原-8由C继续核对。 */
    fldcw 8(%rsp) /* 原控制字在fork的父子用户栈中分别存在，返回C前恢复。 */
    ldmxcsr 12(%rsp) /* 恢复调用者的控制位与原状态，不改变已完成的探针判断。 */
    addq $24, %rsp /* 返回时不借局部数组。 */
    ret /* 仍是原ELF调用链。 */
exec_failed: /* 只有状态不符合才走这里。 */
    fninit /* 不保留错误的x87栈。 */
    movq $-202, %rax /* 与正常ENOEXEC分开报告。 */
    fldcw 8(%rsp) /* 原控制字在fork的父子用户栈中分别存在，返回C前恢复。 */
    ldmxcsr 12(%rsp) /* 恢复调用者的控制位与原状态，不改变已完成的探针判断。 */
    addq $24, %rsp /* 恢复栈。 */
    ret /* C报告精确路径。 */
.size airi_simd_bad_exec,.-airi_simd_bad_exec /* 函数边界。 */
.global airi_simd_spin /* rdi是模式，esi是有限循环次数，返回0或错误。 */
.type airi_simd_spin,@function /* 普通C调用，但内部有跨真实抢占的活寄存器。 */
airi_simd_spin: /* 调用者为父子选择不同的有限double位模式。 */
    subq $40, %rsp /* 模式、期望/实际环境和原调用者环境分开，仍保持16字节对齐。 */
    fnstcw 24(%rsp) /* 本函数会改变RC，先保存callee-saved的原x87控制字。 */
    stmxcsr 28(%rsp) /* 返回C前必须恢复原MXCSR，不能把探针环境泄漏到调用者。 */
    movq %rdi, 0(%rsp) /* x87随后从这个IEEE double字节值装载。 */
    fninit /* 本轮只有一个活x87元素。 */
    fldl 0(%rsp) /* 将本进程模式对应的double留在x87栈中。 */
    movl %edi, %r8d /* 低一位让父子采用不同控制环境。 */
    andl $1, %r8d /* 只选择当前支持的两种舍入模式。 */
    shll $10, %r8d /* x87 RC位从第10位开始。 */
    orl $0x037f, %r8d /* 异常保持掩码，其余默认控制保持。 */
    movw %r8w, 8(%rsp) /* 本进程期望的控制字。 */
    fldcw 8(%rsp) /* 将其写入真实x87控制寄存器。 */
    movl %edi, %r8d /* SSE也独立采用相同的模式选择。 */
    andl $1, %r8d /* 清除模式以外的位。 */
    shll $13, %r8d /* MXCSR RC位从第13位开始。 */
    orl $0x1f80, %r8d /* 默认异常掩码与零状态标志。 */
    movl %r8d, 12(%rsp) /* 本进程期望的MXCSR。 */
    ldmxcsr 12(%rsp) /* 真实写入用户SSE环境。 */
    movq %rdi, %xmm15 /* 向量与x87都保持本进程特有模式。 */
    punpcklqdq %xmm15, %xmm15 /* 高半也保持同一模式。 */
    movl %esi, %ecx /* 有界纯计算，不用yield制造主动切换。 */
1: /* 真实PIT可在循环内任意指令打断并切另一个用户。 */
    movq %xmm15, %rax /* 每轮直接观察活寄存器。 */
    cmpq %rdi, %rax /* 与当前用户GPR保存的模式比较。 */
    jne spin_failed /* 串进另一个进程或被清成零都要失败。 */
    subl $1, %ecx /* 原请求次数由C固定为正数。 */
    jne 1b /* 回到前面的数字标签1，期间没有系统调用。 */
    movhlps %xmm15, %xmm0 /* 核对128位向量的高半。 */
    movq %xmm0, %rax /* 读取高半数值。 */
    cmpq %rdi, %rax /* 不能遗漏高64位。 */
    jne spin_failed /* 状态不符。 */
    fnstcw 16(%rsp) /* 与本进程栈中期望值分别保存。 */
    movw 16(%rsp), %ax /* cmp不能直接比较两个内存操作数，先读一个。 */
    cmpw %ax, 8(%rsp) /* 核对x87控制环境。 */
    jne spin_failed /* 父子的RC位不同，交换会暴露。 */
    stmxcsr 20(%rsp) /* SSE环境也须属于当前身份。 */
    movl 20(%rsp), %eax /* 读回控制与状态。 */
    cmpl %eax, 12(%rsp) /* 纯比较没有设置浮点异常标志。 */
    jne spin_failed /* 不接受另一个用户的MXCSR。 */
    fldl 0(%rsp) /* 再装入同一double作为期望值。 */
    fucomip %st(1), %st /* 与抢占前留在x87里的值比较并弹掉期望。 */
    jp spin_failed /* 无序不算相等。 */
    jne spin_failed /* 活x87数据也必须保持。 */
    fstp %st(0) /* 清掉本轮原值，回C时栈为空。 */
    xorl %eax, %eax /* 0表示本轮状态保持。 */
    fldcw 24(%rsp) /* 恢复调用者x87控制，空栈数据不作为返回值。 */
    ldmxcsr 28(%rsp) /* 恢复原SSE控制与状态，符合普通C调用约定。 */
    addq $40, %rsp /* 恢复用户栈。 */
    ret /* 普通调用者可读取tick并开始下一轮。 */
spin_failed: /* 错误只结束当前用户工作负载。 */
    fninit /* 不将坏栈传给后续C调用。 */
    movl $1, %eax /* 与本轮正常0区分。 */
    fldcw 24(%rsp) /* 恢复调用者x87控制，空栈数据不作为返回值。 */
    ldmxcsr 28(%rsp) /* 恢复原SSE控制与状态，符合普通C调用约定。 */
    addq $40, %rsp /* 回到原调用者帧。 */
    ret /* 用户返回码交给shell。 */
.size airi_simd_spin,.-airi_simd_spin /* 完整函数边界。 */
.section .rodata /* 控制常量属于用户ELF只读段。 */
.balign 4 /* MXCSR按四字节排列。 */
probe_mxcsr: /* 期望SSE环境。 */
.long 0x3f80 /* 所有异常掩码，向下舍入。 */
probe_cw: /* 期望x87环境。 */
.word 0x077f /* 默认精度和异常掩码，向下舍入。 */
probe_broken: /* 用户虚拟地址中的名称。 */
.asciz "broken" /* 复用既有坏ELF，不破坏正常程序。 */
.section .note.GNU-stack,"",@progbits /* 不需要可执行栈。 */
