# L08 · 同一TCP回显负载，分别在同步、就绪和完成处等待

从K33和L07继续。AiriOS的demo已经贯通用户页、文件和块请求，Linux对照需要固定负载才能比较等待位置。本节用完全相同的64条256字节TCP回显，依次运行同步服务器、epoll服务器和io_uring服务器。每种模式只有一条连接、每轮一条在途消息，不把网络epoll与磁盘读取的耗时放进同一个排名。

<a id="compare-workload"></a>
## 同一个客户先发完再收完，服务器先收完再发完

要比较“在哪等”，先固定不应改变的东西：同一客户、相同消息、同一内容核对和连接数量。否则更快的结果也许只是少传了数据。pair 为每个模式重新创建本机回环连接，端口由Linux分配。两端都使用TCP_NODELAY，避免把小消息合并策略误读成API成本；阻塞socket带两秒收发超时，整个脚本另有三十秒总上限。客户端是fork出的独立进程，关闭服务器端FD，只保留自己的连接；父关闭客户端FD。内核仍调度两个进程，并不是C函数之间直接复制数组。

每条消息第i字节是message+i的低八位，恰好遍历全部256种字节值，服务器每轮校验和32640，64轮总和2088960。客户每轮完整发送256字节，再完整收回256字节并memcmp。TCP是字节流，没有这些消息边界，ordinary、readiness和completion都必须累计实际前缀，不能把一次读到的少量字节直接当完整消息。父最后waitpid确认客户也验证通过。

<a id="compare-wait"></a>
## 三种等待点，应用数据寿命保持一致

服务器收到一条 256 字节消息前，都要保存当前已完成长度 done。同步 ordinary 在 send/recv 里等待，正返回才推进 done，EINTR 则在原截止内重试；零 EOF 与超时都让本轮失败。先把同一建连助手和阻塞传输写出，后面的客户也复用 ordinary，所以三种模式不会偷偷换掉发送负载。

**新建 `labs/linux/p6/compare.c`。** 同一个客户与连接助手，三种服务器只改变等待与推进方式。

