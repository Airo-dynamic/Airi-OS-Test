# G22 · 让按钮呈现悬停、按下与释放

窗口已经会响应按下与拖动，但移动到Save按钮时仍只有固定外观。现在把同一指针位置交给绘图，让按钮在悬停、按下和释放后呈现不同底面。Save动作仍由按下事件的输入分派执行。没有持续拖动时，phase=0只更新位置与下一帧反馈；已有窗口拖动或文本拖选时，它还会更新几何或选区，仍不会调用Save。绘图将这份状态变成像素。

<a id="feedback-events"></a>

ui_pointer保存屏幕坐标与pointer_down。按下或双击令pointer_down为true，释放先清false，再结束拖动；即使释放在按钮外，也不能保留旧按压。绘图无需重新猜鼠标状态，只借用这份会话记录。移动窗口和拖选文字同样会更新位置，后续帧看到的是本次事件的坐标。

在 ui.h 中定位 drag_x、drag_y 与 monitor 之间的旧窗口，加入 pointer_down。窗口拖动参数继续保留，新增布尔值只记录本次按压外观。

<a id="code-frontend-include-ui-h-1"></a>

<!-- replace: frontend/include/ui.h -->
<!-- before -->
```c
    int mouse_y;  // 当前鼠标屏幕纵坐标。
    int drag;     // 当前跨事件拖动种类，-1表示无。
    int drag_x;     // 移窗时按下点距窗口左边的偏移。
    int drag_y;     // 移窗时按下点距窗口顶部的偏移。
    bool monitor;   // 右侧状态监视区是否展开。
    bool launcher;  // 应用启动菜单是否展开。
```
<!-- after -->
```c
    int mouse_y;  // 当前鼠标屏幕纵坐标。
    int drag;     // 当前跨事件拖动种类，-1表示无。
    int drag_x;  // 移窗时按下点距窗口左边的偏移。
    int drag_y;  // 移窗时按下点距窗口顶部的偏移。
    bool pointer_down;  // 鼠标按压状态仅用于绘图反馈
    bool monitor;   // 右侧状态监视区是否展开。
    bool launcher;  // 应用启动菜单是否展开。
```

在 pointer.c 的函数开头，用新块替换旧窗口。先按 phase 更新 pointer_down，再保存坐标，随后继续原来的拖动、释放和动作分派；因此释放不依赖按钮是否仍在指针下面。

<a id="code-frontend-core-pointer-c-1"></a>

<!-- replace: frontend/core/pointer.c -->
<!-- before -->
```c
#include "../apps/internal.h"  // 借用Editor正文定位器，续拖不调用它的完整按钮分派。
void ui_pointer(struct ui_desktop *s, int x, int y, int phase) {  // phase=0移动、1按下、2释放、3双击；x/y 在桌面屏幕坐标中。
    s->mouse_x = x;  // 保存当前鼠标横坐标，绘图可以显示指针。
    s->mouse_y = y;  // 保存当前鼠标纵坐标。
```
<!-- after -->
```c
#include "../apps/internal.h"  // 借用Editor正文定位器，续拖不调用它的完整按钮分派。
void ui_pointer(struct ui_desktop *s, int x, int y, int phase) {  // phase=0移动、1按下、2释放、3双击；x/y 在桌面屏幕坐标中。
    if (phase == 1 || phase == 3)  // 按下或双击事件开始按压反馈
        s->pointer_down = true;  // 保存按压状态，下一帧按钮可显示U_DOWN
    if (phase == 2)  // 释放无论命中哪里都应结束按压
        s->pointer_down = false;  // 下一帧不再保留旧按压反馈
    s->mouse_x = x;  // 保存当前鼠标横坐标，绘图可以显示指针。
    s->mouse_y = y;  // 保存当前鼠标纵坐标。
```

## 按区域决定谁可以显示反馈

<a id="interaction-region"></a>

