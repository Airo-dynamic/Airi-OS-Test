# G22 · 让按钮呈现悬停、按下与释放

窗口已经会响应按下与拖动，但移动到Save按钮时仍只有固定外观。现在把同一指针位置交给绘图，让按钮在悬停、按下和释放后呈现不同底面。Save动作仍由按下事件的输入分派执行。没有持续拖动时，phase=0只更新位置与下一帧反馈；已有窗口拖动或文本拖选时，它还会更新几何或选区，仍不会调用Save。绘图将这份状态变成像素。

<a id="feedback-events"></a>

ui_pointer保存屏幕坐标与pointer_down。按下或双击令pointer_down为true，释放先清false，再结束拖动；即使释放在按钮外，也不能保留旧按压。绘图无需重新猜鼠标状态，只借用这份会话记录。移动窗口和拖选文字同样会更新位置，后续帧看到的是本次事件的坐标。

**完整写入 `frontend/include/ui.h`：**

<a id="code-frontend-include-ui-h-1"></a>

<!-- write: frontend/include/ui.h -->
```c
#ifndef AIRI_UI_H  // 首次包含才展开下面声明，避免类型重复定义。
#define AIRI_UI_H  // 标记这个头文件已经进入当前编译单元。
#include <airi/console.h>  // 取得命令环、按键与视图类型。
#include "render.h"  // 取得像素、矩形与共享绘图声明。
#define UI_DOC_SIZE 8192  // 文档总容量含结尾NUL，最多保存8191文本字节。
#define UI_FILES 32  // 单次文件快照至多32条。
#define UI_TASKS 16  // 单次任务快照至多16条。

enum ui_app {  // 应用身份从零递增，与平台名称表和窗口数组下标对应。
    UI_TERMINAL,   // 终端应用，读取同一console命令对象。
    UI_FILES_APP,  // 文件列表应用，读取服务快照。
    UI_EDITOR,     // 维护文档工作副本的编辑器。
    UI_TASKS_APP,  // 显示任务记录和服务统计。
    UI_SYSTEM,     // 展示启动、内存、设备与日志。
    UI_SETTINGS,   // 修改界面主题与密度。
    UI_VIEWER,    // 绘制已成功解析的CSV数据。
    UI_APP_COUNT  // 前面有效应用数量，作为数组容量和循环上界。
};
enum ui_mode { UI_LIVE, UI_REFERENCE };  // 区分内核真实服务与主机会话参考服务。
enum ui_state { UI_NORMAL, UI_EMPTY, UI_LOADING, UI_ERROR, UI_OFFLINE };  // 列表和动作的可见服务状态，不等同线程调度状态。
enum ui_action {  // 回调接收的服务动作，同一动作具有明确文件或任务目标。
    UI_SAVE,  // 将当前字节写回文档id对应记录。
    UI_CREATE_FILE,  // 在显式目录下建立普通文件。
    UI_CREATE_DIR,   // 建立目录记录。
    UI_RENAME,   // 保留id，修改对应记录名称。
    UI_DELETE,   // 删除id对应记录。
    UI_EXECUTE,  // 请求执行文档，当前参考服务报告未连接。
    UI_STOP,     // 删除可停止的参考任务记录，不终止宿主进程。
    UI_COPY_FILE,  // 在目标目录创建源文件的副本。
    UI_MOVE_FILE   // 把源id对应记录移到目标目录。
};
enum ui_dialog { UI_DIALOG_NONE, UI_DIALOG_ERROR, UI_DIALOG_CLOSE, UI_DIALOG_NEW_FILE, UI_DIALOG_NEW_DIR, UI_DIALOG_RENAME, UI_DIALOG_DELETE, UI_DIALOG_STOP, UI_DIALOG_SAVE_AS, UI_DIALOG_INFO };  // 对话框类型决定输入、确认与取消后续动作；NONE为零。
enum ui_focus { UI_FOCUS_NONE, UI_FOCUS_FILTER, UI_FOCUS_FIND };  // 键入目标为应用正文、筛选字段或查找字段。
enum ui_drag { UI_DRAG_NONE = -1, UI_DRAG_RESIZE = 2, UI_DRAG_MOVE = 1, UI_DRAG_SELECT = 3 };  // -1明确无拖动，其余值区分跨事件延续的操作。
enum ui_key {  // 功能键编号与ASCII字节分开，避免把命令误插入正文。
    UI_LEFT = 512,  // 功能键从512起，不与0..127的文本字节混淆。
    UI_RIGHT,  // 后续枚举自动递增，表示右移光标。
    UI_HOME,   // 光标定位当前行开头。
    UI_END,    // 光标定位当前行末尾。
    UI_DELETE_KEY,  // 删除光标后面的字节或文件列表项。
    UI_ESCAPE,      // 取消当前框或字段操作。
    UI_TAB,  // Editor正文收到此键时插入四个空格
    UI_SAVE_KEY,    // 请求保存当前文档。
    UI_FIND_KEY,    // 打开查找字段。
    UI_SELECT_ALL,  // 当前输入对象全选。
    UI_COPY,     // 把选区复制到文本剪贴板。
    UI_CUT,      // 复制选区后删除文本。
    UI_PASTE,    // 按容量与ASCII限制插入剪贴板。
    UI_UNDO,     // 交换前后文本快照。
    UI_RUN_KEY,  // 请求执行当前文档。
    UI_NEXT_WINDOW  // 活动窗口切到绘制顺序中的下一项。
};
struct ui_file {  // 服务复制出来的一条文件快照，不保存文件内容。
    int id;  // 稳定服务身份，排序和筛选不改变它。
    bool directory;  // 区分目录和普通文件，决定打开后的动作。
    char name[64], path[128];  // 保存有NUL终止的名称与完整路径，容量分别64/128字节。
    unsigned bytes;  // 文件内容字节数，目录行可显示其他语义。
};
struct ui_task {  // 当前服务提供的一条任务记录。
    int pid;  // 任务服务身份，停止动作传它而不是可见行号。
    char name[32], state[16];  // 保存任务名称和状态文本，均含NUL容量。
    unsigned cpu, memory_kib;  // CPU百分数与内存KiB，绘图按对应单位输出。
};
struct ui_stats {  // 一次任务查询同时返回的汇总状态。
    bool available;  // false表示当前没有真实汇总数据，不能绘制伪装的零值。
    unsigned cpu, memory_kib, total_kib, read_kib, write_kib;  // CPU百分数、内存/总量/读写KiB分别保存。
    unsigned samples[60];  // 最多60个固定窗口采样值，图表逐项消费。
};
/* Providers own service data. A backend error is an interface state, never fake success. */
struct ui_backend {  // 桌面复制这份服务回调表，服务数据继续由提供方持有。
    enum ui_mode mode;  // 决定当前服务来自真实内核或主机参考进程。
    const AiriBootInfo *boot;  // 借用运行期间有效的启动描述，不转移其所有权。
    const char *label;  // 借用用于标明环境的文字。
    void *context;      // 不透明服务上下文，回调原样接收并自行解释。
    unsigned (*files)(void *, const char *, struct ui_file *, unsigned);  // 函数指针接收上下文/目录/输出数组/容量，返回实际条数。
    bool (*read)(void *, int, char *, size_t);  // 按文件id读取字节到给定容量数组，false表示失败。
    bool (*action)(void *, const char *, enum ui_action, int, const char *, const char *, char *, size_t);  // 接收目录、动作、id、名称、正文及错误缓冲，返回成功与否。
    unsigned (*tasks)(void *, struct ui_task *, unsigned, struct ui_stats *);  // 输出任务数组与汇总，返回有效任务数。
};
struct ui_window {  // 一个应用的窗口布局与显示状态。
    struct ui_rect rect;     // 当前整屏窗口矩形，绘图和命中共用
    struct ui_rect restore;  // 最大化之前的普通布局，恢复时读回
    bool open;  // 窗口是否参加绘制
    bool minimized;   // 隐藏正文但保留打开身份
    bool maximized;   // 是否占满扣除系统栏后的可用区
    unsigned tab;     // 当前应用页签下标
    unsigned scroll;  // 此窗口已卷走的行数
};
struct ui_desktop {  // 全部事件修改这一份状态，重画按同一状态取得内容。
    struct ui_backend backend;     // 桌面按值保存回调表；context与boot仍借用服务提供方对象。
    struct airi_console *console;  // 借用稳定命令对象，终端视图与键输入共用。
    struct ui_window windows[UI_APP_COUNT];  // 每个应用一个窗口，枚举值直接作下标。
    unsigned order[UI_APP_COUNT];  // 保存从底到顶的应用身份，命中逆向读取。
    unsigned order_count;  // order中已登记的有效数量。
    int active;   // 活动应用，-1表示当前没有。
    int width;    // 最近绘图的桌面宽度，单位像素。
    int height;   // 最近绘图的桌面高度，单位像素。
    int mouse_x;  // 当前鼠标屏幕横坐标。
    int mouse_y;  // 当前鼠标屏幕纵坐标。
    int drag;     // 当前跨事件拖动种类，-1表示无。
    int drag_x;  // 移窗时按下点距窗口左边的偏移。
    int drag_y;  // 移窗时按下点距窗口顶部的偏移。
    bool pointer_down;  // 鼠标按压状态仅用于绘图反馈
    bool monitor;   // 右侧状态监视区是否展开。
    bool launcher;  // 应用启动菜单是否展开。
    bool find;      // Editor查找栏是否展开。
    bool dirty;     // 工作文本是否与最近成功保存内容不同。
    bool selection;      // anchor与cursor之间是否构成有效选区。
    bool field_all;      // 对话框字段是否处于全选替换状态。
    bool undo_valid;     // undo数组是否有可交换的历史文本。
    bool pending_close;  // 保存成功后是否还需继续关闭Editor。
    enum ui_state service_state;  // 当前界面服务状态，失败动作通过它显示。
    char directory[128];  // 当前文件操作目录，有NUL终止。
    char filter[UI_APP_COUNT][64];  // 每个应用独立筛选文本，二维数组先按应用再按字节。
    char editor[UI_DOC_SIZE];  // 正在修改的工作文本，服务内容不会随按键自动改变。
    char saved[UI_DOC_SIZE];   // 独立数组接收服务实际保存的文本。
    char undo[UI_DOC_SIZE];    // 上次编辑快照，撤销时与工作文本交换。
    char clipboard[UI_DOC_SIZE];  // 独立持有的文本剪贴板，不借用平台临时字符串。
    char find_text[64];  // 查找输入字段，搜索结果映射回文档字节偏移。
    unsigned cursor;     // 光标位于0..文本长度的字节间隙。
    unsigned anchor;     // 选区起点间隙，拖动时固定。
    unsigned editor_scroll;  // 正文顶部已卷走的行数。
    unsigned editor_column_scroll;  // 正文左侧已卷走的字符列数。
    unsigned selection_file;  // 文件快照数组下标，不是文件id或排序后行。
    unsigned selected_task;   // 任务快照数组下标，停止动作再读取pid。
    int editor_id;  // 当前文档服务id，未保存的新文档另按创建路径处理。
    int dialog;     // 当前对话框枚举值，零为无。
    int dialog_target;  // 确认动作锁定的文件id或任务pid。
    int focus;  // 当前键入字段身份。
    int sort;   // 列表排序方向，0/1切换。
    int file_clipboard;  // 文件剪贴板保存源id，-1表示无。
    bool file_cut;  // 粘贴执行移动还是复制。
    char editor_name[64];    // 当前文档名称，保存和执行请求使用。
    char dialog_title[64];   // 确认或错误框标题。
    char dialog_text[256];   // 服务原因与操作说明。
    char dialog_input[128];  // 创建、重命名等动作的可编辑名称。
    char toast[160];  // 临时操作结果或输入拒绝原因。
    struct ui_file files[UI_FILES];  // 当前目录的有界快照记录。
    unsigned file_count;  // files中有效条数，读取不得超过它。
    struct ui_task tasks[UI_TASKS];  // 当前任务服务的有界快照。
    unsigned task_count;    // tasks中有效条数。
    struct ui_stats stats;  // 与任务快照同时取得的汇总。
    unsigned theme;    // 桌面自己持有的日夜模式，文本会话不依赖此字段。
    unsigned density;  // Files/Tasks列表行高，0为36像素、1为28像素；相应绘图和列表命中共用。
    unsigned series_count;  // 已成功解析的曲线点数，失败保留旧值。
    unsigned series_values[60];  // 各点纵轴整数值，与times同下标配对。
    unsigned series_times[60];   // 各点横轴秒数，按当前CSV合同递增。
    char series_name[64];  // 最近成功解析的数据文件名。
};
void ui_init(struct ui_desktop *, struct ui_backend, struct airi_console *);  // 初始化桌面，复制服务表并借用命令对象。
void ui_open(struct ui_desktop *, enum ui_app);  // 打开、解除最小化并置顶指定应用。
void ui_draw(struct ui_desktop *, const struct airi_framebuffer *);  // 按实际帧缓冲绘图，并更新可用于命中的窗口矩形。
void ui_pointer(struct ui_desktop *, int x, int y,  // 鼠标接口接收同一桌面屏幕坐标。
                int phase);  /* 0 move, 1 down, 2 up, 3 double-click */  // phase把平台事件统一成移动/按下/释放/双击。
bool ui_key(struct ui_desktop *, int key, bool shift);  // 分派字符或功能键，Shift控制扩展选区等规则。
void ui_scroll(struct ui_desktop *, int rows);  // 有符号行增量交给活动视图，负方向需限到零。
void ui_service_state(struct ui_desktop *, enum ui_state);  // 切换服务显示状态并刷新对应快照。
void ui_refresh(struct ui_desktop *);  // 从当前服务取得文件、任务与汇总快照。
void ui_command(struct ui_desktop *, enum ui_action, int, const char *, const char *);  // 把稳定身份、名称与内容交给服务动作回调。
void ui_fixture_backend(struct ui_backend *);  // 为主机观察填入会话内存参考服务，不进入guest。

void u_icon(int, int, int, uint32_t);  // 在给定坐标以类型和颜色绘制小图标。
unsigned ui_file_order(const struct ui_desktop *, unsigned *);  // 把当前文件排序位置写成快照下标数组并返回数量。
void ui_open_document(struct ui_desktop *, unsigned);  // 接收快照下标，读取id对应内容并选择Editor或Viewer。
void ui_apps_draw(struct ui_desktop *, enum ui_app, struct ui_rect);  // 以应用身份分派内容绘图，rect已扣除标题栏。
void ui_apps_click(struct ui_desktop *, enum ui_app, struct ui_rect, int, int, bool);  // 将内容矩形、屏幕点与双击状态交给具体应用。
#endif  // 结束完整桌面接口的重复包含保护
```

