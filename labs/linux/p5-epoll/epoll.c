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
