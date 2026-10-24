#ifndef AIRI_TLS_H // 用户FS状态只通过架构接口交接。
#define AIRI_TLS_H // 防止重复声明。
#include <stdbool.h> // 初始化能力检查返回布尔值。
#include <airi/base.h> // 交接的基址使用固定宽度整数。
bool airi_tls_init(void); // 关闭FSGSBASE直写并将内核FS基址清零，尚未创建用户。
uint64_t airi_tls_take(void); // 读取被打断用户的真实FS基址，再清零活动硬件状态。
void airi_tls_restore(uint64_t base); // 只恢复已验证的低半区数值，不映射或固定任何用户页。
#endif // 结束包含保护。
