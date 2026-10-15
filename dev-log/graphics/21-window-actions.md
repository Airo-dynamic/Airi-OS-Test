# G21 · 操作应用按钮并移动、缩放窗口

Files 与 Editor 的正文点击已经连通。现在在 Editor 标题中央按下，将指针移动 (30,25) 后释放，同一窗口的左上角也应移动 (30,25)；在右下角按下再移动，则改变宽高。桌面保存拖动种类和按下时的偏移，后续移动沿用它，释放再清除。Terminal、System、Tasks 和 Viewer 的内容按钮也在这次接入各自的 click 函数。

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
    int drag_x;     // 移窗时按下点距窗口左边的偏移。
    int drag_y;     // 移窗时按下点距窗口顶部的偏移。
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

桌面接收整个屏幕的坐标，负责对话框、应用入口和窗口框；坐标落在 body 内时才交给对应应用。窗口操作和应用动作因此使用同一份几何，各自修改自己负责的状态。

<a id="window-actions"></a>

移窗只改变当前窗口的rect。绘图重新使用这个rect放置内容，命中也必须读取它；如果点击仍使用移动前的坐标，看到的文件行与实际打开的行就会错位。窗口层继续逆绘制顺序找最上方命中，只交出一次事件。

## 接入其他应用按钮

Terminal 的点击检查清屏按钮范围，命中后调用已有的 CLEAR 命令。应用分派统一清除字段焦点；后续文字由 console_key 处理。

其余应用现在首次接收窗口分派的坐标。先补齐Terminal、System、Tasks与Viewer的click声明，Settings仍沿用G18已经接入的入口。

**完整替换 `frontend/apps/internal.h`：**

<a id="code-frontend-apps-internal-h-1"></a>

<!-- write: frontend/apps/internal.h -->
```c
#ifndef AIRI_APPS_INTERNAL_H  // 防止应用共享接口重复包含
#define AIRI_APPS_INTERNAL_H  // 标记应用共享接口已经包含
#include "ui.h"  // 引入前端服务合同与桌面状态
#include <airi/base.h>  // 引入自有字节操作和有界格式化函数

void ui_app_text(int x, int y, const char *s);   // 用正文体和主要颜色绘制应用文字
void ui_app_muted(int x, int y, const char *s);  // 用小字体和次要颜色绘制说明文字
void ui_app_field(struct ui_rect r, const char *s, bool focused);  // 画输入字段和当前焦点边框
bool ui_app_match(const char *s, const char *q);  // 按字节判断s是否包含q；Terminal识别提示符，Files和Tasks筛选各自使用结果
void ui_app_dialog(struct ui_desktop *s, int kind, int target, const char *title, const char *body,  // 把种类、目标身份和文案保存到桌面对话框状态
                   const char *value);  // value是对话框输入字段的初始文字，函数只复制状态
void ui_app_unavailable(struct ui_desktop *s, struct ui_rect r, const char *service);  // 按服务状态绘制加载、空或不可用提示
void ui_app_toolbar(struct ui_rect r, const char *label);  // 在应用内容顶部绘制44像素工具栏
void ui_app_status(struct ui_rect r, const char *left, const char *right);  // 在应用内容底部绘制27像素状态栏
void ui_terminal_draw(struct ui_desktop *, struct ui_rect);  // 把console逻辑行拆成窗口显示行并绘制输入
void ui_terminal_click(struct ui_desktop *, struct ui_rect, int, int, bool);  // 命中当前内容矩形中的Clear按钮后清命令环；整屏x/y来自窗口分派
void ui_files_draw(struct ui_desktop *, struct ui_rect);  // 读取目录快照和排序索引，画路径、文件行和状态
void ui_files_click(struct ui_desktop *, struct ui_rect, int, int, bool);  // 按整屏x/y取得筛选、工具栏或可见文件行；twice为双击标记
void ui_editor_draw(struct ui_desktop *, struct ui_rect);  // 将文档字节偏移换成行列，画文字、选区和光标
unsigned ui_editor_position(const struct ui_desktop *, struct ui_rect, int, int);  // 只将内容矩形中的屏幕点夹到正文并返回字节间隙，供点击和续拖共用。
void ui_editor_click(struct ui_desktop *, struct ui_rect, int, int, bool);  // 按当前正文几何将整屏x/y换成文档间隙，或分派工具栏动作
void ui_tasks_draw(struct ui_desktop *, struct ui_rect);  // 绘制任务快照、CPU曲线和内存条
void ui_tasks_click(struct ui_desktop *, struct ui_rect, int, int, bool);  // 按当前筛选和行位置选择参考任务，确认框保存它的pid
void ui_system_draw(struct ui_desktop *, struct ui_rect);  // 按当前页显示同一启动信息的内存、设备或日志
void ui_system_click(struct ui_desktop *, struct ui_rect, int, int, bool);  // 按当前内容矩形转换页签或打开应用按钮的整屏坐标
void ui_settings_draw(struct ui_desktop *, struct ui_rect);  // 按页面绘制当前会话配色、密度和接口说明
void ui_settings_click(struct ui_desktop *, struct ui_rect, int, int, bool);  // 分别命中页签、主题和密度矩形；只改变当前会话偏好
void ui_viewer_draw(struct ui_desktop *, struct ui_rect);  // 把已提交时间和测量值缩放到真实时间比例的像素曲线
void ui_viewer_click(struct ui_desktop *, struct ui_rect, int, int, bool);  // 命中打开文件按钮后打开Files，CSV读取仍沿已有服务路径
void ui_open_series(struct ui_desktop *, const struct ui_file *);  // 读取CSV并在整份验证通过后提交成对时间数值数组
#endif  // 结束当前应用接口的重复包含保护
```

