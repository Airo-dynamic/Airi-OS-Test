#include "internal.h"  // 引入当前目录内部共享接口和基础函数

void ui_app_text(int x, int y, const char *s) {  // 用正文体和主要颜色绘制应用文字
    u_text(x, y, s, U_BODY, U_TEXT);  // 在整屏x=x、整屏y=y显示s
}

void ui_app_muted(int x, int y, const char *s) {  // 用小字体和次要颜色绘制说明文字
    u_text(x, y, s, U_SMALL, U_MUTED);  // 在整屏x=x、整屏y=y显示s
}

void ui_app_field(struct ui_rect r, const char *s, bool focused) {  // 画输入字段和当前焦点边框
    u_round(r, UI_CONTROL_RADIUS, focused ? U_BLUE : U_CONTROL);  // 输入框始终可识别
    u_round((struct ui_rect){r.x + 1, r.y + 1, r.w - 2, r.h - 2}, UI_CONTROL_RADIUS - 1, U_SURFACE);  // 内容填充
    if (focused) u_rect(r.x + 4, r.y + r.h - 2, r.w - 8, 2, U_BLUE);  // 加粗底边区分键盘焦点
    ui_app_text(r.x + 10, r.y + 7, s);  // 在内容左缘右移10像素、内容顶边下移7像素显示s
}

bool ui_app_match(const char *s, const char *q) {  // 按字节判断字符串s是否包含子串q，不修改任一借用字符串
    size_t n = airi_strlen(q);  // 子串q的内容字节数，决定每个候选起点需要比较的长度
    if (!n)  // 空子串按约定总是匹配，无需扫描字符串s
        return true;  // 空子串已满足包含关系，调用方如何使用true由当前场景决定
    for (; *s; ++s)  // 沿当前NUL字符串逐字节推进，不读取终止零之后
        if (airi_strlen(s) >= n && !airi_memcmp(s, q, n))  // 剩余字节足够才比较n字节，memcmp为0表示找到子串
            return true;  // 当前位置找到子串q，返回true并停止继续扫描
    return false;  // 字符串s的所有起点都没有匹配子串q，返回false
}

void ui_app_dialog(struct ui_desktop *s, int kind, int target, const char *title, const char *body,  // 把种类、目标身份和文案保存到桌面对话框状态
                   const char *value) {  // value提供确认输入的初值，按字段容量复制
    s->dialog = kind;  // 保存对话框种类，在普通窗口之后绘制；输入分派优先检查此状态
    s->dialog_target = target;  // 保存本次确认将使用的文件id或任务pid
    s->field_all = true;  // 输入字段内容设为整体选中，下一次键入可以替换
    airi_snprintf(s->dialog_title, 64, "%s", title);   // 复制此次对话框标题到桌面独立64字节数组，不借用调用者临时指针
    airi_snprintf(s->dialog_text, 256, "%s", body);    // 复制具体反馈正文到256字节数组，对话框后续重画仍可读取
    airi_snprintf(s->dialog_input, 128, "%s", value);  // 复制名称框初值到128字节独立数组，之后键入只改此数组
}