u_interaction保存本次绘制区域的鼠标位置、按压和响应许可。ui_draw开始绘桌面，遇普通窗口时重新设置许可：窗口必须是active，且没有对话框或启动菜单。底层窗口即使某个按钮与指针重合，也不能得到hover。

资源侧栏在窗口之后绘制，也重新取得桌面许可；active=-1不代表资源区不能操作。对话框最后绘制，并单独取得响应许可。

颜色变化仍由u_control处理，禁用位U_INACTIVE覆盖HOT/DOWN；选中、悬停和按压通过按位或组合，按压外观优先。

u_button把当前矩形与整屏指针比较，符合区域许可才加入U_HOT，pointer_down为true时再加入U_DOWN。它没有桌面、文档或后端服务参数，所以不能完成保存。输入入口负责动作，渲染器负责这一次状态的像素；二者使用同一画布坐标；控件反馈按可见按钮矩形，动作分派保留更宽的工具栏命中带。

坐标、按压和许可通过 u_interaction 的四个实参交接，调用时按值保存到当前绘图状态。下面更新声明与这几个连接，文字、圆角和颜色编码都沿前章已有路径；越过右界仍只跳过本行余下字节，显式换行继续回到 origin。

在 render.h 的 u_control 与 u_button 声明处定位旧窗口，加入 u_interaction 接口；其余绘图声明不重写。

<a id="code-frontend-include-render-h-1"></a>

<!-- replace: frontend/include/render.h -->
<!-- before -->
```c
uint32_t u_color(uint32_t);  // 角色解析成RGB，0至0xffffff的真实RGB原样通过
void u_round(struct ui_rect, int, uint32_t);  // 圆角边缘按4×4样本混合，仍遵守裁剪
void u_control(struct ui_rect, const char *, unsigned);   // 按调用者明确给出的状态绘制按钮，不执行动作
void u_button(struct ui_rect, const char *, bool, bool);  // 将selected与disabled转换为共同状态表
static inline struct ui_rect u_window_body(struct ui_rect r) {  // 由外框得到标题下方的应用矩形
```
<!-- after -->
```c
uint32_t u_color(uint32_t);  // 角色解析成RGB，0至0xffffff的真实RGB原样通过
void u_round(struct ui_rect, int, uint32_t);  // 圆角边缘按4×4样本混合，仍遵守裁剪
void u_control(struct ui_rect, const char *, unsigned);  // 按调用者明确给出的状态绘制按钮，不执行动作
void u_interaction(int, int, bool, bool);  // 本次绘制区域的指针位置、按压与响应许可
void u_button(struct ui_rect, const char *, bool, bool);  // 将selected与disabled转换为共同状态表
static inline struct ui_rect u_window_body(struct ui_rect r) {  // 由外框得到标题下方的应用矩形
```

依次更新 draw.c 的三个唯一旧窗口：为本次绘图保存输入状态，在 u_button 组合反馈位，再在文件末尾定义 u_interaction。旧块只用于定位；新块之外的字形、采样与像素写入保留。

<a id="code-frontend-render-draw-c-1"></a>

<!-- replace: frontend/render/draw.c -->
<!-- before -->
```c
static bool standard;  // 三个8位通道是否恰好组成0xRRGGBB
static struct ui_rect clip;  // 所有图元共同使用的当前可写范围
static unsigned theme;  // 当前绘图使用的调色板下标，单线程调用期间由u_theme设置
static const uint32_t palettes[2][U_COLOR_END - U_BG] = {  // 两行按ui_color角色顺序保存颜色
```
<!-- after -->
```c
static bool standard;  // 三个8位通道是否恰好组成0xRRGGBB
static struct ui_rect clip;  // 所有图元共同使用的当前可写范围
static int pointer_x, pointer_y;  // 当前绘制区域借用的整屏指针坐标
static bool pointer_down, interactive;  // 按压状态与当前区域是否有资格显示反馈
static unsigned theme;  // 当前绘图使用的调色板下标，单线程调用期间由u_theme设置
static const uint32_t palettes[2][U_COLOR_END - U_BG] = {  // 两行按ui_color角色顺序保存颜色
```

