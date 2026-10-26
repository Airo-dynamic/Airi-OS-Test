# L04 · 分散缓冲区、文件缓存与替换名字的同步边界

从K27和L03继续。AiriOS已能从用户程序提交记录并跨QEMU恢复；BOSS与Taishan实际使用的却是Linux文件接口。本节在独立临时目录里执行一次向量写入和EOF短读，观察共享文件映射，尝试预分配与直接I/O，再按文件同步、名称替换、目录同步的顺序发布新版本。所有源码和失败出口给在正文，个人主机不可连接时不拿Mac或guest结果冒充Linux。

<a id="linux-vector-io"></a>
## 返回五字节，具体填到了哪一段

应用的头和内容可能已经分开存放，重新拼成一个连续数组会多一次复制。向量 I/O 让一次调用描述多块现成数据，但仍只返回一个总完成数；因此先要理解这一个数怎样落回各段。iovec 数组每项放地址和长度；地址指向各自缓冲区，数组本身不保存内容。输入三段abc、空段、defg，用pwritev从偏移0写七字节。程序先把打开对象普通偏移设成3，写后仍应为3；显式偏移操作不会移动共享位置，和K24的pread/pwrite相同。

随后用两段输出数组分别接2和8字节，从文件偏移2读；文件只剩cdefg五字节。preadv返回5，先填第一段cd，再填第二段前三字节efg，第二段后五字节不动。返回值描述整个向量实际完成数量，不能认为每段都填满，也不能把同一个总数加到每段指针。

短读已经表明“请求总长”不等于“完成总长”。写入循环也必须从完成位置继续，否则重交原向量会重复前缀。pwritev_all 先复制 iovec 描述数组，仍借调用者原数据。正返回n后，先越过完整消费的段，再前移当前部分段的iov_base并缩短iov_len，同时显式offset只加n。零长度段直接跳过；−1且errno=EINTR才重试未完成范围，正短返回则必须先消费实际前缀。本轮EOF短读是真正的短成功，七字节普通文件写入不宣称一定触发短写。

<a id="linux-cache-and-space"></a>
## 页缓存、空间预留和直接I/O分别改变什么

字节已经经文件接口交给内核，接下来仍要区分谁看得见、内存是否驻留、是否请求了同步。共享文件映射 MAP_SHARED 让 CPU 写入文件缓存页。程序写首字节Z后立即用普通pread读到Z，说明当前可见内容一致；此时还没由这个观察确认同步。mincore只返回当前这一页的驻留快照，既不是进程总RSS，也不表示掉电后可恢复。接着msync(MS_SYNC)请求同步映射修改，munmap结束映射，FD仍可用于fdatasync。可见性、驻留和同步是三个不同观察。

另一种“分配”发生在文件存储范围，不能拿刚才的驻留页模型解释。posix_fallocate 请求为指定文件范围预留空间，成功后记录st_size和以512字节计的st_blocks；它不等于给进程分配2MiB RSS。这个函数返回错误码本身，不能直接用旧errno报告。明确不支持的结果单独记录；ENOSPC等真正失败必须结束实验，不能用“不支持”掩盖。

若应用选择直接 I/O，缓冲地址、长度和文件偏移还要满足后端约束；这不是把普通 write 换一个标志就必然成立。O_DIRECT 的约束来自实际文件系统，不能统一猜成512或4096。先打开独立direct文件，用statx的STATX_DIOALIGN查询内存和偏移对齐；头文件、内核或文件系统未提供字段时标DIRECT_UNVERIFIED。对齐值有效后再posix_memalign，内存分配至少多留一字节，确保后面的错开地址请求仍在已分配范围内。

对齐写要完整写出一个要求的单位，再把地址错开一字节观察返回值。若地址对齐要求为1，错开并不违反要求；有些文件系统也可能对不合要求的请求回退缓冲路径。因此记录实际返回和errno，不硬造统一EINVAL。O_DIRECT不替代fsync，也不能仅凭一次成功断言所有缓存完全绕过。

<a id="linux-eintr"></a>
## 真正中断一次正在等待的读取

