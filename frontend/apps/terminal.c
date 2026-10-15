#include "internal.h"  // 取得本章已经实现的应用绘图与公共文字接口。

void ui_terminal_draw(struct ui_desktop *s, struct ui_rect r) {  // 把console逻辑行拆成窗口显示行并绘制输入
    u_button((struct ui_rect){r.x + r.w - 100, r.y + r.h - 61, 84, 28}, "Clear", false, false);  // 清屏操作放在输入行右侧
    const struct airi_console *c = s->console;  // Terminal借用同一console，拆行只改显示，不改串口日志
    unsigned cols = (unsigned)(r.w - 36) / 10, visible = (unsigned)(r.h > 84 ? r.h - 84 : 0) / 24;  // 扣除边距后每字符10像素、每行24像素，得到可见列数和行数
    uint64_t seq[AIRI_CONSOLE_LINES * 4];  // 显示行索引最多1024项，每项保存来源console逻辑行号
    unsigned offset[AIRI_CONSOLE_LINES * 4], n = 0;  // offset与seq配对存来源字节偏移，n只计已经形成的显示段
    for (uint64_t i = c->total_lines - c->line_count; i < c->total_lines; ++i) {  // 按console有效逻辑行号形成窗口显示行
        unsigned len = (unsigned)airi_strlen(airi_console_line(c, i)),  // 测量第i条原始日志内容字节数，终止零不参与拆行
                 rows = len ? (len + cols - 1) / cols : 1;  // 非空日志按列数向上取整拆行，空日志仍占一条显示行
        for (unsigned j = 0; j < rows && n < AIRI_CONSOLE_LINES * 4; ++j) {  // 把当前日志分解成窗口所需的显示行，受临时索引容量限制
            seq[n] = i;  // 第n条显示行记住所属原console逻辑行号
            offset[n++] = j * cols;  // 保存该显示段在原日志中的字节偏移，再推进显示行计数
        }
    }
    unsigned maxscroll = n > visible ? n - visible : 0,  // 显示段比窗口容量多时才能回滚，最多回看多出的行数
             scroll = c->scroll < maxscroll ? c->scroll : maxscroll, end = n - scroll,  // 把请求滚动收窄到有效范围，再从尾部减滚动量得到显示终点
             begin = end > visible ? end - visible : 0;  // 显示区间起点最多向前visible行，少于一屏时从第0段开始
    u_clip((struct ui_rect){r.x + 16, r.y + 12, r.w - 32, r.h - 82});  // 把后续文字或像素限制在本次显式矩形内，长内容在边界停止写入
    for (unsigned i = begin; i < end; ++i) {  // 遍历本次显示区间，滚动前后的行保留在原状态
        char row[129];  // 显示段最多128内容字节，再留1字节NUL，原日志保持完整
        const char *src = airi_console_line(c, seq[i]) + offset[i];  // 按逻辑行号借用仍保留的日志文字
        unsigned len = (unsigned)airi_strlen(src);  // 当前来源指针之后的日志字节数，下一步收窄到本段
        if (len > cols)  // 当前日志片段超过窗口字符列数时只显示一段
            len = cols;  // 当前显示段最多包含窗口能容纳的等宽字符列数
        if (len > 128)  // 局部显示数组最多装128内容字节，额外一格保存NUL
            len = 128;  // 局部显示数组最多装128内容字节，给NUL留最后一格
        airi_memcpy(row, src, len);  // 按字节复制当前有效数据，复制长度由当前对象内容或容量计算
        row[len] = 0;  // 给复制到局部显示行的有效字节补NUL，原日志不截断
        u_text(r.x + 18, r.y + 14 + (int)(i - begin) * 24, row, U_CODE,  // 在内容左缘右移18像素、整屏y=r.y + 14 + (int)(i - begin) * 24显示本次拆出的显示文字行，使用等宽字形
               ui_app_match(row, "airi>") ? U_BLUE : U_TEXT);  // 当前日志显示段含airi>时用蓝色突出提示符，否则使用普通文字色
    }
    u_clip(r);  // 恢复当前应用内容裁剪，后续列或控件可使用完整内容区
    u_rect(r.x, r.y + r.h - 68, r.w, 1, U_EDGE);  // 用分隔线色写矩形，起点(r.x,r.y + r.h - 68)，尺寸r.w×1像素
    u_text(r.x + 18, r.y + r.h - 54, "airi>", U_CODE, U_BLUE);  // 在内容左缘右移18像素、内容底边上移54像素显示文字"airi>"，使用等宽字形
    unsigned cap = cols > 18 ? cols - 18 : 1,  // 为提示符及Clear按钮合计保留18列，剩余列数用于尚未提交命令
             start = c->input_length > cap ? c->input_length - cap : 0;  // 输入过宽时显示末尾cap个字符，完整输入仍保存在console
    u_text(r.x + 78, r.y + r.h - 54, c->input + start, U_CODE, U_TEXT);  // 在内容左缘右移78像素、内容底边上移54像素显示c->input + start，使用等宽字形
    u_rect(r.x + 78 + (int)(c->input_length - start) * 10, r.y + r.h - 33, 8, 1, U_BLUE);  // 输入尾端每字符前进10像素，在最后一个可见字符之后画8像素宽的光标短线
    ui_app_status(r, "Local input / US keyboard",  // 在内容底部显示文字"Local input / US keyboard"；右侧说明按测得的文字宽度对齐
                  s->backend.mode == UI_REFERENCE ? "Local commands"  // 参考模式说明命令只观察本进程内状态，不提供真实程序执行
                                                  : "COM1 / 115200 / polled I/O");  // 真实内核后端页脚报告当前串口设备模式
}

void ui_terminal_click(struct ui_desktop *s, struct ui_rect r, int x, int y, bool twice) {  // Terminal内容点击只需命中清屏按钮。
    (void)twice;  // 统一点击接口保留双击参数；这个应用当前只使用点击位置。
    if (u_inside((struct ui_rect){r.x + r.w - 100, r.y + r.h - 61, 84, 28}, x, y))  // 命中底部输入行右侧的Clear按钮矩形时清屏。
        airi_console_key(s->console, AIRI_KEY_CLEAR);  // 复用命令对象的清屏规则，不删除另一份日志。
    return;  // 本次事件处理已结束，不再落入其他按钮或底层窗口。
}

