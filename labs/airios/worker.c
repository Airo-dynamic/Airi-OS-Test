#include <airi_libc.h> // 新映像使用自己的入口、用户库和地址空间。
static volatile unsigned char initialized = 0x53; // 新ELF的数据初值不能继承旧用户映像。
static volatile unsigned char zeroed[4096]; // 新BSS由装载器重新清零。
int main(int argc, char **argv) { // exec成功应从_start重建argc/argv，而不是返回旧调用。
    if (argc != 1 || !argv || !argv[0] || strlen(argv[0]) != 6 || memcmp(argv[0], "worker", 7) || initialized != 0x53) // 新参数和新数据都必须存在。
        return 71; // 旧映像残留则失败。
    for (size_t i = 0; i < sizeof(zeroed); ++i) // 检查整个BSS，不只首字节。
        if (zeroed[i]) // 不允许泄漏旧地址空间或页池内容。
            return 72; // 新入口不发布成功。
    unsigned char *page = airi_alloc(4096); // exec应重置用户页游标，旧孩子曾在同一VA写99。
    if ((uintptr_t)page != 0x10000000 || page[0]) // 新页必须重新拥有且清零。
        return 73; // 不能保留旧heap_next或旧数据。
    page[0] = 7; // 新映像拥有自己的可写页。
    zeroed[sizeof(zeroed) - 1] = 9; // BSS尾页也可写。
    airi_puts("[EXEC_WORKER_OK] argv=worker heap=zeroed bss=new\n"); // 真实来自新ELF的用户输出。
    char inherited[2]; // files工作负载把只读seed保留在FD0，普通worker没有FD0。
    long inherited_bytes = airi_read(0, inherited, 2); // 成功exec后的新程序消费同一打开对象。
    if (inherited_bytes != -9) { // 只有明确继承文件的调用才做这组检查。
        if (inherited_bytes != 2 || memcmp(inherited, "cd", 2) || airi_read(3, inherited, 1) != -9) // CLOEXEC表项已关闭，普通偏移继续。
            return 24; // 父亲期望23，因此错误不能被吞掉。
        airi_puts("[FD_EXEC_OK] inherited=0 closed=3 offset=4\n"); // 来自新ELF中的真实读结果。
    }
    return 23; // 父进程应仍通过原孩子PID取得这个状态。
}
