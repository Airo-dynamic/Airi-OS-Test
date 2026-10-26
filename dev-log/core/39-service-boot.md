# K39 · 初始化完成后直接进入用户 init

从 K38 继续。当前镜像开机先做权限故障、堆耗尽、磁盘写入和用户程序演示，再进入监控台，手动输入run init才开始常驻监督。长期无人值守的设备需要另一条启动路径：准备真实资源、恢复已有存储，然后直接进入用户init。教学演示仍保留在默认normal配置中。

本节增加三个串口配置。它们继续使用同一内核、用户ELF、文件和I/O机制，构建目录分别保存，不修改引导协议或用户ABI。启动配置由Make传入的编译宏固定，尚没有运行时启动参数、磁盘服务配置或通用服务管理器。

| 配置 | 启动动作 | 到达的实际结果 |
| --- | --- | --- |
| normal及旧故障配置 | 初始化后运行原诊断演示 | 阶段标记完成后进入airi>；可手动run init |
| service | 初始化、扫描存储、启动init | 不用输入run init，直接出现用户sh> |
| service-no-framebuffer | 同上，适配层模拟缺少图形响应 | UART监督与输入完整，不依赖图形描述 |
| service-init-failure | 初始程序改用已存在的截断ELF broken | 拒绝−8，回到可操作的恢复监控台 |

QEMU仍保留固定VGA硬件；no-framebuffer验证的是引导适配交来的图形描述缺失，不是已经在真实无显示开发板上运行。所有service配置只支持UART交互，CLI在读取前端和字体以前拒绝与--graphics组合。

<a id="service-init-boundary"></a>
## 初始化建立资源，演示消费已经存在的资源

原先有几项必需操作藏在demo函数内：内存初始化里顺便演示页面和权限，timer_demo先配置PIC/PIT，user_demo先建立RAM文件资源。只跳过这些demo会导致时钟或用户文件尚未初始化。现在先分清初始化完成的状态，再让normal选择是否执行观察。

内存初始化保留三个实际动作：建立页池、映射并激活自己的长期CR3、映射64KiB初始对象堆。物理页借还、真实权限故障以及K38的大对象扩展分别进入独立的diagnostic函数，再由airi_memory_demo顺序调用。因此service拥有完整的分配器和页表，也保留1MiB堆上限，但开机没有大对象请求，缓存仍是64KiB。

页演示现在发生在自有CR3已经安装以后。物理页通过airi_memory_direct取得HHDM别名，避免继续依赖加载器阶段的隐含映射。权限演示与原来一样精确匹配故障地址、错误码和恢复位置，normal仍必须实际通过，不能只靠移动函数就发布完成。

先更新`include/airi/memory.h`，为已经初始化的内存服务增加独立观察入口；分配、快照和缓存核对接口保持K38。

<!-- write: include/airi/memory.h -->
```c
#ifndef AIRI_MEMORY_H // 通用内存服务与只读数据快照。
#define AIRI_MEMORY_H // 包含保护。
#include <airi/boot.h> // 启动合同和固定宽整数。
#include <stddef.h> // 小对象请求按字节计数。
struct airi_memory_stats { // 消费者只得到数值，不持有分配器内部块指针。
    uint32_t pages_total, pages_free; // 实际受管理物理页与当前未借出页数。
    size_t heap_bytes, heap_free, heap_used; // 已映射backing、空闲payload与活对象payload分别统计。
};
void airi_memory_init(const AiriBootInfo *info); // 顺序初始化页池、页表和可扩展内核堆。
void airi_memory_demo(void); // 初始化之后才执行页面、权限与堆教学观察，服务启动可跳过。
void airi_memory_snapshot(struct airi_memory_stats *stats); // 同步复制当前数值到调用者对象。
static inline bool airi_memory_same_usage(const struct airi_memory_stats *before, const struct airi_memory_stats *after) { // 每个包含者使用这段小函数，无独立链接符号。
    uint64_t old_available = before->pages_free + before->heap_bytes / 4096; // 堆的缓存数据页从页池转出，但没有外部借用。
    uint64_t new_available = after->pages_free + after->heap_bytes / 4096; // 当前1MiB堆始终使用初始已建的同一张叶子表。
    return old_available == new_available && before->heap_used == after->heap_used; // 新增活对象或堆以外的页泄漏都不能被缓存抵消。
}
void *airi_kmalloc(size_t bytes); // 对象空间不足时按64KiB批次扩展，最大1MiB，不承诺完整libc malloc ABI。
bool airi_kfree(void *pointer); // 结束对象借用，backing 页仍由内核堆持有。
#define AIRI_REGION_PAGES 16u // 当前有界内核区域最多持有十六个数据页。
struct airi_kernel_region { // 所有权句柄必须在使用前清零，不能按值复制后重复释放。
    uint64_t virtual_address; // 所有数据页连续映射的内核虚拟起点。
    uint64_t physical[AIRI_REGION_PAGES]; // 逐页记录实际借出的物理页，便于精确回滚。
    unsigned count; // 只有已经成功映射的页才计入所有权。
};
bool airi_memory_region_create(struct airi_kernel_region *region, uint64_t address, unsigned count); // 建立新的有界RW/NX内核区域，不覆盖已有映射。
bool airi_memory_region_free(struct airi_kernel_region *region); // 所有借用结束后先解除映射，再归还数据页。
struct airi_address_space; // 内核内部可持有地址空间，不向UI交出页表。
bool airi_memory_user_space(struct airi_address_space *space); // 从长期内核根借用高半区。
void airi_memory_use_kernel(void); // 返回启动内核地址空间后才能销毁用户空间。
uint64_t airi_memory_take_page(void); // 内核对象明确持有一个物理数据页。
bool airi_memory_return_page(uint64_t physical); // 映射和借用结束后归还。
void *airi_memory_direct(uint64_t physical); // 当前受支持RAM的HHDM别名，不创建新映射。
#endif // 结束包含保护。
```

更新`kernel/mm/memory.c`。从airi_memory_init取出页借还代码放入physical_memory_demo，从virtual_memory_init取出权限观察放入virtual_memory_demo，从heap_init取出对象观察放入heap_demo。heap_init只完成第一批映射并发布HEAP_READY；末尾的airi_memory_demo把三项观察接回。其余区域、堆扩展和回收顺序保持。

先声明独立 heap_demo，避免它成为初始化的隐含动作。

本章每组替换的第一块只用于定位旧文本，敲入第二块替换它，不把两块同时加入文件。各组按正文顺序完成。

<!-- replace: kernel/mm/memory.c -->
<!-- before -->
```c
#include <airi/pages.h> // 物理页分配与启动信息。
#include <airi/kernel.h> // 日志和具体失败出口。
static struct airi_page_pool pages; // 页池元数据放在内核 BSS，不能自己被分配。
static void heap_init(void); // 自有页表建立后才映射堆backing。
static void virtual_memory_init(const AiriBootInfo *info); // 页池就绪后才建立并激活页表。
void airi_memory_init(const AiriBootInfo *info) { // 在异常设施安装后调用，仍使用加载器页表。
```
<!-- after -->
```c
#include <airi/pages.h> // 物理页分配与启动信息。
#include <airi/kernel.h> // 日志和具体失败出口。
static struct airi_page_pool pages; // 页池元数据放在内核 BSS，不能自己被分配。
static void heap_demo(void); // 教学观察不属于设备或资源初始化。
static void heap_init(void); // 自有页表建立后才映射堆backing。
static void virtual_memory_init(const AiriBootInfo *info); // 页池就绪后才建立并激活页表。
void airi_memory_init(const AiriBootInfo *info) { // 在异常设施安装后调用，仍使用加载器页表。
```

让 airi_memory_init 只建立长期设施，将页借还及访问移入 physical_memory_demo；HHDM访问的位置随它移动。

