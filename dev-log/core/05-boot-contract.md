# 05 · 把启动信息变成可检查的数据

日志已经能把整数变成文字。现在需要决定应该记录哪些内存信息：QEMU 配置了 256 MiB，并不表示这些地址都能任意使用，固件、引导器、内核和设备各自占用了一些范围。内核需要取得一份地图，先判断它的描述是否成立，以后才从可用范围中分配页面。

引导器提供的地图中，每项保存一段物理地址的起点、长度和用途。内核将这些信息转换成自己的 `AiriBootInfo`，日志、控制台与分配器以后只读取这份统一格式。更换引导器时，转换处随协议改变，消费者仍可使用相同字段。本章先在主机上构造数据，检查验证函数；读取真实启动响应要等引导适配接入。

## 先看这次要检查的两段地图

本章观察程序使用两项描述：第一项为 `[0x1000,0x2000)`，用途 reserved；第二项为 `[0x2000,0x6000)`，用途 usable。半开区间包括起点、不包括右端，所以第一段有 4096 字节，最后一个字节在 `0x1fff`；下一段从 `0x2000` 开始，恰好相邻，没有重叠。

这两条记录只描述范围，不包含范围内所有字节。把第二项的起点改成 `0x1800`，描述就与第一项重叠，验证函数必须拒绝；把长度改为零也应拒绝。观察程序会分别制造这些错误，每次恢复上一项，再改变下一项，使报告对应一个明确原因。这里的地图是测试输入，不是 Mac 的真实内存布局，检查它也不需要占用或访问那些物理范围。

<a id="memory-addresses"></a>

## 描述中的地址与 C 实际使用的指针

地图的物理地址给机器内存或设备范围编号。C 指针在当前开启分页的环境中使用虚拟地址，CPU 通过页表中的映射记录，把虚拟地址翻译成物理地址再访问目标。能用 64 位整数写出一个地址，不表示该位置已经映射，也不表示当前程序能够解引用它。

引导器会建立高地址直接映射 HHDM：一组物理范围在固定偏移处获得虚拟别名。例如物理地址 `0x1000` 加上 `0xffff800000000000`，得到 `0xffff800000001000`。这项加法利用已有映射，不会自行创建映射。

同一物理内容可以有不止一种虚拟映射。下面夹具把内核物理起点设为 `0x100000`，于是其 HHDM 别名为 `0xffff800000100000`；另外保存的 `kernel_virtual_base=0xffffffff80000000` 是内核链接时选择的执行地址。一个描述内核装载到哪里，一个描述代码按哪个虚拟起点链接，`direct_map_offset` 则用于另一种直接访问别名，三者不能混成同一个公式。当前主机观察只保存和检查这些整数，不建立页表、不解引用高地址；真实对应关系稍后由引导适配填入。

<a id="boot-layout"></a>

## 保存地图，也保留独立的可选显示描述

`AiriBootInfo` 将地图条目放进内嵌的 `regions` 数组。数组有 128 个槽位，`region_count` 说明本次用了几项；本章只有前两项有效。`loader` 也用自有字符数组保存引导器名称。这样持有的是自己的描述副本，不要求后续模块继续追踪引导器原来的条目指针。

用途值决定每个范围能怎样处理。usable 是引导分类中的可用 RAM；kernel、framebuffer、reserved 等范围仍有各自用途。ACPI 描述硬件与电源信息，ACPI reclaim/NVS 当前都保留；boot reclaim 是引导器使用、以后可回收的范围，本章还未回收，不能提前作为空闲 RAM。

消费者还需要确认自己正在按正确布局读取这些字段。`magic` 是固定识别整数，`version` 标明格式版本，`size` 保存整个结构大小。双方按相同位置和宽度读写的这组二进制约定称为 ABI；`uint64_t` 固定地址和长度的宽度，枚举给用途整数起名，`reserved` 当前置零。内核自身另有物理起点、链接虚拟起点与包含 BSS 的跨度。

地图还可能描述一块显示设备使用的内存，不能把它当作空闲 RAM。启动结构因而同时携带可选 `framebuffer` 描述，供串口报告尺寸或后续图形消费者使用。它与必需地图分别判断：本章夹具没有显示设备，描述全零，地图基线仍然可以有效。

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

