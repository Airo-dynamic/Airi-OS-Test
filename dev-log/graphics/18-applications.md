# G18 · 在未保存文档旁切换主题并查看资源

main.lua已经能编辑、撤销与保存。现在把它替换为print(42)加换行，先保留未保存状态和选区，再打开Settings切到Day、切回Night。外观改变后回到Editor，仍应继续操作同一份文字，保存点、选择与窗口几何也应保留。为此，Settings 只改 desktop 保存的主题偏好，同一份桌面继续持有文档和窗口。随后Tasks和资源侧栏读取同一次参考服务快照，显示明确标作参考数据的任务与统计；CSV仍留待下一章解析。

这次新增任务查询，但不用重建文档。desktop 已持有未保存的 editor、原 saved 和选区，theme 也是它的会话偏好。任务则由服务保存 records，再通过 tasks 回调复制到 desktop.tasks 与 stats。比如 pid=14 的 series-worker 是一条服务记录，selected_task 只选择桌面副本中的位置；后面的停止请求须取出该项 pid。先加入任务合同与显示状态。

依次更新 ui.h 的下列唯一旧窗口，加入任务容量、记录/统计类型与回调，桌面再持有查询副本、选择和资源栏开关。未列出的文档与窗口字段保留。

<a id="code-frontend-include-ui-h-1"></a>

<!-- replace: frontend/include/ui.h -->
<!-- before -->
```c
#define UI_DOC_SIZE 8192  // 文档总容量含结尾NUL，最多保存8191文本字节。
#define UI_FILES 32  // 单次文件快照至多32条。

enum ui_app {  // 应用身份从零递增，与平台名称表和窗口数组下标对应。
```
<!-- after -->
```c
#define UI_DOC_SIZE 8192  // 文档总容量含结尾NUL，最多保存8191文本字节。
#define UI_FILES 32  // 单次文件快照至多32条。
#define UI_TASKS 16  // 单次任务快照至多16条。

enum ui_app {  // 应用身份从零递增，与平台名称表和窗口数组下标对应。
```

<!-- replace: frontend/include/ui.h -->
<!-- before -->
```c
    UI_CREATE_FILE,  // 在显式目录下建立普通文件。
    UI_CREATE_DIR,   // 建立目录记录。
    UI_RENAME,     // 保留id，修改对应记录名称。
    UI_DELETE,     // 删除id对应记录。
    UI_EXECUTE,    // 请求执行文档，当前参考服务报告未连接。
    UI_COPY_FILE,  // 在目标目录创建源文件的副本。
    UI_MOVE_FILE   // 把源id对应记录移到目标目录。
```
<!-- after -->
```c
    UI_CREATE_FILE,  // 在显式目录下建立普通文件。
    UI_CREATE_DIR,   // 建立目录记录。
    UI_RENAME,   // 保留id，修改对应记录名称。
    UI_DELETE,   // 删除id对应记录。
    UI_EXECUTE,  // 请求执行文档，当前参考服务报告未连接。
    UI_STOP,     // 删除可停止的参考任务记录，不终止宿主进程。
    UI_COPY_FILE,  // 在目标目录创建源文件的副本。
    UI_MOVE_FILE   // 把源id对应记录移到目标目录。
```

<!-- replace: frontend/include/ui.h -->
<!-- before -->
```c
    unsigned bytes;  // 文件内容字节数，目录行可显示其他语义。
};
/* Providers own service data. A backend error is an interface state, never fake success. */
struct ui_backend {  // 桌面复制这份服务回调表，服务数据继续由提供方持有。
```
<!-- after -->
```c
    unsigned bytes;  // 文件内容字节数，目录行可显示其他语义。
};
struct ui_task {  // 当前服务提供的一条任务记录。
    int pid;  // 任务服务身份，停止动作传它而不是可见行号。
    char name[32], state[16];  // 保存任务名称和状态文本，均含NUL容量。
    unsigned cpu, memory_kib;  // CPU百分数与内存KiB，绘图按对应单位输出。
};
struct ui_stats {  // 一次任务查询同时返回的汇总状态。
    bool available;  // false表示当前没有真实汇总数据，不能绘制伪装的零值。
    unsigned cpu, memory_kib, total_kib, read_kib, write_kib;  // CPU 按百分数，内存/总量按 KiB，读写速率按 KiB/s 分别保存。
    unsigned samples[60];  // 固定 60 个百分数采样值，图表逐项消费。
};
/* Providers own service data. A backend error is an interface state, never fake success. */
struct ui_backend {  // 桌面复制这份服务回调表，服务数据继续由提供方持有。
```

<!-- replace: frontend/include/ui.h -->
<!-- before -->
```c
    bool (*read)(void *, int, char *, size_t);  // 按文件id读取字节到给定容量数组，false表示失败。
    bool (*action)(void *, const char *, enum ui_action, int, const char *, const char *, char *, size_t);  // 接收目录、动作、id、名称、正文及错误缓冲，返回成功与否。
};
struct ui_window {  // 一个应用的窗口布局与显示状态。
```
<!-- after -->
```c
    bool (*read)(void *, int, char *, size_t);  // 按文件id读取字节到给定容量数组，false表示失败。
    bool (*action)(void *, const char *, enum ui_action, int, const char *, const char *, char *, size_t);  // 接收目录、动作、id、名称、正文及错误缓冲，返回成功与否。
    unsigned (*tasks)(void *, struct ui_task *, unsigned, struct ui_stats *);  // 输出任务数组与汇总，返回有效任务数。
};
struct ui_window {  // 一个应用的窗口布局与显示状态。
```

