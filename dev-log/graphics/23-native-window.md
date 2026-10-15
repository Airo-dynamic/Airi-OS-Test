# G23 · 把共享 C 前端接到 macOS 的真实窗口

共享绘图已经输出Day/Night窗口，指针观察程序也验证了按下与释放。macOS 适配层现在创建真实窗口，把鼠标与按键转换成 ui_pointer/ui_key，再将共享 C 绘制的像素交给 macOS 的界面框架 Cocoa 显示。Linux 仍可以运行可移植的 PPM 与核心测试。

前面的观察程序用固定 C 调用发送确定事件；交互程序改由 macOS 送入真实鼠标和键盘事件。平台创建窗口、取得输入并显示像素，共享 C 核心继续保存文档和计算绘图；两部分运行在同一个主机进程，传函数参数即可交接。

<a id="cocoa-objects"></a>

Cocoa 是 macOS 的应用界面框架。NSApplication 管理应用事件，NSWindow 表示原生窗口，NSView 表示窗口内接受绘制与输入的一块区域。我们定义 AiriView 继承 NSView，提供框架会调用的方法，像 C++ 继承后实现虚函数一样接入既有调用过程；Objective-C 使用消息语法和自身运行时完成派发。

## 由平台持有画布

Objective-C 的 `.m` 文件可以直接包含并调用 C 接口。`#import` 与这里的 include 一样取得声明，并避免重复导入；`@"AiriOS"` 则建立平台字符串对象，与 C 的 `"AiriOS"` 字符数组不同。像素由平台分配，ui_draw 只在绘图期间借用；size_buffer 将尺寸限到 640..1800、480..1100，重新 calloc 后更新帧缓冲描述。

**新建 `frontend/platform/macos/preview.m`：**

<a id="code-frontend-platform-macos-preview-m-1"></a>

<!-- write: frontend/platform/macos/preview.m -->
```objective-c
#import <Cocoa/Cocoa.h>  // 导入 AppKit 与 Foundation 声明；#import 防止重复导入同一头文件。
#include "ui.h"      // 取得共享桌面状态和输入事件接口。
#include <stdio.h>   // 取得主机 printf、文件输出与错误报告接口。
#include <stdlib.h>  // 取得主机分配、释放、退出与数值转换接口。
#include <string.h>  // 取得宿主字符串比较、复制和查找接口。
static struct ui_desktop desktop;    // 静态存储整份桌面；事件回调结束后状态仍保留。
static struct airi_console console;  // 静态保存终端命令与日志，不在每次重画时重建。
static AiriBootInfo boot;  // 平台保存启动描述副本，backend.boot 只借用这个稳定地址。
static struct airi_framebuffer framebuffer;  // 保存软件画布的地址、尺寸、行距和颜色位布局。
static uint32_t *pixels;  // 指向平台拥有的32位像素数组，静态初值为 NULL。
static void size_buffer(int w, int h) {  // w/h 为画布宽高；尺寸变化时重建数组并更新描述。
    if (w < 640)  // 软件画布最小宽度640像素，防止布局缺少空间。
        w = 640;  // 把过小宽度提高到下限。
    if (h < 480)  // 软件画布最小高度480像素。
        h = 480;  // 把过小高度提高到下限。
    if (w > 1800)  // 最大宽度限制控制单帧内存和重画工作量。
        w = 1800;  // 把过大宽度截到当前演示上限。
    if (h > 1100)  // 同样限制画布最大高度。
        h = 1100;  // 把过大高度截到1100像素。
    free(pixels);  // 旧图像包装对象已在前次绘制释放，此时释放旧画布；free(NULL) 合法。
    pixels = calloc((size_t)w * h, 4);  // 先转 size_t 再相乘，分配 w*h 个四字节且清零的像素。
    if (!pixels) {  // 分配失败返回NULL，不能交给绘图继续写像素。
        fputs("AiriOS preview: pixel allocation failed\n", stderr);  // 标准错误报告明确的画布分配失败原因。
        exit(1);  // 以失败状态结束快照进程。
    }
    framebuffer = (struct airi_framebuffer){.virtual_address = (uintptr_t)pixels,  // 复合字面量整体更新描述，uintptr_t 保存主机进程中的数组地址。
                                            .width = (uint64_t)w,      // 把画布宽度保存为接口规定的64位无符号像素数。
                                            .height = (uint64_t)h,     // 保存画布高度，单位是软件像素。
                                            .pitch = (uint64_t)w * 4,  // 每行 w 个四字节像素，因此字节行距为 w*4。
                                            .bpp = 32,      // 每像素总位数32，最高八位当前不用于颜色。
                                            .red_size = 8,  // 红通道占八位。
                                            .red_shift = 16,   // 红通道放在位16..23，读写需平移16位。
                                            .green_size = 8,   // 绿通道占八位。
                                            .green_shift = 8,  // 绿通道放在位8..15。
                                            .blue_size = 8};   // 蓝通道占八位，未指定 blue_shift 时默认零。
    boot.framebuffer = framebuffer;  // 启动描述副本取得同一画布参数，窗口中的设备信息可以读取。
}
static void dump(const char *name) {  // name 是输出文件路径，快照直接使用同一共享绘图。
    ui_draw(&desktop, &framebuffer);  // 共享 C 渲染将当前桌面状态写进平台像素数组。
    FILE *f = fopen(name, "wb");      // 以二进制写模式创建 PPM，失败返回 NULL。
    if (!f) {  // 路径不可写或打开失败时不继续往空文件指针写入。
        perror(name);  // 把当前 errno 原因连同输出路径写到标准错误。
        exit(1);  // 以失败状态结束快照进程。
    }
    bool complete = fprintf(f, "P6\n%llu %llu\n255\n",  // 先写二进制PPM头；返回负数表示输出失败。
                            (unsigned long long)framebuffer.width,  // 宽度转成与%llu对应的无符号整数。
                            (unsigned long long)framebuffer.height) > 0;  // 高度后声明通道上限255；只有完整头才继续输出像素。
    for (unsigned y = 0; complete && y < framebuffer.height; ++y)  // 从上到下逐行输出，首个写入错误后停止。
        for (unsigned x = 0; complete && x < framebuffer.width; ++x) {  // 当前行逐像素输出，同样不在失败后继续写。
            uint32_t p = pixels[y * framebuffer.width + x];  // 当前数组每行恰为width个32位像素，行号乘宽度再加列号定位。
            unsigned char rgb[] = {(p >> 16) & 255, (p >> 8) & 255, p & 255};  // 取出红、绿、蓝三个八位通道，PPM不保存原像素的第四字节。
            complete = fwrite(rgb, 1, sizeof(rgb), f) == sizeof(rgb);  // 返回实际写出的单字节项数，少于三个也属于失败。
        }
    if (fclose(f) != 0)  // 即使前面失败也关闭流；关闭时刷新缓冲仍可能遇到写入错误。
        complete = false;  // 不把只进入stdio缓冲、未完成输出的快照记为成功。
    if (!complete) {  // 头、像素或关闭任一失败，都不能让快照进程返回零。
        perror(name);  // 将当前文件输出错误连同路径写到标准错误。
        exit(1);  // 保留可能存在的部分文件，但以非零状态拒绝这次快照。
    }
}
```

