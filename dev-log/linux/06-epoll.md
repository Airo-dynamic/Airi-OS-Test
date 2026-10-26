# L06 · epoll返回的是当前可推进的连接，数据仍要自己读写

从L05继续，guest机制对应K28–K32。沿你写过的 Reactor 继续往下看：收到可读事件以后，应用为什么还要调用 recv，又为什么必须保存它实际返回的数量？本章用一个真实 TCP 连接把这两步分开。AiriOS管道的ready返回条件，异步块请求的CQ返回结果；Linux epoll属于前者。一个EPOLLIN事件既不携带读出的字节，也不保证之后任意长度的recv都能完成。

<a id="epoll-pair"></a>
## 两个连接端，各自有接收缓冲区和发送进度

发送的 abc 首先进入对端的内核接收缓冲，epoll 只帮助找出现在值得处理的连接，并不替应用把 abc 搬进数组。先准备这两个端点：pair 在 127.0.0.1 上 bind 端口 0，Linux分配空闲端口；getsockname取回，再connect和accept。这里建连用阻塞调用，外层timeout限制整个实验；建立后保留原fcntl标志并增加O_NONBLOCK，正式事件循环不能在recv/send里等另一端。程序不监听外网，不占固定端口，也不把建连辅助函数宣传为完整非阻塞服务器。

epoll_create1建立一个内核兴趣集合，epoll_ctl ADD把连接FD和关心的条件放入其中。data.fd是我们随注册保存的整数身份，epoll_wait返回它，调用者据此定位连接状态；这里没有把局部C指针交给网络另一端。返回的就绪位只说明现在值得尝试，真正进度以recv/send返回值为准。

**新建 `labs/linux/p5-epoll/epoll.c`。** 先写连接和注册助手，再连续写完三个场景；它们不依赖guest网络驱动。

<!-- write: labs/linux/p5-epoll/epoll.c -->
```c
#define _GNU_SOURCE // 暴露Linux的epoll与非阻塞socket扩展。
#include <stdio.h> // 主机输出用于保存真实观察。
#if defined(__linux__) && defined(__x86_64__) // 与个人Linux实验目标一致。
#include <arpa/inet.h> // loopback地址和端口字节序。
#include <errno.h> // EAGAIN/EINTR来自当前失败调用。
#include <fcntl.h> // 把已建立的连接改为非阻塞。
#include <poll.h> // 仅用于等待初始三个字节到达。
#include <stdlib.h> // exit与错误出口。
#include <string.h> // 初始化待发字节。
#include <sys/epoll.h> // Linux就绪集合。
#include <sys/ioctl.h> // FIONREAD观察当前接收字节数。
#include <sys/socket.h> // 本机TCP连接。
#include <time.h> // 单调截止时间不受时钟校准影响。
#include <unistd.h> // close/read。
static void require(int yes, const char *what) { // 条件失败立即停止，不继续制造成功标记。
    if (!yes) { perror(what); exit(1); } // 同时保留最近系统错误，逻辑断言另有明确名称。
}
static long long now_ms(void) { // 为所有循环提供总预算。
    struct timespec t; require(clock_gettime(CLOCK_MONOTONIC, &t) == 0, "clock"); // 查询实际单调时钟。
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000; // 纳秒换成毫秒。
}
static void pair(int fd[2]) { // 私有TCP连接，不占固定端口，不访问外网。
    int listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0); // 仅监听当前实验。
    require(listener >= 0, "listener"); // 缺资源立即失败。
    struct sockaddr_in a = {.sin_family=AF_INET, .sin_addr.s_addr=htonl(INADDR_LOOPBACK)}; // port零由内核分配。
    require(bind(listener, (void *)&a, sizeof a) == 0 && listen(listener, 1) == 0, "bind/listen"); // 接受单一连接。
    socklen_t len = sizeof a; require(getsockname(listener, (void *)&a, &len) == 0, "getsockname"); // 取得实际端口。
    fd[0] = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0); // 发送端先阻塞建立本机连接。
    require(fd[0] >= 0 && connect(fd[0], (void *)&a, sizeof a) == 0, "connect"); // 外层timeout保护整个实验。
    fd[1] = accept4(listener, NULL, NULL, SOCK_CLOEXEC); require(fd[1] >= 0, "accept"); // 教学建连不混进后面的reactor。
    close(listener); // 已连接FD不依赖监听FD继续存活。
    for (int i=0; i<2; ++i) { // 两端后续都不能阻塞事件循环。
        int flags=fcntl(fd[i], F_GETFL); require(flags>=0 && fcntl(fd[i],F_SETFL,flags|O_NONBLOCK)==0,"nonblock"); // 保留原标志。
    }
}
static void add(int ep, int fd, unsigned flags) { // data.fd让返回事件关联原连接。
    struct epoll_event e={.events=flags,.data.fd=fd}; // 不保存临时栈指针。
    require(epoll_ctl(ep,EPOLL_CTL_ADD,fd,&e)==0,"epoll add"); // 失败不能假装已经订阅。
}
static int events(int ep, struct epoll_event *e, int ms) { // 返回实际就绪数量。
    int n=epoll_wait(ep,e,2,ms); // 最多两个连接事件。
    require(n>=0 || errno==EINTR,"epoll wait"); return n<0?0:n; // EINTR交回外层总截止重试。
}
```

