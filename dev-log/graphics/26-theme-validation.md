# G26 · 保留同一份文档，再比较 Day 与 Night

Editor中尚未保存的输入、选择和窗口位置已经可以通过真实鼠标改变。现在点击Settings里的Day再切回Night：颜色可以改变，文档字节、保存点、选区两端和Editor矩形必须保留。视觉比较还要固定场景，否则一张图打开Terminal、另一张图只显示Editor，差异就无法归到主题。

<a id="paired-scenes"></a>

原生程序已能指定应用、尺寸和主题并输出图片。本次参数建立可重复的观察条件：每个进程重新初始化同一份参考服务，然后选择页签、服务状态和工作区。save-error使用不存在的文件ID请求服务，服务返回错误后保留文本；controls直接传控件状态位，避免画面随系统鼠标位置漂移。

## 从实际绘图程序取得颜色

<a id="actual-palette"></a>

颜色角色在C渲染器中才转换为RGB。Python若另外抄一份常量，检查通过的可能是另一套配色。`--palette --theme day`让当前编译程序调用u_color，输出角色名到24位RGB整数的JSON对象。这个分支只需要渲染器，不创建窗口或文件服务。

`dump_palette` 输出本次编译的角色值，`draw_controls` 生成固定状态的控件表；`main` 再把页签、服务状态和场景参数交给对应状态入口。平台数组、CoreGraphics借用和事件回调继续按同一寿命运行；主题变化仍写desktop.theme，不重建文档。PPM头、像素写入及fclose刷新都必须成功才返回零。文字粘贴先检查平台对象与完整字节容量，没有字符串或超过8191字节时只留下短提示，不截断输入或改变文档；Files正文仍沿内部文件id粘贴。

`--state normal|empty|loading|error|offline` 把所选服务状态交给 `ui_service_state`；`--scenario dialog|dirty|workspace|save-error|system-tasks|controls` 在进入事件循环前布置窗口或发出失败动作，省略它时采用 none。`--tab` 接收从零开始的当前应用页签下标，应用名、尺寸、主题与输出路径沿用 G23 的参数。CLI演示参数采用固定合法值；未知应用名不覆盖已有app选择，默认值为Editor；未知初始主题仍采用Night，与控制台明确报错的theme invalid不同。

保留 G23 的画布、CoreGraphics 包装、输入回调和事件循环。依次更新 preview.m 的下列唯一旧窗口：加入控件表开关与两个输出 helper，让 dump 可覆盖成控件场景，最后扩充 main 的参数和场景布置。旧块只用于定位，只写回新块；窗口输入不重新实现。

<a id="code-frontend-platform-macos-preview-m-1"></a>

<!-- replace: frontend/platform/macos/preview.m -->
<!-- before -->
```objective-c
static AiriBootInfo boot;  // 平台保存启动描述副本，backend.boot 只借用这个稳定地址。
static struct airi_framebuffer framebuffer;  // 保存软件画布的地址、尺寸、行距和颜色位布局。
static uint32_t *pixels;  // 指向平台拥有的32位像素数组，静态初值为 NULL。
static void size_buffer(int w, int h) {  // w/h 为画布宽高；尺寸变化时重建数组并更新描述。
    if (w < 640)  // 软件画布最小宽度640像素，防止布局缺少空间。
```
<!-- after -->
```objective-c
static AiriBootInfo boot;  // 平台保存启动描述副本，backend.boot 只借用这个稳定地址。
static struct airi_framebuffer framebuffer;  // 保存软件画布的地址、尺寸、行距和颜色位布局。
static bool control_sheet;  // 快照专用控件表使用共享绘图，不伪装成已接入的应用。
static uint32_t *pixels;    // 指向平台拥有的32位像素数组，静态初值为 NULL。
static void size_buffer(int w, int h) {  // w/h 为画布宽高；尺寸变化时重建数组并更新描述。
    if (w < 640)  // 软件画布最小宽度640像素，防止布局缺少空间。
```

<!-- replace: frontend/platform/macos/preview.m -->
<!-- before -->
```objective-c
    boot.framebuffer = framebuffer;  // 启动描述副本取得同一画布参数，窗口中的设备信息可以读取。
}
static void dump(const char *name) {  // name 是输出文件路径，快照直接使用同一共享绘图。
    ui_draw(&desktop, &framebuffer);  // 共享 C 渲染将当前桌面状态写进平台像素数组。
    FILE *f = fopen(name, "wb");      // 以二进制写模式创建 PPM，失败返回 NULL。
    if (!f) {  // 路径不可写或打开失败时不继续往空文件指针写入。
        perror(name);  // 把当前 errno 原因连同输出路径写到标准错误。
```
<!-- after -->
```objective-c
    boot.framebuffer = framebuffer;  // 启动描述副本取得同一画布参数，窗口中的设备信息可以读取。
}
static void dump_palette(unsigned mode) {  // 输出实际 C 调色板，检查程序不复制一份颜色常量。
    const uint32_t roles[] = {U_BG, U_PANEL, U_SURFACE, U_RAISED, U_TEXT, U_MUTED, U_ACCENT, U_BLUE, U_RED, U_WARM, U_PINK, U_CONTROL, U_HOVER, U_PRESSED, U_SUCCESS, U_CHROME, U_INK, U_ON_ACCENT};  // 列出参与可读性检查的角色。
    const char *names[] = {"BG", "PANEL", "SURFACE", "RAISED", "TEXT", "MUTED", "ACCENT", "BLUE", "RED", "WARM", "PINK", "CONTROL", "HOVER", "PRESSED", "SUCCESS", "CHROME", "INK", "ON_ACCENT"};  // JSON 名称与角色一一对应。
    u_theme(mode);  // 后续 u_color 解析本次指定主题。
    printf("{");    // JSON 对象从左花括号开始。
    for (unsigned i = 0; i < sizeof(roles) / sizeof(roles[0]); ++i)  // 遍历当前角色数组的全部有效项。
        printf("%s\"%s\":%u", i ? "," : "", names[i], u_color(roles[i]));  // 键是角色名，值是实际 RGB 整数；首项不带逗号。
    puts("}");  // 关闭对象并补换行，供标准 JSON 解码器读取。
}
static void draw_controls(void) {  // 同内容状态表用于比较两个主题的可读性。
    u_rect(0, 0, desktop.width, desktop.height, U_BG);  // 使用当前主题的桌面表面。
    u_text(64, 48, "AiriOS / control vocabulary", U_TITLE, U_TEXT);  // 标题说明这是设计校准工具。
    u_text(64, 88, "Shared renderer / identical states in Day and Night", U_BODY, U_MUTED);  // 同一绘图函数承担全部样本。
    const char *labels[] = {"Default", "Hover", "Pressed", "Selected", "Keyboard focus", "Disabled"};  // 行标题给出状态语义。
    unsigned states[] = {U_NORMAL, U_HOT, U_DOWN, U_SELECTED, U_FOCUSED, U_INACTIVE};  // 显式状态避免截图依赖系统鼠标位置。
    for (unsigned i = 0; i < 6; ++i) {  // 所有样本使用一致的尺寸与间距。
        int y = 152 + (int)i * 72;  // 每行留下文字与轮廓的观察空间。
        u_text(64, y + 10, labels[i], U_MEDIUM, U_TEXT);  // 名称独立于颜色，便于识别状态。
        u_control((struct ui_rect){248, y, 152, 36}, "Save document", states[i]);  // 普通按钮使用真实公共控件。
        u_control((struct ui_rect){424, y, 152, 36}, "Current item", states[i] | U_SELECTED);  // 选中与各状态组合。
    }
    u_text(656, 160, "Information hierarchy", U_LABEL, U_TEXT);  // 右列观察正文与辅助信息。
    u_text(656, 208, "/home/airi/projects/main.lua", U_MONO, U_TEXT);  // 保留真实路径大小写和常规文字尺度。
    u_text(656, 248, "0xffff800000200000", U_MONO, U_MUTED);  // 十六进制地址测试细节可读性。
    u_text(656, 312, "Saved / document is up to date", U_BODY, U_SUCCESS);  // 成功同时有文字说明。
    u_text(656, 356, "Warning / unsaved changes", U_BODY, U_WARM);  // 警告没有借用品牌色表达。
    u_text(656, 400, "Error / save failed, input retained", U_BODY, U_RED);  // 错误解释失败后保留的内容。
    u_text(64, desktop.height - 72, "2 px control radius / 2 px window radius / Plex Sans + Mono", U_BODY, U_MUTED);  // 把可调整参数展示为校准信息。
}
static void dump(const char *name) {  // name 是输出文件路径，快照直接使用同一共享绘图。
    ui_draw(&desktop, &framebuffer);  // 共享 C 渲染将当前桌面状态写进平台像素数组。
    if (control_sheet)  // 只有控件场景覆盖应用画面。
        draw_controls();  // 状态表仍使用同一画布和本次主题。
    FILE *f = fopen(name, "wb");  // 以二进制写模式创建 PPM，失败返回 NULL。
    if (!f) {  // 路径不可写或打开失败时不继续往空文件指针写入。
        perror(name);  // 把当前 errno 原因连同输出路径写到标准错误。
```

<!-- replace: frontend/platform/macos/preview.m -->
<!-- before -->
```objective-c
int main(int argc, char **argv) {  // 宿主运行时传入参数数量和 NUL 字符串指针数组。
    @autoreleasepool {  // 离开作用域时处理登记在池内的临时对象，C/CG 资源仍显式释放。
        int w = 1280;  // 软件画布初始宽度为1280逻辑点。
        int h = 800;   // 软件画布初始高度为800逻辑点。
        int app = UI_EDITOR;  // 默认活动应用是Editor，名称表下标将覆盖它。
        const char *snapshot = NULL;  // 未给文件路径时运行真实事件循环。
        const char *theme = "night";  // 初始会话偏好为夜间。
        const char *apps[] = {"terminal", "files",    "editor", "tasks",  // 应用名顺序必须与 enum ui_app 一致，前四项从终端到任务。
                              "system",   "settings", "viewer"};  // 后三项补齐系统、设置和数据查看器。
        for (int i = 1; i < argc; ++i) {  // 跳过 argv[0] 程序名，按顺序读取选项。
            if (!strcmp(argv[i], "--snapshot") && i + 1 < argc)  // strcmp 返回零表示相等，同时确认后面还有路径参数。
                snapshot = argv[++i];  // 先推进到选项值，再保存由运行时持有的路径指针。
            else if (!strcmp(argv[i], "--size") && i + 1 < argc)  // 尺寸选项同样要求有下一个参数。
```
<!-- after -->
```objective-c
int main(int argc, char **argv) {  // 宿主运行时传入参数数量和 NUL 字符串指针数组。
    @autoreleasepool {  // 离开作用域时处理登记在池内的临时对象，C/CG 资源仍显式释放。
        bool palette = false;  // 显式调色板模式只输出机器可读颜色。
        int w = 1280;  // 软件画布初始宽度为1280逻辑点。
        int h = 800;   // 软件画布初始高度为800逻辑点。
        int app = UI_EDITOR;  // 默认活动应用是Editor，名称表下标将覆盖它。
        int tab = 0;  // 默认使用所选应用的首个页签。
        const char *snapshot = NULL;    // 未给文件路径时运行真实事件循环。
        const char *state = "normal";   // 服务状态默认为正常。
        const char *scenario = "none";  // 没有额外固定场景时直接显示所选应用。
        const char *theme = "night";    // 初始会话偏好为夜间。
        const char *apps[] = {"terminal", "files",    "editor", "tasks",  // 应用名顺序必须与 enum ui_app 一致，前四项从终端到任务。
                              "system",   "settings", "viewer"};  // 后三项补齐系统、设置和数据查看器。
        for (int i = 1; i < argc; ++i) {  // 跳过 argv[0] 程序名，按顺序读取选项。
            if (!strcmp(argv[i], "--palette")) {  // 此开关只要求颜色JSON，不消费后续参数。
                palette = true;  // 记录提前退出模式，--theme仍继续解析。
            }
            else if (!strcmp(argv[i], "--snapshot") && i + 1 < argc)  // strcmp 返回零表示相等，同时确认后面还有路径参数。
                snapshot = argv[++i];  // 先推进到选项值，再保存由运行时持有的路径指针。
            else if (!strcmp(argv[i], "--size") && i + 1 < argc)  // 尺寸选项同样要求有下一个参数。
```

