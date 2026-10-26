# G24 · 把真实启动与命令状态交给同一绘图实现

主机窗口已能操作参考文件，共享绘图把 desktop 画进平台分配的 pixels 数组。现在让 QEMU 中的内核调用同一套 C 绘图，把这次启动信息和命令会话画进设备帧缓冲。这里共享的是源码：主机和 guest 分别编译、分别运行，没有共同的 desktop，也不会把主机参考文件带进 guest。

起点要同时具有 G23 的共享绘图和 K13 的串口命令循环。K12、K13 可以在前面任一图形停点之前或之后完成，到本章再组合两条路线。K11 已让内核入口调用 airi_console_draw，默认链接只返回 false 的串口实现；本章写出另一份同名图形实现，由 desktop- 配置选择它。两份定义分别进入各自目标。

<a id="guest-presenter"></a>

## 从串口会话进入设备绘图

K13 的循环仍从 UART 和 PS/2 收集输入，命令解释器仍修改原来的 console。入口把这份 console 传给 airi_console_draw；适配层把 console.view 映射成 Terminal 或 System 页签，再交给 ui_draw。输入与绘图都在内核自己的执行流中调用，当前没有用户态桌面或进程间通信。

设备描述来自 c->boot->framebuffer，包含已映射地址、像素宽高、字节行距和通道布局。先验证这份描述及当前支持的 640..8192、480..8192 尺寸范围；失败立即返回 false，本次不初始化桌面，也不安装新的主题回调，入口仍可继续串口路径。

新建 kernel/console/display.c，先写静态状态和设备检查。desktop 要跨过一次绘图调用继续存在；它保存长期 console 的地址，不能作为这个函数内的临时对象。

<a id="code-kernel-console-display-c-1"></a>

<!-- write: kernel/console/display.c -->
```c
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
```

<a id="guest-services"></a>

设备可用以后，才决定沿用桌面还是建立新会话。首次调用 initialized 为 false，要用 live backend 初始化；它只借用真实 BootInfo，没有 files、read、action 或任务回调。因此 Terminal 与 System 读取实际会话和启动区域，Files、Tasks 会说明服务不可用，不能显示主机的参考记录。

后续调用不能只检查 initialized。若 console 地址换了，desktop 不能继续引用旧会话；即使地址没变，airi_console_init 也可能已经重置它，把 extension_context 清成 NULL。于是还要核对 c->extension_context 是否仍为 &desktop。对象或绑定改变时重新 ui_init，安装这次会话的主题命令扩展；两者都未变才保留窗口和主题。随后把 console 和 boot 的借用地址更新成当前值。

继续追加同一个 airi_console_draw，完成会话绑定。

<!-- append: kernel/console/display.c -->
```c
    if (!initialized || desktop.console != c || c->extension_context != &desktop) {  // 首次、换对象或重置后绑定失效时建立新会话。
        struct ui_backend backend = {.mode = UI_LIVE, .boot = c->boot, .label = AIRI_PANEL_TITLE};  // 构造 live 表，仅提供真实 boot；未指定服务回调自动为 NULL。
        ui_init(&desktop, backend, c);  // 初始化桌面并在可写会话中安装命令消费者，文本与启动数据继续由原平台拥有。
        initialized = true;  // 当前console已绑定桌面，绑定未变时后续帧复用窗口状态。
    }
    desktop.console = c;  // 保存当前长期有效的命令对象地址；新对象已在上面完成扩展绑定。
    desktop.backend.boot = c->boot;     // 保证设备和内存视图读取当前这份启动描述。
```

<a id="frame-completion"></a>

绑定完成后，当前 console 的 view 决定这一帧展示什么。Terminal 视图打开并置顶 Terminal；内存、设备、日志分别打开 System 的页签 1、3、4。两种窗口都设为最大化，System 的窗口滚动回到页首；内存命令的 map_page 仍由 console 保存。

ui_draw 直接借用刚验证的设备帧缓冲，G11 的图元按 pitch 找行首、按设备通道编码颜色，没有另建主机 RGB 数组。绘图调用返回以后，本函数才返回 true；入口拿到这个实际结果再发布 READY 的 graphics=1。这里只说明这一轮绘图调用已经完成，缺少设备或尺寸不适用时仍是 graphics=0。继续追加到函数结束。

<!-- append: kernel/console/display.c -->
```c
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
```

还有一个借用启动描述画初始仪表台的入口 airi_dashboard_draw。它内部也准备 console，并同步交给同一个绘图函数。这个 console 必须是静态对象：ui_init 会把地址保存在 desktop 中，函数返回后地址仍须有效。以后真实输入循环交来另一份 console，前面的绑定检查会重建桌面并重新安装回调。

继续追加仪表台入口。它只负责初始画面，后续命令仍由真实输入会话更新。

<!-- append: kernel/console/display.c -->
```c
bool airi_dashboard_draw(const AiriBootInfo *info) {  // 主机边界观察借用启动描述绘制初始仪表台。
    static struct airi_console console;  // 仪表台会话有静态寿命，桌面保存的地址在函数返回后仍有效。
    airi_console_init(&console, info);   // 重建仪表台的初始文本状态，本次绘图借用调用者的info。
    return airi_console_draw(&console);  // 同步绘完再返回，后续真实输入若换用另一console会重新建立桌面。
}
```

