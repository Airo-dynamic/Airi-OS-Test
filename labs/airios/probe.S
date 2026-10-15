.section .rodata.user_probe,"a" /* 这些字节由内核复制到用户RX页，内核本身不执行。 */
.global airi_user_probe_start, airi_user_probe_end /* 连续映像边界，不是ELF装载接口。 */
airi_user_probe_start: /* 映像内引用用RIP相对；固定布局的测试地址保留立即数。 */
    pushfq /* 用户保存可读标志到自己的有效栈。 */
    orq $0x4000, (%rsp) /* 用户尝试置NT，内核返回时必须清掉。 */
    popfq /* 该标志不能被直接带入内核iretq。 */
    movq $3, %rax /* AiriOS getpid，编号与Linux不兼容。 */
    int $0x80 /* DPL3的门切到TSS内核栈。 */
    cmpq $1, %rax /* 第一个演示程序应得到PID1。 */
    jne 9f /* 返回身份错误则明确退出失败。 */
    movq $1, %rax /* AiriOS write(fd, pointer, bytes)。 */
    movq $1, %rdi /* 只支持串口stdout，fd为1。 */
    leaq 5f(%rip), %rsi /* 消息地址随映像复制一起移动。 */
    movq $(6f-5f), %rdx /* 长度不含隐式NUL。 */
    int $0x80 /* 内核先检查完整用户范围再复制。 */
    movq $1, %rax /* 再次请求write，故意传内核高地址。 */
    movq $0xffff800000000000, %rsi /* 用户不能借系统调用读取内核数据。 */
    movq $8, %rdx /* 小长度也必须检查地址权限。 */
    int $0x80 /* 应返回-EFAULT而不让内核PF。 */
    cmpq $-14, %rax /* 只接受明确的地址错误。 */
    jne 9f /* 错误处理缺失就退出失败。 */
    movq $1, %rax /* 演示跨越映射右边界的缓冲区。 */
    movq $0x70000ffc, %rsi /* 栈最后一页末尾四字节有效。 */
    movq $8, %rdx /* 后四字节超出用户栈映射。 */
    int $0x80 /* 必须整段拒绝，不提前输出前四字节。 */
    cmpq $-14, %rax /* 边界跨页也必须返回-EFAULT。 */
    jne 9f /* 不能只查起点。 */
    movq $5, %rax /* 读取当前实际时钟tick。 */
    int $0x80 /* 这里只返回数值，不阻塞。 */
    movq %rax, %rbx /* 保存用户态等待基准。 */
1: /* 主动在用户态消耗一小段CPU，等待真实IRQ打断。 */
    movl $200000, %ecx /* 有界内部循环不依赖宿主CPU频率。 */
2: /* 每次只给处理器忙等提示。 */
    pause /* 与内核相同指令，但当前CPL为3。 */
    decl %ecx /* 当前循环剩余步数减少。 */
    jnz 2b /* 2b引用前面最近标签2。 */
    movq $5, %rax /* 重新查询tick。 */
    int $0x80 /* 内核系统调用时IF为零，tick需在用户执行期间交付。 */
    subq %rbx, %rax /* 计算这段用户执行期间经过的事件数。 */
    cmpq $2, %rax /* 至少两次真实时钟。 */
    jb 1b /* 不用固定空循环次数冒充IRQ完成。 */
    movq $1, %rax /* 输出非法指针都被拒绝的用户标记。 */
    movq $1, %rdi /* stdout。 */
    leaq 7f(%rip), %rsi /* 用户映像内的第二段消息。 */
    movq $(8f-7f), %rdx /* 只输出实际消息字节。 */
    int $0x80 /* 输出仍经过内核检查。 */
    xorq %rdi, %rdi /* 正常退出码0。 */
    jmp 10f /* 两条路径共用exit系统调用。 */
9: /* 任意行为不符时由用户程序明确失败。 */
    movq $90, %rdi /* 非零退出码交给内核验收。 */
10: /* 不依赖宿主运行时结束用户执行。 */
    movq $2, %rax /* AiriOS exit。 */
    int $0x80 /* 成功退出不会返回此处。 */
    ud2 /* 若内核错误地返回，触发明确用户故障。 */
5: .ascii "[USER_HELLO] ring3 via int80\n" /* 用户缓冲区，不是内核伪造输出。 */
6: /* 第一段消息的半开右端。 */
7: .ascii "[USER_POINTERS_OK] kernel and crossing buffers rejected\n" /* 用户已经核对两次-EFAULT。 */
8: /* 第二段消息的右端。 */
airi_user_probe_end: /* 内核只复制边界之间的字节。 */
.global airi_user_bad_start, airi_user_bad_end /* 第二个程序只验证用户指令权限。 */
airi_user_bad_start: /* CPL3没有IOPL权限修改IF。 */
    cli /* 应产生用户GP，而不是关闭系统时钟。 */
    ud2 /* 若cli错误地成功，仍不能被当成正常退出。 */
airi_user_bad_end: /* 第二段独立映像右端。 */
.section .note.GNU-stack,"",@progbits /* 映像不要求可执行栈。 */
.section .rodata.user_probe,"a" /* 继续保存独立受控映像，不要求内核执行。 */
.global airi_user_load_bad_start, airi_user_load_bad_end /* 验证硬件用户位而非系统调用软件检查。 */
airi_user_load_bad_start: /* kernel起点确实已映射，但所有层禁止user。 */
    movq $0xffffffff80000000, %rax /* 固定内核只读请求段地址。 */
    movq (%rax), %rax /* ring3读supervisor页，应产生PF error=5。 */
    ud2 /* 权限若错误地允许读取，不能宣称正常退出。 */
airi_user_load_bad_end: /* 第三段映像右端。 */
.global airi_user_rsp_bad_start, airi_user_rsp_bad_end /* 验证内核不能把坏RSP直接交给iretq。 */
airi_user_rsp_bad_start: /* mov寄存器本身不会访问这个地址。 */
    movq $0x0000800000000000, %rsp /* 构造48位不规范用户栈地址。 */
    movq $3, %rax /* getpid仍能经TSS内核栈进入系统调用。 */
    int $0x80 /* 返回前必须拒绝坏现场，不能让iretq在kernel里GP。 */
    ud2 /* 如果错误地返回用户，则结果与期望不符。 */
airi_user_rsp_bad_end: /* 第四段映像右端。 */