<!-- replace: frontend/platform/macos/preview.m -->
<!-- before -->
```objective-c
                    if (!strcmp(a, apps[j]))  // 输入字符串与名称相等才匹配。
                        app = j;  // 名称表下标同时就是后续 UI 应用身份。
            }
            else if (!strcmp(argv[i], "--theme") && i + 1 < argc)  // 主题用于固定快照，也可作为原生窗口初始偏好。
                theme = argv[++i];  // 两种模式共享所有内容和布局。
        }
        struct ui_backend b;     // 局部回调表先由参考服务初始化。
```
<!-- after -->
```objective-c
                    if (!strcmp(a, apps[j]))  // 输入字符串与名称相等才匹配。
                        app = j;  // 名称表下标同时就是后续 UI 应用身份。
            } else if (!strcmp(argv[i], "--tab") && i + 1 < argc)  // 页签选项接受整数下标。
                tab = atoi(argv[++i]);  // 把参数十进制文本转成 int；当前演示调用只传有效数字。
            else if (!strcmp(argv[i], "--state") && i + 1 < argc)  // 状态选项选择普通或失败视图。
                state = argv[++i];  // 保存参数字符串，初始化后再转换为状态枚举。
            else if (!strcmp(argv[i], "--theme") && i + 1 < argc)  // 主题用于固定快照，也可作为原生窗口初始偏好。
                theme = argv[++i];  // 两种模式共享所有内容和布局。
            else if (!strcmp(argv[i], "--scenario") && i + 1 < argc)  // 场景选项用于固定快照中的工作区或对话框。
                scenario = argv[++i];  // 借用场景名称，后续按名字修改桌面。
        }
        if (palette) {  // 颜色检查不建立窗口或文件参考服务。
            dump_palette(!strcmp(theme, "day") ? UI_DAY : UI_NIGHT);  // 输出指定模式的实际映射。
            return 0;  // JSON 输出完成后正常退出。
        }
        struct ui_backend b;     // 局部回调表先由参考服务初始化。
```

<!-- replace: frontend/platform/macos/preview.m -->
<!-- before -->
```objective-c
        ui_init(&desktop, b, &console);      // 复制回调表，连接 console 并建立桌面初始窗口。
        desktop.theme = !strcmp(theme, "day") ? UI_DAY : UI_NIGHT;  // 桌面持有呈现偏好，文本会话不持有颜色状态。
        if (app == UI_VIEWER) {  // Viewer 启动前需先装入一份实际 CSV 数据。
            for (unsigned i = 0; i < desktop.file_count; ++i)  // 遍历有效文件快照。
```
<!-- after -->
```objective-c
        ui_init(&desktop, b, &console);      // 复制回调表，连接 console 并建立桌面初始窗口。
        desktop.theme = !strcmp(theme, "day") ? UI_DAY : UI_NIGHT;  // 桌面持有呈现偏好，文本会话不持有颜色状态。
        control_sheet = !strcmp(scenario, "controls");  // 控件表只用于宿主视觉验证。
        if (app == UI_VIEWER) {  // Viewer 启动前需先装入一份实际 CSV 数据。
            for (unsigned i = 0; i < desktop.file_count; ++i)  // 遍历有效文件快照。
```

<!-- replace: frontend/platform/macos/preview.m -->
<!-- before -->
```objective-c
                }
        }
        ui_open(&desktop, (enum ui_app)app);    // 最终打开指定应用，枚举值来自名称表匹配。
        desktop.windows[app].maximized = true;  // 默认把应用内容放到最大化工作区。
        desktop.monitor = app == UI_EDITOR;     // Editor 场景显示旁边的状态监视区域。
        if (snapshot) {  // 有输出路径时只生成图片，不进入事件循环。
            dump(snapshot);  // 先共享绘图，再将画布按 PPM 字节布局输出。
```
<!-- after -->
```objective-c
                }
        }
        ui_open(&desktop, (enum ui_app)app);  // 最终打开指定应用，枚举值来自名称表匹配。
        desktop.windows[app].tab = (unsigned)tab;  // 保存指定页签；快照调用只传有效非负下标。
        desktop.windows[app].maximized = true;     // 默认把应用内容放到最大化工作区。
        desktop.monitor = app == UI_EDITOR;  // Editor 场景显示旁边的状态监视区域。
        if (!strcmp(state, "empty"))  // empty 显示服务返回零项的状态。
            ui_service_state(&desktop, UI_EMPTY);  // 把桌面切到空结果视图。
        else if (!strcmp(state, "loading"))  // loading 显示等待服务结果的状态。
            ui_service_state(&desktop, UI_LOADING);  // 登记加载中，绘图使用对应状态视图。
        else if (!strcmp(state, "error"))  // error 显示服务失败状态。
            ui_service_state(&desktop, UI_ERROR);  // 登记失败，界面提供具体提示与重试动作。
        else if (!strcmp(state, "offline"))  // offline 表示当前服务不可连接。
            ui_service_state(&desktop, UI_OFFLINE);  // 离线状态使相关保存动作报告错误。
        if (!strcmp(scenario, "dialog"))  // dialog 场景主动触发一次未连接的执行请求。
            ui_command(&desktop, UI_EXECUTE, desktop.editor_id, desktop.editor_name,  // 使用当前真实文档身份与名称请求执行。
                       desktop.editor);  // 执行输入为当前编辑文本，而非另一个示例字符串。
        if (!strcmp(scenario, "dirty")) {  // dirty 快照显示未保存关闭确认框。
            desktop.dirty = true;  // 人为建立未保存显示状态，供固定图像观察。
            desktop.dialog = UI_DIALOG_CLOSE;  // 关闭动作等待用户处理未保存内容。
            snprintf(desktop.dialog_title, 64, "Unsaved changes");  // 给确认框写入标题，容量包含 NUL。
            snprintf(desktop.dialog_text, 256,  // 把操作提示写进256字节说明数组。
                     "Save with Ctrl-S before closing, or discard changes.");  // 提示当前支持保存或放弃的路径。
        }
        if (!strcmp(scenario, "workspace")) {  // workspace 场景组合 Editor 与 Terminal 两个窗口。
            ui_open(&desktop, UI_TERMINAL);  // 终端打开并置顶。
            desktop.windows[UI_EDITOR].maximized = false;  // Editor 恢复普通窗口，才能与终端同时展示。
            desktop.windows[UI_EDITOR].rect = (struct ui_rect){80, 58, w - 364, h - 294};  // 设置 Editor 在当前画布内的位置和大小。
            desktop.windows[UI_TERMINAL].rect = (struct ui_rect){80, h - 224, w - 364, 182};  // 终端放在下部，坐标随指定尺寸变化。
            desktop.monitor = true;  // 工作区右侧继续显示状态监视区域。
        }
        if (!strcmp(scenario, "save-error")) {  // 通过真实 provider 拒绝不存在的文件身份，构造保存失败。
            ui_open(&desktop, UI_FILES_APP);  // Files 提供失败动作所在目录的上下文。
            desktop.windows[UI_FILES_APP].maximized = true;  // 列表显示在完整工作区。
            desktop.monitor = false;  // 本组重点是列表和错误恢复。
            desktop.dirty = true;     // 故障后必须保留未保存状态与文本。
            ui_command(&desktop, UI_SAVE, 9999, desktop.editor_name, desktop.editor);  // 失败来自服务结果，不直接编造错误对话框。
        }
        if (!strcmp(scenario, "system-tasks")) {  // 内存和任务取自同一参考 backend。
            ui_open(&desktop, UI_SYSTEM);  // 底层窗口显示内存页。
            desktop.windows[UI_SYSTEM].tab = 1;  // 第二页是实际启动描述中的内存地图。
            desktop.windows[UI_SYSTEM].maximized = true;  // 使用整个工作区显示地址与区间。
            desktop.monitor = false;  // 不重复显示右侧资源监视。
            ui_open(&desktop, UI_TASKS_APP);  // 任务窗口浮在内存页的下方区域。
            desktop.windows[UI_TASKS_APP].rect = (struct ui_rect){w - 756, h - 570, 740, 522};  // 保留上方内存信息可见。
        }
        if (snapshot) {  // 有输出路径时只生成图片，不进入事件循环。
            dump(snapshot);  // 先共享绘图，再将画布按 PPM 字节布局输出。
```

控件表采用2像素控件圆角和2像素窗口圆角，绘制继续使用4×4子像素采样。Default、Hover、Pressed、Selected、Keyboard focus和Disabled与状态位数组按相同下标配对。右列文字分别用正文、辅助、成功、警告与错误角色，因此截图同时检查非按钮内容。

## 状态保持和失败路径先由C检查

<a id="state-preservation"></a>

下面把这些检查写进一个普通 C 进程。start 每次重建 reference、console、desk，使坏 CSV、离线状态或任务删除不带入下一项；typing 则逐字节调用 ui_key，和一次粘贴整串的历史单位不同。各检查函数只修改本项状态，main 在文件末尾依次调用，任何 assert 失败都会立即中止，最后才打印 PASS。先写共同对象和输入 helper。

**新建 `tests/unit/ui_test.c`：**

<a id="code-tests-unit-ui-test-c-1"></a>

