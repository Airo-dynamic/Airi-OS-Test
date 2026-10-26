# L03 · 同一个 PID 换了哪些状态，哪些 FD 仍然存在

从K23和L02继续。AiriOS已经能复制用户页、等待孩子并替换ELF；Linux里的同名操作还带着更多状态。我们沿同一组可观察结果对照：孩子改内存，父为何仍读 42；孩子更换程序，父为何仍等待同一 PID；孩子在新程序里读文件，父为何跳过已读字节。本节用一个父进程创建孩子，孩子先让两次execve失败，再把自己换成同一可执行文件的另一条入口分支。父亲最后检查内存、退出状态和文件偏移。实验文件都由 mkstemp 创建。用于 FD 继承的文件立即 unlink，缺失目标也先删除；坏 ELF 则保留到执行尝试完成再清理。不会使用项目中的数据文件。

<a id="linux-fork-memory"></a>
## fork 后先共享物理页，修改时才复制

先看匿名页中存着 42 的那个地址。fork 给孩子相同的地址数值，不代表以后所有写入都要共同可见。普通 C 指针保存在进程虚拟空间里。Linux fork让孩子得到相同的虚拟地址和初值，父子随后对私有匿名映射的写入互不覆盖。常见实现先让两者读同一物理页并撤掉可写权限；有人写时，内核处理缺页并建立私有副本，这就是写时复制。AiriOS目前在fork内直接分配并复制所有页，因此不需要这一步权限故障。

程序给匿名页写42，孩子改99，父亲等孩子退出后仍读到42；这验证私有语义。getrusage中的ru_minflt是进程累计的次缺页数，包含其他运行路径，差值只能作观察，不能据此精确数出这一页复制了几次。代码也不把差值固定成某个数来验收。

<a id="linux-exec-identity"></a>
## exec 替换映像，失败仍回原调用点

fork返回两次：父进程拿到孩子的PID，孩子拿到0。execve成功没有通常的返回值，内核保留孩子的PID，换掉用户映像和初始栈；新的运行时再次进入main。我们的程序通过实际参数 --child 识别这次入口，并验证初始化全局变量重新为11、BSS重新为0，而不是孩子此前改过的88和66。

argv是以空指针结束的参数指针数组，envp是以空指针结束的环境字符串数组。孩子显式给新程序一份只含AIRI_REPLACED的环境；它不会自动把旧环境的所有名字带过去。实际程序还会有动态装载器与ELF辅助向量，AiriOS本轮仅实现自己的静态子集。

两次失败分别使用已删除路径和八字节假ELF。execve返回−1并设置errno为ENOENT或ENOEXEC，旧PID、代码、全局变量和匿名页继续可用。这里只验证这两种早期失败；Linux极少数资源失败可能发生在旧映像已经拆掉之后，此时可能以SIGSEGV结束，不能把本项目的预先准备策略推广成所有Linux失败都能回旧程序。

<a id="linux-fd-inheritance"></a>
## FD 是表里的编号，偏移属于打开文件描述

内存经过 exec 重新建立，文件读取却应接着孩子先前的位置继续，这需要看清另一种所有权。打开临时文件得到一个 FD，再 dup 得到另一个编号。这两个表项指向同一个打开文件描述，其中持有当前偏移和状态。fork复制FD表后，父子仍引用这个打开文件描述；孩子在exec后读ab，父亲随后读到cd，说明偏移随着孩子的读取一起前移。路径名已被unlink，不影响仍打开的文件对象存活。

两个编号共享偏移，但并不共享“exec 时是否关闭”这项选择。FD_CLOEXEC 属于某个 FD 表项。对duplicate设置它，成功exec就关闭该编号，普通编号仍在。动态装载器可能复用刚关闭的编号，所以仅看到fstat成功还不能说旧文件继承了；程序同时比较设备号与inode，确认那个编号已不再指向原来的已删除临时文件。孩子退出最终释放它余下的表项。

waitpid先等指定孩子状态变化，返回的是孩子PID；status是编码后的状态，不是可以直接当23使用的整数。先用WIFEXITED判断正常退出，再用WEXITSTATUS取出23。AiriOS当前直接传普通退出码或自有负故障编码，两者ABI不同。

**新建 `labs/linux/p3` 并敲入下面程序。** 宏、系统调用声明、错误出口和清理均包含在这里；本机Mac只能编译其平台检查分支，真实机制需要Linux运行。

父子、文件和替换后的入口放在一份可执行文件中，数据跨越边界的位置可连续观察。

<!-- write: labs/linux/p3/process.c -->
```c
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
```

这些助手只处理短传输、错误与数字参数。接着写 after_exec：它是新运行时进入 main 后选择的分支，旧孩子的局部变量已不存在，保留下来的 FD 编号和旧 PID 必须从 argv 重新取出。