<!-- replace: frontend/render/draw.c -->
<!-- before -->
```c
    if (disabled)  // 调用者明确报告此动作不可用
        state |= U_INACTIVE;  // 按位或加入禁用位，保留已有选择位
    u_control(r, label, state);  // 当前只绘制明确状态，不读取鼠标或执行动作
}

```
<!-- after -->
```c
    if (disabled)  // 调用者明确报告此动作不可用
        state |= U_INACTIVE;  // 按位或加入禁用位，保留已有选择位
    if (interactive && u_inside(r, pointer_x, pointer_y)) {  // 只有允许响应且指针在按钮半开矩形内时加入反馈
        state |= U_HOT;    // 按位或加入悬停位，不改变已有选择或禁用位
        if (pointer_down)  // 指针在此按钮内且鼠标仍按着
            state |= U_DOWN;  // 本帧加入按压反馈，服务动作仍由输入分派执行
    }
    u_control(r, label, state);  // 只把组合状态转换成像素，不调用任何应用或服务
}

```

<!-- replace: frontend/render/draw.c -->
<!-- before -->
```c
    u_rect(x + 7, y + 13, 10, 3, U_PINK);  // 粉色横梁只承担产品身份
}
```
<!-- after -->
```c
    u_rect(x + 7, y + 13, 10, 3, U_PINK);  // 粉色横梁只承担产品身份
}

void u_interaction(int x, int y, bool down, bool enabled) {  // 每个绘制区域显式设置它可以响应的指针状态
    pointer_x = x;  // 下一次按钮绘制用这个整屏横坐标命中
    pointer_y = y;  // 纵坐标与按钮矩形采用同一整屏坐标系
    pointer_down = down;    // 当前按压由事件入口保存，绘图不产生按下事件
    interactive = enabled;  // 底层窗口或被对话框遮挡的区域关闭反馈
}
```

控件已经会读取许可，还需要由桌面逐区提供。开始绘桌面时允许全局控件反馈，进入普通窗口时按 active 和覆盖层收窄许可，资源侧栏与对话框再分别设置自己的许可。这样最后一个窗口的局部状态不会被资源按钮沿用。

在 paint.c 的下列旧窗口逐区接入许可：整帧开始、每个窗口身份片之后、资源侧栏进入处、对话框表面形成后。按这些调用的实际次序，后续按钮总是使用当前区域的许可，不沿用上一窗口。

<a id="code-frontend-core-paint-c-1"></a>

<!-- replace: frontend/core/paint.c -->
<!-- before -->
```c
    u_theme(s->theme);  // 按会话模式选整套调色板，颜色角色与业务状态分离
    u_rect(0, 0, s->width, s->height, U_BG);  // 用画布背景色写矩形，起点(0,0)，尺寸s->width×s->height像素
    u_text(86, s->height - 117, "AiriOS", U_TITLE, U_MUTED);  // 空桌面仅保留轻量产品标识
    u_rect(0, 0, s->width, UI_TOP, U_PANEL);  // 系统顶栏先铺满36像素高度，身份片随后覆盖左端
```
<!-- after -->
```c
    u_theme(s->theme);  // 按会话模式选整套调色板，颜色角色与业务状态分离
    u_rect(0, 0, s->width, s->height, U_BG);  // 用画布背景色写矩形，起点(0,0)，尺寸s->width×s->height像素
    u_interaction(s->mouse_x, s->mouse_y, s->pointer_down, !s->dialog);  // 桌面区的指针反馈
    u_text(86, s->height - 117, "AiriOS", U_TITLE, U_MUTED);  // 空桌面仅保留轻量产品标识
    u_rect(0, 0, s->width, UI_TOP, U_PANEL);  // 系统顶栏先铺满36像素高度，身份片随后覆盖左端
```

