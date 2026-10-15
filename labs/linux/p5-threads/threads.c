#define _GNU_SOURCE // Linux syscall与线程接口声明。
#include <stdio.h> // 非Linux分支也可明确报告限制。
#ifndef __linux__ // Mac编译只检查环境拒绝路径。
int main(void) { // 不把宿主pthread当Linux futex实验。
    fputs("Linux is required for pthread/futex observation.\n", stderr); // 明确目标环境。
    return 77; // 未验证，不是通过。
}
#else // 完整机制仅在Linux编译运行。
#include <errno.h> // syscall失败与pthread返回错误码分别处理。
#include <linux/futex.h> // Linux等待/唤醒操作，不是C标准库常量。
#include <pthread.h> // Linux用户线程与条件变量。
#include <sched.h> // 准备阶段让出CPU，不在未准备时无限自旋。
#include <stdatomic.h> // C11发布/获取与独立统计计数。
#include <stdint.h> // 时间与futex字宽。
#include <stdlib.h> // 失败结束整个独立实验。
#include <sys/syscall.h> // SYS_futex和真正内核TID。
#include <time.h> // 单调截止时间和有界模拟工作。
#include <unistd.h> // syscall、进程ID。
static void fail(const char *operation) { // errno必须来自具体失败操作。
    perror(operation); // 保存系统错误身份。
    exit(1); // 运行器不发布成功。
}
static void check_thread(int error, const char *operation) { // pthread返回错误码本身，不自动设置errno。
    if (error) { // 只有非零才失败。
        errno = error; // 给perror一个明确来源。
        fail(operation); // 不用陈旧errno解释线程失败。
    }
}
static void expect(int condition, const char *reason) { // 事实不符与系统errno分开。
    if (!condition) { // 避免无效继续。
        fprintf(stderr, "expectation failed: %s\n", reason); // 保留当前机制位置。
        exit(1); // 无成功记录。
    }
}
static int64_t now_ns(void) { // 相对单调时间，不受墙钟调整。
    struct timespec now; // 秒与纳秒两个字段。
    if (clock_gettime(CLOCK_MONOTONIC, &now)) // 当前机器的真实时钟。
        fail("clock_gettime"); // 不使用未初始化值。
    return (int64_t)now.tv_sec * 1000000000ll + now.tv_nsec; // 实验时间范围内转换成纳秒。
}
static void pause_ns(long ns) { // 有界工作延迟，不在互斥区里睡眠。
    struct timespec remaining = {0, ns}; // 此实验只用小于一秒的常量。
    while (nanosleep(&remaining, &remaining)) // 信号中断保留剩余时间。
        if (errno != EINTR) // 其他错误不能忽略。
            fail("nanosleep"); // 不伪造已完成延迟。
}
struct queue { // 两个消费者共用有界任务值，不借生产者栈指针。
    pthread_mutex_t mutex; // 保护数组、head/count和关闭谓词。
    pthread_cond_t not_empty, not_full; // 通知只提示重查谓词，不保存任务。
    unsigned values[2], head, count, producer_waits, consumer_waits; // 容量二使背压可见。
    int closed; // 生产结束后仍要先消费残余任务。
};
static struct queue queue = {.mutex = PTHREAD_MUTEX_INITIALIZER, .not_empty = PTHREAD_COND_INITIALIZER, .not_full = PTHREAD_COND_INITIALIZER}; // 线程创建前已初始化。
static atomic_uint completed; // 独立遥测计数，不负责发布队列数据。
struct worker { unsigned id, count, sum; }; // 每个线程写自己的结果，main在join后读取。
static void *consume(void *argument) { // pthread入口返回void*，参数寿命覆盖join。
    struct worker *worker = argument; // 只借main的稳定数组项。
    printf("[WORKER] pid=%ld tid=%ld worker=%u\n", (long)getpid(), syscall(SYS_gettid), worker->id); // 同PID不同内核TID，不把pthread_t当整数PID。
    for (;;) { // 直到关闭并排空，不因一条通知就假设有任务。
        check_thread(pthread_mutex_lock(&queue.mutex), "lock consumer"); // 读取谓词前先拿同一互斥锁。
        while (!queue.count && !queue.closed) { // 空且尚有未来生产者才需要等。
            ++queue.consumer_waits; // 此计数也由mutex保护。
            check_thread(pthread_cond_wait(&queue.not_empty, &queue.mutex), "wait not_empty"); // 原子释放锁并等待，返回时重新持锁。
        }
        if (!queue.count && queue.closed) { // 关闭后已排空，才能退出。
            check_thread(pthread_mutex_unlock(&queue.mutex), "unlock finished consumer"); // 不带锁结束线程。
            break; // main的join随后收拢结果。
        }
        unsigned value = queue.values[queue.head]; // 任务值在当前临界区内取得。
        queue.head = (queue.head + 1) % 2; --queue.count; // 消费一个元素并释放容量。
        check_thread(pthread_cond_signal(&queue.not_full), "signal not_full"); // 唤醒生产者重查容量。
        check_thread(pthread_mutex_unlock(&queue.mutex), "unlock consumer"); // 模拟工作在锁外。
        pause_ns(1000000); // 一毫秒处理时间，给另一个线程和生产者运行机会。
        worker->sum += value; ++worker->count; // 仅当前线程写这项局部结果。
        atomic_fetch_add_explicit(&completed, 1, memory_order_relaxed); // 两消费者的统计增量不可丢，但不发布任务内容。
    }
    return NULL; // join只等完成，不把局部栈地址交回。
}
_Alignas(4) static atomic_int futex_word; // Linux要求四字节对齐，C对象仍通过原子接口访问。
static atomic_int waiter_ready; // 仅用于准备时序，和实际发布flag分开。
static int payload; // 由futex_word的release/acquire关系发布，不靠WAKE本身。
struct wait_result { unsigned calls; int value; }; // waiter独占写，main在join后检查。
_Static_assert(sizeof(atomic_int) == sizeof(uint32_t), "Linux futex word must be 32 bits"); // 固定目标不接受额外原子表示。
static void *wait_value(void *argument) { // 同一进程共享futex_word物理对象。
    struct wait_result *result = argument; // 结果寿命由main持到join。
    int64_t deadline = now_ns() + 2000000000ll; // 整个等待最多两秒，不因EINTR反复重置。
    atomic_store_explicit(&waiter_ready, 1, memory_order_release); // 告诉主线程已到准备位置，不替代实际futex比较。
    while (atomic_load_explicit(&futex_word, memory_order_acquire) == 0) { // 每次返回都重查数据发布条件。
        int64_t remaining = deadline - now_ns(); // 只把剩余时间交给相对WAIT。
        expect(remaining > 0, "futex wait deadline expired"); // 无事件不能永远等。
        struct timespec timeout = {remaining / 1000000000ll, remaining % 1000000000ll}; // Linux FUTEX_WAIT使用相对跨度。
        ++result->calls; // 实际进入syscall前才计数。
        long value = syscall(SYS_futex, &futex_word, FUTEX_WAIT_PRIVATE, 0, &timeout, NULL, 0); // 内核比较word仍为0才原子入睡。
        if (value < 0 && errno != EAGAIN && errno != EINTR) // EAGAIN表示发布已抢先发生，不是丢失唤醒。
            fail("FUTEX_WAIT_PRIVATE"); // 超时或其他真实错误明确失败。
    }
    result->value = payload; // acquire看到1之后才读普通共享内容。
    return NULL; // 不把payload指针跨越已结束线程返回。
}
int main(void) { // 所有线程只修改本独立进程内的演示状态。
    atomic_init(&completed, 0); // 在任何消费者启动前初始化。
    pthread_t threads[2]; // pthread句柄不能等同于gettid整数。
    struct worker workers[2] = {{.id = 1}, {.id = 2}}; // 两份结果由各线程独占写。
    for (unsigned i = 0; i < 2; ++i) // 创建两个内核可调度线程。
        check_thread(pthread_create(&threads[i], NULL, consume, &workers[i]), "pthread_create consumer"); // main数组在join前保持有效。
    for (unsigned value = 1; value <= 20; ++value) { // 明确二十个任务，和为210。
        check_thread(pthread_mutex_lock(&queue.mutex), "lock producer"); // 容量谓词和入队在同一锁内。
        while (queue.count == 2 && !queue.closed) { // 满时等待，虚假/竞争唤醒后仍重新检查。
            ++queue.producer_waits; // 不用计数证明一定发生过内核睡眠。
            check_thread(pthread_cond_wait(&queue.not_full, &queue.mutex), "wait not_full"); // 释放锁给消费者，醒后重新持锁。
        }
        expect(!queue.closed, "queue closed before producer finished"); // 关闭不能继续发布新任务。
        queue.values[(queue.head + queue.count) % 2] = value; ++queue.count; // 数据先写，持锁发布新的谓词。
        check_thread(pthread_cond_signal(&queue.not_empty), "signal not_empty"); // 通知等待者重查。
        check_thread(pthread_mutex_unlock(&queue.mutex), "unlock producer"); // unlock/lock交接也建立数据可见关系。
    }
    check_thread(pthread_mutex_lock(&queue.mutex), "lock close"); // 关闭谓词同样受锁保护。
    queue.closed = 1; // 不再有未来任务，但不删除队列现有内容。
    check_thread(pthread_cond_broadcast(&queue.not_empty), "broadcast close"); // 所有可能睡眠的消费者都要重查关闭。
    check_thread(pthread_mutex_unlock(&queue.mutex), "unlock close"); // 消费者能取得锁继续排空。
    for (unsigned i = 0; i < 2; ++i) // 结束后才读取它们的普通结果字段。
        check_thread(pthread_join(threads[i], NULL), "join consumer"); // 线程对象与结果寿命在这里收拢。
    expect(workers[0].count + workers[1].count == 20 && workers[0].sum + workers[1].sum == 210 && atomic_load_explicit(&completed, memory_order_relaxed) == 20, "bounded queue lost or duplicated work"); // 统计原子不替代前面的队列锁或join。
    printf("[QUEUE_OK] tasks=20 sum=210 producer_waits=%u consumer_waits=%u\n", queue.producer_waits, queue.consumer_waits); // 实际等待次数可能随调度变化。
    check_thread(pthread_cond_destroy(&queue.not_empty), "destroy not_empty"); // 所有线程已结束。
    check_thread(pthread_cond_destroy(&queue.not_full), "destroy not_full"); // 无等待者才可销毁。
    check_thread(pthread_mutex_destroy(&queue.mutex), "destroy mutex"); // 结束同步对象寿命。
    atomic_init(&futex_word, 0); atomic_init(&waiter_ready, 0); // 新的独立发布实验。
    expect(atomic_is_lock_free(&futex_word), "target atomic int is not lock-free"); // 不把带隐藏锁的表示直接交给Linux futex。
    struct wait_result result = {0}; // waiter写，join后main读。
    pthread_t waiter; // 真正等待的内核线程句柄。
    check_thread(pthread_create(&waiter, NULL, wait_value, &result), "create futex waiter"); // 两秒deadline由waiter建立。
    int64_t deadline = now_ns() + 2000000000ll; // 准备阶段自己也有上界。
    while (!atomic_load_explicit(&waiter_ready, memory_order_acquire)) { // 等到waiter准备，不假设它已真正睡着。
        expect(now_ns() < deadline, "waiter did not initialize"); // 不无限自旋。
        sched_yield(); // 给等待线程获得CPU机会。
    }
    pause_ns(20000000); // 鼓励观察真实等待，但不把时间安排当互斥正确性依据。
    payload = 42; // 普通数据写先于发布flag。
    atomic_store_explicit(&futex_word, 1, memory_order_release); // acquire见1后可读取这个payload。
    long woken = syscall(SYS_futex, &futex_word, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0); // 唤醒最多一个内核等待者，不替代release。
    if (woken < 0) // 系统调用失败保留errno。
        fail("FUTEX_WAKE_PRIVATE"); // 不伪造通知成功。
    check_thread(pthread_join(waiter, NULL), "join futex waiter"); // 所有payload读取结束后main才验证。
    expect(result.value == 42, "release/acquire publication failed"); // 不是靠volatile或WAKE发布普通数据。
    printf("[FUTEX_OK] value=%d wait_calls=%u actually_woken=%ld word=%p\n", result.value, result.calls, woken, (void *)&futex_word); // 快时woken可0，照实记录。
    puts("[LINUX_THREADS_OK] queue=bounded publication=release_acquire telemetry=relaxed"); // 完整路径才发布。
    return 0; // 独立Linux程序成功。
}
#endif // 非Linux只执行明确拒绝分支。
