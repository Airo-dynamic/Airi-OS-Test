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
