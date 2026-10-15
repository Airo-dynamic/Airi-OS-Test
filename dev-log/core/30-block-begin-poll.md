# K30 · 提交设备请求与收集完成分成两个动作

从K29继续。用户管道已经能阻塞并让别的用户继续，但磁盘perform仍在一次内核调用里等到底。先拆开驱动边界：begin只接收请求，poll只检查一次；原同步read/write/flush用这两个函数组合，旧日志行为保持。下一节的队列才有机会在begin之后安排其他工作，而不是把一个同步函数包在“异步”名字里。

<a id="block-begin-poll"></a>
## 0表示接收，还是0表示完成，取决于当前接口

begin验证操作、扇区、RO与FLUSH能力，确认没有busy后，复制写数据、填写描述符、发布avail并通知设备。返回0只说明这次请求被接收；active_type与request_deadline留在驱动静态状态中，不能随着C局部变量返回而丢失。即使设备已经写完used，只要还没由poll收集，busy仍真，第二次begin返回−16，不覆盖旧请求。

poll返回1表示仍在途，0表示本次完成，负数表示错误。它不hlt、不循环，也不在每次检查时重设截止。观察used后继续核对链首id、长度与status，再复制读数据并清busy。读输出可以是NULL表示明确丢弃，但DMA静态区仍要完成收集后才能复用。内核里的单个可信消费者拥有这组接口，用户不能直接调用poll抢别人的完成。

两秒deadline在首次begin固定，反复poll不能无限延长。超时仍reset并隔离静态DMA，后续begin返回−5，不自动重试。设备完成很快时，第一次poll也可以立即返回0；不能为演示效果硬要求一定观察到pending。

