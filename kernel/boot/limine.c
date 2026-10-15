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
