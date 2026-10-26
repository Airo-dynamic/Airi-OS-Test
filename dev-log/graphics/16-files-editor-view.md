# G16 · 把文件快照和文档显示成可读内容

Terminal 和 System 已能在窗口里读取状态。接下来让 Files 画出 telemetry 目录中的六条记录，让 Editor 画出其中 main.lua 的正文。两种画面都由主机观察程序生成：每次启动先查询目录、读取初始文档，再按应用参数选择这一帧画什么。

这次先把读取与显示接通。Files 的排序只改变文件行的位置，Editor 继续按读入成功时保存的服务 id 指认文档。列表上的 Open、编辑器的 Save 和 Run 仍只绘制外观，输入与保存路径在后续接入。

<a id="document-copies"></a>

G14 的服务数组持有 main.lua 正文。目录查询只把名称、id、大小等元信息复制到 `desktop.files`，因此画文件列表所需的数据已经齐备，Editor 的文字还要通过 `read` 单独取得。

读取成功后，正文进入桌面自己的 `editor` 数组，`saved` 再保存一份相同文字。以后按键修改的是 editor，服务记录和 saved 仍保留原内容；保存成功才更新保存点。本章只读入和显示，初始化后的两份文字相同，`dirty` 保持 false。

桌面还需要分别保存文档的服务身份 `editor_id` 和名称 `editor_name`。Files 的 `selection_file` 只定位目录副本中的一项；它的值不能代替读文档所需的服务 id。下面在原有窗口、服务与绘图接口之间加入这些文档状态，数组容量 `UI_DOC_SIZE=8192` 包含结尾 NUL，正文最多 8191 字节。

**依次更新 `frontend/include/ui.h` 的三处内容：**

<a id="code-frontend-include-ui-h-1"></a>

先在动作枚举与拖动枚举之间加入键入目标。本章先用 UI_FOCUS_NONE 表示没有字段接收键入，后续输入再区分筛选和查找。每处的旧块用于定位，只把对应的新块写回原位置。

<!-- replace: frontend/include/ui.h -->
<!-- before -->
```c
enum ui_state { UI_NORMAL, UI_EMPTY, UI_LOADING, UI_ERROR, UI_OFFLINE };  // 列表和动作的可见服务状态，不等同线程调度状态。
enum ui_action { UI_CREATE_FILE, UI_CREATE_DIR };  // 当前服务仅支持创建普通文件和目录
enum ui_drag { UI_DRAG_NONE = -1 };  // 尚无指针拖动，初始化值表示无操作
struct ui_file {  // 服务复制出来的一条文件快照，不保存文件内容。
```
<!-- after -->
```c
enum ui_state { UI_NORMAL, UI_EMPTY, UI_LOADING, UI_ERROR, UI_OFFLINE };  // 列表和动作的可见服务状态，不等同线程调度状态。
enum ui_action { UI_CREATE_FILE, UI_CREATE_DIR };  // 当前服务仅支持创建普通文件和目录
enum ui_focus { UI_FOCUS_NONE, UI_FOCUS_FILTER, UI_FOCUS_FIND };  // 键入目标为应用正文、筛选字段或查找字段。
enum ui_drag { UI_DRAG_NONE = -1 };  // 尚无指针拖动，初始化值表示无操作
struct ui_file {  // 服务复制出来的一条文件快照，不保存文件内容。
```

接着在 `ui_desktop` 中，从宽高字段到结构末尾这一段加入文档、筛选与选区状态。前面的服务表和窗口次序、后面的函数声明继续保留。`editor` 与 `saved` 各有独立容量；`selection_file` 仍指向文件快照，新增的 `editor_id` 才保存文档的服务身份。

<!-- replace: frontend/include/ui.h -->
<!-- before -->
```c
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
```
<!-- after -->
```c
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
```

最后在应用绘图接口之前声明 `ui_file_order`。它把排好次序的快照下标写入输出数组，并返回有效条数；调用它的 Files 绘图稍后接入。

