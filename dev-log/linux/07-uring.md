# L07 · 把请求交给Linux，再按cookie取回真实完成

L06 的 epoll 返回可读事件以后，应用仍要调用 `recv`，把字节从内核接收缓冲取到自己的数组中。现在改为一次具体的文件读取：应用先说明读哪个 FD、从哪里开始、最多读多少字节、结果写进哪份数组，再把这次操作交给 Linux。以后取得的通知要回答“这次读实际得到了什么”。

io_uring 为此提供提交队列 SQ 和完成队列 CQ。应用填写 SQ 中的操作描述，Linux 执行后向 CQ 写入结果记录；用户库 liburing 封装建环、提交和收取。队列是内核与当前进程共享的一组内存及索引，环形槽位可以反复使用；内核执行读时写入的目标数组是另一块应用内存。下面沿 cookie103 的短读区分这两类存储。

本章从 L06 的工程继续，对照 K32 已有的自制提交/完成接口。本例先完整写入一个 1536 字节临时文件，再准备五次读取。前三次读完整区间，第四次正常短读，第五次得到错误完成；它们使用同一套提交与收取代码。FD 打开后立即 unlink 移除临时名称，打开的 FD 仍引用该文件，正常路径最后关闭 FD 才结束这份引用并回收文件，不留下实验路径。

<a id="uring-submit"></a>
## 先确定每条请求读哪里、写到哪一份数组

`io_uring_queue_init(8,&ring,0)` 请求建立可容纳本批五项的环，ring 保存后续操作需要的队列状态，flags 为零，不启用后台 SQ 轮询。建环也需要 Linux 内核允许相应系统调用，失败时程序沿后面的错误分支退出。

先固定读操作的输入。source 保存稍后写进文件的 1536 个模式字节，buffers 是五行、每行 512 字节的普通数组。每项请求独占一行，即使多项同时处于执行中，也不会互相覆盖目标。实际准备出的五项如下：

| cookie | FD 与文件偏移 | 请求长度 | 目标与预期结果 |
| --- | --- | --- | --- |
| 100 | 有效 FD，0 | 512 | buffers[0]，512 字节 |
| 101 | 同一 FD，512 | 512 | buffers[1]，512 字节 |
| 102 | 同一 FD，1024 | 512 | buffers[2]，512 字节 |
| 103 | 同一 FD，1530 | 16 | buffers[3]，只剩 6 字节 |
| 104 | −1，2048 | 512 | buffers[4]，−EBADF |

其中第四项从文件偏移 1530 请求 16 字节，目标是 buffers[3]。文件只有 1536 字节，最多还剩 `1536−1530=6` 字节，这正是稍后要从完成结果确认的短读。数组这一行有 512 字节容量，请求只允许读 16 字节，实际结果又可能只有 6 字节；容量、请求量与完成量在同一次操作中各有用途。

前四项的 FD 相同，前三项连长度也相同，完成后不能靠 FD 或长度找回目标行。应用选择 100–104 作为请求身份 cookie，写进各提交项 SQE 的 user_data 字段；Linux 在完成项 CQE 中带回同一个整数。收到 103 就能用 `103−100=3` 找回 buffers[3]。这个数由应用选择，既不是 Linux 分配的 FD，也不依赖完成顺序。

五项都给出显式文件偏移。前面的同步 write 会把 FD 的当前位置推进到文件末尾，后面的这些读仍分别从 0、512、1024、1530 开始，不使用或推进那份共享位置。第五项故意给出 FD=-1，准备和提交可以成功，但执行时应返回 −EBADF；偏移 2048 不会把无效 FD 的错误变成 EOF。接收操作描述和操作本身的结果在两个位置检查。

source 与 buffers 都是静态数组，在整个队列生命期内存在。填写描述时交出的目标地址只指向数组，没有复制出另一个 16 字节结果区。提交后 Linux 仍可能访问它，直到取得该次操作的完成，应用都保留原数组且不提前复用对应行。本例没有注册固定缓冲，也没有 AiriOS 的注册 handle；K32 到 pop 才解除 BUSY 是自制接口的规则，这里由应用自己维持普通数组的寿命。

**新建 `labs/linux/p5-uring/uring.c`，先准备固定数据、错误报告和单调时间：**

