# G21 · 操作应用按钮并移动、缩放窗口

Files 与 Editor 的正文点击已经连通。现在在 Editor 标题中央按下，将指针移动 (30,25) 后释放，同一窗口的左上角也应移动 (30,25)；在右下角按下再移动，则改变宽高。桌面保存拖动种类和按下时的偏移，后续移动沿用它，释放再清除。Terminal、System、Tasks 和 Viewer 的内容按钮也在这次接入各自的 click 函数。

先依次更新 ui.h 的唯一旧窗口：拖动枚举加入移动和缩放，desktop 保存按下偏移及启动菜单状态，并声明有符号行滚动入口。原有正文选区和指针坐标继续使用，不重建桌面。

<a id="code-frontend-include-ui-h-1"></a>

<!-- replace: frontend/include/ui.h -->
<!-- before -->
```c
enum ui_dialog { UI_DIALOG_NONE, UI_DIALOG_ERROR, UI_DIALOG_CLOSE, UI_DIALOG_NEW_FILE, UI_DIALOG_NEW_DIR, UI_DIALOG_RENAME, UI_DIALOG_DELETE, UI_DIALOG_STOP, UI_DIALOG_SAVE_AS, UI_DIALOG_INFO };  // 对话框类型决定输入、确认与取消后续动作；NONE为零。
enum ui_focus { UI_FOCUS_NONE, UI_FOCUS_FILTER, UI_FOCUS_FIND };  // 键入目标为应用正文、筛选字段或查找字段。
enum ui_drag { UI_DRAG_NONE = -1, UI_DRAG_SELECT = 3 };  // 按下到释放之间仅续接Editor文本拖选
enum ui_key {  // 功能键编号与ASCII字节分开，避免把命令误插入正文。
    UI_LEFT = 512,  // 功能键从512起，不与0..127的文本字节混淆。
```
<!-- after -->
```c
enum ui_dialog { UI_DIALOG_NONE, UI_DIALOG_ERROR, UI_DIALOG_CLOSE, UI_DIALOG_NEW_FILE, UI_DIALOG_NEW_DIR, UI_DIALOG_RENAME, UI_DIALOG_DELETE, UI_DIALOG_STOP, UI_DIALOG_SAVE_AS, UI_DIALOG_INFO };  // 对话框类型决定输入、确认与取消后续动作；NONE为零。
enum ui_focus { UI_FOCUS_NONE, UI_FOCUS_FILTER, UI_FOCUS_FIND };  // 键入目标为应用正文、筛选字段或查找字段。
enum ui_drag { UI_DRAG_NONE = -1, UI_DRAG_RESIZE = 2, UI_DRAG_MOVE = 1, UI_DRAG_SELECT = 3 };  // -1明确无拖动，其余值区分跨事件延续的操作。
enum ui_key {  // 功能键编号与ASCII字节分开，避免把命令误插入正文。
    UI_LEFT = 512,  // 功能键从512起，不与0..127的文本字节混淆。
```

<!-- replace: frontend/include/ui.h -->
<!-- before -->
```c
    int mouse_y;  // 当前鼠标屏幕纵坐标。
    int drag;     // 当前跨事件拖动种类，-1表示无。
    bool monitor;  // 右侧状态监视区是否展开。
    bool find;     // Editor查找栏是否展开。
    bool dirty;    // 工作文本是否与最近成功保存内容不同。
    bool selection;      // anchor与cursor之间是否构成有效选区。
    bool field_all;      // 对话框字段是否处于全选替换状态。
```
<!-- after -->
```c
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
```

