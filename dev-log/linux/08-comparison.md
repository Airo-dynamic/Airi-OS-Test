# L08 · 同一TCP回显负载，分别在同步、就绪和完成处等待

从K33和L07继续。AiriOS的demo已经贯通用户页、文件和块请求，Linux对照需要固定负载才能比较等待位置。本节用完全相同的64条256字节TCP回显，依次运行同步服务器、epoll服务器和io_uring服务器。每种模式只有一条连接、每轮一条在途消息，不把网络epoll与磁盘读取的耗时放进同一个排名。

<a id="compare-workload"></a>
## 同一个客户先发完再收完，服务器先收完再发完

要比较“在哪等”，先固定不应改变的东西：同一客户、相同消息、同一内容核对和连接数量。否则更快的结果也许只是少传了数据。pair 为每个模式重新创建本机回环连接，端口由Linux分配。两端都使用TCP_NODELAY，避免把小消息合并策略误读成API成本；阻塞socket带两秒收发超时，整个脚本另有三十秒总上限。客户端是fork出的独立进程，关闭服务器端FD，只保留自己的连接；父关闭客户端FD。内核仍调度两个进程，并不是C函数之间直接复制数组。

每条消息第i字节是message+i的低八位，恰好遍历全部256种字节值，服务器每轮校验和32640，64轮总和2088960。客户每轮完整发送256字节，再完整收回256字节并memcmp。TCP是字节流，没有这些消息边界，ordinary、readiness和completion都必须累计实际前缀，不能把一次读到的少量字节直接当完整消息。父最后waitpid确认客户也验证通过。

<a id="compare-wait"></a>
## 三种等待点，应用数据寿命保持一致

服务器收到一条 256 字节消息前，都需要维护当前已完成长度 done。三种实现没有消除这个状态，只是下一次进展来自不同地方。同步 ordinary 在 send/recv 内等待，正数推进偏移，EINTR在原截止内重试，零EOF和超时都是失败。epoll模式把服务器FD改为非阻塞：先尝试实际读写，EAGAIN才epoll_wait；返回就绪后仍需再调用send/recv。每个接收或发送阶段只订阅当前方向，结束即DEL，可写空转不混进结果。

epoll 交回的是“值得再试”的条件；下面则把 buffer+done 和剩余长度先交给内核，等待结果时无需再补一次 recv/send。io_uring 模式为每次剩余前缀准备一个 RECV 或 SEND SQE，数字cookie每次增加。一个请求被接受后才等它自己的CQ；负res是该请求错误，零是提前EOF，正数不超过剩余跨度。cqe_seen后才提交同一消息后缀，父的data数组在全部完成前一直存活。一次只有一个SQE，所以submit必须返回1；更多并发与部分批量提交已经由L07单独解释，本例不借高并发掩盖状态变化。

完成路径的外层循环检查两秒消息预算，单次 CQ 等待另给两秒；后者使用固定跨度，不能把整体误读为严格不超过两秒的总截止。这些秒级上界也不表示正式的取消协议：若超时或错误，程序整体失败退出，由Linux结束这个进程的资源。成功路径则显式取完CQ、拆队列、关闭连接；不能将错误分支改成“提前free数据后继续运行”而仍认为内核再不会使用它。

内容和身份全部核对以后，才解释 elapsed_ns 的范围。计时从 fork 客户之前开始，包含客户调度、系统调用、回环协议栈和观察器开销，不包含建连。三次依次运行，缓存、温度和系统负载可能变化。单连接一在途负载刻意保持简单，无法体现批量或高并发的潜在收益；elapsed_ns只作当前运行记录，不宣布哪种API更快。strace进一步改变耗时，不能把带跟踪的数值当性能结论。

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
        rc=io_uring_wait_cqe_timeout(&ring,&cqe,&limit); if (rc<0) { errno=-rc; require(0,"CQ wait"); } // 超时不释放成功缓冲。
        require(cqe && io_uring_cqe_get_data64(cqe)==cookie,"CQ identity"); int result=cqe->res; // 取回本次请求自己的结果。
        if (result<0) { errno=-result; require(0,"CQ operation"); } // 负res是请求错误。
        require(result>0 && (size_t)result<=size-done,"CQ prefix"); // 提前EOF和越界都不接受。
        io_uring_cqe_seen(&ring,cqe); done+=(size_t)result; // 完成收取后才复用这段缓冲提交后缀。
    }
    require(done==size,"CQ message deadline"); // 完整消息才返回上层。
}
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

