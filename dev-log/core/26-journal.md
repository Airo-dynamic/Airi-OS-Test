# K26 · 一条追加记录什么时候才算提交，坏尾怎样恢复

K25 能确认一次扇区请求完成。如果一条记录跨两个扇区，写完第一个后就中断，设备并没有违反单次请求约定，恢复者却不能把半条记录交给应用。因此需要让介质本身保存“这条记录是否完整”的证据，不能只依赖重启就丢失的内存 count。本节把一个payload写成数据扇区和提交扇区，重新从设备读回有效前缀；再真实破坏第二条记录的一位，观察恢复停在第一条，并用新记录替换坏尾。演示只使用实验盘4–7号扇区，结束恢复原字节，RAM文件仍独立。

<a id="journal-format"></a>
## 一条记录由什么证明完整

以 payload=alpha 为例，一个扇区保存内容，另一个保存承认这份内容的提交信息。恢复时两份都匹配才增加有效前缀。每条占两个 512 字节扇区。数据前8字节AIRIDAT1表示格式版本，偏移8存64位sequence，16存32位长度，20存CRC32，24–31保留为零，32起为最多480字节payload。提交扇区使用AIRICOM1，同样有sequence，偏移16复制数据CRC，20保存提交扇区自己的CRC，余下字节为零。多字节字段显式编码为little endian，不把C结构布局直接写盘。

计算CRC时先把自己的CRC字段当零，对整个512字节计算，再恢复原字段。CRC覆盖头、payload和补零尾，可检测常见意外损坏，不提供对抗恶意篡改的保证。两个扇区的sequence必须等于当前位置从1开始的序号，提交中的数据CRC还要等于数据扇区CRC。因此旧commit不能只靠magic就承认一份不相干的新数据。

这些字段让一对扇区可以自检，还需要规定遇到坏记录以后如何处理顺序。恢复从槽 0 开始连续读两扇区；完整零对表示干净空尾，格式或校验不符则逻辑截断，后面不继续搜索离散有效记录。设备读取错误不同于坏格式：它让poisoned置位并返回具体错误，不能把读取失败解释成空盘。当前最多64条，不实现回收、索引、压缩、事务组或多写者并发。

**新建 `include/airi/journal.h`。** 算法只持有同步块回调和容量，可接真实设备，也可接明确的故障模型。

<!-- write: include/airi/journal.h -->
```c
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
long airi_journal_append(struct airi_journal *journal, const void *data, size_t bytes); // 返回从 1 开始的 sequence，失败负错误且不自动重试。
long airi_journal_read(struct airi_journal *journal, unsigned index, void *data, size_t capacity); // index从0开始，成功返回实际内容长度。
void airi_journal_demo(void); // 正常设备上的真实恢复演示，独立占用扇区4–7。
#endif // 结束保护。
```

<a id="journal-order"></a>
## 先失效旧提交，再同步数据，最后同步新提交

scan 已承认 alpha，但下一槽可能残留旧 beta 的数据与提交。要在那里追加新内容，先让旧提交失效；否则恢复者可能把尚未稳定的新数据与旧提交误配。

继续看 alpha 后的第二个槽。append 先把当前 commit 清零；如果还有下一槽，也把下一槽的数据和 commit 清零，再进行第一次 FLUSH。这次同步确认的是“旧提交已失效，而且新前缀后面有一个空尾”，还没有写入新的 payload。

随后写当前数据，再进行第二次 FLUSH。数据按后端合同完成同步以后，介质上仍没有与它匹配的新 commit，恢复者还不能承认这条记录。最后写新 commit，并进行第三次 FLUSH；这次成功以后，内存 count 才增加，调用者才取得 sequence。单次 write 返回 0 只确认那次请求，三段同步分别约束旧提交、新数据与新提交的顺序。

