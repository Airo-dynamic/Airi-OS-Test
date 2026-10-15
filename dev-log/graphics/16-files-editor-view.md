# G16 · 把文件快照和文档显示成可读内容

Terminal和System已能在窗口中读取状态。现在把G14服务返回的目录元信息画成Files列表，再把main.lua正文复制到Editor显示。Files的选中快照下标和Editor正在编辑的服务id各自保存，排序不会暗中切换已打开的文档。

<a id="editor-frame"></a>

Editor的44像素工具栏下方再用32像素文档条标明main.lua。正文区从内容顶下88像素开始；Files首个列表行则在内容顶下138像素。两个应用共享窗口标题和边框，却各自定义正文起点，点击时必须继续采用各自的转换。u_selection把列表选中状态画成淡底和三像素粉色侧标；代码选区则仍按每字节十像素铺底。

<a id="document-copies"></a>

服务数组拥有正文，read把完整内容复制到desktop.editor；saved是另一块数组，保存本次读取的相同文字。修改editor不会自动修改服务或saved。现在只读入和显示，dirty因此为false；下一章保存成功时才更新保存点。

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
enum ui_action { UI_CREATE_FILE, UI_CREATE_DIR };  // 当前服务仅支持创建普通文件和目录
enum ui_focus { UI_FOCUS_NONE, UI_FOCUS_FILTER, UI_FOCUS_FIND };  // 键入目标为应用正文、筛选字段或查找字段。
enum ui_drag { UI_DRAG_NONE = -1 };  // 尚无指针拖动，初始化值表示无操作
struct ui_file {  // 服务复制出来的一条文件快照，不保存文件内容。
    int id;  // 稳定服务身份，排序和筛选不改变它。
    bool directory;  // 区分目录和普通文件，决定打开后的动作。
    char name[64], path[128];  // 保存有NUL终止的名称与完整路径，容量分别64/128字节。
    unsigned bytes;  // 文件内容字节数，目录行可显示其他语义。
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
    int active;  // 活动应用，-1表示当前没有。
    int width;   // 最近绘图的桌面宽度，单位像素。
    int height;  // 最近绘图的桌面高度，单位像素。
    bool find;   // Editor查找栏是否展开。
    bool dirty;  // 工作文本是否与最近成功保存内容不同。
    bool selection;  // anchor与cursor之间是否构成有效选区。
    enum ui_state service_state;  // 当前界面服务状态，失败动作通过它显示。
    char directory[128];  // 当前文件操作目录，有NUL终止。
    char filter[UI_APP_COUNT][64];  // 每个应用独立筛选文本，二维数组先按应用再按字节。
    char editor[UI_DOC_SIZE];  // 正在修改的工作文本，服务内容不会随按键自动改变。
    char saved[UI_DOC_SIZE];   // 独立数组接收服务实际保存的文本。
    char find_text[64];  // 查找输入字段，搜索结果映射回文档字节偏移。
    unsigned cursor;     // 光标位于0..文本长度的字节间隙。
    unsigned anchor;     // 选区起点间隙，拖动时固定。
    unsigned editor_scroll;  // 正文顶部已卷走的行数。
    unsigned editor_column_scroll;  // 正文左侧已卷走的字符列数。
    unsigned selection_file;  // 文件快照数组下标，不是文件id或排序后行。
    int editor_id;  // 当前文档服务id，未保存的新文档另按创建路径处理。
    int focus;      // 当前键入字段身份。
    int sort;  // 列表排序方向，0/1切换。
    int file_clipboard;    // 文件剪贴板保存源id，-1表示无。
    char editor_name[64];  // 当前文档名称，保存和执行请求使用。
    struct ui_file files[UI_FILES];  // 当前目录的有界快照记录。
    unsigned file_count;  // files中有效条数，读取不得超过它。
    unsigned theme;    // 桌面自己持有的日夜模式，文本会话不依赖此字段。
    unsigned density;  // Files列表行高，0为36像素、非0为28像素；当前绘图读取此值。
};
void ui_init(struct ui_desktop *, struct ui_backend, struct airi_console *);  // 初始化桌面，复制服务表并借用命令对象。
void ui_open(struct ui_desktop *, enum ui_app);  // 打开、解除最小化并置顶指定应用。
void ui_draw(struct ui_desktop *, const struct airi_framebuffer *);  // 按实际帧缓冲绘图，并更新可用于命中的窗口矩形。
void ui_service_state(struct ui_desktop *, enum ui_state);  // 切换服务显示状态并刷新对应快照。
void ui_refresh(struct ui_desktop *);  // 从当前目录取得最多32条文件元信息副本。
void ui_fixture_backend(struct ui_backend *);  // 为主机观察填入会话内存参考服务，不进入guest。

