.section .text /* 异常入口属于可执行段。 */
.global airi_tables_load /* 由 C 初始化函数调用。 */
.type airi_tables_load,@function /* 标记为函数供 ELF 调试器识别。 */
airi_tables_load: /* rdi 指向 GDT 描述符，rsi 指向 IDT 描述符。 */
    lgdt (%rdi) /* 读取 10 字节的 GDT 界限与基地址。 */
    pushq $8 /* 远返回将选择 GDT 中的内核代码段。 */
    leaq 1f(%rip), %rax /* 1f 表示往后最近的数字标签 1。 */
    pushq %rax /* 压入远返回的目标指令地址。 */
    lretq /* 同时装入 RIP 和 CS，切到自己的代码段。 */
1: /* 已经使用自己的 CS=8。 */
    movw $16, %ax /* GDT 第二项是内核数据段。 */
    movw %ax, %ds /* 设置数据段选择子。 */
    movw %ax, %es /* 设置额外数据段选择子。 */
    movw %ax, %ss /* 设置内核栈段选择子。 */
    movw $40, %ax /* TSS 描述符从 GDT 第五项开始，占两项。 */
    ltr %ax /* 加载 TSS，提供双重故障 IST 与用户跨权时的 RSP0。 */
    lidt (%rsi) /* 后续异常从自己的 IDT 取入口。 */
    ret /* 返回 C 初始化函数。 */
.size airi_tables_load, .-airi_tables_load /* 记录函数长度。 */

.macro TRAP number, error=0 /* 为每个异常生成同形的栈；默认没有硬件错误码。 */
.global airi_trap_\number /* C 入口表引用这个符号。 */
airi_trap_\number: /* 宏参数拼成唯一标签。 */
.if !\error /* 只有 CPU 没有压错误码时才补零。 */
    pushq $0 /* 补足统一的 error 槽位。 */
.endif /* 结束错误码差异。 */
    pushq $\number /* 每个入口固定自己的 vector。 */
    jmp airi_trap_common /* 共享寄存器保存和 C 调用。 */
.endm /* 结束宏定义。 */
TRAP 0 /* 除零，无硬件错误码。 */
TRAP 1 /* 调试异常。 */
TRAP 2 /* 不可屏蔽中断，当前不主动启用设备源。 */
TRAP 3 /* 断点。 */
TRAP 4 /* 溢出。 */
TRAP 5 /* 边界异常。 */
TRAP 6 /* 无效指令 UD。 */
TRAP 7 /* 设备不可用。 */
TRAP 8, 1 /* 双重故障，自带零错误码。 */
TRAP 9 /* 保留的旧协处理器向量。 */
TRAP 10, 1 /* 无效 TSS。 */
TRAP 11, 1 /* 段不存在。 */
TRAP 12, 1 /* 栈段故障。 */
TRAP 13, 1 /* 一般保护 GP。 */
TRAP 14, 1 /* 页故障 PF。 */
TRAP 15 /* 保留向量。 */
TRAP 16 /* x87 浮点异常。 */
TRAP 17, 1 /* 对齐检查。 */
TRAP 18 /* 机器检查。 */
TRAP 19 /* SIMD 浮点异常。 */
TRAP 20 /* 虚拟化异常。 */
TRAP 21, 1 /* 控制保护异常。 */
TRAP 22 /* 保留向量。 */
TRAP 23 /* 保留向量。 */
TRAP 24 /* 保留向量。 */
TRAP 25 /* 保留向量。 */
TRAP 26 /* 保留向量。 */
TRAP 27 /* 保留向量。 */
TRAP 28 /* Hypervisor 注入异常，不带错误码。 */
TRAP 29, 1 /* VMM 通信异常。 */
TRAP 30, 1 /* 安全异常。 */
TRAP 31 /* 保留向量。 */
TRAP 32 /* 主PIC IRQ0对应的时钟入口，无硬件错误码。 */
TRAP 43 /* 从PIC IRQ11，当前固定PCI块设备INTx。 */
TRAP 128 /* DPL3的AiriOS系统调用门。 */
TRAP 129 /* DPL0启动/返回用户执行的内核门。 */
TRAP 255 /* 未使用的 IDT 项统一停止；可屏蔽中断仍关闭。 */
airi_trap_common: /* CPU 现场和 vector/error 已形成。 */
    cld /* C ABI 要求字符串方向标志为零；iretq 会恢复原标志。 */
    pushq %rax /* 先保存最后在 C 结构中出现的通用寄存器。 */
    pushq %rbx /* 保存被打断代码的 RBX。 */
    pushq %rcx /* 保存被打断代码的 RCX。 */
    pushq %rdx /* 保存被打断代码的 RDX。 */
    pushq %rbp /* 保存被打断代码的帧指针。 */
    pushq %rdi /* 保存被打断代码的第一参数寄存器。 */
    pushq %rsi /* 保存被打断代码的第二参数寄存器。 */
    pushq %r8 /* 保存通用寄存器 R8。 */
    pushq %r9 /* 保存通用寄存器 R9。 */
    pushq %r10 /* 保存通用寄存器 R10。 */
    pushq %r11 /* 保存通用寄存器 R11。 */
    pushq %r12 /* 保存原 R12，随后可借它保留现场指针。 */
    pushq %r13 /* 保存通用寄存器 R13。 */
    pushq %r14 /* 保存通用寄存器 R14。 */
    pushq %r15 /* 最后保存的 R15 位于结构起点。 */
    movq %rsp, %rdi /* C 的第一个实参是现场指针。 */
    movq %rsp, %r12 /* C ABI 保证被调用者保留 R12。 */
    andq $-16, %rsp /* call 之前对齐到 16 字节，不假设异常发生点的栈对齐。 */
    call airi_trap_dispatch /* C 负责报告与明确登记的演示恢复。 */
    movq %rax, %rsp /* C返回选定的完整现场，同一或另一条调用链。 */
    popq %r15 /* 从保存区域恢复通用寄存器。 */
    popq %r14 /* 顺序与压栈相反。 */
    popq %r13 /* 恢复 R13。 */
    popq %r12 /* 恢复被打断代码的原 R12。 */
    popq %r11 /* 恢复 R11。 */
    popq %r10 /* 恢复 R10。 */
    popq %r9 /* 恢复 R9。 */
    popq %r8 /* 恢复 R8。 */
    popq %rsi /* 恢复 RSI。 */
    popq %rdi /* 恢复 RDI。 */
    popq %rbp /* 恢复 RBP。 */
    popq %rdx /* 恢复 RDX。 */
    popq %rcx /* 恢复 RCX。 */
    popq %rbx /* 恢复 RBX。 */
    popq %rax /* 恢复 RAX。 */
    addq $16, %rsp /* 移除软件 vector 与统一 error 两个槽位。 */
    iretq /* 恢复 RIP、CS、RFLAGS、RSP、SS，继续被打断代码。 */