<!-- replace: frontend/core/paint.c -->
<!-- before -->
```c
            plate_width = available;  // 不把身份片铺进最小化、最大化和关闭按钮区。
        u_plate((struct ui_rect){r.x + 8, r.y + 7, plate_width, UI_TITLE_H - 14}, U_CHROME);  // 用实际字体推进量承载完整应用身份。
        u_icon(r.x + 18, r.y + 14, (int)app, U_INK);   // 应用图标处于独立身份栏
        u_rect(r.x + 53, r.y + 12, 1, 24, U_CONTROL);  // 图标与标题使用一致的分隔尺度
```
<!-- after -->
```c
            plate_width = available;  // 不把身份片铺进最小化、最大化和关闭按钮区。
        u_plate((struct ui_rect){r.x + 8, r.y + 7, plate_width, UI_TITLE_H - 14}, U_CHROME);  // 用实际字体推进量承载完整应用身份。
        u_interaction(s->mouse_x, s->mouse_y, s->pointer_down, focused && !s->dialog && !s->launcher);  // 活动窗口且无 dialog/launcher 时才允许悬停反馈
        u_icon(r.x + 18, r.y + 14, (int)app, U_INK);   // 应用图标处于独立身份栏
        u_rect(r.x + 53, r.y + 12, 1, 24, U_CONTROL);  // 图标与标题使用一致的分隔尺度
```

<!-- replace: frontend/core/paint.c -->
<!-- before -->
```c
        u_line(r.x + r.w - 10, r.y + r.h - 4, r.x + r.w - 4, r.y + r.h - 10, U_EDGE);  // 用分隔线色连接像素端点(r.x + r.w - 10,r.y + r.h - 4)到(r.x + r.w - 4,r.y + r.h - 10)
    }
    if (right)  // 资源栏占用宽度非零时才绘制它，本轮窗口已为其留空间
        monitor(s);  // 资源栏启用时读取同次统计，显示当前可用数据或未采集状态
    if (s->mouse_x < UI_DOCK && s->mouse_y >= 50) {  // 鼠标在应用侧栏区域时再按行距换算应用id
        int app = (s->mouse_y - 50) / 54;      // 先去侧栏顶边50像素再除行距54，得到候选应用编号
```
<!-- after -->
```c
        u_line(r.x + r.w - 10, r.y + r.h - 4, r.x + r.w - 4, r.y + r.h - 10, U_EDGE);  // 用分隔线色连接像素端点(r.x + r.w - 10,r.y + r.h - 4)到(r.x + r.w - 4,r.y + r.h - 10)
    }
    if (right) {  // 资源区是桌面的一部分，不继承刚绘完窗口的hover许可。
        u_interaction(s->mouse_x, s->mouse_y, s->pointer_down, !s->dialog);  // 无模态框时资源按钮独立允许响应，active可为-1。
        monitor(s);  // 按同次资源快照绘图，按钮随后使用这份桌面许可。
    }
    if (s->mouse_x < UI_DOCK && s->mouse_y >= 50) {  // 鼠标在应用侧栏区域时再按行距换算应用id
        int app = (s->mouse_y - 50) / 54;      // 先去侧栏顶边50像素再除行距54，得到候选应用编号
```

