# K26 · 一条追加记录什么时候才算提交，坏尾怎样恢复

从K25继续。设备完成512字节写入，并不自动说明多扇区记录完整。本节把一个payload写成数据扇区和提交扇区，重新从设备读回有效前缀；再真实破坏第二条记录的一位，观察恢复停在第一条，并用新记录替换坏尾。演示只使用实验盘4–7号扇区，结束恢复原字节，RAM文件仍独立。

<a id="journal-format"></a>
## 一条记录由什么证明完整

每条占两个512字节扇区。数据前8字节AIRIDAT1表示格式版本，偏移8存64位sequence，16存32位长度，20存CRC32，24–31保留为零，32起为最多480字节payload。提交扇区使用AIRICOM1，同样有sequence，偏移16复制数据CRC，20保存提交扇区自己的CRC，余下字节为零。多字节字段显式编码为little endian，不把C结构布局直接写盘。

计算CRC时先把自己的CRC字段当零，对整个512字节计算，再恢复原字段。CRC覆盖头、payload和补零尾，可检测常见意外损坏，不提供对抗恶意篡改的保证。两个扇区的sequence必须等于当前位置从1开始的序号，提交中的数据CRC还要等于数据扇区CRC。因此旧commit不能只靠magic就承认一份不相干的新数据。

恢复从槽0开始连续读两扇区；完整零对表示干净空尾，格式或校验不符则逻辑截断，后面不继续搜索离散有效记录。设备读取错误不同于坏格式：它让poisoned置位并返回具体错误，不能把读取失败解释成空盘。当前最多64条，不实现回收、索引、压缩、事务组或多写者并发。

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
long airi_journal_append(struct airi_journal *journal, const void *data, size_t bytes); // 返回一开始的sequence，失败负错误且不自动重试。
long airi_journal_read(struct airi_journal *journal, unsigned index, void *data, size_t capacity); // index从0开始，成功返回实际内容长度。
void airi_journal_demo(void); // 正常设备上的真实恢复演示，独立占用扇区4–7。
#endif // 结束保护。
```

<a id="journal-order"></a>
## 先失效旧提交，再同步数据，最后同步新提交

一次append先把目标commit清零。若还有下一槽，同时清零下一槽的数据和commit，然后FLUSH。接着写当前数据并FLUSH，最后写commit并FLUSH。只有最后一次同步成功，内存count才增加，调用者才取得sequence。每个后端返回0都只是对应操作完成，跨步骤顺序由这些同步边界建立。

为什么要清下一槽？假设磁盘有alpha、损坏的beta、旧gamma。scan只承认alpha；若仅覆盖beta，新的第二条提交之后，旧gamma可能再次接到连续前缀上。清出下一完整零对建立新的结束位置，之后追加再向前移动这个结束位置，旧尾不会复活。这只修改已被恢复规则排除的尾部，不改变已确认前缀。

任一步I/O错误都会置poisoned，后续append返回−5，必须重新scan才能推导真实状态。尤其最后一次FLUSH可能已经把commit同步成功，但错误在确认途中交给调用者；此时append失败，重启却能看到记录。失败不等于什么都没发生，不能无条件自动重试造成重复业务操作。更高层若需要可重试语义，应增加稳定业务请求身份；本节只提供记录序号与明确的不确定结果。

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
```

<a id="journal-real-demo"></a>
## 真实设备验证校验与截断

demo先保存4个扇区，写零并FLUSH，再追加alpha和beta；每次追加仍实际发VirtIO请求。第二次scan重新读磁盘，不能依赖旧内存count。随后读出第二条数据扇区，翻转payload第一字节的一位，写回并FLUSH，commit保持原样。CRC不再相符，scan得到count=1、truncated=true，读索引1返回−2。

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

**按完整版本更新驱动。** 本次只增加airi_block_readonly查询，K25队列与错误处理保留，不需要重敲相同代码。