void u_icon(int, int, int, uint32_t);  // 在给定坐标以类型和颜色绘制小图标。
unsigned ui_file_order(const struct ui_desktop *, unsigned *);  // 把当前文件排序位置写成快照下标数组并返回数量。
void ui_apps_draw(struct ui_desktop *, enum ui_app, struct ui_rect);  // 以应用身份分派内容绘图，rect已扣除标题栏。
#endif  // 结束完整桌面接口的重复包含保护
```

打开 Files 后，main.lua 会与目录和其他文件一起显示；按名称排序后，它所在的行可能改变。Editor 仍须读取 id=1 的记录，不能把屏幕上的行号直接拿去当文件身份。

<a id="file-indices"></a>

快照下标定位 desktop.files 中的一项，可见行号定位筛选排序后的某一行，服务 id 定位后端记录。ui_file_order 用整数数组连接前两者：若 order[0]=3，第一行画 files[3]；该项的 id 再交给 read。它不移动 files 本身，已有选择仍能以原快照下标表示。

**完整写入 `frontend/core/state.c`：**

<a id="code-frontend-core-state-c-1"></a>

<!-- write: frontend/core/state.c -->
```c
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

```

`ui_init` 现在先把首份文档复制进 editor 和 saved，Editor 因而有了可显示的正文。窗口标题也需要使用读入成功后保存的 `editor_name`：在 Editor 身份片右侧画文档名，裁剪仍避开系统按钮。完整替换 `paint.c`，把这条标题路径接入原有窗口遍历。

**完整写入 `frontend/core/paint.c`：**

<a id="code-frontend-core-paint-c-1"></a>

<!-- write: frontend/core/paint.c -->
```c
#include "internal.h"  // 引入当前目录内部共享接口和基础函数
static const char *titles[] = {"Terminal", "Files",    "Editor",     "Tasks",  // 窗口标题按应用枚举顺序排，应用编号可直接索引
                               "System",   "Settings", "Data viewer"};  // 继续应用枚举4到6的窗口标题，数组顺序与应用id一致