<!-- replace: frontend/core/paint.c -->
<!-- before -->
```c
        u_round((struct ui_rect){d.x + 1, d.y + 1, d.w - 2, d.h - 2}, 1, U_SURFACE);  // 中性内容表面
        if (s->dialog == UI_DIALOG_ERROR) u_rect(d.x + 1, d.y + 1, 3, d.h - 2, U_RED);  // 错误沿左缘标明，正文仍完整可读
        u_rect(d.x + 24, d.y + 49, d.w - 48, 1, U_EDGE);  // 标题与说明之间轻分隔
        u_text(d.x + 24, d.y + 20, s->dialog_title, U_LABEL, s->dialog == UI_DIALOG_ERROR ? U_RED : U_TEXT);  // 在整屏x=d.x + 24、整屏y=d.y + 20显示对话框标题
```
<!-- after -->
```c
        u_round((struct ui_rect){d.x + 1, d.y + 1, d.w - 2, d.h - 2}, 1, U_SURFACE);  // 中性内容表面
        if (s->dialog == UI_DIALOG_ERROR) u_rect(d.x + 1, d.y + 1, 3, d.h - 2, U_RED);  // 错误沿左缘标明，正文仍完整可读
        u_interaction(s->mouse_x, s->mouse_y, s->pointer_down, true);  // 弹窗是唯一可操作层
        u_rect(d.x + 24, d.y + 49, d.w - 48, 1, U_EDGE);  // 标题与说明之间轻分隔
        u_text(d.x + 24, d.y + 20, s->dialog_title, U_LABEL, s->dialog == UI_DIALOG_ERROR ? U_RED : U_TEXT);  // 在整屏x=d.x + 24、整屏y=d.y + 20显示对话框标题
```

## 用同一文档观察各个指针状态

<a id="feedback-observation"></a>

下面保留前两章打开main.lua、拖选、移动和缩放的全部操作。新增观察先固定Editor为最大化，用draw取得实际body，再移动到Save按钮内部。normal/hot/down取自同一个按钮底面像素，避开文字和圆角；每次状态改变后都重新绘帧。为了只观察按压外观而不真正保存，程序直接设置pointer_down，随后用phase=2验证真实释放路径。

直接控件观察使用相同矩形，分别关闭和开启区域许可；禁用按钮在鼠标移入、按下和移出时保持同一种底面。最后将Settings覆盖到Editor上方，移动到原Save位置。普通移动必须保留Editor正文，active也继续是Settings。真实点击、服务保存与绘图反馈各自留下可比较的状态。持续拖选仍只调用G20的正文定位器，即使移到Save也不会送出保存动作。

这组观察随后把同一Editor设为800×400普通外框，body宽798所以没有项目栏。将正文替换为print(42)加换行后，在第一行第1间隙按下，再移到Save按钮内部并释放。定位器把上方位置夹到正文第0间隙，anchor仍是1；dirty 保持 true，editor 和 saved 各自与操作前的独立副本逐字节相同，服务 read 也读回旧 saved。这样既确认第一批文字列可以起拖，也确认续拖没有调用保存按钮。

观察程序沿用已有文件打开、文本拖选、移窗和缩放。下面依次更新 include、插入资源区观察 helper，再在 main 的移窗结果之后加入反馈观察及 helper 调用；各处都有唯一旧窗口，已有 observe_text_drag 和原操作序列保留。

<a id="code-labs-tutorial-pointer-c-1"></a>

<!-- replace: labs/tutorial/pointer.c -->
<!-- before -->
```c
#include <string.h>  // 主机观察用独立数组复制与比较检查选择没有触发保存。
#include "ui.h"      // 取得共享桌面状态和输入事件接口。
#include <stdio.h>   // 取得主机 printf、文件输出与错误报告接口。
static struct ui_desktop desktop;      // 静态存储整份桌面；事件回调结束后状态仍保留。
```
<!-- after -->
```c
#include "ui.h"      // 取得共享桌面状态和输入事件接口。
#include <string.h>  // 用主机memcmp核对独立文档副本
#include <stdio.h>   // 取得主机 printf、文件输出与错误报告接口。
static struct ui_desktop desktop;      // 静态存储整份桌面；事件回调结束后状态仍保留。
```

