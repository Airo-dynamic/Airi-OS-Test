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