<!-- replace: frontend/include/ui.h -->
<!-- before -->
```c
    unsigned order[UI_APP_COUNT];  // 保存从底到顶的应用身份，命中逆向读取。
    unsigned order_count;  // order中已登记的有效数量。
    int active;  // 活动应用，-1表示当前没有。
    int width;   // 最近绘图的桌面宽度，单位像素。
    int height;  // 最近绘图的桌面高度，单位像素。
    bool find;   // Editor查找栏是否展开。
    bool dirty;  // 工作文本是否与最近成功保存内容不同。
    bool selection;      // anchor与cursor之间是否构成有效选区。
    bool field_all;      // 对话框字段是否处于全选替换状态。
```
<!-- after -->
```c
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
```

<!-- replace: frontend/include/ui.h -->
<!-- before -->
```c
    unsigned editor_column_scroll;  // 正文左侧已卷走的字符列数。
    unsigned selection_file;  // 文件快照数组下标，不是文件id或排序后行。
    int editor_id;  // 当前文档服务id，未保存的新文档另按创建路径处理。
    int dialog;     // 当前对话框枚举值，零为无。
```
<!-- after -->
```c
    unsigned editor_column_scroll;  // 正文左侧已卷走的字符列数。
    unsigned selection_file;  // 文件快照数组下标，不是文件id或排序后行。
    unsigned selected_task;   // 任务快照数组下标，停止动作再读取pid。
    int editor_id;  // 当前文档服务id，未保存的新文档另按创建路径处理。
    int dialog;     // 当前对话框枚举值，零为无。
```

<!-- replace: frontend/include/ui.h -->
<!-- before -->
```c
    struct ui_file files[UI_FILES];  // 当前目录的有界快照记录。
    unsigned file_count;  // files中有效条数，读取不得超过它。
    unsigned theme;    // 桌面自己持有的日夜模式，文本会话不依赖此字段。
    unsigned density;  // Files列表行高，0为36像素、非0为28像素；当前绘图读取此值。
};
void ui_init(struct ui_desktop *, struct ui_backend, struct airi_console *);  // 初始化桌面，复制服务表并借用命令对象。
```
<!-- after -->
```c
    struct ui_file files[UI_FILES];  // 当前目录的有界快照记录。
    unsigned file_count;  // files中有效条数，读取不得超过它。
    struct ui_task tasks[UI_TASKS];  // 当前任务服务的有界快照。
    unsigned task_count;    // tasks中有效条数。
    struct ui_stats stats;  // 与任务快照同时取得的汇总。
    unsigned theme;    // 桌面自己持有的日夜模式，文本会话不依赖此字段。
    unsigned density;  // Files/Tasks列表行高，0为36像素、1为28像素；相应绘图和列表命中共用。
};
void ui_init(struct ui_desktop *, struct ui_backend, struct airi_console *);  // 初始化桌面，复制服务表并借用命令对象。
```

<!-- replace: frontend/include/ui.h -->
<!-- before -->
```c
bool ui_key(struct ui_desktop *, int key, bool shift);      // 分派字符或功能键，Shift控制扩展选区等规则。
void ui_service_state(struct ui_desktop *, enum ui_state);  // 切换服务显示状态并刷新对应快照。
void ui_refresh(struct ui_desktop *);  // 从当前目录取得最多32条文件元信息副本。
void ui_command(struct ui_desktop *, enum ui_action, int, const char *, const char *);  // 把稳定身份、名称与内容交给服务动作回调。
void ui_fixture_backend(struct ui_backend *);  // 为主机观察填入会话内存参考服务，不进入guest。
```
<!-- after -->
```c
bool ui_key(struct ui_desktop *, int key, bool shift);      // 分派字符或功能键，Shift控制扩展选区等规则。
void ui_service_state(struct ui_desktop *, enum ui_state);  // 切换服务显示状态并刷新对应快照。
void ui_refresh(struct ui_desktop *);  // 从当前服务取得文件、任务与汇总快照。
void ui_command(struct ui_desktop *, enum ui_action, int, const char *, const char *);  // 把稳定身份、名称与内容交给服务动作回调。
void ui_fixture_backend(struct ui_backend *);  // 为主机观察填入会话内存参考服务，不进入guest。
```

<!-- replace: frontend/include/ui.h -->
<!-- before -->
```c
void ui_open_document(struct ui_desktop *, unsigned);  // 接收快照下标，按服务id读取普通文档并打开Editor。
void ui_apps_draw(struct ui_desktop *, enum ui_app, struct ui_rect);  // 以应用身份分派内容绘图，rect已扣除标题栏。
#endif  // 结束完整桌面接口的重复包含保护
```
<!-- after -->
```c
void ui_open_document(struct ui_desktop *, unsigned);  // 接收快照下标，按服务id读取普通文档并打开Editor。
void ui_apps_draw(struct ui_desktop *, enum ui_app, struct ui_rect);  // 以应用身份分派内容绘图，rect已扣除标题栏。
void ui_apps_click(struct ui_desktop *, enum ui_app, struct ui_rect, int, int, bool);  // 将内容矩形、屏幕点与双击状态交给具体应用。
#endif  // 结束完整桌面接口的重复包含保护
```

<a id="task-samples"></a>

Tasks 中的任务记录描述“某个执行单位叫什么、处于什么状态、用了多少资源”。真正的操作系统进程还涉及地址空间与调度，但当前没有这些内核机制；reference 的 pid 和状态只是服务中的示例记录，Stop 改这份表。真实内核接入前，不将这些曲线解释成 QEMU CPU 或 guest 任务实测。

