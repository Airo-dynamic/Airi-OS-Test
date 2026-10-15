#include "internal.h"  // 取得应用内部绘图、命中与对话框的共同声明。
#include "../apps/internal.h"  // 借用Editor正文定位器，续拖不调用它的完整按钮分派。
void ui_pointer(struct ui_desktop *s, int x, int y, int phase) {  // phase=0移动、1按下、2释放、3双击；x/y 在桌面屏幕坐标中。
    s->mouse_x = x;  // 保存当前鼠标横坐标，绘图可以显示指针。
    s->mouse_y = y;  // 保存当前鼠标纵坐标。
    if (phase == 0 && s->drag == UI_DRAG_SELECT) {  // 选择已在按下时开始，移动只续接文字间隙。
        struct ui_rect r = s->windows[UI_EDITOR].rect;  // 使用Editor此刻外框，不保存过期屏幕位置。
        s->cursor = ui_editor_position(s, u_window_body(r), x, y);  // 越界点夹到正文，不发送Save、打开文件或字段事件。
        s->selection = true;  // 固定anchor与当前cursor继续界定半开范围，两端相等时为空。
        return;  // 此移动已处理，窗口和应用按钮不再次消费它。
    }
    if (phase == 0 && s->drag >= 0) {  // 移动时只有已登记拖动才改变窗口。
        struct ui_window *w = &s->windows[s->active];  // 借用发起拖动的活动窗口。
        if (s->drag == UI_DRAG_MOVE) {  // 移窗保持按下点相对窗口位置不变。
            w->rect.x = x - s->drag_x;  // 当前鼠标横坐标减按下偏移得到新左边界。
            w->rect.y = y - s->drag_y;  // 同样得到新上边界。
        } else {  // 当前条件不成立，按随后明确的另一种事件或状态处理。
            w->rect.w = x - w->rect.x;  // 缩放右下角时宽度为鼠标到窗口左边的距离。
            if (w->rect.w < 720)  // 当前内容布局最小支持720像素宽。
                w->rect.w = 720;  // 限住窗口宽度，不能拖成负数或过小区域。
            w->rect.h = y - w->rect.y;  // 高度为鼠标到窗口顶部的距离。
            if (w->rect.h < 480)  // 高度下限为480像素。
                w->rect.h = 480;  // 限住最小高度。
        }
        return;  // 本次事件处理已结束，不再落入其他按钮或底层窗口。
    }
    if (phase == 2) {  // 释放事件结束当前拖动，无需再命中内容。
        s->drag = UI_DRAG_NONE;  // -1 表示没有拖动，下一次移动不会延续选区。
        return;  // 本次事件处理已结束，不再落入其他按钮或底层窗口。
    }
    if (phase != 1 && phase != 3)  // 普通移动且未拖动时不触发应用动作。
        return;  // 本次事件处理已结束，不再落入其他按钮或底层窗口。
    s->toast[0] = 0;  // 新点击清掉旧临时提示，NUL使字符串为空。
    if (s->dialog) {  // 非零对话框优先截获点击，不能让后方窗口同时响应。
        int dx = s->width / 2 - 240, dy = s->height / 2 - 112;  // 对话框以480×224布局围绕桌面中心定位。
        if (s->dialog == UI_DIALOG_CLOSE && u_inside((struct ui_rect){dx + 180, dy + 174, 84, 30}, x, y)) {  // 关闭确认中的取消按钮保持原文并回到编辑。
            s->dialog = UI_DIALOG_NONE;  // 关闭当前确认或消息框。
            s->pending_close = false;    // 清除保存后继续关闭的意图，避免下次保存误关闭。
        } else if (u_inside((struct ui_rect){dx + 276, dy + 174, 84, 30}, x, y)) {  // 第二个按钮在未保存确认中放弃修改，其他框用于返回。
            if (s->dialog == UI_DIALOG_CLOSE) {  // 只有放弃关闭需要恢复文档并关闭Editor。
                copy(s->editor, UI_DOC_SIZE, s->saved);  // 把最近成功保存文本复制回工作数组，不调用服务保存。
                s->cursor = 0;     // 恢复后光标回到第一个字节间隙。
                s->dirty = false;  // 工作文本与saved相同，不再有未保存内容。
                s->undo_valid = false;  // 放弃后旧撤销快照失效，避免把修改重新带回来。
                s->windows[UI_EDITOR].open = false;  // 用户明确放弃后关闭Editor窗口。
                s->active = -1;  // 当前没有活动窗口，不能继续往刚关闭窗口发送键入。
            }
            s->dialog = UI_DIALOG_NONE;  // 关闭当前确认或消息框。
            s->pending_close = false;    // 清除保存后继续关闭的意图，避免下次保存误关闭。
        } else if (u_inside((struct ui_rect){dx + 372, dy + 174, 84, 30}, x, y))  // 最右按钮执行当前确认动作。
            ui_confirm(s);  // 使用已保存dialog_target与输入调用服务，失败仍有明确框。
        return;  // 本次事件处理已结束，不再落入其他按钮或底层窗口。
    }
    if (s->launcher && x < 252 && y >= 82 && y < 82 + UI_APP_COUNT * 42) {  // 已展开的启动器区域按42像素一行选应用。
        ui_open(s, (enum ui_app)((y - 82) / 42));  // 行号经过范围条件后可转换为应用枚举。
        return;  // 本次事件处理已结束，不再落入其他按钮或底层窗口。
    }
    if (y < UI_TOP) {  // 桌面顶部36像素属于全局栏。
        if (x < 140)  // 左端按钮切换应用启动菜单。
            s->launcher = !s->launcher;  // 布尔取反在显示和收起间切换。
        else if (x > s->width - 50)  // 顶栏最右侧切换监视区。
            s->monitor = !s->monitor;  // 保存监视区展开状态，下一帧按它布局。
        return;  // 本次事件处理已结束，不再落入其他按钮或底层窗口。
    }
    s->launcher = false;  // 点击启动器以外区域后收起菜单。
    if (x < UI_DOCK) {    // 左侧56像素应用栏直接打开应用。
        int app = (y - 50) / 54;  // 扣顶部50像素后每54像素一个应用图标。
        if (y >= 50 && app >= 0 && app < UI_APP_COUNT)  // 排除顶部空白和越界图标。
            ui_open(s, (enum ui_app)app);  // 命中窗口置顶并成为活动应用；类型转换把整数恢复为枚举。
        return;  // 本次事件处理已结束，不再落入其他按钮或底层窗口。
    }
    if (y >= s->height - UI_BOTTOM) {  // 桌面底部30像素属于已打开窗口栏。
        int pos = (x - 330) / 88, count = 0;    // 每88像素一项，count只统计已打开应用。
        for (int i = 0; i < UI_APP_COUNT; ++i)  // 逐个检查已有窗口可见性，给底栏中显示的应用分配命中区域。
            if (s->windows[i].open) {  // 底栏只给打开的窗口分配位置。
                if (count++ == pos && x >= 330) {  // 比较当前位置后递增计数，横坐标还需在条目区内。
                    ui_open(s, (enum ui_app)i);  // 打开/解除最小化并置顶命中的窗口。
                    break;  // 当前匹配已完成，停止这一层循环或分支。
                }
            }
        return;  // 本次事件处理已结束，不再落入其他按钮或底层窗口。
    }
    if (s->monitor && s->width >= UI_MONITOR_MIN && x > s->width - UI_MONITOR_W) {  // 宽桌面右侧监视区先于后方窗口响应。
        if (y > s->height - 96 && y < s->height - 64)  // 监视区底部按钮打开任务窗口。
            ui_open(s, UI_TASKS_APP);  // 将Tasks置顶显示实际服务记录。
        return;  // 本次事件处理已结束，不再落入其他按钮或底层窗口。
    }
    for (int n = (int)s->order_count - 1; n >= 0; --n) {  // 从绘制顺序末端逆查，最上方窗口先获得事件。
        int app = (int)s->order[n];  // 顺序数组保存应用身份，不是窗口地址。
        struct ui_window *w = &s->windows[app];  // 借用该应用的窗口状态，后续变更留在桌面中。
        if (!w->open || w->minimized || !u_inside(w->rect, x, y))  // 关闭、最小化或未命中的窗口都不能接收此次点击。
            continue;  // 跳过当前不适用项，继续检查下一项。
        ui_open(s, (enum ui_app)app);    // 命中窗口置顶并成为活动应用；类型转换把整数恢复为枚举。
        struct ui_rect r = w->rect;      // 复制当前命中矩形，接下来拆分标题栏与内容区。
        if (y < r.y + UI_TITLE_H + 1) {  // 窗口标题栏先处理关闭/最大化/最小化/移窗。
            if (x > r.x + r.w - 42) {  // 标题栏最右42像素命中关闭。
                if (app == UI_EDITOR && s->dirty) {  // Editor有未保存内容时不能直接关闭。
                    s->dialog = UI_DIALOG_CLOSE;  // 建立保存、放弃或返回的关闭确认。
                    copy(s->dialog_title, 64, "Unsaved changes");  // 复制确认标题到独立数组。
                    copy(s->dialog_text, 256,  // 复制完整操作说明，容量限制包含NUL。
                         "Save this document, discard changes, or return to editing.");  // 说明当前关闭需要选择的三种路径。
                } else {  // 当前条件不成立，按随后明确的另一种事件或状态处理。
                    w->open = false;  // 无未保存内容可直接关闭当前窗口。
                    s->active = -1;   // 当前没有活动窗口，不能继续往刚关闭窗口发送键入。
                }
                return;  // 本次事件处理已结束，不再落入其他按钮或底层窗口。
            }
            if (x > r.x + r.w - 72) {  // 标题栏关闭按钮左侧命中最大化/恢复。
                w->maximized = !w->maximized;  // 切换最大化状态。
                if (w->maximized)  // 刚进入最大化时需保留此前实际矩形。
                    w->restore = w->rect;  // 恢复位置独立保存，下一次绘图可覆盖rect。
                else  // 条件不成立时采用下面另一种状态更新。
                    w->rect = w->restore;  // 退出最大化时恢复原位置和大小。
                return;  // 本次事件处理已结束，不再落入其他按钮或底层窗口。
            }
            if (x > r.x + r.w - 102) {  // 再向左命中最小化按钮。
                w->minimized = true;  // 保留open与rect，但暂不绘制或接受命中。
                s->active = -1;  // 当前没有活动窗口，不能继续往刚关闭窗口发送键入。
                return;  // 本次事件处理已结束，不再落入其他按钮或底层窗口。
            }
            if (phase == 3) {  // 双击标题栏切换最大化，不开始拖动。
                w->maximized = !w->maximized;  // 切换最大化状态。
                if (w->maximized)  // 刚进入最大化时需保留此前实际矩形。
                    w->restore = w->rect;  // 恢复位置独立保存，下一次绘图可覆盖rect。
                else  // 条件不成立时采用下面另一种状态更新。
                    w->rect = w->restore;  // 退出最大化时恢复原位置和大小。
            } else if (!w->maximized) {  // 普通按下且窗口未最大化才允许移动。
                s->drag = UI_DRAG_MOVE;  // 后续移动事件按窗口移动规则处理。
                s->drag_x = x - r.x;     // 保存按下点距左边界的偏移，防止窗口跳到鼠标处。
                s->drag_y = y - r.y;     // 保存按下点距顶部偏移。
            }
            return;  // 本次事件处理已结束，不再落入其他按钮或底层窗口。
        }
        if (!w->maximized && x > r.x + r.w - 16 && y > r.y + r.h - 16) {  // 普通窗口右下16×16区域启动缩放。
            s->drag = UI_DRAG_RESIZE;  // 后续移动修改宽高，释放结束。
            return;  // 本次事件处理已结束，不再落入其他按钮或底层窗口。
        }
        ui_apps_click(s, (enum ui_app)app, u_window_body(r),  // 未命中窗口框时将去边框/标题栏的内容矩形交给应用。
                      x, y, phase == 3);  // 屏幕点保持不变，应用按内容矩形换算局部位置。
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
void ui_scroll(struct ui_desktop *s, int rows) {  // rows有符号，向上为负、向下为正，活动视图决定存储位置。
    if (s->active == UI_TERMINAL) {  // Terminal卷动命令对象中的逻辑行。
        int n = (int)s->console->scroll + rows;  // 先有符号相加，避免unsigned向下溢出。
        s->console->scroll = n > 0 ? (unsigned)n : 0;  // 负结果限到首行零，正结果才转回unsigned。
    } else if (s->active == UI_EDITOR) {  // Editor使用文档自身行滚动量。
        int n = (int)s->editor_scroll + rows;  // 以有符号临时值计算目标行。
        s->editor_scroll = n > 0 ? (unsigned)n : 0;  // 向上不能越过文档首行。
    } else if (s->active >= 0) {  // 其他活动应用使用窗口自己的列表滚动量。
        int n = (int)s->windows[s->active].scroll + rows;  // 先转换为有符号，再加事件增量。
        s->windows[s->active].scroll = n > 0 ? (unsigned)n : 0;  // 把负数限到零后保存。
    }
}
