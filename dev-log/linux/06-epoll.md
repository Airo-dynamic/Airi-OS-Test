# L06 · epoll返回的是当前可推进的连接，数据仍要自己读写

从L05继续，guest机制对应K28–K32。沿你写过的 Reactor 继续往下看：收到可读事件以后，应用为什么还要调用 recv，又为什么必须保存它实际返回的数量？本章用一个真实 TCP 连接把这两步分开。AiriOS管道的ready返回条件，异步块请求的CQ返回结果；Linux epoll属于前者。一个EPOLLIN事件既不携带读出的字节，也不保证之后任意长度的recv都能完成。

<a id="epoll-pair"></a>
## 两个连接端，各自有接收缓冲区和发送进度

发送的 abc 首先进入对端的内核接收缓冲，epoll 只帮助找出现在值得处理的连接，并不替应用把 abc 搬进数组。先准备这两个端点：pair 在 127.0.0.1 上 bind 端口 0，Linux分配空闲端口；getsockname取回，再connect和accept。这里建连用阻塞调用，外层timeout限制整个实验；建立后保留原fcntl标志并增加O_NONBLOCK，正式事件循环不能在recv/send里等另一端。程序不监听外网，不占固定端口，也不把建连辅助函数宣传为完整非阻塞服务器。

epoll_create1建立一个内核兴趣集合，epoll_ctl ADD把连接FD和关心的条件放入其中。data.fd是我们随注册保存的整数身份，epoll_wait返回它，调用者据此定位连接状态；这里没有把局部C指针交给网络另一端。返回的就绪位只说明现在值得尝试，真正进度以recv/send返回值为准。

<a id="epoll-lt-et"></a>
## abc只读a：LT再次返回，ET需要把bc主动读尽

要区分两种通知方式，先让第一次事件之后的状态保持可辨认：abc 已全到达，应用只读走 a，还剩 bc。trigger 为两种模式分别建立全新连接，发送 abc。FIONREAD只是观察内核当前接收字节数，不消费它；先等三个字节全部到达，避免后续TCP分段到达被误认为相同状态的重复边沿。第一次epoll_wait后recv一个字节a，接收缓冲仍有bc，发送端不再添加数据。

LT默认按仍成立的条件通知，第二次wait应继续有事件。ET加EPOLLET，只读a后没有新的数据到达，第二次短wait不返回新事件；bc仍在内核，不会因为没有通知而消失。作者随后读取bc，再读一次得到EAGAIN，完成正确的ET排空步骤。非阻塞与“读到EAGAIN”相配合：recv零是EOF，负一且errno=EAGAIN是暂时没有数据，不能混成同一种结束。

<a id="epoll-pressure"></a>
## send短写和EAGAIN把未发后缀留在应用

前面的未读 bc 留在内核，发送端还会遇到另一种剩余：send 只接受前缀时，后缀仍属于应用。pressure 把发送缓冲区请求调小，先连续发送，直到真实EAGAIN才转去消费接收端。SO_SNDBUF不是精确容量承诺，Linux可以调整请求值；程序记录实际send接受的字节数，不能预设第几次一定写满。sent只增加返回的正数，短写后下次继续同一消息的未发后缀；本例内容都是x，所以每个后缀值相同，实际业务还需保存对应缓冲区和偏移。

收到 EPOLLOUT 后也只再次尝试发送保存的后缀；如果已经没有待发数据，继续订阅会反复报告一个无需处理的条件。EPOLLOUT 只在还有待发数据时订阅。全部262144字节被接受后DEL发送端注册，并shutdown(SHUT_WR)，FIN排在已有字节之后。接收端看到RDHUP仍继续recv，直到真正返回零才完成；如果立刻close，会丢掉尚未消费的尾部。每轮把接收端读到EAGAIN，使空间释放后发送端能够再次推进。十秒单调总截止和外层三十秒timeout共同限制失败路径。

程序要求实际出现至少一次EAGAIN，并核对全部内容、发送/接收总数和EOF。short_send可以为0，不能为了展示短写而编造一次正返回。单连接LT循环用于解释状态，不是生产公平调度器；多连接服务还要限制每轮配额、管理输出队列上限，防止一条连接占满内存或饿死其他连接。

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

**更新入口的phase选项。** guest构建不依赖这个Linux实验。

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
    parser.add_argument('--phase',choices=('p1','p2','p3','p4','p5-threads','p5-epoll'),default='p1') # Linux阶段显式选择，默认保持P1。
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
./airios linux-lab --phase p5-epoll # 在个人主机建立本机TCP连接并保存系统调用证据。
```

2026-10-16 的既有 SSH 连接返回 255，Linux分支尚未编译、运行和跟踪。Mac拒绝分支可编译且运行返回77，sh -n通过；这些不能证明Linux路径通过。真实失败记录在artifacts/linux-p5-epoll/remote.json与connection.log。连接可用后应出现LT second_events=1、ET second_events=0，以及字节计数相等且EAGAIN大于零的压力结果，次数以实际环境为准。

回到旧网络库时，沿“应用输出队列 → send接受前缀 → 内核socket缓冲 → 对端消费”看背压。等epoll可写不能替代应用保存剩余字节；而AiriOS CQ的result已经对应一次已接受的请求，不需要收到CQ后再补一次实际设备读。AiriOS目前没有socket或TCP/IP，这部分由Linux实测补足。

依据：[Linux epoll手册](https://man7.org/linux/man-pages/man7/epoll.7.html)、[send](https://man7.org/linux/man-pages/man2/send.2.html)、[recv](https://man7.org/linux/man-pages/man2/recv.2.html)与[shutdown](https://man7.org/linux/man-pages/man2/shutdown.2.html)。
