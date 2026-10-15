#include <airi/journal.h> // 格式与有界后端协议，不包含PCI。
static uint32_t get32(const uint8_t *p) { // 文件格式固定little endian，不依赖C结构填充。
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; // 每字节先扩到足够宽再移位。
}
static void put32(uint8_t *p, uint32_t value) { // 显式编码四字节。
    for (unsigned i = 0; i < 4; ++i) // 低字节在前。
        p[i] = (uint8_t)(value >> (8 * i)); // 不写未对齐uint32_t指针。
}
static uint64_t get64(const uint8_t *p) { // sequence使用64位。
    return get32(p) | (uint64_t)get32(p + 4) << 32; // 两个固定宽度字段合并。
}
static void put64(uint8_t *p, uint64_t value) { // 编码sequence，当前最多64条但格式明确。
    put32(p, (uint32_t)value); // 低32位。
    put32(p + 4, (uint32_t)(value >> 32)); // 高32位。
}
static uint32_t crc32(const uint8_t *data, size_t bytes) { // 反射CRC32检测意外损坏，不是密码学完整性。
    uint32_t crc = 0xffffffffu; // 标准初值。
    for (size_t i = 0; i < bytes; ++i) { // 每字节进入低位。
        crc ^= data[i]; // 合并当前输入。
        for (unsigned bit = 0; bit < 8; ++bit) // 每位一次多项式递推。
            crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0); // 固定IEEE多项式反射表示。
    }
    return ~crc; // 最终取反。
}
static uint32_t checksum(uint8_t sector[512]) { // checksum字段自身不参与递归计算。
    uint32_t stored = get32(sector + 20); // 暂存原字段。
    put32(sector + 20, 0); // 按格式约定以零计算。
    uint32_t result = crc32(sector, 512); // 包含头、payload和补零尾。
    put32(sector + 20, stored); // 调用者内容保持不变。
    return result; // 与保存值比较或写入新扇区。
}
static bool all_zero(const uint8_t *data, size_t bytes) { // 仅完整零对才视为干净空尾。
    for (size_t i = 0; i < bytes; ++i) // 不只看magic首字节。
        if (data[i]) // 有残留就是可能撕裂的尾。
            return false; // 交给恢复规则报告截断。
    return true; // 整个区域未包含记录字节。
}
static bool valid(uint8_t data[512], uint8_t commit[512], uint64_t sequence) { // 两个扇区绑定到同一记录身份和内容。
    return !airi_memcmp(data, "AIRIDAT1", 8) && !airi_memcmp(commit, "AIRICOM1", 8) && get64(data + 8) == sequence && get64(commit + 8) == sequence && get32(data + 16) <= AIRI_JOURNAL_PAYLOAD && get32(data + 20) == checksum(data) && get32(commit + 16) == get32(data + 20) && get32(commit + 20) == checksum(commit); // 序号、范围、数据CRC和提交CRC都必须成立。
}
int airi_journal_scan(struct airi_journal *journal, const struct airi_journal_io *io, uint64_t base, unsigned slots) { // io可来自journal.io本身，先复制再覆盖。
    if (!journal || !io || !io->read || !io->write || !io->flush || !slots || slots > 64 || base > io->sectors || slots > (io->sectors - base) / 2) // 任何I/O前检查范围与接口。
        return -22; // -EINVAL，旧对象不修改。
    struct airi_journal_io backend = *io; // 避免输入与输出同对象时被清掉。
    *journal = (struct airi_journal){.io = backend, .base = base, .slots = slots}; // 恢复只从磁盘事实开始。
    uint8_t data[512], commit[512]; // 有界内核副本或主机夹具副本。
    for (unsigned i = 0; i < slots; ++i) { // 仅接受从第0条开始的连续有效前缀。
        int error = backend.read(backend.context, base + 2ull * i, data); // 数据扇区读取失败不能当空尾。
        if (!error) // 先前读成功才取提交扇区。
            error = backend.read(backend.context, base + 2ull * i + 1, commit); // 同一记录的完成声明。
        if (error) { // 存储错误与格式截断分别处理。
            journal->poisoned = true; // 结果不完整，不接受新追加。
            return error; // 保留具体错误，不能宣布前缀恢复完成。
        }
        if (all_zero(data, 512) && all_zero(commit, 512)) // 全零空尾。
            return 0; // 后续区域不跳过扫描，不寻找离散有效记录。
        if (!valid(data, commit, i + 1ull)) { // 数据不全、提交缺失或校验错误。
            journal->truncated = true; // 逻辑截断到上一条，未物理清盘。
            return 0; // 后续append先失效旧commit再覆盖这个尾槽。
        }
        ++journal->count; // 只有完整两扇区验证后才增长可见前缀。
    }
    return 0; // 所有槽均有效，下一append明确返回空间不足。
}
long airi_journal_append(struct airi_journal *journal, const void *payload, size_t bytes) { // 同步事务，不保留payload。
    if (!journal || !journal->io.write || !journal->io.flush || journal->poisoned) // 未初始化或前次结果不确定。
        return -5; // -EIO，必须重新扫描真实磁盘。
    if (!bytes || bytes > AIRI_JOURNAL_PAYLOAD || !payload) // 记录范围固定，空记录暂不支持。
        return -22; // -EINVAL。
    if (journal->count == journal->slots) // 只追加，不环绕覆盖已提交记录。
        return -28; // -ENOSPC。
    uint8_t data[512] = {0}, commit[512] = {0}; // 所有补零参与校验，不泄漏栈字节。
    uint64_t sequence = journal->count + 1ull; // 记录身份与前缀位置严格对应。
    uint64_t sector = journal->base + 2ull * journal->count; // 每条固定两扇区，不跨未验证区域。
    int error = journal->io.write(journal->io.context, sector + 1, commit); // 复用坏尾之前先失效可能存在的旧提交。
    if (!error && journal->count + 1 < journal->slots) // 恢复坏尾后不能让旧的后续记录重新连回前缀。
        error = journal->io.write(journal->io.context, sector + 2, commit); // 下一条数据先清零，形成明确空尾。
    if (!error && journal->count + 1 < journal->slots) // 当前槽之后仍有可用记录位。
        error = journal->io.write(journal->io.context, sector + 3, commit); // 下一条commit也清零，不保留旧有效尾。
    if (!error) // 当前commit失效与下一条空尾必须先达到同步边界。
        error = journal->io.flush(journal->io.context); // 后续新数据不能配上旧commit。
    airi_memcpy(data, "AIRIDAT1", 8); // 数据magic和格式版本。
    put64(data + 8, sequence); // 显式little endian身份。
    put32(data + 16, (uint32_t)bytes); // 有界payload长度。
    airi_memcpy(data + 32, payload, bytes); // 本次调用内复制完，不借用户页。
    uint32_t data_crc = checksum(data); // CRC字段计算时视作零。
    put32(data + 20, data_crc); // 数据扇区自校验。
    if (!error) // 只有commit失效已确认后才写新数据。
        error = journal->io.write(journal->io.context, sector, data); // 512字节数据请求。
    if (!error) // 数据先于提交持久化。
        error = journal->io.flush(journal->io.context); // 不依赖写请求的完成顺序冒充落盘顺序。
    airi_memcpy(commit, "AIRICOM1", 8); // 独立提交扇区。
    put64(commit + 8, sequence); // 绑定本记录身份。
    put32(commit + 16, data_crc); // 绑定具体内容，不能只看有一个commit字样。
    put32(commit + 20, checksum(commit)); // commit本身也必须完整。
    if (!error) // 数据同步已完成才发提交。
        error = journal->io.write(journal->io.context, sector + 1, commit); // 发布记录提交标志。
    if (!error) // 最后一次同步确认之前不能向应用成功。
        error = journal->io.flush(journal->io.context); // 提交确认完成才增长内存前缀。
    if (error) { // 可能有部分内容已到盘，甚至commit已到盘但同步报错。
        journal->poisoned = true; // 不自动重试造成重复记录。
        return error; // 调用者知道失败，但恢复后记录是否存在仍不确定。
    }
    ++journal->count; // 只有全部步骤成功才确认本次追加。
    journal->truncated = false; // 原坏尾已被新提交覆盖。
    return (long)sequence; // 成功返回从1开始的记录身份。
}
long airi_journal_read(struct airi_journal *journal, unsigned index, void *payload, size_t capacity) { // 单条读取，输出仅在验证后写。
    if (!journal || !journal->io.read || journal->poisoned) // 不读取不完整恢复状态。
        return -5; // -EIO。
    if (index >= journal->count) // 尚未确认的尾不可见。
        return -2; // -ENOENT。
    uint8_t data[512], commit[512]; // 不直接向用户写设备内容。
    int error = journal->io.read(journal->io.context, journal->base + 2ull * index, data); // 重新读取实际内容。
    if (!error) // 数据I/O完成才读提交。
        error = journal->io.read(journal->io.context, journal->base + 2ull * index + 1, commit); // 同记录另一扇区。
    if (error || !valid(data, commit, index + 1ull)) { // 已确认前缀之后损坏不能静默返回缓存。
        journal->poisoned = true; // 后续需完整重新扫描。
        return error ? error : -74; // -EBADMSG表示格式校验失败。
    }
    size_t bytes = get32(data + 16); // 验证后长度不超过480。
    if (bytes > capacity || (!payload && bytes)) // 不部分复制并误当完整记录。
        return -34; // -ERANGE，日志状态仍可用。
    airi_memcpy(payload, data + 32, bytes); // 同步输出后不保留指针。
    return (long)bytes; // 调用者取得精确长度。
}
