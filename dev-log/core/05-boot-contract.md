# 05 · 把启动信息变成可检查的数据

日志已经能把整数变成文字。接下来要回答“这台虚拟电脑有哪些内存能供内核使用”。QEMU 配置 256 MiB 只是整台机器的容量；固件、引导器、内核和设备已经占用了其中一些范围，不能把全部地址都当作空闲 RAM。

引导器会交出一张内存地图，每项记录一段物理地址的起点、长度和用途。内核把这张地图转换为自己的 AiriBootInfo。以后换引导器时，只改变转换处，日志、控制台与分配器仍读取同一结构。当前先在主机程序中检查两个相邻区域，暂不读取真实机器数据。

## 地址与区域用途

<a id="memory-addresses"></a>

物理地址给机器内存或设备范围编号；虚拟地址是 C 指针当前使用的地址。CPU 通过一组称为页表的映射记录，把虚拟地址翻译成物理地址，再访问目标。64 位寄存器能保存很大的地址数值，不代表该数值已经映射到可访问内存。

例如本章观察数据用物理起点 0x1000、直接映射偏移 0xffff800000000000。对应虚拟地址为两者之和 0xffff800000001000。直接映射是把一组物理范围按固定偏移映射到虚拟地址的方式，HHDM 是“高地址直接映射”的英文简称；这项映射由引导器建立。主机观察程序只检查这些整数格式，不会尝试解引用这个地址。

一项 regions 记录只描述范围，并不包含范围内所有字节。usable 表示引导分类认为可以使用的 RAM；kernel、framebuffer 和 reserved 等区域各有当前用途。ACPI 是固件描述机器硬件与电源信息的机制，ACPI reclaim/NVS 两类区域也先保留。boot reclaim 表示引导器留下、以后可由内核回收的区域；当前没有执行回收，所以它仍不能交给分配器。

## 保存地图和可选显示描述

<a id="boot-layout"></a>

magic 是识别本格式的固定整数，version 标明版本，size 保存整个结构大小。引导适配与内核都需按同样字段位置读写，这组二进制约定称为 ABI。enum 为每种内存用途给出具名整数，uint64_t 固定地址与长度的位宽，reserved 当前置零。

regions 有 128 个槽位，region_count 才是本次有效条目数。内核位置分别保存物理起点、链接时的虚拟起点和跨度；loader 保存引导器名称。固定数组让内核能直接持有这些数据，不依赖引导器继续维护条目指针。

机器也可能提供显示设备使用的内存。地图必须把这块范围留给设备，串口查询还可以报告它的尺寸；是否绘图由之后选择的消费者决定。AiriBootInfo 因此同时保留一份可选显示描述，缺少它不影响内存地图验证和串口启动。

帧缓冲是保存整幅屏幕像素的可写内存范围，显示设备读取这些像素形成画面。描述中的 width/height 是像素数量，pitch 是相邻两行起点的字节距离，bpp 是每像素位数；24 bpp 用三字节，32 bpp 用四字节。颜色分为红、绿、蓝三个通道，red_size/red_shift 表示红色占多少位、从第几位开始。例如八位红色从第 16 位开始，绿色从第 8 位开始，蓝色从第 0 位开始。

framebuffer 结构复制的是地址和尺寸，像素仍在另一块内存中。图形支线使用已有映射的 virtual_address 写像素；physical_address 只描述物理位置。当前主机程序只检查数值，不访问这块像素内存。

**新建 `include/airi/boot.h`：**

<a id="code-include-airi-boot-h-1"></a>

