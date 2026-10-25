#include <pthread.h> // 同时使用真实线程及标准形状的互斥。
#include <airi_threads.h> // 只为实际请求计数，不替代pthread锁操作。
#include <airi_libc.h> // 名义时钟、串口和临界区主动交接。
#include <errno.h> // 所有pthread返回不得覆盖当前错误哨兵。
#include <stdatomic.h> // 夹具开始/结束独立发布，不借受保护普通数据通信。
static pthread_mutex_t guard = PTHREAD_MUTEX_INITIALIZER; // 本项目DEFAULT映为ERRORCHECK，发布前静态构造。
static unsigned counter, checksum = 0x5a5a, tokens[2]; // 普通共享数据由guard保护，参数本体由主持有。
static _Atomic unsigned ready, finished; // 主持锁时确认两位已开始但尚未完成。
static void require(int condition, const char *message) { // 错误终止本真实用户组。
    if (!condition) { airi_puts(message); airi_puts("\n"); airi_exit(90); } // 不在失败后输出成功。
}
static void *worker(void *argument) { // 两位都通过pthread入口取得独立TLS。
    require(errno == 0, "pthread mutex fresh errno"); errno = 91; // 之后所有锁操作都应保持这个值。
    require(pthread_mutex_unlock(&guard) == EPERM && pthread_mutex_trylock(&guard) == EBUSY, "pthread mutex foreign owner"); // 主仍持锁，外人不能释放。
    atomic_fetch_add_explicit(&ready, 1, memory_order_release); // 只表示即将进入真正阻塞lock。
    for (unsigned i = 0; i < 32; ++i) { // 每人32次普通状态更新。
        require(pthread_mutex_lock(&guard) == 0 && errno == 91, "pthread mutex blocking acquisition"); // 内部200tick到期不得泄漏成公开ETIMEDOUT。
        unsigned before = counter; require(checksum == (before ^ 0x5a5a), "pthread mutex protected checksum"); // 同一次临界区看一致数据。
        airi_yield(); // 故意持锁交接，另一位必须竞争而非读半成品。
        counter = before + 1; checksum = counter ^ 0x5a5a; // 先后写两个普通对象，下一持有者一起看到。
        require(pthread_mutex_unlock(&guard) == 0 && errno == 91, "pthread mutex release errno"); // 正常和竞争通知均不改errno。
    }
    atomic_fetch_add_explicit(&finished, 1, memory_order_release); // 不再访问guard以后才发布完成。
    return argument; // main仍持有参数，join取得原指针。
}
int main(void) { // 静态与动态对象分别观察，均不并发init/destroy。
    errno = 71; pthread_mutexattr_t attributes; pthread_mutex_t dynamic; int type = 99; // 未初始化对象只交init，不先读取旧标记。
    require(pthread_mutexattr_init(0) == EINVAL && pthread_mutexattr_init(&attributes) == 0, "pthread mutex attribute init"); // 显式坏形状不影响下一次正常构造。
    require(pthread_mutexattr_gettype(&attributes, &type) == 0 && type == PTHREAD_MUTEX_ERRORCHECK, "pthread mutex default mapping"); // 不冒充NORMAL行为。
    require(pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_NORMAL) == ENOTSUP && pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_RECURSIVE) == ENOTSUP && pthread_mutexattr_settype(&attributes, 99) == EINVAL, "pthread mutex unsupported policies"); // 失败必须保持旧类型。
    require(pthread_mutexattr_gettype(&attributes, &type) == 0 && type == PTHREAD_MUTEX_ERRORCHECK && pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_ERRORCHECK) == 0, "pthread mutex type unchanged"); // 只接受真正实现的策略。
    require(pthread_mutex_init(&dynamic, &attributes) == 0 && pthread_mutexattr_destroy(&attributes) == 0, "pthread mutex copies policy"); // 互斥不保存属性指针。
    type = 99; require(pthread_mutexattr_gettype(&attributes, &type) == EINVAL && type == 99, "pthread mutex dead attributes"); // 当前诊断扩展，失败不写输出。
    require(pthread_mutex_lock(&dynamic) == 0 && pthread_mutex_lock(&dynamic) == EDEADLK && pthread_mutex_trylock(&dynamic) == EBUSY && pthread_mutex_destroy(&dynamic) == EBUSY, "pthread mutex self and busy destroy"); // 忙时不撤销可用标记。
    require(pthread_mutex_unlock(&dynamic) == 0 && pthread_mutex_destroy(&dynamic) == 0 && pthread_mutex_lock(&dynamic) == EINVAL, "pthread mutex destroy diagnostics"); // 销毁后使用是当前明确诊断，不能当可移植正常用法。
    require(pthread_mutex_init(&dynamic, &attributes) == EINVAL && pthread_mutex_init(&dynamic, 0) == 0, "pthread mutex quiescent reinit"); // 坏属性不构造，NULL默认可重新构造静止对象。
    uint64_t before = airi_thread_syscalls(); // 只包围公共互斥的无竞争操作。
    for (unsigned i = 0; i < 8; ++i) require(pthread_mutex_lock(&dynamic) == 0 && pthread_mutex_unlock(&dynamic) == 0, "pthread mutex fast pairs"); // 初始化标记不需要额外陷入。
    uint64_t after = airi_thread_syscalls(); // 结束查询本身计一次。
    require(after - before == 1 && pthread_mutex_destroy(&dynamic) == 0 && errno == 71, "pthread mutex fast counter"); // 八对锁没有额外请求。
    airi_puts("[PTHREAD_MUTEX_FAST] pairs=8 delta=1\n"); // 打印发生在测量窗口外。
    require(pthread_mutex_lock(&guard) == 0 && pthread_mutex_destroy(&guard) == EBUSY, "pthread mutex primary ownership"); // 主持有时创建两竞争者。
    pthread_t threads[2]; void *result; // 两位实际线程分别收取。
    for (unsigned i = 0; i < 2; ++i) require(pthread_create(&threads[i], 0, worker, &tokens[i]) == 0, "pthread mutex workers"); // 启动包仍由现有适配器交接。
    uint64_t start = airi_ticks(); // 夹具准备也有总期限。
    while (atomic_load_explicit(&ready, memory_order_acquire) != 2) { require(airi_ticks() - start < 100, "pthread mutex contenders ready"); airi_yield(); } // 给两位实际执行机会。
    require(airi_sleep(205) == 0 && atomic_load_explicit(&finished, memory_order_acquire) == 0, "pthread mutex internal timeout retry"); // 超过底层200tick仍未取得，公开调用应继续等待。
    require(pthread_mutex_unlock(&guard) == 0, "pthread mutex opens critical section"); // 之后两人才能更新普通数据。
    for (unsigned i = 0; i < 2; ++i) require(pthread_join(threads[i], &result) == 0 && result == &tokens[i], "pthread mutex worker results"); // 正常收拢以后才能销毁。
    require(pthread_mutex_lock(&guard) == 0 && counter == 64 && checksum == (64 ^ 0x5a5a) && pthread_mutex_unlock(&guard) == 0, "pthread mutex final ordinary state"); // 所有更新完整且未丢失。
    require(pthread_mutex_destroy(&guard) == 0 && errno == 71, "pthread mutex final lifecycle"); // 所有使用者已结束，不把检测当并发销毁许可。
    airi_puts("[PTHREAD_MUTEX_USER_OK] default=errorcheck counter=64 critical_yield=1 timeout=internal init=dynamic destroy=checked errno=preserved\n"); // 正常、错误与生命周期完整结束。
    return 0; // shell取得真实成功结果。
}