<!-- write: labs/linux/p5-uring/uring.c -->
```c
#define _GNU_SOURCE // mkstemp等Linux实验接口。
#include <stdio.h> // 真实完成结果输出。
#if defined(__linux__) && defined(__x86_64__) // 只在实际Linux编译liburing路径。
#include <errno.h> // 同步错误与负CQ结果分开。
#include <fcntl.h> // 临时文件和fsync操作。
#include <liburing.h> // SQ/CQ封装，运行时记录实际版本。
#include <stdlib.h> // mkstemp与退出。
#include <string.h> // strerror与数据比较。
#include <time.h> // 部分提交的总截止。
#include <unistd.h> // 写入基线数据与关闭。
static struct io_uring ring; // 从初始化到最后CQ消费一直有效。
static unsigned char source[1536], buffers[5][512]; // 普通缓冲区不移动、不提前复用，不声称已注册固定缓冲。
static void require(int yes, const char *what) { // 每个失败都停止成功路径。
    if (!yes) { perror(what); exit(1); } // 失败退出由OS回收整个进程，正常路径显式消费再释放。
}
static long long now_ms(void) { // 单调时间限制反复提交。
    struct timespec t; require(clock_gettime(CLOCK_MONOTONIC,&t)==0,"clock"); // 不使用墙上时间。
    return (long long)t.tv_sec*1000+t.tv_nsec/1000000; // 转毫秒便于截止比较。
}
```

<a id="分别处理提交进度与取得完成记录"></a>

## SQ 填好以后，仍要分别提交和等待

下面的 main 将用 `io_uring_get_sqe` 取得尚未提交的 SQ 项。对于 cookie103，`prep_read` 把有效 FD、buffers[3] 的地址、16 和 1530 写进去，`set_data64` 再写入 103。这些调用只准备描述；它们还没有把文件字节取到 buffers[3]。`get_sqe` 返回空指针表示没有可填写的槽，必须先停止，不能继续写字段。

准备完五项以后，`io_uring_submit` 推进提交。未启用 SQPOLL 的本例中，正返回值是这次交出的 SQE 项数；`accepted=5` 表示五份操作已交出，尚未报告任何一项读了多少字节。Linux 可能在提交期间就完成某项，也可能以后才完成，应用统一从 CQ 取得相应结果。

例如某次调用若只交出三项，`submit_count(5)` 就把累计值改成 3，下次继续提交 liburing 保留的剩余项，不重新创建那三项。本轮具体返回多少，由实际内核决定。循环在调用前后用同一个两秒截止检查进度，EINTR 在剩余预算内重试；零进度、数量异常或到期未交齐都退出。这里限制的是重试流程，程序外的三十秒 timeout 还会约束整个运行。

`wait_one` 解决另一件事：向 `io_uring_wait_cqe_timeout` 交出两秒等待期限和指针的地址，让它在成功时把一个 CQ 槽的地址写进 cqe。已有完成时可以立即取得。函数返回 0 表示拿到了记录，接下来才能读取记录里的 res；这项 res 才是 I/O 结果。等待超时返回 −ETIME，属于本次等待失败，和读完成的 res=0 不是同一个零。每次调用 wait_one 都有自己的两秒期限，五次等待没有共用一个批次两秒截止；等待受到 EINTR 等错误打断时，这个 helper 直接退出。

提交和等待 helper 使用同一种错误转换：liburing 返回的负数本身就是负错误码，例如 −ETIME。为让 perror 输出对应文字，先设置 `errno=-rc`，再进入 require 的失败出口。随后读到 CQE 的负 res 也按负错误码解释，但它不会自动更新当前线程的 errno，收取代码要直接比较那个值。

**追加两个有界辅助函数：**

