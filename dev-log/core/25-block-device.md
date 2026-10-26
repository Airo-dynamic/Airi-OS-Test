# K25 · 把一个扇区请求交给设备，再确认谁完成了它

K24 的 /notes 可以跨程序保留，但内容仍在 guest RAM 中，关闭 QEMU 就消失。要跨启动保存，最终需要把内容交给设备；文件名和共享偏移并不能直接告诉设备该访问哪个扇区。本章先独立建立这段交接：读取实验盘最后一个扇区、写入可辨认模式、实际读回比较、恢复原扇区，再请求FLUSH。这样可以把文件偏移与设备扇区、CPU指针与DMA地址、队列发布与设备完成分别看清。

固定硬件为QEMU q35、bus0上的legacy VirtIO PCI块设备，无MSI-X、单请求队列。设备接口参考[VirtIO 1.3规范](https://docs.oasis-open.org/virtio/virtio/v1.3/virtio-v1.3.html)中legacy PCI、split virtqueue和block device部分；本驱动选择该规范仍定义的legacy接口，不实现现代capability/MMIO transport、IOMMU、多队列或设备中断完成。

<a id="block-own-disk"></a>
## 实验盘是项目生成的文件，客体看到的是扇区

boot.command在本次运行目录建立disk.raw，大小8MiB；QEMU将它作为独立块设备暴露给客体。没有挂接宿主物理盘或项目数据目录。已有文件必须大小正确、是普通文件且不是符号链接；脚本不会自动截断它，以便后面验证同一实验盘重启后的内容。删除实验结果目录会同时删除该盘。

file=指定宿主文件，format=raw表示不解析qcow2头，cache=writeback允许后端缓存；驱动必须主动发FLUSH才能请求后端同步。disable-modern=on强制本节实现的legacy寄存器接口，vectors=0固定无MSI-X配置，设备专有字段从端口偏移20开始。现代VirtIO不能直接套用这些偏移。

<a id="block-pci-and-dma"></a>
## PCI配置找到设备，DMA地址找到内存

demo 要读最后一个扇区，先要找到负责这个盘的设备，再告诉设备把结果写进哪一块 RAM。这两步会出现不同地址，不能拿同一数值代用。CPU 的 in/out 指令访问端口空间，C 指针访问经页表翻译的 RAM；描述符交给设备的则是 guest 物理地址。

先找端口。向 0xcf8 写入 PCI 配置地址，便选择了某个槽、功能和字段；随后从 0xcfc 读取那个字段。枚举固定 bus0 后，以 vendor=0x1af4、device=0x1001 识别本设备，再从 BAR0 取得 I/O 端口基址。PCI 命令中的 I/O 允许端口访问，bus master 则允许设备发起 DMA。

接下来才准备 DMA 目标。内核高半区指针只对 CPU 当前页表有意义，设备不会替我们走那份页表；描述符必须保存它能访问的 guest 物理地址。

设备也不能调用 C 函数取得请求，它从共享 RAM 读取描述。队列使用内核 BSS 里 4096 对齐的三页数组。当前Limine启动合同保证内核物理映像连续，所以该数组的整个范围可用kernel physical base加VA偏移换成连续PA；代码核对对象和长度都在kernel_size内。不能对普通堆指针套这个公式，也不能把三个碰巧连续的虚拟页当成物理连续。静态请求也属于内核映像，整个启动期间不释放。

**新建 `include/airi/x86_io.h`。** 端口与中断标志操作限制在x86适配，通用文件代码不包含它。

<!-- write: include/airi/x86_io.h -->
```c
#ifndef AIRI_X86_IO_H // 端口与IF操作仅供x86平台和设备适配。
#define AIRI_X86_IO_H // 包含保护。
#include <stdint.h> // 硬件字段宽度。
static inline uint8_t airi_in8(uint16_t port) { // 从一个I/O端口读取八位。
    uint8_t value; // AL接收设备值。
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port)); // 常量端口或DX，不能优化掉真实访问。
    return value; // 值不是内存地址。
}
static inline uint16_t airi_in16(uint16_t port) { // 队列大小等十六位字段。
    uint16_t value; // AX接收。
    __asm__ volatile("inw %1, %0" : "=a"(value) : "Nd"(port)); // 读取设备端口。
    return value; // 保留字段宽度。
}
static inline uint32_t airi_in32(uint16_t port) { // PCI和VirtIO三十二位字段。
    uint32_t value; // EAX接收。
    __asm__ volatile("inl %1, %0" : "=a"(value) : "Nd"(port)); // 真实端口访问。
    return value; // 不做宿主系统调用。
}
static inline void airi_out8(uint16_t port, uint8_t value) { // 写设备状态等八位字段。
    __asm__ volatile("outb %0, %1" :: "a"(value), "Nd"(port) : "memory"); // memory阻止普通内存发布越过设备通知。
}
static inline void airi_out16(uint16_t port, uint16_t value) { // 写队列号与通知。
    __asm__ volatile("outw %0, %1" :: "a"(value), "Nd"(port) : "memory"); // AX写DX指定端口。
}
static inline void airi_out32(uint16_t port, uint32_t value) { // 写配置与队列PFN。
    __asm__ volatile("outl %0, %1" :: "a"(value), "Nd"(port) : "memory"); // EAX提供三十二位值。
}
static inline uint64_t airi_irq_save(void) { // 暂停本CPU外部可屏蔽IRQ，调用者必须恢复原IF。
    uint64_t flags; // 保存调用前RFLAGS。
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory"); // 保存再关，不把不可抢占内核变成多核锁。
    return flags; // 只用IF决定恢复方式。
}
static inline void airi_irq_restore(uint64_t flags) { // 不修改其他特权标志。
    if (flags & 0x200) // 原调用者允许中断。
        __asm__ volatile("sti" ::: "memory"); // 恢复IF，不无条件打开系统调用上下文。
}
#endif // 结束保护。
```

**新建 `include/airi/block.h`。** 上层接口一次一个512字节扇区，返回0或负错误，不返回普通文件偏移。

<!-- write: include/airi/block.h -->
```c
#ifndef AIRI_BLOCK_H // 通用块接口不暴露PCI端口和VirtIO布局。
#define AIRI_BLOCK_H // 包含保护。
#include <airi/boot.h> // 当前启动提供内核连续物理映像位置。
int airi_block_init(const AiriBootInfo *info); // 固定单队列，失败返回负错误。
uint64_t airi_block_sectors(void); // 以512字节扇区计，不等同设备逻辑块大小配置。
int airi_block_read(uint64_t sector, void *buffer); // 一次精确一个512字节扇区，0成功。
int airi_block_write(uint64_t sector, const void *buffer); // 同步复制并等待设备完成。
int airi_block_flush(void); // 请求后端同步，不宣称真实硬件掉电已实测。
void airi_block_demo(void); // 只操作项目独立实验盘末扇区。
#endif // 结束保护。
```

<a id="block-ring-ownership"></a>
## 描述符、available与used各自发布什么

以 demo 的首次 read 为线索：CPU 已准备目标缓冲，却还要发布“请读这个扇区”的描述，并识别稍后哪条描述被完成。split ring 因而分成三部分。描述符表记录物理地址、长度、flags和next。available环由驱动写，放可用描述符链的首编号；used环由设备写，放完成的链首id和设备写入长度。两个idx是不断递增的16位计数，用idx%queue_size定位数组槽，跨65535回到0仍按16位差值解释。本节最多一个在途请求，因此每次只接受一条完成，id必须为链首0。

环中存的是链首编号，不是 512 字节内容本身。设备沿描述符找到头、数据和状态。一个 read 链为：描述符0指向16字节请求头，设备只读；描述符1指向512字节数据，设备写；描述符2指向一个状态字节，设备写。WRITE标志的方向以设备为准，读磁盘时数据描述符反而要设置WRITE。写磁盘时数据是设备读，不设WRITE。FLUSH没有数据段，链直接0→2。

链准备好仍不代表设备可以消费；如果先公布 idx，设备可能读到尚未填完的地址。先写请求、描述符与 available 槽，再执行内存屏障，最后推进avail.idx并通知端口。推进idx是交出所有权的时刻；在used.idx确认完成前，不能改描述符或重用request.data。设备按相反顺序写数据与status，再发布used.idx；CPU观察新idx后执行屏障，核对id、长度和状态，再把读数据复制给上层。volatile保证共享字段真实访问，屏障约束发布顺序，二者用途不同；当前x86 QEMU是缓存一致DMA，不覆盖非一致缓存设备维护。

legacy队列大小由设备给出，不能自行缩小。本驱动只接受3到256之间且为2的幂的队列，按规范计算used页对齐，三页不够则拒绝。初始化只协商确实实现的RO与FLUSH特征，不启用indirect、event_idx或多队列。

<a id="block-timeout-owner"></a>
## 等待仍属于内核，超时不能让设备继续写已释放内存

现在请求已交给设备，CPU 不能把 request.data 交给下一次调用。它需要等 used.idx 证明设备离开这条链。本阶段同步等待完成。提交时保存RFLAGS并关闭IF；等待循环临时执行sti、hlt、cli，让PIT唤醒后检查used。即使设备恰好在hlt前完成，下一次PIT仍会唤醒，不依赖设备IRQ。CS0的IRQ只计时、不切用户进程，所以这是同步内核等待，不能称为用户任务已经阻塞并让其他用户继续。

如果截止时仍未等到完成，返回错误也不能推断设备已停止 DMA。截止值来自实际 100Hz tick，约两秒后返回 −110。错误路径写设备reset并记录读取到的status，同时永久把ready清掉。队列和请求都是静态存储，既不释放也不用于新请求；后续调用返回−5。这样即使reset尚未确认，晚到DMA也不会打到已借给别人的对象。它保留内存换取明确的失败边界，没有自动重连或重试写入。

三个故障配置截断的是不同环节：

- block-readonly 由 QEMU 暴露真实 RO 特征，驱动在写入前返回 −30。
- block-missing 不挂设备，枚举得到 −19 后按具体原因停止。
- block-timeout 故意漏发布 avail.idx，设备尚未收到可消费请求，驱动仍在真实 PIT 截止后隔离 DMA。它模拟驱动发布遗漏，没有制造硬件挂死或掉电。

**新建 `kernel/drivers/virtio_block.c`。** 先完成枚举、初始化和单链同步请求，再由demo实际读回验证。

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
```

静态队列和请求的位置已有确定存储，但设备还未获知它们。接着实现初始化：枚举 PCI、协商能力、计算环的实际布局并写入 QueuePFN，最后才把 ready 设为真。只给数组赋值不会自动让设备找到它。

<!-- append: kernel/drivers/virtio_block.c -->
```c
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
```

初始化返回以后，设备已经知道队列的位置，仍没有收到本次 read。继续追加 perform：先检查容量和所有权，再填写 0→1→2 链，发布 available，等待 used；上层的目标 buffer 只在完成验证后接收数据。

<!-- append: kernel/drivers/virtio_block.c -->
```c
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
```

perform 负责同一条链的整个寿命。最后追加公开 read/write/flush 包装和 demo：demo 将 capacity−1 作为扇区号，依次保存、写模式、读回比较、恢复并同步。这里恢复的是实验扇区内容，不能只以返回 0 代替逐字节读回。

<!-- append: kernel/drivers/virtio_block.c -->
```c
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

**更新启动调用顺序。** 内存映射、时钟和任务演示完成后初始化设备，再运行用户演示；缺盘不能悄悄通过。

先引入块接口声明。

<!-- replace: kernel/main.c -->
<!-- before -->
```c
#include <airi/user.h> // 验证ring3系统调用和故障隔离。
#include <airi/task.h> // 两个独立栈内核任务主动交接并回收。
#include <airi/timer.h> // 内存建立后开始真实设备中断。
```
<!-- after -->
```c
#include <airi/block.h> // 设备适配在内存、时钟之后初始化。
#include <airi/user.h> // 验证ring3系统调用和故障隔离。
#include <airi/task.h> // 两个独立栈内核任务主动交接并回收。
#include <airi/timer.h> // 内存建立后开始真实设备中断。
```

在时钟与任务演示后初始化设备并运行块演示，再继续用户程序；缺盘保持具体失败。

<!-- replace: kernel/main.c -->
<!-- before -->
```c
    airi_memory_init(info); // 顺序建立页池、自有映射与固定内核堆，完成本阶段演示。
    airi_timer_demo(); // 从PIT/PIC收到真实IRQ0，hlt后恢复到正常上下文。
    airi_task_demo(); // 当前只在正常内核上下文主动切换，IRQ不抢占内核任务。
    airi_user_demo(); // 正常与非法用户程序结束后仍回到启动栈和串口。
    airi_console_init(&console, info);  // 初始化内核静态控制台，借用整个启动期间有效的 info。
    console.service = airi_user_command; // 系统程序服务独立于可选主题/窗口扩展。
```
<!-- after -->
```c
    airi_memory_init(info); // 顺序建立页池、自有映射与固定内核堆，完成本阶段演示。
    airi_timer_demo(); // 从PIT/PIC收到真实IRQ0，hlt后恢复到正常上下文。
    airi_task_demo(); // 当前只在正常内核上下文主动切换，IRQ不抢占内核任务。
    int block_error = airi_block_init(info); // 固定单队列设备，未连接时明确停止当前存储阶段。
    if (block_error) { // 默认运行必须带独立实验盘。
        airi_log("[BLOCK_INIT_ERROR] error=%d", block_error); // 先记录具体设备错误。
        airi_panic("virtio block initialization failed"); // 不把缺设备当成功。
    }
    airi_block_demo(); // 真正DMA读写、FLUSH与范围拒绝。
    airi_user_demo(); // 正常与非法用户程序结束后仍回到启动栈和串口。
    airi_console_init(&console, info);  // 初始化内核静态控制台，借用整个启动期间有效的 info。
    console.service = airi_user_command; // 系统程序服务独立于可选主题/窗口扩展。
```

**更新构建规则。** 加入驱动及block-timeout独立故障宏；其他配置不含注入。

将 VirtIO 驱动加入内核源码。

<!-- replace: Makefile -->
<!-- before -->
```make
# 基础目标只编译文本会话与设备；不读取 frontend 的目录或字体。
SOURCES := kernel/main.c kernel/boot/limine.c kernel/boot/info.c \
    kernel/lib/base.c kernel/lib/runtime.c kernel/arch/x86_64/serial.c \
    kernel/console/console.c kernel/arch/x86_64/input.c kernel/arch/x86_64/cpu.c kernel/arch/x86_64/trap.c kernel/mm/pages.c kernel/mm/memory.c kernel/arch/x86_64/paging.c kernel/mm/heap.c kernel/arch/x86_64/timer.c kernel/task/task.c kernel/process/user.c kernel/process/elf.c kernel/fs/file.c
# 只有 desktop- 前缀目标才读取图形支线自己的构建片段。
ifneq ($(filter desktop-%,$(PROFILE)),)
include frontend/sources.mk # 本分支取得图形自己的源码清单。
```
<!-- after -->
```make
# 基础目标只编译文本会话与设备；不读取 frontend 的目录或字体。
SOURCES := kernel/main.c kernel/boot/limine.c kernel/boot/info.c \
    kernel/lib/base.c kernel/lib/runtime.c kernel/arch/x86_64/serial.c \
    kernel/console/console.c kernel/arch/x86_64/input.c kernel/arch/x86_64/cpu.c kernel/arch/x86_64/trap.c kernel/mm/pages.c kernel/mm/memory.c kernel/arch/x86_64/paging.c kernel/mm/heap.c kernel/arch/x86_64/timer.c kernel/task/task.c kernel/process/user.c kernel/process/elf.c kernel/fs/file.c kernel/drivers/virtio_block.c
# 只有 desktop- 前缀目标才读取图形支线自己的构建片段。
ifneq ($(filter desktop-%,$(PROFILE)),)
include frontend/sources.mk # 本分支取得图形自己的源码清单。
```

在独立 block-timeout 配置定义发布遗漏宏，普通配置不继承。

<!-- replace: Makefile -->
<!-- before -->
```make
# 模拟必需内存地图缺失，内核应打印具体PANIC。
CPPFLAGS += -DAIRI_TEST_NO_MEMMAP
endif # 结束 missing-memory 的编译条件
# 栈保护故障镜像让未映射栈触发DF，由IST1报告。
ifeq ($(CASE),stack-guard)
CPPFLAGS += -DAIRI_TEST_STACK_GUARD # 此镜像不能发布READY。
```
<!-- after -->
```make
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
```

**更新QEMU设备与验收。** 每个目录的盘保持独立，正常读写、只读拒绝、缺盘和超时分别核对标记。

在参数返回前准备独立 raw 盘并挂接对应设备；已有盘先验证而不截断。

<!-- replace: scripts/boot.py -->
<!-- before -->
```python
         '-drive',f'if=pflash,format=raw,unit=1,file={variables}',  # 第1个 pflash 单元使用本轮可写变量副本。
         '-monitor','none','-cdrom',str(ROOT/'build'/profile/'airios.iso'),'-boot','d','-no-reboot',  # 挂载对应配置镜像，从光盘启动，故障时不循环重启。
         '-serial',f'file:{work/"serial.log"}']  # 默认 UART 输出保存到本轮 serial.log。
    if not gui: cmd+=['-display','none']  # 无人操作时隐藏显示窗口，guest 仍可绘图并经 QMP 截帧。
    if debug: cmd+=['-S','-gdb',f'tcp:127.0.0.1:{debug_port}']  # -S 暂停CPU，GDB只监听回环地址指定端口。
    return cmd  # 仅返回参数列表，调用者决定何时启动与回收进程。
```
<!-- after -->
```python
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
```

为缺盘设置具体失败预期，并按不同设备配置核对完成、只读拒绝或超时。

<!-- replace: scripts/boot.py -->
<!-- before -->
```python
    case = profile.removeprefix('desktop-')  # 图形前缀不改变故障注入含义。
    graphics = profile.startswith('desktop-') and case != 'no-framebuffer'  # 有绘图实现且设备存在才应生成画面。
    expected='[AIRI_PANIC] required Limine memory map is missing' if case=='missing-memory' else '[AIRI_P3_READY]'  # 正常配置等完整进程阶段，旧阶段标记只保留兼容。
    if case in ('trap-fatal', 'stack-guard'): # 未处理异常镜像必须进入具体停止路径。
        expected = '[AIRI_PANIC] unhandled CPU exception' # 不以 QEMU 存活推断故障已处理。
    started=time.monotonic()  # 单调秒数作为经过时间起点，不受系统时钟调整影响。
```
<!-- after -->
```python
    case = profile.removeprefix('desktop-')  # 图形前缀不改变故障注入含义。
    graphics = profile.startswith('desktop-') and case != 'no-framebuffer'  # 有绘图实现且设备存在才应生成画面。
    expected='[AIRI_PANIC] required Limine memory map is missing' if case=='missing-memory' else '[AIRI_P3_READY]'  # 正常配置等完整进程阶段，旧阶段标记只保留兼容。
    if case == 'block-missing': # 缺盘是明确设备初始化失败，不能继续宣称存储可用。
        expected = '[AIRI_PANIC] virtio block initialization failed' # 错误码另外核对。
    if case in ('trap-fatal', 'stack-guard'): # 未处理异常镜像必须进入具体停止路径。
        expected = '[AIRI_PANIC] unhandled CPU exception' # 不以 QEMU 存活推断故障已处理。
    started=time.monotonic()  # 单调秒数作为经过时间起点，不受系统时钟调整影响。
```

把缺盘纳入禁止 READY 的错误集合。

<!-- replace: scripts/boot.py -->
<!-- before -->
```python
                text=serial.read_text(errors='replace') if serial.exists() else ''  # 日志尚未创建时取空串；暂时不完整的字节以替代字符读取。
                complete_lines = text.splitlines(keepends=True)  # 保留行结束符，完整记录与写到一半的标记可区分。
                if any(line.endswith('\n') and line.startswith(expected) for line in complete_lines):  # 生成器逐行检查，至少一个完整且以预期标记开头才成立。
                    if case in ('missing-memory', 'trap-fatal', 'stack-guard'):  # 失败配置只允许具体拒绝记录，不能同时宣布启动完成。
                        if case == 'stack-guard': # 坏栈必须真的转到双重故障的独立栈。
                            assert '[STACK_GUARD_PROBE]' in text and '[EXCEPTION] vector=8 error=0x0 ' in text # 不能把任意PANIC冒充保护页演示。
                        assert '[AIRI_P1_READY]' not in text  # 错误路径不得出现成功标记。
                    else:  # 按当前profile的另一条完整验证路径处理。
                        assert '[EXEC_FAILURE_OK]' in text and '[EXEC_WORKER_OK]' in text and '[EXEC_COMMIT]' in text # 坏映像保持与成功替换都须实际运行。
                        assert '[FILES_USER_OK]' in text and '[FD_EXEC_OK]' in text # RAM文件和成功exec继承均完成。
                        assert '[PROCESS_FORK_WAIT_OK]' in text and '[PROCESS_DEMO_OK]' in text # 复制、真实阻塞、timer换身份和回收都须完成。
                        assert '[ELF_LOAD]' in text and '[ELF_USER_OK]' in text # 独立ELF段、BSS与用户libc必须真正运行。
```
<!-- after -->
```python
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
                        assert '[FILES_USER_OK]' in text and '[FD_EXEC_OK]' in text # RAM文件和成功exec继承均完成。
                        assert '[PROCESS_FORK_WAIT_OK]' in text and '[PROCESS_DEMO_OK]' in text # 复制、真实阻塞、timer换身份和回收都须完成。
                        assert '[ELF_LOAD]' in text and '[ELF_USER_OK]' in text # 独立ELF段、BSS与用户libc必须真正运行。
```

**更新已发布配置集合。** 默认完整test现在包含九种启动，继续测试真实UART和PS/2。

在 PROFILES 中登记三种设备配置。

<!-- replace: scripts/manage.py -->
<!-- before -->
```python
from boot import check, command  # check 等待启动完成，command 只组装交互QEMU参数。
from console_test import check_console  # 复用真实键盘和UART输入验证。

PROFILES=('normal','no-framebuffer','missing-memory','edit-experiment','trap-fatal','stack-guard')  # 元组固定六种构建配置，CLI choices 与 test 遍历共用。
def run(cmd):  # 所有统一入口外部命令都从同一根目录执行。
    subprocess.run(cmd,cwd=ROOT,check=True)  # 非零状态转成异常传至入口。
def compilation_database():  # 从 Make 干跑输出收集每个C/汇编文件的实际编译参数。
```
<!-- after -->
```python
from boot import check, command  # check 等待启动完成，command 只组装交互QEMU参数。
from console_test import check_console  # 复用真实键盘和UART输入验证。

PROFILES=('normal','no-framebuffer','missing-memory','edit-experiment','trap-fatal','stack-guard','block-readonly','block-timeout','block-missing')  # 元组固定九种构建配置，CLI choices 与 test 遍历共用。
def run(cmd):  # 所有统一入口外部命令都从同一根目录执行。
    subprocess.run(cmd,cwd=ROOT,check=True)  # 非零状态转成异常传至入口。
def compilation_database():  # 从 Make 干跑输出收集每个C/汇编文件的实际编译参数。
```

让命令参数只接受同一 PROFILES 集合。

<!-- replace: scripts/manage.py -->
<!-- before -->
```python
    parser.add_argument('action',choices=['doctor','build','run','test','debug','kernel-debug','linux-lab','clean','ui-preview','ui-test','docs-check','snapshots'])  # 位置参数限定动作集合，拼错动作不能落到默认运行QEMU。
    parser.add_argument('--app',choices=('terminal','files','editor','tasks','system','settings','viewer'),default='editor')  # 主机窗口默认Editor，只接受已知应用名。
    parser.add_argument('--state',choices=['normal','empty','loading','error','offline'],default='normal')  # 状态名限定到共享视图已有的五种状态。
    parser.add_argument('--profile',choices=PROFILES,default='normal')  # 镜像默认普通配置，只接受固定六项。
    parser.add_argument('--phase',choices=('p1','p2','p3'),default='p1') # Linux阶段显式选择，默认保持P1。
    parser.add_argument('--host',default='airi@ssh.airi-dynamic.top')   # 个人Linux主机可以显式替换，不涉及公司机器。
    parser.add_argument('--graphics', action='store_true', help='explicitly enable the optional desktop')  # 默认 False，不读取图形目录。
```
<!-- after -->
```python
    parser.add_argument('action',choices=['doctor','build','run','test','debug','kernel-debug','linux-lab','clean','ui-preview','ui-test','docs-check','snapshots'])  # 位置参数限定动作集合，拼错动作不能落到默认运行QEMU。
    parser.add_argument('--app',choices=('terminal','files','editor','tasks','system','settings','viewer'),default='editor')  # 主机窗口默认Editor，只接受已知应用名。
    parser.add_argument('--state',choices=['normal','empty','loading','error','offline'],default='normal')  # 状态名限定到共享视图已有的五种状态。
    parser.add_argument('--profile',choices=PROFILES,default='normal')  # 镜像默认普通配置，只接受已发布配置。
    parser.add_argument('--phase',choices=('p1','p2','p3'),default='p1') # Linux阶段显式选择，默认保持P1。
    parser.add_argument('--host',default='airi@ssh.airi-dynamic.top')   # 个人Linux主机可以显式替换，不涉及公司机器。
    parser.add_argument('--graphics', action='store_true', help='explicitly enable the optional desktop')  # 默认 False，不读取图形目录。
```

完整 test 也遍历这份集合，保留后续输入检查。

<!-- replace: scripts/manage.py -->
<!-- before -->
```python
        build('normal')  # 调试默认仍使用串口目标，测试宏不进入调试对象。
        run([sys.executable,str(ROOT/'scripts/kernel_debug.py'),'--host',args.host])  # 远端GDB连接本地QEMU。
        return  # 调试记录完成后结束本动作。
    if args.action=='test':  # 完整验证串联主机算法、可选前端、六种启动和三条设备输入。
        run(make_command(target='host-test'))  # 编译同一纯C实现并运行带ASan/UBSan的主机边界检查。
        if args.graphics:  # 普通 test 不导入或验证前端。
            check_frontend(screenshots=False)  # 图形模型检查仍有自己的一组断言。
        results=[]  # 列表依次保存各配置的实际启动结果。
        profiles = [('desktop-' + p) if args.graphics else p for p in PROFILES]  # 呈现方式和故障场景正交。
        for profile in profiles:  # 按固定六种配置独立编译与启动。
            build(profile)  # 生成对应配置的ELF和镜像，产物目录彼此隔离。
            if profile.endswith('normal'):  # 普通目标额外检查ELF结构。
                inspect_elf(profile)  # 保存机器、类型、依赖与符号报告。
            results.append(check(profile))  # 等待该配置完成标记并保存结果，任一失败异常中止。
        (ROOT/'artifacts'/('desktop-boot-tests.json' if args.graphics else 'boot-tests.json')).write_text(json.dumps(results,indent=2)+'\n')  # 六种启动结果编码为JSON数组。
        prefix = 'desktop-' if args.graphics else ''  # 本轮输入验证选择同一呈现方式。
        console_results=[check_console(prefix+'normal'),check_console(prefix+'no-framebuffer'),check_console(prefix+'normal',uart_input=False)]  # 顺序验证normal UART、无帧缓冲UART与normal PS/2三条真实输入路径。
        (ROOT/'artifacts'/('desktop-console-tests.json' if args.graphics else 'console-tests.json')).write_text(json.dumps(console_results,indent=2)+'\n')  # 保存三次实际输入验证结果。
```
<!-- after -->
```python
        build('normal')  # 调试默认仍使用串口目标，测试宏不进入调试对象。
        run([sys.executable,str(ROOT/'scripts/kernel_debug.py'),'--host',args.host])  # 远端GDB连接本地QEMU。
        return  # 调试记录完成后结束本动作。
    if args.action=='test':  # 完整验证串联主机算法、可选前端、九种启动和三条设备输入。
        run(make_command(target='host-test'))  # 编译同一纯C实现并运行带ASan/UBSan的主机边界检查。
        if args.graphics:  # 普通 test 不导入或验证前端。
            check_frontend(screenshots=False)  # 图形模型检查仍有自己的一组断言。
        results=[]  # 列表依次保存各配置的实际启动结果。
        profiles = [('desktop-' + p) if args.graphics else p for p in PROFILES]  # 呈现方式和故障场景正交。
        for profile in profiles:  # 按固定九种配置独立编译与启动。
            build(profile)  # 生成对应配置的ELF和镜像，产物目录彼此隔离。
            if profile.endswith('normal'):  # 普通目标额外检查ELF结构。
                inspect_elf(profile)  # 保存机器、类型、依赖与符号报告。
            results.append(check(profile))  # 等待该配置完成标记并保存结果，任一失败异常中止。
        (ROOT/'artifacts'/('desktop-boot-tests.json' if args.graphics else 'boot-tests.json')).write_text(json.dumps(results,indent=2)+'\n')  # 九种启动结果编码为JSON数组。
        prefix = 'desktop-' if args.graphics else ''  # 本轮输入验证选择同一呈现方式。
        console_results=[check_console(prefix+'normal'),check_console(prefix+'no-framebuffer'),check_console(prefix+'normal',uart_input=False)]  # 顺序验证normal UART、无帧缓冲UART与normal PS/2三条真实输入路径。
        (ROOT/'artifacts'/('desktop-console-tests.json' if args.graphics else 'console-tests.json')).write_text(json.dumps(console_results,indent=2)+'\n')  # 保存三次实际输入验证结果。
```

<a id="block-run-result"></a>
## 看实际设备完成，而不是看宿主文件存在

```sh
./airios test # 九种独立启动、六组主机算法和三条真实输入。
./airios run --profile block-readonly # 读取真实只读设备并拒绝写入。
./airios run --profile block-timeout # 漏发布idx，约两秒后记录隔离，再继续非磁盘命令。
./airios run --profile block-missing # 枚举失败，打印error=-19和具体PANIC。
```

按当前观察条件，正常串口 BLOCK_READY 应显示实际PCI位置、queue、sectors=16384、readonly=0和flush=1。BLOCK_DEMO_OK只在模式写入、读回比较、原扇区恢复、FLUSH和范围拒绝全部完成后出现。三个故障标记不能和正常成功混用，所有自动等待有上界，过早退出也失败。

调试可在available[1]发布前看链0→1→2、request_physical和三个设备方向，再在used[1]变化后看completion.id=0。设备给回的id是描述符链身份，不是用户PID或提交顺序推断。后续多个请求也必须各自关联身份。

写回再读回证明本次设备内容一致，FLUSH 则请求后端同步，这是不同的结果。FLUSH 完成表示虚拟设备后端接受并完成同步请求；它不等同于本轮实际拉掉主机电源，也不自动让上层多扇区更新成为事务。下一节会为追加记录单独定义校验、提交顺序与重启恢复，保留不同故障模型的边界。
