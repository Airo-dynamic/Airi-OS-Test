# G18 · 在未保存文档旁切换主题并查看资源

main.lua已经能编辑、撤销与保存。现在把它替换为print(42)加换行，先保留未保存状态和选区，再打开Settings切到Day、切回Night。外观改变后回到Editor，仍应继续操作同一份文字，保存点、选择与窗口几何也应保留。为此，Settings 只改 desktop 保存的主题偏好，同一份桌面继续持有文档和窗口。随后Tasks和资源侧栏读取同一次参考服务快照，显示明确标作参考数据的任务与统计；CSV仍留待下一章解析。

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
```

这次要增加的状态有两种来源：未保存文档和主题偏好由现有会话持有；任务表和统计由后端复制到桌面快照。参考服务提供任务和统计回调，`ui_refresh` 取得它们后才能绘制 Tasks 和资源栏。Settings 随后直接修改同一桌面的偏好，最后用一段连续操作核对文档是否仍在。

<a id="task-samples"></a>

Tasks 中的任务记录描述“某个执行单位叫什么、处于什么状态、用了多少资源”。真正的操作系统进程还涉及地址空间与调度，但当前没有这些内核机制；reference 的 pid 和状态只是服务中的示例记录，Stop 改这份表。真实内核接入前，不将这些曲线解释成 QEMU CPU 或 guest 任务实测。

图中六十个 sample 是 0..100 的百分数记录。画图先把数组下标变成横坐标，再把百分数变成高度。例如值 25 在高一百像素的图中占二十五像素；屏幕 y 向下，所以点的 y 要从图底减去这个高度。

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
static struct ui_task records[UI_TASKS];  // 服务拥有最多16条静态参考任务记录
static unsigned record_count;  // 保存静态任务数组的有效前缀长度
static AiriBootInfo boot;      // 参考服务持有示例启动结构，借给System与console使用
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
static bool action(void *unused, const char *directory, enum ui_action act, int id, const char *name, const char *data,  // 校验请求后改变服务记录，错误留在输出缓冲
                   char *out, size_t cap) {  // 错误正文写到调用者out数组，cap是该数组的字节容量，服务不保留此地址
    (void)unused;  // 此参考服务使用静态表，回调合同的上下文参数本实现不读取
    if (act == UI_EXECUTE)  // 执行请求暂不启动主机或guest程序，返回真实未连接错误
        return error(out, cap,  // 把失败原因写给调用者，false阻止前端误记为成功
                     "A program runtime is not connected. Your source is still open.");  // 执行失败只说明用户当前可做的事，不把开发计划放进错误提示

    if (act == UI_STOP) {  // 停止请求处理参考任务表，不向主机发送终止信号
        if (id <= 2)  // 参考桌面与终端记录受保护，不能从样例表移除
            return error(out, cap, "This system task is protected.");  // 拒绝本次服务修改并向调用者报告：The reference desktop and terminal records are protected
        for (unsigned i = 0; i < record_count; ++i)  // 只检查服务当前有效任务记录，未用槽不参与
            if (records[i].pid == id) {  // 找到所请求的参考任务身份后才修改其状态，其他记录不受影响
                airi_memmove(records + i, records + i + 1,  // 搬移可能重叠的当前有效前缀或尾部，复制长度由当前对象内容或容量计算
                             (record_count - i - 1) * sizeof(*records));  // 只前移被删项之后的有效任务，条数乘每项大小得到字节数
                --record_count;  // 任务表后缀已前移，有效任务数量减少一个
                return true;     // 该服务修改已完整提交，前端可重新查询当前记录
            }
        return error(out, cap, "The selected process record no longer exists.");  // 拒绝本次服务修改并向调用者报告：The selected process record no longer exists
    }
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
        return error(out, cap, "Session storage capacity reached (32 items).");  // 拒绝本次服务修改并向调用者报告：Session storage capacity reached (32 items)
    }
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
        return error(out, cap, "Session storage capacity reached.");  // 拒绝本次服务修改并向调用者报告：Session storage capacity reached
    }
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
            return error(out, cap, "Folder renaming is unavailable.");  // 目录重命名需要同步后代路径，本参考服务明确返回未提供该能力。
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
static unsigned tasks(void *unused, struct ui_task *out, unsigned cap, struct ui_stats *stats) {  // 复制参考任务和同次参考资源统计到调用者快照
    (void)unused;  // 此参考服务使用静态表，回调合同的上下文参数本实现不读取
    unsigned n = record_count < cap ? record_count : cap;  // 本次复制任务条数取有效记录与调用者数组容量的较小值
    airi_memcpy(out, records, n * sizeof(*out));  // 把本次可容纳的任务前缀复制给调用者快照
    *stats = (struct ui_stats){.available = true,  // 写调用者的同次资源统计快照，available表明参考服务确实提供了这些字段
                               .cpu = 37,  // 设置0到100的参考CPU百分数
                               .memory_kib = 104 * 1024,  // 设置参考已用内存量，单位KiB
                               .total_kib = 256 * 1024,   // 设置参考总内存量，单位KiB
                               .read_kib = 416,    // 设置参考每秒读量，单位KiB/s
                               .write_kib = 128};  // 设置参考每秒写量，单位KiB/s
    for (unsigned i = 0; i < 60; ++i) {  // 按采样下标处理60个参考百分数点
        unsigned triangle = i % 20;  // 把60个采样下标映射到每20点重复的三角周期
        stats->samples[i] = 19 + (triangle > 10 ? 20 - triangle : triangle) * 3 + (i * 17 % 13);  // 合成明确的参考百分数波形，数据不是CPU实测
    }
    return n;  // 返回本次已经复制到调用者数组的有效记录条数
}
void ui_fixture_backend(struct ui_backend *b) {  // 重置当前主机参考服务并填入回调表
    airi_memset(entries, 0, sizeof(entries));  // 把服务拥有的32个槽清空，再填本会话初始参考文件
    add(0, "/home/airi/projects/telemetry", "main.lua", false,  // 向明确编号的服务槽填这份路径、名称、目录标记与正文
        "-- Read a series and write a summary\nlocal values = {12, 18, 21, 32, 29, 37}\n\nlocal "  // main.lua正文的第一段；\n成为文档换行，无逗号的下一字符串继续同一正文
        "sum = 0\nfor _, value in ipairs(values) do\n    sum = sum + value\nend\n\nlocal mean = "  // main.lua正文继续保存求和循环；C相邻字符串拼接，这些Lua文字不在本次编译执行
        "sum / #values\nprint(string.format(\"mean: %.2f\", mean))\n");  // 均值示例只包含实际程序，不嵌入产品开发说明
    add(1, "/home/airi/projects/telemetry", "series.csv", false,  // 向明确编号的服务槽填这份路径、名称、目录标记与正文
        "seconds,load\n0,12\n1,18\n2,21\n3,32\n4,29\n5,37\n");  // 初始CSV含一行标题与6组时间秒数、测量整数；当前先作为普通文字存服务
    add(2, "/home/airi/projects/telemetry", "README.md", false,  // 向明确编号的服务槽填这份路径、名称、目录标记与正文
        "# Telemetry\n\nmain.lua computes the mean of six load samples.\n"  // 项目说明先介绍计算目的
        "series.csv stores the same observations in seconds and percent.\n"  // CSV列给出时间与负载单位
        "Open series.csv to inspect the time series.\n");  // 项目说明围绕输入、程序和数据查看操作
    add(3, "/home/airi/projects/telemetry", "output", true, "");  // 向明确编号的服务槽填这份路径、名称、目录标记与正文
    add(6, "/home/airi/projects/telemetry", "sum.c", false,  // 向明确编号的服务槽填这份路径、名称、目录标记与正文
        "#include <stdio.h>\n\nint main(void) {\n    const int values[] = {12, 18, 21, 32};\n    "  // sum.c正文含示例包含行与main开头；\n形成文件换行，当前只存文字数据
        "int total = 0;\n    for (unsigned i = 0; i < 4; ++i)\n        total += values[i];\n    "  // sum.c正文继续保存求和循环；与前后字符串在C编译时拼成一个文档参数
        "printf(\"sum: %d\\n\", total);\n    return 0;\n}\n");  // 存入示例C文件最后打印与返回正文，这段字符串是数据而不是本次参与编译的C程序
    add(7, "/home/airi/projects/telemetry", "project.airi", false,  // 向明确编号的服务槽填这份路径、名称、目录标记与正文
        "[project]\nname = telemetry\nentry = main.lua\n");  // 存入项目说明文字，包含名字与入口；当前尚未把它当配置装载
    add(4, "/home/airi/projects", "telemetry", true, "");  // 向明确编号的服务槽填这份路径、名称、目录标记与正文
    add(5, "/home/airi", "projects", true, "");  // 向明确编号的服务槽填这份路径、名称、目录标记与正文
    const struct ui_task initial[] = {{1, "airidesk", "Running", 4, 12 * 1024},  // 第1条参考任务给pid、名称、状态、CPU百分数和12MiB的KiB量
                                      {2, "terminal", "Waiting", 0, 2 * 1024},  // 终端参考记录pid2处于等待，内存示例为2MiB
                                      {14, "series-worker", "Running", 31, 28 * 1024},  // 曲线worker参考记录pid14，CPU31%、内存28MiB
                                      {16, "editor", "Waiting", 2, 9 * 1024},  // Editor参考记录pid16，CPU2%、内存9MiB
                                      {18, "file-service", "Waiting", 0, 6 * 1024}};  // 文件服务参考记录pid18，内存6MiB，五项初始化结束
    airi_memcpy(records, initial, sizeof(initial));     // 把5条初始任务复制到服务拥有的固定数组
    record_count = sizeof(initial) / sizeof(*initial);  // 整组数组字节数除以每项大小得到初始5条任务
    boot = (AiriBootInfo){.magic = AIRI_BOOT_MAGIC,     // 为主机窗口准备固定参考启动结构，System明确标成示例数据
                          .version = AIRI_BOOT_VERSION,  // 设置启动合同版本
                          .size = sizeof(boot),  // 设置本版启动结构字节数
                          .direct_map_offset = UINT64_C(0xffff800000000000),  // 设置参考直接映射偏移
                          .kernel_physical_base = 0x100000,  // 设置参考内核物理起点
                          .kernel_virtual_base = UINT64_C(0xffffffff80000000),  // 设置参考内核虚拟起点
                          .kernel_size = 132 * 1024,  // 设置参考内核镜像长度
                          .region_count = 4};  // 设置参考内存区域有效条数
    airi_snprintf(boot.loader, 32, "UEFI inventory");  // 写明这份启动结构属于主机参考数据，System据此区别实际guest启动
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
                             .action = action,  // 保存参考文件/任务操作函数地址，前端请求经它修改服务表或取得失败说明
                             .tasks = tasks};   // 把任务和同次资源统计查询函数地址交给桌面，服务记录仍归参考后端
}
```

