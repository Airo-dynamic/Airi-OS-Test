#include "internal.h"  // 引入当前目录内部共享接口和基础函数
void ui_confirm(struct ui_desktop *s) {  // 把当前对话框确认转换成明确服务动作
    int kind = s->dialog;  // 先保存待确认种类，再清除显示状态并执行对应动作
    s->dialog = UI_DIALOG_NONE;     // 关闭当前对话框，后续键不再被它优先接收
    if (kind == UI_DIALOG_CLOSE) {  // 未保存关闭的确认先尝试保存，后面的服务成功出口才真正关闭窗口
        s->pending_close = true;  // 记录这是关闭前保存，只有保存成功才关闭Editor
        if (s->editor_id < 0) {   // 新文档尚无服务记录身份，保存前先打开另存名称框
            s->dialog = UI_DIALOG_SAVE_AS;  // 没有文件身份时先请求新名称，暂未调用保存服务
            copy(s->dialog_title, 64, "Save document");  // 把文字"Save document"复制到对话框标题，64是包含NUL的数组容量
            copy(s->dialog_text, 256, "Choose a file name in the current workspace.");  // 把文字"Choose a file name in the current workspace."复制到对话框失败或确认说明，256是包含NUL的数组容量
            copy(s->dialog_input, 128, "untitled.lua");  // 把文字"untitled.lua"复制到对话框输入字段，128是包含NUL的数组容量
            s->field_all = true;  // 输入字段内容设为整体选中，下一次键入可以替换
        } else  // 前一分支条件未成立时转入随后的单条替代操作
            ui_command(s, UI_SAVE, s->editor_id, s->editor_name, s->editor);  // 提交当前文档id、名称与正文，只有服务成功才更新前端保存点
    }
    if (kind == UI_DIALOG_NEW_FILE)  // 新建文件确认把名称框提交给当前目录的创建普通文件动作
        ui_command(s, UI_CREATE_FILE, 0, s->dialog_input, "");  // 以当前目录和已确认名称创建空普通文件，id为0表示此动作不引用已有记录
    if (kind == UI_DIALOG_NEW_DIR)  // 新建目录确认提交名称并创建目录元信息，不创建文档正文
        ui_command(s, UI_CREATE_DIR, 0, s->dialog_input, "");  // 以当前目录和已确认名称创建目录项，正文传空串
    if (kind == UI_DIALOG_RENAME)  // 重命名确认使用弹框前保存的目标服务id，避免当前显示排序改变身份
        ui_command(s, UI_RENAME, s->dialog_target, s->dialog_input, "");  // 向固定目标服务id提交新名称，正文未参与重命名
    if (kind == UI_DIALOG_DELETE)  // 删除确认提交目标id，服务仍会检查它存在且目录可删除
        ui_command(s, UI_DELETE, s->dialog_target, "", "");  // 提交已确认的目标id删除请求，名称与正文均不需要
    if (kind == UI_DIALOG_STOP)  // 停止确认提交当前参考任务pid，服务只移除此记录而不结束真实进程
        ui_command(s, UI_STOP, s->dialog_target, "", "");  // 把已确认参考pid交给停止动作，只移除服务记录，不结束真实进程
    if (kind == UI_DIALOG_SAVE_AS)  // 另存提交id为-1的新文档请求，成功后刷新并重新绑定Editor身份
        ui_command(s, UI_SAVE, s->editor_id, s->dialog_input, s->editor);  // 提交另存名称与当前正文，未绑定时id为-1，成功后前端从新快照绑定新id
}
