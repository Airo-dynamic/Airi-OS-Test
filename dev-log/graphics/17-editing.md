# G17 · 把一次按键变成可回退的文档修改

G16 已经把服务中的 main.lua 读到 Editor，窗口能显示这份正文。现在让同一个桌面接收全选、粘贴、撤销和保存：将原文替换为 `print(42)` 加换行，撤销一次，再恢复这次替换，最后把新文交回服务。按键需要改变的是可以继续使用的文档状态，重画只负责把这些状态显示出来。

服务仍保留自己的正文，桌面里的 `editor` 是独立工作数组，`saved` 是最近一次完整读取或成功保存留下的副本。输入先改 editor，保存才调用 G14 建立的 `backend.action`。回调返回 true 后，前端才能把 saved 更新为当前 editor；服务拒绝时，工作文字仍须留在窗口中。

撤销还需要另一份修改前的文字。新增的 `undo` 保存这份快照，`clipboard` 保存本次要粘贴的输入。undo 与 editor 交换可以撤销，也可以把刚撤销的文字换回来；saved 始终用于判断工作文字是否已经保存。这样，回到原文会重新成为干净状态，保存新文以后，撤销也仍能把原文带回来。

<a id="edit-state-model"></a>

本次输入共有十个内容字节：`print(42)` 的九个字符和一个换行。结尾 NUL 另占一个存储位置。沿观察程序的实际顺序，四份正文会经过以下状态；表中的“原文”均指本次初始化读入的同一份 main.lua。

| 操作完成后 | editor 工作文字 | undo 撤销槽 | saved 保存点 | 服务正文 | dirty |
| --- | --- | --- | --- | --- | --- |
| 初始化 | 原文 | 尚无有效快照 | 原文 | 原文 | false |
| 全选后粘贴 | 十字节新文 | 原文 | 原文 | 原文 | true |
| 第一次 Undo | 原文 | 十字节新文 | 原文 | 原文 | false |
| 第二次 Undo | 十字节新文 | 原文 | 原文 | 原文 | true |
| Save 成功 | 十字节新文 | 原文 | 十字节新文 | 十字节新文 | false |

随后请求 Run，会显示执行服务未连接的错误。它不回退刚才成功的保存。这个主机观察进程中的参考服务把文件放在会话内存中，进程退出后这些修改就结束；本章先接通按键、文档副本、服务提交和可见反馈之间的关系。

## 为一次编辑分别保存文字、输入目标与反馈

下面更新共同头文件，使随后各实现都使用同一套字段和功能键编号。原有窗口矩形、绘制次序、主题和目录快照继续承担原来的用途。新增部分可以沿三组状态阅读：editor/saved/undo/clipboard 保存文字；dialog/focus/active 决定同一个键交给谁；dialog_title/text/input 与 toast 留下下一帧要显示的反馈。`pending_close` 则把“先保存再关闭”的后续动作留到服务结果返回以后。

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
enum ui_action {  // 回调接收的服务动作，同一动作具有明确文件或任务目标。
    UI_SAVE,  // 将当前字节写回文档id对应记录。
    UI_CREATE_FILE,  // 在显式目录下建立普通文件。
    UI_CREATE_DIR,   // 建立目录记录。
    UI_RENAME,     // 保留id，修改对应记录名称。
    UI_DELETE,     // 删除id对应记录。
    UI_EXECUTE,    // 请求执行文档，当前参考服务报告未连接。
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
    unsigned theme;    // 桌面自己持有的日夜模式，文本会话不依赖此字段。
    unsigned density;  // Files列表行高，0为36像素、非0为28像素；当前绘图读取此值。
};
void ui_init(struct ui_desktop *, struct ui_backend, struct airi_console *);  // 初始化桌面，复制服务表并借用命令对象。
void ui_open(struct ui_desktop *, enum ui_app);  // 打开、解除最小化并置顶指定应用。
void ui_draw(struct ui_desktop *, const struct airi_framebuffer *);  // 按实际帧缓冲绘图，并更新可用于命中的窗口矩形。
bool ui_key(struct ui_desktop *, int key, bool shift);      // 分派字符或功能键，Shift控制扩展选区等规则。
void ui_service_state(struct ui_desktop *, enum ui_state);  // 切换服务显示状态并刷新对应快照。
void ui_refresh(struct ui_desktop *);  // 从当前目录取得最多32条文件元信息副本。
void ui_command(struct ui_desktop *, enum ui_action, int, const char *, const char *);  // 把稳定身份、名称与内容交给服务动作回调。
void ui_fixture_backend(struct ui_backend *);  // 为主机观察填入会话内存参考服务，不进入guest。

void u_icon(int, int, int, uint32_t);  // 在给定坐标以类型和颜色绘制小图标。
unsigned ui_file_order(const struct ui_desktop *, unsigned *);  // 把当前文件排序位置写成快照下标数组并返回数量。
void ui_open_document(struct ui_desktop *, unsigned);  // 接收快照下标，按服务id读取普通文档并打开Editor。
void ui_apps_draw(struct ui_desktop *, enum ui_app, struct ui_rect);  // 以应用身份分派内容绘图，rect已扣除标题栏。
#endif  // 结束完整桌面接口的重复包含保护
```

## 服务接受正文以后，保存才算成功

<a id="edit-save-run"></a>

工作数组的声明已经齐备，接下来先完成保存将调用的服务端。沿章首的 Save：前端交出 `editor_id=1`、名称 main.lua 和 editor 中的十字节；参考服务用 `id-1` 定位原槽，把正文复制进服务自己的 text，再更新 bytes。这里的 id 仍是服务身份，与 Files 的可见行号无关。

`UI_EXECUTE` 在修改记录前返回未连接原因。创建、重命名、复制、移动与删除也共用 action 入口，但 name 与 id 的含义随动作确定。下面先恢复记录容器、目录查询和完整读取，再逐段完成 action；本次保存会走已有 id 的 UI_SAVE 分支。

**完整写入 `frontend/backends/reference.c`：**

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
                    break;  // 结束当前最近一层循环或应用分派，不执行其剩余候选
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
    return false;  // 失败原因已写出，请求方据false显示错误而不更新保存点
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
```

目录查询仍复制元信息，read 仍在容量足够时复制整份正文，失败前不写调用者数组。action 在这份服务状态上接受修改：执行请求先拒绝；创建和 `editor_id<0` 的新文档保存先检查名称、完整路径与重名，再寻找空槽。新文档保存把 data 写入新槽，普通创建则从空正文开始。当前 main.lua 已有 id，稍后会越过这组创建分支。

<!-- append: frontend/backends/reference.c -->
```c
static bool action(void *unused, const char *directory, enum ui_action act, int id, const char *name, const char *data,  // 校验请求后改变服务记录，错误留在输出缓冲
                   char *out, size_t cap) {  // 错误正文写到调用者out数组，cap是该数组的字节容量，服务不保留此地址
    (void)unused;  // 此参考服务使用静态表，回调合同的上下文参数本实现不读取
    if (act == UI_EXECUTE)  // 执行请求暂不启动主机或guest程序，返回真实未连接错误
        return error(out, cap,  // 把失败原因写给调用者，false阻止前端误记为成功
                     "Application execution is not connected. The source stays in the editor; no "  // 继续当前固定文字；转义换行属于文档内容，C相邻字符串在编译时连接
                     "guest or host process was started.");  // 结束未连接错误正文：本请求既未启动guest程序也未启动主机程序
    if (act == UI_CREATE_FILE || act == UI_CREATE_DIR || (act == UI_SAVE && id < 0)) {  // 创建项目和未绑定id的另存都需要新槽；已存在文档的保存后面按id处理
        if (!valid_name(name))  // 先检查名称形状，失败时不会拼路径或占用服务槽
            return error(out, cap, "Use a non-empty file name, without slashes, up to 63 bytes.");  // 拒绝本次服务修改并向调用者报告：Use a non-empty file name, without slashes, up to 63 bytes
        char target[128];  // 先在局部数组形成完整目标路径，通过校验后才改服务
        int length = airi_snprintf(target, 128, "%s/%s", directory, name);  // 先拼当前目录与新名称；返回完整所需内容字节数，用于发现128字节数组的截断
        if (length >= 128)  // 所需内容已占满数组，NUL没有位置，拒绝截断路径以免误指另一文件
            return error(out, cap, "Target path is too long.");  // 拒绝本次服务修改并向调用者报告：Target path is too long
        for (unsigned i = 0; i < UI_FILES; ++i)  // 逐个检查服务32槽，查询或创建受当前占用状态约束
            if (entries[i].used && !airi_memcmp(entries[i].entry.path, target, (size_t)length + 1))  // 只比较已占用槽的完整目标路径，长度加1连NUL一起匹配，重名不覆盖原记录
                return error(out, cap, "An item with that name already exists.");  // 拒绝本次服务修改并向调用者报告：An item with that name already exists
        for (unsigned i = 0; i < UI_FILES; ++i)  // 逐个检查服务32槽，查询或创建受当前占用状态约束
            if (!entries[i].used) {  // 找到空服务槽后才能新建记录
                add(i, directory, name, act == UI_CREATE_DIR, act == UI_SAVE ? data : "");  // 新建槽按操作决定目录类型，新文档保存时填正文，其余创建为空
                return true;  // 该服务修改已完整提交，前端可重新查询当前记录
            }
        return error(out, cap, "Reference workspace capacity reached (32 items).");  // 拒绝本次服务修改并向调用者报告：Reference workspace capacity reached (32 items)
    }
```

