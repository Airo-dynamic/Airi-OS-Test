#include <airi/kernel.h>   // 取得启动信息、基础库与串口日志接口。
#include <airi/console.h>  // 取得命令环、按键与视图类型。
#include "ui.h"  // 取得共享桌面状态和输入事件接口。
#ifndef AIRI_PANEL_TITLE  // 允许编译选项覆盖面板标题，未提供时使用下面默认值。
#define AIRI_PANEL_TITLE "Local console"  // 默认标题强调当前显示内核本地命令信息。
#endif  // 结束默认面板标题的条件定义
/* Same frontend as the native reference. No fixture provider is linked here. */
static struct ui_desktop desktop;  // 静态存储整份桌面；事件回调结束后状态仍保留。
static bool initialized;  // 静态初值为false，随后结合对象地址与扩展绑定判断是否沿用桌面。
bool airi_console_draw(struct airi_console *c) {  // c 借用当前命令对象，返回值说明此帧是否实际绘制。
    const struct airi_framebuffer *fb = &c->boot->framebuffer;  // 从命令对象的启动描述借用真实帧缓冲参数。
    if (!airi_framebuffer_valid(fb) || fb->width < 640 || fb->height < 480 || fb->width > 8192 ||  // 先核对地址/位布局，再限制尺寸；任一不适用都放弃图形。
        fb->height > 8192)  // 高度也必须处于当前绘图支持范围。
        return false;  // 本次未完成图形输出，调用者仍可继续串口启动路径。
    if (!initialized || desktop.console != c || c->extension_context != &desktop) {  // 首次、换对象或重置后绑定失效时建立新会话。
        struct ui_backend backend = {.mode = UI_LIVE, .boot = c->boot, .label = AIRI_PANEL_TITLE};  // 构造 live 表，仅提供真实 boot；未指定服务回调自动为 NULL。
        ui_init(&desktop, backend, c);  // 初始化桌面并在可写会话中安装命令消费者，文本与启动数据继续由原平台拥有。
        initialized = true;  // 当前console已绑定桌面，绑定未变时后续帧复用窗口状态。
    }
    desktop.console = c;  // 保存当前长期有效的命令对象地址；新对象已在上面完成扩展绑定。
    desktop.backend.boot = c->boot;     // 保证设备和内存视图读取当前这份启动描述。
    if (c->view == AIRI_VIEW_TERMINAL)  // 终端页面映射到 Terminal 应用。
        ui_open(&desktop, UI_TERMINAL);  // 终端打开并置顶。
    else {  // 内存、设备或日志页面交给 System 的对应页签
        ui_open(&desktop, UI_SYSTEM);  // 内存、设备和日志共用System窗口，只改变页签。
        desktop.windows[UI_SYSTEM].tab = c->view == AIRI_VIEW_MEMORY    ? 1  // 内存命令视图对应 System 页签1。
                                         : c->view == AIRI_VIEW_DEVICES ? 3  // 设备视图对应页签3，分支继续检查下一个条件。
                                                                        : 4;  // 其他内核视图落到日志页签4。
    }
    desktop.windows[UI_TERMINAL].maximized = true;  // 终端采用整个工作区展示。
    desktop.windows[UI_SYSTEM].maximized = true;    // 系统信息也采用同一最大化布局。
    desktop.windows[UI_SYSTEM].scroll = 0;  // 每次命令视图绘制从当前页的首行开始。
    ui_draw(&desktop, fb);  // 在真实内核映射的帧缓冲内写入这一帧像素。
    return true;  // 只有绘图调用完成后才报告图形成功。
}
bool airi_dashboard_draw(const AiriBootInfo *info) {  // 主机边界观察借用启动描述绘制初始仪表台。
    static struct airi_console console;  // 仪表台会话有静态寿命，桌面保存的地址在函数返回后仍有效。
    airi_console_init(&console, info);   // 重建仪表台的初始文本状态，本次绘图借用调用者的info。
    return airi_console_draw(&console);  // 同步绘完再返回，后续真实输入若换用另一console会重新建立桌面。
}