清下一槽解决的是另一种旧尾复活。假设原盘是 alpha、损坏 beta、旧 gamma，scan 只承认 alpha。若只替换 beta，第二条重新有效后，旧 gamma 可能又接到前缀。先清出下一完整零对，就为新前缀建立结束位置；之后追加再向后移动结束位置。被修改的都属于已排除尾部，确认过的前缀不变。

任何 I/O 错误都置 poisoned，之后 append 返回 −5，必须重新 scan 才知道介质实际状态。尤其最后一次 FLUSH 可能已同步 commit，只是确认途中报错：调用者看到失败，重启却能看到新记录。失败不能推出“没有副作用”，自动重试可能重复业务操作。需要可重试业务语义时，还需更高层稳定请求身份；本章提供的是记录序号与明确的不确定结果。

**新建 `kernel/storage/journal.c`。** 所有文件字节由显式编码产生，用户buffer只在本次调用内复制。

<!-- write: kernel/storage/journal.c -->
```c
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
```

上面的编码与 valid 只回答“一对扇区是否自洽”，还没有形成前缀。继续追加 scan：它先复制后端描述，再清空旧 journal 状态，从介质第一个槽重新数。这样，即使调用前 count 为 2，也不能让内存里的旧结论替代这次读取。

<!-- append: kernel/storage/journal.c -->
```c
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
```

scan 返回后，count 指向下一次追加的位置。接下来实现 append，将刚才的三次同步放进同一操作：先处理已排除的旧尾，再写数据，最后发布 commit；任一步 I/O 报错都保留 poisoned，不猜测介质完全没变。

<!-- append: kernel/storage/journal.c -->
```c
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
```

成功追加已经给出序号，读取则使用从零开始的 index。继续追加 read：它重新取回这对扇区并校验，确认长度能完整放进输出以后才复制 payload。输出太小不会只交半条记录，也不会因此把仍然有效的日志标坏。

<!-- append: kernel/storage/journal.c -->
```c
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
```


<a id="journal-real-demo"></a>
## 真实设备验证校验与截断

现在把恢复规则用于真实设备，观察的是读回介质后得到的前缀，而不是沿用 append 留下的 count。demo 先保存 4 个扇区，写零并FLUSH，再追加alpha和beta；每次追加仍实际发VirtIO请求。第二次scan重新读磁盘，不能依赖旧内存count。随后读出第二条数据扇区，翻转payload第一字节的一位，写回并FLUSH，commit保持原样。CRC不再相符，scan得到count=1、truncated=true，读索引1返回−2。

接着append gamma，序号仍为2；再次scan恢复alpha和gamma。最后写回保存的4个扇区并FLUSH，才打印JOURNAL_DEMO_OK。该操作是受控介质位损坏，不是实际拔电；只读盘和已隔离的超时设备明确打印JOURNAL_SKIPPED，不执行写入，也不报告日志成功。

**新建 `kernel/storage/demo.c`。** 设备适配和实验扇区生命周期在这里，不把主机文件改动冒充guest行为。

<!-- write: kernel/storage/demo.c -->
```c
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
```

**更新块接口。** 新增只读查询，消费者不必通过试写猜设备能力。

<!-- write: include/airi/block.h -->
```c
#ifndef AIRI_BLOCK_H // 通用块接口不暴露PCI端口和VirtIO布局。
#define AIRI_BLOCK_H // 包含保护。
#include <airi/boot.h> // 当前启动提供内核连续物理映像位置。
int airi_block_init(const AiriBootInfo *info); // 固定单队列，失败返回负错误。
bool airi_block_readonly(void); // 给调用者明确只读事实，不能靠写失败猜测。
uint64_t airi_block_sectors(void); // 以512字节扇区计，不等同设备逻辑块大小配置。
int airi_block_read(uint64_t sector, void *buffer); // 一次精确一个512字节扇区，0成功。
int airi_block_write(uint64_t sector, const void *buffer); // 同步复制并等待设备完成。
int airi_block_flush(void); // 请求后端同步，不宣称真实硬件掉电已实测。
void airi_block_demo(void); // 只操作项目独立实验盘末扇区。
#endif // 结束保护。
```

