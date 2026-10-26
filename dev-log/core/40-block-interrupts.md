# K40 · 让块设备中断提示完成，仍从 used 环取得结果

从K39继续。块驱动已经能begin提交、poll收集，用户等待时由普通服务栈推进设备，但CPU主要依靠PIT周期醒来重查。现在接通真实VirtIO设备中断：设备发布完成后经PCI与PIC进入IDT43，IRQ只记录提示，普通内核服务再核对used环、复制结果并唤醒用户。

固定设备仍是bus0上的legacy virtio-blk、单队列、单在途，无MSI-X。本节不增加多队列、通用共享IRQ注册、ACPI解释器或IOAPIC。新增屏蔽设备IRQ的故障配置，保留真实DMA和used环，验证即使没有设备中断交付，PIT后备查询仍能收集结果与处理超时。

<a id="block-irq-route"></a>
## PCI 引脚、板级连线、PIC 输入和向量是四个编号

K18已经把主PIC映射到32–39、从PIC映射到40–47。PIT直接使用主片IRQ0，本节设备则从PCI的INTx引脚进入板级路由，再到PIC。PCI配置0x3c的低字节Interrupt Line只记录软件分配的编号，向这里写11不会自动建立真实接线；第二字节Interrupt Pin的1至4表示INTA至INTD，也不是PIC输入号。