<!-- write: kernel/drivers/virtio_block.c -->
```c
#include <airi/block.h> // 上层按扇区请求，不知道PCI和环结构。
#include <airi/kernel.h> // 串口记录与内核不变量停止。
#include <airi/timer.h> // 超时依据真实PIT tick。
#include <airi/x86_io.h> // 固定x86 q35设备适配。
struct descriptor { uint64_t address; uint32_t length; uint16_t flags, next; }; // split ring的16字节DMA描述符。
struct used_element { uint32_t id, length; }; // 设备回报链首编号和实际写入长度。
struct block_request { uint32_t type, reserved; uint64_t sector; uint8_t data[512]; volatile uint8_t status; }; // 请求头、数据和完成状态均属于内核静态存储。
_Static_assert(sizeof(struct descriptor) == 16, "VirtIO descriptor size"); // C布局必须符合设备读取宽度。
_Static_assert(offsetof(struct block_request, data) == 16, "VirtIO header size"); // 数据不与头重叠。
_Alignas(4096) static uint8_t queue[12288]; // 物理连续的内核BSS三页，整个启动期间不释放。
_Alignas(16) static struct block_request request; // 单次在途，超时后也不能回收DMA目标。
static uint16_t io, queue_size, available_index, used_index; // 设备端口与两个自由递增十六位序号。
static volatile uint16_t *available, *used; // 设备共享区必须每次真实读取/写入。
static volatile struct used_element *used_ring; // 只在used.idx变化后消费元素。
static struct descriptor *descriptors; // 发布后直到完成不可改链。
static uint64_t request_physical, capacity; // 设备使用guest物理地址，不理解内核高VA。
static bool readonly, can_flush, ready, busy; // 固定队列状态，失败后禁止再次覆盖DMA。
static uint32_t pci_read(unsigned slot, unsigned function, unsigned offset) { // 仅固定bus0上的q35设备。
    airi_out32(0xcf8, 0x80000000u | slot << 11 | function << 8 | (offset & 0xfcu)); // 选择PCI配置地址。
    return airi_in32(0xcfc); // 配置数据由另一个端口交回。
}
static uint64_t physical(const AiriBootInfo *info, const void *pointer, size_t bytes) { // 只转换内核映像中确实连续拥有的静态对象。
    uint64_t address = (uintptr_t)pointer; // 当前C指针是内核VA。
    if (address < info->kernel_virtual_base || address - info->kernel_virtual_base > info->kernel_size || bytes > info->kernel_size - (address - info->kernel_virtual_base)) // 排除范围和加法溢出。
        airi_panic("DMA object outside contiguous kernel image"); // 不能把任意堆VA减偏移当物理连续。
    return info->kernel_physical_base + address - info->kernel_virtual_base; // 当前Limine合同保证整个内核物理映像连续。
}
static int stop(int error) { // 超时和坏完成后先撤销设备运行资格。
    airi_out8(io + 18, 0); // reset设备；固定DMA区仍永久保留，不因reset结果复用。
    ready = false; // 后续请求返回EIO，避免晚到DMA打到新请求。
    busy = false; // 不再让上层等待旧状态，但不重建队列。
    airi_log("[BLOCK_STOP] error=%d reset_status=%u dma=quarantined", error, (unsigned)airi_in8(io + 18)); // 是否已确认reset作为真实记录。
    return error; // 本驱动不提供自动重试，避免重复写语义不明。
}
int airi_block_init(const AiriBootInfo *info) { // 仅启动时调用，不支持热插拔和再次初始化。
    unsigned found_slot = 32, found_function = 0; // 尚未找到legacy块设备。
    for (unsigned slot = 0; slot < 32; ++slot) // 固定PCI bus0最多32槽。
        for (unsigned function = 0; function < 8; ++function) // 同时支持多功能槽的枚举。
            if (pci_read(slot, function, 0) == 0x10011af4u) { // legacy/transitional virtio-blk的固定vendor/device。
                found_slot = slot; // 保存实际设备地址。
                found_function = function; // 不假定总在slot3。
            }
    if (found_slot == 32) // 不接触未知设备的BAR。
        return -19; // -ENODEV。
    uint32_t bar = pci_read(found_slot, found_function, 0x10); // legacy寄存器在BAR0 I/O空间。
    if (!(bar & 1) || (bar & ~3u) > 0xffc0u) // 拒绝MMIO和超出16位端口空间的布局。
        return -95; // 本节不实现现代PCI capability transport。
    io = (uint16_t)(bar & ~3u); // 低位是类型，不属于端口地址。
    uint32_t command = pci_read(found_slot, found_function, 4); // PCI命令需要启用I/O与总线主控DMA。
    airi_out32(0xcfc, (command & 0xffffu) | 5u); // 地址仍指向command，状态高16位写0以免误清W1C位。
    airi_out8(io + 18, 0); // 先reset，再发布本次驱动状态。
    if (airi_in8(io + 18)) // 不确认停止就不能重用静态队列。
        return -5; // -EIO。
    airi_out8(io + 18, 1); // ACKNOWLEDGE：已识别设备。
    airi_out8(io + 18, 3); // DRIVER：内核知道如何驱动该设备。
    uint32_t features = airi_in32(io); // legacy只能读取低32个特征位。
    readonly = (features & (1u << 5)) != 0; // 只读设备不能尝试写。
    can_flush = (features & (1u << 9)) != 0; // 仅协商真正理解的RO与FLUSH。
    airi_out32(io + 4, features & ((1u << 5) | (1u << 9))); // 不选择indirect、event_idx或多队列。
    airi_out16(io + 14, 0); // 只配置请求队列0。
    queue_size = airi_in16(io + 12); // legacy队列大小不能由驱动缩小。
    if (queue_size < 3 || queue_size > 256 || (queue_size & (queue_size - 1))) // ring索引与静态三页预算要求有效2的幂。
        return stop(-95); // 不把不支持的设备布局强塞进小数组。
    size_t used_offset = (16u * queue_size + 2u * (3u + queue_size) + 4095u) & ~(size_t)4095; // 描述符和avail之后对齐到下一页。
    if (used_offset + 2u * 3u + 8u * queue_size > sizeof(queue)) // 包含legacy尾部字段的容量核对。
        return stop(-95); // 不允许设备写过静态区。
    airi_memset(queue, 0, sizeof(queue)); // idx和flags均从0开始。
    descriptors = (struct descriptor *)queue; // 描述符在第一页开头。
    available = (volatile uint16_t *)(queue + 16u * queue_size); // flags、idx、ring依次为16位字段。
    used = (volatile uint16_t *)(queue + used_offset); // used部分另起4096边界。
    used_ring = (volatile struct used_element *)(queue + used_offset + 4); // flags和idx之后是元素数组。
    available[0] = 1; // NO_INTERRUPT：首版由驱动轮询完成，不启设备IRQ。
    uint64_t queue_physical = physical(info, queue, sizeof(queue)); // 设备需要连续guest PA。
    request_physical = physical(info, &request, sizeof(request)); // 请求对象也是静态内核物理存储。
    if (queue_physical & 4095 || queue_physical >> 44) // PFN寄存器32位、页对齐。
        return stop(-95); // 不能截掉物理地址高位。
    __atomic_thread_fence(__ATOMIC_SEQ_CST); // 描述符和初始化内容在设备可见前完成。
    airi_out32(io + 8, (uint32_t)(queue_physical >> 12)); // legacy QueuePFN以4096字节页号发布。
    bool stable = false; // legacy没有config_generation，容量按高低高核对且限制重试。
    for (unsigned attempt = 0; attempt < 8; ++attempt) { // 固定设备容量不应持续变化。
        uint32_t high = airi_in32(io + 24); // 不使用MSI-X时设备配置从20开始。
        uint32_t low = airi_in32(io + 20); // capacity低32位。
        if (high == airi_in32(io + 24)) { // 再确认高位属于同一稳定值。
            capacity = (uint64_t)high << 32 | low; // 单位为512字节扇区。
            stable = true; // 只有稳定读取才可发布容量。
            break; // 不进行无界设备重试。
        }
    }
    if (!stable) // 配置持续变化不能挂住启动。
        return stop(-5); // 停止设备且不重用静态DMA。
    if (capacity < 16) // 实验留足独立扇区，拒绝过小盘。
        return stop(-22); // 不在空设备上减1。
    airi_out8(io + 18, 7); // legacy不使用现代FEATURES_OK，ACK|DRIVER|DRIVER_OK。
    ready = true; // 队列和配置完整后才接收请求。
    airi_log("[BLOCK_READY] pci=0:%u.%u io=0x%x queue=%u sectors=%llu readonly=%u flush=%u", found_slot, found_function, io, queue_size, (unsigned long long)capacity, readonly, can_flush); // 数据来自真实设备配置。
    return 0; // 上层不接触端口或描述符。
}
bool airi_block_readonly(void) { // 单设备配置启动后保持稳定。
    return readonly; // 不泄露驱动内部队列指针。
}
uint64_t airi_block_sectors(void) { // 只给上层有界容量信息。
    return ready ? capacity : 0; // 停止后不继续发布可用设备。
}
static int perform(uint32_t type, uint64_t sector, void *buffer) { // 单队列且最多一个在途请求。
    if (!ready) // 之前超时或未初始化。
        return -5; // -EIO，不覆盖可能仍被设备使用的旧DMA。
    if (busy) // 不支持重入和多个调用者覆盖同一静态请求。
        return -16; // -EBUSY。
    if (type != 4 && sector >= capacity) // 在设备读取任何描述符前拒绝越界扇区。
        return -22; // -EINVAL。
    if (type == 1 && readonly) // 设备只读不产生写DMA。
        return -30; // -EROFS。
    if (type == 4 && !can_flush) // 不把缺少FLUSH伪装同步成功。
        return -95; // -EOPNOTSUPP。
    uint64_t flags = airi_irq_save(); // 队列发布部分对本CPU不可打断。
    busy = true; // 先取得静态请求所有权。
    request.type = type; // IN=0、OUT=1、FLUSH=4。
    request.reserved = 0; // 协议保留字段写零。
    request.sector = type == 4 ? 0 : sector; // FLUSH不指定数据扇区。
    request.status = 0xff; // 设备必须覆盖状态，不能误用上次成功。
    if (type == 1) // 写请求先复制到内核长期DMA缓冲。
        airi_memcpy(request.data, buffer, 512); // 上层栈和用户页都不交给设备。
    descriptors[0] = (struct descriptor){request_physical, 16, 1, type == 4 ? 2 : 1}; // 设备只读请求头，NEXT指向数据或状态。
    descriptors[1] = (struct descriptor){request_physical + 16, 512, (uint16_t)(1u | (type == 0 ? 2u : 0u)), 2}; // read数据由设备写，write数据由设备读。
    descriptors[2] = (struct descriptor){request_physical + offsetof(struct block_request, status), 1, 2, 0}; // 状态字节始终由设备写。
    available[2 + available_index % queue_size] = 0; // 环项写链首编号0，不是请求指针。
    __atomic_thread_fence(__ATOMIC_SEQ_CST); // 先让设备看到整个链与avail槽。
#ifndef AIRI_TEST_BLOCK_TIMEOUT // 故障镜像故意漏发布idx，模拟驱动发布遗漏而非声称设备硬件挂死。
    available[1] = ++available_index; // 只有idx前进才交出链的所有权。
    __atomic_thread_fence(__ATOMIC_SEQ_CST); // 发布idx在端口通知之前对设备可见。
    airi_out16(io + 16, 0); // 通知队列0有请求。
#endif // 故障分支仍有真实时间上限。
    uint64_t deadline = airi_timer_ticks() + 200; // 真实100Hz IRQ，最多约两秒等待。
    while (used[1] == used_index) { // 设备先写数据/状态，最后发布used.idx。
        if (airi_timer_ticks() >= deadline) { // 不是按忙循环次数估算时间。
            int result = stop(-110); // -ETIMEDOUT，静态DMA区隔离且不再复用。
            airi_irq_restore(flags); // 保留调用前的IF策略。
            return result; // 上层得到具体错误。
        }
        __asm__ volatile("sti; hlt; cli" ::: "memory"); // 暂开IRQ等待PIT唤醒；CS0不会被用户调度器抢占。
    }
    __atomic_thread_fence(__ATOMIC_SEQ_CST); // 观察idx之后再读取设备完成内容。
    unsigned next_index = used[1]; // 读取这次设备序号。
    struct used_element completion = used_ring[used_index % queue_size]; // 同一槽的完整id与length。
    if ((uint16_t)(next_index - used_index) != 1 || completion.id != 0 || completion.length < 1 || completion.length > (type == 0 ? 513u : 1u)) { // 单在途只能有一条本链完成，状态至少一字节。
        int result = stop(-5); // 无法确认DMA生命周期就停止，不复用缓冲。
        airi_irq_restore(flags); // 恢复原调用环境。
        return result; // 坏完成不是成功。
    }
    if (request.status == 0 && type == 0 && completion.length != 513) { // 成功读必须写完512数据和一个状态字节。
        int result = stop(-5); // 不把短设备完成当完整扇区。
        airi_irq_restore(flags); // 恢复原IF。
        return result; // 上层缓冲尚未写入。
    }
    ++used_index; // 本次已消费，不再重复处理同一个槽。
    int result = request.status == 0 ? 0 : request.status == 2 ? -95 : -5; // 设备IOERR与UNSUPP都明确交回。
    if (!result && type == 0) // 只有成功读才能把设备数据交给上层。
        airi_memcpy(buffer, request.data, 512); // 复制结束后设备DMA缓冲才可供下次覆盖。
    (void)airi_in8(io + 19); // 清可能遗留的legacy ISR状态，完成仍以used环判断。
    busy = false; // 已确认完成，可以重用同一静态请求。
    airi_irq_restore(flags); // 系统调用进入时IF0，普通内核演示进入时IF1。
    return result; // 0表示设备完成，不自动代表应用事务持久化。
}
int airi_block_read(uint64_t sector, void *buffer) { // 单扇区固定长度接口。
    return perform(0, sector, buffer); // read只在完成后写入上层副本。
}
int airi_block_write(uint64_t sector, const void *buffer) { // 上层输入在提交前复制。
    return perform(1, sector, (void *)buffer); // perform的OUT分支不修改输入。
}
int airi_block_flush(void) { // FLUSH没有数据描述符。
    return perform(4, 0, NULL); // 描述符链直接从头到状态。
}
void airi_block_demo(void) { // 独立实验盘，不接宿主真实磁盘。
    uint8_t before[512], pattern[512], after[512]; // 同步副本不被DMA直接访问。
    uint64_t sector = airi_block_sectors() - 1; // 只占用最后一扇区，前段留给后续日志。
    int error = airi_block_read(sector, before); // 首先保存原字节，正常结束恢复。
#ifdef AIRI_TEST_BLOCK_TIMEOUT // 漏发布idx应超时，随后设备停止且拒绝复用。
    if (error != -110 || airi_block_read(0, after) != -5) // 不能只让QEMU一直活着。
        airi_panic("block timeout did not quarantine DMA"); // 错误生命周期不能继续。
    airi_log("[BLOCK_TIMEOUT_OK] error=-110 next=-5 dma=quarantined"); // 实际PIT截止后才到这里。
    return; // 不做任何后续写。
#endif // 正常路径还需验证真正读写。
    if (error) // 初始读取必须完成。
        airi_panic("cannot read block demo sector"); // 不用未初始化before覆盖磁盘。
    for (unsigned i = 0; i < sizeof(pattern); ++i) // 不用全零数据掩盖没执行的写。
        pattern[i] = (uint8_t)(i * 37u + 19u); // 可复现非零模式。
    if (readonly) { // 只读盘允许读取，但写必须拒绝。
        if (airi_block_write(sector, pattern) != -30) // 软件特征检查产生明确错误。
            airi_panic("readonly block write was accepted"); // 不能假装已落盘。
        airi_log("[BLOCK_READONLY_OK] read=1 write=-30"); // 真实只读QEMU设备配置。
        return; // 原字节未改。
    }
    if (airi_block_write(sector, pattern) || airi_block_read(sector, after) || airi_memcmp(pattern, after, 512)) // 设备真正读回写入模式。
        airi_panic("block pattern did not round trip"); // 内容不一致不能只看status。
    if (airi_block_write(sector, before) || airi_block_flush() || airi_block_read(capacity, after) != -22) // 恢复原扇区、实际FLUSH、范围拒绝。
        airi_panic("block restore, flush or range check failed"); // 同步错误必须交付。
    airi_log("[BLOCK_DEMO_OK] read=1 write=1 flush=1 range=-22 restored=1"); // 不宣称host真实掉电测试。
}
```

