# G15 · 把状态画成终端和系统窗口

文件服务能返回快照，绘图也能写文字。现在在同一个主机程序中建立文本会话和桌面，让 Terminal 显示会话日志、System 显示它借用的启动描述。参考服务提供这份示例启动数据；内核仍沿 K11 的串口路径运行。打开窗口并写出 PPM 后，窗口位置、标题和遮挡才成为可见结果。

本章的一帧从三个已有对象接起来：reference 服务提供文件和示例 boot，console 借用 boot 建立文本会话，desktop 再借用 console 并复制服务回调表。观察程序调用 ui_open 选择窗口，调用 ui_draw 把当前状态画进主机 pixels 数组，最后把数组写成 PPM。打开只改变窗口状态，画布在 ui_draw 时才出现像素；本章没有持续运行的窗口事件循环。

桌面用 windows[app] 保存每个应用的窗口，用 order 保存从底到顶的应用编号，用 active 指出当前活动应用。默认初始化先打开 Terminal；选择 System 的那次观察还会打开并最大化 System，所以桌面同时保留两个打开身份，画面显示最上方的最大化 System。标题栏按钮目前只画外观。

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
    enum ui_state service_state;  // 当前界面服务状态，失败动作通过它显示。
    char directory[128];      // 当前文件操作目录，有NUL终止。
    unsigned selection_file;  // 文件快照数组下标，不是文件id或排序后行。
    struct ui_file files[UI_FILES];  // 当前目录的有界快照记录。
    unsigned file_count;  // files中有效条数，读取不得超过它。
    unsigned theme;  // 桌面自己持有的日夜模式，文本会话不依赖此字段。
};
void ui_init(struct ui_desktop *, struct ui_backend, struct airi_console *);  // 初始化桌面，复制服务表并借用命令对象。
void ui_open(struct ui_desktop *, enum ui_app);  // 打开、解除最小化并置顶指定应用。
void ui_draw(struct ui_desktop *, const struct airi_framebuffer *);  // 按实际帧缓冲绘图，并更新可用于命中的窗口矩形。
void ui_service_state(struct ui_desktop *, enum ui_state);  // 切换服务显示状态并刷新对应快照。
void ui_refresh(struct ui_desktop *);  // 从当前目录取得最多32条文件元信息副本。
void ui_fixture_backend(struct ui_backend *);  // 为主机观察填入会话内存参考服务，不进入guest。

