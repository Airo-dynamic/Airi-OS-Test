# G20 · 点击文件并在 Editor 拖出选区

Files 显示排序后的目录，Editor 已能修改文档。现在让主机观察程序发送一组鼠标事件：双击 main.lua 打开文档，在正文按下并拖出选区，释放结束。每次事件提供整屏 x/y 与阶段值，桌面先逆着绘制顺序找到顶层窗口，再把它的当前 body 交给 Files 或 Editor。真实平台尚未接入，但这组确定事件已能走完整的命中与修改路径。

<a id="pointer-routing"></a>

排序后的可见行先转换为快照下标，打开文档继续使用记录 id；屏幕坐标、快照位置和文件身份是三次不同的定位。绘图与点击共用当前窗口 rect，移动后的窗口不能沿用旧位置。

Files 的一次内容事件先判断是否在工具栏：Up 改当前目录并刷新，Refresh 恢复正常查询，Create 只打开名称框等待确认。其他位置再进入侧栏、筛选、表头或底部操作；这些分支处理后都立即返回，不能又被当成文件行。只有剩余列表区域才计算行号，经当前排序表取得快照下标；双击再用记录 id 打开。

Files与Editor现在开始接收整屏坐标。先加入各自的click入口，实参r是已扣标题的当前内容矩形，x/y是同一画布中的鼠标位置，twice标识双击；后面的分派按这些声明调用它们。

在 apps/internal.h 中以 Files、Editor 绘图声明定位旧窗口，加入两个点击入口及只读定位器。用新块替换该窗口，其余应用声明保留。

<a id="code-frontend-apps-internal-h-1"></a>

<!-- replace: frontend/apps/internal.h -->
<!-- before -->
```c
void ui_terminal_draw(struct ui_desktop *, struct ui_rect);  // 把console逻辑行拆成窗口显示行并绘制输入
void ui_files_draw(struct ui_desktop *, struct ui_rect);     // 读取目录快照和排序索引，画路径、文件行和状态
void ui_editor_draw(struct ui_desktop *, struct ui_rect);    // 将文档字节偏移换成行列，画文字、选区和光标
void ui_tasks_draw(struct ui_desktop *, struct ui_rect);     // 绘制任务快照、CPU曲线和内存条
void ui_system_draw(struct ui_desktop *, struct ui_rect);    // 按当前页显示同一启动信息的内存、设备或日志
```
<!-- after -->
```c
void ui_terminal_draw(struct ui_desktop *, struct ui_rect);  // 把console逻辑行拆成窗口显示行并绘制输入
void ui_files_draw(struct ui_desktop *, struct ui_rect);     // 读取目录快照和排序索引，画路径、文件行和状态
void ui_files_click(struct ui_desktop *, struct ui_rect, int, int, bool);  // 按整屏x/y取得筛选、工具栏或可见文件行；twice为双击标记
void ui_editor_draw(struct ui_desktop *, struct ui_rect);  // 将文档字节偏移换成行列，画文字、选区和光标
unsigned ui_editor_position(const struct ui_desktop *, struct ui_rect, int, int);  // 只将内容矩形中的屏幕点夹到正文并返回字节间隙，供点击和续拖共用。
void ui_editor_click(struct ui_desktop *, struct ui_rect, int, int, bool);  // 按当前正文几何将整屏x/y换成文档间隙，或分派工具栏动作
void ui_tasks_draw(struct ui_desktop *, struct ui_rect);     // 绘制任务快照、CPU曲线和内存条
void ui_system_draw(struct ui_desktop *, struct ui_rect);    // 按当前页显示同一启动信息的内存、设备或日志
```

**追加到 `frontend/apps/files.c` 末尾：**

<a id="code-frontend-apps-files-c-1"></a>

<!-- append: frontend/apps/files.c -->
```c
void ui_files_click(struct ui_desktop *s, struct ui_rect r, int x, int y, bool twice) {  // s 是当前桌面，r 是 Files 内容矩形，x/y 是屏幕坐标，twice 表示双击。
    if (y < r.y + 44) {  // 内容顶部44像素属于工具栏，先处理按钮而不选中文件行。
        if (x < r.x + 60) {  // 工具栏左端60像素以内命中 Up。
            char *p = s->directory;  // p 借用目录字符串数组，后续写入直接改变当前目录。
            unsigned n = (unsigned)airi_strlen(p);  // 目录字节数变成数组下标；现有128字节容量保证可放入 unsigned。
            while (n && p[n - 1] != '/')  // 先确认 n 非零，再检查末字节，避免 n-1 下溢和越界。
                --n;  // 向前寻找最后一个斜杠，跳过末级目录名。
            if (n > 1)  // 最后一个斜杠不在根目录开头时，删除这一级。
                p[n - 1] = 0;  // 用 NUL 截断到父目录，字符串剩余字节不再参与路径。
            else  // 当前路径已在根目录，保留开头斜杠
                p[1] = 0;  // 保留开头的 /，把根目录表示为单字符字符串。
            ui_refresh(s);  // 按更新后的目录及服务状态重新取得文件与任务快照。
        } else if (x > r.x + r.w - 116) {  // 从内容右缘向左116像素以内命中刷新按钮。
            s->service_state = UI_NORMAL;  // 恢复普通查询状态，再由刷新回调决定实际结果。
            ui_refresh(s);  // 按更新后的目录及服务状态重新取得文件与任务快照。
        } else if (x > r.x + r.w - 210)  // 继续向左的按钮区用于新建目录。
            ui_app_dialog(s, UI_DIALOG_NEW_DIR, 0, "New folder", "Create a folder in the current directory.",  // 准备目录确认框；目标 id=0 表示本次创建尚无已有文件身份。
                          "new-folder");  // 给输入框填入初始名称，确认后才创建。
        else if (x > r.x + r.w - 304)  // 再向左的按钮区用于新建普通文件。
            ui_app_dialog(s, UI_DIALOG_NEW_FILE, 0, "New file", "Create a text document in the current directory.",  // 建立文件创建确认框，复用当前目录作为操作目标。
                          "untitled.lua");  // 初始文件名由用户继续编辑或确认。
        return;  // 本次事件处理已结束，不再落入其他按钮或底层窗口。
    }
```