<!-- replace: frontend/include/ui.h -->
<!-- before -->
```c
void ui_pointer(struct ui_desktop *, int x, int y,  // 鼠标接口接收同一桌面屏幕坐标。
                int phase);  /* 0 move, 1 down, 2 up, 3 double-click */  // phase把平台事件统一成移动/按下/释放/双击。
bool ui_key(struct ui_desktop *, int key, bool shift);      // 分派字符或功能键，Shift控制扩展选区等规则。
void ui_service_state(struct ui_desktop *, enum ui_state);  // 切换服务显示状态并刷新对应快照。
void ui_refresh(struct ui_desktop *);  // 从当前服务取得文件、任务与汇总快照。
```
<!-- after -->
```c
void ui_pointer(struct ui_desktop *, int x, int y,  // 鼠标接口接收同一桌面屏幕坐标。
                int phase);  /* 0 move, 1 down, 2 up, 3 double-click */  // phase把平台事件统一成移动/按下/释放/双击。
bool ui_key(struct ui_desktop *, int key, bool shift);  // 分派字符或功能键，Shift控制扩展选区等规则。
void ui_scroll(struct ui_desktop *, int rows);  // 有符号行增量交给活动视图，负方向需限到零。
void ui_service_state(struct ui_desktop *, enum ui_state);  // 切换服务显示状态并刷新对应快照。
void ui_refresh(struct ui_desktop *);  // 从当前服务取得文件、任务与汇总快照。
```

桌面接收整个屏幕的坐标，负责对话框、应用入口和窗口框；坐标落在 body 内时才交给对应应用。窗口操作和应用动作因此使用同一份几何，各自修改自己负责的状态。

<a id="window-actions"></a>

移窗只改变当前窗口的rect。绘图重新使用这个rect放置内容，命中也必须读取它；如果点击仍使用移动前的坐标，看到的文件行与实际打开的行就会错位。窗口层继续逆绘制顺序找最上方命中，只交出一次事件。

## 接入其他应用按钮

Terminal 的点击检查清屏按钮范围，命中后调用已有的 CLEAR 命令。应用分派统一清除字段焦点；后续文字由 console_key 处理。

其余应用现在首次接收窗口分派的坐标。先补齐Terminal、System、Tasks与Viewer的click声明，Settings仍沿用G18已经接入的入口。

在 apps/internal.h 中更新下面的声明窗口，加入 Terminal、Tasks、System、Viewer 点击入口。Settings、Files、Editor 已有入口继续保留。每处旧块用于定位，只写回新块。

<a id="code-frontend-apps-internal-h-1"></a>

<!-- replace: frontend/apps/internal.h -->
<!-- before -->
```c
void ui_app_status(struct ui_rect r, const char *left, const char *right);  // 在应用内容底部绘制27像素状态栏
void ui_terminal_draw(struct ui_desktop *, struct ui_rect);  // 把console逻辑行拆成窗口显示行并绘制输入
void ui_files_draw(struct ui_desktop *, struct ui_rect);     // 读取目录快照和排序索引，画路径、文件行和状态
void ui_files_click(struct ui_desktop *, struct ui_rect, int, int, bool);  // 按整屏x/y取得筛选、工具栏或可见文件行；twice为双击标记
void ui_editor_draw(struct ui_desktop *, struct ui_rect);  // 将文档字节偏移换成行列，画文字、选区和光标
unsigned ui_editor_position(const struct ui_desktop *, struct ui_rect, int, int);  // 只将内容矩形中的屏幕点夹到正文并返回字节间隙，供点击和续拖共用。
void ui_editor_click(struct ui_desktop *, struct ui_rect, int, int, bool);  // 按当前正文几何将整屏x/y换成文档间隙，或分派工具栏动作
void ui_tasks_draw(struct ui_desktop *, struct ui_rect);     // 绘制任务快照、CPU曲线和内存条
void ui_system_draw(struct ui_desktop *, struct ui_rect);    // 按当前页显示同一启动信息的内存、设备或日志
void ui_settings_draw(struct ui_desktop *, struct ui_rect);  // 按页面绘制当前会话配色、密度和接口说明
void ui_settings_click(struct ui_desktop *, struct ui_rect, int, int, bool);  // 分别命中页签、主题和密度矩形；只改变当前会话偏好
void ui_viewer_draw(struct ui_desktop *, struct ui_rect);  // 把已提交时间和测量值缩放到真实时间比例的像素曲线
void ui_open_series(struct ui_desktop *, const struct ui_file *);  // 读取CSV并在整份验证通过后提交成对时间数值数组
#endif  // 结束当前应用接口的重复包含保护
```
<!-- after -->
```c
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

Tasks 的一次点击先分工具栏和列表：End task 从 selected_task 取 pid，放进确认框，尚不修改服务；Refresh 和错误页 Retry 则重新查询。列表再沿与绘图相同的筛选、反序和滚动，将可见行还原成快照项，普通点击只更新 selected_task，双击复制这项详情到独立对话框。比如选中快照下标2的 series-worker，停止目标是 pid=14，不是下标2或显示行号。

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
```

