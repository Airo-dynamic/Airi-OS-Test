# 08 · 将真实引导响应复制到自己的接口

第 07 章已让 Limine 装入内核，经 `_start` 进入 C 并输出串口标记。第 05 章又有验证 AiriBootInfo 的函数，但它在 Mac 上检查的是两项构造数据。现在把两端接起来：从本次启动取得地图、映射与内核位置，由适配层转换成自己的结构，再交主函数验证与打印。

<a id="boot-requests"></a>

## 请求先成为 ELF 中的记录，引导器在 C 执行前写回

要取得本次地图，先在内核文件中留下引导器能识别的需要。`memory_request` 是 C 源码引用这份静态对象的名字，它的 `id` 保存四个固定的 64 位整数，用来标识内存地图请求。`LIMINE_MEMMAP_REQUEST_ID` 在编译前展开为这组初始化值；未指定的 `response` 初值为零，等待装载阶段填写。

第 06 章已经建立标记区和修订检查，现在把实际请求记录放进同一个区域。起止标记本身是固定字节序列，链接脚本按起点、请求、终点排列。Limine 装入 ELF 后，在内存映像中识别这些记录，为本次启动建立响应，再把响应地址写入 `memory_request.response`。随后才进入 `_start`，汇编建立自己的栈，C 入口开始读取。磁盘上的 ELF 仍保留原来的初值。

一项内存地图请求会得到以下形状。响应对象保存数量和条目指针数组，每个数组元素再指向一条区域描述：

```text
memory_request.response → 响应对象
                           entry_count = N
                           entries → [条目指针0, 条目指针1, …]
                                         ↓
                                   {base, length, type}
```

`entries[i]` 先取得第 i 条描述的指针，`entries[i]->base` 才读取物理起点。响应和条目指针使用启动环境中已经有效的虚拟地址，base 字段保存的是所描述范围的物理地址整数。内核还会用独立请求取得 HHDM 偏移和自身的物理、虚拟起点，才能把这些地址关系写入自有接口。

这里继续要求基础修订 6；它选择整套交接的行为。请求结构自己的 `revision` 则描述该种记录的修订，本次没有显式指定，随静态初始化取零。两者与第 02 章的引导程序发布版本 12.9.2 分别处理。页表模式请求也采用记录修订 0：按协议，mode 给出上界，下界采用 x86-64 的最低模式，两者在这里都是四级。结构中仍填写 max_mode/min_mode，但这两个显式字段要到请求修订 1 才参与选择。可选显示和引导器名称也各有自己的请求。

沿同一个 `memory_request` 看构建过程：`used` 使编译器输出对象，`section` 将它放入 `.limine_requests` 输入节，第 06 章的 `KEEP` 保留这些节并排好扫描范围。随后 `volatile` 约束 C 对外部写回状态的访问。`REQUEST` 宏只是共用属性的简写。下面先保留整组请求，再建立转换方法和读取入口。

**完整替换 `kernel/boot/limine.c`。** 先写请求声明，再继续追加转换与入口；旧入口已移除，全部追加结束后再构建。汇编入口和链接脚本保留。

<a id="code-kernel-boot-limine-c-1"></a>