前面的 EOF 返回正的短前缀或零，这里再区分一次尚未得到数据的等待被信号中断。Linux 管道保留写端且没有数据时，读端会等待；关闭所有写端才会读到EOF。本节保留两个端点，给SIGALRM安装flags为0的处理器，避免SA_RESTART自动重启read。约一秒后信号让read返回−1/EINTR。处理器只设置sig_atomic_t标量并用允许在信号上下文调用的alarm再安排一次；这样即使第一次信号早于read到达，也不会永远等一个已经过去的单次事件。主流程读返回后取消alarm并恢复原处理方式。

EINTR与EOF短读不同：这里返回−1并没有正的完成前缀。处理器不能随意printf、分配对象或调用普通锁；本节不实现完整信号框架，后续等待实验再展开并发交接。

<a id="linux-replace-durability"></a>
## 名字换成new，旧FD为什么还能读old

最后把内容与名字接起来：发布新版本希望后来打开 current 的读者得到 new，同时已有旧 FD 仍指向原对象。这要求先准备好新内容，再改变名字对应，而不是原地覆盖 old。先创建 current 写 old 并同步，再创建replace.tmp写new。发布新版本的顺序是：写完新内容，fsync新文件，renameat将临时名字替换成current，再fsync目录FD。rename负责同目录路径可见性的切换，文件同步负责内容与相应元数据，目录同步负责名称变更；它们不能互相替代，C++流flush也不能自动代替这两次fsync。

rename后重新open current得到新对象，旧FD仍引用原对象，因此两者分别读new和old。这个实验验证实际调用顺序和当前对象身份，没有拉掉主机电源。文件系统不支持目录同步或同步失败时会如实报错，不能用原子替换可见性推断所有掉电保证。对应 FileStorage::atomicWrite 的持久性问题，重点是找出旧注释把哪两个时刻合在了一起；本节不修改其他项目源码。

**新建 `labs/linux/p4`。** Linux完整实现位于平台条件的Linux分支；Mac只编译并运行拒绝分支返回77，不能据此声称Linux分支已经编译。

**新建 `labs/linux/p4/io.c`。** 核心向量、缓存、信号和同步路径必须通过；直接I/O与预分配能力按实际FS分别报告。

<!-- write: labs/linux/p4/io.c -->
```c
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
    action.sa_handler = alarm_handler; // 唯一异步动作是设标量。
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
```

<a id="linux-storage-run"></a>
## 保存真正的内核边界

正文中的返回 5 和同步顺序都必须由本次调用产生，不能把预期串写成观察记录。run.sh 通过 strace 运行同一程序，普通日志与系统调用日志分开。调用参数里的iovec地址、返回5、同步与rename顺序都应来自本次Linux运行；工具缺失或程序失败都不写passed。findmnt存在时补充/tmp的文件系统；O_DIRECT是否验证另存布尔值，不能用核心实验通过掩盖该能力未测。

**新建 `labs/linux/p4/run.sh`。** 只在Linux执行完整实验，结果不会从预期文本生成。

