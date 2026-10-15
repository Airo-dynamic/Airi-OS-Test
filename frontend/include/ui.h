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
enum ui_drag { UI_DRAG_NONE = -1 };  // 尚无指针拖动，初始化值表示无操作
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
    int active;    // 活动应用，-1表示当前没有。
    int width;     // 最近绘图的桌面宽度，单位像素。
    int height;    // 最近绘图的桌面高度，单位像素。
    bool monitor;  // 右侧状态监视区是否展开。
    bool find;     // Editor查找栏是否展开。
    bool dirty;    // 工作文本是否与最近成功保存内容不同。
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
};
void ui_init(struct ui_desktop *, struct ui_backend, struct airi_console *);  // 初始化桌面，复制服务表并借用命令对象。
void ui_open(struct ui_desktop *, enum ui_app);  // 打开、解除最小化并置顶指定应用。
void ui_draw(struct ui_desktop *, const struct airi_framebuffer *);  // 按实际帧缓冲绘图，并更新可用于命中的窗口矩形。
bool ui_key(struct ui_desktop *, int key, bool shift);      // 分派字符或功能键，Shift控制扩展选区等规则。
void ui_service_state(struct ui_desktop *, enum ui_state);  // 切换服务显示状态并刷新对应快照。
void ui_refresh(struct ui_desktop *);  // 从当前服务取得文件、任务与汇总快照。
void ui_command(struct ui_desktop *, enum ui_action, int, const char *, const char *);  // 把稳定身份、名称与内容交给服务动作回调。
void ui_fixture_backend(struct ui_backend *);  // 为主机观察填入会话内存参考服务，不进入guest。

void u_icon(int, int, int, uint32_t);  // 在给定坐标以类型和颜色绘制小图标。
unsigned ui_file_order(const struct ui_desktop *, unsigned *);  // 把当前文件排序位置写成快照下标数组并返回数量。
void ui_open_document(struct ui_desktop *, unsigned);  // 接收快照下标，按服务id读取普通文档并打开Editor。
void ui_apps_draw(struct ui_desktop *, enum ui_app, struct ui_rect);  // 以应用身份分派内容绘图，rect已扣除标题栏。
void ui_apps_click(struct ui_desktop *, enum ui_app, struct ui_rect, int, int, bool);  // 将内容矩形、屏幕点与双击状态交给具体应用。
#endif  // 结束完整桌面接口的重复包含保护
