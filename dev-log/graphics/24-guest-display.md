# G24 · 把真实启动与命令状态交给同一绘图实现

主机窗口已经能显示和操作参考文件；现在让 QEMU 中的内核把真实启动信息与文本会话画到设备帧缓冲。起点需要同时具有 G23 的共享绘图源码和 K13 的串口命令循环。K12、K13 可以在编写前面任一图形停点之前或之后完成，到这里再组合两条路线。K11 准备的共享入口已经调用可选显示接口，默认链接串口实现；本章新增图形实现及独立清单，由 `--graphics` 选择它。

<a id="guest-presenter"></a>

## 从串口会话进入设备绘图

K13 的输入循环仍通过 UART 和 PS/2 收集输入，命令解释器更新同一个 console。可选显示调用 `airi_console_draw(c)` 接收这份状态；适配层根据 `c->view` 打开 Terminal，或把 System 切到内存、设备、日志页，再调用 `ui_draw`。共享 C 源码在主机和 guest 中分别编译、分别运行，两边没有共同的桌面对象。

<a id="guest-services"></a>

图形适配层构造的 live backend 借用真实 BootInfo 和当前 console，没有文件与任务回调。因此内存表来自这次启动的区域记录，Terminal 来自这次会话的日志，而 Files 和 Tasks 会报告服务不可用。

首次有效绘图创建静态 desktop，并由 `ui_init` 安装主题命令扩展。以后若传入另一份 console，就把它视为新会话，重建桌面并重新安装回调。同一个地址也可能被 `airi_console_init` 重置：它会把 extension_context 清成 NULL，指针相等还不足以说明绑定有效。因此同时比较 `c->extension_context` 与 `&desktop`，确认命令消费者仍属于当前桌面；只有对象和绑定都保持时，后续帧才保留窗口和主题。缺帧缓冲时在这一步之前返回。调用仍在内核自己的执行流中完成，尚未实现用户态桌面或进程间通信。

<a id="frame-completion"></a>

帧缓冲描述保存已映射的设备地址、像素尺寸、字节行距及通道布局。`ui_draw` 直接借用它，由 G11 的图元定位和编码像素；没有再建一份主机 RGB 数组。写入完成后 `airi_console_draw` 才返回 true，入口据此发布带有实际 graphics 结果的 READY。静态桌面与长期 console 覆盖后续输入循环。`airi_dashboard_draw` 也使用静态 console：同步调用返回后，桌面保存的地址仍然有效；它只提供初始仪表台，之后换到真实输入会话时会重新绑定。

新建 `kernel/console/display.c`。

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
```

## 选择图形对象和已经生成的字体

Make 的 `desktop-` 配置将下面清单加入图形目标，串口目标继续使用自己的源清单。`UI_SOURCES :=` 立即保存一行共享 C 路径，供对象规则逐个编译；这些文件均已在 G11–G23 写出。清单不包含 reference provider 和 Cocoa，因此 guest 没有主机示例服务或原生窗口依赖。新建 `frontend/sources.mk`。

<!-- write: frontend/sources.mk -->
```make
# 显式选择图形时才包含；列出共享 C 模型与绘图。
UI_SOURCES := frontend/render/draw.c frontend/core/state.c frontend/core/paint.c frontend/core/commands.c frontend/core/pointer.c frontend/core/editing.c frontend/apps/common.c frontend/apps/documents.c frontend/apps/series.c frontend/apps/builtin.c frontend/apps/terminal.c frontend/apps/files.c frontend/apps/editor.c frontend/apps/tasks.c frontend/apps/system.c frontend/apps/settings.c frontend/apps/viewer.c
```

图形构建读取 G12 生成的字体数组。校验模块只需计算资产和图集的摘要，与锁文件比较，不需要重新运行 Pillow。新建 `scripts/graphics_assets.py`，让显式图形入口在构建之前检查这组输入；找不到资产或摘要不符时会抛出异常，构建随之停止。

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

`make_command('desktop-normal')` 选择图形配置，产物与串口对象分目录保存。`subprocess.run(...,check=True)` 要求构建返回成功，随后 `check` 启动本轮 QEMU 并等待新的 READY；只有 graphics=1 才能把随后截取的画面解释为已完成的设备绘图。

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

现在可运行 `./airios run --graphics`，用 F1–F4 和 theme day/theme night 操作真实信息。退出后，默认 `./airios run` 仍是串口版本。窗口、图标和配色不构成页分配器或调度器；文件和任务数据要等主线实现真实服务后再接入。
