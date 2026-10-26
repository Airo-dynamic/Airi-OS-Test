#define _GNU_SOURCE // Linux实验需要openat、pwritev和statx声明。
#include <stdio.h> // 记录实际结果，Mac分支也可报告环境限制。
#ifndef __linux__ // Mac不能把宿主I/O当Linux行为。
int main(void) { // 非Linux仅执行环境拒绝。
    fputs("Linux is required for this storage experiment.\n", stderr); // 明确不验证Linux源码路径。
    return 77; // 不是实验通过。
}
#else // 以下完整机制只在Linux编译和运行。
#include <errno.h> // 失败的系统调用返回-1并设置errno。
#include <fcntl.h> // FD、open标志、文件定位与预分配。
#include <signal.h> // 无SA_RESTART的真实read中断。
#include <stdint.h> // 对齐和长度字段。
#include <stdlib.h> // mkdtemp和posix_memalign。
#include <string.h> // 字节比较、iovec副本和错误说明。
#include <sys/mman.h> // 文件共享映射、驻留查询与同步。
#include <sys/stat.h> // 文件大小、分配块和statx对齐。
#include <sys/uio.h> // preadv/pwritev与iovec。
#include <unistd.h> // read/write、fsync、unlinkat和alarm。
static void fail(const char *operation) { // 进入时errno仍属于失败操作。
    perror(operation); // 保留具体系统错误。
    exit(1); // 非零状态交给运行器。
}
static void expect(int condition, const char *reason) { // 逻辑判断不借用可能陈旧errno。
    if (!condition) { // 实际状态不符才失败。
        fprintf(stderr, "expectation failed: %s\n", reason); // 给出当前事实。
        exit(1); // 不继续成功标记。
    }
}
static void write_all(int fd, const char *data, size_t bytes) { // 准备小文件也不假定一次write完整。
    while (bytes) { // 只处理剩余字节。
        ssize_t n = write(fd, data, bytes); // 实际返回进展数。
        if (n < 0 && errno == EINTR) // 中断不是整个请求已完成。
            continue; // 保留尚未消费范围。
        if (n < 0) // 真正错误保留errno。
            fail("write"); // 当前操作位置。
        expect(n > 0, "write made no progress"); // 零进展不无限重试。
        data += n; // 跳过实际完成字节。
        bytes -= (size_t)n; // 只扣已完成数。
    }
}
static void pwritev_all(int fd, const struct iovec *source, int count, off_t offset) { // 更新自己的iovec副本，不破坏调用者数组。
    struct iovec vectors[4]; // 本实验最多四段，容量有界。
    expect(count > 0 && count <= 4, "iovec count outside experiment budget"); // 不复制越界数组。
    memcpy(vectors, source, (size_t)count * sizeof(*source)); // iov_base借原字节，描述数组自有。
    int first = 0; // 当前尚未写完的段。
    while (first < count) { // 完成一段后才消费下一段。
        if (!vectors[first].iov_len) { // 零长度段没有工作。
            ++first; // 避免把它当成零进展错误。
            continue; // 继续剩余段。
        }
        ssize_t n = pwritev(fd, vectors + first, count - first, offset); // 同一次请求按数组顺序消费多段。
        if (n < 0 && errno == EINTR) // 失败中断时这次没有返回已完成前缀。
            continue; // 保留本轮iovec位置。
        if (n < 0) // 真正错误不能忽略。
            fail("pwritev"); // 退出前保存errno。
        expect(n > 0, "pwritev made no progress"); // 不在零进展时忙等。
        offset += n; // 显式文件偏移只加本次实际进展。
        size_t consumed = (size_t)n; // 在多个段之间分配完成字节。
        while (first < count && consumed >= vectors[first].iov_len) { // 已完成完整段，包含后续零长度段。
            consumed -= vectors[first].iov_len; // 从本轮完成数扣整段。
            ++first; // 进下一段。
        }
        if (first < count && consumed) { // 仅部分消费当前段。
            vectors[first].iov_base = (char *)vectors[first].iov_base + consumed; // 前移字节指针。
            vectors[first].iov_len -= consumed; // 余下长度不再包含已写字节。
        }
    }
}
static volatile sig_atomic_t interrupted; // 信号处理器只修改允许的简单标量。
static void alarm_handler(int signal_number) { // 异步处理器不做printf或分配。
    (void)signal_number; // 这里只处理SIGALRM。
    alarm(1); // 若信号早于read到达，再安排一次，避免一次性计时竞态导致永久阻塞。
    interrupted = 1; // 主流程随后检查实际到达。
}
static void observe_eintr(void) { // 管道保留写端但不写，read必然等待事件。
    int ends[2]; // 0读端、1写端。
    if (pipe(ends)) // 创建真正Linux管道。
        fail("pipe"); // 不能以模拟结果替代。
    struct sigaction action = {0}, old; // flags0避免SA_RESTART自动重启read。
    action.sa_handler = alarm_handler; // 处理器设置标量并用 alarm 重设时钟，不调用普通输出或分配。
    sigemptyset(&action.sa_mask); // 不额外屏蔽其他信号。
    if (sigaction(SIGALRM, &action, &old)) // 保存旧处理方式，实验后恢复。
        fail("sigaction"); // 权限或参数错误明确失败。
    alarm(1); // 约一秒后由内核交付SIGALRM。
    char byte; // read仅在得到数据时才写它。
    errno = 0; // 方便记录这次read的errno。
    ssize_t n = read(ends[0], &byte, 1); // 没数据且有写端，不应EOF。
    int error = errno; // 其他清理操作前保存本次错误。
    alarm(0); // 不让定时信号影响后续同步演示。
    if (sigaction(SIGALRM, &old, NULL)) // 恢复原信号处理。
        fail("restore sigaction"); // 清理也报告失败。
    close(ends[0]); // 不再借管道。
    close(ends[1]); // 最后一个端点回收。
    expect(n == -1 && error == EINTR && interrupted, "read was not interrupted by real signal"); // 真正观察EINTR，不靠手设errno。
    puts("[EINTR_OK] empty pipe read interrupted; no SA_RESTART"); // 与短成功返回区分。
}
static int direct_io(int directory) { // 可选能力必须按实际文件系统报告。
#ifndef STATX_DIOALIGN // 老头文件没有对齐查询，不能凭512或4096猜规则。
    (void)directory; // 不使用未知O_DIRECT约束。
    puts("[DIRECT_UNVERIFIED] headers lack STATX_DIOALIGN"); // 核心向量和同步仍可独立运行。
    return 0; // 返回未验证，脚本另记。
#else // 有明确查询接口才尝试直接I/O。
    int fd = openat(directory, "direct", O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC | O_DIRECT, 0600); // 只操作独立临时目录。
    if (fd < 0) { // 文件系统可能不支持O_DIRECT。
        if (errno != EINVAL && errno != EOPNOTSUPP) // 其他错误不是能力缺失。
            fail("open O_DIRECT"); // 如空间或权限错误明确失败。
        printf("[DIRECT_UNVERIFIED] open: %s\n", strerror(errno)); // 保存实际限制。
        return 0; // 不标直接I/O成功。
    }
    struct statx info = {0}; // 读取当前已打开文件的真实要求。
    int query = statx(fd, "", AT_EMPTY_PATH, STATX_DIOALIGN, &info); // 查询失败与字段未提供分别判断。
    if (query < 0 && errno != ENOSYS && errno != EINVAL && errno != EOPNOTSUPP) // 设备或权限错误不是能力缺失。
        fail("statx DIO alignment"); // 保存真正错误。
    if (query < 0 || !(info.stx_mask & STATX_DIOALIGN) || !info.stx_dio_mem_align || !info.stx_dio_offset_align) { // 支持查询不保证该文件提供字段。
        puts("[DIRECT_UNVERIFIED] file did not provide DIO alignment"); // 不回退为猜测值。
        close(fd); // 结束已打开的测试文件。
        return 0; // 清理由主流程unlinkat处理。
    }
    size_t alignment = info.stx_dio_mem_align, bytes = info.stx_dio_offset_align; // 地址和偏移/长度约束分别记录。
    if (alignment < sizeof(void *)) // posix_memalign另要求至少指针大小。
        alignment = sizeof(void *); // 更强地址对齐仍满足较弱要求。
    expect(!(alignment & (alignment - 1)) && alignment <= 1024 * 1024 && bytes <= 1024 * 1024, "DIO alignment outside experiment budget"); // 有界分配且满足posix_memalign的2幂规则。
    void *buffer = NULL; // 仅成功后可释放和使用。
    int allocation = posix_memalign(&buffer, alignment, bytes + 1); // 返回错误码本身，不自动设置errno。
    if (allocation) { // 不能拿旧errno报告分配失败。
        errno = allocation; // 给统一perror一个明确来源。
        fail("posix_memalign"); // 记录真正分配错误。
    }
    memset(buffer, 0x53, bytes + 1); // 触碰缓冲，建立真实内容。
    ssize_t n = pwrite(fd, buffer, bytes, 0); // 地址、长度和偏移均按查询要求。
    if (n < 0) // 即使对齐也可能有设备/空间错误。
        fail("aligned O_DIRECT pwrite"); // 错误不能被能力成功掩盖。
    expect((size_t)n == bytes, "aligned direct write was short"); // 此有界演示要求一完整单位，通用重试需维持对齐。
    n = pwrite(fd, (char *)buffer + 1, bytes, (off_t)bytes); // 故意错开地址，观察实际文件系统选择。
    int misaligned_error = n < 0 ? errno : 0; // 有些文件系统可能回退缓冲I/O，不硬造EINVAL。
    if (n < 0 && misaligned_error != EINVAL) // 非预期错误不能归因对齐。
        fail("misaligned O_DIRECT pwrite"); // 保存实际errno。
    if (fsync(fd)) // O_DIRECT本身不等于持久化同步。
        fail("direct fsync"); // 真正检查同步返回。
    printf("[DIRECT_OK] mem_align=%u offset_align=%u aligned_bytes=%zu misaligned_result=%ld errno=%d\n", info.stx_dio_mem_align, info.stx_dio_offset_align, bytes, (long)n, misaligned_error); // 记录环境差异，不宣称统一结果。
    free(buffer); // 同步请求全部结束后回收地址。
    close(fd); // 清FD。
    return 1; // 只有这条路径完成才称直接I/O已运行。
#endif // 结束头文件能力差异。
}
int main(void) { // Linux普通用户程序，文件系统和设备由Linux管理。
    char directory[] = "/tmp/airios-storage-XXXXXX"; // mkdtemp替换末尾六位，避免碰已有项目文件。
    if (!mkdtemp(directory)) // 创建私有临时目录。
        fail("mkdtemp"); // 不退回公共文件名。
    int root = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC); // 目录FD固定后续名字解析起点。
    if (root < 0) // 当前FS必须支持目录操作。
        fail("open experiment directory"); // 不使用无效dirfd。
    int fd = openat(root, "data", O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600); // 只创建本实验文件。
    if (fd < 0) // 不覆盖存在名字。
        fail("open data"); // 记录错误。
    expect(lseek(fd, 3, SEEK_SET) == 3, "set shared offset"); // 普通打开位置设3。
    struct iovec input[] = {{"abc", 3}, {"", 0}, {"defg", 4}}; // 三段合起来七字节，包含零长度段。
    pwritev_all(fd, input, 3, 0); // 显式偏移0，不移动FD共享位置。
    expect(lseek(fd, 0, SEEK_CUR) == 3, "pwritev moved shared offset"); // 对照K24定位语义。
    char first[2], second[8]; // 分散读取的两个不同用户数组。
    memset(second, 0x55, sizeof(second)); // 未被读到的尾部应保留。
    struct iovec output[] = {{first, 2}, {second, 8}}; // 请求10字节，但从偏移2只剩5。
    ssize_t n; // preadv也可能被信号中断。
    do { // 只重试返回-1且errno=EINTR的本次调用。
        n = preadv(fd, output, 2, 2); // 依数组顺序填第一段，再填第二段前缀。
    } while (n < 0 && errno == EINTR); // 正返回5不能当作10完成。
    if (n < 0) // 真正系统错误。
        fail("preadv"); // 保留errno。
    expect(n == 5 && !memcmp(first, "cd", 2) && !memcmp(second, "efg", 3) && second[3] == 0x55, "short vector read layout"); // 短读跨段消费位置可见。
    printf("[VECTOR_OK] requested=10 returned=%ld first=cd second_prefix=efg offset=3\n", (long)n); // 真实EOF短读，不伪造短写。
    int allocation = posix_fallocate(fd, 0, 2 * 1024 * 1024); // 返回错误码，与普通syscall的-1/errno不同。
    if (allocation) { // 只把明确不支持归入能力缺失，空间错误仍失败。
        if (allocation != EOPNOTSUPP && allocation != ENOSYS && allocation != EINVAL) { // ENOSPC等不能假装不支持。
            errno = allocation; // posix_fallocate返回错误码本身。
            fail("posix_fallocate"); // 保存真实空间或设备错误。
        }
        printf("[PREALLOC_UNVERIFIED] %s\n", strerror(allocation)); // 不把失败记成已预留空间。
    } else { // 成功时观察真实大小与分配块。
        struct stat info; // st_blocks以512字节单位统计，不是RSS。
        if (fstat(fd, &info)) // 不使用未初始化值。
            fail("fstat allocated file"); // 记录读取元数据失败。
        printf("[PREALLOC_OK] size=%lld allocated_512_blocks=%lld\n", (long long)info.st_size, (long long)info.st_blocks); // 逻辑长度与物理分配分开。
    }
    long page_size = sysconf(_SC_PAGESIZE); // 实际Linux页大小，不用固定4096推断驻留跨度。
    expect(page_size > 0, "page size unavailable"); // 避免无效mmap长度。
    char *mapped = mmap(NULL, (size_t)page_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0); // 文件第一页共享映射，不创建独立匿名副本。
    if (mapped == MAP_FAILED) // 映射失败不是NULL。
        fail("mmap shared file"); // 当前程序仍由Linux管理页表。
    mapped[0] = 'Z'; // CPU写映射页，尚未通过msync确认同步。
    char visible; // 普通pread观察相同文件的缓存内容。
    ssize_t visible_count = pread(fd, &visible, 1, 0); // 不移动原FD位置3。
    if (visible_count < 0) // 系统错误先保存errno。
        fail("pread shared mapping"); // 不比较未初始化字节。
    expect(visible_count == 1 && visible == 'Z', "shared cache visibility"); // 可见性成立不说明掉电持久性。
    unsigned char resident = 0; // mincore每页一字节，低位表示当前驻留。
    if (mincore(mapped, (size_t)page_size, &resident)) // 只观察这一个页，不把它当整个进程RSS。
        fail("mincore file"); // 不伪造驻留结果。
    if (msync(mapped, (size_t)page_size, MS_SYNC)) // 文件映射的显式同步请求。
        fail("msync file"); // 普通CPU写不能替代这次完成确认。
    printf("[PAGE_CACHE_OK] shared_write_visible=Z resident=%u msync=1 power_loss_tested=0\n", resident & 1u); // 驻留是本时刻快照，不是永久保证。
    if (munmap(mapped, (size_t)page_size)) // 结束映射借用不等于关闭FD。
        fail("munmap file"); // 清理也保留失败。
    if (fdatasync(fd)) // 同步数据及恢复它必要的元数据，例如长度。
        fail("data fdatasync"); // 不把close当同步替代。
    close(fd); // 不再借此FD。
    observe_eintr(); // 真信号中断，不和EOF短读混为一谈。
    int direct_verified = direct_io(root); // 可选直接I/O按环境能力记录。
    int old = openat(root, "current", O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600); // 旧名称与仍打开的旧对象。
    if (old < 0) // 独立临时目录内也检查创建错误。
        fail("open old"); // 不忽略失败。
    write_all(old, "old", 3); // 旧版本内容。
    if (fsync(old)) // 完整同步旧文件内容和元数据。
        fail("fsync old"); // 不预设已持久。
    int replacement = openat(root, "replace.tmp", O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600); // 新内容先写独立名字。
    if (replacement < 0) // 不覆盖别人文件。
        fail("open replacement"); // 明确错误。
    write_all(replacement, "new", 3); // 小写入同样按实际返回循环。
    if (fsync(replacement)) // 名字替换前先同步新内容。
        fail("fsync replacement"); // 失败不能发布新名字。
    if (renameat(root, "replace.tmp", root, "current")) // 同目录原子改变路径可见对象。
        fail("rename replacement"); // 不混同内容同步。
    if (fsync(root)) // 显式同步目录项变更。
        fail("fsync directory"); // 文件fsync不单独保证这一步。
    char old_bytes[3], new_bytes[3]; // 分别观察旧打开对象和新路径。
    int current = openat(root, "current", O_RDONLY | O_CLOEXEC); // 路径现在找到新文件。
    if (current < 0) // 名字替换后必须还能打开新版本。
        fail("open current"); // 保留open错误。
    ssize_t old_count = pread(old, old_bytes, 3, 0); // 这段观察没有并发截断或定时信号。
    if (old_count < 0) // 系统错误与短成功分开。
        fail("pread old"); // 不使用陈旧errno。
    ssize_t new_count = pread(current, new_bytes, 3, 0); // 不移动任何打开对象偏移。
    if (new_count < 0) // 真正系统错误。
        fail("pread current"); // 保存本次errno。
    expect(old_count == 3 && new_count == 3, "short version read"); // 此固定三字节样本不足时不比较未初始化尾。
    expect(!memcmp(old_bytes, "old", 3) && !memcmp(new_bytes, "new", 3), "rename object identity"); // 路径换了，旧FD不自动改指向。
    puts("[REPLACE_OK] open_old=old path_new=new file_sync=1 rename=1 directory_sync=1"); // 只验证操作顺序与当前可见结果，未拔电。
    close(current); // 新读者结束。
    close(old); // 旧inode最后引用可回收。
    close(replacement); // 新文件写FD结束。
    if (unlinkat(root, "data", 0) || unlinkat(root, "current", 0)) // 仅清本实验创建的名字。
        fail("cleanup data/current"); // 不隐瞒清理失败。
    if (unlinkat(root, "direct", 0) && errno != ENOENT) // 不支持O_DIRECT时可能没创建文件。
        fail("cleanup direct"); // 其他错误仍记录。
    close(root); // 移除临时目录前结束目录FD。
    if (rmdir(directory)) // 仅删除本轮独有目录。
        fail("rmdir experiment"); // 不留下误以为成功的脏结果。
    printf("[LINUX_P4_OK] direct_verified=%d power_loss_tested=0\n", direct_verified); // 环境能力与强制路径分开。
    return 0; // 真实Linux程序成功。
}
#endif // 非Linux分支只报告77。