图中六十个 sample 是 0..100 的百分数记录。画图先把数组下标变成横坐标，再把百分数变成高度。例如值 25 在高一百像素的图中占二十五像素；屏幕 y 向下，所以点的 y 要从图底减去这个高度。

reference.c 继续沿用文件容器和保存路径。现在依次替换以下唯一旧窗口，加入服务拥有的任务表、UI_STOP 分支与 tasks 查询，并更新本章参考内容。每处先定位旧块，只把新块写回；未出现的文件读写部分保留。停止只压紧 records 的有效前缀，pid 1、2 受保护；没有向主机发送终止信号。

<a id="code-frontend-backends-reference-c-1"></a>

<!-- replace: frontend/backends/reference.c -->
<!-- before -->
```c
};
static struct reference_file entries[UI_FILES];  // 服务拥有32个静态文件槽，跨查询保留而退出进程丢失
static AiriBootInfo boot;  // 参考服务持有示例启动结构，借给System与console使用
static void add(unsigned slot, const char *path, const char *name, bool directory,  // 把路径、名称和文字填进指定内存服务槽
                const char *body) {  // body为新服务记录的NUL正文，目录可传空串
```
<!-- after -->
```c
};
static struct reference_file entries[UI_FILES];  // 服务拥有32个静态文件槽，跨查询保留而退出进程丢失
static struct ui_task records[UI_TASKS];  // 服务拥有最多16条静态参考任务记录
static unsigned record_count;  // 保存静态任务数组的有效前缀长度
static AiriBootInfo boot;      // 参考服务持有示例启动结构，借给System与console使用
static void add(unsigned slot, const char *path, const char *name, bool directory,  // 把路径、名称和文字填进指定内存服务槽
                const char *body) {  // body为新服务记录的NUL正文，目录可传空串
```

<!-- replace: frontend/backends/reference.c -->
<!-- before -->
```c
    if (act == UI_EXECUTE)  // 执行请求暂不启动主机或guest程序，返回真实未连接错误
        return error(out, cap,  // 把失败原因写给调用者，false阻止前端误记为成功
                     "Application execution is not connected. The source stays in the editor; no "  // 继续当前固定文字；转义换行属于文档内容，C相邻字符串在编译时连接
                     "guest or host process was started.");  // 结束未连接错误正文：本请求既未启动guest程序也未启动主机程序
    if (act == UI_CREATE_FILE || act == UI_CREATE_DIR || (act == UI_SAVE && id < 0)) {  // 创建项目和未绑定id的另存都需要新槽；已存在文档的保存后面按id处理
        if (!valid_name(name))  // 先检查名称形状，失败时不会拼路径或占用服务槽
```
<!-- after -->
```c
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
```

<!-- replace: frontend/backends/reference.c -->
<!-- before -->
```c
                return true;  // 该服务修改已完整提交，前端可重新查询当前记录
            }
        return error(out, cap, "Reference workspace capacity reached (32 items).");  // 拒绝本次服务修改并向调用者报告：Reference workspace capacity reached (32 items)
    }
    if (id < 1 || id > UI_FILES || !entries[id - 1].used)  // 拒绝1到32之外或已删除的服务id，检查完成前不访问id-1槽
```
<!-- after -->
```c
                return true;  // 该服务修改已完整提交，前端可重新查询当前记录
            }
        return error(out, cap, "Session storage capacity reached (32 items).");  // 拒绝本次服务修改并向调用者报告：Session storage capacity reached (32 items)
    }
    if (id < 1 || id > UI_FILES || !entries[id - 1].used)  // 拒绝1到32之外或已删除的服务id，检查完成前不访问id-1槽
```

<!-- replace: frontend/backends/reference.c -->
<!-- before -->
```c
                return true;  // 该服务修改已完整提交，前端可重新查询当前记录
            }
        return error(out, cap, "Reference workspace capacity reached.");  // 拒绝本次服务修改并向调用者报告：Reference workspace capacity reached
    }
    if (act == UI_SAVE) {  // 保存请求只修改这个服务记录的文字与长度
```
<!-- after -->
```c
                return true;  // 该服务修改已完整提交，前端可重新查询当前记录
            }
        return error(out, cap, "Session storage capacity reached.");  // 拒绝本次服务修改并向调用者报告：Session storage capacity reached
    }
    if (act == UI_SAVE) {  // 保存请求只修改这个服务记录的文字与长度
```

<!-- replace: frontend/backends/reference.c -->
<!-- before -->
```c
            return error(out, cap, "Invalid file name.");  // 拒绝本次服务修改并向调用者报告：Invalid file name
        if (f->entry.directory)  // 目录重命名还需同步改变全部后代路径，本参考服务不实现该操作
            return error(out, cap, "Folder renaming requires the future VFS provider.");  // 拒绝本次服务修改并向调用者报告：Folder renaming requires the future VFS provider
        char target[128];  // 先在局部数组形成完整目标路径，通过校验后才改服务
        airi_snprintf(target, sizeof(target), "%s", f->entry.path);  // 将原完整路径复制到128字节临时数组，随后只替换最后名称
```
<!-- after -->
```c
            return error(out, cap, "Invalid file name.");  // 拒绝本次服务修改并向调用者报告：Invalid file name
        if (f->entry.directory)  // 目录重命名还需同步改变全部后代路径，本参考服务不实现该操作
            return error(out, cap, "Folder renaming is unavailable.");  // 目录重命名需要同步后代路径，本参考服务明确返回未提供该能力。
        char target[128];  // 先在局部数组形成完整目标路径，通过校验后才改服务
        airi_snprintf(target, sizeof(target), "%s", f->entry.path);  // 将原完整路径复制到128字节临时数组，随后只替换最后名称
```

