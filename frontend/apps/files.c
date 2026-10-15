#include "internal.h"  // 引入当前目录内部共享接口和基础函数

static int file_compare(const struct ui_file *a, const struct ui_file *b) {  // 先按目录类型，再按无符号名称字节比较两项
    if (a->directory != b->directory)  // 类型不同时先决定目录与普通文件顺序
        return a->directory ? -1 : 1;  // 升序下目录排在普通文件之前
    const unsigned char *x = (const unsigned char *)a->name, *y = (const unsigned char *)b->name;  // 把两名称作为无符号字节串比较，避免char符号性改变顺序
    while (*x && *x == *y) {  // 共同非零前缀相同时继续到下一对字符
        ++x;  // 第一个名称指针前进一字节
        ++y;  // 第二个名称指针前进一字节
    }
    return (int)*x - (int)*y;  // 首个不同字节的差决定名称顺序，0表示完全相同
}

unsigned ui_file_order(const struct ui_desktop *s, unsigned *order) {  // 生成可见行到原快照下标的筛选排序表
    unsigned count = 0;  // 筛选排序的有效索引表起初为空
    for (unsigned i = 0; i < s->file_count; ++i) {  // 只遍历本次目录快照有效前缀，未用槽不参与
        if (!ui_app_match(s->files[i].name, s->filter[UI_FILES_APP]))  // 当前名称不包含Files筛选文字时，不加入可见索引表
            continue;  // 本项不参与当前结果，跳过本轮剩余操作进入下一项
        unsigned j = count;  // 从当前有序前缀末尾开始寻找新项插入位置
        while (j) {  // 新项仍可向前时继续在有序前缀找插入位置
            int comparison = file_compare(&s->files[i], &s->files[order[j - 1]]);  // 先按目录类型，再按无符号名称字节比较两项
            if (s->sort ? comparison <= 0 : comparison >= 0)  // 升序或降序已到合法位置时停止向前寻找
                break;  // 结束当前最近一层循环或应用分派，不执行其剩余候选
            order[j] = order[j - 1];  // 给新项腾位置，有序前缀当前项后移一槽
            --j;  // 向前一槽继续比较新项顺序
        }
        order[j] = i;  // 将当前快照下标放到排序后的可见行位置
        ++count;  // 又有一条匹配项进入索引表有效前缀
    }
    return count;  // 返回索引表有效条数，调用者仅遍历此前缀
}