**追加到 `frontend/apps/terminal.c` 末尾：**

<a id="code-frontend-apps-terminal-c-1"></a>

<!-- append: frontend/apps/terminal.c -->
```c
void ui_terminal_click(struct ui_desktop *s, struct ui_rect r, int x, int y, bool twice) {  // Terminal内容点击只需命中清屏按钮。
    (void)twice;  // 统一点击接口保留双击参数；这个应用当前只使用点击位置。
    if (u_inside((struct ui_rect){r.x + r.w - 100, r.y + r.h - 61, 84, 28}, x, y))  // 命中底部输入行右侧的Clear按钮矩形时清屏。
        airi_console_key(s->console, AIRI_KEY_CLEAR);  // 复用命令对象的清屏规则，不删除另一份日志。
    return;  // 本次事件处理已结束，不再落入其他按钮或底层窗口。
}

```

System 将页签坐标转成 tab 下标，检查范围后更新页面。概况和存储页中的按钮调用 ui_open，打开 Settings 或 Files；窗口层次继续由同一桌面维护。

**追加到 `frontend/apps/system.c` 末尾：**

<a id="code-frontend-apps-system-c-1"></a>

<!-- append: frontend/apps/system.c -->
```c
void ui_system_click(struct ui_desktop *s, struct ui_rect r, int x, int y, bool twice) {  // System将内容坐标解释成页签与页面按钮。
    (void)twice;  // 统一点击接口保留双击参数；这个应用当前只使用点击位置。
    enum ui_app app = UI_SYSTEM;  // 本函数只修改System窗口状态。
    if (y < r.y + 45) {  // 内容顶部45像素属于页签区。
        int tab = (x - r.x - 14) / 108;  // 扣掉14像素内边距后按108像素一项算页签。
        if (tab >= 0 && tab < 5)  // 仅0..4是System有效页签。
            s->windows[app].tab = (unsigned)tab;  // 边界检查后再转无符号下标。
        return;  // 本次事件处理已结束，不再落入其他按钮或底层窗口。
    }
    if (s->windows[app].tab == 0 && y >= r.y + 328 && y < r.y + 360)  // 概况页中的设置按钮按当前内容纵坐标命中。
        ui_open(s, UI_SETTINGS);  // 打开并置顶设置窗口。
    if (s->windows[app].tab == 2 && y >= r.y + 285 && y < r.y + 317)  // 存储页中的文件按钮命中时打开Files。
        ui_open(s, UI_FILES_APP);  // 打开并置顶当前目录文件窗口。
    if (s->service_state == UI_ERROR) {  // 失败状态下的点击触发一次重新查询。
        s->service_state = UI_NORMAL;  // 恢复普通查询状态，再由刷新回调决定实际结果。
        ui_refresh(s);  // 按更新后的目录及服务状态重新取得文件与任务快照。
    }
    return;  // 本次事件处理已结束，不再落入其他按钮或底层窗口。
}

```