<!-- write: include/airi/boot.h -->
```c
#ifndef AIRI_BOOT_H   // 首次包含时展开启动接口，避免同一翻译单元重复类型定义
#define AIRI_BOOT_H   // 标记此头已处理，由末尾 endif 关闭包含保护
#include <stdbool.h>  // 提供 bool、true、false，验证函数使用具名真假值
#include <stddef.h>   // 取得 size_t、ptrdiff_t、NULL 等工具链定义
#include <stdint.h>   // 取得固定宽度整数及 uintptr_t，不会因此链接宿主 libc

#define AIRI_BOOT_MAGIC UINT64_C(0x41495249424f4f54)  // 固定识别值；UINT64_C 使字面量采用至少 64 位的适当类型
#define AIRI_BOOT_VERSION 1  // 当前自有交接格式版本，双方按此版本解释字段
#define AIRI_MAX_MEMORY_REGIONS 128  // 固定地图容量，超过此值拒绝交接而非静默截断
enum airi_memory_kind {  // 按声明顺序从零编号，为每种物理区域用途给出名字
    AIRI_MEMORY_USABLE,    // 引导分类允许使用的 RAM；此时尚未分配给任务
    AIRI_MEMORY_RESERVED,  // 不能按可用 RAM 处理的保留范围
    AIRI_MEMORY_ACPI_RECLAIM,  // 固件 ACPI 表所在范围，当前仍保留，暂不回收
    AIRI_MEMORY_ACPI_NVS,      // ACPI 要保留的状态范围，不作为普通可分配内存
    AIRI_MEMORY_BAD,  // 被标为不可用的内存范围
    AIRI_MEMORY_BOOT_RECLAIM,  // 引导器使用的范围，当前阶段继续保留
    AIRI_MEMORY_KERNEL,      // 内核及相关装载内容占用的范围
    AIRI_MEMORY_FRAMEBUFFER  // 显示设备像素范围，不能视为普通空闲 RAM
};
struct airi_memory_region {  // 一项只描述一段区域，不包含该区域内所有字节
    uint64_t base;  /* Physical byte address. */  // 物理起点，单位是字节地址，不是可直接解引用的 C 指针
    uint64_t length;    // 该区域长度，单位为字节，右端为 base+length
    uint32_t kind;      // 保存上面枚举的用途值，决定统计或保留方式
    uint32_t reserved;  // 为固定布局预留的位置，当前保持零
};
struct airi_framebuffer {  // 描述像素内存的地址、行距和颜色布局，像素本身仍在别处
    uint64_t virtual_address;  /* Mapped writable address, NOT a physical address. */  // 已映射可写的虚拟地址，绘图代码转成指针后使用
    uint64_t physical_address;      // 对应物理起点，仅作描述，不能据此直接写像素
    uint64_t width, height, pitch;  // 宽高为像素数，pitch 是两行起点间字节距离
    uint16_t bpp;  // 每像素位数，本实现接受 24 或 32
    uint8_t red_size, red_shift, green_size, green_shift, blue_size, blue_shift;  // 各色通道的位宽与低位起点，例如红色八位从第 16 位开始
};
/* 固定字段描述由内核持有，地图元数据不借用固件指针。
 * 引导器范围当前继续保留，直到后续明确回收；设备映射不能视为可用 RAM。 */
typedef struct AiriBootInfo {  // 为整份固定字段交接结构同时定义 AiriBootInfo 类型别名
    uint64_t magic;  // 验证者据此识别 AiriOS 自有交接格式
    uint32_t version, size;      // 版本和完整结构字节数，防止以错误布局读取后续字段
    uint64_t direct_map_offset;  // 直接映射虚拟地址减物理地址所得偏移
    uint64_t kernel_physical_base, kernel_virtual_base, kernel_size;  // 内核物理起点、链接虚拟起点及含 BSS 的总跨度
    uint32_t region_count, reserved;  // 本次有效地图项数和置零保留字段，项数不等于数组总容量
    char loader[32];  // 自有字符数组保存引导器名称，内容必须在 32 字节内以 NUL 结束
    struct airi_framebuffer framebuffer;  // 内嵌复制显示描述，地址字段仍指向原像素范围
    struct airi_memory_region regions[AIRI_MAX_MEMORY_REGIONS];  // 内嵌固定数组持有地图副本，只使用 region_count 项
} AiriBootInfo;  // 之后可以直接用别名 AiriBootInfo 声明对象

const char *airi_boot_validate(const AiriBootInfo *info);  // 只读验证，成功返回 NULL，失败返回不需释放的静态错误文字
bool airi_framebuffer_valid(const struct airi_framebuffer *fb);  // 只判断当前描述是否符合可支持布局，不安装页表映射
uint64_t airi_memory_bytes(const AiriBootInfo *info, uint32_t kind);  // 对已经验证的地图汇总指定用途的区域长度
const char *airi_memory_name(uint32_t kind);  // 把用途值转为日志能显示的静态名称
_Noreturn void airi_kernel_main(const AiriBootInfo *info);  // 内核主函数借用启动结构，正常路径也不返回引导适配层
#endif  // 结束头文件包含保护，后续使用同一公开声明
```

