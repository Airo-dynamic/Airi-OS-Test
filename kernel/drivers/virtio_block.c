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
