#include <airi_queue.h> // 同一有界队列实现，不借宿主队列。
#include <airi_threads.h> // 真正共享地址空间的工作执行者。
#include <airi_libc.h> // 时钟、串口和主动交接。
#include <stdatomic.h> // 夹具进度和锁外计算结果独立发布。
static airi_queue probe = AIRI_QUEUE_INITIALIZER, draining = AIRI_QUEUE_INITIALIZER, jobs = AIRI_QUEUE_INITIALIZER, limited = AIRI_QUEUE_INITIALIZER; // 各观察对象有独立生命周期。
static _Atomic unsigned started, completed; // 只协调观察，不读写队列内部状态。
static _Atomic uint64_t seen, sum, squares; // 每项在队列之外计算并累计。
static void require(int condition, const char *message) { // 任意失败结束整个真实用户进程组。
    if (!condition) { airi_puts(message); airi_puts("\n"); airi_exit(87); } // 不继续输出成功标记。
}
static void await(_Atomic unsigned *word, unsigned value) { // 夹具握手有独立总期限。
    uint64_t start = airi_ticks(); // 同一个名义PIT时钟。
    while (atomic_load_explicit(word, memory_order_acquire) < value) { // 等真实工作者的发布。
        require(airi_ticks() - start < 100, "queue handshake timeout"); airi_yield(); // 不用无限轮询掩盖阻塞。
    }
}
static long blocked_producer(void *argument) { // 队列四槽已满以后才创建。
    (void)argument; // 本轮固定追加值5。
    atomic_fetch_add_explicit(&started, 1, memory_order_release); // 只是已开始，不伪造内核等待登记。
    long result = airi_queue_put(&probe, 5, 200); // 主取走一项以后才有空位。
    atomic_store_explicit(&completed, 1, memory_order_release); // 即使错误也发布结束，再由join检查结果。
    return result; // 0才表示确实接受。
}
static long closing_consumer(void *argument) { // 空队列上等待关闭广播。
    (void)argument; uint64_t value = 99; // 私人输出，EOF应保持99。
    atomic_fetch_add_explicit(&started, 1, memory_order_release); // 主等待两人都开始。
    long result = airi_queue_get(&probe, &value, 200); // 本轮没有新项，只会关闭。
    return result == 0 && value == 99 ? 0 : -240; // 结束不是一个伪造任务值。
}
static long closing_producer(void *argument) { // 满队列上的生产者必须被关闭广播唤醒并拒绝。
    airi_queue *queue = argument; // 主创建时借用draining，保持到join以后。
    atomic_fetch_add_explicit(&started, 1, memory_order_release); // 只表示已经开始尝试。
    return airi_queue_put(queue, 10, 200); // 关闭以后原调用返回-32，不能偷偷接受第十项。
}
static long producer(void *argument) { // 两人各产生连续但互不重叠的24个任务号。
    unsigned index = (unsigned)(uintptr_t)argument; // 创建时传入0或1。
    for (unsigned i = 1; i <= 24; ++i) // 总共48项，均适合64位已见位图。
        if (airi_queue_put(&jobs, index * 24 + i, 200)) return -241; // 背压时真实等待，不覆盖仍未取走的槽。
    return 24; // 主join以后才关闭，不把尚未入队任务丢掉。
}
static long consumer(void *argument) { // 取值以后在队列临界区外计算。
    (void)argument; long count = 0; // 每人完成数可以不同，不承诺公平分配。
    atomic_fetch_add_explicit(&started, 1, memory_order_release); // 工作负载也先让两消费者等待空队列。
    for (;;) { // 直到已关闭且排空。
        uint64_t value; // 私人输出，不持有队列槽地址。
        long result = airi_queue_get(&jobs, &value, 200); // 1得到任务，0结束，负数失败。
        if (result == 0) return count; // 不能把EOF计成一次工作。
        if (result != 1 || value < 1 || value > 48) return -242; // 失败时不读取无效输出。
        uint64_t bit = 1ull << (value - 1); // 任务1对应最低位，任务48不越过64位宽度。
        if (atomic_fetch_or_explicit(&seen, bit, memory_order_relaxed) & bit) return -243; // 旧位已存在说明同一项被重复消费。
        atomic_fetch_add_explicit(&sum, value, memory_order_relaxed); // 普通算术位于queue_get返回以后。
        atomic_fetch_add_explicit(&squares, value * value, memory_order_relaxed); // 一项有限计算，不在队列锁内执行回调。
        ++count; airi_yield(); // 主动交接也在锁外，其他人可以继续进出队列。
    }
}
int main(void) { // 先逐项展示失败和关闭，再跑实际生产消费。
    uint64_t value = 71; long result; // 所有失败与EOF都应保留私人哨兵。
    require(airi_queue_get(&probe, 0, 0) == -22 && airi_queue_put(&probe, 9, 6001) == -22, "queue invalid arguments"); // 错误参数不进入队列操作。
    require(airi_queue_get(&probe, &value, 0) == -11 && value == 71, "queue empty nonblocking"); // 空且不等待不是EOF。
    uint64_t start = airi_ticks(); // 空队列真实等待三tick。
    require(airi_queue_get(&probe, &value, 3) == -110 && value == 71 && airi_ticks() - start >= 3, "queue empty timeout"); // 超时不写输出。
    airi_puts("[QUEUE_BACKPRESSURE_BEGIN]\n"); // 宿主独立核对满队列上的实际等待。
    for (uint64_t i = 1; i <= 4; ++i) require(airi_queue_put(&probe, i, 0) == 0, "queue initial fill"); // 正好占满四槽。
    require(airi_queue_put(&probe, 9, 0) == -11, "queue full nonblocking"); // 不覆盖首项。
    start = airi_ticks(); require(airi_queue_put(&probe, 9, 3) == -110 && airi_ticks() - start >= 3, "queue full timeout"); // 到期仍不接收9。
    long blocked = airi_thread_create(blocked_producer, 0); require(blocked > 0, "queue blocked producer create"); // 第五项由真实worker提交。
    await(&started, 1); require(airi_sleep(3) == 0 && atomic_load_explicit(&completed, memory_order_acquire) == 0, "queue producer blocked"); // 满时尚不能完成。
    require(airi_queue_get(&probe, &value, 0) == 1 && value == 1, "queue opens one slot"); // 取走旧首项后才能接受5。
    require(airi_thread_join(blocked, &result) == 0 && result == 0, "queue producer resumed"); // 原put继续，不要求重新提交。
    for (uint64_t i = 2; i <= 5; ++i) require(airi_queue_get(&probe, &value, 0) == 1 && value == i, "queue FIFO after wrap"); // 环绕后仍按2/3/4/5返回，没有失败提交的9。
    airi_puts("[QUEUE_BACKPRESSURE_OK] capacity=4 resumed=1 fifo=2,3,4,5\n"); // 锁外完成正常路径标记。
    atomic_store_explicit(&started, 0, memory_order_relaxed); // 已join，没有旧worker访问进度。
    airi_puts("[QUEUE_CLOSE_WAITERS_BEGIN]\n"); // 后面两位应真正等待空队列。
    long first = airi_thread_create(closing_consumer, 0), second = airi_thread_create(closing_consumer, 0); // 同一开放空队列。
    require(first > 0 && second > 0, "queue closing consumers"); // 不能用单人冒充广播。
    await(&started, 2); require(airi_sleep(3) == 0 && airi_queue_close(&probe, 200) == 0, "queue close wakes consumers"); // closed在guard内发布，两人重查得到EOF。
    require(airi_thread_join(first, &result) == 0 && result == 0 && airi_thread_join(second, &result) == 0 && result == 0, "queue close results"); // 两个私人输出都仍为99。
    require(airi_queue_close(&probe, 0) == 0 && airi_queue_put(&probe, 8, 0) == -32, "queue closed rejects put"); // 重复关闭成功但不重新开放。
    value = 71; require(airi_queue_get(&probe, &value, 0) == 0 && value == 71, "queue EOF unchanged"); // EOF与EAGAIN区分。
    airi_puts("[QUEUE_CLOSE_WAITERS_OK] consumers=2 eof=unchanged\n"); // 本轮真实等待和广播已经收拢。
    for (uint64_t i = 6; i <= 9; ++i) require(airi_queue_put(&draining, i, 0) == 0, "queue drain fill"); // 独立队列拥有四个已接受值。
    atomic_store_explicit(&started, 0, memory_order_relaxed); // 先前两位关闭消费者均已join。
    airi_puts("[QUEUE_CLOSE_PRODUCER_BEGIN]\n"); // 宿主核对满队列另一侧的关闭通知。
    blocked = airi_thread_create(closing_producer, &draining); require(blocked > 0, "queue closing producer create"); // 借用对象地址，不转移队列所有权。
    await(&started, 1); require(airi_sleep(3) == 0, "queue closing producer registration"); // 确实有机会登记到space键。
    require(airi_queue_close(&draining, 200) == 0 && airi_queue_put(&draining, 10, 0) == -32, "queue drain closed"); // 关闭不清空已接受工作。
    require(airi_thread_join(blocked, &result) == 0 && result == -32, "queue closed producer rejected"); // 原等待调用被唤醒以后重查closed。
    airi_puts("[QUEUE_CLOSE_PRODUCER_OK] blocked=1 rejected=closed\n"); // 只有真实返回-32才输出。
    for (uint64_t i = 6; i <= 9; ++i) require(airi_queue_get(&draining, &value, 0) == 1 && value == i, "queue drains before EOF"); // 先排空再结束。
    require(airi_queue_get(&draining, &value, 0) == 0 && value == 9, "queue drained EOF"); // EOF保持最后一次私人输出。
    limited.available.sequence = UINT32_MAX; // 尚未发布给worker的独立夹具初值，不重置活对象。
    require(airi_queue_put(&limited, 7, 0) == -75 && airi_queue_get(&limited, &value, 0) == -11 && value == 9, "queue version failure before mutation"); // 饱和在修改槽以前报告，没有半入队。
    atomic_store_explicit(&started, 0, memory_order_relaxed); // 前组worker已经全部join。
    long consumers[2], producers[2]; // 两种角色分别收取，不固定每消费者拿到多少项。
    for (unsigned i = 0; i < 2; ++i) { consumers[i] = airi_thread_create(consumer, 0); require(consumers[i] > 0, "queue workload consumer"); } // 先启动实际等待者。
    await(&started, 2); require(airi_sleep(3) == 0, "queue workload initially empty"); // 无工作时不忙等消耗CPU。
    for (unsigned i = 0; i < 2; ++i) { producers[i] = airi_thread_create(producer, (void *)(uintptr_t)i); require(producers[i] > 0, "queue workload producer"); } // 每人固定24个不同ID。
    for (unsigned i = 0; i < 2; ++i) require(airi_thread_join(producers[i], &result) == 0 && result == 24, "queue workload produced"); // 等全部接受以后才能关闭。
    require(airi_queue_close(&jobs, 200) == 0, "queue workload close"); // 尚在环中的项继续由消费者排空。
    long total = 0; // 本人已结束的结果相加，不读运行中的普通局部值。
    for (unsigned i = 0; i < 2; ++i) { require(airi_thread_join(consumers[i], &result) == 0 && result >= 0, "queue workload consumed"); total += result; } // 不承诺公平分配。
    require(total == 48 && atomic_load_explicit(&seen, memory_order_relaxed) == ((1ull << 48) - 1), "queue exactly once"); // 每个1至48ID均出现且没有重复。
    require(atomic_load_explicit(&sum, memory_order_relaxed) == 1176 && atomic_load_explicit(&squares, memory_order_relaxed) == 38024, "queue outside lock computation"); // 1至48和及平方和实算。
    airi_puts("[QUEUE_USER_OK] capacity=4 jobs=48 exactly_once=1 sum=1176 squares=38024 backpressure=wait close=drain timeout=preserved\n"); // 所有失败、关闭与工作结果完成后才发布。
    return 0; // shell收取真实成功结果。
}