Tasks 按筛选、排序与滚动将行位置还原成任务项。双击显示详情，End task 将该项 pid 保存到确认框；确认后才调用服务删除记录。

**追加到 `frontend/apps/tasks.c` 末尾：**

<a id="code-frontend-apps-tasks-c-1"></a>

<!-- append: frontend/apps/tasks.c -->
```c
void ui_tasks_click(struct ui_desktop *s, struct ui_rect r, int x, int y, bool twice) {  // 任务列表按位置选择快照项，停止确认保存pid。
    if (y < r.y + 44) {  // 当前应用工具栏先处理按钮，不把这一点击解释为列表行。
        if (x > r.x + r.w - 98 && s->task_count)  // 右侧End task按钮还要求有可选任务记录。
            ui_app_dialog(  // 先建立确认框，不在鼠标按下时立即停止。
                s, UI_DIALOG_STOP, s->tasks[s->selected_task].pid, "End task",  // 取当前快照项的稳定pid作为目标。
                "Remove this task from the recorded session?", "");  // 说明只移除参考记录，不会终止宿主进程。
        else if (x > r.x + r.w - 184) {  // 相邻刷新按钮重新请求任务快照。
            s->service_state = UI_NORMAL;  // 恢复普通查询状态，再由刷新回调决定实际结果。
            ui_refresh(s);  // 按更新后的目录及服务状态重新取得文件与任务快照。
        }
        return;  // 本次事件处理已结束，不再落入其他按钮或底层窗口。
    }
    if (s->service_state == UI_ERROR &&  // 仅在服务失败界面检测重试按钮。
        u_inside((struct ui_rect){r.x + 72, r.y + 179, 92, 32}, x, y)) {  // 失败页面的重试按钮范围为92×32。
        s->service_state = UI_NORMAL;  // 恢复普通查询状态，再由刷新回调决定实际结果。
        ui_refresh(s);  // 按更新后的目录及服务状态重新取得文件与任务快照。
        return;  // 本次事件处理已结束，不再落入其他按钮或底层窗口。
    }
    int top = r.y + (r.h >= 540 ? 160 : 100) + 139;  // 高窗口为汇总面板留160像素，矮窗口留100像素，再跳过固定顶部布局。
    if (y >= top && y < top + 32) {  // 此高度范围是任务筛选字段。
        s->focus = UI_FOCUS_FILTER;  // 后续键入先交给当前应用的筛选字段。
        return;  // 本次事件处理已结束，不再落入其他按钮或底层窗口。
    }
    if (y >= top + 48 && y < top + 76) {  // 表头区域切换排序方向。
        s->sort = !s->sort;  // 逻辑非把排序方向在0与1之间切换。
        return;  // 本次事件处理已结束，不再落入其他按钮或底层窗口。
    }
    top += 80;  // 跳过筛选和表头得到实际首行顶部。
    if (y >= top) {  // 只有列表内容区域才把y转换成行号。
        unsigned row =  // 以下计算筛选后的全列表行号。
            (unsigned)(y - top) / (s->density ? 28 : 36) + s->windows[UI_TASKS_APP].scroll;  // 使用与绘图同一密度行高，并补卷走行数。
        unsigned shown = 0;  // 只统计筛选后可见记录。
        for (unsigned k = 0; k < s->task_count; ++k) {  // 在有效任务条数内遍历排序位置。
            unsigned i = s->sort ? s->task_count - 1 - k : k;  // 降序反向取快照下标，升序直接取k。
            if (!ui_app_match(s->tasks[i].name, s->filter[UI_TASKS_APP]))  // 不匹配的任务不占可见行。
                continue;  // 跳过当前不适用项，继续检查下一项。
            if (shown++ == row) {  // 先比较当前可见行，再递增计数；命中时 i 就是原记录下标。
                s->selected_task = i;  // 选择保存原快照下标，后续操作再读取pid。
                if (twice) {  // 双击额外显示这条记录的详情。
                    char details[256];  // 局部详情文本，dialog函数会复制其内容。
                    airi_snprintf(details, 256,  // 有界格式化到详情数组，容量包含终止NUL。
                                  "PID %d / %s / %s / CPU %u%% / %u KiB. Recorded session.",  // %%输出百分号，其他字段分别对应pid、名称、状态、CPU和内存。
                                  s->tasks[i].pid, s->tasks[i].name, s->tasks[i].state,  // 按实际命中记录取身份和文本。
                                  s->tasks[i].cpu, s->tasks[i].memory_kib);  // 数值单位与格式字符串中的百分数/KiB一致。
                    ui_app_dialog(s, UI_DIALOG_INFO, 0, "Process details", details, "");  // 复用消息框显示详情，此处不对应待提交服务目标。
                }
                break;  // 当前匹配已完成，停止这一层循环或分支。
            }
        }
    }
    return;  // 本次事件处理已结束，不再落入其他按钮或底层窗口。
}

```