<!-- replace: kernel/mm/memory.c -->
<!-- before -->
```c
        airi_panic("physical page pool has no usable pages"); // 停止，而不是假装分配成功。
    airi_log("[PAGES_READY] page_bytes=4096 capacity_bytes=%llu managed=%u free=%u", // 明确固定上限和实际管理数量。
             (unsigned long long)AIRI_PHYSICAL_LIMIT, pages.total, pages.free); // 内存地图中的非 usable 页面不在统计中。
    uint32_t before = pages.free; // 保存演示前空闲数量，最后必须恢复。
    uint64_t first = airi_page_alloc(&pages); // 取得第一个物理页，0 是失败。
    uint64_t second = airi_page_alloc(&pages); // 第二个页面必须独立借出。
    if (!first || !second || first == second) // 耗尽和重复借出都不允许继续写入。
        airi_panic("physical page allocation failed"); // 停止潜在的内存破坏。
    volatile uint64_t *mapped = (volatile uint64_t *)(uintptr_t)(info->direct_map_offset + first); // HHDM 将物理页转换为当前可访问的虚拟别名。
    *mapped = 0x4149524950414745ull; // 真正写入刚借出的 RAM，volatile 保留本次普通 RAM 写读，不提供并发同步。
    if (*mapped != 0x4149524950414745ull || pages.free != before - 2) // 验证内容和所有权统计是同一次分配结果。
        airi_panic("physical page write/accounting failed"); // 不能只凭非零地址宣称可用。
    if (!airi_page_free(&pages, first) || airi_page_free(&pages, first) || airi_page_free(&pages, second + 1)) // 验证正常归还、重复释放拒绝和未对齐释放拒绝。
        airi_panic("physical page release validation failed"); // 失败释放不应改变 second 的所有权。
    uint64_t again = airi_page_alloc(&pages); // 应复用刚归还的低地址页。
    if (again != first || !airi_page_free(&pages, again) || !airi_page_free(&pages, second) || pages.free != before) // 所有演示借用在结束前归还。
        airi_panic("physical page reuse/accounting failed"); // 任何泄漏都阻止后续阶段。
    airi_log("[PAGES_DEMO_OK] first=0x%llx second=0x%llx reused=0x%llx free=%u", // 日志给出本轮真实分配地址。
             (unsigned long long)first, (unsigned long long)second, (unsigned long long)again, pages.free); // 记录数值，不在归还后解引用旧指针。
    virtual_memory_init(info); // 页表页也从同一所有权池取得。
    heap_init(); // 小对象分配不能反过来成为页表构建的前置。
}

static struct airi_address_space kernel_space; // 当前内核持有根页，所有映射在切换后持续存在。
```
<!-- after -->
```c
        airi_panic("physical page pool has no usable pages"); // 停止，而不是假装分配成功。
    airi_log("[PAGES_READY] page_bytes=4096 capacity_bytes=%llu managed=%u free=%u", // 明确固定上限和实际管理数量。
             (unsigned long long)AIRI_PHYSICAL_LIMIT, pages.total, pages.free); // 内存地图中的非 usable 页面不在统计中。
    virtual_memory_init(info); // 只建立并激活长期页表，不触发权限故障。
    heap_init(); // 初始映射64KiB，服务启动不先运行容量演示。
}
static void physical_memory_demo(void) { // 页池已经初始化，临时借出和归还只用于诊断配置。
    uint32_t before = pages.free; // 保存演示前空闲数量，最后必须恢复。
    uint64_t first = airi_page_alloc(&pages); // 取得第一个物理页，0 是失败。
    uint64_t second = airi_page_alloc(&pages); // 第二个页面必须独立借出。
    if (!first || !second || first == second) // 耗尽和重复借出都不允许继续写入。
        airi_panic("physical page allocation failed"); // 停止潜在的内存破坏。
    volatile uint64_t *mapped = airi_memory_direct(first); // HHDM 将物理页转换为当前可访问的虚拟别名。
    *mapped = 0x4149524950414745ull; // 真正写入刚借出的 RAM，volatile 保留本次普通 RAM 写读，不提供并发同步。
    if (*mapped != 0x4149524950414745ull || pages.free != before - 2) // 验证内容和所有权统计是同一次分配结果。
        airi_panic("physical page write/accounting failed"); // 不能只凭非零地址宣称可用。
    if (!airi_page_free(&pages, first) || airi_page_free(&pages, first) || airi_page_free(&pages, second + 1)) // 验证正常归还、重复释放拒绝和未对齐释放拒绝。
        airi_panic("physical page release validation failed"); // 失败释放不应改变 second 的所有权。
    uint64_t again = airi_page_alloc(&pages); // 应复用刚归还的低地址页。
    if (again != first || !airi_page_free(&pages, again) || !airi_page_free(&pages, second) || pages.free != before) // 所有演示借用在结束前归还。
        airi_panic("physical page reuse/accounting failed"); // 任何泄漏都阻止后续阶段。
    airi_log("[PAGES_DEMO_OK] first=0x%llx second=0x%llx reused=0x%llx free=%u", // 日志给出本轮真实分配地址。
             (unsigned long long)first, (unsigned long long)second, (unsigned long long)again, pages.free); // 记录数值，不在归还后解引用旧指针。
}

static struct airi_address_space kernel_space; // 当前内核持有根页，所有映射在切换后持续存在。
```

长期根建立完成即结束 virtual_memory_init，权限故障另从 virtual_memory_demo 进入。

<!-- replace: kernel/mm/memory.c -->
<!-- before -->
```c
        airi_panic("x86 paging requires four levels, NX and no PCID"); // 不在未支持的启动配置上假装成功。
    airi_log("[VM_READY] cr3=0x%llx levels=4 page_bytes=4096 wp=1 nx=1 free=%u", // 只有真正切根后才输出。
             (unsigned long long)kernel_space.root, pages.free); // 页表持有的页面计入已借出数量。
    uint32_t before = pages.free; // 演示只临时创建一个独立分支，最后应全部回收。
    uint64_t data = airi_page_alloc(&pages); // 调用者拥有这个数据页，页表不会替它归还。
    if (!data) // 演示也必须处理耗尽。
        airi_panic("cannot allocate paging demonstration page"); // 不写失败哨兵。
    uint64_t *alias = (uint64_t *)(uintptr_t)(info->direct_map_offset + data); // 新根已保留全部受管理 RAM 的 HHDM。
    *alias = 0xc3; // 第一字节是 ret，若 NX 缺失，调用也能返回并被演示检查拒绝。
    if (!airi_vm_map(&kernel_space, AIRI_VM_DEMO_ADDRESS, data, 0)) // 建立 supervisor 只读、不可执行的别名。
        airi_panic("cannot map paging demonstration page"); // 失败不能访问目标 VA。
```
<!-- after -->
```c
        airi_panic("x86 paging requires four levels, NX and no PCID"); // 不在未支持的启动配置上假装成功。
    airi_log("[VM_READY] cr3=0x%llx levels=4 page_bytes=4096 wp=1 nx=1 free=%u", // 只有真正切根后才输出。
             (unsigned long long)kernel_space.root, pages.free); // 页表持有的页面计入已借出数量。
}
static void virtual_memory_demo(void) { // 安装长期根以后才执行可恢复的只读/NX/未映射故障。
    uint32_t before = pages.free; // 演示只临时创建一个独立分支，最后应全部回收。
    uint64_t data = airi_page_alloc(&pages); // 调用者拥有这个数据页，页表不会替它归还。
    if (!data) // 演示也必须处理耗尽。
        airi_panic("cannot allocate paging demonstration page"); // 不写失败哨兵。
    uint64_t *alias = airi_memory_direct(data); // 新根已保留全部受管理 RAM 的 HHDM。
    *alias = 0xc3; // 第一字节是 ret，若 NX 缺失，调用也能返回并被演示检查拒绝。
    if (!airi_vm_map(&kernel_space, AIRI_VM_DEMO_ADDRESS, data, 0)) // 建立 supervisor 只读、不可执行的别名。
        airi_panic("cannot map paging demonstration page"); // 失败不能访问目标 VA。
```

heap_init 只建立最小批次并报告就绪，原对象与扩容观察从独立 heap_demo 开始。

<!-- replace: kernel/mm/memory.c -->
<!-- before -->
```c
static void heap_init(void) { // 所需页与页表都由当前内核拥有。
    if (!heap_grow()) // 第一批也遵守映射成功后才接入的顺序。
        airi_panic("cannot initialize kernel heap"); // 启动必须有最小对象容量。
    size_t before = airi_heap_free_bytes(&heap); // 结束后对象级空闲应恢复。
    uint8_t *a = airi_kmalloc(80); // 第一个对象拥有独立payload。
    uint8_t *b = airi_kmalloc(128); // 中间对象用于演示相邻归还合并。
```
<!-- after -->
```c
static void heap_init(void) { // 所需页与页表都由当前内核拥有。
    if (!heap_grow()) // 第一批也遵守映射成功后才接入的顺序。
        airi_panic("cannot initialize kernel heap"); // 启动必须有最小对象容量。
    airi_log("[HEAP_READY] bytes=%zu limit=1048576 chunks=%u", heap.bytes, heap_chunks); // 初始化只报告已经持有的最小容量。
}
static void heap_demo(void) { // 小对象、扩展与耗尽留在诊断入口，服务启动不执行。
    size_t before = airi_heap_free_bytes(&heap); // 结束后对象级空闲应恢复。
    uint8_t *a = airi_kmalloc(80); // 第一个对象拥有独立payload。
    uint8_t *b = airi_kmalloc(128); // 中间对象用于演示相邻归还合并。
```

在三个独立观察函数可用后，增加 airi_memory_demo 逐一调用；原小对象分配接口继续在其后。

