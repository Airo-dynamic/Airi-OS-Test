#include <pthread.h> // 使用同进程worker之间的源码join入口。
#include <airi_event.h> // 分别控制A开始join与B尝试反向边。
#include <airi_malloc.h> // 最后检查启动包已经全部归还。
#include <airi_libc.h> // 有限握手、串口与进程回收。
#include <errno.h> // 正错误和各执行者错误哨兵。
#include <stdatomic.h> // 观察进度在join以外独立发布。
static unsigned tokens[4]; // 参数/结果在main持有的稳定对象中，不借已结束工作栈。
static pthread_t peers[2]; // 主创建A/B后才发布start_a，回调不会读未完成身份。
static airi_event start_a = AIRI_EVENT_INITIALIZER, start_b = AIRI_EVENT_INITIALIZER; // 两条边分别放行。
static _Atomic unsigned attempted, cycle_done; // A准备join，B已实际收到闭环错误。
static void require(int condition, const char *message) { // 错误结束整个真实进程组。
    if (!condition) { airi_puts(message); airi_puts("\n"); airi_exit(89); } // 不继续输出成功。
}
static void gate(airi_event *event) { // 仅观察握手，不替内核join轮询结果。
    uint64_t start = airi_ticks(); // 所有重试共用一次总预算。
    while (airi_event_sequence(event) == 0) { // 允许发布早于真正WAIT。
        uint64_t elapsed = airi_ticks() - start; // PIT时间继续经过。
        require(elapsed < 200 && airi_event_wait(event, 0, (unsigned)(200 - elapsed)) == 0, "join gate timeout"); // 有限等待。
    }
}
static void await(_Atomic unsigned *word) { // 主只等观察位置，不拿目标结果。
    uint64_t start = airi_ticks(); // 独立100tick握手期限。
    while (!atomic_load_explicit(word, memory_order_acquire)) { // 对方release发布以后读取依赖状态。
        require(airi_ticks() - start < 100, "join observation timeout"); airi_yield(); // 真正让其他用户执行者运行。
    }
}
static long raw_join(pthread_t target, uintptr_t output) { // 只为观察内核输出验证，绕开用户包装的私人long槽。
    long number = 41; // 仍是自有41号，不是Linux线程ABI。
    __asm__ volatile("int $0x80" : "+a"(number) : "D"(target), "S"(output), "d"(0ull) : "memory", "cc"); // 完整64位地址进入原始边界。
    return number; // 负错误不经过errno翻译。
}
static void *simple_child(void *argument) { // 被另一个worker创建并收取。
    require(errno == 0 && airi_sleep(2) == 0, "peer child initial state"); // 新TLS和实际结束前等待。
    return argument; // 正确类型指针返回。
}
static void *simple_parent(void *argument) { // 不经主中转收取自己的子执行者。
    pthread_t child; void *result; errno = 91; // 当前执行者独立错误槽。
    require(pthread_create(&child, 0, simple_child, &tokens[0]) == 0 && pthread_join(child, &result) == 0 && result == &tokens[0] && errno == 91, "peer creates and joins"); // 内核必须让这个worker真正等待/收取。
    return argument; // 主只收取parent，不再重复收取child。
}
static void *left(void *argument) { // A先形成A->B等待边。
    gate(&start_a); errno = 92; // 两个身份已由主完整发布。
    atomic_store_explicit(&attempted, 1, memory_order_release); // 主随后给出实际登记机会。
    void *result; // B的指针结果只交给这一位收取者。
    require(pthread_join(peers[1], &result) == 0 && result == &tokens[2] && errno == 92, "peer claimed result"); // B闭环请求失败以后会正常返回。
    return argument; // A也有自己的结果供主join。
}
static void *right(void *argument) { // B在主确认A认领以后才尝试B->A。
    gate(&start_b); errno = 93; // 不与A的首次登记竞赛猜先后。
    void *result = (void *)(uintptr_t)99; // 被拒绝的边不得改结果输出。
    require(pthread_join(peers[0], &result) == EDEADLK && result == (void *)(uintptr_t)99 && errno == 93, "peer join cycle rejected"); // 内核沿A->B回到自己，不能把两人都睡下。
    atomic_store_explicit(&cycle_done, 1, memory_order_release); // 主在反向尝试以后才认领A，避免干扰这个闭环观察。
    return argument; // B没有形成新边，可以正常结束唤醒A。
}
int main(void) { // 主仍可join worker，但不再是唯一可收取者。
    errno = 71; pthread_t parent; void *result; // 主错误状态不随其他执行者改变。
    require(pthread_create(&parent, 0, simple_parent, &tokens[1]) == 0 && pthread_join(parent, &result) == 0 && result == &tokens[1], "peer nested completion"); // child由parent收取，parent由主收取。
    require(pthread_create(&peers[0], 0, left, &tokens[3]) == 0 && pthread_create(&peers[1], 0, right, &tokens[2]) == 0, "peer cycle pair create"); // 两人先等各自门。
    require(airi_event_publish(&start_a) >= 0, "peer start A"); await(&attempted); // A即将调用join B。
    require(airi_sleep(3) == 0, "peer A registers claim"); // B门仍关闭，A应真正停在41号中。
    require(raw_join(peers[1], 0xffff800000000000ull) == -14, "peer bad output cannot steal claim"); // 坏内核地址在认领以前拒绝，A资格仍保留。
    result = (void *)(uintptr_t)99; // 主尝试成为B第二位收取者。
    require(pthread_join(peers[1], &result) == EINVAL && result == (void *)(uintptr_t)99 && errno == 71, "peer duplicate claim rejected"); // 返回22，不能把B从A手中交给主。
    airi_puts("[JOIN_CLAIM_OK] competing=22 bad_output=14 unchanged=1\n"); // 两种拒绝都真实发生以后才输出。
    require(airi_event_publish(&start_b) >= 0, "peer start B"); await(&cycle_done); // B实际尝试反向边并收到35。
    require(pthread_join(peers[0], &result) == 0 && result == &tokens[3], "peer A final result"); // B先结束，A收取B，再由主收取A。
    result = (void *)(uintptr_t)99; require(pthread_join(peers[1], &result) == ESRCH && result == (void *)(uintptr_t)99, "peer result consumed exactly once"); // 当前不复用TID的诊断扩展，不能再拿一次结果。
    struct airi_malloc_info info = airi_malloc_info(); // 四位回调前均归还启动包。
    require(info.live_objects == 0 && info.live_bytes == 0 && errno == 71, "peer resources and errno"); // 没有隐藏包或错误槽污染。
    long child = airi_fork(); require(child >= 0, "peer no leftover worker lifecycle"); // 所有目标已收拢，has_workers不应留下忙资格。
    if (!child) airi_exit(0); // 普通进程复制，不把它算成第五位worker。
    int status; require(airi_waitpid(child, &status) == child && status == 0, "peer process continues"); // join图结束以后进程继续服务。
    airi_puts("[JOIN_USER_OK] peers=4 nested=joined claim=unique cycle=35 bad_output=14 result=once resources=clean\n"); // 正常、竞争、闭环和回收实际完成。
    return 0; // shell取得真实成功。
}