<!-- append: labs/linux/p5-uring/uring.c -->
```c
static void submit_count(unsigned expected) { // io_uring_submit可能只接受部分SQE。
    unsigned submitted=0; long long end=now_ms()+2000; // 预算不在每次重试时重置。
    while (submitted<expected && now_ms()<end) { // 仅重交尚未接受的SQ尾部。
        int n=io_uring_submit(&ring); // 返回实际提交数，负数是负错误码。
        if (n==-EINTR) continue; // 信号打断可在剩余预算内继续。
        if (n<0) { errno=-n; require(0,"io_uring_submit"); } // 不用可能陈旧的errno解释liburing返回值。
        require(n>0 && (unsigned)n<=expected-submitted,"submission made bounded progress"); // 零进度不忙转。
        submitted+=(unsigned)n; printf("[URING_SUBMIT] accepted=%d cumulative=%u\n",n,submitted); // 记录实际批量结果。
    }
    require(submitted==expected,"submission deadline"); // 未接受完整批次不能假定都在内核。
}
static struct io_uring_cqe *wait_one(void) { // 等待有界，不以队列存在当成功。
    struct __kernel_timespec limit={.tv_sec=2,.tv_nsec=0}; // 单个完成等待最多两秒。
    struct io_uring_cqe *cqe=NULL; int rc=io_uring_wait_cqe_timeout(&ring,&cqe,&limit); // 返回借用CQ槽指针。
    if (rc<0) { errno=-rc; require(0,"CQ wait (timeout is failure)"); } // -ETIME也是明确失败，不是EOF。
    require(cqe!=NULL,"CQ pointer"); return cqe; // 只有成功后才能读结果。
}
```

## 准备同一批输入，再把五项交给 Linux

main 先建环。返回 −ENOSYS、−EPERM 或 −EACCES 时，程序报告内核能力或权限、策略拒绝，并返回 77。其他初始化错误转成 errno 后停止；失败时还没有准备这批读操作。只有建环成功，才用 mkstemp 创建一份独立临时文件，随后移除名称并保留 FD。

每个 source 字节取 `(i*31+7)` 的低八位，文件偏移和预期内容因而一一对应。写入基线时，written 保存已经写入的数量，`source+written` 与 `sizeof source-written` 只交出尚未写入的后缀。遇到 EINTR 重试，零或其他错误退出，正数才增加 written；全部 1536 字节写齐，才进入异步读的准备循环。

循环下标 i 同时选择 buffers[i] 和 cookie `100+i`。走到 i=3 时，bytes=16、offset=1530，写出刚才跟随的 cookie103；走到 i=4 时改用 FD=-1。五项都填写完成，再调用 submit_count。它返回时只确定提交已交齐，所有目标数组继续保留，读取结果接在下一块。main 在下一块结束以前尚未闭合，中途不编译。

**追加初始化、文件输入和五条提交：**

<!-- append: labs/linux/p5-uring/uring.c -->
```c
int main(void) { // 单线程提交五个读，再独立提交同步请求。
    int rc=io_uring_queue_init(8,&ring,0); // 不使用SQPOLL，不擅自更改系统策略。
    if (rc==-ENOSYS || rc==-EPERM || rc==-EACCES) { // 内核缺能力或策略禁用不能伪装成库测试成功。
        fprintf(stderr,"io_uring unavailable: %s; kernel/policy not changed.\n",strerror(-rc)); return 77; // 如实未验证。
    }
    if (rc<0) { errno=-rc; require(0,"queue_init"); } // 其他初始化错误保留实际原因。
    char path[]="/tmp/airios-uring-XXXXXX"; int fd=mkstemp(path); require(fd>=0,"mkstemp"); // 独立实验文件。
    require(unlink(path)==0,"unlink temporary name"); // FD仍持有文件，退出后不留下路径。
    for (unsigned i=0;i<sizeof source;++i) source[i]=(unsigned char)(i*31+7); // 每个偏移有可核对内容。
    size_t written=0; while (written<sizeof source) { // 写入基线也处理短写。
        ssize_t n=write(fd,source+written,sizeof source-written); // 只提交尚未写入后缀。
        if (n<0 && errno==EINTR) continue; // 尚无进度可重试。
        require(n>0,"seed write"); written+=(size_t)n; // 零或错误都不能算完整文件。
    }
    for (unsigned i=0;i<5;++i) { // SQ容量八，本批五个请求。
        struct io_uring_sqe *sqe=io_uring_get_sqe(&ring); require(sqe!=NULL,"SQ space"); // 空间不足不能写空指针。
        size_t bytes=i==3?16:512; off_t offset=i==3?1530:(off_t)i*512; // EOF请求只剩六字节。
        io_uring_prep_read(sqe,i==4?-1:fd,buffers[i],(unsigned)bytes,offset); // 无效FD也作为请求被提交，错误在CQ返回。
        io_uring_sqe_set_data64(sqe,100+i); // 数字cookie映射到独立缓冲区，不按完成顺序猜身份。
    }
    submit_count(5); unsigned seen=0; // 完成顺序由实际内核决定。
```