Settings的内容点击已在G18完成，当前分派继续复用它。Night/Day逐个检查126×34像素按钮，按钮间隙不触发动作；density位于另一行，页签只改变Settings窗口的tab。

Viewer 的按钮打开 Files，数据仍通过已有文件读取与 CSV 解析流程进入，点击不另建立一条读取路径。

**追加到 `frontend/apps/viewer.c` 末尾：**

<a id="code-frontend-apps-viewer-c-1"></a>

<!-- append: frontend/apps/viewer.c -->
```c
void ui_viewer_click(struct ui_desktop *s, struct ui_rect r, int x, int y, bool twice) {  // Viewer的工具栏只提供打开Files按钮。
    (void)twice;  // 统一点击接口保留双击参数；这个应用当前只使用点击位置。
    if (y < r.y + 44 && x > r.x + r.w - 120)  // 命中Viewer顶部右侧文件入口的命中带，打开Files。
        ui_open(s, UI_FILES_APP);  // 打开并置顶当前目录文件窗口。
    return;  // 本次事件处理已结束，不再落入其他按钮或底层窗口。
}

```

各应用点击函数完成后，分派表加入 click。调用前清字段焦点，再由被命中的应用选择搜索框等新焦点；没有匹配应用时直接结束。

**完整替换 `frontend/apps/builtin.c`：**

<a id="code-frontend-apps-builtin-c-1"></a>

<!-- write: frontend/apps/builtin.c -->
```c
#include "internal.h"  // 取得应用内部绘图、命中与对话框的共同声明。
void ui_apps_draw(struct ui_desktop *s, enum ui_app app, struct ui_rect r) {  // 根据应用枚举绘制同一内容矩形，窗口框由外层负责。
    switch (app) {     // 应用枚举决定本次内容绘制器。
    case UI_TERMINAL:  // 应用身份为终端时进入对应分支。
        ui_terminal_draw(s, r);  // 用当前桌面和内容矩形绘制终端。
        break;  // 当前匹配已完成，停止这一层循环或分支。
    case UI_FILES_APP:  // 应用身份为目录时进入对应分支。
        ui_files_draw(s, r);  // 用当前桌面和内容矩形绘制目录。
        break;  // 当前匹配已完成，停止这一层循环或分支。
    case UI_EDITOR:  // 应用身份为文档时进入对应分支。
        ui_editor_draw(s, r);  // 用当前桌面和内容矩形绘制文档。
        break;  // 当前匹配已完成，停止这一层循环或分支。
    case UI_TASKS_APP:  // 应用身份为任务时进入对应分支。
        ui_tasks_draw(s, r);  // 用当前桌面和内容矩形绘制任务。
        break;  // 当前匹配已完成，停止这一层循环或分支。
    case UI_SYSTEM:  // 应用身份为系统信息时进入对应分支。
        ui_system_draw(s, r);  // 用当前桌面和内容矩形绘制系统信息。
        break;  // 当前匹配已完成，停止这一层循环或分支。
    case UI_SETTINGS:  // 应用身份为设置时进入对应分支。
        ui_settings_draw(s, r);  // 用当前桌面和内容矩形绘制设置。
        break;  // 当前匹配已完成，停止这一层循环或分支。
    case UI_VIEWER:  // 应用身份为曲线时进入对应分支。
        ui_viewer_draw(s, r);  // 用当前桌面和内容矩形绘制曲线。
        break;  // 当前匹配已完成，停止这一层循环或分支。
    default:  // 未知应用编号不绘制内容；窗口框仍由外层处理
        break;  // 当前匹配已完成，停止这一层循环或分支。
    }
}

void ui_apps_click(struct ui_desktop *s, enum ui_app app, struct ui_rect r, int x, int y,  // 总入口先按应用身份分派内容点击。
                   bool twice) {  // 双击标志保持原值交给应用，不在桌面重复解释。
    s->focus = UI_FOCUS_NONE;  // 先取消旧字段焦点，应用命中字段后可重新设置。
    switch (app) {     // 按已命中的应用编号选择点击处理器
    case UI_TERMINAL:  // 应用身份为终端时进入对应分支。
        ui_terminal_click(s, r, x, y, twice);  // 将当前矩形、屏幕点和双击状态交给终端的命中规则。
        break;  // 当前匹配已完成，停止这一层循环或分支。
    case UI_FILES_APP:  // 应用身份为目录时进入对应分支。
        ui_files_click(s, r, x, y, twice);  // 把当前内容矩形和屏幕点交给 Files。
        break;  // 当前匹配已完成，停止这一层循环或分支。
    case UI_EDITOR:  // 应用身份为文档时进入对应分支。
        ui_editor_click(s, r, x, y, twice);  // 把相同事件表示交给 Editor。
        break;  // 当前匹配已完成，停止这一层循环或分支。
    case UI_TASKS_APP:  // 应用身份为任务时进入对应分支。
        ui_tasks_click(s, r, x, y, twice);  // 将当前矩形、屏幕点和双击状态交给任务的命中规则。
        break;  // 当前匹配已完成，停止这一层循环或分支。
    case UI_SYSTEM:  // 应用身份为系统信息时进入对应分支。
        ui_system_click(s, r, x, y, twice);  // 将当前矩形、屏幕点和双击状态交给系统信息的命中规则。
        break;  // 当前匹配已完成，停止这一层循环或分支。
    case UI_SETTINGS:  // 应用身份为设置时进入对应分支。
        ui_settings_click(s, r, x, y, twice);  // 将当前矩形、屏幕点和双击状态交给设置的命中规则。
        break;  // 当前匹配已完成，停止这一层循环或分支。
    case UI_VIEWER:  // 应用身份为曲线时进入对应分支。
        ui_viewer_click(s, r, x, y, twice);  // 将当前矩形、屏幕点和双击状态交给曲线的命中规则。
        break;  // 当前匹配已完成，停止这一层循环或分支。
    default:  // 未知或本阶段尚未接点击的应用不改变状态
        break;  // 当前匹配已完成，停止这一层循环或分支。
    }
}
```