分配失败时 calloc 返回 NULL，立即打印原因并结束；只有有效数组才进入帧缓冲描述。PPM输出也必须检查完成：fprintf写头，fwrite每次交出三个通道，返回值少于请求数量就停止。即使这些调用暂时成功，字节仍可能只进入stdio缓冲；fclose会尝试刷新并关闭，失败也使进程返回1。部分文件可能留下，但Python的check=True不会把它当成成功快照。`[self setNeedsDisplay:YES]` 中 self 是当前视图对象，setNeedsDisplay: 是包含参数位置的消息名，YES 是传入值。`@interface` 与 `@implementation` 分别提供类声明和实现；`- (void)drawRect:(NSRect)dirty` 是某个实例的方法，dirty 是本次允许重画的范围，当前代码选择重画整帧。

<a id="display-lifetime"></a>

pixels 是 C 平台层分配的数组，framebuffer 只描述它。CGDataProvider 临时让图像读取这块内存，CGImage 保存图像描述；绘制完成先释放这些包装对象，数组继续保留。下次尺寸变化才释放旧数组并分配新数组，不能在当前图像还要读取时提前 free。

这里按视图逻辑尺寸分配软件画布，再缩放到视图显示，没有按 Retina 物理像素另建高分辨率画布。窗口的点坐标与这份画布坐标对应，实际屏幕像素可能更多。

前缀 `-` 表示实例方法，YES/NO 是布尔常量，`self.bounds.size.width` 读取当前视图宽度。

isFlipped 让视图事件以左上为原点。drawRect 先调用 ui_draw 填像素，再经 CGDataProvider 和 CGImage 交给 CoreGraphics；按 32 位小端 RGB 描述数据，并在绘制时平移、反转 Y。完成后释放 image、provider 和 colorspace，像素仍留给平台到下次 resize 或退出。当前每次全帧重画。

**追加到 `frontend/platform/macos/preview.m` 末尾：**

<a id="code-frontend-platform-macos-preview-m-2"></a>