<!-- write: tests/unit/ui_test.c -->
```c
#include "ui.h"      // 取得共享桌面状态和输入事件接口。
#include <assert.h>  // 取得条件不成立就中止的 assert；检查构建不能定义 NDEBUG。
#include <stdio.h>   // 取得主机 printf、文件输出与错误报告接口。
#include <stdlib.h>  // 取得主机分配、释放、退出与数值转换接口。
#include <string.h>  // 取得宿主字符串比较、复制和查找接口。
static struct ui_desktop desk;  // 测试进程持有一份桌面，每个检查用 start 重新初始化。
static struct airi_console console;  // 静态保存终端命令与日志，不在每次重画时重建。
static struct ui_backend backend;    // 测试进程保存回调表，读取与动作针对同一参考服务。
static void start(void) {  // 每个独立检查重建服务与桌面，前一检查状态不会污染下一项。
    ui_fixture_backend(&backend);  // 初始化参考文件和回调，把表写入 backend。
    airi_console_init(&console, backend.boot);  // 命令对象借用参考启动描述。
    ui_init(&desk, backend, &console);  // 桌面复制这一份回调表，借用同一console。
}
static void typing(const char *s) {  // 把NUL字符串转换为连续ASCII键入事件。
    while (*s)  // 当前字节非NUL才继续键入。
        ui_key(&desk, *s++, false);  // 先读取当前字节送键事件，再推进指针一字节。
}
```

共享起点和 typing 已定义。workflow 沿一份文档串起编辑、保存、撤销、剪贴板及服务动作：全选后的第一字符替换旧选区，后续逐键追加，所以这里第一次 Undo 恢复的是最后一个键入之前的文字，不是 G17 整次粘贴前的 main.lua。保存后还要 read 回同样正文；执行与离线保存失败分别留下反馈。后半创建、改名、删除、CSV、任务和切窗继续使用这一份服务。追加这个独立检查函数。

<!-- append: tests/unit/ui_test.c -->
```c
static void workflow(void) {  // 沿同一文档修改、保存、撤销、剪贴板与服务失败走完动作。
    start();  // 以重新初始化的参考服务和桌面开始本项检查。
    assert(desk.active == UI_EDITOR && desk.editor_id == 1);  // 初始活动Editor必须打开服务id=1的文档。
    ui_key(&desk, UI_SELECT_ALL, false);  // 建立整个文档选区，下一次键入替换旧内容。
    typing("print(42)\n");  // 逐字节键入完整Lua文本，包含末尾换行。
    assert(desk.dirty && !strcmp(desk.editor, "print(42)\n"));  // 修改后工作文本匹配输入且尚未保存。
    ui_key(&desk, UI_SAVE_KEY, false);  // 请求当前文件id保存工作文本。
    assert(!desk.dirty && desk.dialog == UI_DIALOG_NONE);  // 成功保存后dirty清除且没有错误框。
    char saved[UI_DOC_SIZE];  // 独立数组接收服务实际保存的文本。
    assert(backend.read(NULL, 1, saved, sizeof(saved)));  // 参考服务上下文可为NULL，读取id=1并检查回调成功。
    assert(!strcmp(saved, "print(42)\n"));  // 成功保存必须可从服务重新读出同一文本。
    ui_key(&desk, UI_UNDO, false);  // 使用撤销快照交换前后文本，不重新读取服务。
    assert(desk.dirty);  // 当前工作文本与saved不同，必须标为未保存。
    ui_key(&desk, UI_UNDO, false);  // 使用撤销快照交换前后文本，不重新读取服务。
    assert(!strcmp(desk.editor, "print(42)\n"));  // 操作后工作文本必须仍为这份完整字节串。
    ui_key(&desk, UI_SELECT_ALL, false);  // 建立整个文档选区，下一次键入替换旧内容。
    ui_key(&desk, UI_COPY, false);  // 把当前选区复制到独立剪贴板数组。
    assert(!strcmp(desk.clipboard, "print(42)\n"));  // 剪贴板必须持有完整选区文本。
    ui_key(&desk, UI_CUT, false);  // 复制当前选区后删除工作文本中的选中字节。
    assert(!desk.editor[0]);  // 全选剪切后首字节为NUL，文档为空。
    ui_key(&desk, UI_PASTE, false);  // 以容量和ASCII合同插入完整剪贴板。
    assert(!strcmp(desk.editor, "print(42)\n"));  // 操作后工作文本必须仍为这份完整字节串。
    ui_key(&desk, UI_RUN_KEY, false);  // 执行请求由当前服务明确返回未连接。
    assert(desk.dialog == UI_DIALOG_ERROR && strstr(desk.dialog_text, "not connected"));  // 错误框存在且原因包含服务未连接，不能假装脚本已运行。
    ui_key(&desk, UI_ESCAPE, false);      // 关闭当前消息框或取消字段，回到原编辑对象。
    ui_service_state(&desk, UI_OFFLINE);  // 切为离线状态，后续服务动作应失败。
    ui_key(&desk, UI_SAVE_KEY, false);    // 请求当前文件id保存工作文本。
    assert(desk.dialog == UI_DIALOG_ERROR);  // 失败动作必须留下错误框。
    assert(!strcmp(desk.editor, "print(42)\n"));  // 操作后工作文本必须仍为这份完整字节串。
    ui_key(&desk, UI_ESCAPE, false);     // 关闭当前消息框或取消字段，回到原编辑对象。
    ui_service_state(&desk, UI_NORMAL);  // 恢复普通服务状态，下一动作重新访问参考服务。
    ui_command(&desk, UI_CREATE_FILE, 0, "test.lua", "");  // 在显式当前目录创建空文件，id=0表示尚未分配身份。
    assert(desk.file_count == 7);  // 六条初始目录记录加一次创建应变成七条。
    int id = 0;  // 零表示尚未找到新建文件服务身份。
    for (unsigned i = 0; i < desk.file_count; ++i)  // 遍历有效目录快照，不读取容量剩余项。
        if (!strcmp(desk.files[i].name, "test.lua"))  // strcmp为零表示名称完全相等。
            id = desk.files[i].id;  // 取稳定文件身份，不把i作为服务目标。
    assert(id);  // 新建记录必须分配非零id。
    ui_command(&desk, UI_RENAME, id, "renamed.lua", "");  // 同一id重命名，位置或名称变化不更换身份。
    ui_command(&desk, UI_DELETE, id, "", "");  // 删除该id，无需额外名称或正文。
    assert(desk.file_count == 6);  // 删除新文件后恢复六条初始记录。
    ui_command(&desk, UI_CREATE_FILE, 0, "main.lua", "");  // 尝试同目录重名创建，必须返回错误。
    assert(desk.dialog == UI_DIALOG_ERROR);  // 失败动作必须留下错误框。
    ui_key(&desk, UI_ESCAPE, false);  // 关闭当前消息框或取消字段，回到原编辑对象。
    ui_open_document(&desk, 1);  // 快照下标1是初始CSV，打开后转入Viewer。
    assert(desk.active == UI_VIEWER && desk.series_count == 6 && desk.series_values[5] == 37);  // 核对窗口、六个有效点和最后一个纵轴值。
    unsigned before = desk.task_count;  // 记录停止动作之前的任务记录数。
    ui_command(&desk, UI_STOP, 14, "", "");  // 删除允许停止的参考任务pid=14。
    assert(desk.task_count == before - 1);   // 成功停止只减少一条服务记录。
    ui_command(&desk, UI_STOP, 1, "", "");   // 受保护的pid=1停止请求必须失败。
    assert(desk.dialog == UI_DIALOG_ERROR);  // 失败动作必须留下错误框。
    ui_key(&desk, UI_ESCAPE, false);  // 关闭当前消息框或取消字段，回到原编辑对象。
    for (int i = 0; i < UI_APP_COUNT; ++i)  // 打开每个有效应用观察顺序数组容量。
        ui_open(&desk, (enum ui_app)i);  // 将整数身份转枚举，打开并置顶该应用。
    assert(desk.order_count == UI_APP_COUNT);  // 重复打开已有应用不能增加重复顺序项。
    ui_open(&desk, UI_EDITOR);  // 将Editor再次置顶，不创建第二个Editor窗口。
    assert(desk.order_count == UI_APP_COUNT);  // 重复打开已有应用不能增加重复顺序项。
    ui_key(&desk, UI_NEXT_WINDOW, false);      // 活动应用沿窗口顺序切换。
    assert(desk.active != UI_EDITOR);  // 切换后不能仍停在原Editor。
}
```

workflow 结束后，下一函数先 start，回到独立原文。它先让查找字段接收粘贴，再保留 Editor 查找栏和选区切到 Files，确认筛选回车没有覆盖文档选择。回到 Editor 后才检查8191字节上限、全选短文释放容量、滚动和坏CSV；末尾用 Escape 取消待关闭意图，并观察名称框的全选替换。追加输入边界检查。

