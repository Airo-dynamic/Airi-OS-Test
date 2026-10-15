#include "internal.h"  // 引入当前目录内部共享接口和基础函数

static void chart(struct ui_desktop *s, struct ui_rect r) {  // 把60个百分数采样映射成窗口曲线
    int plotx = r.x + 30, width = r.w - 30;  // CPU曲线左边留30像素给纵轴刻度，余宽表示60个等间隔历史采样
    for (int i = 0; i <= 4; ++i) {  // i取0..4，共五条水平网格和四个高度间隔
        int y = r.y + i * r.h / 4;  // 第i条刻度按i/4分布，从图区顶到图区底均分，屏幕y向下增加
        for (int gx = 0; gx < width; gx += 8)  // 当前CPU刻度每8像素取一个短线起点
            u_rect(plotx + gx, y, 2, 1, U_EDGE);  // 在当前刻度的gx起点写2×1像素短线，八像素间距形成点线
        char v[16];  // 16字节局部数组把当前CPU纵轴百分数转换成可绘制文字
        airi_snprintf(v, 16, "%u", 100 - (unsigned)i * 25);  // 五条刻度依次写100、75、50、25、0，数值与上方网格位置配对
        ui_app_muted(r.x, y - 7, v);  // 在内容左缘、整屏y=y - 7显示v
    }
    for (unsigned i = 1; i < 60; ++i) {  // 按采样下标处理60个参考百分数点
        int x = plotx + (int)i * width / 59, y = r.y + r.h - (int)s->stats.samples[i] * r.h / 100;  // 第i个采样按59个间隔换成横坐标；百分数越高，屏幕y越靠上
        u_line(plotx + (int)(i - 1) * width / 59,  // 从上一采样的横坐标开始，60点对应59个等间隔，不按百分数改变横距
               r.y + r.h - (int)s->stats.samples[i - 1] * r.h / 100, x, y, U_BLUE);  // 上一采样百分数从图底向上换算，连到本次当前点
    }
    ui_app_muted(plotx, r.y + r.h + 13, "60 seconds ago");  // 在整屏x=plotx、整屏y=r.y + r.h + 13显示文字"60 seconds ago"
    ui_app_muted(r.x + r.w - 26, r.y + r.h + 13, "now");  // 在内容右缘左移26像素、整屏y=r.y + r.h + 13显示文字"now"
}