void ui_draw(struct ui_desktop *s, const struct airi_framebuffer *f) {  // 按当前窗口次序绘制整帧
    if (!airi_framebuffer_valid(f))  // 画布地址、尺寸或通道无效时，在写像素之前结束
        return;  // 当前按当前窗口次序绘制整帧在本分支结束，后续语句不执行
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
    u_icon(s->width - 37, 8, UI_TASKS_APP, U_MUTED);  // 顶栏右侧画资源入口图标，当前只绘制图标，资源栏状态将在18章加入
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
    }
    int right = 0;  // 本阶段还没有资源侧栏，窗口可用区不需预留右侧宽度
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
}
```

## 将快照下标转换为可见行

files 数组保存服务返回次序，画面还要排序和筛选。ui_file_order 产生“可见行 → 快照下标”的索引表，保留原数组：先排除不匹配 filter 的项，再插入有序前缀。默认先目录后文件，再按无符号字节比较名字，sort 可以反转方向。

本章六项快照按服务次序为 main.lua、series.csv、README.md、output、sum.c、project.airi，默认排序得到 `[3,2,0,5,1,4]`：先目录 output，再按名字排 README、main、project、series、sum。order[0]=3 指出第零行来自快照第三项，文件 id 仍由服务决定。之后绘图、点击和上下键都使用这个映射。

Files与Editor现在首次参与绘图。先为应用分派加入这两个绘制入口，继续保留Terminal与System的当前接口；点击和对话框尚不进入这份声明。

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
bool ui_app_match(const char *s, const char *q);  // 按字节判断s是否包含q；Terminal识别提示符，Files筛选使用自己的结果
void ui_app_unavailable(struct ui_desktop *s, struct ui_rect r, const char *service);  // 按服务状态绘制加载、空或不可用提示
void ui_app_toolbar(struct ui_rect r, const char *label);  // 在应用内容顶部绘制44像素工具栏
void ui_app_status(struct ui_rect r, const char *left, const char *right);  // 在应用内容底部绘制27像素状态栏
void ui_terminal_draw(struct ui_desktop *, struct ui_rect);  // 把console逻辑行拆成窗口显示行并绘制输入
void ui_files_draw(struct ui_desktop *, struct ui_rect);     // 读取目录快照和排序索引，画路径、文件行和状态
void ui_editor_draw(struct ui_desktop *, struct ui_rect);    // 将文档字节偏移换成行列，画文字、选区和光标
void ui_system_draw(struct ui_desktop *, struct ui_rect);    // 按当前页显示同一启动信息的内存、设备或日志
#endif  // 结束当前应用接口的重复包含保护
```

`ui_file_order(s,order)` 接收桌面和调用者的整数数组，返回数组中有效映射条数。比较函数先比较目录标记，再比较名字；插入排序移动的是这些下标，不搬动文件元信息。

**新建 `frontend/apps/files.c`：**

<a id="code-frontend-apps-files-c-1"></a>

<!-- write: frontend/apps/files.c -->
```c
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

```

索引表完成后，Files 按它取得 ui_file，再扣除 scroll 形成当前可见行。画面依次包含路径、工具栏、筛选、列标题、内容和状态。宽度至少 800 时保留 176 像素侧栏，窄窗口把空间还给列表。

文件名裁剪在本列范围内，避免覆盖大小和类型；缺服务时显示 unavailable，筛选结果为空时显示 No matching files。底部操作目前仅绘制。

**追加到 `frontend/apps/files.c` 末尾：**

<a id="code-frontend-apps-files-c-2"></a>

<!-- append: frontend/apps/files.c -->
```c
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

```

## 将文档字节定位到行列

<a id="cursor-gap"></a>

光标位于字符之间。文字 ABCD 长四字节，却有 0..4 共五个插入位置：0 在 A 前，2 在 B 与 C 之间，4 在 D 后。cursor 保存这个位置，anchor 保存开始选择时的位置，两者界定选区。`[1,3)` 选中 B、C，右端不含位置 3 的 D。

文档位置使用从首字节开始的偏移；屏幕位置使用行列与滚动量。遇到换行时下一字节转到下一行开头，不能把一个字节下标直接当作屏幕 x 坐标。这次遍历完成文字偏移到显示行列的转换，点击时会做反方向查找。

Editor 显示 desktop.editor，与 Files 的选中下标各自独立。idx 记录每行首字节偏移，遇换行后 line 增加；可见行的 y 每行加24像素，每行的 tx 从文字左缘重新扣除水平滚动量。最后的状态栏另用 ln/col 统计光标行列。空行仍占一行，等宽字体让每个 ASCII 字符按固定间距定位。

绘图将 cursor/anchor 中较小者作为选区起点、较大者作为右端。先画选区底色，再画字符，查找栏打开时还要扣掉 44 像素高度。dirty 与 saved 当前由初始化提供，输入算法在下一章接入。

状态栏的 UTF-8 说明只对应 ASCII 与 UTF-8 共用的单字节子集；当前没有多字节光标或中文输入处理。