读取区域之前先调用 `airi_boot_validate`。返回 NULL 表示结构符合要求，返回其他指针则指向静态错误文本，调用者无需释放。参数 `const AiriBootInfo *` 限制验证函数只读输入；失败时保留原值，便于报告具体原因。

验证按“先确认描述能安全计算，再允许使用”的顺序进行。虚拟地址、尺寸、像素位数与行跨度必须一致，否则按画布描述写一个矩形可能写到错误位置。帧缓冲验证单独返回 bool，后面的启动路径允许图形缺失而仍保留串口。

例如 width=3、24 bpp 时，一行至少需要九字节，pitch 可以是 12，却不能是 8。红色八位从第 16 位开始，掩码为 0x00ff0000；若绿色也占这些位，写绿色就同时改变红色，因此 occupied 检查通道是否相交。

首先加入帧缓冲检查。每行 pitch 必须装得下 width×每像素字节数，整幅 height×pitch 以及地址加跨度都需要防溢出。颜色掩码用 `((1<<size)-1)<<shift` 计算，occupied 累积已占用位；掩码相交说明两个颜色通道占了同一位。当前支持 24 和 32 bpp。

**新建 `kernel/boot/info.c`：**

<a id="code-kernel-boot-info-c-1"></a>

<!-- write: kernel/boot/info.c -->
```c
#include <airi/boot.h>  // 使用双方共同的字段布局和枚举常量

bool airi_framebuffer_valid(const struct airi_framebuffer *fb) {  // 调用方提供有效描述指针；这里检验地址数值、尺寸及颜色范围
    if (!fb->virtual_address || !fb->width || !fb->height)  // 任一必需地址或尺寸为零就无法形成可用画布
        return false;  // 当前条件不符合支持的画布描述，报告不可绘制
    if (fb->width > INT32_MAX || fb->height > INT32_MAX)  // 绘图坐标使用32位有符号整数，尺寸必须能无损转入
        return false;  // 拒绝不能表示为绘图坐标的巨大尺寸，不发生截断转换
    if (fb->bpp != 24 && fb->bpp != 32)  // 必须是两个支持值之一；其他像素格式先拒绝
        return false;  // 当前条件不符合支持的画布描述，报告不可绘制
    uint64_t bytes = fb->bpp / 8;  // 把每像素位数换成字节数，即 3 或 4
    if (fb->width > UINT64_MAX / bytes || fb->pitch < fb->width * bytes)  // 先排除乘法溢出，再检查行距装得下一行像素
        return false;  // 当前条件不符合支持的画布描述，报告不可绘制
    if (fb->height > UINT64_MAX / fb->pitch)  // 前面已保证 pitch 非零；整幅行距乘高度必须可表示
        return false;  // 当前条件不符合支持的画布描述，报告不可绘制
    uint64_t span = fb->height * fb->pitch;  // 在无溢出的前提下求整幅像素范围的字节跨度
    if (fb->virtual_address > UINT64_MAX - span)  // 起点加跨度不能回绕，否则最后一行地址会错
        return false;  // 当前条件不符合支持的画布描述，报告不可绘制
    const uint8_t sizes[] = {fb->red_size, fb->green_size, fb->blue_size};  // 按红、绿、蓝顺序收集各通道位宽
    const uint8_t shifts[] = {fb->red_shift, fb->green_shift, fb->blue_shift};  // 按相同次序收集各通道的低位起点
    uint64_t occupied = 0;  // 初始没有颜色位被占用，后续将已接受掩码按位或累积
    for (unsigned i = 0; i < 3; ++i) {  // 逐项检验三个颜色通道，sizes 与 shifts 使用同一索引
        if (!sizes[i] || sizes[i] > 8 || shifts[i] + sizes[i] > fb->bpp)  // 位宽必须为 1..8，且整个通道都落在一个像素范围内
            return false;  // 当前条件不符合支持的画布描述，报告不可绘制
        uint64_t mask = ((UINT64_C(1) << sizes[i]) - 1) << shifts[i];  // 先造出 size 个连续低位 1，再移到该颜色的起点
        if (mask & occupied)  // 按位与非零说明本通道和此前通道共用了位
            return false;  // 当前条件不符合支持的画布描述，报告不可绘制
        occupied |= mask;  // 将本通道位加入已占用集合，供后续通道比较
    }
    return true;  // 地址、尺寸及三通道检查全部通过，描述可供当前绘图实现使用
}
```