**完整写入 `frontend/core/pointer.c`：**

<a id="code-frontend-core-pointer-c-1"></a>

<!-- write: frontend/core/pointer.c -->
```c
#include "internal.h"  // 取得应用内部绘图、命中与对话框的共同声明。
#include "../apps/internal.h"  // 借用Editor正文定位器，续拖不调用它的完整按钮分派。
void ui_pointer(struct ui_desktop *s, int x, int y, int phase) {  // phase=0移动、1按下、2释放、3双击；x/y 在桌面屏幕坐标中。
    if (phase == 1 || phase == 3)  // 按下或双击事件开始按压反馈
        s->pointer_down = true;  // 保存按压状态，下一帧按钮可显示U_DOWN
    if (phase == 2)  // 释放无论命中哪里都应结束按压
        s->pointer_down = false;  // 下一帧不再保留旧按压反馈
    s->mouse_x = x;  // 保存当前鼠标横坐标，绘图可以显示指针。
    s->mouse_y = y;  // 保存当前鼠标纵坐标。
    if (phase == 0 && s->drag == UI_DRAG_SELECT) {  // 选择已在按下时开始，移动只续接文字间隙。
        struct ui_rect r = s->windows[UI_EDITOR].rect;  // 使用Editor此刻外框，不保存过期屏幕位置。
        s->cursor = ui_editor_position(s, u_window_body(r), x, y);  // 越界点夹到正文，不发送Save、打开文件或字段事件。
        s->selection = true;  // 固定anchor与当前cursor继续界定半开范围，两端相等时为空。
        return;  // 此移动已处理，窗口和应用按钮不再次消费它。
    }
    if (phase == 0 && s->drag >= 0) {  // 移动时只有已登记拖动才改变窗口。
        struct ui_window *w = &s->windows[s->active];  // 借用发起拖动的活动窗口。
        if (s->drag == UI_DRAG_MOVE) {  // 移窗保持按下点相对窗口位置不变。
            w->rect.x = x - s->drag_x;  // 当前鼠标横坐标减按下偏移得到新左边界。
            w->rect.y = y - s->drag_y;  // 同样得到新上边界。
        } else {  // 当前条件不成立，按随后明确的另一种事件或状态处理。
            w->rect.w = x - w->rect.x;  // 缩放右下角时宽度为鼠标到窗口左边的距离。
            if (w->rect.w < 720)  // 当前内容布局最小支持720像素宽。
                w->rect.w = 720;  // 限住窗口宽度，不能拖成负数或过小区域。
            w->rect.h = y - w->rect.y;  // 高度为鼠标到窗口顶部的距离。
            if (w->rect.h < 480)  // 高度下限为480像素。
                w->rect.h = 480;  // 限住最小高度。
        }
        return;  // 本次事件处理已结束，不再落入其他按钮或底层窗口。
    }
    if (phase == 2) {  // 释放事件结束当前拖动，无需再命中内容。
        s->drag = UI_DRAG_NONE;  // -1 表示没有拖动，下一次移动不会延续选区。
        return;  // 本次事件处理已结束，不再落入其他按钮或底层窗口。
    }
    if (phase != 1 && phase != 3)  // 普通移动且未拖动时不触发应用动作。
        return;  // 本次事件处理已结束，不再落入其他按钮或底层窗口。
    s->toast[0] = 0;  // 新点击清掉旧临时提示，NUL使字符串为空。
    if (s->dialog) {  // 非零对话框优先截获点击，不能让后方窗口同时响应。
        int dx = s->width / 2 - 240, dy = s->height / 2 - 112;  // 对话框以480×224布局围绕桌面中心定位。
        if (s->dialog == UI_DIALOG_CLOSE && u_inside((struct ui_rect){dx + 180, dy + 174, 84, 30}, x, y)) {  // 关闭确认中的取消按钮保持原文并回到编辑。
            s->dialog = UI_DIALOG_NONE;  // 关闭当前确认或消息框。
            s->pending_close = false;    // 清除保存后继续关闭的意图，避免下次保存误关闭。
        } else if (u_inside((struct ui_rect){dx + 276, dy + 174, 84, 30}, x, y)) {  // 第二个按钮在未保存确认中放弃修改，其他框用于返回。
            if (s->dialog == UI_DIALOG_CLOSE) {  // 只有放弃关闭需要恢复文档并关闭Editor。
                copy(s->editor, UI_DOC_SIZE, s->saved);  // 把最近成功保存文本复制回工作数组，不调用服务保存。
                s->cursor = 0;     // 恢复后光标回到第一个字节间隙。
                s->dirty = false;  // 工作文本与saved相同，不再有未保存内容。
                s->undo_valid = false;  // 放弃后旧撤销快照失效，避免把修改重新带回来。
                s->windows[UI_EDITOR].open = false;  // 用户明确放弃后关闭Editor窗口。
                s->active = -1;  // 当前没有活动窗口，不能继续往刚关闭窗口发送键入。
            }
            s->dialog = UI_DIALOG_NONE;  // 关闭当前确认或消息框。
            s->pending_close = false;    // 清除保存后继续关闭的意图，避免下次保存误关闭。
        } else if (u_inside((struct ui_rect){dx + 372, dy + 174, 84, 30}, x, y))  // 最右按钮执行当前确认动作。
            ui_confirm(s);  // 使用已保存dialog_target与输入调用服务，失败仍有明确框。
        return;  // 本次事件处理已结束，不再落入其他按钮或底层窗口。
    }
    if (s->launcher && x < 252 && y >= 82 && y < 82 + UI_APP_COUNT * 42) {  // 已展开的启动器区域按42像素一行选应用。
        ui_open(s, (enum ui_app)((y - 82) / 42));  // 行号经过范围条件后可转换为应用枚举。
        return;  // 本次事件处理已结束，不再落入其他按钮或底层窗口。
    }
    if (y < UI_TOP) {  // 桌面顶部36像素属于全局栏。
        if (x < 140)  // 左端按钮切换应用启动菜单。
            s->launcher = !s->launcher;  // 布尔取反在显示和收起间切换。
        else if (x > s->width - 50)  // 顶栏最右侧切换监视区。
            s->monitor = !s->monitor;  // 保存监视区展开状态，下一帧按它布局。
        return;  // 本次事件处理已结束，不再落入其他按钮或底层窗口。
    }
    s->launcher = false;  // 点击启动器以外区域后收起菜单。
    if (x < UI_DOCK) {    // 左侧56像素应用栏直接打开应用。
        int app = (y - 50) / 54;  // 扣顶部50像素后每54像素一个应用图标。
        if (y >= 50 && app >= 0 && app < UI_APP_COUNT)  // 排除顶部空白和越界图标。
            ui_open(s, (enum ui_app)app);  // 命中窗口置顶并成为活动应用；类型转换把整数恢复为枚举。
        return;  // 本次事件处理已结束，不再落入其他按钮或底层窗口。
    }
    if (y >= s->height - UI_BOTTOM) {  // 桌面底部30像素属于已打开窗口栏。
        int pos = (x - 330) / 88, count = 0;    // 每88像素一项，count只统计已打开应用。
        for (int i = 0; i < UI_APP_COUNT; ++i)  // 逐个检查已有窗口可见性，给底栏中显示的应用分配命中区域。
            if (s->windows[i].open) {  // 底栏只给打开的窗口分配位置。
                if (count++ == pos && x >= 330) {  // 比较当前位置后递增计数，横坐标还需在条目区内。
                    ui_open(s, (enum ui_app)i);  // 打开/解除最小化并置顶命中的窗口。
                    break;  // 当前匹配已完成，停止这一层循环或分支。
                }
            }
        return;  // 本次事件处理已结束，不再落入其他按钮或底层窗口。
    }
    if (s->monitor && s->width >= UI_MONITOR_MIN && x > s->width - UI_MONITOR_W) {  // 宽桌面右侧监视区先于后方窗口响应。
        if (y > s->height - 96 && y < s->height - 64)  // 监视区底部按钮打开任务窗口。
            ui_open(s, UI_TASKS_APP);  // 将Tasks置顶显示实际服务记录。
        return;  // 本次事件处理已结束，不再落入其他按钮或底层窗口。
    }
    for (int n = (int)s->order_count - 1; n >= 0; --n) {  // 从绘制顺序末端逆查，最上方窗口先获得事件。
        int app = (int)s->order[n];  // 顺序数组保存应用身份，不是窗口地址。
        struct ui_window *w = &s->windows[app];  // 借用该应用的窗口状态，后续变更留在桌面中。
        if (!w->open || w->minimized || !u_inside(w->rect, x, y))  // 关闭、最小化或未命中的窗口都不能接收此次点击。
            continue;  // 跳过当前不适用项，继续检查下一项。
        ui_open(s, (enum ui_app)app);    // 命中窗口置顶并成为活动应用；类型转换把整数恢复为枚举。
        struct ui_rect r = w->rect;      // 复制当前命中矩形，接下来拆分标题栏与内容区。
        if (y < r.y + UI_TITLE_H + 1) {  // 窗口标题栏先处理关闭/最大化/最小化/移窗。
            if (x > r.x + r.w - 42) {  // 标题栏最右42像素命中关闭。
                if (app == UI_EDITOR && s->dirty) {  // Editor有未保存内容时不能直接关闭。
                    s->dialog = UI_DIALOG_CLOSE;  // 建立保存、放弃或返回的关闭确认。
                    copy(s->dialog_title, 64, "Unsaved changes");  // 复制确认标题到独立数组。
                    copy(s->dialog_text, 256,  // 复制完整操作说明，容量限制包含NUL。
                         "Save this document, discard changes, or return to editing.");  // 说明当前关闭需要选择的三种路径。
                } else {  // 当前条件不成立，按随后明确的另一种事件或状态处理。
                    w->open = false;  // 无未保存内容可直接关闭当前窗口。
                    s->active = -1;   // 当前没有活动窗口，不能继续往刚关闭窗口发送键入。
                }
                return;  // 本次事件处理已结束，不再落入其他按钮或底层窗口。
            }
            if (x > r.x + r.w - 72) {  // 标题栏关闭按钮左侧命中最大化/恢复。
                w->maximized = !w->maximized;  // 切换最大化状态。
                if (w->maximized)  // 刚进入最大化时需保留此前实际矩形。
                    w->restore = w->rect;  // 恢复位置独立保存，下一次绘图可覆盖rect。
                else  // 条件不成立时采用下面另一种状态更新。
                    w->rect = w->restore;  // 退出最大化时恢复原位置和大小。
                return;  // 本次事件处理已结束，不再落入其他按钮或底层窗口。
            }
            if (x > r.x + r.w - 102) {  // 再向左命中最小化按钮。
                w->minimized = true;  // 保留open与rect，但暂不绘制或接受命中。
                s->active = -1;  // 当前没有活动窗口，不能继续往刚关闭窗口发送键入。
                return;  // 本次事件处理已结束，不再落入其他按钮或底层窗口。
            }
            if (phase == 3) {  // 双击标题栏切换最大化，不开始拖动。
                w->maximized = !w->maximized;  // 切换最大化状态。
                if (w->maximized)  // 刚进入最大化时需保留此前实际矩形。
                    w->restore = w->rect;  // 恢复位置独立保存，下一次绘图可覆盖rect。
                else  // 条件不成立时采用下面另一种状态更新。
                    w->rect = w->restore;  // 退出最大化时恢复原位置和大小。
            } else if (!w->maximized) {  // 普通按下且窗口未最大化才允许移动。
                s->drag = UI_DRAG_MOVE;  // 后续移动事件按窗口移动规则处理。
                s->drag_x = x - r.x;     // 保存按下点距左边界的偏移，防止窗口跳到鼠标处。
                s->drag_y = y - r.y;     // 保存按下点距顶部偏移。
            }
            return;  // 本次事件处理已结束，不再落入其他按钮或底层窗口。
        }
        if (!w->maximized && x > r.x + r.w - 16 && y > r.y + r.h - 16) {  // 普通窗口右下16×16区域启动缩放。
            s->drag = UI_DRAG_RESIZE;  // 后续移动修改宽高，释放结束。
            return;  // 本次事件处理已结束，不再落入其他按钮或底层窗口。
        }
        ui_apps_click(s, (enum ui_app)app, u_window_body(r),  // 未命中窗口框时将去边框/标题栏的内容矩形交给应用。
                      x, y, phase == 3);  // 屏幕点保持不变，应用按内容矩形换算局部位置。
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
void ui_scroll(struct ui_desktop *s, int rows) {  // rows有符号，向上为负、向下为正，活动视图决定存储位置。
    if (s->active == UI_TERMINAL) {  // Terminal卷动命令对象中的逻辑行。
        int n = (int)s->console->scroll + rows;  // 先有符号相加，避免unsigned向下溢出。
        s->console->scroll = n > 0 ? (unsigned)n : 0;  // 负结果限到首行零，正结果才转回unsigned。
    } else if (s->active == UI_EDITOR) {  // Editor使用文档自身行滚动量。
        int n = (int)s->editor_scroll + rows;  // 以有符号临时值计算目标行。
        s->editor_scroll = n > 0 ? (unsigned)n : 0;  // 向上不能越过文档首行。
    } else if (s->active >= 0) {  // 其他活动应用使用窗口自己的列表滚动量。
        int n = (int)s->windows[s->active].scroll + rows;  // 先转换为有符号，再加事件增量。
        s->windows[s->active].scroll = n > 0 ? (unsigned)n : 0;  // 把负数限到零后保存。
    }
}
```


