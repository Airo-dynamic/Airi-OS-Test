#include "internal.h"  // 取得本章已经实现的应用绘图与公共文字接口。

void ui_system_draw(struct ui_desktop *s, struct ui_rect r) {  // 按当前页显示同一启动信息的内存、设备或日志
    const char *tabs[] = {"Overview", "Memory", "Storage", "Devices", "Journal"};  // 5个System页面名称按tab编号顺序保存
    unsigned tab = s->windows[UI_SYSTEM].tab;  // System窗口的页下标决定概况、内存、存储、设备或日志
    for (unsigned i = 0; i < 5; ++i)  // 按有效页面下标画5个页签按钮
        u_button((struct ui_rect){r.x + 14 + (int)i * 108, r.y + 9, 98, 28}, tabs[i], tab == i,  // 画当前页面按钮名按钮；其矩形供后续事件命中共用，当前仅输出像素
                 false);  // 当前按钮未禁用，实参只控制绘图外观
    u_rect(r.x, r.y + 45, r.w, 1, U_EDGE);  // 用分隔线色写矩形，起点(r.x,r.y + 45)，尺寸r.w×1像素
    int x = r.x + 24, y = r.y + 65;  // System正文起点从内容左上分别留24和65像素
    char line[160];  // 160字节局部数组承接当前启动字段的格式化说明
    if (tab == 0) {  // System第0页显示当前后端的启动概况
        u_text(x, y, "System information", U_LABEL, U_TEXT);  // 在整屏x=x、整屏y=y显示文字"System information"
        ui_app_text(x, y + 41, "AiriOS / x86_64 workstation");  // 在整屏x=x、整屏y=y + 41显示文字"AiriOS / x86_64 workstation"
        ui_app_text(x, y + 73, "QEMU q35 / UEFI / single CPU");  // 在整屏x=x、整屏y=y + 73显示文字"QEMU q35 / UEFI / single CPU"
        airi_snprintf(line, 160, "Boot loader: %s", s->backend.boot->loader);  // System概况页显示这次绑定启动结构中的加载器名称
        ui_app_text(x, y + 105, line);  // 在概况页显示刚格式化的启动加载器名称
        const struct airi_framebuffer *display = &s->backend.boot->framebuffer;  // 查询服务提供的启动描述，不是 ui_draw 当前借用的主机画布
        if (airi_framebuffer_valid(display)) {  // 真实内核提供合法设备描述时显示其布局
            airi_snprintf(line, sizeof(line), "Display: %llux%llu / %u bpp / pitch %llu",  // 宽高按像素、pitch 按每行字节显示
                          (unsigned long long)display->width,   // 宽度转为 %llu 对应类型，值不改变
                          (unsigned long long)display->height,  // 高度转为 %llu 对应类型，值不改变
                          (unsigned)display->bpp,  // 每像素位数按 %u 显示
                          (unsigned long long)display->pitch);  // 每行字节跨度可大于宽度乘每像素字节数
        } else {  // 主机参考服务没有真实启动帧缓冲，不能把零值显示为屏幕参数
            airi_snprintf(line, sizeof(line), "Display: windowed session");  // 窗口仍可在独立的主机画布上绘制
        }
        ui_app_text(x, y + 137, line);  // 在概况页显示启动帧缓冲布局，或主机画布与启动描述分离的说明
        ui_app_text(x, y + 185, "Connected services");  // 在整屏x=x、整屏y=y + 185显示文字"Connected services"
        ui_app_muted(x, y + 216,  // 用小字体和次要颜色绘制说明文字，本次实参取自当前桌面状态
                     s->backend.mode == UI_REFERENCE  // 按实际绑定后端模式选择后续参考说明或内核说明，不修改mode字段
                         ? "Session storage / no disk mounted"  // 参考文件只存于本进程数组，尚无磁盘服务
                         : "Boot information / console / framebuffer");  // 真实内核后端当前只提供启动、console与像素输出
        u_button((struct ui_rect){x, y + 263, 180, 32}, "Open preferences", false, false);  // 画文字"Open preferences"按钮；其矩形供后续事件命中共用，当前仅输出像素
    } else if (tab == 1) {  // System第1页显示启动内存地图及各类字节总量
        u_label(x, y, "PHYSICAL MEMORY / BOOT INVENTORY", U_MUTED);  // 内存分类是启动库存，不是分配器实时用量
        uint64_t sizes[] = {airi_memory_bytes(s->backend.boot, AIRI_MEMORY_USABLE),  // 三类数据读取同一启动描述
                            airi_memory_bytes(s->backend.boot, AIRI_MEMORY_BOOT_RECLAIM),  // 保留中的引导内存
                            airi_memory_bytes(s->backend.boot, AIRI_MEMORY_KERNEL)};  // 内核及模块内存
        const char *names[] = {"Usable", "Boot reclaim / held", "Kernel"};  // 文字明确对应三类，不只靠颜色区分
        uint32_t colors[] = {U_ACCENT, U_BLUE, U_PINK};  // 内存图三类固定颜色
        uint64_t sum = sizes[0] + sizes[1] + sizes[2], accumulated = 0;  // 总和只用于三类字节占比
        int width = r.w - 48, previous = 0;  // 同一个列宽服务标题、图形与下方地址表
        for (unsigned k = 0; k < 3; ++k) {   // 三个读数同一基准线对齐
            int cx = x + (int)k * width / 3;  // 按当前窗口等分列宽
            airi_snprintf(line, sizeof(line), "%llu", (unsigned long long)(sizes[k] / 1024));  // 字节换算成KiB
            u_text(cx, y + 23, line, U_TITLE, colors[k]);    // 主要数值用22像素，长数字仍留有空间
            u_text(cx, y + 55, names[k], U_SMALL, U_MUTED);  // 分类独立一行
            accumulated += sizes[k];  // 累计边界可让整条宽度精确闭合
            int end = sum ? (int)(accumulated * (uint64_t)width / sum) : previous;  // 空库存不除零
            u_rect(x + previous, y + 85, end - previous, 16, colors[k]);  // 真实长度决定每段所占像素
            if (end > previous) u_rect(x + previous, y + 85, 1, 16, U_SURFACE);  // 分类之间用一像素分开
            previous = end;  // 下一段从前一段末尾继续
        }
        ui_app_muted(x, y + 110, "KiB / usable, boot reclaim and kernel");  // 分母与单位都明确
        int row = y + 142;  // 内存地图列标题从System正文起点下移142像素
        u_rect(x, row, width, 27, U_PANEL);  // 用工具栏底色写矩形，起点(x,row)，尺寸width×27像素
        ui_app_muted(x + 8, row + 6, "ID");  // 在整屏x=x + 8、整屏y=row + 6显示文字"ID"
        ui_app_muted(x + 56, row + 6, "Physical start");  // 在整屏x=x + 56、整屏y=row + 6显示文字"Physical start"
        ui_app_muted(x + 224, row + 6, "Physical end");   // 在整屏x=x + 224、整屏y=row + 6显示文字"Physical end"
        ui_app_muted(x + 392, row + 6, "Class");  // 在整屏x=x + 392、整屏y=row + 6显示文字"Class"
        row += 35;  // 标题行之后向下留35像素，作为首条地图记录位置
        unsigned capacity = (unsigned)(r.y + r.h - 42 - row) / 25,  // 扣状态栏后按每条25像素算当前窗口最多可见多少区域
                 begin = s->console->map_page * 12 + s->windows[UI_SYSTEM].scroll;  // 逻辑页先换成12条一页的区域下标，再加入窗口滚动量
        for (unsigned i = begin;  // 遍历本次显示区间，滚动前后的行保留在原状态
             i < s->backend.boot->region_count && i < begin + capacity && i < begin + 12; ++i) {  // 区域有效数、窗口容量和每页12条同时限制地图遍历
            const struct airi_memory_region *region = &s->backend.boot->regions[i];  // 地址与类别属于同一记录
            int ry = row + (int)(i - begin) * 25;  // 保持分页与滚动含义
            airi_snprintf(line, sizeof(line), "%03u", i);  // 地图下标为实际行身份
            u_text(x + 8, ry, line, U_MONO, U_MUTED);      // 下标列与地址留独立间隙
            airi_snprintf(line, sizeof(line), "%016llx", (unsigned long long)region->base);  // 十六进制物理起点
            u_text(x + 56, ry, line, U_MONO, U_TEXT);  // 起点固定16位
            airi_snprintf(line, sizeof(line), "%016llx", (unsigned long long)(region->base + region->length));  // 半开区间终点
            u_text(x + 224, ry, line, U_MONO, U_TEXT);  // 独立列避免两组长地址粘连
            u_text(x + 392, ry, airi_memory_name(region->kind), U_MONO,  // 类别名与实际枚举一致
                   region->kind == AIRI_MEMORY_USABLE ? U_ACCENT : U_TEXT);  // 可用内存强调规则不变

        }
    } else if (tab == 2) {  // System第2页说明参考内存工作区，或报告尚无块设备文件系统
        u_text(x, y, "Storage volumes", U_LABEL, U_TEXT);  // 在整屏x=x、整屏y=y显示文字"Storage volumes"
        if (s->backend.mode == UI_REFERENCE && s->service_state == UI_NORMAL) {  // 只有当前主机参考服务正常时才显示本会话内存工作区
            ui_app_text(x, y + 45, "Session storage");  // 在整屏x=x、整屏y=y + 45显示文字"Session storage"
            ui_app_muted(x, y + 76, "Files are kept until this session closes.");  // 在整屏x=x、整屏y=y + 76显示文字"Files are kept until this session closes."
            ui_app_muted(x, y + 124, "Capacity: 32 records / documents up to 8191 bytes");  // 在整屏x=x、整屏y=y + 124显示文字"Capacity: 32 records / documents up to 8191 bytes"
            ui_app_text(x, y + 159, "Files and Editor share the same workspace provider.");  // 在整屏x=x、整屏y=y + 159显示文字"Files and Editor share the same workspace provider."
            u_button((struct ui_rect){x, y + 220, 140, 32}, "Open files", true, false);  // 画文字"Open files"按钮；其矩形供后续事件命中共用，当前仅输出像素
        } else  // 前一分支条件未成立时转入随后的单条替代操作
            ui_app_unavailable(s, (struct ui_rect){r.x, r.y + 44, r.w, r.h - 72},  // 按服务状态绘制加载、空或不可用提示，本次实参取自当前桌面状态
                               "No mounted block-device filesystem");  // 存储未接入时明确说明尚无块设备文件系统
    } else if (tab == 3) {  // System第3页显示固定控制台设备端点
        u_text(x, y, "Console devices", U_LABEL, U_TEXT);  // 在整屏x=x、整屏y=y显示文字"Console devices"
        for (unsigned i = 0; i < 3; ++i) {  // 按固定端点下标0到2生成3条控制台设备说明
            airi_device_line(s->console, i, line, sizeof(line));  // 形成第i项设备说明，下一行直接绘制，不新增console日志
            u_text(x, y + 51 + (int)i * 42, line, U_MONO, U_TEXT);  // 把第i条控制台设备说明按42像素行距绘到Devices页
        }
        ui_app_muted(x, y + 202, "Console input and display endpoints");  // 在整屏x=x、整屏y=y + 202显示文字"Console input and display endpoints"
    } else {  // 其余页下标显示Journal启动交接记录；正常页签将这里选为第4页
        u_text(x, y, "Boot records", U_LABEL, U_TEXT);  // 在整屏x=x、整屏y=y显示文字"Boot records"
        ui_app_muted(x, y + 33, "Ordered handoff records / no wall-clock timestamp");  // 在整屏x=x、整屏y=y + 33显示文字"Ordered handoff records / no wall-clock timestamp"
        for (unsigned i = 0; i < airi_journal_count(); ++i) {  // 按8条启动记录的有效下标查询并显示
            airi_journal_line(s->console, i, line, sizeof(line));   // 形成第i项固定启动记录，下一行显示在System日志页
            u_text(x, y + 71 + (int)i * 32, line, U_MONO, U_TEXT);  // 把第i条启动交接记录按32像素行距绘到Journal页
        }
    }
    ui_app_status(r,  // 在应用内容底部绘制27像素状态栏，本次实参取自当前桌面状态
                  s->backend.mode == UI_REFERENCE  // 按实际绑定后端模式选择后续参考说明或内核说明，不修改mode字段
                      ? "Recorded boot inventory"  // 参考后端明确标示启动数据为示例
                      : "Live boot handoff",  // 真实后端标示数据来自当前内核启动交接
                  tab == 1 ? "PgUp / PgDn: memory inventory" : NULL);  // Memory页右侧显示翻页说明，其他页面不额外画这条状态文字
}