void u_icon(int, int, int, uint32_t);  // 在给定坐标以类型和颜色绘制小图标。
void ui_apps_draw(struct ui_desktop *, enum ui_app, struct ui_rect);  // 以应用身份分派内容绘图，rect已扣除标题栏。
#endif  // 结束完整桌面接口的重复包含保护
```

桌面现在可以保存“Terminal 打开、位于某个矩形”的状态。绘图把这些状态变成可见结果：背景先占满画布，窗口边框覆盖背景，正文再画在边框内部。同一条 console 日志既可送往 UART，也可转成字形画在终端窗口里，保存日志的对象不需要复制一份。

<a id="window-coordinates"></a>

ui_rect 保存左上位置 x/y 和宽高 w/h，单位都是当前软件画布的像素。窗口外框坐标相对整屏；body 是扣除标题栏后得到的内容矩形，应用依据它定位行与按钮。屏幕上的按钮目前只是一组像素，真正点击还要由后面的输入函数按同样几何判断。

**完整写入 `frontend/core/internal.h`：**

<a id="code-frontend-core-internal-h-1"></a>

<!-- write: frontend/core/internal.h -->
```c
#ifndef AIRI_CORE_INTERNAL_H  // 防止桌面内部接口重复包含
#define AIRI_CORE_INTERNAL_H  // 标记桌面内部接口已经包含
#include "ui.h"  // 引入前端服务合同与桌面状态
#include <airi/base.h>  // 引入自有字节操作和有界格式化函数
static inline void copy(char *out, size_t n, const char *text) {  // 按调用者数组容量复制NUL文字
    airi_snprintf(out, n, "%s", text);  // 按调用者容量复制NUL文字；n为0时不写内存，过长文字截到n-1字节
}
#endif  // 结束当前条件编译范围
```

## 让标题、内容与命中采用同一把像素尺

<a id="window-metrics"></a>

窗口的整屏矩形包含48像素标题和一像素边框。若外框为(100,80,800,600)，应用内容从(101,129)开始，宽798、高550；标题下方还有44像素工具栏，底部27像素为状态栏。这几个区域都按同一画布像素计量。u_window_body只从外框算一次内容矩形，绘图现在使用它，鼠标接入后也使用相同结果。

标题的短标签和正文用途不同。u_label逐字符借用G12生成的14像素、字重500字形，每个字符推进后多留2像素；文件名仍用普通推进量，避免整段正文变得松散。

u_label_width累加相同字形advance与每字符两像素额外间距；例如DATA VIEWER比FILES需要更宽的身份片。片宽先加图标、左间距占用的54像素，再为斜切右缘留12像素，短标签至少174；右界仍限制在系统按钮之前。

u_plate以每行减去y/3的宽度形成斜切右缘，三行只缩一像素。u_brand用已有整数线段与矩形组成A形标识，整段绘图继续受到当前裁剪范围限制。

在 render.h 中定位原来只有 UI_CONTROL_RADIUS 的 ui_metric 声明，用第一个新块替换它，加入本章实际使用的窗口度量与标签函数声明。旧块用于定位；像素、字体、颜色和状态接口继续保留。

<a id="code-frontend-include-render-h-1"></a>

<!-- replace: frontend/include/render.h -->
<!-- before -->
```c
enum ui_metric { UI_CONTROL_RADIUS = 2 };  // 本章只需要按钮的2像素圆角，不预置窗口尺寸
```
<!-- after -->
```c
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
```

再定位文件末尾的 endif，替换成正文矩形与选择标记函数，并保留结尾包含保护。两个函数都复用当前绘图接口。

<!-- replace: frontend/include/render.h -->
<!-- before -->
```c
#endif  // 结束包含保护
```
<!-- after -->
```c
static inline struct ui_rect u_window_body(struct ui_rect r) {  // 由外框得到标题下方的应用矩形
    return (struct ui_rect){r.x + 1, r.y + UI_TITLE_H + 1, r.w - 2, r.h - UI_TITLE_H - 2};  // 左右各去一像素，顶部去48像素标题及一像素边框
}
static inline void u_selection(struct ui_rect r) {  // 列表选择共用底色和左侧形状标记
    u_rect(r.x, r.y, r.w, r.h, U_RAISED);  // 先铺整个当前选择区域，文字随后画在其上
    u_rect(r.x, r.y + 4, 3, r.h - 8, U_ACCENT);  // 三像素侧标距上下各四像素，颜色变化同时伴随形状
}
#endif  // 结束包含保护
```

**追加到 `frontend/render/draw.c` 末尾：**

<a id="code-frontend-render-draw-c-1"></a>

<!-- append: frontend/render/draw.c -->
```c

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
```

这几项辅助函数还没有建立窗口。后面的 ui_draw 会先用 u_label_width 测身份文字，算出片宽，再依次画 u_plate、图标和 u_label；测量与绘制用相同的字形推进量。接下来准备窗口内容要读取的启动与文件状态。

## 让文本会话和 System 借用同一份启动描述

Terminal 的日志与 System 的内存表需要同一个启动来源。参考服务在静态 `boot` 中建立四条示例内存区域，再把 `&boot` 放进回调表；它和文件槽都活到进程结束。主机画布另由观察程序拥有，示例 `boot` 的帧缓冲字段仍为零，所以 System 会说明当前是窗口会话。

reference.c 此次增加长期有效的 boot 对象和初始化字段。下面仍按本检查点完整内容写入；已有文件查询、读取和创建合同保持 G14 的含义。先写到 action 末尾，再追加初始化。

<a id="code-frontend-backends-reference-c-1"></a>

<!-- write: frontend/backends/reference.c -->
```c
#include "ui.h"  // 引入前端服务合同与桌面状态
#include <airi/base.h>  // 引入自有字节操作和有界格式化函数
/* This provider is linked only into native design tools, never the kernel. */
struct reference_file {  // 内存服务拥有的一条元信息、正文和占用状态
    struct ui_file entry;    // 服务记录内嵌的可复制文件元信息
    char text[UI_DOC_SIZE];  // 服务拥有的文档内容数组
    bool used;  // 服务槽是否占用
};
static struct reference_file entries[UI_FILES];  // 服务拥有32个静态文件槽，跨查询保留而退出进程丢失
static AiriBootInfo boot;  // 参考服务持有示例启动结构，借给System与console使用
static void add(unsigned slot, const char *path, const char *name, bool directory,  // 把路径、名称和文字填进指定内存服务槽
                const char *body) {  // body为新服务记录的NUL正文，目录可传空串
    struct reference_file *f = &entries[slot];  // 借用准备填充的服务槽，add的调用者已选择有效下标
    f->used = true;  // 占用选定服务槽，之后目录查询可见此记录
    f->entry.id = (int)slot + 1;     // 槽号从0开始，服务身份从1开始，0保留为无文件
    f->entry.directory = directory;  // 记录新条目是目录还是普通文字文件
    airi_snprintf(f->entry.name, 64, "%s", name);  // 复制单项名称到服务元信息，64字节数组最多容纳63内容字节与NUL
    airi_snprintf(f->entry.path, 128, "%s/%s", path, name);  // 把父目录、斜杠与单项名称组合成服务完整路径，容量128字节
    airi_snprintf(f->text, sizeof(f->text), "%s", body);  // 复制初始正文到服务拥有的8192字节数组，后续read从此处取得内容
    f->entry.bytes = (unsigned)airi_strlen(body);  // 保存内容长度，不把终止零计入文件字节数
}
static unsigned files(void *unused, const char *path, struct ui_file *out, unsigned cap) {  // 将当前目录直接子项元信息复制给调用者快照
    (void)unused;    // 此参考服务使用静态表，回调合同的上下文参数本实现不读取
    unsigned n = 0;  // 本次已复制给调用者的文件条数从0开始，仅此计数之前的out槽有效
    size_t length = airi_strlen(path);  // 当前查询目录的内容字节长度，不含NUL；它决定父路径前缀的比较范围
    for (unsigned i = 0; i < UI_FILES && n < cap; ++i) {  // 遍历最多32个服务槽，输出有效条数达到cap时停止复制
        struct reference_file *f = &entries[i];  // 借用当前服务槽，检查占用与路径后再复制元信息
        if (!f->used)  // 空槽不参与当前目录查询
            continue;  // 本项不参与当前结果，跳过本轮剩余操作进入下一项
        if (!airi_memcmp(f->entry.path, path, length) && f->entry.path[length] == '/') {  // 先比较完整父目录前缀，再要求下一字节是斜杠，避免projects误命中projects2
            const char *rest = f->entry.path + length + 1;  // 跳过父目录的length字节和一个斜杠，rest借用子项名称开头
            bool direct = true;    // 先把路径余部视为当前目录直接子项，遇斜杠再否定
            for (; *rest; ++rest)  // 只在父目录之后的路径余部查斜杠，判断是否直接子项
                if (*rest == '/') {  // 子项名称中又有斜杠说明它在更深目录，本次只列直接子项
                    direct = false;  // 路径余部还有层级分隔，条目位于更深目录而非直接子项
                    break;  // 此路径含子目录分隔符，结束当前斜杠查找循环
                }
            if (direct)  // 仅直接子项进入本次目录查询结果
                out[n++] = f->entry;  // 按旧下标复制整条元信息，再增加返回条数；服务记录未搬动
        }
    }
    return n;  // 返回本次已经复制到调用者数组的有效记录条数
}
static bool read(void *unused, int id, char *out, size_t cap) {  // 按服务id复制完整文档到调用者数组
    (void)unused;  // 此参考服务使用静态表，回调合同的上下文参数本实现不读取
    if (id < 1 || id > UI_FILES || !entries[id - 1].used || entries[id - 1].entry.directory)  // 先用短路或保护id-1下标，再要求槽已占用且不是目录，失败不写输出
        return false;  // id无效或指向目录，未复制任何文档内容
    size_t length = airi_strlen(entries[id - 1].text);  // 取得服务正文内容字节数，不含NUL
    if (length >= cap)  // 完整正文及NUL装不下时失败，输出数组保留原内容
        return false;  // 容量不足不能把截断文本当成完整读取成功
    airi_memcpy(out, entries[id - 1].text, length + 1);  // 内容连NUL一起复制，输出由调用者独立拥有
    return true;  // 完整正文连NUL已写入调用者数组，服务自身正文未改变
}
static bool error(char *out, size_t cap, const char *message) {  // 把失败原因写入调用者错误缓冲并返回false
    airi_snprintf(out, cap, "%s", message);  // 将具体失败原因复制到调用者错误数组，cap包含终止零的位置
    return false;  // 失败原因已写到调用者数组，false表示服务记录没有提交本次修改
}
static bool valid_name(const char *name) {  // 检查文件名非空、长度与禁止路径分隔符
    size_t n = airi_strlen(name);  // 名称内容字节数用于空名与63字节上限检查，不计NUL
    if (!n || n >= 64 || (n == 1 && name[0] == '.') || (n == 2 && name[0] == '.' && name[1] == '.'))  // 拒绝空名、超过63字节以及特殊目录名.和..
        return false;  // 名称含非法形状或字节，阻止后续服务修改
    for (; *name; ++name)  // 逐个验证名称字节，不把终止零当内容
        if (*name == '/' || *name == '\\' || (unsigned char)*name < 32)  // 名称不能含路径分隔符或ASCII控制字节，避免把单个名称当路径
            return false;  // 名称含非法形状或字节，阻止后续服务修改
    return true;  // 所有名称约束均通过，可继续生成目标路径
}
static bool action(void *unused, const char *directory, enum ui_action act, int id,  // 校验请求后改变服务记录，错误留在输出缓冲
                   const char *name, const char *data, char *out, size_t cap) {  // name和data借用请求文字，out与cap由调用者提供来接收本次错误原因
    (void)unused;  // 此参考服务使用静态表，回调合同的上下文参数本实现不读取
    (void)id;      // 此阶段只创建新记录，接口保留的现有文件id参数尚不使用
    (void)data;    // 此阶段只创建空记录，接口保留的文字参数尚不使用
    if (act != UI_CREATE_FILE && act != UI_CREATE_DIR)  // 本阶段仅支持创建，其他操作在占用任何服务槽之前报告不可用
        return error(out, cap, "This checkpoint only creates files and directories.");  // 拒绝本次服务修改并向调用者报告：This checkpoint only creates files and directories
    if (!valid_name(name))  // 先检查名称形状，失败时不会拼路径或占用服务槽
        return error(out, cap, "Use a non-empty file name without slashes, up to 63 bytes.");  // 拒绝本次服务修改并向调用者报告：Use a non-empty file name without slashes, up to 63 bytes
    char target[128];  // 先在局部数组形成完整目标路径，通过校验后才改服务
    int length = airi_snprintf(target, sizeof(target), "%s/%s", directory, name);  // 先拼父目录、斜杠和名称；返回所需字节数供完整路径容量检查
    if (length >= (int)sizeof(target))  // 完整路径所需长度达到128时没有NUL位置，拒绝创建
        return error(out, cap, "Target path is too long.");  // 拒绝本次服务修改并向调用者报告：Target path is too long
    for (unsigned i = 0; i < UI_FILES; ++i)  // 逐个检查服务32槽，查询或创建受当前占用状态约束
        if (entries[i].used && !airi_memcmp(entries[i].entry.path, target, (size_t)length + 1))  // 只比较已占用槽的完整目标路径，长度加1连NUL一起匹配，重名不覆盖原记录
            return error(out, cap, "An item with that name already exists.");  // 拒绝本次服务修改并向调用者报告：An item with that name already exists
    for (unsigned i = 0; i < UI_FILES; ++i)  // 逐个检查服务32槽，查询或创建受当前占用状态约束
        if (!entries[i].used) {  // 找到空服务槽后才能新建记录
            add(i, directory, name, act == UI_CREATE_DIR, "");  // 填当前空服务槽，操作种类决定是否目录，初始正文为空
            return true;  // 该服务修改已完整提交，前端可重新查询当前记录
        }
    return error(out, cap, "Reference workspace capacity reached.");  // 拒绝本次服务修改并向调用者报告：Reference workspace capacity reached
}
```

追加初始化函数。前半段继续建立同一批服务文件；后半段填写 boot 的合同字段及四条区域。最后把 &boot 放入 backend.boot，console 和 System 后面都会沿这一指针读取它。这里没有为 boot.framebuffer 填入画布：主机 pixels 的地址将在观察程序中单独交给 ui_draw。

<!-- append: frontend/backends/reference.c -->
```c
void ui_fixture_backend(struct ui_backend *b) {  // 重置当前主机参考服务并填入回调表
    airi_memset(entries, 0, sizeof(entries));  // 把服务拥有的32个槽清空，再填本会话初始参考文件
    add(0, "/home/airi/projects/telemetry", "main.lua", false,  // 向明确编号的服务槽填这份路径、名称、目录标记与正文
        "-- Read a series and write a summary\nlocal values = {12, 18, 21, 32, 29, 37}\n\nlocal "  // 继续当前固定文字；转义换行属于文档内容，C相邻字符串在编译时连接
        "sum = 0\nfor _, value in ipairs(values) do\n    sum = sum + value\nend\n\nlocal mean = "  // 继续当前固定文字；转义换行属于文档内容，C相邻字符串在编译时连接
        "sum / #values\nprint(string.format(\"mean: %.2f\", mean))\n\n-- Run requires the future "  // 继续当前固定文字；转义换行属于文档内容，C相邻字符串在编译时连接
        "application service.\n-- This reference workspace only edits the source.\n");  // 提供本次调用的完整文字输入，C相邻字符串会在编译时连接
    add(1, "/home/airi/projects/telemetry", "series.csv", false,  // 向明确编号的服务槽填这份路径、名称、目录标记与正文
        "seconds,load\n0,12\n1,18\n2,21\n3,32\n4,29\n5,37\n");  // 提供本次调用的完整文字输入，C相邻字符串会在编译时连接
    add(2, "/home/airi/projects/telemetry", "README.md", false,  // 向明确编号的服务槽填这份路径、名称、目录标记与正文
        "# Telemetry workspace\n\nThis is a frontend reference dataset.\nSave changes update this "  // 继续当前固定文字；转义换行属于文档内容，C相邻字符串在编译时连接
        "session's memory only.\nNative application execution is intentionally unconnected.\n");  // 提供本次调用的完整文字输入，C相邻字符串会在编译时连接
    add(3, "/home/airi/projects/telemetry", "output", true, "");  // 向明确编号的服务槽填这份路径、名称、目录标记与正文
    add(6, "/home/airi/projects/telemetry", "sum.c", false,  // 向明确编号的服务槽填这份路径、名称、目录标记与正文
        "#include <stdio.h>\n\nint main(void) {\n    const int values[] = {12, 18, 21, 32};\n    "  // 继续当前固定文字；转义换行属于文档内容，C相邻字符串在编译时连接
        "int total = 0;\n    for (unsigned i = 0; i < 4; ++i)\n        total += values[i];\n    "  // 继续当前固定文字；转义换行属于文档内容，C相邻字符串在编译时连接
        "printf(\"sum: %d\\n\", total);\n    return 0;\n}\n");  // 提供本次调用的完整文字输入，C相邻字符串会在编译时连接
    add(7, "/home/airi/projects/telemetry", "project.airi", false,  // 向明确编号的服务槽填这份路径、名称、目录标记与正文
        "[project]\nname = telemetry\nentry = main.lua\n");  // 提供本次调用的完整文字输入，C相邻字符串会在编译时连接
    add(4, "/home/airi/projects", "telemetry", true, "");  // 向明确编号的服务槽填这份路径、名称、目录标记与正文
    add(5, "/home/airi", "projects", true, "");      // 向明确编号的服务槽填这份路径、名称、目录标记与正文
    boot = (AiriBootInfo){.magic = AIRI_BOOT_MAGIC,  // 为主机窗口准备固定参考启动结构，System明确标成示例数据
                          .version = AIRI_BOOT_VERSION,  // 设置启动合同版本
                          .size = sizeof(boot),  // 设置本版启动结构字节数
                          .direct_map_offset = UINT64_C(0xffff800000000000),  // 设置参考直接映射偏移
                          .kernel_physical_base = 0x100000,  // 设置参考内核物理起点
                          .kernel_virtual_base = UINT64_C(0xffffffff80000000),  // 设置参考内核虚拟起点
                          .kernel_size = 132 * 1024,  // 设置参考内核镜像长度
                          .region_count = 4};  // 设置参考内存区域有效条数
    airi_snprintf(boot.loader, 32, "Reference / UEFI");  // 写明这份启动结构属于主机参考数据，System据此区别实际guest启动
    boot.regions[0] =  // 参考地图第0项描述最低1MiB保留范围，下一行填完整记录
        (struct airi_memory_region){.base = 0, .length = 0x100000, .kind = AIRI_MEMORY_RESERVED};  // 复合字面量按字段名给起点0、长度1MiB和保留类别
    boot.regions[1] = (struct airi_memory_region){  // 参考地图第1项填写内核占用范围
        .base = 0x100000, .length = 0x100000, .kind = AIRI_MEMORY_KERNEL};  // 设置参考物理范围起点、参考物理范围字节长度、参考内存用途分类
    boot.regions[2] = (struct airi_memory_region){  // 参考地图第2项填写当前保留的引导可回收范围
        .base = 0x200000, .length = 0x2900000, .kind = AIRI_MEMORY_BOOT_RECLAIM};  // 设置参考物理范围起点、参考物理范围字节长度、参考内存用途分类
    boot.regions[3] = (struct airi_memory_region){  // 参考地图第3项填写可用物理范围
        .base = 0x2b00000, .length = 0xd500000, .kind = AIRI_MEMORY_USABLE};  // 设置参考物理范围起点、参考物理范围字节长度、参考内存用途分类
    *b = (struct ui_backend){.mode = UI_REFERENCE, .boot = &boot, .files = files, .read = read, .action = action};  // 通过b写调用者的整份回调表，函数地址交给files/read/action，未指定context字段自动为NULL
}
```

## 初始化桌面并打开窗口

桌面建立后也持有一份文件查询副本。本章观察程序重新初始化服务，所以首次刷新仍得到原来的六条；G14 中前一次进程创建的 hello.txt 不会带进来。以后服务改变记录，也须再次查询才能更新 desktop.files。file_count 记录本次有效条数，32 记录本地数组容量，遍历不能把二者混用。

K11 的文本解释器保留一个可选扩展回调，将内置分支未识别的命令交给它。桌面在 `ui_init` 中把 `appearance` 和当前桌面地址安装到 extension 与 extension_context，`theme day`、`theme night` 和查询 `theme` 因而交给图形层处理；未识别文字返回 false，由文本解释器生成未知命令错误。

`context` 原样带回当前桌面指针，回调只修改这份桌面的主题，不保留命令副本。平台须保证桌面和 console 在输入期间都存活；一个 console 同时只绑定一个呈现消费者。

**完整写入 `frontend/core/state.c`：**

<a id="code-frontend-core-state-c-1"></a>

<!-- write: frontend/core/state.c -->
```c
#include "internal.h"  // 取得核心内部的桌面类型、有限copy与基础库接口。
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
```

ui_refresh 先清文件快照计数，服务状态非正常便返回，旧成功数据不继续显示。正常时调用现有回调，并把条数限在本地容量内。选中下标超出新快照则归零。本次实际调用把 s->directory 作为输入，把 s->files 和 UI_FILES 作为输出数组及容量，返回值写进 s->file_count。追加刷新函数。

<!-- append: frontend/core/state.c -->
```c
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

