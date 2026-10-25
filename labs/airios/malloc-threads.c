#include <stdlib.h> // 公开对象接口，现在共用内部元数据锁。
#include <airi_malloc.h> // 同一时刻三项一致的统计快照。
#include <airi_threads.h> // 真实共享执行者，不使用fork模拟共享堆。
#include <airi_libc.h> // PIT、yield和串口观察。
#include <errno.h> // 每个执行者保留独立错误槽。
#include <stdatomic.h> // 对象交接用release/acquire明确移交所有权。
static _Atomic unsigned gate, done; // 全部工作者创建完后才一起争取第一次arena初始化。
static _Atomic uintptr_t handed[4]; // 每槽只发布一次活对象指针，交给下一位工作者归还。
static long work(void *argument) { // 四位工作者共享同一BSS、块链和页面缓存。
    unsigned identity = (unsigned)(uintptr_t)argument; // 参数是0至3的小数值，不是临时对象地址。
    int tag = (int)(300 + identity); // 成功分配与锁等待都应保留本执行者的错误状态。
    errno = tag; // 工作者TLS由内核从声明模板建立。
    while (!atomic_load_explicit(&gate, memory_order_acquire)) airi_yield(); // 主执行者还在创建其他工作者，不提前开始申请。
    for (unsigned round = 0; round < 24; ++round) { // 每轮只拥有两个对象，内存峰值有明确上界。
        size_t bytes = 16 + (round % 8) * 17; // 不同请求长度形成补齐、分裂和合并。
        unsigned char *first = malloc(bytes), *zero = calloc(3, bytes); // 所有线程共用唯一按需arena。
        if (!first || !zero || ((uintptr_t)first & 15) || ((uintptr_t)zero & 15) || errno != tag) return -201; // 成功不得借另一个执行者的errno。
        for (size_t i = 0; i < bytes; ++i) first[i] = (unsigned char)(identity * 31 + round + i); // 该对象当前只由本工作者使用。
        for (size_t i = 0; i < 3 * bytes; ++i) if (zero[i]) return -202; // 复用过的块也必须清零完整请求。
        airi_yield(); // 对象可以跨其他执行者分配操作保持内容，不持有内部分配锁。
        unsigned char *grown = realloc(first, bytes * 2); // 复合操作只在内部取得一次锁。
        if (!grown || errno != tag) return -203; // 失败由进程组退出收拢，不伪造继续成功。
        for (size_t i = 0; i < bytes; ++i) if (grown[i] != (unsigned char)(identity * 31 + round + i)) return -204; // 只检查原有效范围。
        free(grown); free(zero); // 正常释放不能因一次内部等待到期而静默丢失请求。
    }
    volatile size_t overflow = SIZE_MAX; // 保留运行时乘法溢出分支，不由优化器折叠观察。
    if (calloc(overflow, 2) || errno != ENOMEM) return -205; // 错误出口必须释放内部锁，其他执行者继续分配。
    errno = tag; // 后续成功仍保留本执行者明确设定的错误。
    unsigned char *gift = malloc(64); // 新对象即将交给另一个执行者，不再由自己归还。
    if (!gift) return -206; // 不能发布无效指针。
    for (unsigned i = 0; i < 64; ++i) gift[i] = (unsigned char)(identity + i); // 发布以前写完完整内容。
    atomic_store_explicit(&handed[identity], (uintptr_t)gift, memory_order_release); // 从此放弃对gift的访问和释放权。
    unsigned previous = (identity + 3) % 4; // 每人只接收上一人的对象，恰好一位接收者。
    uint64_t start = airi_ticks(); // 交接夹具也必须在有限时间内推进。
    uintptr_t received; // acquire取得的不只是地址，还包含对应发布的数据顺序。
    while (!(received = atomic_load_explicit(&handed[previous], memory_order_acquire))) { // 等前一位发布，不在分配器锁内等待。
        if (airi_ticks() - start >= 300) return -207; // 失败不能无限卡住shell观察。
        airi_yield(); // 让其他生产者继续完成自己的分配。
    }
    unsigned char *owned = (unsigned char *)received; // 现在由本执行者独占这个活对象。
    for (unsigned i = 0; i < 64; ++i) if (owned[i] != (unsigned char)(previous + i)) return -208; // 读取只发生在接收所有权以后。
    free(owned); // 可由不同TID向同一个分配器归还精确起点。
    if (errno != tag) return -209; // 另一个线程的ENOMEM不能污染当前TLS。
    atomic_fetch_add_explicit(&done, 1, memory_order_release); // 当前工作者不再持有任何对象。
    return 24; // 主执行者通过真实join取得完成轮数。
}
static void require(int condition, const char *message) { // 单独保留主执行者的具体错误原因。
    if (!condition) { airi_puts(message); airi_puts("\n"); airi_exit(83); } // 进程组失败时由内核统一回收页面和执行栈。
}
int main(void) { // 初始堆描述和内部锁均来自本ELF的新BSS。
    struct airi_malloc_info initial = airi_malloc_info(); // 观察快照不能替自己申请arena。
    require(!initial.arena_bytes && !initial.live_bytes && !initial.live_objects, "lazy shared heap"); // 四人放行前确实尚未创建arena。
    errno = 600; // 主执行者也有独立TLS错误状态。
    long ids[4]; // 所有TID的生命周期都持续到join。
    for (unsigned i = 0; i < 4; ++i) { // 同一进程中四位真实工作者。
        ids[i] = airi_thread_create(work, (void *)(uintptr_t)i); // 不复制进程地址空间或FD。
        require(ids[i] > 0, "create heap workers"); // 资源不足应明确停止观察。
    }
    atomic_store_explicit(&gate, 1, memory_order_release); // 四人都存在后才开始争取首次初始化。
    uint64_t start = airi_ticks(); unsigned snapshots = 0; // 主执行者并发观察，不读取内部块指针。
    while (atomic_load_explicit(&done, memory_order_acquire) != 4) { // 快照和其他线程的分配操作互斥。
        struct airi_malloc_info view = airi_malloc_info(); // 取回按值一致的三项，随后其他工作者仍可推进。
        require((!view.arena_bytes || view.arena_bytes == 262144) && view.live_bytes <= view.arena_bytes && (!view.live_objects == !view.live_bytes), "consistent heap snapshot"); // 空闲请求与缓存页面分别统计。
        require(airi_ticks() - start < 1000 && errno == 600, "heap progress and parent errno"); // 不把卡住或错误状态串扰当完成。
        ++snapshots; airi_yield(); // 不在主循环持续抢占私有锁，让工作者完成。
    }
    for (unsigned i = 0; i < 4; ++i) { // done只说明完成发布，join才收拢执行者。
        long result; // 每个工作者的结果独立复制到当前栈。
        require(airi_thread_join(ids[i], &result) == 0 && result == 24, "join heap worker"); // 每人都完成24轮及跨线程对象归还。
    }
    struct airi_malloc_info final = airi_malloc_info(); // 全部工作者已结束，此刻没有活对象。
    require(final.arena_bytes == 262144 && !final.live_bytes && !final.live_objects && snapshots, "shared heap fully returned"); // 页面缓存仍在，不能称为已归还物理页。
    airi_puts("[MALLOC_THREADS_OK] workers=4 rounds=96 arena=262144 cross_free=4 snapshots=consistent errno=private critical_yield=1\n"); // 所有真实状态与生命周期检查完成后才输出。
    return 0; // 用户shell收取成功，进程退出才归还页面缓存。
}