<!-- replace: kernel/mm/memory.c -->
<!-- before -->
```c
        airi_panic("kernel heap cache accounting failed"); // 页或对象泄漏仍需报告。
    airi_log("[HEAP_CAPACITY_OK] initial=65536 bytes=%zu live=%zu chunks=%u preserved=1 limit=null", grown.heap_bytes, grown.heap_used, heap_chunks); // 真实映射、写入、耗尽和归还完成。

}
void *airi_kmalloc(size_t bytes) { // 内核小对象入口，不引入宿主libc。
    if (!heap.first || !bytes || bytes > HEAP_MAX_CHUNKS * HEAP_STEP_PAGES * 4096u - 32) // 未初始化、空请求或绝不可能容纳的对象不触发增长。
```
<!-- after -->
```c
        airi_panic("kernel heap cache accounting failed"); // 页或对象泄漏仍需报告。
    airi_log("[HEAP_CAPACITY_OK] initial=65536 bytes=%zu live=%zu chunks=%u preserved=1 limit=null", grown.heap_bytes, grown.heap_used, heap_chunks); // 真实映射、写入、耗尽和归还完成。

}
void airi_memory_demo(void) { // 从完整初始化状态执行三类观察，不能代替初始化。
    physical_memory_demo(); // 临时页面的内容和归还。
    virtual_memory_demo(); // 真实硬件权限故障与精确恢复。
    heap_demo(); // 对象分割/合并及1MiB容量边界。
}
void *airi_kmalloc(size_t bytes) { // 内核小对象入口，不引入宿主libc。
    if (!heap.first || !bytes || bytes > HEAP_MAX_CHUNKS * HEAP_STEP_PAGES * 4096u - 32) // 未初始化、空请求或绝不可能容纳的对象不触发增长。
```

时钟配置同样只执行一次。更新`kernel/arch/x86_64/timer.c`，从timer_demo删除初始化调用；main稍后显式调用timer_init，normal再等待五个真实tick。service也需要PIT支持sleep、超时与用户调度，不能因没有教学等待就漏掉设备初始化。

<!-- write: kernel/arch/x86_64/timer.c -->
```c
#include <airi/timer.h> // 共享IRQ0的初始化、读取与处理接口。
#include <airi/kernel.h> // 在正常上下文发布时钟观察结果。
static volatile uint64_t ticks; // 当前单CPU下，IRQ写、内核读取，volatile保留每次观察。
static void out8(uint16_t port, uint8_t value) { // 端口I/O只留在PC架构驱动中。
    __asm__ volatile("outb %0,%1" :: "a"(value), "Nd"(port)); // Nd允许8位立即端口或DX，a传入AL数据。
}
static void pic_write(uint16_t port, uint8_t value) { // 旧式PIC配置操作之间保留I/O间隔。
    out8(port, value); // 先提交本次配置字节。
    out8(0x80, 0); // PC兼容延迟端口；不是向UART输出文字。
}
void airi_timer_init(void) { // 调用前已安装自己的IDT，内核目前仍关闭IF。
    __asm__ volatile("cli" ::: "memory"); // 配置PIC的过程中不接收未完成重映射的IRQ。
    uint32_t low, high; // IA32_APIC_BASE同时包含物理基址和APIC模式标志。
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(0x1bu)); // 固定qemu64具备本地APIC，读取其全局使能状态。
    if (low & (1u << 10)) // 当前不实现x2APIC状态迁移。
        airi_panic("legacy PIC timer does not support x2APIC mode"); // 拒绝未支持配置而不是永远等不到IRQ。
    low &= ~(1u << 11); // 关闭本地APIC全局使能，选择单核PC兼容PIC直接交付路径。
    __asm__ volatile("wrmsr" :: "a"(low), "d"(high), "c"(0x1bu) : "memory"); // 保留APIC物理基址和其他标志。
    pic_write(0x21, 0xff); // 首先屏蔽主PIC全部八路。
    pic_write(0xa1, 0xff); // 屏蔽从PIC，键盘继续使用轮询而不是IRQ。
    pic_write(0x20, 0x11); // 主PIC初始化：级联、边沿、后续提供ICW4。
    pic_write(0xa0, 0x11); // 从PIC使用同一模式。
    pic_write(0x21, 0x20); // 主PIC IRQ0–7映射到IDT32–39，避开CPU异常0–31。
    pic_write(0xa1, 0x28); // 从PIC IRQ8–15映射到IDT40–47。
    pic_write(0x21, 4); // 主PIC的IRQ2连接从PIC，位2置一。
    pic_write(0xa1, 2); // 从PIC的级联身份为2。
    pic_write(0x21, 1); // 主PIC使用8086模式，软件发送EOI。
    pic_write(0xa1, 1); // 从PIC同样不自动EOI。
    uint16_t divisor = 1193182u / 100u; // PIT输入频率近似1.193182MHz，整数分频得到约100Hz。
    out8(0x43, 0x34); // 通道0、先低后高两字节、模式2周期计数、二进制。
    out8(0x40, (uint8_t)divisor); // 先写分频器低八位。
    out8(0x40, (uint8_t)(divisor >> 8)); // 再写高八位，完整周期配置生效。
    ticks = 0; // 在开放IRQ之前初始化软件计数。
    uint8_t mask = 0xfe; // 普通配置只开放IRQ0。
#ifdef AIRI_TEST_TIMER_MASKED // 作者故障镜像故意屏蔽所有设备IRQ。
    mask = 0xff; // CPU可以开启IF，但没有任何PIC输入获准交付。
#endif // 普通镜像保留IRQ0。
    pic_write(0x21, mask); // 其他设备继续屏蔽。
    pic_write(0xa1, 0xff); // 从PIC仍全部屏蔽，没有未实现的设备IRQ入口。
    airi_log("[TIMER_CONFIG] lapic=disabled pic_mask=0x%x pit_divisor=%u", (unsigned)mask, (unsigned)divisor); // 尚未开放IF时记录硬件配置。
    __asm__ volatile("sti" ::: "memory"); // 允许CPU接收已经安装入口的时钟IRQ。
}
uint64_t airi_timer_ticks(void) { // x86-64对齐64位读取不撕裂，当前无多核并发。
    return ticks; // 每次真正读取IRQ更新过的对象。
}
void airi_timer_interrupt(void) { // IDT32中断门进入时CPU已经清IF。
    ++ticks; // 先记录本次实际完成入口的时钟事件。
    out8(0x20, 0x20); // 向主PIC发送EOI，允许后续IRQ0继续到达。
}
void airi_timer_demo(void) { // 此时只演示中断唤醒，不存在任务调度。
    // 调用者已开启固定平台时钟；此函数不重新配置PIC或清零ticks。
    uint64_t start = airi_timer_ticks(); // 记录当前基准，不假定初始化后尚未经过第一个tick。
    while (airi_timer_ticks() - start < 5) // 至少处理五个真实IRQ，而不是忙等某个常数。
        __asm__ volatile("hlt" ::: "memory"); // IF为一，下一次IRQ唤醒后从这里继续。
    airi_log("[TIMER_DEMO_OK] source=pit-pic vector=32 target_hz=100 elapsed_ticks=%llu", // 正常上下文输出，IRQ里不调用串口或分配器。
             (unsigned long long)(airi_timer_ticks() - start)); // 计数可以超过5，不把整数分频当精确墙钟。
}
```

<a id="service-user-start"></a>
## 用户服务从真实空状态启动

新增airi_user_init，在启动时建立只读seed、空RAM文件目录和固定终端对象。user_demo只消费这些资源，不再重置它们。这样service可以直接fork、exec和读写FD，无需先运行hello或文件演示。

airi_user_boot接到的是内核启动策略选定的程序名。它从与命令入口相同的初始ELF目录取得映像，再调用已有launch；因此开机启动init仍经过ELF校验、私有用户页、内核陷入栈和异步资源登记，全部准备好以后才进入ring3。

进入用户态以后，内核这次启动调用尚未结束。init沿K36的fork/exec/wait和sleep持续监督shell，一代shell退出只让同一个init继续下一轮。服务模式在此以前没有运行诊断用户程序，所以本轮init自然取得PID1；授予监督资格依据的仍是launch记录的身份，而不是把数字1写死。