## 按区域决定谁可以显示反馈

<a id="interaction-region"></a>

u_interaction保存本次绘制区域的鼠标位置、按压和响应许可。ui_draw开始绘桌面，遇普通窗口时重新设置许可：窗口必须是active，且没有对话框或启动菜单。底层窗口即使某个按钮与指针重合，也不能得到hover；资源侧栏在窗口之后绘制，也重新取得桌面许可；active=-1不代表资源区不能操作。对话框最后绘制，并单独取得响应许可。

颜色变化仍由u_control处理，禁用位U_INACTIVE覆盖HOT/DOWN；选中、悬停和按压通过按位或组合，按压外观优先。

u_button把当前矩形与整屏指针比较，符合区域许可才加入U_HOT，pointer_down为true时再加入U_DOWN。它没有桌面、文档或后端服务参数，所以不能完成保存。输入入口负责动作，渲染器负责这一次状态的像素；二者使用同一画布坐标；控件反馈按可见按钮矩形，动作分派保留更宽的工具栏命中带。

渲染器现在需要保存 `u_interaction` 传来的指针、按压和许可，供后续 `u_button` 使用。完整替换接口和实现，再让桌面绘图在进入每个区域时设置这些值。文字仍沿 G12 的路径处理：越过右界只跳过本行余下字节，显式换行继续回到 origin。

**完整写入 `frontend/include/render.h`：**

<a id="code-frontend-include-render-h-1"></a>

<!-- write: frontend/include/render.h -->
```c
#ifndef AIRI_RENDER_H   // 同一编译单元只展开一次绘图接口
#define AIRI_RENDER_H   // 记住本头文件已经处理
#include <airi/boot.h>  // 借用前文的帧缓冲描述、固定宽整数和bool
struct ui_rect {  // 用像素数描述半开矩形，右下边界不属于矩形
    int x;  // 左边所在的整屏列号，可为负以表示部分在屏外
    int y;  // 顶边所在的整屏行号，向下增加
    int w;  // 横向像素数，不是右边坐标
    int h;  // 纵向像素数，不是底边坐标
};
void u_begin(const struct airi_framebuffer *);  // 借用已验证的描述与可写像素数组，直到本次绘图结束
void u_clip(struct ui_rect);  // 请求矩形与整幅画布求交，替换当前裁剪区
void u_reset_clip(void);      // 恢复整幅画布的可写范围
void u_rect(int, int, int, int, uint32_t);  // 按左上、宽高及0xRRGGBB填充裁剪内像素
void u_line(int, int, int, int, uint32_t);  // 用整数误差将含两端点的线段变成像素
void u_frame(struct ui_rect, uint32_t);     // 用四条一像素宽矩形绘制边框
bool u_inside(struct ui_rect, int, int);    // 判断整屏坐标是否在半开矩形内
enum ui_font {  // 枚举下标与生成器faces的顺序一一对应
    U_SMALL,  // Plex Sans 12px、字重400，用于次要信息
    U_BODY,   // Plex Sans 14px、字重400，用于正文
    U_LABEL,  // Plex Sans 16px、字重500，用于标签
    U_TITLE,  // Plex Sans 22px、字重500，用于标题
    U_MONO,   // Plex Mono 14px常规字重，用于终端
    U_CODE,   // Plex Mono 16px常规字重，用于代码
    U_MEDIUM,  // Plex Sans 14px、字重500，用于紧凑按钮
    U_DISPLAY  // Plex Sans 32px、字重300，用于大读数
};
void u_text(int, int, const char *, enum ui_font, uint32_t);  // 用度量与覆盖率绘制可打印ASCII文字
int u_text_width(const char *, enum ui_font);  // 累加单行字形advance供文字布局
enum ui_color {  // 数值从24位RGB范围之外开始，名字表示用途而非固定像素
    U_BG = 0x1000000,  // 整幅画布背景，第一个角色的编号
    U_PANEL,    // 标题、工具栏和普通按钮的底面
    U_SURFACE,  // 正文内容区域
    U_RAISED,   // 被选择的列表背景
    U_EDGE,     // 轻分隔和不活动外框
    U_TEXT,     // 主要正文
    U_MUTED,    // 次要说明
    U_ACCENT,   // 粉色主动作与身份标记
    U_BLUE,     // 蓝色焦点与辅助强调
    U_RED,      // 错误和危险操作
    U_WARM,     // 警示与代码数值
    U_PINK,     // 独立身份色，区别于可操作强调
    U_CONTROL,  // 普通可操作边界
    U_SHADOW,     // 窗口或对话框阴影
    U_HOVER,      // 悬停按钮底面
    U_PRESSED,    // 按下按钮底面
    U_DISABLED,   // 禁用文字
    U_SUCCESS,    // 成功状态
    U_ON_ACCENT,  // 粉色实面上的文字
    U_CHROME,  // 标题身份片的表面
    U_INK,     // 标题身份片的前景
    U_COLOR_END  // 半开角色终点，只用于边界检查，不是可绘制颜色
};
enum ui_theme { UI_NIGHT, UI_DAY };  // 下标0夜间、1日间，零初始化自然选择夜间
enum ui_metric {  // 绘图与后续命中共用同一组像素度量
    UI_TOP = 36, UI_DOCK = 56, UI_BOTTOM = 30, UI_TITLE_H = 48,  // 整屏顶部、应用栏、底栏及窗口标题高度
    UI_TOOLBAR_H = 44, UI_STATUS_H = 27,  // 窗口内容顶部和底部的独立区域
    UI_MONITOR_W = 268, UI_MONITOR_MIN = 1120,   // 资源栏宽度及允许展开它的最小画布宽度
    UI_WINDOW_RADIUS = 2, UI_CONTROL_RADIUS = 2  // 外框和控件都使用两像素半径
};
int u_label_width(const char *);  // 按Medium推进量和每字符两像素字距测当前标题标签。
void u_label(int, int, const char *, uint32_t);  // 14px Medium短标签，每字符推进后加两像素字距
void u_plate(struct ui_rect, uint32_t);  // 逐行收窄右缘的窗口身份片
void u_brand(int, int);  // 用已有线段画原创A形系统标识
enum ui_control_state {  // 不同状态占不同二进制位，可以按位或组合
    U_NORMAL = 0,    // 没有任何额外状态位
    U_SELECTED = 1,  // 第0位：当前已选中的动作
    U_HOT = 2,      // 第1位：悬停反馈
    U_DOWN = 4,     // 第2位：按下反馈
    U_FOCUSED = 8,  // 第3位：键盘焦点
    U_INACTIVE = 16  // 第4位：服务不可用，覆盖其他交互强调
};
void u_theme(unsigned);      // 选择本轮绘图调色板，不拥有应用的会话数据
uint32_t u_color(uint32_t);  // 角色解析成RGB，0至0xffffff的真实RGB原样通过
void u_round(struct ui_rect, int, uint32_t);  // 圆角边缘按4×4样本混合，仍遵守裁剪
void u_control(struct ui_rect, const char *, unsigned);  // 按调用者明确给出的状态绘制按钮，不执行动作
void u_interaction(int, int, bool, bool);  // 本次绘制区域的指针位置、按压与响应许可
void u_button(struct ui_rect, const char *, bool, bool);  // 将selected与disabled转换为共同状态表
static inline struct ui_rect u_window_body(struct ui_rect r) {  // 由外框得到标题下方的应用矩形
    return (struct ui_rect){r.x + 1, r.y + UI_TITLE_H + 1, r.w - 2, r.h - UI_TITLE_H - 2};  // 左右各去一像素，顶部去48像素标题及一像素边框
}
static inline void u_selection(struct ui_rect r) {  // 列表选择共用底色和左侧形状标记
    u_rect(r.x, r.y, r.w, r.h, U_RAISED);  // 先铺整个当前选择区域，文字随后画在其上
    u_rect(r.x, r.y + 4, 3, r.h - 8, U_ACCENT);  // 三像素侧标距上下各四像素，颜色变化同时伴随形状
}
#endif  // 结束包含保护
```

**完整写入 `frontend/render/draw.c`：**

<a id="code-frontend-render-draw-c-1"></a>