**更新启动顺序。** 块设备正常或具体故障演示之后再调用日志演示。

<!-- write: kernel/main.c -->
```c
#include <airi/journal.h> // 追加记录恢复演示独立于RAM文件。
#include <airi/block.h> // 设备适配在内存、时钟之后初始化。
#include <airi/user.h> // 验证ring3系统调用和故障隔离。
#include <airi/task.h> // 两个独立栈内核任务主动交接并回收。
#include <airi/timer.h> // 内存建立后开始真实设备中断。
#include <airi/memory.h> // 通过内存服务建立页池、页表、堆并查询快照。
#include <airi/trap.h> // 安装异常表并演示处理器现场。
#include <airi/arch.h>     // 通过架构边界使用忙等提示。
#include <airi/kernel.h>   // 取得启动信息、基础库与串口日志接口。
#include <airi/console.h>  // 取得命令环、按键与视图类型。

static struct airi_console console;  // 静态保存终端命令与日志，不在每次重画时重建。

#ifndef AIRI_DEMO_MESSAGE  // 允许修改实验在编译时替换输出文本。
#define AIRI_DEMO_MESSAGE "Hello from AiriOS P1"  // 普通配置默认输出的学习标记文字。
#endif  // 结束默认演示文字的条件定义
_Noreturn void airi_kernel_main(const AiriBootInfo *info) {  // 引导适配交入稳定启动描述；_Noreturn 声明内核不返回加载器。
    const char *error = airi_boot_validate(info);  // 校验必需启动字段，NULL 表示通过，字符串给出失败原因。
    if (error)  // 有具体原因则不能继续读取未经验证的数据。
        airi_panic(error);  // 输出失败原因并停止当前 CPU 执行。
    airi_trap_init(); // 通过启动校验后先接管异常，不启用设备中断。
    airi_trap_demo(); // 正常启动验证带与不带硬件错误码的返回路径。
#ifdef AIRI_TEST_FATAL_TRAP // 只在作者故障镜像中执行未处理异常。
    airi_trap_fatal_demo(); // 必须报告并停止，不能发布 READY。
#endif // 普通镜像继续初始化。
    airi_log("[BOOT_INFO] version=%u regions=%u loader=%s", info->version, info->region_count,  // 打印协议版本和有效内存区域数，%u 与无符号参数匹配。
             info->loader);  // 输出适配层复制且有 NUL 终止的加载器名称。
    airi_log("[KERNEL] physical=0x%llx virtual=0x%llx size=%llu",  // 串口分别记录内核物理位置、虚拟地址和字节长度。
             (unsigned long long)info->kernel_physical_base,  // 转换到 %llx 要求的无符号 long long。
             (unsigned long long)info->kernel_virtual_base, (unsigned long long)info->kernel_size);  // 虚拟地址采用十六进制，内核大小按字节十进制输出。
    airi_log("[HHDM] offset=0x%llx", (unsigned long long)info->direct_map_offset);  // 记录把物理地址换成直接映射虚拟地址的偏移量。
    for (uint32_t i = 0; i < info->region_count; ++i) {  // 只遍历启动描述声明的有效区域，不读取固定数组剩余项。
        const struct airi_memory_region *r = &info->regions[i];  // 借用第 i 条区域描述。
        airi_log("[MEMORY] index=%u base=0x%llx length=0x%llx kind=%s", i,  // 逐项输出下标、物理起点、长度及用途。
                 (unsigned long long)r->base, (unsigned long long)r->length,  // 地址和长度都是64位范围，按 long long 适配格式。
                 airi_memory_name(r->kind));  // 将区域种类转成可读名称，不改变区域数据。
    }
    uint64_t usable = airi_memory_bytes(info, AIRI_MEMORY_USABLE);  // 按实际种类累加可用区域字节数，不等同虚拟机总配置内存。
    airi_log("[MEMORY_SUMMARY] usable_bytes=%llu boot_reclaim_bytes=%llu",  // 输出可用内存与暂由加载器占用的可回收内存两个值。
             (unsigned long long)usable,  // 可用字节数转换匹配 %llu。
             (unsigned long long)airi_memory_bytes(info, AIRI_MEMORY_BOOT_RECLAIM));  // 另行累计加载器可回收区域，不混入 usable。
    airi_log("[LESSON] %s", AIRI_DEMO_MESSAGE);  // 发送当前配置的学习文本，修改实验可据此核对版本。
    airi_memory_init(info); // 只借出完整 usable 页，并在真实 RAM 演示写入与归还。
    airi_timer_demo(); // 从PIT/PIC收到真实IRQ0，hlt后恢复到正常上下文。
    airi_task_demo(); // 当前只在正常内核上下文主动切换，IRQ不抢占内核任务。
    int block_error = airi_block_init(info); // 固定单队列设备，未连接时明确停止当前存储阶段。
    if (block_error) { // 默认运行必须带独立实验盘。
        airi_log("[BLOCK_INIT_ERROR] error=%d", block_error); // 先记录具体设备错误。
        airi_panic("virtio block initialization failed"); // 不把缺设备当成功。
    }
    airi_block_demo(); // 真正DMA读写、FLUSH与范围拒绝。
    airi_journal_demo(); // 固定实验扇区上提交、校验、坏尾截断与恢复。
    airi_user_demo(); // 正常与非法用户程序结束后仍回到启动栈和串口。
    airi_console_init(&console, info);  // 初始化内核静态控制台，借用整个启动期间有效的 info。
    console.service = airi_user_command; // 系统程序服务独立于可选主题/窗口扩展。
    console.read_memory = airi_memory_snapshot; // 文本查询只读取数值，不持有分配器内部对象。
    bool graphics = airi_console_draw(&console);  // 实际完成首帧后取得布尔结果，不能仅凭描述存在认定成功。
    if (graphics) {  // 有效图形路径才输出图形参数。
        const struct airi_framebuffer *fb = &info->framebuffer;  // 借用刚用于绘图的同一份帧缓冲描述。
        airi_log("[FRAMEBUFFER] width=%llu height=%llu pitch=%llu bpp=%u",  // 记录尺寸、字节行距和每像素位数，供截图与日志比对。
                 (unsigned long long)fb->width, (unsigned long long)fb->height,  // 宽高转为与 %llu 相同的参数类型。
                 (unsigned long long)fb->pitch, (unsigned)fb->bpp);  // pitch 按字节输出，bpp 按位数输出。
    } else  // 未完成图形时仍发送串口原因并继续启动。
        airi_log("[DISPLAY_SERIAL] no frame produced; using serial output");  // 图形不适用时记录原因路径，后面仍进入串口命令循环。
    /* Written AFTER all drawing, so tests can take a completed screenshot. */
    airi_log("[AIRI_P1_READY] graphics=%u usable_bytes=%llu", graphics ? 1u : 0u,  // 首帧调用结束后才发布完成标记，布尔转换为明确的0/1无符号值。
             (unsigned long long)usable);  // 完成记录附同一实际可用字节数。
    airi_log("[AIRI_P2_READY] exceptions=1 pages=1 own_cr3=1 heap=1"); // 所有内存演示和控制台初始化结束后才发布阶段完成。
    airi_log("[AIRI_P3_READY] user=ring3 elf=static fork=copy exec=transaction wait=blocked"); // 完整内核进程路径结束并恢复后才发布。
    airi_serial_write("airi> ");  // 输出首个串口提示符。
    for (;;) {  // 没有退出条件，内核持续轮询输入。
        int key = airi_input_poll();  // 从虚拟键盘或 UART 取得一个共享键值，0 表示暂无输入。
        if (!key) {  // 没有新键时不处理命令和重画。
            airi_arch_relax();  // 提示当前架构正在忙等；这不是睡眠或调度。
            continue;  // 跳过当前不适用项，继续检查下一项。
        }
        uint64_t before = console.total_lines;  // 记录输入前日志尾，回车后只回显本次产生的新行。
        bool changed = airi_console_key(&console, key);  // 按键改变命令或页面状态，返回是否需要刷新。
        if (!changed)  // 被忽略的按键不产生新帧。
            continue;  // 跳过当前不适用项，继续检查下一项。
        if (key == '\n' || key == '\r') {  // 回车完成命令，才把新增输出回显到 UART。
            uint64_t first = console.total_lines - console.line_count;  // 日志环只保留 line_count 行，first 是尚可读取的最早逻辑行。
            if (before < first)  // 若本次命令挤掉旧行，先把读取起点限到仍存在的区域。
                before = first;  // 不访问已经被覆盖的逻辑行。
            for (uint64_t i = before; i < console.total_lines; ++i) {  // 逐行回显本次命令产生且仍保留的内容。
                airi_serial_write(airi_console_line(&console, i));  // 逻辑行号由环接口转换为数组下标。
                airi_serial_write("\n");  // 为该行补串口行结束。
            }
            airi_serial_write("airi> ");  // 输出首个串口提示符。
        }
        if (graphics)  // 只在启动时建立了图形路径的情况下重画。
            airi_console_draw(&console);  // 把变更后的命令与页面状态写成完整画面。
        /* Published after rendering: interaction tests observe completed frames. */
        if (key == '\n' || key == '\r' || key >= AIRI_KEY_PAGE_UP)  // 命令提交与功能键在绘图后发布可观察完成记录。
            airi_log("[CONSOLE_UPDATE] view=%u commands=%u page=%u", (unsigned)console.view,  // 输出当前页面、累计命令和内存页号，观察程序据此等待具体状态。
                     console.commands, console.map_page);  // 这两个计数来自刚处理并重画的同一命令对象。
    }
}
```

