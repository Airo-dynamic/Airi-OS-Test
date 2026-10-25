#ifndef AIRI_EVENT_H // 自有私有变化通知，不是eventfd或完整pthread条件变量。
#define AIRI_EVENT_H // 包含保护。
#include <stdint.h> // 等待字保持四字节。
typedef struct { uint32_t sequence; } airi_event; // 只记录变化版本；并发期间均由原子操作访问，不复制活对象。
#define AIRI_EVENT_INITIALIZER {0} // 发布以前初始化，不能重置还有等待者的对象。
#ifdef __cplusplus // 后续C++使用相同C符号。
extern "C" { // 不依赖宿主运行库。
#endif // C编译不需要语言声明。
long airi_event_sequence(airi_event *event); // 返回0至UINT32_MAX的快照或负错误；本目标long为64位。
long airi_event_wait(airi_event *event, uint32_t expected, unsigned ticks); // 0仅表示重查，-110到期；变化先于等待也不会睡错。
long airi_event_publish(airi_event *event); // 版本加一后通知至多十六位，返回实际唤醒数；饱和-75且不回绕。
#ifdef __cplusplus // 结束函数声明语言。
} // 类型可由C++聚合初始化。
#endif // 结束语言条件。
#endif // 结束包含保护。
