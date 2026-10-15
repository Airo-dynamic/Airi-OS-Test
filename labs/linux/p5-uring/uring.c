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