<!-- replace: labs/tutorial/pointer.c -->
<!-- before -->
```c
    puts("selection: body798 starts at column1; drag over Save preserves text, saved and anchor");  // 报告这组真实事件的结果。
    return true;  // 起拖、越界、服务回读与释放均符合当前选择规则。
}

```
<!-- after -->
```c
    puts("selection: body798 starts at column1; drag over Save preserves text, saved and anchor");  // 报告这组真实事件的结果。
    return true;  // 起拖、越界、服务回读与释放均符合当前选择规则。
}

static bool observe_monitor_feedback(uint32_t *pixels, const struct airi_framebuffer *fb) {  // 观察资源区从窗口反馈状态中独立出来。
    ui_open(&desktop, UI_EDITOR);    // 保留一个后方窗口，避免空桌面绕过窗口绘制路径。
    ui_open(&desktop, UI_SETTINGS);  // Settings成为当前活动窗口，再通过真实关闭事件离开。
    desktop.monitor = true;  // 本观察必须绘制右侧资源区。
    ui_draw(&desktop, fb);   // 取得当前Settings实际外框，关闭坐标不能猜初始布局。
    struct ui_rect r = desktop.windows[UI_SETTINGS].rect;  // 读取同一帧回写的当前外框。
    ui_pointer(&desktop, r.x + r.w - 20, r.y + 20, 1);     // 点标题关闭，Settings没有文档保存确认。
    ui_pointer(&desktop, r.x + r.w - 20, r.y + 20, 2);     // 释放清按压与拖动，随后只观察hover。
    if (desktop.active != -1 || !desktop.windows[UI_EDITOR].open || desktop.windows[UI_SETTINGS].open)  // 无活动窗口，但后方Editor仍存在。
        return false;  // 尚未形成会暴露区域许可遗留的状态。
    ui_pointer(&desktop, 200, 200, 0);  // 先让鼠标离开资源按钮，保持无活动窗口。
    ui_draw(&desktop, fb);  // 后方Editor关闭自己的hover许可，资源区必须重新建立自己的许可。
    int left = desktop.width - UI_MONITOR_W + 24, top = desktop.height - 96;  // 资源按钮和monitor绘图使用同一内边距。
    unsigned at = (unsigned)(top + 3) * 1280 + (unsigned)(left + 3);  // 按钮底面取样避开文字和两像素圆角。
    uint32_t normal = pixels[at];  // 保存鼠标远离时本帧的实际底面颜色。
    ui_pointer(&desktop, left + 20, top + 16, 0);  // 普通移动进入资源按钮，不应打开任务窗口。
    ui_draw(&desktop, fb);  // 资源区重新开放许可，此像素应进入悬停色。
    if (desktop.active != -1 || pixels[at] == normal)  // 既检查外观变化，也检查移动没有执行动作。
        return false;  // 沿用底层窗口的false许可会使底面一直不变。
    ui_pointer(&desktop, left + 20, top + 16, 1);  // 真正按下才进入资源按钮动作分派。
    ui_pointer(&desktop, left + 20, top + 16, 2);  // 本次动作后仍完整结束按压。
    if (desktop.active != UI_TASKS_APP)  // 点击必须打开Tasks，不能只拥有悬停像素。
        return false;  // 绘图反馈与动作入口需分别满足对应结果。
    puts("monitor: no active window still permits hover and opens Tasks on click");  // 报告关闭、移动、按下的同一实际事件序列。
    return true;  // 资源区不继承底层窗口的反馈许可。
}

```