<!-- write: labs/linux/p6/compare.c -->
```c
#define _GNU_SOURCE // Linux进程、socket和时间接口。
#include <stdio.h> // 保存同一负载三种路径的真实记录。
#if defined(__linux__) && defined(__x86_64__) // Mac不能验证Linux内核实现。
#include <arpa/inet.h> // 私有TCP回环地址。
#include <errno.h> // EINTR/EAGAIN与负CQ错误分开。
#include <fcntl.h> // epoll服务器使用非阻塞。
#include <liburing.h> // 完成模型。
#include <netinet/tcp.h> // 小消息关闭Nagle，避免把延迟策略误读成API成本。
#include <stdlib.h> // 错误出口。
#include <string.h> // 验证回显内容。
#include <sys/epoll.h> // 就绪模型。
#include <sys/socket.h> // 同一TCP传输。
#include <sys/time.h> // 阻塞socket时间上限。
#include <sys/wait.h> // 收取真实客户进程结果。
#include <time.h> // 单调计时。
#include <unistd.h> // fork/close。
enum {MESSAGES=64,BYTES=256}; // 三种服务器完全相同的消息数和内容。
static struct io_uring ring; // 每次uring服务器独有，最后完成后拆除。
static uint64_t sequence; // CQ身份不以返回顺序猜测。
static void require(int yes,const char *what) { // 错误不继续输出成功计时。
    if (!yes) { perror(what); exit(1); } // 子进程同样有明确非零退出。
}
static long long ns(void) { // 只测当前短负载，不称基准排名。
    struct timespec t; require(clock_gettime(CLOCK_MONOTONIC,&t)==0,"clock"); // 单调时钟。
    return (long long)t.tv_sec*1000000000+t.tv_nsec; // 纳秒单位。
}
static void pair(int fd[2]) { // 建连过程对三个服务器完全相同。
    int listener=socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,0); require(listener>=0,"listener"); // 不占固定端口。
    struct sockaddr_in a={.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_LOOPBACK)}; // 只允许本机连接。
    require(!bind(listener,(void *)&a,sizeof a) && !listen(listener,1),"bind/listen"); // 单客户。
    socklen_t length=sizeof a; require(!getsockname(listener,(void *)&a,&length),"port"); // 内核选择真实端口。
    fd[0]=socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,0); require(fd[0]>=0 && !connect(fd[0],(void *)&a,sizeof a),"connect"); // 外层timeout覆盖建连。
    fd[1]=accept4(listener,NULL,NULL,SOCK_CLOEXEC); require(fd[1]>=0,"accept"); close(listener); // 连接与监听独立。
    for (unsigned i=0;i<2;++i) { // 两端相同传输设置。
        int one=1; struct timeval limit={.tv_sec=2}; // 单次阻塞也有上界。
        require(!setsockopt(fd[i],IPPROTO_TCP,TCP_NODELAY,&one,sizeof one),"TCP_NODELAY"); // 禁用小消息合并等待。
        require(!setsockopt(fd[i],SOL_SOCKET,SO_RCVTIMEO,&limit,sizeof limit) && !setsockopt(fd[i],SOL_SOCKET,SO_SNDTIMEO,&limit,sizeof limit),"socket timeouts"); // 失败不无限等。
    }
}
static void ordinary(int fd,unsigned char *buffer,size_t size,int writing) { // 同一个阻塞客户端与同步服务器共用短读写循环。
    size_t done=0; long long end=ns()+2000000000ll; // 总预算不随EINTR重置。
    while (done<size && ns()<end) { // 不假定TCP一次传完整消息。
        ssize_t n=writing?send(fd,buffer+done,size-done,MSG_NOSIGNAL):recv(fd,buffer+done,size-done,0); // 只借剩余前缀。
        if (n<0 && errno==EINTR) continue; // 返回前未取得进度。
        require(n>0,"blocking transfer"); done+=(size_t)n; // 零是提前EOF，负数含超时都失败。
    }
    require(done==size,"blocking deadline"); // 不能把半条消息作为回显成功。
}
```

服务器改用非阻塞 FD 以后，done 仍保存同一条消息的前缀。readiness 先尝试实际 recv/send，遇 EAGAIN 才调用 epoll_wait；返回就绪以后，还要再次尝试传输。下面每个接收或发送阶段只订阅当前方向，完成就 DEL，不让空闲的 EPOLLOUT 反复制造事件。

<!-- append: labs/linux/p6/compare.c -->
```c
static void readiness(int ep,int fd,unsigned char *buffer,size_t size,int writing) { // 相同字节流换成就绪后重试。
    struct epoll_event wanted={.events=writing?EPOLLOUT:EPOLLIN,.data.fd=fd}; // 仅订阅当前确实需要的方向。
    require(!epoll_ctl(ep,EPOLL_CTL_ADD,fd,&wanted),"epoll add"); // 每阶段先增加，结束删除。
    size_t done=0; long long end=ns()+2000000000ll; // 两秒单调总预算。
    while (done<size && ns()<end) { // 完整消息由应用偏移组合。
        ssize_t n=writing?send(fd,buffer+done,size-done,MSG_NOSIGNAL):recv(fd,buffer+done,size-done,0); // 非阻塞实际推进。
        if (n>0) { done+=(size_t)n; continue; } // 正数只消费已完成前缀。
        if (n<0 && errno==EINTR) continue; // 重查同一方向。
        require(n<0 && (errno==EAGAIN || errno==EWOULDBLOCK),"nonblocking transfer"); // 零EOF不当EAGAIN。
        struct epoll_event ready; int count=epoll_wait(ep,&ready,1,100); // 条件未真时才等待。
        require(count>=0 || errno==EINTR,"epoll wait"); // 不把事件当成已经读好的数据。
    }
    require(done==size && !epoll_ctl(ep,EPOLL_CTL_DEL,fd,NULL),"epoll stage complete"); // 空闲时不常驻可写订阅。
}
```