<!-- replace: frontend/backends/reference.c -->
<!-- before -->
```c
    return error(out, cap, "Unsupported workspace operation.");  // 拒绝本次服务修改并向调用者报告：Unsupported workspace operation
}
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
```
<!-- after -->
```c
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
```

<!-- replace: frontend/backends/reference.c -->
<!-- before -->
```c
                          .kernel_size = 132 * 1024,  // 设置参考内核镜像长度
                          .region_count = 4};  // 设置参考内存区域有效条数
    airi_snprintf(boot.loader, 32, "Reference / UEFI");  // 写明这份启动结构属于主机参考数据，System据此区别实际guest启动
    boot.regions[0] =  // 参考地图第0项描述最低1MiB保留范围，下一行填完整记录
        (struct airi_memory_region){.base = 0, .length = 0x100000, .kind = AIRI_MEMORY_RESERVED};  // 复合字面量按字段名给起点0、长度1MiB和保留类别
```
<!-- after -->
```c
                          .kernel_size = 132 * 1024,  // 设置参考内核镜像长度
                          .region_count = 4};  // 设置参考内存区域有效条数
    airi_snprintf(boot.loader, 32, "UEFI inventory");  // 写明这份启动结构属于主机参考数据，System据此区别实际guest启动
    boot.regions[0] =  // 参考地图第0项描述最低1MiB保留范围，下一行填完整记录
        (struct airi_memory_region){.base = 0, .length = 0x100000, .kind = AIRI_MEMORY_RESERVED};  // 复合字面量按字段名给起点0、长度1MiB和保留类别
```

<!-- replace: frontend/backends/reference.c -->
<!-- before -->
```c
                             .files = files,  // 保存目录查询函数地址，前端提供目录与数组，回调复制元信息而不出借服务槽
                             .read = read,    // 服务合同保存完整正文读取函数地址，桌面传服务id与调用者数组
                             .action = action};  // 服务合同保存操作函数地址，修改请求与失败正文通过该回调交接
}
```
<!-- after -->
```c
                             .files = files,  // 保存目录查询函数地址，前端提供目录与数组，回调复制元信息而不出借服务槽
                             .read = read,    // 服务合同保存完整正文读取函数地址，桌面传服务id与调用者数组
                             .action = action,  // 保存参考文件/任务操作函数地址，前端请求经它修改服务表或取得失败说明
                             .tasks = tasks};   // 把任务和同次资源统计查询函数地址交给桌面，服务记录仍归参考后端
}
```

参考服务初始化五条记录，tasks 将有效前缀复制进调用者数组，并同次填入 stats：CPU=37%，内存104 MiB/256 MiB，读写416/128 KiB/s，六十个历史点由固定公式生成。这些值供两个视图共同消费，不是启动了后台采样线程。

桌面刷新调用 tasks(backend.context, s->tasks, UI_TASKS, &s->stats)，分别交出上下文、十六槽任务数组、容量与统计地址。刷新先清旧计数及统计；服务正常才查询，返回条数限在容量内，CPU与每个采样限到100，已用内存限到总量。停止服务返回成功后仍由 ui_command 再刷新，不能仅因点过一行便认为已停止。

在 state.c 中依次定位以下旧窗口并替换为完整新块。ui_refresh 接住任务与统计，其余初始化、窗口打开和动作函数继续共用这份 desktop；未列出的实现保留。

<a id="code-frontend-core-state-c-1"></a>

<!-- replace: frontend/core/state.c -->
<!-- before -->
```c
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
```
<!-- after -->
```c
    airi_console_print(console, "Appearance: %s / theme day | theme night", desktop->theme ? "Day" : "Night");  // 查询和设置都报告实际呈现状态。
    return true;  // 表示本条命令已经同步处理。
}
void ui_refresh(struct ui_desktop *s) {  // 重新取得文件任务快照，更新显示所需有效计数
    s->file_count = 0;  // 刷新前清有效文件条数，失败时不显示旧成功快照
    s->task_count = 0;  // 刷新前清有效任务条数，随后由任务服务填写
    airi_memset(&s->stats, 0, sizeof(s->stats));  // 清零整个当前状态或快照，使计数和标记从确定初值开始
    if (s->service_state != UI_NORMAL)  // 服务未正常时停止复制或显示成功快照
        return;  // 服务未处于正常状态，本轮不发布成功快照
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
```

<!-- replace: frontend/core/state.c -->
<!-- before -->
```c
            }
    }
}

void ui_open(struct ui_desktop *s, enum ui_app app) {  // 打开或置顶一个应用，更新窗口次序和活动编号
    if (app >= UI_APP_COUNT)  // 应用编号超出窗口数组范围时拒绝打开
        return;  // 应用编号未通过范围检查，不访问窗口数组或改变次序
```
<!-- after -->
```c
            }
    }
}
void ui_open(struct ui_desktop *s, enum ui_app app) {  // 打开或置顶一个应用，更新窗口次序和活动编号
    if (app >= UI_APP_COUNT)  // 应用编号超出窗口数组范围时拒绝打开
        return;  // 应用编号未通过范围检查，不访问窗口数组或改变次序
```

<!-- replace: frontend/core/state.c -->
<!-- before -->
```c
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
```
<!-- after -->
```c
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
```

<!-- replace: frontend/core/state.c -->
<!-- before -->
```c
                      action == UI_SAVE ? " Your edits are still open." : "");  // 追加恢复说明，不清空编辑器或重设保存点。
    }
}

```
<!-- after -->
```c
                      action == UI_SAVE ? " Your edits are still open." : "");  // 追加恢复说明，不清空编辑器或重设保存点。
    }
}
```