创建分支结束后，其余动作必须指向一个仍占用的服务 id。复制、移动把 name 解释为目标目录，保留源文件名；目录复制被拒绝，普通复制另占槽并取得新 id，移动只改原槽路径而保留 id。保存当前 main.lua 会越过它们，进入后面的 UI_SAVE。继续追加这些分支。

<!-- append: frontend/backends/reference.c -->
```c
    if (id < 1 || id > UI_FILES || !entries[id - 1].used)  // 拒绝1到32之外或已删除的服务id，检查完成前不访问id-1槽
        return error(out, cap, "The item no longer exists. Refresh the directory.");  // 拒绝本次服务修改并向调用者报告：The item no longer exists. Refresh the directory
    struct reference_file *f = &entries[id - 1];  // id已验证，减1找到对应服务槽
    if (act == UI_COPY_FILE || act == UI_MOVE_FILE) {  // 这两种操作使用name作为目标目录，并保留原普通文件名称与正文
        if (f->entry.directory)  // 目录没有普通文档内容，此操作需要拒绝
            return error(out, cap, "Directory copy requires a recursive filesystem provider.");  // 拒绝本次服务修改并向调用者报告：Directory copy requires a recursive filesystem provider
        char target[128];  // 先在局部数组形成完整目标路径，通过校验后才改服务
        int length = airi_snprintf(target, 128, "%s/%s", name, f->entry.name);  // name在复制移动操作中是目标目录；保持原文件名，先检查组合路径能否完整保存
        if (length >= 128)  // 所需内容已占满数组，NUL没有位置，拒绝截断路径以免误指另一文件
            return error(out, cap, "Target path is too long.");  // 拒绝本次服务修改并向调用者报告：Target path is too long
        for (unsigned i = 0; i < UI_FILES; ++i)  // 逐个检查服务32槽，查询或创建受当前占用状态约束
            if (entries[i].used && !airi_memcmp(entries[i].entry.path, target, (size_t)length + 1))  // 只比较已占用槽的完整目标路径，长度加1连NUL一起匹配，重名不覆盖原记录
                return error(out, cap, "The target already contains an item with that name.");  // 拒绝本次服务修改并向调用者报告：The target already contains an item with that name
        if (act == UI_MOVE_FILE) {  // 普通文件移动保留id，只修改完整路径
            airi_snprintf(f->entry.path, 128, "%s", target);  // 校验通过后提交新完整路径，文件服务id与正文保持不变
            return true;  // 该服务修改已完整提交，前端可重新查询当前记录
        }
        for (unsigned i = 0; i < UI_FILES; ++i)  // 逐个检查服务32槽，查询或创建受当前占用状态约束
            if (!entries[i].used) {  // 找到空服务槽后才能新建记录
                add(i, name, f->entry.name, false, f->text);  // 复制普通文件到目标目录name，以同一名称和独立正文创建新id
                return true;  // 该服务修改已完整提交，前端可重新查询当前记录
            }
        return error(out, cap, "Reference workspace capacity reached.");  // 拒绝本次服务修改并向调用者报告：Reference workspace capacity reached
    }
```

现在接到本轮 Save 真正经过的分支。id 已通过检查，普通文档才接受 data，目录不能当正文覆盖；文本复制完成后，bytes 依据服务实际存下的内容重新计算，再返回 true。随后删除与重命名也先验证再改记录：非空目录拒绝删除，目录重命名尚不支持，普通文件的新名称不覆盖其他已有目标。继续追加到 action 结束。

<!-- append: frontend/backends/reference.c -->
```c
    if (act == UI_SAVE) {  // 保存请求只修改这个服务记录的文字与长度
        if (f->entry.directory)  // 目录没有普通文档内容，此操作需要拒绝
            return error(out, cap, "Cannot write a directory as a document.");  // 拒绝本次服务修改并向调用者报告：Cannot write a directory as a document
        airi_snprintf(f->text, sizeof(f->text), "%s", data);  // 把此次保存正文复制到服务的8192字节数组，前端输入合同已将正文限制为8191字节
        f->entry.bytes = (unsigned)airi_strlen(f->text);  // 保存后以实际存储文字长度更新快照中的字节数
        return true;  // 该服务修改已完整提交，前端可重新查询当前记录
    }
    if (act == UI_DELETE) {  // 删除请求先检查目录是否仍含后代记录
        if (f->entry.directory) {  // 删除目录前检查全部服务槽，当前实现只允许删除没有后代的目录
            size_t len = airi_strlen(f->entry.path);  // 当前待删目录路径的字节长度，稍后用它定位后代路径中的斜杠边界
            for (unsigned i = 0; i < UI_FILES; ++i)   // 逐个检查服务32槽，查询或创建受当前占用状态约束
                if (entries[i].used && airi_strlen(entries[i].entry.path) > len &&  // 只检查占用槽且路径比待删目录长的项，目录自己不算自己的后代
                    !airi_memcmp(entries[i].entry.path, f->entry.path, len) &&  // 父路径字节必须完整相等，随后还需检查真正的斜杠层级边界
                    entries[i].entry.path[len] == '/')  // 相同前缀后还必须紧接斜杠，避免把projects2误当projects的后代
                    return error(out, cap, "The folder is not empty. Remove its contents first.");  // 拒绝本次服务修改并向调用者报告：The folder is not empty. Remove its contents first
        }
        f->used = false;  // 删除仅释放服务槽，之后查询不再返回此记录
        return true;      // 该服务修改已完整提交，前端可重新查询当前记录
    }
    if (act == UI_RENAME) {  // 重命名先验证目标名称与完整路径，再更新服务元信息
        if (airi_strlen(f->entry.name) == airi_strlen(name) &&  // 名称长度相同后才比较全部字节，同名重命名无需改服务记录
            !airi_memcmp(f->entry.name, name, airi_strlen(name) + 1))  // 连NUL比较当前名字，完全相同的重命名视为已完成
            return true;  // 该服务修改已完整提交，前端可重新查询当前记录
        if (!valid_name(name))  // 先检查名称形状，失败时不会拼路径或占用服务槽
            return error(out, cap, "Invalid file name.");  // 拒绝本次服务修改并向调用者报告：Invalid file name
        if (f->entry.directory)  // 目录重命名还需同步改变全部后代路径，本参考服务不实现该操作
            return error(out, cap, "Folder renaming requires the future VFS provider.");  // 拒绝本次服务修改并向调用者报告：Folder renaming requires the future VFS provider
        char target[128];  // 先在局部数组形成完整目标路径，通过校验后才改服务
        airi_snprintf(target, sizeof(target), "%s", f->entry.path);  // 将原完整路径复制到128字节临时数组，随后只替换最后名称
        size_t parent = airi_strlen(target);  // 先从原路径文尾开始向前寻找最后斜杠，找到后保留其前面的父目录
        while (parent && target[parent - 1] != '/')  // 从原路径末尾向前找最后斜杠，保留父目录前缀
            --parent;  // 从路径尾部向前寻找最后一个目录分隔符
        int written = airi_snprintf(target + parent, sizeof(target) - parent, "%s", name);  // 在父目录后的剩余空间写新名称，返回未截断名称的所需字节数
        if (written < 0 || (size_t)written >= sizeof(target) - parent)  // 新名称未完整装入剩余空间时拒绝重命名，服务原路径仍保留
            return error(out, cap, "Target path is too long.");  // 拒绝本次服务修改并向调用者报告：Target path is too long
        size_t length = airi_strlen(target);     // 校验通过后的新完整路径长度，不含NUL，后续重名比较会加1
        for (unsigned i = 0; i < UI_FILES; ++i)  // 逐个检查服务32槽，查询或创建受当前占用状态约束
            if (entries[i].used && !airi_memcmp(entries[i].entry.path, target, length + 1))  // 把新完整路径连NUL与已占用槽比较，完全同名时拒绝覆盖已有文件
                return error(out, cap, "An item with that name already exists.");  // 拒绝本次服务修改并向调用者报告：An item with that name already exists
        airi_snprintf(f->entry.path, 128, "%s", target);  // 校验通过后提交新完整路径，文件服务id与正文保持不变
        airi_snprintf(f->entry.name, 64, "%s", name);     // 复制单项名称到服务元信息，64字节数组最多容纳63内容字节与NUL
        return true;  // 该服务修改已完整提交，前端可重新查询当前记录
    }
    return error(out, cap, "Unsupported workspace operation.");  // 拒绝本次服务修改并向调用者报告：Unsupported workspace operation
}
```

