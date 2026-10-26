# L07 · 把请求交给Linux，再按cookie取回真实完成

L06 的 epoll 告诉应用某个 FD 现在值得尝试读取，真正的读仍由应用调用。io_uring 接收的是一次具体操作：文件、目标数组、长度与偏移已经给出，Linux 执行后再把这次操作的结果放进完成队列 CQ。K32 已有自制提交/完成接口，本章用 liburing 接入 Linux，沿实际请求辨认两者相同的交接问题与不同的资源约定。

本例先完整写入一个 1536 字节临时文件，再准备五次读取。前三次读完整区间，第四次正常短读，第五次得到错误完成；它们使用同一套提交与收取代码。FD 打开后 unlink 移除临时名称，仍可沿 FD 访问文件，最后关闭才结束这份引用并回收文件，不在磁盘上留下实验路径。

<a id="uring-submit"></a>
## 先确定每条请求读哪里、写到哪一份数组

`queue_init(8)` 建立本批五项能放下的环，不启用后台 SQ 轮询，也不修改主机策略。下面的 source 保存已写入文件的模式字节，buffers 为各请求提供独立目标：

| cookie | FD 与文件偏移 | 请求长度 | 目标与预期结果 |
| --- | --- | --- | --- |
| 100 | 有效 FD，0 | 512 | buffers[0]，512 字节 |
| 101 | 同一 FD，512 | 512 | buffers[1]，512 字节 |
| 102 | 同一 FD，1024 | 512 | buffers[2]，512 字节 |
| 103 | 同一 FD，1530 | 16 | buffers[3]，只剩 6 字节 |
| 104 | −1，2048 | 512 | buffers[4]，−EBADF |

前四项的 FD 相同，前三项连长度也相同，不能靠这些值判断某条完成属于谁。应用选择 100–104 作为 cookie，经 `set_data64` 放进 SQE 的 user_data；Linux 在 CQE 中带回这个数值，应用再映射到对应数组下标。这里的 cookie 不是内核替我们生成的 FD，也不要求 CQE 按提交位置出现。

读请求各自给出显式文件偏移，不推进或依赖普通 read 的共享当前位置。第四项的 `1536−1530=6` 决定正常有效前缀；第五项虽然准备和提交可以成功，执行时无效 FD 的错误仍应由 CQ 返回。接收请求和执行结果要分别处理。

本例 source 与 buffers 都是静态数组，在整个队列生命期内存在。它们不是 Linux 固定注册缓冲，也没有 AiriOS 的注册 handle；自制接口到 pop 才解除 BUSY 是那套接口的约定，这里需要应用自己保证数组在相关完成之前不失效或提前复用。

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

## 分别处理提交进度与取得完成记录

`get_sqe` 取得可填写的 SQ 项，`prep_read` 写入 FD、缓冲地址、长度和偏移，`set_data64` 加入 cookie。填好描述还没有读取结果；`io_uring_submit` 才推进提交，其正返回值表示本次交出的项数，不是字节数，更不是五项已经完成。

例如本次若只提交三项，`submit_count(5)` 累计到 3，再继续交剩余项，不重建已经交出的请求。这个数量只是解释分支，实际由本轮内核返回。EINTR 可在同一两秒总预算内重试，预算不能每次重置；零进度或截止前未提交齐都明确失败，不能无限空转。

`wait_one` 解决另一件事：最多等两秒，取得一条可读的 CQE。等待函数返回 0 表示取得记录，记录中的 res 才是具体 I/O 结果；等待超时是失败，不能当成一次读返回零字节。 这里的两秒属于这一次取得 CQE 的等待；收五条时会分别调用 wait_one，不能把它读成整个批次共用一个两秒截止。

liburing 函数的负返回值本身就是负错误码。为了使用 perror，代码将它转换成 `errno=-rc`，不能借用其他调用留下的线程 errno。随后从 CQE 读到的负 res 也代表请求错误，不会另外通过线程 errno 发布。

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

main 先建环。ENOSYS、EPERM 或 EACCES 表示内核不支持或策略拒绝，程序说明不可用并返回 77，不会因此更改策略。其他初始化错误也停止。文件写入要处理短写，只把尚未写入的后缀继续交给 write；全部 1536 字节形成以后才准备读取，比较才有确定的基线。

循环下标 i 同时选择 buffers[i] 与 cookie100+i，长度、偏移和无效 FD 分支按上表形成五份描述。`io_uring_get_sqe` 返回空指针时不能继续写请求。填写后调用 submit_count，成功只说明这批提交已经交齐，所有数组仍要保留，结果处理接在下一块中。main 在下一块结束以前尚未闭合，中途不编译。

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

假设下一条 CQE 带回 cookie103。程序先核对它在 100–104 范围内，再用 `103−100=3` 找到请求下标；seen 位集合拒绝重复身份，避免把同一请求算两次而漏掉另一条。第四项应得到 res=6，因而只比较 buffers[3] 前六字节与 source 从偏移 1530 开始的六字节。请求长度是 16，不代表后十字节也有本次读取结果。

