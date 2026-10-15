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