<!-- replace: frontend/include/ui.h -->
<!-- before -->
```c

void u_icon(int, int, int, uint32_t);  // 在给定坐标以类型和颜色绘制小图标。
void ui_apps_draw(struct ui_desktop *, enum ui_app, struct ui_rect);  // 以应用身份分派内容绘图，rect已扣除标题栏。
#endif  // 结束完整桌面接口的重复包含保护
```
<!-- after -->
```c

void u_icon(int, int, int, uint32_t);  // 在给定坐标以类型和颜色绘制小图标。
unsigned ui_file_order(const struct ui_desktop *, unsigned *);  // 把当前文件排序位置写成快照下标数组并返回数量。
void ui_apps_draw(struct ui_desktop *, enum ui_app, struct ui_rect);  // 以应用身份分派内容绘图，rect已扣除标题栏。
#endif  // 结束完整桌面接口的重复包含保护
```

打开 Files 后，main.lua 会与目录和其他文件一起显示；按名称排序后，它所在的行可能改变。Editor 仍须读取 id=1 的记录，不能把屏幕上的行号直接拿去当文件身份。

<a id="file-indices"></a>

快照下标定位 desktop.files 中的一项，可见行号定位筛选排序后的某一行，服务 id 定位后端记录。ui_file_order 用整数数组连接前两者：若 order[0]=3，第一行画 files[3]；该项的 id 再交给 read。它不移动 files 本身，已有选择仍能以原快照下标表示。

`ui_init` 先把 editor_id 和文件剪贴板身份设为 −1，把文档名设为 Untitled，再刷新目录。参考模式下，它沿服务返回的次序寻找第一项普通文件，本次找到的是 id=1 的 main.lua。`read` 收到服务上下文、该 id、editor 地址和完整数组容量；成功以后才保存 id、名称与 saved 副本。找到第一项普通文件后循环就结束，读取失败也不会继续换另一项。

这次初始化在参考模式默认打开 Editor；真实模式仍打开 Terminal。打开和置顶继续交给原有 `ui_open`，这次还会清除旧字段焦点。现在更新 state.c，保留主题命令扩展、目录刷新与窗口次序的既有用途。

**依次更新 `frontend/core/state.c` 的三处内容：**

<a id="code-frontend-core-state-c-1"></a>

文件已由 G15 建立，`appearance` 的主题处理、`ui_refresh` 的查询和容量检查继续保留。


在 `ui_init` 中定位绑定 console 的位置，替换到函数结尾的这一段。清零、安装主题扩展的函数前半段保留；新段先建立无身份状态，再查询目录，最后尝试读取首个普通文件。

<!-- replace: frontend/core/state.c -->
<!-- before -->
```c
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
<!-- after -->
```c
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

```

`ui_open` 继续沿用原有窗口几何。下面这处保留 720×480 的初始下限，并将旁注与当前绘图收窄的位置接起来。

<!-- replace: frontend/core/state.c -->
<!-- before -->
```c
        w->rect = (struct ui_rect){92 + offset, 72 + offset, s->width - 152, s->height - 144};  // 新窗口从屏幕左上留92和72像素并按已有数量错开，宽高先扣固定边距
        if (w->rect.w < 720)  // 初始窗口宽度至少720像素，画布尺寸后续再约束
            w->rect.w = 720;  // 首次打开宽度至少720像素；真正绘制时再按画布收窄
        if (w->rect.h < 480)  // 初始窗口高度至少480像素，画布尺寸后续再约束
            w->rect.h = 480;  // 首次打开高度至少480像素；真正绘制时再按画布收窄
        w->restore = w->rect;  // 保留当前普通窗口几何，退出最大化时可恢复
        w->open = true;  // 该应用窗口现在进入已打开状态
```
<!-- after -->
```c
        w->rect = (struct ui_rect){92 + offset, 72 + offset, s->width - 152, s->height - 144};  // 新窗口从屏幕左上留92和72像素并按已有数量错开，宽高先扣固定边距
        if (w->rect.w < 720)  // 初始窗口宽度至少720像素，画布尺寸后续再约束
            w->rect.w = 720;  // 初次打开宽度至少720像素；这里建立初始布局，ui_draw再按实际画布收窄
        if (w->rect.h < 480)  // 初始窗口高度至少480像素，画布尺寸后续再约束
            w->rect.h = 480;  // 初次打开高度至少480像素；当前画布不足时ui_draw再收窄可见矩形
        w->restore = w->rect;  // 保留当前普通窗口几何，退出最大化时可恢复
        w->open = true;  // 该应用窗口现在进入已打开状态