<a id="uring-completion"></a>
## 从 cookie103 找回六个有效字节

提交之后，Linux 为已完成的操作写入 CQE，其中 user_data 带回 cookie，res 带回实际结果。下面把“收到 cookie103”作为一条收取路径来读；它可能第一个到达，也可能排在其他项之后，代码都按同样方式处理。

`wait_one` 成功交回槽指针后，程序先把 cookie 和 res 复制到局部整数。cookie103 落在 100–104 内，减去 100 得到下标 3。seen 是一个位集合，下标 3 对应 `1u<<3`；先检查这位尚未置一，再置一，防止重复身份占掉五次循环中的一次，导致另一条请求没有被收取。

对于下标 3，expected 为 6，程序要求本次 res 正好是 6，再比较 `buffers[3][0..5]` 与 `source[1530..1535]`。按输入公式，这六个预期字节依次为 0x4d、0x6c、0x8b、0xaa、0xc9、0xe8。读取请求允许最多 16 字节，文件尾部只给出六个；后十字节不属于本次有效结果，512 字节数组的剩余空间也不进入比较。正常路径不会自动补第二次读。

前三项要求 res=512，第五项要求 res=−EBADF。前者比较各自 512 字节，后者核对错误值，不比较 buffers[4] 内容。这批读中，正 res 给出有效前缀长度，0 表示到达 EOF，负数表示操作错误；本例没有预期 res=0 的读。完成顺序可以恰好与提交相同，验证依据 cookie、结果和内容，没有要求制造乱序。

核对通过后，程序才打印 URING_CQ，并用 `io_uring_cqe_seen` 推进 CQ 的消费位置，归还这个完成槽。以后该槽可能保存另一条记录，旧 cqe 指针便不再代表 cookie103。buffers[3] 是独立的应用数组，seen 不释放它；本例选择消费完成以后才允许复用对应行，实际上数组一直保留到进程结束。这个顺序分别结束“借用完成记录”和“等待目标数据写完”两件事。

五条读取收完后，文件 FD 仍有效，再准备 cookie900 的 FSYNC。它与读批次分开提交，等待 res=0 后才拆队列并关闭 FD。FSYNC 的 0 表示文件同步成功，读的 0 才表示 EOF，结果必须连同操作种类解释。这里没有目录持久化或物理断电实验；初始文件也可能仍在页缓存中，本批使用普通缓冲 I/O，没有设置 O_DIRECT，不承担裸盘吞吐测量。

**追加收取、核对与正常清理，并闭合整个程序：**

<!-- append: labs/linux/p5-uring/uring.c -->
```c
    for (unsigned order=0;order<5;++order) { // 每个普通SQE期望一个CQE。
        struct io_uring_cqe *cqe=wait_one(); // 缓冲区仍然在静态数组中存活。
        uint64_t cookie=io_uring_cqe_get_data64(cqe); int result=cqe->res; // 先保存值再释放CQ槽。
        require(cookie>=100 && cookie<105,"CQ cookie range"); unsigned i=(unsigned)(cookie-100); // 身份决定验证规则。
        require(!(seen&(1u<<i)),"unique CQ cookie"); seen|=1u<<i; // 重复完成不能漏算另一个请求。
        int expected=i==4?-EBADF:(i==3?6:512); require(result==expected,"CQ exact result"); // 负结果直接是-errno。
        if (result>0) require(memcmp(buffers[i],source+(i==3?1530:i*512),(size_t)result)==0,"read bytes"); // 只比较真正读出的前缀。
        printf("[URING_CQ] order=%u cookie=%llu result=%d\n",order,(unsigned long long)cookie,result); // 不要求顺序一定乱序。
        io_uring_cqe_seen(&ring,cqe); // 到这里CQ槽可重用，本程序此后才允许重用对应缓冲。
    }
    struct io_uring_sqe *sqe=io_uring_get_sqe(&ring); require(sqe!=NULL,"fsync SQ"); // 与前面读批次分开，顺序明确。
    io_uring_prep_fsync(sqe,fd,0); io_uring_sqe_set_data64(sqe,900); // flags零表示文件fsync，不代表目录同步。
    submit_count(1); struct io_uring_cqe *cqe=wait_one(); // 同步本身也是有完成结果的请求。
    require(io_uring_cqe_get_data64(cqe)==900 && cqe->res==0,"fsync CQ"); io_uring_cqe_seen(&ring,cqe); // 错误同步不能算落盘成功。
    io_uring_queue_exit(&ring); require(close(fd)==0,"close"); // 全部完成已收取后再拆队列、关闭文件。
    puts("[LINUX_URING_OK] reads=3 short_eof=6 error=-EBADF fsync=0"); return 0; // 只有真实完成全部验证才发布。
}
#else // 非Linux只提供环境拒绝分支。
int main(void) { fputs("x86-64 Linux with liburing is required.\n",stderr); return 77; } // 不能代替Linux编译。
#endif // 完成平台分支。
```