参考服务已经能返回任务和统计，桌面刷新也要接住它们：先清旧计数与统计，再在服务正常时调用任务回调。停止动作同样经过 `ui_command`，服务成功后重新刷新快照；前端不从屏幕上的任务行直接推断停止成功。

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

<a id="resource-sidebar"></a>

资源侧栏与Tasks借用同一次ui_refresh得到的stats。monitor开启且画布至少1120像素宽时，右侧预留268像素；最大化窗口从可用宽度扣除它，绘图与后续命中共享同一判断。资源栏先检查stats.available，无数据时写No runtime samples；参考快照存在时才画CPU百分比、内存KiB换算出的MiB和读写KiB/s。CPU的60个样本按下标均分59个间隔，屏幕y向下，所以100%画在顶部、0%画在底部。

**完整写入 `frontend/core/paint.c`：**

<a id="code-frontend-core-paint-c-1"></a>

<!-- write: frontend/core/paint.c -->
```c
#include "internal.h"  // 引入当前目录内部共享接口和基础函数
static const char *titles[] = {"Terminal", "Files",    "Editor",     "Tasks",  // 窗口标题按应用枚举顺序排，应用编号可直接索引
                               "System",   "Settings", "Data viewer"};  // 继续应用枚举4到6的窗口标题，数组顺序与应用id一致
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
bool ui_app_match(const char *s, const char *q);  // 按字节判断s是否包含q；Terminal识别提示符，Files和Tasks筛选各自使用结果
void ui_app_dialog(struct ui_desktop *s, int kind, int target, const char *title, const char *body,  // 把种类、目标身份和文案保存到桌面对话框状态
                   const char *value);  // value是对话框输入字段的初始文字，函数只复制状态
void ui_app_unavailable(struct ui_desktop *s, struct ui_rect r, const char *service);  // 按服务状态绘制加载、空或不可用提示
void ui_app_toolbar(struct ui_rect r, const char *label);  // 在应用内容顶部绘制44像素工具栏
void ui_app_status(struct ui_rect r, const char *left, const char *right);  // 在应用内容底部绘制27像素状态栏
void ui_terminal_draw(struct ui_desktop *, struct ui_rect);  // 把console逻辑行拆成窗口显示行并绘制输入
void ui_files_draw(struct ui_desktop *, struct ui_rect);     // 读取目录快照和排序索引，画路径、文件行和状态
void ui_editor_draw(struct ui_desktop *, struct ui_rect);    // 将文档字节偏移换成行列，画文字、选区和光标
void ui_tasks_draw(struct ui_desktop *, struct ui_rect);     // 绘制任务快照、CPU曲线和内存条
void ui_system_draw(struct ui_desktop *, struct ui_rect);    // 按当前页显示同一启动信息的内存、设备或日志
void ui_settings_draw(struct ui_desktop *, struct ui_rect);  // 按页面绘制当前会话配色、密度和接口说明
void ui_settings_click(struct ui_desktop *, struct ui_rect, int, int, bool);  // 分别命中页签、主题和密度矩形；只改变当前会话偏好
#endif  // 结束当前应用接口的重复包含保护
```

