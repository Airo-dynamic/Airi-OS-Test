#ifndef AIRI_QUEUE_H // 有界值队列，不接收借用指针或执行回调。
#define AIRI_QUEUE_H // 包含保护。
#include <airi_mutex.h> // 普通队列字段由一把所有权锁保护。
#include <airi_event.h> // 非空和有空位分别通知，不把通知当任务令牌。
#define AIRI_QUEUE_CAPACITY 4u // 固定四项，只限制排队数量，不代表计算或线程上限。
typedef struct { // 初始化后通过本接口访问，不复制或重置活队列。
    airi_mutex guard; // 保护下面的环位置、个数、关闭状态和槽内容。
    airi_event available, space; // 通知只要求重查非空/非满谓词。
    uint64_t values[AIRI_QUEUE_CAPACITY]; // 队列按值持有64位任务标识，不拥有外部对象。
    unsigned head, count, closed; // head为首项下标，count含尚未取出的项，closed拒绝后续put。
} airi_queue; // 所有使用者结束以前保持同一有效地址。
#define AIRI_QUEUE_INITIALIZER {AIRI_MUTEX_INITIALIZER, AIRI_EVENT_INITIALIZER, AIRI_EVENT_INITIALIZER, {0}, 0, 0, 0} // 在发布以前建立空环和开放状态。
#ifdef __cplusplus // 对未来C++调用保留C符号。
extern "C" { // 不使用宿主容器或线程类型。
#endif // 结束语言差异。
long airi_queue_put(airi_queue *queue, uint64_t value, unsigned ticks); // 0已入队；满且零预算-11，到期-110，关闭-32。
long airi_queue_get(airi_queue *queue, uint64_t *value, unsigned ticks); // 1取得并写私人输出，0关闭且排空；其他失败保持输出，输出不与队列重叠。
long airi_queue_close(airi_queue *queue, unsigned ticks); // 0关闭或原已关闭，唤醒生产/消费者；保留已排队项供排空。
#ifdef __cplusplus // 结束函数的C链接范围。
} // 类型仍按普通聚合布局。
#endif // 结束语言差异。
#endif // 结束包含保护。