工具栏分支结束后，继续处理 Files 的当前内容几何。176 像素侧栏只在 body 宽至少800时存在；错误界面的 Retry、快捷目录、筛选字段和表头各有自己的返回出口。快捷目录改的是参考服务查询路径，不会访问主机磁盘；表头只切换排序方向。继续追加同一个点击函数。

<!-- append: frontend/apps/files.c -->
```c
    int side = r.w >= 800 ? 176 : 0, xx = r.x + side + 16;  // 宽窗口预留176像素侧栏；xx 是主内容左边界加16像素内边距。
    if (s->service_state == UI_ERROR &&  // 仅在服务失败界面检测重试按钮。
        u_inside((struct ui_rect){xx + 72, r.y + 277, 92, 32}, x, y)) {  // 复合字面量创建92×32按钮矩形，检查屏幕点是否落入。
        s->service_state = UI_NORMAL;  // 恢复普通查询状态，再由刷新回调决定实际结果。
        ui_refresh(s);  // 按更新后的目录及服务状态重新取得文件与任务快照。
        return;  // 本次事件处理已结束，不再落入其他按钮或底层窗口。
    }
    if (side && x < r.x + side) {  // 侧栏实际存在且点落在其宽度内时，按快捷目录处理。
        if (y < r.y + 128)  // 侧栏第一项对应主目录。
            airi_snprintf(s->directory, 128, "/home/airi");  // 写入固定目录；128同时限制可写容量与终止字节。
        else if (y < r.y + 172)  // 侧栏第二项对应项目目录。
            airi_snprintf(s->directory, 128, "/home/airi/projects");  // 切回参考项目目录，后续刷新读取这里的记录。
        else if (y < r.y + 210)  // 侧栏第三项对应程序目录。
            airi_snprintf(s->directory, 128, "/bin");  // 切到参考服务中的 /bin，不是访问宿主磁盘。
        else {  // 侧栏其余位置切到系统存储页，不改变当前目录
            ui_open(s, UI_SYSTEM);  // 打开并置顶系统窗口，继续使用这份桌面状态。
            s->windows[UI_SYSTEM].tab = 2;  // 选择系统窗口中的存储页签，下标从零开始。
        }
        ui_refresh(s);  // 按更新后的目录及服务状态重新取得文件与任务快照。
        return;  // 本次事件处理已结束，不再落入其他按钮或底层窗口。
    }
    if (y >= r.y + 58 && y < r.y + 90) {  // 内容顶部58..89像素属于文件筛选输入框。
        s->focus = UI_FOCUS_FILTER;  // 后续键入先交给当前应用的筛选字段。
        return;  // 本次事件处理已结束，不再落入其他按钮或底层窗口。
    }
    if (y >= r.y + 106 && y < r.y + 134) {  // 106..133像素属于目录表头。
        s->sort = !s->sort;  // 逻辑非把排序方向在0与1之间切换。
        return;  // 本次事件处理已结束，不再落入其他按钮或底层窗口。
    }
```

下面是底部动作区。文字之外，文件剪贴板保存服务 id：复制/剪切先登记身份，粘贴才向当前目录请求复制或移动；移动成功才清这份身份，失败可继续重试。打开使用选中快照下标，重命名与删除的确认框则固定 f->id，随后排序也不能换掉目标。继续追加底部操作。