<!-- write: labs/linux/p4/run.sh -->
```sh
#!/bin/sh
set -eu # 未定义变量或失败命令都停止。
if [ "$(uname -s)" != Linux ] || [ "$(uname -m)" != x86_64 ]; then # strace步骤固定个人x86-64 Linux。
    echo 'x86-64 Linux is required for this storage experiment.' >&2 # 不用Mac结果替代。
    exit 77 # 环境不符不是成功。
fi # 结束环境判断。
mkdir -p artifacts # 所有本次证据在独立实验目录。
printf '{"passed":false,"phase":"p4"}\n' > artifacts/result.json # 先撤掉旧成功身份。
uname -a > artifacts/environment.txt # 真正内核和架构。
cc --version >> artifacts/environment.txt # 实际编译器。
strace --version >> artifacts/environment.txt # 缺工具直接失败，不能伪造跟踪。
if command -v findmnt >/dev/null 2>&1; then # util-linux存在时补充/tmp实际文件系统。
    findmnt -T /tmp >> artifacts/environment.txt # 文件系统能力影响O_DIRECT与同步。
fi # 不是必须安装新工具才能进行核心实验。
cc -std=c11 -O0 -g -Wall -Wextra -Werror io.c -o io # 普通Linux C程序使用本机libc。
if strace -f -o artifacts/syscalls.log -e trace=openat,close,read,write,pread64,pwrite64,preadv,pwritev,fsync,fdatasync,renameat,unlinkat,fallocate,statx,alarm,rt_sigaction,mmap,msync,munmap,mincore ./io > artifacts/io.log 2>&1; then # 一次真实运行同时保留库输出与Linux边界。
    cat artifacts/io.log # 只有命令完成后回显结果。
else # strace或程序失败都不发布passed。
    status=$? # 在其他命令前保存失败码。
    cat artifacts/io.log >&2 # 具体错误供定位。
    exit "$status" # 交回实际失败。
fi # 核心程序已正常退出。
grep -q '^\[LINUX_P4_OK\]' artifacts/io.log # 还必须出现完整路径结束标记。
direct=false # 可选能力默认未验证。
if grep -q '^\[DIRECT_OK\]' artifacts/io.log; then # 仅实际执行成功才改状态。
    direct=true # JSON布尔字面量，不是带引号文本。
fi # 能力差异独立保存。
printf '{"passed":true,"phase":"p4","direct_verified":%s,"physical_power_loss_tested":false}\n' "$direct" > artifacts/result.json # 核心路径通过不等于所有FS能力通过。
```

**更新远端阶段选择。** p4只复制io.c与run.sh，使用AiriOS-labs/p4和artifacts/linux-p4。