## 让按键选择和停止任务读取同一份快照

前一章按键只操作Files、Terminal和Editor。现在tasks回调已经复制任务数组，才有selected_task与task_count可供上下键使用。Tasks筛选焦点仍独占文字输入，普通文档光标不会同时移动。停止确认的dialog_target保存参考pid，确认后服务只移除那条参考记录，并不结束一个真实主机进程。确认入口把保存的 pid 交给 UI_STOP；键盘路由则先让 Tasks 的筛选字段接收文字，字段未占用输入时才处理任务上下选择。完整替换以下两个文件，将这两条路径接入原有事件循环。

**完整写入 `frontend/core/commands.c`：**

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
    if (kind == UI_DIALOG_STOP)  // 停止确认提交当前参考任务pid，服务只移除此记录而不结束真实进程
        ui_command(s, UI_STOP, s->dialog_target, "", "");  // 把已确认参考pid交给停止动作，只移除服务记录，不结束真实进程
    if (kind == UI_DIALOG_SAVE_AS)  // 另存提交id为-1的新文档请求，成功后刷新并重新绑定Editor身份
        ui_command(s, UI_SAVE, s->editor_id, s->dialog_input, s->editor);  // 提交另存名称与当前正文，未绑定时id为-1，成功后前端从新快照绑定新id
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
        s->find = false;  // 隐藏Editor查找栏，后续正文可使用这部分高度
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

Tasks 先检查服务状态与 tasks 回调。当前参考回调同时填入完整统计，状态正常且回调存在时便绘制曲线；Tasks 自身没有再次检查 stats.available，因此这一绘图路径要求回调同时提供有效统计。资源侧栏另外按 available 决定是否显示读数。

六十点横坐标按 i/59 分布，纵坐标按 sample/100 缩放；内存量先转 uint64_t 再乘宽度，保护中间计算。任务行按 filter 和 scroll 选取。鼠标点击随后会按可见行还原快照下标，当前上下键则直接移动原快照下标，筛选或反序后仍沿原数组选择。

PID 是进程标识的名称；这里的数值来自 reference 示例表，Stop 修改这份记录，不终止宿主操作系统中的同号进程。

**新建 `frontend/apps/tasks.c`：**

<a id="code-frontend-apps-tasks-c-1"></a>

