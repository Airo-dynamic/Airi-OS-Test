#include "internal.h"  // 引入当前目录内部共享接口和基础函数

void ui_editor_draw(struct ui_desktop *s, struct ui_rect r) {  // 将文档字节偏移换成行列，画文字、选区和光标
    ui_app_toolbar(r, "");  // 在内容顶端铺44像素工具栏并显示文字""
    u_button((struct ui_rect){r.x + 12, r.y + 8, 68, 28}, "Save", s->dirty, false);  // 画文字"Save"按钮；其矩形供后续事件命中共用，当前仅输出像素
    u_button((struct ui_rect){r.x + 90, r.y + 8, 68, 28}, "Find", false, false);  // 画文字"Find"按钮；其矩形供后续事件命中共用，当前仅输出像素
    u_button((struct ui_rect){r.x + 168, r.y + 8, 68, 28}, "Undo", false, false);  // 画文字"Undo"按钮；其矩形供后续事件命中共用，当前仅输出像素
    u_button((struct ui_rect){r.x + r.w - 96, r.y + 8, 80, 28}, "Run  F5", true, false);  // 画文字"Run  F5"按钮；其矩形供后续事件命中共用，当前仅输出像素
    ui_app_muted(r.x + 258, r.y + 15, s->dirty ? "Unsaved changes" : "Saved");  // 在内容左缘右移258像素、内容顶边下移15像素显示按当前状态选择的文案
    int tree = r.w >= 800 ? 190 : 0, x = r.x + tree + 1, w = r.w - tree - 2;  // 宽窗口保留190像素项目栏，正文另扣左右边框各1像素
    if (tree) {  // 窗口够宽时为Editor项目文件列表保留190像素
        u_rect(r.x, r.y + 44, tree, r.h - 71, U_BG);  // 用画布背景色写矩形，起点(r.x,r.y + 44)，尺寸tree×r.h - 71像素
        u_label(r.x + 16, r.y + 61, "PROJECT", U_MUTED);  // 在内容左缘右移16像素、内容顶边下移61像素显示项目导航标签PROJECT
        u_text(r.x + 16, r.y + 88, "telemetry", U_LABEL, U_TEXT);  // 当前项目作为导航对象
        u_rect(r.x + 16, r.y + 118, tree - 32, 1, U_EDGE);  // 项目与文件列表共享左缘
        for (unsigned i = 0; i < s->file_count && i < 12; ++i) {  // 只遍历本次目录快照有效前缀，未用槽不参与
            int y = r.y + 132 + (int)i * 30;     // 项目列表第i项按30像素行距排列，原快照次序保持
            if (s->files[i].id == s->editor_id)  // 项目项的服务id等于当前编辑文件id时画选择背景
                u_selection((struct ui_rect){r.x + 8, y - 3, tree - 16, 28});  // 用选中背景色写矩形，起点(r.x + 8,y - 3)，尺寸tree - 16×28像素
            u_icon(r.x + 16, y + 1, s->files[i].directory ? UI_FILES_APP : UI_EDITOR, U_MUTED);  // 文件与目录采用同一套符号
            u_clip((struct ui_rect){r.x + 46, y, tree - 54, 27});  // 把后续文字或像素限制在本次显式矩形内，长内容在边界停止写入
            ui_app_text(r.x + 46, y, s->files[i].name);  // 在内容左缘右移46像素、当前列表行y显示这条快照的文件名
            u_clip(r);  // 恢复当前应用内容裁剪，后续列或控件可使用完整内容区
        }
        u_rect(r.x + tree - 1, r.y + 44, 1, r.h - 71, U_EDGE);  // 用分隔线色写矩形，起点(r.x + tree - 1,r.y + 44)，尺寸1×r.h - 71像素
    }
    u_rect(x, r.y + 44, w, 32, U_PANEL);  // 用工具栏底色写矩形，起点(x,r.y + 44)，尺寸w×32像素
    u_rect(x, r.y + 74, w, 1, U_EDGE);    // 文档条与代码正文的边界
    u_rect(x + 14, r.y + 74, 150, 2, U_ACCENT);     // 用当前主题强调色标记写矩形，起点(x + 14,r.y + 74)，尺寸150×2像素
    ui_app_text(x + 20, r.y + 51, s->editor_name);  // 标签显示当前实际文件名
    if (w > 400)  // 文档条足够宽才显示编码标签，避免覆盖文件名
        u_label(x + w - 140, r.y + 53, "SOURCE / UTF-8", U_MUTED);  // 在文档条右侧显示当前文本范围标签
    u_rect(x + 46, r.y + 85, 1, r.h - 117, U_EDGE);  // 行号与正文的连续边界
    int top = r.y + 88 + (s->find ? 44 : 0);  // 正文起始行扣工具栏和文档标签，查找打开再下移44像素
    if (s->find) {  // 查找栏打开时在正文上方绘制独立短字段
        ui_app_field((struct ui_rect){x + 16, r.y + 85, w - 32, 32},  // 画输入字段和当前焦点边框，本次实参取自当前桌面状态
                     s->find_text[0] ? s->find_text : "Find in document / Enter to select",  // 查找字段有内容就显示它，否则显示回车选择命中项的提示
                     s->focus == UI_FOCUS_FIND);  // 查找输入拥有焦点时画强调边框，文档正文继续保留但不接收同一事件
    }
    u_clip((struct ui_rect){x + 8, top, w - 16, r.h - (top - r.y) - 30});  // 把后续文字或像素限制在本次显式矩形内，长内容在边界停止写入
    const char *p = s->editor;   // 借用当前编辑副本首字节，绘图遍历不改变文字
    unsigned line = 0, idx = 0;  // line计当前逻辑行，idx计当前行首相对文档首字节偏移
    int y = top;  // 首个可见文档行从已计算的正文起点开始绘制
    while ((*p || idx == s->cursor) && y < r.y + r.h - 32) {  // 文档仍有字节或文尾有光标时继续处理行，同时让行起点停在状态栏上方
        const char *end = p;  // 从当前行首向后寻找换行或终止零，p仍保留原行首
        while (*end && *end != '\n')  // 从当前行首向后找换行或文尾，得到本行半开终点
            ++end;  // 扫描位置向后一字节，直至当前文档行末
        if (line++ >= s->editor_scroll) {  // 先按旧行号判断是否已经滚到可见区，再增加逻辑行号供行号显示
            char number[16];  // 16字节局部数组把一起算的文档行号转换成文字
            airi_snprintf(number, 16, "%u", line);  // 绘图行号从1开始，把当前逻辑行号形成16字节短文字
            u_text(x + 12, y, number, U_MONO, U_MUTED);  // 在整屏x=x + 12、整屏y=y显示当前文档行号，使用等宽字形
            u_clip((struct ui_rect){x + 60, top, w - 70, r.h - (top - r.y) - 30});  // 把后续文字或像素限制在本次显式矩形内，长内容在边界停止写入
            int tx = x + 60 - (int)s->editor_column_scroll * 10;  // 正文左边留60像素给行号，水平滚动每列左移10像素
            bool comment = false;  // 每行先处于普通代码状态，双字符前缀才开启注释着色
            char quote = 0;  // 本行尚无打开引号，0表示不在简化字符串着色中
            for (const char *q = p; q < end; ++q) {  // 只遍历当前文档行的内容字节，不把换行画成字形
                unsigned at = idx + (unsigned)(q - p);  // 行首文档偏移加当前字符相对行首距离，得到全篇字节下标
                bool selected = s->selection &&  // 只在有有效选区时继续判断当前字节是否落在两端之间
                                at >= (s->cursor < s->anchor ? s->cursor : s->anchor) &&  // 两选区端点取较小值，当前字节至少等于左端
                                at < (s->cursor > s->anchor ? s->cursor : s->anchor);  // 选区右端排除，大于等于较小端且小于较大端才选中
                if (selected)  // 当前字节落在选区内时先铺选中背景
                    u_rect(tx, y, 10, 23, U_RAISED);  // 用选中背景色写矩形，起点(tx,y)，尺寸10×23像素
                char ch[2] = {*q, 0};  // 当前ASCII字符加NUL形成独立短串，文字绘图不读后面的文档字节
                if (!quote && q + 1 < end &&  // 只有引号外且还有后一字节时才检查双字符注释前缀
                    ((*q == '-' && q[1] == '-') || (*q == '/' && q[1] == '/')))  // 识别Lua双横线和C双斜杠，本次只作行内颜色提示
                    comment = true;  // 后面的同行字符都按注释颜色画，不执行或解析这段代码
                bool opening = false;  // 先记录当前字符尚未新开引号，避免同一引号立即被关闭
                if (!comment && !quote && (*q == '"' || *q == '\'')) {  // 非注释且无打开引号时，当前单或双引号开始字符串着色
                    quote = *q;      // 保存本次打开的单或双引号种类，等待同种非转义引号关闭
                    opening = true;  // 当前引号刚打开，本轮关闭判断必须跳过它
                }
                uint32_t color = comment                    ? U_MUTED  // 优先选注释色，随后再按字符串和数字决定其他字符颜色
                                 : quote                    ? U_BLUE  // 打开引号内的字符采用字符串颜色
                                 : (*q >= '0' && *q <= '9') ? U_PINK  // 注释和引号之外的数字字符采用品牌粉色
                                                            : U_TEXT;  // 其余普通代码字符使用主文字色
                if (quote && !opening && *q == quote && (q == p || q[-1] != '\\'))  // 同种非转义引号且不是本轮刚打开时结束简化字符串着色
                    quote = 0;  // 对应闭引号已遇到，后续字符恢复普通代码着色
                u_text(tx, y, ch, U_CODE, color);  // 在整屏x=tx、整屏y=y显示当前单字节字符与NUL，使用等宽字形
                if (at == s->cursor)  // 当前字符之前的间隙就是光标位置时画一像素竖线
                    u_rect(tx, y + 3, 1, 19, U_BLUE);  // 用当前主题强调色标记写矩形，起点(tx,y + 3)，尺寸1×19像素
                tx += 10;  // 等宽ASCII字符下一列右移10像素
            }
            if (idx + (unsigned)(end - p) == s->cursor)  // 光标在当前行最后字符后面时仍需画文尾竖线
                u_rect(tx, y + 3, 1, 19, U_BLUE);  // 用当前主题强调色标记写矩形，起点(tx,y + 3)，尺寸1×19像素
            y += 24;  // 当前可见文档行完成，下一行下移24像素
            u_clip((struct ui_rect){x + 8, top, w - 16, r.h - (top - r.y) - 30});  // 把后续文字或像素限制在本次显式矩形内，长内容在边界停止写入
        }
        idx += (unsigned)(end - p);  // 当前行的内容长度加入文档字节偏移，换行随后另计
        if (*end) {  // 本行由换行结束而非NUL时，跳过换行进入下一行
            p = end + 1;  // 跳过换行字节后，把下一行第一个字符作为新的行首
            ++idx;  // 将换行字节也计入下一行首的文档偏移
        } else  // 前一分支条件未成立时转入随后的单条替代操作
            break;  // 结束当前最近一层循环或应用分派，不执行其剩余候选
    }
    if (!s->editor[0]) {  // 文档为空时仍在第0间隙画光标并给出输入提示
        u_rect(x + 60, top + 3, 1, 19, U_BLUE);  // 用当前主题强调色标记写矩形，起点(x + 60,top + 3)，尺寸1×19像素
        ui_app_muted(x + 76, top, "Start typing a document.");  // 在整屏x=x + 76、整屏y=top显示文字"Start typing a document."
    }
    u_clip(r);  // 恢复当前应用内容裁剪，后续列或控件可使用完整内容区
    unsigned ln = 1, col = 1;  // 状态栏行列从1开始，内部字节和显示滚动下标仍从0开始
    for (unsigned i = 0; i < s->cursor && s->editor[i]; ++i)  // 扫描光标前的ASCII字节，换行决定行列转换
        if (s->editor[i] == '\n') {  // 扫描到光标前的换行时增加状态行号并复位列号
            ++ln;     // 换行后状态栏的一起算行号增加一
            col = 1;  // 状态栏下一行的首个字符间隙从第1列显示
        } else  // 前一分支条件未成立时转入随后的单条替代操作
            ++col;  // 非换行字符使当前列增加一
    char right[96];  // 96字节局部文字保存光标行列、编码范围和内容长度说明
    airi_snprintf(right, 96, "Ln %u, Col %u   /   UTF-8   /   %u B", ln, col,  // Editor状态栏显示1起算的光标行列与内容字节数；本阶段输入仅支持ASCII子集
                  (unsigned)airi_strlen(s->editor));  // 正文最多8191字节，长度转 unsigned 匹配状态栏 %u
    ui_app_status(r,  // 在应用内容底部绘制27像素状态栏，本次实参取自当前桌面状态
                  s->backend.mode == UI_REFERENCE  // 按实际绑定后端模式选择后续参考说明或内核说明，不修改mode字段
                      ? "Session storage"  // 参考服务页脚说明保存只改变当前主机会话内存
                      : "File service offline / buffer only",  // 未连接真实文件服务时说明当前只有前端编辑缓冲
                  right);  // 把当前光标行列与文档字节数写到状态栏右侧
}