**更新块接口。** 同步0/负错误和poll的1待完成分别声明，pending表示未收集的所有权。

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
int airi_block_begin(unsigned operation, uint64_t sector, const void *write_data); // 0读/1写/4同步；0表示已接收，不表示完成。
int airi_block_poll(void *read_data); // 1仍在途、0完成、负错误；读输出可NULL表示丢弃，不释放DMA前不重用。
bool airi_block_pending(void); // 已接收但未收集完成，含设备已完成却未poll的状态。
void airi_block_demo(void); // 只操作项目独立实验盘末扇区。
#endif // 结束保护。
```

<a id="block-sync-adapter"></a>
## 同步适配只收集自己成功提交的请求

perform保存IF后调用begin；只有它成功才反复poll，遇1由PIT唤醒后继续，其他结果直接返回。若设备已经属于另一请求，begin的−16直接交给调用者，不得继续poll把别人的数据收走。临时开IRQ时仍在CS0，不抢占内核，旧同步日志顺序不变。

启动演示先begin读同一扇区，确认pending，并立刻第二次begin得到−16；接着poll收集并比较原字节。BLOCK_NONBLOCK_OK证明的是独占提交和真实完成收集，不声称已经让用户等待或实现多在途队列。只读设备同样可验证读；故意漏发布idx的故障配置先超时隔离，不再运行这个正常演示。

**按完整版本更新驱动。** 描述符格式与发布顺序沿K25，只把提交、一次收集和同步等待分开。

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
static uint32_t active_type; // 当前在途操作只属于成功begin的调用者。
static uint64_t request_deadline; // 截止跨begin/poll调用保存，不能每次poll重置。
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
int airi_block_begin(unsigned type, uint64_t sector, const void *write_data) { // 只提交，不在这个调用中等设备完成。
    if ((type != 0 && type != 1 && type != 4) || (type == 1 && !write_data)) // 内核接口也验证操作与输入，用户不能直接调用。
        return -22; // -EINVAL。
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
    active_type = type; // 类型与静态DMA所有权一起保存到后续poll。
    busy = true; // 先取得静态请求所有权。
    request.type = type; // IN=0、OUT=1、FLUSH=4。
    request.reserved = 0; // 协议保留字段写零。
    request.sector = type == 4 ? 0 : sector; // FLUSH不指定数据扇区。
    request.status = 0xff; // 设备必须覆盖状态，不能误用上次成功。
    if (type == 1) // 写请求先复制到内核长期DMA缓冲。
        airi_memcpy(request.data, write_data, 512); // 上层栈和用户页都不交给设备。
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
    request_deadline = airi_timer_ticks() + 200; // 一次提交只建立一个真实截止，不因多次poll延长。
    airi_irq_restore(flags); // 返回到原调用者的IF策略，不在begin中睡眠。
    return 0; // 仅表示已接受，busy直到poll收集或错误隔离才释放。
}
bool airi_block_pending(void) { // 仅查询驱动所有权状态。
    return busy; // 设备可能已写used，未收集仍属于在途请求。
}
int airi_block_poll(void *buffer) { // 只检查一次，不hlt，不要求用户指针。
    if (!busy) // 没有本驱动接收的请求可收集。
        return -22; // -EINVAL，不能读上次残留status当新完成。
    uint64_t flags = airi_irq_save(); // 完成收集和busy撤销作为同一次内核操作。
    if (used[1] == used_index) { // 设备尚未发布新完成。
        int result = airi_timer_ticks() >= request_deadline ? stop(-110) : 1; // 真超时才reset并隔离，1明确仍在途。
        airi_irq_restore(flags); // 无完成也不改变原IF。
        return result; // 调用者可去做别的工作，之后再poll。
    }
    unsigned type = active_type; // 不从新调用参数猜测旧请求类型。
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
    if (!result && type == 0 && buffer) // 只有成功读才能把设备数据交给上层。
        airi_memcpy(buffer, request.data, 512); // 复制结束后设备DMA缓冲才可供下次覆盖。
    (void)airi_in8(io + 19); // 清可能遗留的legacy ISR状态，完成仍以used环判断。
    busy = false; // 已确认完成，可以重用同一静态请求。
    airi_irq_restore(flags); // 系统调用进入时IF0，普通内核演示进入时IF1。
    return result; // 收集完成后才能重用静态DMA，0不是应用事务持久化。
}
static int perform(unsigned type, uint64_t sector, void *buffer) { // 原同步接口组合begin与poll，语义保持。
    uint64_t flags = airi_irq_save(); // 等待过程仍临时开IRQ，但不抢占CS0。
    int result = airi_block_begin(type, sector, buffer); // busy或参数错误不会偷取别人的完成。
    if (!result) // 只有本次提交成功才可以收集。
        while ((result = airi_block_poll(buffer)) == 1) // 1是驱动待完成状态，不是成功字节数。
            __asm__ volatile("sti; hlt; cli" ::: "memory"); // 用PIT唤醒后重查，截止由首次begin固定。
    airi_irq_restore(flags); // 系统调用IF0与普通内核IF1都按原样恢复。
    return result; // 0或负错误，旧read/write/flush契约不变。
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
    if (airi_block_begin(0, sector, NULL) || !airi_block_pending() || airi_block_begin(0, sector, NULL) != -16) // 接收后即使设备已快完成，也不可覆盖未收集请求。
        airi_panic("nonblocking block submission ownership failed"); // 不把第二次提交覆盖旧DMA。
    int polled; // 首次poll可能已完成，不能伪造必须观测到pending。
    while ((polled = airi_block_poll(after)) == 1) // 每次只看一次，等待策略留在调用者。
        __asm__ volatile("sti; hlt" ::: "memory"); // 此启动演示等待真实PIT，不改变begin的非等待语义。
    if (polled || airi_block_pending() || airi_memcmp(before, after, 512)) // 完成身份和原内容都要正确。
        airi_panic("nonblocking block completion failed"); // 不能只看busy清了。
    airi_log("[BLOCK_NONBLOCK_OK] accepted=1 overlap=-16 collected=1"); // 未收集前所有权独占，实际数据读回。
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

<a id="block-log-interlock"></a>
## 日志事务不能偷用一个已在途的块请求

service在进入journal_append或journal_read前检查pending，若设备属于别的请求就返回−16。这个拒绝发生在日志算法之前，不会把一个尚未开始的事务误置为poisoned。count只读缓存数值，不触发设备I/O。当前单核不可抢占内核使一次同步日志操作内部不会插进另一用户提交；未来异步完成只在普通服务边界处理，IRQ仍不直接发新日志事务。

**更新正式日志服务。** 保存明确忙状态，旧记录与错误语义不变。

<!-- write: kernel/storage/service.c -->
```c
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
    if (airi_block_pending()) // 异步适配接入后，不能把别人的在途请求当本次日志I/O。
        return -16; // -EBUSY，未进入日志所以不会错误置poisoned。
    return airi_journal_append(&store, data, bytes); // 数据、提交和同步均完成后才成功。
}
long airi_storage_read(unsigned index, void *data, size_t capacity) { // 单条记录不是RAM文件偏移接口。
    if (airi_block_pending()) // 读日志同样不能收集其他调用者的完成。
        return -16; // -EBUSY，原日志状态保持。
    return available ? airi_journal_read(&store, index, data, capacity) : -19; // 返回实际长度或明确错误。
}
```

**更新启动检查。** 可用读设备必须真实运行begin/重叠拒绝/poll，隔离设备不得继续请求。

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
    expected='[AIRI_PANIC] required Limine memory map is missing' if case=='missing-memory' else '[AIRI_P3_READY]' if case == 'block-timeout' else '[AIRI_P4_READY]' # 正常配置等完整存储阶段，隔离盘只保留进程可用标记。
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
                        if case != 'block-timeout': # 隔离盘不再允许新提交。
                            assert '[BLOCK_NONBLOCK_OK]' in text # begin接受、重叠拒绝和poll收集已运行。
                        journal_marker = '[JOURNAL_SKIPPED]' if case in ('block-timeout', 'block-readonly') else '[JOURNAL_DEMO_OK]' # 没有可写设备时不冒充日志完成。
                        assert journal_marker in text # 正常配置必须真实恢复并替换坏尾。
                        storage_marker = '[STORAGE_SKIPPED]' if case == 'block-timeout' else '[STORAGE_RECOVERED]' # 只读仍可恢复，隔离盘不能提供空日志。
                        assert storage_marker in text # 正式区必须实际扫描或明确未提供。
                        assert '[WAIT_USER_OK]' in text and '[WAIT_IDLE_OK]' in text and '[WAIT_DEMO_OK]' in text # 真阻塞、超时与现场继续均已完成。
                        assert text.index('[WAIT_TIMEOUT_OK]') < text.index('[WAIT_BUSY_DONE]') # 纯计算用户不能饿死等待超时。
                        assert '[PIPE_USER_OK]' in text # 有界流、背压、关闭与fork共享均已实际运行。
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

```sh
./airios test # 原同步、管道、故障与跨启动恢复继续通过。
./airios run # 看BLOCK_NONBLOCK_OK，之后仍可run storage和run waits。
```

正常结果为accepted=1、overlap=−16、collected=1；读回内容和原扇区一致。故障block-timeout仍在真实PIT截止后返回−110，下一次读得到−5，不因接口拆分延长等待或重用DMA。原BLOCK_DEMO_OK、JOURNAL_DEMO_OK、WAIT_DEMO_OK和双启动恢复仍要成立。

下一节在这个可信单设备消费者上加有限SQ/CQ与稳定请求身份。设备描述符id、软件请求cookie、注册缓冲句柄将分别有自己的含义；不能直接用返回顺序猜出完成属于谁。