<a id="epoll-lt-et"></a>
## abc只读a：LT再次返回，ET需要把bc主动读尽

要区分两种通知方式，先让第一次事件之后的状态保持可辨认：abc 已全到达，应用只读走 a，还剩 bc。trigger 为两种模式分别建立全新连接，发送 abc。FIONREAD只是观察内核当前接收字节数，不消费它；先等三个字节全部到达，避免后续TCP分段到达被误认为相同状态的重复边沿。第一次epoll_wait后recv一个字节a，接收缓冲仍有bc，发送端不再添加数据。

LT默认按仍成立的条件通知，第二次wait应继续有事件。ET加EPOLLET，只读a后没有新的数据到达，第二次短wait不返回新事件；bc仍在内核，不会因为没有通知而消失。作者随后读取bc，再读一次得到EAGAIN，完成正确的ET排空步骤。非阻塞与“读到EAGAIN”相配合：recv零是EOF，负一且errno=EAGAIN是暂时没有数据，不能混成同一种结束。

<!-- append: labs/linux/p5-epoll/epoll.c -->
```c
static void trigger(int edge) { // 同样三个字节，比较LT与ET未读尽的结果。
    int fd[2]; pair(fd); int ep=epoll_create1(EPOLL_CLOEXEC); require(ep>=0,"epoll create"); // 每个模式新建连接。
    add(ep,fd[1],EPOLLIN | (edge?EPOLLET:0)); // ET需要调用者主动排空。
    require(send(fd[0],"abc",3,MSG_NOSIGNAL)==3,"send abc"); // 不在后面追加新数据。
    int available=0; long long end=now_ms()+2000; // 先保证三个字节全部到达。
    while (available!=3 && now_ms()<end) { // 避免第二个TCP包被误当ET重复通知。
        require(ioctl(fd[1],FIONREAD,&available)==0,"FIONREAD"); // 只观察，不消费。
        if (available!=3) { struct pollfd p={fd[1],POLLIN,0}; require(poll(&p,1,10)>=0,"poll arrival"); } // 到达后再做正式比较。
    }
    require(available==3,"three bytes arrived"); struct epoll_event e[2]; // 后续无新数据来源。
    require(events(ep,e,1000)==1,"first readiness"); char bytes[4]={0}; // 第一次必须有可读事件。
    require(recv(fd[1],bytes,1,0)==1 && bytes[0]=='a',"read one"); // 故意留下bc。
    int again=events(ep,e,50); require(again==(edge?0:1),"LT repeats / ET needs drain"); // LT条件仍真，ET没有新边沿。
    require(recv(fd[1],bytes,3,0)==2 && bytes[0]=='b' && bytes[1]=='c',"drain bc"); // 作者现在把遗漏数据取完。
    require(recv(fd[1],bytes,1,0)<0 && (errno==EAGAIN || errno==EWOULDBLOCK),"drain to EAGAIN"); // ET停止读取的可靠位置。
    printf("[EPOLL_%s_OK] remaining_before_drain=2 second_events=%d drained_to=EAGAIN\n",edge?"ET":"LT",again); // 记录真实返回值。
    close(ep); close(fd[0]); close(fd[1]); // 取消订阅并回收全部连接。
}
```

<a id="epoll-pressure"></a>
## send短写和EAGAIN把未发后缀留在应用

前面的未读 bc 留在内核，发送端还会遇到另一种剩余：send 只接受前缀时，后缀仍属于应用。pressure 把发送缓冲区请求调小，先连续发送，直到真实EAGAIN才转去消费接收端。SO_SNDBUF不是精确容量承诺，Linux可以调整请求值；程序记录实际send接受的字节数，不能预设第几次一定写满。sent只增加返回的正数，短写后下次继续同一消息的未发后缀；本例内容都是x，所以每个后缀值相同，实际业务还需保存对应缓冲区和偏移。

