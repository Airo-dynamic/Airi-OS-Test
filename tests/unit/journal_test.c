#include <airi/journal.h> // 同一日志算法，可替换块后端模拟同步边界。
#include <assert.h> // 验证数据与故障后状态。
#include <stdio.h> // 只发布完整结果。
struct model { uint8_t live[8][512], durable[8][512]; unsigned operation, fail; bool after; }; // live是易失缓存，durable是模型同步介质。
static struct model disk, saved; // 静态避免把大数组压进测试栈。
static int fault(void) { // 在特定写或flush之前/之后返回错误。
    return ++disk.operation == disk.fail ? -5 : 0; // fail0表示不注入。
}
static int read_sector(void *context, uint64_t sector, void *data) { // 读当前可见缓存，重启时由durable恢复。
    (void)context; // 测试只用一个模型。
    assert(sector < 8); // 算法不能越后端容量。
    airi_memcpy(data, disk.live[sector], 512); // 同步读完整扇区。
    return 0; // 本测试I/O错误在写和flush注入。
}
static int write_sector(void *context, uint64_t sector, const void *data) { // 模拟写完成但未必持久。
    (void)context; // 使用明确模型对象。
    assert(sector < 8); // 不允许越界写。
    int error = fault(); // 每次写计入故障位置。
    if (!error || disk.after) // after错误表示副作用已经发生，但调用者收到失败。
        airi_memcpy(disk.live[sector], data, 512); // 尚未到durable。
    return error; // 应用不能假定失败意味着什么都没写。
}
static int flush(void *context) { // 模拟同步完成时刻。
    (void)context; // 当前固定后端。
    int error = fault(); // flush也可能失败或结果不确定。
    if (!error || disk.after) // after故障可表示已持久但没有向应用确认。
        airi_memcpy(disk.durable, disk.live, sizeof(disk.live)); // 全部之前写入进入模型介质。
    return error; // 算法必须停止自动追加。
}
static void crash(void) { // 模型掉电丢易失缓存，不修改持久副本。
    airi_memcpy(disk.live, disk.durable, sizeof(disk.live)); // 重启后的读只看到已同步状态。
    disk.fail = 0; // 新启动不自动继续旧注入。
    disk.operation = 0; // 单次模型运行独立计数。
}
int main(void) { // 模型能验证顺序，不替代真实主机掉电实验。
    struct airi_journal_io io = {NULL, read_sector, write_sector, flush, 8}; // 四条记录容量。
    struct airi_journal journal; // 算法缓存对象会在每次scan重新建立。
    char data[16]; // 校验输出，容量失败不能部分复制。
    assert(airi_journal_scan(&journal, &io, 0, 4) == 0 && journal.count == 0); // 零盘是空日志。
    assert(airi_journal_append(&journal, "alpha", 5) == 1); // 初始已确认记录。
    saved = disk; // 每个故障从相同durable前缀开始。
    for (unsigned after = 0; after < 2; ++after) // 分别在副作用前报错和副作用后报错。
        for (unsigned operation = 1; operation <= 8; ++operation) { // 失效、空尾、数据和提交同步八步。
            disk = saved; // 不把前一个模型失败污染下一轮。
            disk.operation = 0; // 本次追加操作重新计数。
            disk.fail = operation; // 选一个真实算法调用注入。
            disk.after = after != 0; // 模拟是否已发生该步副作用。
            assert(!airi_journal_scan(&journal, &io, 0, 4)); // 缓存先从介质状态恢复。
            assert(airi_journal_append(&journal, "beta", 4) == -5 && journal.poisoned); // 错误必须返回且停止复用未确定状态。
            assert(airi_journal_append(&journal, "duplicate", 9) == -5); // 不自动重试造成重复。
            crash(); // 只保留模型已经同步的字节。
            assert(!airi_journal_scan(&journal, &io, 0, 4)); // 重新推导可见前缀。
            assert(journal.count == (after && operation == 8 ? 2u : 1u)); // 最后flush已生效但报错时新记录可能存在。
            assert(airi_journal_read(&journal, 0, data, sizeof(data)) == 5 && !airi_memcmp(data, "alpha", 5)); // 每条已确认旧记录必须保留。
        }
    disk = saved; // 正常再建三条记录。
    disk.fail = 0; // 无故障注入。
    assert(!airi_journal_scan(&journal, &io, 0, 4)); // 恢复初始alpha。
    assert(airi_journal_append(&journal, "beta", 4) == 2 && airi_journal_append(&journal, "old-tail", 8) == 3); // 后面有旧有效尾。
    disk.durable[2][32] ^= 1; // 第二记录payload损坏，第三记录依然完整。
    crash(); // 从损坏介质重启。
    assert(!airi_journal_scan(&journal, &io, 0, 4) && journal.count == 1 && journal.truncated); // 停在坏记录，不跳到第三。
    assert(airi_journal_append(&journal, "replacement", 11) == 2); // 下一空尾先清掉旧第三记录。
    crash(); // 再次恢复不能让旧尾复活。
    assert(!airi_journal_scan(&journal, &io, 0, 4) && journal.count == 2); // 新前缀仅alpha和replacement。
    assert(airi_journal_read(&journal, 1, data, 2) == -34); // 容量不足不截断记录。
    assert(airi_journal_read(&journal, 1, data, sizeof(data)) == 11 && !airi_memcmp(data, "replacement", 11)); // 坏尾替换可读。
    assert(airi_journal_append(&journal, "c", 1) == 3 && airi_journal_append(&journal, "d", 1) == 4 && airi_journal_append(&journal, "e", 1) == -28); // 满容量不会环绕覆盖。
    puts("PASS journal: 16 crash/error boundaries, uncertain final flush, CRC prefix and no tail resurrection"); // 模型结论与真设备演示分别记录。
    return 0; // 宿主测试成功。
}