前三项应各为 512，第五项为 −EBADF。只有正结果才有相应有效前缀可比较；本批读请求返回 0 意味着 EOF，负数是请求错误。完成顺序由实际内核决定，可以恰好与提交相同，程序不把强制乱序作为成功条件，也不保证将来始终同序。

程序复制 cookie 与 result、核对内容以后，才用 `cqe_seen` 归还这个 CQ 槽。此后不能继续借那个 CQE 指针读取旧记录；缓冲数组却是另一份应用持有的内存，cqe_seen 不替应用释放它。数组跨整个批次存活，避免把循环中的临时地址交出去后提前离开作用域。

五条读取收完后，另交一条 cookie900 的 FSYNC，等待 res=0 再退出队列并关闭 FD。这里 0 表示同步成功，不能套用“读的 0 是 EOF”。它展示文件同步也有完成点，没有做目录持久化或物理断电实验。初始文件可能仍在页缓存，本批不使用 O_DIRECT，也不是裸盘吞吐测试。

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

本例的 `require` 在检查不符时仍用 perror 打印标签。提交和等待函数的负 rc 已先转换为 errno；但 CQ 的 res 不会自动写 errno，所以“CQ exact result”或“fsync CQ”失败后附带的系统错误文字可能是旧值，甚至为 Success。这里非零退出能证明观察不符，不能据该尾部文字判断请求错误；正常完成记录也在比较通过后才打印，失败项的实际 cookie/res 不一定已保存。这是当前错误诊断的限制，不能把它说成已完整记录了每条意外完成。

<a id="uring-environment"></a>
## 把 Linux 环境、程序结果和观察证据分别留下

liburing 是用户库，安装开发库不保证内核允许 `io_uring_setup`。程序已经为内核能力或权限拒绝保留退出 77；运行脚本检查 cc、pkg-config、strace、timeout 和 liburing，缺少时给出 Debian/Ubuntu 的 liburing-dev、Arch/CachyOS 的 liburing 等包名，不自动安装或修改 sysctl、容器权限、安全策略。

run.sh 用 pkg-config 取得库的编译和链接参数，将内核、编译器、liburing 与 strace 的实际版本写进环境文件，再在三十秒总上限内运行并观察 setup/enter/register。普通 read SQE 不要求注册缓冲，追踪中没有 io_uring_register 并不表示失败。

每次先将 result.json 置为 false，防止旧成功留下；缺工具、初始化拒绝、超时或结果不符都不能改成 true。只有程序核对全部预期 CQ 并出现成功标记，脚本才报告 passed，后续统一执行器再依据取回的实际结果写 verified=true。版本文件、CQ 输出和系统调用观察因此都对应本次运行。

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

原 Linux 执行器已经按阶段选择源码、传到个人主机的独立目录、执行 run.sh，再取回 artifacts。本次只给阶段映射增加 p5-uring，关联刚才写出的 uring.c 与 run.sh；其余传输、超时和失败记录保持。不把编写完本地文件当成已在 Linux 运行。

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

下面命令选择个人 Linux 主机的 p5-uring 独立目录，编译并运行真正的 Linux 分支，再取回版本、提交数量和 CQ 记录：

```sh
./airios linux-lab --phase p5-uring # 编译实际Linux分支并取回SQ/CQ、错误与版本记录。
```

可用环境中应收到 cookie100–104 对应的 `512、512、512、6、−EBADF`，再收到 cookie900 的同步成功 0。URING_SUBMIT 中的数量是提交进度，URING_CQ 中的 result 是各操作结果；最终标记只在这批结果和内容均符合时出现。若连接、依赖、策略或等待失败，沿保存的阶段状态定位，不把预期表格当作已有输出。

2026-10-16 的既有记录中 SSH 返回 255，Linux 分支、liburing 依赖、内核支持和 CQ 运行均未验证。Mac 只编译了拒绝分支、执行返回 77，并通过脚本语法检查；失败记录保存在 `artifacts/linux-p5-uring/remote.json` 和 `connection.log`。本次文字修订没有重新执行远端实验，不更新这份运行结论。

回到 BOSS 的批量读取，关联目标页、长度与错误依靠每条请求自己的身份；部分提交以后还要辨认哪些已经交出、哪些仍未提交。超时、退出或取消不能单独证明内核再不会访问缓冲，回收依据具体接口的完成语义。AiriOS 请求副本将退出后的设备目标与用户页分开，代价是复制和固定容量；Linux 本例则由应用保留普通数组，不能混称固定缓冲注册。

接口依据：[liburing 提交手册](https://github.com/axboe/liburing/blob/master/man/io_uring_submit.3)、[有界 CQ 等待](https://github.com/axboe/liburing/blob/master/man/io_uring_wait_cqe_timeout.3)与[Linux io_uring](https://man7.org/linux/man-pages/man7/io_uring.7.html)。库版本以实际环境文件为准，master 链接用于接口说明，不代表已锁定并运行该版本。