.global airi_probe_ud, airi_probe_ud_site, airi_probe_ud_resume /* C 用精确地址登记预期故障。 */
airi_probe_ud: /* 普通 C 调用进入演示函数。 */
airi_probe_ud_site: /* 仅这个地址的 UD 可以在演示时恢复。 */
    ud2 /* 必然触发向量 6，不是普通函数调用。 */
airi_probe_ud_resume: /* 处理器保存的 RIP 被明确改为此处。 */
    ret /* 回到调用演示的 C 函数。 */
.global airi_probe_gp, airi_probe_gp_site, airi_probe_gp_resume /* GP 演示地址供 C 比对。 */
airi_probe_gp: /* 原 AX 会由 C 调用约定视为可变。 */
    movw $0xffff, %ax /* 构造超出当前表界限的选择子。 */
airi_probe_gp_site: /* 故障报告的 RIP 应指向这条装载。 */
    movw %ax, %ds /* 无效选择子触发带硬件错误码的 GP。 */
airi_probe_gp_resume: /* 故障装载未成功，原 DS 仍有效。 */
    ret /* 返回 C 检查点。 */

.global airi_probe_read, airi_probe_read_site, airi_probe_read_resume /* 访问地址由 rdi 传入。 */
airi_probe_read: /* 调用者登记预期的未映射读取。 */
airi_probe_read_site: /* PF 的保存 RIP 指向这条 load。 */
    movq (%rdi), %rax /* 未映射数据访问产生 PF。 */
airi_probe_read_resume: /* 恢复时不再访问坏地址。 */
    ret /* 返回 C 检查状态。 */
.global airi_probe_write, airi_probe_write_site, airi_probe_write_resume /* supervisor 只读权限演示。 */
airi_probe_write: /* rdi 是只读映射的虚拟起点。 */
airi_probe_write_site: /* PF 的保存 RIP 指向这条 store。 */
    movq $0x55, (%rdi) /* CR0.WP 开启后只读叶子拒绝这次写。 */
airi_probe_write_resume: /* 恢复时跳过整条已知 store。 */
    ret /* 返回调用者。 */
.global airi_probe_execute, airi_probe_execute_resume /* NX 页的执行入口与恢复标签。 */
airi_probe_execute: /* 调用目标地址由 rdi 传入。 */
    call *%rdi /* 返回地址先入栈，取指时因 NX 产生 PF，RIP 是目标地址。 */
    ret /* 若未发生 PF，则 C 调用者会报告演示未发生。 */
airi_probe_execute_resume: /* PF 现场的 RSP 已包含上面 call 压入的返回地址。 */
    addq $8, %rsp /* 丢弃不能再使用的内部 call 返回地址。 */
    ret /* 回到调用 probe_execute 的 C 函数。 */

.section .rodata /* 入口地址表只读。 */
.global airi_trap_entries /* C 用这张表填 IDT。 */
airi_trap_entries: /* 连续 32 个 64 位入口地址。 */
.irp n,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31 /* irp 将下面一项按 n 展开。 */
    .quad airi_trap_\n /* 地址必须与所属向量一致。 */
.endr /* 结束重复展开。 */
.section .note.GNU-stack,"",@progbits /* 声明不需要可执行栈。 */