<!-- append: tests/unit/ui_test.c -->
```c
static void input_boundaries(void) {  // 沿查找、容量、滚动、坏CSV和对话框检查输入边界。
    start();  // 以重新初始化的参考服务和桌面开始本项检查。
    ui_key(&desk, UI_FIND_KEY, false);  // 查找栏获得输入焦点。
    strcpy(desk.clipboard, "local values");  // 准备查找要粘贴的ASCII文本。
    ui_key(&desk, UI_PASTE, false);  // 以容量和ASCII合同插入完整剪贴板。
    assert(!strcmp(desk.find_text, "local values"));  // 查找字段应接收粘贴，不能误改文档。
    ui_key(&desk, '\n', false);  // 回车确认当前框或查找，具体用途由当前focus/dialog决定。
    assert(desk.selection);      // 查找匹配应在文档中建立选区。
    ui_key(&desk, UI_ESCAPE, false);    // 关闭当前消息框或取消字段，回到原编辑对象。
    ui_key(&desk, UI_FIND_KEY, false);  // 重新打开Editor查找，保留会话中的find状态。
    strcpy(desk.find_text, "local");    // 准备能匹配当前main.lua的文本。
    desk.cursor = 2;  // 文档光标间隙与匹配结果不同。
    desk.anchor = 1;  // 建立需要保留的选区起点。
    desk.selection = true;  // 当前文档选择字节1至2。
    ui_open(&desk, UI_FILES_APP);  // 转到Files但不销毁Editor状态。
    ui_key(&desk, UI_FIND_KEY, false);  // 获取Files筛选焦点。
    ui_key(&desk, 'm', false);   // 只改变Files筛选文本。
    ui_key(&desk, '\n', false);  // 提交筛选不能触发仍打开的Editor查找栏。
    assert(desk.cursor == 2 && desk.anchor == 1 && desk.selection);  // 跨应用事件不得覆盖文档选择。
    ui_open(&desk, UI_EDITOR);  // 返回原文档继续后面的编辑边界检查。
    ui_key(&desk, UI_SELECT_ALL, false);  // 建立整个文档选区，下一次键入替换旧内容。
    typing("one\n");  // 全选后用短文替换原内容。
    ui_key(&desk, UI_SAVE_KEY, false);  // 请求当前文件id保存工作文本。
    ui_key(&desk, UI_UNDO, false);      // 使用撤销快照交换前后文本，不重新读取服务。
    assert(desk.dirty);  // 当前工作文本与saved不同，必须标为未保存。
    ui_key(&desk, UI_UNDO, false);  // 使用撤销快照交换前后文本，不重新读取服务。
    assert(!desk.dirty);  // 工作文本与最近成功保存相同，dirty必须清除。
    ui_key(&desk, UI_SELECT_ALL, false);  // 建立整个文档选区，下一次键入替换旧内容。
    for (unsigned i = 0; i < UI_DOC_SIZE - 1; ++i)  // 为8191有效字节填内容，保留最后一字节给NUL。
        desk.clipboard[i] = 'a';  // 每项填一个ASCII字节，构造满容量文本。
    desk.clipboard[UI_DOC_SIZE - 1] = 0;  // 最后写NUL，后续strlen和插入才有明确终点。
    ui_key(&desk, UI_PASTE, false);  // 以容量和ASCII合同插入完整剪贴板。
    assert(strlen(desk.editor) == UI_DOC_SIZE - 1);  // 满容量插入后的有效文本长度应为8191。
    ui_key(&desk, 'b', false);  // 追加一个字节故意超过可保存长度。
    assert(strlen(desk.editor) == UI_DOC_SIZE - 1 && strstr(desk.toast, "limit"));  // 超限必须保留原长度并显示拒绝原因。
    assert(desk.editor_column_scroll > 0);  // 长行光标移到尾部应推动水平滚动。
    ui_key(&desk, UI_SELECT_ALL, false);    // 建立整个文档选区，下一次键入替换旧内容。
    typing("short\n");  // 全选替换短文释放容量，不是向满文档追加。
    assert(!strcmp(desk.editor, "short\n"));  // 替换后的工作文本必须完整匹配短文。
    for (unsigned i = 0; i < 50; ++i)  // 连续追加五十行以触发垂直滚动。
        typing("row\n");  // 一次写入完整四字节行。
    assert(desk.editor_scroll > 0);     // 末尾光标应促使文档视口向下卷动。
    ui_key(&desk, UI_SAVE_KEY, false);  // 请求当前文件id保存工作文本。
    ui_open_document(&desk, 1);      // 快照下标1是初始CSV，打开后转入Viewer。
    assert(desk.series_count == 6);  // 当前有效曲线仍有六个点。
    assert(backend.action(NULL, desk.directory, UI_SAVE, 2, "series.csv", "seconds,load\n0,12\n1,invalid\n",  // 将文件id=2保存为可读取但第二行数值损坏的CSV。
                          desk.dialog_text, sizeof(desk.dialog_text)));  // 服务错误缓冲独立有界；本次写入本身应成功。
    ui_open_document(&desk, 1);  // 快照下标1是初始CSV，打开后转入Viewer。
    assert(desk.dialog == UI_DIALOG_ERROR && desk.series_count == 6);  // 坏CSV解析失败，错误框出现但旧六点曲线保留。
    ui_key(&desk, UI_ESCAPE, false);  // 关闭当前消息框或取消字段，回到原编辑对象。
    desk.pending_close = true;  // 模拟保存后继续关闭的待办状态。
    desk.dialog = UI_DIALOG_SAVE_AS;  // 当前显示另存为输入框。
    ui_key(&desk, UI_ESCAPE, false);  // 关闭当前消息框或取消字段，回到原编辑对象。
    assert(!desk.pending_close);      // 取消另存为必须连待关闭意图一起取消。
    ui_open(&desk, UI_FILES_APP);     // 把后续键事件转到Files视图。
    ui_key(&desk, UI_DELETE_KEY, false);      // 列表删除键先建立确认框。
    assert(desk.dialog == UI_DIALOG_DELETE);  // 不能在确认前直接删除选中记录。
    ui_key(&desk, UI_ESCAPE, false);   // 关闭当前消息框或取消字段，回到原编辑对象。
    desk.dialog = UI_DIALOG_NEW_FILE;  // 模拟新文件名称输入框。
    desk.field_all = true;  // 输入字段全选，粘贴应替换旧名称。
    strcpy(desk.dialog_input, "untitled");  // 初始化旧名称，供替换后比较。
    strcpy(desk.clipboard, "paste.lua");    // 名称剪贴板独立保存完整新名称。
    ui_key(&desk, UI_PASTE, false);  // 以容量和ASCII合同插入完整剪贴板。
    assert(!strcmp(desk.dialog_input, "paste.lua"));  // 字段全选粘贴必须完整替换名称。
}
```

再从新起点检查拒绝与恢复。两个 UTF-8 字节组成的 é 交给 ASCII 插入路径，必须整体拒绝，原文、undo 和 dirty 不变；随后真正修改文档，离线关闭前保存应失败，并消耗 pending_close。恢复服务再普通保存时，Editor 要继续打开。后半用两个显式目录与筛选后的唯一项，确认查询不偷偷改变动作目录、快照下标不冒充服务身份。追加这些合同检查。

<!-- append: tests/unit/ui_test.c -->
```c
static void explicit_contracts(void) {  // 核对拒绝非ASCII、失败关闭、显式目录和可见选择三组合同。
    start();  // 以重新初始化的参考服务和桌面开始本项检查。
    char original[UI_DOC_SIZE];     // 独立数组保留操作前的文档字节，供后面比较。
    strcpy(original, desk.editor);  // 当前有界文档复制到同容量快照，不能只借用工作数组。
    strcpy(desk.clipboard, "\xc3\xa9");  // 构造é的两个UTF-8字节，当前ASCII合同应拒绝它们。
    ui_key(&desk, UI_PASTE, false);      // 以容量和ASCII合同插入完整剪贴板。
    assert(!strcmp(original, desk.editor) && !desk.undo_valid && !desk.dirty);  // 拒绝整次插入，不改变工作文本、撤销或dirty。
    assert(strstr(desk.toast, "ASCII"));  // 输入拒绝必须说明ASCII限制。
    typing("changed");  // 制造尚未保存的修改，供关闭失败观察。
    ui_service_state(&desk, UI_OFFLINE);  // 切为离线状态，后续服务动作应失败。
    desk.dialog = UI_DIALOG_CLOSE;  // 未保存关闭要先确认保存或放弃。
    ui_key(&desk, '\n', false);     // 回车确认当前框或查找，具体用途由当前focus/dialog决定。
    assert(desk.dialog == UI_DIALOG_ERROR && !desk.pending_close && desk.dirty);  // 离线保存失败应保留修改并取消待关闭，窗口不被误关。
    ui_key(&desk, '\n', false);  // 回车确认当前框或查找，具体用途由当前focus/dialog决定。
    ui_service_state(&desk, UI_NORMAL);  // 恢复普通服务状态，下一动作重新访问参考服务。
    ui_key(&desk, UI_SAVE_KEY, false);   // 请求当前文件id保存工作文本。
    assert(desk.windows[UI_EDITOR].open && !desk.dirty);  // 恢复服务并保存后Editor仍打开且内容已保存。
    struct ui_file other[UI_FILES];  // 独立快照承接另一个目录查询，不替换桌面当前目录。
    backend.files(NULL, "/home/airi", other, UI_FILES);  // 显式查询主目录，后续动作仍应使用desk.directory。
    ui_command(&desk, UI_CREATE_FILE, 0, "explicit.txt", "");  // 创建目标来自桌面显式目录，不受刚才查询副作用影响。
    assert(desk.file_count == 7);  // 六条初始目录记录加一次创建应变成七条。
    ui_open(&desk, UI_FILES_APP);  // 把后续键事件转到Files视图。
    strcpy(desk.filter[UI_FILES_APP], ".lua");  // 筛选只保留Lua名称，后续上下键应沿可见索引选择。
    unsigned order[UI_FILES];  // 数组保存排序后的快照下标，不复制文件记录。
    unsigned count = ui_file_order(&desk, order);  // 获取筛选排序后的有效快照下标。
    assert(count == 1);  // 此输入应只留下一个匹配项。
    desk.selection_file = UI_FILES - 1;   // 故意放入不对应有效可见项的旧选择。
    ui_key(&desk, AIRI_KEY_DOWN, false);  // 从旧选择恢复到实际可见项，不能用容量下标访问记录。
    assert(desk.selection_file == order[0]);  // 修正选择后应指向唯一匹配的原快照下标。
    char error[160];  // 服务失败原因写入这个有界数组。
    assert(backend.action(NULL, "/home/airi", UI_CREATE_FILE, 0, "separate.txt", "", error, sizeof(error)));  // 在另一显式目录建立记录，必须不受桌面当前目录影响。
    unsigned n = backend.files(NULL, "/home/airi", other, UI_FILES);  // 在同一显式目录重新查询并取得有效条数。
    int id = -1;  // -1表示尚未找到目标文件。
    for (unsigned i = 0; i < n; ++i)  // 仅遍历这次查询返回的有效项。
        if (!strcmp(other[i].name, "separate.txt"))  // 完整名称相等时取稳定服务id。
            id = other[i].id;  // 保存跨排序和名称修改仍可使用的身份。
    assert(id > 0);  // 新创建项必须分配有效正id。
    assert(backend.action(NULL, "/home/airi", UI_RENAME, id, "main.lua", "", error, sizeof(error)));  // 不同目录可以有同名main.lua，显式目录与id决定此次目标。
}
```

下面回到主题问题。appearance 先在原文首部逐键插入 unsaved，再全选形成真实未保存文档；document、saved、cursor/anchor 和 Editor rect 各自留一份比较值。打开 Settings 后由 ui_pointer 发送 Day、Night 的按下与释放，要求 theme 改变，而文档、保存点、选区、dirty 和 Editor 几何保留。随后同一 console 输入主题命令，非法主题也不能覆盖已有模式。点击点仍由 u_window_body 取得原点并叠加按钮坐标，主题间隙也须保持已有偏好；末尾还以0x123456与日夜背景检查实际颜色映射。追加外观状态检查。

