#include "internal.h"  // 取得应用内部绘图、命中与对话框的共同声明。
#include "../apps/internal.h"  // 借用Editor正文定位器，续拖不调用它的完整按钮分派。
void ui_pointer(struct ui_desktop *s, int x, int y, int phase) {  // phase=0移动、1按下、2释放、3双击；x/y 在桌面屏幕坐标中。
    s->mouse_x = x;    // 保存当前鼠标横坐标，绘图可以显示指针。
    s->mouse_y = y;    // 保存当前鼠标纵坐标。
    if (phase == 2) {  // 释放事件结束当前拖动，无需再命中内容。
        s->drag = UI_DRAG_NONE;  // -1 表示没有拖动，下一次移动不会延续选区。
        return;  // 本次事件处理已结束，不再落入其他按钮或底层窗口。
    }
    if (phase == 0 && s->drag == UI_DRAG_SELECT) {  // 选择已在按下时开始，移动只续接文字间隙。
        struct ui_rect r = s->windows[UI_EDITOR].rect;  // 使用Editor此刻外框，不保存过期屏幕位置。
        s->cursor = ui_editor_position(s, u_window_body(r), x, y);  // 越界点夹到正文，不发送Save、打开文件或字段事件。
        s->selection = true;  // 固定anchor与当前cursor继续界定半开范围，两端相等时为空。
        return;  // 此移动已处理，窗口和应用按钮不再次消费它。
    }
    if (phase != 1 && phase != 3)  // 普通移动且未拖动时不触发应用动作。
        return;  // 本次事件处理已结束，不再落入其他按钮或底层窗口。
    for (int n = (int)s->order_count - 1; n >= 0; --n) {  // 从绘制顺序末端逆查，最上方窗口先获得事件。
        int app = (int)s->order[n];  // 顺序数组保存应用身份，不是窗口地址。
        struct ui_window *w = &s->windows[app];  // 借用该应用的窗口状态，后续变更留在桌面中。
        if (!w->open || w->minimized || !u_inside(w->rect, x, y))  // 关闭、最小化或未命中的窗口都不能接收此次点击。
            continue;  // 跳过当前不适用项，继续检查下一项。
        ui_open(s, (enum ui_app)app);  // 命中窗口置顶并成为活动应用；类型转换把整数恢复为枚举。
        struct ui_rect r = w->rect;    // 复制当前命中矩形，接下来拆分标题栏与内容区。
        if (y < r.y + UI_TITLE_H + 1)  // 标题栏输入留给下一检查点，当前不送入应用正文。
            return;  // 本次事件处理已结束，不再落入其他按钮或底层窗口。
        ui_apps_click(s, (enum ui_app)app, u_window_body(r), x, y, phase == 3);  // 内容矩形去掉边框和标题栏；双击由 phase==3 转成布尔值。
        struct ui_rect body = u_window_body(r);  // 从当前外框先扣标题和边框，与Editor绘图使用同一宽度。
        int tree = body.w >= 800 ? 190 : 0;      // body798没有项目栏，不能因为外框800就扣掉它。
        struct ui_rect text = {  // 只在实际正文裁剪内的按下开始文字选择。
            body.x + tree + 61, body.y + 88 + (s->find ? 44 : 0),  // 左缘扣行号，查找展开时正文再下移。
            body.w - tree - 72, body.h - 118 - (s->find ? 44 : 0)  // 宽高与字形可写范围相同，不把状态栏算正文。
        };
        if (app == UI_EDITOR && u_inside(text, x, y)) {  // 半开正文包含当前点时才登记持续选择。
            s->drag = UI_DRAG_SELECT;  // 之后phase0只进入上面的窄定位路径。
            s->anchor = s->cursor;     // 按下取得的合法间隙固定为整个动作起点。
        }
        return;  // 本次事件处理已结束，不再落入其他按钮或底层窗口。
    }
}
