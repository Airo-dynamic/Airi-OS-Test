#include <airi/storage.h> // 进程和控制台只消费数值结果。
#include <airi/journal.h> // 完整提交与恢复协议。
#include <airi/block.h> // 固定单设备后端。
#include <airi/kernel.h> // 实际恢复记录。
static struct airi_journal store; // 内核拥有，跨用户进程退出保留。
static bool available; // 初始化失败或设备受限不能伪造空日志。
static int read_sector(void *context, uint64_t sector, void *data) { // 窄同步适配不包含用户指针。
    (void)context; // 当前只有一块实验盘。
    return airi_block_read(sector, data); // 真实VirtIO完成。
}
static int write_sector(void *context, uint64_t sector, const void *data) { // 算法写入独立正式区域。
    (void)context; // 单设备无需额外状态。
    return airi_block_write(sector, data); // 错误保持原样。
}
static int flush(void *context) { // 不以普通write完成代替同步。
    (void)context; // 接口形状与日志后端一致。
    return airi_block_flush(); // 虚拟后端FLUSH。
}
int airi_storage_init(void) { // 正常启动只扫描，不偷偷新增记录。
    available = false; // 旧内存状态不可当恢复结果。
    if (!airi_block_sectors()) { // 超时隔离后不能继续读盘。
        airi_log("[STORAGE_SKIPPED] block_unavailable=1"); // 明确本阶段未提供磁盘服务。
        return 0; // 故障配置可继续非存储命令。
    }
    struct airi_journal_io io = {NULL, read_sector, write_sector, flush, airi_block_sectors()}; // 函数与静态设备在整个启动期有效。
    int error = airi_journal_scan(&store, &io, 16, 64); // 正式区占16–143，避开演示扇区与盘末尾。
    if (error) // 真实I/O错误不能当新空盘。
        return error; // main决定报告并停止。
    available = true; // 完整恢复后才接受请求。
    airi_log("[STORAGE_RECOVERED] count=%u truncated=%u readonly=%u", store.count, store.truncated, airi_block_readonly()); // 字段来自扫描与真实设备特征。
    return 0; // 空日志与已有前缀都合法。
}
long airi_storage_count(void) { // 用户取得数值，不持有store指针。
    return !available ? -19 : store.poisoned ? -5 : (long)store.count; // 缺设备、不确定与可用分开。
}
long airi_storage_append(const void *data, size_t bytes) { // 当前同步副本不跨调用保存。
    if (!available) // 超时设备不能继续发布请求。
        return -19; // -ENODEV。
    if (airi_block_readonly()) // 只读状态查询不破坏已恢复前缀。
        return -30; // -EROFS，不把正常只读拒绝标成不确定写。
    return airi_journal_append(&store, data, bytes); // 数据、提交和同步均完成后才成功。
}
long airi_storage_read(unsigned index, void *data, size_t capacity) { // 单条记录不是RAM文件偏移接口。
    return available ? airi_journal_read(&store, index, data, capacity) : -19; // 返回实际长度或明确错误。
}
