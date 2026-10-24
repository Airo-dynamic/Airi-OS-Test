#include <airi_libc.h> // 编译器生成TLS地址；用户主程序不再手工分配或设置FS。
_Thread_local volatile uint64_t initialized = 42; // .tdata提供每个新映像的相同初值。
_Thread_local volatile unsigned char zero[73]; // .tbss只有内存长度，必须真正零填充。
_Alignas(64) _Thread_local volatile uint64_t aligned = 7; // TLS对齐约束高于普通TCB的16字节。
static int initial(void) { // main在任何修改前观察装载器创建的本地副本。
    if (initialized != 42 || aligned != 7 || (uintptr_t)&aligned % 64) // 初值、实际地址与编译器对齐分别核对。
        return 1; // 不能只读PT_TLS描述却没正确复制。
    for (unsigned i = 0; i < sizeof(zero); ++i) // 检查整个零填充跨度，不只看第一个字节。
        if (zero[i]) // 旧用户内容不能进入新TLS块。
            return 2; // 明确失败。
    long base = airi_fs_base(); // 必须已经在_start调用main以前设好。
    if (base < 0x60000000 || base > 0x60001000 || base % 16 || *(uintptr_t *)(uintptr_t)base != (uintptr_t)base) // TP后的TCB首字为self。
        return 3; // 本节只支持固定静态块和local-exec模型。
    return 0; // 初始副本与默认布局完整成立。
}
int main(int argc, char **argv) { // 一份静态ELF以目录名区分普通工作与exec重置观察。
    int error = initial(); // 编译器实际使用FS负偏移读取，不调用手工TLS初始化函数。
    if (error) // 初始环境错误保留不同退出位置。
        return 210 + error; // shell可继续报告错误。
    if (argc == 1 && strlen(argv[0]) > 4 && argv[0][4] == '-') { // ctls-reset是同一ELF的新独立装载，不是旧调用返回。
        airi_puts("[CTLS_EXEC_RESET_OK] initialized=42 zero=73 aligned=64 self=valid\n"); // 新模板重新生成默认本地数据。
        return 0; // 原父按同一孩子PID取得结果。
    }
    initialized = 99; zero[0] = 14; // fork应继承当前修改后的TLS，而非再次从文件初值42开始。
    long child = airi_fork(); // 普通地址空间复制也包括固定TLS运行页。
    if (child < 0 || initialized != 99 || zero[0] != 14) // 父子返回后都看到调用点的旧值。
        return 214; // 不把fork做成exec式初始化。
    uint64_t expected = child ? 111 : 222; // 父子采用不同本地数据但相同的符号与虚拟地址。
    initialized = expected; aligned = expected + 1; // 当前进程的TLS物理页面分别拥有。
    uint64_t start = airi_ticks(); // 观察真实抢占窗口。
    do { // 每轮只有用户计算，时钟可以任意打断。
        for (unsigned i = 0; i < 5000000; ++i) // volatile使编译器每次实际读TLS变量。
            if (initialized != expected || aligned != expected + 1 || zero[0] != 14) // 保持各自值，普通GPR副本不能代替全部读。
                return 215; // 跨身份串扰即失败。
    } while (airi_ticks() - start < 20); // 二十个实际tick，非主动yield造出的轮转。
    if (airi_exec("tls-too-big") != -8 || initialized != expected || zero[0] != 14) // 超限PT_TLS必须在分配前拒绝，旧FS与值不变。
        return 216; // 不是仅在宿主解析器里跑合成错误。
    if (!child) { // 孩子成功替换同一份ELF，以新名称进入重置分支。
        airi_puts("[CTLS_CHILD_OK] fork=modified-inherited preempt=private invalid_elf=preserved\n"); // 实际C本地变量检查已完成。
        airi_exec("ctls-reset"); // 新副本应重新为42、7和全零。
        return 217; // 成功exec不回旧程序。
    }
    int status = 1; // 同一孩子在新ELF中的实际退出结果。
    if (airi_waitpid(child, &status) != child || status || initialized != 111 || aligned != 112) // 父自己的副本继续活着。
        return 218; // 等待与exec不能覆盖父TLS。
    airi_puts("[CTLS_USER_OK] c11=thread_local tdata=42 tbss=73 align=64 fork=copy exec=reset invalid=unchanged\n"); // 所有机制均完成后发布。
    airi_exec("ctls-char"); // 再覆盖仅一字节、对齐1的真实链接布局。
    return 219; // 父成功exec后由新main返回0。
}