收到 EPOLLOUT 后也只再次尝试发送保存的后缀；如果已经没有待发数据，继续订阅会反复报告一个无需处理的条件。EPOLLOUT 只在还有待发数据时订阅。全部262144字节被接受后DEL发送端注册，并shutdown(SHUT_WR)，FIN排在已有字节之后。接收端看到RDHUP仍继续recv，直到真正返回零才完成；如果立刻close，会丢掉尚未消费的尾部。每轮把接收端读到EAGAIN，使空间释放后发送端能够再次推进。十秒单调总截止和外层三十秒timeout共同限制失败路径。

程序要求实际出现至少一次EAGAIN，并核对全部内容、发送/接收总数和EOF。short_send可以为0，不能为了展示短写而编造一次正返回。单连接LT循环用于解释状态，不是生产公平调度器；多连接服务还要限制每轮配额、管理输出队列上限，防止一条连接占满内存或饿死其他连接。

前两次连接各自完成 abc 的比较，压力观察又创建新连接，不继承先前未读的 bc。下面追加 pressure 和 main，写完再编译；sent/received 在同一条压力连接中累计真实前缀。

<!-- append: labs/linux/p5-epoll/epoll.c -->
```c
static void pressure(void) { // 有界发送队列遇到背压后交还事件循环。
    int fd[2]; pair(fd); int small=8192; // 内核可能把请求缓冲区大小加倍，不能当精确容量。
    require(setsockopt(fd[0],SOL_SOCKET,SO_SNDBUF,&small,sizeof small)==0,"SO_SNDBUF"); // 鼓励出现实际EAGAIN。
    int ep=epoll_create1(EPOLL_CLOEXEC); require(ep>=0,"pressure epoll"); // 单循环管理发送和接收。
    add(ep,fd[0],EPOLLOUT); add(ep,fd[1],EPOLLIN|EPOLLRDHUP); // 只有尚有待发数据时订阅可写。
    enum {TOTAL=262144}; char data[4096]; memset(data,'x',sizeof data); // 相同字节便于逐块核对。
    size_t sent=0,received=0; unsigned blocked=0,shorts=0; int shut=0,eof=0; // 偏移和半关闭各有独立状态。
    long long end=now_ms()+10000; // 即使丢失推进条件也不能无限运行。
    while (!eof && now_ms()<end) { // 每轮先尝试发送，满后立刻处理接收端。
        while (sent<TOTAL) { // 不把一次send当成完整消息。
            size_t want=TOTAL-sent; if (want>sizeof data) want=sizeof data; // 限制本次借用范围。
            ssize_t n=send(fd[0],data,want,MSG_NOSIGNAL); // 数据在调用返回前保持有效。
            if (n>0) { sent+=(size_t)n; shorts+=(size_t)n<want; continue; } // 仅推进已接受前缀。
            if (n<0 && errno==EINTR) continue; // 被信号打断尚无进度可重试。
            require(n<0 && (errno==EAGAIN || errno==EWOULDBLOCK),"send pressure"); ++blocked; break; // 满了交回reactor。
        }
        if (sent==TOTAL && !shut) { // 没有用户待发数据就停止订阅可写，防止LT忙转。
            require(epoll_ctl(ep,EPOLL_CTL_DEL,fd[0],NULL)==0,"remove EPOLLOUT"); // 关闭注册而非关闭连接。
            require(shutdown(fd[0],SHUT_WR)==0,"half close"); shut=1; // FIN排在已发送字节之后，接收仍须排空。
        }
        for (;;) { // 接收端始终读到EAGAIN或真正EOF。
            ssize_t n=recv(fd[1],data,sizeof data,0); // RDHUP本身不说明没有尾部数据。
            if (n>0) { for (ssize_t i=0;i<n;++i) require(data[i]=='x',"payload"); received+=(size_t)n; continue; } // 核对实际内容。
            if (!n) { eof=1; break; } // 只有recv零才消费到字节流结尾。
            if (errno==EINTR) continue; // 重新读取当前状态。
            require(errno==EAGAIN || errno==EWOULDBLOCK,"recv pressure"); break; // 当前排空，等新就绪。
        }
        if (!eof) { struct epoll_event e[2]; (void)events(ep,e,100); } // LT会再次报告仍可推进的一端。
    }
    require(eof && sent==TOTAL && received==TOTAL && blocked>0,"pressure finished with real EAGAIN"); // 不把超时或零背压冒充演示。
    printf("[EPOLL_PRESSURE_OK] sent=%zu received=%zu EAGAIN=%u short_send=%u EOF=%d\n",sent,received,blocked,shorts,eof); // 短写次数允许为零。
    close(ep); close(fd[0]); close(fd[1]); // 资源不留给下一实验。
}
int main(void) { trigger(0); trigger(1); pressure(); puts("[LINUX_EPOLL_OK]"); return 0; } // 三种真实行为都通过才成功。
#else // Mac不能提供Linux epoll语义。
int main(void) { fputs("x86-64 Linux is required for epoll.\n",stderr); return 77; } // 未验证出口。
#endif // Linux实现结束。
```