void ui_tasks_draw(struct ui_desktop *s, struct ui_rect r) {  // 绘制任务快照、CPU曲线和内存条
    ui_app_toolbar(r, "Processes");  // 在内容顶端铺44像素工具栏并显示文字"Processes"
    u_button((struct ui_rect){r.x + r.w - 184, r.y + 8, 76, 28}, "Refresh", false, false);  // 画文字"Refresh"按钮；其矩形供后续事件命中共用，当前仅输出像素
    u_button((struct ui_rect){r.x + r.w - 98, r.y + 8, 82, 28}, "End task", false, false);  // 画文字"End task"按钮；其矩形供后续事件命中共用，当前仅输出像素
    if (s->service_state != UI_NORMAL || !s->backend.tasks) {  // 参考服务未正常提供任务查询时不绘旧任务快照，明确显示不可用
        ui_app_unavailable(s, r, "Process and scheduler service");  // 按当前服务状态显示文字"Process and scheduler service"的缺失或失败信息
        ui_app_status(r, "No process snapshot", NULL);  // 在内容底部显示文字"No process snapshot"；右侧说明按测得的文字宽度对齐
        return;  // 当前绘制任务快照、CPU曲线和内存条在本分支结束，后续语句不执行
    }
    int graphheight = r.h >= 540 ? 160 : 100;  // 较高窗口给CPU图区160像素，否则100像素，后面的任务列表起点随之移动
    char label[80];  // 80字节局部数组保存CPU或内存标题数值，绘完当前标题后可复用
    airi_snprintf(label, 80, "CPU   %u%%", s->stats.cpu);  // Tasks标题显示同次参考CPU百分数，后面的CPU曲线读取同次samples
    u_text(r.x + 20, r.y + 56, label, U_TITLE, U_BLUE);  // 在内容左缘右移20像素、内容顶边下移56像素显示当前CPU或内存说明
    chart(s, (struct ui_rect){r.x + 20, r.y + 92, r.w / 2 - 40, graphheight});  // 把左半窗口正文区域交给CPU参考曲线绘制
    int rx = r.x + r.w / 2 + 20;  // 内容右半部再留20像素作为内存显示起点，CPU图区位于左半部
    u_label(rx, r.y + 61, "PHYSICAL MEMORY", U_ACCENT);  // 在整屏x=rx、内容顶边下移61像素显示内存区域标签PHYSICAL MEMORY
    airi_snprintf(label, 80, "%u MiB used / %u MiB total", s->stats.memory_kib / 1024,  // Tasks把参考已用和总内存各从KiB转MiB，说明后面比例条对应哪两个量
                  s->stats.total_kib / 1024);  // 总KiB除1024后作为MiB显示，与已用量单位一致
    ui_app_text(rx, r.y + 92, label);  // 在整屏x=rx、内容顶边下移92像素显示当前CPU或内存说明
    u_rect(rx, r.y + 134, r.w / 2 - 40, 10, U_RAISED);  // 用选中背景色写矩形，起点(rx,r.y + 134)，尺寸r.w / 2 - 40×10像素
    if (s->stats.total_kib)  // 总内存非零时才用它作比例分母
        u_rect(rx, r.y + 134,  // 内存比例条从右半区rx列与内容顶下134像素开始，下一行按已用比例计算宽度
               (int)((uint64_t)s->stats.memory_kib * (unsigned)(r.w / 2 - 40) / s->stats.total_kib),  // 已用KiB除总KiB得到比例；先扩大中间乘积，再换算成内存条像素宽度
               10, U_ACCENT);  // 已用内存比例条高10像素，宽度已按总量缩放
    ui_app_muted(rx, r.y + 164, "Memory usage / session total");  // 在整屏x=rx、内容顶边下移164像素显示文字"Memory usage / session total"
    ui_app_muted(rx, r.y + 187, "Recorded session");  // 在整屏x=rx、内容顶边下移187像素显示文字"Recorded session"
    int top = r.y + graphheight + 139;  // 任务列表起点在CPU图区和标题之后，graphheight影响剩余列表高度
    ui_app_field((struct ui_rect){r.x + 20, top, r.w - 40, 32},  // 画输入字段和当前焦点边框，本次实参取自当前桌面状态
                 s->filter[UI_TASKS_APP][0] ? s->filter[UI_TASKS_APP] : "Filter processes",  // 显示实际任务筛选文字，空时使用操作提示
                 s->focus == UI_FOCUS_FILTER);  // 筛选输入拥有焦点时画强调边框，不改变其他应用自己的筛选数组
    top += 48;  // 筛选输入框后向下留48像素安放任务列标题
    u_rect(r.x + 16, top, r.w - 32, 28, U_PANEL);  // 用工具栏底色写矩形，起点(r.x + 16,top)，尺寸r.w - 32×28像素
    ui_app_muted(r.x + 26, top + 6, "PID");    // 在内容左缘右移26像素、整屏y=top + 6显示文字"PID"
    ui_app_muted(r.x + 100, top + 6, "Name");  // 在内容左缘右移100像素、整屏y=top + 6显示文字"Name"
    ui_app_muted(r.x + r.w - 300, top + 6, "State");  // 在内容右缘左移300像素、整屏y=top + 6显示文字"State"
    ui_app_muted(r.x + r.w - 180, top + 6, "CPU");    // 在内容右缘左移180像素、整屏y=top + 6显示文字"CPU"
    ui_app_muted(r.x + r.w - 92, top + 6, "Memory");  // 在内容右缘左移92像素、整屏y=top + 6显示文字"Memory"
    top += 32;  // 列标题后向下留32像素安放第一条任务行
    unsigned shown = 0, rh = s->density ? 28 : 36;  // shown计匹配后的遍历行；密度决定每行28或36像素
    for (unsigned k = 0; k < s->task_count; ++k) {  // 只遍历本次任务快照有效前缀
        unsigned i = s->sort ? s->task_count - 1 - k : k;  // 排序开关只反转参考任务快照下标，k仍是当前遍历次序
        struct ui_task *t = &s->tasks[i];  // 借用选定快照项的pid、名称与参考资源字段，列表绘图不修改服务任务
        if (!ui_app_match(t->name, s->filter[UI_TASKS_APP]))  // 当前参考任务名称不包含筛选文字时跳过此项
            continue;  // 本项不参与当前结果，跳过本轮剩余操作进入下一项
        if (shown++ < s->windows[UI_TASKS_APP].scroll)  // 匹配项先计入shown，再跳过滚动位置之前的条目
            continue;  // 本项不参与当前结果，跳过本轮剩余操作进入下一项
        int y = top + (int)(shown - 1 - s->windows[UI_TASKS_APP].scroll) * (int)rh;  // 匹配条目减滚动量后按行高换成整屏行，滚动外条目不占可见行
        if (y + (int)rh > r.y + r.h - 34)  // 当前任务行会进入底部状态栏时结束绘图，后续快照仍保存但不可见
            break;  // 结束当前最近一层循环或应用分派，不执行其剩余候选
        if (i == s->selected_task)  // 按原快照下标判断选中项，反序显示不改变任务身份
            u_selection((struct ui_rect){r.x + 16, y, r.w - 32, (int)rh});  // 用选中背景色写矩形，起点(r.x + 16,y)，尺寸r.w - 32×(int)rh像素
        char value[40];  // 40字节局部数组依次格式化当前任务的pid、CPU与MiB内存文字
        airi_snprintf(value, 40, "%d", t->pid);  // 把当前参考任务身份转换成有符号十进制文字，供此任务行PID列
        ui_app_text(r.x + 26, y + 8, value);     // 在内容左缘右移26像素、整屏y=y + 8显示当前任务资源数值文字
        ui_app_text(r.x + 100, y + 8, t->name);  // 在内容左缘右移100像素、整屏y=y + 8显示当前任务名称
        ui_app_text(r.x + r.w - 300, y + 8, t->state);  // 在内容右缘左移300像素、整屏y=y + 8显示当前任务状态
        airi_snprintf(value, 40, "%u%%", t->cpu);    // 把当前任务参考CPU占比附百分号，供此任务行CPU列
        ui_app_text(r.x + r.w - 180, y + 8, value);  // 在内容右缘左移180像素、整屏y=y + 8显示当前任务资源数值文字
        airi_snprintf(value, 40, "%u MiB", t->memory_kib / 1024);  // 将当前任务KiB内存除1024转换成MiB，供此任务行内存列
        ui_app_text(r.x + r.w - 92, y + 8, value);  // 在内容右缘左移92像素、整屏y=y + 8显示当前任务资源数值文字
    }
    ui_app_status(r, "Select a process to inspect or end it.", "Session tasks");  // 在内容底部显示文字"Select a process to inspect or end it."；右侧说明按测得的文字宽度对齐
}

