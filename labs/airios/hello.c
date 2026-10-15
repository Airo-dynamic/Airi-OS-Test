#include <airi_libc.h> // 只链接用户库，不引用内核符号。
static volatile unsigned char initialized[4] = {0x41, 0x49, 0x52, 0x49}; // 验证ELF文件提供的可写初始化数据。
static volatile unsigned char zeroed[8192]; // BSS横跨多页，必须由内核按memsz清零。
int main(int argc, char **argv) { // C参数由用户_start从用户初始栈取得。
    if (argc != 1 || !argv || !argv[0] || argv[1] || strlen(argv[0]) != 5 || memcmp(argv[0], "hello", 6)) // 核对最小argc/argv布局和用户字符串。
        return 91; // 用户程序自己报告参数传递失败。
    if (initialized[0] != 0x41 || initialized[3] != 0x49) // .data必须来自ELF文件内容。
        return 92; // 不能把所有段都只清零。
    for (size_t i = 0; i < sizeof(zeroed); ++i) // 核对每个BSS字节，而不是只看首字节。
        if (zeroed[i]) // 页池旧内容不能泄漏进新用户映像。
            return 93; // 不输出成功标记。
    initialized[0] = 0x42; // .data确实可写。
    zeroed[sizeof(zeroed) - 1] = 0x5a; // BSS最后一页也应可写，证明memsz全部映射。
    char bytes[16]; // 用户局部对象位于自己的栈。
    memset(bytes, 0, sizeof(bytes)); // 用户库填充，不调用内核memset。
    memcpy(bytes, "ABCD", 5); // 复制含NUL的四字符字符串。
    memmove(bytes + 1, bytes, 5); // 重叠向后移动，结果为AABCD加NUL。
    if (memcmp(bytes, "AABCD", 6) || zeroed[sizeof(zeroed) - 1] != 0x5a || initialized[0] != 0x42 || airi_getpid() <= 0) // 核对用户库、数据权限和系统调用身份。
        return 94; // 结果不符则main非零退出。
    const char *message = "[ELF_USER_OK] argc=1 data=initialized bss=8192 libc=own\n"; // 该消息来自独立用户ELF的只读段。
    if (airi_puts(message) != (long)strlen(message)) // 真实输出也必须返回完整逻辑长度。
        return 95; // 错误输出不被计作完成。
    return 0; // 用户_start再通过airi_exit把结果交给内核。
}