<!-- append: frontend/apps/files.c -->
```c
    if (y > r.y + r.h - 76 && y < r.y + r.h - 28 && x >= xx + 426 && s->file_clipboard >= 0) {  // 底部粘贴按钮还要求剪贴板保存有效文件 id；-1 表示空。
        ui_command(s, s->file_cut ? UI_MOVE_FILE : UI_COPY_FILE, s->file_clipboard, s->directory,  // 剪切选移动、复制选拷贝；传入稳定文件 id 与目标目录。
                   "");  // 此动作不需要额外文本输入，传入空字符串。
        if (s->dialog == UI_DIALOG_NONE && s->file_cut)  // 只有操作没有打开错误框且是剪切，才消费待移动的身份。
            s->file_clipboard = -1;  // 清空文件剪贴板，避免同一移动被重复提交。
        return;  // 本次事件处理已结束，不再落入其他按钮或底层窗口。
    }
    if (y > r.y + r.h - 76 && y < r.y + r.h - 28 && s->file_count) {  // 底部文件操作要求快照至少有一条记录。
        struct ui_file *f = &s->files[s->selection_file];  // 按快照下标借用选中记录，f->id 才是服务操作身份。
        if (x < xx + 85)  // 底部第一个按钮打开选中文档。
            ui_open_document(s, s->selection_file);  // 文档打开接口接收快照下标，在内部取出文件 id。
        else if (x < xx + 180)  // 第二个按钮准备重命名。
            ui_app_dialog(s, UI_DIALOG_RENAME, f->id, "Rename file", "Change the selected item's name.", f->name);  // 确认框保存 id，初始输入为旧名，不受随后列表排序影响。
        else if (x < xx + 270)  // 第三个按钮准备删除。
            ui_app_dialog(s, UI_DIALOG_DELETE, f->id, "Delete item",  // 记录待删除文件 id，实际删除等待确认。
                          "Delete the selected item?", "");  // 说明删除范围是参考工作区，确认框无需名称输入。
        else if (x < xx + 338) {  // 底部复制按钮仅登记身份，暂不复制内容。
            s->file_clipboard = f->id;  // 文件剪贴板保存稳定 id，不保存可见行号。
            s->file_cut = false;  // 后续粘贴对这份身份执行复制。
        } else if (x < xx + 416) {  // 底部剪切按钮登记同一身份并改变粘贴动作。
            s->file_clipboard = f->id;  // 文件剪贴板保存稳定 id，不保存可见行号。
            s->file_cut = true;  // 后续粘贴执行移动，成功才清空身份。
        }
        return;  // 本次事件处理已结束，不再落入其他按钮或底层窗口。
    }
```

底部动作也未消费的事件才进入列表。y 先扣 body 顶下138像素的表头，再除当前28/36行高并加 scroll，得到筛选列表的行号。ui_file_order 给出可见顺序到快照下标的映射；循环只为匹配项增加 shown，命中才更新 selection_file。普通单击至此结束，twice 才继续打开同一项。

<!-- append: frontend/apps/files.c -->
```c
    if (y >= r.y + 138) {  // 从列表首行顶部开始把垂直距离转换为行号。
        unsigned row = (unsigned)(y - r.y - 138) / (s->density ? 28 : 36) +  // 距离除以当前28或36像素行高，先得到视口内行号。
                       s->windows[UI_FILES_APP].scroll,  // 加上已卷走行数，得到筛选后的全列表行号。
                 shown = 0;  // shown 只统计未被筛选掉的记录。
        unsigned order[UI_FILES];  // 数组保存排序后的快照下标，不复制文件记录。
        unsigned count = ui_file_order(s, order);  // 取得当前排序顺序及有效项数，读取只限到 count。
        for (unsigned k = 0; k < count; ++k) {     // 按排序位置逐项检查，k 不是服务 id。
            unsigned i = order[k];  // 从排序位置取回原快照下标。
            if (!ui_app_match(s->files[i].name, s->filter[UI_FILES_APP]))  // 名称不匹配筛选时，这条记录不占可见行。
                continue;  // 跳过当前不适用项，继续检查下一项。
            if (shown++ == row) {  // 先比较当前可见行，再递增计数；命中时 i 就是原记录下标。
                s->selection_file = i;  // 持久选择保存快照下标，重绘时仍能取得同一记录。
                if (twice)  // 仅双击才立即打开，普通点击只改变选择。
                    ui_open_document(s, i);  // 以命中快照下标读取对应文件，进入 Editor 或 Viewer。
                break;  // 当前匹配已完成，停止这一层循环或分支。
            }
        }
    }
    return;  // 本次事件处理已结束，不再落入其他按钮或底层窗口。
}

```

Editor 工具栏将保存、查找、撤销和 Run 转交 ui_key，复用已有状态规则。正文点击先扣掉侧栏与行号宽度，换算成视口行列后加上滚动量，再扫描文档找到对应行列，cursor 因而落在字符间隙。

<a id="text-position"></a>

普通点击需要区分工具栏和正文，持续拖选却已经确定在操作文字。如果移动时再调用整个Editor点击函数，指针进入Save便会真的保存。因此先写一个只返回字节间隙的ui_editor_position，再让两条入口复用它。它不接收服务动作，不修改正文或选择。

它的r已经是body。外框宽800得到body宽798，绘图不会放项目栏，定位也不能按外框误扣190像素。正文左缘为body.x+tree+61，顶部为body.y+88，右下边界与当前文字裁剪相同；查找打开时顶部再下移44。