若 cookie 范围、重复位、res 或内容检查失败，require 立即退出，后面的成功记录和正常清理不会执行，进程资源由 Linux 退出路径回收。诊断文字需要按失败位置读取：提交和等待的负 rc 已先转成 errno，perror 能解释对应错误；“CQ exact result”“read bytes”或“fsync CQ”检查则没有设置 errno，尾部文字可能来自旧调用，甚至显示 Success。此时标签只指出哪项核对失败，请求错误仍取决于实际 res。

URING_CQ 放在这些核对以后，因此失败项未必已经打印出 cookie/res。需要定位意外完成时，在调试器中停到对应 require 之前查看局部 cookie、result；FSYNC 则查看当前 cqe 的 user_data 与 res。当前输出没有记录所有失败项的实际值。无论诊断文字怎样，非零退出都会使运行脚本保留 passed=false。

<a id="uring-environment"></a>
<a id="把-linux-环境程序结果和观察证据分别留下"></a>
<a id="把-linux-环境、程序结果和观察证据分别留下"></a>

## 在实际 Linux 上编译，再记录队列调用与完成结果

这份程序要在 x86-64 Linux 上使用 liburing。库头文件和链接库解决“能否编译”，内核能力与系统策略决定“能否建环”，两项条件分别检查。run.sh 先检查系统和架构，再找 cc、pkg-config、strace、timeout 与 liburing 元数据；缺少时列出 Debian/Ubuntu 或 Arch/CachyOS 的对应包名并返回 77。工具齐全以后，queue_init 仍可能被内核拒绝，程序也保留了这条退出路径。

脚本用 pkg-config 取得编译与链接参数：Shell 的命令替换先运行查询，把返回的参数加入 cc 命令。编译前把实际内核、编译器、liburing、strace 版本写入 environment.txt，getconf 的 libc 查询允许非 glibc 环境不提供结果。随后 timeout 为 strace 和程序设置三十秒期限，超时后再留两秒才强制结束。程序标准输出、错误输出一起进入 uring.log，系统调用另入 uring-syscalls.log。

追踪范围为 `io_uring_setup`、`io_uring_enter`、`io_uring_register`：它们分别涉及建立环、提交或等待、注册资源。普通 read SQE 使用普通数组，不要求先调用注册缓冲接口，所以追踪中没有 io_uring_register 也可以正常完成。具体数据结果仍从 URING_CQ 和内容比较取得，系统调用记录本身不会替程序验证那六个字节。

每次脚本先把 result.json 写为 passed=false。缺工具、初始化拒绝、超时或结果不符会提前退出，这个状态就保留下来。只有程序正常结束，并且日志有 LINUX_URING_OK，脚本才写 passed=true；后面的统一执行器还要取回并读取这份实际结果，才写 verified=true。脚本不会安装依赖、改 sysctl 或放宽容器权限，环境拒绝就沿明确失败出口结束。

**新建 `labs/linux/p5-uring/run.sh`：**