<!-- write: frontend/render/draw.c -->
```c
#include "render.h"  // 取得矩形、画布描述与本文件提供的接口
#include "../assets/font-atlas.h"  // 借用主机生成的八套字形度量和覆盖率
static const struct airi_framebuffer *fb;  // 借用本次调用者持有的描述，不拥有像素内存
static bool standard;  // 三个8位通道是否恰好组成0xRRGGBB
static struct ui_rect clip;  // 所有图元共同使用的当前可写范围
static int pointer_x, pointer_y;  // 当前绘制区域借用的整屏指针坐标
static bool pointer_down, interactive;  // 按压状态与当前区域是否有资格显示反馈
static unsigned theme;  // 当前绘图使用的调色板下标，单线程调用期间由u_theme设置
static const uint32_t palettes[2][U_COLOR_END - U_BG] = {  // 两行按ui_color角色顺序保存颜色
    {0x0d131c, 0x172130, 0x192330, 0x3b2c38, 0x46566b,  // 夜间：背景、框架、内容、选中和分隔
     0xe7ebf5, 0xa9b2c6, 0xe5a6c7, 0x83aff7, 0xf0919e, 0xe5ba73,  // 夜间文字和状态
     0xdd99be, 0x758198, 0x0b0e16, 0x2b3244, 0x493442, 0x7e889b, 0x76c8ad, 0x191d29, 0xe7c9d6, 0x39283a},  // 夜间控件
    {0xeff1f7, 0xf0f2f8, 0xfafbfe, 0xf4e2eb, 0xd5dae6,  // 日间表面逐层区分
     0x24283b, 0x586174, 0xa13f70, 0x3564cb, 0xb92f4f, 0x845c15,  // 日间文字和状态
     0xaa487b, 0x7b849a, 0xdce1ed, 0xe4e9f3, 0xf0dce7, 0x7a8294, 0x2a7265, 0xffffff, 0x4b3046, 0xfff4fa}  // 日间控件
};
void u_theme(unsigned mode) {  // 非法模式退回夜间，避免越界取调色板
    theme = mode == UI_DAY ? UI_DAY : UI_NIGHT;  // 只保留有效枚举
}
uint32_t u_color(uint32_t value) {  // 字形和图元共用同一套颜色解析
    if (value >= U_BG && value < U_COLOR_END)  // 只将完整角色范围作为数组下标
        return palettes[theme][value - U_BG];  // 减去首角色得到当前主题行中的列号
    return value;  // 普通RGB和范围外数值原样返回，后者不索引调色板
}
static uint32_t native(uint32_t rgb) {  // 把颜色角色或八位RGB转换成设备布局
    rgb = u_color(rgb);  // 先解析角色，之后的位域计算只处理真实RGB
    if (standard)  // 常见布局不需要逐通道缩放和移位
        return rgb;  // 直接写入已有0xRRGGBB数值
    const unsigned shifts[] = {fb->red_shift, fb->green_shift, fb->blue_shift};  // 通道在设备像素中的最低位
    const unsigned sizes[] = {fb->red_size, fb->green_size, fb->blue_size};  // 同下标保存该通道的1至8位宽度
    uint32_t result = 0;  // 三个不重叠的通道从零组合
    for (unsigned k = 0; k < 3; ++k) {  // 按红、绿、蓝顺序逐个转换
        unsigned channel = (rgb >> ((2 - k) * 8)) & 255u;  // 右移16、8、0位取得当前八位颜色
        unsigned maximum = (1u << sizes[k]) - 1;     // 例如5位通道最大值为31
        unsigned scaled = channel * maximum / 255u;  // 将0至255映射为设备通道范围，整数除法向下取整
        result |= scaled << shifts[k];  // 移到设备位域后按位或加入结果
    }
    return result;  // 返回可按小端字节次序写入的设备像素值
}
void u_begin(const struct airi_framebuffer *f) {  // 调用者必须保证描述及整幅像素内存有效
    fb = f;  // 保存借用，之后的图元通过它定位像素
    standard = f->red_size == 8 && f->green_size == 8 && f->blue_size == 8 &&  // 先确认三色都占八位
               f->red_shift == 16 && f->green_shift == 8 && f->blue_shift == 0;  // 再确认各通道恰好位于RGB数值的位置
    u_reset_clip();  // 新画布从整屏范围开始，不继承此前裁剪
}
void u_reset_clip(void) {  // 恢复当前画布的整屏半开范围
    clip = (struct ui_rect){0, 0, (int)fb->width, (int)fb->height};  // 复合字面量按x、y、w、h构造矩形
}
void u_clip(struct ui_rect r) {  // 将请求限制到当前画布，不与上一次裁剪叠加
    int64_t right = (int64_t)r.x + r.w;   // 先提升为64位再相加，避免两个int的边界和溢出
    int64_t bottom = (int64_t)r.y + r.h;  // 保存请求底边，负起点仍能保留原来的终点
    int left = r.x < 0 ? 0 : r.x;    // 左边至少是画布第0列
    int top = r.y < 0 ? 0 : r.y;     // 顶边至少是画布第0行
    if (right > (int64_t)fb->width)  // 超出整屏右边时收窄终点
        right = (int64_t)fb->width;  // width是最后一列之后的半开边界
    if (bottom > (int64_t)fb->height)  // 超出整屏底边时收窄终点
        bottom = (int64_t)fb->height;  // height是最后一行之后的半开边界
    if (r.w <= 0 || r.h <= 0 || right <= left || bottom <= top) {  // 空请求或完全在屏外的请求没有可写交集
        clip = (struct ui_rect){0, 0, 0, 0};  // 保存空范围，随后图元直接返回
        return;  // 不把负尺寸转换成巨大的无符号范围
    }
    clip = (struct ui_rect){left, top, (int)(right - left), (int)(bottom - top)};  // 交集在已验证的屏幕尺寸内，可转回int
}
bool u_inside(struct ui_rect r, int x, int y) {  // 绘图与后续命中测试共用边界规则
    return r.w > 0 && r.h > 0 && x >= r.x && y >= r.y &&  // 左顶包含，空或负尺寸永不命中
           (int64_t)x < (int64_t)r.x + r.w && (int64_t)y < (int64_t)r.y + r.h;  // 右下排除，加法先提升以免int回绕
}
void u_rect(int x, int y, int w, int h, uint32_t rgb) {  // 写绘制矩形与当前裁剪区的交集
    if (w <= 0 || h <= 0)  // 非正尺寸没有任何像素
        return;  // 在计算地址前结束空绘制
    int a = x > clip.x ? x : clip.x;  // 交集左边取两者较大值
    int b = y > clip.y ? y : clip.y;  // 交集顶边取两者较大值
    int64_t right = (int64_t)x + w;   // 绘制右边在64位中计算
    int64_t bottom = (int64_t)y + h;  // 绘制底边在64位中计算
    int64_t clip_right = (int64_t)clip.x + clip.w;   // 当前裁剪的半开右边
    int64_t clip_bottom = (int64_t)clip.y + clip.h;  // 当前裁剪的半开底边
    if (right > clip_right)  // 写入不能越过裁剪右边
        right = clip_right;  // 交集右边取较小终点
    if (bottom > clip_bottom)  // 写入不能越过裁剪底边
        bottom = clip_bottom;  // 交集底边取较小终点
    if (right <= a || bottom <= b)  // 求交后没有行或列需要写入
        return;  // 不形成任何像素地址
    int c = (int)right;   // 有效交集受画布限制，转换为循环半开列终点
    int d = (int)bottom;  // 同样取得半开行终点
    uint32_t color = native(rgb);  // 将输入RGB编码为当前设备像素值
    unsigned bytes = fb->bpp / 8;  // 24位像素用3字节，32位用4字节
    for (int row = b; row < d; ++row) {  // 从交集顶行逐行写到d之前
        volatile uint8_t *p = (volatile uint8_t *)(uintptr_t)fb->virtual_address +  // 将数值地址还原成可读写的像素字节指针
                              (uint64_t)row * fb->pitch + (uint64_t)a * bytes;  // 行距用字节计算，再加入首列的像素字节偏移
        if (bytes == 4 && !((uintptr_t)p & 3u)) {  // 四字节像素且地址低两位为零时采用对齐写入
            volatile uint32_t *q = (volatile uint32_t *)p;  // 当前行首可按一个uint32_t一个像素访问
            for (int col = a; col < c; ++col) {  // 写交集中的每一列，不碰行尾填充
                *q = color;  // 将一个完整设备像素值写入当前位置
                ++q;  // uint32_t指针推进一个元素，跨过四字节
            }
        } else {  // 三字节或非对齐像素使用字节访问
            for (int col = a; col < c; ++col) {  // 仍只写同一交集列范围
                for (unsigned k = 0; k < bytes; ++k)  // 每次写像素内的一个字节
                    p[k] = (uint8_t)(color >> (k * 8));  // 先写低八位，按x86-64的小端次序保存数值
                p += bytes;  // 跨过当前像素，到本行的下一列
            }
        }
    }
}
void u_line(int x0, int y0, int x1, int y1, uint32_t color) {  // 包含起终点的整数栅格线段
    int64_t dx = x1 > x0 ? (int64_t)x1 - x0 : (int64_t)x0 - x1;  // 横向距离的绝对值，先提升避免差值溢出
    int64_t dy = y1 > y0 ? (int64_t)y0 - y1 : (int64_t)y1 - y0;  // 纵向距离的相反数，进入同一误差式
    int sx = x0 < x1 ? 1 : -1;  // 当前列向终点每次移动一格的方向
    int sy = y0 < y1 ? 1 : -1;  // 当前行向终点每次移动一格的方向
    int64_t error = dx + dy;    // 以横距减纵距作为初始整数误差
    for (;;) {  // 只有绘制完终点才退出
        u_rect(x0, y0, 1, 1, color);  // 每个候选点仍经过矩形裁剪，不直接访问数组
        if (x0 == x1 && y0 == y1)     // 当前写入的点已经是终点
            break;  // 保留终点像素后结束线段
        int64_t twice = error * 2;  // 两轴判断共享同一轮的旧误差
        if (twice >= dy) {  // 误差达到横向前进条件
            error += dy;  // 横向前进累计负纵距
            x0 += sx;     // 当前列向终点移动一格
        }
        if (twice <= dx) {  // 此判断独立，同一轮可以同时沿两轴走
            error += dx;  // 纵向前进累计横距
            y0 += sy;     // 当前行向终点移动一格
        }
    }
}
void u_frame(struct ui_rect r, uint32_t color) {  // 以一像素厚边围住传入矩形
    if (r.w <= 0 || r.h <= 0)  // 空矩形不计算底边和右边
        return;  // 保留当前画布
    u_rect(r.x, r.y, r.w, 1, color);  // 顶边覆盖r.w个像素
    u_rect(r.x, r.y + r.h - 1, r.w, 1, color);  // 底边位于半开底边前一行
    u_rect(r.x, r.y, 1, r.h, color);  // 左边覆盖r.h个像素
    u_rect(r.x + r.w - 1, r.y, 1, r.h, color);  // 右边位于半开右边前一列
}
static void blend(int x, int y, uint32_t rgb, unsigned alpha) {  // 将0至255覆盖率的前景混入原像素
    if (!alpha || !u_inside(clip, x, y))  // 零覆盖率和裁剪外像素不需要读背景
        return;  // 保持原像素不变
    rgb = u_color(rgb);  // 混合前把角色变成真实RGB，混合结果不会重新成为角色
    if (alpha == 255) {  // 完整覆盖可以直接使用既有写入路径
        u_rect(x, y, 1, 1, rgb);  // 写前景而不读取背景
        return;  // 当前像素已经处理完
    }
    unsigned bytes = fb->bpp / 8;  // 与矩形相同，24位用三字节，32位用四字节
    volatile uint8_t *p = (volatile uint8_t *)(uintptr_t)fb->virtual_address +  // 将借用的数值地址还原成字节指针
                          (uint64_t)y * fb->pitch + (uint64_t)x * bytes;  // 按实际行距定位待混合像素
    uint32_t old = 0;  // 从零开始恢复设备布局中的原像素数值
    for (unsigned k = 0; k < bytes; ++k)  // 读取当前像素各字节，不读取行尾填充
        old |= (uint32_t)p[k] << (k * 8);  // 提升到32位后按小端位置组合字节
    const unsigned shifts[] = {fb->red_shift, fb->green_shift, fb->blue_shift};  // 当前设备的红绿蓝最低位
    const unsigned sizes[] = {fb->red_size, fb->green_size, fb->blue_size};  // 与最低位按同下标对应的位宽
    unsigned channels[3];  // 保存混合后的三个八位通道
    for (unsigned k = 0; k < 3; ++k) {  // 每个通道独立混合，避免颜色位之间串入
        unsigned maximum = (1u << sizes[k]) - 1;  // 当前设备通道的最大值，例如五位为31
        unsigned before = ((old >> shifts[k]) & maximum) * 255u / maximum;  // 从设备位域还原成背景八位通道
        unsigned after = (rgb >> ((2 - k) * 8)) & 255u;  // 从0xRRGGBB取得前景八位通道
        channels[k] = (after * alpha + before * (255 - alpha) + 127) / 255;  // 覆盖率加权，加127使结果就近取整
    }
    uint32_t mixed = (channels[0] << 16) | (channels[1] << 8) | channels[2];  // 把三个八位结果组回RGB数值
    uint32_t color = native(mixed);  // 再编码成当前设备真正的位域排列
    for (unsigned k = 0; k < bytes; ++k)  // 将完整混合结果写回同一个像素
        p[k] = (uint8_t)(color >> (k * 8));  // 保持与矩形写入相同的小端字节次序
}
void u_text(int x, int y, const char *s, enum ui_font font, uint32_t color) {  // 将字符串叠到已经铺好的背景
    int origin = x;  // 换行时回到此次文字的起始列
    while (*s) {     // 遇到NUL结束，不读取字符串之后的内存
        unsigned char code = (unsigned char)*s;  // 按0至255读取当前字节，避免有符号char成为负下标
        ++s;  // 当前字节已经取出，指向下一个字符
        if (code == '\n') {  // 换行只改变下一字形的原点
            x = origin;  // 回到本段文字起始列
            y += font == U_CODE ? 24 : 20;  // 代码文字用24像素行距，其余用20像素
            continue;  // 换行不取覆盖率数组
        }
        if (code < 32 || code > 126)  // 固定图集只有可打印ASCII
            code = '?';  // 对未收录的字节使用同一问号字形
        const struct u_glyph *g = &u_glyphs[font][code - 32];  // 二维下标先选字体，再将ASCII32映到第0项
        if ((int64_t)x > (int64_t)clip.x + clip.w) {  // 当前行原点已超过右界，64位计算避免坐标相加溢出
            while (*s && *s != '\n')  // 只跳过本行余下字节，遇NUL或换行停止
                ++s;  // 当前行余下字符仍向右，无需继续读取字形覆盖率
            continue;  // 保留换行给外层下一轮处理，下一行仍回到origin
        }
        for (unsigned row = 0; row < g->h; ++row)  // 按字形局部行遍历包围盒
            for (unsigned col = 0; col < g->w; ++col) {  // 按字形局部列遍历覆盖率
                unsigned alpha = u_coverage[g->offset + row * g->w + col];  // 大数组偏移加局部行列位置
                blend(x + g->x + (int)col, y + g->y + (int)row, color, alpha);  // 原点加字形偏移才是屏幕位置
            }
        x += g->advance;  // 下一字符按推进间距开始，不按包围盒宽度开始
    }
}
int u_text_width(const char *s, enum ui_font font) {  // 返回单行字符串的推进距离，单位为软件像素
    int width = 0;     // 尚未处理任何字形时宽度为零
    for (; *s; ++s) {  // 初始指针已由调用者给出，每轮走到下一字节
        unsigned char code = (unsigned char)*s;  // 与绘制保持相同的无符号编码范围
        if (code < 32 || code > 126)  // 宽度计算也只能访问已收录字形
            code = '?';  // 替代规则与绘制一致，避免布局和外观不符
        width += u_glyphs[font][code - 32].advance;  // 累加原点之间的距离，空格也有advance
    }
    return width;  // 调用者用总推进距离进行居中或对齐
}
void u_round(struct ui_rect r, int radius, uint32_t color) {  // 中间填实色，四角单独计算像素覆盖率
    if (r.w <= 0 || r.h <= 0)  // 空矩形没有任何像素需要写
        return;  // 不计算圆心和角落地址
    if (radius > r.w / 2)  // 左右圆角不能相互越过
        radius = r.w / 2;  // 将半径限制到宽度的一半
    if (radius > r.h / 2)  // 上下圆角也不能相互越过
        radius = r.h / 2;  // 再限制到高度的一半
    if (radius <= 0) {  // 零或负半径使用直角矩形
        u_rect(r.x, r.y, r.w, r.h, color);  // 保留同样的输入范围与裁剪行为
        return;  // 直角路径已经写完，不计算采样
    }
    u_rect(r.x, r.y + radius, r.w, r.h - 2 * radius, color);  // 中部完整横条无须读背景
    for (int y = 0; y < radius; ++y) {  // 只遍历上方角落行，下方按镜像复用
        u_rect(r.x + radius, r.y + y, r.w - 2 * radius, 1, color);  // 上方两圆角之间的实心段
        u_rect(r.x + radius, r.y + r.h - 1 - y, r.w - 2 * radius, 1, color);  // 下方对应实心段
        for (int x = 0; x < radius; ++x) {  // 只在左上角的小范围计算覆盖率
            unsigned count = 0;  // 当前像素内部落在圆内的样本数量，范围0至16
            for (int sy = 0; sy < 4; ++sy)  // 在像素内按四行取样
                for (int sx = 0; sx < 4; ++sx) {  // 每行再取四个样本，共16个
                    int64_t dx = 8 * ((int64_t)radius - x) - 2 * sx - 1;  // 样本中心按八倍坐标保存，避免浮点小数
                    int64_t dy = 8 * ((int64_t)radius - y) - 2 * sy - 1;  // 同样取得到圆心的纵向距离
                    if (dx * dx + dy * dy <= 64 * (int64_t)radius * radius)  // 距离平方不超过半径平方的样本在圆内
                        ++count;  // 该样本贡献1/16像素覆盖
                }
            unsigned alpha = (count * 255 + 8) / 16;  // 将0至16计数转换成0至255覆盖率并就近取整
            blend(r.x + x, r.y + y, color, alpha);    // 左上像素混合进既有背景
            blend(r.x + r.w - 1 - x, r.y + y, color, alpha);  // 同一个覆盖率用于右上镜像
            blend(r.x + x, r.y + r.h - 1 - y, color, alpha);  // 同一个覆盖率用于左下镜像
            blend(r.x + r.w - 1 - x, r.y + r.h - 1 - y, color, alpha);  // 同一个覆盖率用于右下镜像
        }
    }
}
void u_control(struct ui_rect r, const char *label, unsigned state) {  // 一个状态值决定当前按钮的像素外观
    bool disabled = (state & U_INACTIVE) != 0;  // 按位与只取禁用位，非零转换为true
    uint32_t fill = U_PANEL;  // 没有其他状态时使用普通按钮底面
    if (state & U_DOWN)  // 按下的优先级最高
        fill = U_PRESSED;  // 按下底面覆盖悬停与选中底面
    else if (state & U_HOT)  // 未按下而处于悬停时使用悬停底面
        fill = U_HOVER;  // 当前状态仍可同时带焦点位
    else if (state & U_SELECTED)  // 未按下或悬停时才使用选中实面
        fill = U_ACCENT;  // 选中主动作使用主题粉色
    uint32_t edge = state & U_SELECTED ? U_ACCENT : U_CONTROL;  // 选中与普通可操作边界分别取色
    if (disabled) {  // 服务不可用时覆盖交互强调
        fill = U_PANEL;  // 禁用仍有可辨认的底面
        edge = U_EDGE;   // 底边改为轻分隔色
    }
    u_round(r, UI_CONTROL_RADIUS, fill);    // 两像素圆角保持当前紧凑几何
    if (!(state & U_SELECTED) || disabled)  // 普通或禁用按钮使用底边提示可操作区域
        u_rect(r.x + 4, r.y + r.h - 1, r.w - 8, 1, edge);  // 留出左右四像素的细底边
    if ((state & U_SELECTED) && !disabled)  // 启用的选中按钮额外保留内部粗底线
        u_rect(r.x + 8, r.y + r.h - 3, r.w - 16, 2, U_ACCENT);  // 在悬停或按下底面中保留选中的粉色标记
    if ((state & U_FOCUSED) && !disabled)  // 焦点与选中可组合，但禁用不显示焦点
        u_frame((struct ui_rect){r.x - 2, r.y - 2, r.w + 4, r.h + 4}, U_BLUE);  // 外围蓝框不改变按钮内容宽高
    uint32_t text = U_TEXT;  // 普通、悬停和按下使用正文前景
    if (disabled)  // 禁用文字采用独立角色
        text = U_DISABLED;  // 角色在日夜主题中分别保留可读性
    else if ((state & U_SELECTED) && !(state & (U_DOWN | U_HOT)))  // 仅选中实面使用其专用前景
        text = U_ON_ACCENT;  // 避免把该前景用于悬停或按下底面
    int x = r.x + (r.w - u_text_width(label, U_MEDIUM)) / 2;  // 用14px中等字重的推进量水平居中
    int y = r.y + (r.h - 18) / 2;  // 18像素是按钮文字占位高度，不是字体字号
    if ((state & U_DOWN) && !disabled)  // 可操作按钮按下时出现一像素位移
        ++y;  // 只移动文字，按钮与命中矩形保持不变
    u_text(x, y, label, U_MEDIUM, text);  // 背景已经形成，再按覆盖率混入居中文字
}
void u_button(struct ui_rect r, const char *label, bool selected, bool disabled) {  // 供只持有选择与可用性数据的调用者使用
    unsigned state = selected ? U_SELECTED : U_NORMAL;  // 当前应用选择转换成第0位
    if (disabled)  // 调用者明确报告此动作不可用
        state |= U_INACTIVE;  // 按位或加入禁用位，保留已有选择位
    if (interactive && u_inside(r, pointer_x, pointer_y)) {  // 只有允许响应且指针在按钮半开矩形内时加入反馈
        state |= U_HOT;    // 按位或加入悬停位，不改变已有选择或禁用位
        if (pointer_down)  // 指针在此按钮内且鼠标仍按着
            state |= U_DOWN;  // 本帧加入按压反馈，服务动作仍由输入分派执行
    }
    u_control(r, label, state);  // 只把组合状态转换成像素，不调用任何应用或服务
}

int u_label_width(const char *s) {  // 身份标签使用Medium推进量，每个字符另加两像素，测量与绘图一致。
    int width = u_text_width(s, U_MEDIUM);  // 取得这条单行标签的全部字形推进量。
    for (const char *p = s; *p; ++p)  // 同u_label一样逐字节处理，ASCII标签每项留两像素字距。
        width += 2;  // 包括最后字形后的留白，使承载身份片有足够范围。
    return width;  // 返回软件画布中的像素距离，不是字符串字节数。
}

void u_label(int x, int y, const char *s, uint32_t color) {  // 标签按逐字推进加2像素字距，限于短分区名
    for (; *s; ++s) {  // 正常文件名与正文不经过这个函数
        char ch[2] = {*s, 0};  // 单字符形成合法NUL字符串复用字形绘制
        u_text(x, y, ch, U_MEDIUM, color);    // 使用已生成的14像素、字重500字体
        x += u_text_width(ch, U_MEDIUM) + 2;  // 字宽加固定空隙构成稳定标签节奏
    }
}
void u_plate(struct ui_rect r, uint32_t color) {  // 斜切片保持矩形左缘，右端随行逐步收缩
    for (int y = 0; y < r.h; ++y)  // 每行仍由原矩形图元执行裁剪
        u_rect(r.x, r.y + y, r.w - y / 3, 1, color);  // 约18度右缘使标题身份片区别于内容面板
}
void u_brand(int x, int y) {  // 两条斜边和悬浮横梁组成原创A形标识
    for (int i = 0; i < 3; ++i) {  // 三条相邻线提供系统标识所需的笔画重量
        u_line(x + i, y + 20, x + 10 + i, y, U_INK);  // 左侧上升边
        u_line(x + 10 + i, y, x + 20 + i, y + 20, U_INK);  // 右侧下降边
    }
    u_rect(x + 7, y + 13, 10, 3, U_PINK);  // 粉色横梁只承担产品身份
}

void u_interaction(int x, int y, bool down, bool enabled) {  // 每个绘制区域显式设置它可以响应的指针状态
    pointer_x = x;  // 下一次按钮绘制用这个整屏横坐标命中
    pointer_y = y;  // 纵坐标与按钮矩形采用同一整屏坐标系
    pointer_down = down;    // 当前按压由事件入口保存，绘图不产生按下事件
    interactive = enabled;  // 底层窗口或被对话框遮挡的区域关闭反馈
}
```