帧缓冲描述有了检查方法，再检查整个启动结构。内存范围采用半开区间 `[base, base+length)`，包括起点，不包括右端。previous_end 保存上一项右端，下一项起点小于它便说明重叠或未排序。

右端采用“不包含在区域内”的地址。`[0x1000,0x2000)` 有 4096 字节，最后一个字节在 0x1fff；下一段从 0x2000 开始不会重叠。排序检查使逐项统计和后来选页不需要猜区域先后。

无符号整数超过 UINT64_MAX 会回绕成较小值。若先算 base+length 再判断，已经可能丢失溢出信息；先比较 `base > UINT64_MAX-length` 才能拒绝超出表示范围的输入。这里的格式检查也不负责实际安装页表，地址整数合法与能够解引用是两项条件。

loader 名称必须在 32 字节内结束，避免日志 `%s` 越界读取。内核大小非零，两种起点都能加上大小；内存图还需至少包含一段 usable RAM。验证保证输入满足这些格式条件，实际页表映射由引导过程建立。

**追加到 `kernel/boot/info.c` 末尾：**

<a id="code-kernel-boot-info-c-2"></a>

<!-- append: kernel/boot/info.c -->
```c
const char *airi_boot_validate(const AiriBootInfo *info) {  // 检查内核依赖的必需启动字段，返回首个具体错误
    if (!info)  // 未取得结构指针时停止，不读取任何成员
        return "boot info is missing";  // 向调用者报告整份启动结构缺失
    if (info->magic != AIRI_BOOT_MAGIC || info->version != AIRI_BOOT_VERSION ||  // 识别值或版本不一致时，后面的布局不能按本接口理解
        info->size != sizeof(*info))  // 结构字节数也必须匹配；sizeof 不访问 info 指向的内容
        return "boot ABI mismatch";   // 格式标识、版本或布局大小不一致，拒绝继续读地图
    if (!info->region_count || info->region_count > AIRI_MAX_MEMORY_REGIONS)  // 有效项必须非零且不超固定数组容量
        return "memory map is missing or exceeds capacity";  // 报告缺少地图或项数越界，后续不遍历数组
    if (!info->direct_map_offset)  // 本接口要求引导器提供非零高地址直接映射偏移
        return "direct mapping is missing";  // 缺少当前地址解释依赖的直接映射信息
    bool terminated = false;  // 先假定名称没有终止零，直到在数组范围内找到
    for (size_t i = 0; i < sizeof(info->loader); ++i)  // 只检查固定名称数组的 32 字节，不越过对象边界
        if (!info->loader[i]) {  // 当前字节为 NUL 表明名称已在数组范围内结束
            terminated = true;  // 记录名称可以安全按字符串读取
            break;  // 名称终止零已找到，不再检查后面的未使用字节
        }
    if (!terminated)  // 整个名称数组都没有零，不能交给日志的 %s
        return "loader name is not terminated";  // 报告名称不终止，防止后续字符串读取越界
    if (!info->kernel_size || info->kernel_physical_base > UINT64_MAX - info->kernel_size ||  // 内核跨度非零，物理起点加跨度也必须不回绕
        info->kernel_virtual_base > UINT64_MAX - info->kernel_size)  // 同样检查链接虚拟起点加跨度不超地址整数上限
        return "kernel address range is invalid";  // 拒绝缺失或计算溢出的内核范围
    uint64_t previous_end = 0;  // 保存已检查区域的半开右端，供下一项比较
    for (uint32_t i = 0; i < info->region_count; ++i) {  // 只检查本次有效地图项，不把未使用槽位算进去
        const struct airi_memory_region *r = &info->regions[i];  // 取当前条目地址供只读检查，不复制整段内存
        if (!r->length || r->base > UINT64_MAX - r->length)      // 长度必须非零，起点加长度必须在 uint64_t 内可表示
            return "memory range overflow or empty range";  // 遇到空区域或回绕区域时立即交回具体错误
        if (i && r->base < previous_end)  // 非首项必须从前项右端或更高处开始，相等表示相邻
            return "memory map overlaps or is unsorted";  // 当前起点落入前项或顺序颠倒，拒绝地图
        if (r->kind > AIRI_MEMORY_FRAMEBUFFER)  // 用途值不得超出本接口的最后一个枚举值
            return "unknown memory kind";  // 不把未知用途误认为普通可用内存
        previous_end = r->base + r->length;  // 无溢出已证明，保存本项右端供下一项使用
    }
    if (!airi_memory_bytes(info, AIRI_MEMORY_USABLE))  // 至少一段区域必须归类为可用 RAM，后续内核才有内存来源
        return "no usable memory";  // 格式虽可能完整，但当前缺少可用 RAM
    return NULL;  // 全部必需条件通过，以空指针表示没有错误文字
}
```