移出范围的点收回到最近边界像素，再按10像素一列、24像素一行换算，加滚动量后扫描字符串。换行推进文档行号，目标列超过短行末尾时就停在换行或NUL之前，因此返回的始终是合法间隙。拖到Save时只会定位到正文顶行，不进入按钮分派。

**追加到 `frontend/apps/editor.c` 末尾：**

<a id="code-frontend-apps-editor-c-1"></a>

<!-- append: frontend/apps/editor.c -->
```c
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

```

定位器现在只返回合法字节间隙，不执行动作。普通点击入口还要先辨认工具栏、项目树与查找字段：工具栏复用 ui_key，项目树按原快照顺序打开文件，查找字段只取得焦点。真正正文点击才调用定位器，并把 cursor、anchor 放到同一间隙，selection=false；拖动层随后决定是否扩成选区。继续追加独立点击函数。

<!-- append: frontend/apps/editor.c -->
```c
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

```

文本选择要跨越多个事件：按下确定 anchor，移动只改变 cursor，释放结束。ui_pointer 返回后，这些状态仍须留在 desktop，所以加入 mouse_x/mouse_y 和 drag，并声明统一指针入口。下面依次替换 ui.h 的唯一旧窗口，现有文字副本与选区字段继续保留。

依次更新 ui.h 的拖动枚举、桌面指针字段和入口声明。旧块用于定位，只写回新块。

<a id="code-frontend-include-ui-h-1"></a>

<!-- replace: frontend/include/ui.h -->
<!-- before -->
```c
enum ui_dialog { UI_DIALOG_NONE, UI_DIALOG_ERROR, UI_DIALOG_CLOSE, UI_DIALOG_NEW_FILE, UI_DIALOG_NEW_DIR, UI_DIALOG_RENAME, UI_DIALOG_DELETE, UI_DIALOG_STOP, UI_DIALOG_SAVE_AS, UI_DIALOG_INFO };  // 对话框类型决定输入、确认与取消后续动作；NONE为零。
enum ui_focus { UI_FOCUS_NONE, UI_FOCUS_FILTER, UI_FOCUS_FIND };  // 键入目标为应用正文、筛选字段或查找字段。
enum ui_drag { UI_DRAG_NONE = -1 };  // 尚无指针拖动，初始化值表示无操作
enum ui_key {  // 功能键编号与ASCII字节分开，避免把命令误插入正文。
    UI_LEFT = 512,  // 功能键从512起，不与0..127的文本字节混淆。
```
<!-- after -->
```c
enum ui_dialog { UI_DIALOG_NONE, UI_DIALOG_ERROR, UI_DIALOG_CLOSE, UI_DIALOG_NEW_FILE, UI_DIALOG_NEW_DIR, UI_DIALOG_RENAME, UI_DIALOG_DELETE, UI_DIALOG_STOP, UI_DIALOG_SAVE_AS, UI_DIALOG_INFO };  // 对话框类型决定输入、确认与取消后续动作；NONE为零。
enum ui_focus { UI_FOCUS_NONE, UI_FOCUS_FILTER, UI_FOCUS_FIND };  // 键入目标为应用正文、筛选字段或查找字段。
enum ui_drag { UI_DRAG_NONE = -1, UI_DRAG_SELECT = 3 };  // 按下到释放之间仅续接Editor文本拖选
enum ui_key {  // 功能键编号与ASCII字节分开，避免把命令误插入正文。
    UI_LEFT = 512,  // 功能键从512起，不与0..127的文本字节混淆。
```

<!-- replace: frontend/include/ui.h -->
<!-- before -->
```c
    unsigned order[UI_APP_COUNT];  // 保存从底到顶的应用身份，命中逆向读取。
    unsigned order_count;  // order中已登记的有效数量。
    int active;    // 活动应用，-1表示当前没有。
    int width;     // 最近绘图的桌面宽度，单位像素。
    int height;    // 最近绘图的桌面高度，单位像素。
    bool monitor;  // 右侧状态监视区是否展开。
    bool find;     // Editor查找栏是否展开。
```
<!-- after -->
```c
    unsigned order[UI_APP_COUNT];  // 保存从底到顶的应用身份，命中逆向读取。
    unsigned order_count;  // order中已登记的有效数量。
    int active;   // 活动应用，-1表示当前没有。
    int width;    // 最近绘图的桌面宽度，单位像素。
    int height;   // 最近绘图的桌面高度，单位像素。
    int mouse_x;  // 当前鼠标屏幕横坐标。
    int mouse_y;  // 当前鼠标屏幕纵坐标。
    int drag;     // 当前跨事件拖动种类，-1表示无。
    bool monitor;  // 右侧状态监视区是否展开。
    bool find;     // Editor查找栏是否展开。
```