epoll 交回的是“值得再试”的条件；下面则把 buffer+done 和剩余长度先交给内核，等待结果时无需再补一次 recv/send。io_uring 模式为每次剩余前缀准备一个 RECV 或 SEND SQE，数字cookie每次增加。一个请求被接受后才等它自己的CQ；负res是该请求错误，零是提前EOF，正数不超过剩余跨度。cqe_seen后才提交同一消息后缀，父的data数组在全部完成前一直存活。一次只有一个SQE，所以submit必须返回1；更多并发与部分批量提交已经由L07单独解释，本例不借高并发掩盖状态变化。

<!-- append: labs/linux/p6/compare.c -->
```c
static void completion(int fd,unsigned char *buffer,size_t size,int writing) { // 同一消息换成一次具体操作的完成。
    size_t done=0; long long end=ns()+2000000000ll; // 短完成仍在同一个消息预算中。
    while (done<size && ns()<end) { // 一次只有一个请求，便于完整跟踪所有权。
        struct io_uring_sqe *sqe=io_uring_get_sqe(&ring); require(sqe!=NULL,"SQ space"); // 缓冲在函数返回前一直存在。
        uint64_t cookie=++sequence; // 新请求有新身份，不能收到旧结果继续。
        if (writing) io_uring_prep_send(sqe,fd,buffer+done,(unsigned)(size-done),MSG_NOSIGNAL); // 正确保留尚未发后缀。
        else io_uring_prep_recv(sqe,fd,buffer+done,(unsigned)(size-done),0); // 允许短接收，之后重新提交剩余部分。
        io_uring_sqe_set_data64(sqe,cookie); int rc=io_uring_submit(&ring); // 单个SQE仍检查真实接受数量。
        require(rc==1,"one SQE accepted"); // 本教学路径不使用SQPOLL；失败终止整次实验。
        struct io_uring_cqe *cqe=NULL; struct __kernel_timespec limit={.tv_sec=2}; // CQ等待有界。
        rc=io_uring_wait_cqe_timeout(&ring,&cqe,&limit); if (rc<0) { errno=-rc; require(0,"CQ wait"); } // 超时使整次进程失败退出；不能继续复用仍可能在途的缓冲。
        require(cqe && io_uring_cqe_get_data64(cqe)==cookie,"CQ identity"); int result=cqe->res; // 取回本次请求自己的结果。
        if (result<0) { errno=-result; require(0,"CQ operation"); } // 负res是请求错误。
        require(result>0 && (size_t)result<=size-done,"CQ prefix"); // 提前EOF和越界都不接受。
        io_uring_cqe_seen(&ring,cqe); done+=(size_t)result; // 完成收取后才复用这段缓冲提交后缀。
    }
    require(done==size,"CQ message deadline"); // 完整消息才返回上层。
}
```

完成路径的外层循环检查两秒消息预算，单次 CQ 等待另给两秒；后者使用固定跨度，不能把整体误读为严格不超过两秒的总截止。这些秒级上界也不表示正式的取消协议：若超时或错误，程序整体失败退出，由Linux结束这个进程的资源。成功路径则显式取完CQ、拆队列、关闭连接；不能将错误分支改成“提前free数据后继续运行”而仍认为内核再不会使用它。

ordinary 也在循环入口检查预算，但一次阻塞 send/recv 受 socket 自己的两秒超时约束；readiness 的一次 epoll_wait 另有 100 ms 上限。因此这些循环都有失败出口，却都不能据循环条件推导毫秒级严格总截止。整个脚本的三十秒 timeout 是更外层的保护。

