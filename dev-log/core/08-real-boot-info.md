# 08 · 将真实引导响应复制到自己的接口

串口入口已经启动，内存检查却还使用主机构造的数据。现在让 Limine 适配层请求真实内存图、映射和内核地址，将响应复制进第 05 章的 AiriBootInfo，再交给内核主函数验证与打印。

第 05 章的两个区域只用于验证结构，不能用于实际机器的内存判断。现在取本次启动的引导响应：内存条目、内核位置、直接映射和显示描述都来自 Limine，内核再形成自己的副本。日志会沿“外部响应 → 自有结构 → 验证 → 打印”读取同一份数据。

<a id="boot-requests"></a>

## 将外部响应复制进内核数据

request 是编译进 ELF 的一份请求记录，id 按协议指定需要哪类信息；response 是由引导器在跳入内核之前填入的指针。这个请求没有网络发送，也没有内核线程等回应，它是文件中的数据与启动前写入之间的协议。volatile 使代码按此约定读取可能被外部填写的值，used 与 KEEP 使构建时不删除请求。

响应指向引导器管理的内存，适配层只在当前阶段读取它们；内核自己的 static boot_info 保存复制后的字段和数组，持续存活。

内存条目逐项复制 base、length 和类型，loader 名称复制进固定字符数组。帧缓冲只复制映射描述，像素仍位于引导器建立的可写映射。当前没有回收引导页，因此这块映射继续有效。

**完整替换 `kernel/boot/limine.c`：**

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
    .mode = LIMINE_PAGING_MODE_X86_64_4LVL,  // 首选四级模式，本项目按该模式理解地址
    .max_mode = LIMINE_PAGING_MODE_X86_64_4LVL,   // 不接受超过四级的模式，保护固定布局前提
    .min_mode = LIMINE_PAGING_MODE_X86_64_4LVL};  // 不接受低于四级的模式，三项共同限制为四级
__attribute__((used, section(".limine_requests_end"))) static volatile uint64_t request_end[] =  // 保留放到结束标记节的静态数组
    LIMINE_REQUESTS_END_MARKER;  // 固定结束字节告诉引导器请求区在哪里结束
```

链接脚本中的 `__kernel_start/end` 以 extern 数组名声明，代码只取其地址，把两个标签各自转换为 uintptr_t 地址整数后相减，给出含 BSS 的内核虚拟地址跨度。它们不是同一个 C 数组的两项，不能用一般数组内指针减法作为依据。这里依赖固定目标的平坦地址表示，整数差仍不访问任何内核字节。这里不会另创建两份 C 数组。

内存类型转换函数 memory_kind 将 Limine 常量对应到内核枚举，未知类型保守保留。转换后其他模块只需处理自己的类型值。

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
        return AIRI_MEMORY_BOOT_RECLAIM;  // 当前仍保留此范围，P2 才安排回收
    case LIMINE_MEMMAP_EXECUTABLE_AND_MODULES:  // 匹配内核及引导模块的装载范围
        return AIRI_MEMORY_KERNEL;  // 按内核占用分类，不归入空闲 RAM
    case LIMINE_MEMMAP_FRAMEBUFFER:  // 匹配显示像素内存范围
        return AIRI_MEMORY_FRAMEBUFFER;  // 保留设备用途，后续单独绘图
    default:  // 外部未匹配的用途统一进入保守分支
        return AIRI_MEMORY_RESERVED;  // 未知范围继续保留，不猜测其可用性
    }
}
```

请求和转换函数已齐备，入口按顺序组装 boot_info。先初始化串口，让后续失败能留下文本；缺内存图、条目数组或映射，条目超过 128，或页表模式不符合四级要求时，立即报告并停机。测试宏可以将已经读取的局部 memory 指针置空，模拟缺失响应。

<a id="boot-lifetimes"></a>