**新建 `frontend/apps/editor.c`：**

<a id="code-frontend-apps-editor-c-1"></a>

<!-- write: frontend/apps/editor.c -->
```c
#include "internal.h"  // 引入当前目录内部共享接口和基础函数

void ui_editor_draw(struct ui_desktop *s, struct ui_rect r) {  // 将文档字节偏移换成行列，画文字、选区和光标
    ui_app_toolbar(r, "");  // 在内容顶端铺44像素工具栏并显示文字""
    u_button((struct ui_rect){r.x + 12, r.y + 8, 68, 28}, "Save", s->dirty, false);  // 画文字"Save"按钮；其矩形供后续事件命中共用，当前仅输出像素
    u_button((struct ui_rect){r.x + 90, r.y + 8, 68, 28}, "Find", false, false);  // 画文字"Find"按钮；其矩形供后续事件命中共用，当前仅输出像素
    u_button((struct ui_rect){r.x + 168, r.y + 8, 68, 28}, "Undo", false, false);  // 画文字"Undo"按钮；其矩形供后续事件命中共用，当前仅输出像素
    u_button((struct ui_rect){r.x + r.w - 96, r.y + 8, 80, 28}, "Run  F5", true, false);  // 画文字"Run  F5"按钮；其矩形供后续事件命中共用，当前仅输出像素
    ui_app_muted(r.x + 258, r.y + 15, s->dirty ? "Unsaved changes" : "Saved");  // 在内容左缘右移258像素、内容顶边下移15像素显示按当前状态选择的文案
    int tree = r.w >= 800 ? 190 : 0, x = r.x + tree + 1, w = r.w - tree - 2;  // 宽窗口保留190像素项目栏，正文另扣左右边框各1像素
    if (tree) {  // 窗口够宽时为Editor项目文件列表保留190像素
        u_rect(r.x, r.y + 44, tree, r.h - 71, U_BG);  // 用画布背景色写矩形，起点(r.x,r.y + 44)，尺寸tree×r.h - 71像素
        u_label(r.x + 16, r.y + 61, "PROJECT", U_MUTED);  // 在内容左缘右移16像素、内容顶边下移61像素显示项目导航标签PROJECT
        u_text(r.x + 16, r.y + 88, "telemetry", U_LABEL, U_TEXT);  // 当前项目作为导航对象
        u_rect(r.x + 16, r.y + 118, tree - 32, 1, U_EDGE);  // 项目与文件列表共享左缘
        for (unsigned i = 0; i < s->file_count && i < 12; ++i) {  // 只遍历本次目录快照有效前缀，未用槽不参与
            int y = r.y + 132 + (int)i * 30;     // 项目列表第i项按30像素行距排列，原快照次序保持
            if (s->files[i].id == s->editor_id)  // 项目项的服务id等于当前编辑文件id时画选择背景
                u_selection((struct ui_rect){r.x + 8, y - 3, tree - 16, 28});  // 用选中背景色写矩形，起点(r.x + 8,y - 3)，尺寸tree - 16×28像素
            u_icon(r.x + 16, y + 1, s->files[i].directory ? UI_FILES_APP : UI_EDITOR, U_MUTED);  // 文件与目录采用同一套符号
            u_clip((struct ui_rect){r.x + 46, y, tree - 54, 27});  // 把后续文字或像素限制在本次显式矩形内，长内容在边界停止写入
            ui_app_text(r.x + 46, y, s->files[i].name);  // 在内容左缘右移46像素、当前列表行y显示这条快照的文件名
            u_clip(r);  // 恢复当前应用内容裁剪，后续列或控件可使用完整内容区
        }
        u_rect(r.x + tree - 1, r.y + 44, 1, r.h - 71, U_EDGE);  // 用分隔线色写矩形，起点(r.x + tree - 1,r.y + 44)，尺寸1×r.h - 71像素
    }
    u_rect(x, r.y + 44, w, 32, U_PANEL);  // 用工具栏底色写矩形，起点(x,r.y + 44)，尺寸w×32像素
    u_rect(x, r.y + 74, w, 1, U_EDGE);    // 文档条与代码正文的边界
    u_rect(x + 14, r.y + 74, 150, 2, U_ACCENT);     // 用当前主题强调色标记写矩形，起点(x + 14,r.y + 74)，尺寸150×2像素
    ui_app_text(x + 20, r.y + 51, s->editor_name);  // 标签显示当前实际文件名
    if (w > 400)  // 文档条足够宽才显示编码标签，避免覆盖文件名
        u_label(x + w - 140, r.y + 53, "SOURCE / UTF-8", U_MUTED);  // 在文档条右侧显示当前文本范围标签
    u_rect(x + 46, r.y + 85, 1, r.h - 117, U_EDGE);  // 行号与正文的连续边界
    int top = r.y + 88 + (s->find ? 44 : 0);  // 正文起始行扣工具栏和文档标签，查找打开再下移44像素
    if (s->find) {  // 查找栏打开时在正文上方绘制独立短字段
        ui_app_field((struct ui_rect){x + 16, r.y + 85, w - 32, 32},  // 画输入字段和当前焦点边框，本次实参取自当前桌面状态
                     s->find_text[0] ? s->find_text : "Find in document / Enter to select",  // 查找字段有内容就显示它，否则显示回车选择命中项的提示
                     s->focus == UI_FOCUS_FIND);  // 查找输入拥有焦点时画强调边框，文档正文继续保留但不接收同一事件
    }
    u_clip((struct ui_rect){x + 8, top, w - 16, r.h - (top - r.y) - 30});  // 把后续文字或像素限制在本次显式矩形内，长内容在边界停止写入
    const char *p = s->editor;   // 借用当前编辑副本首字节，绘图遍历不改变文字
    unsigned line = 0, idx = 0;  // line计当前逻辑行，idx计当前行首相对文档首字节偏移
    int y = top;  // 首个可见文档行从已计算的正文起点开始绘制
    while ((*p || idx == s->cursor) && y < r.y + r.h - 32) {  // 文档仍有字节或文尾有光标时继续处理行，同时让行起点停在状态栏上方
        const char *end = p;  // 从当前行首向后寻找换行或终止零，p仍保留原行首
        while (*end && *end != '\n')  // 从当前行首向后找换行或文尾，得到本行半开终点
            ++end;  // 扫描位置向后一字节，直至当前文档行末
        if (line++ >= s->editor_scroll) {  // 先按旧行号判断是否已经滚到可见区，再增加逻辑行号供行号显示
            char number[16];  // 16字节局部数组把一起算的文档行号转换成文字
            airi_snprintf(number, 16, "%u", line);  // 绘图行号从1开始，把当前逻辑行号形成16字节短文字
            u_text(x + 12, y, number, U_MONO, U_MUTED);  // 在整屏x=x + 12、整屏y=y显示当前文档行号，使用等宽字形
            u_clip((struct ui_rect){x + 60, top, w - 70, r.h - (top - r.y) - 30});  // 把后续文字或像素限制在本次显式矩形内，长内容在边界停止写入
            int tx = x + 60 - (int)s->editor_column_scroll * 10;  // 正文左边留60像素给行号，水平滚动每列左移10像素
            bool comment = false;  // 每行先处于普通代码状态，双字符前缀才开启注释着色
            char quote = 0;  // 本行尚无打开引号，0表示不在简化字符串着色中
            for (const char *q = p; q < end; ++q) {  // 只遍历当前文档行的内容字节，不把换行画成字形
                unsigned at = idx + (unsigned)(q - p);  // 行首文档偏移加当前字符相对行首距离，得到全篇字节下标
                bool selected = s->selection &&  // 只在有有效选区时继续判断当前字节是否落在两端之间
                                at >= (s->cursor < s->anchor ? s->cursor : s->anchor) &&  // 两选区端点取较小值，当前字节至少等于左端
                                at < (s->cursor > s->anchor ? s->cursor : s->anchor);  // 选区右端排除，大于等于较小端且小于较大端才选中
                if (selected)  // 当前字节落在选区内时先铺选中背景
                    u_rect(tx, y, 10, 23, U_RAISED);  // 用选中背景色写矩形，起点(tx,y)，尺寸10×23像素
                char ch[2] = {*q, 0};  // 当前ASCII字符加NUL形成独立短串，文字绘图不读后面的文档字节
                if (!quote && q + 1 < end &&  // 只有引号外且还有后一字节时才检查双字符注释前缀
                    ((*q == '-' && q[1] == '-') || (*q == '/' && q[1] == '/')))  // 识别Lua双横线和C双斜杠，本次只作行内颜色提示
                    comment = true;  // 后面的同行字符都按注释颜色画，不执行或解析这段代码
                bool opening = false;  // 先记录当前字符尚未新开引号，避免同一引号立即被关闭
                if (!comment && !quote && (*q == '"' || *q == '\'')) {  // 非注释且无打开引号时，当前单或双引号开始字符串着色
                    quote = *q;      // 保存本次打开的单或双引号种类，等待同种非转义引号关闭
                    opening = true;  // 当前引号刚打开，本轮关闭判断必须跳过它
                }
                uint32_t color = comment                    ? U_MUTED  // 优先选注释色，随后再按字符串和数字决定其他字符颜色
                                 : quote                    ? U_BLUE  // 打开引号内的字符采用字符串颜色
                                 : (*q >= '0' && *q <= '9') ? U_PINK  // 注释和引号之外的数字字符采用品牌粉色
                                                            : U_TEXT;  // 其余普通代码字符使用主文字色
                if (quote && !opening && *q == quote && (q == p || q[-1] != '\\'))  // 同种非转义引号且不是本轮刚打开时结束简化字符串着色
                    quote = 0;  // 对应闭引号已遇到，后续字符恢复普通代码着色
                u_text(tx, y, ch, U_CODE, color);  // 在整屏x=tx、整屏y=y显示当前单字节字符与NUL，使用等宽字形
                if (at == s->cursor)  // 当前字符之前的间隙就是光标位置时画一像素竖线
                    u_rect(tx, y + 3, 1, 19, U_BLUE);  // 用当前主题强调色标记写矩形，起点(tx,y + 3)，尺寸1×19像素
                tx += 10;  // 等宽ASCII字符下一列右移10像素
            }
            if (idx + (unsigned)(end - p) == s->cursor)  // 光标在当前行最后字符后面时仍需画文尾竖线
                u_rect(tx, y + 3, 1, 19, U_BLUE);  // 用当前主题强调色标记写矩形，起点(tx,y + 3)，尺寸1×19像素
            y += 24;  // 当前可见文档行完成，下一行下移24像素
            u_clip((struct ui_rect){x + 8, top, w - 16, r.h - (top - r.y) - 30});  // 把后续文字或像素限制在本次显式矩形内，长内容在边界停止写入
        }
        idx += (unsigned)(end - p);  // 当前行的内容长度加入文档字节偏移，换行随后另计
        if (*end) {  // 本行由换行结束而非NUL时，跳过换行进入下一行
            p = end + 1;  // 跳过换行字节后，把下一行第一个字符作为新的行首
            ++idx;  // 将换行字节也计入下一行首的文档偏移
        } else  // 前一分支条件未成立时转入随后的单条替代操作
            break;  // 结束当前最近一层循环或应用分派，不执行其剩余候选
    }
    if (!s->editor[0]) {  // 文档为空时仍在第0间隙画光标并给出输入提示
        u_rect(x + 60, top + 3, 1, 19, U_BLUE);  // 用当前主题强调色标记写矩形，起点(x + 60,top + 3)，尺寸1×19像素
        ui_app_muted(x + 76, top, "Start typing a document.");  // 在整屏x=x + 76、整屏y=top显示文字"Start typing a document."
    }
    u_clip(r);  // 恢复当前应用内容裁剪，后续列或控件可使用完整内容区
    unsigned ln = 1, col = 1;  // 状态栏行列从1开始，内部字节和显示滚动下标仍从0开始
    for (unsigned i = 0; i < s->cursor && s->editor[i]; ++i)  // 扫描光标前的ASCII字节，换行决定行列转换
        if (s->editor[i] == '\n') {  // 扫描到光标前的换行时增加状态行号并复位列号
            ++ln;     // 换行后状态栏的一起算行号增加一
            col = 1;  // 状态栏下一行的首个字符间隙从第1列显示
        } else  // 前一分支条件未成立时转入随后的单条替代操作
            ++col;  // 非换行字符使当前列增加一
    char right[96];  // 96字节局部文字保存光标行列、编码范围和内容长度说明
    airi_snprintf(right, 96, "Ln %u, Col %u   /   UTF-8   /   %u B", ln, col,  // Editor状态栏显示1起算的光标行列与内容字节数；本阶段输入仅支持ASCII子集
                  (unsigned)airi_strlen(s->editor));  // 正文最多8191字节，长度转 unsigned 匹配状态栏 %u
    ui_app_status(r,  // 在应用内容底部绘制27像素状态栏，本次实参取自当前桌面状态
                  s->backend.mode == UI_REFERENCE  // 按实际绑定后端模式选择后续参考说明或内核说明，不修改mode字段
                      ? "Session storage"  // 参考服务页脚说明保存只改变当前主机会话内存
                      : "File service offline / buffer only",  // 未连接真实文件服务时说明当前只有前端编辑缓冲
                  right);  // 把当前光标行列与文档字节数写到状态栏右侧
}

```

