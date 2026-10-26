#define _GNU_SOURCE // Linux/POSIX实验使用匿名mmap与完整进程接口声明。
#include <errno.h> // execve失败返回-1，具体原因在errno。
#include <fcntl.h> // FD_CLOEXEC属于描述符标志。
#include <limits.h> // 检查传给新映像的整数范围。
#include <stdio.h> // 记录真实PID、状态和FD观察。
#include <stdlib.h> // strtol、环境变量和mkstemp。
#include <string.h> // 参数与文件内容核对。
#include <sys/mman.h> // 私有匿名页用于观察fork后的独立写入。
#include <sys/resource.h> // 记录真实minor fault，不伪造COW页计数。
#include <sys/stat.h> // 比较打开文件身份，避免把FD编号复用误判成继承。
#include <sys/wait.h> // Linux waitpid的编码状态。
#include <unistd.h> // fork、execve、read/write和PID。
static volatile int initialized = 11; // 新exec映像应恢复文件初值。
static volatile int zeroed; // 新exec映像应恢复BSS零值。
static void fail(const char *name) { // 系统接口失败立即保留errno。
    perror(name); // 不先调用其他会改变errno的接口。
    exit(1); // 实验失败不继续发布成功。
}
static void expect(int condition, const char *reason) { // 逻辑失败与系统errno分开。
    if (!condition) { // 只在事实不符时结束。
        fprintf(stderr, "expectation failed: %s\n", reason); // 不把陈旧errno当作这条逻辑失败原因。
        exit(1); // 非零状态由运行脚本保留。
    }
}
static void write_all(int fd, const void *buffer, size_t bytes) { // 小文件准备也不假定一次write必然完成。
    const unsigned char *data = buffer; // 当前尚未写出的字节起点。
    while (bytes) { // 直到全部内容交给Linux。
        ssize_t n = write(fd, data, bytes); // 返回实际完成字节数。
        if (n < 0 && errno == EINTR) // 信号中断时重试尚未完成部分。
            continue; // 不前移缓冲或误扣长度。
        if (n < 0) // 真正写入错误保留errno。
            fail("write"); // 保存失败位置。
        expect(n > 0, "write made no progress"); // 零进展是独立结果，不借用陈旧errno。
        data += n; // 跳过本次实际写出字节。
        bytes -= (size_t)n; // 只扣除已完成部分。
    }
}
static void read_two(int fd, char *out) { // 此实验精确读取两字节以观察共享偏移。
    size_t done = 0; // 当前完整读取数量。
    while (done < 2) { // 允许短读，但文件必须含足够字节。
        ssize_t n = read(fd, out + done, 2 - done); // 读取仍缺少部分。
        if (n < 0 && errno == EINTR) // 中断不表示EOF。
            continue; // 保留当前完成数量。
        if (n < 0) // 真正读取错误。
            fail("read"); // 明确错误。
        expect(n > 0, "unexpected EOF"); // 不能把不足两字节算共享偏移通过。
        done += (size_t)n; // 推进实际数量。
    }
}
static int number(const char *text) { // 新映像只能从argv字符串接收旧FD数值和PID。
    char *end; // strtol告诉我们是否有未消费后缀。
    errno = 0; // 区分溢出与合法数值。
    long value = strtol(text, &end, 10); // 以十进制读取。
    expect(!errno && *text && !*end && value >= 0 && value <= INT_MAX, "invalid numeric argv"); // 不把部分解析或溢出当有效句柄。
    return (int)value; // 已确认可表示。
}
static int after_exec(char **argv) { // 同一个main的新映像分支，旧用户堆栈已不存在。
    int kept = number(argv[2]); // 普通FD应跨exec保留。
    int cloexec = number(argv[3]); // 此编号对应的旧描述符应被关闭。
    int previous_pid = number(argv[4]); // exec之前孩子记录的真实身份。
    expect(getpid() == previous_pid && initialized == 11 && zeroed == 0, "PID kept, initialized/BSS reset"); // 身份保持，映像内容替换。
    expect(getenv("AIRI_REPLACED") && !strcmp(getenv("AIRI_REPLACED"), "1") && !getenv("AIRI_PARENT_ONLY"), "explicit exec environment"); // envp采用新数组，不是自动保留旧环境。
    struct stat original; // 用文件身份而不是数字判断继承。
    if (fstat(kept, &original) != 0) // 保留描述符必须仍指向临时打开文件。
        fail("fstat inherited fd"); // 不把无效编号当作可用文件。
    errno = 0; // 紧邻检查EBADF。
    int flags = fcntl(cloexec, F_GETFD); // 动态运行时可能复用刚关闭的编号。
    if (flags < 0) // 最常见情况是该编号尚未复用。
        expect(errno == EBADF, "CLOEXEC descriptor closed"); // 区分其他错误。
    else { // 编号被重新使用时核对原文件对象已不在该编号。
        struct stat replacement; // 当前编号指向的新对象。
        if (fstat(cloexec, &replacement) != 0) // 读取新身份。
            fail("fstat reused fd"); // 保留具体错误。
        expect(replacement.st_dev != original.st_dev || replacement.st_ino != original.st_ino, "old CLOEXEC object not inherited"); // 原文件已unlink，不能被新映像按路径重开。
        puts("CLOEXEC fd number was reused; original object is absent"); // 编号复用不等于关闭失败。
    }
    char bytes[2]; // 只读取文件开头两字节。
    read_two(kept, bytes); // 新映像仍使用fork继承的打开文件描述。
    expect(!memcmp(bytes, "ab", 2), "child reads first two bytes"); // 读取后共享偏移应为2。
    printf("[LINUX_EXEC_CHILD] pid=%ld global=%d bss=%d inherited_fd=%d read=ab\n", (long)getpid(), initialized, zeroed, kept); // 真实新程序状态。
    return 23; // 父用Linux wait编码解码这个普通退出码。
}
int main(int argc, char **argv) { // 普通Linux C运行时进入，不使用AiriOS系统调用号。
#ifndef __linux__ // Mac只做语法检查，不冒充Linux运行证据。
    fputs("This experiment requires Linux fork/exec/proc semantics.\n", stderr); // 环境原因明确。
    return 77; // 未验证，不是成功。
#endif // Linux继续真实实验。
    if (argc == 5 && !strcmp(argv[1], "--child")) // exec后由新argv选择分支。
        return after_exec(argv); // 新main不是从旧fork的返回点进入。
    expect(argc == 1, "use without arguments"); // 未知参数不创建额外实验文件。
    char missing[] = "artifacts/missing-XXXXXX"; // 内核生成唯一名字，删除后才用作确定缺失目标。
    int temporary = mkstemp(missing); // 不猜测远端某个系统路径一定不存在。
    if (temporary < 0 || close(temporary) || unlink(missing)) // 只处理本实验刚创建的文件。
        fail("prepare missing executable"); // 失败时不执行未知路径。
    char broken[] = "artifacts/broken-XXXXXX"; // 受控损坏输入也使用唯一文件名。
    temporary = mkstemp(broken); // 独立于用户其他文件。
    if (temporary < 0) // 必须先拥有文件再写。
        fail("mkstemp broken"); // 明确错误。
    const unsigned char header[] = {0x7f, 'E', 'L', 'F', 2, 1, 1, 0}; // 不足完整ELF头的八字节。
    write_all(temporary, header, sizeof(header)); // 写出本次已知坏内容。
    if (fchmod(temporary, 0700) || close(temporary)) // 有执行权限，故障应是格式而不是EACCES。
        fail("finish broken executable"); // 保留设置或关闭错误。
    char path[] = "artifacts/descriptor-XXXXXX"; // 只观察打开文件，马上删除名字。
    int kept = mkstemp(path); // 保留打开文件描述的引用。
    if (kept < 0 || unlink(path)) // 原名字不再可被新映像重开。
        fail("prepare inherited file"); // 不依赖残留路径。
    write_all(kept, "abcdef", 6); // 准备可观察偏移的内容。
    if (lseek(kept, 0, SEEK_SET) != 0) // 子应从开头读ab。
        fail("rewind inherited file"); // 不伪造初始偏移。
    int cloexec = dup(kept); // 两个描述符引用同一个打开文件描述。
    if (cloexec < 0 || fcntl(cloexec, F_SETFD, FD_CLOEXEC) != 0) // 标志只设在这一个描述符项上。
        fail("set FD_CLOEXEC"); // 普通kept仍应继承。
    long page = sysconf(_SC_PAGESIZE); // 使用真实页大小。
    expect(page > 0, "page size"); // 不用失败值做映射长度。
    volatile unsigned char *memory = mmap(NULL, (size_t)page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); // 私有页在fork后由Linux按COW策略处理。
    if (memory == MAP_FAILED) // 失败值不是NULL。
        fail("mmap private page"); // 不触碰无效地址。
    memory[0] = 42; // fork前实际触碰页面。
    initialized = 77; // 旧父映像初值已被修改。
    zeroed = 55; // 旧父BSS也已写入。
    if (setenv("AIRI_PARENT_ONLY", "present", 1) != 0) // 新exec自定义envp会删掉它。
        fail("setenv"); // 环境操作失败不继续。
    fflush(NULL); // fork前不复制尚未输出的stdio缓冲。
    pid_t child = fork(); // 父正PID、子0、失败-1。
    if (child < 0) // 没有孩子不能wait。
        fail("fork"); // 保留错误。
    if (!child) { // 老孩子映像，仍从fork调用之后继续。
        struct rusage before, after; // 缺页计数反映进程实际事件，不等同某个映射独占页数。
        if (getrusage(RUSAGE_SELF, &before)) // 记录写入之前。
            fail("getrusage before"); // 不输出未初始化计数。
        memory[0] = 99; // Linux对私有页写入，不改变父的42。
        initialized = 88; // 新exec后应恢复11。
        zeroed = 66; // 新exec后应恢复0。
        if (getrusage(RUSAGE_SELF, &after)) // 写入之后实际计数。
            fail("getrusage after"); // 不编造COW数字。
        char kept_text[32], closed_text[32], pid_text[32]; // 字符串在旧栈上，只需活到execve复制参数。
        snprintf(kept_text, sizeof(kept_text), "%d", kept); // 普通FD编号。
        snprintf(closed_text, sizeof(closed_text), "%d", cloexec); // CLOEXEC项编号。
        snprintf(pid_text, sizeof(pid_text), "%ld", (long)getpid()); // exec前真实孩子PID。
        char *next_argv[] = {argv[0], "--child", kept_text, closed_text, pid_text, NULL}; // 内核复制字符串到新初始栈。
        char *next_env[] = {"AIRI_REPLACED=1", NULL}; // 明确的新环境，不自动沿用environ。
        pid_t identity = getpid(); // 两次失败与一次成功都应保持身份。
        execve(missing, next_argv, next_env); // 已删除的唯一名字必然不存在。
        expect(errno == ENOENT && initialized == 88 && memory[0] == 99 && getpid() == identity, "missing exec keeps old image"); // 失败回到老调用链。
        execve(broken, next_argv, next_env); // 有权限但ELF头截断。
        expect(errno == ENOEXEC && zeroed == 66 && memory[0] == 99 && getpid() == identity, "bad ELF exec keeps old image"); // 不能先丢弃旧映像。
        printf("[LINUX_BEFORE_EXEC] pid=%ld private=99 global=88 minor_fault_delta=%ld failures=ENOENT,ENOEXEC\n", (long)identity, after.ru_minflt - before.ru_minflt); // delta 还包含本进程其他路径的缺页，保留真实累计差值。
        fflush(NULL); // exec成功不会替旧stdio自动flush。
        execve("./process", next_argv, next_env); // 成功不返回，而是从同一PID的新运行时进入main。
        perror("execve self"); // 只有失败才到这里。
        _exit(111); // 不重复清理复制来的父stdio状态。
    }
    int status; // Linux编码后的退出状态，不等于AiriOS普通int协议。
    pid_t waited; // 保存实际wait返回身份。
    do { // 中断重试同一个孩子。
        waited = waitpid(child, &status, 0); // 等待exec后的同一PID结束。
    } while (waited < 0 && errno == EINTR); // 其他错误不重试。
    if (waited < 0) // 必须真正取得状态。
        fail("waitpid"); // 不读未初始化status。
    expect(waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 23, "same child PID exits23"); // 正常解码与信号终止区分。
    expect(memory[0] == 42 && initialized == 77 && zeroed == 55, "parent private state unchanged"); // 子COW与exec不能改父内存。
    char bytes[2]; // 继续读取同一个打开文件描述。
    read_two(kept, bytes); // 子已跨exec读ab，共享偏移现在是2。
    expect(!memcmp(bytes, "cd", 2), "open file description offset shared"); // 描述符表不同，打开文件描述仍共享。
    if (munmap((void *)memory, (size_t)page) || close(kept) || close(cloexec) || unlink(broken)) // 只回收本次实验拥有的资源。
        fail("cleanup"); // 不能把清理失败藏到成功标记之后。
    printf("[LINUX_P3_OK] parent=%ld child=%ld wait=23 private=42 shared_offset=cd\n", (long)getpid(), (long)child); // 整条实际路径结束才发布。
    return 0; // 普通Linux运行时结束父进程。
}