**在驱动末尾加入只读查询。** 以 K25 已有的 airi_block_capacity 为定位窗口，保留该函数并接上 airi_block_readonly。队列和错误处理没有变化。

<!-- replace: kernel/drivers/virtio_block.c -->
<!-- before -->
```c
    ready = true; // 队列和配置完整后才接收请求。
    airi_log("[BLOCK_READY] pci=0:%u.%u io=0x%x queue=%u sectors=%llu readonly=%u flush=%u", found_slot, found_function, io, queue_size, (unsigned long long)capacity, readonly, can_flush); // 数据来自真实设备配置。
    return 0; // 上层不接触端口或描述符。
```
<!-- after -->
```c
    ready = true; // 队列和配置完整后才接收请求。
    airi_log("[BLOCK_READY] pci=0:%u.%u io=0x%x queue=%u sectors=%llu readonly=%u flush=%u", found_slot, found_function, io, queue_size, (unsigned long long)capacity, readonly, can_flush); // 数据来自真实设备配置。
    return 0; // 上层不接触端口或描述符。
}
bool airi_block_readonly(void) { // 单设备配置启动后保持稳定。
    return readonly; // 不泄露驱动内部队列指针。
```

**更新启动顺序。** 块设备正常或具体故障演示之后再调用日志演示。

先引入日志演示声明。

<!-- replace: kernel/main.c -->
<!-- before -->
```c
#include <airi/block.h> // 设备适配在内存、时钟之后初始化。
#include <airi/user.h> // 验证ring3系统调用和故障隔离。
#include <airi/task.h> // 两个独立栈内核任务主动交接并回收。
```
<!-- after -->
```c
#include <airi/journal.h> // 追加记录恢复演示独立于RAM文件。
#include <airi/block.h> // 设备适配在内存、时钟之后初始化。
#include <airi/user.h> // 验证ring3系统调用和故障隔离。
#include <airi/task.h> // 两个独立栈内核任务主动交接并回收。
```

块演示结束后调用日志演示，之后才进入原用户阶段。

<!-- replace: kernel/main.c -->
<!-- before -->
```c
        airi_panic("virtio block initialization failed"); // 不把缺设备当成功。
    }
    airi_block_demo(); // 真正DMA读写、FLUSH与范围拒绝。
    airi_user_demo(); // 正常与非法用户程序结束后仍回到启动栈和串口。
    airi_console_init(&console, info);  // 初始化内核静态控制台，借用整个启动期间有效的 info。
    console.service = airi_user_command; // 系统程序服务独立于可选主题/窗口扩展。
```
<!-- after -->
```c
        airi_panic("virtio block initialization failed"); // 不把缺设备当成功。
    }
    airi_block_demo(); // 真正DMA读写、FLUSH与范围拒绝。
    airi_journal_demo(); // 固定实验扇区上提交、校验、坏尾截断与恢复。
    airi_user_demo(); // 正常与非法用户程序结束后仍回到启动栈和串口。
    airi_console_init(&console, info);  // 初始化内核静态控制台，借用整个启动期间有效的 info。
    console.service = airi_user_command; // 系统程序服务独立于可选主题/窗口扩展。
```

<a id="journal-crash-model"></a>
## 模型里的掉电只丢哪些状态

真实 guest 的位翻转证明校验能识别坏尾，还没有逐一覆盖提交中的中断位置。主机后端为此保留 live 和 durable 两份数组，明确模拟“已写但未同步”的区别。write只改live；FLUSH成功才把live复制到durable；crash丢live并从durable恢复。它刻意规定了同步边界，不是宿主文件系统和真实磁盘缓存的完整模型。每次从同一已确认alpha开始，在第二条追加的八个写/同步调用上分别于副作用前、后返回错误，共16条路径。

