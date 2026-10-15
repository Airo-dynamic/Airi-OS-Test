#ifndef AIRI_IO_ABI_H // 未来用户与内核共享的自有固定宽度记录，不含内部指针。
#define AIRI_IO_ABI_H // 包含保护。
#include <stdint.h> // 明确字段宽度，避免宿主long大小影响布局。
#define AIRI_IO_READ 0u // 固定512字节块读取。
#define AIRI_IO_WRITE 1u // 固定512字节块写入。
#define AIRI_IO_FLUSH 4u // 设备同步，无数据缓冲。
struct airi_io_submission { uint64_t sector, cookie; }; // 用户描述只含数值，缓冲句柄单独传入。
struct airi_io_completion { // 结果归属于明确cookie与buffer，不按完成顺序猜身份。
    uint64_t cookie, buffer; // 用户请求身份与注册缓冲句柄。
    int64_t result; // 读写成功512，FLUSH成功0，错误负数。
    uint32_t operation, reserved; // 操作类型与必须为零的保留字段。
};
_Static_assert(sizeof(struct airi_io_submission) == 16, "submission ABI size"); // 读用户结构前固定字节数。
_Static_assert(sizeof(struct airi_io_completion) == 32, "completion ABI size"); // CQ输出不能随编译器结构变化。
#endif // 结束保护。