<!-- append: labs/linux/p3/process.c -->
```c
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
```

新映像的检查已经写好，原入口再负责准备它要继承的对象。继续写 main 的前半段：创建受控缺失/坏 ELF、匿名页与临时文件，设置两个 FD 的标志。此时还没有孩子。

<!-- append: labs/linux/p3/process.c -->
```c
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
```

到 fork 才出现父子两条调用续点。下面一块让孩子先修改自己的内存，观察两次早期 exec 失败，再带显式 argv/envp 执行同一文件；成功以后走的是上方 after_exec，不能越过 exec 继续旧分支。

<!-- append: labs/linux/p3/process.c -->
```c
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
```

孩子路径已经给出，父路径接着 waitpid 并解码状态。父亲验证自己的 42 和打开对象的新偏移，收束文件、映射与坏材料的寿命；下面闭合整个程序。

<!-- append: labs/linux/p3/process.c -->
```c
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
```

<a id="linux-exec-gdb"></a>
## 在成功替换那一刻看新入口

普通日志已经能检查身份和数据结果，调试运行再观察替换发生的时刻。它是一份新的独立进程，不能拿它的 PID 去拼接前一次正常运行。run.sh 先正常运行并检查完整父子结果，再启动一份独立的GDB观察运行。set follow-fork-mode child让调试器跟随孩子；catch exec只在真正成功的替换事件停住，两次失败不会生成这条事件。commands到end之间是刚建立捕捉点的动作，只有命中之后才打印GDB_EXEC_STOP。仅创建catchpoint不算观察成功。

动态链接程序的exec停点可能在动态装载器，RDI此时不能直接叫argc。先观察RIP落在哪个映射，再读RSP指向的初始栈，栈首才是参数个数，后面接参数指针。这里的寄存器和x/8gx语法承接L01：从RSP开始，以十六进制读八个八字节单位。

Shell的printf以%s原样写出每条字符串，再以\n换行。命令字符串放在单引号里，$rsp保持为GDB寄存器文本，不会变成Shell变量；重定向>>按顺序追加到同一个观察文件。生成的观察脚本属于本节明确写出的配方；不需要另外下载作者脚本。

**新建 `labs/linux/p3/run.sh`。** 程序和真实捕捉动作都成功后才写passed=true。

<!-- write: labs/linux/p3/run.sh -->
```sh
#!/bin/sh
set -eu # 命令失败或未定义变量都停止。
if [ "$(uname -s)" != Linux ] || [ "$(uname -m)" != x86_64 ]; then # 本节GDB寄存器步骤固定x86-64 Linux。
    echo 'x86-64 Linux is required for this process/GDB experiment.' >&2 # 其他环境不假装通过。
    exit 77 # 明确未验证。
fi # 完成环境检查。
mkdir -p artifacts # 仅本实验输出目录。
printf '{"passed":false,"phase":"p3"}\n' > artifacts/result.json # 先抹掉旧成功身份。
uname -a > artifacts/environment.txt # 实际内核与架构。
cc --version >> artifacts/environment.txt # 实际编译器。
gdb --version >> artifacts/environment.txt # 实际调试器版本。
cc -std=c11 -O0 -g -Wall -Wextra -Werror process.c -o process # 普通Linux程序链接Linux运行时。
readelf -h -l process > artifacts/elf.txt # 保存Linux程序真正的入口和装载段。
if ./process > artifacts/process.log 2>&1; then # 不经pipeline丢失程序退出码。
    cat artifacts/process.log # 回显已完成的真实输出。
else # 失败也保留日志。
    status=$? # 立即保存失败码。
    cat artifacts/process.log >&2 # 给出具体失败位置。
    exit "$status" # 不写成功。
fi # 正常运行已验证父子与FD路径。
: > artifacts/observe.gdb # 冒号是无操作命令，重定向先清空旧GDB脚本。
printf '%s\n' 'set pagination off' >> artifacts/observe.gdb # 输出不分页，批量模式不等待按键。
printf '%s\n' 'set follow-fork-mode child' >> artifacts/observe.gdb # 跟随孩子，父结果由独立正常运行验证。
printf '%s\n' 'catch exec' >> artifacts/observe.gdb # 只在真正成功exec后停。
printf '%s\n' 'commands' >> artifacts/observe.gdb # 开始为刚建立的捕捉点定义动作。
printf '%s\n' 'silent' >> artifacts/observe.gdb # 省去重复停止提示。
printf '%s\n' 'info inferiors' >> artifacts/observe.gdb # 观察被跟随孩子的实际PID。
printf '%s\n' 'info registers rip rsp rdi rsi' >> artifacts/observe.gdb # 停点可能在动态装载器，RDI不能直接当argc。
printf '%s\n' 'x/8gx $rsp' >> artifacts/observe.gdb # 读初始栈的argc及后续参数指针。
printf '%s\n' 'info proc mappings' >> artifacts/observe.gdb # 保存新程序与解释器映射。
printf '%s\n' 'printf "[GDB_EXEC_STOP]\n"' >> artifacts/observe.gdb # 只有真正命中并完成观察才发布标记。
printf '%s\n' 'continue' >> artifacts/observe.gdb # 恢复孩子执行直到退出。
printf '%s\n' 'end' >> artifacts/observe.gdb # 结束捕捉动作定义。
printf '%s\n' 'run' >> artifacts/observe.gdb # 启动这份独立调试运行。
# 固定GDB输出语言；目标exec的envp仍由C程序自己提供。
LC_ALL=C gdb -q -batch -x artifacts/observe.gdb ./process > artifacts/gdb.log 2>&1 # 批量执行明确写出的观察脚本。
# 该精确单行只能来自真正命中exec后的动作，不接受只有catchpoint设置成功。
grep -Fxq '[GDB_EXEC_STOP]' artifacts/gdb.log # -F字面匹配，-x整行，-q只交退出码。
printf '{"passed":true,"phase":"p3"}\n' > artifacts/result.json # 只有正常程序和调试命令都完成才发布。
```