<!-- write: scripts/linux_lab.py -->
```python
"""Run the authorized Linux programs on the user's personal Linux host."""  # 从本机发起个人 Linux 主机上的两份实验
import argparse  # 命令行解析器把字符串选项转换为带名称的属性。
import io     # BytesIO 为内存字节提供文件式读写接口。
import json   # 在 Python 对象与 JSON 文本之间转换，不执行其中内容。
import shlex  # 按 Shell 引用规则拆分或引用文本，区别于实际执行。
import subprocess  # 以参数列表启动已有外部程序，并读取输出或退出码。
import tarfile     # 读取或构造 tar 归档，成员路径及类型还需单独检查。
from tools import ROOT  # 共享 ROOT 指向项目根目录，后续路径不依赖调用位置。

parser=argparse.ArgumentParser()  # 主机参数解析器提供--host选项与用法提示。
parser.add_argument('--host',default='airi@ssh.airi-dynamic.top')  # 个人Linux主机可以显式替换，不涉及公司机器。
parser.add_argument('--phase', choices=('p1', 'p2', 'p3', 'p4'), default='p1') # 实验来源与远端目录使用同一阶段名。
args=parser.parse_args()  # 把进程参数转为Namespace，后续通过 args.host 取得目标主机字符串。
if args.host.startswith('-') or any(c.isspace() for c in args.host):  # 禁止选项形状和空白，避免host被外部工具误解析；生成器逐字符检查空白。
    raise RuntimeError('Invalid SSH host')  # 无效目标在启动SSH之前拒绝。
ssh=['ssh','-o','BatchMode=yes','-o','ConnectTimeout=10',args.host]  # 非交互认证，连接最多十秒，最后元素是单个host参数。
# Relative to SSH's login directory; does not overwrite any other project.
remote='AiriOS-labs/' + args.phase  # 相对个人主机登录目录的独立实验目录。
dest=ROOT/'artifacts'/('linux-' + args.phase) # 每阶段独立保存本轮状态。
dest.mkdir(parents=True, exist_ok=True) # SSH失败也必须有证据目录。
(dest/'remote.json').write_text(json.dumps({'host':args.host,'directory':remote,'verified':False,'status':'not yet verified'},indent=2)+'\n') # 先覆盖旧成功身份。
connection=subprocess.run(ssh+[f'mkdir -p {shlex.quote(remote)}'],capture_output=True,text=True,timeout=20) # 非交互连接另有总时间上界。
(dest/'connection.log').write_text(connection.stdout + connection.stderr) # 真实失败输出在复制源码之前也保留。
if connection.returncode: # 未连接就不复制也不声称程序运行。
    (dest/'remote.json').write_text(json.dumps({'host':args.host,'directory':remote,'verified':False,'status':'connection failed','exit':connection.returncode},indent=2)+'\n') # 明确本轮未验证。
    raise RuntimeError(f'Linux SSH failed ({connection.returncode}); see {dest}/connection.log') # 路径和失败状态交回统一入口。  # 本地参数列表启动SSH，远端命令文本中的目录由quote保护。
names = ('ordinary.c','raw_start.S','run.sh') if args.phase == 'p1' else ('memory.c','run.sh') if args.phase == 'p2' else ('process.c','run.sh') if args.phase == 'p3' else ('io.c','run.sh') # 只选择本阶段已写出的程序。
sources=[ROOT/'labs/linux'/args.phase/name for name in names]  # 列表推导按所选阶段名称构成Path对象。
subprocess.run(['scp','-q','-o','BatchMode=yes','-o','ConnectTimeout=10',  # scp安静且非交互传输，连接上限同SSH。
                *[str(p) for p in sources],f'{args.host}:{remote}/'],check=True)  # 列表内*展开本阶段路径字符串，最后一项是远端目标。
execution=subprocess.run(ssh+[f'cd {shlex.quote(remote)} && sh run.sh'])  # 远端先切实验目录，再执行shell；暂不check，失败也先下载证据。
data=subprocess.check_output(ssh+[f'cd {shlex.quote(remote)} && tar -cf - artifacts'])  # tar从标准输出交回归档bytes，-f -表示不写远端归档文件。
dest.mkdir(parents=True,exist_ok=True)  # 确保证据父目录存在。
with tarfile.open(fileobj=io.BytesIO(data)) as tar:  # 把内存归档作为文件交给tarfile，with结束关闭归档。
    tar.extractall(dest,filter='data')  # data过滤限制危险成员，解包到本轮证据目录。
if execution.returncode:  # 非零远端状态意味着实验失败，即使tar传输成功。
    raise RuntimeError(f'Linux experiment failed; evidence downloaded to {dest}')  # 原始证据已经带回，再报告失败位置。
result=json.loads((dest/'artifacts/result.json').read_text())  # 实际远端程序写出的JSON恢复成字典。
if result.get('passed') is not True:  # get缺键返回None，只有布尔True才接受。
    raise RuntimeError('Remote validation failed')  # 远端记录不满足成功合同。
(dest/'remote.json').write_text(json.dumps({'host':args.host,'directory':remote,'verified':True,'status':'program and GDB completed'},indent=2)+'\n')  # 记录本次实际host和目录，不把另一主机记录混用。
print(f'Remote evidence downloaded: {dest}')  # 显示本轮证据可查看位置。
```

**更新统一入口。** 扩充phase到p4，其余guest命令保持。