void ui_app_unavailable(struct ui_desktop *s, struct ui_rect r, const char *service) {  // 按服务状态绘制加载、空或不可用提示
    const char *title = s->service_state == UI_LOADING ? "Loading data"  // 按当前服务状态选择不可用提示标题，先处理正在加载
                        : s->service_state == UI_ERROR ? "Unable to load data"  // 加载失败选择明确错误标题，正常正文不会借此假装已经取得数据
                        : s->service_state == UI_EMPTY ? "No records"  // 查询结果为空时选择无记录标题，和缺少服务区分
                                                       : "Unavailable";  // 其余未正常状态用明确未连接标题，避免画出成功快照
    u_icon(r.x + 36, r.y + 80, UI_SYSTEM, U_MUTED);      // 在提示区域左侧画系统图标，旁边文字说明具体缺失的服务
    u_text(r.x + 72, r.y + 74, title, U_LABEL, U_TEXT);  // 在内容左缘右移72像素、内容顶边下移74像素显示按状态选定的标题
    char line[128];  // 128字节局部数组保存当前服务身份说明，供次要文字绘图
    airi_snprintf(line, 128, "%s", service);  // 把当前服务标识复制为本次提示第二行，128字节局部数组供后面的文字绘图
    ui_app_muted(r.x + 72, r.y + 108, line);  // 在提示图标右侧显示service的有界副本，说明具体未接入的服务
    ui_app_muted(r.x + 72, r.y + 137,  // 用小字体和次要颜色绘制说明文字，本次实参取自当前桌面状态
                 s->service_state == UI_LOADING ? "Waiting for the provider to return a snapshot."  // 加载中说明等待提供者快照，尚无可绘制新数据
                 : s->service_state == UI_ERROR  // 错误状态的正文使用失败说明，其他情况继续选择离线说明
                     ? "The operation failed. Retry to refresh this view."  // 错误状态说明当前请求失败，重试将重新查询
                     : "No data is available for this view.");  // 尚未连接状态说明这个视图依赖的服务接口
    if (s->service_state == UI_ERROR)  // 只有操作失败状态需要显示Retry按钮
        u_button((struct ui_rect){r.x + 72, r.y + 179, 92, 32}, "Retry", true, false);  // 画文字"Retry"按钮；其矩形供后续事件命中共用，当前仅输出像素
}

void ui_app_toolbar(struct ui_rect r, const char *label) {  // 在应用内容顶部绘制44像素工具栏
    u_rect(r.x, r.y, r.w, UI_TOOLBAR_H, U_PANEL);  // 用工具栏底色写矩形，起点(r.x,r.y)，尺寸r.w×44像素
    u_rect(r.x, r.y + UI_TOOLBAR_H - 1, r.w, 1, U_EDGE);  // 用分隔线色写矩形，起点(r.x,r.y + UI_TOOLBAR_H - 1)，尺寸r.w×1像素
    ui_app_text(r.x + 16, r.y + 12, label);  // 在工具栏左上绘制调用者传入的label；Terminal传入当前后端标题
}

void ui_app_status(struct ui_rect r, const char *left, const char *right) {  // 在应用内容底部绘制27像素状态栏
    u_rect(r.x, r.y + r.h - UI_STATUS_H, r.w, UI_STATUS_H, U_PANEL);  // 用工具栏底色写矩形，起点(r.x,r.y + r.h - UI_STATUS_H)，尺寸r.w×27像素
    u_rect(r.x, r.y + r.h - UI_STATUS_H, r.w, 1, U_EDGE);  // 用分隔线色写矩形，起点(r.x,r.y + r.h - UI_STATUS_H)，尺寸r.w×1像素
    ui_app_muted(r.x + 14, r.y + r.h - 21, left);  // 在内容左缘右移14像素、内容底边上移21像素显示left
    if (right)  // 调用方提供右侧文字时才测量和绘制；NULL 表示这一侧没有文字
        ui_app_muted(r.x + r.w - u_text_width(right, U_SMALL) - 16, r.y + r.h - 21, right);  // 用同一小字体测right的像素宽度，让文字右边距保持16像素；right借用调用者状态说明
}