<a id="journal-crash-model"></a>
## 模型里的掉电只丢哪些状态

主机测试后端有live和durable两份数组。write只改live；FLUSH成功才把live复制到durable；crash丢live并从durable恢复。它刻意规定了同步边界，不是宿主文件系统和真实磁盘缓存的完整模型。每次从同一已确认alpha开始，在第二条追加的八个写/同步调用上分别于副作用前、后返回错误，共16条路径。

故障后算法必须返回−5并停止自动追加；模型重启后，只有最后FLUSH已发生但确认报错的那一路能恢复两条，其余仍为alpha。测试还保留损坏第二条之后的完整第三条，验证修复后第三条不复活；容量满返回−28，不环绕覆盖已提交记录，输出缓冲太小返回−34且不部分复制。

这里区分三个模型：进程退出保留内核缓存；QEMU重启保留宿主文件及可能已同步的后端状态；本测试crash丢指定live缓存。真正主机掉电、文件系统错误与设备易失缓存行为没有在本轮实测，不能把模型通过当作这些环境的持久性结论。

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

<!-- write: Makefile -->
```make
# 用既有make_command取得交叉工具参数；统一airios入口在K13加入。
# 内核C编译器由调用参数覆盖；此处默认clang，不借宿主libc。
CC := clang
# ELF链接器默认LLD，调用参数可传入真实找到的路径。
LD := ld.lld
# 主机实验用宿主编译器，可以链接宿主libc。
HOST_CC := cc
# ?=仅在调用者未赋值时取normal，各测试配置独立构建。
PROFILE ?= normal
# $(PROFILE)展开当前配置名称，OUT指向这一配置的产物目录。
OUT := build/$(PROFILE)
# 基础目标只编译文本会话与设备；不读取 frontend 的目录或字体。
SOURCES := kernel/main.c kernel/boot/limine.c kernel/boot/info.c \
    kernel/lib/base.c kernel/lib/runtime.c kernel/arch/x86_64/serial.c \
    kernel/console/console.c kernel/arch/x86_64/input.c kernel/arch/x86_64/cpu.c kernel/arch/x86_64/trap.c kernel/mm/pages.c kernel/mm/memory.c kernel/arch/x86_64/paging.c kernel/mm/heap.c kernel/arch/x86_64/timer.c kernel/task/task.c kernel/process/user.c kernel/process/elf.c kernel/fs/file.c kernel/drivers/virtio_block.c kernel/storage/journal.c kernel/storage/demo.c
# 只有 desktop- 前缀目标才读取图形支线自己的构建片段。
ifneq ($(filter desktop-%,$(PROFILE)),)
include frontend/sources.mk # 本分支取得图形自己的源码清单。
# guest 图形适配与纯 C 前端进入同一内核，参考服务和 Cocoa 不进入。
SOURCES += kernel/console/display.c $(UI_SOURCES)
else # 没有desktop-前缀时只选择串口实现。
# 串口实现没有字体、绘图与桌面状态。
SOURCES += kernel/console/serial_display.c
endif # 结束当前可选图形分支。
# 去掉图形前缀后，两种呈现方式使用同一故障配置含义。
CASE := $(patsubst desktop-%,%,$(PROFILE))
# patsubst把每个.c相对路径改为当前OUT下的.o，汇编入口对象另加入。
OBJECTS := $(patsubst %.c,$(OUT)/%.o,$(SOURCES)) $(OUT)/kernel/arch/x86_64/entry.o $(OUT)/kernel/arch/x86_64/traps.o $(OUT)/kernel/arch/x86_64/switch.o $(OUT)/labs/airios/probe.o $(OUT)/labs/airios/images.o
# 交叉目标为无宿主ELF；C11/O2/调试信息/警告作为错误。freestanding与fno-builtin关闭宿主假设；禁栈保护和PIC/PIE；不使用red zone与浮点寄存器；kernel地址模型；MMD/MP生成头文件依赖。
CFLAGS := --target=x86_64-unknown-none-elf -std=c11 -O2 -g -Wall -Wextra -Werror \
          -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-pie \
          -mno-red-zone -mgeneral-regs-only -mcmodel=kernel -MMD -MP
# 预处理器从自有include、固定协议头third_party和前端include取得声明。
CPPFLAGS := -Iinclude -Ithird_party
# 只有显式图形配置需要前端头文件。
ifneq ($(filter desktop-%,$(PROFILE)),)
CPPFLAGS += -Ifrontend/include # 为已选择的图形实现提供头文件搜索位置。
endif # 结束当前可选图形分支。
# Make条件只在no-framebuffer配置加入下方测试宏。
ifeq ($(CASE),no-framebuffer)
# +=追加预处理选项，让启动适配模拟缺少图形响应。
CPPFLAGS += -DAIRI_TEST_NO_FRAMEBUFFER
endif # 结束 no-framebuffer 的编译条件
# 缺内存响应配置采用独立构建目录和下面宏。
ifeq ($(CASE),missing-memory)
# 模拟必需内存地图缺失，内核应打印具体PANIC。
CPPFLAGS += -DAIRI_TEST_NO_MEMMAP
endif # 结束 missing-memory 的编译条件
# 设备超时故障镜像故意不发布avail.idx，正常镜像不含注入。
ifeq ($(CASE),block-timeout)
CPPFLAGS += -DAIRI_TEST_BLOCK_TIMEOUT # 验证真实PIT截止与DMA隔离，不冒充设备断电。
endif # 结束块设备故障配置。
# 栈保护故障镜像让未映射栈触发DF，由IST1报告。
ifeq ($(CASE),stack-guard)
CPPFLAGS += -DAIRI_TEST_STACK_GUARD # 此镜像不能发布READY。
endif # 结束栈故障配置。
# 时钟故障镜像仅供K18作者演示超时，不进入正常启动配置集合。
ifeq ($(CASE),timer-masked)
CPPFLAGS += -DAIRI_TEST_TIMER_MASKED # PIT仍计数，但PIC阻止IRQ0到达CPU。
endif # 结束时钟故障条件。
# 作者故障配置不登记恢复，应报告真实异常后停止。
ifeq ($(CASE),trap-fatal)
CPPFLAGS += -DAIRI_TEST_FATAL_TRAP # 仅此构建目录触发未处理 UD2。
endif # 结束故障配置。
# 修改实验仅改变标题与输出文本，普通配置不继承这两个宏。
ifeq ($(CASE),edit-experiment)
# 单引号由shell保护内部双引号，编译器取得C字符串宏值；下一续行同时设置学习日志文字。
CPPFLAGS += -DAIRI_PANEL_TITLE='"LOCAL / MAINTENANCE CONSOLE"' \
            -DAIRI_DEMO_MESSAGE='"I changed the output and rebooted"'
endif # 结束 edit-experiment 的文字覆盖条件

# 这些名称是动作目标，即使同名文件存在也执行其规则。
.PHONY: all kernel image host-test clean
# 默认动作依赖镜像，触发内核编译、链接和镜像制作。
all: image
# 只构建独立内核ELF，不制作启动镜像。
kernel: $(OUT)/airios.elf
# 模式规则将相同相对路径的C源文件编成当前配置对象；%匹配路径主体。
$(OUT)/%.o: %.c
# @隐藏命令回显；$@是目标路径，dir取得其目录并确保存在。
	@mkdir -p $(dir $@)
# $<取第一项依赖即当前源文件，$@取输出对象；-c只编译不链接。
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
# 同一对象布局下编译带预处理的汇编入口。
$(OUT)/%.o: %.S
# @隐藏命令回显；$@是目标路径，dir取得其目录并确保存在。
	@mkdir -p $(dir $@)
# $<取第一项依赖即当前源文件，$@取输出对象；-c只编译不链接。
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
# ELF依赖全部对象与链接脚本，任一变化会重新链接。
$(OUT)/airios.elf: $(OBJECTS) kernel/linker.ld
# 这一完整续行命令用ELF x86-64、无标准库、静态链接、4KiB页对齐和不可执行栈；-T指定自有布局，最后-o写目标。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 \
	    -z noexecstack -T kernel/linker.ld $(OBJECTS) -o $@
# 镜像先要求独立ELF成功。
image: $(OUT)/airios.elf
# 把同一配置的ELF装入UEFI启动镜像。
	python3 scripts/image.py $(PROFILE)
# 主机字节算法、启动合同与命令状态检查，保留assert并启用ASan/UBSan。
host-test:
# 先建立主机检查产物目录。
	@mkdir -p build/tests
# 以下完整续行命令编译主机检查；fno-builtin避免优化绕过被测实现，sanitizer检查内存和未定义行为，AIRI_HOST_TEST避开真实端口指令。
	$(HOST_CC) -std=c11 -g -O1 -Wall -Wextra -Werror -fno-builtin \
	    -fsanitize=address,undefined -DAIRI_HOST_TEST -Iinclude tests/unit/base_test.c \
	    kernel/lib/base.c kernel/boot/info.c \
	    kernel/console/console.c kernel/arch/x86_64/input.c \
		    -o build/tests/base_test
# 只有全部assert成立才打印PASS，失败状态返回Make。
	./build/tests/base_test
# 独立编译同一物理页算法，主机不解引用 guest 地址。
	$(HOST_CC) -std=c11 -g -O1 -Wall -Wextra -Werror -fno-builtin \
	    -fsanitize=address,undefined -Iinclude tests/unit/pages_test.c \
	    kernel/mm/pages.c kernel/boot/info.c kernel/lib/base.c -o build/tests/pages_test
# 验证耗尽、保留范围、重复释放和舍入边界。
	./build/tests/pages_test
# 同一四级页表算法在受控arena里验证，绝不安装宿主页表。
	$(HOST_CC) -std=c11 -g -O1 -Wall -Wextra -Werror -fno-builtin \
	    -fsanitize=address,undefined -DAIRI_HOST_TEST -Iinclude tests/unit/vm_test.c \
	    kernel/arch/x86_64/paging.c kernel/mm/pages.c kernel/boot/info.c kernel/lib/base.c -o build/tests/vm_test
# 覆盖翻译、共享分支、空表回收和中途耗尽的所有权回滚。
	./build/tests/vm_test
# 在宿主受控backing上验证同一小对象实现，不借用guest地址。
	$(HOST_CC) -std=c11 -g -O1 -Wall -Wextra -Werror -fno-builtin \
	    -fsanitize=address,undefined -Iinclude tests/unit/heap_test.c \
	    kernel/mm/heap.c kernel/lib/base.c -o build/tests/heap_test
# 交错分配与归还时检查所有活对象内容和最终容量。
	./build/tests/heap_test
# 解析器在独立小端字节夹具中核对正常与损坏ELF，不执行输入指令。
	$(HOST_CC) -std=c11 -g -O1 -Wall -Wextra -Werror -fno-builtin \
	    -fsanitize=address,undefined -Iinclude tests/unit/elf_test.c \
	    kernel/process/elf.c kernel/lib/base.c -o build/tests/elf_test
# 失败不得写出半份view，也不得越界读取映像。
	./build/tests/elf_test
# 文件算法在主机使用同一实现，验证引用、偏移和容量回滚。
	$(HOST_CC) -std=c11 -g -O1 -Wall -Wextra -Werror -fno-builtin \
	    -fsanitize=address,undefined -Iinclude tests/unit/file_test.c \
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
	python3 -c 'import shutil; shutil.rmtree("build", ignore_errors=True)'
# 后缀替换得到自动依赖文件；前缀-允许首次构建尚无.d。
-include $(OBJECTS:.o=.d)


# 手动选择图形动作时才读取支线目标；默认构建不检查该目录。
ifneq ($(filter ui-preview ui-test lab-editor,$(MAKECMDGOALS)),)
include frontend/targets.mk # 仅显式图形动作读取相应规则。
endif # 结束当前可选图形分支。

# 三个观察名称均是动作，即使有同名文件也运行。
.PHONY: lab-memmove lab-boot-info
# 重新构建重叠移动与截断输出观察。
lab-memmove:
# 建立观察程序目录，保持所有可重建产物在build。
	@mkdir -p build/labs
# 下面续行按O0保留便于调试的步骤，链接当前观察所需的同一自有实现。
	$(HOST_CC) -std=c11 -O0 -g -Wall -Wextra -Werror -Iinclude \
	    labs/host/base/memmove_trace.c kernel/lib/base.c -o build/labs/memmove
# 打印左/右重叠及截断结果。
	./build/labs/memmove
# 重新构建合法/损坏启动数据观察。
lab-boot-info:
# 建立观察程序目录，保持所有可重建产物在build。
	@mkdir -p build/labs
# 下面续行按O0保留便于调试的步骤，链接当前观察所需的同一自有实现。
	$(HOST_CC) -std=c11 -O0 -g -Wall -Wextra -Werror -Iinclude \
	    labs/host/boot/boot_info_probe.c kernel/boot/info.c -o build/labs/boot-info
# 打印启动合同的正常与独立失败原因。
	./build/labs/boot-info

# 用户C使用自己的低地址模型、入口和库；绝不沿用内核高地址链接脚本。
USER_FLAGS := --target=x86_64-unknown-none-elf -std=c11 -O1 -g -Wall -Wextra -Werror -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -mgeneral-regs-only -mcmodel=small
# 每个配置拥有独立用户对象，避免跨配置复用旧二进制。
USER_OBJECTS := $(OUT)/user/start.o $(OUT)/user/hello.o $(OUT)/user/bytes.o $(OUT)/user/syscall.o
# 入口汇编与内核汇编分别形成对象，符号_start不在同一个链接目标中冲突。
$(OUT)/user/start.o: labs/airios/start.S
# 先建立独立用户产物目录。
	@mkdir -p $(dir $@)
# 当前交叉编译器不链接宿主运行时。
	$(CC) $(USER_FLAGS) -c $< -o $@
# 用户C程序只读取自有用户库声明。
$(OUT)/user/hello.o: labs/airios/hello.c labs/airios/include/airi_libc.h
# 先建立输出目录。
	@mkdir -p $(dir $@)
# 编译为用户模型对象。
	$(CC) $(USER_FLAGS) -Ilabs/airios/include -c $< -o $@
# 用户库的字节实现与系统调用实现共用这一规则。
$(OUT)/user/%.o: labs/airios/libc/%.c labs/airios/include/airi_libc.h
# 输出目录与当前配置绑定。
	@mkdir -p $(dir $@)
# 所有用户库均使用同样的freestanding约束。
	$(CC) $(USER_FLAGS) -Ilabs/airios/include -c $< -o $@
# 用户独立静态ELF是嵌入输入，不是内核的直接链接对象。
$(OUT)/user/hello.elf: $(USER_OBJECTS) labs/airios/program.ld
# 用户只链接自己的_start、main和libc子集，无宿主依赖。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_OBJECTS) -o $@
# 包装汇编明确依赖本配置用户ELF，文件变动就重新嵌入。
$(OUT)/labs/airios/images.o: labs/airios/images.S $(OUT)/user/hello.elf $(OUT)/user/processes.elf $(OUT)/user/worker.elf $(OUT)/user/files.elf
# 建立包装对象目录。
	@mkdir -p $(dir $@)
# 单引号保护路径宏内部双引号，.incbin只读本配置生成文件。
	$(CC) $(CPPFLAGS) $(CFLAGS) -DAIRI_HELLO_ELF='"$(OUT)/user/hello.elf"' -DAIRI_PROCESS_ELF='"$(OUT)/user/processes.elf"' -DAIRI_WORKER_ELF='"$(OUT)/user/worker.elf"' -DAIRI_FILES_ELF='"$(OUT)/user/files.elf"' -c $< -o $@

# 第二个用户程序只复用用户入口与库，不把hello的main再次链接进去。
USER_COMMON := $(filter-out $(OUT)/user/hello.o,$(USER_OBJECTS))
# 普通用户C工作负载仍使用相同低地址编译约束。
$(OUT)/user/processes.o: labs/airios/processes.c labs/airios/include/airi_libc.h
# 创建当前配置目录。
	@mkdir -p $(dir $@)
# 只读取用户库头，不引入内核实现。
	$(CC) $(USER_FLAGS) -Ilabs/airios/include -c $< -o $@
# 多进程工作负载有自己的独立静态ELF。
$(OUT)/user/processes.elf: $(USER_COMMON) $(OUT)/user/processes.o labs/airios/program.ld
# 入口与库复用，main只选择processes.o。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/processes.o -o $@

# exec目标独立编译，保持同一用户ABI与链接布局。
$(OUT)/user/worker.o: labs/airios/worker.c labs/airios/include/airi_libc.h
# 建立当前配置输出目录。
	@mkdir -p $(dir $@)
# 普通用户C仅链接自己的运行库。
	$(CC) $(USER_FLAGS) -Ilabs/airios/include -c $< -o $@
# worker也有自己的_start和main，原始ELF只在包装时进入内核。
$(OUT)/user/worker.elf: $(USER_COMMON) $(OUT)/user/worker.o labs/airios/program.ld
# 复用用户入口与库，不混合其他程序main。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/worker.o -o $@

# 文件演示有独立用户main，复用自有入口与库。
$(OUT)/user/files.o: labs/airios/files.c labs/airios/include/airi_libc.h
# 按配置建立目录。
	@mkdir -p $(dir $@)
# 同样禁止宿主库和浮点寄存器。
	$(CC) $(USER_FLAGS) -Ilabs/airios/include -c $< -o $@
# 只链接这份工作负载的main。
$(OUT)/user/files.elf: $(USER_COMMON) $(OUT)/user/files.o labs/airios/program.ld
# 独立低地址ELF由内核包装后装载。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T labs/airios/program.ld $(USER_COMMON) $(OUT)/user/files.o -o $@
```