同一次刷新现在取得文件、任务及统计的有效副本；旧文件选择或任务选择超出有效条数时归零。ui_init 和成功的 ui_command 都调用这个刷新，因此后续 Tasks 与资源栏读取的是同一次回调结果，不另发两次查询。

<a id="resource-sidebar"></a>

资源侧栏与Tasks借用同一次ui_refresh得到的stats。monitor开启且画布至少1120像素宽时，右侧预留268像素；最大化窗口从可用宽度扣除它，绘图与后续命中共享同一判断。资源栏先检查stats.available，无数据时写No runtime samples；参考快照存在时才画CPU百分比、内存KiB换算出的MiB和读写KiB/s。CPU的60个样本按下标均分59个间隔，屏幕y向下，所以100%画在顶部、0%画在底部。

paint.c 在现有窗口绘制之外加入 monitor。依次更新以下三处唯一旧窗口：先把资源绘制 helper 放在 ui_draw 前，再令 right 按 monitor 与1120阈值预留268，最后在窗口绘完后、底栏之前调用 helper。状态与取色仍来自本次 desktop；旧块只用于定位。

<a id="code-frontend-core-paint-c-1"></a>

<!-- replace: frontend/core/paint.c -->
<!-- before -->
```c
static const char *titles[] = {"Terminal", "Files",    "Editor",     "Tasks",  // 窗口标题按应用枚举顺序排，应用编号可直接索引
                               "System",   "Settings", "Data viewer"};  // 继续应用枚举4到6的窗口标题，数组顺序与应用id一致
void ui_draw(struct ui_desktop *s, const struct airi_framebuffer *f) {  // 按窗口层次画整帧，并将对话框留在最后
    if (!airi_framebuffer_valid(f))  // 画布地址、尺寸或通道无效时，在写像素之前结束
```
<!-- after -->
```c
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
```

<!-- replace: frontend/core/paint.c -->
<!-- before -->
```c
            u_rect(25, y + 25, 6, 2, U_ACCENT);  // 用会话强调色写矩形，起点(25,y + 25)，尺寸6×2像素
    }
    int right = 0;  // 本阶段还没有资源侧栏，窗口可用区不需预留右侧宽度
    unsigned first = 0;  // 默认从最底层窗口开始绘制，后面找可遮挡底层的最大化窗口
    for (unsigned n = 0; n < s->order_count; ++n) {  // 按有效窗口层次遍历，数组后部不作窗口身份
```
<!-- after -->
```c
            u_rect(25, y + 25, 6, 2, U_ACCENT);  // 用会话强调色写矩形，起点(25,y + 25)，尺寸6×2像素
    }
    int right = s->monitor && s->width >= UI_MONITOR_MIN ? 268 : 0;  // 资源栏开启且屏幕够宽才预留268像素，否则窗口可用至右边
    unsigned first = 0;  // 默认从最底层窗口开始绘制，后面找可遮挡底层的最大化窗口
    for (unsigned n = 0; n < s->order_count; ++n) {  // 按有效窗口层次遍历，数组后部不作窗口身份
```

<!-- replace: frontend/core/paint.c -->
<!-- before -->
```c
        u_line(r.x + r.w - 10, r.y + r.h - 4, r.x + r.w - 4, r.y + r.h - 10, U_EDGE);  // 用分隔线色连接像素端点(r.x + r.w - 10,r.y + r.h - 4)到(r.x + r.w - 4,r.y + r.h - 10)
    }
    u_reset_clip();  // 恢复整个画布的可写范围，后续控件不继承上一次局部裁剪
    u_rect(0, s->height - UI_BOTTOM, s->width, 30, U_PANEL);  // 用工具栏底色写矩形，起点(0,s->height - UI_BOTTOM)，尺寸s->width×30像素
```
<!-- after -->
```c
        u_line(r.x + r.w - 10, r.y + r.h - 4, r.x + r.w - 4, r.y + r.h - 10, U_EDGE);  // 用分隔线色连接像素端点(r.x + r.w - 10,r.y + r.h - 4)到(r.x + r.w - 4,r.y + r.h - 10)
    }
    if (right)  // 资源栏占用宽度非零时才绘制它，本轮窗口已为其留空间
        monitor(s);  // 资源栏启用时读取同次统计，显示当前可用数据或未采集状态
    u_reset_clip();  // 恢复整个画布的可写范围，后续控件不继承上一次局部裁剪
    u_rect(0, s->height - UI_BOTTOM, s->width, 30, U_PANEL);  // 用工具栏底色写矩形，起点(0,s->height - UI_BOTTOM)，尺寸s->width×30像素
```

在 `frontend/core/paint.c` 中同步本阶段已变化的条件说明；执行语句保留。旧块用于定位，只写回对应新块。