**按旧/新窗口更新 `scripts/linux_lab.py`。** p3选择process.c与run.sh，远端独立目录和本地证据目录沿同一阶段名。

把 p3 加入参数合法集合。

<!-- replace: scripts/linux_lab.py -->
<!-- before -->
```python

parser=argparse.ArgumentParser()  # 主机参数解析器提供--host选项与用法提示。
parser.add_argument('--host',default='airi@ssh.airi-dynamic.top')  # 个人Linux主机可以显式替换，不涉及公司机器。
parser.add_argument('--phase', choices=('p1', 'p2'), default='p1') # 实验来源与远端目录使用同一阶段名。
args=parser.parse_args()  # 把进程参数转为Namespace，后续通过 args.host 取得目标主机字符串。
if args.host.startswith('-') or any(c.isspace() for c in args.host):  # 禁止选项形状和空白，避免host被外部工具误解析；生成器逐字符检查空白。
    raise RuntimeError('Invalid SSH host')  # 无效目标在启动SSH之前拒绝。
```
<!-- after -->
```python

parser=argparse.ArgumentParser()  # 主机参数解析器提供--host选项与用法提示。
parser.add_argument('--host',default='airi@ssh.airi-dynamic.top')  # 个人Linux主机可以显式替换，不涉及公司机器。
parser.add_argument('--phase', choices=('p1', 'p2', 'p3'), default='p1') # 实验来源与远端目录使用同一阶段名。
args=parser.parse_args()  # 把进程参数转为Namespace，后续通过 args.host 取得目标主机字符串。
if args.host.startswith('-') or any(c.isspace() for c in args.host):  # 禁止选项形状和空白，避免host被外部工具误解析；生成器逐字符检查空白。
    raise RuntimeError('Invalid SSH host')  # 无效目标在启动SSH之前拒绝。
```

将 p3 对应到 process.c 与 run.sh，其余阶段仍选择原文件。

<!-- replace: scripts/linux_lab.py -->
<!-- before -->
```python
if connection.returncode: # 未连接就不复制也不声称程序运行。
    (dest/'remote.json').write_text(json.dumps({'host':args.host,'directory':remote,'verified':False,'status':'connection failed','exit':connection.returncode},indent=2)+'\n') # 明确本轮未验证。
    raise RuntimeError(f'Linux SSH failed ({connection.returncode}); see {dest}/connection.log') # 路径和失败状态交回统一入口。  # 本地参数列表启动SSH，远端命令文本中的目录由quote保护。
names = ('ordinary.c','raw_start.S','run.sh') if args.phase == 'p1' else ('memory.c','run.sh') # 只选择本阶段已写出的程序。
sources=[ROOT/'labs/linux'/args.phase/name for name in names]  # 列表推导按所选阶段名称构成Path对象。
subprocess.run(['scp','-q','-o','BatchMode=yes','-o','ConnectTimeout=10',  # scp安静且非交互传输，连接上限同SSH。
                *[str(p) for p in sources],f'{args.host}:{remote}/'],check=True)  # 列表内*展开本阶段路径字符串，最后一项是远端目标。
```
<!-- after -->
```python
if connection.returncode: # 未连接就不复制也不声称程序运行。
    (dest/'remote.json').write_text(json.dumps({'host':args.host,'directory':remote,'verified':False,'status':'connection failed','exit':connection.returncode},indent=2)+'\n') # 明确本轮未验证。
    raise RuntimeError(f'Linux SSH failed ({connection.returncode}); see {dest}/connection.log') # 路径和失败状态交回统一入口。  # 本地参数列表启动SSH，远端命令文本中的目录由quote保护。
names = ('ordinary.c','raw_start.S','run.sh') if args.phase == 'p1' else ('memory.c','run.sh') if args.phase == 'p2' else ('process.c','run.sh') # 只选择本阶段已写出的程序。
sources=[ROOT/'labs/linux'/args.phase/name for name in names]  # 列表推导按所选阶段名称构成Path对象。
subprocess.run(['scp','-q','-o','BatchMode=yes','-o','ConnectTimeout=10',  # scp安静且非交互传输，连接上限同SSH。
                *[str(p) for p in sources],f'{args.host}:{remote}/'],check=True)  # 列表内*展开本阶段路径字符串，最后一项是远端目标。
```