控件已经会读取许可，还需要由桌面逐区提供。开始绘桌面时允许全局控件反馈，进入普通窗口时按 active 和覆盖层收窄许可，资源侧栏与对话框再分别设置自己的许可。这样最后一个窗口的局部状态不会被资源按钮沿用。

**完整写入 `frontend/core/paint.c`：**

<a id="code-frontend-core-paint-c-1"></a>

<!-- write: frontend/core/paint.c -->
```c
#include "internal.h"  // 引入当前目录内部共享接口和基础函数
static const char *titles[] = {"Terminal", "Files",    "Editor",     "Tasks",  // 窗口标题按应用枚举顺序排，应用编号可直接索引
                               "System",   "Settings", "Data viewer"};  // 继续应用枚举4到6的窗口标题，数组顺序与应用id一致
static const char *icons[] = {"Terminal", "Files",    "Editor", "Tasks",  // 侧栏悬停名称按相同应用顺序排
                              "System",   "Settings", "Viewer"};  // 继续应用枚举4到6的侧栏名称，同一编号显示同一应用
static void monitor(struct ui_desktop *s) {  // 共享资源仪表，全部数值来自同一次backend快照
    int x = s->width - UI_MONITOR_W, left = x + 24, width = UI_MONITOR_W - 48;  // 单一内边距对齐标题、数值与坐标轴
    char value[96];  // 临时格式化数值，不持有另一个统计状态
    u_rect(x, UI_TOP, UI_MONITOR_W, s->height - UI_TOP - UI_BOTTOM, U_BG);  // 仪表与应用正文分离
    u_rect(x, UI_TOP, 1, s->height - UI_TOP - UI_BOTTOM, U_EDGE);  // 系统侧栏边界
    u_label(left, 60, "SYSTEM / RESOURCES", U_MUTED);  // 与窗口身份统一的短标签
    u_text(left, 88, "Resource monitor", U_LABEL, U_TEXT);  // 可理解的区域标题
    u_rect(left, 123, width, 1, U_CONTROL);  // 模块的共同基准线
    u_text(left, 138, s->backend.mode == UI_REFERENCE ? "Recorded session" : "Current session", U_SMALL, U_MUTED);  // 明确实际数据来源
    if (s->stats.available) {  // 只有快照存在才绘制数值和曲线
        u_label(left, 174, "CPU LOAD", U_BLUE);  // 蓝色对应CPU测量
        airi_snprintf(value, sizeof(value), "%u", s->stats.cpu);  // 读取已有百分比
        u_text(left, 191, value, U_DISPLAY, U_TEXT);   // 大字号突出当前值，图表解释历史
        u_text(left + 58, 212, "%", U_BODY, U_MUTED);  // 单位与数值分层
        u_text(left + width - 54, 209, "60 sec", U_SMALL, U_MUTED);  // 横轴的采样跨度
        int top = 255, bottom = 355, graphw = width - 28;  // 纵轴固定0..100%，不根据当前曲线自动放大
        for (int i = 0; i <= 4; ++i) {  // 五条刻度线对应0、25、50、75、100
            int y = top + i * 25;  // 每百分比一个像素
            for (int gx = 0; gx < graphw; gx += 8)  // 每8像素取一个CPU网格短线起点
                u_rect(left + gx, y, 2, 1, U_EDGE);  // 每个起点写2×1像素，形成对应真实刻度的点线
            u_rect(left - 4, y, 4, 1, U_CONTROL);  // 轴端刻度确定实际读数位置
        }
        u_text(left + graphw + 6, top - 6, "100", U_SMALL, U_MUTED);   // 上界百分比
        u_text(left + graphw + 6, top + 44, "50", U_SMALL, U_MUTED);   // 中线百分比
        u_text(left + graphw + 6, bottom - 6, "0", U_SMALL, U_MUTED);  // 下界百分比
        for (unsigned i = 1; i < 60; ++i) {  // 用backend已有60个历史点连接折线
            unsigned prev = s->stats.samples[i - 1], now = s->stats.samples[i];  // 借用同一统计快照
            if (prev > 100)  // 前一个采样超过百分数上界时限制高度
                prev = 100;  // 顶点最多落在100%刻度处
            if (now > 100)  // 当前采样也按相同上界限制
                now = 100;  // 当前顶点最多落在图表顶部
            int px = left + (int)(i - 1) * graphw / 59, nx = left + (int)i * graphw / 59;  // 时间均匀映射到横轴
            u_line(px, bottom - (int)prev, nx, bottom - (int)now, U_BLUE);  // 蓝色走势与CPU标签保持一致
        }
        u_rect(left, bottom + 1, graphw, 1, U_CONTROL);  // 明确时间轴
        u_text(left, bottom + 10, "-60s", U_SMALL, U_MUTED);  // 过去的时间端点
        u_text(left + graphw - 24, bottom + 10, "now", U_SMALL, U_MUTED);  // 当前时间端点
        u_rect(left, 401, width, 1, U_EDGE);     // CPU与内存使用同一列宽
        u_label(left, 420, "MEMORY", U_ACCENT);  // 内存沿用粉色类别
        airi_snprintf(value, sizeof(value), "%u", s->stats.memory_kib / 1024);  // 实际KiB换算MiB
        u_text(left, 437, value, U_DISPLAY, U_TEXT);  // 已使用量为主读数
        airi_snprintf(value, sizeof(value), "/ %u MiB", s->stats.total_kib / 1024);  // 容量保留明确单位
        u_text(left + 75, 458, value, U_BODY, U_MUTED);  // 与CPU单位共享同一文字基准
        u_rect(left, 493, width, 12, U_RAISED);  // 连续容量条不虚构内存分段
        if (s->stats.total_kib) {  // 无总量时避免除零
            uint64_t used = s->stats.memory_kib < s->stats.total_kib ? s->stats.memory_kib : s->stats.total_kib;  // 限制异常比例
            u_rect(left, 493, (int)(used * width / s->stats.total_kib), 12, U_ACCENT);  // 宽度线性表达已使用量
        }
        u_rect(left, 537, width, 1, U_EDGE);  // I/O区沿相同栅格排列
        u_label(left, 556, "I/O THROUGHPUT", U_MUTED);  // 速率与容量明确分区
        airi_snprintf(value, sizeof(value), "%u", s->stats.read_kib);  // 读取当前读速率
        u_text(left, 582, value, U_TITLE, U_BLUE);  // 读操作的颜色与标记一致
        airi_snprintf(value, sizeof(value), "%u", s->stats.write_kib);  // 读取当前写速率
        u_text(left + 116, 582, value, U_TITLE, U_PINK);      // 写操作用独立图表类别色
        u_text(left, 615, "Read / KiB/s", U_SMALL, U_MUTED);  // 标明读操作和单位
        u_text(left + 116, 615, "Write / KiB/s", U_SMALL, U_MUTED);  // 标明写操作和单位
    } else {  // 未采集时保留明确空状态
        u_text(left, 180, "No runtime samples", U_LABEL, U_TEXT);  // 不用装饰图形冒充测量
        u_text(left, 212, "CPU and I/O collection is offline.", U_SMALL, U_MUTED);  // 指出具体缺失能力
    }
    u_button((struct ui_rect){left, s->height - 96, width, 32}, "Open task manager", false, false);  // 与既有点击区域一致
}
void ui_draw(struct ui_desktop *s, const struct airi_framebuffer *f) {  // 按窗口层次画整帧，并将对话框留在最后
    if (!airi_framebuffer_valid(f))  // 画布地址、尺寸或通道无效时，在写像素之前结束
        return;  // 当前按窗口层次画整帧，并将对话框留在最后在本分支结束，后续语句不执行
    s->width = (int)f->width;    // 当前帧画布宽度按像素保存，绘图已检查帧缓冲能用且宽度可转int
    s->height = (int)f->height;  // 当前帧画布高度按像素保存，窗口收窄和坐标布局使用这个尺寸
    u_begin(f);  // 借用本次画布描述，建立颜色布局与整屏裁剪
    u_theme(s->theme);  // 按会话模式选整套调色板，颜色角色与业务状态分离
    u_rect(0, 0, s->width, s->height, U_BG);  // 用画布背景色写矩形，起点(0,0)，尺寸s->width×s->height像素
    u_interaction(s->mouse_x, s->mouse_y, s->pointer_down, !s->dialog);  // 桌面区的指针反馈
    u_text(86, s->height - 117, "AiriOS", U_TITLE, U_MUTED);  // 空桌面仅保留轻量产品标识
    u_rect(0, 0, s->width, UI_TOP, U_PANEL);  // 系统顶栏先铺满36像素高度，身份片随后覆盖左端
    u_plate((struct ui_rect){0, 0, 140, UI_TOP}, U_CHROME);  // 系统入口使用唯一品牌身份片
    u_rect(0, 35, s->width, 1, U_EDGE);  // 用分隔线色写矩形，起点(0,35)，尺寸s->width×1像素
    u_brand(16, 7);  // 系统入口的原创品牌符号
    u_label(52, 8, "AIRI", U_INK);  // 在整屏(52,8)写系统品牌文字，与左侧A形图标共同构成身份区
    u_label(156, 9, "WORKSPACE", U_MUTED);  // 在整屏(156,9)写WORKSPACE，位于140像素身份片右侧
    u_text(288, 9, s->active < 0 ? "Desktop" : titles[s->active], U_MEDIUM, U_TEXT);  // 在整屏(288,9)写当前活动应用名；无活动窗口时显示Desktop
    u_text(s->width - 263, 10,  // 顶栏右侧说明当前画面来自主机参考还是内核控制台
           s->backend.mode == UI_REFERENCE ? "Local session" : "Local console",  // 顶栏用实际后端模式标明主机参考工作区或guest内核控制台
           U_SMALL, U_MUTED);  // 系统状态保持次要前景
    u_icon(s->width - 37, 8, UI_TASKS_APP, U_MUTED);      // 顶栏右侧画资源入口图标，是否展开由桌面monitor状态决定
    u_rect(0, UI_TOP, UI_DOCK, s->height - 66, U_PANEL);  // 用工具栏底色写矩形，起点(0,36)，尺寸56×s->height - 66像素
    u_rect(55, 36, 1, s->height - 66, U_EDGE);     // 用分隔线色写矩形，起点(55,36)，尺寸1×s->height - 66像素
    for (unsigned i = 0; i < UI_APP_COUNT; ++i) {  // 逐个检查7个有效应用编号
        int y = 58 + (int)i * 54;   // 侧栏第i个应用图标从第58行起每54像素排列
        if (s->active == (int)i) {  // 当前应用是活动窗口时给侧栏对应项铺选中背景
            u_round((struct ui_rect){7, y - 8, 42, 38}, 2, U_RAISED);  // 用选中背景色写矩形，起点(7,y - 8)，尺寸42×38像素
            u_round((struct ui_rect){2, y, 3, 22}, 1, U_ACCENT);  // 活动应用旁写三像素宽、22像素高的强调色竖标，起点(2,y)
        }
        u_icon(18, y, (int)i, s->active == (int)i ? U_TEXT : U_MUTED);  // 按应用编号画左侧图标，活动项使用主要色
        if (s->windows[i].open)  // 已打开应用即使最小化也保留侧栏打开标记
            u_rect(25, y + 25, 6, 2, U_ACCENT);  // 用会话强调色写矩形，起点(25,y + 25)，尺寸6×2像素
        if (u_inside((struct ui_rect){0, y - 8, 56, 38}, s->mouse_x, s->mouse_y)) {  // 鼠标落在当前应用侧栏行的半开矩形时显示其名称提示
            u_rect(60, y, 100, 28, U_RAISED);  // 用选中背景色写矩形，起点(60,y)，尺寸100×28像素
            u_text(70, y + 5, icons[i], U_BODY, U_TEXT);  // 在整屏列70、整屏y=y + 5显示当前应用悬停名称
        }
    }
    int right = s->monitor && s->width >= UI_MONITOR_MIN ? 268 : 0;  // 资源栏开启且屏幕够宽才预留268像素，否则窗口可用至右边
    unsigned first = 0;  // 默认从最底层窗口开始绘制，后面找可遮挡底层的最大化窗口
    for (unsigned n = 0; n < s->order_count; ++n) {  // 按有效窗口层次遍历，数组后部不作窗口身份
        struct ui_window *w = &s->windows[s->order[n]];  // 先由绘图次序取得应用编号，再借用对应窗口，不把层次n当应用id
        if (w->open && !w->minimized && w->maximized)    // 只选已打开、可见且最大化的窗口作为底层跳过起点
            first = n;  // 记录目前找到的最高最大化窗口层次，它下方窗口无需输出被覆盖像素
    }
    for (unsigned n = first; n < s->order_count; ++n) {  // 按有效窗口层次遍历，数组后部不作窗口身份
        unsigned app = s->order[n];  // 当前层次n保存的是应用id，后续按该id取窗口和标题
        struct ui_window *w = &s->windows[app];  // 借用当前应用的窗口状态，app已经通过范围检查
        if (!w->open || w->minimized)  // 关闭或最小化窗口不画正文，但其其他会话状态仍保留
            continue;  // 本项不参与当前结果，跳过本轮剩余操作进入下一项
        if (w->maximized)  // 最大化窗口按当前画布扣系统栏和资源栏重新计算矩形
            w->rect = (struct ui_rect){68, 48, s->width - 80 - right, s->height - 90};  // 最大化窗口从侧栏右边和顶栏下开始，扣右资源栏及上下固定栏得到可用尺寸
        if (w->rect.w > s->width - 80 - right)  // 普通窗口宽度不能占到左侧系统栏和右侧资源栏
            w->rect.w = s->width - 80 - right;  // 把过宽窗口收窄到侧栏与资源栏之间，避免正文侵入系统栏
        if (w->rect.h > s->height - 88)  // 窗口高度超过上下保留区时缩短
            w->rect.h = s->height - 88;  // 把过高窗口收窄到整屏高度扣上下栏，保留任务栏空间
        if (w->rect.x + w->rect.w > s->width - right - 10)  // 窗口右边侵入资源栏或右边距时左移回可用区
            w->rect.x = s->width - right - w->rect.w - 10;  // 右边越界时重新算左缘，使窗口右边落在资源栏左侧10像素处
        if (w->rect.y + w->rect.h > s->height - 40)  // 窗口底边侵入任务栏时上移回可用区
            w->rect.y = s->height - w->rect.h - 40;  // 底边越界时重新算顶缘，使窗口底边停在屏幕底40像素之上
        if (w->rect.x < 68)  // 窗口左缘越过侧栏后的最小位置时向右收回
            w->rect.x = 68;  // 左缘最小保留68像素，确保窗口不会盖住56像素侧栏
        if (w->rect.y < 46)  // 窗口顶缘越过顶栏后的最小位置时向下收回
            w->rect.y = 46;  // 顶缘最小46像素，确保标题栏位于系统顶栏之下
        struct ui_rect r = w->rect;  // 复制已收窄的当前窗口矩形，后面从它计算边框、标题与body
        u_reset_clip();  // 恢复整个画布的可写范围，后续控件不继承上一次局部裁剪
        bool focused = s->active == (int)app;  // 标题和局部边界共同指出当前窗口
        u_round((struct ui_rect){r.x + 3, r.y + 5, r.w, r.h}, UI_WINDOW_RADIUS + 1, U_SHADOW);  // 有限偏移阴影
        u_round(r, UI_WINDOW_RADIUS, focused ? U_CONTROL : U_EDGE);  // 窗口外轮廓
        u_round((struct ui_rect){r.x + 1, r.y + 1, r.w - 2, r.h - 2}, UI_WINDOW_RADIUS - 1, U_PANEL);  // 标题与框架表面
        u_rect(r.x + 1, r.y + UI_TITLE_H + 1, r.w - 2, r.h - UI_TITLE_H - 6, U_SURFACE);  // 正文保留底部圆角
        u_rect(r.x + 1, r.y + 1, r.w - 2, 1, focused ? U_ACCENT : U_CONTROL);  // 连续细轨统一窗口基线
        static const char *heads[] = {"TERMINAL", "FILES", "EDITOR", "TASKS", "SYSTEM", "SETTINGS", "DATA VIEWER"};  // 窗口身份标签独立于文件名
        int plate_width = 54 + u_label_width(heads[app]) + 12;  // 标签左缘距片左缘54像素，斜切右端再留12像素。
        if (plate_width < 174)  // 短标签保留当前身份片的统一最小宽度。
            plate_width = 174;  // 长标签才扩大片面，普通窗口外观保持相同度量。
        int available = r.w - 116;    // 片从外框左8像素开始，右界留在系统按钮前108像素。
        if (plate_width > available)  // 窄窗口的标题可用范围优先于理想字宽。
            plate_width = available;  // 不把身份片铺进最小化、最大化和关闭按钮区。
        u_plate((struct ui_rect){r.x + 8, r.y + 7, plate_width, UI_TITLE_H - 14}, U_CHROME);  // 用实际字体推进量承载完整应用身份。
        u_interaction(s->mouse_x, s->mouse_y, s->pointer_down, focused && !s->dialog && !s->launcher);  // 只有最前窗口可响应悬停
        u_icon(r.x + 18, r.y + 14, (int)app, U_INK);   // 应用图标处于独立身份栏
        u_rect(r.x + 53, r.y + 12, 1, 24, U_CONTROL);  // 图标与标题使用一致的分隔尺度
        u_clip((struct ui_rect){r.x + 62, r.y + 1, r.w - 170, UI_TITLE_H - 2});  // 长文件名不会覆盖系统按钮
        u_label(r.x + 62, r.y + 14, heads[app], U_INK);  // 身份标签使用14像素Medium字形和额外字距，与普通正文区别
        if (app == UI_EDITOR && r.w > 500) {  // 宽编辑器在标题后保留当前对象
            u_text(r.x + 196, r.y + 14, "/", U_BODY, U_MUTED);  // 应用与文档层次分明
            u_text(r.x + 220, r.y + 14, s->editor_name, U_MEDIUM, U_TEXT);  // 文件名保持正常大小写
        }
        if (app == UI_TERMINAL && r.w > 600)  // 连接身份位于标题里，避免占用一整行工具栏
            u_text(r.x + 200, r.y + 15, s->backend.mode == UI_REFERENCE ? "/  Local console" : (s->backend.label ? s->backend.label : "Kernel console"), U_BODY, U_MUTED);  // 只报告实际接入类型
        u_reset_clip();  // 窗口操作区独立于标题裁剪
        u_line(r.x + r.w - 88, r.y + 28, r.x + r.w - 78, r.y + 28, U_MUTED);  // 最小化
        u_frame((struct ui_rect){r.x + r.w - 60, r.y + 19, 10, 10}, U_MUTED);  // 最大化
        u_line(r.x + r.w - 30, r.y + 19, r.x + r.w - 20, r.y + 29, U_MUTED);  // 关闭的第一条对角线
        u_line(r.x + r.w - 20, r.y + 19, r.x + r.w - 30, r.y + 29, U_MUTED);  // 关闭的第二条对角线
        struct ui_rect body = u_window_body(r);  // 左右边框各1像素、标题48像素及顶边1像素，body从外框顶下49像素开始
        u_rect(body.x, body.y, body.w, body.h - 5, U_SURFACE);  // 内容底色明确分离标题
        u_clip(body);  // 应用绘图仅写扣除窗口标题和边框的body，避免覆盖框外像素
        ui_apps_draw(s, (enum ui_app)app, body);  // 按应用编号选择已接入的内容绘图函数，本次实参取自当前桌面状态
        u_reset_clip();  // 恢复整个画布的可写范围，后续控件不继承上一次局部裁剪
        u_line(r.x + r.w - 10, r.y + r.h - 4, r.x + r.w - 4, r.y + r.h - 10, U_EDGE);  // 用分隔线色连接像素端点(r.x + r.w - 10,r.y + r.h - 4)到(r.x + r.w - 4,r.y + r.h - 10)
    }
    if (right) {  // 资源区是桌面的一部分，不继承刚绘完窗口的hover许可。
        u_interaction(s->mouse_x, s->mouse_y, s->pointer_down, !s->dialog);  // 无模态框时资源按钮独立允许响应，active可为-1。
        monitor(s);  // 按同次资源快照绘图，按钮随后使用这份桌面许可。
    }
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
    u_rect(0, s->height - UI_BOTTOM, s->width, 1, U_EDGE);  // 用分隔线色写矩形，起点(0,s->height - UI_BOTTOM)，尺寸s->width×1像素
    u_text(14, s->height - 23,  // 底栏明确写当前帧来自参考内存还是实际内核启动数据
           s->backend.mode == UI_REFERENCE ? "Session storage"  // 底栏明确标明本进程内存参考工作区，和真实guest启动数据区分
                                           : "AiriOS / x86-64",  // 底栏将当前真实启动数据来源写明
           U_SMALL, U_MUTED);  // 底栏保持中性次文字
    int tx = 330;  // 任务栏的首个已打开应用标题从整屏第330列开始
    for (unsigned i = 0; i < UI_APP_COUNT; ++i)  // 逐个检查7个有效应用编号
        if (s->windows[i].open) {  // 仅已打开的应用进入底栏标题列表
            u_text(tx, s->height - 23, titles[i], U_SMALL, s->active == (int)i ? U_ACCENT : U_MUTED);  // 在整屏x=tx、整屏y=s->height - 23显示titles[i]
            tx += 88;  // 任务栏下一应用标题右移88像素
        }
    if (s->toast[0]) {  // 短提示非空时在普通窗口之后覆盖绘制
        u_rect(s->width - 382, s->height - 78, 360, 34, U_RAISED);  // 用选中背景色写矩形，起点(s->width - 382,s->height - 78)，尺寸360×34像素
        u_text(s->width - 368, s->height - 70, s->toast, U_SMALL, U_TEXT);  // 在整屏x=s->width - 368、整屏y=s->height - 70显示本次操作短提示
    }
    if (s->launcher) {  // 启动菜单打开时，最后绘制应用选择列表
        u_rect(8, 43, 244, UI_APP_COUNT * 42 + 48, U_PANEL);  // 用工具栏底色写矩形，起点(8,43)，尺寸244×UI_APP_COUNT * 42 + 48像素
        u_frame((struct ui_rect){8, 43, 244, UI_APP_COUNT * 42 + 48}, U_EDGE);  // 沿当前显式矩形的四边画一像素边框
        u_text(24, 54, "Applications", U_LABEL, U_TEXT);  // 在整屏列24、整屏行54显示文字"Applications"
        for (int i = 0; i < UI_APP_COUNT; ++i) {  // 逐个检查7个有效应用编号
            u_icon(25, 90 + i * 42, i, U_MUTED);  // 启动菜单按每行42像素绘制第i个应用身份图标
            u_text(59, 90 + i * 42, titles[i], U_BODY, U_TEXT);  // 在整屏列59、整屏y=90 + i * 42显示titles[i]
        }
    }

    if (s->dialog) {  // 菜单和普通窗口已经绘完，再写对话框像素，使它覆盖下层
        struct ui_rect d = {s->width / 2 - 240, s->height / 2 - 112, 480, 224};  // 480×224对话框以整屏中心为中心，左上扣除半宽半高
        u_round((struct ui_rect){d.x + 4, d.y + 6, d.w, d.h}, 7, U_SHADOW);  // 在对话框右移4、下移6像素的位置画7像素半径阴影
        u_round(d, 2, U_CONTROL);  // 弹窗使用独立清晰边界
        u_round((struct ui_rect){d.x + 1, d.y + 1, d.w - 2, d.h - 2}, 1, U_SURFACE);  // 中性内容表面
        if (s->dialog == UI_DIALOG_ERROR) u_rect(d.x + 1, d.y + 1, 3, d.h - 2, U_RED);  // 错误沿左缘标明，正文仍完整可读
        u_interaction(s->mouse_x, s->mouse_y, s->pointer_down, true);  // 弹窗是唯一可操作层
        u_rect(d.x + 24, d.y + 49, d.w - 48, 1, U_EDGE);  // 标题与说明之间轻分隔
        u_text(d.x + 24, d.y + 20, s->dialog_title, U_LABEL, s->dialog == UI_DIALOG_ERROR ? U_RED : U_TEXT);  // 在整屏x=d.x + 24、整屏y=d.y + 20显示对话框标题
        /* Wrap body on words without breaking paths outside their containing dialog. */
        char row[64];  // 对话框单显示行最多63内容字节，再留一格NUL
        unsigned used = 0;  // 当前对话框显示片段的内容长度从0字节开始
        int y = d.y + 58;   // 对话框正文第一行在框顶下移58像素
        const char *p = s->dialog_text;  // 借用对话框正文首字节，分词换行不修改原说明
        while (*p && y < d.y + 135) {    // 正文仍有字节且下一行起点未到框内135像素位置时继续绘制
            while (*p == ' ')  // 新显示行开始时跳过连续空格，避免换行后的行首保留分词空白
                ++p;  // 消费当前文字字节后指针向后推进一字节
            const char *start = p, *last_space = NULL;  // 记录本显示行起点；NULL表示尚未找到可回退的单词边界
            while (*p && *p != '\n' && (size_t)(p - start) < sizeof(row) - 1) {  // 扫描到NUL、显式换行或63字节容量便停止；每次写候选行后还要检查像素宽度
                row[p - start] = *p;     // 用当前扫描偏移把一个对话框字节放入局部候选行
                row[p - start + 1] = 0;  // 候选行每新增一个字节就补NUL，才能立即测像素宽度
                if (u_text_width(row, U_BODY) > 432)  // 候选行超过对话框正文432像素时准备换行
                    break;  // 候选行已超432像素，退出这层扫描，后面选择可回退的单词边界
                if (*p == ' ')  // 记住当前空格，超过宽度时可回退到单词边界
                    last_space = p;  // 记住当前空格位置，超宽时可回到这里结束本显示行
                ++p;  // 消费当前文字字节后指针向后推进一字节
            }
            if (*p && *p != '\n' && last_space && last_space > start)  // 因像素宽或字节容量停住且仍有本行余文时，若记录过非行首空格便回退到它
                p = last_space;  // 把下一行扫描起点回退到最后空格，后续先跳空格继续
            used = (unsigned)(p - start);  // 两个同数组指针之差给出本次需要复制的字节数
            if (!used && *p) {  // 长词首字节就超宽时至少推进一个字节，避免无限停在原地
                ++p;  // 消费当前文字字节后指针向后推进一字节
                used = 1;  // 过宽长词至少显示并消费一字节，保证下一轮推进
            }
            airi_memcpy(row, start, used);  // 复制当前显示片段[start,p)的used个字节到局部row，下一行补NUL
            row[used] = 0;  // 给本次选定显示片段补NUL，再交文字绘图
            u_text(d.x + 24, y, row, U_BODY, U_TEXT);  // 在整屏x=d.x + 24、整屏y=y显示本次拆出的显示文字行
            if (*p == '\n')  // 原文显式换行也应消费，不成为下一行首内容
                ++p;  // 消费当前文字字节后指针向后推进一字节
            y += 22;  // 对话框下一行向下移动22像素
        }
        if (s->dialog == UI_DIALOG_NEW_FILE || s->dialog == UI_DIALOG_NEW_DIR || s->dialog == UI_DIALOG_RENAME || s->dialog == UI_DIALOG_SAVE_AS) {  // 创建、重命名和另存需要名称字段，其他确认框不画输入
            u_rect(d.x + 24, d.y + 118, 432, 34, U_SURFACE);  // 用窗口正文底色写矩形，起点(d.x + 24,d.y + 118)，尺寸432×34像素
            u_frame((struct ui_rect){d.x + 24, d.y + 118, 432, 34}, U_BLUE);  // 沿当前显式矩形的四边画一像素边框
            u_clip((struct ui_rect){d.x + 34, d.y + 119, 412, 32});  // 把后续文字或像素限制在本次显式矩形内，长内容在边界停止写入
            u_text(d.x + 34, d.y + 126, s->dialog_input, U_BODY, U_TEXT);  // 在整屏x=d.x + 34、整屏y=d.y + 126显示对话框输入字段
            u_reset_clip();  // 恢复整个画布的可写范围，后续控件不继承上一次局部裁剪
        }
        u_button((struct ui_rect){d.x + d.w - 204, d.y + 174, 84, 30},  // 对话框右侧先画取消或关闭按钮，接下来的种类选择文字
                 (s->dialog == UI_DIALOG_ERROR || s->dialog == UI_DIALOG_INFO) ? "Close" : "Cancel", false, false);  // 错误框只关闭提示，其余框此按钮取消待确认操作
        if (s->dialog == UI_DIALOG_CLOSE) {  // 未保存关闭对话框额外提供取消与丢弃两条明确操作
            u_button((struct ui_rect){d.x + 180, d.y + 174, 84, 30}, "Cancel", false, false);  // 画文字"Cancel"按钮；其矩形供后续事件命中共用，当前仅输出像素
            u_button((struct ui_rect){d.x + 276, d.y + 174, 84, 30}, "Discard", false, false);  // 画文字"Discard"按钮；其矩形供后续事件命中共用，当前仅输出像素
        }
        if (s->dialog != UI_DIALOG_ERROR && s->dialog != UI_DIALOG_INFO)  // 纯错误框只有关闭按钮，其余种类再画确认动作
            u_button((struct ui_rect){d.x + d.w - 108, d.y + 174, 84, 30},  // 在对话框右端画当前种类的确认按钮，事件接入后才执行动作
                     s->dialog == UI_DIALOG_CLOSE   ? "Save"     // 未保存关闭确认的主按钮先保存，成功回调之后才关闭
                     : s->dialog == UI_DIALOG_DELETE ? "Delete"  // 删除确认框主按钮明确使用Delete文字，避免与保存混淆
                     : s->dialog == UI_DIALOG_STOP ? "End task"  // 停止参考任务确认框主按钮明确显示End task
                                      : "Confirm",  // 其他确认种类采用通用Confirm按钮文字
                     true, false);  // 确认按钮用强调状态且可用，实际动作仍由事件入口执行
    }
}
```