让第二条 append 在每个边界停止，再只用 durable 重建，才能判断已确认前缀是否保住。故障后算法必须返回 −5 并停止自动追加；模型重启后，只有最后FLUSH已发生但确认报错的那一路能恢复两条，其余仍为alpha。测试还保留损坏第二条之后的完整第三条，验证修复后第三条不复活；容量满返回−28，不环绕覆盖已提交记录，输出缓冲太小返回−34且不部分复制。

这里区分三个模型：进程退出保留内核缓存；QEMU重启保留宿主文件及可能已同步的后端状态；本测试crash丢指定live缓存。真正主机掉电、文件系统错误与设备易失缓存行为未在既有运行中实测，不能把模型通过当作这些环境的持久性结论。

**新建 `tests/unit/journal_test.c`。** 同一算法使用明确后端模型，ASan/UBSan另外检查数组和整数访问。

<!-- write: tests/unit/journal_test.c -->
```c
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
```

**更新构建规则。** guest加入journal与demo，host-test加入第七组观察。

把日志算法与设备演示加入内核清单。

<!-- replace: Makefile -->
<!-- before -->
```make
# 基础目标只编译文本会话与设备；不读取 frontend 的目录或字体。
SOURCES := kernel/main.c kernel/boot/limine.c kernel/boot/info.c \
    kernel/lib/base.c kernel/lib/runtime.c kernel/arch/x86_64/serial.c \
    kernel/console/console.c kernel/arch/x86_64/input.c kernel/arch/x86_64/cpu.c kernel/arch/x86_64/trap.c kernel/mm/pages.c kernel/mm/memory.c kernel/arch/x86_64/paging.c kernel/mm/heap.c kernel/arch/x86_64/timer.c kernel/task/task.c kernel/process/user.c kernel/process/elf.c kernel/fs/file.c kernel/drivers/virtio_block.c
# 只有 desktop- 前缀目标才读取图形支线自己的构建片段。
ifneq ($(filter desktop-%,$(PROFILE)),)
include frontend/sources.mk # 本分支取得图形自己的源码清单。
```
<!-- after -->
```make
# 基础目标只编译文本会话与设备；不读取 frontend 的目录或字体。
SOURCES := kernel/main.c kernel/boot/limine.c kernel/boot/info.c \
    kernel/lib/base.c kernel/lib/runtime.c kernel/arch/x86_64/serial.c \
    kernel/console/console.c kernel/arch/x86_64/input.c kernel/arch/x86_64/cpu.c kernel/arch/x86_64/trap.c kernel/mm/pages.c kernel/mm/memory.c kernel/arch/x86_64/paging.c kernel/mm/heap.c kernel/arch/x86_64/timer.c kernel/task/task.c kernel/process/user.c kernel/process/elf.c kernel/fs/file.c kernel/drivers/virtio_block.c kernel/storage/journal.c kernel/storage/demo.c
# 只有 desktop- 前缀目标才读取图形支线自己的构建片段。
ifneq ($(filter desktop-%,$(PROFILE)),)
include frontend/sources.mk # 本分支取得图形自己的源码清单。
```

在 host-test 加入同一日志算法及 live/durable 模型。

<!-- replace: Makefile -->
<!-- before -->
```make
	    kernel/fs/file.c kernel/lib/base.c -o build/tests/file_test
# 仅全部文件边界成立才继续启动检查。
	./build/tests/file_test
# 清理仅删除可重建产物，保留运行证据artifacts。
clean:
# 调用shutil删除build；ignore_errors允许尚不存在目录。
```
<!-- after -->
```make
	    kernel/fs/file.c kernel/lib/base.c -o build/tests/file_test
# 仅全部文件边界成立才继续启动检查。
	./build/tests/file_test
# 日志后端模型验证提交顺序、错误返回与掉电缓存边界。
	$(HOST_CC) -std=c11 -g -O1 -Wall -Wextra -Werror -fno-builtin \
	    -fsanitize=address,undefined -Iinclude tests/unit/journal_test.c \
	    kernel/storage/journal.c kernel/lib/base.c -o build/tests/journal_test
# 模型不声称真实主机掉电，真设备另由guest演示。
	./build/tests/journal_test
# 清理仅删除可重建产物，保留运行证据artifacts。
clean:
# 调用shutil删除build；ignore_errors允许尚不存在目录。
```

