#include "internal.h"  // 引入当前目录内部共享接口和基础函数

void ui_open_document(struct ui_desktop *s, unsigned index) {  // 按快照下标打开目录或读取文档，成功才切换编辑身份
    if (index >= s->file_count)  // 快照下标必须落在这次查询的有效前缀
        return;  // 当前按快照下标打开目录或读取文档，成功才切换编辑身份在本分支结束，后续语句不执行
    struct ui_file *f = &s->files[index];  // 借用快照第index项，后面用其服务id而非下标读取
    if (f->directory) {  // 目录项改变当前目录，不当成文字文档读取
        airi_snprintf(s->directory, 128, "%s", f->path);  // 用目录记录的完整路径替换当前查询目录
        ui_refresh(s);  // 以当前directory重新复制文件快照；任务回调接入后同次刷新它和资源统计
        return;  // 当前按快照下标打开目录或读取文档，成功才切换编辑身份在本分支结束，后续语句不执行
    }
    if (s->dirty) {  // 当前文档未保存时先阻止另一个文档覆盖编辑数组
        ui_app_dialog(s, UI_DIALOG_ERROR, 0, "Unsaved changes",  // 保存文字"Unsaved changes"的种类、目标和说明，最后绘图且优先接收后续事件
                      "Save or discard the current document before opening another file.", "");  // 说明未保存正文阻止新文档覆盖，最后空串表示此错误框没有名称初值
        return;  // 当前按快照下标打开目录或读取文档，成功才切换编辑身份在本分支结束，后续语句不执行
    }
    if (!s->backend.read ||  // 先检查读取函数地址是否存在，短路或保证缺回调时不调用空函数指针
        !s->backend.read(s->backend.context, f->id, s->editor, sizeof(s->editor))) {  // 按服务id读取文档，缺回调或失败时保留原文并显示原因
        ui_app_dialog(s, UI_DIALOG_ERROR, 0, "Unable to open file",  // 保存文字"Unable to open file"的种类、目标和说明，最后绘图且优先接收后续事件
                      "The file service did not return the document.", "");  // 说明读取服务失败，错误框无需名称字段，原编辑正文保持完整
        return;  // 当前按快照下标打开目录或读取文档，成功才切换编辑身份在本分支结束，后续语句不执行
    }
    s->editor_id = f->id;  // 读取成功后才让编辑器绑定新文件的服务身份
    airi_snprintf(s->editor_name, 64, "%s", f->name);  // 完整读取成功后复制文件名作为Editor身份说明，服务id另存于editor_id
    airi_snprintf(s->saved, UI_DOC_SIZE, "%s", s->editor);  // 读入文档时为当前内容建立独立保存副本
    s->dirty = false;  // 当前文字与保存副本一致，清除未保存标记
    s->cursor = 0;     // 新文档光标从第一个字符前的间隙开始
    s->editor_scroll = 0;  // 垂直滚动回到第一行
    s->editor_column_scroll = 0;  // 水平滚动回到第0列
    s->selection = false;   // 取消现有文档选区，anchor不再参与当前显示
    s->undo_valid = false;  // 新文档不沿用之前文档的撤销副本
    ui_open(s, UI_EDITOR);  // 将已读入的文档所在Editor窗口打开并置顶，正文数组不会再次复制
}