## 用同一文档观察各个指针状态

<a id="feedback-observation"></a>

下面保留前两章打开main.lua、拖选、移动和缩放的全部操作。新增观察先固定Editor为最大化，用draw取得实际body，再移动到Save按钮内部。normal/hot/down取自同一个按钮底面像素，避开文字和圆角；每次状态改变后都重新绘帧。为了只观察按压外观而不真正保存，程序直接设置pointer_down，随后用phase=2验证真实释放路径。

直接控件观察使用相同矩形，分别关闭和开启区域许可；禁用按钮在鼠标移入、按下和移出时保持同一种底面。最后将Settings覆盖到Editor上方，移动到原Save位置。普通移动必须保留Editor正文，active也继续是Settings。真实点击、服务保存与绘图反馈各自留下可比较的状态。持续拖选仍只调用G20的正文定位器，即使移到Save也不会送出保存动作。

这组观察随后把同一Editor设为800×400普通外框，body宽798所以没有项目栏。将正文替换为print(42)加换行后，在第一行第1间隙按下，再移到Save按钮内部并释放。定位器把上方位置夹到正文第0间隙，anchor仍是1；dirty 保持 true，editor 和 saved 各自与操作前的独立副本逐字节相同，服务 read 也读回旧 saved。这样既确认第一批文字列可以起拖，也确认续拖没有调用保存按钮。