**更新启动验收。** 正常可写设备必须出现JOURNAL_DEMO_OK，受限设备必须明确跳过。

<!-- write: scripts/boot.py -->
```python
"""QEMU launch and bounded boot checks; success requires a kernel marker."""  # 固定虚拟机参数并按本轮内核标记判断启动结果
import json  # 在 Python 对象与 JSON 文本之间转换，不执行其中内容。
from pathlib import Path  # Path 表示主机路径对象，/ 在这里拼接路径片段。
import shutil      # 取得目录复制、文件复制与临时结果清理接口。
import socket      # 取得主机通信连接接口，QMP/调试器通过独立连接交接。
import subprocess  # 以参数列表启动已有外部程序，并读取输出或退出码。
import tempfile    # 创建互不冲突的临时路径，用完后显式回收。
import time  # 单调时钟计算截止时间，sleep 给设备与进程处理时间。
from tools import ROOT, tool  # ROOT 确定项目位置，tool 按已建立优先级寻找工具。

def command(profile, work, gui=False, debug=False, debug_port=1234):  # 返回 QEMU 参数列表；profile 选镜像，work 保存本轮可写固件与日志。
    work.mkdir(parents=True,exist_ok=True)  # 建立本轮输出目录，已有目录允许复用。
    firmware=ROOT/'third_party/firmware'    # 定位固定固件目录。
    variables=work/'ovmf-vars.fd'  # 本轮固件变量单独存放，避免改动锁定的原始模板。
    shutil.copyfile(firmware/'edk2-i386-vars.fd',variables)  # 复制变量模板，QEMU 可写副本而保持外部依赖原件。
    cmd=[tool('qemu-system-x86_64','qemu'),'-machine','q35','-accel','tcg',  # 先选 QEMU 路径与 q35/TCG，参数列表按元素边界传递。
         '-cpu','qemu64','-smp','1','-m','256M','-vga','std',  # 固定CPU型号、单核、256MiB及标准VGA。
         '-drive',f'if=pflash,format=raw,unit=0,readonly=on,file={firmware/"edk2-x86_64-code.fd"}',  # 只读代码固件是第0个 pflash 单元，f 字符串填入实际路径。
         '-drive',f'if=pflash,format=raw,unit=1,file={variables}',  # 第1个 pflash 单元使用本轮可写变量副本。
         '-monitor','none','-cdrom',str(ROOT/'build'/profile/'airios.iso'),'-boot','d','-no-reboot',  # 挂载对应配置镜像，从光盘启动，故障时不循环重启。
         '-serial',f'file:{work/"serial.log"}']  # 默认 UART 输出保存到本轮 serial.log。
    disk = work / 'disk.raw' # 每个运行目录只使用项目自己的独立实验盘。
    if disk.is_symlink(): # 拒绝把实验写入重定向到外部文件。
        raise RuntimeError('Experiment disk cannot be a symlink') # 不跟随未知目标。
    if not disk.exists(): # 保留已有实验盘用于后续重启恢复观察。
        with disk.open('xb') as stream: # 排他创建，不覆盖竞争产生的文件。
            stream.truncate(8 * 1024 * 1024) # 稀疏零文件，固定8MiB。
    if not disk.is_file() or disk.stat().st_size != 8 * 1024 * 1024: # 当前设备容量固定，未知文件不交给guest写。
        raise RuntimeError('Expected an 8 MiB project experiment disk') # 要求显式处理错误，不自动truncate旧文件。
    drive = f'if=none,id=airi-disk,format=raw,file={disk},cache=writeback' # QEMU后端可缓存，持久化需客体FLUSH。
    if profile.removeprefix('desktop-') == 'block-readonly': # 故障演示使用真实只读设备特征。
        drive += ',readonly=on' # 不修改宿主文件的权限设置。
    if profile.removeprefix('desktop-') != 'block-missing': # 缺设备配置故意不接实验盘。
        cmd += ['-drive', drive, '-device', 'virtio-blk-pci,drive=airi-disk,disable-modern=on,vectors=0'] # 固定legacy与无MSI-X布局，现代接口另行学习。
    if not gui: cmd+=['-display','none']  # 无人操作时隐藏显示窗口，guest 仍可绘图并经 QMP 截帧。
    if debug: cmd+=['-S','-gdb',f'tcp:127.0.0.1:{debug_port}']  # -S 暂停CPU，GDB只监听回环地址指定端口。
    return cmd  # 仅返回参数列表，调用者决定何时启动与回收进程。
def qmp_execute(sockpath, execute, arguments=None):  # 本机连接只执行本次指定的控制请求。
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:  # 外层拥有底层连接，最后关闭。
        sock.settimeout(5)  # 连接、发送和读取最多阻塞五秒。
        sock.connect(str(sockpath))  # 路径定位QEMU控制端点，不是普通输出文件。
        with sock.makefile('rb') as stream:  # 内层包装按行读取字节，先关闭包装再关闭连接。
            json.loads(stream.readline())  # 消费服务器问候，JSON库直接接受bytes。
            result = None  # 尚未收到动作结果，不与空字典混淆。
            request = {'execute': execute}  # 当前控制动作保存为请求字段。
            if arguments is not None:  # 空字典也是显式参数，只有None表示不提供。
                request['arguments'] = arguments  # 将本次参数加到同一请求。
            for req in ({'execute': 'qmp_capabilities'}, request):  # 先协商能力再发实际动作。
                sock.sendall((json.dumps(req) + '\n').encode())  # 直到全部消息字节交出或失败，不假设一次write写完。
                while True:  # 跳过异步事件，等待当前请求的结果。
                    reply = json.loads(stream.readline())  # 按消息换行取得完整JSON字节。
                    if 'error' in reply:  # QEMU拒绝了当前请求。
                        raise RuntimeError(str(reply['error']))  # 保留服务器的具体错误。
                    if 'return' in reply:  # event不满足完成条件。
                        result = reply['return']  # 取得当前请求结果。
                        break  # 此请求已完成，进入下一个或返回。
            return result  # 返回实际动作结果，两个with仍按由内到外关闭资源。

def qmp_screenshot(sockpath, output):  # 把本轮截图目标路径交给QEMU控制端点。
    qmp_execute(sockpath, 'screendump', {'filename': str(output)})  # 控制显示设备保存PPM，不向UART发送命令。
def check(profile='normal',timeout=35):  # 每次启动最多35秒，成功条件由配置对应完成标记决定。
    work=ROOT/'artifacts'/('boot-'+profile)  # 不同配置分开保存启动证据。
    work.mkdir(parents=True,exist_ok=True)   # 建立本轮输出目录，已有目录允许复用。
    (work/'result.json').unlink(missing_ok=True)  # 删除上次汇总，失败不能留下旧成功身份。
    serial=work/'serial.log'  # 当前检查只读取这次启动的串口文件。
    serial.unlink(missing_ok=True)  # 删去旧日志；不存在不报错，旧 READY 不会使新启动提前通过。
    # AF_UNIX paths are capped at 104 bytes on macOS. Source exports can be deep.
    endpoints=tempfile.TemporaryDirectory(prefix='airios-qmp-',dir='/tmp')  # 在短/tmp路径建立控制端点，避免 macOS UNIX socket 路径过长。
    sockpath=Path(endpoints.name)/'qmp.sock'  # 临时目录中的 qmp.sock 命名本次控制连接。
    cmd=command(profile,work)+['-qmp',f'unix:{sockpath},server=on,wait=off']  # 在固定启动参数后追加控制端点，QEMU不等待客户端才启动。
    (work/'command.json').write_text(json.dumps(cmd,indent=2))  # 保存实际参数列表供本轮复现。
    case = profile.removeprefix('desktop-')  # 图形前缀不改变故障注入含义。
    graphics = profile.startswith('desktop-') and case != 'no-framebuffer'  # 有绘图实现且设备存在才应生成画面。
    expected='[AIRI_PANIC] required Limine memory map is missing' if case=='missing-memory' else '[AIRI_P3_READY]'  # 正常配置等完整进程阶段，旧阶段标记只保留兼容。
    if case == 'block-missing': # 缺盘是明确设备初始化失败，不能继续宣称存储可用。
        expected = '[AIRI_PANIC] virtio block initialization failed' # 错误码另外核对。
    if case in ('trap-fatal', 'stack-guard'): # 未处理异常镜像必须进入具体停止路径。
        expected = '[AIRI_PANIC] unhandled CPU exception' # 不以 QEMU 存活推断故障已处理。
    started=time.monotonic()  # 单调秒数作为经过时间起点，不受系统时钟调整影响。
    with (work/'qemu.log').open('w') as errors:  # 主机 QEMU 输出单独存档，with 保证文件最终关闭。
        process=subprocess.Popen(cmd,stdout=errors,stderr=errors)  # 启动后立即取得进程句柄，轮询 guest 标记而非等待进程退出。
        try:  # 受保护区域失败也必须进入后面的资源回收。
            while time.monotonic()-started<timeout:  # 按实际经过秒数限制整次启动等待。
                text=serial.read_text(errors='replace') if serial.exists() else ''  # 日志尚未创建时取空串；暂时不完整的字节以替代字符读取。
                complete_lines = text.splitlines(keepends=True)  # 保留行结束符，完整记录与写到一半的标记可区分。
                if any(line.endswith('\n') and line.startswith(expected) for line in complete_lines):  # 生成器逐行检查，至少一个完整且以预期标记开头才成立。
                    if case in ('missing-memory', 'trap-fatal', 'stack-guard', 'block-missing'):  # 失败配置只允许具体拒绝记录，不能同时宣布启动完成。
                        if case == 'block-missing': # 必须确认真实枚举结果为缺设备。
                            assert '[BLOCK_INIT_ERROR] error=-19' in text # 不能用其他初始化失败充数。
                        if case == 'stack-guard': # 坏栈必须真的转到双重故障的独立栈。
                            assert '[STACK_GUARD_PROBE]' in text and '[EXCEPTION] vector=8 error=0x0 ' in text # 不能把任意PANIC冒充保护页演示。
                        assert '[AIRI_P1_READY]' not in text  # 错误路径不得出现成功标记。
                    else:  # 按当前profile的另一条完整验证路径处理。
                        assert '[EXEC_FAILURE_OK]' in text and '[EXEC_WORKER_OK]' in text and '[EXEC_COMMIT]' in text # 坏映像保持与成功替换都须实际运行。
                        block_marker = '[BLOCK_TIMEOUT_OK]' if case == 'block-timeout' else '[BLOCK_READONLY_OK]' if case == 'block-readonly' else '[BLOCK_DEMO_OK]' # 不同设备条件分别核对真实结果。
                        assert '[BLOCK_READY]' in text and block_marker in text # 完成、拒绝或超时必须符合本配置。
                        journal_marker = '[JOURNAL_SKIPPED]' if case in ('block-timeout', 'block-readonly') else '[JOURNAL_DEMO_OK]' # 没有可写设备时不冒充日志完成。
                        assert journal_marker in text # 正常配置必须真实恢复并替换坏尾。
                        assert '[FILES_USER_OK]' in text and '[FD_EXEC_OK]' in text # RAM文件和成功exec继承均完成。
                        assert '[PROCESS_FORK_WAIT_OK]' in text and '[PROCESS_DEMO_OK]' in text # 复制、真实阻塞、timer换身份和回收都须完成。
                        assert '[ELF_LOAD]' in text and '[ELF_USER_OK]' in text # 独立ELF段、BSS与用户libc必须真正运行。
                        assert '[USER_DEMO_OK]' in text # 用户输出、指针拒绝和用户故障后内核必须继续。
                        assert '[TASK_DEMO_OK]' in text # 两个独立栈必须交错并全部回收。
                        assert '[TIMER_DEMO_OK]' in text # 时钟必须在真实IRQ唤醒后完成。
                        assert '[AIRI_P2_READY]' in text # 当前主线还必须完成页池、自有页表和堆。
                        assert '[TRAP_DEMO_OK] ud=6 gp=13 resumed=2' in text # 正常启动必须真的从两种异常栈返回。
                        assert '[AIRI_PANIC]' not in text  # 正常完成路径不得混入内核停止记录。
                        assert f'graphics={int(graphics)}' in text  # 报告值须与本次选择的呈现实现一致。。
                        assert '[MEMORY_SUMMARY]' in text  # 完成之前应已记录实际内存总计。
                        if case=='edit-experiment':  # 修改实验额外核对替换后的文字。
                            assert 'I changed the output and rebooted' in text  # 确认运行的是实验配置的输出行为。
                        if graphics:  # 有图形的完成路径才请求显示面截图。
                            qmp_screenshot(sockpath,work/'screen.ppm')  # 在完整 READY 之后读取对应已完成画面。
                    result={'profile':profile,'passed':True,'expected_marker':expected,  # 记录配置、成功与本次实际等待的标记。
                            'seconds':round(time.monotonic()-started,2)}  # 记录经过秒数，round(...,2) 保留两位小数。
                    (work/'result.json').write_text(json.dumps(result,indent=2)+'\n')  # 结果编码为便于阅读的 JSON，并补文件末尾换行。
                    print(f'PASS boot/{profile} ({result["seconds"]}s)')  # 打印本次配置和耗时，而非以QEMU仍运行判定通过。
                    return result  # 当前配置标记与截图核对成功，返回这次启动记录。
                if process.poll() is not None:  # poll 非 None 表示主机 QEMU 已退出，不能继续等 guest 标记。
                    raise RuntimeError(f'QEMU exited before marker; see {work}')  # 提前退出时报告证据目录。
                time.sleep(0.1)  # 给虚拟CPU和日志写入100毫秒处理时间，再检查。
            raise RuntimeError(f'Boot timeout after {timeout}s; see {work}')  # 耗尽截止时间后明确失败，并保留该配置的日志。
        finally:  # 成功返回与异常离开都进入收尾。
            process.terminate()  # 先请求主机 QEMU 进程结束。
            try:  # terminate只是结束请求，随后仍需收回子进程。
                process.wait(timeout=5)  # 最多等待五秒，防止清理本身无限卡住。
            except subprocess.TimeoutExpired:  # QEMU没有在正常终止期限内退出。
                process.kill()  # 强制结束本轮仍存活的QEMU。
                process.wait()  # 收回已结束子进程的退出状态和资源。
            sockpath.unlink(missing_ok=True)  # 删除本轮控制端点，端点已消失时允许继续。
            endpoints.cleanup()  # 回收临时目录，不删除保留在 artifacts 的证据。
```

<a id="journal-run"></a>
## 完整运行与观察

```sh
./airios test # 七组主机观察、九种有超时启动和三条真实输入。
./airios run # 看BLOCK_DEMO_OK之后的JOURNAL_DEMO_OK，再继续串口命令。
```

正常真实guest输出应包含committed=2、corrupt_tail=1、prefix=1、replaced=gamma、restored=1。模型输出的16条故障和真实guest的位损坏分别保存；它们证明各自明确的边界，不互相替代。

调试可在每次io.flush前后比较journal.count与目标数据/commit字节。最后commit写出但FLUSH尚未成功时count仍旧；重新scan之后按介质事实重建，不能拿旧count覆盖磁盘。下一节把这个服务接到用户系统调用，并用同一实验盘跨QEMU重新启动验证恢复；当前还没有用户日志接口或跨启动证据。