更新`include/airi/user.h`，然后在user.c的user_demo之前加入两个函数，并删除user_demo中的文件初始化调用。boot函数只有在装载失败或整个持续用户会话已经停止时才返回数值结果；一次shell logout仍只唤醒init并在100tick后重启。

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
struct airi_trap_frame *airi_user_timer(struct airi_trap_frame *frame); // 用户时间片在完整现场边界轮转，内核仍不可抢占。
struct airi_trap_frame *airi_user_resume(struct airi_trap_frame *frame); // 检查用户返回地址并清理不能交给iretq的标志。
struct airi_console; // 只借用文本会话，不依赖图形状态。
bool airi_user_command(struct airi_console *console, const char *command); // programs与run命令使用实际用户服务。
#endif // 结束包含保护。
```

在 user_demo 之前提供独立 user_init 与 user_boot；删除演示入口里的重复文件初始化。服务和诊断从此消费同一份已建立资源。

<!-- replace: kernel/process/user.c -->
<!-- before -->
```c
    if (require_scheduler) // 实际多用户会话单独报告行为证据。
        airi_log("[PROCESS_DEMO_OK] fork=copy exec=transaction wait=blocked timer_switches=%u wait_blocks=%u reclaimed=1", result.switches, result.blocks); // 计数来自真实IRQ与状态转换。
}
void airi_user_demo(void) { // 串口主线的真实用户边界与多进程回归。
    airi_files_init(); // 用户会话开始前创建只读seed与空RAM目录，仅启动一次。
    run_one(airi_user_probe_start, (size_t)((uintptr_t)airi_user_probe_end - (uintptr_t)airi_user_probe_start), NULL, 0, false, true, false); // 原有非法指针与用户IRQ验证。
    run_one(airi_user_bad_start, (size_t)((uintptr_t)airi_user_bad_end - (uintptr_t)airi_user_bad_start), NULL, -141, false, false, false); // 用户cli只杀当前身份。
    run_one(airi_user_load_bad_start, (size_t)((uintptr_t)airi_user_load_bad_end - (uintptr_t)airi_user_load_bad_start), NULL, -142, false, false, false); // USER保护PF。
```
<!-- after -->
```c
    if (require_scheduler) // 实际多用户会话单独报告行为证据。
        airi_log("[PROCESS_DEMO_OK] fork=copy exec=transaction wait=blocked timer_switches=%u wait_blocks=%u reclaimed=1", result.switches, result.blocks); // 计数来自真实IRQ与状态转换。
}
void airi_user_init(void) { // 启动只调用一次；不依赖先运行任何用户演示。
    airi_files_init(); // 为普通用户与init同样建立只读seed和空RAM目录。
    airi_log("[USER_RESOURCES_READY] slots=%u elf=static", PROCESS_COUNT); // 仅声明资源可用，不宣称已经运行服务。
}
int airi_user_boot(const char *name) { // 输入来自内核固定启动策略，不接受外部未验证指针。
    const struct executable *image = find_program(name); // 与命令入口使用同一真实映像目录。
    if (!image) // 不存在程序时尚未改变任何进程资源。
        return -2; // -ENOENT，交给启动恢复策略。
    struct user_result result; // 持续会话停止后才取得根退出状态。
    int error = launch(image->start, (size_t)((uintptr_t)image->end - (uintptr_t)image->start), name, true, &result); // 准备失败与正常返回均由launch核对回收。
    return error ? error : result.code; // 初始化失败或用户退出都不能被当作仍在服务。
}
void airi_user_demo(void) { // 串口主线的真实用户边界与多进程回归。
    // 用户资源已由airi_user_init建立，演示不重置正式初始化状态。
    run_one(airi_user_probe_start, (size_t)((uintptr_t)airi_user_probe_end - (uintptr_t)airi_user_probe_start), NULL, 0, false, true, false); // 原有非法指针与用户IRQ验证。
    run_one(airi_user_bad_start, (size_t)((uintptr_t)airi_user_bad_end - (uintptr_t)airi_user_bad_start), NULL, -141, false, false, false); // 用户cli只杀当前身份。
    run_one(airi_user_load_bad_start, (size_t)((uintptr_t)airi_user_load_bad_end - (uintptr_t)airi_user_load_bad_start), NULL, -142, false, false, false); // USER保护PF。
```

<a id="service-main-flow"></a>
## 服务启动只扫描存储，再进入监督者

从复位走向init时，main先安装异常入口，建立内存服务并配置时钟，再准备VirtIO块设备。日志服务扫描已有存储内容，异步队列和用户文件资源也在这条启动路径上建立。扫描仍会真实读盘；失败时不能把盘当作空盘继续提供服务。

normal在相应资源准备好以后执行原来的观察，service则跳过主动异常、大堆请求、临时任务、磁盘写入和用户演示。它仍拥有同一套资源，只是不先运行这些消耗资源的程序。两种模式也都建立文本监控台，作为启动失败后可从UART操作的恢复位置；service不先绘图。

发布AIRI_SERVICE_RESOURCES_READY以后，内核资源已经就绪，接下来才调用airi_user_boot装载init。INIT_SESSION来自内核会话，INIT_START来自已经进入ring3的init，USER_SHELL_PROMPT则来自它启动的真实shell。沿这几个位置继续执行hello，才能同时观察资源准备、用户启动和命令服务；只到达资源标记时，shell尚未提供输入。

若初始映像被拒绝，launch按原事务规则保持页池、活对象和FD引用恢复，main报告SERVICE_RECOVERY并进入原监控台。这里不在内核里无限重试坏ELF，以免持续刷日志而失去诊断入口。该恢复也不等于完整进程树崩溃管理：已经运行的init若退出但仍有活后代，launch会先按现有会话规则等待后代结束；通用监督、信号和watchdog另行接入。

更新`kernel/main.c`。diagnostics是由当前编译配置固定的bool；旧P1–P6标记只由真正运行过演示的normal分支发布，service不会冒充这些测试已在本次启动执行。

在静态控制台之后用编译配置选择 diagnostics，normal和service各取固定策略。

<!-- replace: kernel/main.c -->
<!-- before -->
```c
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
```
<!-- after -->
```c
#ifndef AIRI_DEMO_MESSAGE  // 允许修改实验在编译时替换输出文本。
#define AIRI_DEMO_MESSAGE "Hello from AiriOS P1"  // 普通配置默认输出的学习标记文字。
#endif  // 结束默认演示文字的条件定义
#ifdef AIRI_BOOT_SERVICE // 专用构建配置明确选择无屏常驻路径。
static const bool diagnostics = false; // 不在开机时运行教学故障、磁盘写入或进程演示。
#else // normal和旧故障配置继续保留完整教学启动。
static const bool diagnostics = true; // 默认运行方式和旧检查点的输入行为保持。
#endif // 这里只选择启动策略，不改变用户ABI。
_Noreturn void airi_kernel_main(const AiriBootInfo *info) {  // 引导适配交入稳定启动描述；_Noreturn 声明内核不返回加载器。
    const char *error = airi_boot_validate(info);  // 校验必需启动字段，NULL 表示通过，字符串给出失败原因。
    if (error)  // 有具体原因则不能继续读取未经验证的数据。
        airi_panic(error);  // 输出失败原因并停止当前 CPU 执行。
    airi_trap_init(); // 通过启动校验后先接管异常，不启用设备中断。
    airi_log("[BOOT_MODE] name=%s", diagnostics ? "diagnostic" : "service"); // 当前镜像的启动策略明确可见。
    if (diagnostics) // 服务启动只安装异常入口，不主动触发错误。
        airi_trap_demo(); // 诊断配置验证两种异常栈形状。
#ifdef AIRI_TEST_FATAL_TRAP // 只在作者故障镜像中执行未处理异常。
    airi_trap_fatal_demo(); // 必须报告并停止，不能发布 READY。
#endif // 普通镜像继续初始化。
```

按资源就绪位置调用可选观察，时钟与用户文件资源在两种模式都显式初始化。

<!-- replace: kernel/main.c -->
<!-- before -->
```c
             (unsigned long long)usable,  // 可用字节数转换匹配 %llu。
             (unsigned long long)airi_memory_bytes(info, AIRI_MEMORY_BOOT_RECLAIM));  // 另行累计加载器可回收区域，不混入 usable。
    airi_log("[LESSON] %s", AIRI_DEMO_MESSAGE);  // 发送当前配置的学习文本，修改实验可据此核对版本。
    airi_memory_init(info); // 顺序建立页池、自有映射与固定内核堆，完成本阶段演示。
    airi_timer_demo(); // 从PIT/PIC收到真实IRQ0，hlt后恢复到正常上下文。
    airi_task_demo(); // 当前只在正常内核上下文主动切换，IRQ不抢占内核任务。
    int block_error = airi_block_init(info); // 固定单队列设备，未连接时明确停止当前存储阶段。
    if (block_error) { // 默认运行必须带独立实验盘。
        airi_log("[BLOCK_INIT_ERROR] error=%d", block_error); // 先记录具体设备错误。
        airi_panic("virtio block initialization failed"); // 不把缺设备当成功。
    }
    airi_block_demo(); // 真正DMA读写、FLUSH与范围拒绝。
    airi_journal_demo(); // 固定实验扇区上提交、校验、坏尾截断与恢复。
    int storage_error = airi_storage_init(); // 启动扫描，绝不自动追加正式记录。
    if (storage_error) { // I/O错误不能当作空盘。
        airi_log("[STORAGE_INIT_ERROR] error=%d", storage_error); // 保留具体失败。
        airi_panic("storage recovery failed"); // 不继续提供错误前缀。
    }
    airi_async_init(); // 所有设备配置都初始化固定资源池，设备错误由CQ表达。
    airi_async_demo(); // 三个真实设备操作和一个有界范围错误完成。
    airi_user_demo(); // 正常与非法用户程序结束后仍回到启动栈和串口。
    airi_console_init(&console, info);  // 初始化内核静态控制台，借用整个启动期间有效的 info。
    console.service = airi_user_command; // 系统程序服务独立于可选主题/窗口扩展。
    console.read_memory = airi_memory_snapshot; // 文本查询只读取数值，不持有分配器内部对象。
    bool graphics = airi_console_draw(&console);  // 实际完成首帧后取得布尔结果，不能仅凭描述存在认定成功。
    if (graphics) {  // 有效图形路径才输出图形参数。
        const struct airi_framebuffer *fb = &info->framebuffer;  // 借用刚用于绘图的同一份帧缓冲描述。
        airi_log("[FRAMEBUFFER] width=%llu height=%llu pitch=%llu bpp=%u",  // 记录尺寸、字节行距和每像素位数，供截图与日志比对。
```
<!-- after -->
```c
             (unsigned long long)usable,  // 可用字节数转换匹配 %llu。
             (unsigned long long)airi_memory_bytes(info, AIRI_MEMORY_BOOT_RECLAIM));  // 另行累计加载器可回收区域，不混入 usable。
    airi_log("[LESSON] %s", AIRI_DEMO_MESSAGE);  // 发送当前配置的学习文本，修改实验可据此核对版本。
    airi_memory_init(info); // 只初始化页池、自有页表与最小堆，不依赖教学观察。
    if (diagnostics) // 用户可保留完整旧启动回归，也可选择不运行演示。
        airi_memory_demo(); // 页面、权限故障和堆容量均在资源就绪后观察。
    airi_timer_init(); // 服务与诊断都需要真实周期时钟，不能靠demo偷偷初始化。
    if (diagnostics) { // 已安装时钟后才演示hlt与主动任务切换。
        airi_timer_demo(); // 此时不再重置计数或重配PIC。
        airi_task_demo(); // 服务启动不创建这两个临时演示任务。
    }
    int block_error = airi_block_init(info); // 固定单队列设备，未连接时明确停止当前存储阶段。
    if (block_error) { // 默认运行必须带独立实验盘。
        airi_log("[BLOCK_INIT_ERROR] error=%d", block_error); // 先记录具体设备错误。
        airi_panic("virtio block initialization failed"); // 不把缺设备当成功。
    }
    if (diagnostics) { // 设备初始化与恢复不需要先写教学扇区。
        airi_block_demo(); // 诊断配置保留真实DMA与FLUSH观察。
        airi_journal_demo(); // 只在诊断配置执行临时区域的提交与坏尾演示。
    }
    int storage_error = airi_storage_init(); // 启动扫描，绝不自动追加正式记录。
    if (storage_error) { // I/O错误不能当作空盘。
        airi_log("[STORAGE_INIT_ERROR] error=%d", storage_error); // 保留具体失败。
        airi_panic("storage recovery failed"); // 不继续提供错误前缀。
    }
    airi_async_init(); // 所有设备配置都初始化固定资源池，设备错误由CQ表达。
    airi_user_init(); // 正式初始化FD资源，不借用户演示建立隐含前置。
    if (diagnostics) { // 两种模式共用已初始化的设备、队列与用户资源。
        airi_async_demo(); // 真实异步操作与错误仅在诊断配置观察。
        airi_user_demo(); // 非法用户、fork/exec和I/O完整回归保持。
    }
    airi_console_init(&console, info);  // 初始化内核静态控制台，借用整个启动期间有效的 info。
    console.service = airi_user_command; // 系统程序服务独立于可选主题/窗口扩展。
    console.read_memory = airi_memory_snapshot; // 文本查询只读取数值，不持有分配器内部对象。
    bool graphics = diagnostics && airi_console_draw(&console);  // 实际完成首帧后取得布尔结果，不能仅凭描述存在认定成功。
    if (graphics) {  // 有效图形路径才输出图形参数。
        const struct airi_framebuffer *fb = &info->framebuffer;  // 借用刚用于绘图的同一份帧缓冲描述。
        airi_log("[FRAMEBUFFER] width=%llu height=%llu pitch=%llu bpp=%u",  // 记录尺寸、字节行距和每像素位数，供截图与日志比对。
```

首帧之后分开服务装载与诊断完成记录：服务失败回同一监控台，正常服务调用在用户监督期间不返回。

<!-- replace: kernel/main.c -->
<!-- before -->
```c
                 (unsigned long long)fb->pitch, (unsigned)fb->bpp);  // pitch 按字节输出，bpp 按位数输出。
    } else  // 未完成图形时仍发送串口原因并继续启动。
        airi_log("[DISPLAY_SERIAL] no frame produced; using serial output");  // 图形不适用时记录原因路径，后面仍进入串口命令循环。
    /* Written AFTER all drawing, so tests can take a completed screenshot. */
    airi_log("[AIRI_P1_READY] graphics=%u usable_bytes=%llu", graphics ? 1u : 0u,  // 首帧调用结束后才发布完成标记，布尔转换为明确的0/1无符号值。
             (unsigned long long)usable);  // 完成记录附同一实际可用字节数。
    airi_log("[AIRI_P2_READY] exceptions=1 pages=1 own_cr3=1 heap=1"); // 所有内存演示和控制台初始化结束后才发布阶段完成。
    airi_log("[AIRI_P3_READY] user=ring3 elf=static fork=copy exec=transaction wait=blocked"); // 完整内核进程路径结束并恢复后才发布。
    if (airi_storage_count() >= 0) // 超时隔离的故障镜像不宣称磁盘阶段可用。
        airi_log("[AIRI_P4_READY] files=ram pipes=blocking block=virtio journal=crc writable=%u", !airi_block_readonly()); // Linux实测状态另记，不由guest标记证明。
    if (airi_storage_count() >= 0 && !airi_block_readonly()) // 可写正常设备已完成用户异步生命周期。
        airi_log("[AIRI_P5_READY] wait=blocked sq=4 cq=4 registered=1 lifecycle=reclaimed"); // Linux实验实测另记，不由guest标记代替。
    airi_serial_write("airi> ");  // 输出首个串口提示符。
    airi_log("[AIRI_P6_AVAILABLE] shell=sh demo=demo input=UART"); // 只声明入口可用，完整演示由实际shell测试验证。
    for (;;) {  // 没有退出条件，内核持续轮询输入。
        int key = airi_input_poll();  // 从虚拟键盘或 UART 取得一个共享键值，0 表示暂无输入。
        if (!key) {  // 没有新键时不处理命令和重画。
```
<!-- after -->
```c
                 (unsigned long long)fb->pitch, (unsigned)fb->bpp);  // pitch 按字节输出，bpp 按位数输出。
    } else  // 未完成图形时仍发送串口原因并继续启动。
        airi_log("[DISPLAY_SERIAL] no frame produced; using serial output");  // 图形不适用时记录原因路径，后面仍进入串口命令循环。
    if (!diagnostics) { // 常驻启动不先进入monitor，也不宣称教学阶段演示已经运行。
        airi_log("[AIRI_SERVICE_RESOURCES_READY] input=UART heap=minimal storage=recovered framebuffer=%u", airi_framebuffer_valid(&info->framebuffer)); // 这里只表示内核资源就绪，服务可用还需真实用户提示符。
#ifdef AIRI_TEST_INIT_FAILURE // 故障镜像用已有截断ELF演示装载失败后的恢复。
        const char *initial = "broken"; // 不修改正常init二进制或磁盘内容。
#else // 普通常驻镜像从同一个初始目录启动用户监督者。
        const char *initial = "init"; // 首个用户PID自然从1开始，不绕过fork/exec/wait。
#endif // 两条路径都走完整资源准备与回滚。
        int result = airi_user_boot(initial); // 正常init不返回；只有装载失败或整个根会话停止才继续。
        airi_log("[SERVICE_RECOVERY] init=%s result=%d monitor=UART", initial, result); // 退出不能假装仍在提供服务。
        airi_serial_write("airi> "); // 退回可操作的串口监控台，保留故障诊断与显式run入口。
    } else { // 默认诊断目标保留各阶段实际演示完成标记。
        /* Written AFTER all drawing, so tests can take a completed screenshot. */
        airi_log("[AIRI_P1_READY] graphics=%u usable_bytes=%llu", graphics ? 1u : 0u,  // 首帧调用结束后才发布完成标记，布尔转换为明确的0/1无符号值。
                 (unsigned long long)usable);  // 完成记录附同一实际可用字节数。
        airi_log("[AIRI_P2_READY] exceptions=1 pages=1 own_cr3=1 heap=1"); // 所有内存演示和控制台初始化结束后才发布阶段完成。
        airi_log("[AIRI_P3_READY] user=ring3 elf=static fork=copy exec=transaction wait=blocked"); // 完整内核进程路径结束并恢复后才发布。
        if (airi_storage_count() >= 0) // 超时隔离的故障镜像不宣称磁盘阶段可用。
            airi_log("[AIRI_P4_READY] files=ram pipes=blocking block=virtio journal=crc writable=%u", !airi_block_readonly()); // Linux实测状态另记，不由guest标记证明。
        if (airi_storage_count() >= 0 && !airi_block_readonly()) // 可写正常设备已完成用户异步生命周期。
            airi_log("[AIRI_P5_READY] wait=blocked sq=4 cq=4 registered=1 lifecycle=reclaimed"); // Linux实验实测另记，不由guest标记代替。
        airi_serial_write("airi> ");  // 输出首个串口提示符。
        airi_log("[AIRI_P6_AVAILABLE] shell=sh demo=demo input=UART"); // 只声明入口可用，完整演示由实际shell测试验证。
    }
    for (;;) {  // 没有退出条件，内核持续轮询输入。
        int key = airi_input_poll();  // 从虚拟键盘或 UART 取得一个共享键值，0 表示暂无输入。
        if (!key) {  // 没有新键时不处理命令和重画。
```

<a id="service-profile"></a>
## 用显式构建配置选择启动策略

更新Makefile中的配置条件：no-framebuffer条件也接受service-no-framebuffer；三个服务名称都增加AIRI_BOOT_SERVICE，只有故障镜像再增加AIRI_TEST_INIT_FAILURE。filter从CASE中筛选这些明确名称，结果非空才进入该条件；默认normal不取得服务宏，旧九种故障配置也保留原含义。源码、链接器与固定QEMU硬件保持，产物分别进入build/service等目录。

在既有 no-framebuffer 条件处接入三个服务名称，并只给对应配置增加启动/故障宏。其他故障条件继续保持。

<!-- replace: Makefile -->
<!-- before -->
```make
CPPFLAGS += -DAIRI_GRAPHICAL_CONSOLE -Ifrontend/include # 为已选择的图形实现提供头文件搜索位置。
endif # 结束当前可选图形分支。
# Make条件只在no-framebuffer配置加入下方测试宏。
ifeq ($(CASE),no-framebuffer)
# +=追加预处理选项，让启动适配模拟缺少图形响应。
CPPFLAGS += -DAIRI_TEST_NO_FRAMEBUFFER
endif # 结束 no-framebuffer 的编译条件
# 缺内存响应配置采用独立构建目录和下面宏。
ifeq ($(CASE),missing-memory)
# 模拟必需内存地图缺失，内核应打印具体PANIC。
```
<!-- after -->
```make
CPPFLAGS += -DAIRI_GRAPHICAL_CONSOLE -Ifrontend/include # 为已选择的图形实现提供头文件搜索位置。
endif # 结束当前可选图形分支。
# Make条件只在no-framebuffer配置加入下方测试宏。
ifneq ($(filter no-framebuffer service-no-framebuffer,$(CASE)),)
# +=追加预处理选项，让启动适配模拟缺少图形响应。
CPPFLAGS += -DAIRI_TEST_NO_FRAMEBUFFER
endif # 结束 no-framebuffer 的编译条件
# 服务目标只改变启动策略，与旧九种诊断目标使用独立产物目录。
ifneq ($(filter service service-no-framebuffer service-init-failure,$(CASE)),)
CPPFLAGS += -DAIRI_BOOT_SERVICE # 初始化完成后直接进入用户init，不运行教学演示。
endif # 普通配置继续原诊断路径。
ifeq ($(CASE),service-init-failure)
CPPFLAGS += -DAIRI_TEST_INIT_FAILURE # 只在此故障镜像把初始程序换成截断ELF。
endif # 不修改正常init或用户资源接口。
# 缺内存响应配置采用独立构建目录和下面宏。
ifeq ($(CASE),missing-memory)
# 模拟必需内存地图缺失，内核应打印具体PANIC。
```

统一入口的PROFILES继续表示九种诊断启动，SERVICE_PROFILES另外列三个服务目标。argparse允许从两组选择；默认test完成原来的启动、输入、shell和init观察后，再分别构建三个服务镜像并驱动真实UART。图形test仍走诊断图形路线，service与--graphics在读取前端之前明确拒绝。

测试入口将在下面接入新的check_init参数，先更新`scripts/manage.py`。此处构建service只能证明镜像可生成，自动进入用户服务的证据由下一段真实运行获得。

独立声明服务配置集合，原九种诊断集合不变。

<!-- replace: scripts/manage.py -->
<!-- before -->
```python
from console_test import check_console  # 复用真实键盘和UART输入验证。

PROFILES=('normal','no-framebuffer','missing-memory','edit-experiment','trap-fatal','stack-guard','block-readonly','block-timeout','block-missing')  # 元组固定九种构建配置，CLI choices 与 test 遍历共用。
def run(cmd):  # 所有统一入口外部命令都从同一根目录执行。
    subprocess.run(cmd,cwd=ROOT,check=True)  # 非零状态转成异常传至入口。
def compilation_database():  # 从 Make 干跑输出收集每个C/汇编文件的实际编译参数。
```
<!-- after -->
```python
from console_test import check_console  # 复用真实键盘和UART输入验证。

PROFILES=('normal','no-framebuffer','missing-memory','edit-experiment','trap-fatal','stack-guard','block-readonly','block-timeout','block-missing')  # 元组固定九种构建配置，CLI choices 与 test 遍历共用。
SERVICE_PROFILES=('service','service-no-framebuffer','service-init-failure') # 三种UART常驻/恢复配置，不混入旧阶段完成标记检查。
def run(cmd):  # 所有统一入口外部命令都从同一根目录执行。
    subprocess.run(cmd,cwd=ROOT,check=True)  # 非零状态转成异常传至入口。
def compilation_database():  # 从 Make 干跑输出收集每个C/汇编文件的实际编译参数。
```

允许选择这些配置，并在读取前端之前拒绝 service 加 graphics。

<!-- replace: scripts/manage.py -->
<!-- before -->
```python
    parser.add_argument('action',choices=['doctor','build','run','test','debug','kernel-debug','linux-lab','clean','ui-preview','ui-test','docs-check','snapshots'])  # 位置参数限定动作集合，拼错动作不能落到默认运行QEMU。
    parser.add_argument('--app',choices=('terminal','files','editor','tasks','system','settings','viewer'),default='editor')  # 主机窗口默认Editor，只接受已知应用名。
    parser.add_argument('--state',choices=['normal','empty','loading','error','offline'],default='normal')  # 状态名限定到共享视图已有的五种状态。
    parser.add_argument('--profile',choices=PROFILES,default='normal')  # 镜像默认普通配置，只接受已发布配置。
    parser.add_argument('--phase',choices=('p1','p2','p3','p4','p5-threads','p5-epoll','p5-uring','p6'),default='p1') # Linux阶段显式选择，默认保持P1。
    parser.add_argument('--host',default='airi@ssh.airi-dynamic.top')   # 个人Linux主机可以显式替换，不涉及公司机器。
    parser.add_argument('--graphics', action='store_true', help='explicitly enable the optional desktop')  # 默认 False，不读取图形目录。
    args=parser.parse_args()      # 把进程参数转为Namespace，后续通过args.action等属性读取。
    if args.action=='snapshots':  # 只有导出动作需要读取Git教学提交链。
        from snapshots import generate  # 局部导入后续提供的快照模块。
        generate()  # 核对固定main提交链和旧目录保护后导出阶段源码。
```
<!-- after -->
```python
    parser.add_argument('action',choices=['doctor','build','run','test','debug','kernel-debug','linux-lab','clean','ui-preview','ui-test','docs-check','snapshots'])  # 位置参数限定动作集合，拼错动作不能落到默认运行QEMU。
    parser.add_argument('--app',choices=('terminal','files','editor','tasks','system','settings','viewer'),default='editor')  # 主机窗口默认Editor，只接受已知应用名。
    parser.add_argument('--state',choices=['normal','empty','loading','error','offline'],default='normal')  # 状态名限定到共享视图已有的五种状态。
    parser.add_argument('--profile',choices=PROFILES+SERVICE_PROFILES,default='normal')  # 镜像默认普通配置，只接受已发布配置。
    parser.add_argument('--phase',choices=('p1','p2','p3','p4','p5-threads','p5-epoll','p5-uring','p6'),default='p1') # Linux阶段显式选择，默认保持P1。
    parser.add_argument('--host',default='airi@ssh.airi-dynamic.top')   # 个人Linux主机可以显式替换，不涉及公司机器。
    parser.add_argument('--graphics', action='store_true', help='explicitly enable the optional desktop')  # 默认 False，不读取图形目录。
    args=parser.parse_args()      # 把进程参数转为Namespace，后续通过args.action等属性读取。
    if args.graphics and args.profile in SERVICE_PROFILES: # 常驻用户输入目前只接UART，不能进入不可见图形等待。
        parser.error('service profiles use UART; omit --graphics') # 在读取字体或前端以前给出实际修复方式。
    if args.action=='snapshots':  # 只有导出动作需要读取Git教学提交链。
        from snapshots import generate  # 局部导入后续提供的快照模块。
        generate()  # 核对固定main提交链和旧目录保护后导出阶段源码。
```

默认串口 test 完成原观察后，再逐个构建并运行三种服务检查。

<!-- replace: scripts/manage.py -->
<!-- before -->
```python
        if not args.graphics: # 当前用户shell只独占串口，图形终端尚无用户TTY接入。
            check_shell() # 同一核心构建实际执行用户解析、fork/exec/wait和慢设备I/O。
            check_init() # 同一监督者重启十二代shell，不结束会话清空资源。
        check_storage_restart('desktop-normal' if args.graphics else 'normal') # 正式日志双启动恢复，另以第三个guest验证慢设备CQ等待。
    else:  # 普通build/run/debug共用指定配置构建路径。
        build(args.profile)  # build/run/debug先构建调用者选择的配置。
```
<!-- after -->
```python
        if not args.graphics: # 当前用户shell只独占串口，图形终端尚无用户TTY接入。
            check_shell() # 同一核心构建实际执行用户解析、fork/exec/wait和慢设备I/O。
            check_init() # 同一监督者重启十二代shell，不结束会话清空资源。
            services=[] # 服务配置的可用条件不同于旧诊断READY，分别记录。
            for profile in SERVICE_PROFILES: # 两种正常无屏路径与一个失败恢复路径。
                build(profile) # 独立构建目录，不能借用normal已运行演示的状态。
                inspect_elf(profile) # 每个服务内核仍必须为无宿主依赖的静态ELF。
                services.append(check_init(profile)) # UART实际观察自动启动、重启或恢复。
            (ROOT/'artifacts/service-tests.json').write_text(json.dumps(services,indent=2)+'\n') # 只保存本轮实际结果。
        check_storage_restart('desktop-normal' if args.graphics else 'normal') # 正式日志双启动恢复，另以第三个guest验证慢设备CQ等待。
    else:  # 普通build/run/debug共用指定配置构建路径。
        build(args.profile)  # build/run/debug先构建调用者选择的配置。
```

<a id="service-observer"></a>
## 不发送 run init，观察两种自动启动与一次失败恢复

更新`scripts/init_test.py`。同一个驱动按profile选择镜像和独立证据目录。normal仍发送run init；service两种成功配置只从复位继续CPU，完全不发送该命令。资源标记到达后检查没有诊断完成记录、堆仍64KiB，并从真实BootInfo报告核对图形描述有无。

每个测试盘由该驱动排他创建，启动前保存8MiB内容摘要；资源就绪后摘要应保持，说明这次自动启动没有更改新实验盘内容。它不是主机掉电或物理盘持久性结论，也不表示设备没有读取。随后每种成功服务连续观察十二代shell，每代真正执行hello；前十一代logout都进入同一init的阻塞退避，最后一代保持活着，由宿主结束测试VM。

失败配置改为等待−8恢复记录，再从UART执行echo、mem和run hello。这样同时检查恢复监控台可操作、最小堆保持，以及用户资源在没有user_demo的情况下仍完整可用。每个等待沿用单调35秒截止，提前退出与PANIC失败，最后只回收本驱动的QEMU。

引入磁盘内容摘要计算。

<!-- replace: scripts/init_test.py -->
<!-- before -->
```python
"""Observe a real persistent user supervisor across repeated UART shell exits.""" # 不把宿主脚本当用户shell。
import json # 记录本轮真实结果。
from pathlib import Path # 短控制端点。
import re # 按实际PID关联等待和进展。
```
<!-- after -->
```python
"""Observe a real persistent user supervisor across repeated UART shell exits.""" # 不把宿主脚本当用户shell。
import hashlib # 只核对测试盘内容是否因自动启动改变，不代替掉电模型。
import json # 记录本轮真实结果。
from pathlib import Path # 短控制端点。
import re # 按实际PID关联等待和进展。
```

check_init 接受 profile，按启动方式选择自动/恢复路径和独立证据目录。

<!-- replace: scripts/init_test.py -->
<!-- before -->
```python
import time # 单调总截止。
from boot import command, qmp_execute # 固定硬件与真实限速。
from tools import ROOT # 不依赖调用目录。
def check_init(): # 默认串口主线，不读取frontend或字体。
    work=ROOT/'artifacts/init-normal'; work.mkdir(parents=True,exist_ok=True) # 本测试独占目录。
    result_path=work/'result.json'; result_path.write_text('{"passed":false}\n') # 旧成功立即撤销。
    disk=work/'disk.raw' # 仅测试盘可重置，不动交互run的正式实验盘。
    if disk.is_symlink() or (disk.exists() and (not disk.is_file() or disk.stat().st_size!=8*1024*1024)): # 未知对象不替换。
```
<!-- after -->
```python
import time # 单调总截止。
from boot import command, qmp_execute # 固定硬件与真实限速。
from tools import ROOT # 不依赖调用目录。
def check_init(profile='normal'): # 默认串口主线，不读取frontend或字体。
    automatic=profile.startswith('service') # 服务镜像直接启动init，normal仍从监控台显式启动。
    recovery=profile=='service-init-failure' # 本配置期待装载拒绝后回到monitor。
    work=ROOT/('artifacts/init-'+profile); work.mkdir(parents=True,exist_ok=True) # 本测试独占目录。
    result_path=work/'result.json'; result_path.write_text('{"passed":false}\n') # 旧成功立即撤销。
    disk=work/'disk.raw' # 仅测试盘可重置，不动交互run的正式实验盘。
    if disk.is_symlink() or (disk.exists() and (not disk.is_file() or disk.stat().st_size!=8*1024*1024)): # 未知对象不替换。
```

本次 QEMU 使用选定 profile，启动前保留同一新测试盘的摘要。

<!-- replace: scripts/init_test.py -->
<!-- before -->
```python
    disk.unlink(missing_ok=True) # 下一command排他创建空盘。
    with tempfile.TemporaryDirectory(prefix='airios-init-',dir='/tmp') as directory: # 本次端点随测试销毁。
        qmp=Path(directory)/'qmp.sock'; uart=Path(directory)/'uart.sock' # 控制与设备分开。
        cmd=command('normal',work); cmd[cmd.index('-serial')+1]=f'unix:{uart},server=on,wait=off' # 实际UART而非内存注入。
        cmd+=['-S','-qmp',f'unix:{qmp},server=on,wait=off'] # 连接UART后才继续CPU。
        (work/'command.json').write_text(json.dumps(cmd,indent=2)+'\n') # 固定本次QEMU参数。
        with (work/'qemu.log').open('w') as errors: # QEMU诊断单独保存。
            process=subprocess.Popen(cmd,stdout=errors,stderr=errors); wire=None; data=bytearray() # 保留真实累积字节。
            try: # 任何失败都回收自己的QEMU。
```
<!-- after -->
```python
    disk.unlink(missing_ok=True) # 下一command排他创建空盘。
    with tempfile.TemporaryDirectory(prefix='airios-init-',dir='/tmp') as directory: # 本次端点随测试销毁。
        qmp=Path(directory)/'qmp.sock'; uart=Path(directory)/'uart.sock' # 控制与设备分开。
        cmd=command(profile,work); cmd[cmd.index('-serial')+1]=f'unix:{uart},server=on,wait=off' # 实际UART而非内存注入。
        cmd+=['-S','-qmp',f'unix:{qmp},server=on,wait=off'] # 连接UART后才继续CPU。
        (work/'command.json').write_text(json.dumps(cmd,indent=2)+'\n') # 固定本次QEMU参数。
        initial_disk=hashlib.sha256(disk.read_bytes()).hexdigest() # command已排他创建本测试盘，启动前保存完整内容摘要。
        with (work/'qemu.log').open('w') as errors: # QEMU诊断单独保存。
            process=subprocess.Popen(cmd,stdout=errors,stderr=errors); wire=None; data=bytearray() # 保留真实累积字节。
            try: # 任何失败都回收自己的QEMU。
```

到达资源准备后检查自动启动或坏ELF恢复；normal仍显式发送run init。

<!-- replace: scripts/init_test.py -->
<!-- before -->
```python
                        if '[AIRI_PANIC]' in tail or process.poll() is not None: raise RuntimeError(f'Shell failed before {marker}') # 不把QEMU存活当成功。
                        time.sleep(.02) # 给真实设备与CPU推进。
                    raise RuntimeError(f'Shell timeout: {marker}; see {work}/serial.log') # 可定位失败证据。
                text=wait('[AIRI_P5_READY] ') # 只在完整内核启动后进入常驻模式。
                wire.sendall(b'run init\n'); text=wait('[INIT_START] service=sh input=UART restart_delay=100',len(text)) # 真正运行用户监督程序。
                supervisor=re.search(r'\[INIT_SESSION\] pid=(\d+) uart=exclusive',text) # 内核记录真实根身份，不假设PID1。
                assert supervisor # 该模式必须明确建立监督身份。
                pid=supervisor.group(1); shells=[]; offset=text.index('[INIT_START]') # 只看本模式之后的shell。
```
<!-- after -->
```python
                        if '[AIRI_PANIC]' in tail or process.poll() is not None: raise RuntimeError(f'Shell failed before {marker}') # 不把QEMU存活当成功。
                        time.sleep(.02) # 给真实设备与CPU推进。
                    raise RuntimeError(f'Shell timeout: {marker}; see {work}/serial.log') # 可定位失败证据。
                if automatic: # 不发送run init；服务必须从复位自行到达用户入口。
                    text=wait('[AIRI_SERVICE_RESOURCES_READY] ') # 此标记只声明内核资源，下面还需真实shell命令。
                    assert '[BOOT_MODE] name=service' in text and '[HEAP_READY] bytes=65536' in text # 没有借诊断模式扩堆或初始化。
                    assert all(marker not in text for marker in ('[TRAP_DEMO_OK]','[PAGES_DEMO_OK]','[HEAP_CAPACITY_OK]','[BLOCK_DEMO_OK]','[JOURNAL_DEMO_OK]','[USER_DEMO_OK]','[AIRI_P5_READY]')) # 跳过的演示不能发布完成。
                    expected_fb=0 if profile=='service-no-framebuffer' else 1 # 两个正常目标分别验证描述缺失与未绘图。
                    assert 'storage=recovered framebuffer='+str(expected_fb) in text # 从真实BootInfo而非宿主截图猜设备状态。
                    assert hashlib.sha256(disk.read_bytes()).hexdigest()==initial_disk # 自动启动没有更改新实验盘的内容。
                    if recovery: # 截断ELF必须明确拒绝，并保留可操作的串口恢复位置。
                        text=wait('[SERVICE_RECOVERY] init=broken result=-8 monitor=UART') # launch已验证失败资源恢复。
                        begin=len(text); wire.sendall(b'echo service-recovered\n'); text=wait('[CONSOLE_UPDATE] view=0 commands=1 page=0',begin) # 恢复路径仍能消费真实UART。
                        assert 'service-recovered' in text[begin:] # 不只是打印一个无法操作的提示符。
                        begin=len(text); wire.sendall(b'mem\n'); text=wait('[CONSOLE_UPDATE] view=0 commands=2 page=0',begin) # 失败装载没有留下大堆或页面借用。
                        assert 'kernel heap: bytes=65536' in text[begin:] # 服务保持最小缓存，未偷跑容量演示。
                        begin=len(text); wire.sendall(b'run hello\n'); text=wait('[CONSOLE_UPDATE] view=0 commands=3 page=0',begin) # 用户资源确实已初始化，不依赖user_demo。
                        assert 'program hello exited 0' in text[begin:] and '[ELF_USER_OK]' in text[begin:] # 故障之后仍可实际装载和回收。
                        result={'passed':True,'profile':profile,'automatic':True,'recovery':'bad ELF -8','monitor_echo_mem_and_user_hello':True,'boot_disk_unchanged':True} # 只记录此故障配置的实际能力。
                        result_path.write_text(json.dumps(result,indent=2)+'\n'); print('PASS service recovery: bad init ELF, UART echo/mem, independent user hello') # QEMU存活本身不足以通过。
                        return result # finally仍只结束自己的测试VM。
                    text=wait('[INIT_START] service=sh input=UART restart_delay=100') # 资源标记和此行可能在同一次读取到达。
                else: # 旧诊断目标行为保持，用户选择何时进入持续会话。
                    text=wait('[AIRI_P5_READY] ') # 完整演示已经结束。
                    wire.sendall(b'run init\n'); text=wait('[INIT_START] service=sh input=UART restart_delay=100',len(text)) # 显式启动同一个监督程序。
                supervisor=re.search(r'\[INIT_SESSION\] pid=(\d+) uart=exclusive',text) # 内核记录真实根身份，不假设PID1。
                assert supervisor # 该模式必须明确建立监督身份。
                pid=supervisor.group(1); shells=[]; offset=text.index('[INIT_START]') # 只看本模式之后的shell。
```

结果记录带上实际 profile 与启动方式，十二代关系和宿主终止仍按原方式核对。

<!-- replace: scripts/init_test.py -->
<!-- before -->
```python
                assert text.count('[INIT_SESSION]')==1 and 'program init exited' not in text # 不能偷偷结束会话再重启。
                assert not re.search(r'\[USER_EXIT\] pid='+pid+r' ',text) # 监督者本身也不能已退出、只留下孤儿shell继续运行。
                assert len(re.findall(r'\[WAIT_REGISTER\] pid='+pid+r' operation=sleep ticks=100',text))==11 # 每次logout都真正登记退避等待。
                result={'passed':True,'generations':12,'same_supervisor_pid':int(pid),'shell_pids':[int(n) for n in shells],'restart_sleep_ticks':100,'launch_remained_active':True,'harness_stopped_vm':True} # 观察结束由宿主停测试VM，不冒充guest关机。
                result_path.write_text(json.dumps(result,indent=2)+'\n'); print('PASS init: 12 UART shell generations, one supervisor, blocked restart delay, continued service') # 所有路径实际观察后发布。
                return result # 默认test可继续其他会话验证。
            finally: # 包括断言、超时和QEMU错误。
                if wire is not None: wire.close() # 只关闭自己的设备连接。
```
<!-- after -->
```python
                assert text.count('[INIT_SESSION]')==1 and 'program init exited' not in text # 不能偷偷结束会话再重启。
                assert not re.search(r'\[USER_EXIT\] pid='+pid+r' ',text) # 监督者本身也不能已退出、只留下孤儿shell继续运行。
                assert len(re.findall(r'\[WAIT_REGISTER\] pid='+pid+r' operation=sleep ticks=100',text))==11 # 每次logout都真正登记退避等待。
                result={'passed':True,'generations':12,'same_supervisor_pid':int(pid),'shell_pids':[int(n) for n in shells],'restart_sleep_ticks':100,'launch_remained_active':True,'harness_stopped_vm':True,'profile':profile,'automatic':automatic,'boot_disk_unchanged':automatic} # 观察结束由宿主停测试VM，不冒充guest关机。
                result_path.write_text(json.dumps(result,indent=2)+'\n'); print(f'PASS init/{profile}: 12 UART shell generations, one supervisor, blocked restart delay, continued service') # 所有路径实际观察后发布。
                return result # 默认test可继续其他会话验证。
            finally: # 包括断言、超时和QEMU错误。
                if wire is not None: wire.close() # 只关闭自己的设备连接。
```

<a id="service-observe"></a>
## 从复位直接到 sh，再 logout

完成上述文件后构建服务镜像：

<!-- run -->
```sh
./airios build --profile service # 只准备真实资源，再直接启动用户init的独立镜像。
```

Mac若默认python3仍是旧系统解释器，在命令前使用`PATH=/opt/homebrew/bin:$PATH`。从主机运行以下命令后，等待sh>；无需先在airi>输入run init。

```sh
./airios run --profile service # 自动进入UART用户服务；Ctrl-A后X由宿主结束QEMU。
```

```text
hello
exit
hello
```

本轮实际看到资源就绪后init为PID1，第一代shell为PID2；hello返回后shell仍接收输入。exit使第一代停止，init等待100个真实tick，再创建下一代并继续执行hello。关键记录形状如下，后续PID按实际命令数量增长：

```text
[BOOT_MODE] name=service
[HEAP_READY] bytes=65536 limit=1048576 chunks=1
[AIRI_SERVICE_RESOURCES_READY] input=UART heap=minimal storage=recovered framebuffer=1
[INIT_SESSION] pid=1 uart=exclusive
[INIT_START] service=sh input=UART restart_delay=100
[USER_SHELL_PROMPT]
```

使用`./airios run --profile service-no-framebuffer`走同一流程，资源记录中的framebuffer应为0；其余用户命令保持。要观察启动装载故障，运行故障镜像：

```sh
./airios run --profile service-init-failure # 只替换初始映像选择，不破坏正常init或实验盘。
```

```text
[ELF_REJECTED] ELF header is missing or truncated
[SERVICE_RECOVERY] init=broken result=-8 monitor=UART
airi>
```

在恢复监控台输入`echo service-recovered`、`mem`、`run hello`，应分别得到原样文本、65536字节堆与hello正常退出。它仍可显式run init；故障配置只控制开机选择，未修改目录中的真实init映像。

默认诊断路线与完整回归继续保留：

<!-- run -->
```sh
./airios test # 原诊断路径，加两种自动监督与坏初始ELF的恢复。
```

`./airios build --profile service --graphics`会以状态2退出并提示省略--graphics；这发生在字体检查之前。服务目前只消费UART，尚未将PS/2或图形Terminal接成用户TTY。

service不是已经完成开发板部署。它仍依赖当前x86-64 QEMU、OVMF/Limine、固定实验盘和轮询块后端，网络、C++服务、隧道与物理按键尚在后续。下一步沿块请求接入真实设备中断，把IRQ中的事件记录与普通服务栈中的完成收集分开。

