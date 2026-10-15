#include <stdio.h>   // 取得主机 printf、文件输出与错误报告接口。
#include <unistd.h>  // 取得 Linux getpid 声明。

int main(int argc, char **argv) {  // 宿主运行时传入参数数量和 NUL 字符串指针数组。
    printf("ordinary: main(argc=%d), argv[0]=%s, pid=%ld\n",  // 宿主libc格式化参数数、启动名称和当前Linux进程号。
           argc, argv[0], (long)getpid());  // argc/argv来自运行时，getpid返回值转long匹配%ld。
    puts("ordinary: libc -> Linux write -> stdout");  // libc写出这行并补换行，实际系统调用数量取决于缓冲。
    return 0;  // 正常结束当前程序，成功状态交给启动它的命令。
}