void ui_files_draw(struct ui_desktop *s, struct ui_rect r) {  // 读取目录快照和排序索引，画路径、文件行和状态
    ui_app_toolbar(r, "");  // 在内容顶端铺44像素工具栏并显示文字""
    u_button((struct ui_rect){r.x + 12, r.y + 8, 44, 28}, "Up", false, false);  // 画文字"Up"按钮；其矩形供后续事件命中共用，当前仅输出像素
    u_clip((struct ui_rect){r.x + 66, r.y + 5, r.w - 388, 34});  // 把后续文字或像素限制在本次显式矩形内，长内容在边界停止写入
    ui_app_text(r.x + 70, r.y + 12, s->directory);  // 在内容左缘右移70像素、内容顶边下移12像素显示当前目录路径
    u_clip(r);  // 恢复当前应用内容裁剪，后续列或控件可使用完整内容区
    u_button((struct ui_rect){r.x + r.w - 304, r.y + 8, 84, 28}, "New file", false, false);  // 画文字"New file"按钮；其矩形供后续事件命中共用，当前仅输出像素
    u_button((struct ui_rect){r.x + r.w - 210, r.y + 8, 84, 28}, "Folder", false, false);  // 画文字"Folder"按钮；其矩形供后续事件命中共用，当前仅输出像素
    u_button((struct ui_rect){r.x + r.w - 116, r.y + 8, 100, 28}, "Refresh", false, false);  // 画文字"Refresh"按钮；其矩形供后续事件命中共用，当前仅输出像素
    int sidebar = r.w >= 800 ? 176 : 0, x = r.x + sidebar + 16, w = r.w - sidebar - 32;  // 宽内容区保留176像素侧栏，列表左侧再留16像素，两侧共扣32像素
    if (sidebar) {  // 窗口够宽时才给Files绘制176像素侧栏
        u_rect(r.x, r.y + 44, sidebar, r.h - 71, U_BG);  // 用画布背景色写矩形，起点(r.x,r.y + 44)，尺寸sidebar×r.h - 71像素
        ui_app_muted(r.x + 16, r.y + 66, "PLACES");  // 在内容左缘右移16像素、内容顶边下移66像素显示文字"PLACES"
        ui_app_text(r.x + 16, r.y + 98, "Home");     // 在内容左缘右移16像素、内容顶边下移98像素显示文字"Home"
        u_selection((struct ui_rect){r.x + 8, r.y + 128, sidebar - 16, 32});  // 用选中背景色写矩形，起点(r.x + 8,r.y + 128)，尺寸sidebar - 16×32像素
        ui_app_text(r.x + 16, r.y + 135, "Projects");  // 在内容左缘右移16像素、内容顶边下移135像素显示文字"Projects"
        ui_app_text(r.x + 16, r.y + 176, "System");    // 在内容左缘右移16像素、内容顶边下移176像素显示文字"System"
        ui_app_text(r.x + 16, r.y + 216, "Volumes");   // 在内容左缘右移16像素、内容顶边下移216像素显示文字"Volumes"
        u_rect(r.x + sidebar - 1, r.y + 44, 1, r.h - 71, U_EDGE);  // 用分隔线色写矩形，起点(r.x + sidebar - 1,r.y + 44)，尺寸1×r.h - 71像素
    }
    ui_app_field((struct ui_rect){x, r.y + 58, w, 32},  // 画输入字段和当前焦点边框，本次实参取自当前桌面状态
                 s->filter[UI_FILES_APP][0] ? s->filter[UI_FILES_APP] : "Search files",  // 筛选有内容就画实际文本，否则画空字段提示
                 s->focus == UI_FOCUS_FILTER);  // 筛选输入拥有焦点时画强调边框，不改变其他应用自己的筛选数组
    if (s->service_state != UI_NORMAL || !s->backend.files) {  // 服务不正常或缺查询回调时显示不可用提示，不画旧文件快照
        ui_app_unavailable(s, (struct ui_rect){x, r.y + 98, w, r.h - 128},  // 按服务状态绘制加载、空或不可用提示，本次实参取自当前桌面状态
                           "File service / current directory");  // 不可用说明具体缺的是当前目录查询服务
        ui_app_status(r, "No directory snapshot", NULL);  // 在内容底部显示文字"No directory snapshot"；右侧说明按测得的文字宽度对齐
        return;  // 当前读取目录快照和排序索引，画路径、文件行和状态在本分支结束，后续语句不执行
    }
    u_rect(x, r.y + 106, w, 28, U_PANEL);     // 用工具栏底色写矩形，起点(x,r.y + 106)，尺寸w×28像素
    ui_app_muted(x + 12, r.y + 112, "Name");  // 在整屏x=x + 12、内容顶边下移112像素显示文字"Name"
    ui_app_muted(x + w - 218, r.y + 112, "Type");  // 在整屏x=x + w - 218、内容顶边下移112像素显示文字"Type"
    ui_app_muted(x + w - 102, r.y + 112, "Size");  // 在整屏x=x + w - 102、内容顶边下移112像素显示文字"Size"
    unsigned order[UI_FILES];  // 调用者保存可见行到原快照下标的最多32项映射
    unsigned count = ui_file_order(s, order);  // 生成可见行到原快照下标的筛选排序表
    unsigned shown = 0, rh = s->density ? 28 : 36;  // shown计匹配后的遍历行；密度决定每行28或36像素
    for (unsigned k = 0; k < count; ++k) {  // 按筛选排序后的可见索引表遍历，不直接使用服务返回顺序
        unsigned i = order[k];  // 将可见索引表第k项转换为原files数组下标
        struct ui_file *f = &s->files[i];  // 借用这个原快照项，绘图仍使用它的名称、大小和服务id
        if (!ui_app_match(f->name, s->filter[UI_FILES_APP]))  // 未匹配筛选的名称不进入当前显示结果
            continue;  // 本项不参与当前结果，跳过本轮剩余操作进入下一项
        if (shown++ < s->windows[UI_FILES_APP].scroll)  // 先计这条匹配行，再跳过滚动位置之前的行
            continue;  // 本项不参与当前结果，跳过本轮剩余操作进入下一项
        int y = r.y + 138 + (int)(shown - 1 - s->windows[UI_FILES_APP].scroll) * (int)rh;  // 可见行减滚动量换成当前屏幕行，从内容顶下138像素起按行高排列
        if (y + (int)rh > r.y + r.h - 84)  // 该行底部会进入下方操作区时结束文件绘制
            break;  // 结束当前最近一层循环或应用分派，不执行其剩余候选
        if (i == s->selection_file)  // 原快照下标与当前选择相等时画选中背景，排序后身份仍一致
            u_selection((struct ui_rect){x, y, w, (int)rh});  // 用选中背景色写矩形，起点(x,y)，尺寸w×(int)rh像素
        u_icon(x + 12, y + 7, f->directory ? UI_FILES_APP : UI_EDITOR,  // 目录用文件夹图标，普通正文用编辑器图标，位置跟随该行
               i == s->selection_file ? U_ACCENT : U_MUTED);  // 当前选中行图标使用强调色，其余使用次要色
        u_clip((struct ui_rect){x + 44, y, w - 280, (int)rh});  // 把后续文字或像素限制在本次显式矩形内，长内容在边界停止写入
        ui_app_text(x + 44, y + 8, f->name);  // 在整屏x=x + 44、整屏y=y + 8显示当前快照的文件名
        u_clip(r);  // 恢复当前应用内容裁剪，后续列或控件可使用完整内容区
        ui_app_muted(x + w - 218, y + 9, f->directory ? "Directory" : "Text document");  // 在整屏x=x + w - 218、整屏y=y + 9显示按当前状态选择的文案
        char size[32];  // 32字节局部文字保存文件大小加单位B
        airi_snprintf(size, 32, "%u B", f->bytes);  // 把当前普通文件内容字节数附单位B，目录行不使用这段大小文字
        ui_app_muted(x + w - 102, y + 9, f->directory ? "--" : size);  // 在整屏x=x + w - 102、整屏y=y + 9显示按当前状态选择的文案
        u_rect(x, y + (int)rh - 1, w, 1, U_EDGE);  // 以固定暗色画本文件行底部的一像素分隔线
    }
    if (!shown) {  // 没有匹配项进入显示计数时给出空或筛选无结果提示
        u_text(x + 18, r.y + 172,  // 无显示结果时，在列表内显示筛选无匹配或目录为空的主要提示
               s->filter[UI_FILES_APP][0] ? "No matching files" : "This folder is empty", U_LABEL,  // 有查询却无结果与目录本身为空分别报告
               U_TEXT);  // 该主要提示使用正文文字色
        ui_app_muted(x + 18, r.y + 207, "Try another search or create a document.");  // 在整屏x=x + 18、内容顶边下移207像素显示文字"Try another search or create a document."
    }
    char total[96];  // 96字节状态文字保存当前目录条数和选择状态
    airi_snprintf(total, 96, "%u items / %u selected", s->file_count, s->file_count ? 1u : 0u);  // Files状态栏报告原目录快照条数，当前单选模型在非空目录报告1项选择
    ui_app_status(r, total, "Enter: open / Delete: remove / Ctrl-F: search");  // 在内容底部显示目录条数与选择状态文字；右侧说明按测得的文字宽度对齐
    if (s->file_clipboard >= 0)  // 文件剪贴板已有服务身份时才显示Paste按钮
        u_button((struct ui_rect){x + 426, r.y + r.h - 70, 76, 30}, "Paste", true, false);  // 画文字"Paste"按钮；其矩形供后续事件命中共用，当前仅输出像素
    if (s->file_count) {  // 快照非空时才显示打开、重命名、删除和文件剪贴板操作按钮
        u_button((struct ui_rect){x + 4, r.y + r.h - 70, 76, 30}, "Open", true, false);  // 画文字"Open"按钮；其矩形供后续事件命中共用，当前仅输出像素
        u_button((struct ui_rect){x + 90, r.y + r.h - 70, 84, 30}, "Rename", false, false);  // 画文字"Rename"按钮；其矩形供后续事件命中共用，当前仅输出像素
        u_button((struct ui_rect){x + 184, r.y + r.h - 70, 76, 30}, "Delete", false, false);  // 画文字"Delete"按钮；其矩形供后续事件命中共用，当前仅输出像素
        u_button((struct ui_rect){x + 270, r.y + r.h - 70, 68, 30}, "Copy", false, false);  // 画文字"Copy"按钮；其矩形供后续事件命中共用，当前仅输出像素
        u_button((struct ui_rect){x + 348, r.y + r.h - 70, 68, 30}, "Cut", false, false);  // 画文字"Cut"按钮；其矩形供后续事件命中共用，当前仅输出像素
    }
}

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