把 Files 与 Editor 加入绘图分派。它们使用已经存在的快照和文档状态，其他应用仍只有窗口外框。

**完整替换 `frontend/apps/builtin.c`：**

<a id="code-frontend-apps-builtin-c-1"></a>

<!-- write: frontend/apps/builtin.c -->
```c
#include "internal.h"  // 取得当前已实现的应用绘图与公共文字声明。
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
    case UI_SYSTEM:  // 应用身份为系统信息时进入对应分支。
        ui_system_draw(s, r);  // 用当前桌面和内容矩形绘制系统信息。
        break;  // 当前匹配已完成，停止这一层循环或分支。
    default:  // 未知应用编号不绘制内容；窗口框仍由外层处理
        break;  // 当前匹配已完成，停止这一层循环或分支。
    }
}

```

重新链接观察程序，先以参数 1 生成 Files，再以参数 2 生成 Editor。每次生成后查看同一个输出路径。

**在学习目录根目录运行：**

<!-- run -->
```sh
mkdir -p build  # 保存本次主机程序和图像，已有目录可以继续
cc -std=c11 -O0 -g -Wall -Wextra -Werror -Iinclude -Ifrontend/include labs/tutorial/desktop.c kernel/lib/base.c kernel/boot/info.c kernel/console/console.c frontend/core/*.c frontend/apps/*.c frontend/backends/reference.c frontend/render/draw.c -o build/desktop  # Shell展开两个通配符，把当前已经写出的核心和应用一起链接
./build/desktop  # 运行当前完整观察程序，退出非零表示本组观察失败
./build/desktop 1  # Files从参考服务目录快照绘制排序结果
cp build/desktop.ppm build/files.ppm  # 保存目录图像，再生成编辑器图像
./build/desktop 2  # Editor显示同一会话服务中的main.lua
cp build/desktop.ppm build/editor.ppm  # 保存Editor帧，文字还没有发生输入修改
```

Files 显示排序后的真实快照，Editor 显示服务中的 main.lua 文本。Run 按钮仍仅有外观；修改这份文档需要把按键交给编辑缓冲，再通过服务提交新内容。