<!-- write: kernel/boot/limine.c -->
```c
/* 请求声明采用 Limine 官方 0BSD 示例；外部协议只在本适配文件读取。 */
#include <airi/kernel.h>  // 取得串口、日志、停机及基础函数的共同声明
#include <limine.h>  // 读取锁定协议的类型与标识常量，头文件本身不执行引导

#define REQUEST __attribute__((used, section(".limine_requests")))  // 把保留属性与节选择合成仅供本适配文件使用的宏
__attribute__((used, section(".limine_requests_start"))) static volatile uint64_t request_start[] =  // 声明本次请求区起点，used 保留对象，volatile 保留协议读取
    LIMINE_REQUESTS_START_MARKER;  // 以固定协议字节初始化起点标记数组
REQUEST static volatile uint64_t base_revision[] = LIMINE_BASE_REVISION(6);  // 请求基础修订 6，引导器通过同一记录交回是否支持
REQUEST static volatile struct limine_memmap_request memory_request = {  // 请求内存地图响应，其 response 字段初始为零
    .id = LIMINE_MEMMAP_REQUEST_ID};  // 以协议固定 id 标识内存地图请求，未写字段置零
REQUEST static volatile struct limine_hhdm_request mapping_request = {.id = LIMINE_HHDM_REQUEST_ID};  // 请求高地址直接映射偏移，供后续解释物理与虚拟地址
REQUEST static volatile struct limine_executable_address_request address_request = {  // 请求内核装载的物理起点与虚拟起点
    .id = LIMINE_EXECUTABLE_ADDRESS_REQUEST_ID};  // 按可执行文件地址类别标识本请求
REQUEST static volatile struct limine_framebuffer_request framebuffer_request = {  // 请求可用显示画布描述，响应允许缺失
    .id = LIMINE_FRAMEBUFFER_REQUEST_ID};  // 固定 id 告诉引导器填帧缓冲响应
REQUEST static volatile struct limine_bootloader_info_request loader_request = {  // 请求引导器名称与版本文字，用于日志身份
    .id = LIMINE_BOOTLOADER_INFO_REQUEST_ID};  // 按引导器身份类别填写此请求 id
REQUEST static volatile struct limine_paging_mode_request paging_request = {  // 请求并限制虚拟地址翻译使用的页表层数
    .id = LIMINE_PAGING_MODE_REQUEST_ID,     // 协议标识说明这项请求针对页表模式
    .mode = LIMINE_PAGING_MODE_X86_64_4LVL,  // 修订0以首选四级作为模式上界，下界为架构最低四级
    .max_mode = LIMINE_PAGING_MODE_X86_64_4LVL,   // 显式上界字段从请求修订1起生效，本请求修订0忽略此字段
    .min_mode = LIMINE_PAGING_MODE_X86_64_4LVL};  // 显式下界也属于修订1，本次下界由协议的架构最低模式给出
__attribute__((used, section(".limine_requests_end"))) static volatile uint64_t request_end[] =  // 保留放到结束标记节的静态数组
    LIMINE_REQUESTS_END_MARKER;  // 固定结束字节告诉引导器请求区在哪里结束
```

请求声明已经给出外部数据的入口。接下来准备接收转换结果的静态 `boot_info`：它在整个内核运行期间存在，地图条目和名称保存到它自己的数组中。最后交给主函数的就是这份对象的地址，第 05 章的验证器和统计函数也继续读取自有格式。

内核跨度由链接标签 `__kernel_start/end` 给出。extern 数组声明只让 C 引用这些地址，不另创建数组；分别转 uintptr_t 再求整数差，得到包含 BSS 的虚拟跨度，不读取范围中的内容。两个标签不是同一 C 数组的元素，这里依赖固定目标的平坦地址表示，不套用一般数组内指针相减。

两套格式的用途编号分别定义。`memory_kind` 根据 Limine 的 `type` 选择 AiriOS 的 `kind`：例如引导器标为 usable 的范围转为 `AIRI_MEMORY_USABLE`，标为 bootloader reclaimable 的范围转为 `AIRI_MEMORY_BOOT_RECLAIM`。未识别的用途统一转为保留范围。先写出这些转换，入口逐项复制时才有明确规则。

**追加到 `kernel/boot/limine.c` 末尾：**

<a id="code-kernel-boot-limine-c-2"></a>

<!-- append: kernel/boot/limine.c -->
```c
extern char __kernel_start[], __kernel_end[];  // 声明链接脚本给出的两个地址标签，不在 C 中创建数组
static AiriBootInfo boot_info;  // 内核持有的静态结构初始全零，入口返回也不会失效

static uint32_t memory_kind(uint64_t raw) {  // 把外部协议用途值转换为内核自己的枚举编号
    switch (raw) {  // 按照本次原始用途值选择一个对应分支
    case LIMINE_MEMMAP_USABLE:  // 引导器将此范围归类为可用 RAM
        return AIRI_MEMORY_USABLE;  // 交回内核可用分类，后续统计只认识这个自有值
    case LIMINE_MEMMAP_ACPI_RECLAIMABLE:  // 匹配可回收 ACPI 表分类，但当前阶段尚未回收
        return AIRI_MEMORY_ACPI_RECLAIM;  // 转换为自有 ACPI 可回收分类，继续保留
    case LIMINE_MEMMAP_ACPI_NVS:  // 匹配固件要求保留的 ACPI 状态区域
        return AIRI_MEMORY_ACPI_NVS;  // 保留此专用用途，不改成可用 RAM
    case LIMINE_MEMMAP_BAD_MEMORY:  // 匹配已标坏的内存区域
        return AIRI_MEMORY_BAD;  // 自有分类明确不允许将此区域分配使用
    case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE:  // 匹配引导器占用且以后可回收的范围
        return AIRI_MEMORY_BOOT_RECLAIM;  // 当前继续保留；引导页回收须由明确策略安排
    case LIMINE_MEMMAP_EXECUTABLE_AND_MODULES:  // 匹配内核及引导模块的装载范围
        return AIRI_MEMORY_KERNEL;  // 按内核占用分类，不归入空闲 RAM
    case LIMINE_MEMMAP_FRAMEBUFFER:  // 匹配显示像素内存范围
        return AIRI_MEMORY_FRAMEBUFFER;  // 保留设备用途，后续单独绘图
    default:  // 外部未匹配的用途统一进入保守分支
        return AIRI_MEMORY_RESERVED;  // 未知范围继续保留，不猜测其可用性
    }
}
```

