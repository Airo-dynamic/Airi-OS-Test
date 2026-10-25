#include <pthread.h> // 本节实现的create/join/self/equal子集。
#include <airi_event.h> // 容量观察用一个独立的版本门。
#include <airi_malloc.h> // 观察启动包是否在成功/失败路径归还。
#include <stdlib.h> // 容量失败观察占满当前有界arena，再归还。
#include <airi_libc.h> // 串口、真实PIT与主动交接。
#include <errno.h> // 各线程错误槽独立，pthread失败不覆盖它。
#include <stdatomic.h> // 夹具准备位置，不借宿主同步。
struct token { unsigned index; pthread_t identity; }; // 参数和结果都指向主仍持有的对象，不指向适配器启动包。
static struct token tokens[8]; // main写index以后才创建，identity只在join以后读取。
static pthread_t primary; // 创建任何worker以前写入，之后只读。
static airi_event gate = AIRI_EVENT_INITIALIZER; // 八容量worker等待主统一发布。
static _Atomic unsigned ready; // 回调开始时启动包已经被适配器释放。
static void require(int condition, const char *message) { // 失败结束本真实进程组。
    if (!condition) { airi_puts(message); airi_puts("\n"); airi_exit(88); } // 不继续输出成功标记。
}
static void *echo(void *argument) { // 真正的POSIX形状函数，不强转函数指针。
    struct token *token = argument; // 用户参数保持到主join以后。
    require(errno == 0 && !pthread_equal(pthread_self(), primary), "pthread fresh errno and identity"); // 新TLS不继承主71。
    token->identity = pthread_self(); // 只写自己那一份输出，主尚未读取。
    errno = 101 + (int)token->index; // 每位执行者保存不同错误哨兵。
    if (!token->index) { // 第一位观察未支持的worker管理能力。
        pthread_t untouched = 777; void *output = (void *)(uintptr_t)99; // 失败输出哨兵。
        require(pthread_create(&untouched, 0, echo, token) == ENOTSUP && untouched == 777, "pthread nested create unsupported"); // 不能悄悄创建后返回失败。
        require(pthread_join(primary, &output) == ENOTSUP && output == (void *)(uintptr_t)99, "pthread peer join unsupported"); // 保留当前主管理限制。
        require(pthread_join(pthread_self(), 0) == EDEADLK, "pthread worker self join"); // 自己等待自己明确报告。
    }
    airi_yield(); // 在真正共享执行者之间交替，errno仍属于自己。
    require(errno == 101 + (int)token->index, "pthread worker errno private"); // pthread调用和别人的TLS不污染它。
    return token; // 目标返回值不是启动包，而是仍存活的用户对象。
}
static void *held(void *argument) { // 容量worker从回调开始就不再占启动包。
    atomic_fetch_add_explicit(&ready, 1, memory_order_release); // 主可据此检查所有包已归还。
    uint64_t start = airi_ticks(); // 固定200tick，不被无变化通知续借。
    while (airi_event_sequence(&gate) == 0) { // 主发布以前不能结束并释放工作槽。
        uint64_t elapsed = airi_ticks() - start; // 切换也消耗本次预算。
        require(elapsed < 200 && airi_event_wait(&gate, 0, (unsigned)(200 - elapsed)) == 0, "pthread capacity gate"); // 真实等待，不用宿主线程阻塞。
    }
    return argument; // 八个结果分别指回自己的token。
}
int main(void) { // 所有创建/收取都从主执行者开始。
    primary = pthread_self(); errno = 71; // 不把pthread_t的具体整数当可移植接口使用。
    pthread_t unchanged = 777; pthread_attr_t attributes = {0}; // 非NULL属性当前不支持，即使保留字为0。
    require(pthread_equal(primary, pthread_self()), "pthread equality"); // 只比较有效身份，不靠猜测下一个TID。
    require(pthread_create(0, 0, echo, 0) == EINVAL && pthread_create(&unchanged, 0, 0, 0) == EINVAL, "pthread invalid arguments"); // 不创建实际worker。
    require(pthread_create(&unchanged, &attributes, echo, 0) == ENOTSUP && unchanged == 777 && errno == 71, "pthread attributes unsupported"); // 属性未生效不能返回成功。
    void *output = (void *)(uintptr_t)99; // join失败保持私人输出。
    require(pthread_join(primary, &output) == EDEADLK && pthread_join(0, &output) == ESRCH && pthread_join((pthread_t)UINT32_MAX + 1, &output) == ESRCH, "pthread invalid joins"); // 不窄化过宽值。
    require(output == (void *)(uintptr_t)99 && errno == 71, "pthread join failure preserves state"); // 正错误不写errno。
    pthread_t threads[8]; // 只在create成功以后使用每个槽。
    for (unsigned i = 0; i < 2; ++i) { tokens[i].index = i; require(pthread_create(&threads[i], 0, echo, &tokens[i]) == 0 && errno == 71, "pthread initial create"); } // 新worker可以在下一次创建以前已经返回。
    require(!pthread_equal(threads[0], threads[1]), "pthread distinct live identities"); // 两个已创建尚未join的身份都仍有效。
    require(pthread_join(threads[0], &output) == 0 && output == &tokens[0] && pthread_equal(tokens[0].identity, threads[0]), "pthread typed result"); // 指针完整往返，身份属于真实worker。
    require(pthread_join(threads[1], 0) == 0 && pthread_equal(tokens[1].identity, threads[1]), "pthread discard result"); // NULL不省略收取。
    output = (void *)(uintptr_t)99; require(pthread_join(threads[0], &output) == ESRCH && output == (void *)(uintptr_t)99 && errno == 71, "pthread already joined extension"); // 本实现不复用TID，旧身份明确拒绝；不可移植代码不依赖此扩展。
    for (unsigned i = 0; i < 8; ++i) require(pthread_create(&threads[i], 0, held, &tokens[i]) == 0 && errno == 71, "pthread capacity fill"); // 八个真实工作槽全部占用。
    uint64_t start = airi_ticks(); // 只等回调实际开始，不从创建成功推断包已释放。
    while (atomic_load_explicit(&ready, memory_order_acquire) != 8) { require(airi_ticks() - start < 100, "pthread capacity ready"); airi_yield(); } // 有限握手。
    struct airi_malloc_info before = airi_malloc_info(); // 所有已运行worker的启动包都应归还。
    require(before.live_objects == 0, "pthread packets freed before callbacks"); // 不把包保留到join才清理。
    unchanged = 777; require(pthread_create(&unchanged, 0, held, 0) == EAGAIN && unchanged == 777 && errno == 71, "pthread capacity error"); // 第九次malloc包后创建失败，必须归还。
    struct airi_malloc_info after = airi_malloc_info(); // 一致快照与失败以前比较。
    require(after.live_objects == before.live_objects && after.live_bytes == before.live_bytes, "pthread failed packet cleanup"); // 失败路径不泄漏包。
    require(airi_event_publish(&gate) >= 0, "pthread capacity release"); // 版本前进让尚未登记的人也不睡错。
    for (unsigned i = 0; i < 8; ++i) require(pthread_join(threads[i], &output) == 0 && output == &tokens[i], "pthread capacity join"); // 八个原参数分别返回。
    for (unsigned i = 0; i < 24; ++i) { // 超过八槽总量，验证重复创建/收取与包复用。
        tokens[1].index = 1; // 上一位已经join，可以再次借出同一参数对象。
        require(pthread_create(&threads[0], 0, echo, &tokens[1]) == 0 && pthread_join(threads[0], &output) == 0 && output == &tokens[1], "pthread sequential reuse"); // 保留正确函数类型与真实生命周期。
    }
    void *reserve = malloc(262144 - 32 - 16); // 当前唯一arena减32字节块头和16字节请求头，正好占满可分配空间。
    require(reserve != 0, "pthread allocator reserve"); // 此时全部启动包均已释放，不能有阻碍整块分配的活对象。
    before = airi_malloc_info(); errno = 71; // pthread_create内部malloc将失败，但应恢复这个错误哨兵。
    unchanged = 777; require(pthread_create(&unchanged, 0, echo, &tokens[1]) == EAGAIN && unchanged == 777 && errno == 71, "pthread allocation failure preserves errno"); // 失败发生在内核创建以前。
    after = airi_malloc_info(); require(after.live_objects == before.live_objects && after.live_bytes == before.live_bytes, "pthread allocation failure ownership"); // 不伪造启动包或消耗新的worker。
    free(reserve); // 所有借用都结束以后归还唯一观察大对象。
    after = airi_malloc_info(); // 页面可缓存，启动对象不能残留。
    require(after.live_objects == 0 && after.live_bytes == 0 && after.arena_bytes == 262144 && errno == 71, "pthread final allocator and errno"); // 不把缓存页面误判成包泄漏。
    airi_puts("[PTHREAD_USER_OK] functions=typed result=pointer errno=preserved workers=34 capacity=8 failed_packet=freed alloc_failure=preserved reuse=24 attributes=unsupported\n"); // 正常、失败与资源边界实际完成后才输出。
    return 0; // shell收取真实程序结果。
}