<!-- append: tests/unit/ui_test.c -->
```c
static void appearance(void) {  // 用真实设置点击检查换色不损坏文档和窗口。
    start();  // 每个用例重建独立会话。
    typing("unsaved");  // 制造真实未保存文本。
    ui_key(&desk, UI_SELECT_ALL, false);  // 建立必须保留的光标与选择。
    char document[UI_DOC_SIZE];     // 只复制被测文本，不重建整份状态。
    strcpy(document, desk.editor);  // 保存换色前内容作为独立预期。
    char saved[UI_DOC_SIZE];    // 独立保存最近一次成功保存的字节，换色不能重设保存点。
    strcpy(saved, desk.saved);  // 复制保存点，不借用稍后可能被修改的原数组。
    unsigned cursor = desk.cursor, anchor = desk.anchor;     // 记录选择两端。
    struct ui_rect position = desk.windows[UI_EDITOR].rect;  // 编辑窗口不因 Settings 打开而移动。
    ui_open(&desk, UI_SETTINGS);  // 通过正常入口激活设置。
    struct ui_rect body = u_window_body(desk.windows[UI_SETTINGS].rect);  // 点击复用公共布局度量。
    for (unsigned mode = 0; mode < 2; ++mode) {  // 先点击 Day，再切回 Night。
        unsigned expected = mode == 0 ? UI_DAY : UI_NIGHT;  // 明确本次按钮含义。
        int x = body.x + 28 + (int)expected * 138 + 30, y = body.y + 166;  // 按下点落在按钮内部。
        ui_pointer(&desk, x, y, 1);      // 正常桌面命中路由执行设置。
        assert(desk.theme == expected);  // 主题由桌面拥有，命令通过扩展消费它。
        assert(desk.pointer_down);   // 按下状态供公共绘图使用。
        ui_pointer(&desk, x, y, 2);  // 释放不重复执行设置。
        assert(!desk.pointer_down && desk.dirty && desk.selection);  // 未保存和选择状态不能被清空。
        assert(desk.cursor == cursor && desk.anchor == anchor);      // 光标与选择两端不变。
        assert(!strcmp(document, desk.editor));  // 工作文本逐字节保留。
        assert(!strcmp(saved, desk.saved));      // 最近成功保存的字节也保留，dirty含义不能因换色改变。
        assert(!memcmp(&position, &desk.windows[UI_EDITOR].rect, sizeof(position)));  // 窗口位置和大小都不变。
    }
    ui_pointer(&desk, body.x + 159, body.y + 166, 1);  // 两个按钮间的空隙不属于任何操作。
    assert(desk.theme == UI_NIGHT);  // 间隙不能切换为 Day。
    ui_pointer(&desk, body.x + 159, body.y + 166, 2);  // 恢复释放状态。
    const char *command = "theme day\n";  // 控制台提供相同设置入口。
    for (const char *p = command; *p; ++p) airi_console_key(&console, *p);  // 逐键送入命令模型，不绕过解析。
    assert(desk.theme == UI_DAY && !strcmp(document, desk.editor));  // 命令成功也不触碰编辑器。
    for (const char *p = "theme invalid\n"; *p; ++p)  // 沿完整命令字节递进至NUL。
        airi_console_key(&console, *p);  // 未知主题经真实解析被拒绝，不覆盖偏好。
    assert(desk.theme == UI_DAY);  // 无效输入保留已有模式。
    u_theme(UI_DAY);  // 下方检查颜色边界与双主题映射。
    uint32_t day = u_color(U_BG);  // 日间桌面应不同于夜间。
    assert(u_color(0x123456) == 0x123456);  // 字形混合生成的真实 RGB 不得再映射。
    u_theme(UI_NIGHT);  // 切换渲染器模式。
    assert(day != u_color(U_BG));  // 确认不是仅更改设置标签。
}
```

状态保持已检查，boundary_render 再给同一个渲染器换输入布局。四组宽高、24/32位和两种主题逐组配对；每行在可见像素后额外留16字节，整块首尾各留32字节并置0xa5。全部应用、页签、服务状态、长名称框和越界圆角都在这块数组内绘制；最后既查首尾，也逐行查填充，才释放内存。追加绘图边界检查。

<!-- append: tests/unit/ui_test.c -->
```c
static void boundary_render(void) {  // 在多尺寸、多位布局和哨兵内存上调用同一绘图实现。
    const unsigned widths[] = {640, 1024, 1280, 1600}, heights[] = {480, 768, 800, 900};  // 同下标配对四种画布尺寸。
    for (unsigned mode = UI_NIGHT; mode <= UI_DAY; ++mode)  // 所有像素边界用例在两种主题下执行。
    for (unsigned k = 0; k < 4; ++k)  // 遍历四组宽高组合。
        for (unsigned bpp = 24; bpp <= 32; bpp += 8) {  // 每个尺寸分别检查三字节和四字节像素。
            unsigned bytes = bpp / 8, pitch = widths[k] * bytes + 16;  // 位数除八换字节；每行故意多留16字节填充。
            size_t size = (size_t)pitch * heights[k];  // 先扩展成size_t再相乘，得到全画布字节数。
            unsigned char *p = malloc(size + 64);      // 两端各额外32字节，像素区域放在中间。
            assert(p);  // 分配成功后才能初始化和绘图。
            memset(p, 0xa5, size + 64);  // 连同两端与行填充置为哨兵值0xa5。
            struct airi_framebuffer fb = {.virtual_address = (uintptr_t)(p + 32),  // 真实像素地址跳过前端32字节哨兵，uintptr_t保存地址值。
                                          .width = widths[k],    // 本次像素宽度来自第k组。
                                          .height = heights[k],  // 高度取同组配对值。
                                          .pitch = pitch,  // 每行跨度包含16字节填充，绘图必须按它寻址。
                                          .bpp = bpp,      // 当前采用24或32位像素。
                                          .red_size = 8,   // 红通道占八位。
                                          .red_shift = 16,   // 红通道放在位16..23，读写需平移16位。
                                          .green_size = 8,   // 绿通道占八位。
                                          .green_shift = 8,  // 绿通道放在位8..15。
                                          .blue_size = 8};   // 蓝通道占八位，未指定 blue_shift 时默认零。
            start();  // 当前尺寸、位数与主题使用独立重置后的服务。
            desk.theme = mode;  // 桌面绘制从会话模型获取当前主题。
            for (int app = 0; app < UI_APP_COUNT; ++app) {  // 每个应用在同一布局分别绘制。
                ui_open(&desk, (enum ui_app)app);    // 把当前检查应用打开置顶。
                desk.windows[app].maximized = true;  // 用全工作区测试实际布局边界。
                for (unsigned tab = 0; tab < (app == UI_SYSTEM     ? 5u  // System检查五个有效页签。
                                              : app == UI_SETTINGS ? 4u  // Settings检查四个页签。
                                                                   : 1u);  // 其他应用只有一个内容页。
                     ++tab) {  // 每次推进一个页签下标。
                    desk.windows[app].tab = tab;  // 把页签状态交给实际绘图。
                    ui_draw(&desk, &fb);  // 在有哨兵和填充的本次像素数组内绘制。
                }
            }
            for (int state = UI_EMPTY; state <= UI_OFFLINE; ++state) {  // 分别检查空、加载、错误和离线四种服务状态。
                ui_service_state(&desk, (enum ui_state)state);  // 把整数状态转枚举并刷新状态显示。
                ui_open(&desk, UI_FILES_APP);  // 把后续键事件转到Files视图。
                ui_draw(&desk, &fb);  // 在有哨兵和填充的本次像素数组内绘制。
                ui_open(&desk, UI_TASKS_APP);  // Tasks也需在各服务状态下绘制。
                ui_draw(&desk, &fb);  // 在有哨兵和填充的本次像素数组内绘制。
            }
            desk.dialog = UI_DIALOG_SAVE_AS;  // 当前显示另存为输入框。
            strcpy(desk.dialog_title, "Save document");  // 构造另存为标题，覆盖确认框文字布局。
            strcpy(desk.dialog_text, "A long path or document name stays inside the dialog.");  // 完整说明仍应限制在对话框矩形内。
            memset(desk.dialog_input, 'x', 127);  // 填满输入框有效内容，观察长名称裁剪。
            desk.dialog_input[127] = 0;  // 保留第128字节终止NUL。
            ui_draw(&desk, &fb);  // 在有哨兵和填充的本次像素数组内绘制。
            u_round((struct ui_rect){-4, -3, 14, 12}, 6, U_ACCENT);  // 圆角越过左上边界仍只能写有效像素。
            u_round((struct ui_rect){(int)widths[k] - 3, (int)heights[k] - 4, 12, 14}, 6, U_BLUE);  // 同样验证右下裁剪。
            for (unsigned i = 0; i < 32; ++i)  // 检查数组首尾各32字节的写边界。
                assert(p[i] == 0xa5 && p[size + 32 + i] == 0xa5);  // 前后哨兵均未变化才说明这组绘图没越界。
            for (unsigned y = 0; y < heights[k]; ++y)  // 按真实行数检查每一行填充。
                for (unsigned x = widths[k] * bytes; x < pitch; ++x)  // 只遍历可见像素后到实际pitch之间的字节。
                    assert(p[32 + (size_t)y * pitch + x] == 0xa5);  // 行填充未变化，才能排除将pitch误当宽度的写错。
            free(p);  // 全部绘图和检查完成后才释放整块主机内存。
        }
}
```

最后的窗口检查继续走事件入口：标题按下、移动、释放改变位置；最大化与恢复、最小化与重新打开保留同一窗口；有修改时关闭必须先确认，Discard 恢复 saved 后才关闭。继续追加这个独立函数。

<!-- append: tests/unit/ui_test.c -->
```c
static void mouse_windows(void) {  // 通过 ui_pointer 发送 C 事件序列，观察移动、最大化、最小化与关闭。
    start();  // 以重新初始化的参考服务和桌面开始本项检查。
    struct ui_window *w = &desk.windows[UI_EDITOR];  // 借用同一Editor窗口对象，后续事件直接修改它。
    int ox = w->rect.x, oy = w->rect.y;  // 保存初始左上坐标供位移比较。
    ui_pointer(&desk, ox + 220, oy + 15, 1);  // 在标题栏普通位置按下，启动移动并保存偏移。
    ui_pointer(&desk, ox + 250, oy + 40, 0);  // 鼠标横移30、纵移25，核心应保持按下偏移。
    ui_pointer(&desk, ox + 250, oy + 40, 2);  // 释放停止移动，保留新的窗口矩形。
    assert(w->rect.x == ox + 30 && w->rect.y == oy + 25);  // 实际窗口位移必须与鼠标位移一致。
    ui_pointer(&desk, w->rect.x + w->rect.w - 55, w->rect.y + 16, 1);  // 标题栏最大化按钮坐标，用当前实际矩形定位。
    assert(w->maximized);  // 首次点击进入最大化状态。
    ui_pointer(&desk, w->rect.x + w->rect.w - 55, w->rect.y + 16, 1);  // 标题栏最大化按钮坐标，用当前实际矩形定位。
    assert(!w->maximized);  // 再点击应恢复普通窗口。
    ui_pointer(&desk, w->rect.x + w->rect.w - 85, w->rect.y + 16, 1);  // 最小化按钮把窗口暂时移出命中/绘制。
    assert(w->minimized);  // 最小化必须保留对应状态。
    ui_open(&desk, UI_EDITOR);  // 将Editor再次置顶，不创建第二个Editor窗口。
    assert(!w->minimized);      // ui_open重新显示同一窗口，不创建副本。
    typing("changed");  // 制造尚未保存的修改，供关闭失败观察。
    ui_pointer(&desk, w->rect.x + w->rect.w - 25, w->rect.y + 16, 1);  // 点击关闭，Editor有修改时必须先出确认框。
    assert(desk.dialog == UI_DIALOG_CLOSE);  // 未保存内容不能直接丢失或自动关闭。
    ui_pointer(&desk, desk.width / 2 + 36, desk.height / 2 + 78, 1);  // 命中关闭确认框的放弃按钮。
    assert(!w->open && !desk.dirty);  // 放弃后窗口关闭且工作文本回到已保存状态。
    assert(!strcmp(desk.editor, desk.saved));  // 放弃动作恢复saved字节，不写新文本到服务。
}
```