## 由顶层窗口接收坐标

<a id="drag-state"></a>

拖动需要记住按下时的关系。窗口左上为 (100,80)，鼠标在标题栏 (130,95) 按下，保存偏移 (30,15)。后续鼠标移到 (160,120)，窗口新左上应为 (130,105)，仍保持鼠标抓住原来的位置。只把鼠标坐标当窗口原点会令窗口在按下瞬间跳动。

drag 保存当前动作种类，drag_x/drag_y 保存移动时的按下点到外框原点偏移；缩放直接以当前鼠标减外框原点计算新宽高。释放后清 drag，后续普通移动才不会继续移动旧窗口。

平台事件约定 phase=0 移动、1 按下、2 释放、3 双击。已有 drag 时先续接拖动：标题栏按下保存鼠标到窗口原点的偏移，移动时从鼠标坐标减去该值；右下角 Resize 改尺寸并保持最小值，释放清 drag。

没有拖动时，先处理对话框，再处理 launcher、系统栏、侧栏和任务栏。活动对话框截获点击，Save 提交、Discard 还原 saved 后关闭、Cancel 清关闭意图。普通窗口随后才有机会接收事件。

**完整写入 `frontend/core/pointer.c`：**

<a id="code-frontend-core-pointer-c-1"></a>

<!-- write: frontend/core/pointer.c -->
```c
#include "internal.h"  // 取得应用内部绘图、命中与对话框的共同声明。
#include "../apps/internal.h"  // 借用Editor正文定位器，续拖不调用它的完整按钮分派。
void ui_pointer(struct ui_desktop *s, int x, int y, int phase) {  // phase=0移动、1按下、2释放、3双击；x/y 在桌面屏幕坐标中。
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

同一坐标可能同时落在两个重叠窗口。应由画面中最上方的窗口接收，所以命中逆着绘制顺序查找，找到第一个就停止；若继续把事件交给底下窗口，一次点击会改变两份状态。

实现从 order 末尾向前检查，找到窗口后调用 ui_open 置顶，再区分标题栏按钮、拖动和内容；最大化先保存 restore，恢复时读回它。

内容坐标交给应用 click。Editor拖选仍调用G20的正文定位器，anchor固定初始间隙，cursor由夹回正文的当前坐标得到；持续移动不再进入应用按钮分派。

滚轮按行更新对应视图。先用有符号临时值相加，再把负数限到零，避免 unsigned 向下溢出。Terminal 使用 console.scroll，Editor 使用 editor_scroll，其余使用窗口自身 scroll。

## 沿同一鼠标序列观察窗口几何

观察程序仍先双击main.lua并拖出Editor选区，再在标题中央按下，移动(30,25)后释放。新rect左上角必须增加相同偏移。随后在右下角按下，将鼠标移动到原右下角外侧(24,19)，宽高按鼠标到左上角的距离增加，释放后drag必须恢复为NONE。下一次绘图会按屏幕可用区收窄窗口；这组观察先检查事件直接留下的几何。

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
    if (desktop.editor_id != 1 || !desktop.selection || desktop.cursor == anchor)  // 先确认此前文件与窗口观察，再改变外框测试临界宽度。
        return 1;  // 原来的选择尚未建立，不能用后一组操作替代其结果。
    return observe_text_drag() ? 0 : 1;  // 窄窗口起拖与越过Save同样必须通过，最终才返回成功。
}
```