工具栏和重试都已有返回出口，接下来才换算任务行。先按窗口高度给统计区留160或100像素，再加固定139定位筛选；跳过筛选和表头的80像素才是首行。当前 density 决定28/36行高，scroll 加回被卷走的匹配行；shown 只为匹配记录增加，命中后的 i 才用于 selected_task 和详情。继续追加同一个函数。

<!-- append: frontend/apps/tasks.c -->
```c
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

点击分派仍使用同一个 app 和 body，依次更新 builtin.c 的旧窗口加入新应用。已有绘图 switch 与 Files、Editor、Settings 点击规则保留。

<a id="code-frontend-apps-builtin-c-1"></a>

<!-- replace: frontend/apps/builtin.c -->
<!-- before -->
```c
                   bool twice) {  // 双击标志保持原值交给应用，不在桌面重复解释。
    s->focus = UI_FOCUS_NONE;  // 先取消旧字段焦点，应用命中字段后可重新设置。
    switch (app) {      // 按已命中的应用编号选择点击处理器
    case UI_FILES_APP:  // 应用身份为目录时进入对应分支。
        ui_files_click(s, r, x, y, twice);  // 把当前内容矩形和屏幕点交给 Files。
```
<!-- after -->
```c
                   bool twice) {  // 双击标志保持原值交给应用，不在桌面重复解释。
    s->focus = UI_FOCUS_NONE;  // 先取消旧字段焦点，应用命中字段后可重新设置。
    switch (app) {     // 按已命中的应用编号选择点击处理器
    case UI_TERMINAL:  // 应用身份为终端时进入对应分支。
        ui_terminal_click(s, r, x, y, twice);  // 将当前矩形、屏幕点和双击状态交给终端的命中规则。
        break;  // 当前匹配已完成，停止这一层循环或分支。
    case UI_FILES_APP:  // 应用身份为目录时进入对应分支。
        ui_files_click(s, r, x, y, twice);  // 把当前内容矩形和屏幕点交给 Files。
```

<!-- replace: frontend/apps/builtin.c -->
<!-- before -->
```c
        ui_editor_click(s, r, x, y, twice);  // 把相同事件表示交给 Editor。
        break;  // 当前匹配已完成，停止这一层循环或分支。
    case UI_SETTINGS:  // 应用身份为设置时进入对应分支。
        ui_settings_click(s, r, x, y, twice);  // 将当前矩形、屏幕点和双击状态交给设置的命中规则。
        break;  // 当前匹配已完成，停止这一层循环或分支。
    default:  // 未知或本阶段尚未接点击的应用不改变状态
```
<!-- after -->
```c
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
```

## 由顶层窗口接收坐标

<a id="drag-state"></a>

拖动需要记住按下时的关系。窗口左上为 (100,80)，鼠标在标题栏 (130,95) 按下，保存偏移 (30,15)。后续鼠标移到 (160,120)，窗口新左上应为 (130,105)，仍保持鼠标抓住原来的位置。只把鼠标坐标当窗口原点会令窗口在按下瞬间跳动。

drag 保存当前动作种类，drag_x/drag_y 保存移动时的按下点到外框原点偏移；缩放直接以当前鼠标减外框原点计算新宽高。释放后清 drag，后续普通移动才不会继续移动旧窗口。

仍使用 phase=0 移动、1 按下、2 释放、3 双击。入口先续接已经开始的拖动，选择继续走 G20 的只读定位器；移窗减去 drag_x/drag_y，缩放以鼠标到当前左上角的距离求宽高并限到720×480。释放统一清 drag，不再让这个事件穿到标题或应用按钮。

没有拖动时，先处理对话框，再处理 launcher、系统栏、侧栏和任务栏。活动对话框截获点击，Save 提交、Discard 还原 saved 后关闭、Cancel 清关闭意图。普通窗口随后才有机会接收事件。

同一坐标可能同时落在两个重叠窗口。应由画面中最上方的窗口接收，所以命中逆着绘制顺序查找，找到第一个就停止；若继续把事件交给底下窗口，一次点击会改变两份状态。

实现从 order 末尾向前检查，找到窗口后调用 ui_open 置顶，再区分标题栏按钮、拖动和内容；最大化先保存 restore，恢复时读回它。

内容坐标交给应用 click。Editor拖选仍调用G20的正文定位器，anchor固定初始间隙，cursor由夹回正文的当前坐标得到；持续移动不再进入应用按钮分派。

现在按以下唯一旧窗口更新 pointer.c。先更新函数开头，保留当前坐标与已有拖选入口；再从拖选出口接上移窗、缩放、释放和桌面分派；窗口查找处接标题与右下角；最后接入 ui_scroll。各组旧块用于定位，未出现的 G20 正文选择逻辑保留。

<a id="code-frontend-core-pointer-c-1"></a>

<!-- replace: frontend/core/pointer.c -->
<!-- before -->
```c
#include "../apps/internal.h"  // 借用Editor正文定位器，续拖不调用它的完整按钮分派。
void ui_pointer(struct ui_desktop *s, int x, int y, int phase) {  // phase=0移动、1按下、2释放、3双击；x/y 在桌面屏幕坐标中。
    s->mouse_x = x;    // 保存当前鼠标横坐标，绘图可以显示指针。
    s->mouse_y = y;    // 保存当前鼠标纵坐标。
    if (phase == 2) {  // 释放事件结束当前拖动，无需再命中内容。
        s->drag = UI_DRAG_NONE;  // -1 表示没有拖动，下一次移动不会延续选区。
        return;  // 本次事件处理已结束，不再落入其他按钮或底层窗口。
    }
    if (phase == 0 && s->drag == UI_DRAG_SELECT) {  // 选择已在按下时开始，移动只续接文字间隙。
        struct ui_rect r = s->windows[UI_EDITOR].rect;  // 使用Editor此刻外框，不保存过期屏幕位置。
```
<!-- after -->
```c
#include "../apps/internal.h"  // 借用Editor正文定位器，续拖不调用它的完整按钮分派。
void ui_pointer(struct ui_desktop *s, int x, int y, int phase) {  // phase=0移动、1按下、2释放、3双击；x/y 在桌面屏幕坐标中。
    s->mouse_x = x;  // 保存当前鼠标横坐标，绘图可以显示指针。
    s->mouse_y = y;  // 保存当前鼠标纵坐标。
    if (phase == 0 && s->drag == UI_DRAG_SELECT) {  // 选择已在按下时开始，移动只续接文字间隙。
        struct ui_rect r = s->windows[UI_EDITOR].rect;  // 使用Editor此刻外框，不保存过期屏幕位置。
```

<!-- replace: frontend/core/pointer.c -->
<!-- before -->
```c
        return;  // 此移动已处理，窗口和应用按钮不再次消费它。
    }
    if (phase != 1 && phase != 3)  // 普通移动且未拖动时不触发应用动作。
        return;  // 本次事件处理已结束，不再落入其他按钮或底层窗口。
    for (int n = (int)s->order_count - 1; n >= 0; --n) {  // 从绘制顺序末端逆查，最上方窗口先获得事件。
        int app = (int)s->order[n];  // 顺序数组保存应用身份，不是窗口地址。
```
<!-- after -->
```c
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
```

<!-- replace: frontend/core/pointer.c -->
<!-- before -->
```c
        if (!w->open || w->minimized || !u_inside(w->rect, x, y))  // 关闭、最小化或未命中的窗口都不能接收此次点击。
            continue;  // 跳过当前不适用项，继续检查下一项。
        ui_open(s, (enum ui_app)app);  // 命中窗口置顶并成为活动应用；类型转换把整数恢复为枚举。
        struct ui_rect r = w->rect;    // 复制当前命中矩形，接下来拆分标题栏与内容区。
        if (y < r.y + UI_TITLE_H + 1)  // 标题栏输入留给下一检查点，当前不送入应用正文。
            return;  // 本次事件处理已结束，不再落入其他按钮或底层窗口。
        ui_apps_click(s, (enum ui_app)app, u_window_body(r), x, y, phase == 3);  // 内容矩形去掉边框和标题栏；双击由 phase==3 转成布尔值。
        struct ui_rect body = u_window_body(r);  // 从当前外框先扣标题和边框，与Editor绘图使用同一宽度。
        int tree = body.w >= 800 ? 190 : 0;      // body798没有项目栏，不能因为外框800就扣掉它。
```
<!-- after -->
```c
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
```

<!-- replace: frontend/core/pointer.c -->
<!-- before -->
```c
    }
}
```
<!-- after -->
```c
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

滚轮按行更新对应视图。先用有符号临时值相加，再把负数限到零，避免 unsigned 向下溢出。Terminal 使用 console.scroll，Editor 使用 editor_scroll，其余使用窗口自身 scroll。

## 沿同一鼠标序列观察窗口几何

观察程序仍先双击main.lua并拖出Editor选区，再在标题中央按下，移动(30,25)后释放。新rect左上角必须增加相同偏移。随后在右下角按下，将鼠标移动到原右下角外侧(24,19)，宽高按鼠标到左上角的距离增加，释放后drag必须恢复为NONE。下一次绘图会按屏幕可用区收窄窗口；这组观察先检查事件直接留下的几何。

这组观察随后把同一Editor设为800×400普通外框，body宽798所以没有项目栏。将正文替换为print(42)加换行后，在第一行第1间隙按下，再移到Save按钮内部并释放。定位器把上方位置夹到正文第0间隙，anchor仍是1；dirty 保持 true，editor 和 saved 各自与操作前的独立副本逐字节相同，服务 read 也读回旧 saved。这样既确认第一批文字列可以起拖，也确认续拖没有调用保存按钮。

观察程序沿用 G20 的文件双击、正文选择和 observe_text_drag。定位 main 中打印 editor_id 的旧窗口，用新块加入标题移动及右下缩放序列；已有 helper 与初始化保留。这里不调用 ui_draw，所以比较的是事件直接留下的 rect，屏幕收窄发生在以后的绘图。

<a id="code-labs-tutorial-pointer-c-1"></a>

<!-- replace: labs/tutorial/pointer.c -->
<!-- before -->
```c
    printf("editor_id=%d anchor=%u cursor=%u selected=%d drag=%d\n", desktop.editor_id,  // 打印实际身份、选区间隙与释放后的拖动状态。
           anchor, desktop.cursor, desktop.selection, desktop.drag);  // 布尔值经可变参数提升为 int，%d 打印0或1。
    if (desktop.editor_id != 1 || !desktop.selection || desktop.cursor == anchor)  // 先确认此前文件与窗口观察，再改变外框测试临界宽度。
        return 1;  // 原来的选择尚未建立，不能用后一组操作替代其结果。
```
<!-- after -->
```c
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
```

## 绘制启动菜单，并让 Escape 结束当前交互

顶栏点击会切换 launcher，绘图据此在普通窗口上方显示应用菜单。命中菜单项后打开对应应用并结束本次分派；点击菜单外会先收起菜单，再按所在的桌面区域继续处理。初始化令 launcher 关闭。Escape 收起菜单并结束当前反馈，已有窗口继续保留；更新状态、绘图和按键三个文件后，新的窗口动作才有完整的进入与退出路径。

在 state.c 的 ui_open 中，定位取消最小化和设置 active 的窗口，加入 launcher=false。选择应用后便收起启动菜单，其他窗口状态不重置。

<a id="code-frontend-core-state-c-1"></a>

<!-- replace: frontend/core/state.c -->
<!-- before -->
```c
    w->minimized = false;  // 重新打开或激活窗口时取消最小化
    s->active = (int)app;  // 令本次打开的应用成为按键与窗口操作接收者
    unsigned n = 0;  // 去重后的窗口次序从空前缀开始，n计保留的应用编号条数
    for (unsigned i = 0; i < s->order_count; ++i)  // 按有效窗口层次遍历，数组后部不作窗口身份
```
<!-- after -->
```c
    w->minimized = false;  // 重新打开或激活窗口时取消最小化
    s->active = (int)app;  // 令本次打开的应用成为按键与窗口操作接收者
    s->launcher = false;   // 打开应用后关闭启动菜单
    unsigned n = 0;  // 去重后的窗口次序从空前缀开始，n计保留的应用编号条数
    for (unsigned i = 0; i < s->order_count; ++i)  // 按有效窗口层次遍历，数组后部不作窗口身份
```

在 paint.c 中定位 toast 之后、dialog 之前的旧窗口，插入启动菜单绘制。菜单覆盖普通窗口，对话框仍最后覆盖菜单；原有整帧其余部分保留。

<a id="code-frontend-core-paint-c-1"></a>

<!-- replace: frontend/core/paint.c -->
<!-- before -->
```c
        u_text(s->width - 368, s->height - 70, s->toast, U_SMALL, U_TEXT);  // 在整屏x=s->width - 368、整屏y=s->height - 70显示本次操作短提示
    }
    if (s->dialog) {  // 菜单和普通窗口已经绘完，再写对话框像素，使它覆盖下层
        struct ui_rect d = {s->width / 2 - 240, s->height / 2 - 112, 480, 224};  // 480×224对话框以整屏中心为中心，左上扣除半宽半高
```
<!-- after -->
```c
        u_text(s->width - 368, s->height - 70, s->toast, U_SMALL, U_TEXT);  // 在整屏x=s->width - 368、整屏y=s->height - 70显示本次操作短提示
    }
    if (s->launcher) {  // 普通窗口与提示后画应用菜单，对话框随后覆盖
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
```

在 editing.c 的 Escape 分支定位旧窗口，加入清 launcher；原有 dialog、pending_close、find 和 focus 的清理继续保留。

<a id="code-frontend-core-editing-c-1"></a>

<!-- replace: frontend/core/editing.c -->
<!-- before -->
```c
        s->dialog = UI_DIALOG_NONE;  // 关闭当前对话框，后续键不再被它优先接收
        s->pending_close = false;    // 结束关闭请求，后续普通保存不应沿用旧关闭动作
        s->find = false;  // 隐藏Editor查找栏，后续正文可使用这部分高度
        s->focus = UI_FOCUS_NONE;  // 当前字段结束接收，后续按键回到活动应用
        return true;  // 本逻辑事件已处理，调用者可重画当前状态
```
<!-- after -->
```c
        s->dialog = UI_DIALOG_NONE;  // 关闭当前对话框，后续键不再被它优先接收
        s->pending_close = false;    // 结束关闭请求，后续普通保存不应沿用旧关闭动作
        s->launcher = false;  // Escape关闭应用启动菜单，保持已有窗口打开状态
        s->find = false;      // 隐藏Editor查找栏，后续正文可使用这部分高度
        s->focus = UI_FOCUS_NONE;  // 当前字段结束接收，后续按键回到活动应用
        return true;  // 本逻辑事件已处理，调用者可重画当前状态
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