复制并不都意味着相同的所有权变化。regions 每项的三个数值写进 boot_info 自己的数组，loader 的字节也写进自己的字符数组，之后不再依赖原条目。framebuffer 地址只是一个数值副本，两个描述仍指向同一像素区域；绘图能用它，是因为引导映射持续保留。

四级页表指当前虚拟地址翻译经过四层索引记录。这一章只要求引导器采用预期模式，尚未逐层建立或修改页表；拒绝其他模式是在保护当前固定地址解释的前提。

`memory->entries[i]` 取得第 i 个条目指针，`->base` 再读取该条目的起点。检查条目数后才转为 `uint32_t`。指定成员初始化 `.field=value` 设置各字段，未指定者置零；可选 loader 响应缺少时使用备用名称。

图形响应依次检查指针、数量和 RGB 模式。address 已是虚拟映射地址，按当前直接映射偏移计算物理地址，尺寸与颜色字段一并复制。完成后调用 `airi_kernel_main(&boot_info)`，指针经普通 C 调用的 RDI 传入，执行仍在同一个内核中。

**追加到 `kernel/boot/limine.c` 末尾：**

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

## 用同一结构校验并打印

主函数先调用 boot_validate，失败则将返回文本交给 panic；成功才遍历区域。打印 64 位字段时转成 unsigned long long，与 `%llu/%llx` 匹配。

主函数第一整数/指针参数按前文的 C 调用约定由 RDI 传入，info 借用适配层静态 boot_info。静态存储期表示对象在整个内核运行期间都有存储，不会像入口的普通局部数组那样在返回后失效。验证成功后取 `&info->regions[i]`，只是当前条目的地址，不再复制区域内容。

例如某条区域 length 为 0x4000，统计函数按其 kind 决定是否将 16384 加进 usable。统计来自类型分类，尚未减去后续分配给任务的页，也没有测量进程实际驻留内存（RSS）。

usable 和 boot reclaim 分别汇总：前者是引导内存分类中的可用量，后者暂时保留。页分配器尚未建立，所以此处没有实时剩余页数。

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

SOURCES 加入 info.c 与 main.c，链接器才能取得验证和主函数。其余规则保持已有构建方式，本次入口仍只输出串口。

**完整替换 `Makefile`：**

<a id="code-makefile-1"></a>