内容和身份全部核对以后，才解释 elapsed_ns 的范围。计时从 fork 客户之前开始，包含客户调度、系统调用、回环协议栈和观察器开销，不包含建连。三次依次运行，缓存、温度和系统负载可能变化。单连接一在途负载刻意保持简单，无法体现批量或高并发的潜在收益；elapsed_ns只作当前运行记录，不宣布哪种API更快。strace进一步改变耗时，不能把带跟踪的数值当性能结论。 对逻辑条件的 require 仍使用 perror，尾部错误文字可能是旧 errno；应按具体检查标签与非零状态判断失败，不据这段后缀给数据问题分类。

三个传输函数已经使用相同 buffer/done 模型。最后追加唯一的客户端与 main：每种模式重新建连、fork 客户，父亲先收完再发完，客户先发完再收完；waitpid 和校验和都成立以后才报告本模式的耗时。

<!-- append: labs/linux/p6/compare.c -->
```c
static void client(int fd) { // 三种服务器使用同一阻塞客户、相同每轮一条在途消息。
    unsigned char out[BYTES],in[BYTES]; // 整个往返期间都有效。
    for (unsigned message=0;message<MESSAGES;++message) { // 不把并发数变化混进API比较。
        for (unsigned i=0;i<BYTES;++i) out[i]=(unsigned char)(message+i); // 内容可确定核对。
        ordinary(fd,out,BYTES,1); ordinary(fd,in,BYTES,0); // 发送全部后收回全部。
        require(!memcmp(out,in,BYTES),"echo bytes"); // 真实内容一致才继续下一条。
    }
    close(fd); _exit(0); // 子进程不刷新复制来的父stdio缓冲。
}
int main(void) { // 一次程序固定三种模式和相同负载。
    for (unsigned mode=0;mode<3;++mode) { // 0同步、1epoll、2io_uring。
        int fd[2]; pair(fd); int ep=-1; // 每种模式全新连接，避免前一种残留。
        if (mode==1) { int flags=fcntl(fd[1],F_GETFL); require(flags>=0 && !fcntl(fd[1],F_SETFL,flags|O_NONBLOCK),"server nonblock"); ep=epoll_create1(EPOLL_CLOEXEC); require(ep>=0,"epoll create"); } // 仅就绪服务器需要非阻塞。
        if (mode==2) { int rc=io_uring_queue_init(8,&ring,0); if (rc<0) { fprintf(stderr,"io_uring unavailable: %s\n",strerror(-rc)); return 77; } } // 不绕过宿主策略。
        fflush(stdout); long long started=ns(); pid_t child=fork(); require(child>=0,"fork client"); // 时间包含客户调度，不宣称纯内核延迟。
        if (!child) { close(fd[1]); client(fd[0]); } // 独立客户进程只保留自己一端。
        close(fd[0]); unsigned char data[BYTES]; unsigned long checksum=0; // 父服务器拥有连接与消息状态。
        for (unsigned message=0;message<MESSAGES;++message) { // 相同64次完整往返。
            for (int writing=0;writing<2;++writing) { // 先收再发同一数组。
                if (!mode) ordinary(fd[1],data,BYTES,writing); // 同步操作在内核等待。
                else if (mode==1) readiness(ep,fd[1],data,BYTES,writing); // 就绪返回后应用仍读写。
                else completion(fd[1],data,BYTES,writing); // 完成直接给实际字节数。
            }
            for (unsigned i=0;i<BYTES;++i) checksum+=data[i]; // 同一负载必须相同校验和。
        }
        close(fd[1]); int status; require(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0,"client result"); // 客户也验证了全部内容。
        require(checksum==(unsigned long)MESSAGES*32640,"same checksum"); // 每轮256字节完整遍历所有字节值。
        if (ep>=0) close(ep); // 就绪集合只属于本次服务器。
        if (mode==2) io_uring_queue_exit(&ring); // CQ全部收取后才拆资源。
        printf("[COMPARE] mode=%s messages=%d bytes_each=%d checksum=%lu elapsed_ns=%lld\n",mode==0?"sync":mode==1?"epoll":"uring",MESSAGES,BYTES,checksum,ns()-started); // 只报告实际短负载耗时。
    }
    puts("[LINUX_COMPARE_OK] one_connection one_message_in_flight no_performance_ranking"); return 0; // 不把三次数值当优劣结论。
}
#else // 非Linux不链接不存在的内核能力。
int main(void) { fputs("x86-64 Linux and liburing required for comparison.\n",stderr); return 77; } // 明确未验证。
#endif // 平台边界。
```