密度检查先点两个按钮之间和同高度的右侧空白，状态保持宽松；再真实点Compact与Comfortable，分别得到1和0。这样不会把“所有输入都忽略”当成命中正确。 密度函数也先 start，随后 main 组合全部独立检查，只有所有断言通过才打印 PASS。继续追加至文件结束。

<!-- append: tests/unit/ui_test.c -->
```c
static void density_hit_boxes(void) {  // 密度状态只由两个当前可见按钮改变，空白不冒充Compact。
    start();  // 独立重建参考服务与桌面，不继承前一项的模式或对话框。
    ui_open(&desk, UI_SETTINGS);  // Settings成为当前内容点击接收者。
    struct ui_rect body = u_window_body(desk.windows[UI_SETTINGS].rect);  // 按同一标题与边框度量取得内容原点。
    ui_apps_click(&desk, UI_SETTINGS, body, body.x + 160, body.y + 266, false);  // 两126像素按钮之间的十二像素间隙。
    assert(desk.density == 0);  // 未命中任何按钮，宽松模式保留。
    ui_apps_click(&desk, UI_SETTINGS, body, body.x + body.w - 1, body.y + 266, false);  // 同高度右侧空白不是Compact。
    assert(desk.density == 0);  // 宽泛横条判断会在此错误切换。
    ui_apps_click(&desk, UI_SETTINGS, body, body.x + 176, body.y + 266, false);  // Compact矩形内部产生有效动作。
    assert(desk.density == 1);  // 有效按钮必须可切到紧凑模式。
    ui_apps_click(&desk, UI_SETTINGS, body, body.x + 38, body.y + 266, false);  // Comfortable矩形内部回到宽松模式。
    assert(desk.density == 0);  // 两个方向都通过实际内容分派执行。
}

int main(void) {  // 观察程序入口；返回非零表示预期状态未出现。
    density_hit_boxes();  // 先核对空白、间隙与两个真实密度按钮。
    appearance();  // 换色验收先于通用应用回归。
    explicit_contracts();  // 先检查输入拒绝、目录目标与可见选择合同。
    workflow();  // 以独立重置后的状态检查完整文件编辑过程。
    input_boundaries();  // 运行容量、查找、滚动及坏CSV边界。
    mouse_windows();     // 检查跨阶段鼠标窗口动作。
    boundary_render();   // 最后覆盖应用、状态和多布局绘图哨兵。
    puts("PASS frontend: Day/Night state preservation, document workflow, provider failures, CRUD, task actions, window input, 7 "  // 所有assert均通过后才打印最终结果，字符串继续到下一行。
         "applications/states, 24/32 bpp guarded rendering (ASan/UBSan)");  // 相邻C字符串字面量在编译时合并，不是第二次输出。
    return 0;  // 正常结束当前程序，成功状态交给启动它的命令。
}
```

ASan/UBSan检查非法访问和未定义行为；哨兵另检查“仍在已分配数组内、却写入非像素填充”的错误。

ui_test.c 检查共享桌面，接下来另建 tests/unit/display_test.c 单独链接 G24 的显示适配。它在主机内构造启动描述，把设备地址换成带哨兵和填充的数组后同步绘图。这里检查适配代码的写入与会话关系，实际 UEFI 映射及 QEMU 展示仍由后面的 guest 命令检查。先写启动 fixture 和完整命令输入 helper。

<a id="code-tests-unit-display-test-c-1"></a>

<!-- write: tests/unit/display_test.c -->
```c
#include <assert.h>  // 断言实际绘图结果。
#include <stdlib.h>  // 为带哨兵的宿主画布分配空间。
#include <string.h>  // 初始化哨兵字节。
#include <stdint.h>  // 画布地址使用固定宽整数。
#include <stdio.h>   // 输出本轮完成记录。
#include <airi/kernel.h>   // 借用启动描述与图形适配声明。
#include <airi/console.h>  // 通过同一文本命令模型切换视图。
static AiriBootInfo fixture(void) {  // 构造合法启动输入；每组布局取一份副本，同组故障逐项修改并恢复字段。
    AiriBootInfo b = {.magic = AIRI_BOOT_MAGIC,  // 指定magic识别协议，未指定成员按C规则清零。
                      .version = AIRI_BOOT_VERSION,  // 采用当前接口版本。
                      .size = sizeof(AiriBootInfo),  // 字节大小与编译的结构布局一致。
                      .direct_map_offset = UINT64_C(0xffff800000000000),  // UINT64_C明确64位常量，用作物理直接映射偏移。
                      .kernel_physical_base = 0x100000,  // 构造内核物理起点1MiB。
                      .kernel_virtual_base = UINT64_C(0xffffffff80000000),  // 内核虚拟起点与链接模型对应。
                      .kernel_size = 4096,  // 构造一页4096字节内核长度。
                      .region_count = 2};   // 只有两条内存区域有效。
    b.regions[0] =  // 第一项描述0..1MiB保留区域。
        (struct airi_memory_region){.base = 0, .length = 0x100000, .kind = AIRI_MEMORY_RESERVED};  // 复合字面量按字段赋值，物理范围不标为可分配。
    b.regions[1] = (struct airi_memory_region){  // 第二项从1MiB开始，紧邻首项且不重叠。
        .base = 0x100000, .length = 0x400000, .kind = AIRI_MEMORY_USABLE};  // 1..5MiB共4MiB构造可用范围。
    return b;  // 按值返回整个结构，调用者得到可独立损坏的副本。
}
static void type(struct airi_console *c, const char *s) {  // 把完整命令按字节送入同一个console，再回车执行。
    while (*s)  // 当前字节不是NUL才继续输入。
        airi_console_key(c, *s++);  // 先取当前字节送键事件，再将源指针推进。
    airi_console_key(c, '\n');  // 命令末尾明确发送提交键，不把NUL当按键。
}
```

fixture 只建立启动合同与两条内存区域，尚无可写设备。graphics_tests 每轮分配带哨兵和填充的主机数组，把地址写进本轮 BootInfo，再调用 G24 的真实适配代码；这仍是主机边界检查，不是实际 UEFI 映射。先画仪表台，再切换和重置命令会话，后面继续检查主题像素与各命令视图。追加本轮绘图检查。

同一地址不一定仍是原会话。显示检查先由静态仪表台切到局部 console，确认新对象获得扩展；再在原地址重新初始化，使 extension 清空，重画必须恢复绑定。随后沿同一对象发送 theme day、theme night，底栏同一像素先变化再恢复。前者检查漏重绑，后者检查每帧重建而丢主题，两项不能用一次返回true互相代替。

<!-- append: tests/unit/display_test.c -->
```c
static void graphics_tests(void) {  // 让真实共享绘图在主机有界画布运行，核对两端和行填充。
    /* Guarded 24/32-bit framebuffer with padded scanlines: exercise real renderer. */
    const unsigned layouts[] = {24, 32, 32};  // 前两次640×480分别24/32位，第三次1280×800采用32位。
    for (size_t k = 0; k < 3; ++k) {  // 遍历上述三组实际布局。
        AiriBootInfo b = fixture();  // 在当前调用栈中取得合法完整副本。
        unsigned width = k == 2 ? 1280 : 640, height = k == 2 ? 800 : 480;  // 第三组使用大画布，其余使用最小支持尺寸。
        uint64_t pitch = width * (layouts[k] / 8) + 16, size = pitch * height;  // 像素位数换字节，每行附16字节填充，总字节数包含它。
        unsigned char *pixels = malloc((size_t)size + 64);  // 画布两侧各多分配32字节哨兵。
        assert(pixels);  // 分配有效后才可写填充和调用绘图。
        memset(pixels, 0xa5, (size_t)size + 64);  // 可见区域、行填充和首尾都先置同一哨兵。
        b.framebuffer = (struct airi_framebuffer){.virtual_address = (uintptr_t)(pixels + 32),  // 画布地址跳过前端哨兵，后端仍预留32字节。
                                                  .width = width,    // 按本轮选择的像素宽度绘图。
                                                  .height = height,  // 使用同组高度。
                                                  .pitch = pitch,    // 行地址必须按包含填充的字节跨度计算。
                                                  .bpp = (uint16_t)layouts[k],  // 结构要求16位字段，当前24/32均在其范围内。
                                                  .red_size = 8,     // 红通道占八位。
                                                  .red_shift = 16,   // 红通道放在位16..23，读写需平移16位。
                                                  .green_size = 8,   // 绿通道占八位。
                                                  .green_shift = 8,  // 绿通道放在位8..15。
                                                  .blue_size = 8};   // 蓝通道占八位，未指定 blue_shift 时默认零。
        assert(airi_framebuffer_valid(&b.framebuffer));  // 当前布局应满足地址、范围与颜色位合同。
        assert(airi_dashboard_draw(&b));  // 仪表台必须实际在该布局完成一帧。
        struct airi_console c;      // 独立命令状态在当前检查期间有效。
        airi_console_init(&c, &b);  // 借用当前合法启动描述，初始化命令与日志。
        assert(airi_console_draw(&c));  // 从仪表台切换到另一份输入会话，先为它绘制首帧。
        assert(c.extension);  // 新会话必须已安装自己的图形命令回调，不能沿用仪表台的绑定。
        airi_console_init(&c, &b);  // 同一地址也可成为重置后的新会话，初始化会清空扩展。
        assert(!c.extension);  // 确认这次重置确实撤下了此前的命令消费者。
        assert(airi_console_draw(&c));  // 地址未变也必须发现绑定失效并重建桌面。
        assert(c.extension);  // 重置后的会话重新获得自己的主题命令入口。
```

绑定检查通过以后，取底栏最右侧像素的前三字节，避开文字和窗口。night 是独立副本，后续帧可以覆盖原像素；同一 console 输入 Day 后要求字节不同，输入 Night 后要求恢复。再绘四种视图、内存翻页并查首尾及行填充，最后故意损坏 pitch、高度和通道重叠，分别要求描述验证拒绝。继续追加同一函数及主入口。

