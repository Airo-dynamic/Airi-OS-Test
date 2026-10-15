#ifndef AIRI_JOURNAL_H // 简单追加日志与具体块设备分开。
#define AIRI_JOURNAL_H // 包含保护。
#include <airi/base.h> // 有界字节和固定宽度。
#include <stdbool.h> // 恢复和不确定结果状态。
#define AIRI_JOURNAL_PAYLOAD 480u // 一个数据扇区32字节头加480字节内容。
struct airi_journal_io { // 回调持有者的寿命必须覆盖整个日志使用期。
    void *context; // 块后端自有状态，用户不接触。
    int (*read)(void *, uint64_t, void *); // 精确读512字节，0成功。
    int (*write)(void *, uint64_t, const void *); // 精确写512字节，0成功。
    int (*flush)(void *); // 确认之前写入同步完成。
    uint64_t sectors; // 后端容量，范围在任何I/O前校验。
};
struct airi_journal { // 只缓存已确认前缀，不缓存用户buffer。
    struct airi_journal_io io; // 借后端context，复制函数指针和容量。
    uint64_t base; // 首记录数据扇区，commit紧随其后。
    unsigned slots, count; // 固定记录容量与已恢复有效前缀。
    bool truncated, poisoned; // 尾记录无效可覆盖；I/O错误后需重新扫描。
};
int airi_journal_scan(struct airi_journal *journal, const struct airi_journal_io *io, uint64_t base, unsigned slots); // 从磁盘重建有效前缀，最多64条。
long airi_journal_append(struct airi_journal *journal, const void *data, size_t bytes); // 返回一开始的sequence，失败负错误且不自动重试。
long airi_journal_read(struct airi_journal *journal, unsigned index, void *data, size_t capacity); // index从0开始，成功返回实际内容长度。
void airi_journal_demo(void); // 正常设备上的真实恢复演示，独立占用扇区4–7。
#endif // 结束保护。
