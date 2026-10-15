#include <airi/journal.h> // 追加与恢复算法。
#include <airi/block.h> // 固定QEMU后端。
#include <airi/kernel.h> // 本轮实际记录。
static int read_sector(void *context, uint64_t sector, void *data) { // 算法只借同步块服务。
    (void)context; // 当前只有一个设备，无额外实例状态。
    return airi_block_read(sector, data); // 返回真实设备完成或错误。
}
static int write_sector(void *context, uint64_t sector, const void *data) { // 不在算法内访问PCI。
    (void)context; // 与read相同后端。
    return airi_block_write(sector, data); // 副本在驱动内由DMA持有。
}
static int flush(void *context) { // 每个提交边界都实际执行VirtIO FLUSH。
    (void)context; // 不需要上下文。
    return airi_block_flush(); // 错误原样交回。
}
void airi_journal_demo(void) { // 只占用4–7扇区，与后续正式日志区分开。
    if (!airi_block_sectors() || airi_block_readonly()) { // 超时隔离或只读故障镜像不能尝试写演示。
        airi_log("[JOURNAL_SKIPPED] writable_block=0"); // 不伪装日志验证成功。
        return; // 原设备故障已由对应配置报告。
    }
    struct airi_journal_io io = {NULL, read_sector, write_sector, flush, airi_block_sectors()}; // 算法通过窄后端接入真实磁盘。
    struct airi_journal journal; // 恢复得到的计数仅在当前内核栈上。
    uint8_t saved[4][512], empty[512] = {0}, data[512]; // 保存原实验扇区，演示结束恢复。
    for (unsigned i = 0; i < 4; ++i) // 先全部保存再改任何扇区。
        if (airi_block_read(4 + i, saved[i])) // 保存失败不能继续覆盖。
            airi_panic("journal demo could not preserve scratch sectors"); // 不使用未初始化备份。
    for (unsigned i = 0; i < 4; ++i) // 构造可复现的空演示区。
        if (airi_block_write(4 + i, empty)) // 真实设备写，不修改宿主文件代替客体操作。
            airi_panic("journal demo could not clear scratch sectors"); // 设备错误明确停止。
    if (airi_block_flush() || airi_journal_scan(&journal, &io, 4, 2) || journal.count || airi_journal_append(&journal, "alpha", 5) != 1 || airi_journal_append(&journal, "beta", 4) != 2) // 两条真正提交并同步。
        airi_panic("journal append sequence failed"); // 不只核对内存计数。
    if (airi_journal_scan(&journal, &io, 4, 2) || journal.count != 2 || airi_journal_read(&journal, 1, data, sizeof(data)) != 4 || airi_memcmp(data, "beta", 4)) // 重新从设备恢复，清掉旧内存状态。
        airi_panic("journal committed records did not recover"); // 恢复算法必须实际读盘。
    if (airi_block_read(6, data)) // 第二条数据扇区。
        airi_panic("journal corruption setup read failed"); // 不修改未知内容。
    data[32] ^= 1; // 模拟已落盘记录的一个payload位损坏，原commit仍保留。
    if (airi_block_write(6, data) || airi_block_flush()) // 损坏真实写到实验盘。
        airi_panic("journal corruption setup write failed"); // 故障准备也要真实完成。
    if (airi_journal_scan(&journal, &io, 4, 2) || journal.count != 1 || !journal.truncated || airi_journal_read(&journal, 1, data, sizeof(data)) != -2) // 只恢复到第一条，不跳过坏记录。
        airi_panic("journal did not truncate corrupt tail"); // 不把损坏当空记录继续。
    if (airi_journal_append(&journal, "gamma", 5) != 2 || airi_journal_scan(&journal, &io, 4, 2) || journal.count != 2 || airi_journal_read(&journal, 1, data, sizeof(data)) != 5 || airi_memcmp(data, "gamma", 5)) // 覆盖坏尾后同样可重新恢复。
        airi_panic("journal tail replacement failed"); // 旧commit不能错误承认新数据。
    for (unsigned i = 0; i < 4; ++i) // 实验结束恢复原扇区。
        if (airi_block_write(4 + i, saved[i])) // 仍走同一真实设备服务。
            airi_panic("journal demo could not restore scratch sectors"); // 不隐瞒清理失败。
    if (airi_block_flush()) // 恢复请求也需要同步。
        airi_panic("journal scratch restore flush failed"); // 不能以写完成冒充恢复已同步。
    airi_log("[JOURNAL_DEMO_OK] committed=2 corrupt_tail=1 prefix=1 replaced=gamma restored=1"); // 区分受控位损坏与真实掉电。
}