动作处理已有定义，最后追加本章完整初始化。它仍重置参考文件和示例 boot，再把当前 action 的函数地址放进 backend。下面的 main.lua、CSV 和 C 文件字符串是服务保存的数据，不作为本次主机程序执行。

<!-- append: frontend/backends/reference.c -->
```c
void ui_fixture_backend(struct ui_backend *b) {  // 重置当前主机参考服务并填入回调表
    airi_memset(entries, 0, sizeof(entries));  // 把服务拥有的32个槽清空，再填本会话初始参考文件
    add(0, "/home/airi/projects/telemetry", "main.lua", false,  // 向明确编号的服务槽填这份路径、名称、目录标记与正文
        "-- Read a series and write a summary\nlocal values = {12, 18, 21, 32, 29, 37}\n\nlocal "  // 继续当前固定文字；转义换行属于文档内容，C相邻字符串在编译时连接
        "sum = 0\nfor _, value in ipairs(values) do\n    sum = sum + value\nend\n\nlocal mean = "  // 继续当前固定文字；转义换行属于文档内容，C相邻字符串在编译时连接
        "sum / #values\nprint(string.format(\"mean: %.2f\", mean))\n\n-- Run requires the future "  // 继续当前固定文字；转义换行属于文档内容，C相邻字符串在编译时连接
        "application service.\n-- This reference workspace only edits the source.\n");  // 初始main.lua末尾说明Run尚缺应用执行服务，当前只提供正文编辑
    add(1, "/home/airi/projects/telemetry", "series.csv", false,  // 向明确编号的服务槽填这份路径、名称、目录标记与正文
        "seconds,load\n0,12\n1,18\n2,21\n3,32\n4,29\n5,37\n");  // 初始CSV含一行标题与6组时间秒数、测量整数；当前先作为普通文字存服务
    add(2, "/home/airi/projects/telemetry", "README.md", false,  // 向明确编号的服务槽填这份路径、名称、目录标记与正文
        "# Telemetry workspace\n\nThis is a frontend reference dataset.\nSave changes update this "  // 继续当前固定文字；转义换行属于文档内容，C相邻字符串在编译时连接
        "session's memory only.\nNative application execution is intentionally unconnected.\n");  // README明确保存只改当前进程内存，执行服务尚未连接
    add(3, "/home/airi/projects/telemetry", "output", true, "");  // 向明确编号的服务槽填这份路径、名称、目录标记与正文
    add(6, "/home/airi/projects/telemetry", "sum.c", false,  // 向明确编号的服务槽填这份路径、名称、目录标记与正文
        "#include <stdio.h>\n\nint main(void) {\n    const int values[] = {12, 18, 21, 32};\n    "  // 继续当前固定文字；转义换行属于文档内容，C相邻字符串在编译时连接
        "int total = 0;\n    for (unsigned i = 0; i < 4; ++i)\n        total += values[i];\n    "  // 继续当前固定文字；转义换行属于文档内容，C相邻字符串在编译时连接
        "printf(\"sum: %d\\n\", total);\n    return 0;\n}\n");  // 存入示例C文件最后打印与返回正文，这段字符串是数据而不是本次参与编译的C程序
    add(7, "/home/airi/projects/telemetry", "project.airi", false,  // 向明确编号的服务槽填这份路径、名称、目录标记与正文
        "[project]\nname = telemetry\nentry = main.lua\n");  // 存入项目说明文字，包含名字与入口；当前尚未把它当配置装载
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
    *b = (struct ui_backend){.mode = UI_REFERENCE,  // 通过b写调用者的整份回调表，函数地址交给files/read/action，未指定context字段自动为NULL
                             .boot = &boot,   // 设置借用启动信息，描述与内存地图须持续有效
                             .files = files,  // 保存目录查询函数地址，前端提供目录与数组，回调复制元信息而不出借服务槽
                             .read = read,    // 服务合同保存完整正文读取函数地址，桌面传服务id与调用者数组
                             .action = action};  // 服务合同保存操作函数地址，修改请求与失败正文通过该回调交接
}
```

## 将调用中的错误保存到下一帧

<a id="dialog-lifetime"></a>

服务已经能接受或拒绝动作，前端还需要把结果留下来。action 使用调用者提供的 error 数组交回原因；这个局部数组只在本次调用中有效，而提示框要到之后重画时才读取文字。

`ui_app_dialog` 因而把标题、原因和输入初值复制进 desktop，`ui_command` 的保存失败分支也写入这组持久数组。后续绘图读取 desktop 中的副本，原局部 error 结束寿命后提示仍然有效。错误框最后写像素以覆盖普通窗口，按键入口先检查 dialog；Escape 关闭反馈以后，输入才恢复到 Editor。

**完整写入 `frontend/apps/internal.h`：**

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
void ui_app_dialog(struct ui_desktop *s, int kind, int target, const char *title, const char *body,  // 把种类、目标身份和文案保存到桌面对话框状态
                   const char *value);  // value是对话框输入字段的初始文字，函数只复制状态
void ui_app_unavailable(struct ui_desktop *s, struct ui_rect r, const char *service);  // 按服务状态绘制加载、空或不可用提示
void ui_app_toolbar(struct ui_rect r, const char *label);  // 在应用内容顶部绘制44像素工具栏
void ui_app_status(struct ui_rect r, const char *left, const char *right);  // 在应用内容底部绘制27像素状态栏
void ui_terminal_draw(struct ui_desktop *, struct ui_rect);  // 把console逻辑行拆成窗口显示行并绘制输入
void ui_files_draw(struct ui_desktop *, struct ui_rect);     // 读取目录快照和排序索引，画路径、文件行和状态
void ui_editor_draw(struct ui_desktop *, struct ui_rect);    // 将文档字节偏移换成行列，画文字、选区和光标
void ui_system_draw(struct ui_desktop *, struct ui_rect);    // 按当前页显示同一启动信息的内存、设备或日志
#endif  // 结束当前应用接口的重复包含保护
```

common.c 继续提供原有文字、字段、工具栏、状态栏与图标；此次新增 ui_app_dialog，保存反馈所需的独立文案。下面按本检查点完整内容更新，先写已有文字和匹配函数，再接对话框状态。

<a id="code-frontend-apps-common-c-1"></a>

<!-- write: frontend/apps/common.c -->
```c
#include "internal.h"  // 引入当前目录内部共享接口和基础函数

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

追加 ui_app_dialog 时只复制状态，不写像素。kind 决定下一帧画哪一种框，target 固定确认动作的身份；title、body、value 分别复制进 64、256、128 字节数组。field_all=true 表示名称初值被整体选中。调用返回后，原来的临时 error 即使失效，桌面副本仍可继续使用。

<!-- append: frontend/apps/common.c -->
```c
void ui_app_dialog(struct ui_desktop *s, int kind, int target, const char *title, const char *body,  // 把种类、目标身份和文案保存到桌面对话框状态
                   const char *value) {  // value提供确认输入的初值，按字段容量复制
    s->dialog = kind;  // 保存对话框种类，在普通窗口之后绘制；输入分派优先检查此状态
    s->dialog_target = target;  // 保存本次确认将使用的文件id或任务pid
    s->field_all = true;  // 输入字段内容设为整体选中，下一次键入可以替换
    airi_snprintf(s->dialog_title, 64, "%s", title);   // 复制此次对话框标题到桌面独立64字节数组，不借用调用者临时指针
    airi_snprintf(s->dialog_text, 256, "%s", body);    // 复制具体反馈正文到256字节数组，对话框后续重画仍可读取
    airi_snprintf(s->dialog_input, 128, "%s", value);  // 复制名称框初值到128字节独立数组，之后键入只改此数组
}

```

继续追加既有服务提示、工具栏、状态栏和图标。它们仍只按当前状态画外观；新的对话框文案已由 desktop 保存，接下来在整帧结尾消费。

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

