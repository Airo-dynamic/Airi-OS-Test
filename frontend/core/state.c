#include "internal.h"  // 引入当前目录内部共享接口和基础函数
static bool appearance(struct airi_console *console, const char *command, void *context) {  // 本扩展只消费完整的主题命令。
    struct ui_desktop *desktop = context;  // 原样取回 ui_init 绑定的桌面，数据由调用平台拥有。
    if (airi_strlen(command) == 9 && !airi_memcmp(command, "theme day", 9))  // 比较长度后再比较字节，不接受附带尾部参数。
        desktop->theme = UI_DAY;  // 只修改呈现状态，文档和窗口保留。
    else if (airi_strlen(command) == 11 && !airi_memcmp(command, "theme night", 11))  // 夜间命令具有自己的完整长度。
        desktop->theme = UI_NIGHT;  // 将下一帧使用的调色板切回夜间。
    else if (airi_strlen(command) != 5 || airi_memcmp(command, "theme", 5))  // 无参数 theme 是查询，其余文字不归本扩展。
        return false;  // 让文本解释器生成原有未知命令错误。
    airi_console_print(console, "Appearance: %s / theme day | theme night", desktop->theme ? "Day" : "Night");  // 查询和设置都报告实际呈现状态。
    return true;  // 表示本条命令已经同步处理。
}
void ui_refresh(struct ui_desktop *s) {  // 重新查询当前目录元信息，并重新建立文件快照的有效前缀
    s->file_count = 0;  // 刷新前清有效文件条数，失败时不显示旧成功快照
    if (s->service_state != UI_NORMAL)  // 服务未正常时停止复制或显示成功快照
        return;  // 服务状态不是NORMAL，保持有效文件计数为0，不调用文件查询回调
    if (s->backend.files)  // 文件查询回调存在时才调用
        s->file_count = s->backend.files(s->backend.context, s->directory, s->files, UI_FILES);  // 查询当前目录并复制最多32项元信息到桌面数组，返回值标出此次有效前缀
    if (s->file_count > UI_FILES)  // 服务条数超过本地32槽容量时收窄
        s->file_count = UI_FILES;  // 把服务返回条数限制到本地文件快照容量
    if (s->selection_file >= s->file_count)  // 旧选择不在新文件快照有效范围时归零
        s->selection_file = 0;  // 旧选中下标失效时回到新快照第一项
}

void ui_init(struct ui_desktop *s, struct ui_backend b, struct airi_console *c) {  // 清桌面状态，绑定服务和console并打开默认应用
    airi_memset(s, 0, sizeof(*s));  // 清零整个当前状态或快照，使计数和标记从确定初值开始
    s->backend = b;  // 把回调表和上下文复制进桌面，服务数据仍归服务拥有
    c->extension = appearance;  // 安装同步命令扩展；console 不包含主题或窗口类型。
    c->extension_context = s;   // 桌面和 console 都由平台持有，须覆盖后续输入调用。
    s->console = c;     // 桌面借用同一console状态，Terminal不另复制日志
    s->active = -1;     // 没有活动窗口，-1哨兵不能拿去索引应用数组
    s->editor_id = -1;  // 编辑器尚未绑定任何服务文件身份
    s->file_clipboard = -1;  // 文件剪贴板没有服务id，不能发起复制或移动
    s->width = 1280;  // 绘图前暂用1280像素默认画布宽度
    s->height = 800;  // 绘图前暂用800像素默认画布高度
    copy(s->directory, sizeof(s->directory), "/home/airi/projects/telemetry");  // 把文字"/home/airi/projects/telemetry"复制到当前目录路径，sizeof(s->directory)是包含NUL的数组容量
    copy(s->editor_name, sizeof(s->editor_name), "Untitled");  // 把文字"Untitled"复制到当前文档名，sizeof(s->editor_name)是包含NUL的数组容量
    ui_refresh(s);  // 以当前directory重新复制文件快照；任务回调接入后同次刷新它和资源统计
    ui_open(s, b.mode == UI_REFERENCE ? UI_EDITOR : UI_TERMINAL);  // 打开或置顶一个应用，更新窗口次序和活动编号，本次实参取自当前桌面状态
    if (b.mode == UI_REFERENCE && s->file_count && b.read) {  // 参考服务有文件且提供read时，才尝试读取一份初始普通文档
        for (unsigned i = 0; i < s->file_count; ++i)  // 只遍历本次目录快照有效前缀，未用槽不参与
            if (!s->files[i].directory) {  // 跳过目录元信息，首个普通文件才可能作为Editor初始文档
                if (b.read(b.context, s->files[i].id, s->editor, sizeof(s->editor))) {  // 按服务id完整读入成功后才绑定编辑身份、名称和保存副本
                    s->editor_id = s->files[i].id;  // 读取或保存成功后绑定该快照项的服务id，后续保存用此身份而不用显示下标
                    copy(s->editor_name, sizeof(s->editor_name), s->files[i].name);  // 把s->files[i].name复制到当前文档名，sizeof(s->editor_name)是包含NUL的数组容量
                    copy(s->saved, sizeof(s->saved), s->editor);  // 把服务已接受的文字记录为独立保存点
                }
                break;  // 结束当前最近一层循环或应用分派，不执行其剩余候选
            }
    }
}