## 绘制启动菜单，并让 Escape 结束当前交互

顶栏点击会切换 launcher，绘图据此在普通窗口上方显示应用菜单。命中菜单项后打开对应应用并结束本次分派；点击菜单外会先收起菜单，再按所在的桌面区域继续处理。初始化令 launcher 关闭。Escape 收起菜单并结束当前反馈，已有窗口继续保留；更新状态、绘图和按键三个文件后，新的窗口动作才有完整的进入与退出路径。

**完整写入 `frontend/core/state.c`：**

<a id="code-frontend-core-state-c-1"></a>

<!-- write: frontend/core/state.c -->
```c
#include "internal.h"  // 引入当前目录内部共享接口和基础函数
#include "ui.h"  // 引入前端服务合同与桌面状态
#include <airi/base.h>  // 引入自有字节操作和有界格式化函数
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
void ui_refresh(struct ui_desktop *s) {  // 重新取得文件任务快照，更新显示所需有效计数
    s->file_count = 0;  // 刷新前清有效文件条数，失败时不显示旧成功快照
    s->task_count = 0;  // 刷新前清有效任务条数，随后由任务服务填写
    airi_memset(&s->stats, 0, sizeof(s->stats));  // 清零整个当前状态或快照，使计数和标记从确定初值开始
    if (s->service_state != UI_NORMAL)  // 服务未正常时停止复制或显示成功快照
        return;  // 当前重新取得文件任务快照，更新显示所需有效计数在本分支结束，后续语句不执行
    if (s->backend.files)  // 文件查询回调存在时才调用
        s->file_count = s->backend.files(s->backend.context, s->directory, s->files, UI_FILES);  // 查询当前目录并复制最多32项元信息到桌面数组，返回值标出此次有效前缀
    if (s->file_count > UI_FILES)  // 服务条数超过本地32槽容量时收窄
        s->file_count = UI_FILES;  // 把服务返回条数限制到本地文件快照容量
    if (s->backend.tasks)  // 任务查询回调存在时才取得同次任务与统计
        s->task_count = s->backend.tasks(s->backend.context, s->tasks, UI_TASKS, &s->stats);  // 同次调用复制最多16项参考任务与资源统计，返回值界定任务数组有效前缀
    if (s->task_count > UI_TASKS)  // 服务任务条数超过16槽容量时收窄
        s->task_count = UI_TASKS;  // 把服务返回条数限制到本地任务快照容量
    if (s->stats.cpu > 100)  // CPU百分数超过100时截到绘图上界
        s->stats.cpu = 100;  // CPU百分数超过100时限制显示值
    for (unsigned i = 0; i < 60; ++i)  // 按采样下标处理60个参考百分数点
        if (s->stats.samples[i] > 100)  // 当前历史采样超过100时限制高度，避免曲线越出图区
            s->stats.samples[i] = 100;  // 把当前历史采样限制在百分数绘图上界
    if (s->stats.memory_kib > s->stats.total_kib)  // 已用参考内存超过总量时收窄显示值
        s->stats.memory_kib = s->stats.total_kib;  // 已用内存显示量最多等于本次统计总量
    if (s->selection_file >= s->file_count)  // 旧选择不在新文件快照有效范围时归零
        s->selection_file = 0;  // 旧选中下标失效时回到新快照第一项
    if (s->selected_task >= s->task_count)  // 旧任务选择不在新快照有效范围时归零
        s->selected_task = 0;  // 旧任务选择失效时回到任务快照首项
}
void ui_init(struct ui_desktop *s, struct ui_backend b, struct airi_console *c) {  // 清桌面状态，绑定服务和console并打开默认应用
    airi_memset(s, 0, sizeof(*s));  // 清零整个当前状态或快照，使计数和标记从确定初值开始
    s->backend = b;  // 把回调表和上下文复制进桌面，服务数据仍归服务拥有
    c->extension = appearance;  // 安装同步命令扩展；console 不包含主题或窗口类型。
    c->extension_context = s;   // 桌面和 console 都由平台持有，须覆盖后续输入调用。
    s->console = c;  // 桌面借用同一console状态，Terminal不另复制日志
    s->active = -1;  // 没有活动窗口，-1哨兵不能拿去索引应用数组
    s->drag = UI_DRAG_NONE;  // 当前没有进行窗口或选区拖动
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
    s->launcher = false;   // 打开应用后关闭启动菜单
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
void ui_command(struct ui_desktop *s, enum ui_action action, int id, const char *name,  // 调用服务操作，成功后刷新，失败形成错误对话框
                const char *data) {  // data借用本次请求正文，回调返回前按合同复制或读取
    char error[160] = "This operation is unavailable.";  // 先给160字节错误缓冲设置未连接原因，服务失败时可覆盖
    if (s->service_state == UI_NORMAL && s->backend.action &&  // 先要求服务正常且动作回调存在，再调用它；短路与避免空函数指针
        s->backend.action(s->backend.context, s->directory, action, id, name, data, error, sizeof(error))) {  // 服务回调收到明确操作与完整输入，仅true使前端进入成功提交
        copy(s->toast, sizeof(s->toast),  // 成功提示写到160字节toast数组，下一行按操作选择具体反馈
             s->backend.mode == UI_REFERENCE  // 按实际绑定后端模式选择后续参考说明或内核说明，不修改mode字段
                 ? (action == UI_SAVE ? "Saved to session storage."  // 参考保存成功只说明本会话内存记录已更新
                                      : "Session storage updated.")  // 其他参考记录操作成功使用工作区更新提示
                 : (action == UI_SAVE ? "Document saved." : "Operation completed."));  // 真实后端按保存或其他操作选择成功提示，前提是回调已返回true
        ui_refresh(s);  // 以当前directory重新复制文件快照；任务回调接入后同次刷新它和资源统计
        if (action == UI_RENAME && id == s->editor_id)  // 重命名目标正是当前编辑文档时，同时更新窗口内名称，正文与保存点不变
            copy(s->editor_name, 64, name);  // 把name复制到当前文档名，64是包含NUL的数组容量
        if (action == UI_SAVE) {  // 只有保存服务已返回成功才进入此处，随后建立新的前端保存点
            if (s->pending_close) {  // 此保存是未保存关闭确认的一部分，成功提交后才关Editor窗口
                s->windows[UI_EDITOR].open = false;  // 保存关闭请求已经成功提交，此时才隐藏Editor窗口，保存文字仍在服务中
                s->active = -1;  // 没有活动窗口，-1哨兵不能拿去索引应用数组
                s->pending_close = false;  // 结束关闭请求，后续普通保存不应沿用旧关闭动作
            }
            s->dirty = false;  // 当前文字与保存副本一致，清除未保存标记
            copy(s->saved, sizeof(s->saved), s->editor);  // 把服务已接受的文字记录为独立保存点
            if (s->editor_id < 0)  // 新文档尚无服务id，刷新后按提交名称找到新建记录并绑定编辑器
                for (unsigned i = 0; i < s->file_count; ++i)  // 只遍历本次目录快照有效前缀，未用槽不参与
                    if (!airi_memcmp(s->files[i].name, name, airi_strlen(name) + 1)) {  // 在新目录快照中完整匹配刚保存的名称，连NUL比较避免前缀相同误绑定
                        s->editor_id = s->files[i].id;  // 读取或保存成功后绑定该快照项的服务id，后续保存用此身份而不用显示下标
                        copy(s->editor_name, 64, name);  // 把name复制到当前文档名，64是包含NUL的数组容量
                        break;  // 结束当前最近一层循环或应用分派，不执行其剩余候选
                    }
        }
    } else {  // 前一分支条件未成立时处理当前替代路径
        s->pending_close = false;     // 结束关闭请求，后续普通保存不应沿用旧关闭动作
        s->dialog = UI_DIALOG_ERROR;  // 把服务失败显示成错误对话框
        copy(s->dialog_title, sizeof(s->dialog_title), action == UI_SAVE ? "Save failed" : "Operation unavailable");  // 保存失败使用明确标题，其余保留通用操作错误。
        airi_snprintf(s->dialog_text, sizeof(s->dialog_text), "%s%s", error,  // 在容量内先写服务返回的具体原因。
                      action == UI_SAVE ? " Your edits are still open." : "");  // 追加恢复说明，不清空编辑器或重设保存点。
    }
}
```

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