<!-- append: frontend/platform/macos/preview.m -->
```objective-c
@interface AiriView : NSView  // 声明 AiriView 继承 NSView，接收窗口绘制与输入回调。
@end  // 结束 AiriView 的类接口声明
@implementation AiriView  // 接下来定义该视图类的实例方法。
- (BOOL)isFlipped {  // 实例方法返回视图是否采用左上角坐标原点。
    return YES;  // 该回调返回真，接受对应框架约定。
}
- (BOOL)acceptsFirstResponder {  // 允许这个视图成为首先接收键盘事件的对象。
    return YES;  // 该回调返回真，接受对应框架约定。
}
- (void)drawRect:(NSRect)dirty {  // 框架请求重画时传入脏矩形；当前实现重画整帧。
    (void)dirty;  // NSView 规定有该参数；全帧重画无需按其范围裁剪。
    int w = (int)self.bounds.size.width;   // 将当前视图逻辑宽度转为软件画布的整数像素数。
    int h = (int)self.bounds.size.height;  // 高度同样取逻辑点；当前没有另建 Retina 物理像素画布。
    if (w != (int)framebuffer.width || h != (int)framebuffer.height)  // 视图尺寸不同才重建像素，普通重画复用原数组。
        size_buffer(w, h);  // 按新尺寸更新平台画布及启动描述。
    ui_draw(&desktop, &framebuffer);  // 共享 C 渲染将当前桌面状态写进平台像素数组。
    CGColorSpaceRef color = CGColorSpaceCreateDeviceRGB();      // 创建 RGB 颜色空间，当前函数负责最终 Release。
    CGDataProviderRef provider = CGDataProviderCreateWithData(  // 建立只借用 pixels 的数据提供者，包装对象不拥有数组。
        NULL, pixels, (size_t)framebuffer.pitch * framebuffer.height, NULL);  // 无额外上下文，提供完整画布字节数；无释放回调，数组仍由平台释放。
    CGImageRef image =  // image 保存图像包装对象，实际字节仍由 provider 读取。
        CGImageCreate(framebuffer.width, framebuffer.height, 8, 32, framebuffer.pitch, color,  // 指定宽高、每通道八位、每像素32位及真实字节行距。
                      kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little, provider, NULL,  // 组合无 alpha 与32位小端布局；provider 交付像素，NULL 使用默认解码。
                      false, kCGRenderingIntentDefault);  // 不进行插值，采用默认颜色呈现意图。
    CGContextRef context = NSGraphicsContext.currentContext.CGContext;  // 借用框架本次绘制的图形上下文，不对它 Release。
    CGContextSaveGState(context);  // 保存坐标变换，避免影响框架后续绘制。
    CGContextTranslateCTM(context, 0, self.bounds.size.height);  // 先平移纵轴，为接下来的上下反转留出画布高度。
    CGContextScaleCTM(context, 1, -1);  // 保持横轴、反转纵轴，将软件首行显示到上边。
    CGContextDrawImage(context, self.bounds, image);  // 把图像画到整个视图范围；这里实际进行图像读取。
    CGContextRestoreGState(context);  // 恢复原图形状态，撤销本次平移与缩放。
    CGImageRelease(image);  // 图像已经绘完，释放其包装对象。
    CGDataProviderRelease(provider);  // 释放提供者包装，借用的 pixels 继续有效。
    CGColorSpaceRelease(color);  // 释放本次创建的颜色空间，配对 Create。
}
```

<a id="event-loop"></a>

打开窗口后 main 不可能事先列出每次点击。NSApp run 等待输入，按事件调用 mouseDown 或 keyDown 等方法；方法改变 desktop 并登记重画需求后返回。事件循环随后调用 drawRect 消费新状态。这是请求重画与实际重画之间的交接，不能认为 setNeedsDisplay 在当前行就完成了绘图。

