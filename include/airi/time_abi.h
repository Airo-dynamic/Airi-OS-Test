#ifndef AIRI_TIME_ABI_H // 单调时间数值记录，不携带内核对象或墙钟。
#define AIRI_TIME_ABI_H // 包含保护。
#include <stdint.h> // 两端按固定64位字段复制。
#define AIRI_CLOCK_MONOTONIC 1u // 自有调用目前唯一支持的时钟。
struct airi_time_value { uint64_t seconds, nanoseconds; }; // 纳秒字段始终小于十亿，分辨率仍是10ms。
_Static_assert(sizeof(struct airi_time_value) == 16, "time ABI size"); // 固定输出跨度供用户验证。
#endif // 结束包含保护。