<!-- write: labs/linux/p5-uring/run.sh -->
```sh
#!/bin/sh
set -eu # 任一步失败都不发布成功。
mkdir -p artifacts # 即使缺环境也保存明确状态。
printf '{"passed":false,"phase":"p5-uring"}\n' > artifacts/result.json # 撤销旧成功记录。
if [ "$(uname -s)" != Linux ] || [ "$(uname -m)" != x86_64 ]; then # 固定本轮个人Linux目标。
    echo 'x86-64 Linux with liburing is required.' >&2 # Mac不能提供Linux内核队列。
    exit 77 # 未验证。
fi # 环境检查结束。
for tool in cc pkg-config strace timeout; do # 编译、依赖查询、观察与总运行上界。
    if ! command -v "$tool" >/dev/null 2>&1; then # 缺工具给具体修复提示。
        echo "Missing $tool. Debian/Ubuntu: apt install build-essential pkg-config liburing-dev strace coreutils; Arch/CachyOS: pacman -S base-devel pkgconf liburing strace coreutils." >&2 # 不自动提权安装。
        exit 77 # 保持未验证。
    fi # 当前工具存在。
done # 工具清单完成。
if ! pkg-config --exists liburing; then # 有pkg-config不等于安装了开发库。
    echo 'Missing liburing: Debian/Ubuntu liburing-dev; Arch/CachyOS liburing.' >&2 # 明确包名。
    exit 77 # 不假造CQ。
fi # liburing元数据已找到。
uname -a > artifacts/environment.txt # 真实Linux内核和架构。
cc --version >> artifacts/environment.txt # 实际编译器。
pkg-config --modversion liburing >> artifacts/environment.txt # 真实链接依赖版本。
strace --version >> artifacts/environment.txt # 观察器本身也需可复现。
getconf GNU_LIBC_VERSION >> artifacts/environment.txt 2>&1 || true # 非glibc可没有此项。
cc -std=c11 -O0 -g -Wall -Wextra -Werror uring.c $(pkg-config --cflags --libs liburing) -o uring # 固定工具给出库参数，Shell展开为编译参数。
if timeout -k 2 30 strace -o artifacts/uring-syscalls.log -e trace=io_uring_setup,io_uring_enter,io_uring_register ./uring > artifacts/uring.log 2>&1; then # 三十秒总预算。
    cat artifacts/uring.log # 实际返回与cookie顺序。
else # 依赖策略、超时和完成错误都记录失败。
    status=$?; cat artifacts/uring.log >&2; exit "$status" # 不丢失真实退出码。
fi # 完成实际观察。
grep -q '^\[LINUX_URING_OK\]' artifacts/uring.log # 所有预期CQ都被核对才成功。
printf '{"passed":true,"phase":"p5-uring","registered_buffers":false}\n' > artifacts/result.json # 普通持久数组不冒充固定注册缓冲。
```

## 让阶段名选择这份实验，保留已有传输和取证流程

原 Linux 执行器已经按阶段选择源码，传到个人主机的阶段目录，进入该目录执行 `sh run.sh`，再取回 artifacts。本次给映射增加 p5-uring，关联刚才写出的 uring.c 与 run.sh。它们在 Mac 学习目录中写好，由个人 Linux 主机编译；guest 内核不会读取 liburing，也不会运行这个程序。其余传输、超时和失败记录沿用原流程。

**更新 `scripts/linux_lab.py`：**

第一块定位旧文本，第二块完整替换；其余实现保留。

<!-- replace: scripts/linux_lab.py -->
<!-- before -->
```python
    'p3': ('process.c', 'run.sh'), # fork/exec与FD继承。
    'p4': ('io.c', 'run.sh'), # 向量、缓存和同步替换。
    'p5-epoll': ('epoll.c', 'run.sh'), # 本机TCP就绪与背压。
    'p5-threads': ('threads.c', 'run.sh'), # 线程队列、原子发布与futex。
```
<!-- after -->
```python
    'p3': ('process.c', 'run.sh'), # fork/exec与FD继承。
    'p4': ('io.c', 'run.sh'), # 向量、缓存和同步替换。
    'p5-uring': ('uring.c', 'run.sh'), # liburing提交、身份和完成生命周期。
    'p5-epoll': ('epoll.c', 'run.sh'), # 本机TCP就绪与背压。
    'p5-threads': ('threads.c', 'run.sh'), # 线程队列、原子发布与futex。
```

统一入口还要接受 p5-uring 作为 --phase 取值，再传给同一个 Linux 执行器。它不增加 guest 编译依赖，也不改变 K32 自制接口。

**更新 `scripts/manage.py`：**

第一块定位旧文本，第二块完整替换；其余实现保留。