<!-- replace: frontend/core/paint.c -->
<!-- before -->
```c
    u_text(s->width - 263, 10,  // 顶栏右侧说明当前画面来自主机参考还是内核控制台
           s->backend.mode == UI_REFERENCE ? "Local session" : "Local console",  // 顶栏用实际后端模式标明主机参考工作区或guest内核控制台
           U_SMALL, U_MUTED);  // 系统状态保持次要前景
    u_icon(s->width - 37, 8, UI_TASKS_APP, U_MUTED);      // 顶栏右侧当前只画资源图标，资源栏状态在 G18 加入
    u_rect(0, UI_TOP, UI_DOCK, s->height - 66, U_PANEL);  // 用工具栏底色写矩形，起点(0,36)，尺寸56×s->height - 66像素
    u_rect(55, 36, 1, s->height - 66, U_EDGE);     // 用分隔线色写矩形，起点(55,36)，尺寸1×s->height - 66像素
    for (unsigned i = 0; i < UI_APP_COUNT; ++i) {  // 逐个检查7个有效应用编号
```
<!-- after -->
```c
    u_text(s->width - 263, 10,  // 顶栏右侧说明当前画面来自主机参考还是内核控制台
           s->backend.mode == UI_REFERENCE ? "Local session" : "Local console",  // 顶栏用实际后端模式标明主机参考工作区或guest内核控制台
           U_SMALL, U_MUTED);  // 系统状态保持次要前景
    u_icon(s->width - 37, 8, UI_TASKS_APP, U_MUTED);      // 顶栏右侧画资源入口图标，是否展开由桌面 monitor 状态决定
    u_rect(0, UI_TOP, UI_DOCK, s->height - 66, U_PANEL);  // 用工具栏底色写矩形，起点(0,36)，尺寸56×s->height - 66像素
    u_rect(55, 36, 1, s->height - 66, U_EDGE);     // 用分隔线色写矩形，起点(55,36)，尺寸1×s->height - 66像素
    for (unsigned i = 0; i < UI_APP_COUNT; ++i) {  // 逐个检查7个有效应用编号
```

在 apps/internal.h 中更新下列旧窗口，补上当前 Tasks 绘制、Settings 绘制与点击声明。已有工具和四个应用入口继续保留。

<a id="code-frontend-apps-internal-h-1"></a>

<!-- replace: frontend/apps/internal.h -->
<!-- before -->
```c
void ui_app_muted(int x, int y, const char *s);  // 用小字体和次要颜色绘制说明文字
void ui_app_field(struct ui_rect r, const char *s, bool focused);  // 画输入字段和当前焦点边框
bool ui_app_match(const char *s, const char *q);  // 按字节判断s是否包含q；Terminal识别提示符，Files筛选使用自己的结果
void ui_app_dialog(struct ui_desktop *s, int kind, int target, const char *title, const char *body,  // 把种类、目标身份和文案保存到桌面对话框状态
                   const char *value);  // value是对话框输入字段的初始文字，函数只复制状态
```
<!-- after -->
```c
void ui_app_muted(int x, int y, const char *s);  // 用小字体和次要颜色绘制说明文字
void ui_app_field(struct ui_rect r, const char *s, bool focused);  // 画输入字段和当前焦点边框
bool ui_app_match(const char *s, const char *q);  // 按字节判断s是否包含q；Terminal识别提示符，Files和Tasks筛选各自使用结果
void ui_app_dialog(struct ui_desktop *s, int kind, int target, const char *title, const char *body,  // 把种类、目标身份和文案保存到桌面对话框状态
                   const char *value);  // value是对话框输入字段的初始文字，函数只复制状态
```

<!-- replace: frontend/apps/internal.h -->
<!-- before -->
```c
void ui_files_draw(struct ui_desktop *, struct ui_rect);     // 读取目录快照和排序索引，画路径、文件行和状态
void ui_editor_draw(struct ui_desktop *, struct ui_rect);    // 将文档字节偏移换成行列，画文字、选区和光标
void ui_system_draw(struct ui_desktop *, struct ui_rect);    // 按当前页显示同一启动信息的内存、设备或日志
#endif  // 结束当前应用接口的重复包含保护
```
<!-- after -->
```c
void ui_files_draw(struct ui_desktop *, struct ui_rect);     // 读取目录快照和排序索引，画路径、文件行和状态
void ui_editor_draw(struct ui_desktop *, struct ui_rect);    // 将文档字节偏移换成行列，画文字、选区和光标
void ui_tasks_draw(struct ui_desktop *, struct ui_rect);     // 绘制任务快照、CPU曲线和内存条
void ui_system_draw(struct ui_desktop *, struct ui_rect);    // 按当前页显示同一启动信息的内存、设备或日志
void ui_settings_draw(struct ui_desktop *, struct ui_rect);  // 按页面绘制当前会话配色、密度和接口说明
void ui_settings_click(struct ui_desktop *, struct ui_rect, int, int, bool);  // 分别命中页签、主题和密度矩形；只改变当前会话偏好
#endif  // 结束当前应用接口的重复包含保护
```

## 让按键选择和停止任务读取同一份快照

前一章按键已按 dialog、focus、active 选择接收者。现在任务回调建立 task_count 后，Tasks 的上下键才可在这个有效前缀移动 selected_task；筛选字段仍先接收文字，Editor 光标不同时移动。停止确认保存 dialog_target=任务 pid，ui_confirm 再交给 UI_STOP。下面局部更新确认和按键分派，其余文档插入、撤销、保存路径继续保留。

先在 commands.c 的删除与另存确认之间加入任务停止分支，用下方新块替换唯一旧窗口。

<a id="code-frontend-core-commands-c-1"></a>