**完整写入 `labs/tutorial/pointer.c`：**

<a id="code-labs-tutorial-pointer-c-1"></a>

<!-- write: labs/tutorial/pointer.c -->
```c
#include "ui.h"      // 取得共享桌面状态和输入事件接口。
#include <string.h>  // 用主机memcmp核对独立文档副本
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

static bool observe_monitor_feedback(uint32_t *pixels, const struct airi_framebuffer *fb) {  // 观察资源区从窗口反馈状态中独立出来。
    ui_open(&desktop, UI_EDITOR);    // 保留一个后方窗口，避免空桌面绕过窗口绘制路径。
    ui_open(&desktop, UI_SETTINGS);  // Settings成为当前活动窗口，再通过真实关闭事件离开。
    desktop.monitor = true;  // 本观察必须绘制右侧资源区。
    ui_draw(&desktop, fb);   // 取得当前Settings实际外框，关闭坐标不能猜初始布局。
    struct ui_rect r = desktop.windows[UI_SETTINGS].rect;  // 读取同一帧回写的当前外框。
    ui_pointer(&desktop, r.x + r.w - 20, r.y + 20, 1);     // 点标题关闭，Settings没有文档保存确认。
    ui_pointer(&desktop, r.x + r.w - 20, r.y + 20, 2);     // 释放清按压与拖动，随后只观察hover。
    if (desktop.active != -1 || !desktop.windows[UI_EDITOR].open || desktop.windows[UI_SETTINGS].open)  // 无活动窗口，但后方Editor仍存在。
        return false;  // 尚未形成会暴露区域许可遗留的状态。
    ui_pointer(&desktop, 200, 200, 0);  // 先让鼠标离开资源按钮，保持无活动窗口。
    ui_draw(&desktop, fb);  // 后方Editor关闭自己的hover许可，资源区必须重新建立自己的许可。
    int left = desktop.width - UI_MONITOR_W + 24, top = desktop.height - 96;  // 资源按钮和monitor绘图使用同一内边距。
    unsigned at = (unsigned)(top + 3) * 1280 + (unsigned)(left + 3);  // 按钮底面取样避开文字和两像素圆角。
    uint32_t normal = pixels[at];  // 保存鼠标远离时本帧的实际底面颜色。
    ui_pointer(&desktop, left + 20, top + 16, 0);  // 普通移动进入资源按钮，不应打开任务窗口。
    ui_draw(&desktop, fb);  // 资源区重新开放许可，此像素应进入悬停色。
    if (desktop.active != -1 || pixels[at] == normal)  // 既检查外观变化，也检查移动没有执行动作。
        return false;  // 沿用底层窗口的false许可会使底面一直不变。
    ui_pointer(&desktop, left + 20, top + 16, 1);  // 真正按下才进入资源按钮动作分派。
    ui_pointer(&desktop, left + 20, top + 16, 2);  // 本次动作后仍完整结束按压。
    if (desktop.active != UI_TASKS_APP)  // 点击必须打开Tasks，不能只拥有悬停像素。
        return false;  // 绘图反馈与动作入口需分别满足对应结果。
    puts("monitor: no active window still permits hover and opens Tasks on click");  // 报告关闭、移动、按下的同一实际事件序列。
    return true;  // 资源区不继承底层窗口的反馈许可。
}

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
    ui_pointer(&desktop, x + 40, y, 2);  // 再释放一次仍保持无拖动，避免观察继承旧动作
    struct ui_rect before = desktop.windows[UI_EDITOR].rect;  // 保存普通窗口此刻的实际位置与尺寸
    int title_x = before.x + 210, title_y = before.y + 20;    // 标题中央避开最右侧三个系统按钮
    ui_pointer(&desktop, title_x, title_y, 1);  // 在标题按下，核心保存鼠标到窗口左上角的偏移
    ui_pointer(&desktop, title_x + 30, title_y + 25, 0);     // 移动事件沿同一拖动种类改变rect位置
    ui_pointer(&desktop, title_x + 30, title_y + 25, 2);     // 释放结束窗口移动，几何变化继续保留
    struct ui_rect moved = desktop.windows[UI_EDITOR].rect;  // 取得移动后而非原来的窗口矩形
    if (moved.x != before.x + 30 || moved.y != before.y + 25)  // 标题偏移必须转成同样的窗口位移
        return 1;  // 移动结果不符，不继续缩放观察
    ui_pointer(&desktop, moved.x + moved.w - 6, moved.y + moved.h - 6, 1);  // 右下16×16区域内按下开始缩放
    ui_pointer(&desktop, moved.x + moved.w + 24, moved.y + moved.h + 19, 0);  // 鼠标距窗口左上角的距离成为新宽高
    ui_pointer(&desktop, moved.x + moved.w + 24, moved.y + moved.h + 19, 2);  // 释放停止缩放，普通移动不再延续
    struct ui_rect resized = desktop.windows[UI_EDITOR].rect;  // 同一窗口的新尺寸与当前位置
    if (resized.w != moved.w + 24 || resized.h != moved.h + 19 || desktop.drag != UI_DRAG_NONE)  // 同时核对宽高与释放状态
        return 1;  // 有一项不符便停止，不能只以窗口仍存在判为成功
    puts("window moved by (30,25), resized by (24,19), released");  // 输出这组实际三事件操作留下的结果
    static uint32_t pixels[1280 * 800];  // 本主机观察拥有像素，绘图仅借用它
    struct airi_framebuffer fb = {  // 画布使用已建立的32位RGB合同
        .virtual_address = (uintptr_t)pixels, .width = 1280, .height = 800,  // 地址指向本进程数组，尺寸以像素计
        .pitch = 1280 * 4, .bpp = 32, .red_size = 8, .red_shift = 16,  // 每行5120字节，红通道在16至23位
        .green_size = 8, .green_shift = 8, .blue_size = 8  // 绿通道8至15位，蓝通道0至7位
    };
    ui_open(&desktop, UI_EDITOR);  // 让Editor成为最前可操作窗口
    desktop.windows[UI_EDITOR].maximized = true;  // 固定观察几何，首次draw更新实际rect
    ui_pointer(&desktop, 1000, 600, 0);  // 指针先远离工具栏，普通移动不触发动作
    ui_draw(&desktop, &fb);  // 把实际布局回写并生成初始像素
    struct ui_rect body = u_window_body(desktop.windows[UI_EDITOR].rect);  // 命中使用绘图刚算出的内容矩形
    int bx = body.x + 32, by = body.y + 16;  // Save按钮内部远离边缘、文字和底线的采样像素
    int button_x = body.x + 12, button_y = body.y + 8;  // 同一Save按钮的左上位置供采样定位
    unsigned at = (unsigned)(button_y + 3) * 1280 + (unsigned)(button_x + 3);  // 用一行1280像素把二维点转换为数组下标
    uint32_t normal = pixels[at];  // 记录指针远离时的按钮底面实际颜色
    char original[UI_DOC_SIZE];    // 独立保存移动前正文，避免指针别名掩盖变化
    memcpy(original, desktop.editor, sizeof(original));  // 后续hover不得修改这些字节
    ui_pointer(&desktop, bx, by, 0);  // 仅把位置移到Save按钮，仍不产生点击
    ui_draw(&desktop, &fb);     // 新帧加入U_HOT，只改变绘图状态
    uint32_t hot = pixels[at];  // 此像素位于按钮底面，悬停应与normal不同
    if (normal == hot || memcmp(original, desktop.editor, sizeof(original)))  // 同时检查反馈存在、正文保留
        return 1;  // 只改了位置却误改正文或没有反馈都应失败
    desktop.pointer_down = true;  // 单独演示绘图按压值，不发送会真正保存的phase1事件
    ui_draw(&desktop, &fb);      // 当前按钮同时带U_HOT和U_DOWN，按压优先
    uint32_t down = pixels[at];  // 取得按压底面，后续与hover比较
    if (hot == down)  // 按压应产生独立于悬停的底面
        return 1;  // 当前绘图没有呈现按压状态
    ui_pointer(&desktop, bx, by, 2);  // 真正释放事件清pointer_down，但位置仍在Save内部
    ui_draw(&desktop, &fb);  // 释放后恢复悬停底面
    if (desktop.pointer_down || pixels[at] != hot)  // 不能保留按压；hover仍应有效
        return 1;  // 释放路径没有清掉原来的按压反馈
    struct ui_rect sample = {4, 4, 90, 30};  // 直接控件观察位于整屏角落，与工作区内容分开
    u_begin(&fb);  // 借用同一数组并恢复整屏裁剪
    u_theme(UI_NIGHT);  // 将直接控件观察固定为夜间，使颜色比较可重复
    u_interaction(8, 8, false, false);  // 下层区域没有响应资格，即使指针在按钮内也无悬停
    u_button(sample, "Test", false, false);   // 普通启用按钮只绘制明确的normal状态
    uint32_t blocked = pixels[8 * 1280 + 8];  // 读取区域禁止响应时的同一点
    u_interaction(8, 8, false, true);  // 相同位置开放响应，只改变当前绘制区域许可
    u_button(sample, "Test", false, false);  // 同一按钮现在加入U_HOT
    if (blocked == pixels[8 * 1280 + 8])     // 区域许可必须确实影响视觉反馈
        return 1;  // 没有区分可响应和被遮挡区域
    u_interaction(8, 8, true, true);  // 指针既在按钮内又按着，先形成最强输入状态
    u_button(sample, "Test", false, true);     // disabled仍覆盖hover/down，只显示禁用外观
    uint32_t disabled = pixels[8 * 1280 + 8];  // 保存禁用按钮这一点的颜色
    u_interaction(1000, 600, false, true);     // 移出按钮并解除按压，禁用身份保持
    u_button(sample, "Test", false, true);     // 禁用按钮不因位置或按压变化而改变底面
    if (pixels[8 * 1280 + 8] != disabled)      // 两种指针输入下的禁用底面应一致
        return 1;  // 禁用控件仍错误显示可操作反馈
    ui_open(&desktop, UI_SETTINGS);  // Settings置顶，原Editor成为不能响应按钮的底层窗口
    struct ui_rect overlap = desktop.windows[UI_EDITOR].rect;  // 将上层几何暂设为同样位置，明确构成遮挡
    desktop.windows[UI_SETTINGS].rect = overlap;    // 仅观察遮挡，不改变文档数组
    desktop.windows[UI_SETTINGS].maximized = true;  // 相同外框将在下一帧得到同样布局
    ui_pointer(&desktop, bx, by, 0);  // 位于旧Save屏幕点的普通移动，不能调用Editor保存
    ui_draw(&desktop, &fb);  // 窗口绘图依层次决定谁有响应许可并覆盖下层像素
    if (desktop.active != UI_SETTINGS || memcmp(original, desktop.editor, sizeof(original)))  // hover不得置顶Editor或修改它的正文
        return 1;  // 被遮挡内容在普通移动中产生了动作
    puts("hover/down/up, disabled and occluded feedback observed; hover changed no document");  // 输出各输入状态的实际观察结果
    if (!observe_monitor_feedback(pixels, &fb))  // 再关闭活动窗口，验证资源区不继承后方Editor的禁止反馈。
        return 1;  // 资源hover或真实打开Tasks任一失败都不能结束为成功。
    if (desktop.editor_id != 1 || !desktop.selection || desktop.cursor == anchor)  // 先确认此前文件与窗口观察，再改变外框测试临界宽度。
        return 1;  // 原来的选择尚未建立，不能用后一组操作替代其结果。
    return observe_text_drag() ? 0 : 1;  // 窄窗口起拖与越过Save同样必须通过，最终才返回成功。
}
```

