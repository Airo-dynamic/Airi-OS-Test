#include "internal.h"  // 引入当前目录内部共享接口和基础函数

void ui_viewer_draw(struct ui_desktop *s, struct ui_rect r) {  // 把已提交时间和测量值缩放到真实时间比例的像素曲线
    ui_app_toolbar(r, s->series_name[0] ? s->series_name : "Data viewer");  // 在内容顶端铺44像素工具栏并显示按当前状态选择的文案
    u_button((struct ui_rect){r.x + r.w - 108, r.y + 8, 92, 28}, "Open files", false, false);  // 画文字"Open files"按钮；其矩形供后续事件命中共用，当前仅输出像素
    u_label(r.x + 24, r.y + 67, "TIME SERIES", U_MUTED);  // 在内容左缘右移24像素、顶边下移67像素显示时间序列分区身份。
    ui_app_muted(r.x + 24, r.y + 102, "Time / seconds");  // 在内容左缘右移24像素、内容顶边下移102像素显示文字"Time / seconds"
    if (s->service_state != UI_NORMAL || !s->series_count) {  // 服务不正常或没有成功提交曲线时仅显示提示，不绘造假的数据线
        ui_app_unavailable(s, (struct ui_rect){r.x, r.y + 44, r.w, r.h - 72},  // 按服务状态绘制加载、空或不可用提示，本次实参取自当前桌面状态
                           "Open a two-column CSV document from Files.");  // 提示从Files打开两列CSV的实际入口，尚未加载时不显示参考假曲线
        ui_app_status(r, "No series loaded", NULL);  // 在内容底部显示文字"No series loaded"；右侧说明按测得的文字宽度对齐
        return;  // 服务状态不可用或尚无已提交曲线，只显示提示，不缩放或连点
    }
    struct ui_rect graph = {r.x + 52, r.y + 156, r.w - 84, r.h - 241};  // 曲线图区扣左侧52像素标签、顶156像素工具栏及底部空间，尺寸仍按像素
    unsigned max = 1;  // 纵轴上限至少为1，使全零序列也不除零
    for (unsigned i = 0; i < s->series_count; ++i)  // 只遍历已经整份验证并提交的曲线点
        if (s->series_values[i] > max)  // 当前测量值超过现有纵轴上限时更新上限，较小值不缩短比例范围
            max = s->series_values[i];  // 遇到更大测量值就更新纵轴缩放上限
    for (unsigned i = 0; i < 4; ++i) {  // 给测量值纵轴画四个整数刻度和网格线
        int y = graph.y + (int)i * graph.h / 3;  // 第i条网格沿曲线高度的三等分排列，i从0到3包含上下边界
        u_rect(graph.x, y, graph.w, 1, U_EDGE);  // 用分隔线色写矩形，起点(graph.x,y)，尺寸graph.w×1像素
        char n[24];  // 24字节局部数组保存这条网格对应的整数测量值刻度
        airi_snprintf(n, 24, "%u", max - i * max / 3);  // 当前纵轴网格从最大测量值降到0，用实际数据范围形成刻度
        ui_app_muted(r.x + 20, y - 7, n);  // 在内容左缘右移20像素、整屏y=y - 7显示当前纵轴数值文字
    }
    unsigned duration = s->series_times[s->series_count - 1] - s->series_times[0];  // 最后时间减首时间得到整幅横轴表示的实际跨度
    if (!duration)  // 单个数据点的时间跨度为0时采用非零分母
        duration = 1;  // 仅为缩放计算保留1，原始时间数组不修改
    for (unsigned i = 1; i < s->series_count; ++i)  // 只遍历已经整份验证并提交的曲线点
        u_line(graph.x + (int)((uint64_t)(s->series_times[i - 1] - s->series_times[0]) *  // 上一点时间减首时间后乘图宽，随后除总跨度得到真实时间比例的起点x
                               (unsigned)graph.w / duration),  // 把已知为正的图宽按像素参与64位乘法，再除时间跨度得到前一点横向像素距离
               graph.y + graph.h -  // 纵轴从图底向上扣除测量值对应的像素高度
                   (int)((uint64_t)s->series_values[i - 1] * (unsigned)graph.h / max),  // 前一点测量值乘图高再除纵轴上限，64位计算后转成int像素高度
               graph.x + (int)((uint64_t)(s->series_times[i] - s->series_times[0]) *  // 当前点真实时间减首时间，先升64位再乘图宽避免中间溢出
                               (unsigned)graph.w / duration),  // 把已知为正的图宽按像素参与64位乘法，再除时间跨度得到当前点横向像素距离
               graph.y + graph.h - (int)((uint64_t)s->series_values[i] * (unsigned)graph.h / max),  // 当前值按纵轴上限换算为高度，再从图底扣除得到屏幕y
               U_ACCENT);  // 当前数据图元使用会话强调色
    for (unsigned i = 0; i < s->series_count; ++i) {  // 观测点与插值连线使用不同形状
        int px = graph.x + (int)((uint64_t)(s->series_times[i] - s->series_times[0]) * (unsigned)graph.w / duration);  // 横坐标取实际时间
        int py = graph.y + graph.h - (int)((uint64_t)s->series_values[i] * (unsigned)graph.h / max);  // 纵坐标取实际值
        u_rect(px - 3, py - 3, 7, 7, U_SURFACE);  // 隔开线段与采样标记
        u_frame((struct ui_rect){px - 2, py - 2, 5, 5}, U_ACCENT);  // 空心方点只表示存在的观测
    }
    char first[24], last[24];  // 两个24字节数组分别保存首时间和末时间标签，绘图期间同时保留
    airi_snprintf(first, 24, "%u s", s->series_times[0]);  // 形成CSV首点时间的秒数标签，显示在横轴左端
    airi_snprintf(last, 24, "%u s", s->series_times[s->series_count - 1]);  // 形成CSV末点时间的秒数标签，显示在横轴右端
    ui_app_muted(graph.x, graph.y + graph.h + 13, first);  // 在整屏x=graph.x、整屏y=graph.y + graph.h + 13显示首个时间标签
    ui_app_muted(graph.x + graph.w - u_text_width(last, U_SMALL), graph.y + graph.h + 13, last);  // 在整屏x=graph.x + graph.w - u_text_width(last, U_SMALL)、整屏y=graph.y + graph.h + 13显示最后时间标签
    char description[80];  // 80字节状态文字保存实际已提交点数与来源CSV文件名称
    airi_snprintf(description, 80, "%u rows / %s", s->series_count, s->series_name);  // 状态栏报告实际提交点数与来源文件名，验证失败时仍显示旧曲线身份
    ui_app_status(r, description, "CSV data");  // 在内容底部显示曲线条数和来源名称；右侧说明按测得的文字宽度对齐
}

void ui_viewer_click(struct ui_desktop *s, struct ui_rect r, int x, int y, bool twice) {  // Viewer的工具栏只提供打开Files按钮。
    (void)twice;  // 统一点击接口保留双击参数；这个应用当前只使用点击位置。
    if (y < r.y + 44 && x > r.x + r.w - 120)  // 命中Viewer顶部右侧文件入口的命中带，打开Files。
        ui_open(s, UI_FILES_APP);  // 打开并置顶当前目录文件窗口。
    return;  // 本次事件处理已结束，不再落入其他按钮或底层窗口。
}