**更新启动验收。** 正常可写设备必须出现JOURNAL_DEMO_OK，受限设备必须明确跳过。

在块结果之后核对日志结果；隔离和只读必须明确跳过，正常盘才要求提交恢复成功。

<!-- replace: scripts/boot.py -->
<!-- before -->
```python
                        assert '[EXEC_FAILURE_OK]' in text and '[EXEC_WORKER_OK]' in text and '[EXEC_COMMIT]' in text # 坏映像保持与成功替换都须实际运行。
                        block_marker = '[BLOCK_TIMEOUT_OK]' if case == 'block-timeout' else '[BLOCK_READONLY_OK]' if case == 'block-readonly' else '[BLOCK_DEMO_OK]' # 不同设备条件分别核对真实结果。
                        assert '[BLOCK_READY]' in text and block_marker in text # 完成、拒绝或超时必须符合本配置。
                        assert '[FILES_USER_OK]' in text and '[FD_EXEC_OK]' in text # RAM文件和成功exec继承均完成。
                        assert '[PROCESS_FORK_WAIT_OK]' in text and '[PROCESS_DEMO_OK]' in text # 复制、真实阻塞、timer换身份和回收都须完成。
                        assert '[ELF_LOAD]' in text and '[ELF_USER_OK]' in text # 独立ELF段、BSS与用户libc必须真正运行。
```
<!-- after -->
```python
                        assert '[EXEC_FAILURE_OK]' in text and '[EXEC_WORKER_OK]' in text and '[EXEC_COMMIT]' in text # 坏映像保持与成功替换都须实际运行。
                        block_marker = '[BLOCK_TIMEOUT_OK]' if case == 'block-timeout' else '[BLOCK_READONLY_OK]' if case == 'block-readonly' else '[BLOCK_DEMO_OK]' # 不同设备条件分别核对真实结果。
                        assert '[BLOCK_READY]' in text and block_marker in text # 完成、拒绝或超时必须符合本配置。
                        journal_marker = '[JOURNAL_SKIPPED]' if case in ('block-timeout', 'block-readonly') else '[JOURNAL_DEMO_OK]' # 没有可写设备时不冒充日志完成。
                        assert journal_marker in text # 正常配置必须真实恢复并替换坏尾。
                        assert '[FILES_USER_OK]' in text and '[FD_EXEC_OK]' in text # RAM文件和成功exec继承均完成。
                        assert '[PROCESS_FORK_WAIT_OK]' in text and '[PROCESS_DEMO_OK]' in text # 复制、真实阻塞、timer换身份和回收都须完成。
                        assert '[ELF_LOAD]' in text and '[ELF_USER_OK]' in text # 独立ELF段、BSS与用户libc必须真正运行。
```

<a id="journal-run"></a>
## 完整运行与观察

```sh
./airios test # 七组主机观察、九种有超时启动和三条真实输入。
./airios run # 看BLOCK_DEMO_OK之后的JOURNAL_DEMO_OK，再继续串口命令。
```

按既有检查条件，正常真实 guest 输出应包含committed=2、corrupt_tail=1、prefix=1、replaced=gamma、restored=1。模型输出的16条故障和真实guest的位损坏分别保存；它们证明各自明确的边界，不互相替代。

调试可在每次io.flush前后比较journal.count与目标数据/commit字节。最后commit写出但FLUSH尚未成功时count仍旧；重新scan之后按介质事实重建，不能拿旧count覆盖磁盘。下一节把这个服务接到用户系统调用，并用同一实验盘跨QEMU重新启动验证恢复；当前还没有用户日志接口或跨启动证据。