<!-- replace: frontend/include/ui.h -->
<!-- before -->
```c
void ui_open(struct ui_desktop *, enum ui_app);  // 打开、解除最小化并置顶指定应用。
void ui_draw(struct ui_desktop *, const struct airi_framebuffer *);  // 按实际帧缓冲绘图，并更新可用于命中的窗口矩形。
bool ui_key(struct ui_desktop *, int key, bool shift);      // 分派字符或功能键，Shift控制扩展选区等规则。
void ui_service_state(struct ui_desktop *, enum ui_state);  // 切换服务显示状态并刷新对应快照。
```
<!-- after -->
```c
void ui_open(struct ui_desktop *, enum ui_app);  // 打开、解除最小化并置顶指定应用。
void ui_draw(struct ui_desktop *, const struct airi_framebuffer *);  // 按实际帧缓冲绘图，并更新可用于命中的窗口矩形。
void ui_pointer(struct ui_desktop *, int x, int y,  // 鼠标接口接收同一桌面屏幕坐标。
                int phase);  /* 0 move, 1 down, 2 up, 3 double-click */  // phase把平台事件统一成移动/按下/释放/双击。
bool ui_key(struct ui_desktop *, int key, bool shift);      // 分派字符或功能键，Shift控制扩展选区等规则。
void ui_service_state(struct ui_desktop *, enum ui_state);  // 切换服务显示状态并刷新对应快照。
```

在 builtin.c 的内容点击 switch 中加入 Files、Editor，其他应用分派保持原样。替换下方唯一旧窗口。

<a id="code-frontend-apps-builtin-c-1"></a>

<!-- replace: frontend/apps/builtin.c -->
<!-- before -->
```c
                   bool twice) {  // 双击标志保持原值交给应用，不在桌面重复解释。
    s->focus = UI_FOCUS_NONE;  // 先取消旧字段焦点，应用命中字段后可重新设置。
    switch (app) {     // 按已命中的应用编号选择点击处理器
    case UI_SETTINGS:  // 应用身份为设置时进入对应分支。
        ui_settings_click(s, r, x, y, twice);  // 将当前矩形、屏幕点和双击状态交给设置的命中规则。
```
<!-- after -->
```c
                   bool twice) {  // 双击标志保持原值交给应用，不在桌面重复解释。
    s->focus = UI_FOCUS_NONE;  // 先取消旧字段焦点，应用命中字段后可重新设置。
    switch (app) {      // 按已命中的应用编号选择点击处理器
    case UI_FILES_APP:  // 应用身份为目录时进入对应分支。
        ui_files_click(s, r, x, y, twice);  // 把当前内容矩形和屏幕点交给 Files。
        break;  // 当前匹配已完成，停止这一层循环或分支。
    case UI_EDITOR:  // 应用身份为文档时进入对应分支。
        ui_editor_click(s, r, x, y, twice);  // 把相同事件表示交给 Editor。
        break;  // 当前匹配已完成，停止这一层循环或分支。
    case UI_SETTINGS:  // 应用身份为设置时进入对应分支。
        ui_settings_click(s, r, x, y, twice);  // 将当前矩形、屏幕点和双击状态交给设置的命中规则。
```

<a id="drag-state"></a>

`ui_pointer(s,x,y,phase)` 的 phase 为 0 表示移动、1 表示按下、2 表示释放、3 表示双击。调用者传整屏坐标，桌面保存当前位置；只有按下或双击才开始新的内容点击。拖出选区时保留按下位置 anchor，每次移动只调用正文定位器更新 cursor；位置在工具栏或正文外时先夹回可见范围。释放把 drag 清为 NONE；下一次普通移动不会继续旧选区。当前只接入正文点击，标题栏的移动、缩放和其他应用按钮下一步加入。

**完整写入 `frontend/core/pointer.c`：**

<a id="code-frontend-core-pointer-c-1"></a>

<!-- write: frontend/core/pointer.c -->
```c
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
```

前面先处理释放与已经开始的拖选，它们不重新命中工具栏。只有按下或双击才来到窗口查找：从 order 末尾逆查，跳过关闭、最小化和点外窗口，首个命中窗口成为活动窗口并置顶。标题本章先返回；内容交给对应应用后，只有当前点确在 Editor 正文裁剪内，才把这次定位的 cursor 固定成 anchor 并登记 UI_DRAG_SELECT。继续追加至函数结束。

<!-- append: frontend/core/pointer.c -->
```c
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
```

这组观察随后把同一Editor设为800×400普通外框，body宽798所以没有项目栏。将正文替换为print(42)加换行后，在第一行第1间隙按下，再移到Save按钮内部并释放。定位器把上方位置夹到正文第0间隙，anchor仍是1；dirty 保持 true，editor 和 saved 各自与操作前的独立副本逐字节相同，服务 read 也读回旧 saved。这样既确认第一批文字列可以起拖，也确认续拖没有调用保存按钮。

**完整写入 `labs/tutorial/pointer.c`：**

<a id="code-labs-tutorial-pointer-c-1"></a>

