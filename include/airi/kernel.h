#ifndef AIRI_KERNEL_H   // 首次包含内核共用接口时进入，防止重复声明
#define AIRI_KERNEL_H   // 标记本头已处理，末尾 endif 结束保护范围
#include <airi/boot.h>  // 使用双方共同的字段布局和枚举常量
#include <airi/base.h>  // 按公开声明编译自己的字节函数，保证定义与调用使用同一签名

void airi_serial_init(void);  // 配置 COM1 UART，之后数据端口才能按当前格式发送字符
void airi_serial_write(const char *s);  // 同步发送 NUL 之前的字节，不保留调用者指针
void airi_log(const char *fmt, ...);    // 在局部数组形成日志，再通过串口逐字发送
_Noreturn void airi_halt(void);  // 使当前 CPU 停在不返回的循环中，QEMU 主机进程仍存在
_Noreturn void airi_panic(const char *message);      // 先报告具体错误再停机，参数须为有效 NUL 字符串
bool airi_dashboard_draw(const AiriBootInfo *info);  // 预留图形接口，当前入口未调用，因此本章无需提供定义
#endif  // 结束头文件包含保护，后续使用同一公开声明