**完整写入 `frontend/core/editing.c`：**

<a id="code-frontend-core-editing-c-1"></a>

<!-- write: frontend/core/editing.c -->
```c
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
        s->launcher = false;  // Escape关闭应用启动菜单，保持已有窗口打开状态
        s->find = false;      // 隐藏Editor查找栏，后续正文可使用这部分高度
        s->focus = UI_FOCUS_NONE;  // 当前字段结束接收，后续按键回到活动应用
        return true;  // 本逻辑事件已处理，调用者可重画当前状态
    }
    if (s->dialog) {  // 有对话框时先由它消费当前键，下面的字段和应用不再接收同一事件
        if (key == '\n' || key == '\r') {  // 换行或回车表示提交当前命令、字段或文档换行动作，按当前接收者处理
            if (s->dialog == UI_DIALOG_ERROR || s->dialog == UI_DIALOG_INFO)  // 错误提示只需关闭，不应当被回车转成文件服务动作
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
    if (key == UI_FIND_KEY && (s->active == UI_FILES_APP || s->active == UI_TASKS_APP)) {  // Files和Tasks的查找快捷键只切到该应用筛选框，不开启Editor文内查找
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
    if (s->active == UI_TASKS_APP && (key == AIRI_KEY_UP || key == AIRI_KEY_DOWN)) {  // Tasks的上下键只调整任务快照选择，Editor光标不受影响
        if (key == AIRI_KEY_UP && s->selected_task)  // 选中任务不在第0项时才向前一项移动
            --s->selected_task;  // 沿当前操作方向调整当前任务快照的选中下标
        if (key == AIRI_KEY_DOWN && s->selected_task + 1 < s->task_count)  // 下一个任务下标仍小于快照有效条数时才增加选择
            ++s->selected_task;  // 沿当前操作方向调整当前任务快照的选中下标
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
        while (s->cursor && s->editor[s->cursor - 1] != '\n')  // 扫描光标前的ASCII字节，换行决定行列转换
            --s->cursor;  // 光标向前移动一个ASCII字节间隙
    } else if (key == UI_END) {  // 向后扫描到本行换行之前或文档结尾，不跨到下一行
        while (s->cursor < len && s->editor[s->cursor] != '\n')  // 从当前光标向后扫描，遇本行换行或文档末尾就停止
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
```

**在学习目录根目录运行：**

<!-- run -->
```sh
mkdir -p build  # 保存本次主机程序和图像，已有目录可以继续
cc -std=c11 -O0 -g -Wall -Wextra -Werror -Iinclude -Ifrontend/include labs/tutorial/pointer.c kernel/lib/base.c kernel/boot/info.c kernel/console/console.c frontend/core/*.c frontend/apps/*.c frontend/backends/reference.c frontend/render/draw.c -o build/pointer  # Shell展开两个通配符，把当前已经写出的核心和应用一起链接
./build/pointer  # 运行当前完整观察程序，退出非零表示本组观察失败
```

输出除了文档身份和选区，还包括`window moved by (30,25), resized by (24,19), released`。标题与应用正文共用同一实际外框；窗口改变后，下一次命中会从更新的rect重新取得body。

选择边界观察还输出`selection: body798 starts at column1; drag over Save preserves text, saved and anchor`，对应本章完整程序中实际发送的按下、越界移动与释放。