## 入口先核对能否读取，再复制必需描述

汇编进入 C 时，装载阶段已经完成，先初始化串口，使后续拒绝路径也能报告。读取 `memory_request.response` 后，局部 `memory` 借用那份响应。检查沿刚才的数据形状从外向内进行：确认响应指针，再确认 entries 数组和非零数量；数量不超过 128，才允许存入自有固定数组并缩窄为 `uint32_t`。

缺地图测试在这里把局部 memory 置空，随后走同一个拒绝分支。直接映射、内核位置和四级页表响应也都是自有描述的必需输入，缺少时立即报告并停机。这一轮检查决定能否开始转换；转换后的区域还要交给第 05 章的验证器，检查重叠、溢出和可用 RAM 等内容。

**追加入口的串口初始化与响应检查，后面继续接组装过程：**

<a id="code-kernel-boot-limine-c-3"></a>

<!-- append: kernel/boot/limine.c -->
```c
_Noreturn void airi_limine_start(void) {  // 汇编 call 进入的 C 入口，当前栈已满足调用约定
    airi_serial_init();  // 先配置串口，后续协议错误也能通过同一设备报告
    airi_log("[BOOT] x86-64 entry -> owned stack -> C adapter");  // 留下适配入口已运行的记录，此时还未宣告启动数据成功
    if (!LIMINE_BASE_REVISION_SUPPORTED(base_revision))  // 按外部写回结果核对基础修订支持
        airi_panic("Limine base revision 6 is unsupported");  // 版本不符立即报告并停机
    struct limine_memmap_response *memory = memory_request.response;  // 读取交接前由 Limine 填入的地图响应指针
#ifdef AIRI_TEST_NO_MEMMAP  // 只在缺地图故障 profile 的编译中包含下面赋值
    memory = NULL;  /* Fault injection: exercise a required response failure. */  // 故意让局部响应指针缺失，观察必需信息错误路径
#endif  // 结束当前条件编译范围，被排除的分支不会进入本次对象文件
    if (!memory || !memory->entries || !memory->entry_count)  // 从左到右短路，先确保响应存在再读成员
        airi_panic("required Limine memory map is missing");  // 地图、条目数组或有效数量缺失都不能继续适配
    if (memory->entry_count > AIRI_MAX_MEMORY_REGIONS)  // 检查外部数量不超内核固定数组容量后再缩窄类型
        airi_panic("memory map exceeds 128 regions; refusing truncation");  // 拒绝丢掉地图尾部条目，不形成残缺的成功结构
    if (!mapping_request.response || !address_request.response)  // 直接映射和内核装载地址都是自有结构需要的必需响应
        airi_panic("required kernel address / direct map response is missing");  // 缺少任一地址依据时停止，避免编造映射
    if (!paging_request.response || paging_request.response->mode != LIMINE_PAGING_MODE_X86_64_4LVL)  // 先确认响应存在，再用协议具名常量检查确为四级页表模式
        airi_panic("expected four-level x86-64 paging");  // 拒绝与当前地址模型不同的页表层数

```

<a id="boot-lifetimes"></a>

现在各份必需响应都可以读取，适配层开始形成自有副本。识别值、版本和 size 来自 `AiriBootInfo` 的格式约定；HHDM 偏移与两种内核起点来自各自响应；kernel_size 用前面的链接标签地址整数差求出。引导器名称与版本经限容格式化进入自己的 loader 数组，可选身份响应缺失时采用备用文字。

