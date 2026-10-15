#define _GNU_SOURCE // Linux提供匿名mmap、proc和当前实验用的POSIX扩展声明。
#include <errno.h> // 失败后立即读取errno的文字表示。
#include <signal.h> // SIGSEGV用于识别只读页写入的进程结果。
#include <stdint.h> // uintptr_t承接指针数值。
#include <stdio.h> // 读取proc并输出实验结果。
#include <stdlib.h> // EXIT_FAILURE和exit。
#include <string.h> // 只筛选当前VMA的几个字段。
#include <sys/mman.h> // mmap、mprotect、munmap。
#include <sys/resource.h> // 禁止core文件并观察minor fault计数。
#include <sys/wait.h> // 父进程等待故障子进程。
#include <unistd.h> // 页大小、PID和fork。
static void fail(const char *operation) { // 保留具体失败系统接口名。
    perror(operation); // 当前errno在做其他系统操作前写到stderr。
    exit(EXIT_FAILURE); // 失败不继续运行后续观察。
}
static int snapshot(const char *stage, uintptr_t address) { // 只读取包含这个地址的VMA统计。
    struct rusage usage; // 观察本进程累计缺页，不把它解释成某个VMA独占数据。
    if (getrusage(RUSAGE_SELF, &usage) != 0) // 系统调用失败需要明确报告。
        fail("getrusage"); // 不输出未初始化计数。
    printf("\n[%s] pid=%ld address=0x%lx minor_faults=%ld\n", stage, (long)getpid(), (unsigned long)address, usage.ru_minflt); // 计数为进程累计值。
    FILE *maps = fopen("/proc/self/smaps", "r"); // Linux内核提供本进程映射与驻留统计。
    if (!maps) // proc未挂载或无权限时不能伪造RSS。
        fail("open /proc/self/smaps"); // 明确环境限制。
    char line[512]; // 一行足以读取映射头与本实验选择的统计字段。
    int inside = 0, found = 0; // 当前行是否属于目标VMA，以及是否找到过它。
    while (fgets(line, sizeof(line), maps)) { // 逐行消费同一次proc快照读取。
        unsigned long long start, end; // Linux映射头中的十六进制半开范围。
        char permissions[5]; // 四个权限字符加终止零。
        if (sscanf(line, "%llx-%llx %4s", &start, &end, permissions) == 3) { // 新映射头开始，重新判断所属VMA。
            inside = address >= start && address < end; // 目标地址只属于包含它的一段。
            if (inside) { // 输出实际映射范围和权限，可能被内核和邻居合并。
                found = 1; // 后面可判断解除后是否仍有映射。
                fputs(line, stdout); // 保留真实头，不按请求长度编造VMA。
            }
        } else if (inside && (!strncmp(line, "Size:", 5) || !strncmp(line, "Rss:", 4) || !strncmp(line, "Private_Dirty:", 14) || !strncmp(line, "AnonHugePages:", 14))) // 只选择当前机制相关字段。
            fputs(line, stdout); // RSS和巨大页数都是本次实际内核统计。
    }
    if (ferror(maps)) // EOF和读取失败必须区分。
        fail("read smaps"); // 不把截断快照当完成。
    fclose(maps); // 结束对proc文件的借用。
    if (!found) // munmap后预期没有任何VMA覆盖旧地址。
        puts("no VMA contains this address"); // 不解引用已释放指针，只比较地址数值。
    return found; // 主流程检查映射存在与解除的结果。
}
int main(void) { // 普通Linux用户态程序，由Linux拥有页表和物理页。
#ifndef __linux__ // 源码可在Mac检查语法，运行明确拒绝冒充Linux。
    fputs("This experiment requires Linux /proc and Linux VM semantics.\n", stderr); // 给出环境原因。
    return 77; // 77表示未在所需环境运行，不是实验成功。
#endif // Linux继续实际观察。
    struct rlimit no_core = {0, 0}; // 只读写入的预期信号不生成core文件。
    if (setrlimit(RLIMIT_CORE, &no_core) != 0) // 禁止修改无关主机配置，仅限制本进程及其子进程。
        fail("setrlimit core"); // 设置失败也保留原因。
    long page = sysconf(_SC_PAGESIZE); // 不假定Linux目标也一定使用4KiB页。
    if (page <= 0) // 查询失败不能作为循环步长。
        fail("sysconf page size"); // 避免无穷循环。
    size_t bytes = 64u * 1024u * 1024u; // 申请64MiB匿名虚拟范围，不等于立刻占用64MiB物理RAM。
    volatile unsigned char *mapping = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); // -1表示不由文件FD提供内容，偏移为零。
    if (mapping == MAP_FAILED) // mmap失败值是(void*)-1，不是NULL。
        fail("mmap"); // 不访问失败哨兵。
    uintptr_t address = (uintptr_t)mapping; // 保存数值用于后续proc范围查询。
    printf("page_bytes=%ld requested=%zu\n", page, bytes); // 记录当前环境而非写死页大小。