<a id="compare-run"></a>
## 保存环境和失败，连接恢复后再补运行证据

三份结果必须来自实际 Linux，不能拿 guest 的完成队列替代最后一种服务器。本节和 L07 使用相同 liburing 开发依赖，脚本记录内核、编译器、liburing和strace版本。strace -f跟踪客户与服务器，源码send/recv在日志中通常呈现sendto/recvfrom。可在实际Linux上对不带strace的程序另用perf stat观察task-clock、context-switches、cpu-migrations和page-faults；权限失败保留原错误，不自动更改perf策略。此处没有取得perf证据，也没有用猜测值补表。

**新建运行脚本。** 三种模式都实际核对后才发布LINUX_COMPARE_OK。

<!-- write: labs/linux/p6/run.sh -->
```sh
#!/bin/sh
set -eu # 任一步失败都不发布成功。
mkdir -p artifacts # 即使缺环境也保存明确状态。
printf '{"passed":false,"phase":"p6"}\n' > artifacts/result.json # 撤销旧成功记录。
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
cc -std=c11 -O0 -g -Wall -Wextra -Werror compare.c $(pkg-config --cflags --libs liburing) -o compare # 固定工具给出库参数，Shell展开为编译参数。
if timeout -k 2 30 strace -f -o artifacts/compare-syscalls.log -e trace=io_uring_setup,io_uring_enter,epoll_wait,epoll_ctl,sendto,recvfrom,clone ./compare > artifacts/compare.log 2>&1; then # 三十秒总预算。
    cat artifacts/compare.log # 实际返回与cookie顺序。
else # 依赖策略、超时和完成错误都记录失败。
    status=$?; cat artifacts/compare.log >&2; exit "$status" # 不丢失真实退出码。
fi # 完成实际观察。
grep -q '^\[LINUX_COMPARE_OK\]' artifacts/compare.log # 所有预期CQ都被核对才成功。
printf '{"passed":true,"phase":"p6","one_connection":true}\n' > artifacts/result.json # 相同TCP负载，不发布性能排名。
```

**增加p6源码映射。** 远端仍用AiriOS-labs/p6，失败和其他阶段分开保存。

在阶段字典中加入 p6，对应 compare.c 与 run.sh。

<!-- replace: scripts/linux_lab.py -->
<!-- before -->
```python
    'p2': ('memory.c', 'run.sh'), # 匿名映射、驻留与权限。
    'p3': ('process.c', 'run.sh'), # fork/exec与FD继承。
    'p4': ('io.c', 'run.sh'), # 向量、缓存和同步替换。
    'p5-uring': ('uring.c', 'run.sh'), # liburing提交、身份和完成生命周期。
    'p5-epoll': ('epoll.c', 'run.sh'), # 本机TCP就绪与背压。
    'p5-threads': ('threads.c', 'run.sh'), # 线程队列、原子发布与futex。
```
<!-- after -->
```python
    'p2': ('memory.c', 'run.sh'), # 匿名映射、驻留与权限。
    'p3': ('process.c', 'run.sh'), # fork/exec与FD继承。
    'p4': ('io.c', 'run.sh'), # 向量、缓存和同步替换。
    'p6': ('compare.c', 'run.sh'), # 相同TCP回显负载的三个等待模型。
    'p5-uring': ('uring.c', 'run.sh'), # liburing提交、身份和完成生命周期。
    'p5-epoll': ('epoll.c', 'run.sh'), # 本机TCP就绪与背压。
    'p5-threads': ('threads.c', 'run.sh'), # 线程队列、原子发布与futex。
```