<!-- write: frontend/apps/tasks.c -->
```c
#include "internal.h"  // 引入当前目录内部共享接口和基础函数

static void chart(struct ui_desktop *s, struct ui_rect r) {  // 把60个百分数采样映射成窗口曲线
    int plotx = r.x + 30, width = r.w - 30;  // CPU曲线左边留30像素给纵轴刻度，余宽表示60个等间隔历史采样
    for (int i = 0; i <= 4; ++i) {  // i取0..4，共五条水平网格和四个高度间隔
        int y = r.y + i * r.h / 4;  // 第i条刻度按i/4分布，从图区顶到图区底均分，屏幕y向下增加
        for (int gx = 0; gx < width; gx += 8)  // 当前CPU刻度每8像素取一个短线起点
            u_rect(plotx + gx, y, 2, 1, U_EDGE);  // 在当前刻度的gx起点写2×1像素短线，八像素间距形成点线
        char v[16];  // 16字节局部数组把当前CPU纵轴百分数转换成可绘制文字
        airi_snprintf(v, 16, "%u", 100 - (unsigned)i * 25);  // 五条刻度依次写100、75、50、25、0，数值与上方网格位置配对
        ui_app_muted(r.x, y - 7, v);  // 在内容左缘、整屏y=y - 7显示v
    }
    for (unsigned i = 1; i < 60; ++i) {  // 按采样下标处理60个参考百分数点
        int x = plotx + (int)i * width / 59, y = r.y + r.h - (int)s->stats.samples[i] * r.h / 100;  // 第i个采样按59个间隔换成横坐标；百分数越高，屏幕y越靠上
        u_line(plotx + (int)(i - 1) * width / 59,  // 从上一采样的横坐标开始，60点对应59个等间隔，不按百分数改变横距
               r.y + r.h - (int)s->stats.samples[i - 1] * r.h / 100, x, y, U_BLUE);  // 上一采样百分数从图底向上换算，连到本次当前点
    }
    ui_app_muted(plotx, r.y + r.h + 13, "60 seconds ago");  // 在整屏x=plotx、整屏y=r.y + r.h + 13显示文字"60 seconds ago"
    ui_app_muted(r.x + r.w - 26, r.y + r.h + 13, "now");  // 在内容右缘左移26像素、整屏y=r.y + r.h + 13显示文字"now"
}

void ui_tasks_draw(struct ui_desktop *s, struct ui_rect r) {  // 绘制任务快照、CPU曲线和内存条
    ui_app_toolbar(r, "Processes");  // 在内容顶端铺44像素工具栏并显示文字"Processes"
    u_button((struct ui_rect){r.x + r.w - 184, r.y + 8, 76, 28}, "Refresh", false, false);  // 画文字"Refresh"按钮；其矩形供后续事件命中共用，当前仅输出像素
    u_button((struct ui_rect){r.x + r.w - 98, r.y + 8, 82, 28}, "End task", false, false);  // 画文字"End task"按钮；其矩形供后续事件命中共用，当前仅输出像素
    if (s->service_state != UI_NORMAL || !s->backend.tasks) {  // 参考服务未正常提供任务查询时不绘旧任务快照，明确显示不可用
        ui_app_unavailable(s, r, "Process and scheduler service");  // 按当前服务状态显示文字"Process and scheduler service"的缺失或失败信息
        ui_app_status(r, "No process snapshot", NULL);  // 在内容底部显示文字"No process snapshot"；右侧说明按测得的文字宽度对齐
        return;  // 当前绘制任务快照、CPU曲线和内存条在本分支结束，后续语句不执行
    }
    int graphheight = r.h >= 540 ? 160 : 100;  // 较高窗口给CPU图区160像素，否则100像素，后面的任务列表起点随之移动
    char label[80];  // 80字节局部数组保存CPU或内存标题数值，绘完当前标题后可复用
    airi_snprintf(label, 80, "CPU   %u%%", s->stats.cpu);  // Tasks标题显示同次参考CPU百分数，后面的CPU曲线读取同次samples
    u_text(r.x + 20, r.y + 56, label, U_TITLE, U_BLUE);  // 在内容左缘右移20像素、内容顶边下移56像素显示当前CPU或内存说明
    chart(s, (struct ui_rect){r.x + 20, r.y + 92, r.w / 2 - 40, graphheight});  // 把左半窗口正文区域交给CPU参考曲线绘制
    int rx = r.x + r.w / 2 + 20;  // 内容右半部再留20像素作为内存显示起点，CPU图区位于左半部
    u_label(rx, r.y + 61, "PHYSICAL MEMORY", U_ACCENT);  // 在整屏x=rx、内容顶边下移61像素显示内存区域标签PHYSICAL MEMORY
    airi_snprintf(label, 80, "%u MiB used / %u MiB total", s->stats.memory_kib / 1024,  // Tasks把参考已用和总内存各从KiB转MiB，说明后面比例条对应哪两个量
                  s->stats.total_kib / 1024);  // 总KiB除1024后作为MiB显示，与已用量单位一致
    ui_app_text(rx, r.y + 92, label);  // 在整屏x=rx、内容顶边下移92像素显示当前CPU或内存说明
    u_rect(rx, r.y + 134, r.w / 2 - 40, 10, U_RAISED);  // 用选中背景色写矩形，起点(rx,r.y + 134)，尺寸r.w / 2 - 40×10像素
    if (s->stats.total_kib)  // 总内存非零时才用它作比例分母
        u_rect(rx, r.y + 134,  // 内存比例条从右半区rx列与内容顶下134像素开始，下一行按已用比例计算宽度
               (int)((uint64_t)s->stats.memory_kib * (unsigned)(r.w / 2 - 40) / s->stats.total_kib),  // 已用KiB除总KiB得到比例；先扩大中间乘积，再换算成内存条像素宽度
               10, U_ACCENT);  // 已用内存比例条高10像素，宽度已按总量缩放
    ui_app_muted(rx, r.y + 164, "Memory usage / session total");  // 在整屏x=rx、内容顶边下移164像素显示文字"Memory usage / session total"
    ui_app_muted(rx, r.y + 187, "Recorded session");  // 在整屏x=rx、内容顶边下移187像素显示文字"Recorded session"
    int top = r.y + graphheight + 139;  // 任务列表起点在CPU图区和标题之后，graphheight影响剩余列表高度
    ui_app_field((struct ui_rect){r.x + 20, top, r.w - 40, 32},  // 画输入字段和当前焦点边框，本次实参取自当前桌面状态
                 s->filter[UI_TASKS_APP][0] ? s->filter[UI_TASKS_APP] : "Filter processes",  // 显示实际任务筛选文字，空时使用操作提示
                 s->focus == UI_FOCUS_FILTER);  // 筛选输入拥有焦点时画强调边框，不改变其他应用自己的筛选数组
    top += 48;  // 筛选输入框后向下留48像素安放任务列标题
    u_rect(r.x + 16, top, r.w - 32, 28, U_PANEL);  // 用工具栏底色写矩形，起点(r.x + 16,top)，尺寸r.w - 32×28像素
    ui_app_muted(r.x + 26, top + 6, "PID");    // 在内容左缘右移26像素、整屏y=top + 6显示文字"PID"
    ui_app_muted(r.x + 100, top + 6, "Name");  // 在内容左缘右移100像素、整屏y=top + 6显示文字"Name"
    ui_app_muted(r.x + r.w - 300, top + 6, "State");  // 在内容右缘左移300像素、整屏y=top + 6显示文字"State"
    ui_app_muted(r.x + r.w - 180, top + 6, "CPU");    // 在内容右缘左移180像素、整屏y=top + 6显示文字"CPU"
    ui_app_muted(r.x + r.w - 92, top + 6, "Memory");  // 在内容右缘左移92像素、整屏y=top + 6显示文字"Memory"
    top += 32;  // 列标题后向下留32像素安放第一条任务行
    unsigned shown = 0, rh = s->density ? 28 : 36;  // shown计匹配后的遍历行；密度决定每行28或36像素
    for (unsigned k = 0; k < s->task_count; ++k) {  // 只遍历本次任务快照有效前缀
        unsigned i = s->sort ? s->task_count - 1 - k : k;  // 排序开关只反转参考任务快照下标，k仍是当前遍历次序
        struct ui_task *t = &s->tasks[i];  // 借用选定快照项的pid、名称与参考资源字段，列表绘图不修改服务任务
        if (!ui_app_match(t->name, s->filter[UI_TASKS_APP]))  // 当前参考任务名称不包含筛选文字时跳过此项
            continue;  // 本项不参与当前结果，跳过本轮剩余操作进入下一项
        if (shown++ < s->windows[UI_TASKS_APP].scroll)  // 匹配项先计入shown，再跳过滚动位置之前的条目
            continue;  // 本项不参与当前结果，跳过本轮剩余操作进入下一项
        int y = top + (int)(shown - 1 - s->windows[UI_TASKS_APP].scroll) * (int)rh;  // 匹配条目减滚动量后按行高换成整屏行，滚动外条目不占可见行
        if (y + (int)rh > r.y + r.h - 34)  // 当前任务行会进入底部状态栏时结束绘图，后续快照仍保存但不可见
            break;  // 结束当前最近一层循环或应用分派，不执行其剩余候选
        if (i == s->selected_task)  // 按原快照下标判断选中项，反序显示不改变任务身份
            u_selection((struct ui_rect){r.x + 16, y, r.w - 32, (int)rh});  // 用选中背景色写矩形，起点(r.x + 16,y)，尺寸r.w - 32×(int)rh像素
        char value[40];  // 40字节局部数组依次格式化当前任务的pid、CPU与MiB内存文字
        airi_snprintf(value, 40, "%d", t->pid);  // 把当前参考任务身份转换成有符号十进制文字，供此任务行PID列
        ui_app_text(r.x + 26, y + 8, value);     // 在内容左缘右移26像素、整屏y=y + 8显示当前任务资源数值文字
        ui_app_text(r.x + 100, y + 8, t->name);  // 在内容左缘右移100像素、整屏y=y + 8显示当前任务名称
        ui_app_text(r.x + r.w - 300, y + 8, t->state);  // 在内容右缘左移300像素、整屏y=y + 8显示当前任务状态
        airi_snprintf(value, 40, "%u%%", t->cpu);    // 把当前任务参考CPU占比附百分号，供此任务行CPU列
        ui_app_text(r.x + r.w - 180, y + 8, value);  // 在内容右缘左移180像素、整屏y=y + 8显示当前任务资源数值文字
        airi_snprintf(value, 40, "%u MiB", t->memory_kib / 1024);  // 将当前任务KiB内存除1024转换成MiB，供此任务行内存列
        ui_app_text(r.x + r.w - 92, y + 8, value);  // 在内容右缘左移92像素、整屏y=y + 8显示当前任务资源数值文字
    }
    ui_app_status(r, "Select a process to inspect or end it.", "Session tasks");  // 在内容底部显示文字"Select a process to inspect or end it."；右侧说明按测得的文字宽度对齐
}

```