<!-- replace: labs/tutorial/pointer.c -->
<!-- before -->
```c
        return 1;  // 有一项不符便停止，不能只以窗口仍存在判为成功
    puts("window moved by (30,25), resized by (24,19), released");  // 输出这组实际三事件操作留下的结果
    if (desktop.editor_id != 1 || !desktop.selection || desktop.cursor == anchor)  // 先确认此前文件与窗口观察，再改变外框测试临界宽度。
        return 1;  // 原来的选择尚未建立，不能用后一组操作替代其结果。
```
<!-- after -->
```c
        return 1;  // 有一项不符便停止，不能只以窗口仍存在判为成功
    puts("window moved by (30,25), resized by (24,19), released");  // 输出这组实际三事件操作留下的结果
    static uint32_t pixels[1280 * 800];  // 本主机观察拥有像素，绘图仅借用它
    struct airi_framebuffer fb = {  // 画布使用已建立的32位RGB合同
        .virtual_address = (uintptr_t)pixels, .width = 1280, .height = 800,  // 地址指向本进程数组，尺寸以像素计
        .pitch = 1280 * 4, .bpp = 32, .red_size = 8, .red_shift = 16,  // 每行5120字节，红通道在16至23位
        .green_size = 8, .green_shift = 8, .blue_size = 8  // 绿通道8至15位，蓝通道0至7位
    };
    ui_open(&desktop, UI_EDITOR);  // 让Editor成为最前可操作窗口
    desktop.windows[UI_EDITOR].maximized = true;  // 固定观察几何，首次draw更新实际rect
    ui_pointer(&desktop, 1000, 600, 0);  // 指针先远离工具栏，普通移动不触发动作
    ui_draw(&desktop, &fb);  // 把实际布局回写并生成初始像素
    struct ui_rect body = u_window_body(desktop.windows[UI_EDITOR].rect);  // 命中使用绘图刚算出的内容矩形
    int bx = body.x + 32, by = body.y + 16;  // Save按钮内部远离边缘、文字和底线的采样像素
    int button_x = body.x + 12, button_y = body.y + 8;  // 同一Save按钮的左上位置供采样定位
    unsigned at = (unsigned)(button_y + 3) * 1280 + (unsigned)(button_x + 3);  // 用一行1280像素把二维点转换为数组下标
    uint32_t normal = pixels[at];  // 记录指针远离时的按钮底面实际颜色
    char original[UI_DOC_SIZE];    // 独立保存移动前正文，避免指针别名掩盖变化
    memcpy(original, desktop.editor, sizeof(original));  // 后续hover不得修改这些字节
    ui_pointer(&desktop, bx, by, 0);  // 仅把位置移到Save按钮，仍不产生点击
    ui_draw(&desktop, &fb);     // 新帧加入U_HOT，只改变绘图状态
    uint32_t hot = pixels[at];  // 此像素位于按钮底面，悬停应与normal不同
    if (normal == hot || memcmp(original, desktop.editor, sizeof(original)))  // 同时检查反馈存在、正文保留
        return 1;  // 只改了位置却误改正文或没有反馈都应失败
    desktop.pointer_down = true;  // 单独演示绘图按压值，不发送会真正保存的phase1事件
    ui_draw(&desktop, &fb);      // 当前按钮同时带U_HOT和U_DOWN，按压优先
    uint32_t down = pixels[at];  // 取得按压底面，后续与hover比较
    if (hot == down)  // 按压应产生独立于悬停的底面
        return 1;  // 当前绘图没有呈现按压状态
    ui_pointer(&desktop, bx, by, 2);  // 真正释放事件清pointer_down，但位置仍在Save内部
    ui_draw(&desktop, &fb);  // 释放后恢复悬停底面
    if (desktop.pointer_down || pixels[at] != hot)  // 不能保留按压；hover仍应有效
        return 1;  // 释放路径没有清掉原来的按压反馈
    struct ui_rect sample = {4, 4, 90, 30};  // 直接控件观察位于整屏角落，与工作区内容分开
    u_begin(&fb);  // 借用同一数组并恢复整屏裁剪
    u_theme(UI_NIGHT);  // 将直接控件观察固定为夜间，使颜色比较可重复
    u_interaction(8, 8, false, false);  // 下层区域没有响应资格，即使指针在按钮内也无悬停
    u_button(sample, "Test", false, false);   // 普通启用按钮只绘制明确的normal状态
    uint32_t blocked = pixels[8 * 1280 + 8];  // 读取区域禁止响应时的同一点
    u_interaction(8, 8, false, true);  // 相同位置开放响应，只改变当前绘制区域许可
    u_button(sample, "Test", false, false);  // 同一按钮现在加入U_HOT
    if (blocked == pixels[8 * 1280 + 8])     // 区域许可必须确实影响视觉反馈
        return 1;  // 没有区分可响应和被遮挡区域
    u_interaction(8, 8, true, true);  // 指针既在按钮内又按着，先形成最强输入状态
    u_button(sample, "Test", false, true);     // disabled仍覆盖hover/down，只显示禁用外观
    uint32_t disabled = pixels[8 * 1280 + 8];  // 保存禁用按钮这一点的颜色
    u_interaction(1000, 600, false, true);     // 移出按钮并解除按压，禁用身份保持
    u_button(sample, "Test", false, true);     // 禁用按钮不因位置或按压变化而改变底面
    if (pixels[8 * 1280 + 8] != disabled)      // 两种指针输入下的禁用底面应一致
        return 1;  // 禁用控件仍错误显示可操作反馈
    ui_open(&desktop, UI_SETTINGS);  // Settings置顶，原Editor成为不能响应按钮的底层窗口
    struct ui_rect overlap = desktop.windows[UI_EDITOR].rect;  // 将上层几何暂设为同样位置，明确构成遮挡
    desktop.windows[UI_SETTINGS].rect = overlap;    // 仅观察遮挡，不改变文档数组
    desktop.windows[UI_SETTINGS].maximized = true;  // 相同外框将在下一帧得到同样布局
    ui_pointer(&desktop, bx, by, 0);  // 位于旧Save屏幕点的普通移动，不能调用Editor保存
    ui_draw(&desktop, &fb);  // 窗口绘图依层次决定谁有响应许可并覆盖下层像素
    if (desktop.active != UI_SETTINGS || memcmp(original, desktop.editor, sizeof(original)))  // hover不得置顶Editor或修改它的正文
        return 1;  // 被遮挡内容在普通移动中产生了动作
    puts("hover/down/up, disabled and occluded feedback observed; hover changed no document");  // 输出各输入状态的实际观察结果
    if (!observe_monitor_feedback(pixels, &fb))  // 再关闭活动窗口，验证资源区不继承后方Editor的禁止反馈。
        return 1;  // 资源hover或真实打开Tasks任一失败都不能结束为成功。
    if (desktop.editor_id != 1 || !desktop.selection || desktop.cursor == anchor)  // 先确认此前文件与窗口观察，再改变外框测试临界宽度。
        return 1;  // 原来的选择尚未建立，不能用后一组操作替代其结果。
```