接口中的两种验证有不同用途。`airi_boot_validate` 检查必需启动字段和地图，成功返回 NULL，失败返回静态错误文字，调用者不必释放。`const AiriBootInfo *` 使验证函数只读输入，错误数据保留原样，方便定位原因。`airi_framebuffer_valid` 独立返回 bool，决定可选显示描述能否用于绘图；必需验证不会调用它，因此无图形仍能走串口路径。



## 地图验证先建立计算条件，再判断是否有可用 RAM

回到两段地图。`airi_boot_validate` 先确认结构指针、格式标识、版本和大小，再限制有效条目数。地图数组最多 128 项，数量检查通过才遍历；直接映射偏移也必须满足本接口的非零要求。loader 名称需要在 32 字节内出现 NUL，后面的日志 `%s` 才不会越过数组继续寻找结束位置。

每项用 base 与 length 表示半开范围。计算右端前，先检查长度非零和加法不会溢出：无符号数超过 `UINT64_MAX` 会回绕，先算和再判断可能已经丢失原范围；比较 `base > UINT64_MAX-length` 能在相加前拒绝它。内核跨度与两种内核起点也按同一原则检查。

`previous_end` 保存上一项的右端。本例第一项结束于 `0x2000`，第二项从同一个值开始，允许相邻；若改成 `0x1800`，便小于 previous_end，应报告重叠或未排序。用途值还要落在本接口枚举中。排序和非重叠约束用于确认地图范围可以按此表示处理；分类求和本身并不依赖排列顺序。

这些字段和区间检查都通过后，验证器调用 `airi_memory_bytes` 汇总 usable，确认至少有一段可用 RAM，才返回 NULL。这里不是“统计必须等整个验证函数已经返回成功”：统计在验证器内部承担最后一个条件，进入它之前，条目数和范围已检查完毕。外部日志、控制台等消费者则应先取得验证成功，再使用地图。格式符合要求仍不代表地址已经有实际映射。

**新建 `kernel/boot/info.c`。** 先写头文件引用，再写必需数据的验证。

<a id="code-kernel-boot-info-c-2"></a>