<a id="theme-owner"></a>

设置也是状态输入。desktop.theme保存这个桌面的Night/Day偏好。Settings修改这个字段，Terminal的theme命令则经同步扩展回调修改同一字段；文本会话本身不再保存主题。ui_draw直接读取桌面选择的调色板。切换外观只改变颜色解析，现有窗口和文档继续由原来的desktop持有。

Settings 保存会话选项，tab 选择当前页。Appearance页显示Night/Day与density控件，本次已经接入内容点击，Keyboard 显示快捷键，Display 显示软件画布尺寸，About 显示代码与字体许可。密度仅改变Files和Tasks的列表行高：Comfortable用36像素，Compact用28像素；图形字体、Editor行距和窗口标题尺寸仍沿已有度量。对应列表点击也按同一行高换算。关闭整个进程后这些值重置。

tabs字符串数组的四项分别是Appearance、Keyboard、Display、About。内容点击先把工具栏内的点转换为0..3页签，再检查Night和Day各自的完整矩形；两按钮之间的空隙不触发切换。密度行同样只接受两个126×34矩形：从body.x+28与body.x+166开始，顶边为body.y+256；同一高度的右侧空白不属于Compact。

**新建 `frontend/apps/settings.c`：**

<a id="code-frontend-apps-settings-c-1"></a>