地图循环每次沿 `memory->entries[i]` 取得一个条目指针，先检查它非空，再读取 base、length、type。base 和 length 复制到 `boot_info.regions[i]`，type 经 `memory_kind` 转成自有 kind；指定成员初始化使保留字段置零。结果数组保存范围的描述，没有搬动这些范围内的 RAM。

**继续追加固定字段、名称与逐项地图复制：**

<!-- append: kernel/boot/limine.c -->
```c
    boot_info.magic = AIRI_BOOT_MAGIC;      // 写入自有结构识别值，之后由独立验证函数读取
    boot_info.version = AIRI_BOOT_VERSION;  // 标明内核与适配层共同使用的接口版本
    boot_info.size = sizeof(boot_info);     // 记录完整结构所占字节数，包含固定地图数组
    boot_info.direct_map_offset = mapping_request.response->offset;  // 复制外部映射偏移整数，不安装新页表
    boot_info.kernel_physical_base = address_request.response->physical_base;  // 复制内核在机器物理地址中的装载起点
    boot_info.kernel_virtual_base = address_request.response->virtual_base;  // 复制内核使用的虚拟起点，与链接布局一致
    boot_info.kernel_size = (uint64_t)((uintptr_t)__kernel_end - (uintptr_t)__kernel_start);  // 先把两个链接标签转成地址整数，再求含 BSS 的虚拟地址跨度，不做无关数组指针相减
    boot_info.region_count = (uint32_t)memory->entry_count;  // 128 上限已检查，再保存为自有 32 位有效条目数
    airi_snprintf(boot_info.loader, sizeof(boot_info.loader), "%s %s",  // 在自有 32 字节名称数组中合并引导器名与版本，自动限容
                  loader_request.response ? loader_request.response->name : "Limine",  // 有身份响应时读名称，缺少时用 Limine 备用文字
                  loader_request.response ? loader_request.response->version : "unknown");  // 同一次响应给出版本，缺少时用 unknown，终止零由格式化保证
    for (uint32_t i = 0; i < boot_info.region_count; ++i) {  // 逐项复制有效地图，原始条目和自有数组索引一致
        if (!memory->entries[i])  // 每个外部条目本身也是指针，读其字段前必须检查
            airi_panic("null memory map entry");  // 一个条目为空就不能形成完整地图，立即报告
        boot_info.regions[i] =  // 向静态自有数组当前槽位写入整个区域描述
            (struct airi_memory_region){.base = memory->entries[i]->base,  // 从条目指针所指对象复制物理起点
                                        .length = memory->entries[i]->length,  // 复制区域字节长度，稍后统一验证是否为空或溢出
                                        .kind = memory_kind(memory->entries[i]->type)};  // 把外部用途转成自有枚举；未指定保留字段置零
    }
```

必需副本已经形成，显示响应另行可选。代码依次检查响应、数量、数组和第一项指针，只适配RGB模式。no-framebuffer宏略过这整段，静态framebuffer保持零，必需地图仍可使用。

显示副本保存地址、尺寸、pitch 与颜色布局，像素仍留在原显示内存。`raw->address` 是已经映射的虚拟地址，按本次 `direct_map_offset` 减去偏移得到对应物理起点；以后的绘图仍使用虚拟地址字段。

复制完成后，地图数值由 `boot_info.regions` 自己持有，运行环境还有独立的保留责任：CPU 此时仍使用 Limine 建立的页表，显示访问也借用既有映射和原像素范围。因此引导器可回收区域继续保留，等内核有了明确的替换与回收过程再处理。

最后，`airi_kernel_main(&boot_info)` 按普通 C ABI 经 RDI 交出静态对象地址，仍处于同一内核调用链。主函数接下来验证的，是刚才这一轮真实响应形成的自有副本。

**最后追加可选显示与主函数交接，闭合入口：**