<!-- write: scripts/manage.py -->
```python
import argparse  # 命令行解析器把字符串选项转换为带名称的属性。
import json      # 在 Python 对象与 JSON 文本之间转换，不执行其中内容。
import shlex     # 按 Shell 引用规则拆分或引用文本，区别于实际执行。
import subprocess  # 以参数列表启动已有外部程序，并读取输出或退出码。
import sys  # 取得当前解释器路径、入口参数及标准错误流。
from tools import ROOT, tool, make_command  # 复用路径、工具查找与构建参数，不另建工具选择规则。
from deps import prepare  # 复用外部依赖下载与校验，统一入口先调用它。
from boot import check, command  # check 等待启动完成，command 只组装交互QEMU参数。
from storage_test import check_storage_restart # 两个独立CPU从同一实验盘恢复。
from console_test import check_console  # 复用真实键盘和UART输入验证。

PROFILES=('normal','no-framebuffer','missing-memory','edit-experiment','trap-fatal','stack-guard','block-readonly','block-timeout','block-missing')  # 元组固定九种构建配置，CLI choices 与 test 遍历共用。
def run(cmd):  # 所有统一入口外部命令都从同一根目录执行。
    subprocess.run(cmd,cwd=ROOT,check=True)  # 非零状态转成异常传至入口。
def compilation_database():  # 从 Make 干跑输出收集每个C/汇编文件的实际编译参数。
    # GNU Make dry-run is the single source of compiler flags, including headers.
    command=make_command('normal','kernel')  # 局部 command 是 Make 参数列表，不是导入的QEMU command函数。
    command.insert(1,'-Bn')  # 在可执行路径之后插入 -B强制列规则、-n只打印的选项。
    lines=subprocess.check_output(command,cwd=ROOT,text=True).splitlines()  # 读取命令文本并按行拆分，不执行打印出的编译器命令。
    entries=[]  # 保存编译数据库条目，每项对应一份源文件及参数。
    for line in lines:  # 逐条检查 Make 打印的命令。
        if ' -c ' not in line or ' -o ' not in line: continue  # 过滤目录创建与链接命令，只考虑单文件编译。
        tokens=shlex.split(line)  # 按shell引号解析参数边界，列表值随后直接保存。
        if '-c' not in tokens or '-o' not in tokens: continue  # 分词后再确认编译选项，排除仅字符串碰巧匹配的行。
        source=tokens[tokens.index('-c')+1]  # index 找到-c位置，其后一个元素是被编译的源文件路径。
        entries.append({'directory':str(ROOT),'file':str(ROOT/source),'arguments':tokens})  # 保存工作目录、绝对源路径和完整参数列表供 clangd 消费。
    target=ROOT/'build/normal/compile_commands.json'  # 数据库放在普通配置目录，.clangd 从此处读取。
    target.parent.mkdir(parents=True,exist_ok=True)   # 数据库父目录缺失时创建，已有目录保持。
    target.write_text(json.dumps(entries,indent=2)+'\n')  # 列表编码为JSON数组，输出本次真实构建参数。

def build(profile):  # 给指定配置调用同一Make规则，普通配置额外更新编辑器数据库。
    run(make_command(profile))  # 组装配置及工具覆盖，执行完整内核与镜像构建。
    if profile=='normal':  # 测试配置不替换日常编辑参数。
        compilation_database()  # 普通配置才导出clangd数据库。
def inspect_elf(profile='normal'):  # 核对独立内核目标，不接受宿主解释器、动态依赖或未解析符号。
    binary=ROOT/'build'/profile/'airios.elf'  # 定位本次配置的ELF，避免检查另一目录的旧产物。
    readelf=tool('llvm-readelf','llvm')  # 按既定规则找到 LLVM ELF 检查器。
    report=subprocess.check_output([readelf,'-h','-l','-d','-s',str(binary)],text=True)  # 合并ELF头、程序段、动态信息和符号表的文本报告。
    nm=subprocess.check_output([tool('llvm-nm','llvm'),'-u',str(binary)],text=True)  # -u 只列未定义符号，独立内核应没有内容。
    if nm.strip() or 'INTERP' in report or '(NEEDED)' in report:  # 非空未定义项、解释器段或动态依赖任一存在就拒绝。
        raise RuntimeError('Kernel is not a fully resolved freestanding executable')  # 报告不满足完全解析的独立目标约定。
    if 'X86-64' not in report and 'x86-64' not in report:  # 兼顾工具文字大小写，必须出现x86-64机器类型。
        raise RuntimeError('Wrong ELF target')  # 错误机器目标不能作为此次内核成功结果。
    if 'EXEC (Executable file)' not in report:  # 要求固定地址静态ET_EXEC，与链接脚本装载方式一致。
        raise RuntimeError('Expected a static ET_EXEC ELF')  # PIE或其他ELF种类不满足目前启动路径。
    (ROOT/'artifacts').mkdir(exist_ok=True)  # 建立检查报告目录。
    (ROOT/'artifacts/kernel-elf.txt').write_text(report)  # 保存实际ELF检查输出，便于后续调试查看。
    print('PASS ELF: x86-64 ET_EXEC, no interpreter/dependencies/undefined symbols')  # 只在机器、类型、依赖和符号全部符合后打印通过。

def doctor():  # 查工具与版本，再实际准备和构建内核以验证环境。
    record={}  # 字典以工具名称为键，值保存路径和首行版本。
    for name,formula in [('clang','llvm'),('ld.lld','lld'),('qemu-system-x86_64','qemu'),  # 每项二元组给可执行名称与对应软件包标识。
                         ('xorriso','xorriso'),('gmake','make'),('llvm-readelf','llvm')]:  # 继续列镜像制作、GNU Make和ELF检查器；循环解包到name/formula。
        executable=make_command()[0] if name=='gmake' else tool(name,formula)  # GNU Make沿用gmake再make的选择，其余工具按既有规则查找。
        flag='-version' if name=='xorriso' else '--version'  # xorriso 使用单横线，其余工具使用双横线版本参数。
        result=subprocess.run([executable,flag],capture_output=True,text=True)  # 启动版本命令，分别收集标准输出与错误文本。
        version=(result.stdout+result.stderr).splitlines()[0]  # 版本可能写在任一流，取合并后的第一行。
        record[name]={'path':executable,'version':version}     # 工具键映射到实际路径和版本，不只有安装包名称。
        print(f'OK {name}: {version}')  # 显示本轮查到的工具身份和版本。
    prepare()  # 下载并校验当前锁定引导器、协议头与固件。
    run(make_command(target='kernel'))  # 通过真实内核编译和链接验证环境，不仅看工具是否存在。
    compilation_database()  # 使用同一规则生成编辑器参数。
    inspect_elf()  # 检查刚构建出的普通配置ELF。
    (ROOT/'artifacts/toolchain.json').write_text(json.dumps(record,indent=2)+'\n')  # 保存本次实际工具路径和版本。
def main():  # 解析统一入口动作，并把它交给先前实现的脚本。
    if len(sys.argv)>1 and sys.argv[1]=='learn':  # 先检查有动作参数，再识别采用独立子解析器的learn。
        from learning import main as learn  # 只进入learn时导入后续提供的学习模块，别名避免与当前main混淆。
        learn(sys.argv[2:])  # 切片丢掉程序名和learn，把余下参数交给子入口。
        return  # 当前动作完成，不继续后面的构建或分派。
    parser=argparse.ArgumentParser(description='AiriOS build and learning tools')  # 建立动作解析器，不合法参数由argparse给出用法。
    parser.add_argument('action',choices=['doctor','build','run','test','debug','kernel-debug','linux-lab','clean','ui-preview','ui-test','docs-check','snapshots'])  # 位置参数限定动作集合，拼错动作不能落到默认运行QEMU。
    parser.add_argument('--app',choices=('terminal','files','editor','tasks','system','settings','viewer'),default='editor')  # 主机窗口默认Editor，只接受已知应用名。
    parser.add_argument('--state',choices=['normal','empty','loading','error','offline'],default='normal')  # 状态名限定到共享视图已有的五种状态。
    parser.add_argument('--profile',choices=PROFILES,default='normal')  # 镜像默认普通配置，只接受已发布配置。
    parser.add_argument('--phase',choices=('p1','p2','p3','p4'),default='p1') # Linux阶段显式选择，默认保持P1。
    parser.add_argument('--host',default='airi@ssh.airi-dynamic.top')   # 个人Linux主机可以显式替换，不涉及公司机器。
    parser.add_argument('--graphics', action='store_true', help='explicitly enable the optional desktop')  # 默认 False，不读取图形目录。
    args=parser.parse_args()      # 把进程参数转为Namespace，后续通过args.action等属性读取。
    if args.action=='snapshots':  # 只有导出动作需要读取Git教学提交链。
        from snapshots import generate  # 局部导入后续提供的快照模块。
        generate()  # 核对固定main提交链和旧目录保护后导出阶段源码。
        return      # 当前动作完成，不继续后面的构建或分派。
    if args.action=='docs-check':  # 只检查重建与导航时无需启动QEMU。
        from docs_check import check_docs  # 只有明确来源检查才读取全部课程。
        check_docs()  # 核对正文重建与本地导航，不阻塞默认 test。
        return  # 当前动作完成，不继续后面的构建或分派。
    if args.graphics:  # G24已提供纯标准库资产检查，运行图形不依赖G26的截图工具。
        from graphics_assets import verify_fonts  # 显式选择后才读取图形目录。
        verify_fonts()  # 不导入Pillow或Cocoa。
    if args.action in ('ui-preview', 'ui-test') or (args.action == 'test' and args.graphics):  # 完整图形回归在G26后使用。
        from ui_preview import preview, check_frontend  # 主线不会导入这份可选工具。
    if args.action=='ui-preview':  # 主机窗口无需内核启动依赖。
        preview(args.app,args.state)  # 打开指定应用和服务状态。
        return  # 此动作已完成。
    if args.action=='ui-test':  # 主机行为与图像观察采用前章接口。
        check_frontend()  # 运行共享C测试与平台可用快照。
        return  # 此动作已完成。
    if args.action=='clean':  # 清理不需要再构建内核。
        run(make_command(target='clean'))  # 调用Make既有删除规则。
        return  # 清理结束。
    if args.action=='linux-lab':  # Linux对照程序由独立脚本传到个人主机运行。
        run([sys.executable,str(ROOT/'scripts/linux_lab.py'),'--host',args.host,'--phase',args.phase])  # 当前解释器执行个人主机实验。
        return  # 远程实验结束后不启动另一台虚拟机。
    if args.action=='doctor':  # doctor自己按需准备和构建。
        doctor()  # 查工具、构建、生成编辑器参数并检查ELF。
        return    # 环境检查结束。
    prepare()  # 下载并校验当前锁定引导器、协议头与固件。
    if args.graphics:  # 图形和串口使用不同产物目录，不复用旧目标。
        args.profile = 'desktop-' + args.profile  # 如 normal 变为 desktop-normal。
    if args.action=='kernel-debug':  # 内核GDB观察需要先得到普通配置带符号ELF。
        build('normal')  # 调试默认仍使用串口目标，测试宏不进入调试对象。
        run([sys.executable,str(ROOT/'scripts/kernel_debug.py'),'--host',args.host])  # 远端GDB连接本地QEMU。
        return  # 调试记录完成后结束本动作。
    if args.action=='test':  # 完整验证串联主机算法、可选前端、九种启动和三条设备输入。
        run(make_command(target='host-test'))  # 编译同一纯C实现并运行带ASan/UBSan的主机边界检查。
        if args.graphics:  # 普通 test 不导入或验证前端。
            check_frontend(screenshots=False)  # 图形模型检查仍有自己的一组断言。
        results=[]  # 列表依次保存各配置的实际启动结果。
        profiles = [('desktop-' + p) if args.graphics else p for p in PROFILES]  # 呈现方式和故障场景正交。
        for profile in profiles:  # 按固定九种配置独立编译与启动。
            build(profile)  # 生成对应配置的ELF和镜像，产物目录彼此隔离。
            if profile.endswith('normal'):  # 普通目标额外检查ELF结构。
                inspect_elf(profile)  # 保存机器、类型、依赖与符号报告。
            results.append(check(profile))  # 等待该配置完成标记并保存结果，任一失败异常中止。
        (ROOT/'artifacts'/('desktop-boot-tests.json' if args.graphics else 'boot-tests.json')).write_text(json.dumps(results,indent=2)+'\n')  # 九种启动结果编码为JSON数组。
        prefix = 'desktop-' if args.graphics else ''  # 本轮输入验证选择同一呈现方式。
        console_results=[check_console(prefix+'normal'),check_console(prefix+'no-framebuffer'),check_console(prefix+'normal',uart_input=False)]  # 顺序验证normal UART、无帧缓冲UART与normal PS/2三条真实输入路径。
        (ROOT/'artifacts'/('desktop-console-tests.json' if args.graphics else 'console-tests.json')).write_text(json.dumps(console_results,indent=2)+'\n')  # 保存三次实际输入验证结果。
        check_storage_restart('desktop-normal' if args.graphics else 'normal') # 独立正式日志区的真实跨QEMU恢复。
    else:  # 普通build/run/debug共用指定配置构建路径。
        build(args.profile)  # build/run/debug先构建调用者选择的配置。
        if args.action!='build':  # 单独构建到此结束，run和debug继续启动虚拟机。
            work=ROOT/'artifacts'/('interactive-'+args.profile)  # 交互日志按配置分开保存。
            if args.action=='debug':  # debug把CPU停在复位并打开回环GDB端点。
                print('QEMU waits at reset. Remote GDB: target remote localhost:1234\n'  # 告诉调试者当前CPU尚未前进及连接地址。
                      'Then hbreak _start; continue. See dev-log/linux/01-entry.md.',flush=True)  # 提示入口硬件断点，flush保证运行虚拟机前已打印。
            if args.graphics:  # 图形交互仍将UART写入本轮文件。
                print(f'Serial log: {work/"serial.log"}',flush=True)  # 显示本次实际串口日志路径，立即刷新输出。
            cmd = command(args.profile,work,gui=args.graphics,debug=args.action=='debug')  # 先形成具体 QEMU 参数。
            if not args.graphics:  # 串口交互直接连接当前主机终端。
                cmd[cmd.index('-serial')+1] = 'mon:stdio'  # QEMU 用 Ctrl-A X 退出，Ctrl-A C 切监视器。
                print('Serial console: this terminal. Exit QEMU with Ctrl-A X.', flush=True)  # 字节不再写入文件。
            run(cmd)  # 启动所选串口或图形QEMU，debug动作才带-S和GDB。

if __name__=='__main__':  # 直接执行脚本才启动CLI，导入时仅定义接口。
    try:  # 入口错误统一转成明确的失败状态。
        main()  # 执行解析和分派，异常留给下面处理。
    except (RuntimeError,subprocess.CalledProcessError,AssertionError) as error:  # 三类异常都保留具体原因并转成非零退出。
        print(f'ERROR: {error}',file=sys.stderr)  # 错误文字送标准错误，不混入正常结果流。
        sys.exit(1)  # 失败状态交给shell和调用脚本，不能继续宣称通过。
```


