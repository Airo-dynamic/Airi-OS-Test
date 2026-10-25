#include <airi_pthread_internal.h> // 显式内部协议观察，尚不冒称条件变量。
#include <airi_threads.h> // 实际身份和单执行者请求计数。
#include <airi_libc.h> // UART、名义tick和调度交接。
#include <errno.h> // 正错误与当前执行者私有errno。
#include <stdatomic.h> // 夹具阶段独立于被观察互斥。
static pthread_mutex_t guard = PTHREAD_MUTEX_INITIALIZER; // 初始引用为0。
static _Atomic unsigned released, resume; // 工作者释放以后，主再验证空闲但不可销毁。
static void require(int condition, const char *message) { // 不把错误路径继续当成功。
    if (!condition) { airi_puts(message); airi_puts("\n"); airi_exit(93); } // 整组终止以免遗留等待者。
}
static void await(_Atomic unsigned *word) { // 夹具本身有界，不拿无限等待掩盖失败。
    uint64_t start = airi_ticks(); // 一次总起点。
    while (!atomic_load_explicit(word, memory_order_acquire)) { // 对方release发布以后才能继续。
        require(airi_ticks() - start < 100, "mutex lifetime handshake timeout"); airi_yield(); // 真正允许另一执行者运行。
    }
}
static void *worker(void *argument) { // 演示一次未来条件等待所需的锁寿命，不模拟通知。
    (void)argument; errno = 81; // 私有错误槽不应被内部协议改动。
    require(pthread_mutex_lock(&guard) == 0 && __airi_pthread_mutex_wait_ref(&guard) == 0, "mutex lifetime acquire reference"); // 先持锁保留。
    require(pthread_mutex_unlock(&guard) == 0 && __airi_pthread_mutex_wait_unref(&guard) == EPERM, "mutex lifetime no early release"); // 引用必须跨越整个无所有权区间。
    atomic_store_explicit(&released, 1, memory_order_release); await(&resume); // 主可以取得这把锁，但不能销毁。
    require(pthread_mutex_lock(&guard) == 0 && __airi_pthread_mutex_wait_unref(&guard) == 0 && __airi_pthread_mutex_wait_unref(&guard) == EINVAL, "mutex lifetime paired return"); // 先重取再归还，额外归还不下溢。
    require(pthread_mutex_unlock(&guard) == 0 && errno == 81, "mutex lifetime worker errno"); // 全部访问结束。
    return (void *)71; // join收取同一真实工作者结果。
}
int main(void) { // 主与worker同时使用同一个静态互斥。
    errno = 71; pthread_t thread; void *result = 0; // 私有结果槽。
    require(__airi_pthread_mutex_wait_ref(0) == EINVAL && __airi_pthread_mutex_wait_ref(&guard) == EPERM, "mutex lifetime invalid registration"); // 显式坏对象和未持有都不登记。
    require(pthread_create(&thread, 0, worker, 0) == 0, "mutex lifetime worker create"); await(&released); // worker此时确已释放core。
    require(__atomic_load_n(&guard.core.owner, __ATOMIC_SEQ_CST) == 0 && __atomic_load_n(&guard.wait_users, __ATOMIC_SEQ_CST) == 1, "mutex lifetime idle owner live reference"); // 空闲不能代表无人再用。
    require(pthread_mutex_destroy(&guard) == EBUSY && pthread_mutex_trylock(&guard) == 0, "mutex lifetime retained but usable"); // 引用阻止撤销对象，但不锁住数据。
    require(pthread_mutex_unlock(&guard) == 0 && pthread_mutex_destroy(&guard) == EBUSY, "mutex lifetime still retained"); // 主释放以后引用仍属于未完成操作。
    atomic_store_explicit(&resume, 1, memory_order_release); // 允许原worker回来重取。
    require(pthread_join(thread, &result) == 0 && result == (void *)71 && !__atomic_load_n(&guard.wait_users, __ATOMIC_SEQ_CST), "mutex lifetime join reclaimed"); // 已配对，当前再无使用者。
    require(pthread_mutex_destroy(&guard) == 0 && pthread_mutex_init(&guard, 0) == 0 && !guard.wait_users && pthread_mutex_destroy(&guard) == 0 && errno == 71, "mutex lifetime quiescent reinit"); // 静止后销毁/重建才合法。
    airi_puts("[MUTEX_LIFETIME_OK] owner=idle reference=live usable=1 destroy=busy reacquire=before_unref errno=preserved\n"); // 只报告本节实际机制。
    return 0; // 不声称已经实现pthread_cond_wait。
}
