#include "internal.h"  // 引入当前目录内部共享接口和基础函数
static const char *titles[] = {"Terminal", "Files",    "Editor",     "Tasks",  // 窗口标题按应用枚举顺序排，应用编号可直接索引
                               "System",   "Settings", "Data viewer"};  // 继续应用枚举4到6的窗口标题，数组顺序与应用id一致
static const char *icons[] = {"Terminal", "Files",    "Editor", "Tasks",  // 侧栏悬停名称按相同应用顺序排
                              "System",   "Settings", "Viewer"};  // 继续应用枚举4到6的侧栏名称，同一编号显示同一应用
static void monitor(struct ui_desktop *s) {  // 共享资源仪表，全部数值来自同一次backend快照
    int x = s->width - UI_MONITOR_W, left = x + 24, width = UI_MONITOR_W - 48;  // 单一内边距对齐标题、数值与坐标轴
    char value[96];  // 临时格式化数值，不持有另一个统计状态
    u_rect(x, UI_TOP, UI_MONITOR_W, s->height - UI_TOP - UI_BOTTOM, U_BG);  // 仪表与应用正文分离
    u_rect(x, UI_TOP, 1, s->height - UI_TOP - UI_BOTTOM, U_EDGE);  // 系统侧栏边界
    u_label(left, 60, "SYSTEM / RESOURCES", U_MUTED);  // 与窗口身份统一的短标签
    u_text(left, 88, "Resource monitor", U_LABEL, U_TEXT);  // 可理解的区域标题
    u_rect(left, 123, width, 1, U_CONTROL);  // 模块的共同基准线
    u_text(left, 138, s->backend.mode == UI_REFERENCE ? "Recorded session" : "Current session", U_SMALL, U_MUTED);  // 明确实际数据来源
    if (s->stats.available) {  // 只有快照存在才绘制数值和曲线
        u_label(left, 174, "CPU LOAD", U_BLUE);  // 蓝色对应CPU测量
        airi_snprintf(value, sizeof(value), "%u", s->stats.cpu);  // 读取已有百分比
        u_text(left, 191, value, U_DISPLAY, U_TEXT);   // 大字号突出当前值，图表解释历史
        u_text(left + 58, 212, "%", U_BODY, U_MUTED);  // 单位与数值分层
        u_text(left + width - 54, 209, "60 sec", U_SMALL, U_MUTED);  // 横轴的采样跨度
        int top = 255, bottom = 355, graphw = width - 28;  // 纵轴固定0..100%，不根据当前曲线自动放大
        for (int i = 0; i <= 4; ++i) {  // 五条刻度线对应0、25、50、75、100
            int y = top + i * 25;  // 每百分比一个像素
            for (int gx = 0; gx < graphw; gx += 8)  // 每8像素取一个CPU网格短线起点
                u_rect(left + gx, y, 2, 1, U_EDGE);  // 每个起点写2×1像素，形成对应真实刻度的点线
            u_rect(left - 4, y, 4, 1, U_CONTROL);  // 轴端刻度确定实际读数位置
        }
        u_text(left + graphw + 6, top - 6, "100", U_SMALL, U_MUTED);   // 上界百分比
        u_text(left + graphw + 6, top + 44, "50", U_SMALL, U_MUTED);   // 中线百分比
        u_text(left + graphw + 6, bottom - 6, "0", U_SMALL, U_MUTED);  // 下界百分比
        for (unsigned i = 1; i < 60; ++i) {  // 用backend已有60个历史点连接折线
            unsigned prev = s->stats.samples[i - 1], now = s->stats.samples[i];  // 借用同一统计快照
            if (prev > 100)  // 前一个采样超过百分数上界时限制高度
                prev = 100;  // 顶点最多落在100%刻度处
            if (now > 100)  // 当前采样也按相同上界限制
                now = 100;  // 当前顶点最多落在图表顶部
            int px = left + (int)(i - 1) * graphw / 59, nx = left + (int)i * graphw / 59;  // 时间均匀映射到横轴
            u_line(px, bottom - (int)prev, nx, bottom - (int)now, U_BLUE);  // 蓝色走势与CPU标签保持一致
        }
        u_rect(left, bottom + 1, graphw, 1, U_CONTROL);  // 明确时间轴
        u_text(left, bottom + 10, "-60s", U_SMALL, U_MUTED);  // 过去的时间端点
        u_text(left + graphw - 24, bottom + 10, "now", U_SMALL, U_MUTED);  // 当前时间端点
        u_rect(left, 401, width, 1, U_EDGE);     // CPU与内存使用同一列宽
        u_label(left, 420, "MEMORY", U_ACCENT);  // 内存沿用粉色类别
        airi_snprintf(value, sizeof(value), "%u", s->stats.memory_kib / 1024);  // 实际KiB换算MiB
        u_text(left, 437, value, U_DISPLAY, U_TEXT);  // 已使用量为主读数
        airi_snprintf(value, sizeof(value), "/ %u MiB", s->stats.total_kib / 1024);  // 容量保留明确单位
        u_text(left + 75, 458, value, U_BODY, U_MUTED);  // 与CPU单位共享同一文字基准
        u_rect(left, 493, width, 12, U_RAISED);  // 连续容量条不虚构内存分段
        if (s->stats.total_kib) {  // 无总量时避免除零
            uint64_t used = s->stats.memory_kib < s->stats.total_kib ? s->stats.memory_kib : s->stats.total_kib;  // 限制异常比例
            u_rect(left, 493, (int)(used * width / s->stats.total_kib), 12, U_ACCENT);  // 宽度线性表达已使用量
        }
        u_rect(left, 537, width, 1, U_EDGE);  // I/O区沿相同栅格排列
        u_label(left, 556, "I/O THROUGHPUT", U_MUTED);  // 速率与容量明确分区
        airi_snprintf(value, sizeof(value), "%u", s->stats.read_kib);  // 读取当前读速率
        u_text(left, 582, value, U_TITLE, U_BLUE);  // 读操作的颜色与标记一致
        airi_snprintf(value, sizeof(value), "%u", s->stats.write_kib);  // 读取当前写速率
        u_text(left + 116, 582, value, U_TITLE, U_PINK);      // 写操作用独立图表类别色
        u_text(left, 615, "Read / KiB/s", U_SMALL, U_MUTED);  // 标明读操作和单位
        u_text(left + 116, 615, "Write / KiB/s", U_SMALL, U_MUTED);  // 标明写操作和单位
    } else {  // 未采集时保留明确空状态
        u_text(left, 180, "No runtime samples", U_LABEL, U_TEXT);  // 不用装饰图形冒充测量
        u_text(left, 212, "CPU and I/O collection is offline.", U_SMALL, U_MUTED);  // 指出具体缺失能力
    }
    u_button((struct ui_rect){left, s->height - 96, width, 32}, "Open task manager", false, false);  // 与既有点击区域一致
}
void ui_draw(struct ui_desktop *s, const struct airi_framebuffer *f) {  // 按窗口层次画整帧，并将对话框留在最后
    if (!airi_framebuffer_valid(f))  // 画布地址、尺寸或通道无效时，在写像素之前结束
        return;  // 当前按窗口层次画整帧，并将对话框留在最后在本分支结束，后续语句不执行
    s->width = (int)f->width;    // 当前帧画布宽度按像素保存，绘图已检查帧缓冲能用且宽度可转int
    s->height = (int)f->height;  // 当前帧画布高度按像素保存，窗口收窄和坐标布局使用这个尺寸
    u_begin(f);  // 借用本次画布描述，建立颜色布局与整屏裁剪
    u_theme(s->theme);  // 按会话模式选整套调色板，颜色角色与业务状态分离
    u_rect(0, 0, s->width, s->height, U_BG);  // 用画布背景色写矩形，起点(0,0)，尺寸s->width×s->height像素
    u_text(86, s->height - 117, "AiriOS", U_TITLE, U_MUTED);  // 空桌面仅保留轻量产品标识
    u_rect(0, 0, s->width, UI_TOP, U_PANEL);  // 系统顶栏先铺满36像素高度，身份片随后覆盖左端
    u_plate((struct ui_rect){0, 0, 140, UI_TOP}, U_CHROME);  // 系统入口使用唯一品牌身份片
    u_rect(0, 35, s->width, 1, U_EDGE);  // 用分隔线色写矩形，起点(0,35)，尺寸s->width×1像素
    u_brand(16, 7);  // 系统入口的原创品牌符号
    u_label(52, 8, "AIRI", U_INK);  // 在整屏(52,8)写系统品牌文字，与左侧A形图标共同构成身份区
    u_label(156, 9, "WORKSPACE", U_MUTED);  // 在整屏(156,9)写WORKSPACE，位于140像素身份片右侧
    u_text(288, 9, s->active < 0 ? "Desktop" : titles[s->active], U_MEDIUM, U_TEXT);  // 在整屏(288,9)写当前活动应用名；无活动窗口时显示Desktop
    u_text(s->width - 263, 10,  // 顶栏右侧说明当前画面来自主机参考还是内核控制台
           s->backend.mode == UI_REFERENCE ? "Local session" : "Local console",  // 顶栏用实际后端模式标明主机参考工作区或guest内核控制台
           U_SMALL, U_MUTED);  // 系统状态保持次要前景
    u_icon(s->width - 37, 8, UI_TASKS_APP, U_MUTED);      // 顶栏右侧画资源入口图标，是否展开由桌面monitor状态决定
    u_rect(0, UI_TOP, UI_DOCK, s->height - 66, U_PANEL);  // 用工具栏底色写矩形，起点(0,36)，尺寸56×s->height - 66像素
    u_rect(55, 36, 1, s->height - 66, U_EDGE);     // 用分隔线色写矩形，起点(55,36)，尺寸1×s->height - 66像素
    for (unsigned i = 0; i < UI_APP_COUNT; ++i) {  // 逐个检查7个有效应用编号
        int y = 58 + (int)i * 54;   // 侧栏第i个应用图标从第58行起每54像素排列
        if (s->active == (int)i) {  // 当前应用是活动窗口时给侧栏对应项铺选中背景
            u_round((struct ui_rect){7, y - 8, 42, 38}, 2, U_RAISED);  // 用选中背景色写矩形，起点(7,y - 8)，尺寸42×38像素
            u_round((struct ui_rect){2, y, 3, 22}, 1, U_ACCENT);  // 活动应用旁写三像素宽、22像素高的强调色竖标，起点(2,y)
        }
        u_icon(18, y, (int)i, s->active == (int)i ? U_TEXT : U_MUTED);  // 按应用编号画左侧图标，活动项使用主要色
        if (s->windows[i].open)  // 已打开应用即使最小化也保留侧栏打开标记
            u_rect(25, y + 25, 6, 2, U_ACCENT);  // 用会话强调色写矩形，起点(25,y + 25)，尺寸6×2像素
        if (u_inside((struct ui_rect){0, y - 8, 56, 38}, s->mouse_x, s->mouse_y)) {  // 鼠标落在当前应用侧栏行的半开矩形时显示其名称提示
            u_rect(60, y, 100, 28, U_RAISED);  // 用选中背景色写矩形，起点(60,y)，尺寸100×28像素
            u_text(70, y + 5, icons[i], U_BODY, U_TEXT);  // 在整屏列70、整屏y=y + 5显示当前应用悬停名称
        }
    }
    int right = s->monitor && s->width >= UI_MONITOR_MIN ? 268 : 0;  // 资源栏开启且屏幕够宽才预留268像素，否则窗口可用至右边
    unsigned first = 0;  // 默认从最底层窗口开始绘制，后面找可遮挡底层的最大化窗口
    for (unsigned n = 0; n < s->order_count; ++n) {  // 按有效窗口层次遍历，数组后部不作窗口身份
        struct ui_window *w = &s->windows[s->order[n]];  // 先由绘图次序取得应用编号，再借用对应窗口，不把层次n当应用id
        if (w->open && !w->minimized && w->maximized)    // 只选已打开、可见且最大化的窗口作为底层跳过起点
            first = n;  // 记录目前找到的最高最大化窗口层次，它下方窗口无需输出被覆盖像素
    }
    for (unsigned n = first; n < s->order_count; ++n) {  // 按有效窗口层次遍历，数组后部不作窗口身份
        unsigned app = s->order[n];  // 当前层次n保存的是应用id，后续按该id取窗口和标题
        struct ui_window *w = &s->windows[app];  // 借用当前应用的窗口状态，app已经通过范围检查
        if (!w->open || w->minimized)  // 关闭或最小化窗口不画正文，但其其他会话状态仍保留
            continue;  // 本项不参与当前结果，跳过本轮剩余操作进入下一项
        if (w->maximized)  // 最大化窗口按当前画布扣系统栏和资源栏重新计算矩形
            w->rect = (struct ui_rect){68, 48, s->width - 80 - right, s->height - 90};  // 最大化窗口从侧栏右边和顶栏下开始，扣右资源栏及上下固定栏得到可用尺寸
        if (w->rect.w > s->width - 80 - right)  // 普通窗口宽度不能占到左侧系统栏和右侧资源栏
            w->rect.w = s->width - 80 - right;  // 把过宽窗口收窄到侧栏与资源栏之间，避免正文侵入系统栏
        if (w->rect.h > s->height - 88)  // 窗口高度超过上下保留区时缩短
            w->rect.h = s->height - 88;  // 把过高窗口收窄到整屏高度扣上下栏，保留任务栏空间
        if (w->rect.x + w->rect.w > s->width - right - 10)  // 窗口右边侵入资源栏或右边距时左移回可用区
            w->rect.x = s->width - right - w->rect.w - 10;  // 右边越界时重新算左缘，使窗口右边落在资源栏左侧10像素处
        if (w->rect.y + w->rect.h > s->height - 40)  // 窗口底边侵入任务栏时上移回可用区
            w->rect.y = s->height - w->rect.h - 40;  // 底边越界时重新算顶缘，使窗口底边停在屏幕底40像素之上
        if (w->rect.x < 68)  // 窗口左缘越过侧栏后的最小位置时向右收回
            w->rect.x = 68;  // 左缘最小保留68像素，确保窗口不会盖住56像素侧栏
        if (w->rect.y < 46)  // 窗口顶缘越过顶栏后的最小位置时向下收回
            w->rect.y = 46;  // 顶缘最小46像素，确保标题栏位于系统顶栏之下
        struct ui_rect r = w->rect;  // 复制已收窄的当前窗口矩形，后面从它计算边框、标题与body
        u_reset_clip();  // 恢复整个画布的可写范围，后续控件不继承上一次局部裁剪
        bool focused = s->active == (int)app;  // 标题和局部边界共同指出当前窗口
        u_round((struct ui_rect){r.x + 3, r.y + 5, r.w, r.h}, UI_WINDOW_RADIUS + 1, U_SHADOW);  // 有限偏移阴影
        u_round(r, UI_WINDOW_RADIUS, focused ? U_CONTROL : U_EDGE);  // 窗口外轮廓
        u_round((struct ui_rect){r.x + 1, r.y + 1, r.w - 2, r.h - 2}, UI_WINDOW_RADIUS - 1, U_PANEL);  // 标题与框架表面
        u_rect(r.x + 1, r.y + UI_TITLE_H + 1, r.w - 2, r.h - UI_TITLE_H - 6, U_SURFACE);  // 正文保留底部圆角
        u_rect(r.x + 1, r.y + 1, r.w - 2, 1, focused ? U_ACCENT : U_CONTROL);  // 连续细轨统一窗口基线
        static const char *heads[] = {"TERMINAL", "FILES", "EDITOR", "TASKS", "SYSTEM", "SETTINGS", "DATA VIEWER"};  // 窗口身份标签独立于文件名
        int plate_width = 54 + u_label_width(heads[app]) + 12;  // 标签左缘距片左缘54像素，斜切右端再留12像素。
        if (plate_width < 174)  // 短标签保留当前身份片的统一最小宽度。
            plate_width = 174;  // 长标签才扩大片面，普通窗口外观保持相同度量。
        int available = r.w - 116;    // 片从外框左8像素开始，右界留在系统按钮前108像素。
        if (plate_width > available)  // 窄窗口的标题可用范围优先于理想字宽。
            plate_width = available;  // 不把身份片铺进最小化、最大化和关闭按钮区。
        u_plate((struct ui_rect){r.x + 8, r.y + 7, plate_width, UI_TITLE_H - 14}, U_CHROME);  // 用实际字体推进量承载完整应用身份。
        u_icon(r.x + 18, r.y + 14, (int)app, U_INK);   // 应用图标处于独立身份栏
        u_rect(r.x + 53, r.y + 12, 1, 24, U_CONTROL);  // 图标与标题使用一致的分隔尺度
        u_clip((struct ui_rect){r.x + 62, r.y + 1, r.w - 170, UI_TITLE_H - 2});  // 长文件名不会覆盖系统按钮
        u_label(r.x + 62, r.y + 14, heads[app], U_INK);  // 身份标签使用14像素Medium字形和额外字距，与普通正文区别
        if (app == UI_EDITOR && r.w > 500) {  // 宽编辑器在标题后保留当前对象
            u_text(r.x + 196, r.y + 14, "/", U_BODY, U_MUTED);  // 应用与文档层次分明
            u_text(r.x + 220, r.y + 14, s->editor_name, U_MEDIUM, U_TEXT);  // 文件名保持正常大小写
        }
        if (app == UI_TERMINAL && r.w > 600)  // 连接身份位于标题里，避免占用一整行工具栏
            u_text(r.x + 200, r.y + 15, s->backend.mode == UI_REFERENCE ? "/  Local console" : (s->backend.label ? s->backend.label : "Kernel console"), U_BODY, U_MUTED);  // 只报告实际接入类型
        u_reset_clip();  // 窗口操作区独立于标题裁剪
        u_line(r.x + r.w - 88, r.y + 28, r.x + r.w - 78, r.y + 28, U_MUTED);  // 最小化
        u_frame((struct ui_rect){r.x + r.w - 60, r.y + 19, 10, 10}, U_MUTED);  // 最大化
        u_line(r.x + r.w - 30, r.y + 19, r.x + r.w - 20, r.y + 29, U_MUTED);  // 关闭的第一条对角线
        u_line(r.x + r.w - 20, r.y + 19, r.x + r.w - 30, r.y + 29, U_MUTED);  // 关闭的第二条对角线
        struct ui_rect body = u_window_body(r);  // 左右边框各1像素、标题48像素及顶边1像素，body从外框顶下49像素开始
        u_rect(body.x, body.y, body.w, body.h - 5, U_SURFACE);  // 内容底色明确分离标题
        u_clip(body);  // 应用绘图仅写扣除窗口标题和边框的body，避免覆盖框外像素
        ui_apps_draw(s, (enum ui_app)app, body);  // 按应用编号选择已接入的内容绘图函数，本次实参取自当前桌面状态
        u_reset_clip();  // 恢复整个画布的可写范围，后续控件不继承上一次局部裁剪
        u_line(r.x + r.w - 10, r.y + r.h - 4, r.x + r.w - 4, r.y + r.h - 10, U_EDGE);  // 用分隔线色连接像素端点(r.x + r.w - 10,r.y + r.h - 4)到(r.x + r.w - 4,r.y + r.h - 10)
    }
    if (right)  // 资源栏占用宽度非零时才绘制它，本轮窗口已为其留空间
        monitor(s);  // 资源栏启用时读取同次统计，显示当前可用数据或未采集状态
    if (s->mouse_x < UI_DOCK && s->mouse_y >= 50) {  // 鼠标在应用侧栏区域时再按行距换算应用id
        int app = (s->mouse_y - 50) / 54;      // 先去侧栏顶边50像素再除行距54，得到候选应用编号
        if (app >= 0 && app < UI_APP_COUNT) {  // 悬停换算的应用编号必须落在0到6才取名称
            int y = 58 + app * 54;  // 候选应用名称提示与它的侧栏图标使用同一个行位置
            u_rect(60, y, 110, 30, U_RAISED);  // 用选中背景色写矩形，起点(60,y)，尺寸110×30像素
            u_frame((struct ui_rect){60, y, 110, 30}, U_EDGE);  // 沿当前显式矩形的四边画一像素边框
            u_text(70, y + 6, icons[app], U_BODY, U_TEXT);      // 在整屏列70、整屏y=y + 6显示命中的应用名称
        }
    }
    u_reset_clip();  // 恢复整个画布的可写范围，后续控件不继承上一次局部裁剪
    u_rect(0, s->height - UI_BOTTOM, s->width, 30, U_PANEL);  // 用工具栏底色写矩形，起点(0,s->height - UI_BOTTOM)，尺寸s->width×30像素
    u_rect(0, s->height - UI_BOTTOM, s->width, 1, U_EDGE);  // 用分隔线色写矩形，起点(0,s->height - UI_BOTTOM)，尺寸s->width×1像素
    u_text(14, s->height - 23,  // 底栏明确写当前帧来自参考内存还是实际内核启动数据
           s->backend.mode == UI_REFERENCE ? "Session storage"  // 底栏明确标明本进程内存参考工作区，和真实guest启动数据区分
                                           : "AiriOS / x86-64",  // 底栏将当前真实启动数据来源写明
           U_SMALL, U_MUTED);  // 底栏保持中性次文字
    int tx = 330;  // 任务栏的首个已打开应用标题从整屏第330列开始
    for (unsigned i = 0; i < UI_APP_COUNT; ++i)  // 逐个检查7个有效应用编号
        if (s->windows[i].open) {  // 仅已打开的应用进入底栏标题列表
            u_text(tx, s->height - 23, titles[i], U_SMALL, s->active == (int)i ? U_ACCENT : U_MUTED);  // 在整屏x=tx、整屏y=s->height - 23显示titles[i]
            tx += 88;  // 任务栏下一应用标题右移88像素
        }
    if (s->toast[0]) {  // 短提示非空时在普通窗口之后覆盖绘制
        u_rect(s->width - 382, s->height - 78, 360, 34, U_RAISED);  // 用选中背景色写矩形，起点(s->width - 382,s->height - 78)，尺寸360×34像素
        u_text(s->width - 368, s->height - 70, s->toast, U_SMALL, U_TEXT);  // 在整屏x=s->width - 368、整屏y=s->height - 70显示本次操作短提示
    }
    if (s->dialog) {  // 菜单和普通窗口已经绘完，再写对话框像素，使它覆盖下层
        struct ui_rect d = {s->width / 2 - 240, s->height / 2 - 112, 480, 224};  // 480×224对话框以整屏中心为中心，左上扣除半宽半高
        u_round((struct ui_rect){d.x + 4, d.y + 6, d.w, d.h}, 7, U_SHADOW);  // 在对话框右移4、下移6像素的位置画7像素半径阴影
        u_round(d, 2, U_CONTROL);  // 弹窗使用独立清晰边界
        u_round((struct ui_rect){d.x + 1, d.y + 1, d.w - 2, d.h - 2}, 1, U_SURFACE);  // 中性内容表面
        if (s->dialog == UI_DIALOG_ERROR) u_rect(d.x + 1, d.y + 1, 3, d.h - 2, U_RED);  // 错误沿左缘标明，正文仍完整可读
        u_rect(d.x + 24, d.y + 49, d.w - 48, 1, U_EDGE);  // 标题与说明之间轻分隔
        u_text(d.x + 24, d.y + 20, s->dialog_title, U_LABEL, s->dialog == UI_DIALOG_ERROR ? U_RED : U_TEXT);  // 在整屏x=d.x + 24、整屏y=d.y + 20显示对话框标题
        /* Wrap body on words without breaking paths outside their containing dialog. */
        char row[64];  // 对话框单显示行最多63内容字节，再留一格NUL
        unsigned used = 0;  // 当前对话框显示片段的内容长度从0字节开始
        int y = d.y + 58;   // 对话框正文第一行在框顶下移58像素
        const char *p = s->dialog_text;  // 借用对话框正文首字节，分词换行不修改原说明
        while (*p && y < d.y + 135) {    // 正文仍有字节且下一行起点未到框内135像素位置时继续绘制
            while (*p == ' ')  // 新显示行开始时跳过连续空格，避免换行后的行首保留分词空白
                ++p;  // 消费当前文字字节后指针向后推进一字节
            const char *start = p, *last_space = NULL;  // 记录本显示行起点；NULL表示尚未找到可回退的单词边界
            while (*p && *p != '\n' && (size_t)(p - start) < sizeof(row) - 1) {  // 扫描到NUL、显式换行或63字节容量便停止；每次写候选行后还要检查像素宽度
                row[p - start] = *p;     // 用当前扫描偏移把一个对话框字节放入局部候选行
                row[p - start + 1] = 0;  // 候选行每新增一个字节就补NUL，才能立即测像素宽度
                if (u_text_width(row, U_BODY) > 432)  // 候选行超过对话框正文432像素时准备换行
                    break;  // 候选行已超432像素，退出这层扫描，后面选择可回退的单词边界
                if (*p == ' ')  // 记住当前空格，超过宽度时可回退到单词边界
                    last_space = p;  // 记住当前空格位置，超宽时可回到这里结束本显示行
                ++p;  // 消费当前文字字节后指针向后推进一字节
            }
            if (*p && *p != '\n' && last_space && last_space > start)  // 因像素宽或字节容量停住且仍有本行余文时，若记录过非行首空格便回退到它
                p = last_space;  // 把下一行扫描起点回退到最后空格，后续先跳空格继续
            used = (unsigned)(p - start);  // 两个同数组指针之差给出本次需要复制的字节数
            if (!used && *p) {  // 长词首字节就超宽时至少推进一个字节，避免无限停在原地
                ++p;  // 消费当前文字字节后指针向后推进一字节
                used = 1;  // 过宽长词至少显示并消费一字节，保证下一轮推进
            }
            airi_memcpy(row, start, used);  // 复制当前显示片段[start,p)的used个字节到局部row，下一行补NUL
            row[used] = 0;  // 给本次选定显示片段补NUL，再交文字绘图
            u_text(d.x + 24, y, row, U_BODY, U_TEXT);  // 在整屏x=d.x + 24、整屏y=y显示本次拆出的显示文字行
            if (*p == '\n')  // 原文显式换行也应消费，不成为下一行首内容
                ++p;  // 消费当前文字字节后指针向后推进一字节
            y += 22;  // 对话框下一行向下移动22像素
        }
        if (s->dialog == UI_DIALOG_NEW_FILE || s->dialog == UI_DIALOG_NEW_DIR || s->dialog == UI_DIALOG_RENAME || s->dialog == UI_DIALOG_SAVE_AS) {  // 创建、重命名和另存需要名称字段，其他确认框不画输入
            u_rect(d.x + 24, d.y + 118, 432, 34, U_SURFACE);  // 用窗口正文底色写矩形，起点(d.x + 24,d.y + 118)，尺寸432×34像素
            u_frame((struct ui_rect){d.x + 24, d.y + 118, 432, 34}, U_BLUE);  // 沿当前显式矩形的四边画一像素边框
            u_clip((struct ui_rect){d.x + 34, d.y + 119, 412, 32});  // 把后续文字或像素限制在本次显式矩形内，长内容在边界停止写入
            u_text(d.x + 34, d.y + 126, s->dialog_input, U_BODY, U_TEXT);  // 在整屏x=d.x + 34、整屏y=d.y + 126显示对话框输入字段
            u_reset_clip();  // 恢复整个画布的可写范围，后续控件不继承上一次局部裁剪
        }
        u_button((struct ui_rect){d.x + d.w - 204, d.y + 174, 84, 30},  // 对话框右侧先画取消或关闭按钮，接下来的种类选择文字
                 (s->dialog == UI_DIALOG_ERROR || s->dialog == UI_DIALOG_INFO) ? "Close" : "Cancel", false, false);  // 错误框只关闭提示，其余框此按钮取消待确认操作
        if (s->dialog == UI_DIALOG_CLOSE) {  // 未保存关闭对话框额外提供取消与丢弃两条明确操作
            u_button((struct ui_rect){d.x + 180, d.y + 174, 84, 30}, "Cancel", false, false);  // 画文字"Cancel"按钮；其矩形供后续事件命中共用，当前仅输出像素
            u_button((struct ui_rect){d.x + 276, d.y + 174, 84, 30}, "Discard", false, false);  // 画文字"Discard"按钮；其矩形供后续事件命中共用，当前仅输出像素
        }
        if (s->dialog != UI_DIALOG_ERROR && s->dialog != UI_DIALOG_INFO)  // 纯错误框只有关闭按钮，其余种类再画确认动作
            u_button((struct ui_rect){d.x + d.w - 108, d.y + 174, 84, 30},  // 在对话框右端画当前种类的确认按钮，事件接入后才执行动作
                     s->dialog == UI_DIALOG_CLOSE   ? "Save"     // 未保存关闭确认的主按钮先保存，成功回调之后才关闭
                     : s->dialog == UI_DIALOG_DELETE ? "Delete"  // 删除确认框主按钮明确使用Delete文字，避免与保存混淆
                     : s->dialog == UI_DIALOG_STOP ? "End task"  // 停止参考任务确认框主按钮明确显示End task
                                      : "Confirm",  // 其他确认种类采用通用Confirm按钮文字
                     true, false);  // 确认按钮用强调状态且可用，实际动作仍由事件入口执行
    }
}