```sh
./airios linux-lab --phase p4 # 在个人Linux主机运行完整路径并取回真实证据。
```

2026-10-15 的既有个人主机连接返回SSH255，仍为verified=false；Linux分支编译、程序结果、strace、直接I/O及文件系统能力均未验证。Mac只通过拒绝分支编译和返回77，Shell 语法检查通过。连接失败日志保留于artifacts/linux-p4/connection.log，不伪造上面描述的预期结果。

连接可用时，VECTOR_OK应记录请求10返回5和偏移3；PAGE_CACHE_OK记录共享映射当前可见与驻留；EINTR_OK必须来自真信号；REPLACE_OK显示旧FD与新名字分开。DIRECT_OK与PREALLOC_OK是否出现取决于实际能力，缺失必须有明确未验证原因。

依据为[向量I/O](https://man7.org/linux/man-pages/man2/readv.2.html)、[statx](https://man7.org/linux/man-pages/man2/statx.2.html)、[posix_fallocate](https://man7.org/linux/man-pages/man3/posix_fallocate.3.html)、[msync](https://man7.org/linux/man-pages/man2/msync.2.html)、[信号安全调用](https://man7.org/linux/man-pages/man7/signal-safety.7.html)、[fsync](https://man7.org/linux/man-pages/man2/fsync.2.html)与[rename](https://man7.org/linux/man-pages/man2/rename.2.html)。后续回到BOSS的分散缓冲区和Taishan文件创建路径时，分别核对实际返回数、地址/长度/偏移对齐，以及数据和名称各自的同步位置。