<!-- replace: frontend/core/commands.c -->
<!-- before -->
```c
    if (kind == UI_DIALOG_DELETE)  // 删除确认提交目标id，服务仍会检查它存在且目录可删除
        ui_command(s, UI_DELETE, s->dialog_target, "", "");  // 提交已确认的目标id删除请求，名称与正文均不需要
    if (kind == UI_DIALOG_SAVE_AS)  // 另存提交id为-1的新文档请求，成功后刷新并重新绑定Editor身份
        ui_command(s, UI_SAVE, s->editor_id, s->dialog_input, s->editor);  // 提交另存名称与当前正文，未绑定时id为-1，成功后前端从新快照绑定新id
```
<!-- after -->
```c
    if (kind == UI_DIALOG_DELETE)  // 删除确认提交目标id，服务仍会检查它存在且目录可删除
        ui_command(s, UI_DELETE, s->dialog_target, "", "");  // 提交已确认的目标id删除请求，名称与正文均不需要
    if (kind == UI_DIALOG_STOP)  // 停止确认提交当前参考任务pid，服务只移除此记录而不结束真实进程
        ui_command(s, UI_STOP, s->dialog_target, "", "");  // 把已确认参考pid交给停止动作，只移除服务记录，不结束真实进程
    if (kind == UI_DIALOG_SAVE_AS)  // 另存提交id为-1的新文档请求，成功后刷新并重新绑定Editor身份
        ui_command(s, UI_SAVE, s->editor_id, s->dialog_input, s->editor);  // 提交另存名称与当前正文，未绑定时id为-1，成功后前端从新快照绑定新id
```

再依次更新 editing.c 的下列窗口：INFO框回车只关闭提示；Files或Tasks都可切筛选焦点；Tasks上下键在Editor分支之前结束事件。末两处保留本章既有Home/End扫描说明，移动规则沿用G17。

<a id="code-frontend-core-editing-c-1"></a>

<!-- replace: frontend/core/editing.c -->
<!-- before -->
```c
    if (s->dialog) {  // 有对话框时先由它消费当前键，下面的字段和应用不再接收同一事件
        if (key == '\n' || key == '\r') {  // 换行或回车表示提交当前命令、字段或文档换行动作，按当前接收者处理
            if (s->dialog == UI_DIALOG_ERROR)  // 错误提示只需关闭，不应当被回车转成文件服务动作
                s->dialog = UI_DIALOG_NONE;  // 关闭当前对话框，后续键不再被它优先接收
            else  // 前一分支未成立时执行下方替代操作
```
<!-- after -->
```c
    if (s->dialog) {  // 有对话框时先由它消费当前键，下面的字段和应用不再接收同一事件
        if (key == '\n' || key == '\r') {  // 换行或回车表示提交当前命令、字段或文档换行动作，按当前接收者处理
            if (s->dialog == UI_DIALOG_ERROR || s->dialog == UI_DIALOG_INFO)  // 错误提示只需关闭，不应当被回车转成文件服务动作
                s->dialog = UI_DIALOG_NONE;  // 关闭当前对话框，后续键不再被它优先接收
            else  // 前一分支未成立时执行下方替代操作
```

<!-- replace: frontend/core/editing.c -->
<!-- before -->
```c
        return airi_console_key(s->console, key);  // 把当前键交给同一console，并把它的处理结果原样返回给调用者
    }
    if (key == UI_FIND_KEY && s->active == UI_FILES_APP) {  // Files的查找快捷键只切到自己的筛选框，不开启Editor文内查找
        s->focus = UI_FOCUS_FILTER;  // 后续文字交给当前应用筛选字段
        s->field_all = true;  // 输入字段内容设为整体选中，下一次键入可以替换
```
<!-- after -->
```c
        return airi_console_key(s->console, key);  // 把当前键交给同一console，并把它的处理结果原样返回给调用者
    }
    if (key == UI_FIND_KEY && (s->active == UI_FILES_APP || s->active == UI_TASKS_APP)) {  // Files和Tasks的查找快捷键只切到该应用筛选框，不开启Editor文内查找
        s->focus = UI_FOCUS_FILTER;  // 后续文字交给当前应用筛选字段
        s->field_all = true;  // 输入字段内容设为整体选中，下一次键入可以替换
```

<!-- replace: frontend/core/editing.c -->
<!-- before -->
```c
            ++row;  // 当前计数位置前进一项，文档分支按行、Files分支按可见索引计数
        s->selection_file = order[row];  // 把选定可见行转换回原快照下标，后面取files仍能找到同一项目
        return true;  // 本逻辑事件已处理，调用者可重画当前状态
    }
```
<!-- after -->
```c
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
```

<!-- replace: frontend/core/editing.c -->
<!-- before -->
```c
            ++s->cursor;  // 光标向后移动一个ASCII字节间隙
    } else if (key == UI_HOME) {  // 向前扫描到本行第一个字符之前，前一字节的换行作为停止边界
        while (s->cursor && s->editor[s->cursor - 1] != '\n')  // 从光标向前找本行首间隙；到文首或前一字节为换行时停止
            --s->cursor;  // 光标向前移动一个ASCII字节间隙
    } else if (key == UI_END) {  // 向后扫描到本行换行之前或文档结尾，不跨到下一行
        while (s->cursor < len && s->editor[s->cursor] != '\n')  // 从光标向后找本行尾间隙；到文尾或当前字节为换行时停止
            ++s->cursor;  // 光标向后移动一个ASCII字节间隙
    } else if (key == AIRI_KEY_UP || key == AIRI_KEY_DOWN) {  // 上下移动先求当前行首和原列，再把该列限制在目标行长度内
```
<!-- after -->
```c
            ++s->cursor;  // 光标向后移动一个ASCII字节间隙
    } else if (key == UI_HOME) {  // 向前扫描到本行第一个字符之前，前一字节的换行作为停止边界
        while (s->cursor && s->editor[s->cursor - 1] != '\n')  // 扫描光标前的ASCII字节，换行决定行列转换
            --s->cursor;  // 光标向前移动一个ASCII字节间隙
    } else if (key == UI_END) {  // 向后扫描到本行换行之前或文档结尾，不跨到下一行
        while (s->cursor < len && s->editor[s->cursor] != '\n')  // 从当前光标向后扫描，遇本行换行或文档末尾就停止
            ++s->cursor;  // 光标向后移动一个ASCII字节间隙
    } else if (key == AIRI_KEY_UP || key == AIRI_KEY_DOWN) {  // 上下移动先求当前行首和原列，再把该列限制在目标行长度内
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

```