<a id="epoll-observe"></a>
## 观察真实调用，失败不能只保留预期输出

完整收发结果由程序核对，系统调用边界再由跟踪记录；两者来自同一 Linux 运行。run.sh 记录内核、编译器和 strace 版本，跟踪epoll注册/等待、sendto/recvfrom与shutdown。libc的send/recv通常对应这些系统调用名称，源码函数名不必与strace显示名相同。strace改变调度与缓冲区时序，输出用于机制观察，不用于吞吐排名。

本例 require 用 perror 打印检查标签。系统调用失败时 errno 有对应含义；字节数或逻辑条件不符时，后缀却可能来自旧 errno，不能把它当成这次数据错误的分类。程序仍会非零结束，检查标签指出实际未满足的条件；本章没有改造这套诊断接口。

**新建运行脚本。** 缺编译器、strace或timeout时打印具体依赖修复方法，result保持false。

<!-- write: labs/linux/p5-epoll/run.sh -->
```sh
#!/bin/sh
set -eu # 未定义变量或命令失败都停止。
if [ "$(uname -s)" != Linux ] || [ "$(uname -m)" != x86_64 ]; then # 固定个人x86-64 Linux观察路径。
    echo 'x86-64 Linux is required for epoll/TCP observation.' >&2 # 不用Mac模拟Linux。
    exit 77 # 未验证。
fi # 结束环境检查。
mkdir -p artifacts # 本阶段独立证据目录。
printf '{"passed":false,"phase":"p5-epoll"}\n' > artifacts/result.json # 先撤销旧成功。
for required in cc strace timeout; do # 核心编译、观察和运行上界缺一不可。
    if ! command -v "$required" >/dev/null 2>&1; then # 先给具体修复方法，不运行半个实验。
        echo "Missing $required. Debian/Ubuntu: apt install build-essential strace coreutils; Arch/CachyOS: pacman -S base-devel strace coreutils." >&2 # 这里只提示，不擅自提权安装。
        exit 77 # 依赖未满足，不能写passed。
    fi # 当前工具存在才继续。
done # 所需工具都已找到。
uname -a > artifacts/environment.txt # 实际Linux与架构。
cc --version >> artifacts/environment.txt # 实际C11编译器。
strace --version >> artifacts/environment.txt # 跟踪器必须真实存在。
timeout --version >> artifacts/environment.txt # GNU coreutils总运行上界，缺工具不能无限等待。
getconf GNU_LIBC_VERSION >> artifacts/environment.txt 2>&1 || true # 非glibc系统可以缺此项，其他环境事实仍保存。
cc -std=c11 -O0 -g -Wall -Wextra -Werror epoll.c -o epoll # 编译真实Linux epoll路径。
if timeout -k 2 30 strace -f -o artifacts/epoll-syscalls.log -e trace=epoll_create1,epoll_ctl,epoll_wait,sendto,recvfrom,shutdown ./epoll > artifacts/epoll.log 2>&1; then # 跟踪本机TCP和就绪调用，整个观察最多三十秒。
    cat artifacts/epoll.log # 完成后回显真实输出。
else # 超时、程序错误和工具失败都保留证据。
    status=$? # 不被后续cat覆盖。
    cat artifacts/epoll.log >&2 # 具体失败原因。
    exit "$status" # 不发布passed。
fi # 程序正常退出。
grep -q '^\[LINUX_EPOLL_OK\]' artifacts/epoll.log # 不能仅以strace进程存在当成功。
printf '{"passed":true,"phase":"p5-epoll","airios_network_stack_implemented":false}\n' > artifacts/result.json # Linux观察不冒充AiriOS网络栈。
```

**更新阶段字典，加入p5-epoll。** 其他阶段保留相同传输与失败记录语义。

在已有阶段字典中加入 p5-epoll，继续使用原传输和失败记录。

