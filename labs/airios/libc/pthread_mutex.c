#include <pthread.h> // 默认ERRORCHECK映射、属性与初始化状态。
#include <errno.h> // pthread直接返回正错误号。
static int aligned(const void *pointer, unsigned alignment) { // 只检查显式形状，不能验证任意映射或对象寿命。
    return pointer && !((uintptr_t)pointer % alignment); // 参数保持有效仍由调用者保证。
}
static int attribute_valid(const pthread_mutexattr_t *attributes) { // 属性变更和销毁必须由调用者排除并发。
    return aligned(attributes, _Alignof(pthread_mutexattr_t)) && attributes->ready == 1; // 不把销毁后的属性当默认。
}
static int initialized(pthread_mutex_t *mutex) { // 并发锁操作只读同一可用标记，不与init/destroy竞赛。
    return aligned(mutex, _Alignof(pthread_mutex_t)) && __atomic_load_n(&mutex->state, __ATOMIC_ACQUIRE) == AIRI_PTHREAD_MUTEX_READY; // 状态不是内核授权。
}
int pthread_mutexattr_init(pthread_mutexattr_t *attributes) { // 不读取未初始化对象的旧内容。
    if (!aligned(attributes, _Alignof(pthread_mutexattr_t))) return EINVAL; // 显式坏形状先拒绝。
    *attributes = (pthread_mutexattr_t){PTHREAD_MUTEX_DEFAULT, 1}; // 独占构造，类型与可用标记一起形成。
    return 0; // 不申请内存、不改errno。
}
int pthread_mutexattr_destroy(pthread_mutexattr_t *attributes) { // 属性无需跟随已初始化mutex一起存活。
    if (!attribute_valid(attributes)) return EINVAL; // 当前实现检测销毁后的再次使用。
    attributes->ready = 0; // 调用者已确保无人并发读写此属性。
    return 0; // 类型字段不再授权后续操作。
}
int pthread_mutexattr_gettype(const pthread_mutexattr_t *attributes, int *type) { // 输出只在成功以后改动。
    if (!attribute_valid(attributes) || !aligned(type, _Alignof(int))) return EINVAL; // 不把失败写成一个伪造默认类型。
    *type = attributes->type; // 私人有效输出，不能与属性重叠。
    return 0; // 正常返回不触碰errno。
}
int pthread_mutexattr_settype(pthread_mutexattr_t *attributes, int type) { // 当前只实现一种所有权策略。
    if (!attribute_valid(attributes)) return EINVAL; // 坏属性不是未支持类型。
    if (type == PTHREAD_MUTEX_NORMAL || type == PTHREAD_MUTEX_RECURSIVE) return ENOTSUP; // 已知但尚未实现，不改旧属性。
    if (type != PTHREAD_MUTEX_ERRORCHECK) return EINVAL; // DEFAULT与ERRORCHECK是同一个本项目值。
    attributes->type = type; // 全部检查成功以后才提交。
    return 0; // 不修改其他对象。
}
int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attributes) { // 调用者必须在发布以前或销毁后静止地初始化。
    if (!aligned(mutex, _Alignof(pthread_mutex_t)) || (attributes && !attribute_valid(attributes))) return EINVAL; // 不读mutex旧状态，不以未初始化字节猜是否活锁。
    if (attributes && attributes->type != PTHREAD_MUTEX_ERRORCHECK) return ENOTSUP; // 不接入手改的其他策略。
    mutex->core = (airi_mutex)AIRI_MUTEX_INITIALIZER; // 当前独占构造，不与正在使用的锁并发。
    __atomic_store_n(&mutex->state, AIRI_PTHREAD_MUTEX_READY, __ATOMIC_RELEASE); // 形成可用标记，属性指针不保存到对象。
    return 0; // 成功为空闲互斥。
}
int pthread_mutex_destroy(pthread_mutex_t *mutex) { // 调用者已经停止所有将来的锁访问，检查不能替代这个寿命约定。
    if (!initialized(mutex)) return EINVAL; // 当前诊断已销毁状态。
    if (__atomic_load_n(&mutex->core.owner, __ATOMIC_SEQ_CST) || __atomic_load_n(&mutex->core.contenders, __ATOMIC_SEQ_CST)) return EBUSY; // 活所有权或慢路径都不能撤销标记。
    __atomic_store_n(&mutex->state, 0, __ATOMIC_RELEASE); // 仅静止对象销毁，底层两字段已为0。
    return 0; // 可在之后无并发地重新init。
}
int pthread_mutex_lock(pthread_mutex_t *mutex) { // POSIX形状的lock没有调用者给出的总超时。
    if (!initialized(mutex)) return EINVAL; // 不把销毁后的对象当一把新锁。
    long result; // 内部原始负错误，与公开正错误分开。
    do result = airi_mutex_lock(&mutex->core, 200); // 每次底层等待有界，超时不等于公开lock失败。
    while (result == -ETIMEDOUT); // 持有者仍可继续运行释放，持续争取直到取得或明确其他错误。
    return result < 0 ? (int)-result : 0; // self重入变EDEADLK，成功才持有普通数据访问权。
}
int pthread_mutex_trylock(pthread_mutex_t *mutex) { // 非阻塞只尝试一次。
    if (!initialized(mutex)) return EINVAL; // 未生效对象不作原子争取。
    long result = airi_mutex_trylock(&mutex->core); // 自己已持有也为忙，不递归计数。
    return result < 0 ? (int)-result : 0; // EBUSY或成功0，errno不变。
}
int pthread_mutex_unlock(pthread_mutex_t *mutex) { // 释放仍依赖底层TID所有权。
    if (!initialized(mutex)) return EINVAL; // 不绕过销毁标记。
    long result = airi_mutex_unlock(&mutex->core); // 普通写发布及参与者通知沿已有协议。
    return result < 0 ? (int)-result : 0; // 外人或空锁为EPERM，已经释放的通知错误仍如实报告。
}
