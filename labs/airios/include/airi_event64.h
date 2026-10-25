#ifndef AIRI_EVENT64_H // 宽变化序号，不将最高位值混成负错误。
#define AIRI_EVENT64_H // 包含保护。
#include <stdint.h> // 完整64位版本及指针宽度。
typedef struct { uint64_t sequence; } airi_event64; // 并发期间只用原子操作访问，存活期间不移动/重置。
#define AIRI_EVENT64_INITIALIZER {0} // 在发布给任何执行者以前构造。
#ifdef __cplusplus // C++可以使用同一形状，不依赖宿主运行库。
extern "C" { // 保持C链接符号。
#endif // C直接声明。
long airi_event64_snapshot(airi_event64 *event, uint64_t *value); // 成功0且写私人不重叠输出，失败负错误且保持输出。
long airi_event64_wait(airi_event64 *event, uint64_t expected, unsigned ticks); // 0只表示重查，-110到期，完整64位比较。
long airi_event64_advance(airi_event64 *event); // 仅推进版本，0成功，饱和-75不回绕；不通知已登记者。
long airi_event64_notify(airi_event64 *event, unsigned count); // 仅通知至多count<=16人，不改版本，返回真实人数，0数量为空操作。
#ifdef __cplusplus // 结束函数链接语言。
} // 对象布局为当前项目ABI。
#endif // 结束语言差异。
#endif // 结束包含保护。