void u_icon(int x, int y, int id, uint32_t c) {  // 按应用编号绘制20像素附近的几何图标
    /* Original, simple geometric symbols. Icons remain recognisable at 20 px. */
    switch (id) {      // 按应用编号选择几何图标，不由像素触发应用动作
    case UI_TERMINAL:  // 按Terminal应用和windows数组下标0进入对应分支
        u_frame((struct ui_rect){x, y + 2, 20, 16}, c);  // 沿当前显式矩形的四边画一像素边框
        u_line(x + 4, y + 6, x + 7, y + 9, c);      // 用传入图标色连接像素端点(x + 4,y + 6)到(x + 7,y + 9)
        u_line(x + 7, y + 9, x + 4, y + 12, c);     // 用传入图标色连接像素端点(x + 7,y + 9)到(x + 4,y + 12)
        u_line(x + 10, y + 13, x + 15, y + 13, c);  // 用传入图标色连接像素端点(x + 10,y + 13)到(x + 15,y + 13)
        break;  // 结束当前最近一层循环或应用分派，不执行其剩余候选
    case UI_FILES_APP:  // 按Files应用和windows数组下标1进入对应分支
        u_line(x, y + 5, x + 8, y + 5, c);  // 用传入图标色连接像素端点(x,y + 5)到(x + 8,y + 5)
        u_line(x + 8, y + 5, x + 10, y + 8, c);   // 用传入图标色连接像素端点(x + 8,y + 5)到(x + 10,y + 8)
        u_line(x + 10, y + 8, x + 20, y + 8, c);  // 用传入图标色连接像素端点(x + 10,y + 8)到(x + 20,y + 8)
        u_frame((struct ui_rect){x, y + 8, 21, 11}, c);  // 沿当前显式矩形的四边画一像素边框
        break;  // 结束当前最近一层循环或应用分派，不执行其剩余候选
    case UI_EDITOR:  // 按Editor应用和windows数组下标2进入对应分支
        u_frame((struct ui_rect){x + 3, y, 15, 21}, c);  // 沿当前显式矩形的四边画一像素边框
        for (int r = 5; r < 18; r += 4)  // 每4像素画一条Editor图标内的文字短横线
            u_line(x + 6, y + r, x + 14, y + r, c);  // 用传入图标色连接像素端点(x + 6,y + r)到(x + 14,y + r)
        break;  // 结束当前最近一层循环或应用分派，不执行其剩余候选
    case UI_TASKS_APP:  // 按Tasks应用和windows数组下标3进入对应分支
        for (int i = 0; i < 4; ++i)  // 画Tasks图标的4根柱，每柱右移5像素、高度依次增加4像素
            u_rect(x + i * 5, y + 18 - i * 4, 3, i * 4 + 3, c);  // 用传入图标色写矩形，起点(x + i * 5,y + 18 - i * 4)，尺寸3×i * 4 + 3像素
        break;  // 结束当前最近一层循环或应用分派，不执行其剩余候选
    case UI_SYSTEM:  // 按System应用和windows数组下标4进入对应分支
        u_frame((struct ui_rect){x + 2, y + 2, 17, 13}, c);  // 沿当前显式矩形的四边画一像素边框
        u_line(x + 10, y + 15, x + 10, y + 19, c);  // 用传入图标色连接像素端点(x + 10,y + 15)到(x + 10,y + 19)
        u_line(x + 5, y + 19, x + 15, y + 19, c);   // 用传入图标色连接像素端点(x + 5,y + 19)到(x + 15,y + 19)
        break;  // 结束当前最近一层循环或应用分派，不执行其剩余候选
    case UI_SETTINGS:  // 按Settings应用和windows数组下标5进入对应分支
        for (int i = 0; i < 3; ++i) {  // Settings图标按三条滑轨生成标记，逐项下移6像素
            u_line(x, y + 4 + i * 6, x + 20, y + 4 + i * 6, c);  // 用传入图标色连接像素端点(x,y + 4 + i * 6)到(x + 20,y + 4 + i * 6)
            u_rect(x + 4 + i * 4, y + 1 + i * 6, 3, 7, c);  // 用传入图标色写矩形，起点(x + 4 + i * 4,y + 1 + i * 6)，尺寸3×7像素
        }
        break;  // 结束当前最近一层循环或应用分派，不执行其剩余候选
    default:  // 其他编号进入当前switch的默认结果
        u_frame((struct ui_rect){x, y, 21, 21}, c);  // 沿当前显式矩形的四边画一像素边框
        u_line(x + 3, y + 17, x + 9, y + 8, c);      // 用传入图标色连接像素端点(x + 3,y + 17)到(x + 9,y + 8)
        u_line(x + 9, y + 8, x + 14, y + 14, c);     // 用传入图标色连接像素端点(x + 9,y + 8)到(x + 14,y + 14)
        u_line(x + 14, y + 14, x + 18, y + 6, c);    // 用传入图标色连接像素端点(x + 14,y + 14)到(x + 18,y + 6)
        break;  // 结束当前最近一层循环或应用分派，不执行其剩余候选
    }
}