日志需要按类型汇总内存量。`airi_memory_bytes` 只扫描 region_count 个有效条目，类型与请求一致才加长度；`airi_memory_name` 将类型转换为固定文本，未知值返回 UNKNOWN。汇总发生在整张图验证成功之后。

**追加到 `kernel/boot/info.c` 末尾：**

<a id="code-kernel-boot-info-c-3"></a>

<!-- append: kernel/boot/info.c -->
```c
uint64_t airi_memory_bytes(const AiriBootInfo *info, uint32_t kind) {  // 汇总已验证地图中用途为 kind 的字节长度
    uint64_t bytes = 0;  // 尚未处理区域时总量为零
    for (uint32_t i = 0; i < info->region_count; ++i)  // 只统计有效条目，不读剩余固定容量槽位
        if (info->regions[i].kind == kind)  // 本条用途与请求类型一致才加入总量
            bytes += info->regions[i].length;  // 把当前区域长度加入指定用途的累计字节数
    return bytes;  // 返回分类总量，还没有扣除以后实际分配的页面
}
const char *airi_memory_name(uint32_t kind) {  // 把枚举用途转成日志固定名称，未知值保留为 UNKNOWN
    static const char *names[] = {"USABLE",     "RESERVED",     "ACPI RECLAIM", "ACPI NVS",  // 静态字符串指针表按枚举 0..3 的顺序保存名称
                                  "BAD MEMORY", "BOOT RECLAIM", "KERNEL",       "FRAMEBUFFER"};  // 继续保存枚举 4..7 的名称，索引和用途值保持一致
    return kind <= AIRI_MEMORY_FRAMEBUFFER ? names[kind] : "UNKNOWN";  // 先检查索引范围，合法时返回对应名称，否则返回备用名称
}
```

用两个相邻区域运行一次完整检查：第一段为 `[0x1000,0x2000)`，第二段为 `[0x2000,0x6000)`。观察程序持有静态 info，expect 每次调用验证并打印错误或 valid。

`(AiriBootInfo){.magic=..., ...}` 是 C 复合字面量，在表达式中建立结构值。`.magic=` 等按成员名初始化，省略成员为零，再赋值复制到 info。依次运行合法输入、错误版本、重叠区域和零长度区域；改变下一个条件前先恢复上一项，使每条错误能对应到明确原因。

**新建 `labs/host/boot/boot_info_probe.c`：**

<a id="code-labs-host-boot-boot-info-probe-c-1"></a>