```

最后在窗口置顶后清除字段焦点，后续按键才会回到新活动应用。紧邻的 `ui_service_state` 仍先改变服务状态，再调用同一个 `ui_refresh`。

<!-- replace: frontend/core/state.c -->
<!-- before -->
```c
    s->order[n++] = (unsigned)app;  // 把当前应用放到绘图顺序末尾，使它成为顶层
    s->order_count = n;  // 记录去重后绘图顺序的有效长度
}

void ui_service_state(struct ui_desktop *s, enum ui_state state) {  // 切换服务显示状态并刷新相应快照
    s->service_state = state;  // 用调用者指定状态切换服务反馈，随后刷新决定是否继续查询快照
    ui_refresh(s);  // 按新服务状态重新查询；非正常状态留下0条有效元信息
}

```
<!-- after -->
```c
    s->order[n++] = (unsigned)app;  // 把当前应用放到绘图顺序末尾，使它成为顶层
    s->order_count = n;  // 记录去重后绘图顺序的有效长度
    s->focus = UI_FOCUS_NONE;  // 当前字段结束接收，后续按键回到活动应用
}

void ui_service_state(struct ui_desktop *s, enum ui_state state) {  // 切换服务显示状态并刷新相应快照
    s->service_state = state;  // 用调用者指定状态切换服务反馈，随后刷新决定是否继续查询快照
    ui_refresh(s);  // 以当前directory重新复制文件快照；任务回调接入后同次刷新它和资源统计
}

```

初始化已经让 editor、saved 和 editor_name 对应 main.lua。绘制窗口外框时，Editor 的身份标签右侧也要显示这个名字。新增行为放在原有窗口遍历中：画完 `heads[app]` 后，若应用为 `UI_EDITOR` 且外框宽度大于 500，就画分隔符和文档名；随后继续原来的 Terminal 标题与窗口按钮。

标题仍使用已有裁剪区，长名称不会进入系统按钮区域。在 paint.c 中定位标题绘制处，用新块替换这一小段，其他内容保留。旧块用于定位，只把新块写回该位置。

**更新 `frontend/core/paint.c` 的标题绘制：**

<a id="code-frontend-core-paint-c-1"></a>


身份标签仍按 u_label 的字距绘制，文件名使用普通文字的推进量。下面保留原身份标签这一行，在它后面接入当前文件名；宽度条件仍使用外框 r.w。

<!-- replace: frontend/core/paint.c -->
<!-- before -->
```c
        u_label(r.x + 62, r.y + 14, heads[app], U_INK);  // 身份标签使用14像素Medium字形和额外字距，与普通正文区别
```
<!-- after -->
```c
        u_label(r.x + 62, r.y + 14, heads[app], U_INK);  // 身份标签使用14像素Medium字形和额外字距，与普通正文区别
        if (app == UI_EDITOR && r.w > 500) {  // 宽编辑器在标题后保留当前对象
            u_text(r.x + 196, r.y + 14, "/", U_BODY, U_MUTED);  // 应用与文档层次分明
            u_text(r.x + 220, r.y + 14, s->editor_name, U_MEDIUM, U_TEXT);  // 文件名保持正常大小写
        }