<!-- append: tests/unit/display_test.c -->
```c
        unsigned char *sample = pixels + 32 + (height - 1) * pitch + (width - 1) * (layouts[k] / 8);  // 取底栏最右像素的前三字节，避开文字和窗口。
        unsigned char night[3];  // 保存夜间底面前三个设备颜色字节；24/32 位均比较它们。
        memcpy(night, sample, sizeof(night));  // 独立记录新会话首帧，不借用会被下一帧覆盖的像素。
        type(&c, "theme day");  // 命令必须通过新console的扩展改变同一桌面主题。
        assert(airi_console_draw(&c));  // 同一console重画，不能重新初始化并丢掉刚设置的主题。
        assert(memcmp(night, sample, sizeof(night)) != 0);  // 日间底栏必须真正换色，只有命令返回不足以通过。
        type(&c, "theme night");  // 沿同一输入会话切回夜间。
        assert(airi_console_draw(&c));  // 再次重画保留本次设置，且不更换底层画布。
        assert(memcmp(night, sample, sizeof(night)) == 0);  // 同一个底面像素恢复首帧字节。
        for (unsigned view = 0; view < 4; ++view) {  // 依次覆盖终端、内存、设备与日志四页。
            airi_console_key(&c, AIRI_KEY_F1 + (int)view);  // 连续功能键枚举以F1为起点加当前页号。
            assert(airi_console_draw(&c));  // 每个真实命令视图都必须成功绘制。
        }
        type(&c, "mem map");  // 输出同一启动描述中的完整物理地图。
        airi_console_key(&c, AIRI_KEY_PAGE_UP);  // 从当前内存页向前翻页，首页面不能下溢。
        assert(airi_console_draw(&c));     // 每个真实命令视图都必须成功绘制。
        for (unsigned i = 0; i < 32; ++i)  // 检查首尾各32字节的数组界限。
            assert(pixels[i] == 0xa5 && pixels[size + 32 + i] == 0xa5);  // 绘图不得改变画布之外的两个哨兵区。
        for (unsigned row = 0; row < height; ++row)  // 每行分别检查，不只看整个数组两端。
            for (unsigned col = width * (layouts[k] / 8); col < pitch; ++col)  // 从可见行末到真实pitch之间都是不可写填充。
                assert(pixels[32 + row * pitch + col] == 0xa5);  // 按真实pitch定位，确认没有写到行尾填充。
        b.framebuffer.pitch = 1;  // 故意让一行跨度小于可见像素字节数。
        assert(!airi_framebuffer_valid(&b.framebuffer));  // 当前非法布局应被拒绝，不能继续绘图。
        b.framebuffer.pitch = pitch;  // 恢复合法行距，下一故障只损坏高度。
        b.framebuffer.height = UINT64_MAX;  // 乘行距必溢出的高度必须拒绝。
        assert(!airi_framebuffer_valid(&b.framebuffer));  // 当前非法布局应被拒绝，不能继续绘图。
        b.framebuffer.height = (uint64_t)INT32_MAX + 1;   // 总字节跨度仍可表示，但高度已不能转成绘图 int 坐标
        assert(!airi_framebuffer_valid(&b.framebuffer));  // 坐标范围也必须拒绝，不能只检查64位乘法
        b.framebuffer.height = 480;      // 恢复合法高度，再检查颜色布局冲突。
        b.framebuffer.green_shift = 16;  // 绿色与红色位区间重叠，不能接受。
        assert(!airi_framebuffer_valid(&b.framebuffer));  // 当前非法布局应被拒绝，不能继续绘图。
        free(pixels);  // 本轮所有绘图、哨兵和非法描述检查完成后，释放主机数组。
    }
    AiriBootInfo b = fixture();  // 在当前调用栈中取得合法完整副本。
    assert(!airi_dashboard_draw(&b));  // 未提供画布的fixture不应报告图形成功。
}
int main(void) {  // 图形目标单独验证实际绘图实现。
    graphics_tests();  // 检查布局、填充、哨兵和无设备拒绝。
    puts("PASS display guards");  // 所有断言完成后才报告通过。
    return 0;  // 正常退出本轮宿主检查。
}
```

## 将状态检查和原生观察接进 Make

`ui-test` 将共享 C、参考服务和检查程序链接到主机，保留 assert 并启用 sanitizer；显示边界程序再单独链接 guest 适配层。`ui-preview` 目标只在 macOS 构建 Cocoa 观察程序。两个主机目标都在可选的 `frontend/targets.mk` 中声明，由入口显式选择。

**完整写入 `frontend/targets.mk`：**

<a id="code-makefile-1"></a>

<!-- write: frontend/targets.mk -->
```make
# 图形支线自己的构建目标；默认构建不读取此片段。
include frontend/sources.mk
# 原生窗口与共享C检查均为动作目标。
.PHONY: ui-preview ui-test
# 链接宿主C/Objective-C与Cocoa；这个目标仅macOS可用。
ui-preview:
# 确保原生窗口和前端检查输出目录存在。
	@mkdir -p build/ui
# 下面续行用共享C源码及reference服务，最终链接Cocoa输出宿主窗口程序。
	$(HOST_CC) -std=c11 -O2 -g -Wall -Wextra -Werror -Iinclude -Ifrontend/include \
	    $(UI_SOURCES) frontend/backends/reference.c kernel/lib/base.c kernel/boot/info.c \
	    kernel/console/console.c frontend/platform/macos/preview.m -framework Cocoa -o build/ui/airios-ui
# 编译共享C状态与像素边界检查，不需要Cocoa。
ui-test:
# 确保原生窗口和前端检查输出目录存在。
	@mkdir -p build/ui
# 下面续行编译所有共享源码与参考服务，ASan/UBSan检查该主机进程。
	$(HOST_CC) -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
	    -Iinclude -Ifrontend/include tests/unit/ui_test.c $(UI_SOURCES) frontend/backends/reference.c \
	    kernel/lib/base.c kernel/boot/info.c kernel/console/console.c -o build/ui/ui_test
# 运行前端动作序列与渲染哨兵检查。
	./build/ui/ui_test
# 在主机中链接 guest 显示适配代码，检查像素边界。
	$(HOST_CC) -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -Iinclude -Ifrontend/include tests/unit/display_test.c kernel/lib/base.c kernel/boot/info.c kernel/console/console.c kernel/console/display.c $(UI_SOURCES) -o build/ui/display_test
	./build/ui/display_test # 执行当前带哨兵画布检查，失败中止Make。


.PHONY: lab-editor # 同名文件不应使观察动作被跳过。
# 重新构建编辑、保存与执行失败的状态观察。
lab-editor:
# 建立观察程序目录，保持所有可重建产物在build。
	@mkdir -p build/labs
# 下面续行按O0保留便于调试的步骤，链接当前观察所需的同一自有实现。
	$(HOST_CC) -std=c11 -O0 -g -Wall -Wextra -Werror -Iinclude -Ifrontend/include \
	    labs/host/frontend/editor_transaction.c $(UI_SOURCES) frontend/backends/reference.c \
	    kernel/lib/base.c kernel/boot/info.c kernel/console/console.c -o build/labs/editor
# 打印同一文档编辑、保存及Run失败的实际状态。
	./build/labs/editor
```

## 比较相同场景的数值和像素

<a id="contrast-and-snapshots"></a>

新建 ui_preview.py。它有两个入口：preview 构建并打开交互应用，check_frontend 先运行可移植 C 检查，再按平台与 screenshots 选择是否生成图库。共同导入不启动窗口；先写导入与应用名称表。

<a id="code-scripts-ui-preview-py-1"></a>

<!-- write: scripts/ui_preview.py -->
```python
"""Build and inspect the shared native frontend without bootloader dependencies."""  # 模块说明文字；构建和窗口启动由下面的函数调用触发
import hashlib   # 摘要比较读取到的真实字库与图集字节。
import json      # 从锁文件恢复字典，报告再编码为JSON。
import platform  # 判断当前操作系统是否提供Cocoa。
import subprocess  # 按参数列表运行Make、原生窗口和图像转换。
import plistlib    # 把应用元数据字典编码成macOS属性列表。
import shutil      # 复制原生可执行文件并保留权限。
from pathlib import Path  # 路径对象支持/拼接，不是字符串除法。
from graphics_assets import verify_fonts  # 与guest共用纯标准库资产校验。
from tools import ROOT, make_command      # 复用根目录和Make工具参数。

APPS = ('terminal', 'files', 'editor', 'tasks', 'system', 'settings', 'viewer')  # 固定顺序对应C应用枚举。

```

交互入口先需要一个可由 macOS 打开的包。package_preview 把本轮可执行文件复制到 Contents/MacOS/AiriOSFrontend，并写 Info.plist；CFBundleExecutable 必须同这个文件名对应。preview 校验字体、构建 ui-preview 后取得包路径，再用 open -n -W 启动新实例并等待退出。包装没有增加绘图实现，高分屏标志也不改变逻辑画布尺寸。继续追加这两个入口 helper。

<!-- append: scripts/ui_preview.py -->
```python
def package_preview():  # 给已有可执行文件建立macOS .app目录结构。
    app = ROOT / 'build/ui/AiriOS Frontend.app'  # .app是包含程序和元数据的目录，不是另一份源码。
    executable = app / 'Contents/MacOS/AiriOSFrontend'     # macOS按元数据从这里找实际可执行文件。
    executable.parent.mkdir(parents=True, exist_ok=True)   # 补齐目录，已有目录不报错。
    shutil.copy2(ROOT / 'build/ui/airios-ui', executable)  # 复制本次编译结果并保留可执行权限。
    metadata = {  # 各键都被macOS应用加载路径读取。
        'CFBundleIdentifier': 'dev.airios.frontend',  # 应用身份字符串，与窗口标题不同。
        'CFBundleName': 'AiriOS',  # 应用内部名称。
        'CFBundleDisplayName': 'AiriOS',  # 面向用户显示的名称。
        'CFBundleExecutable': 'AiriOSFrontend',  # 必须与Contents/MacOS下的文件名一致。
        'CFBundleVersion': '1',  # 本演示包版本字符串。
        'CFBundlePackageType': 'APPL',    # 表示这是应用程序包。
        'NSHighResolutionCapable': True,  # 允许高分屏呈现；画布仍按前章逻辑尺寸分配。
    }
    (app / 'Contents/Info.plist').write_bytes(plistlib.dumps(metadata))  # 字典转plist字节写入规定路径。
    return app  # 返回包目录，open以它为启动目标。

def preview(app='editor', state='normal'):  # 默认打开Editor普通状态。
    verify_fonts()  # 字体有效才继续构建。
    if platform.system() != 'Darwin':  # Cocoa适配只存在于macOS。
        raise RuntimeError('Native preview uses macOS Cocoa; shared C model tests run without Cocoa')  # 给出平台边界。
    subprocess.run(make_command(target='ui-preview'), cwd=ROOT, check=True)  # 用同一Make规则链接平台适配。
    bundle = package_preview()  # 把刚生成的程序装进.app目录。
    subprocess.run(['open', '-n', '-W', str(bundle), '--args', '--app', app, '--state', state], cwd=ROOT, check=True)  # -n新建实例，-W等待退出，--args后传给程序。

```

