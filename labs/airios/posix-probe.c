#include <airi_libc.h> // 原始进程控制暂与POSIX风格文件接口共存，原始调用不设置errno。
#include <errno.h> // 错误变量是当前TLS的可修改左值。
#include <fcntl.h> // open源码标志由用户库转换。
#include <time.h> // 单调时间不是日历日期，超时以此比较。
#include <unistd.h> // 有符号字节数、显式偏移与标准文件调用名。
int main(int argc, char **argv) { // 同一ELF另以libc-reset观察exec后的错误初值。
    if (errno) // .tbss中的错误槽必须在main前初始化为0。
        return 227; // 不能继承上一个映像的错误值。
    if (argc == 1 && strlen(argv[0]) > 4 && argv[0][4] == '-') { // 孩子成功替换到libc-reset。
        char byte; // 测试成功exec关闭之前的CLOEXEC表项。
        if (read(STDIN_FILENO, &byte, 1) != -1 || errno != EBADF) // 默认错误为0已先核对，失败后才应变为9。
            return 228; // 本会话复用的FD0不得遗留。
        airi_puts("[LIBC_EXEC_RESET_OK] errno=zero cloexec=closed\n"); // 只在两项实际观察完成后报告。
        _exit(0); // 标准名字调用用户库，仍由原内核退出当前身份。
    }
    struct timespec before, after, guard = {71, 83}; // 真实时间与失败输出哨兵分别持有。
    if (clock_gettime(CLOCK_REALTIME, &guard) != -1 || errno != EINVAL || guard.tv_sec != 71 || guard.tv_nsec != 83 || clock_gettime(CLOCK_MONOTONIC, (void *)(uintptr_t)0xffff800000000000ull) != -1 || errno != EFAULT) // 未实现墙钟和坏输出都不能改有效对象。
        return 20; // 用户程序应继续而非因库解引用坏指针退出。
    if (clock_gettime(CLOCK_MONOTONIC, &before) || before.tv_sec < 0 || before.tv_nsec < 0 || before.tv_nsec >= 1000000000 || before.tv_nsec % 10000000 || airi_sleep(3) || clock_gettime(CLOCK_MONOTONIC, &after)) // 原始sleep真实阻塞至少三个PIT tick。
        return 21; // 不把宿主等待当作guest时钟推进。
    uint64_t elapsed = (uint64_t)(after.tv_sec - before.tv_sec) * 1000000000ull + (uint64_t)after.tv_nsec - (uint64_t)before.tv_nsec; // 无符号算术处理跨秒的纳秒回绕，当前观察差距有界。
    if (after.tv_sec < before.tv_sec || (after.tv_sec == before.tv_sec && after.tv_nsec < before.tv_nsec) || elapsed < 30000000ull) // 不能倒退且真实阻塞窗口应至少30ms名义时间。
        return 22; // 实际PIT分辨率不等于真实墙钟精度。
    airi_puts("[LIBC_CLOCK_OK] monotonic=1 quantum_ns=10000000 sleep=blocked invalid=unchanged\n"); // 当前只承诺名义100Hz单调时间。
    unsigned char input[300], output[300]; // 请求故意超过当前256字节系统调用副本。
    for (unsigned i = 0; i < sizeof(input); ++i) // 逐位置可辨识，不能只比较固定重复值。
        input[i] = (unsigned char)(i * 7u + 3u); // 字节内容独立于错误返回和偏移。
    int fd = open("/abi", O_RDWR | O_CREAT | O_TRUNC, (mode_t)0600); // 当前只使用mode的源码形状，RAM尚无权限模型。
    if (fd < 0) // 必须先看返回值，不能只看errno。
        return 229; // 明确资源或路径错误。
    errno = EBUSY; // 先留一项旧错误，后续成功不应该靠清零来表示成功。
    if (write(fd, input, sizeof(input)) != 256 || errno != EBUSY || write(fd, input + 256, 44) != 44) // 库只提交一次前缀，调用方明确消费剩余。
        return 230; // 不接受偷偷写满并伪装返回300。
    if (lseek(fd, 0, SEEK_SET) || read(fd, output, sizeof(output)) != 256 || read(fd, output + 256, 44) != 44 || memcmp(input, output, sizeof(input))) // 实际短读次数与内容都符合已写前缀。
        return 231; // 不以返回值正确代替数据正确。
    if (read(fd, output, 1) != 0 || errno != EBUSY) // EOF是成功0，旧errno仍无本次错误含义。
        return 232; // 不能把旧EBUSY误判为EOF失败。
    if (lseek(fd, 0, SEEK_SET) || pread(fd, output, 5, 10) != 5 || memcmp(output, input + 10, 5) || read(fd, output, 1) != 1 || output[0] != input[0]) // pread不移动共享位置。
        return 233; // 后一次read应仍从offset0开始。
    if (pwrite(fd, "XYZ", 3, 5) != 3 || read(fd, output, 1) != 1 || output[0] != input[1] || pread(fd, output, 3, 5) != 3 || memcmp(output, "XYZ", 3)) // pwrite同样保持位置且内容真实可见。
        return 234; // 显式偏移不是先seek再write的别名。
    if (read(fd, (void *)(uintptr_t)0xffff800000000000ull, 1) != -1 || errno != EFAULT || read(fd, output, 1) != 1 || output[0] != input[2] || errno != EFAULT) // 坏输出在消费偏移前拒绝，后一次成功保留旧错误。
        return 235; // 不让包装破坏原内核错误原子性。
    if (pread(fd, output, 1, -1) != -1 || errno != EINVAL || lseek(fd, 0, SEEK_CUR) != 3 || lseek(fd, -1, SEEK_CUR) != 2) // 显式读偏移不能为负，当前位置的合法负差值却应成功。
        return 236; // 参数的意义不同，不能统一拒绝所有负数。
    if (lseek(fd, -1, SEEK_END) != 299 || read(fd, output, 1) != 1 || output[0] != input[299] || lseek(fd, 10, SEEK_END) != 310 || read(fd, output, 1) != 0 || lseek(fd, 0, SEEK_END) != 300) // 文件尾、定位后EOF与不扩文件分别观察。
        return 252; // 容量1024不能当作当前内容尾。
    if (lseek(fd, 302, SEEK_SET) != 302 || write(fd, "Q", 1) != 1 || pread(fd, output, 3, 300) != 3 || output[0] || output[1] || output[2] != 'Q') // 真正写入才扩size并清零两字节洞。
        return 253; // 不允许暴露原来未写的旧内容。
    if (lseek(fd, -304, SEEK_END) != -1 || errno != EINVAL || lseek(fd, INT64_MIN, SEEK_CUR) != -1 || errno != EINVAL || lseek(fd, INT64_MAX, SEEK_CUR) != -1 || errno != EINVAL || lseek(fd, 0, 99) != -1 || errno != EINVAL || lseek(fd, 0, SEEK_CUR) != 303) // 最小/最大差值与非法基准不能修改旧位置。
        return 254; // 不在有符号加法或对最小整数取负中产生UB。
    airi_puts("[LIBC_SEEK_OK] set=1 cur=1 end=1 signed=checked hole=zero failure=unchanged\n"); // 新语义由guest实际核对。
    if (close(fd) || close(fd) != -1 || errno != EBADF || open("/missing", O_RDONLY) != -1 || errno != ENOENT || open("/seed", O_ACCMODE) != -1 || errno != EINVAL) // 真实内核错误和库参数错误都转换为-1+正errno。
        return 237; // 成功结果不能被旧errno遮蔽。
    fd = open("/seed", O_RDONLY); // 独立open从offset0开始。
    if (fd < 0) // 先检查返回值，不读旧errno猜结果。
        return 249; // 保留具体失败位置。
    int alias = dup(fd); // 新表项共享打开对象及偏移。
    if (alias < 0 || read(fd, output, 1) != 1 || output[0] != 'a' || read(alias, output, 1) != 1 || output[0] != 'b' || close(fd) || read(alias, output, 1) != 1 || output[0] != 'c' || close(alias)) // 关原FD不能破坏别名持有的对象。
        return 250; // 标准包装不应复制偏移或重复关闭对象。
    if (open("/seed", O_WRONLY) != -1 || errno != EACCES) // 只读初始节点仍由内核拒绝写模式。
        return 251; // 包装不能因标志映射错误而提升权限。
    int ends[2]; // 同一用户地址直接给内核验证，库不借局部输出副本。
    if (pipe(ends) || write(ends[1], input, 100) != 64 || read(ends[0], output, 100) != 64 || memcmp(input, output, 64)) // 当前管道只有64字节，短操作要保持。
        return 238; // 不把短流操作自动扩成循环等待。
    if (close(ends[1]) || read(ends[0], output, 1) != 0 || close(ends[0])) // 最后写端关闭后才是EOF。
        return 239; // 原FD引用和流结束语义仍在内核。
    unsigned raw[2]; // 非阻塞模式尚无标准fcntl，明确借原始实验接口建立。
    if (airi_pipe(raw, 1) || read((int)raw[0], output, 1) != -1 || errno != EAGAIN || close((int)raw[0]) || close((int)raw[1])) // 暂无字节是错误-1/EAGAIN，不是成功EOF0。
        return 240; // 不混淆两个结果。
    if (pipe((int *)(uintptr_t)0xffff800000000000ull) != -1 || errno != EFAULT) // 坏输出必须在创建FD前拒绝。
        return 241; // 不能在用户库里先创建局部管道再因写坏指针而崩溃。
    airi_puts("[LIBC_IO_OK] short=256 pipe=64 eof=zero offset=preserved bad_buffer=unchanged\n"); // 有界文件与错误路径都已实际运行。
    errno = EBUSY; // fork应复制当前错误值，并在此后各自改变。
    long child = airi_fork(); // 原始进程接口仍保留负错误，不自动改errno。
    if (child < 0 || errno != EBUSY) // 父子都须先继承16。
        return 242; // 不重新从模板初始化fork。
    if (child) { // 父制造一个名称错误。
        if (open("/missing", O_RDONLY) != -1 || errno != ENOENT) // 当前本地槽改为2。
            return 243; // 另一进程错误不能混入。
    } else if (read(-1, output, 1) != -1 || errno != EBADF) // 子将同一符号改为9。
        return 244; // 完整有符号FD转换仍应得到正确错误。
    int expected = child ? ENOENT : EBADF; // 两个真实身份的不同值。
    uint64_t start = airi_ticks(); // 原始查询不会改errno。
    do { // 普通用户纯计算，让PIT实际切换。
        for (unsigned i = 0; i < 1000000; ++i) // errno定位函数在独立对象，不由本main内联成常量。
            if (errno != expected) // 真实本地错误槽不能串到另一个进程。
                return 245; // 任一次错误即结束。
    } while (airi_ticks() - start < 20); // 二十个真实tick，非宿主注入。
    if (!child) { // shell的输入已在进入libc时CLOEXEC，当前最低空FD是0。
        if (open("/seed", O_RDONLY | O_CLOEXEC) != STDIN_FILENO || errno != EBADF) // 成功open也不会清旧错误。
            return 246; // 后续reset可以准确观察此表项。
        airi_exec("libc-reset"); // 错误槽来自新ELF模板0，PID仍不变。
        return 247; // 成功不返回。
    }
    int status = 1; // 父按实际孩子身份取得新程序结果。
    if (airi_waitpid(child, &status) != child || status || errno != ENOENT || getpid() != airi_getpid() || errno != ENOENT) // 原始wait与成功标准getpid均不清父错误。
        return 248; // 控制路径不能覆盖本地错误环境。
    airi_puts("[LIBC_ERRNO_OK] initial=0 fork=copied preempt=private success=preserved exec=reset\n"); // 全路径结束后才发布。
    return 0; // shell持续服务，会话最终核对页/FD/堆恢复。
}