**在学习目录根目录运行：**

<!-- run -->
```sh
mkdir -p build  # 保存本次主机程序和图像，已有目录可以继续
cc -std=c11 -O0 -g -Wall -Wextra -Werror -Iinclude -Ifrontend/include labs/tutorial/pointer.c kernel/lib/base.c kernel/boot/info.c kernel/console/console.c frontend/core/*.c frontend/apps/*.c frontend/backends/reference.c frontend/render/draw.c -o build/pointer  # Shell展开两个通配符，把当前已经写出的核心和应用一起链接
./build/pointer  # 运行当前完整观察程序，退出非零表示本组观察失败
```

输出先确认文档与窗口操作，再报告`hover/down/up, disabled and occluded feedback observed; hover changed no document`。像素数组由这个观察进程拥有，渲染器只在调用期间借用；进程退出后数组结束。接下来将同一事件接口接入Cocoa窗口，真实平台只需提供同样的整屏坐标与phase。

选择边界观察还输出`selection: body798 starts at column1; drag over Save preserves text, saved and anchor`，对应本章完整程序中实际发送的按下、越界移动与释放。

资源观察发送 Settings 标题关闭的按下和释放，使 active=-1 而 Editor 仍打开，再让指针从资源按钮外移入。Editor 此时关闭自己的反馈许可，资源区必须重新设置桌面许可；所以普通移动改变按钮底面却不打开窗口，最后真正发送按下才打开 Tasks。输出仍为 monitor: no active window still permits hover and opens Tasks on click。