void u_icon(int x, int y, int id, uint32_t c) {  // 按应用编号绘制20像素附近的几何图标
    /* Original, simple geometric symbols. Icons remain recognisable at 20 px. */
    switch (id) {      // 按应用编号选择几何图标，不由像素触发应用动作
    case UI_TERMINAL:  // 按Terminal应用和windows数组下标0进入对应分支
        u_frame((struct ui_rect){x, y + 2, 20, 16}, c);  // 沿当前显式矩形的四边画一像素边框
        u_line(x + 4, y + 6, x + 7, y + 9, c);      // 用传入图标色连接像素端点(x + 4,y + 6)到(x + 7,y + 9)
        u_line(x + 7, y + 9, x + 4, y + 12, c);     // 用传入图标色连接像素端点(x + 7,y + 9)到(x + 4,y + 12)
        u_line(x + 10, y + 13, x + 15, y + 13, c);  // 用传入图标色连接像素端点(x + 10,y + 13)到(x + 15,y + 13)
        break;  // 结束当前最近一层循环或应用分派，不执行其剩余候选
    case UI_FILES_APP:  // 按Files应用和windows数组下标1进入对应分支
        u_line(x, y + 5, x + 8, y + 5, c);  // 用传入图标色连接像素端点(x,y + 5)到(x + 8,y + 5)
        u_line(x + 8, y + 5, x + 10, y + 8, c);   // 用传入图标色连接像素端点(x + 8,y + 5)到(x + 10,y + 8)
        u_line(x + 10, y + 8, x + 20, y + 8, c);  // 用传入图标色连接像素端点(x + 10,y + 8)到(x + 20,y + 8)
        u_frame((struct ui_rect){x, y + 8, 21, 11}, c);  // 沿当前显式矩形的四边画一像素边框
        break;  // 结束当前最近一层循环或应用分派，不执行其剩余候选
    case UI_EDITOR:  // 按Editor应用和windows数组下标2进入对应分支
        u_frame((struct ui_rect){x + 3, y, 15, 21}, c);  // 沿当前显式矩形的四边画一像素边框
        for (int r = 5; r < 18; r += 4)  // 每4像素画一条Editor图标内的文字短横线
            u_line(x + 6, y + r, x + 14, y + r, c);  // 用传入图标色连接像素端点(x + 6,y + r)到(x + 14,y + r)
        break;  // 结束当前最近一层循环或应用分派，不执行其剩余候选
    case UI_TASKS_APP:  // 按Tasks应用和windows数组下标3进入对应分支
        for (int i = 0; i < 4; ++i)  // 画Tasks图标的4根柱，每柱右移5像素、高度依次增加4像素
            u_rect(x + i * 5, y + 18 - i * 4, 3, i * 4 + 3, c);  // 用传入图标色写矩形，起点(x + i * 5,y + 18 - i * 4)，尺寸3×i * 4 + 3像素
        break;  // 结束当前最近一层循环或应用分派，不执行其剩余候选
    case UI_SYSTEM:  // 按System应用和windows数组下标4进入对应分支
        u_frame((struct ui_rect){x + 2, y + 2, 17, 13}, c);  // 沿当前显式矩形的四边画一像素边框
        u_line(x + 10, y + 15, x + 10, y + 19, c);  // 用传入图标色连接像素端点(x + 10,y + 15)到(x + 10,y + 19)
        u_line(x + 5, y + 19, x + 15, y + 19, c);   // 用传入图标色连接像素端点(x + 5,y + 19)到(x + 15,y + 19)
        break;  // 结束当前最近一层循环或应用分派，不执行其剩余候选
    case UI_SETTINGS:  // 按Settings应用和windows数组下标5进入对应分支
        for (int i = 0; i < 3; ++i) {  // Settings图标按三条滑轨生成标记，逐项下移6像素
            u_line(x, y + 4 + i * 6, x + 20, y + 4 + i * 6, c);  // 用传入图标色连接像素端点(x,y + 4 + i * 6)到(x + 20,y + 4 + i * 6)
            u_rect(x + 4 + i * 4, y + 1 + i * 6, 3, 7, c);  // 用传入图标色写矩形，起点(x + 4 + i * 4,y + 1 + i * 6)，尺寸3×7像素
        }
        break;  // 结束当前最近一层循环或应用分派，不执行其剩余候选
    default:  // 其他编号进入当前switch的默认结果
        u_frame((struct ui_rect){x, y, 21, 21}, c);  // 沿当前显式矩形的四边画一像素边框
        u_line(x + 3, y + 17, x + 9, y + 8, c);      // 用传入图标色连接像素端点(x + 3,y + 17)到(x + 9,y + 8)
        u_line(x + 9, y + 8, x + 14, y + 14, c);     // 用传入图标色连接像素端点(x + 9,y + 8)到(x + 14,y + 14)
        u_line(x + 14, y + 14, x + 18, y + 6, c);    // 用传入图标色连接像素端点(x + 14,y + 14)到(x + 18,y + 6)
        break;  // 结束当前最近一层循环或应用分派，不执行其剩余候选
    }
}

