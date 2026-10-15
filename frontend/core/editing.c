#include "internal.h"  // 引入当前目录内部共享接口和基础函数
static void delete_selection(struct ui_desktop *s) {  // 用重叠复制删除半开选区并归一化光标和锚点
    if (s->selection && s->cursor != s->anchor) {  // 存在非空半开选区时才删除文字
        unsigned a = s->cursor < s->anchor ? s->cursor : s->anchor,  // 光标与锚点取较小的字节间隙作为选区左端a
                 b = s->cursor > s->anchor ? s->cursor : s->anchor;  // 两端较大的字节间隙为右端b，选区包括[a,b)而不包括b处字符
        airi_memmove(s->editor + a, s->editor + b, airi_strlen(s->editor + b) + 1);  // 将b后的正文连NUL左移到a，重叠复制删除[a,b)且不破坏尚未读取的尾部
        s->cursor = a;  // 删除选区后光标落在原选区左端
    }
    s->selection = false;   // 取消现有文档选区，anchor不再参与当前显示
    s->anchor = s->cursor;  // 取消选区后锚点回到当前光标间隙
}
static void changed(struct ui_desktop *s);  // 比较编辑与保存副本，更新dirty并显露光标
static void insert(struct ui_desktop *s, const char *text) {  // 整份检查输入与容量，通过后才保存撤销副本并插入
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {  // 按无符号字节遍历拟插入文字，128以上字节不会因char有符号而误读成负数
        if (*p >= 128) {  // 整个输入含非ASCII字节时拒绝本次修改
            copy(s->toast, sizeof(s->toast), "ASCII editor: paste contains unsupported bytes.");  // 把文字"ASCII editor: paste contains unsupported bytes."复制到本次操作短提示，sizeof(s->toast)是包含NUL的数组容量
            return;  // 提示写好后立即退出，当前文字、选区和撤销副本尚未修改
        }
    }
    size_t len = airi_strlen(s->editor), n = airi_strlen(text),  // len是修改前正文长度，n是拟插入文字长度，均按内容字节计且不含NUL
           selected = s->selection  // 有有效选区时才计算被替换长度，随后从原正文长度中扣除它
                          ? (s->cursor > s->anchor ? s->cursor - s->anchor : s->anchor - s->cursor)  // 有效选区取两端差的绝对值，不受向左或向右选择影响
                          : 0;  // 没有选区时扣除长度为0，原文全部保留参与容量计算
    if (len - selected + n >= sizeof(s->editor)) {  // 先扣被替换选区再加输入，最终内容必须留一格NUL
        copy(s->toast, sizeof(s->toast), "Document limit: 8191 bytes.");  // 把文字"Document limit: 8191 bytes."复制到本次操作短提示，sizeof(s->toast)是包含NUL的数组容量
        return;  // 提示写好后立即退出，当前文字、选区和撤销副本尚未修改
    }
    copy(s->undo, sizeof(s->undo), s->editor);  // 在修改前保存完整旧文档，供一次撤销切回
    s->undo_valid = true;  // 旧文档已存好，允许下一次Undo使用它
    delete_selection(s);   // 先删除当前选区并把光标放到左端，再执行插入或剪切
    len = airi_strlen(s->editor);  // 选区删除后重测文档长度，尾部移动使用新长度
    airi_memmove(s->editor + s->cursor + n, s->editor + s->cursor, len - s->cursor + 1);  // 尾部连NUL整体右移n字节，重叠复制保住尚未读的源
    airi_memcpy(s->editor + s->cursor, text, n);  // 空位已经腾出，复制新文字内容而不复制其NUL
    s->cursor += (unsigned)n;  // 光标移到新插入文字之后
    changed(s);  // 重新比较保存副本并滚动到光标，使内容和可见状态一致
}
static void field_key(struct ui_desktop *s, char *field, size_t cap, int key) {  // 让一个事件只修改当前短字段和字段级选择
    if (key == UI_SELECT_ALL) {  // 当前接收者内全选：短字段标记整字段，文档分支则设置两端字节间隙
        s->field_all = true;  // 输入字段内容设为整体选中，下一次键入可以替换
        return;  // 当前让一个事件只修改当前短字段和字段级选择在本分支结束，后续语句不执行
    }
    if (key == UI_COPY || key == UI_CUT) {  // 复制保留当前文字，剪切还会删除；具体范围由此接收者的选中状态决定
        if (s->field_all) {  // 只有短字段整体选中时才复制其正文，文档选区不参与此字段操作
            copy(s->clipboard, sizeof(s->clipboard), field);  // 把field复制到s->clipboard，sizeof(s->clipboard)是包含NUL的数组容量
            if (key == UI_CUT)  // 短字段剪切复制完成后清空字段，普通复制保留输入
                field[0] = 0;  // 清空当前短输入字段，文档编辑数组不受影响
        }
        return;  // 当前让一个事件只修改当前短字段和字段级选择在本分支结束，后续语句不执行
    }
    if (s->field_all && (key == 8 || key == UI_PASTE || (key >= 32 && key <= 126))) {  // 整字段选中后首次退格、粘贴或字符输入替换整份字段，其余快捷键不清空
        field[0] = 0;  // 清空当前短输入字段，文档编辑数组不受影响
        s->field_all = false;  // 首次替换后取消整字段选中状态
    }
    size_t n = airi_strlen(field);  // 当前短字段已有的内容字节数，下一字符写在n处，NUL另留一格
    if (key == 8 && n)  // 退格且字段非空时才删除末字节，避免n-1下溢
        field[n - 1] = 0;  // 删除字段尾部一个字符，用NUL缩短有效字符串
    else if (key == UI_PASTE) {  // 此处粘贴只追加短字段可打印ASCII，不能把换行当确认事件
        for (const char *p = s->clipboard; *p && n + 1 < cap; ++p)  // 扫描剪贴板直到NUL或字段只剩终止零的位置，可打印字节才追加
            if (*p >= 32 && *p <= 126)  // 字段或Terminal粘贴只消费可打印ASCII，控制字节不自动提交
                field[n++] = *p;  // 把可打印剪贴板字节追加到字段，再增加当前长度
        field[n] = 0;  // 字段粘贴结束后为有效前缀补NUL
    } else if (key >= 32 && key <= 126 && n + 1 < cap) {  // 单字符追加仍需给终止零留一格容量
        field[n] = (char)key;  // 把当前可打印逻辑键写到短字段末尾
        field[n + 1] = 0;      // 给单字符追加结果补NUL，容量判断已预留此位置
    }
}
static void reveal_cursor(struct ui_desktop *s) {  // 将文档光标换成行列并滚动到可见范围
    unsigned row = 0, col = 0;  // 从文档首间隙的第0行第0列累计到当前光标，换行时列号重新从0开始
    for (unsigned i = 0; i < s->cursor && s->editor[i]; ++i) {  // 扫描光标前的ASCII字节，换行决定行列转换
        if (s->editor[i] == '\n') {  // 扫描到光标前的换行时增加状态行号并复位列号
            ++row;    // 在光标前遇到换行，逻辑文档行号增加一；随后的col=0开始累计新行列号
            col = 0;  // 经过换行后的光标列从新行第0个间隙重新累计
        } else  // 前一分支条件未成立时转入随后的单条替代操作
            ++col;  // 非换行字符使当前列增加一
    }
    struct ui_rect r = s->windows[UI_EDITOR].rect;  // 复制Editor当前窗口矩形，其宽高用于计算光标可见的行列范围
    struct ui_rect body = u_window_body(r);  // 与实际绘图共用内容矩形，48像素标题及边框先从外框扣除
    int rows = (body.h - 88 - 30 - (s->find ? 44 : 0)) / 24,  // 内容扣正文前88像素、底部裁剪30像素，查找再扣44像素，按24像素算可见行
        columns = (body.w - (body.w >= 800 ? 190 : 0) - 72) / 10;  // 与Editor正文裁剪共用宽度：项目栏按body判断，再扣72像素行号及边距，十像素为一列
    if (rows < 1)  // 极小窗口也将逻辑可见行数限制为至少1
        rows = 1;  // 即使窗口小到不足一行，也保留1行逻辑可见容量以免滚动计算下溢
    if (columns < 1)  // 极小窗口也将逻辑可见列数限制为至少1
        columns = 1;  // 可见列至少按1计算，避免零列无法容纳任何光标位置
    if (row < s->editor_scroll)  // 光标行在当前顶部之前时，必须向上滚到它所在行
        s->editor_scroll = row;  // 把光标行放在可见区第一行，只改变显示不移动文档内容
    if (row >= s->editor_scroll + (unsigned)rows)  // 光标到达可见区末端或之后时，向下滚到它能落入最后一行
        s->editor_scroll = row - (unsigned)rows + 1;  // 新顶部保证光标位于rows行可见范围的最后一行
    if (col < s->editor_column_scroll)  // 光标列在当前左缘之前时需要水平左滚
        s->editor_column_scroll = col;  // 把光标列放到显示区最左侧，原字符与字节下标不变
    if (col >= s->editor_column_scroll + (unsigned)columns)  // 光标列超出可见列范围时需要水平右滚
        s->editor_column_scroll = col - (unsigned)columns + 1;  // 新左缘让光标处在可见区最后一列
}
static void changed(struct ui_desktop *s) {  // 比较编辑与保存副本，更新dirty并显露光标
    size_t n = airi_strlen(s->editor);  // 当前编辑正文长度用于与保存副本比较，NUL在后面的memcmp中一并检查
    s->dirty = n != airi_strlen(s->saved) || airi_memcmp(s->editor, s->saved, n + 1) != 0;  // 长度不同或同长内容含NUL比较不同即为未保存；短路或避免长短不等时继续比较
    reveal_cursor(s);  // 按文档行列和窗口可见范围调整滚动，使光标可见
}
bool ui_key(struct ui_desktop *s, int key, bool shift) {  // 按对话框、字段焦点和活动应用选择事件接收者
    s->toast[0] = 0;  // 新按键先清上一次短提示，随后错误可重新写入
    if (key == UI_NEXT_WINDOW) {  // 全局切窗先于字段和应用处理，寻找当前活动应用之外已打开的窗口
        for (unsigned i = 0; i < s->order_count; ++i) {  // 按有效窗口层次遍历，数组后部不作窗口身份
            unsigned app = s->order[i];  // 绘图次序项保存应用id，后续用它查看窗口是否打开，i本身不是应用id
            if (s->windows[app].open && (int)app != s->active) {  // 候选必须仍打开且不是活动应用，找到后ui_open将它置顶
                ui_open(s, (enum ui_app)app);  // 打开或置顶一个应用，更新窗口次序和活动编号，本次实参取自当前桌面状态
                return true;  // 本逻辑事件已处理，调用者可重画当前状态
            }
        }
        return false;  // 当前事件没有可执行操作，调用者无需因此重画
    }
    if (key == UI_ESCAPE) {  // 全局Escape取消弹出交互和搜索，后续按键重新交给活动应用
        s->dialog = UI_DIALOG_NONE;  // 关闭当前对话框，后续键不再被它优先接收
        s->pending_close = false;    // 结束关闭请求，后续普通保存不应沿用旧关闭动作
        s->find = false;  // 隐藏Editor查找栏，后续正文可使用这部分高度
        s->focus = UI_FOCUS_NONE;  // 当前字段结束接收，后续按键回到活动应用
        return true;  // 本逻辑事件已处理，调用者可重画当前状态
    }
    if (s->dialog) {  // 有对话框时先由它消费当前键，下面的字段和应用不再接收同一事件
        if (key == '\n' || key == '\r') {  // 换行或回车表示提交当前命令、字段或文档换行动作，按当前接收者处理
            if (s->dialog == UI_DIALOG_ERROR)  // 错误提示只需关闭，不应当被回车转成文件服务动作
                s->dialog = UI_DIALOG_NONE;  // 关闭当前对话框，后续键不再被它优先接收
            else  // 前一分支未成立时执行下方替代操作
                ui_confirm(s);  // 把当前对话框确认转换成明确服务动作，本次实参取自当前桌面状态
            return true;  // 本逻辑事件已处理，调用者可重画当前状态
        }
        if (s->dialog == UI_DIALOG_NEW_FILE || s->dialog == UI_DIALOG_NEW_DIR || s->dialog == UI_DIALOG_RENAME || s->dialog == UI_DIALOG_SAVE_AS)  // 只有需要名称的四种对话框把普通键交给dialog_input，其他确认框忽略文字
            field_key(s, s->dialog_input, sizeof(s->dialog_input), key);  // 当前事件仅修改对话框输入字段，Editor不再次消费它
        return true;  // 本逻辑事件已处理，调用者可重画当前状态
    }
    if (s->focus == UI_FOCUS_FILTER || s->focus == UI_FOCUS_FIND) {  // 焦点在短字段时，该字段独占文字事件，当前文档不再次插入同一个字符
        enum ui_focus receiver = s->focus;  // 保存本次实际接收者，回车清焦点后仍能区分筛选与查找。
        char *field = s->focus == UI_FOCUS_FILTER ? s->filter[s->active] : s->find_text;  // 筛选焦点选当前应用的独立数组，查找焦点选Editor查找数组，两者均为64字节
        field_key(s, field, 64, key);      // 将事件交给已选中的64字节筛选或查找字段
        if (key == '\n' || key == '\r') {  // 换行或回车表示提交当前命令、字段或文档换行动作，按当前接收者处理
            s->focus = UI_FOCUS_NONE;  // 当前字段结束接收，后续按键回到活动应用
            if (receiver == UI_FOCUS_FIND) {  // 只有查找字段的回车扫描Editor，Files筛选不改变文档选区。
                size_t q = airi_strlen(s->find_text);  // 查找串的内容字节长度；q为0时不扫描正文也不创建空选区
                const char *found = NULL;  // 尚未发现匹配，成功后此指针借用editor数组内第一个匹配字节
                for (const char *p = s->editor; q && *p; ++p)  // 查找串非空时逐字节枚举正文起点，遇正文NUL停止
                    if (airi_strlen(p) >= q && !airi_memcmp(p, s->find_text, q)) {  // 先保证剩余正文至少q字节，再比较当前起点的完整查找串
                        found = p;  // 保存第一个完整匹配的文档内地址，随后停止寻找后续匹配
                        break;      // 结束当前最近一层循环或应用分派，不执行其剩余候选
                    }
                if (found) {  // 查找确实返回文档内地址时，才换算成字节选区
                    s->cursor = (unsigned)(found - s->editor) + q;  // 匹配起点相对文档首字节的偏移加q，将光标放在匹配文字之后
                    s->anchor = (unsigned)(found - s->editor);  // 锚点放在匹配首字节之前，与光标构成完整匹配选区
                    s->selection = true;  // cursor与anchor之间的半开范围现在形成有效选区
                    reveal_cursor(s);     // 按文档行列和窗口可见范围调整滚动，使光标可见
                } else  // 前一分支条件未成立时转入随后的单条替代操作
                    copy(s->toast, sizeof(s->toast), "No match in document.");  // 把文字"No match in document."复制到本次操作短提示，sizeof(s->toast)是包含NUL的数组容量
            }
        }
        return true;  // 本逻辑事件已处理，调用者可重画当前状态
    }
    if (s->active == UI_TERMINAL) {  // Terminal独占当前应用事件，正文输入进入console命令数组
        if (key == UI_PASTE) {  // Terminal粘贴逐字节交给console，只接收可打印ASCII；粘贴换行不提交命令
            for (const char *p = s->clipboard; *p; ++p)  // 逐字节扫描Terminal文字剪贴板，下一条件排除不可打印字节与换行
                if (*p >= 32 && *p <= 126)  // 字段或Terminal粘贴只消费可打印ASCII，控制字节不自动提交
                    airi_console_key(s->console, *p);  // Terminal粘贴逐个送可打印ASCII到命令输入，剪贴板换行不会自动提交
            return true;  // 本逻辑事件已处理，调用者可重画当前状态
        }
        return airi_console_key(s->console, key);  // 把当前键交给同一console，并把它的处理结果原样返回给调用者
    }
    if (key == UI_FIND_KEY && s->active == UI_FILES_APP) {  // Files的查找快捷键只切到自己的筛选框，不开启Editor文内查找
        s->focus = UI_FOCUS_FILTER;  // 后续文字交给当前应用筛选字段
        s->field_all = true;  // 输入字段内容设为整体选中，下一次键入可以替换
        return true;  // 本逻辑事件已处理，调用者可重画当前状态
    }
    if (s->active == UI_FILES_APP) {  // 当前接收者是Files时，复制剪切保存文件身份，而不是复制Editor文字
        if ((key == UI_COPY || key == UI_CUT) && s->file_count) {  // 目录快照非空才可取得选中服务id，复制剪切并不立即改服务记录
            s->file_clipboard = s->files[s->selection_file].id;  // 文件剪贴板存选中项服务id，后续即使排序变化仍可找到同一源文件
            s->file_cut = key == UI_CUT;  // 剪切表示后续粘贴请求移动，普通复制表示创建独立新服务记录
            return true;  // 本逻辑事件已处理，调用者可重画当前状态
        }
        if (key == UI_PASTE && s->file_clipboard >= 0) {  // 文件剪贴板有源id时，向当前目录请求复制或移动，正文剪贴板不参与
            ui_command(s, s->file_cut ? UI_MOVE_FILE : UI_COPY_FILE, s->file_clipboard,  // 调用服务操作，成功后刷新，失败形成错误对话框，本次实参取自当前桌面状态
                       s->directory, "");  // 文件剪贴板操作把当前目录作为明确目标，不传文字正文
            if (!s->dialog && s->file_cut)  // 移动成功未产生错误框时清文件剪贴板，失败保留源身份便于重试
                s->file_clipboard = -1;  // 文件剪贴板没有服务id，不能发起复制或移动
            return true;  // 本逻辑事件已处理，调用者可重画当前状态
        }
        if (key == UI_DELETE_KEY && s->file_count) {  // Files中Delete先为当前选中服务id打开确认，未确认前不删除文件
            s->dialog = UI_DIALOG_DELETE;  // 弹出删除确认后事件先由对话框接收，不能直接继续编辑正文
            s->dialog_target = s->files[s->selection_file].id;  // 把弹框瞬间选中项的服务id固定为删除目标，和可变显示下标分开
            copy(s->dialog_title, 64, "Delete item");  // 把文字"Delete item"复制到对话框标题，64是包含NUL的数组容量
            copy(s->dialog_text, 256, "Remove the selected item from the workspace?");  // 把文字"Remove the selected item from the workspace?"复制到对话框失败或确认说明，256是包含NUL的数组容量
            return true;  // 本逻辑事件已处理，调用者可重画当前状态
        }
    }
    if (s->active == UI_FILES_APP && (key == '\n' || key == '\r')) {  // Files中回车按当前原快照下标打开目录或文件，不在Editor插入换行
        ui_open_document(s, s->selection_file);  // 按快照下标打开目录或读取文档，成功才切换编辑身份，本次实参取自当前桌面状态
        return true;  // 本逻辑事件已处理，调用者可重画当前状态
    }
    if (s->active == UI_FILES_APP && (key == AIRI_KEY_UP || key == AIRI_KEY_DOWN)) {  // Files上下键先将原快照选择换成当前筛选排序后的可见行
        unsigned order[UI_FILES];  // 调用者保存可见行到原快照下标的最多32项映射
        unsigned count = ui_file_order(s, order);  // 生成可见行到原快照下标的筛选排序表
        if (!count)  // 没有筛选结果时上下键无可选项，不取order[0]
            return false;  // 当前事件没有可执行操作，调用者无需因此重画
        unsigned row = 0;  // 从可见索引表第0行开始寻找当前原快照选择，不把服务id当行号
        while (row < count && order[row] != s->selection_file)  // 在可见索引表中寻找旧快照选择对应的可见行
            ++row;  // 当前计数位置前进一项，文档分支按行、Files分支按可见索引计数
        if (row == count)  // 旧选择被筛掉时从新的第0可见行开始
            row = 0;  // 原选择不在筛选结果中时，新的可见选择从第0行开始
        else if (key == AIRI_KEY_UP && row)  // 找到旧选择且不在首行时才向上一可见行移动
            --row;  // 向上一可见行移动，外层条件已经保护第0行
        else if (key == AIRI_KEY_DOWN && row + 1 < count)  // 候选下一行必须仍在有效可见索引前缀内
            ++row;  // 当前计数位置前进一项，文档分支按行、Files分支按可见索引计数
        s->selection_file = order[row];  // 把选定可见行转换回原快照下标，后面取files仍能找到同一项目
        return true;  // 本逻辑事件已处理，调用者可重画当前状态
    }
    if (s->active != UI_EDITOR)  // 除Editor之外的应用没有多行文本编辑行为，到这里结束普通字符处理
        return false;  // 当前事件没有可执行操作，调用者无需因此重画
    size_t len = airi_strlen(s->editor);  // 本事件开始时的文档内容字节长度，后续光标与删除范围都以它为文尾
    unsigned before = s->cursor;  // 保存本次移动前的光标，Shift开始选择时用作锚点
    if (key == UI_SAVE_KEY) {     // 当前Editor保存键将已绑定文档直接提交，未绑定文档先询问名称
        if (s->editor_id < 0) {  // 新文档尚无服务记录身份，保存前先打开另存名称框
            s->dialog = UI_DIALOG_SAVE_AS;  // 没有文件身份时先请求新名称，暂未调用保存服务
            copy(s->dialog_title, 64, "Save document");  // 把文字"Save document"复制到对话框标题，64是包含NUL的数组容量
            copy(s->dialog_text, 256, "Choose a file name in the current workspace.");  // 把文字"Choose a file name in the current workspace."复制到对话框失败或确认说明，256是包含NUL的数组容量
            copy(s->dialog_input, 128, "untitled.lua");  // 把文字"untitled.lua"复制到对话框输入字段，128是包含NUL的数组容量
            s->field_all = true;  // 输入字段内容设为整体选中，下一次键入可以替换
        } else  // 前一分支条件未成立时转入随后的单条替代操作
            ui_command(s, UI_SAVE, s->editor_id, s->editor_name, s->editor);  // 提交当前文档id、名称与正文，只有服务成功才更新前端保存点
        return true;  // 本逻辑事件已处理，调用者可重画当前状态
    }
    if (key == UI_RUN_KEY) {  // 以当前文档服务id与正文请求运行，参考服务会明确报告执行尚未连接
        ui_command(s, UI_EXECUTE, s->editor_id, s->editor_name, s->editor);  // 当前文档执行请求带完整身份与正文，参考服务明确拒绝而不是启动程序
        return true;  // 本逻辑事件已处理，调用者可重画当前状态
    }
    if (key == UI_FIND_KEY) {  // 打开当前Editor查找栏并切换短字段焦点，后续字符不直接修改正文
        s->find = true;  // 打开Editor查找栏，并为正文腾出其高度
        s->focus = UI_FOCUS_FIND;  // 后续文字交给Editor的查找输入字段
        return true;  // 本逻辑事件已处理，调用者可重画当前状态
    }
    if (key == UI_SELECT_ALL) {  // 当前接收者内全选：短字段标记整字段，文档分支则设置两端字节间隙
        s->cursor = (unsigned)len;  // 全选另一端落在文档最后一个字符之后
        s->anchor = 0;  // 全选从文档第一个字符前的间隙开始
        s->selection = true;  // cursor与anchor之间的半开范围现在形成有效选区
        return true;  // 本逻辑事件已处理，调用者可重画当前状态
    }
    if (key == UI_COPY || key == UI_CUT) {  // 复制保留当前文字，剪切还会删除；具体范围由此接收者的选中状态决定
        if (s->selection) {  // 正文存在选区时才复制对应[a,b)文字，普通光标位置不构成复制内容
            unsigned a = s->cursor < s->anchor ? s->cursor : s->anchor,  // 光标与锚点取较小的字节间隙作为选区左端a
                     b = s->cursor > s->anchor ? s->cursor : s->anchor;  // 两端较大的字节间隙为右端b，选区包括[a,b)而不包括b处字符
            airi_memcpy(s->clipboard, s->editor + a, b - a);  // 复制选区[a,b)内容，原文档暂不修改
            s->clipboard[b - a] = 0;  // 选区内容复制后补NUL，剪贴板自身成为独立字符串
            if (key == UI_CUT) {      // 剪切除复制选区之外，还保存旧文档并删除对应文字，供Undo恢复
                copy(s->undo, sizeof(s->undo), s->editor);  // 在修改前保存完整旧文档，供一次撤销切回
                s->undo_valid = true;  // 旧文档已存好，允许下一次Undo使用它
                delete_selection(s);   // 先删除当前选区并把光标放到左端，再执行插入或剪切
                changed(s);  // 重新比较保存副本并滚动到光标，使内容和可见状态一致
            }
        }
        return true;  // 本逻辑事件已处理，调用者可重画当前状态
    }
    if (key == UI_PASTE) {  // Editor粘贴整份文字剪贴板，容量或ASCII检查失败时保留原文档
        insert(s, s->clipboard);  // 粘贴沿同一完整检查与插入路径，失败保留原文档
        return true;  // 本逻辑事件已处理，调用者可重画当前状态
    }
    if (key == UI_UNDO) {  // 交换当前文字和唯一旧文档槽，再比较保存点，第二次Undo可切回刚才文字
        if (!s->undo_valid)  // 还没有修改前副本时拒绝撤销，不能把未初始化内容写进当前文档
            return false;  // 当前事件没有可执行操作，调用者无需因此重画
        char temp[UI_DOC_SIZE];  // 8192字节局部数组暂存当前文档，让editor与undo交换时不丢失一方
        copy(temp, sizeof(temp), s->editor);  // 交换前暂存当前文档，避免覆盖后无法恢复
        copy(s->editor, sizeof(s->editor), s->undo);  // 把撤销副本变成当前编辑文字
        copy(s->undo, sizeof(s->undo), temp);  // 把刚才的编辑文字存回撤销槽，再按Undo可切回
        s->cursor = (unsigned)airi_strlen(s->editor);  // 撤销后光标落到恢复文档的尾部
        s->selection = false;  // 取消现有文档选区，anchor不再参与当前显示
        changed(s);   // 重新比较保存副本并滚动到光标，使内容和可见状态一致
        return true;  // 本逻辑事件已处理，调用者可重画当前状态
    }
    if (key == UI_LEFT) {  // 光标向左移动一ASCII间隙，位于文首则保持原位置
        if (s->cursor)  // 光标不在第0间隙时才能减1，避免无符号位置下溢
            --s->cursor;  // 光标向前移动一个ASCII字节间隙
    } else if (key == UI_RIGHT) {  // 向右移动最多到文尾间隙，不访问NUL之后的字节
        if (s->cursor < len)  // 当前位置严格在文尾之前时才允许向右移动
            ++s->cursor;  // 光标向后移动一个ASCII字节间隙
    } else if (key == UI_HOME) {  // 向前扫描到本行第一个字符之前，前一字节的换行作为停止边界
        while (s->cursor && s->editor[s->cursor - 1] != '\n')  // 从光标向前找本行首间隙；到文首或前一字节为换行时停止
            --s->cursor;  // 光标向前移动一个ASCII字节间隙
    } else if (key == UI_END) {  // 向后扫描到本行换行之前或文档结尾，不跨到下一行
        while (s->cursor < len && s->editor[s->cursor] != '\n')  // 从光标向后找本行尾间隙；到文尾或当前字节为换行时停止
            ++s->cursor;  // 光标向后移动一个ASCII字节间隙
    } else if (key == AIRI_KEY_UP || key == AIRI_KEY_DOWN) {  // 上下移动先求当前行首和原列，再把该列限制在目标行长度内
        unsigned start = s->cursor;  // 从光标间隙开始向前找当前行首，start始终是文档字节偏移
        while (start && s->editor[start - 1] != '\n')  // 逐字节向前寻找当前或上一文档行的开始间隙
            --start;  // 向前一个字节扫描，寻找本行或上一行开头
        unsigned column = s->cursor - start;  // 光标与当前行首的字节距离就是本ASCII行的第0起算列号
        if (key == AIRI_KEY_UP && start) {    // 当前行不是首行时才能寻找上一行，start前一字节是上一行结束的换行
            unsigned end = start - 1;  // 上一行换行处作为半开右端，不把换行算进可放光标的文本列
            start = end;  // 从上一行末间隙继续向前找它的行首
            while (start && s->editor[start - 1] != '\n')  // 逐字节向前寻找当前或上一文档行的开始间隙
                --start;  // 向前一个字节扫描，寻找本行或上一行开头
            s->cursor = start + (column < end - start ? column : end - start);  // 保留原列；目标行较短时取其长度，将光标限制在该行末间隙
        }
        if (key == AIRI_KEY_DOWN) {  // 先向后找到当前行结束，确有后续行才尝试向下移动
            unsigned end = s->cursor;  // 从当前光标继续向后找当前行换行或NUL，end表示扫描间隙
            while (end < len && s->editor[end] != '\n')  // 逐字节向后寻找当前或下一文档行的结束间隙
                ++end;  // 扫描位置向后一字节，直至当前文档行末
            if (end < len) {  // 当前行后确实还有换行和下一行时才尝试向下移动
                start = end + 1;  // 跳过当前换行字节，下一字节就是目标行首
                end = start;      // 从目标行首向后测量该行长度，用于限制原列号
                while (end < len && s->editor[end] != '\n')  // 逐字节向后寻找当前或下一文档行的结束间隙
                    ++end;  // 扫描位置向后一字节，直至当前文档行末
                s->cursor = start + (column < end - start ? column : end - start);  // 保留原列；目标行较短时取其长度，将光标限制在该行末间隙
            }
        }
    } else if (key == 8 || key == UI_DELETE_KEY) {  // 退格删光标前一字节，Delete删后一字节；非空选区时两者都先删整段
        copy(s->undo, sizeof(s->undo), s->editor);  // 在修改前保存完整旧文档，供一次撤销切回
        s->undo_valid = true;  // 旧文档已存好，允许下一次Undo使用它
        if (s->selection)      // 有效文档选区优先整体删除，不再额外退格或删除旁边字节
            delete_selection(s);  // 先删除当前选区并把光标放到左端，再执行插入或剪切
        else if (key == 8 && s->cursor) {  // 没有选区且光标不在文首时，退格可以左移尾部并删前一字节
            airi_memmove(s->editor + s->cursor - 1, s->editor + s->cursor, len - s->cursor + 1);  // 光标后尾部连NUL左移1字节，覆盖光标之前的字符
            --s->cursor;  // 光标向前移动一个ASCII字节间隙
        } else if (key == UI_DELETE_KEY && s->cursor < len)  // 光标不是文尾时才能Delete，文尾后的NUL不是可删除字符
            airi_memmove(s->editor + s->cursor, s->editor + s->cursor + 1, len - s->cursor);  // 下一字节起的尾部连NUL左移1字节，覆盖光标后字符
        changed(s);   // 重新比较保存副本并滚动到光标，使内容和可见状态一致
        return true;  // 本逻辑事件已处理，调用者可重画当前状态
    } else if (key == '\n' || key == '\r') {  // 文档接收换行或回车时，通过完整插入路径增加一字节换行
        insert(s, "\n");  // 在当前间隙或选区插入一个换行，容量校验、撤销保存和dirty更新仍走同一路径
        return true;      // 本逻辑事件已处理，调用者可重画当前状态
    } else if (key == UI_TAB) {  // Tab走完整插入路径增加四个空格，本阶段不保存制表符字节
        insert(s, "    ");  // Tab插入四个ASCII空格，不引入制表符对绘图列宽的额外规则
        return true;  // 本逻辑事件已处理，调用者可重画当前状态
    } else if (key >= 32 && key <= 126) {  // 文档只把可打印ASCII事件变成新字符文字
        char ch[2] = {(char)key, 0};  // 将当前可打印ASCII键变成一个字符加NUL的短串，insert据此测量长度
        insert(s, ch);  // 把这个一字节ASCII短串交给完整插入路径，成功后光标移到新字节之后
        return true;    // 本逻辑事件已处理，调用者可重画当前状态
    } else  // 前一分支条件未成立时转入随后的单条替代操作
        return false;  // 当前事件没有可执行操作，调用者无需因此重画
    if (shift) {  // 移动事件伴随Shift时保留旧光标为锚点，以移动后的光标扩展选区
        if (!s->selection)  // 第一次扩选时用旧光标设锚点
            s->anchor = before;  // Shift首次移动时用旧光标固定选区另一端
        s->selection = true;  // cursor与anchor之间的半开范围现在形成有效选区
    } else {  // 前一分支条件未成立时处理当前替代路径
        s->selection = false;   // 取消现有文档选区，anchor不再参与当前显示
        s->anchor = s->cursor;  // 取消选区后锚点回到当前光标间隙
    }
    reveal_cursor(s);  // 按文档行列和窗口可见范围调整滚动，使光标可见
    return true;  // 本逻辑事件已处理，调用者可重画当前状态
}