<!-- write: labs/tutorial/pointer.c -->
```c
#include <string.h>  // 主机观察用独立数组复制与比较检查选择没有触发保存。
#include "ui.h"      // 取得共享桌面状态和输入事件接口。
#include <stdio.h>   // 取得主机 printf、文件输出与错误报告接口。
static struct ui_desktop desktop;      // 静态存储整份桌面；事件回调结束后状态仍保留。
static struct airi_console console;    // 静态保存终端命令与日志，不在每次重画时重建。
static bool observe_text_drag(void) {  // 在同一桌面验证窄窗口起拖和越过Save时的选择边界。
    ui_key(&desktop, UI_ESCAPE, false);  // 清反馈与查找焦点，本次文本输入不交给短字段。
    ui_open(&desktop, UI_EDITOR);  // 让原Editor重新成为输入接收者，不重建文档或服务。
    desktop.windows[UI_EDITOR].maximized = false;  // 使用明确普通外框，避免下一帧改成最大化宽度。
    desktop.windows[UI_EDITOR].rect = (struct ui_rect){100, 80, 800, 400};  // 外框800像素，内容宽798，不显示190像素项目栏。
    ui_key(&desktop, UI_SELECT_ALL, false);    // 本次完整替换当前工作正文，保存点仍独立持有。
    strcpy(desktop.clipboard, "print(42)\n");  // 十字节ASCII输入加NUL，已知可放入剪贴板。
    ui_key(&desktop, UI_PASTE, false);  // 形成实际未保存文档，dirty由已有编辑路径更新。
    char text[UI_DOC_SIZE], saved[UI_DOC_SIZE], stored[UI_DOC_SIZE];  // 观察者独立持有文字与保存点，stored稍后读回服务。
    memcpy(text, desktop.editor, sizeof(text));   // 保存本次工作数组全部字节，选择不能修改它。
    memcpy(saved, desktop.saved, sizeof(saved));  // 保存旧提交点，拖过Save也不能更新它。
    struct ui_rect body = u_window_body(desktop.windows[UI_EDITOR].rect);  // 用当前实际外框取得与绘图相同的内容范围。
    if (body.w != 798 || !desktop.dirty)  // 先确认临界宽度和实际未保存状态，才观察后续移动。
        return false;  // 尚未构造目标输入，不能宣称选择观察通过。
    int x = body.x + 61 + 10, y = body.y + 88 + 8;  // 无项目栏，第一行第1间隙在正文左缘之后十像素。
    ui_pointer(&desktop, x, y, 1);  // 在第一批文字列按下，不能误按外框宽度排除不存在的项目栏。
    if (desktop.drag != UI_DRAG_SELECT || desktop.anchor != 1)  // 必须真正开始拖选并锁定第1间隙。
        return false;  // 仅移动cursor而未登记持续动作仍算失败。
    unsigned anchor = desktop.anchor;  // 按下间隙随后保持不变，鼠标只移动另一端。
    ui_pointer(&desktop, body.x + 20, body.y + 16, 0);  // 将选择移到实际Save按钮内部，只走正文定位器。
    if (!desktop.dirty || desktop.anchor != anchor || desktop.cursor != 0)  // 顶部越界夹到文首，保存状态及起点不能被按钮分派改动。
        return false;  // 保存动作清dirty或改anchor时立即报告失败。
    if (memcmp(text, desktop.editor, sizeof(text)) || memcmp(saved, desktop.saved, sizeof(saved)))  // 两份数组字节都应与移动前相同。
        return false;  // 选择和外部位置不能提交或重写当前文档。
    if (!desktop.backend.read(desktop.backend.context, desktop.editor_id, stored, sizeof(stored)) || strcmp(stored, saved))  // 读回真实参考记录，确认服务仍持有旧保存点。
        return false;  // 不仅检查前端标记，也检查服务没有收到保存。
    ui_pointer(&desktop, body.x + 20, body.y + 16, 2);  // 在同一按钮上释放，只结束选择动作。
    if (desktop.drag != UI_DRAG_NONE || !desktop.dirty || desktop.anchor != anchor)  // 释放后不再续拖，未保存状态与固定起点继续保留。
        return false;  // 释放不能另行执行Save或留下旧拖动。
    puts("selection: body798 starts at column1; drag over Save preserves text, saved and anchor");  // 报告这组真实事件的结果。
    return true;  // 起拖、越界、服务回读与释放均符合当前选择规则。
}

```

上面的 observe_text_drag 会在已打开文档上检查798宽body和越过Save的边界。main 先完成最初的文件路径：初始化后打开 Files，生成当前排序表，找到服务 id=1 所在的真实行，再按同一行高构造双击坐标。打开 Editor 后重新读取它的 rect，发送按下、横移40像素、释放，确认形成选区，最后才调用边界观察。继续追加 main。