#ifdef MADV_NOHUGEPAGE // Linux支持时减少透明巨大页对RSS步进的影响。
    if (madvise((void *)mapping, bytes, MADV_NOHUGEPAGE) != 0) // 建议失败不改变mmap本身的成功。
        perror("madvise NOHUGEPAGE (continuing)"); // 后面仍打印AnonHugePages说明实际环境。
#endif // 不支持时也可以观察，不宣称固定粒度。
    if (!snapshot("reserved, not touched", address)) // 地址范围必须已经出现在映射图。
        fail("new VMA not found"); // 不用进程总RSS代替映射存在性。
    mapping[0] = 0x41; // 第一次真实写入，Linux为该范围处理所需缺页。
    snapshot("one page written", address); // 对比该VMA实际RSS和进程minor fault计数。
    for (size_t i = 0; i < bytes; i += (size_t)page) // 每页至少写一个字节，不只reserve地址。
        mapping[i] = 0x41; // volatile保证编译器保留这些实际访问。
    snapshot("all pages written", address); // 不强制RSS精确等于请求量，保留实际结果。
    if (mprotect((void *)mapping, (size_t)page, PROT_READ) != 0) // 首个页改成只读，Linux更新页表和缓存。
        fail("mprotect read-only"); // 权限设置失败不能继续预期故障。
    snapshot("first page read-only", address); // VMA可能被分成只读首段和后续读写段。
    fflush(NULL); // fork前刷完stdio缓冲，避免子进程复制旧输出后重复打印。
    pid_t child = fork(); // 返回0的是子进程；父进程得到其PID；失败为-1。
    if (child < 0) // 失败没有子进程可以等待。
        fail("fork fault child"); // 不继续访问未知PID。
    if (child == 0) { // 只让子进程执行预期非法写，父进程负责记录结果。
        mapping[0] = 0x42; // 只读页写入应被Linux转为SIGSEGV。
        _exit(99); // 若错误地返回这里，父进程会把它判为失败。
    }
    int status; // waitpid写入编码后的退出或信号状态。
    if (waitpid(child, &status, 0) != child) // 精确等待刚创建的那个子进程。
        fail("waitpid"); // 不把未取得状态当成正常退出。
    if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGSEGV || mapping[0] != 0x41) { // 要求实际信号终止且父进程仍可读取原值。
        fputs("read-only fault did not produce the expected SIGSEGV\n", stderr); // 明确哪项机制未成立。
        return EXIT_FAILURE; // 不输出完成标记。
    }
    printf("fault_child=%ld signal=%d parent_value=0x%x\n", (long)child, WTERMSIG(status), mapping[0]); // Linux将用户访问故障限制在进程内。
    if (munmap((void *)mapping, bytes) != 0) // 结束整个范围的借用，包括被mprotect拆分的VMA。
        fail("munmap"); // 失败不能声称映射已解除。
    if (snapshot("unmapped", address)) // 这里只比较旧地址数值，绝不再次访存。
        return EXIT_FAILURE; // 旧VMA仍存在则拒绝完成。
    puts("[LINUX_P2_OK] reserve-touch-protect-fault-unmap"); // 所有实际步骤完成才发布标记。
    return 0; // 正常返回运行时退出。
}