<!-- replace: scripts/linux_lab.py -->
<!-- before -->
```python
    'p2': ('memory.c', 'run.sh'), # 匿名映射、驻留与权限。
    'p3': ('process.c', 'run.sh'), # fork/exec与FD继承。
    'p4': ('io.c', 'run.sh'), # 向量、缓存和同步替换。
    'p5-threads': ('threads.c', 'run.sh'), # 线程队列、原子发布与futex。
}
parser=argparse.ArgumentParser()  # 主机参数解析器提供--host选项与用法提示。
```
<!-- after -->
```python
    'p2': ('memory.c', 'run.sh'), # 匿名映射、驻留与权限。
    'p3': ('process.c', 'run.sh'), # fork/exec与FD继承。
    'p4': ('io.c', 'run.sh'), # 向量、缓存和同步替换。
    'p5-epoll': ('epoll.c', 'run.sh'), # 本机TCP就绪与背压。
    'p5-threads': ('threads.c', 'run.sh'), # 线程队列、原子发布与futex。
}
parser=argparse.ArgumentParser()  # 主机参数解析器提供--host选项与用法提示。
```

**更新入口的phase选项。** guest构建不依赖这个Linux实验。

为统一入口加入同名 phase。

<!-- replace: scripts/manage.py -->
<!-- before -->
```python
    parser.add_argument('--app',choices=('terminal','files','editor','tasks','system','settings','viewer'),default='editor')  # 主机窗口默认Editor，只接受已知应用名。
    parser.add_argument('--state',choices=['normal','empty','loading','error','offline'],default='normal')  # 状态名限定到共享视图已有的五种状态。
    parser.add_argument('--profile',choices=PROFILES,default='normal')  # 镜像默认普通配置，只接受已发布配置。
    parser.add_argument('--phase',choices=('p1','p2','p3','p4','p5-threads'),default='p1') # Linux阶段显式选择，默认保持P1。
    parser.add_argument('--host',default='airi@ssh.airi-dynamic.top')   # 个人Linux主机可以显式替换，不涉及公司机器。
    parser.add_argument('--graphics', action='store_true', help='explicitly enable the optional desktop')  # 默认 False，不读取图形目录。
    args=parser.parse_args()      # 把进程参数转为Namespace，后续通过args.action等属性读取。
```
<!-- after -->
```python
    parser.add_argument('--app',choices=('terminal','files','editor','tasks','system','settings','viewer'),default='editor')  # 主机窗口默认Editor，只接受已知应用名。
    parser.add_argument('--state',choices=['normal','empty','loading','error','offline'],default='normal')  # 状态名限定到共享视图已有的五种状态。
    parser.add_argument('--profile',choices=PROFILES,default='normal')  # 镜像默认普通配置，只接受已发布配置。
    parser.add_argument('--phase',choices=('p1','p2','p3','p4','p5-threads','p5-epoll'),default='p1') # Linux阶段显式选择，默认保持P1。
    parser.add_argument('--host',default='airi@ssh.airi-dynamic.top')   # 个人Linux主机可以显式替换，不涉及公司机器。
    parser.add_argument('--graphics', action='store_true', help='explicitly enable the optional desktop')  # 默认 False，不读取图形目录。
    args=parser.parse_args()      # 把进程参数转为Namespace，后续通过args.action等属性读取。
```


```sh
./airios linux-lab --phase p5-epoll # 在个人主机建立本机TCP连接并保存系统调用证据。
```

2026-10-16 的既有 SSH 连接返回 255，Linux分支尚未编译、运行和跟踪。Mac拒绝分支可编译且运行返回77，sh -n通过；这些不能证明Linux路径通过。真实失败记录在artifacts/linux-p5-epoll/remote.json与connection.log。连接可用后应出现LT second_events=1、ET second_events=0，以及字节计数相等且EAGAIN大于零的压力结果，次数以实际环境为准。

回到旧网络库时，沿“应用输出队列 → send接受前缀 → 内核socket缓冲 → 对端消费”看背压。等epoll可写不能替代应用保存剩余字节；而AiriOS CQ的result已经对应一次已接受的请求，不需要收到CQ后再补一次实际设备读。AiriOS目前没有socket或TCP/IP，这部分由Linux实测补足。

依据：[Linux epoll手册](https://man7.org/linux/man-pages/man7/epoll.7.html)、[send](https://man7.org/linux/man-pages/man2/send.2.html)、[recv](https://man7.org/linux/man-pages/man2/recv.2.html)与[shutdown](https://man7.org/linux/man-pages/man2/shutdown.2.html)。