**增加p6选项。** 默认guest测试与Linux阶段独立。

统一入口接受 p6，不改变默认 guest 测试。

<!-- replace: scripts/manage.py -->
<!-- before -->
```python
    parser.add_argument('--app',choices=('terminal','files','editor','tasks','system','settings','viewer'),default='editor')  # 主机窗口默认Editor，只接受已知应用名。
    parser.add_argument('--state',choices=['normal','empty','loading','error','offline'],default='normal')  # 状态名限定到共享视图已有的五种状态。
    parser.add_argument('--profile',choices=PROFILES,default='normal')  # 镜像默认普通配置，只接受已发布配置。
    parser.add_argument('--phase',choices=('p1','p2','p3','p4','p5-threads','p5-epoll','p5-uring'),default='p1') # Linux阶段显式选择，默认保持P1。
    parser.add_argument('--host',default='airi@ssh.airi-dynamic.top')   # 个人Linux主机可以显式替换，不涉及公司机器。
    parser.add_argument('--graphics', action='store_true', help='explicitly enable the optional desktop')  # 默认 False，不读取图形目录。
    args=parser.parse_args()      # 把进程参数转为Namespace，后续通过args.action等属性读取。
```
<!-- after -->
```python
    parser.add_argument('--app',choices=('terminal','files','editor','tasks','system','settings','viewer'),default='editor')  # 主机窗口默认Editor，只接受已知应用名。
    parser.add_argument('--state',choices=['normal','empty','loading','error','offline'],default='normal')  # 状态名限定到共享视图已有的五种状态。
    parser.add_argument('--profile',choices=PROFILES,default='normal')  # 镜像默认普通配置，只接受已发布配置。
    parser.add_argument('--phase',choices=('p1','p2','p3','p4','p5-threads','p5-epoll','p5-uring','p6'),default='p1') # Linux阶段显式选择，默认保持P1。
    parser.add_argument('--host',default='airi@ssh.airi-dynamic.top')   # 个人Linux主机可以显式替换，不涉及公司机器。
    parser.add_argument('--graphics', action='store_true', help='explicitly enable the optional desktop')  # 默认 False，不读取图形目录。
    args=parser.parse_args()      # 把进程参数转为Namespace，后续通过args.action等属性读取。
```


```sh
./airios linux-lab --phase p6 # 真实Linux上执行三条相同TCP负载，记录等待路径。
```

2026-10-16 的既有 SSH 连接返回 255，Linux分支编译、依赖和三种服务器运行均未验证，没有实际耗时排名。Mac拒绝分支编译并返回77、Shell语法通过；artifacts/linux-p6/remote.json明确verified=false。连接恢复后重新执行上述命令，只有实际三个COMPARE结果及客户校验都通过，远端状态才变为成功。

回看BOSS/Taishan时，先标出当前路径在哪等：线程在系统调用里阻塞、Reactor等FD可推进，还是批量请求已交内核后等身份对应的完成。再找缓冲区归谁、容量满时谁暂停、错误从哪里返回、退出后谁收拢未完成请求。三条路径可以组合在同一个服务里，不需要给整个项目贴一个唯一“同步/异步”的标签。

依据：[Linux epoll](https://man7.org/linux/man-pages/man7/epoll.7.html)、[liburing接口手册](https://github.com/axboe/liburing/tree/master/man)与[TCP socket选项](https://man7.org/linux/man-pages/man7/tcp.7.html)。本节不要求AiriOS实现TCP，也不以回显实验替代L04的存储持久化边界。
