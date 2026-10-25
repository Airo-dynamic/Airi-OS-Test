#ifndef AIRI_RUNTIME_ABI_H // 当前静态TLS运行块头的自有数值布局。
#define AIRI_RUNTIME_ABI_H // 包含保护，不引入内核对象指针。
#include <stdint.h> // 用户和内核都使用相同固定宽度。
#define AIRI_RUNTIME_VERSION 1u // 版本0仍可由用户库退回原始身份查询。
struct airi_runtime_tcb { // 占用既有TP之后的16字节，不扩大TLS运行页数。
    uint64_t self; // 当前执行者TP的用户虚拟地址，不是物理地址或权限令牌。
    uint32_t tid; // 内核在真正发布执行者以前写入其非零身份。
    uint32_t version; // 只描述这16字节布局，不代表完整pthread或Linux TCB。
};
#endif // 结束包含保护。