<!-- append: labs/tutorial/pointer.c -->
```c
int main(void) {  // 观察程序入口；返回非零表示预期状态未出现。
    struct ui_backend backend;     // 局部回调表连接参考服务；桌面会复制这份表。
    ui_fixture_backend(&backend);  // 初始化参考文件和回调，把表写入 backend。
    airi_console_init(&console, backend.boot);  // 命令对象借用参考启动描述。
    ui_init(&desktop, backend, &console);  // 初始化桌面，复制回调表并借用稳定的 console。
    ui_open(&desktop, UI_FILES_APP);  // 置顶 Files，实验从目录操作开始。
    unsigned order[UI_FILES];  // 数组保存排序后的快照下标，不复制文件记录。
    unsigned count = ui_file_order(&desktop, order);  // 得到当前实际排序顺序及有效条数。
    unsigned row = 0;  // 从排序后的第一行开始查找 main.lua 的服务 id。
    while (row < count && desktop.files[order[row]].id != 1)  // 先确认行有效，再经排序下标读取服务 id，查找 id=1 的文件。
        ++row;  // 推进排序后行号，而不改变文件身份。
    if (row == count)  // 没有 id=1 时不能伪造后续鼠标成功。
        return 1;  // 观察失败，以非零状态让命令立即停止。
    struct ui_rect files = desktop.windows[UI_FILES_APP].rect;  // 读取 Files 的实际窗口位置作为事件坐标起点。
    ui_pointer(&desktop, files.x + 220, files.y + UI_TITLE_H + 1 + 138 + (int)row * (desktop.density ? 28 : 36) + 8, 3);  // 从49像素标题及外框下方定位首行138像素顶部，按实际行高选第row行内部；3表示双击。
    if (desktop.active != UI_EDITOR)  // 双击必须把该文件送进 Editor，否则停止实验。
        return 1;  // 观察失败，以非零状态让命令立即停止。
    struct ui_rect editor = desktop.windows[UI_EDITOR].rect;      // 打开后重新读取 Editor 矩形，不复用 Files 坐标。
    int x = editor.x + 260, y = editor.y + UI_TITLE_H + 1 + 100;  // 选取 Editor 正文内的一点，避开树与行号区。
    ui_pointer(&desktop, x, y, 1);     // 发送按下事件，让正文计算初始字节间隙。
    unsigned anchor = desktop.anchor;  // 保存实验起点，用于比较拖动后的光标。
    ui_pointer(&desktop, x + 40, y, 0);  // 横移40像素，模拟约四个字符的拖选。
    ui_pointer(&desktop, x + 40, y, 2);  // 释放鼠标，必须结束 drag 而保留已形成的选区。
    printf("editor_id=%d anchor=%u cursor=%u selected=%d drag=%d\n", desktop.editor_id,  // 打印实际身份、选区间隙与释放后的拖动状态。
           anchor, desktop.cursor, desktop.selection, desktop.drag);  // 布尔值经可变参数提升为 int，%d 打印0或1。
    if (desktop.editor_id != 1 || !desktop.selection || desktop.cursor == anchor)  // 先确认此前文件与窗口观察，再改变外框测试临界宽度。
        return 1;  // 原来的选择尚未建立，不能用后一组操作替代其结果。
    return observe_text_drag() ? 0 : 1;  // 窄窗口起拖与越过Save同样必须通过，最终才返回成功。
}
```

## 初始化指针位置，再把状态画回画布

初始化清零桌面，并将 drag 设为 NONE；初始指针 (0,0) 位于侧栏图标范围之外。之后 `ui_pointer` 保存实际坐标，`ui_draw` 读取它决定侧栏名称提示；文件和文档仍由原桌面对象持有。

在 state.c 的 ui_init 中定位 console/active 初始化窗口，加入 drag=UI_DRAG_NONE。其余初始化与服务路径保留。

<a id="code-frontend-core-state-c-1"></a>

<!-- replace: frontend/core/state.c -->
<!-- before -->
```c
    c->extension = appearance;  // 安装同步命令扩展；console 不包含主题或窗口类型。
    c->extension_context = s;   // 桌面和 console 都由平台持有，须覆盖后续输入调用。
    s->console = c;     // 桌面借用同一console状态，Terminal不另复制日志
    s->active = -1;     // 没有活动窗口，-1哨兵不能拿去索引应用数组
    s->editor_id = -1;  // 编辑器尚未绑定任何服务文件身份
    s->file_clipboard = -1;  // 文件剪贴板没有服务id，不能发起复制或移动
```
<!-- after -->
```c
    c->extension = appearance;  // 安装同步命令扩展；console 不包含主题或窗口类型。
    c->extension_context = s;   // 桌面和 console 都由平台持有，须覆盖后续输入调用。
    s->console = c;  // 桌面借用同一console状态，Terminal不另复制日志
    s->active = -1;  // 没有活动窗口，-1哨兵不能拿去索引应用数组
    s->drag = UI_DRAG_NONE;  // 当前没有进行窗口或选区拖动
    s->editor_id = -1;  // 编辑器尚未绑定任何服务文件身份
    s->file_clipboard = -1;  // 文件剪贴板没有服务id，不能发起复制或移动
```

