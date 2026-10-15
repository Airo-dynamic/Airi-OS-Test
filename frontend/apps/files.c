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