<!-- append: kernel/boot/limine.c -->
```c
#ifndef AIRI_TEST_NO_FRAMEBUFFER  // 缺画布故障版本完全略过图形适配，静态描述继续为零
    struct limine_framebuffer_response *response = framebuffer_request.response;  // 取得可选显示响应，允许不存在
    if (response && response->framebuffer_count && response->framebuffers &&  // 依次确认响应、数量和指针数组存在，短路避免空指针访问
        response->framebuffers[0]) {  // 第零个画布指针也必须存在，只选择这一份画布
        struct limine_framebuffer *raw = response->framebuffers[0];  // 借用引导器的第一份显示描述，不复制像素
        if (raw->memory_model == LIMINE_FRAMEBUFFER_RGB) {  // 只适配 RGB 颜色描述，其他模型保持图形不可用
            struct airi_framebuffer *fb = &boot_info.framebuffer;  // 取自有结构内嵌显示描述的地址，下面向其写字段
            fb->virtual_address = (uintptr_t)raw->address;  // 把已映射画布指针转换成可保存的整数地址
            fb->physical_address = fb->virtual_address - boot_info.direct_map_offset;  // 按此固定直接映射求物理起点；绘图仍使用虚拟地址
            fb->width = raw->width;    // 复制一行像素数，作为横坐标范围
            fb->height = raw->height;  // 复制像素行数，作为纵坐标范围
            fb->pitch = raw->pitch;    // 复制两行起点之间的字节距离，不假定等于宽度乘像素字节数
            fb->bpp = raw->bpp;  // 复制每像素位数，后续验证只支持 24 或 32
            fb->red_size = raw->red_mask_size;      // 复制红色通道位宽
            fb->red_shift = raw->red_mask_shift;    // 复制红色通道在像素整数中的低位起点
            fb->green_size = raw->green_mask_size;  // 复制绿色通道位宽
            fb->green_shift = raw->green_mask_shift;  // 复制绿色通道的低位起点
            fb->blue_size = raw->blue_mask_size;      // 复制蓝色通道位宽
            fb->blue_shift = raw->blue_mask_shift;    // 复制蓝色通道的低位起点
        }
    }
#endif  // 结束当前条件编译范围，被排除的分支不会进入本次对象文件
    airi_kernel_main(&boot_info);  // 借用静态自有对象进入主函数，地址按普通 C 调用约定传入 RDI
}
```

## 主函数验证自有格式，再消费同一份副本

info 借用静态 boot_info，不是入口临时数组。主函数先调用 airi_boot_validate，错误文字交 panic；通过以后才取 `&info->regions[i]` 读取当前条目。取得条目地址没有复制物理内存，也不会回到外部响应。

日志的 `%llu/%llx` 要求 unsigned long long，64 位字段先显式转为匹配类型，不能仅因位宽相同就忽略变参类型。内核位置、HHDM 和各区域因此都沿本次副本打印。

假设本次某条 usable 长度为 `0x4000`，汇总就加入 16384 字节；这沿用第 05 章的统计规则，真实条目数量和长度则取自当前机器。boot reclaim 另算，继续保留。这里得到引导分类总量，等后面建立页面分配器以后，才另外记录哪些可用页已经借给调用者。

**新建 `kernel/main.c`：**

<a id="code-kernel-main-c-1"></a>