鼠标方法先用 convertPoint 将窗口坐标变成视图坐标，按事件种类送相应 phase，再请求重绘。nil 表示没有对象指针；本次 `fromView:nil` 特意表示输入来自窗口坐标，而非另一视图，含义由这个接口约定。这样坐标才落在软件画布采用的原点下。[Apple 的坐标转换接口](https://developer.apple.com/documentation/appkit/nsview/convert%28_%3Afrom%3A%29-1dq9l?language=objc)说明了这个参数。

滚轮的小数增量转换为行数，微小非零增量也保留方向；平台若交入零增量，回调直接返回，不产生共享滚动事件。

**追加到 `frontend/platform/macos/preview.m` 末尾：**

<a id="code-frontend-platform-macos-preview-m-3"></a>

<!-- append: frontend/platform/macos/preview.m -->
```objective-c
- (void)mouseDown:(NSEvent *)e {  // e 是框架交来的鼠标按下事件，本方法只在回调期间借用。
    NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];  // 把窗口坐标换到当前视图；nil 特指输入来自窗口坐标系。
    ui_pointer(&desktop, (int)p.x, (int)p.y, e.clickCount > 1 ? 3 : 1);  // 逻辑点转整数画布坐标，多次点击送3，首次按下送1。
    [self setNeedsDisplay:YES];  // 登记需要重画，实际 drawRect 在事件循环稍后执行。
}
- (void)mouseUp:(NSEvent *)e {  // 接收释放事件，结束共享核心中的拖动。
    NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];  // 把窗口坐标换到当前视图；nil 特指输入来自窗口坐标系。
    ui_pointer(&desktop, (int)p.x, (int)p.y, 2);  // phase=2 释放，保留光标与选区但结束 drag。
    [self setNeedsDisplay:YES];  // 登记需要重画，实际 drawRect 在事件循环稍后执行。
}
- (void)mouseDragged:(NSEvent *)e {  // 按住鼠标移动时框架调用此方法。
    NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];  // 把窗口坐标换到当前视图；nil 特指输入来自窗口坐标系。
    ui_pointer(&desktop, (int)p.x, (int)p.y, 0);  // phase=0 表示位置更新，核心按已有拖动状态决定动作。
    [self setNeedsDisplay:YES];  // 登记需要重画，实际 drawRect 在事件循环稍后执行。
}
- (void)mouseMoved:(NSEvent *)e {  // 未按下的鼠标移动更新指针位置。
    NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];  // 把窗口坐标换到当前视图；nil 特指输入来自窗口坐标系。
    ui_pointer(&desktop, (int)p.x, (int)p.y, 0);  // phase=0 表示位置更新，核心按已有拖动状态决定动作。
    [self setNeedsDisplay:YES];  // 登记需要重画，实际 drawRect 在事件循环稍后执行。
}
- (void)scrollWheel:(NSEvent *)e {  // 滚轮增量是平台浮点值，需换成共享核心的整数行数。
    if (e.scrollingDeltaY == 0)  // 平台可能交入零增量，此时无需改变滚动状态。
        return;  // 零滚动量不应被补成向下一行。
    int delta = (int)(-e.scrollingDeltaY / 3);  // 反转平台方向并按三单位一行换算，强转向零截断。
    if (!delta)  // 小数换算后为零时仍保留非零滚动方向。
        delta = e.scrollingDeltaY > 0 ? -1 : 1;  // 正平台增量对应向上-1行，负增量对应向下1行。
    ui_scroll(&desktop, delta);  // 共享核心按活动视图更新其行滚动量。
    [self setNeedsDisplay:YES];  // 登记需要重画，实际 drawRect 在事件循环稍后执行。
}
```

## 将平台按键转成共享事件

<a id="native-text"></a>

普通字符和快捷键采用不同表示。Command-S 被转换为整数 UI_SAVE_KEY，核心执行保存；没有 Command 的 S 被当作 ASCII 字节送入文档。NSString 是平台持有的字符串对象，UTF8String 提供其当前 UTF-8 字节视图；复制进 desktop.clipboard 后，核心拥有独立数组，不继续借用平台临时指针。

NSEvent 带修饰位和字符。按位与 `&` 判断 Command、Ctrl、Shift 等标志，charactersIgnoringModifiers 取得识别快捷键的字符，unichar 保存 UTF-16 码元。Unicode 给字符分配编号，UTF-8 与 UTF-16 则约定编号如何存成字节或十六位单元；ASCII 范围在 UTF-8 中仍是同值的单字节，范围外字符可能需要多个单元。当前只用首个 UTF-16 单元识别快捷键，再把已识别功能键或 ASCII 送进核心，没有实现完整 Unicode 文本编辑。[NSString 的表示说明](https://developer.apple.com/documentation/foundation/nsstring?language=objc)可用于核对两种接口。

Command/Ctrl 统一为快捷动作，Alt-Tab 切窗口。普通键入循环会跳过非 ASCII 字节；粘贴则把整份字节交给 insert 检查，遇非 ASCII 会拒绝整次插入。两种操作的范围都只支持当前 ASCII 文档。

文字粘贴从NSPasteboard取得NSString，先检查是否存在完整字节视图、是否能容纳字节和NUL，再复制到desktop.clipboard。没有字符串时不继续发UI_PASTE，否则先前复制的旧内容会误入当前文档；超过8191字节时也整体拒绝，不能先截短再让insert检查。

完整交接后，Editor的insert才按ASCII合同检查全部字节，拒绝时旧文档、保存点与undo均保留。Files正文粘贴沿文件id动作处理，名称对话框与筛选字段才读取系统文字。

复制/剪切的文字路径反向将C字符串变成NSString；对象指针只在本回调内借用，复制后的数组独立持有内容。

**追加到 `frontend/platform/macos/preview.m` 末尾：**

<a id="code-frontend-platform-macos-preview-m-4"></a>

<!-- append: frontend/platform/macos/preview.m -->
```objective-c
- (void)keyDown:(NSEvent *)e {  // 按键回调把平台修饰键和字符换成共享整数事件。
    BOOL command =  // command 同时接受 Command 与 Control 快捷键。
             (e.modifierFlags & (NSEventModifierFlagCommand | NSEventModifierFlagControl)) != 0,  // | 合并两种掩码，& 提取位，!=0 判断至少一种被按下。
         shift = (e.modifierFlags & NSEventModifierFlagShift) != 0;  // 同样取得 Shift 状态，供共享核心扩展选区。
    NSString *chars = e.charactersIgnoringModifiers;  // 借用忽略部分修饰影响的字符对象，用于识别快捷键。
    unichar c = chars.length ? [chars characterAtIndex:0] : 0;  // 有字符才读取首个 UTF-16 码元；空对象或空串使用零。
    int key = 0;    // 零表示尚未识别功能事件，后续可转为普通字符。
    if (command) {  // 修饰键路径先匹配保存、查找与剪贴板动作。
        switch (c) {  // 首个码元决定本次快捷或功能键分支。
        case 's':     // 此平台码元对应保存当前文档。
            key = UI_SAVE_KEY;  // 保存共享事件值，稍后统一交给 ui_key。
            break;  // 当前匹配已完成，停止这一层循环或分支。
        case 'f':  // 此平台码元对应打开查找字段。
            key = UI_FIND_KEY;  // 保存共享事件值，稍后统一交给 ui_key。
            break;  // 当前匹配已完成，停止这一层循环或分支。
        case 'a':  // 此平台码元对应选择全部文本。
            key = UI_SELECT_ALL;  // 保存共享事件值，稍后统一交给 ui_key。
            break;  // 当前匹配已完成，停止这一层循环或分支。
        case 'c':  // 此平台码元对应复制当前选区。
            key = UI_COPY;  // 保存共享事件值，稍后统一交给 ui_key。
            break;  // 当前匹配已完成，停止这一层循环或分支。
        case 'x':  // 此平台码元对应剪切当前选区。
            key = UI_CUT;  // 保存共享事件值，稍后统一交给 ui_key。
            break;  // 当前匹配已完成，停止这一层循环或分支。
        case 'v':  // 此平台码元对应插入剪贴板内容。
            key = UI_PASTE;  // 保存共享事件值，稍后统一交给 ui_key。
            break;  // 当前匹配已完成，停止这一层循环或分支。
        case 'z':  // 此平台码元对应切换撤销前后的文本。
            key = UI_UNDO;  // 保存共享事件值，稍后统一交给 ui_key。
            break;  // 当前匹配已完成，停止这一层循环或分支。
        case 'l':  // 此平台码元对应清空终端日志。
            key = AIRI_KEY_CLEAR;  // 保存共享事件值，稍后统一交给 ui_key。
            break;  // 当前匹配已完成，停止这一层循环或分支。
        default:  // 当前码元不匹配此组快捷键，保持 key 为零。
            break;  // 当前匹配已完成，停止这一层循环或分支。
        }
    } else if (c == NSTabCharacter && (e.modifierFlags & NSEventModifierFlagOption))  // 没有 Command/Ctrl 时，Option-Tab 对应窗口切换。
        key = UI_NEXT_WINDOW;  // 登记共享核心的切换窗口事件。
    else  // 未匹配组合快捷键时，继续识别普通功能键
        switch (c) {  // 首个码元决定本次快捷或功能键分支。
        case NSLeftArrowFunctionKey:  // 此平台码元对应光标向左移动。
            key = UI_LEFT;  // 保存共享事件值，稍后统一交给 ui_key。
            break;  // 当前匹配已完成，停止这一层循环或分支。
        case NSRightArrowFunctionKey:  // 此平台码元对应光标向右移动。
            key = UI_RIGHT;  // 保存共享事件值，稍后统一交给 ui_key。
            break;  // 当前匹配已完成，停止这一层循环或分支。
        case NSUpArrowFunctionKey:  // 此平台码元对应向上移动或选择上一行。
            key = AIRI_KEY_UP;  // 保存共享事件值，稍后统一交给 ui_key。
            break;  // 当前匹配已完成，停止这一层循环或分支。
        case NSDownArrowFunctionKey:  // 此平台码元对应向下移动或选择下一行。
            key = AIRI_KEY_DOWN;  // 保存共享事件值，稍后统一交给 ui_key。
            break;  // 当前匹配已完成，停止这一层循环或分支。
        case NSHomeFunctionKey:  // 此平台码元对应移到当前行开头。
            key = UI_HOME;  // 保存共享事件值，稍后统一交给 ui_key。
            break;  // 当前匹配已完成，停止这一层循环或分支。
        case NSEndFunctionKey:  // 此平台码元对应移到当前行末尾。
            key = UI_END;  // 保存共享事件值，稍后统一交给 ui_key。
            break;  // 当前匹配已完成，停止这一层循环或分支。
        case NSDeleteFunctionKey:  // 此平台码元对应删除光标后字节。
            key = UI_DELETE_KEY;  // 保存共享事件值，稍后统一交给 ui_key。
            break;  // 当前匹配已完成，停止这一层循环或分支。
        case NSPageUpFunctionKey:  // 平台PageUp没有文本输入，直接滚动视图。
            ui_scroll(&desktop, -10);  // PageUp 把当前视图向上滚动十行。
            break;  // 当前匹配已完成，停止这一层循环或分支。
        case NSPageDownFunctionKey:  // 平台PageDown直接向下滚动视图。
            ui_scroll(&desktop, 10);  // PageDown 把当前视图向下滚动十行。
            break;  // 当前匹配已完成，停止这一层循环或分支。
        case NSF5FunctionKey:  // 此平台码元对应提交当前文档执行请求。
            key = UI_RUN_KEY;  // 保存共享事件值，稍后统一交给 ui_key。
            break;  // 当前匹配已完成，停止这一层循环或分支。
        case 27:  // 此平台码元对应取消当前框或字段。
            key = UI_ESCAPE;  // 保存共享事件值，稍后统一交给 ui_key。
            break;  // 当前匹配已完成，停止这一层循环或分支。
        case 127:  // 此平台码元对应平台退格转为共享ASCII退格值8。
            key = 8;  // 保存共享事件值，稍后统一交给 ui_key。
            break;    // 当前匹配已完成，停止这一层循环或分支。
        case '\t':  // 平台制表符映射到UI_TAB；当前Editor正文将它插为四个空格。
            key = UI_TAB;  // 保存共享事件值，稍后统一交给 ui_key。
            break;  // 当前匹配已完成，停止这一层循环或分支。
        default:  // 当前码元不匹配此组快捷键，保持 key 为零。
            break;  // 当前匹配已完成，停止这一层循环或分支。
        }
    bool file_paste = desktop.active == UI_FILES_APP &&  // Files正文粘贴使用前文保存的文件id，不读取系统文字剪贴板。
                      desktop.dialog == UI_DIALOG_NONE && desktop.focus == UI_FOCUS_NONE;  // 名称对话框和筛选字段仍是文字接收者，不能走文件动作。
    if (key == UI_PASTE && !file_paste) {  // 只有文字粘贴才需要从平台字符串交接完整字节。
        NSString *p = [NSPasteboard.generalPasteboard stringForType:NSPasteboardTypeString];  // 无字符串类型时返回nil，本回调只借用这个对象。
        const char *text = p ? p.UTF8String : NULL;  // 只在对象有效时取得UTF-8视图，不保存其指针到桌面。
        if (!text) {  // 没有平台文字或无法得到字节视图，不能重用上次clipboard。
            snprintf(desktop.toast, sizeof(desktop.toast), "Clipboard has no text.");  // 使用已有的短提示数组报告拒绝原因，不建立新文档状态。
            key = 0;  // 不分派UI_PASTE，避免空插入删除选区或粘贴旧内容。
        } else if (strlen(text) >= sizeof(desktop.clipboard)) {  // 先按完整UTF-8字节数检查，容量还必须容纳末尾NUL。
            snprintf(desktop.toast, sizeof(desktop.toast), "Clipboard limit: 8191 bytes.");  // 超限整体拒绝，不把截断后的前缀伪装成完整输入。
            key = 0;  // 文档、保存点和撤销快照都还未被核心修改。
        } else  // 完整平台文字及终止零都能装入共享数组。
            memcpy(desktop.clipboard, text, strlen(text) + 1);  // 连同NUL复制到桌面自有数组，核心之后不借用NSString字节。
    }
    if (key)  // 识别出非零功能事件后走共享键规则。
        ui_key(&desktop, key, shift);  // 传递事件与 Shift 状态，改变同一文档或窗口状态。
    else if (!command && e.characters.length) {  // 没有快捷键修饰且有普通字符时才进行文本键入。
        const char *p = e.characters.UTF8String;  // 借用当前事件字符串的 UTF-8 字节视图，不保存这个指针。
        for (; *p; ++p)  // 无初始化子句；逐字节读到 NUL，p 每次前移一字节。
            if ((unsigned char)*p < 128)  // 转成无符号后只接收 ASCII 字节，避免 signed char 负值误入。
                ui_key(&desktop, *p, shift);  // 把该 ASCII 字节作为共享文本事件。
    }
    if (key == UI_COPY || key == UI_CUT) {  // 核心已经更新自身剪贴板后，再同步给 macOS。
        [NSPasteboard.generalPasteboard clearContents];  // 清除系统剪贴板原有类型和值。
        [NSPasteboard.generalPasteboard setString:[NSString stringWithUTF8String:desktop.clipboard]  // 先把共享 NUL 字节串转成 NSString，再交给系统剪贴板。
                                          forType:NSPasteboardTypeString];  // 声明所写内容是平台字符串类型。
    }
    [self setNeedsDisplay:YES];  // 登记需要重画，实际 drawRect 在事件循环稍后执行。
}
@end  // 结束 AiriView 的全部回调方法实现
```

应用 delegate 接收生命周期回调，最后窗口关闭后结束程序。NSObject 是基础对象，尖括号中的 NSApplicationDelegate 声明实现该协议，系统据此调用对应方法。

**追加到 `frontend/platform/macos/preview.m` 末尾：**

<a id="code-frontend-platform-macos-preview-m-5"></a>

<!-- append: frontend/platform/macos/preview.m -->
```objective-c
@interface AiriDelegate : NSObject <NSApplicationDelegate>  // 基础对象实现应用代理协议，接收生命周期回调。
@end  // 结束 AiriDelegate 的类接口声明
@implementation AiriDelegate  // 定义应用代理的方法实现。
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender {  // 框架在最后窗口关闭后询问是否结束应用。
    (void)sender;  // 回调签名必须包含应用对象，本策略无需读取它。
    return YES;    // 该回调返回真，接受对应框架约定。
}
@end  // 结束 AiriDelegate 的生命周期方法实现
```

autoreleasepool 在离开作用域时处理登记在池中的临时对象，并不替代所有 C 内存或 CoreGraphics 对象的显式释放。firstResponder 是首先接收键盘事件的视图；设置为 AiriView，才能让 keyDown 接到输入，而不是仅有一张能显示的图片。

`main` 在池的作用域中解析四类可选参数：`--size 宽x高` 决定软件画布，`--app` 选择初始应用，`--theme day` 选择日间颜色，`--snapshot 路径` 生成一帧后退出；没有快照路径时进入交互窗口。随后初始化 reference 服务。boot 复制成平台持有的结构，backend.boot 指向该副本，画布尺寸变化可单独更新它。

snapshot 分支直接绘制并 dump；交互分支创建 NSApplication、delegate、window 与 AiriView，设置 contentView 和 firstResponder。makeFirstResponder 让键盘送到该视图，NSApp run 开始事件循环，关闭窗口后返回。

`[[AiriView alloc] initWithFrame:...]` 连续发送两次消息：先分配实例，再按给出的矩形初始化，结果成为窗口的内容视图。NSApplication 的 sharedApplication 则取得框架提供的应用对象，而不是每次输入都新建一个应用。

**追加到 `frontend/platform/macos/preview.m` 末尾：**

<a id="code-frontend-platform-macos-preview-m-6"></a>

<!-- append: frontend/platform/macos/preview.m -->
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
                sscanf(argv[++i], "%dx%d", &w, &h);  // 从1280x800形式读取整数到 w/h；& 传可写变量地址。
            else if (!strcmp(argv[i], "--app") && i + 1 < argc) {  // 应用选项在已知名称表中查找枚举下标。
                const char *a = argv[++i];  // 借用当前应用名参数。
                for (int j = 0; j < UI_APP_COUNT; ++j)  // 只遍历有效应用名称数量。
                    if (!strcmp(a, apps[j]))  // 输入字符串与名称相等才匹配。
                        app = j;  // 名称表下标同时就是后续 UI 应用身份。
            }
            else if (!strcmp(argv[i], "--theme") && i + 1 < argc)  // 主题用于固定快照，也可作为原生窗口初始偏好。
                theme = argv[++i];  // 两种模式共享所有内容和布局。
        }
        struct ui_backend b;     // 局部回调表先由参考服务初始化。
        ui_fixture_backend(&b);  // 重建本进程内的参考文件和任务记录。
        boot = *b.boot;     // 解引用后复制整个启动结构，不保留服务原结构的借用。
        b.boot = &boot;     // 让桌面之后借用平台稳定副本。
        size_buffer(w, h);  // 按新尺寸更新平台画布及启动描述。
        airi_console_init(&console, &boot);  // 终端和界面都读取同一平台启动副本。
        ui_init(&desktop, b, &console);      // 复制回调表，连接 console 并建立桌面初始窗口。
        desktop.theme = !strcmp(theme, "day") ? UI_DAY : UI_NIGHT;  // 桌面持有呈现偏好，文本会话不持有颜色状态。
        if (app == UI_VIEWER) {  // Viewer 启动前需先装入一份实际 CSV 数据。
            for (unsigned i = 0; i < desktop.file_count; ++i)  // 遍历有效文件快照。
                if (strstr(desktop.files[i].name, ".csv")) {  // 找到包含.csv的文件名；strstr 返回匹配位置或 NULL。
                    ui_open_document(&desktop, i);  // 按快照下标读取并解析数据，供 Viewer 渲染。
                    break;  // 当前匹配已完成，停止这一层循环或分支。
                }
        }
        ui_open(&desktop, (enum ui_app)app);    // 最终打开指定应用，枚举值来自名称表匹配。
        desktop.windows[app].maximized = true;  // 默认把应用内容放到最大化工作区。
        desktop.monitor = app == UI_EDITOR;     // Editor 场景显示旁边的状态监视区域。
        if (snapshot) {  // 有输出路径时只生成图片，不进入事件循环。
            dump(snapshot);  // 先共享绘图，再将画布按 PPM 字节布局输出。
            free(pixels);    // 快照写完后释放平台像素数组，再结束当前进程。
            return 0;  // 正常结束当前程序，成功状态交给启动它的命令。
        }
        [NSApplication sharedApplication];  // 取得框架唯一应用对象，并建立 NSApp 全局入口。
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];  // 作为有窗口与 Dock 图标的普通应用运行。
        AiriDelegate *delegate = [AiriDelegate new];  // new 返回拥有的代理对象；当前未启用 ARC，局部指针不自动释放它。
        [NSApp setDelegate:delegate];  // 登记生命周期回调的接收对象。
        NSWindow *window = [[NSWindow alloc]  // 先分配原生窗口实例，再由下一行初始化。
            initWithContentRect:NSMakeRect(0, 0, w, h)  // 以指定宽高建立窗口内容区域。
                      styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |  // 掩码组合标题栏和关闭按钮。
                                NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable  // 继续允许最小化与调整原生窗口尺寸。
                        backing:NSBackingStoreBuffered  // 让平台缓存绘制结果，按窗口事件更新。
                          defer:NO];  // 立即创建窗口底层资源，不延后到首次显示。
        window.title = @"AiriOS";  // @ 字符串创建 NSString，标题使用项目名称；参考服务的能力边界由应用内错误说明。
        window.contentMinSize = NSMakeSize(1024, 768);   // 交互窗口内容最小1024×768逻辑点。
        window.contentMaxSize = NSMakeSize(1800, 1100);  // 最大内容尺寸与软件画布上限一致。
        window.acceptsMouseMovedEvents = YES;  // 允许未按下时收到 mouseMoved 回调。
        AiriView *view = [[AiriView alloc] initWithFrame:NSMakeRect(0, 0, w, h)];  // 分配并初始化共享画布视图，初始矩形匹配内容大小。
        window.contentView = view;  // 把视图装入窗口，由窗口负责展示和投递事件。
        [window makeFirstResponder:view];  // 键盘事件首先送到 AiriView 的 keyDown。
        [window center];  // 将原生窗口居中显示。
        [window makeKeyAndOrderFront:nil];      // 置前并成为键窗口，nil 表示此操作没有特定发送对象。
        [NSApp activateIgnoringOtherApps:YES];  // 激活当前应用，使键盘输入可进入此窗口。
        [NSApp run];   // 进入框架事件循环；回调修改 desktop 并请求重画。
        free(pixels);  // 事件循环结束后没有绘图读取者，再释放最后一份画布。
        return 0;      // 正常结束当前程序，成功状态交给启动它的命令。
    }
}
```

先建立 `build/ui` 目录，再在 macOS 编译 Cocoa 程序。同一次 `cc` 调用按 `.c` 与 `.m` 后缀选择语言，再将共享实现、`preview.m` 和 Cocoa 链接到一个程序；`--snapshot` 选择单帧分支，`--app editor` 指定初始应用。Linux 可继续使用 G22 的纯 C 观察程序生成 PPM。

**在学习目录根目录运行：**

<!-- run -->
```sh
if [ "$(uname -s)" = Darwin ]; then  # 命令替换取得内核名；仅macOS能链接Cocoa。
    mkdir -p build/ui  # 建立原生窗口产物目录。
    cc -std=c11 -O1 -g -Wall -Wextra -Werror -Iinclude -Ifrontend/include       frontend/render/draw.c frontend/core/*.c frontend/apps/*.c frontend/backends/reference.c       kernel/lib/base.c kernel/boot/info.c kernel/console/console.c       frontend/platform/macos/preview.m -framework Cocoa -o build/ui/airios-ui  # 共享C实现和Objective-C适配在同一进程链接Cocoa。
    ./build/ui/airios-ui --snapshot build/workspace.ppm --app editor --theme night  # 绘制当前Editor并输出PPM，随后退出。
else  # 当前平台没有 Cocoa，保留可移植 C 观察路径
    echo 'Cocoa adapter: macOS only; the portable C/PPM checkpoints remain runnable.'  # 其他平台提示可继续的主机观察路径。
fi
```

macOS 得到 `build/workspace.ppm`，此时内容是单个最大化Editor；文件名只是主机输出路径。执行 `./build/ui/airios-ui` 打开交互窗口：Files 与 Editor 使用同一内存服务，保存后刷新可见，F5 返回执行服务未连接。关闭程序后文件重置。

真实鼠标现在能操作共享状态。回到 QEMU 时继续使用 C 绘图与窗口逻辑，Cocoa 只留在主机适配层。
