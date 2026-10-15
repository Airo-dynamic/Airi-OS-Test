.section .text /* 独立用户ELF的入口，不是内核_start。 */
.global _start /* 用户链接脚本选择此符号。 */
.type _start,@function /* 调试器识别用户入口函数。 */
_start: /* RSP指向内核准备的argc/argv最小启动布局。 */
    xorq %rbp, %rbp /* 初始调用链没有上一层C帧。 */
    movq (%rsp), %rdi /* 第一个C参数argc来自栈首。 */
    leaq 8(%rsp), %rsi /* 第二个参数argv指向紧随的指针数组。 */
    andq $-16, %rsp /* 调用C前保持16字节对齐。 */
    call main /* 运行用户自己链接的普通C函数。 */
    movl %eax, %edi /* main返回值作为exit第一个参数。 */
    call airi_exit /* 用户库通过AiriOS系统调用退出。 */
    ud2 /* _Noreturn函数意外返回时明确失败。 */
.size _start,.-_start /* 记录入口大小。 */
.section .note.GNU-stack,"",@progbits /* 用户栈不要求执行权限。 */