**在学习目录根目录运行：**

<!-- run -->
```sh
mkdir -p build  # 保存本次主机程序和图像，已有目录可以继续
cc -std=c11 -O0 -g -Wall -Wextra -Werror -Iinclude -Ifrontend/include labs/tutorial/pointer.c kernel/lib/base.c kernel/boot/info.c kernel/console/console.c frontend/core/*.c frontend/apps/*.c frontend/backends/reference.c frontend/render/draw.c -o build/pointer  # Shell展开两个通配符，把当前已经写出的核心和应用一起链接
./build/pointer  # 运行当前完整观察程序，退出非零表示本组观察失败
```

输出先确认文档与窗口操作，再报告`hover/down/up, disabled and occluded feedback observed; hover changed no document`。像素数组由这个观察进程拥有，渲染器只在调用期间借用；进程退出后数组结束。接下来将同一事件接口接入Cocoa窗口，真实平台只需提供同样的整屏坐标与phase。

选择边界观察还输出`selection: body798 starts at column1; drag over Save preserves text, saved and anchor`，对应本章完整程序中实际发送的按下、越界移动与释放。

资源观察通过标题按钮真正关闭Settings，使active=-1而Editor仍打开，再将指针移入资源按钮比较底面，最后按下打开Tasks。输出`monitor: no active window still permits hover and opens Tasks on click`；普通移动仍不执行应用动作。