当前锁定QEMU q35为普通根槽提供PIRQ E–H接线模型，LPC桥的路由字节再把其中一路接到PIC。这个范围依据[QEMU v11.1.2的ICH9模型](https://github.com/qemu/qemu/blob/v11.1.2/hw/isa/lpc_ich9.c)与[对应寄存器声明](https://github.com/qemu/qemu/blob/v11.1.2/include/hw/southbridge/ich9.h)确定。这里只支持根槽0–24和INTA–D，特殊片上设备、桥后设备和真实主板都要另外确认连线。

本轮设备实际在0:3.0，Pin=1，即INTA。四层关系是：

| 当前对象 | 数值与含义 | 下一步 |
| --- | --- | --- |
| PCI设备引脚 | slot3、INTA编码1 | q35普通槽公式得到PIRQ H，内部编号7 |
| LPC路由字节 | 0x6b配置为11且不禁用 | 来源接到PIC IRQ11 |
| PIC输入 | 从片输入3，经主片IRQ2级联 | 从片基址40加3 |
| CPU入口 | IDT43 | 保存完整现场，进入设备IRQ分派 |

平台适配先确认bus0:31.0确实是预期ICH9 LPC，再修改本设备所接PIRQ的那个路由字节，并读回核对。按本轮slot3、INTA的接线，来源进入从PIC的第3个输入；从片基址40加3，最终才成为CPU收到的向量43。PCI引脚、板级路由、PIC输入和IDT入口由这条连接对应起来，各自仍使用自己的编号。

路由写好以后，还要让来源真正交到CPU。IRQ11在ELCR中设为电平触发，设备将来源电平保持到驱动读取ISR撤销；PIC既要开放从片输入，也要开放主片IRQ2级联。CPU的RFLAGS.IF也必须允许中断。来源到了从片而级联未开放，或CPU仍关闭IF，都还不能进入IDT43。

先新建`include/airi/irq.h`。IRQ11是本节固定平台的唯一PCI设备输入，通用块接口不暴露这些端口。

<!-- write: include/airi/irq.h -->
```c
#ifndef AIRI_IRQ_H // 当前q35的PCI中断接线与PIC确认边界。
#define AIRI_IRQ_H // 包含保护。
#include <stdbool.h> // 路由校验明确成功或拒绝。
#define AIRI_PCI_IRQ 11u // 当前固定平台只给一个VirtIO设备分配IRQ11，向量为43。
bool airi_irq_route_pci(unsigned slot, unsigned pin); // bus0普通根槽、INTA-D，配置q35 PIRQ到IRQ11，不在此开放PIC。
void airi_irq_mask(unsigned irq, bool masked); // 修改本CPU平台PIC屏蔽位，开放从片时同时开放级联IRQ2。
void airi_irq_end(unsigned irq); // 设备先撤销来源，再按从片、主片顺序确认。
#endif // 结束包含保护。
```

新建`kernel/arch/x86_64/irq.c`。它只处理当前q35的接线、PIC屏蔽和EOI，沿用已有的端口与IF保存接口。设置路由时还不开放设备IRQ，块驱动准备完队列与状态后才调用unmask。读取旧掩码后只修改目标位，不能覆盖已经开放的PIT。

EOI顺序也属于这条接线：设备先撤销INTx，再向从PIC确认IRQ11，最后向主PIC确认级联IRQ2。只确认主片会让从片继续认为旧中断在服务；只确认从片则级联仍不能继续交付。

<!-- write: kernel/arch/x86_64/irq.c -->
```c
#include <airi/irq.h> // 固定q35路由留在平台层，块接口不暴露芯片寄存器。
#include <airi/x86_io.h> // 端口访问与原IF保存恢复。
static uint32_t lpc_read(unsigned offset) { // 当前ICH9 LPC固定在bus0:31.0。
    airi_out32(0xcf8, 0x80000000u | 31u << 11 | (offset & 0xfcu)); // 选择芯片PCI配置双字。
    return airi_in32(0xcfc); // 只读取配置，不解释成普通内存地址。
}
bool airi_irq_route_pci(unsigned slot, unsigned pin) { // 针对锁定QEMU q35普通根槽的实际连线，不冒充通用ACPI路由。
    if (slot >= 25 || pin < 1 || pin > 4) // 特殊片上设备与桥后设备的路由公式不同，本阶段拒绝。
        return false; // 不修改其他平台配置。
    uint64_t flags = airi_irq_save(); // PCI选择/数据与ELCR修改在本CPU不可打断。
    if (lpc_read(0) != 0x29188086u) { // 确认是QEMU ICH9 LPC，而非把其他桥寄存器当作同一布局。
        airi_irq_restore(flags); // 拒绝也保持原IF。
        return false; // 真实开发板需按其芯片和ACPI单独接入。
    }
    unsigned pirq = (slot + pin - 1) % 4 + 4; // QEMU普通根槽的INTA-D接到PIRQ E-H，pin从1转成0基数。
    unsigned offset = 0x68 + pirq - 4; // ICH9的PIRQ E-H路由字节位于0x68至0x6b。
    (void)lpc_read(offset); // 选择包含该路由字节的双字，其他字节保持原值。
    airi_out8((uint16_t)(0xcfc + (offset & 3)), AIRI_PCI_IRQ); // 清禁用位并只将本来源接到IRQ11。
    bool routed = ((lpc_read(offset) >> ((offset & 3) * 8)) & 0x8fu) == AIRI_PCI_IRQ; // 读回路由和禁用位，不能只假定写入生效。
    if (routed) // 只有已确认的PCI电平来源才修改ELCR。
        airi_out8(0x4d1, airi_in8(0x4d1) | (1u << (AIRI_PCI_IRQ - 8))); // IRQ11改为电平触发，其他IRQ类型保持。
    airi_irq_restore(flags); // PIC仍屏蔽此设备，调用者准备完成后才开放。
    return routed; // 块驱动决定路由失败是否拒绝启动。
}
void airi_irq_mask(unsigned irq, bool masked) { // 当前驱动只传0至15的合法PIC输入。
    if (irq >= 16) // 不将未知编号截断成另一路设备。
        return; // 无状态改变。
    uint64_t flags = airi_irq_save(); // 屏蔽位的读改写不能丢掉另一调用的位。
    uint16_t port = irq < 8 ? 0x21 : 0xa1; // IRQ8-15属于从PIC。
    uint8_t bit = (uint8_t)(1u << (irq & 7)); // 片内输入编号。
    uint8_t mask = airi_in8(port); // 保留时钟和其他设备当前屏蔽策略。
    airi_out8(port, masked ? mask | bit : mask & (uint8_t)~bit); // 只改变目标输入。
    if (!masked && irq >= 8) // 从PIC的中断必须还能通过主PIC IRQ2。
        airi_out8(0x21, airi_in8(0x21) & (uint8_t)~4u); // 级联开放不改变PIT IRQ0。
    airi_irq_restore(flags); // 系统调用原IF0不能被无条件打开。
}
void airi_irq_end(unsigned irq) { // 调用时设备ISR已读取，来源电平先撤销。
    if (irq >= 8) // 从片发来的IRQ先结束从片服务状态。
        airi_out8(0xa0, 0x20); // 非特定EOI，当前没有嵌套的从片处理。
    airi_out8(0x20, 0x20); // 最后结束主片的目标或级联服务状态。
}
```

<a id="block-irq-hint"></a>
## 中断提示没有请求身份，used 环才有

legacy设备的ISR位0表示队列通知，位1表示配置变化；读取ISR会清除状态并使设备撤销INTx。该接口与读取后撤销的约定见[VirtIO 1.3 PCI ISR说明](https://docs.oasis-open.org/virtio/virtio/v1.3/virtio-v1.3.html)。驱动仍沿当前固定legacy偏移19读取，未启MSI-X。

一次通知可以晚于普通poll，也可能与多个完成合并，不能用interrupt_count作为请求编号。真正收集仍要先观察used.idx，再以原有屏障读取链首id、长度和status。只有这些检查通过，才复制512字节读数据、撤销busy并允许下一个请求覆盖静态DMA。

IRQ函数先读取ISR撤销设备电平，有队列通知时增加观察计数；遇到不支持的配置变化，也只留下标记。这条中断调用链不复制用户数据、不分配、不打印，也不reset设备。配置变化由下一次普通begin/poll消费，届时停止设备并隔离DMA；本轮没有在线扩容路径。

正常配置让IRQ消费ISR，普通poll不再提前读取它，以免CPU尚未进入设备中断就清掉来源。屏蔽IRQ的故障配置没有这条交付路径，改由后备poll清通知。两种配置最终都从used环核对同一份完成身份，ISR由谁读取不改变数据收集条件。

更新`include/airi/block.h`，新增IRQ处理与只读计数入口。begin的0仍只表示接收，poll的1仍表示在途，两者没有因加入中断而改变。

<!-- write: include/airi/block.h -->
```c
#ifndef AIRI_BLOCK_H // 通用块接口不暴露PCI端口和VirtIO布局。
#define AIRI_BLOCK_H // 包含保护。
#include <airi/boot.h> // 当前启动提供内核连续物理映像位置。
int airi_block_init(const AiriBootInfo *info); // 固定单队列，失败返回负错误。
bool airi_block_interrupt(void); // IRQ只读清ISR并记录提示，不收集DMA结果。
uint64_t airi_block_interrupts(void); // 实际设备IRQ计数，不等于完成条数。
bool airi_block_readonly(void); // 给调用者明确只读事实，不能靠写失败猜测。
uint64_t airi_block_sectors(void); // 以512字节扇区计，不等同设备逻辑块大小配置。
int airi_block_read(uint64_t sector, void *buffer); // 一次精确一个512字节扇区，0成功。
int airi_block_write(uint64_t sector, const void *buffer); // 同步复制并等待设备完成。
int airi_block_flush(void); // 请求后端同步，不宣称真实硬件掉电已实测。
int airi_block_begin(unsigned operation, uint64_t sector, const void *write_data); // 0读/1写/4同步；0表示已接收，不表示完成。
int airi_block_poll(void *read_data); // 1仍在途、0完成、负错误；读输出可NULL表示丢弃，静态 DMA 区继续保留，收集完成前不得复用。
bool airi_block_pending(void); // 已接收但未收集完成，含设备已完成却未poll的状态。
void airi_block_demo(void); // 只操作项目独立实验盘末扇区。
#endif // 结束保护。
```

更新`kernel/drivers/virtio_block.c`。初始化时清PCI命令的INTx Disable位，avail.flags不再设置NO_INTERRUPT，并读取设备Pin、配置平台路由。队列和设备准备好后才开放PIC。PCI命令改用16位写入，避免把邻接的W1C状态字段一起写回。

队列通知的interrupt_count只在真实设备IRQ函数内增加，未收到该向量时数值必须保持零。原stop路径另外屏蔽本驱动IRQ，静态DMA仍永久保留，不因屏蔽或reset就归还。同步perform的hlt现在可被设备IRQ或PIT唤醒；两秒截止仍由首次begin建立，不随重试延长。

接入平台 IRQ 接线接口，块接口仍不暴露端口。

本章每组替换的第一块只用于定位旧文本，敲入第二块替换它，不把两块同时加入文件。各组按正文顺序完成。

<!-- replace: kernel/drivers/virtio_block.c -->
<!-- before -->
```c
#include <airi/block.h> // 上层按扇区请求，不知道PCI和环结构。
#include <airi/kernel.h> // 串口记录与内核不变量停止。
#include <airi/timer.h> // 超时依据真实PIT tick。
```
<!-- after -->
```c
#include <airi/irq.h> // q35路由、PIC屏蔽和确认由平台层提供。
#include <airi/block.h> // 上层按扇区请求，不知道PCI和环结构。
#include <airi/kernel.h> // 串口记录与内核不变量停止。
#include <airi/timer.h> // 超时依据真实PIT tick。
```

在静态队列状态旁保存队列通知计数、配置变化和路由是否完成。

<!-- replace: kernel/drivers/virtio_block.c -->
<!-- before -->
```c
static uint64_t request_physical, capacity; // 设备使用guest物理地址，不理解内核高VA。
static uint32_t active_type; // 当前在途操作只属于成功begin的调用者。
static uint64_t request_deadline; // 截止跨begin/poll调用保存，不能每次poll重置。
static bool readonly, can_flush, ready, busy; // 固定队列状态，失败后禁止再次覆盖DMA。
static uint32_t pci_read(unsigned slot, unsigned function, unsigned offset) { // 仅固定bus0上的q35设备。
    airi_out32(0xcf8, 0x80000000u | slot << 11 | function << 8 | (offset & 0xfcu)); // 选择PCI配置地址。
```
<!-- after -->
```c
static uint64_t request_physical, capacity; // 设备使用guest物理地址，不理解内核高VA。
static uint32_t active_type; // 当前在途操作只属于成功begin的调用者。
static uint64_t request_deadline; // 截止跨begin/poll调用保存，不能每次poll重置。
static volatile uint64_t interrupt_count; // 只有真实IRQ处理增加；不作为完成身份或完成条数。
static volatile bool config_changed; // IRQ仅记录配置变化，普通上下文再停止不支持的动态变更。
static bool irq_routed; // 成功配置后才能开放或屏蔽该固定设备输入。
static bool readonly, can_flush, ready, busy; // 固定队列状态，失败后禁止再次覆盖DMA。
static uint32_t pci_read(unsigned slot, unsigned function, unsigned offset) { // 仅固定bus0上的q35设备。
    airi_out32(0xcf8, 0x80000000u | slot << 11 | function << 8 | (offset & 0xfcu)); // 选择PCI配置地址。
```

stop 在路由已建立时屏蔽本设备输入，仍永久保留静态DMA。

<!-- replace: kernel/drivers/virtio_block.c -->
<!-- before -->
```c
}
static int stop(int error) { // 超时和坏完成后先撤销设备运行资格。
    airi_out8(io + 18, 0); // reset设备；固定DMA区仍永久保留，不因reset结果复用。
    ready = false; // 后续请求返回EIO，避免晚到DMA打到新请求。
    busy = false; // 不再让上层等待旧状态，但不重建队列。
    airi_log("[BLOCK_STOP] error=%d reset_status=%u dma=quarantined", error, (unsigned)airi_in8(io + 18)); // 是否已确认reset作为真实记录。
```
<!-- after -->
```c
}
static int stop(int error) { // 超时和坏完成后先撤销设备运行资格。
    airi_out8(io + 18, 0); // reset设备；固定DMA区仍永久保留，不因reset结果复用。
    if (irq_routed) // 固定平台此IRQ只交本驱动管理，隔离后也停止交付。
        airi_irq_mask(AIRI_PCI_IRQ, true); // 静态DMA仍保持，不能因屏蔽IRQ就归还。
    ready = false; // 后续请求返回EIO，避免晚到DMA打到新请求。
    busy = false; // 不再让上层等待旧状态，但不重建队列。
    airi_log("[BLOCK_STOP] error=%d reset_status=%u dma=quarantined", error, (unsigned)airi_in8(io + 18)); // 是否已确认reset作为真实记录。
```

初始化只写PCI命令低16位，同时清除INTx Disable，不碰邻接W1C状态。

<!-- replace: kernel/drivers/virtio_block.c -->
<!-- before -->
```c
        return -95; // 本节不实现现代PCI capability transport。
    io = (uint16_t)(bar & ~3u); // 低位是类型，不属于端口地址。
    uint32_t command = pci_read(found_slot, found_function, 4); // PCI命令需要启用I/O与总线主控DMA。
    airi_out32(0xcfc, (command & 0xffffu) | 5u); // 地址仍指向command，状态高16位写0以免误清W1C位。
    airi_out8(io + 18, 0); // 先reset，再发布本次驱动状态。
    if (airi_in8(io + 18)) // 不确认停止就不能重用静态队列。
        return -5; // -EIO。
```
<!-- after -->
```c
        return -95; // 本节不实现现代PCI capability transport。
    io = (uint16_t)(bar & ~3u); // 低位是类型，不属于端口地址。
    uint32_t command = pci_read(found_slot, found_function, 4); // PCI命令需要启用I/O与总线主控DMA。
    airi_out16(0xcfc, (uint16_t)((command & 0xffffu & ~(1u << 10)) | 5u)); // 只写命令低16位，启I/O/DMA并清INTx Disable，不触碰W1C状态。
    airi_out8(io + 18, 0); // 先reset，再发布本次驱动状态。
    if (airi_in8(io + 18)) // 不确认停止就不能重用静态队列。
        return -5; // -EIO。
```

avail.flags允许设备通知；完成身份仍从used取得。

<!-- replace: kernel/drivers/virtio_block.c -->
<!-- before -->
```c
    available = (volatile uint16_t *)(queue + 16u * queue_size); // flags、idx、ring依次为16位字段。
    used = (volatile uint16_t *)(queue + used_offset); // used部分另起4096边界。
    used_ring = (volatile struct used_element *)(queue + used_offset + 4); // flags和idx之后是元素数组。
    available[0] = 1; // NO_INTERRUPT：首版由驱动轮询完成，不启设备IRQ。
    uint64_t queue_physical = physical(info, queue, sizeof(queue)); // 设备需要连续guest PA。
    request_physical = physical(info, &request, sizeof(request)); // 请求对象也是静态内核物理存储。
    if (queue_physical & 4095 || queue_physical >> 44) // PFN寄存器32位、页对齐。
```
<!-- after -->
```c
    available = (volatile uint16_t *)(queue + 16u * queue_size); // flags、idx、ring依次为16位字段。
    used = (volatile uint16_t *)(queue + used_offset); // used部分另起4096边界。
    used_ring = (volatile struct used_element *)(queue + used_offset + 4); // flags和idx之后是元素数组。
    available[0] = 0; // 不抑制used通知；中断只是提示，完成仍消费used环。
    uint64_t queue_physical = physical(info, queue, sizeof(queue)); // 设备需要连续guest PA。
    request_physical = physical(info, &request, sizeof(request)); // 请求对象也是静态内核物理存储。
    if (queue_physical & 4095 || queue_physical >> 44) // PFN寄存器32位、页对齐。
```

队列准备后确认Pin与真实路由、开放交付，再提供只读ISR提示与计数接口。

<!-- replace: kernel/drivers/virtio_block.c -->
<!-- before -->
```c
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
```
<!-- after -->
```c
        return stop(-5); // 停止设备且不重用静态DMA。
    if (capacity < 16) // 实验留足独立扇区，拒绝过小盘。
        return stop(-22); // 不在空设备上减1。
    uint32_t interrupt = pci_read(found_slot, found_function, 0x3c); // Interrupt Pin在第二字节，低字节Line仅作软件记录。
    unsigned pin = (interrupt >> 8) & 0xffu; // PCI INTA-D编码为1至4，不是PIC输入编号。
    airi_out8(0xcfc, AIRI_PCI_IRQ); // 当前选择仍指向此设备0x3c，只记录固定分配的IRQ11。
    if (!airi_irq_route_pci(found_slot, pin)) // q35 PIRQ、ELCR与芯片身份必须明确支持。
        return stop(-95); // 不默默假定固件已把INTx接到正确PIC输入。
    irq_routed = true; // 从此stop可关闭该输入，但不复用DMA。
    interrupt_count = 0; // 计数只用于观察实际IRQ，不给请求分配身份。
    config_changed = false; // 首次容量已由前面的稳定读取确定。
    (void)airi_in8(io + 19); // 清初始化前残留状态，设备尚未接收本驱动请求。
    airi_out8(io + 18, 7); // legacy不使用现代FEATURES_OK，ACK|DRIVER|DRIVER_OK。
    ready = true; // 队列和配置完整后才接收请求。
#ifdef AIRI_TEST_BLOCK_IRQ_MASKED // 实际used环继续工作，只屏蔽PIC交付。
    airi_irq_mask(AIRI_PCI_IRQ, true); // 让后备PIT路径收集完成，计数应保持零。
#else // 普通与服务目标均开放真实设备IRQ。
    airi_irq_mask(AIRI_PCI_IRQ, false); // 同时开放从PIC和主片级联。
#endif // 不改变设备返回值与DMA生命周期。
    airi_log("[BLOCK_IRQ_READY] irq=%u vector=%u transport=legacy-intx", AIRI_PCI_IRQ, 32u + AIRI_PCI_IRQ); // 路由确认后才报告可交付。
    airi_log("[BLOCK_READY] pci=0:%u.%u io=0x%x queue=%u sectors=%llu readonly=%u flush=%u", found_slot, found_function, io, queue_size, (unsigned long long)capacity, readonly, can_flush); // 数据来自真实设备配置。
    return 0; // 上层不接触端口或描述符。
}
bool airi_block_interrupt(void) { // IRQ上下文只读ISR、记录提示；不复制数据、不分配、不打印。
    if (!io || !irq_routed) // 未配置设备时不能随意读未知端口。
        return false; // 调用者仍确认PIC，当前平台不提供通用共享IRQ分派。
    uint8_t status = airi_in8(io + 19); // 读取使设备撤销INTx电平，随后才能给PIC EOI。
    if (status & 1u) // 队列提示可能合并多个完成，也可能迟于普通poll。
        ++interrupt_count; // 不把中断次数当作请求编号。
    if (status & 2u) // 当前不支持热改容量或配置，留给普通上下文处理。
        config_changed = true; // 不在IRQ里reset、日志或改变DMA所有权。
    return (status & 3u) != 0; // 只有本设备声明来源时才请求用户服务交接。
}
uint64_t airi_block_interrupts(void) { // x86-64单核对齐64位读取，实际计数不含PIT。
    return interrupt_count; // 观察接口不暴露队列对象。
}
bool airi_block_readonly(void) { // 单设备配置启动后保持稳定。
    return readonly; // 不泄露驱动内部队列指针。
```

begin 在接收新请求前消费配置变化，普通上下文停止不支持的设备变更。

<!-- replace: kernel/drivers/virtio_block.c -->
<!-- before -->
```c
        return -22; // -EINVAL。
    if (!ready) // 之前超时或未初始化。
        return -5; // -EIO，不覆盖可能仍被设备使用的旧DMA。
    if (busy) // 不支持重入和多个调用者覆盖同一静态请求。
        return -16; // -EBUSY。
    if (type != 4 && sector >= capacity) // 在设备读取任何描述符前拒绝越界扇区。
```
<!-- after -->
```c
        return -22; // -EINVAL。
    if (!ready) // 之前超时或未初始化。
        return -5; // -EIO，不覆盖可能仍被设备使用的旧DMA。
    if (config_changed) // 不支持在线容量变更，停止比沿用旧容量更明确。
        return stop(-95); // 普通上下文隔离，静态DMA保持。
    if (busy) // 不支持重入和多个调用者覆盖同一静态请求。
        return -16; // -EBUSY。
    if (type != 4 && sector >= capacity) // 在设备读取任何描述符前拒绝越界扇区。
```

poll 收集前也检查变化，隔离以后恢复调用者原IF。

<!-- replace: kernel/drivers/virtio_block.c -->
<!-- before -->
```c
    if (!busy) // 没有本驱动接收的请求可收集。
        return -22; // -EINVAL，不能读上次残留status当新完成。
    uint64_t flags = airi_irq_save(); // 完成收集和busy撤销作为同一次内核操作。
    if (used[1] == used_index) { // 设备尚未发布新完成。
        int result = airi_timer_ticks() >= request_deadline ? stop(-110) : 1; // 真超时才reset并隔离，1明确仍在途。
        airi_irq_restore(flags); // 无完成也不改变原IF。
```
<!-- after -->
```c
    if (!busy) // 没有本驱动接收的请求可收集。
        return -22; // -EINVAL，不能读上次残留status当新完成。
    uint64_t flags = airi_irq_save(); // 完成收集和busy撤销作为同一次内核操作。
    if (config_changed) { // ISR只登记，当前普通上下文才撤销设备资格。
        int result = stop(-95); // 不把容量变化后的不确定请求当普通成功。
        airi_irq_restore(flags); // 保持调用者原IF。
        return result; // 固定设备本轮没有热改操作，失败策略保留明确。
    }
    if (used[1] == used_index) { // 设备尚未发布新完成。
        int result = airi_timer_ticks() >= request_deadline ? stop(-110) : 1; // 真超时才reset并隔离，1明确仍在途。
        airi_irq_restore(flags); // 无完成也不改变原IF。
```

正常poll不抢先读取ISR；只有屏蔽配置从后备收集处清通知。

<!-- replace: kernel/drivers/virtio_block.c -->
<!-- before -->
```c
    int result = request.status == 0 ? 0 : request.status == 2 ? -95 : -5; // 设备IOERR与UNSUPP都明确交回。
    if (!result && type == 0 && buffer) // 只有成功读才能把设备数据交给上层。
        airi_memcpy(buffer, request.data, 512); // 复制结束后设备DMA缓冲才可供下次覆盖。
    (void)airi_in8(io + 19); // 清可能遗留的legacy ISR状态，完成仍以used环判断。
    busy = false; // 已确认完成，可以重用同一静态请求。
    airi_irq_restore(flags); // 系统调用进入时IF0，普通内核演示进入时IF1。
    return result; // 收集完成后才能重用静态DMA，0不是应用事务持久化。
```
<!-- after -->
```c
    int result = request.status == 0 ? 0 : request.status == 2 ? -95 : -5; // 设备IOERR与UNSUPP都明确交回。
    if (!result && type == 0 && buffer) // 只有成功读才能把设备数据交给上层。
        airi_memcpy(buffer, request.data, 512); // 复制结束后设备DMA缓冲才可供下次覆盖。
#ifdef AIRI_TEST_BLOCK_IRQ_MASKED // 没有IRQ处理者时由后备收集清除设备来源。
    (void)airi_in8(io + 19); // 只清通知；used.idx与状态已独立核对。
#endif // 正常模式只在IRQ读取ISR，不让poll抢先吃掉通知。
    busy = false; // 已确认完成，可以重用同一静态请求。
    airi_irq_restore(flags); // 系统调用进入时IF0，普通内核演示进入时IF1。
    return result; // 收集完成后才能重用静态DMA，0不是应用事务持久化。
```

同步perform的hlt同时允许设备IRQ与PIT唤醒，原截止保持。

<!-- replace: kernel/drivers/virtio_block.c -->
<!-- before -->
```c
    int result = airi_block_begin(type, sector, buffer); // busy或参数错误不会偷取别人的完成。
    if (!result) // 只有本次提交成功才可以收集。
        while ((result = airi_block_poll(buffer)) == 1) // 1是驱动待完成状态，不是成功字节数。
            __asm__ volatile("sti; hlt; cli" ::: "memory"); // 用PIT唤醒后重查，截止由首次begin固定。
    airi_irq_restore(flags); // 系统调用IF0与普通内核IF1都按原样恢复。
    return result; // 0或负错误，旧read/write/flush契约不变。
}
```
<!-- after -->
```c
    int result = airi_block_begin(type, sector, buffer); // busy或参数错误不会偷取别人的完成。
    if (!result) // 只有本次提交成功才可以收集。
        while ((result = airi_block_poll(buffer)) == 1) // 1是驱动待完成状态，不是成功字节数。
            __asm__ volatile("sti; hlt; cli" ::: "memory"); // 设备IRQ或PIT都可唤醒；截止仍由首次begin固定。
    airi_irq_restore(flags); // 系统调用IF0与普通内核IF1都按原样恢复。
    return result; // 0或负错误，旧read/write/flush契约不变。
}
```

第一条真实读后分别检查实际IRQ计数或屏蔽后备，used完成不能代替IRQ已经接通。

<!-- replace: kernel/drivers/virtio_block.c -->
<!-- before -->
```c
#endif // 正常路径还需验证真正读写。
    if (error) // 初始读取必须完成。
        airi_panic("cannot read block demo sector"); // 不用未初始化before覆盖磁盘。
    if (airi_block_begin(0, sector, NULL) || !airi_block_pending() || airi_block_begin(0, sector, NULL) != -16) // 接收后即使设备已快完成，也不可覆盖未收集请求。
        airi_panic("nonblocking block submission ownership failed"); // 不把第二次提交覆盖旧DMA。
    int polled; // 首次poll可能已完成，不能伪造必须观测到pending。
```
<!-- after -->
```c
#endif // 正常路径还需验证真正读写。
    if (error) // 初始读取必须完成。
        airi_panic("cannot read block demo sector"); // 不用未初始化before覆盖磁盘。
#ifdef AIRI_TEST_BLOCK_IRQ_MASKED // 第一条真实读已完成，PIC未交付任何设备中断。
    if (airi_block_interrupts()) // 不能用实际收到IRQ的配置冒充屏蔽后备路径。
        airi_panic("masked block IRQ unexpectedly delivered"); // 明确拒绝错误测试环境。
    airi_log("[BLOCK_IRQ_FALLBACK_OK] interrupts=0 used=collected timer=active"); // 数据来自真实used环，未伪造完成。
#else // 给已完成请求的延迟通知一个有界交付窗口。
    uint64_t irq_deadline = airi_timer_ticks() + 20; // 最多等二十个实际PIT tick，不关闭唯一超时时钟。
    while (!airi_block_interrupts() && airi_timer_ticks() < irq_deadline) // 完成通知可迟于首个poll，仍须实际进入IRQ43。
        __asm__ volatile("sti; hlt" ::: "memory"); // 等设备或PIT事件，不在用户地址上复制。
    if (!airi_block_interrupts()) // used可用也不能冒充IRQ已经接通。
        airi_panic("PCI block IRQ did not reach its vector"); // 原始计数只由IRQ处理者增加。
    airi_log("[BLOCK_IRQ_OK] vector=43 interrupts=%llu used=collected", (unsigned long long)airi_block_interrupts()); // 完成身份仍由前面的used检查保证。
#endif // 后续重叠请求、只读和真实写回归保持。
    if (airi_block_begin(0, sector, NULL) || !airi_block_pending() || airi_block_begin(0, sector, NULL) != -16) // 接收后即使设备已快完成，也不可覆盖未收集请求。
        airi_panic("nonblocking block submission ownership failed"); // 不把第二次提交覆盖旧DMA。
    int polled; // 首次poll可能已完成，不能伪造必须观测到pending。
```

驱动的第一条真实读完成后，normal最多再等二十个PIT tick，要求IRQ43的计数实际增加，再发布BLOCK_IRQ_OK。这个观察没有关闭PIT，接线错误时仍有有界的失败窗口；used已经可读不能冒充中断接通。屏蔽故障则反向要求计数为零，第一条读仍须由used收集后才发布后备标记。

<a id="block-irq-context"></a>
## IRQ 只交接现场，普通栈才交付结果

在`kernel/arch/x86_64/traps.S`的TRAP32之后加入TRAP43。设备IRQ没有硬件错误码，沿公共入口补零、保存完整GPR，再由C返回所选现场。下面用相邻入口定位，原32个CPU异常与软件调用入口保持。

在TRAP32与软件调用入口之间生成TRAP43，仍复用同一份完整保存布局。

<!-- replace: kernel/arch/x86_64/traps.S -->
<!-- before -->
```asm
TRAP 30, 1 /* 安全异常。 */
TRAP 31 /* 保留向量。 */
TRAP 32 /* 主PIC IRQ0对应的时钟入口，无硬件错误码。 */
TRAP 128 /* DPL3的AiriOS系统调用门。 */
TRAP 129 /* DPL0启动/返回用户执行的内核门。 */
TRAP 255 /* 未使用的 IDT 项统一停止；可屏蔽中断仍关闭。 */
```
<!-- after -->
```asm
TRAP 30, 1 /* 安全异常。 */
TRAP 31 /* 保留向量。 */
TRAP 32 /* 主PIC IRQ0对应的时钟入口，无硬件错误码。 */
TRAP 43 /* 从PIC IRQ11，当前固定PCI块设备INTx。 */
TRAP 128 /* DPL3的AiriOS系统调用门。 */
TRAP 129 /* DPL0启动/返回用户执行的内核门。 */
TRAP 255 /* 未使用的 IDT 项统一停止；可屏蔽中断仍关闭。 */
```

在trap.c为IDT43安装独立入口，并在通用CS3故障判断之前处理它。否则设备打断用户时会被误判为用户异常，得到−171退出，而不是继续等待I/O。

分派先让设备读清ISR，再确认从片和主片。打断CS0时直接恢复原现场，保持内核不可抢占；若真正打断CS3且来源属于该设备，则交给用户调度边界决定是否暂回普通服务栈。

包含平台确认与块设备提示接口。

<!-- replace: kernel/arch/x86_64/trap.c -->
<!-- before -->
```c
#include <airi/user.h> // 用户陷入可选择另一条完整返回现场。
#include <airi/timer.h> // 唯一开放的设备IRQ交给架构时钟驱动。
#include <airi/trap.h> // 异常现场的 C 布局。
```
<!-- after -->
```c
#include <airi/irq.h> // 设备来源先撤销，再确认PIC级联。
#include <airi/block.h> // 当前固定设备IRQ只做通知，不在中断里收集DMA。
#include <airi/user.h> // 用户陷入可选择另一条完整返回现场。
#include <airi/timer.h> // IRQ0 的计数与确认交给架构时钟驱动。
#include <airi/trap.h> // 异常现场的 C 布局。
```

声明43号汇编入口。

<!-- replace: kernel/arch/x86_64/trap.c -->
<!-- before -->
```c
static uint8_t double_fault_stack[16384] __attribute__((aligned(16))); // 原栈损坏时 DF 换这块栈。
extern const uintptr_t airi_trap_entries[32]; // 汇编产生的异常入口地址表。
extern void airi_trap_128(void), airi_trap_129(void); // 系统调用与内核启动用户上下文的门。
extern void airi_trap_32(void); // IRQ0也复用完整通用寄存器保存入口。
extern void airi_trap_255(void); // 未启用的向量转到统一停止路径。
extern void airi_tables_load(const void *, const void *); // 汇编依次加载 GDT/TSS/IDT。
```
<!-- after -->
```c
static uint8_t double_fault_stack[16384] __attribute__((aligned(16))); // 原栈损坏时 DF 换这块栈。
extern const uintptr_t airi_trap_entries[32]; // 汇编产生的异常入口地址表。
extern void airi_trap_128(void), airi_trap_129(void); // 系统调用与内核启动用户上下文的门。
extern void airi_trap_43(void); // 当前q35设备IRQ11的独立向量入口。
extern void airi_trap_32(void); // IRQ0也复用完整通用寄存器保存入口。
extern void airi_trap_255(void); // 未启用的向量转到统一停止路径。
extern void airi_tables_load(const void *, const void *); // 汇编依次加载 GDT/TSS/IDT。
```

IDT初始化把43绑定到该独立入口。

<!-- replace: kernel/arch/x86_64/trap.c -->
<!-- before -->
```c
        uintptr_t address = i < 32 ? airi_trap_entries[i] : (uintptr_t)airi_trap_255; // 未使用向量也有明确停止入口。
        if (i == 32) // 唯一开放的设备向量有自己的编号，不能冒充未知255。
            address = (uintptr_t)airi_trap_32; // 指向TRAP32生成的入口。
        if (i == 128 || i == 129) // 两个软件中断各有固定身份。
            address = i == 128 ? (uintptr_t)airi_trap_128 : (uintptr_t)airi_trap_129; // 不能让它们落入未知向量255。
        idt[i] = (struct idt_gate){(uint16_t)address, 8, i == 8 ? 1 : 0, i == 128 ? 0xee : 0x8e, (uint16_t)(address >> 16), (uint32_t)(address >> 32), 0}; // int80门DPL3，其余DPL0；仅DF使用IST1。
```
<!-- after -->
```c
        uintptr_t address = i < 32 ? airi_trap_entries[i] : (uintptr_t)airi_trap_255; // 未使用向量也有明确停止入口。
        if (i == 32) // 时钟向量有自己的编号，不能冒充未知 255。
            address = (uintptr_t)airi_trap_32; // 指向TRAP32生成的入口。
        if (i == 32u + AIRI_PCI_IRQ) // 唯一登记的PCI设备有自己的43号现场。
            address = (uintptr_t)airi_trap_43; // 与TRAP宏压入的编号一致。
        if (i == 128 || i == 129) // 两个软件中断各有固定身份。
            address = i == 128 ? (uintptr_t)airi_trap_128 : (uintptr_t)airi_trap_129; // 不能让它们落入未知向量255。
        idt[i] = (struct idt_gate){(uint16_t)address, 8, i == 8 ? 1 : 0, i == 128 ? 0xee : 0x8e, (uint16_t)(address >> 16), (uint32_t)(address >> 32), 0}; // int80门DPL3，其余DPL0；仅DF使用IST1。
```

在通用CS3故障判断以前先读清设备ISR、确认两片PIC，再决定是否交用户服务边界。

<!-- replace: kernel/arch/x86_64/trap.c -->
<!-- before -->
```c
    airi_log("[TRAP_READY] vectors=32 cs=8 double_fault_ist=1 interrupts=off"); // 该标记只表示异常设施，不宣称调度或 IRQ 完成。
}
struct airi_trap_frame *airi_trap_dispatch(struct airi_trap_frame *f) { // 汇编传入的指针指向当前异常栈。
    if (f->vector == 128 || f->vector == 129 || ((f->cs & 3) == 3 && f->vector != 32)) // 用户系统调用和用户故障由用户上下文所有者处理。
        return airi_user_trap(f); // 返回哪条完整现场由明确边界决定。
    if (f->vector == 32) { // PIC重映射后的时钟，不能作为CPU故障停止。
```
<!-- after -->
```c
    airi_log("[TRAP_READY] vectors=32 cs=8 double_fault_ist=1 interrupts=off"); // 该标记只表示异常设施，不宣称调度或 IRQ 完成。
}
struct airi_trap_frame *airi_trap_dispatch(struct airi_trap_frame *f) { // 汇编传入的指针指向当前异常栈。
    if (f->vector == 32u + AIRI_PCI_IRQ) { // 必须先于通用CS3故障判断，设备IRQ不能杀死用户进程。
        bool notified = airi_block_interrupt(); // 读取设备ISR使INTx先撤销，仅保存提示。
        airi_irq_end(AIRI_PCI_IRQ); // 再按从PIC到主PIC确认，避免电平来源持续触发。
        return notified && (f->cs & 3) == 3 ? airi_user_device_irq(f) : f; // 内核不抢占；用户可在完整现场交普通服务栈。
    }
    if (f->vector == 128 || f->vector == 129 || ((f->cs & 3) == 3 && f->vector != 32)) // 用户系统调用和用户故障由用户上下文所有者处理。
        return airi_user_trap(f); // 返回哪条完整现场由明确边界决定。
    if (f->vector == 32) { // PIC重映射后的时钟，不能作为CPU故障停止。
```

在user.h声明airi_user_device_irq，并在user.c的timer入口前加入实现。若设备通知打断了用户且还有异步工作，先把这次完整frame记在当前进程，设回RUNNABLE；被打断的计算因此仍可从原指令继续，不因回服务栈而丢掉运行资格。函数随后借已有pause_for_service返回长期启动栈。

到launch沿普通栈继续以后，才pump设备、形成CQ、检查各进程等待条件，并按目标页表复制用户结果。这些操作可以使用普通内核调用链；设备IRQ只交出提示和完整继续点，不在中断链上完成结果交付。

没有异步工作时，通知可能属于已经收集的同步请求，直接沿统一用户返回检查继续。PIT仍负责时间片、sleep和超时，并作为遗漏或被屏蔽通知的后备事件源。本节没有去掉周期重查，也不保证每次通知必定对应一次用户身份切换。

<!-- write: include/airi/user.h -->
```c
#ifndef AIRI_USER_H // 用户态边界声明一次。
#define AIRI_USER_H // 包含保护。
#include <stdbool.h> // 系统命令回调明确报告是否处理。
#include <airi/trap.h> // 系统调用和用户故障使用完整CPU现场。
void airi_user_init(void); // 初始化用户文件资源，教学演示与服务启动共用。
int airi_user_boot(const char *name); // 内核启动固定目录中的根用户，返回仅表示该持续会话已经停止。
void airi_user_demo(void); // 运行受控隔离演示与独立C用户ELF，结束后回收。
struct airi_trap_frame *airi_user_trap(struct airi_trap_frame *frame); // 允许返回另一条完整中断调用链。
struct airi_trap_frame *airi_user_device_irq(struct airi_trap_frame *frame); // 真实设备提示可把用户现场交给普通服务栈，不在IRQ复制结果。
struct airi_trap_frame *airi_user_timer(struct airi_trap_frame *frame); // 用户时间片在完整现场边界轮转，内核仍不可抢占。
struct airi_trap_frame *airi_user_resume(struct airi_trap_frame *frame); // 检查用户返回地址并清理不能交给iretq的标志。
struct airi_console; // 只借用文本会话，不依赖图形状态。
bool airi_user_command(struct airi_console *console, const char *command); // programs与run命令使用实际用户服务。
#endif // 结束包含保护。
```

在统一返回与 timer 入口之间加入设备IRQ交接。保存被打断者的完整frame与可运行资格，再返回普通启动服务栈；没有工作仍走统一用户返回。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    f->rflags |= 0x202; // 保留必需bit1并保持用户IF。
    return f; // 返回选中进程的完整现场。
}
struct airi_trap_frame *airi_user_timer(struct airi_trap_frame *f) { // IRQ路径不分配、不回收、不打印，只有状态与根选择。
    if (!session_active || !current || current->state != USER_RUNNING) // CS3的IRQ必须有明确用户所有者。
        airi_panic("user IRQ without a running process"); // 不把未知内核栈交给调度器。
```
<!-- after -->
```c
    f->rflags |= 0x202; // 保留必需bit1并保持用户IF。
    return f; // 返回选中进程的完整现场。
}
struct airi_trap_frame *airi_user_device_irq(struct airi_trap_frame *f) { // 本函数只在真实设备IRQ打断ring3后调用。
    if (!session_active || !current || current->state != USER_RUNNING) // 用户返回必须有明确现场所有者。
        airi_panic("device IRQ without a running user"); // 不把未知栈交给启动继续点。
    if (!airi_async_work()) // 同步启动请求或已收集的旧通知无需切用户身份。
        return airi_user_resume(f); // 用户返回策略仍统一核对。
    current->frame = f; // 保存完整GPR与用户RIP/RSP，稍后从同一指令继续。
    current->state = USER_RUNNABLE; // 不丢掉被设备提示打断的计算用户运行资格。
    return pause_for_service(false); // 切到长期普通栈后才pump、复制用户输出和唤醒等待者。
}
struct airi_trap_frame *airi_user_timer(struct airi_trap_frame *f) { // IRQ路径不分配、不回收、不打印，只有状态与根选择。
    if (!session_active || !current || current->state != USER_RUNNING) // CS3的IRQ必须有明确用户所有者。
        airi_panic("user IRQ without a running process"); // 不把未知内核栈交给调度器。
```

<a id="block-irq-fallback"></a>
## 将真实交付与屏蔽后备放进同一验证入口

更新Makefile，加入新平台源码；增加block-irq-masked配置，只有该配置在设备准备后仍保持PIC屏蔽。它没有修改设备完成内容，也不模拟一条成功CQ。设备照常DMA并更新used，PIT使原有poll继续运行。

保留PIT还覆盖检查与睡眠之间的时间窗口。在同步perform中，保存IF、检查poll、再sti/hlt使设备待处理中断可唤醒；在其他普通循环中，即使提示已在下一次hlt前处理完，周期PIT仍会再次唤醒。后续若改成完全事件驱动与tickless，需要单独建立防丢唤醒协议，不能直接删除时钟后备。

SOURCES 接入平台 irq.c。

<!-- replace: Makefile -->
<!-- before -->
```make
# 基础目标只编译文本会话与设备；不读取 frontend 的目录或字体。
SOURCES := kernel/main.c kernel/boot/limine.c kernel/boot/info.c \
    kernel/lib/base.c kernel/lib/runtime.c kernel/arch/x86_64/serial.c \
    kernel/console/console.c kernel/arch/x86_64/input.c kernel/arch/x86_64/cpu.c kernel/arch/x86_64/trap.c kernel/mm/pages.c kernel/mm/memory.c kernel/arch/x86_64/paging.c kernel/mm/heap.c kernel/arch/x86_64/timer.c kernel/task/task.c kernel/process/user.c kernel/process/elf.c kernel/fs/file.c kernel/drivers/virtio_block.c kernel/storage/journal.c kernel/storage/demo.c kernel/storage/service.c kernel/io/engine.c kernel/io/service.c
# 只有 desktop- 前缀目标才读取图形支线自己的构建片段。
ifneq ($(filter desktop-%,$(PROFILE)),)
include frontend/sources.mk # 本分支取得图形自己的源码清单。
```
<!-- after -->
```make
# 基础目标只编译文本会话与设备；不读取 frontend 的目录或字体。
SOURCES := kernel/main.c kernel/boot/limine.c kernel/boot/info.c \
    kernel/lib/base.c kernel/lib/runtime.c kernel/arch/x86_64/serial.c \
    kernel/console/console.c kernel/arch/x86_64/input.c kernel/arch/x86_64/cpu.c kernel/arch/x86_64/trap.c kernel/mm/pages.c kernel/mm/memory.c kernel/arch/x86_64/paging.c kernel/mm/heap.c kernel/arch/x86_64/timer.c kernel/task/task.c kernel/process/user.c kernel/process/elf.c kernel/fs/file.c kernel/arch/x86_64/irq.c kernel/drivers/virtio_block.c kernel/storage/journal.c kernel/storage/demo.c kernel/storage/service.c kernel/io/engine.c kernel/io/service.c
# 只有 desktop- 前缀目标才读取图形支线自己的构建片段。
ifneq ($(filter desktop-%,$(PROFILE)),)
include frontend/sources.mk # 本分支取得图形自己的源码清单。
```

在故障配置条件中加入 block-irq-masked，其他目标保持真实设备交付。

<!-- replace: Makefile -->
<!-- before -->
```make
ifeq ($(CASE),block-timeout)
CPPFLAGS += -DAIRI_TEST_BLOCK_TIMEOUT # 验证真实PIT截止与DMA隔离，不冒充设备断电。
endif # 结束块设备故障配置。
# 栈保护故障镜像让未映射栈触发DF，由IST1报告。
ifeq ($(CASE),stack-guard)
CPPFLAGS += -DAIRI_TEST_STACK_GUARD # 此镜像不能发布READY。
```
<!-- after -->
```make
ifeq ($(CASE),block-timeout)
CPPFLAGS += -DAIRI_TEST_BLOCK_TIMEOUT # 验证真实PIT截止与DMA隔离，不冒充设备断电。
endif # 结束块设备故障配置。
# 设备完成存在但故意不交付IRQ，验证PIT驱动的used环后备收集。
ifeq ($(CASE),block-irq-masked)
CPPFLAGS += -DAIRI_TEST_BLOCK_IRQ_MASKED # 不伪造完成，仍使用真实DMA与used.idx。
endif # 其他配置正常开放PCI设备IRQ。
# 栈保护故障镜像让未映射栈触发DF，由IST1报告。
ifeq ($(CASE),stack-guard)
CPPFLAGS += -DAIRI_TEST_STACK_GUARD # 此镜像不能发布READY。
```

统一入口的诊断集合增加这一项，当前默认test变为十种诊断启动，K39的三种服务目标仍单独验证。更新`scripts/manage.py`的配置元组，其他动作保留。

把屏蔽IRQ配置加入原诊断元组，三种服务配置仍单独处理。

<!-- replace: scripts/manage.py -->
<!-- before -->
```python
from storage_test import check_storage_restart # 跨启动恢复与真实QMP节流下的CQ等待。
from console_test import check_console  # 复用真实键盘和UART输入验证。

PROFILES=('normal','no-framebuffer','missing-memory','edit-experiment','trap-fatal','stack-guard','block-readonly','block-timeout','block-missing')  # 元组固定九种构建配置，CLI choices 与 test 遍历共用。
SERVICE_PROFILES=('service','service-no-framebuffer','service-init-failure') # 三种UART常驻/恢复配置，不混入旧阶段完成标记检查。
def run(cmd):  # 所有统一入口外部命令都从同一根目录执行。
    subprocess.run(cmd,cwd=ROOT,check=True)  # 非零状态转成异常传至入口。
```
<!-- after -->
```python
from storage_test import check_storage_restart # 跨启动恢复与真实QMP节流下的CQ等待。
from console_test import check_console  # 复用真实键盘和UART输入验证。

PROFILES=('normal','no-framebuffer','missing-memory','edit-experiment','trap-fatal','stack-guard','block-readonly','block-timeout','block-missing','block-irq-masked')  # 元组固定九种构建配置，CLI choices 与 test 遍历共用。
SERVICE_PROFILES=('service','service-no-framebuffer','service-init-failure') # 三种UART常驻/恢复配置，不混入旧阶段完成标记检查。
def run(cmd):  # 所有统一入口外部命令都从同一根目录执行。
    subprocess.run(cmd,cwd=ROOT,check=True)  # 非零状态转成异常传至入口。
```

更新`scripts/boot.py`。对成功进入块演示的配置，要求先有BLOCK_IRQ_READY，再按当前配置看到真实IRQ或屏蔽后备标记。故意漏发布avail的block-timeout仍应该超时隔离，它不能为了得到IRQ标记而假装设备已经接收请求；缺盘和早期CPU故障也保持各自停止位置。

块演示完成前要求路由就绪，并按当前配置检查真实IRQ或屏蔽后备记录。

<!-- replace: scripts/boot.py -->
<!-- before -->
```python
                        block_marker = '[BLOCK_TIMEOUT_OK]' if case == 'block-timeout' else '[BLOCK_READONLY_OK]' if case == 'block-readonly' else '[BLOCK_DEMO_OK]' # 不同设备条件分别核对真实结果。
                        assert '[BLOCK_READY]' in text and block_marker in text # 完成、拒绝或超时必须符合本配置。
                        if case != 'block-timeout': # 隔离盘不再允许新提交。
                            assert '[BLOCK_NONBLOCK_OK]' in text # begin接受、重叠拒绝和poll收集已运行。
                        journal_marker = '[JOURNAL_SKIPPED]' if case in ('block-timeout', 'block-readonly') else '[JOURNAL_DEMO_OK]' # 没有可写设备时不冒充日志完成。
                        assert journal_marker in text # 正常配置必须真实恢复并替换坏尾。
```
<!-- after -->
```python
                        block_marker = '[BLOCK_TIMEOUT_OK]' if case == 'block-timeout' else '[BLOCK_READONLY_OK]' if case == 'block-readonly' else '[BLOCK_DEMO_OK]' # 不同设备条件分别核对真实结果。
                        assert '[BLOCK_READY]' in text and block_marker in text # 完成、拒绝或超时必须符合本配置。
                        if case != 'block-timeout': # 隔离盘不再允许新提交。
                            irq_marker = '[BLOCK_IRQ_FALLBACK_OK]' if case=='block-irq-masked' else '[BLOCK_IRQ_OK]' # 后备与真实IRQ两条路径分别验收。
                            assert '[BLOCK_IRQ_READY]' in text and irq_marker in text # 接线、独立向量与used收集均须成立。
                            assert '[BLOCK_NONBLOCK_OK]' in text # begin接受、重叠拒绝和poll收集已运行。
                        journal_marker = '[JOURNAL_SKIPPED]' if case in ('block-timeout', 'block-readonly') else '[JOURNAL_DEMO_OK]' # 没有可写设备时不冒充日志完成。
                        assert journal_marker in text # 正常配置必须真实恢复并替换坏尾。
```

<a id="block-irq-observe"></a>
## 读回仍相同，唤醒来源已经增加

从K39完成全部修改后运行：

<!-- run -->
```sh
./airios build # 接入独立平台路由、IRQ43入口和used环收集路径。
./airios test # 十种诊断启动、三种服务、原输入与用户/存储路径均实际运行。
```

Mac若仍选择旧python3，给命令加`PATH=/opt/homebrew/bin:$PATH`。普通启动应先显示真实路由和设备，再给出观察：

```text
[BLOCK_IRQ_READY] irq=11 vector=43 transport=legacy-intx
[BLOCK_READY] pci=0:3.0 io=0x6000 queue=256 sectors=16384 readonly=0 flush=1
[BLOCK_IRQ_OK] vector=43 interrupts=1 used=collected
```

PCI位置、端口与首次观察计数来自本轮固定环境；不能把中断计数当作已完成请求数量。后续日志、异步提交/完成、用户等待与读回仍以原请求身份和数据比较验收。

可以按以下完整操作运行屏蔽配置：

```sh
./airios run --profile block-irq-masked # 保留真实设备完成，但PIC不交付IRQ11。
```

```text
[BLOCK_IRQ_FALLBACK_OK] interrupts=0 used=collected timer=active
```

它继续通过读写、日志恢复和用户演示，进入airi>。这条路径说明遗漏设备通知不会让本实现永久等待；它不表示可以忽略DMA完成，也不把每10ms重查等同于已经实现io_uring或多队列。

服务启动`./airios run --profile service`使用同一路由与设备IRQ，但继续跳过教学写入和BLOCK_IRQ_OK演示；自动监督本身由真实shell与命令验证。实际Linux实验状态不因guest新增IRQ而改变，L01–L08仍需在个人主机实测。

P7的持续回收、init、用户/内核容量与当前块设备事件路径到这里已经串联。真实板的中断路由、其他设备共享IRQ、IOAPIC/MSI-X、多核和长时间稳定性仍未验收；后续开始为C++运行准备用户CPU状态、线程与运行库边界。

