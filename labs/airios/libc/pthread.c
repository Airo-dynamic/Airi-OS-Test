#include <pthread.h> // 标准形状的线程函数与指针结果。
#include <airi_threads.h> // 内核真实TID、共享执行者与long结果。
#include <airi_libc.h> // 主执行者TID与PID的当前关系。
#include <stdlib.h> // 已同步的共享用户分配器。
#include <errno.h> // 返回正错误，不把pthread错误写进errno。
_Static_assert(sizeof(void *) == sizeof(long) && sizeof(long) == 8, "pthread result requires current 64-bit carrier"); // 本目标指针可完整经过已有long结果槽。
struct start_packet { void *(*function)(void *); void *argument; }; // 单独持有正确函数类型，不能把void*返回函数强转成long返回函数调用。
static long start(void *argument) { // 这个适配器才符合airi_thread_function的long返回类型。
    struct start_packet *packet = argument; // 创建成功以后只由这个新worker拥有。
    void *(*function)(void *) = packet->function; // 先复制真正目标，不在free以后再读包。
    void *value = packet->argument; // 用户参数本体仍归调用者，寿命须覆盖目标函数使用。
    free(packet); // worker在调用业务函数以前交还启动包，创建者成功后不再访问它。
    return (long)(uintptr_t)function(value); // 按原签名调用，再以当前目标64位载体交回指针位型。
}
pthread_t pthread_self(void) { // 无需每次进入内核查询。
    return (pthread_t)airi_thread_id(); // 有效TCB直接读，旧版本仍沿已有后备。
}
int pthread_equal(pthread_t left, pthread_t right) { // 本实现身份载体是整数，但应用依赖这个接口。
    return left == right; // 不设置errno。
}
int pthread_create(pthread_t *thread, const pthread_attr_t *attributes, void *(*function)(void *), void *argument) { // 默认可join线程的最小源码路径。
    if (!thread || (uintptr_t)thread % _Alignof(pthread_t) || !function) return EINVAL; // 显式坏输出/函数在分配前拒绝，其他无效用户地址仍可故障。
    if (attributes) return ENOTSUP; // 不把未实现的栈、调度或detach属性当作生效。
    if (pthread_self() != (pthread_t)airi_getpid()) return ENOTSUP; // 当前生命周期仍由主执行者管理，worker递归创建明确拒绝。
    int saved = errno; // malloc可能设置ENOMEM，但pthread返回规则不允许改调用者errno。
    struct start_packet *packet = malloc(sizeof(*packet)); // 启动包跨创建者栈寿命，不借一个很快失效的局部对象。
    if (!packet) { errno = saved; return EAGAIN; } // 资源不足通过正返回值报告。
    packet->function = function; packet->argument = argument; // 在公开新执行者以前完整准备参数。
    long tid = airi_thread_create(start, packet); // 内核验证适配器的可执行地址，目标函数仍须是调用者提供的有效函数。
    if (tid < 0) { // 新worker未发布，包仍只归创建者。
        free(packet); errno = saved; // 不泄漏失败包，恢复原错误槽。
        return tid == -ENOMEM ? EAGAIN : (int)-tid; // 内核页不足也归资源不足，其余原始负错误映成正值。
    }
    *thread = (pthread_t)tid; // 只有成功才提交身份；worker可能已运行，不能再读packet。
    errno = saved; // 创建过程的内部操作不污染调用者错误状态。
    return 0; // 表示执行者已建立，不保证函数尚未开始或已经结束。
}
int pthread_join(pthread_t thread, void **result) { // 当前不支持worker之间的join图。
    if (!thread || thread > UINT32_MAX) return ESRCH; // 不截断过宽身份命中别人。
    if (pthread_equal(thread, pthread_self())) return EDEADLK; // 自己不能等到自己结束再继续，先于主执行者限制报告。
    if (pthread_self() != (pthread_t)airi_getpid()) return ENOTSUP; // 不伪装完整POSIX的对等线程join能力。
    long value; // 内核只写这个已经存在的私人long槽。
    long error = airi_thread_join((long)thread, result ? &value : 0); // NULL仍执行真实join并回收，不创建虚假结果。
    if (error) return (int)-error; // 失败保持调用者输出，原始接口不改errno。
    if (result) *result = (void *)(uintptr_t)value; // 成功后恢复当前目标的指针表示，不返回启动包地址。
    return 0; // 目标已经结束且本次结果只能收取一次。
}