LAB_SOURCES = { # 阶段名到本阶段完整源码元组，新增实验不再嵌套条件表达式。
    'p1': ('ordinary.c', 'raw_start.S', 'run.sh'), # 普通入口与原始Linux系统调用。
    'p2': ('memory.c', 'run.sh'), # 匿名映射、驻留与权限。
    'p3': ('process.c', 'run.sh'), # fork/exec与FD继承。
    'p4': ('io.c', 'run.sh'), # 向量、缓存和同步替换。
    'p6': ('compare.c', 'run.sh'), # 相同TCP回显负载的三个等待模型。
    'p5-uring': ('uring.c', 'run.sh'), # liburing提交、身份和完成生命周期。
    'p5-epoll': ('epoll.c', 'run.sh'), # 本机TCP就绪与背压。
    'p5-threads': ('threads.c', 'run.sh'), # 线程队列、原子发布与futex。
}
parser=argparse.ArgumentParser()  # 主机参数解析器提供--host选项与用法提示。
parser.add_argument('--host',default='airi@ssh.airi-dynamic.top')  # 个人Linux主机可以显式替换，不涉及公司机器。
parser.add_argument('--phase', choices=tuple(LAB_SOURCES), default='p1') # 实验来源与远端目录使用同一阶段名。
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
names = LAB_SOURCES[args.phase] # argparse已按字典键限制阶段，取得对应文件名元组。
sources=[ROOT/'labs/linux'/args.phase/name for name in names]  # 列表推导按所选阶段名称构成Path对象。
subprocess.run(['scp','-q','-o','BatchMode=yes','-o','ConnectTimeout=10',  # scp安静且非交互传输，连接上限同SSH。
                *[str(p) for p in sources],f'{args.host}:{remote}/'],check=True,timeout=30)  # 列表内*展开本阶段路径字符串，最后一项是远端目标。
execution=subprocess.run(ssh+[f'cd {shlex.quote(remote)} && sh run.sh'],timeout=120)  # 远端先切实验目录，再执行shell；暂不check，失败也先下载证据。
data=subprocess.check_output(ssh+[f'cd {shlex.quote(remote)} && tar -cf - artifacts'],timeout=30)  # tar从标准输出交回归档bytes，-f -表示不写远端归档文件。
dest.mkdir(parents=True,exist_ok=True)  # 确保证据父目录存在。
with tarfile.open(fileobj=io.BytesIO(data)) as tar:  # 把内存归档作为文件交给tarfile，with结束关闭归档。
    tar.extractall(dest,filter='data')  # data过滤限制危险成员，解包到本轮证据目录。
if execution.returncode:  # 非零远端状态意味着实验失败，即使tar传输成功。
    (dest/'remote.json').write_text(json.dumps({'host':args.host,'directory':remote,'verified':False,'status':'program or observation failed','exit':execution.returncode},indent=2)+'\n') # 例如缺liburing或运行超时不能保留旧成功。
    raise RuntimeError(f'Linux experiment failed; evidence downloaded to {dest}')  # 原始证据已经带回，再报告失败位置。
result=json.loads((dest/'artifacts/result.json').read_text())  # 实际远端程序写出的JSON恢复成字典。
if result.get('passed') is not True:  # get缺键返回None，只有布尔True才接受。
    raise RuntimeError('Remote validation failed')  # 远端记录不满足成功合同。
(dest/'remote.json').write_text(json.dumps({'host':args.host,'directory':remote,'verified':True,'status':'program and requested observations completed'},indent=2)+'\n')  # 记录本次实际host和目录，不把另一主机记录混用。
print(f'Remote evidence downloaded: {dest}')  # 显示本轮证据可查看位置。
```

**增加p6选项。** 默认guest测试与Linux阶段独立。

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
from shell_test import check_shell # 真正用户shell与端到端存储演示。
from storage_test import check_storage_restart # 跨启动恢复与真实QMP节流下的CQ等待。
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
    parser.add_argument('--phase',choices=('p1','p2','p3','p4','p5-threads','p5-epoll','p5-uring','p6'),default='p1') # Linux阶段显式选择，默认保持P1。
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
        if not args.graphics: # 当前用户shell只独占串口，图形终端尚无用户TTY接入。
            check_shell() # 同一核心构建实际执行用户解析、fork/exec/wait和慢设备I/O。
        check_storage_restart('desktop-normal' if args.graphics else 'normal') # 正式日志双启动恢复，另以第三个guest验证慢设备CQ等待。
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
./airios linux-lab --phase p6 # 真实Linux上执行三条相同TCP负载，记录等待路径。
```

2026-10-16 的既有 SSH 连接返回 255，Linux分支编译、依赖和三种服务器运行均未验证，没有实际耗时排名。Mac拒绝分支编译并返回77、Shell语法通过；artifacts/linux-p6/remote.json明确verified=false。连接恢复后重新执行上述命令，只有实际三个COMPARE结果及客户校验都通过，远端状态才变为成功。

回看BOSS/Taishan时，先标出当前路径在哪等：线程在系统调用里阻塞、Reactor等FD可推进，还是批量请求已交内核后等身份对应的完成。再找缓冲区归谁、容量满时谁暂停、错误从哪里返回、退出后谁收拢未完成请求。三条路径可以组合在同一个服务里，不需要给整个项目贴一个唯一“同步/异步”的标签。

依据：[Linux epoll](https://man7.org/linux/man-pages/man7/epoll.7.html)、[liburing接口手册](https://github.com/axboe/liburing/tree/master/man)与[TCP socket选项](https://man7.org/linux/man-pages/man7/tcp.7.html)。本节不要求AiriOS实现TCP，也不以回显实验替代L04的存储持久化边界。