```

## 将快照下标转换为可见行

<a id="editor-frame"></a>

Editor的44像素工具栏下方再用32像素文档条标明main.lua。正文区从内容顶下88像素开始；Files首个列表行则在内容顶下138像素。两个应用共享窗口标题和边框，却各自定义正文起点，点击时必须继续采用各自的转换。u_selection把列表选中状态画成淡底和三像素粉色侧标；代码选区则仍按每字节十像素铺底。

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

索引表已经给出 `[3,2,0,5,1,4]`，Files 接下来按这个次序取元信息，再扣除 scroll 决定画到哪一行。初始化的 selection_file 为 0，所以被选中的 main.lua 会出现在排序后的第三行；绘图通过原快照下标比较选择，排序不会把选择转给第一行的 output。

应用接收的是已经扣除窗口标题的 body。先画路径、工具栏、筛选字段与列标题；body 宽度至少 800 时保留 176 像素侧栏，窄窗口把这块空间还给列表。服务不可用时在此处给出提示并结束，已有服务时才继续画排序后的记录。

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
        return;  // 目录服务状态不可用或缺查询回调，保留提示且不读文件条目
    }
    u_rect(x, r.y + 106, w, 28, U_PANEL);     // 用工具栏底色写矩形，起点(x,r.y + 106)，尺寸w×28像素
    ui_app_muted(x + 12, r.y + 112, "Name");  // 在整屏x=x + 12、内容顶边下移112像素显示文字"Name"
    ui_app_muted(x + w - 218, r.y + 112, "Type");  // 在整屏x=x + w - 218、内容顶边下移112像素显示文字"Type"
    ui_app_muted(x + w - 102, r.y + 112, "Size");  // 在整屏x=x + w - 102、内容顶边下移112像素显示文字"Size"
```

路径和表头已经占好位置，现在才消费本次排序表。循环从 order[k] 取回快照下标 i，再借用 files[i] 的名字、大小和 id；滚动只跳过显示行。每行先按 selection_file 画底色，再写图标与文字。文件名使用自己的列裁剪，画完恢复 body，类型和大小因此仍能写到右侧两列。

**继续追加到 `frontend/apps/files.c`，完成同一个 `ui_files_draw` 函数：**

<!-- append: frontend/apps/files.c -->
```c
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

Editor 从自己的 editor 数组读取 main.lua，Files 中哪一行被选中不会改变这次绘制的文字。先铺工具栏与文档条，再按窗口宽度决定是否保留 190 像素项目栏；项目栏仍按服务返回次序列文件，并用 editor_id 标出当前文档。

正文原点从 body 顶部下移 88 像素，查找栏打开再下移 44 像素。绘制每行时，idx 保存该行首字节在整篇文档中的偏移，可见行 y 每次加 24 像素，文字 tx 从本行左缘减去水平滚动量。空行也保留一行高度；当前 ASCII 等宽字符按 10 像素一列定位。最后状态栏另算 ln/col，并以从 1 开始的行列显示。

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
```

工具栏和正文裁剪已经确定，接下来沿同一份 editor 扫描。p 指向当前行首，end 找到换行或 NUL；q 只遍历这段行内字节。`idx+(q-p)` 因而把当前字符换回整篇文档偏移，既能判断是否处于选区，也能判断光标是否在它之前。每行先铺选区底色，再画字形；注释、引号和数字只决定这次颜色，不执行文档中的代码。

**继续追加到 `frontend/apps/editor.c`，写出同一个函数中的文档遍历：**

<!-- append: frontend/apps/editor.c -->
```c
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
```

一行结束后，idx 先计入本行字节，再单独计入换行，p 才转到下一行。末行没有换行时直接结束；光标位于行尾时仍有独立竖线，因此 ABCD 后面的第 4 间隙也能显示。下面补空文档提示，再恢复 body 裁剪并填写状态栏，函数到最后一行才闭合。

**继续追加到 `frontend/apps/editor.c`，完成空文档和状态栏：**

<!-- append: frontend/apps/editor.c -->
```c
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

观察程序沿用 G15 的 main，每次启动都会重新初始化 reference、console 和 desktop。`ui_init` 已先读 main.lua，命令行参数再决定将哪个窗口置顶并最大化：1 选择 Files，2 选择 Editor。两个进程各有自己的快照与文档数组，前一次退出后不会把它们传给下一次。

各次仍输出同一个 build/desktop.ppm，所以每次生成后立即复制到对应文件名，再查看两份画面。编译命令中的两个通配符由 Shell 展开，参与链接的仍是当前已经写出的核心与应用文件。

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

Files 图中，目录 output 排在最前，main.lua 仍由其服务身份确定；Editor 图中，标题、文档条与正文都来自本进程读到的 main.lua。当前数组没有经历按键修改，editor 与 saved 仍相同。下一章让按键改变工作数组，再让保存将这些新字节提交给服务。