<!-- write: Makefile -->
```makefile
# C 编译器默认名称；命令行 CC=路径可以覆盖此赋值
CC := clang
# ELF 链接器默认名称，工具脚本会传入实际路径
LD := ld.lld
# 主机观察程序的编译器名称，与内核目标工具分开
HOST_CC := cc
# 变量尚未设置时采用 normal，命令行可选择独立故障目录
PROFILE ?= normal
# 立即将本次 profile 展开为产物目录，防止不同版本共用对象
OUT := build/$(PROFILE)
# 加入自有启动校验和主函数，链接器才能取得新入口调用的定义
SOURCES := kernel/boot/limine.c kernel/lib/base.c kernel/lib/runtime.c kernel/arch/x86_64/serial.c kernel/boot/info.c kernel/main.c
# 将每个 .c 路径换成构建目录中的 .o，再加汇编入口对象
OBJECTS := $(patsubst %.c,$(OUT)/%.o,$(SOURCES)) $(OUT)/kernel/arch/x86_64/entry.o
# 编译 x86-64 ELF，选择 C11、优化及调试信息，并把常见警告作为错误。
# 续行中的 freestanding/builtin/stack-protector 选项选择自有运行环境；pic/pie 关闭地址无关产物。
# 最后一行禁用 red zone/SIMD，采用高地址代码模型，并生成 .d 头文件依赖。
CFLAGS := --target=x86_64-unknown-none-elf -std=c11 -O2 -g -Wall -Wextra -Werror \
          -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-pie \
          -mno-red-zone -mgeneral-regs-only -mcmodel=kernel -MMD -MP
# 为预处理器指定自有头和锁定协议头的搜索路径
CPPFLAGS := -Iinclude -Ithird_party
ifeq ($(PROFILE),no-framebuffer) # 当 profile 名称是 no-framebuffer 时加入缺画布故障宏
# 在预处理阶段定义故障宏，让适配层略过可选图形响应
CPPFLAGS += -DAIRI_TEST_NO_FRAMEBUFFER
endif # 结束当前 Make 条件范围，下面规则恢复对全部 profile 生效
ifeq ($(PROFILE),missing-memory) # 缺地图 profile 采用下一项预处理定义
# 使入口局部地图指针变空，验证必需信息错误路径
CPPFLAGS += -DAIRI_TEST_NO_MEMMAP
endif # 结束当前 Make 条件范围，下面规则恢复对全部 profile 生效
ifeq ($(PROFILE),edit-experiment) # 改动展示 profile 采用下面两条文字覆盖定义
# 向编译器传入两个字符串宏；单引号保护里面的双引号，使它们成为 C 字面量。
CPPFLAGS += -DAIRI_PANEL_TITLE='"LOCAL / MAINTENANCE CONSOLE"' \
            -DAIRI_DEMO_MESSAGE='"I changed the output and rebooted"'
endif # 结束当前 Make 条件范围，下面规则恢复对全部 profile 生效

.PHONY: all kernel image host-test clean # 这些名字表示动作而非同名文件，避免同名文件使动作被误判已完成
all: image # 默认目标先取得 image，也就需要完成其 ELF 前置依赖
kernel: $(OUT)/airios.elf # 请求 kernel 动作时只生成本 profile 的 ELF
$(OUT)/%.o: %.c # 模式规则将任意匹配的 C 源文件编译为同路径对象文件
	@mkdir -p $(dir $@) # 自动变量 $@ 是对象路径，dir 取父目录；@ 隐藏命令回显
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@ # $< 为第一个依赖源码，-c 只编译，-o 指定对象目标
$(OUT)/%.o: %.S # 预处理汇编使用同一对象目录和编译驱动
	@mkdir -p $(dir $@) # 自动变量 $@ 是对象路径，dir 取父目录；@ 隐藏命令回显
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@ # $< 为第一个依赖源码，-c 只编译，-o 指定对象目标
$(OUT)/airios.elf: $(OBJECTS) kernel/linker.ld # 全部对象和链接脚本变化都会使 ELF 需要重新生成
	# 按 x86-64 ELF 静态链接，不加入宿主启动库；最大页大小采用 4096 字节。
	# 续行再禁用可执行栈，使用当前链接脚本，把全部对象写成此目标 ELF。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 \
	    -z noexecstack -T kernel/linker.ld $(OBJECTS) -o $@
image: $(OUT)/airios.elf # 先有当前 ELF，才执行下一行镜像打包
	python3 scripts/image.py $(PROFILE) # 把 profile 作为独立脚本参数交给打包流程
-include $(OBJECTS:.o=.d) # 替换对象后缀读取头依赖文件；开头 - 允许首次尚无 .d
```

重新生成镜像并运行启动观察。标记之前的内存记录来自这次 Limine 响应，最后仍以完整教学标记判断成功。

**在学习目录根目录运行：**

<!-- run -->
```sh
PYTHONPATH=scripts python3 - <<'PY'  # 标准输入中到下一独立 PY 行之前的内容交给 Python，不由 shell 展开
import subprocess  # 启动主机工具，取得退出状态或保存进程对象
from tools import make_command  # 取得指向当前工具和目标的 Make 参数列表
subprocess.run(make_command(target='all'), check=True)  # 按当前源文件生成 ELF 与 ISO；任一步非零退出停止此观察
PY
python3 scripts/first_boot.py  # 启动本轮虚拟机并等待完整串口标记，成功或失败后都回收进程
```

串口依次出现 BOOT、BOOT_INFO、KERNEL、HHDM、各项 MEMORY、MEMORY_SUMMARY 和完成标记。区域数与地址随本次固件布局确定。主函数打印后停机；要在启动后继续查询，需要输入缓冲和命令分派。