sRGB的八位编码不是线性光强。luminance先从RGB整数右移并掩码取出红、绿、蓝，把每个0..255通道除以255；较小值除12.92，其余值经过2.4次幂，再按三个权重求和。两个相对亮度排序为low/high，(high+0.05)/(low+0.05)给出对比。普通文字目标4.5:1，必要控件边界和焦点目标3:1，对应[W3C文字对比](https://www.w3.org/WAI/WCAG22/Understanding/contrast-minimum.html)与[非文字对比](https://www.w3.org/WAI/WCAG22/Understanding/non-text-contrast.html)。比值不能判断遮挡、截字或窗口内容是否出现，仍需检查实际图像。

pairs每项是(前景名,背景名,目标比例)。例如("TEXT","SURFACE",4.5)从本次palette字典取两个RGB；29组组合分别跑Night和Day，形成58条结果。报告保存原始比值，失败组合导致总检查失败。轻分隔线不承担控件识别，禁用状态也不冒充正常可操作状态。

现在追加 check_contrast，让 JSON 恢复的角色字典成为亮度计算的实际输入。

<!-- append: scripts/ui_preview.py -->
```python
def check_contrast(out):  # 使用已编译程序导出的角色颜色计算实际组合。
    def luminance(rgb):  # sRGB 编码先转换成线性光，再加权求相对亮度。
        total = 0.0  # 黑色的初始亮度为零。
        for shift, weight in ((16, .2126), (8, .7152), (0, .0722)):  # 依红绿蓝位移与亮度权重取通道。
            value = ((rgb >> shift) & 255) / 255  # 八位编码归一化到零至一。
            linear = value / 12.92 if value <= .04045 else ((value + .055) / 1.055) ** 2.4  # 逆 sRGB 转移函数。
            total += weight * linear  # 累积该通道的线性亮度贡献。
        return total  # 结果用于两个颜色之间的对比。
    pairs = []  # 每项三元组保存前景角色、背景角色和目标比例。
    for foreground in ('TEXT', 'MUTED'):  # 正文与辅助文字各检查相同六个表面。
        for background in ('BG', 'PANEL', 'SURFACE', 'RAISED', 'HOVER', 'PRESSED'):  # 实际出现的内容与交互背景。
            pairs.append((foreground, background, 4.5))  # 普通文字要求至少4.5:1。
    for foreground in ('ACCENT', 'BLUE', 'RED', 'WARM', 'PINK', 'SUCCESS'):  # 六种语义和代码文字颜色。
        pairs.append((foreground, 'SURFACE', 4.5))  # 这些文字位于内容表面。
    for background in ('RAISED', 'PRESSED', 'HOVER'):  # 选中按钮使用的交互底色。
        pairs.append(('ACCENT', background, 4.5))  # 选中前景仍需可读。
    for background in ('PANEL', 'SURFACE', 'BG'):  # 必要控件边界邻接表面。
        pairs.append(('CONTROL', background, 3.0))  # 控件边界使用非文字3:1目标。
    for background in ('PANEL', 'SURFACE', 'RAISED'):  # 焦点框邻接背景。
        pairs.append(('BLUE', background, 3.0))  # 蓝色焦点边界同样使用3:1目标。
    pairs.append(('INK', 'CHROME', 4.5))  # 标题壳体文字与底色。
    pairs.append(('ON_ACCENT', 'ACCENT', 4.5))  # 实色主按钮内部文字与底色。
    results = []  # 记录每个实际检查值，失败时也保留。
    for theme in ('night', 'day'):  # 两个模式分别输出自己的完整调色板。
        raw = subprocess.check_output([str(ROOT / 'build/ui/airios-ui'), '--palette', '--theme', theme], timeout=15)  # 有限等待原生工具 JSON 输出。
        palette = json.loads(raw)     # 解码得到角色名称到 RGB 整数的字典。
        for fg, bg, target in pairs:  # 解包当前文字或边界组合及目标阈值。
            low, high = sorted((luminance(palette[fg]), luminance(palette[bg])))  # 按线性亮度排序，模式深浅不影响公式。
            ratio = (high + .05) / (low + .05)  # WCAG 对比公式，原值比较前不四舍五入。
            results.append(dict(theme=theme, foreground=fg, background=bg, ratio=ratio, target=target, passed=ratio >= target))  # 保存输入角色与实际结论。
    (out / 'contrast.json').write_text(json.dumps(results, indent=2) + '\n')  # 报告与截图保存在同一验证目录。
    failures = [row for row in results if not row['passed']]  # 只汇总未达到阈值的组合。
    if failures:  # 对比失败使总验证失败，不能仅产生报告。
        raise RuntimeError(f'Contrast targets failed: {failures}')  # 显示具体模式、角色、比值与目标。
    print(f'PASS contrast: {len(results)} text/control combinations')  # 仅所有组合成立才报告通过。

```

颜色计算已有了输入与判定，check_frontend 再把整次运行接起来：先 verify_fonts，再以 check=True 运行 ui-test；无截图请求或非macOS到此结束。需要图库时重新构建原生程序，建立 artifacts/frontend，移除旧的两份汇总，再检查本轮配色。中途失败不能留下旧成功JSON冒充本次结果。追加总入口的准备部分。

<!-- append: scripts/ui_preview.py -->
```python
def check_frontend(screenshots=True):  # 先跑可移植C行为检查，平台支持时再生成状态图库。
    verify_fonts()  # 检查和显示使用相同字库。
    subprocess.run(make_command(target='ui-test'), cwd=ROOT, check=True)  # C状态与绘图检查可在非macOS运行。
    if not screenshots or platform.system() != 'Darwin':  # 显式禁截图或平台无Cocoa时到此结束。
        return  # 不执行macOS快照和sips。
    subprocess.run(make_command(target='ui-preview'), cwd=ROOT, check=True)  # 重新构建实际截图程序。
    out = ROOT / 'artifacts/frontend'  # 保留本轮图像与JSON结果。
    out.mkdir(parents=True, exist_ok=True)  # 本轮报告和图片共享产物目录。
    for name in ('contrast.json', 'render-tests.json'):  # 两份汇总只能表示本轮完整完成的检查。
        (out / name).unlink(missing_ok=True)  # 本轮若中途失败，不保留旧成功汇总。
    check_contrast(out)  # 先检查实际语义颜色，再生成状态图供目视复核。
```

cases每项是(输出名,应用名,尺寸,服务状态,场景,页签)，如("editor-1280x800","editor","1280x800","normal","none",0)。七个应用乘三尺寸是21项，再加入四个System页签、三个Settings页签、八个列表状态和六个场景，共42项。外层遍历主题，内层解包六元组，两个模式各重跑42个独立进程，保留84张图。

下面只建立参数记录，尚未运行这些场景。元组每项的角色保持固定，后面的循环才把它们逐一传给原生程序。继续追加场景表。

<!-- append: scripts/ui_preview.py -->
```python
    cases = []  # 每项六元组依次为name/app/size/state/scenario/tab。
    for size in ('1024x768', '1280x800', '1600x900'):  # 三种实际布局尺寸。
        for app in APPS:  # 七个应用每种尺寸各生成一张。
            cases.append((f'{app}-{size}', app, size, 'normal', 'none', 0))  # 名称用于文件路径，其余项直接作为程序参数。
    for tab in range(1, 5):  # range上界不含5，补System的页签1..4。
        cases.append((f'system-tab{tab}', 'system', '1280x800', 'normal', 'none', tab))  # 页签0已经由普通应用快照覆盖。
    for tab in range(1, 4):  # 补Settings的页签1..3。
        cases.append((f'settings-tab{tab}', 'settings', '1280x800', 'normal', 'none', tab))  # 保存应用与页签之间的对应。
    for state in ('empty', 'loading', 'error', 'offline'):  # 服务状态分别影响空、等待、失败和离线画面。
        for app in ('files', 'tasks'):  # 这两种列表应用需要分别观察每种状态。
            cases.append((f'{app}-{state}', app, '1280x800', state, 'none', 0))  # 状态名传入真实程序状态分支。
    for scenario in ('dialog', 'dirty', 'workspace', 'save-error', 'system-tasks', 'controls'):  # 单独观察执行失败、未保存与两窗口工作区。
        cases.append((scenario, 'editor', '1280x800', 'normal', scenario, 0))  # 场景名称也成为输出文件名。
    results = []  # 只在快照和转换均成功后加入对应结果。
```

场景表已形成42项，现在外层选主题、内层解包这六项。输出名加主题前缀，PPM路径仍是 command 列表中的一个元素，即使目录有空格也不会拆成多项。原生快照和sips转换各限15秒，两者成功才加入结果；所有场景完成后才写 render-tests.json 并报告84张。继续追加循环与报告。

<!-- append: scripts/ui_preview.py -->
```python
    for theme in ('night', 'day'):  # 两个模式各自重跑相同42项场景。
        for name, app, size, state, scenario, tab in cases:  # 六元组解包成输出名与原生参数。
            name = theme + '-' + name    # 文件名明确标出模式，两个版本都保留。
            ppm = out / (name + '.ppm')  # 原生程序输出二进制PPM。
            png = out / (name + '.png')  # 转换后便于查看的图像路径。
            command = [  # 使用参数列表，文件名含空格仍是单个参数。
                str(ROOT / 'build/ui/airios-ui'), '--snapshot', str(ppm),  # 同一平台绘图程序直接生成快照后退出。
                '--app', app, '--size', size, '--state', state,  # 确定本次应用、画布和服务状态。
                '--scenario', scenario, '--tab', str(tab), '--theme', theme,  # 整数页签显式转文本交给CLI。
            ]
            subprocess.run(command, cwd=ROOT, check=True, timeout=15)  # 单张快照最多15秒，非零或超时都失败。
            subprocess.run(['sips', '-s', 'format', 'png', str(ppm), '--out', str(png)], check=True, stdout=subprocess.DEVNULL, timeout=15)  # 只隐藏转换正常输出，错误仍可见。
            results.append({'name': name, 'png': str(png.relative_to(ROOT)), 'passed': True})  # 保存可搬迁的项目相对路径。
    (out / 'render-tests.json').write_text(json.dumps(results, indent=2) + '\n')  # 所有结果写成JSON数组。
    print(f'PASS frontend renders: {len(results)} application/layout/state snapshots')  # 按真实完成数量报告双主题样本。
```

两份JSON分别记录本轮颜色比值与成功快照路径；生成文件和数值通过以后，仍要查看相同场景中的遮挡、截字和内容。原生图来自独立参考进程，guest 的显示与输入还要走下方独立命令，不能用主机图库替代。

**在学习目录根目录运行：**

<!-- run -->
```sh
PYTHONPATH=scripts python3 - <<'PY'  # 仅给本进程增加scripts模块路径，Shell不展开Python正文。
from ui_preview import check_frontend  # 先执行C状态测试，macOS再核对颜色并生成图片。
check_frontend()  # 其他平台只运行可移植检查，不能记成Cocoa图库已生成。
PY
```

macOS成功输出包含PASS contrast: 58 text/control combinations和PASS frontend renders: 84 application/layout/state snapshots。打开artifacts/frontend/night-workspace.png与day-workspace.png比较同一Editor、Terminal和资源栏，再比较两个save-error场景的错误文字。contrast.json和render-tests.json保存本次实际结果；窗口图不能证明guest已实现文件服务。

本节完成后运行`./airios test --graphics`，复用K12的等待器，在真实guest经PS/2切换两次主题并保存对应画面；`./airios test`仍只走串口主线。