## 选择图形对象和已经生成的字体

Make 的 `desktop-` 配置将下面清单加入图形目标，串口目标继续使用自己的源清单。`UI_SOURCES :=` 立即保存一行共享 C 路径，供对象规则逐个编译；这些文件均已在 G11–G23 写出。清单不包含 reference provider 和 Cocoa，因此 guest 没有主机示例服务或原生窗口依赖。新建 `frontend/sources.mk`。

<!-- write: frontend/sources.mk -->
```make
# 显式选择图形时才包含；列出共享 C 模型与绘图。
UI_SOURCES := frontend/render/draw.c frontend/core/state.c frontend/core/paint.c frontend/core/commands.c frontend/core/pointer.c frontend/core/editing.c frontend/apps/common.c frontend/apps/documents.c frontend/apps/series.c frontend/apps/builtin.c frontend/apps/terminal.c frontend/apps/files.c frontend/apps/editor.c frontend/apps/tasks.c frontend/apps/system.c frontend/apps/settings.c frontend/apps/viewer.c
```

设备绘图仍读取 G12 已生成的八套字体，不在内核运行 Pillow。新增 `verify_fonts`，供 K13 的统一命令入口在显式选择图形时调用：`fonts.lock.json` 的 `assets` 每项给文件名和预期摘要，脚本据此读取 `frontend/assets` 下的实际字体及许可字节；`config/deps.lock.json` 的 `frontend_fonts` 再给生成图集的预期摘要。两层都相符，统一入口才继续使用现成的 `font-atlas.h` 构建；缺文件或摘要不符便抛异常并停止。新建 `scripts/graphics_assets.py`。

<!-- write: scripts/graphics_assets.py -->
```python
"""Validate explicitly selected graphics assets using only the Python standard library."""  # 不参与默认串口路径。
import hashlib  # 比较实际字节和锁定摘要。
import json     # 读取已提供的字体身份清单。
from tools import ROOT  # 图形目标与内核使用同一工程根。


def verify_fonts():  # 仅显式选择图形时检查，不导入字体生成器或Cocoa。
    manifest = json.loads((ROOT / 'frontend/assets/fonts.lock.json').read_text())  # 每项含文件名与SHA-256。
    for item in manifest['assets']:  # 遍历字体资源，不扫描任意用户文件。
        actual = hashlib.sha256((ROOT / 'frontend/assets' / item['file']).read_bytes()).hexdigest()  # 摘要来自实际资产。
        if actual != item['sha256']:  # 字节身份不同则拒绝使用。
            raise RuntimeError(f'Font asset checksum mismatch: {item["file"]}')  # 报告具体字体。
    lock = json.loads((ROOT / 'config/deps.lock.json').read_text())['frontend_fonts']  # 总锁记录生成结果身份。
    atlas = ROOT / 'frontend/assets/font-atlas.h'  # 图集由G12配方生成，不手敲数组。
    if hashlib.sha256(atlas.read_bytes()).hexdigest() != lock['atlas_sha256']:  # 同时核对产物而不重新生成。
        raise RuntimeError('Generated font atlas checksum mismatch; rebuild it deliberately')  # 依赖更新必须明确进行。
```

## 启动本轮 guest 并等待首帧完成

三个文件已经接齐显示定义、共享编译路径和资产验证。先在当前学习目录执行 `./airios build --graphics`；K13 的统一入口会先调用 `verify_fonts`，再准备构建材料并选择 `desktop-normal`，验证和构建都通过后再继续。

下面演示直接调用构建与首帧检查助手的入口。它不经过管理入口，也不会再次验证字体，因此沿用刚才已经验证、此后未改变的字体、许可和图集字节；这些文件或其锁定摘要有变化时，先重新运行上面的统一构建命令。

`make_command` 的 `desktop-normal` 将图形产物与串口对象分目录保存，`subprocess.run` 的 `check=True` 要求本次构建成功。随后 `check` 启动新的 QEMU，等待本轮完整 READY 并核对 `graphics=1`，才截取设备画面。内核输入循环继续使用原来的 console，不复制进前端。

<!-- run -->
```sh
PYTHONPATH=scripts python3 - <<'PY'  # 从当前工程导入模块，Shell不展开Python正文。
import subprocess  # 构建显式图形配置。
from tools import make_command  # 根 Makefile仍属于内核主线。
from boot import check  # 检查这一轮真实 guest 的完成记录。
subprocess.run(make_command('desktop-normal'), check=True)  # 与串口对象放在不同目录。
check('desktop-normal')  # graphics=1 后才保存实际画面。
PY
```

接着可运行 `./airios run --graphics`；这条统一入口同样先验证资产，再启动可交互的 guest，用 F1–F4 和 theme day/theme night 操作真实信息。退出后，默认 `./airios run` 仍是串口版本。窗口、图标和配色不构成页分配器或调度器；文件和任务数据要等主线实现真实服务后再接入。