chart 已把同一 stats.samples 的六十点连成五十九段。ui_tasks_draw 先检查服务状态与回调，随后把左半内容交给 chart，右半按 memory_kib/total_kib 画内存条；总量为零不作除法。下面接起整个视图，先写工具栏和两块统计。

<!-- append: frontend/apps/tasks.c -->
```c
void ui_tasks_draw(struct ui_desktop *s, struct ui_rect r) {  // 绘制任务快照、CPU曲线和内存条
    ui_app_toolbar(r, "Processes");  // 在内容顶端铺44像素工具栏并显示文字"Processes"
    u_button((struct ui_rect){r.x + r.w - 184, r.y + 8, 76, 28}, "Refresh", false, false);  // 画文字"Refresh"按钮；其矩形供后续事件命中共用，当前仅输出像素
    u_button((struct ui_rect){r.x + r.w - 98, r.y + 8, 82, 28}, "End task", false, false);  // 画文字"End task"按钮；其矩形供后续事件命中共用，当前仅输出像素
    if (s->service_state != UI_NORMAL || !s->backend.tasks) {  // 参考服务未正常提供任务查询时不绘旧任务快照，明确显示不可用
        ui_app_unavailable(s, r, "Process and scheduler service");  // 按当前服务状态显示文字"Process and scheduler service"的缺失或失败信息
        ui_app_status(r, "No process snapshot", NULL);  // 在内容底部显示文字"No process snapshot"；右侧说明按测得的文字宽度对齐
        return;  // 任务服务状态不可用或缺查询回调，保留提示且不读任务条目
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
```

统计区已经占好位置，任务列表从 graphheight+139 处接续，先放筛选和列名，再遍历 tasks 的有效前缀。k 表示当前遍历次序，i 才是原快照下标：反序只改变取 i 的次序，筛选和滚动只改变哪行可见。选中仍比较 i 与 selected_task，输出各列时才读取这项的 pid、名称和资源。继续追加同一个 ui_tasks_draw。

<!-- append: frontend/apps/tasks.c -->
```c
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
```

tabs字符串数组的四项分别是Appearance、Keyboard、Display、About。内容点击先把工具栏内的点转换为0..3页签，再检查Night和Day各自的完整矩形；两按钮之间的空隙不触发切换。密度行同样只接受两个126×34矩形：从body.x+28与body.x+166开始，顶边为body.y+256；同一高度的右侧空白不属于Compact。

上面已经按同一偏好画好按钮。现在追加内容点击：Night/Day只写 theme，两个密度按钮只写 density；不调用 ui_init，也不建立新文档。绘图函数到此已闭合，下面是独立点击函数。

<!-- append: frontend/apps/settings.c -->
```c
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

在 builtin.c 中依次替换下列旧窗口，将 Tasks、Settings 加入绘图分派，并追加只处理 Settings 的内容点击入口。旧块用于定位，已有应用绘图保留。

<a id="code-frontend-apps-builtin-c-1"></a>

<!-- replace: frontend/apps/builtin.c -->
<!-- before -->
```c
#include "internal.h"  // 取得当前已实现的应用绘图与公共文字声明。
void ui_apps_draw(struct ui_desktop *s, enum ui_app app, struct ui_rect r) {  // 根据应用枚举绘制同一内容矩形，窗口框由外层负责。
    switch (app) {     // 应用枚举决定本次内容绘制器。
```
<!-- after -->
```c
#include "internal.h"  // 取得应用内部绘图、命中与对话框的共同声明。
void ui_apps_draw(struct ui_desktop *s, enum ui_app app, struct ui_rect r) {  // 根据应用枚举绘制同一内容矩形，窗口框由外层负责。
    switch (app) {     // 应用枚举决定本次内容绘制器。
```

<!-- replace: frontend/apps/builtin.c -->
<!-- before -->
```c
        ui_editor_draw(s, r);  // 用当前桌面和内容矩形绘制文档。
        break;  // 当前匹配已完成，停止这一层循环或分支。
    case UI_SYSTEM:  // 应用身份为系统信息时进入对应分支。
        ui_system_draw(s, r);  // 用当前桌面和内容矩形绘制系统信息。
        break;  // 当前匹配已完成，停止这一层循环或分支。
    default:  // 未知应用编号不绘制内容；窗口框仍由外层处理
```
<!-- after -->
```c
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
```

<!-- replace: frontend/apps/builtin.c -->
<!-- before -->
```c
}

```
<!-- after -->
```c
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

前面的两个 PPM 命令分别启动进程，只能查看各自初始化的一帧。下面另写同一进程中的连续操作：将 main.lua 替换成十字节，saved 仍是原文，再全选新文并打开 Settings。程序在点击前保存 editor、saved、cursor/anchor、dirty/selection 和全部 windows 字节，然后直接送入 Day、Night 的内容坐标；它检查状态保留，不在这段观察中生成图片。ui_apps_click 只清旧字段 focus，再分派设置动作。

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
```

此时工作文字是未保存的新文，选区覆盖它，保存点仍是原 main.lua；Settings 已打开，body 来自它的外框。继续 main，用 body.x+176、body.y+166 命中 Day 内部，再用 body.x+38、同一 y 命中 Night。两次只改变 theme。密度观察再点间隙和右侧空白，要求仍为0，然后点真正的 Compact 与 Comfortable，要求分别为1和0。

<!-- append: labs/tutorial/theme.c -->
```c
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