<!-- write: frontend/apps/settings.c -->
```c
#include "internal.h"  // 引入当前目录内部共享接口和基础函数

void ui_settings_draw(struct ui_desktop *s, struct ui_rect r) {  // 按页面绘制当前会话配色、密度和接口说明
    const char *tabs[] = {"Appearance", "Keyboard", "Display", "About"};  // Settings四页名称按tab编号排列
    unsigned tab = s->windows[UI_SETTINGS].tab;  // Settings自己的页下标选外观、输入说明或关于页，不复用System页状态
    for (unsigned i = 0; i < 4; ++i)  // 只画Settings四个有效页面按钮
        u_button((struct ui_rect){r.x + 16 + (int)i * 118, r.y + 9, 108, 28}, tabs[i], tab == i,  // 画当前页面按钮名按钮；其矩形供后续事件命中共用，当前仅输出像素
                 false);  // 当前按钮未禁用，实参只控制绘图外观
    int x = r.x + 28, y = r.y + 73;  // Settings正文从内容左边28像素、顶边73像素开始，标题和页签在上方
    u_text(x, y, tabs[tab], U_LABEL, U_TEXT);  // 在整屏x=x、整屏y=y显示已选页面名
    if (tab == 0) {  // Settings第0页显示当前会话日夜模式与界面密度
        ui_app_text(x, y + 49, "Appearance mode");  // 在整屏x=x、整屏y=y + 49显示文字"Appearance mode"
        const char *names[] = {"Night", "Day"};     // 日夜名称顺序与 ui_theme 一致
        for (unsigned i = 0; i < 2; ++i)  // 画两个可选择的会话模式按钮
            u_button((struct ui_rect){x + (int)i * 138, y + 83, 126, 34}, names[i], s->theme == i,  // 画当前模式名称按钮；其矩形供后续事件命中共用，当前仅输出像素
                     false);  // 当前按钮未禁用，实参只控制绘图外观
        ui_app_text(x, y + 148, "Interface density");  // 在整屏x=x、整屏y=y + 148显示文字"Interface density"
        u_button((struct ui_rect){x, y + 183, 126, 34}, "Comfortable", !s->density, false);  // 画文字"Comfortable"按钮；其矩形供后续事件命中共用，当前仅输出像素
        u_button((struct ui_rect){x + 138, y + 183, 126, 34}, "Compact", s->density, false);  // 画文字"Compact"按钮；其矩形供后续事件命中共用，当前仅输出像素
        ui_app_muted(x, y + 244, "Plex Sans / interface   -   Plex Mono / terminal and code");  // 在整屏x=x、整屏y=y + 244显示文字"Plex Sans / interface   -   Plex Mono / terminal and code"
    } else if (tab == 1) {  // Settings第1页显示当前键位与快捷动作说明
        ui_app_text(x, y + 48, "US keyboard layout");      // 在整屏x=x、整屏y=y + 48显示文字"US keyboard layout"
        const char *keys[] = {"Alt-Tab / switch windows",  // 按行保存快捷动作说明，这些文字不是事件处理实现
                              "Ctrl-S / save document",   // 独立快捷键说明数组项，逗号把它与下一项分开
                              "Ctrl-F / find or filter",  // 独立快捷键说明数组项，逗号把它与下一项分开
                              "Ctrl-A, C, X, V / select and clipboard",  // 独立快捷键说明数组项，逗号把它与下一项分开
                              "Ctrl-Z / undo",  // 独立快捷键说明数组项，逗号把它与下一项分开
                              "F5 / request program execution",  // 独立快捷键说明数组项，逗号把它与下一项分开
                              "Esc / close dialog or search"};   // 输入说明数组的最后一项，告知Escape关闭弹框或查找
        for (unsigned i = 0; i < 7; ++i)  // 逐行显示7条当前快捷键说明
            ui_app_text(x, y + 88 + (int)i * 32, keys[i]);  // 在整屏x=x、整屏y=y + 88 + (int)i * 32显示当前快捷键说明
    } else if (tab == 2) {  // Display页显示软件画布尺寸和显示方式
        char size[80];  // 80字节局部数组显示当前软件画布的像素尺寸和渲染方式
        airi_snprintf(size, 80, "%d x %d / software-rendered", s->width, s->height);  // Settings的Display页把软件画布像素宽高与绘图方式格式化成文字
        ui_app_text(x, y + 49, size);  // 在Display页显示刚格式化的软件画布宽高，不是文件字节数
        ui_app_text(x, y + 90, "1280 x 800 / baseline layout");  // 在整屏x=x、整屏y=y + 90显示文字"1280 x 800 / baseline layout"
        ui_app_text(x, y + 126, "1024 x 768 / compact layout");  // 在整屏x=x、整屏y=y + 126显示文字"1024 x 768 / compact layout"
        ui_app_text(x, y + 162, "1600 x 900 / expanded workspace");  // 在整屏x=x、整屏y=y + 162显示文字"1600 x 900 / expanded workspace"
        ui_app_muted(x, y + 225, "Window geometry adapts to the available work area.");  // 在整屏x=x、整屏y=y + 225显示文字"Window geometry adapts to the available work area."
    } else {  // 前一分支条件未成立时处理当前替代路径
        ui_app_text(x, y + 49, "AiriOS");  // 在整屏x=x、整屏y=y + 49显示文字"AiriOS"
        ui_app_text(x, y + 89, "Personal workstation");  // 在整屏x=x、整屏y=y + 89显示文字"Personal workstation"
        ui_app_muted(x, y + 142, "Original interface code: MIT");  // 在整屏x=x、整屏y=y + 142显示文字"Original interface code: MIT"
        ui_app_muted(x, y + 174, "IBM Plex fonts: SIL Open Font License");  // 在整屏x=x、整屏y=y + 174显示文字"IBM Plex fonts: SIL Open Font License"
    }
    ui_app_status(r, "Preferences apply to this session.", NULL);  // 在内容底部显示文字"Preferences apply to this session."；右侧说明按测得的文字宽度对齐
}
void ui_settings_click(struct ui_desktop *s, struct ui_rect r, int x, int y, bool twice) {  // 设置点击只修改共享界面选项，不写系统配置文件。
    (void)twice;  // 统一点击接口保留双击参数；这个应用当前只使用点击位置。
    enum ui_app app = UI_SETTINGS;  // 页签状态属于Settings窗口。
    if (y < r.y + 45) {  // 内容顶部45像素属于页签区。
        int tab = (x - r.x - 16) / 118;  // 按16像素内边距和118像素页签宽度定位。
        if (tab >= 0 && tab < 4)  // Settings只接受0..3页签。
            s->windows[app].tab = (unsigned)tab;  // 边界检查后再转无符号下标。
        return;  // 本次事件处理已结束，不再落入其他按钮或底层窗口。
    }
    if (s->windows[app].tab == 0) {  // 当前可修改主题选项位于首页。
        for (unsigned i = 0; i < 2; ++i)  // 用与绘制完全相同的矩形，间隙不属于按钮
            if (u_inside((struct ui_rect){r.x + 28 + (int)i * 138, r.y + 156, 126, 34}, x, y)) {  // Night与Day的真实点击区
                s->theme = i;  // 不调用初始化，不触碰文档和窗口状态
            }
        for (unsigned i = 0; i < 2; ++i)  // 密度也逐个命中真实按钮，不把整行空白当动作区。
            if (u_inside((struct ui_rect){r.x + 28 + (int)i * 138, r.y + 256, 126, 34}, x, y))  // 两按钮之间十二像素空隙不属于任何矩形。
                s->density = i;  // 下标0宽松、1紧凑，行高选择与实际命中对象一致。
    }
}
```

