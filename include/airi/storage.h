#ifndef AIRI_STORAGE_H // 用户存储服务不暴露日志后端结构。
#define AIRI_STORAGE_H // 包含保护。
#include <airi/base.h> // 字节容量和返回值。
int airi_storage_init(void); // 从正式日志区16起恢复，设备受限时明确跳过。
long airi_storage_count(void); // 已确认前缀或负错误。
long airi_storage_append(const void *data, size_t bytes); // 同步追加，成功返回sequence。
long airi_storage_read(unsigned index, void *data, size_t capacity); // 索引从0，失败不复制输出。
#endif // 结束保护。