```

## 在窗口绘完以后覆盖反馈

对话框文字现在已经保存在 desktop 中，显示发生在下一次 `ui_draw`。先沿用原来的窗口遍历画正文，再在整屏范围读取 toast 与 dialog 并覆盖反馈。窗口本身的文字和几何保持原状态，关闭提示后下一帧会重新显示它们。

paint.c 的窗口遍历、标题和应用内容沿用 G16。依次更新下面三处：函数开头的说明、顶栏图标这一行，以及文件末尾从 tx 增量到函数闭合的窗口。新增的提示与对话框绘制接在底栏之后。顶栏资源图标本章仍只画外观，资源栏展开状态在 G18 才加入。每处旧块用于唯一定位，只把新块写回对应位置。

<a id="code-frontend-core-paint-c-1"></a>

<!-- replace: frontend/core/paint.c -->
<!-- before -->
```c
void ui_draw(struct ui_desktop *s, const struct airi_framebuffer *f) {  // 按当前窗口次序绘制整帧
    if (!airi_framebuffer_valid(f))  // 画布地址、尺寸或通道无效时，在写像素之前结束
        return;  // 画布验证未通过，本帧不写像素
```
<!-- after -->
```c
void ui_draw(struct ui_desktop *s, const struct airi_framebuffer *f) {  // 按窗口层次画整帧，并将对话框留在最后
    if (!airi_framebuffer_valid(f))  // 画布地址、尺寸或通道无效时，在写像素之前结束
        return;  // 画布验证未通过，本帧不写像素
```

<!-- replace: frontend/core/paint.c -->
<!-- before -->
```c
    u_icon(s->width - 37, 8, UI_TASKS_APP, U_MUTED);  // 顶栏右侧画资源入口图标，当前只绘制图标，资源栏状态将在18章加入
```
<!-- after -->
```c
    u_icon(s->width - 37, 8, UI_TASKS_APP, U_MUTED);      // 顶栏右侧当前只画资源图标，资源栏状态在 G18 加入
```

前两处更新后，继续替换文件末尾。toast 非空便画短提示，dialog 非零则在所有窗口和底栏之后画居中框。对话框正文借用 desktop.dialog_text，局部 row 只装当前显示行：每加入字节就测 U_BODY 宽度，遇到 432 像素或 63 字节边界时优先退回最后空格；没有可用空格也至少消费一字节，避免停在原地。每行下移 22 像素，原说明不被截断修改。名称框再使用自己的局部裁剪，画完恢复整屏；按钮在这里仍只输出外观。

<!-- replace: frontend/core/paint.c -->
<!-- before -->
```c
            tx += 88;  // 任务栏下一应用标题右移88像素
        }
}
```
<!-- after -->
```c
            tx += 88;  // 任务栏下一应用标题右移88像素
        }
    if (s->toast[0]) {  // 短提示非空时在普通窗口之后覆盖绘制
        u_rect(s->width - 382, s->height - 78, 360, 34, U_RAISED);  // 用选中背景色写矩形，起点(s->width - 382,s->height - 78)，尺寸360×34像素
        u_text(s->width - 368, s->height - 70, s->toast, U_SMALL, U_TEXT);  // 在整屏x=s->width - 368、整屏y=s->height - 70显示本次操作短提示
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

## 用服务结果更新保存点与关闭请求

<a id="save-transition"></a>

服务返回 true 以后，ui_command 才能把前端也推进到新的保存点。本轮它以当前 telemetry 目录、UI_SAVE、editor_id=1、main.lua、editor 及局部 error[160] 调用 action。服务接受十字节正文后，ui_refresh 取得更新后的元信息，saved 再复制当前 editor，dirty 清为 false；undo 仍保留原 main.lua，保存不会顺带清空撤销槽。

失败则不刷新保存点，不清空当前 editor，而是把 error 复制到桌面对话框。pending_close 另表示“这次保存完成后关闭”：保存成功才关闭 Editor，失败则继续保留窗口；两个出口都消耗这项关闭请求，避免下一次普通保存意外关闭文档。

`ui_command(s,action,id,name,data)` 接收当前桌面、动作、目标身份、名称参数与正文。保存使用当前文件名，创建使用新名称，复制和移动则把 name 用作目标目录；新建记录的 id 传 0，未绑定文档的保存传 -1。回调只在此次调用内借用这些字符串。

保存反馈同时区分当前参考服务的会话内存与真实后端：参考成功显示Saved to session storage。失败标题为Save failed，正文追加Your edits are still open；同一工作数组继续保存输入。

state.c 只增加所需 include 和动作入口，appearance、refresh、init、open 均保留。先用新块替换原来的文件首行，再追加动作函数。

<a id="code-frontend-core-state-c-1"></a>

<!-- replace: frontend/core/state.c -->
<!-- before -->
```c
#include "internal.h"  // 取得核心内部的桌面类型、有限copy与基础库接口。
```
<!-- after -->
```c
#include "internal.h"  // 取得核心内部的桌面类型、有限copy与基础库接口。
#include "ui.h"  // 引入前端服务合同与桌面状态
#include <airi/base.h>  // 引入自有字节操作和有界格式化函数
```

现有初始化仍按 G16 读取 main.lua，并建立 editor 与 saved 两份副本。下面在 state.c 末尾追加 ui_command，把服务结果接回这份桌面。

<!-- append: frontend/core/state.c -->
```c
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

## 读入文档后再切换当前文件

按键还可从 Files 打开别的对象。ui_open_document 接收原快照下标，先检查有效范围；若是目录，只更新 directory 并刷新，Editor 中的文档继续保留。普通文件才检查 dirty：有未保存修改就提示并停止，避免另一份正文覆盖工作数组。完整 read 成功以后，才一起切换 id、名称与 saved，重置光标、滚动、选区和撤销状态，再打开 Editor。当前 reference 在 read 失败前不复制内容，因此失败也保留原文。

**完整写入 `frontend/apps/documents.c`：**

<a id="code-frontend-apps-documents-c-1"></a>

<!-- write: frontend/apps/documents.c -->
```c
#include "internal.h"  // 引入当前目录内部共享接口和基础函数

void ui_open_document(struct ui_desktop *s, unsigned index) {  // 按快照下标打开目录或读取文档，成功才切换编辑身份
    if (index >= s->file_count)  // 快照下标必须落在这次查询的有效前缀
        return;  // 快照下标越界，不读取条目或改变当前文档
    struct ui_file *f = &s->files[index];  // 借用快照第index项，后面用其服务id而非下标读取
    if (f->directory) {  // 目录项改变当前目录，不当成文字文档读取
        airi_snprintf(s->directory, 128, "%s", f->path);  // 用目录记录的完整路径替换当前查询目录
        ui_refresh(s);  // 以当前directory重新复制文件快照；任务回调接入后同次刷新它和资源统计
        return;  // 已切换查询目录并刷新快照，Editor 文档保持原样
    }
    if (s->dirty) {  // 当前文档未保存时先阻止另一个文档覆盖编辑数组
        ui_app_dialog(s, UI_DIALOG_ERROR, 0, "Unsaved changes",  // 保存文字"Unsaved changes"的种类、目标和说明，最后绘图且优先接收后续事件
                      "Save or discard the current document before opening another file.", "");  // 说明未保存正文阻止新文档覆盖，最后空串表示此错误框没有名称初值
        return;  // 当前文档未保存，显示提示并保留工作数组
    }
    if (!s->backend.read ||  // 先检查读取函数地址是否存在，短路或保证缺回调时不调用空函数指针
        !s->backend.read(s->backend.context, f->id, s->editor, sizeof(s->editor))) {  // 按服务id读取文档，缺回调或失败时保留原文并显示原因
        ui_app_dialog(s, UI_DIALOG_ERROR, 0, "Unable to open file",  // 保存文字"Unable to open file"的种类、目标和说明，最后绘图且优先接收后续事件
                      "The file service did not return the document.", "");  // 说明读取服务失败，错误框无需名称字段，原编辑正文保持完整
        return;  // 读取回调缺失或读取失败，保留原文与编辑身份
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
```

## 把确认框转换成一次具体服务动作

读取别的文档已有保护，关闭和命名还需要把用户的确认接回服务。错误框只需关闭，有名称或危险动作的确认框则还要继续操作。ui_confirm 先把当前 dialog 种类保存到局部 kind，再清掉显示状态，随后按 kind 生成 ui_command。关闭前选择保存会设置 pending_close；已有 id 直接 Save，新文档先进入 Save As 等待名称。这里建立可由输入调用的确认入口，标题栏鼠标操作尚未接入。

保存路径只有在服务返回 true 后才能更新 saved 并清 dirty。撤销等编辑动作则仍按 editor 与 saved 是否相同计算 dirty。取消操作会清 pending_close，避免将关闭请求留给之后普通保存。

**新建 `frontend/core/commands.c`：**

<a id="code-frontend-core-commands-c-1"></a>

<!-- write: frontend/core/commands.c -->
```c
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
    if (kind == UI_DIALOG_SAVE_AS)  // 另存提交id为-1的新文档请求，成功后刷新并重新绑定Editor身份
        ui_command(s, UI_SAVE, s->editor_id, s->dialog_input, s->editor);  // 提交另存名称与当前正文，未绑定时id为-1，成功后前端从新快照绑定新id
}
```

## 在数组中插入字符

<a id="edit-transaction"></a>

一次粘贴可能替换选区、移动尾部并更新撤销副本。任何步骤开始后才发现容量不足，都会留下半次修改。因此 insert 先计算全部输入是否合法、最终长度是否能容纳 NUL；通过后才改变数组。这里“整次修改或保留原值”的范围是一份内存文档，没有涉及磁盘事务。

本轮全选 main.lua 后，selected 等于原长度 len，拟插入 n=10；最终长度按 len-selected+n 得到 10，而不是把十字节叠在原文长度上。8192 是数组容量，最大内容长度为 8191，末尾必须另留 NUL。即使原文已满，完整选区被短文替换仍可以通过检查。

insert 先检查输入所有字节。任一字节 >=128 就拒绝整次插入，当前文字、选区和 undo 保持原值。然后计算原长度 len、输入长度 n 和选区长度，结果达到 8192 则仅给提示。

检查通过后才保存 undo、删除选区、移动尾部并复制新内容。对于 `ABCD\0`、cursor=anchor=2、输入 xy，先把 `CD\0` 从偏移 2 搬到 4，再写 xy，得到 `ABxyCD\0`、cursor=4。选区 `[1,3)` 时先将 `D\0` 搬到 1，插入后为 `AxyD\0`。尾部连 NUL 一起移动，先写新文字会破坏尚未搬走的源字节。

**新建 `frontend/core/editing.c`：**

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
```

到这里，输入字节与最终容量都已通过检查，当前文字、选区和 undo 仍未改变。下面才记录旧文、删除选区并腾出新字节的位置。本轮全选会把旧正文删到只剩 NUL，写入十字节后 cursor=10；最后 changed 通过与 saved 比较得出 dirty，而不是见到一次按键就固定设为 true。继续追加 insert 的提交部分。

<!-- append: frontend/core/editing.c -->
```c
    copy(s->undo, sizeof(s->undo), s->editor);  // 在修改前保存完整旧文档，供一次撤销切回
    s->undo_valid = true;  // 旧文档已存好，允许下一次Undo使用它
    delete_selection(s);   // 先删除当前选区并把光标放到左端，再执行插入或剪切
    len = airi_strlen(s->editor);  // 选区删除后重测文档长度，尾部移动使用新长度
    airi_memmove(s->editor + s->cursor + n, s->editor + s->cursor, len - s->cursor + 1);  // 尾部连NUL整体右移n字节，重叠复制保住尚未读的源
    airi_memcpy(s->editor + s->cursor, text, n);  // 空位已经腾出，复制新文字内容而不复制其NUL
    s->cursor += (unsigned)n;  // 光标移到新插入文字之后
    changed(s);  // 重新比较保存副本并滚动到光标，使内容和可见状态一致
}
```

## 短字段与文档各自处理输入

文档插入已经能够在检查通过后一次完成修改。接下来同一个按键还可能交给 Save As 名称框、Files 筛选框或 Editor 查找框。这些字段保存一行短文字，使用独立 `field_key`：field_all 表示整个字段选中，复制取得整个字段，粘贴只保留可打印 ASCII 并受 cap 限制。它们各自传入数组和容量，不使用文档的 cursor/anchor 或 undo。

**追加到 `frontend/core/editing.c` 末尾：**

<a id="code-frontend-core-editing-c-2"></a>

<!-- append: frontend/core/editing.c -->
```c
static void field_key(struct ui_desktop *s, char *field, size_t cap, int key) {  // 让一个事件只修改当前短字段和字段级选择
    if (key == UI_SELECT_ALL) {  // 当前接收者内全选：短字段标记整字段，文档分支则设置两端字节间隙
        s->field_all = true;  // 输入字段内容设为整体选中，下一次键入可以替换
        return;  // 已标记整字段选中，此键不再作为普通字符插入
    }
    if (key == UI_COPY || key == UI_CUT) {  // 复制保留当前文字，剪切还会删除；具体范围由此接收者的选中状态决定
        if (s->field_all) {  // 只有短字段整体选中时才复制其正文，文档选区不参与此字段操作
            copy(s->clipboard, sizeof(s->clipboard), field);  // 把field复制到s->clipboard，sizeof(s->clipboard)是包含NUL的数组容量
            if (key == UI_CUT)  // 短字段剪切复制完成后清空字段，普通复制保留输入
                field[0] = 0;  // 清空当前短输入字段，文档编辑数组不受影响
        }
        return;  // 复制或剪切分支已结束；未全选时也不把快捷键插入字段
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
```

## 由文档间隙计算光标的可见位置

修改文字以后，cursor 指向新文中的字节间隙，屏幕还需要显示这个位置。reveal_cursor 扫描到 cursor 得到行列，再从 u_window_body 取得实际内容宽高。纵向扣正文前88像素和底部裁剪30像素，查找栏打开再扣44；横向扣当前实际项目栏与72像素行号、边距，24像素为一行、10像素为一列。光标刚越过可见末行才卷动，文字仍容纳得下时不提前藏起首列。

内容改变时 changed 比较 editor 与 saved 决定 dirty，撤销回保存内容会重新变为干净状态。

**追加到 `frontend/core/editing.c` 末尾：**

<a id="code-frontend-core-editing-c-3"></a>

<!-- append: frontend/core/editing.c -->
```c
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
```

## 先选择按键接收者，再执行编辑规则

<a id="key-routing"></a>

同一字符 S 在编辑器正文里应插入，在 Save As 名称框里应修改名称；快捷动作 UI_SAVE_KEY 又应发起保存。因此事件先选接收者，再执行接收者自己的规则。dialog、focus 和 active 分别表示弹出的交互、字段焦点与活动窗口，不能只看 active 就把所有文字送给 Editor。

ui_key 首先判断事件去向：全局窗口与 Escape，活动对话框，搜索字段，当前应用。保存文件名框获得文字时，Editor 内容不会同时接受同一个键。

Escape 清 dialog、pending_close、find 和 focus；对话框回车调用 ui_confirm。查找字段提交后逐起点匹配，命中便设置选区并显露光标。Terminal 粘贴只送可打印字符，提交命令仍需回车。Files 上下键通过 ui_file_order 移动可见选择，文件剪贴板保存服务 id。

**追加到 `frontend/core/editing.c` 末尾：**

<a id="code-frontend-core-editing-c-4"></a>

<!-- append: frontend/core/editing.c -->
```c
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
        s->find = false;  // 隐藏Editor查找栏，后续正文可使用这部分高度
        s->focus = UI_FOCUS_NONE;  // 当前字段结束接收，后续按键回到活动应用
        return true;  // 本逻辑事件已处理，调用者可重画当前状态
    }
    if (s->dialog) {  // 有对话框时先由它消费当前键，下面的字段和应用不再接收同一事件
        if (key == '\n' || key == '\r') {  // 换行或回车表示提交当前命令、字段或文档换行动作，按当前接收者处理
            if (s->dialog == UI_DIALOG_ERROR)  // 错误提示只需关闭，不应当被回车转成文件服务动作
                s->dialog = UI_DIALOG_NONE;  // 关闭当前对话框，后续键不再被它优先接收
            else  // 前一分支未成立时执行下方替代操作
                ui_confirm(s);  // 把当前对话框确认转换成明确服务动作，本次实参取自当前桌面状态
            return true;  // 本逻辑事件已处理，调用者可重画当前状态
        }
        if (s->dialog == UI_DIALOG_NEW_FILE || s->dialog == UI_DIALOG_NEW_DIR || s->dialog == UI_DIALOG_RENAME || s->dialog == UI_DIALOG_SAVE_AS)  // 只有需要名称的四种对话框把普通键交给dialog_input，其他确认框忽略文字
            field_key(s, s->dialog_input, sizeof(s->dialog_input), key);  // 当前事件仅修改对话框输入字段，Editor不再次消费它
        return true;  // 本逻辑事件已处理，调用者可重画当前状态
    }
```

弹框未接收这个事件时，再检查短字段。receiver 在回车清除 focus 之前记住本次来自 FILTER 还是 FIND；只有 FIND 才搜索文档、设置选区。Editor 的查找栏可以仍然显示，Files 筛选回车却不能因此触发 Editor 搜索：find 保存可见状态，receiver 保存本次事件去向。继续追加字段处理。

<!-- append: frontend/core/editing.c -->
```c
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
```

对话框与字段分支都已提前返回，下面才按 active 进入应用。Terminal 把输入交给同一个 console，粘贴逐字节过滤为可打印字符，换行不会自动提交。Files 则把复制、剪切记成文件 id；粘贴是向当前目录请求复制或移动，不使用文字 clipboard。接着追加这些应用分支。

<!-- append: frontend/core/editing.c -->
```c
    if (s->active == UI_TERMINAL) {  // Terminal独占当前应用事件，正文输入进入console命令数组
        if (key == UI_PASTE) {  // Terminal粘贴逐字节交给console，只接收可打印ASCII；粘贴换行不提交命令
            for (const char *p = s->clipboard; *p; ++p)  // 逐字节扫描Terminal文字剪贴板，下一条件排除不可打印字节与换行
                if (*p >= 32 && *p <= 126)  // 字段或Terminal粘贴只消费可打印ASCII，控制字节不自动提交
                    airi_console_key(s->console, *p);  // Terminal粘贴逐个送可打印ASCII到命令输入，剪贴板换行不会自动提交
            return true;  // 本逻辑事件已处理，调用者可重画当前状态
        }
        return airi_console_key(s->console, key);  // 把当前键交给同一console，并把它的处理结果原样返回给调用者
    }
    if (key == UI_FIND_KEY && s->active == UI_FILES_APP) {  // Files的查找快捷键只切到自己的筛选框，不开启Editor文内查找
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
```

前面的接收者都未消费事件时，只有 active=UI_EDITOR 才继续处理正文。本轮 UI_SELECT_ALL 将 anchor=0、cursor=len；UI_PASTE 再把 clipboard 交给已实现的 insert。复制将选区写进独立 clipboard 并补 NUL，剪切还先留 undo 再删除。保存键不进入 insert：有 id 就提交服务，无 id 才打开名称框；Run 同样走 ui_command，由 reference 返回未连接错误。

**追加到 `frontend/core/editing.c` 末尾：**

<a id="code-frontend-core-editing-c-5"></a>

<!-- append: frontend/core/editing.c -->
```c
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
```

## 交换撤销快照，并保持光标与选区一致

现在补 Undo。本轮第一次替换后，editor 是十字节新文、undo 是原 main.lua、saved 还是原文。第一次 Undo 用 temp 暂存 editor，交换后 editor 恢复原文、undo 留住新文，changed 比较 saved 得到 dirty=false。第二次 Undo 再交换回来，dirty=true。当前只有这一个可交换槽，没有多步历史；保存点始终由 saved 单独保存。

交换后光标移到文尾，清选区，再比较 saved。左右移动一个 ASCII 字节间隙，Home/End 找本行边界，上下移动保留原列并限制在目标行长度内。

文档限定 ASCII，字节位置因此能作为字符位置使用。

**追加到 `frontend/core/editing.c` 末尾：**

<a id="code-frontend-core-editing-c-6"></a>

<!-- append: frontend/core/editing.c -->
```c
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
        while (s->cursor && s->editor[s->cursor - 1] != '\n')  // 从光标向前找本行首间隙；到文首或前一字节为换行时停止
            --s->cursor;  // 光标向前移动一个ASCII字节间隙
    } else if (key == UI_END) {  // 向后扫描到本行换行之前或文档结尾，不跨到下一行
        while (s->cursor < len && s->editor[s->cursor] != '\n')  // 从光标向后找本行尾间隙；到文尾或当前字节为换行时停止
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
```

退格删除光标前字节并左移，Delete 删除后字节，两者都搬动终止零。换行插入 `\n`，Tab 插入四个空格，普通字符形成两字节字符串交给 insert。Shift 移动第一次保存 before 为 anchor，随后只改变 cursor。

**追加到 `frontend/core/editing.c` 末尾：**

<a id="code-frontend-core-editing-c-7"></a>

<!-- append: frontend/core/editing.c -->
```c
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

## 在同一个桌面中完成替换、撤销与保存

实现已经从按键接收者连到工作数组，并从保存回调连到服务正文。现在由主机观察入口按章首的顺序驱动同一个 desktop，打印 cursor、dirty、dialog 和字节数。前半段直接核对副本状态，随后才绘图检查光标像素，最后重新初始化服务检查字段路由。这三段共享一次主机进程，但最后一段会明确重置文档。

先让 `ui_fixture_backend` 建立参考记录，`ui_init` 按 G16 读取 main.lua，并在 reference 模式中打开 Editor。全选将 anchor 放到 0、cursor 放到原文末尾；粘贴十字节后，观察第一次检查输入修改是否与服务正文分离。

**新建 `labs/host/frontend/editor_transaction.c`：**

<a id="code-labs-host-frontend-editor-transaction-c-1"></a>

<!-- write: labs/host/frontend/editor_transaction.c -->
```c
#include <airi/base.h>  // 声明回归观察中使用的自有有界字符串复制。
#include "ui.h"      // 引入前端服务合同与桌面状态
#include <stdio.h>   // 主机观察使用标准文件输出与打印接口
#include <string.h>  // 主机观察使用字符串比较检查实验结果

static struct ui_desktop desktop;    // 主机观察拥有整份静态桌面，多个步骤修改同一对象
static struct airi_console console;  // 主机观察拥有同一console输入输出状态
static void observe(const char *step) {  // 打印同一桌面在当前步骤的光标、保存和错误状态
    printf("%s: cursor=%u dirty=%u dialog=%d editor_bytes=%zu\n", step, desktop.cursor,  // 向主机终端报告本次观察的实际状态或输出位置
           desktop.dirty ? 1u : 0u, desktop.dialog, strlen(desktop.editor));  // 将bool转换为%u所需unsigned，打印对话框种类与不含NUL的实际字节数
}
int main(void) {  // 在主机准备输入、运行当前观察并以退出码报告结果
    struct ui_backend backend;     // 观察程序持有回调合同副本，初始化后才可调用
    ui_fixture_backend(&backend);  // 把服务回调写入本观察程序的 backend 结构，并重置参考静态记录
    airi_console_init(&console, backend.boot);  // 观察console与System借用同一参考启动结构，日志和面板来源一致
    ui_init(&desktop, backend, &console);  // 同一desktop借用console并复制后端合同，后续每次操作都修改这份状态
    observe("open");  // 记录刚读取main.lua后的干净文档状态，作为后续比较起点
    ui_key(&desktop, UI_SELECT_ALL, false);    // 把当前文档从首间隙到文尾选中，false表示此动作不伴随Shift扩选
    strcpy(desktop.clipboard, "print(42)\n");  // 本观察用容量已确定的剪贴板数组保存十字节替换文字
    ui_key(&desktop, UI_PASTE, false);  // 用文字剪贴板替换整份选区，沿容量与ASCII检查路径完成一次修改
    observe("replace");  // 记录全选替换后的10字节正文、文尾光标与dirty=true
    if (!desktop.dirty || strcmp(desktop.editor, "print(42)\n"))  // 要求替换正文完全相同且已标为未保存，任一条件失败即停止观察
        return 1;  // 当前主机观察失败，以非零状态阻止后续误判通过
```

此时服务与 saved 仍留原文，editor 已替换为十字节，undo 已存原文。继续追加两次 Undo：第一次要求 dirty=false 且 editor 不再等于新文，第二次要求十字节新文恢复且 dirty=true。这两步只交换前端副本，没有调用保存服务。

<!-- append: labs/host/frontend/editor_transaction.c -->
```c
    ui_key(&desktop, UI_UNDO, false);  // 恢复全选替换之前的main.lua，它与读取时保存点相同
    observe("undo");  // 观察恢复原文后dirty=false，撤销不改服务正文
    if (desktop.dirty || !strcmp(desktop.editor, "print(42)\n"))  // 要求恢复原文且不再未保存，不能仍停留在替换文字
        return 1;  // 撤销观察不符合预期时立即失败
    ui_key(&desktop, UI_UNDO, false);  // 再交换一次唯一副本，回到尚未提交的十字节替换文字
    observe("restore replacement");    // 此时dirty再次为true，下一步保存这份替换结果
    if (!desktop.dirty || strcmp(desktop.editor, "print(42)\n"))  // 要求替换正文完全相同且已标为未保存，任一条件失败即停止观察
        return 1;  // 副本交换没有恢复替换结果时停止
```

现在 editor 又是十字节替换结果，saved 仍是原文。接着发 Save，要求 dirty 清零，并用独立 readback[64] 再按 editor_id 从服务完整读取。这里传 NULL 与初始化表中的 context 一致，当前静态参考服务不使用上下文；readback 的结果还须逐字等于新文，才能说明保存已改变服务记录。

<!-- append: labs/host/frontend/editor_transaction.c -->
```c
    ui_key(&desktop, UI_SAVE_KEY, false);  // 请求将当前编辑文字提交给已绑定服务id，成功后才更新saved
    observe("save");    // 记录提交服务之后的dirty=false，下一步还要重新read核对服务正文
    char readback[64];  // 64字节局部数组承接服务重新读出的正文，和Editor副本互相独立
    if (desktop.dirty || !backend.read(NULL, desktop.editor_id, readback, sizeof(readback)) ||  // 要求保存点已更新且服务读取完整成功，短路或避免失败后使用未写入的readback
        strcmp(readback, "print(42)\n"))  // 保存成功之外还要求服务重新读出的文字完全等于十字节输入
        return 1;  // 当前主机观察失败，以非零状态阻止后续误判通过
```

保存完成后，editor、saved 和服务正文都已是十字节；undo 仍是原文。继续请求运行，它应得到 UI_DIALOG_ERROR 及 not connected 原因。这个失败不撤回保存，也不启动主机或 guest 程序。随后 Escape 只关闭错误反馈，下一次输入回到这份 Editor。

<!-- append: labs/host/frontend/editor_transaction.c -->
```c
    ui_key(&desktop, UI_RUN_KEY, false);  // 以当前文档发起运行请求，观察参考服务真实未连接错误
    observe("request execution");  // 记录Run返回错误框后的同一文档状态，保存成功不等于执行可用
    printf("error: %s\n", desktop.dialog_text);  // 向主机终端报告本次观察的实际状态或输出位置
    if (desktop.dialog != UI_DIALOG_ERROR || !strstr(desktop.dialog_text, "not connected"))  // 要求反馈确实是执行未连接错误，不能把失败请求伪装成成功
        return 1;  // 当前主机观察失败，以非零状态阻止后续误判通过
    ui_key(&desktop, UI_ESCAPE, false);  // 关闭Run错误，下一次输入回到同一Editor
```

## 通过实际光标像素检查滚动，再重置输入接收者

文档保存和执行失败已经分别核对，下面在同一进程里转向光标观察。先把普通外框固定成 800×400，正文按当前标题、起点和底部裁剪只容纳九条完整 24 像素行；十行文字将光标放在第十行末尾。粘贴触发 reveal_cursor 后再 ui_draw，程序逐个检查光标竖线的 19 个蓝色像素，而不只检查滚动数值。继续追加纵向观察。

<!-- append: labs/host/frontend/editor_transaction.c -->
```c
    desktop.windows[UI_EDITOR].rect = (struct ui_rect){100, 80, 800, 400};  // 当前48px标题下只容纳九条完整24px文本行
    desktop.windows[UI_EDITOR].maximized = false;  // 固定普通外框，避免最大化替换这份几何
    ui_key(&desktop, UI_SELECT_ALL, false);  // 全选已有十字节，使以下输入替换同一文档
    strcpy(desktop.clipboard, "x\nx\nx\nx\nx\nx\nx\nx\nx\nx");  // 九个换行生成十行，光标在第十行末尾
    ui_key(&desktop, UI_PASTE, false);   // 插入会调用reveal_cursor，最后一行应完整滚入正文
    static uint32_t pixels[1280 * 800];  // 本进程拥有本次观察画布，渲染器只借用
    struct airi_framebuffer fb = {  // 使用同一路径验证实际像素，不只比较滚动公式
        .virtual_address = (uintptr_t)pixels, .width = 1280, .height = 800,  // 画布地址是主机静态数组，单位为像素
        .pitch = 1280 * 4, .bpp = 32, .red_size = 8, .red_shift = 16,  // 每行5120字节，红通道占16..23位
        .green_size = 8, .green_shift = 8, .blue_size = 8  // 其余两个八位通道形成标准RGB
    };
    ui_draw(&desktop, &fb);  // 按同一文档与当前滚动生成可检查的一帧
    struct ui_rect body = u_window_body(desktop.windows[UI_EDITOR].rect);  // 借用绘图回写的实际外框转换正文矩形
    int cursor_x = body.x + 61 + 10;  // 未出现项目栏，正文左缘加一字节推进量到第十行文尾
    int cursor_y = body.y + 88 + (9 - (int)desktop.editor_scroll) * 24 + 3;  // 第九逻辑下标减卷走行数，再到光标竖线顶端
    for (int i = 0; i < 19; ++i)  // 每个光标像素都应保留，不能只让竖线上半段露出
        if (pixels[(cursor_y + i) * 1280 + cursor_x] != u_color(U_BLUE))  // 对照渲染器当前蓝色角色的实际RGB
            return 1;  // 任一像素被底部裁剪说明光标尚未完整显露
    puts("cursor: all 19 pixels visible after the tenth line");  // 报告跨过当前正文末行后的实际绘图结果
```

纵向观察完成后，把外框宽度改为 400，重新替换成 31 个 A。body 宽 398，扣去行号与边距的 72 后，可容纳 32 列；文尾间隙在第 31 列，仍应可见，所以 editor_column_scroll 须保持零。绘图后重新取得 body，再核对这一位置的完整光标，不能沿用上一幅窗口的几何。

<!-- append: labs/host/frontend/editor_transaction.c -->
```c
    desktop.windows[UI_EDITOR].rect = (struct ui_rect){100, 80, 400, 400};  // 398像素body扣72像素行号与边距后，仍能显示32列
    ui_key(&desktop, UI_SELECT_ALL, false);  // 新观察继续替换同一文档，释放前一个选区
    strcpy(desktop.clipboard, "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");  // 31个ASCII字符，文尾间隙仍在当前32列可见范围
    ui_key(&desktop, UI_PASTE, false);  // 完整替换后应保持最左列，而不是提前滚走第一字符
    ui_draw(&desktop, &fb);  // 用本次宽度生成实际文尾光标
    body = u_window_body(desktop.windows[UI_EDITOR].rect);  // 更新刚变化的body，不能使用前一幅800像素窗口位置
    cursor_x = body.x + 61 + 31 * 10;  // 31字符的文尾间隙位于第31列，每列十像素
    cursor_y = body.y + 88 + 3;  // 单行文本已回到第一可见行，竖线从行顶下三像素开始
    if (desktop.editor_column_scroll)  // 当前完整输入及光标仍容纳得下，无需隐藏第一列
        return 1;  // 按旧外框常数提前水平滚动会在此暴露
    for (int i = 0; i < 19; ++i)  // 再逐像素确认文尾竖线确实写入当前正文裁剪区
        if (pixels[(cursor_y + i) * 1280 + cursor_x] != u_color(U_BLUE))  // 状态值正确之外还要满足最终可见像素
            return 1;  // 行尾光标被右边界或其他绘图覆盖时停止观察
    puts("cursor: column 31 visible without horizontal scroll");  // 报告宽度改变后无需提前滚动的实际行为
```

最后重置服务和 desktop，恢复含 local 的原 main.lua。这是新一轮路由观察，不延续前面 31 个 A。Editor 保留打开的查找栏及一段选区后切到 Files，让 m 与回车只进入 Files 筛选。程序要求原来的 cursor=2、anchor=1、selection=true 全部保留，确认同一事件没有越过接收者边界。继续追加至 main 结束。

<!-- append: labs/host/frontend/editor_transaction.c -->
```c
    ui_fixture_backend(&backend);  // 恢复原main.lua，下面local查找确实能匹配。
    ui_init(&desktop, backend, &console);  // 为字段路由观察建立新的桌面，绑定现有服务。
    ui_open_document(&desktop, 0);  // 打开main.lua，作为查找可能匹配的文档。
    ui_key(&desktop, UI_FIND_KEY, false);  // Editor查找栏保持打开。
    airi_snprintf(desktop.find_text, sizeof(desktop.find_text), "%s", "local");  // 准备能匹配文档的查找串。
    desktop.cursor = 2;  // 给文档留下与匹配范围不同的光标间隙。
    desktop.anchor = 1;  // 选择当前字节1至2，后续不应被Files筛选改变。
    desktop.selection = true;  // 当前文档选区有效，便于观察越界分派。
    ui_open(&desktop, UI_FILES_APP);  // 切到Files，Editor查找栏状态继续保留。
    ui_key(&desktop, UI_FIND_KEY, false);  // 本次焦点是Files筛选，不是Editor查找。
    ui_key(&desktop, 'm', false);   // 筛选字段接收当前字符。
    ui_key(&desktop, '\n', false);  // 回车只结束Files筛选输入。
    if (desktop.cursor != 2 || desktop.anchor != 1 || !desktop.selection)  // 核对Editor三个状态没有被另一字段搜索覆盖。
        return 1;  // 任何变化都说明本次事件越过了接收者边界。
    puts("fields: Files filter keeps Editor selection");  // 报告实际跨应用字段观察结果。
    puts("PASS lab/editor: reference save and failed execution are distinct");  // 向主机终端报告本次观察的实际状态或输出位置
    return 0;  // 观察完整通过，主机进程以成功状态退出
}
```

将输入实现与观察程序链接，运行全选、替换、撤销、恢复替换、保存和请求运行的完整序列。

## 连接确认入口并运行完整编辑序列

输入路由会在对话框回车时调用 `ui_confirm`。把它的声明加入核心内部头文件，使 `editing.c` 使用的函数与 `commands.c` 的定义一致，再将这些文件一起链接到观察程序。

在 core/internal.h 中定位 copy 的函数首行，用新块替换它，把 ui_confirm 声明放在 copy 之前。copy 的函数体与包含保护继续保留。

<a id="code-frontend-core-internal-h-1"></a>

<!-- replace: frontend/core/internal.h -->
<!-- before -->
```c
static inline void copy(char *out, size_t n, const char *text) {  // 按调用者数组容量复制NUL文字
```
<!-- after -->
```c
void ui_confirm(struct ui_desktop *s);  // 把当前对话框确认转换成明确服务动作
static inline void copy(char *out, size_t n, const char *text) {  // 按调用者数组容量复制NUL文字
```

**在学习目录根目录运行：**

整个块交给同一个 `sh -eu` 执行；编译失败时会立即结束，不会接着运行目录里以前留下的 `build/editor`。结束行 `SH` 单独顶格输入。

<!-- run -->
```sh
sh -eu <<'SH'  # 任一编译或运行步骤失败便结束本次命令。
mkdir -p build  # 保存本次主机程序和图像，已有目录可以继续
cc -std=c11 -O0 -g -Wall -Wextra -Werror -Iinclude -Ifrontend/include labs/host/frontend/editor_transaction.c kernel/lib/base.c kernel/boot/info.c kernel/console/console.c frontend/core/*.c frontend/apps/*.c frontend/backends/reference.c frontend/render/draw.c -o build/editor  # Shell展开两个通配符，把当前已经写出的核心和应用一起链接
./build/editor  # 运行当前完整观察程序，退出非零表示本组观察失败
SH
```

替换后 cursor=10、dirty=1、editor_bytes=10；第一次Undo恢复原main.lua，dirty=0；第二次Undo交换回替换文字，dirty=1；保存后 dirty=0。请求运行得到 UI_DIALOG_ERROR，文档内容仍在，错误说明执行服务未连接。

这次观察从服务读入的 main.lua 出发，键事件先改变工作副本，撤销只交换前端文字，保存成功再改变服务正文与保存点。执行失败留下错误框，Escape 后仍能继续使用已保存文档；光标像素和字段路由观察又分别核对显示位置与事件接收者。

Files 和 Editor 现在能通过同一份服务表查询、修改并报告失败。下一章把这组交接扩展到任务与设置：服务提供任务和资源快照，界面保存选择，确认动作再请求停止或更新。CSV 随后沿读取出的文字建立自己的数据解析过程。