将Tasks与Settings加入ui_apps_draw，并先让ui_apps_click只分派Settings。这使普通C观察能向主题按钮送入坐标；来自原生鼠标的窗口命中仍在G20–G21接入。超出枚举的值仍由 default 返回，调用范围保持明确。

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
    default:  // 未知应用编号不绘制内容；窗口框仍由外层处理
        break;  // 当前匹配已完成，停止这一层循环或分支。
    }
}

void ui_apps_click(struct ui_desktop *s, enum ui_app app, struct ui_rect r, int x, int y,  // 总入口先按应用身份分派内容点击。
                   bool twice) {  // 双击标志保持原值交给应用，不在桌面重复解释。
    s->focus = UI_FOCUS_NONE;  // 先取消旧字段焦点，应用命中字段后可重新设置。
    switch (app) {     // 按已命中的应用编号选择点击处理器
    case UI_SETTINGS:  // 应用身份为设置时进入对应分支。
        ui_settings_click(s, r, x, y, twice);  // 将当前矩形、屏幕点和双击状态交给设置的命中规则。
        break;  // 当前匹配已完成，停止这一层循环或分支。
    default:  // 未知或本阶段尚未接点击的应用不改变状态
        break;  // 当前匹配已完成，停止这一层循环或分支。
    }
}
```

使用同一观察程序分别打开Tasks和Settings两个窗口。任务曲线仍来自声明的参考数据；设置只改变当前会话。

**在学习目录根目录运行：**

<!-- run -->
```sh
mkdir -p build  # 保存本次主机程序和图像，已有目录可以继续
cc -std=c11 -O0 -g -Wall -Wextra -Werror -Iinclude -Ifrontend/include labs/tutorial/desktop.c kernel/lib/base.c kernel/boot/info.c kernel/console/console.c frontend/core/*.c frontend/apps/*.c frontend/backends/reference.c frontend/render/draw.c -o build/desktop  # Shell展开两个通配符，把当前已经写出的核心和应用一起链接
./build/desktop  # 运行当前完整观察程序，退出非零表示本组观察失败
./build/desktop 3  # Tasks显示同次参考快照中的任务与资源
cp build/desktop.ppm build/tasks.ppm  # 保存Tasks帧
./build/desktop 5  # Settings显示当前会话偏好
cp build/desktop.ppm build/settings.ppm  # 保存Settings帧
```

Tasks 显示 reference 曲线，Settings 显示会话选项。读取曲线文件需要下一步完成字符到数值的转换。

## 切换外观，保留正在编辑的文档

下面在同一个进程里将main.lua替换为print(42)加换行，保留原saved，再全选这份未保存文字。随后打开Settings，直接对内容矩形送入Day和Night按钮内的坐标。观察程序各保存一份文字、保存点和窗口字节；选区比较cursor与anchor，未保存状态比较dirty。ui_apps_click会清旧字段focus，但不会重建desktop或清editor，所以正文、保存点、选择和窗口保持原值。

**完整写入 `labs/tutorial/theme.c`：**

<a id="code-labs-tutorial-theme-c-1"></a>

<!-- write: labs/tutorial/theme.c -->
```c
#include "ui.h"      // 取得会话、窗口与服务的共享接口
#include <stdio.h>   // 主机观察使用puts输出实际结果
#include <string.h>  // 主机比较文档副本和窗口状态的字节
static struct ui_desktop desktop;    // 观察前后修改同一桌面，不重新初始化
static struct airi_console console;  // 保存文本输入与日志；主题偏好由同一desktop持有
int main(void) {  // 在普通主机进程观察设置操作留下哪些状态
    struct ui_backend backend;     // 参考服务回调表，桌面按值复制
    ui_fixture_backend(&backend);  // 创建本会话参考文件和记录
    airi_console_init(&console, backend.boot);  // 控制台借用同一启动描述
    ui_init(&desktop, backend, &console);      // 读入main.lua并建立初始窗口
    ui_key(&desktop, UI_SELECT_ALL, false);    // 先在Editor选中整份初始文档
    strcpy(desktop.clipboard, "print(42)\n");  // 十字节替换内容放进独立剪贴板
    ui_key(&desktop, UI_PASTE, false);  // 替换工作正文，保存点仍为原文件
    ui_key(&desktop, UI_SELECT_ALL, false);  // 让主题切换时存在一个实际选区
    ui_open(&desktop, UI_SETTINGS);  // 设置成为活动窗口，编辑文档继续保留
    struct ui_rect body = u_window_body(desktop.windows[UI_SETTINGS].rect);  // 由同一外框扣除48像素标题与边框
    char text[UI_DOC_SIZE], saved[UI_DOC_SIZE];   // 两份独立观察副本，不借用会改变的数组
    memcpy(text, desktop.editor, sizeof(text));   // 保存切换前的工作文字全部字节
    memcpy(saved, desktop.saved, sizeof(saved));  // 保存切换前的服务保存点
    unsigned cursor = desktop.cursor, anchor = desktop.anchor;  // 记录选区的两个字节间隙
    bool dirty = desktop.dirty, selection = desktop.selection;  // 记录未保存标记和选区有效性
    struct ui_window windows[UI_APP_COUNT];  // 观察窗口位置、页签与可见状态
    memcpy(windows, desktop.windows, sizeof(windows));  // 保存全部窗口字节，设置点击不应改它们
    ui_apps_click(&desktop, UI_SETTINGS, body, body.x + 28 + 138 + 10, body.y + 166, false);  // 命中Day按钮内部，直接调用已接入的内容分派
    if (desktop.theme != UI_DAY)  // 桌面拥有的呈现偏好应立即变为Day
        return 1;  // 模式未改变，后续状态观察不能视为成功
    ui_apps_click(&desktop, UI_SETTINGS, body, body.x + 28 + 10, body.y + 166, false);  // 再命中Night按钮，沿同一内容接口返回夜间
    if (desktop.theme != UI_NIGHT)  // Night同样写入桌面自己持有的状态
        return 1;  // 第二次切换没有正确执行
    ui_apps_click(&desktop, UI_SETTINGS, body, body.x + 160, body.y + 266, false);  // 点两密度按钮之间的十二像素空隙，不能改变初始宽松模式。
    ui_apps_click(&desktop, UI_SETTINGS, body, body.x + body.w - 1, body.y + 266, false);  // 点同一高度的右侧空白，也不是Compact按钮。
    if (desktop.density)  // 两次未命中都必须保持0。
        return 1;  // 宽泛横条命中会在右侧空白处错误切到紧凑模式。
    ui_apps_click(&desktop, UI_SETTINGS, body, body.x + 176, body.y + 266, false);  // 真正点Compact矩形内部。
    if (desktop.density != 1)  // 正确按钮需要产生真实状态改变。
        return 1;  // 不能通过忽略全部输入来满足空白不变。
    ui_apps_click(&desktop, UI_SETTINGS, body, body.x + 38, body.y + 266, false);  // 再点Comfortable矩形内部返回宽松模式。
    if (desktop.density)  // 返回0，后面的文档比较沿原来的密度继续。
        return 1;  // 第二个实际按钮也需要正确分派。
    puts("density: gaps and blank preserve state; both buttons change it");  // 报告同一会话中四次真实点击的结果。
    if (memcmp(text, desktop.editor, sizeof(text)) || memcmp(saved, desktop.saved, sizeof(saved)))  // 两个文档数组必须保留每个字节
        return 1;  // 外观操作误改了文档内容或保存点
    if (cursor != desktop.cursor || anchor != desktop.anchor || dirty != desktop.dirty || selection != desktop.selection)  // 文档的选择和保存状态也应保留
        return 1;  // 模式以外的文档状态发生变化
    if (memcmp(windows, desktop.windows, sizeof(windows)))  // 全部窗口几何与页面字节仍应相同
        return 1;  // 主题按钮不应重新建立窗口
    puts("Day -> Night: text, saved, selection and windows preserved");  // 输出这次实际状态比较完成的结果
    return 0;  // 所有观察均符合当前设置合同
}
```

**在学习目录根目录运行：**

<!-- run -->
```sh
mkdir -p build  # 保存本次主机程序和图像，已有目录可以继续
cc -std=c11 -O0 -g -Wall -Wextra -Werror -Iinclude -Ifrontend/include labs/tutorial/theme.c kernel/lib/base.c kernel/boot/info.c kernel/console/console.c frontend/core/*.c frontend/apps/*.c frontend/backends/reference.c frontend/render/draw.c -o build/theme  # Shell展开两个通配符，把当前已经写出的核心和应用一起链接
./build/theme  # 运行当前完整观察程序，退出非零表示本组观察失败
```

输出`Day -> Night: text, saved, selection and windows preserved`。关闭整个进程后，会话偏好与参考文件一同结束；当前没有向磁盘写入设置。

密度观察还输出`density: gaps and blank preserve state; both buttons change it`。观察先点间隙和右侧空白，再点Compact与Comfortable，拒绝误触发和接受有效按钮分别由同一程序验证。