<!-- write: kernel/boot/info.c -->
```c
#include <airi/boot.h>  // 使用双方共同的字段布局和枚举常量

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

## 同一张地图怎样得到分类数量和名称

统计函数只遍历 `region_count` 项，遇到用途与参数一致的条目才加长度。本例 reserved 一项为 `0x1000` 字节，usable 一项为 `0x4000`，所以查询 usable 得到 16384。它统计的是地图分类总量，不扣除后面实际分配的页面，也没有测量进程驻留内存。

`airi_memory_name` 将枚举值用作固定名称表的索引，未知值返回 `UNKNOWN`。名称表与返回的文字是静态内容，调用者只借用这些字符串。函数实现写在验证器之后仍能被调用，因为前面的头文件已经给出声明。

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

## 主机程序让同一份输入依次经过四种状态

实现就绪后，主机观察程序负责构造 `info` 并显示验证结果，`kernel/boot/info.c` 负责验证与统计。稍后将两份 C 文件链接成同一个 Mac 程序：观察入口可借主机 `printf/puts/strcmp`，验证实现仍是之后进入内核的同一份代码。

静态 `info` 在整个观察进程期间存在。`(AiriBootInfo){.magic=..., ...}` 是 C 复合字面量，在表达式中创建结构值；指定成员初始化选择字段，省略字段置零，随后整值赋给 info。这里设置了两项有效地图，其他槽位以及可选显示描述仍为零。

`expect` 每次都把当前 info 交给验证器，再比较返回结果与本轮期望。`wanted=NULL` 表示期望成功，任何错误指针都算不符；wanted 指向错误文字时，就要求实际得到同样文字。`!error || strcmp(...)` 先处理没有错误的情况，避免把空指针交给 strcmp。expect 返回非零表示观察不符，main 随即结束并向 shell 报告失败。

观察先运行合法基线，再只改变版本得到 `boot ABI mismatch`。恢复版本后把第二段起点改到第一段内部，得到重叠错误；恢复起点，再把长度设零，得到空范围错误。前一次条件若未恢复，会遮住下一次检查，因此每次只让当前错误成为首次拒绝原因。

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

<a id="code-kernel-boot-info-c-1"></a>

## 必需地图的路径写完，再补可选显示检查

观察入口现在已经给出四组地图状态，验证器与统计也都齐备。它不会调用显示验证；下面补足同一启动接口留给图形消费者的独立函数，章末再一起构建。没有有效显示描述时，这个函数返回 false，不会让刚才的地图基线失效。

帧缓冲是设备读取并形成画面的像素内存。描述中的 `width/height` 是像素数量，`pitch` 是相邻两行起点的字节距离，`bpp` 是每像素位数；24 bpp 占三字节，32 bpp 占四字节。红、绿、蓝通道各用 `size/shift` 表示位宽和起始位，例如红色八位从第 16 位开始，绿色从第 8 位开始，蓝色从第 0 位开始。

复制 framebuffer 结构只复制地址与布局，像素仍在另一块内存。图形消费者通过已有映射的 `virtual_address` 写像素，`physical_address` 只描述物理位置。本章主机程序不访问像素；后面的检查函数也只验证描述是否符合支持范围，不负责安装映射。

例如三像素宽、24 bpp 的一行至少需要九字节，`pitch=12` 可以容纳，`pitch=8` 就会使下一行过早开始。函数先限制非零地址和尺寸，要求尺寸能表示为绘图使用的 32 位有符号坐标，再确认像素格式为 24 或 32 bpp。求 `width×bytes`、`height×pitch` 和地址加跨度之前，都先排除溢出，避免一个看似合理的尺寸计算回绕到更小范围。

颜色通道也不能互相覆盖。红色八位从第 16 位开始时，掩码为 `0x00ff0000`；若绿色也使用这些位，写绿色就会改变红色。`((1<<size)-1)<<shift` 生成一个通道的掩码，`occupied` 累积已接受的位，下一通道与它按位与非零就表示相交。代码还限制每通道 1–8 位，并要求整个通道落在一个像素的 bpp 范围内。

下面在 `info.c` 开头的头文件引用后插入实现。第一块代码用于定位原文，用第二块的完整内容替换它，不要把两块都写入文件。地图验证与统计继续保留在后面，主机观察仍沿原来的必需数据路径运行。

**文件开头：第一块定位旧文本，第二块是替换内容。**

<!-- replace: kernel/boot/info.c -->
<!-- before -->
```c
#include <airi/boot.h>  // 使用双方共同的字段布局和枚举常量

```
<!-- after -->
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

## 编译观察程序，检查数据而不访问描述的内存

头文件规定共同结构，验证实现读取这份结构，观察入口创建并修改它。下面命令用 `-Iinclude` 找到头文件，把两个实现文件链接为 `build/probe`。运行的是主机程序；既没有查询 Mac 的实际物理地图，也没有启动虚拟机。

**在学习目录根目录运行：**

<!-- run -->
```sh
mkdir -p build  # 建立主机观察产物目录，已有目录也可继续
cc -std=c11 -O0 -g -Wall -Wextra -Werror -Iinclude labs/host/boot/boot_info_probe.c kernel/boot/info.c  -o build/probe  # 把本轮观察源码和实现编译链接成指定主机可执行文件
./build/probe  # 运行刚生成的主机观察，非零退出表示实际结果不符
```

输出依次为 `valid`、`boot ABI mismatch`、`memory map overlaps or is unsorted`、`memory range overflow or empty range`。最后的 PASS 只在四次结果都符合时出现。第一项表明这份构造的必需数据通过格式与范围检查；后三项各自表明一次变更在预期位置被拒绝。

现在已有统一描述和能报告具体原因的验证函数。真实启动还缺生产这些数据的一端：引导器装载内核，汇编建立自己的调用栈，适配层再把真实响应复制进 AiriBootInfo。下一章先建立独立 ELF 与 C 入口，这里的函数随后才能进入那条启动路径。