<!-- write: labs/host/boot/boot_info_probe.c -->
```c
#include <airi/boot.h>  // 使用双方共同的字段布局和枚举常量
#include <stdio.h>      // 主机观察程序借用 printf 和 puts 显示结果；内核函数仍使用自己的实现
#include <string.h>     // 主机观察用 strcmp/strcpy 检查或准备文字，不加入内核链接

static AiriBootInfo info;  // 主机夹具的静态对象初始全零，整个进程期间都存在
static int expect(const char *name, const char *wanted) {  // 验证当前 info，将实际错误与本轮想看到的错误比较
    const char *error = airi_boot_validate(&info);      // 把夹具对象地址传入只读验证，NULL 表示成功
    printf("%s: %s\n", name, error ? error : "valid");  // 按 error 是否非空显示具体错误或 valid
    return wanted ? (!error || strcmp(error, wanted)) : error != NULL;  // 有期望错误时必须实际得到同一文字；期望成功时必须返回 NULL
}
int main(void) {  // 主机 C 运行时调用此入口，返回值成为观察进程退出状态
    info = (AiriBootInfo){.magic = AIRI_BOOT_MAGIC,  // 复合字面量构造新结构值，指定成员之外的字段都初始化为零
                          .version = AIRI_BOOT_VERSION,  // 使用本接口当前版本，作为合法基线
                          .size = sizeof(info),  // 保存完整对象的字节数，而非指针大小
                          .direct_map_offset = UINT64_C(0xffff800000000000),  // 只供格式检查的高地址偏移，主机夹具不解引用相应虚拟地址
                          .kernel_physical_base = 0x100000,  // 模拟内核位于物理 1 MiB 处
                          .kernel_virtual_base = UINT64_C(0xffffffff80000000),  // 模拟高地址链接起点，当前只是整数描述
                          .kernel_size = 0x1000,  // 模拟内核跨度为 4096 字节
                          .region_count = 2};     // 仅声明接下来的两个地图条目有效，其余槽位保持零
    strcpy(info.loader, "host probe");  // 把短名称和 NUL 复制进 32 字节数组，当前字面量一定装得下
    info.regions[0] =  // 给第零条地图项写入下面构造的完整区域值
        (struct airi_memory_region){.base = 0x1000, .length = 0x1000, .kind = AIRI_MEMORY_RESERVED};  // 第一段为 [0x1000,0x2000)，用途保留
    info.regions[1] =  // 给第一条地图项写入下面构造的完整区域值
        (struct airi_memory_region){.base = 0x2000, .length = 0x4000, .kind = AIRI_MEMORY_USABLE};  // 第二段为 [0x2000,0x6000)，与前段相邻并提供可用 RAM
    if (expect("baseline", NULL))  // 合法基线必须返回无错误；返回非零说明观察不符
        return 1;  // 本轮实际结果与期望不符，以非零状态结束主机观察
    info.version++;  // 只改版本一个条件，制造接口版本不匹配
    if (expect("wrong version", "boot ABI mismatch"))  // 必须得到版本不匹配错误，否则观察失败
        return 1;  // 本轮实际结果与期望不符，以非零状态结束主机观察
    info.version = AIRI_BOOT_VERSION;  // 恢复版本后再制造下一个错误，避免上一错误遮住下一条件
    info.regions[1].base = 0x1800;     // 让第二段起点落在第一段内，制造重叠
    if (expect("overlap", "memory map overlaps or is unsorted"))  // 验证重叠必须在地图顺序检查处被拒绝
        return 1;  // 本轮实际结果与期望不符，以非零状态结束主机观察
    info.regions[1].base = 0x2000;  // 恢复相邻起点，再测试零长度错误
    info.regions[1].length = 0;     // 第二段改为空区域，保持其他条件不变
    if (expect("empty range", "memory range overflow or empty range"))  // 验证空区域必须在长度检查处被拒绝
        return 1;  // 本轮实际结果与期望不符，以非零状态结束主机观察
    puts("PASS lab/boot-info: valid ABI and rejected ranges observed");  // 四组结果均符合才报告这轮夹具观察成功
    return 0;  // 本轮所有结果均符合，向 shell 交回成功状态
}
```

将验证实现与观察程序链接，用相同结构依次运行这四个状态。主机程序构造数据，当前 Mac 的真实内存图不参与这个检查。

**在学习目录根目录运行：**

<!-- run -->
```sh
mkdir -p build  # 建立主机观察产物目录，已有目录也可继续
cc -std=c11 -O0 -g -Wall -Wextra -Werror -Iinclude labs/host/boot/boot_info_probe.c kernel/boot/info.c  -o build/probe  # 把本轮观察源码和实现编译链接成指定主机可执行文件
./build/probe  # 运行刚生成的主机观察，非零退出表示实际结果不符
```

输出依次为 valid、boot ABI mismatch、memory map overlaps or is unsorted、memory range overflow or empty range。合法区域可以进入汇总，非法结构则带着具体错误返回。

内核内部的数据格式已确定。这些函数进入独立 ELF 后，由汇编入口建立栈，再通过 C 调用使用。