```

ui_init 接收观察程序准备的 backend 和 console。清零后，UI_NORMAL 与 Night 都对应零初值；active 则显式设为 -1，表示尚无活动窗口。随后保存 1280×800 的初始布局尺寸、设置查询目录、取得六条元信息，最后调用 ui_open(s, UI_TERMINAL)。此时没有编辑文字数组，也还没向画布写像素。追加初始化。

<!-- append: frontend/core/state.c -->
```c
void ui_init(struct ui_desktop *s, struct ui_backend b, struct airi_console *c) {  // 清桌面状态，绑定服务和console并打开默认应用
    airi_memset(s, 0, sizeof(*s));  // 清零整个当前状态或快照，使计数和标记从确定初值开始
    s->backend = b;  // 把回调表和上下文复制进桌面，服务数据仍归服务拥有
    c->extension = appearance;  // 安装同步命令扩展；console 不包含主题或窗口类型。
    c->extension_context = s;   // 桌面和 console 都由平台持有，须覆盖后续输入调用。
    s->console = c;   // 桌面借用同一console状态，Terminal不另复制日志
    s->active = -1;   // 没有活动窗口，-1哨兵不能拿去索引应用数组
    s->width = 1280;  // 绘图前暂用1280像素默认画布宽度
    s->height = 800;  // 绘图前暂用800像素默认画布高度
    copy(s->directory, sizeof(s->directory), "/home/airi/projects/telemetry");  // 把文字"/home/airi/projects/telemetry"复制到当前目录路径，sizeof(s->directory)是包含NUL的数组容量
    ui_refresh(s);  // 首次查询当前目录，目录有效条数和选择下标由ui_refresh更新
    ui_open(s, UI_TERMINAL);  // 打开或置顶一个应用，更新窗口次序和活动编号，本次实参取自当前桌面状态
}