<!-- write: kernel/main.c -->
```c
#include <airi/kernel.h>  // 取得串口、日志、停机及基础函数的共同声明


#ifndef AIRI_DEMO_MESSAGE  // 未由构建命令覆盖展示文字时使用下面默认值
#define AIRI_DEMO_MESSAGE "Hello from AiriOS P1"  // 默认日志输入，故障/修改 profile 可在编译时替换
#endif  // 结束当前条件编译范围，被排除的分支不会进入本次对象文件
_Noreturn void airi_kernel_main(const AiriBootInfo *info) {  // 接收适配层静态对象，成功后打印并停机，不返回
    const char *error = airi_boot_validate(info);  // 使用自有格式检查，先验证再遍历，成功为 NULL
    if (error)  // 错误指针非空表示验证失败，应显示其静态文字
        airi_panic(error);  // 沿统一串口通路报告首个验证错误并停机
    airi_log("[BOOT_INFO] version=%u regions=%u loader=%s", info->version, info->region_count,  // 打印格式身份与有效数量，字段类型和 %u 匹配
             info->loader);  // 显示自有名称数组，前面的验证已证明它有终止零
    airi_log("[KERNEL] physical=0x%llx virtual=0x%llx size=%llu",  // 分别以十六进制起点和十进制字节跨度打印内核位置
             (unsigned long long)info->kernel_physical_base,  // 转换成格式 %llx 所读取的 unsigned long long，避免仅按位宽猜类型
             (unsigned long long)info->kernel_virtual_base, (unsigned long long)info->kernel_size);  // 虚拟起点和跨度也按 %llx/%llu 的实参类型转换
    airi_log("[HHDM] offset=0x%llx", (unsigned long long)info->direct_map_offset);  // 记录本次直接映射偏移，说明虚拟与物理描述的关系
    for (uint32_t i = 0; i < info->region_count; ++i) {  // 只检查本次有效地图项，不把未使用槽位算进去
        const struct airi_memory_region *r = &info->regions[i];  // 取当前条目地址供只读检查，不复制整段内存
        airi_log("[MEMORY] index=%u base=0x%llx length=0x%llx kind=%s", i,  // 每个有效条目输出一次，索引对应当前自有数组位置
                 (unsigned long long)r->base, (unsigned long long)r->length,  // 把当前条目的物理起点和字节长度转为格式约定的类型
                 airi_memory_name(r->kind));  // 由自有枚举取得静态用途名称，不再访问外部协议常量
    }
    uint64_t usable = airi_memory_bytes(info, AIRI_MEMORY_USABLE);  // 汇总引导分类的可用量，这里没有实时页分配扣减
    airi_log("[MEMORY_SUMMARY] usable_bytes=%llu boot_reclaim_bytes=%llu",  // 将可用内存与仍保留的引导可回收内存分开报告
             (unsigned long long)usable,  // 把可用字节总量转成 %llu 对应的参数类型
             (unsigned long long)airi_memory_bytes(info, AIRI_MEMORY_BOOT_RECLAIM));  // 统计保留中的引导范围，当前不把它并入可用量
    airi_log("[LESSON] %s", AIRI_DEMO_MESSAGE);  // 发出当前构建配置的展示文字，供完整重启观察
    airi_log("[AIRI_LESSON_READY] boot info validated; serial only");  // 必需启动结构已验证且日志遍历完成，最后发送成功标记
    airi_halt();  // 当前入口到此停止，不再向调用者返回
}
```

适配入口现在调用主函数，主函数又引用 info.c 的验证和统计。把两份实现加入 SOURCES，链接器才取得这些定义；原串口、运行时和构建选项继续保留。取得显示描述不意味着本次入口已经绘图，输出仍走串口。

**在 Makefile 找到原 SOURCES 及前一行说明，用第二块替换第一块；其余规则保留：**

<a id="code-makefile-1"></a>

<!-- replace: Makefile -->
<!-- before -->
```makefile
# 列出当前入口路径依赖的四份 C 输入，不提前编入后续模块
SOURCES := kernel/boot/limine.c kernel/lib/base.c kernel/lib/runtime.c kernel/arch/x86_64/serial.c
```
<!-- after -->
```makefile
# 加入自有启动校验和主函数，链接器才能取得新入口调用的定义
SOURCES := kernel/boot/limine.c kernel/lib/base.c kernel/lib/runtime.c kernel/arch/x86_64/serial.c kernel/boot/info.c kernel/main.c
```

重新构建 ELF 和介质，再启动一次机器。观察脚本仍等待完整教学标记，标记之前的地址与地图则来自这次 Limine 响应。不能拿 05 的两项主机夹具或先前启动的日志代替本次数据。

命令块在独立的 `sh -eu` 中顺序执行，任一构建或检查失败都会结束这次 shell，后面的命令不会消费旧产物。外层 SH 与内层 PY 各自关闭完整程序，保留它们所在的独占行。

**在学习目录根目录运行：**

<!-- run -->
```sh
sh -eu <<'SH'  # 本次构建成功后才继续检查或启动；失败时结束独立shell
PYTHONPATH=scripts python3 - <<'PY'  # 标准输入中到下一独立 PY 行之前的内容交给 Python，不由 shell 展开
import subprocess  # 启动主机工具，取得退出状态或保存进程对象
from tools import make_command  # 取得指向当前工具和目标的 Make 参数列表
subprocess.run(make_command(target='all'), check=True)  # 按当前源文件生成 ELF 与 ISO；任一步非零退出停止此观察
PY
python3 scripts/first_boot.py  # 启动本轮虚拟机并等待完整串口标记，成功或失败后都回收进程
SH
```

应依次出现 BOOT_INFO、KERNEL、HHDM、各项 MEMORY、MEMORY_SUMMARY 与完成标记，固件/引导器自身还可能先输出 BOOT 相关文字。区域数和地址由本次固件布局决定，不强行要求等于主机夹具。

主函数验证并打印后仍停机。现在内核可以说明本次启动的内存，却不能等待用户再问一条命令；下一步保存输入和日志状态，让启动后的查询能够持续进行。