**按旧/新窗口更新 `scripts/manage.py`。** 只扩充linux-lab的phase选项，其余命令行为保持。

统一入口接受 p3，再交给同一个执行器。

<!-- replace: scripts/manage.py -->
<!-- before -->
```python
    parser.add_argument('--app',choices=('terminal','files','editor','tasks','system','settings','viewer'),default='editor')  # 主机窗口默认Editor，只接受已知应用名。
    parser.add_argument('--state',choices=['normal','empty','loading','error','offline'],default='normal')  # 状态名限定到共享视图已有的五种状态。
    parser.add_argument('--profile',choices=PROFILES,default='normal')  # 镜像默认普通配置，只接受固定六项。
    parser.add_argument('--phase',choices=('p1','p2'),default='p1') # Linux阶段显式选择，默认保持P1。
    parser.add_argument('--host',default='airi@ssh.airi-dynamic.top')   # 个人Linux主机可以显式替换，不涉及公司机器。
    parser.add_argument('--graphics', action='store_true', help='explicitly enable the optional desktop')  # 默认 False，不读取图形目录。
    args=parser.parse_args()      # 把进程参数转为Namespace，后续通过args.action等属性读取。
```
<!-- after -->
```python
    parser.add_argument('--app',choices=('terminal','files','editor','tasks','system','settings','viewer'),default='editor')  # 主机窗口默认Editor，只接受已知应用名。
    parser.add_argument('--state',choices=['normal','empty','loading','error','offline'],default='normal')  # 状态名限定到共享视图已有的五种状态。
    parser.add_argument('--profile',choices=PROFILES,default='normal')  # 镜像默认普通配置，只接受固定六项。
    parser.add_argument('--phase',choices=('p1','p2','p3'),default='p1') # Linux阶段显式选择，默认保持P1。
    parser.add_argument('--host',default='airi@ssh.airi-dynamic.top')   # 个人Linux主机可以显式替换，不涉及公司机器。
    parser.add_argument('--graphics', action='store_true', help='explicitly enable the optional desktop')  # 默认 False，不读取图形目录。
    args=parser.parse_args()      # 把进程参数转为Namespace，后续通过args.action等属性读取。
```

<a id="linux-p3-evidence"></a>
## 执行路径与已有证据边界

```sh
./airios linux-lab --phase p3 # 向个人主机独立p3目录运行并取回实际证据。
```

执行器先写verified=false，再尝试个人主机airi@ssh.airi-dynamic.top。成功运行应在process.log中出现LINUX_P3_OK，在gdb.log中出现精确的[GDB_EXEC_STOP]，最后remote.json才标verified=true。2026-10-15 的既有连接实际返回255，日志为Connection closed by UNKNOWN port 65535；程序、GDB 和 Linux 输出都未验证。完整源码与操作已提供，不能把下面的预期误当成作者实测。

连接可用时，正常结果应表明：父映射保持42；孩子PID跨exec不变；新数据/BSS分别11和0；普通FD继承、CLOEXEC表项不再指向旧文件；孩子退出23；父随后读cd。这些是检查条件，不预设PID或缺页次数。

BOSS与Taishan的工作请求还会涉及FD关闭、短读写、向量缓冲区和持久性；下一存储阶段把FD对象与偏移纳入AiriOS，再用Linux的pread/pwritev和同步顺序对照。此处没有验证掉电恢复或多线程fork。

机制依据为[Linux fork](https://man7.org/linux/man-pages/man2/fork.2.html)、[execve](https://man7.org/linux/man-pages/man2/execve.2.html)、[waitpid](https://man7.org/linux/man-pages/man2/waitpid.2.html)和[GDB捕捉点](https://sourceware.org/gdb/current/onlinedocs/gdb.html/Set-Catchpoints.html)。
