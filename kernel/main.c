#include <airi/kernel.h>   // 引入启动日志、错误停机与串口接口
#include <airi/console.h>  // 引入控制台状态与逻辑键编号

static struct airi_console console;  // 内核静态对象保留同一控制台的输入缓冲和显示内容

#ifndef AIRI_DEMO_MESSAGE  // 构建参数未指定输出文字时采用下方默认值
#define AIRI_DEMO_MESSAGE "Hello from AiriOS P1"  // 给本次启动准备默认演示文字
#endif  // 结束当前条件编译范围
_Noreturn void airi_kernel_main(const AiriBootInfo *info) {  // 校验启动信息后持续轮询输入并回送新命令输出
    const char *error = airi_boot_validate(info);  // 验证启动合同，NULL表示有效，非NULL是可直接输出的只读错误原因
    if (error)  // 启动合同验证返回非NULL原因时，在使用信息之前停止
        airi_panic(error);  // 把当前失败原因送到UART并停止内核
    airi_log("[BOOT_INFO] version=%u regions=%u loader=%s", info->version, info->region_count,  // 把当前启动状态记录送到UART
             info->loader);  // 把启动适配器保存的引导器名称作为%s实参
    airi_log("[KERNEL] physical=0x%llx virtual=0x%llx size=%llu",  // 把当前启动状态记录送到UART
             (unsigned long long)info->kernel_physical_base,  // 把当前64位字段转成格式要求的unsigned long long
             (unsigned long long)info->kernel_virtual_base, (unsigned long long)info->kernel_size);  // 把当前64位字段转成格式要求的unsigned long long
    airi_log("[HHDM] offset=0x%llx", (unsigned long long)info->direct_map_offset);  // 把当前启动状态记录送到UART
    for (uint32_t i = 0; i < info->region_count; ++i) {  // 逐条记录实际地图有效范围，尚未对其进行分配
        const struct airi_memory_region *r = &info->regions[i];  // 取实际启动地图第i条范围的只读地址，后续记录其物理起点和长度
        airi_log("[MEMORY] index=%u base=0x%llx length=0x%llx kind=%s", i,  // 把当前启动状态记录送到UART
                 (unsigned long long)r->base, (unsigned long long)r->length,  // 把当前64位字段转成格式要求的unsigned long long
                 airi_memory_name(r->kind));  // 把当前用途编号转为记录中的可读分类文字
    }
    uint64_t usable = airi_memory_bytes(info, AIRI_MEMORY_USABLE);  // 沿实际启动地图累加可用范围长度，结果单位是字节
    airi_log("[MEMORY_SUMMARY] usable_bytes=%llu boot_reclaim_bytes=%llu",  // 把当前启动状态记录送到UART
             (unsigned long long)usable,  // 把当前64位字段转成格式要求的unsigned long long
             (unsigned long long)airi_memory_bytes(info, AIRI_MEMORY_BOOT_RECLAIM));  // 把当前64位字段转成格式要求的unsigned long long
    airi_log("[LESSON] %s", AIRI_DEMO_MESSAGE);  // 把当前启动状态记录送到UART
    airi_console_init(&console, info);  // console绑定已验证的同一启动结构，初始化命令历史和输出环
    bool graphics = false;  // 当前只接串口，图形状态尚未启用
    if (graphics) {  // 只有本次图形真正启用时才报告成功显示参数
        const struct airi_framebuffer *fb = &info->framebuffer;  // 借用适配器给出的实际帧缓冲描述
        airi_log("[FRAMEBUFFER] width=%llu height=%llu pitch=%llu bpp=%u",  // 把当前启动状态记录送到UART
                 (unsigned long long)fb->width, (unsigned long long)fb->height,  // 把软件画布像素宽度、软件画布像素高度转成格式要求的unsigned long long
                 (unsigned long long)fb->pitch, (unsigned)fb->bpp);  // pitch是字节行距，转unsigned long long配%llu；bpp转unsigned配%u。
    } else  // 前一分支条件未成立时转入随后的单条替代操作
        airi_log("[FRAMEBUFFER_UNAVAILABLE] using serial output");  // 把当前启动状态记录送到UART
    /* P1 input checkpoint: serial state is ready; no drawing is implemented yet. */
    airi_log("[AIRI_P1_READY] graphics=%u usable_bytes=%llu", graphics ? 1u : 0u,  // 把当前启动状态记录送到UART
             (unsigned long long)usable);  // 把当前64位字段转成格式要求的unsigned long long
    airi_log("[AIRI_LESSON_READY] serial command loop active");  // 把当前启动状态记录送到UART
    airi_serial_write("airi> ");  // 发送没有换行的提示符，等待下一条输入
    for (;;) {  // 本操作持续循环，只有明确终点或分支才能退出
        int key = airi_input_poll();  // 每轮从键盘或UART取一个事件，无事件返回0
        if (!key) {  // 当前轮询没有逻辑事件时保持输入输出状态并继续等待
            __asm__ volatile("pause");  // 无输入时向CPU发忙等提示，仍继续轮询而没有睡眠调度
            continue;  // 本项不参与当前结果，跳过本轮剩余操作进入下一项
        }
        uint64_t before = console.total_lines;  // 记录按键前逻辑行号，提交后只发送新增输出
        bool changed = airi_console_key(&console, key);  // 把一个逻辑事件交给命令模型，返回是否可能需要刷新显示
        if (!changed)  // 该事件未改变可显示状态时无需发送更新记录
            continue;  // 本项不参与当前结果，跳过本轮剩余操作进入下一项
        if (key == '\n' || key == '\r') {  // 回车或换行说明本轮已提交命令，向串口发布完成后的控制台输出
            uint64_t first = console.total_lines - console.line_count;  // 求环中最早仍有效的逻辑行号
            if (before < first)  // 按键前起点已经被新输出覆盖时，从仍有效的最旧行开始
                before = first;  // 原起点已被覆盖时提升到仍可读取的最旧记录
            for (uint64_t i = before; i < console.total_lines; ++i) {  // 按逻辑行号读取仍保留或本次新增的console输出
                airi_serial_write(airi_console_line(&console, i));  // 读取第i条仍保留文字并逐字节送给UART
                airi_serial_write("\n");  // 每发送一条环记录后补串口换行
            }
            airi_serial_write("airi> ");  // 发送没有换行的提示符，等待下一条输入
        }

        /* Published after command handling; this checkpoint reports serial completion only. */
        if (key == '\n' || key == '\r' || key >= AIRI_KEY_PAGE_UP)  // 提交或功能事件处理完后才发更新记录，普通逐字节输入不产生这条完成标记
            airi_log("[CONSOLE_UPDATE] view=%u commands=%u page=%u", (unsigned)console.view,  // 把当前启动状态记录送到UART
                     console.commands, console.map_page);  // 发布处理后的提交计数与内存页号，供主机确认动作已完成
    }
}