void ui_open(struct ui_desktop *s, enum ui_app app) {  // 打开或置顶一个应用，更新窗口次序和活动编号
    if (app >= UI_APP_COUNT)  // 应用编号超出窗口数组范围时拒绝打开
        return;  // 当前打开或置顶一个应用，更新窗口次序和活动编号在本分支结束，后续语句不执行
    struct ui_window *w = &s->windows[app];  // 借用当前应用的窗口状态，app已经通过范围检查
    if (!w->open) {  // 只在首次打开或关闭后重新设置窗口初始几何
        int offset = (int)s->order_count * 24;  // 每个已排入次序的窗口错开24像素，新窗口避免完全重合
        w->rect = (struct ui_rect){92 + offset, 72 + offset, s->width - 152, s->height - 144};  // 新窗口从屏幕左上留92和72像素并按已有数量错开，宽高先扣固定边距
        if (w->rect.w < 720)  // 初始窗口宽度至少720像素，画布尺寸后续再约束
            w->rect.w = 720;  // 初次打开宽度至少720像素；这里建立初始布局，ui_draw再按实际画布收窄
        if (w->rect.h < 480)  // 初始窗口高度至少480像素，画布尺寸后续再约束
            w->rect.h = 480;  // 初次打开高度至少480像素；当前画布不足时ui_draw再收窄可见矩形
        w->restore = w->rect;  // 保留当前普通窗口几何，退出最大化时可恢复
        w->open = true;  // 该应用窗口现在进入已打开状态
    }
    w->minimized = false;  // 重新打开或激活窗口时取消最小化
    s->active = (int)app;  // 令本次打开的应用成为按键与窗口操作接收者
    unsigned n = 0;  // 去重后的窗口次序从空前缀开始，n计保留的应用编号条数
    for (unsigned i = 0; i < s->order_count; ++i)  // 按有效窗口层次遍历，数组后部不作窗口身份
        if (s->order[i] != (unsigned)app)  // 重排时保留其他应用，当前应用将另放到末尾
            s->order[n++] = s->order[i];  // 保留其他应用次序并压紧前缀，当前应用稍后追加
    s->order[n++] = (unsigned)app;  // 把当前应用放到绘图顺序末尾，使它成为顶层
    s->order_count = n;  // 记录去重后绘图顺序的有效长度
    s->focus = UI_FOCUS_NONE;  // 当前字段结束接收，后续按键回到活动应用
}

void ui_service_state(struct ui_desktop *s, enum ui_state state) {  // 切换服务显示状态并刷新相应快照
    s->service_state = state;  // 用调用者指定状态切换服务反馈，随后刷新决定是否继续查询快照
    ui_refresh(s);  // 以当前directory重新复制文件快照；任务回调接入后同次刷新它和资源统计
}