<!-- replace: scripts/manage.py -->
<!-- before -->
```python
    parser.add_argument('--state',choices=['normal','empty','loading','error','offline'],default='normal')  # 状态名限定到共享视图已有的五种状态。
    parser.add_argument('--profile',choices=PROFILES,default='normal')  # 镜像默认普通配置，只接受已发布配置。
    parser.add_argument('--phase',choices=('p1','p2','p3','p4','p5-threads','p5-epoll'),default='p1') # Linux阶段显式选择，默认保持P1。
    parser.add_argument('--host',default='airi@ssh.airi-dynamic.top')   # 个人Linux主机可以显式替换，不涉及公司机器。
    parser.add_argument('--graphics', action='store_true', help='explicitly enable the optional desktop')  # 默认 False，不读取图形目录。
```
<!-- after -->
```python
    parser.add_argument('--state',choices=['normal','empty','loading','error','offline'],default='normal')  # 状态名限定到共享视图已有的五种状态。
    parser.add_argument('--profile',choices=PROFILES,default='normal')  # 镜像默认普通配置，只接受已发布配置。
    parser.add_argument('--phase',choices=('p1','p2','p3','p4','p5-threads','p5-epoll','p5-uring'),default='p1') # Linux阶段显式选择，默认保持P1。
    parser.add_argument('--host',default='airi@ssh.airi-dynamic.top')   # 个人Linux主机可以显式替换，不涉及公司机器。
    parser.add_argument('--graphics', action='store_true', help='explicitly enable the optional desktop')  # 默认 False，不读取图形目录。
```

## 运行并区分预期结果与实际验证状态

全部编辑完成后，在学习目录根运行下面命令。既有执行器默认连接个人主机 `airi@ssh.airi-dynamic.top`，在登录目录下使用 `AiriOS-labs/p5-uring`，将本章两份文件传入后执行脚本，再取回版本、提交数量和 CQ 记录。本机仍需要已配置的非交互 SSH/scp，远端需要前面脚本检查的工具和内核支持；连接失败时还没有进入 Linux 编译。

```sh
./airios linux-lab --phase p5-uring # 编译实际Linux分支并取回SQ/CQ、错误与版本记录。
```

可用环境中，五条 URING_CQ 应分别关联 cookie100–104，结果依身份为 `512、512、512、6、−EBADF`，行的出现次序由实际内核决定。URING_SUBMIT 的 accepted/cumulative 记录提交数量，CQ 的 result 记录实际操作结果。cookie900 的同步成功 0 会被程序核对，但这段实现没有单独打印一条 FSYNC 完成行；最后的 LINUX_URING_OK 才表示它也已收取并核对。若连接、依赖、策略或等待失败，沿保存的阶段状态定位。

2026-10-16 的既有记录中 SSH 返回 255，Linux 分支、liburing 依赖、内核支持和 CQ 运行均未验证。Mac 当时编译了拒绝分支，执行返回 77，并通过脚本语法检查；失败记录保存在 `artifacts/linux-p5-uring/remote.json` 和 `connection.log`。这份历史记录没有对应的 CQ 输出。

2026-10-27 重新执行上面的统一入口，SSH 已连接个人 Linux 主机，脚本在工具检查处报告 `Missing strace` 并返回 77。统一执行器取回的结果仍为 passed=false、verified=false；程序尚未编译，也未建立队列。补齐该工具后可重跑同一命令，上面的 CQ 数值仍是需要核对的预期。

回到 BOSS 的批量读取，关联目标页、长度与错误依靠每条请求自己的身份；部分提交以后还要辨认哪些已经交出、哪些仍未提交。超时、退出或取消不能单独证明内核再不会访问缓冲，回收依据具体接口的完成语义。AiriOS 请求副本将退出后的设备目标与用户页分开，代价是复制和固定容量；Linux 本例则由应用保留普通数组，不能混称固定缓冲注册。

接口依据：[liburing 提交手册](https://github.com/axboe/liburing/blob/master/man/io_uring_submit.3)、[读请求准备](https://github.com/axboe/liburing/blob/master/man/io_uring_prep_read.3)、[有界 CQ 等待](https://github.com/axboe/liburing/blob/master/man/io_uring_wait_cqe_timeout.3)、[完成槽消费](https://github.com/axboe/liburing/blob/master/man/io_uring_cqe_seen.3)与 [Linux io_uring](https://man7.org/linux/man-pages/man7/io_uring.7.html)。库版本以实际环境文件为准，master 链接用于接口说明，不代表已锁定并运行该版本。