paint.c 新增侧栏名称表，并在两处已有绘图窗口读取当前 mouse_x/mouse_y。依次用下方新块替换唯一旧窗口，窗口正文和对话框继续使用原路径；本次观察不需要重敲整帧。

<a id="code-frontend-core-paint-c-1"></a>

<!-- replace: frontend/core/paint.c -->
<!-- before -->
```c
static const char *titles[] = {"Terminal", "Files",    "Editor",     "Tasks",  // 窗口标题按应用枚举顺序排，应用编号可直接索引
                               "System",   "Settings", "Data viewer"};  // 继续应用枚举4到6的窗口标题，数组顺序与应用id一致
static void monitor(struct ui_desktop *s) {  // 共享资源仪表，全部数值来自同一次backend快照
    int x = s->width - UI_MONITOR_W, left = x + 24, width = UI_MONITOR_W - 48;  // 单一内边距对齐标题、数值与坐标轴
```
<!-- after -->
```c
static const char *titles[] = {"Terminal", "Files",    "Editor",     "Tasks",  // 窗口标题按应用枚举顺序排，应用编号可直接索引
                               "System",   "Settings", "Data viewer"};  // 继续应用枚举4到6的窗口标题，数组顺序与应用id一致
static const char *icons[] = {"Terminal", "Files",    "Editor", "Tasks",  // 侧栏悬停名称按相同应用顺序排
                              "System",   "Settings", "Viewer"};  // 继续应用枚举4到6的侧栏名称，同一编号显示同一应用
static void monitor(struct ui_desktop *s) {  // 共享资源仪表，全部数值来自同一次backend快照
    int x = s->width - UI_MONITOR_W, left = x + 24, width = UI_MONITOR_W - 48;  // 单一内边距对齐标题、数值与坐标轴
```

<!-- replace: frontend/core/paint.c -->
<!-- before -->
```c
        if (s->windows[i].open)  // 已打开应用即使最小化也保留侧栏打开标记
            u_rect(25, y + 25, 6, 2, U_ACCENT);  // 用会话强调色写矩形，起点(25,y + 25)，尺寸6×2像素
    }
    int right = s->monitor && s->width >= UI_MONITOR_MIN ? 268 : 0;  // 资源栏开启且屏幕够宽才预留268像素，否则窗口可用至右边
```
<!-- after -->
```c
        if (s->windows[i].open)  // 已打开应用即使最小化也保留侧栏打开标记
            u_rect(25, y + 25, 6, 2, U_ACCENT);  // 用会话强调色写矩形，起点(25,y + 25)，尺寸6×2像素
        if (u_inside((struct ui_rect){0, y - 8, 56, 38}, s->mouse_x, s->mouse_y)) {  // 鼠标落在当前应用侧栏行的半开矩形时显示其名称提示
            u_rect(60, y, 100, 28, U_RAISED);  // 用选中背景色写矩形，起点(60,y)，尺寸100×28像素
            u_text(70, y + 5, icons[i], U_BODY, U_TEXT);  // 在整屏列70、整屏y=y + 5显示当前应用悬停名称
        }
    }
    int right = s->monitor && s->width >= UI_MONITOR_MIN ? 268 : 0;  // 资源栏开启且屏幕够宽才预留268像素，否则窗口可用至右边
```

<!-- replace: frontend/core/paint.c -->
<!-- before -->
```c
    if (right)  // 资源栏占用宽度非零时才绘制它，本轮窗口已为其留空间
        monitor(s);  // 资源栏启用时读取同次统计，显示当前可用数据或未采集状态
    u_reset_clip();  // 恢复整个画布的可写范围，后续控件不继承上一次局部裁剪
    u_rect(0, s->height - UI_BOTTOM, s->width, 30, U_PANEL);  // 用工具栏底色写矩形，起点(0,s->height - UI_BOTTOM)，尺寸s->width×30像素
```
<!-- after -->
```c
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
```

**在学习目录根目录运行：**

<!-- run -->
```sh
mkdir -p build  # 保存本次主机程序和图像，已有目录可以继续
cc -std=c11 -O0 -g -Wall -Wextra -Werror -Iinclude -Ifrontend/include labs/tutorial/pointer.c kernel/lib/base.c kernel/boot/info.c kernel/console/console.c frontend/core/*.c frontend/apps/*.c frontend/backends/reference.c frontend/render/draw.c -o build/pointer  # Shell展开两个通配符，把当前已经写出的核心和应用一起链接
./build/pointer  # 运行当前完整观察程序，退出非零表示本组观察失败
```

输出显示 editor_id=1、不同的 anchor/cursor、selection=1，释放后 drag=-1。观察程序先查找 id=1 在实际排序结果中的行，再以绘图采用的28/36像素行高计算点击位置，内容起点统一通过u_window_body得到；换排序也不会误打开另一个文件。坐标现在可以改变正在编辑的同一份文档。

选择边界观察还输出`selection: body798 starts at column1; drag over Save preserves text, saved and anchor`，对应本章完整程序中实际发送的按下、越界移动与释放。
