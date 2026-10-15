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

unsigned ui_editor_position(const struct ui_desktop *s, struct ui_rect r, int x, int y) {  // 只把同一画布的点换成文档字节间隙，不执行按钮或修改状态。
    int tree = r.w >= 800 ? 190 : 0;  // r已是内容矩形；是否有项目栏按绘图使用的这份宽度判断。
    struct ui_rect text = {  // 与Editor字形裁剪共用正文左缘、起点和右下边界。
        r.x + tree + 61, r.y + 88 + (s->find ? 44 : 0),  // 正文先扣项目栏、行号和一像素边缘，查找展开再下移44像素。
        r.w - tree - 72, r.h - 118 - (s->find ? 44 : 0)  // 宽度扣行号与边距，高度扣前88与底30像素。
    };
    if (text.w <= 0 || text.h <= 0)  // 极小内容矩形没有可见正文，不能换算行列。
        return s->cursor;  // 保留当前合法间隙，不产生负尺寸转换后的大下标。
    if (x < text.x)  // 拖到行号栏或按钮左侧时，定位到当前可见文字左缘。
        x = text.x;  // 当前事件只改变本函数的坐标副本。
    if (x >= text.x + text.w)  // 拖出右边时，最后可见列由正文裁剪右界决定。
        x = text.x + text.w - 1;  // 半开右边不属于正文，收回到最后一个允许像素。
    if (y < text.y)  // 拖到Save等工具栏上方时，只定位到正文顶行。
        y = text.y;  // 不把这个点重新送进工具栏点击分派。
    if (y >= text.y + text.h)  // 拖出正文底部时仍使用最后可见文字行。
        y = text.y + text.h - 1;  // 同样收回到半开底边之前。
    unsigned target = (unsigned)(y - text.y) / 24 + s->editor_scroll;  // 可见行加已卷走行数，得到文档目标行。
    unsigned col = (unsigned)(x - text.x) / 10 + s->editor_column_scroll;  // 可见列加水平滚动，得到文档目标列。
    unsigned pos = 0;  // 从文档首字节扫描，不能把屏幕列直接当全文偏移。
    for (unsigned line = 0; line < target && s->editor[pos]; ++line) {  // 逐行跨过换行，NUL阻止读过文尾。
        while (s->editor[pos] && s->editor[pos] != '\n')  // 先找到当前逻辑行的换行或文尾。
            ++pos;  // 当前字节存在，扫描位置可以前进一格。
        if (s->editor[pos])  // 确实停在换行时才跨到下一行。
            ++pos;  // 换行也占一个文档字节。
    }
    while (col && s->editor[pos] && s->editor[pos] != '\n') {  // 短行把目标列限制在本行末间隙。
        --col;  // 消费一个待前进列，不使用递减后的值作数组下标。
        ++pos;  // 一个ASCII列对应一个文档字节。
    }
    return pos;  // 调用者决定怎样使用这个间隙；此函数不保存、不改选区。
}

void ui_editor_click(struct ui_desktop *s, struct ui_rect r, int x, int y, bool twice) {  // 将 Editor 内容区屏幕坐标转换为工具栏动作或文档字节间隙。
    (void)twice;  // 统一点击接口保留双击参数；这个应用当前只使用点击位置。
    if (y < r.y + 44) {  // 内容顶部44像素属于工具栏，先处理按钮而不选中文件行。
        if (x < r.x + 84)  // Editor 工具栏最左按钮保存当前文档。
            ui_key(s, UI_SAVE_KEY, false);  // 送入已实现的保存事件，不重新写一套保存规则。
        else if (x < r.x + 162)  // 继续向右命中查找按钮。
            ui_key(s, UI_FIND_KEY, false);  // 显示查找字段并将键盘焦点转向它。
        else if (x < r.x + 242)  // 工具栏下一块命中撤销按钮。
            ui_key(s, UI_UNDO, false);  // 使用同一份 undo 状态切换前后文本。
        else if (x > r.x + r.w - 108)  // 内容最右108像素命中 Run。
            ui_key(s, UI_RUN_KEY, false);  // 请求执行当前文档；参考服务仍会明确报告未连接。
        return;  // 本次事件处理已结束，不再落入其他按钮或底层窗口。
    }
    int tree = r.w >= 800 ? 190 : 0;  // 宽视图保留190像素文件树，窄视图不显示树。
    if (tree && x < r.x + tree && y >= r.y + 132) {  // 文件树存在且命中其行区域时，按树中的记录打开。
        unsigned row = (unsigned)(y - r.y - 132) / 30;  // 扣掉树表头，按30像素一行得到快照行号。
        if (row < s->file_count)  // 打开前确认下标在有效快照范围内。
            ui_open_document(s, row);  // 树直接使用快照顺序，与 Files 排序行的转换不同。
        return;  // 本次事件处理已结束，不再落入其他按钮或底层窗口。
    }
    if (s->find && y >= r.y + 85 && y < r.y + 120) {  // 查找栏开启时，命中它不应改变正文光标。
        s->focus = UI_FOCUS_FIND;  // 后续字符修改查找文本。
        return;  // 本次事件处理已结束，不再落入其他按钮或底层窗口。
    }
    int top = r.y + 88 + (s->find ? 44 : 0);  // 当前正文起点与绘图相同，查找栏额外占44像素。
    if (y >= top && y < r.y + r.h - 30) {     // 普通点击必须在正文高度内，才改变文档位置。
        s->cursor = ui_editor_position(s, r, x, y);  // 复用只读定位器，坐标夹取与行列扫描只产生间隙。
        s->anchor = s->cursor;  // 普通点击从同一间隙重新建立可能的选择起点。
        s->selection = false;   // 当前只是定位，持续拖动层随后决定选区。
    }
    return;  // 本次事件处理已结束，不再落入其他按钮或底层窗口。
}