```

接着实现 ui_open。首次打开 Terminal 时，order_count 为 0，窗口从 (92,72) 开始建立初始 rect，并复制到 restore；open 变为 true，active 变为 0，order 的有效前缀变成 [0]。之后打开 System，会用已有数量形成 24 像素错位，次序变成 [0,4]、active=4。若再次打开已有应用，它保留 rect，取消最小化，从旧 order 中去重后追加到末尾；这就是置顶，不需要重建日志或启动对象。

<!-- append: frontend/core/state.c -->
```c
void ui_open(struct ui_desktop *s, enum ui_app app) {  // 打开或置顶一个应用，更新窗口次序和活动编号
    if (app >= UI_APP_COUNT)  // 应用编号超出窗口数组范围时拒绝打开
        return;  // 应用编号未通过范围检查，不访问窗口数组或改变次序
    struct ui_window *w = &s->windows[app];  // 借用当前应用的窗口状态，app已经通过范围检查
    if (!w->open) {  // 只在首次打开或关闭后重新设置窗口初始几何
        int offset = (int)s->order_count * 24;  // 每个已排入次序的窗口错开24像素，新窗口避免完全重合
        w->rect = (struct ui_rect){92 + offset, 72 + offset, s->width - 152, s->height - 144};  // 新窗口从屏幕左上留92和72像素并按已有数量错开，宽高先扣固定边距
        if (w->rect.w < 720)  // 初始窗口宽度至少720像素，画布尺寸后续再约束
            w->rect.w = 720;  // 首次打开宽度至少720像素；真正绘制时再按画布收窄
        if (w->rect.h < 480)  // 初始窗口高度至少480像素，画布尺寸后续再约束
            w->rect.h = 480;  // 首次打开高度至少480像素；真正绘制时再按画布收窄
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
}

```

最后追加服务状态入口。它先记录新状态，再走同一个 ui_refresh；因此切到非正常状态后，有效文件条数成为 0，后面的不可用提示不会继续显示旧的成功快照。

<!-- append: frontend/core/state.c -->
```c
void ui_service_state(struct ui_desktop *s, enum ui_state state) {  // 切换服务显示状态并刷新相应快照
    s->service_state = state;  // 用调用者指定状态切换服务反馈，随后刷新决定是否继续查询快照
    ui_refresh(s);  // 按新服务状态重新查询；非正常状态留下0条有效元信息
}

```

## 应用共用的文字与控件

应用内部头文件声明文字、字段和状态栏辅助函数，它们随即供这两个视图使用。这些声明只对应当前已有的绘图函数，点击函数在实际接入时再加入。

**新建 `frontend/apps/internal.h`：**

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
bool ui_app_match(const char *s, const char *q);  // 按字节判断s是否包含q；当前Terminal据返回值识别提示符片段
void ui_app_unavailable(struct ui_desktop *s, struct ui_rect r, const char *service);  // 按服务状态绘制加载、空或不可用提示
void ui_app_toolbar(struct ui_rect r, const char *label);  // 在应用内容顶部绘制44像素工具栏
void ui_app_status(struct ui_rect r, const char *left, const char *right);  // 在应用内容底部绘制27像素状态栏
void ui_terminal_draw(struct ui_desktop *, struct ui_rect);  // 把console逻辑行拆成窗口显示行并绘制输入
void ui_system_draw(struct ui_desktop *, struct ui_rect);    // 按当前页显示同一启动信息的内存、设备或日志
#endif  // 结束当前应用接口的重复包含保护
```

`ui_app_text(x,y,s)` 使用正文体和主要颜色，`ui_app_muted(x,y,s)` 使用小字和次要颜色；x/y 都是整屏坐标。`ui_app_field(r,s,focused)` 先画底色与边框再画内容。`ui_app_toolbar(r,label)` 从 body 顶部留出工具栏，`ui_app_status(r,left,right)` 从底部留出状态栏，right 传 NULL 时省去右侧文字。字段的focused只影响蓝色边界，当前没有键盘字段输入；短文字匹配随即用于识别终端提示符。

先写文字与字段外观，再补日志匹配、服务状态提示与应用图标。

**新建 `frontend/apps/common.c`：**

<a id="code-frontend-apps-common-c-1"></a>

<!-- write: frontend/apps/common.c -->
```c
#include "internal.h"  // 取得本章已经实现的应用绘图与公共文字接口。

void ui_app_text(int x, int y, const char *s) {  // 用正文体和主要颜色绘制应用文字
    u_text(x, y, s, U_BODY, U_TEXT);  // 在整屏x=x、整屏y=y显示s
}

void ui_app_muted(int x, int y, const char *s) {  // 用小字体和次要颜色绘制说明文字
    u_text(x, y, s, U_SMALL, U_MUTED);  // 在整屏x=x、整屏y=y显示s
}

void ui_app_field(struct ui_rect r, const char *s, bool focused) {  // 画输入字段和当前焦点边框
    u_round(r, UI_CONTROL_RADIUS, focused ? U_BLUE : U_CONTROL);  // 输入框始终可识别
    u_round((struct ui_rect){r.x + 1, r.y + 1, r.w - 2, r.h - 2}, UI_CONTROL_RADIUS - 1, U_SURFACE);  // 内容填充
    if (focused) u_rect(r.x + 4, r.y + r.h - 2, r.w - 8, 2, U_BLUE);  // 加粗底边区分键盘焦点
    ui_app_text(r.x + 10, r.y + 7, s);  // 在内容左缘右移10像素、内容顶边下移7像素显示s
}

```

字符串包含判断ui_app_match先供Terminal识别日志片段中的airi>提示符。它在输入字符串的各起点找子串；每次先保证剩余长度足够，再 memcmp。空查询匹配全部，当前按字节比较，名称完全相等的服务检查则使用另一条规则。

**追加到 `frontend/apps/common.c` 末尾：**

<a id="code-frontend-apps-common-c-2"></a>

<!-- append: frontend/apps/common.c -->
```c
bool ui_app_match(const char *s, const char *q) {  // 按字节判断字符串s是否包含子串q，不修改任一借用字符串
    size_t n = airi_strlen(q);  // 子串q的内容字节数，决定每个候选起点需要比较的长度
    if (!n)  // 空子串按约定总是匹配，无需扫描字符串s
        return true;  // 空子串已满足包含关系，调用方如何使用true由当前场景决定
    for (; *s; ++s)  // 沿当前NUL字符串逐字节推进，不读取终止零之后
        if (airi_strlen(s) >= n && !airi_memcmp(s, q, n))  // 剩余字节足够才比较n字节，memcmp为0表示找到子串
            return true;  // 当前位置找到子串q，返回true并停止继续扫描
    return false;  // 字符串s的所有起点都没有匹配子串q，返回false
}

```

应用遇到 loading、error、offline 或 empty 时，用 unavailable 给出对应文字。缺少服务与目录为空分别显示，Retry 此时只绘制像素。

**追加到 `frontend/apps/common.c` 末尾：**

<a id="code-frontend-apps-common-c-3"></a>

<!-- append: frontend/apps/common.c -->
```c
void ui_app_unavailable(struct ui_desktop *s, struct ui_rect r, const char *service) {  // 按服务状态绘制加载、空或不可用提示
    const char *title = s->service_state == UI_LOADING ? "Loading data"  // 按当前服务状态选择不可用提示标题，先处理正在加载
                        : s->service_state == UI_ERROR ? "Unable to load data"  // 加载失败选择明确错误标题，正常正文不会借此假装已经取得数据
                        : s->service_state == UI_EMPTY ? "No records"  // 查询结果为空时选择无记录标题，和缺少服务区分
                                                       : "Unavailable";  // 其余未正常状态用明确未连接标题，避免画出成功快照
    u_icon(r.x + 36, r.y + 80, UI_SYSTEM, U_MUTED);      // 在提示区域左侧画系统图标，旁边文字说明具体缺失的服务
    u_text(r.x + 72, r.y + 74, title, U_LABEL, U_TEXT);  // 在内容左缘右移72像素、内容顶边下移74像素显示按状态选定的标题
    char line[128];  // 128字节局部数组保存当前服务身份说明，供次要文字绘图
    airi_snprintf(line, 128, "%s", service);  // 把当前服务标识复制为本次提示第二行，128字节局部数组供后面的文字绘图
    ui_app_muted(r.x + 72, r.y + 108, line);  // 在提示图标右侧显示service的有界副本，说明具体未接入的服务
    ui_app_muted(r.x + 72, r.y + 137,  // 用小字体和次要颜色绘制说明文字，本次实参取自当前桌面状态
                 s->service_state == UI_LOADING ? "Waiting for the provider to return a snapshot."  // 加载中说明等待提供者快照，尚无可绘制新数据
                 : s->service_state == UI_ERROR  // 错误状态的正文使用失败说明，其他情况继续选择离线说明
                     ? "The operation failed. Retry to refresh this view."  // 错误状态说明当前请求失败，重试将重新查询
                     : "No data is available for this view.");  // 尚未连接状态说明这个视图依赖的服务接口
    if (s->service_state == UI_ERROR)  // 只有操作失败状态需要显示Retry按钮
        u_button((struct ui_rect){r.x + 72, r.y + 179, 92, 32}, "Retry", true, false);  // 画文字"Retry"按钮；其矩形供后续事件命中共用，当前仅输出像素
}

void ui_app_toolbar(struct ui_rect r, const char *label) {  // 在应用内容顶部绘制44像素工具栏
    u_rect(r.x, r.y, r.w, UI_TOOLBAR_H, U_PANEL);  // 用工具栏底色写矩形，起点(r.x,r.y)，尺寸r.w×44像素
    u_rect(r.x, r.y + UI_TOOLBAR_H - 1, r.w, 1, U_EDGE);  // 用分隔线色写矩形，起点(r.x,r.y + UI_TOOLBAR_H - 1)，尺寸r.w×1像素
    ui_app_text(r.x + 16, r.y + 12, label);  // 在工具栏左上绘制调用者传入的label；Terminal传入当前后端标题
}

void ui_app_status(struct ui_rect r, const char *left, const char *right) {  // 在应用内容底部绘制27像素状态栏
    u_rect(r.x, r.y + r.h - UI_STATUS_H, r.w, UI_STATUS_H, U_PANEL);  // 用工具栏底色写矩形，起点(r.x,r.y + r.h - UI_STATUS_H)，尺寸r.w×27像素
    u_rect(r.x, r.y + r.h - UI_STATUS_H, r.w, 1, U_EDGE);  // 用分隔线色写矩形，起点(r.x,r.y + r.h - UI_STATUS_H)，尺寸r.w×1像素
    ui_app_muted(r.x + 14, r.y + r.h - 21, left);  // 在内容左缘右移14像素、内容底边上移21像素显示left
    if (right)  // 调用方提供右侧文字时才测量和绘制；NULL 表示这一侧没有文字
        ui_app_muted(r.x + r.w - u_text_width(right, U_SMALL) - 16, r.y + r.h - 21, right);  // 用同一小字体测right的像素宽度，让文字右边距保持16像素；right借用调用者状态说明
}

```

应用图标按 id 选择线段和矩形，使用已完成图元。每个图标表示应用身份，实际打开窗口由桌面状态操作完成。

**追加到 `frontend/apps/common.c` 末尾：**

<a id="code-frontend-apps-common-c-4"></a>

<!-- append: frontend/apps/common.c -->
```c
void u_icon(int x, int y, int id, uint32_t c) {  // 按应用编号绘制20像素附近的几何图标
    /* Original, simple geometric symbols. Icons remain recognisable at 20 px. */
    switch (id) {      // 按应用编号选择几何图标，不由像素触发应用动作
    case UI_TERMINAL:  // 按Terminal应用和windows数组下标0进入对应分支
        u_frame((struct ui_rect){x, y + 2, 20, 16}, c);  // 沿当前显式矩形的四边画一像素边框
        u_line(x + 4, y + 6, x + 7, y + 9, c);      // 用传入图标色连接像素端点(x + 4,y + 6)到(x + 7,y + 9)
        u_line(x + 7, y + 9, x + 4, y + 12, c);     // 用传入图标色连接像素端点(x + 7,y + 9)到(x + 4,y + 12)
        u_line(x + 10, y + 13, x + 15, y + 13, c);  // 用传入图标色连接像素端点(x + 10,y + 13)到(x + 15,y + 13)
        break;  // 当前应用图标已经画完，退出图标种类switch
    case UI_FILES_APP:  // 按Files应用和windows数组下标1进入对应分支
        u_line(x, y + 5, x + 8, y + 5, c);  // 用传入图标色连接像素端点(x,y + 5)到(x + 8,y + 5)
        u_line(x + 8, y + 5, x + 10, y + 8, c);   // 用传入图标色连接像素端点(x + 8,y + 5)到(x + 10,y + 8)
        u_line(x + 10, y + 8, x + 20, y + 8, c);  // 用传入图标色连接像素端点(x + 10,y + 8)到(x + 20,y + 8)
        u_frame((struct ui_rect){x, y + 8, 21, 11}, c);  // 沿当前显式矩形的四边画一像素边框
        break;  // 当前应用图标已经画完，退出图标种类switch
    case UI_EDITOR:  // 按Editor应用和windows数组下标2进入对应分支
        u_frame((struct ui_rect){x + 3, y, 15, 21}, c);  // 沿当前显式矩形的四边画一像素边框
        for (int r = 5; r < 18; r += 4)  // 每4像素画一条Editor图标内的文字短横线
            u_line(x + 6, y + r, x + 14, y + r, c);  // 用传入图标色连接像素端点(x + 6,y + r)到(x + 14,y + r)
        break;  // 当前应用图标已经画完，退出图标种类switch
    case UI_TASKS_APP:  // 按Tasks应用和windows数组下标3进入对应分支
        for (int i = 0; i < 4; ++i)  // 画Tasks图标的4根柱，每柱右移5像素、高度依次增加4像素
            u_rect(x + i * 5, y + 18 - i * 4, 3, i * 4 + 3, c);  // 用传入图标色写矩形，起点(x + i * 5,y + 18 - i * 4)，尺寸3×i * 4 + 3像素
        break;  // 当前应用图标已经画完，退出图标种类switch
    case UI_SYSTEM:  // 按System应用和windows数组下标4进入对应分支
        u_frame((struct ui_rect){x + 2, y + 2, 17, 13}, c);  // 沿当前显式矩形的四边画一像素边框
        u_line(x + 10, y + 15, x + 10, y + 19, c);  // 用传入图标色连接像素端点(x + 10,y + 15)到(x + 10,y + 19)
        u_line(x + 5, y + 19, x + 15, y + 19, c);   // 用传入图标色连接像素端点(x + 5,y + 19)到(x + 15,y + 19)
        break;  // 当前应用图标已经画完，退出图标种类switch
    case UI_SETTINGS:  // 按Settings应用和windows数组下标5进入对应分支
        for (int i = 0; i < 3; ++i) {  // Settings图标按三条滑轨生成标记，逐项下移6像素
            u_line(x, y + 4 + i * 6, x + 20, y + 4 + i * 6, c);  // 用传入图标色连接像素端点(x,y + 4 + i * 6)到(x + 20,y + 4 + i * 6)
            u_rect(x + 4 + i * 4, y + 1 + i * 6, 3, 7, c);  // 用传入图标色写矩形，起点(x + 4 + i * 4,y + 1 + i * 6)，尺寸3×7像素
        }
        break;  // 当前应用图标已经画完，退出图标种类switch
    default:  // 其他编号进入当前switch的默认结果
        u_frame((struct ui_rect){x, y, 21, 21}, c);  // 沿当前显式矩形的四边画一像素边框
        u_line(x + 3, y + 17, x + 9, y + 8, c);      // 用传入图标色连接像素端点(x + 3,y + 17)到(x + 9,y + 8)
        u_line(x + 9, y + 8, x + 14, y + 14, c);     // 用传入图标色连接像素端点(x + 9,y + 8)到(x + 14,y + 14)
        u_line(x + 14, y + 14, x + 18, y + 6, c);    // 用传入图标色连接像素端点(x + 14,y + 14)到(x + 18,y + 6)
        break;  // 当前应用图标已经画完，退出图标种类switch
    }
}

```

## 把命令环放进窗口

一条日志可能比窗口宽。拆行只改变显示：例如一条二十字符日志在只能容纳十二字符时画成两行，console 中仍是一条记录。滚动选择可见显示行，不能靠修改原始字符串截断它，否则串口和另一个尺寸的窗口会失去原内容。

Terminal 按逻辑行号读取 console，依据窗口宽度拆成可见行，再用console.scroll选择回看范围。输入提示和光标来自 input/input_length，内容直接对应文本会话中的命令模型。

**新建 `frontend/apps/terminal.c`：**

<a id="code-frontend-apps-terminal-c-1"></a>

<!-- write: frontend/apps/terminal.c -->
```c
#include "internal.h"  // 取得本章已经实现的应用绘图与公共文字接口。

void ui_terminal_draw(struct ui_desktop *s, struct ui_rect r) {  // 把console逻辑行拆成窗口显示行并绘制输入
    u_button((struct ui_rect){r.x + r.w - 100, r.y + r.h - 61, 84, 28}, "Clear", false, false);  // 清屏操作放在输入行右侧
    const struct airi_console *c = s->console;  // Terminal借用同一console，拆行只改显示，不改串口日志
    unsigned cols = (unsigned)(r.w - 36) / 10, visible = (unsigned)(r.h > 84 ? r.h - 84 : 0) / 24;  // 扣除边距后每字符10像素、每行24像素，得到可见列数和行数
    uint64_t seq[AIRI_CONSOLE_LINES * 4];  // 显示行索引最多1024项，每项保存来源console逻辑行号
    unsigned offset[AIRI_CONSOLE_LINES * 4], n = 0;  // offset与seq配对存来源字节偏移，n只计已经形成的显示段
    for (uint64_t i = c->total_lines - c->line_count; i < c->total_lines; ++i) {  // 按console有效逻辑行号形成窗口显示行
        unsigned len = (unsigned)airi_strlen(airi_console_line(c, i)),  // 测量第i条原始日志内容字节数，终止零不参与拆行
                 rows = len ? (len + cols - 1) / cols : 1;  // 非空日志按列数向上取整拆行，空日志仍占一条显示行
        for (unsigned j = 0; j < rows && n < AIRI_CONSOLE_LINES * 4; ++j) {  // 把当前日志分解成窗口所需的显示行，受临时索引容量限制
            seq[n] = i;  // 第n条显示行记住所属原console逻辑行号
            offset[n++] = j * cols;  // 保存该显示段在原日志中的字节偏移，再推进显示行计数
        }
    }
```

上面得到两列配对索引：seq[i] 指向一条 console 逻辑行，offset[i] 指出这一显示段从该行哪个字节开始；n 是显示段总数。例如二十字符按十二列拆成两段，两项 seq 相同而 offset 分别为 0、12。接下来把 console.scroll 收窄到可回看的范围，求出 [begin,end)，再从原日志复制本屏各段来画。原始日志仍保持完整。继续追加同一函数。

<!-- append: frontend/apps/terminal.c -->
```c
    unsigned maxscroll = n > visible ? n - visible : 0,  // 显示段比窗口容量多时才能回滚，最多回看多出的行数
             scroll = c->scroll < maxscroll ? c->scroll : maxscroll, end = n - scroll,  // 把请求滚动收窄到有效范围，再从尾部减滚动量得到显示终点
             begin = end > visible ? end - visible : 0;  // 显示区间起点最多向前visible行，少于一屏时从第0段开始
    u_clip((struct ui_rect){r.x + 16, r.y + 12, r.w - 32, r.h - 82});  // 把后续文字或像素限制在本次显式矩形内，长内容在边界停止写入
    for (unsigned i = begin; i < end; ++i) {  // 遍历本次显示区间，滚动前后的行保留在原状态
        char row[129];  // 显示段最多128内容字节，再留1字节NUL，原日志保持完整
        const char *src = airi_console_line(c, seq[i]) + offset[i];  // 按逻辑行号借用仍保留的日志文字
        unsigned len = (unsigned)airi_strlen(src);  // 当前来源指针之后的日志字节数，下一步收窄到本段
        if (len > cols)  // 当前日志片段超过窗口字符列数时只显示一段
            len = cols;  // 当前显示段最多包含窗口能容纳的等宽字符列数
        if (len > 128)  // 局部显示数组最多装128内容字节，额外一格保存NUL
            len = 128;  // 局部显示数组最多装128内容字节，给NUL留最后一格
        airi_memcpy(row, src, len);  // 按字节复制当前有效数据，复制长度由当前对象内容或容量计算
        row[len] = 0;  // 给复制到局部显示行的有效字节补NUL，原日志不截断
        u_text(r.x + 18, r.y + 14 + (int)(i - begin) * 24, row, U_CODE,  // 在内容左缘右移18像素、整屏y=r.y + 14 + (int)(i - begin) * 24显示本次拆出的显示文字行，使用等宽字形
               ui_app_match(row, "airi>") ? U_BLUE : U_TEXT);  // 当前日志显示段含airi>时用蓝色突出提示符，否则使用普通文字色
    }
```

日志区临时收窄过 clip，下面先恢复传入的整个 body，再画分隔线、当前输入和状态栏。输入超过可见列时只让显示起点 start 向后走，完整命令仍在 console.input；光标按可见尾部的位置绘制。继续追加到函数结束。

<!-- append: frontend/apps/terminal.c -->
```c
    u_clip(r);  // 恢复当前应用内容裁剪，后续列或控件可使用完整内容区
    u_rect(r.x, r.y + r.h - 68, r.w, 1, U_EDGE);  // 用分隔线色写矩形，起点(r.x,r.y + r.h - 68)，尺寸r.w×1像素
    u_text(r.x + 18, r.y + r.h - 54, "airi>", U_CODE, U_BLUE);  // 在内容左缘右移18像素、内容底边上移54像素显示文字"airi>"，使用等宽字形
    unsigned cap = cols > 18 ? cols - 18 : 1,  // 为提示符及Clear按钮合计保留18列，剩余列数用于尚未提交命令
             start = c->input_length > cap ? c->input_length - cap : 0;  // 输入过宽时显示末尾cap个字符，完整输入仍保存在console
    u_text(r.x + 78, r.y + r.h - 54, c->input + start, U_CODE, U_TEXT);  // 在内容左缘右移78像素、内容底边上移54像素显示c->input + start，使用等宽字形
    u_rect(r.x + 78 + (int)(c->input_length - start) * 10, r.y + r.h - 33, 8, 1, U_BLUE);  // 输入尾端每字符前进10像素，在最后一个可见字符之后画8像素宽的光标短线
    ui_app_status(r, "Local input / US keyboard",  // 在内容底部显示文字"Local input / US keyboard"；右侧说明按测得的文字宽度对齐
                  s->backend.mode == UI_REFERENCE ? "Local commands"  // 参考模式说明命令只观察本进程内状态，不提供真实程序执行
                                                  : "COM1 / 115200 / polled I/O");  // 真实内核后端页脚报告当前串口设备模式
}

```

System 读取相同 boot，五个 tab 分别显示概况、内存、存储、设备和日志。tab 先从 System 窗口状态取得，下面按它选择一页正文。

reference 与 live 使用不同页脚。当前存储页说明服务范围，尚无挂载磁盘。主机观察把像素数组作为画布交给 ui_draw，但参考 boot 没有真实固件帧缓冲；System 因此显示启动显示描述缺失，而不会把这份 1280×800 画布误记成引导器提供的设备。真实内核运行时，System 才从同一启动描述显示设备宽高和 pitch。

**新建 `frontend/apps/system.c`：**

<a id="code-frontend-apps-system-c-1"></a>

<!-- write: frontend/apps/system.c -->
```c
#include "internal.h"  // 取得本章已经实现的应用绘图与公共文字接口。

void ui_system_draw(struct ui_desktop *s, struct ui_rect r) {  // 按当前页显示同一启动信息的内存、设备或日志
    const char *tabs[] = {"Overview", "Memory", "Storage", "Devices", "Journal"};  // 5个System页面名称按tab编号顺序保存
    unsigned tab = s->windows[UI_SYSTEM].tab;  // System窗口的页下标决定概况、内存、存储、设备或日志
    for (unsigned i = 0; i < 5; ++i)  // 按有效页面下标画5个页签按钮
        u_button((struct ui_rect){r.x + 14 + (int)i * 108, r.y + 9, 98, 28}, tabs[i], tab == i,  // 画当前页面按钮名按钮；其矩形供后续事件命中共用，当前仅输出像素
                 false);  // 当前按钮未禁用，实参只控制绘图外观
    u_rect(r.x, r.y + 45, r.w, 1, U_EDGE);  // 用分隔线色写矩形，起点(r.x,r.y + 45)，尺寸r.w×1像素
    int x = r.x + 24, y = r.y + 65;  // System正文起点从内容左上分别留24和65像素
    char line[160];  // 160字节局部数组承接当前启动字段的格式化说明
    if (tab == 0) {  // System第0页显示当前后端的启动概况
        u_text(x, y, "System information", U_LABEL, U_TEXT);  // 在整屏x=x、整屏y=y显示文字"System information"
        ui_app_text(x, y + 41, "AiriOS / x86_64 workstation");  // 在整屏x=x、整屏y=y + 41显示文字"AiriOS / x86_64 workstation"
        ui_app_text(x, y + 73, "QEMU q35 / UEFI / single CPU");  // 在整屏x=x、整屏y=y + 73显示文字"QEMU q35 / UEFI / single CPU"
        airi_snprintf(line, 160, "Boot loader: %s", s->backend.boot->loader);  // System概况页显示这次绑定启动结构中的加载器名称
        ui_app_text(x, y + 105, line);  // 在概况页显示刚格式化的启动加载器名称
        const struct airi_framebuffer *display = &s->backend.boot->framebuffer;  // 查询服务提供的启动描述，不是 ui_draw 当前借用的主机画布
        if (airi_framebuffer_valid(display)) {  // 真实内核提供合法设备描述时显示其布局
            airi_snprintf(line, sizeof(line), "Display: %llux%llu / %u bpp / pitch %llu",  // 宽高按像素、pitch 按每行字节显示
                          (unsigned long long)display->width,   // 宽度转为 %llu 对应类型，值不改变
                          (unsigned long long)display->height,  // 高度转为 %llu 对应类型，值不改变
                          (unsigned)display->bpp,  // 每像素位数按 %u 显示
                          (unsigned long long)display->pitch);  // 每行字节跨度可大于宽度乘每像素字节数
        } else {  // 主机参考服务没有真实启动帧缓冲，不能把零值显示为屏幕参数
            airi_snprintf(line, sizeof(line), "Display: windowed session");  // 窗口仍可在独立的主机画布上绘制
        }
        ui_app_text(x, y + 137, line);  // 在概况页显示启动帧缓冲布局，或主机画布与启动描述分离的说明
        ui_app_text(x, y + 185, "Connected services");  // 在整屏x=x、整屏y=y + 185显示文字"Connected services"
        ui_app_muted(x, y + 216,  // 用小字体和次要颜色绘制说明文字，本次实参取自当前桌面状态
                     s->backend.mode == UI_REFERENCE  // 按实际绑定后端模式选择后续参考说明或内核说明，不修改mode字段
                         ? "Session storage / no disk mounted"  // 参考文件只存于本进程数组，尚无磁盘服务
                         : "Boot information / console / framebuffer");  // 真实内核后端当前只提供启动、console与像素输出
        u_button((struct ui_rect){x, y + 263, 180, 32}, "Open preferences", false, false);  // 画文字"Open preferences"按钮；其矩形供后续事件命中共用，当前仅输出像素
```

Memory 页使用同一 boot 的启动分类，页表和分配器尚未提供实时统计。sizes、names、colors 按相同下标对应 usable、held、kernel 三类；柱条按累计字节边界计算，所以最后一段与总宽闭合，总和为零时不除零。下方地址表再用 map_page×12 加窗口 scroll 取得首条区域，同时受真实条数、窗口容量和每页十二条约束。继续追加 Memory 分支，开头的右花括号闭合上面的 Overview。

<!-- append: frontend/apps/system.c -->
```c
    } else if (tab == 1) {  // System第1页显示启动内存地图及各类字节总量
        u_label(x, y, "PHYSICAL MEMORY / BOOT INVENTORY", U_MUTED);  // 内存分类是启动库存，不是分配器实时用量
        uint64_t sizes[] = {airi_memory_bytes(s->backend.boot, AIRI_MEMORY_USABLE),  // 三类数据读取同一启动描述
                            airi_memory_bytes(s->backend.boot, AIRI_MEMORY_BOOT_RECLAIM),  // 保留中的引导内存
                            airi_memory_bytes(s->backend.boot, AIRI_MEMORY_KERNEL)};  // 内核及模块内存
        const char *names[] = {"Usable", "Boot reclaim / held", "Kernel"};  // 文字明确对应三类，不只靠颜色区分
        uint32_t colors[] = {U_ACCENT, U_BLUE, U_PINK};  // 内存图三类固定颜色
        uint64_t sum = sizes[0] + sizes[1] + sizes[2], accumulated = 0;  // 总和只用于三类字节占比
        int width = r.w - 48, previous = 0;  // 同一个列宽服务标题、图形与下方地址表
        for (unsigned k = 0; k < 3; ++k) {   // 三个读数同一基准线对齐
            int cx = x + (int)k * width / 3;  // 按当前窗口等分列宽
            airi_snprintf(line, sizeof(line), "%llu", (unsigned long long)(sizes[k] / 1024));  // 字节换算成KiB
            u_text(cx, y + 23, line, U_TITLE, colors[k]);    // 主要数值用22像素，长数字仍留有空间
            u_text(cx, y + 55, names[k], U_SMALL, U_MUTED);  // 分类独立一行
            accumulated += sizes[k];  // 累计边界可让整条宽度精确闭合
            int end = sum ? (int)(accumulated * (uint64_t)width / sum) : previous;  // 空库存不除零
            u_rect(x + previous, y + 85, end - previous, 16, colors[k]);  // 真实长度决定每段所占像素
            if (end > previous) u_rect(x + previous, y + 85, 1, 16, U_SURFACE);  // 分类之间用一像素分开
            previous = end;  // 下一段从前一段末尾继续
        }
        ui_app_muted(x, y + 110, "KiB / usable, boot reclaim and kernel");  // 分母与单位都明确
        int row = y + 142;  // 内存地图列标题从System正文起点下移142像素
        u_rect(x, row, width, 27, U_PANEL);  // 用工具栏底色写矩形，起点(x,row)，尺寸width×27像素
        ui_app_muted(x + 8, row + 6, "ID");  // 在整屏x=x + 8、整屏y=row + 6显示文字"ID"
        ui_app_muted(x + 56, row + 6, "Physical start");  // 在整屏x=x + 56、整屏y=row + 6显示文字"Physical start"
        ui_app_muted(x + 224, row + 6, "Physical end");   // 在整屏x=x + 224、整屏y=row + 6显示文字"Physical end"
        ui_app_muted(x + 392, row + 6, "Class");  // 在整屏x=x + 392、整屏y=row + 6显示文字"Class"
        row += 35;  // 标题行之后向下留35像素，作为首条地图记录位置
        unsigned capacity = (unsigned)(r.y + r.h - 42 - row) / 25,  // 扣状态栏后按每条25像素算当前窗口最多可见多少区域
                 begin = s->console->map_page * 12 + s->windows[UI_SYSTEM].scroll;  // 逻辑页先换成12条一页的区域下标，再加入窗口滚动量
        for (unsigned i = begin;  // 遍历本次显示区间，滚动前后的行保留在原状态
             i < s->backend.boot->region_count && i < begin + capacity && i < begin + 12; ++i) {  // 区域有效数、窗口容量和每页12条同时限制地图遍历
            const struct airi_memory_region *region = &s->backend.boot->regions[i];  // 地址与类别属于同一记录
            int ry = row + (int)(i - begin) * 25;  // 保持分页与滚动含义
            airi_snprintf(line, sizeof(line), "%03u", i);  // 地图下标为实际行身份
            u_text(x + 8, ry, line, U_MONO, U_MUTED);      // 下标列与地址留独立间隙
            airi_snprintf(line, sizeof(line), "%016llx", (unsigned long long)region->base);  // 十六进制物理起点
            u_text(x + 56, ry, line, U_MONO, U_TEXT);  // 起点固定16位
            airi_snprintf(line, sizeof(line), "%016llx", (unsigned long long)(region->base + region->length));  // 半开区间终点
            u_text(x + 224, ry, line, U_MONO, U_TEXT);  // 独立列避免两组长地址粘连
            u_text(x + 392, ry, airi_memory_name(region->kind), U_MONO,  // 类别名与实际枚举一致
                   region->kind == AIRI_MEMORY_USABLE ? U_ACCENT : U_TEXT);  // 可用内存强调规则不变

        }
```

其余页面继续读取当前已有来源。Storage 只在参考服务正常时说明会话内存工作区；否则明确显示尚无块设备文件系统。Devices 和 Journal 分别格式化固定设备说明与有序启动记录，不把这些文字再追加进 console 日志。最后状态栏区分 Recorded boot inventory 与 Live boot handoff。继续追加至函数结束。

<!-- append: frontend/apps/system.c -->
```c
    } else if (tab == 2) {  // System第2页说明参考内存工作区，或报告尚无块设备文件系统
        u_text(x, y, "Storage volumes", U_LABEL, U_TEXT);  // 在整屏x=x、整屏y=y显示文字"Storage volumes"
        if (s->backend.mode == UI_REFERENCE && s->service_state == UI_NORMAL) {  // 只有当前主机参考服务正常时才显示本会话内存工作区
            ui_app_text(x, y + 45, "Session storage");  // 在整屏x=x、整屏y=y + 45显示文字"Session storage"
            ui_app_muted(x, y + 76, "Files are kept until this session closes.");  // 在整屏x=x、整屏y=y + 76显示文字"Files are kept until this session closes."
            ui_app_muted(x, y + 124, "Capacity: 32 records / documents up to 8191 bytes");  // 在整屏x=x、整屏y=y + 124显示文字"Capacity: 32 records / documents up to 8191 bytes"
            ui_app_text(x, y + 159, "Files and Editor share the same workspace provider.");  // 在整屏x=x、整屏y=y + 159显示文字"Files and Editor share the same workspace provider."
            u_button((struct ui_rect){x, y + 220, 140, 32}, "Open files", true, false);  // 画文字"Open files"按钮；其矩形供后续事件命中共用，当前仅输出像素
        } else  // 前一分支条件未成立时转入随后的单条替代操作
            ui_app_unavailable(s, (struct ui_rect){r.x, r.y + 44, r.w, r.h - 72},  // 按服务状态绘制加载、空或不可用提示，本次实参取自当前桌面状态
                               "No mounted block-device filesystem");  // 存储未接入时明确说明尚无块设备文件系统
    } else if (tab == 3) {  // System第3页显示固定控制台设备端点
        u_text(x, y, "Console devices", U_LABEL, U_TEXT);  // 在整屏x=x、整屏y=y显示文字"Console devices"
        for (unsigned i = 0; i < 3; ++i) {  // 按固定端点下标0到2生成3条控制台设备说明
            airi_device_line(s->console, i, line, sizeof(line));  // 形成第i项设备说明，下一行直接绘制，不新增console日志
            u_text(x, y + 51 + (int)i * 42, line, U_MONO, U_TEXT);  // 把第i条控制台设备说明按42像素行距绘到Devices页
        }
        ui_app_muted(x, y + 202, "Console input and display endpoints");  // 在整屏x=x、整屏y=y + 202显示文字"Console input and display endpoints"
    } else {  // 其余页下标显示Journal启动交接记录；正常页签将这里选为第4页
        u_text(x, y, "Boot records", U_LABEL, U_TEXT);  // 在整屏x=x、整屏y=y显示文字"Boot records"
        ui_app_muted(x, y + 33, "Ordered handoff records / no wall-clock timestamp");  // 在整屏x=x、整屏y=y + 33显示文字"Ordered handoff records / no wall-clock timestamp"
        for (unsigned i = 0; i < airi_journal_count(); ++i) {  // 按8条启动记录的有效下标查询并显示
            airi_journal_line(s->console, i, line, sizeof(line));   // 形成第i项固定启动记录，下一行显示在System日志页
            u_text(x, y + 71 + (int)i * 32, line, U_MONO, U_TEXT);  // 把第i条启动交接记录按32像素行距绘到Journal页
        }
    }
    ui_app_status(r,  // 在应用内容底部绘制27像素状态栏，本次实参取自当前桌面状态
                  s->backend.mode == UI_REFERENCE  // 按实际绑定后端模式选择后续参考说明或内核说明，不修改mode字段
                      ? "Recorded boot inventory"  // 参考后端明确标示启动数据为示例
                      : "Live boot handoff",  // 真实后端标示数据来自当前内核启动交接
                  tab == 1 ? "PgUp / PgDn: memory inventory" : NULL);  // Memory页右侧显示翻页说明，其他页面不额外画这条状态文字
}

```

两个内容函数已有定义，ui_apps_draw 依据 app 编号选择调用。其他应用暂时只显示窗口外框，内容函数完成后再增加分派。

**新建 `frontend/apps/builtin.c`：**

<a id="code-frontend-apps-builtin-c-1"></a>

<!-- write: frontend/apps/builtin.c -->
```c
#include "internal.h"  // 取得本章已经实现的应用绘图与公共文字接口。
void ui_apps_draw(struct ui_desktop *s, enum ui_app app, struct ui_rect r) {  // 根据应用枚举绘制同一内容矩形，窗口框由外层负责。
    switch (app) {     // 应用枚举决定本次内容绘制器。
    case UI_TERMINAL:  // 应用身份为终端时进入对应分支。
        ui_terminal_draw(s, r);  // 用当前桌面和内容矩形绘制终端。
        break;  // 当前匹配已完成，停止这一层循环或分支。
    case UI_SYSTEM:  // 应用身份为系统信息时进入对应分支。
        ui_system_draw(s, r);  // 用当前桌面和内容矩形绘制系统信息。
        break;  // 当前匹配已完成，停止这一层循环或分支。
    default:  // 未知应用编号不绘制内容；窗口框仍由外层处理
        break;  // 当前匹配已完成，停止这一层循环或分支。
    }
}

```

## 按窗口层次绘制

<a id="paint-order"></a>

重叠窗口不需要像素知道属于谁。若 Terminal 先画、System 后画，两者重叠处最终留下 System 的颜色，这就是当前的遮挡方式。order 保存这种从底到顶的顺序，active 保存接受窗口操作的应用编号；置顶时两者一起更新。

裁剪限制一个应用写到自己的 body 之内，避免长行越过标题和边框。窗口之间的遮挡仍由先后覆盖决定：上层窗口随后写入同一片画布。每个应用返回后恢复整屏裁剪，下一个窗口和最后的底栏才不会被前一个 body 限住。

具体绘制先铺背景、顶栏和左侧应用栏，再从 order 底部向顶部画可见窗口，最后画底部窗口栏。Terminal 与 System 已有内容函数，其余应用编号当前只绘制外框。`ui_draw(s,f)` 借用桌面 s 与画布描述 f；它会按本帧尺寸回写窗口几何，后续事件必须读取这些更新后的矩形。

因此一次绘图也会确定本帧真正使用的几何。ui_open 中的尺寸是初始值；ui_draw 先读取实际画布宽高，再收窄或最大化窗口并回写 rect。随后才计算 `body={x+1,y+49,w-2,h-50}`，把它交给 ui_apps_draw。后面输入接入时须读取这些更新后的矩形，不能另算一套旧位置。

颜色角色已经由G13解析。本次绘图先借用实际画布，再从desktop.theme取得会话模式，铺桌面栏和应用标识；下面的函数尚未闭合，窗口遍历接在同一个ui_draw中。

**新建 `frontend/core/paint.c`：**

<a id="code-frontend-core-paint-c-1"></a>

<!-- write: frontend/core/paint.c -->
```c
#include "internal.h"  // 取得核心内部的桌面类型、有限copy与基础库接口。
static const char *titles[] = {"Terminal", "Files",    "Editor",     "Tasks",  // 窗口标题按应用枚举顺序排，应用编号可直接索引
                               "System",   "Settings", "Data viewer"};  // 继续应用枚举4到6的窗口标题，数组顺序与应用id一致
void ui_draw(struct ui_desktop *s, const struct airi_framebuffer *f) {  // 按当前窗口次序绘制整帧
    if (!airi_framebuffer_valid(f))  // 画布地址、尺寸或通道无效时，在写像素之前结束
        return;  // 画布验证未通过，本帧不写像素
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
```

active<0 时顶栏显示 Desktop，否则用应用编号取标题。当前没有资源栏，right为0，窗口可一直使用到右侧固定边距。找到最上方可见最大化窗口后，可以跳过被它完全挡住的底层窗口。

普通窗口先限制宽高，再移回屏幕范围，左上避开系统栏。48像素标题在外框内部保留应用身份片；内容从标题下方第49像素开始。窗口绘完后还要恢复整屏裁剪，才能写底部窗口栏。

**追加到 `frontend/core/paint.c` 末尾：**

<a id="code-frontend-core-paint-c-2"></a>

<!-- append: frontend/core/paint.c -->
```c
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
```

到这里，w->rect 已是本帧可用区内的外框。下面复制成局部 r，画阴影、表面和标题身份片；heads 的字距与片宽使用前面的标签辅助函数。标题文字另设裁剪区，系统按钮仍在它的右侧。随后 u_window_body(r) 只扣除外框边线与标题，将 body 交给应用内容函数。继续追加当前窗口循环。

<!-- append: frontend/core/paint.c -->
```c
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
```

底部窗口栏按应用编号遍历已打开窗口，active决定其文字颜色。它与应用正文共用画布，但先恢复整屏裁剪，避免被最后一个body限制。

**追加到 `frontend/core/paint.c` 末尾：**

<a id="code-frontend-core-paint-c-3"></a>

<!-- append: frontend/core/paint.c -->
```c
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

最后写出触发本章这一帧的调用方。它依次初始化 reference、console、desktop；再把参数转换为 app，确认编号在 0..UI_APP_COUNT-1 内，打开该应用并设置 maximized。然后建立指向 pixels 的画布描述，调用一次 ui_draw，才把结果保存为图片。所有对象都覆盖这一轮调用，console 和 boot 没有为各个窗口另复制一份。

**新建 `labs/tutorial/desktop.c`：**

<a id="code-labs-tutorial-desktop-c-1"></a>

<!-- write: labs/tutorial/desktop.c -->
```c
#include "ui.h"      // 引入前端服务合同与桌面状态
#include <stdio.h>   // 主机观察使用标准文件输出与打印接口
#include <stdlib.h>  // 主机观察使用atoi转换应用参数
static uint32_t pixels[1280 * 800];  // 主机观察程序拥有静态像素数组，绘图描述只借用它
static struct ui_desktop desktop;    // 主机观察拥有整份静态桌面，多个步骤修改同一对象
static struct airi_console console;  // 主机观察拥有同一console输入输出状态
int main(int argc, char **argv) {    // 在主机准备输入、运行当前观察并以退出码报告结果
    struct ui_backend backend;     // 观察程序持有回调合同副本，初始化后才可调用
    ui_fixture_backend(&backend);  // 重置当前主机参考服务并填入回调表，本次实参取自当前桌面状态
    airi_console_init(&console, backend.boot);  // 观察console与System借用同一参考启动结构，日志和面板来源一致
    ui_init(&desktop, backend, &console);  // 清桌面状态，绑定服务和console并打开默认应用，本次实参取自当前桌面状态
    int app = argc > 1 ? atoi(argv[1]) : UI_TERMINAL;  // 有参数时把十进制字符串变成应用编号，否则生成Terminal
    if (app < 0 || app >= UI_APP_COUNT)  // 检查转换结果的数组边界后才索引窗口
        return 1;  // 当前主机观察失败，以非零状态阻止后续误判通过
    ui_open(&desktop, (enum ui_app)app);    // 打开或置顶一个应用，更新窗口次序和活动编号，本次实参取自当前桌面状态
    desktop.windows[app].maximized = true;  // 观察程序先把目标窗口设为最大化，绘图据此计算实际矩形
    struct airi_framebuffer fb = {  // 当前操作使用的airi_framebuffer记录布局
        .virtual_address = (uintptr_t)pixels, .width = 1280, .height = 800,  // 设置像素数组的主机虚拟地址、软件画布像素宽度、软件画布像素高度
        .pitch = 1280 * 4, .bpp = 32, .red_size = 8, .red_shift = 16,  // 设置每行实际字节跨度、每像素位数、红色通道位数、红色通道最低位位置
        .green_size = 8, .green_shift = 8, .blue_size = 8};  // 设置绿色通道位数、绿色通道最低位位置、蓝色通道位数
    ui_draw(&desktop, &fb);  // 按当前窗口次序绘制整帧，本次实参取自当前桌面状态
```

ui_draw 返回时，pixels 已保存本次状态对应的整帧像素，desktop 中也保留本帧修正后的窗口几何。下面继续 main，把数组按 PPM 的 RGB 字节次序输出；这一步只保存图片，不再刷新服务或执行窗口操作。

<!-- append: labs/tutorial/desktop.c -->
```c
    FILE *file = fopen("build/desktop.ppm", "wb");  // 以二进制写并截断旧内容打开本次桌面PPM文件
    if (!file)  // 打开PPM输出失败时不继续写文件
        return 1;  // 当前主机观察失败，以非零状态阻止后续误判通过
    fprintf(file, "P6\n1280 800\n255\n");  // 把PPM魔数、尺寸和255通道上限写到主机输出文件
    for (unsigned i = 0; i < 1280 * 800; ++i) {  // 逐个像素向PPM输出1280×800个RGB三字节
        unsigned char rgb[] = {(pixels[i] >> 16) & 255, (pixels[i] >> 8) & 255, pixels[i] & 255};  // 从像素数值按红绿蓝顺序提取3个八位通道，形成与主机字节序无关的PPM字节
        if (fwrite(rgb, 1, 3, file) != 3)  // 每像素应写3个RGB字节，短写即报告失败
            return 1;  // 当前主机观察失败，以非零状态阻止后续误判通过
    }
    if (fclose(file))  // 关闭时检查缓冲输出是否真正交给主机文件系统接口
        return 1;  // 当前主机观察失败，以非零状态阻止后续误判通过
    printf("app=%d -> build/desktop.ppm\n", app);  // 向主机终端报告本次观察的实际状态或输出位置
    return 0;  // 观察完整通过，主机进程以成功状态退出
}
```

下面两次运行是两个独立主机进程，每次都会从参考数据重新初始化。第一次生成 Terminal，复制图片后再运行参数 4 生成 System；两次写入同一路径，所以每次紧接着另存这一帧。

**在学习目录根目录运行：**

<!-- run -->
```sh
mkdir -p build  # 保存本次主机程序和图像，已有目录可以继续
cc -std=c11 -O0 -g -Wall -Wextra -Werror -Iinclude -Ifrontend/include labs/tutorial/desktop.c kernel/lib/base.c kernel/boot/info.c kernel/console/console.c frontend/core/*.c frontend/apps/*.c frontend/backends/reference.c frontend/render/draw.c -o build/desktop  # Shell展开两个通配符，把当前已经写出的核心和应用一起链接
./build/desktop  # 运行当前完整观察程序，退出非零表示本组观察失败
cp build/desktop.ppm build/terminal.ppm  # 留下Terminal这一帧，随后System输出不会覆盖它
./build/desktop 4  # 应用编号4选择System，使用同一启动描述绘图
cp build/desktop.ppm build/system.ppm  # 保存System实际生成的另一帧
```

不带参数时，ui_init 已打开 Terminal，main 再打开它只做去重置顶，order 仍是 [0]。参数 4 时，order 是 [0,4]；System 最大化后，绘图把 first 设为它的层次，跳过被完全遮住的 Terminal 像素，但 Terminal 仍出现在底栏的已打开应用中。查看 build/terminal.ppm 与 build/system.ppm，可以分别对应这两次状态；当前没有任务统计回调或资源栏，G18 才接入。下一章在同样的窗口框架中绘制 Files 和 Editor。
