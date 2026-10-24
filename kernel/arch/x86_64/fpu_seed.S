.section .text /* 启动CPU状态清理只在独立汇编中使用浮点与XMM。 */
.global airi_fpu_seed /* rdi指向内核16字节对齐的512字节模板。 */
.type airi_fpu_seed,@function /* 调试器可定位这段实际状态初始化。 */
airi_fpu_seed: /* 能力和CR0/CR4已由C初始化检查。 */
    fninit /* 先建立空x87栈与默认控制字，避免旧异常干扰。 */
    .rept 8 /* 八个物理x87数据寄存器都需要清除，空标签本身不清数据。 */
    fldz /* 每次压一个真正的正零，八次覆盖全部数据寄存器。 */
    .endr /* 结束八次压栈展开。 */
    fninit /* 数据仍为零，标签重新标空，控制字恢复0x037f。 */
    ldmxcsr airi_fpu_default_mxcsr(%rip) /* SSE异常掩码和舍入采用0x1f80，状态标志为零。 */
    pxor %xmm0, %xmm0 /* 逐个清除用户未来可读到的128位寄存器。 */
    pxor %xmm1, %xmm1 /* 不依赖固件退出时留下的值。 */
    pxor %xmm2, %xmm2 /* 新进程不继承旧向量数据。 */
    pxor %xmm3, %xmm3 /* 保留统一零模板。 */
    pxor %xmm4, %xmm4 /* 同样覆盖低组寄存器。 */
    pxor %xmm5, %xmm5 /* 所有128位都清零。 */
    pxor %xmm6, %xmm6 /* 不只清低64位。 */
    pxor %xmm7, %xmm7 /* 低八个寄存器处理完毕。 */
    pxor %xmm8, %xmm8 /* 64位用户还可访问高八个寄存器。 */
    pxor %xmm9, %xmm9 /* 同一模板覆盖完整64位模式集合。 */
    pxor %xmm10, %xmm10 /* 不将高组遗漏在上下文之外。 */
    pxor %xmm11, %xmm11 /* 初始向量全部为零。 */
    pxor %xmm12, %xmm12 /* 后续进程分别复制此模板。 */
    pxor %xmm13, %xmm13 /* 不能只处理普通C常用的前几个寄存器。 */
    pxor %xmm14, %xmm14 /* 清除高组中的倒数第二项。 */
    pxor %xmm15, %xmm15 /* 新用户探针会明确检查这一高寄存器。 */
    fxsave64 (%rdi) /* 只保存，不再次清空；得到可用FXRSTOR64恢复的映像。 */
    ret /* 返回仍只使用通用寄存器的内核C。 */
.size airi_fpu_seed,.-airi_fpu_seed /* 固定汇编函数边界。 */
.section .rodata /* 默认控制值由内核只读区持有。 */
.balign 4 /* MXCSR输入按四字节对齐。 */
airi_fpu_default_mxcsr: /* 没有用户指针参与。 */
.long 0x1f80 /* 默认舍入到最近、所有SSE浮点异常掩码开启。 */
.section .note.GNU-stack,"",@progbits /* 不需要可执行栈。 */
