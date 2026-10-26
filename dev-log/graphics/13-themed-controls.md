# G13 · 同一组控件使用日夜两套颜色

字体比较已经能在背景上写文字。一组按钮还需要同时决定底面、文字、底边和焦点框的颜色：夜间的白色正文在日间浅底上不能直接复用。现在用同样的标签、位置与状态生成Night和Day两张控件图。改变的是呈现颜色，按钮内容与几何保持不变；暂时没有鼠标事件，悬停和按下状态由观察程序明确传入。

## 用颜色用途选整套调色板

<a id="color-roles"></a>

`0xRRGGBB`的最大值是`0xffffff`。颜色用途从更大的`0x1000000`开始编号，因此`U_BG`表示“当前主题背景”而不是某个像素数值。`u_color`将用途映射为24位RGB；真正RGB仍可直接传入，例如`0x123456`在两个主题中都保持原值。字形混合后的RGB也在24位范围内，不会被再次误认成颜色用途。

同一个按钮会把 U_PANEL 交给底面，把 U_TEXT 交给文字，再把状态位交给控件函数。前两者说明颜色的用途，状态位说明此刻怎样画按钮；它们不保存文件，也不触发操作。先在 render.h 末尾，用下面的新块替换原有 endif，把这些声明加入已有包含保护。旧块只用于定位。

<a id="code-frontend-include-render-h-1"></a>

<!-- replace: frontend/include/render.h -->
<!-- before -->
```c
#endif  // 结束包含保护
```
<!-- after -->
```c
enum ui_color {  // 数值从24位RGB范围之外开始，名字表示用途而非固定像素
    U_BG = 0x1000000,  // 整幅画布背景，第一个角色的编号
    U_PANEL,    // 标题、工具栏和普通按钮的底面
    U_SURFACE,  // 正文内容区域
    U_RAISED,   // 被选择的列表背景
    U_EDGE,     // 轻分隔和不活动外框
    U_TEXT,     // 主要正文
    U_MUTED,    // 次要说明
    U_ACCENT,   // 粉色主动作与身份标记
    U_BLUE,     // 蓝色焦点与辅助强调
    U_RED,      // 错误和危险操作
    U_WARM,     // 警示与代码数值
    U_PINK,     // 独立身份色，区别于可操作强调
    U_CONTROL,  // 普通可操作边界
    U_SHADOW,     // 窗口或对话框阴影
    U_HOVER,      // 悬停按钮底面
    U_PRESSED,    // 按下按钮底面
    U_DISABLED,   // 禁用文字
    U_SUCCESS,    // 成功状态
    U_ON_ACCENT,  // 粉色实面上的文字
    U_CHROME,  // 标题身份片的表面
    U_INK,     // 标题身份片的前景
    U_COLOR_END  // 半开角色终点，只用于边界检查，不是可绘制颜色
};
enum ui_theme { UI_NIGHT, UI_DAY };  // 下标0夜间、1日间，零初始化自然选择夜间
enum ui_metric { UI_CONTROL_RADIUS = 2 };  // 本章只需要按钮的2像素圆角，不预置窗口尺寸
enum ui_control_state {  // 不同状态占不同二进制位，可以按位或组合
    U_NORMAL = 0,    // 没有任何额外状态位
    U_SELECTED = 1,  // 第0位：当前已选中的动作
    U_HOT = 2,      // 第1位：悬停反馈
    U_DOWN = 4,     // 第2位：按下反馈
    U_FOCUSED = 8,  // 第3位：键盘焦点
    U_INACTIVE = 16  // 第4位：服务不可用，覆盖其他交互强调
};
void u_theme(unsigned);      // 选择本轮绘图调色板，不拥有应用的会话数据
uint32_t u_color(uint32_t);  // 角色解析成RGB，0至0xffffff的真实RGB原样通过
void u_round(struct ui_rect, int, uint32_t);  // 圆角边缘按4×4样本混合，仍遵守裁剪
void u_control(struct ui_rect, const char *, unsigned);   // 按调用者明确给出的状态绘制按钮，不执行动作
void u_button(struct ui_rect, const char *, bool, bool);  // 将selected与disabled转换为共同状态表
#endif  // 结束包含保护
```

调色板是一个二维数组，第一下标选0=Night或1=Day，第二下标是角色编号减`U_BG`。例如`U_ACCENT`是第8个角色，`palettes[UI_DAY][7]`得到`0xa13f70`；夜间同一列得到`0xe5a6c7`。角色声明顺序与每行颜色顺序须同时保持，数组列数为`U_COLOR_END-U_BG=21`。终点是边界值，不进入调色板下标。

渲染器的 `theme` 只保存绘图期间使用的调色板下标。观察程序直接调用 `u_theme` 选择两轮呈现；以后桌面会保存自己的选择，并在每帧开始时传给这个函数。传入非Day的值会退回Night，不用任意整数索引二维数组。非法颜色角色范围外的值原样返回；调用者应传已声明角色或合法24位RGB，范围检查保证错误值不会越界读调色板。

现在让已有绘图路径消费这些角色。矩形经 native 编码成设备像素，字形和后面的圆角经 blend 混入背景；两条入口都须先把角色变成 RGB。先定位 draw.c 从第一条 include 到 native 函数开头的一小段，替换为调色板及解析入口。native 中原有的 standard 判断和通道编码继续接在新块之后。

<a id="code-frontend-render-draw-c-1"></a>

<!-- replace: frontend/render/draw.c -->
<!-- before -->
```c
#include "render.h"  // 取得矩形、画布描述与本文件提供的接口
static const struct airi_framebuffer *fb;  // 借用本次调用者持有的描述，不拥有像素内存
static bool standard;  // 三个8位通道是否恰好组成0xRRGGBB
static struct ui_rect clip;  // 所有图元共同使用的当前可写范围
static uint32_t native(uint32_t rgb) {  // 把八位RGB数值转换为设备的实际通道布局
```
<!-- after -->
```c
#include "render.h"  // 取得矩形、画布描述与本文件提供的接口
#include "../assets/font-atlas.h"  // 借用主机生成的八套字形度量和覆盖率
static const struct airi_framebuffer *fb;  // 借用本次调用者持有的描述，不拥有像素内存
static bool standard;  // 三个8位通道是否恰好组成0xRRGGBB
static struct ui_rect clip;  // 所有图元共同使用的当前可写范围
static unsigned theme;  // 当前绘图使用的调色板下标，单线程调用期间由u_theme设置
static const uint32_t palettes[2][U_COLOR_END - U_BG] = {  // 两行按ui_color角色顺序保存颜色
    {0x0d131c, 0x172130, 0x192330, 0x3b2c38, 0x46566b,  // 夜间：背景、框架、内容、选中和分隔
     0xe7ebf5, 0xa9b2c6, 0xe5a6c7, 0x83aff7, 0xf0919e, 0xe5ba73,  // 夜间文字和状态
     0xdd99be, 0x758198, 0x0b0e16, 0x2b3244, 0x493442, 0x7e889b, 0x76c8ad, 0x191d29, 0xe7c9d6, 0x39283a},  // 夜间控件
    {0xeff1f7, 0xf0f2f8, 0xfafbfe, 0xf4e2eb, 0xd5dae6,  // 日间表面逐层区分
     0x24283b, 0x586174, 0xa13f70, 0x3564cb, 0xb92f4f, 0x845c15,  // 日间文字和状态
     0xaa487b, 0x7b849a, 0xdce1ed, 0xe4e9f3, 0xf0dce7, 0x7a8294, 0x2a7265, 0xffffff, 0x4b3046, 0xfff4fa}  // 日间控件
};
void u_theme(unsigned mode) {  // 非法模式退回夜间，避免越界取调色板
    theme = mode == UI_DAY ? UI_DAY : UI_NIGHT;  // 只保留有效枚举
}
uint32_t u_color(uint32_t value) {  // 字形和图元共用同一套颜色解析
    if (value >= U_BG && value < U_COLOR_END)  // 只将完整角色范围作为数组下标
        return palettes[theme][value - U_BG];  // 减去首角色得到当前主题行中的列号
    return value;  // 普通RGB和范围外数值原样返回，后者不索引调色板
}
static uint32_t native(uint32_t rgb) {  // 把颜色角色或八位RGB转换成设备布局
    rgb = u_color(rgb);  // 先解析角色，之后的位域计算只处理真实RGB
```


原始RGB和语义角色在进入设备布局之前统一成24位颜色。矩形直接覆盖不读取旧背景；文字混合则必须先解析前景角色，之后再用每通道权重计算新RGB。两条路径最终都通过`native`编码，所以主机标准32位数组和guest实际24/32位设备继续使用同一接口。

文字路径已经由 G12 建立。现在定位原来紧邻 blend 的图集 include 和函数开头，用下方新块替换：图集 include 已移到文件顶部，这里移除旧位置，并在混合前加入 `rgb=u_color(rgb)`。字体选择、间距和覆盖率循环继续保留；超出裁剪右界仍只跳过当前行，显式换行继续回到起始列。

<a id="code-frontend-render-draw-c-2"></a>

<!-- replace: frontend/render/draw.c -->
<!-- before -->
```c
#include "../assets/font-atlas.h"  // 读取主机生成的八套字形度量和覆盖率
static void blend(int x, int y, uint32_t rgb, unsigned alpha) {  // 将0至255覆盖率的前景混入原像素
    if (!alpha || !u_inside(clip, x, y))  // 零覆盖率和裁剪外像素不需要读背景
        return;  // 保持原像素不变
```
<!-- after -->
```c
static void blend(int x, int y, uint32_t rgb, unsigned alpha) {  // 将0至255覆盖率的前景混入原像素
    if (!alpha || !u_inside(clip, x, y))  // 零覆盖率和裁剪外像素不需要读背景
        return;  // 保持原像素不变
    rgb = u_color(rgb);  // 混合前把角色变成真实RGB，混合结果不会重新成为角色
```

混合仍读取已经铺好的背景。比如按钮传入 U_ACCENT，blend 先取得当前主题的粉色，再从这个 RGB 中拆出通道与背景混合；若跳过解析，就会把角色编号的各位误当成颜色。混合结果最大仍是 `0xffffff`，交给 native 时原样通过 u_color，不会再取一次调色板。原有文字循环与推进距离计算无需重写。

## 用十六个样本得到圆角边缘

<a id="round-coverage"></a>

矩形的角是直角，一个半径2的按钮仍需要让边缘像素部分覆盖背景。圆角填充先写中间的实心横条，再只在角落做4×4采样。一个像素内取四行、四列样本中心，记录其中多少落在圆内，最后把0至16计数转成覆盖率；整个圆角不是把每像素直接舍入成“在圆内/在圆外”。

样本局部位置是`1/8、3/8、5/8、7/8`像素。将圆心距离乘8，横向距离可写成`8*(radius-x)-2*sx-1`；纵向同理，圆内判断为`dx²+dy² <= (8*radius)²`。整数坐标避免在内核引入浮点计算。默认radius=2、角像素x=0/y=0时，16个样本中6个在圆内，`alpha=(6*255+8)/16=96`；夜间选中底面红229混入背景红13，结果为`(229*96+13*159+127)/255=94`。部分覆盖不是“六个像素”，而是同一个像素的十六份采样中占六份。

圆角将半径先收窄到两边的一半，零或负半径走直角路径。左右和上下角通过镜像复用同一个覆盖率，中部不用逐像素读取背景。当前按钮半径2、观察样本半径8，画布和控件都是有限屏幕尺寸；这一算法的工作量随角半径平方增加，后面窗口也使用小半径。

先在 draw.c 末尾追加圆角填充。它将上面的采样计数交给既有 blend，因而仍受当前裁剪约束，并与文字使用相同的背景读写路径。

<a id="code-frontend-render-draw-c-3"></a>

<!-- append: frontend/render/draw.c -->
```c
void u_round(struct ui_rect r, int radius, uint32_t color) {  // 中间填实色，四角单独计算像素覆盖率
    if (r.w <= 0 || r.h <= 0)  // 空矩形没有任何像素需要写
        return;  // 不计算圆心和角落地址
    if (radius > r.w / 2)  // 左右圆角不能相互越过
        radius = r.w / 2;  // 将半径限制到宽度的一半
    if (radius > r.h / 2)  // 上下圆角也不能相互越过
        radius = r.h / 2;  // 再限制到高度的一半
    if (radius <= 0) {  // 零或负半径使用直角矩形
        u_rect(r.x, r.y, r.w, r.h, color);  // 保留同样的输入范围与裁剪行为
        return;  // 直角路径已经写完，不计算采样
    }
    u_rect(r.x, r.y + radius, r.w, r.h - 2 * radius, color);  // 中部完整横条无须读背景
    for (int y = 0; y < radius; ++y) {  // 只遍历上方角落行，下方按镜像复用
        u_rect(r.x + radius, r.y + y, r.w - 2 * radius, 1, color);  // 上方两圆角之间的实心段
        u_rect(r.x + radius, r.y + r.h - 1 - y, r.w - 2 * radius, 1, color);  // 下方对应实心段
        for (int x = 0; x < radius; ++x) {  // 只在左上角的小范围计算覆盖率
            unsigned count = 0;  // 当前像素内部落在圆内的样本数量，范围0至16
            for (int sy = 0; sy < 4; ++sy)  // 在像素内按四行取样
                for (int sx = 0; sx < 4; ++sx) {  // 每行再取四个样本，共16个
                    int64_t dx = 8 * ((int64_t)radius - x) - 2 * sx - 1;  // 样本中心按八倍坐标保存，避免浮点小数
                    int64_t dy = 8 * ((int64_t)radius - y) - 2 * sy - 1;  // 同样取得到圆心的纵向距离
                    if (dx * dx + dy * dy <= 64 * (int64_t)radius * radius)  // 距离平方不超过半径平方的样本在圆内
                        ++count;  // 该样本贡献1/16像素覆盖
                }
            unsigned alpha = (count * 255 + 8) / 16;  // 将0至16计数转换成0至255覆盖率并就近取整
            blend(r.x + x, r.y + y, color, alpha);    // 左上像素混合进既有背景
            blend(r.x + r.w - 1 - x, r.y + y, color, alpha);  // 同一个覆盖率用于右上镜像
            blend(r.x + x, r.y + r.h - 1 - y, color, alpha);  // 同一个覆盖率用于左下镜像
            blend(r.x + r.w - 1 - x, r.y + r.h - 1 - y, color, alpha);  // 同一个覆盖率用于右下镜像
        }
    }
}
```

<a id="control-state"></a>

控件状态需要组合：选中是二进制第0位1，焦点是第3位8，`U_SELECTED|U_FOCUSED`得到9。按位或组合后，`state&U_SELECTED`与`state&U_FOCUSED`仍能各自识别。U_NORMAL=0表示没有附加位；禁用16可以与按下4同时存在，但绘图规则让禁用覆盖按下强调。

按钮底面的优先级为按下、悬停、选中、普通；禁用随后将底面与边界改回较弱的角色。选中状态还写一条内部粉色底线；普通选中的底面也是粉色，此时底线与底面重合。选中与悬停或按下组合时，底面换色，底线才成为可见的选择标记。焦点独立画外围蓝框。文字使用14px中等字重，水平位置按推进总量居中；代码中的18是纵向占位高度，不是字体字号。按下时文字向下移动一像素，按钮的几何范围保持不变。禁用分支不会显示焦点框，也不会下移文字。

接着追加控件外观函数。u_control 接收已确定的状态，u_button 则把应用手里的 selected、disabled 两个布尔值转换成状态位，再调用它。

<!-- append: frontend/render/draw.c -->
```c
void u_control(struct ui_rect r, const char *label, unsigned state) {  // 一个状态值决定当前按钮的像素外观
    bool disabled = (state & U_INACTIVE) != 0;  // 按位与只取禁用位，非零转换为true
    uint32_t fill = U_PANEL;  // 没有其他状态时使用普通按钮底面
    if (state & U_DOWN)  // 按下的优先级最高
        fill = U_PRESSED;  // 按下底面覆盖悬停与选中底面
    else if (state & U_HOT)  // 未按下而处于悬停时使用悬停底面
        fill = U_HOVER;  // 当前状态仍可同时带焦点位
    else if (state & U_SELECTED)  // 未按下或悬停时才使用选中实面
        fill = U_ACCENT;  // 选中主动作使用主题粉色
    uint32_t edge = state & U_SELECTED ? U_ACCENT : U_CONTROL;  // 选中与普通可操作边界分别取色
    if (disabled) {  // 服务不可用时覆盖交互强调
        fill = U_PANEL;  // 禁用仍有可辨认的底面
        edge = U_EDGE;   // 底边改为轻分隔色
    }
    u_round(r, UI_CONTROL_RADIUS, fill);    // 两像素圆角保持当前紧凑几何
    if (!(state & U_SELECTED) || disabled)  // 普通或禁用按钮使用底边提示可操作区域
        u_rect(r.x + 4, r.y + r.h - 1, r.w - 8, 1, edge);  // 留出左右四像素的细底边
    if ((state & U_SELECTED) && !disabled)  // 启用的选中按钮额外保留内部粗底线
        u_rect(r.x + 8, r.y + r.h - 3, r.w - 16, 2, U_ACCENT);  // 在悬停或按下底面中保留选中的粉色标记
    if ((state & U_FOCUSED) && !disabled)  // 焦点与选中可组合，但禁用不显示焦点
        u_frame((struct ui_rect){r.x - 2, r.y - 2, r.w + 4, r.h + 4}, U_BLUE);  // 外围蓝框不改变按钮内容宽高
    uint32_t text = U_TEXT;  // 普通、悬停和按下使用正文前景
    if (disabled)  // 禁用文字采用独立角色
        text = U_DISABLED;  // 角色在日夜主题中分别保留可读性
    else if ((state & U_SELECTED) && !(state & (U_DOWN | U_HOT)))  // 仅选中实面使用其专用前景
        text = U_ON_ACCENT;  // 避免把该前景用于悬停或按下底面
    int x = r.x + (r.w - u_text_width(label, U_MEDIUM)) / 2;  // 用14px中等字重的推进量水平居中
    int y = r.y + (r.h - 18) / 2;  // 18像素是按钮文字占位高度，不是字体字号
    if ((state & U_DOWN) && !disabled)  // 可操作按钮按下时出现一像素位移
        ++y;  // 只移动文字，按钮与命中矩形保持不变
    u_text(x, y, label, U_MEDIUM, text);  // 背景已经形成，再按覆盖率混入居中文字
}
void u_button(struct ui_rect r, const char *label, bool selected, bool disabled) {  // 供只持有选择与可用性数据的调用者使用
    unsigned state = selected ? U_SELECTED : U_NORMAL;  // 当前应用选择转换成第0位
    if (disabled)  // 调用者明确报告此动作不可用
        state |= U_INACTIVE;  // 按位或加入禁用位，保留已有选择位
    u_control(r, label, state);  // 当前只绘制明确状态，不读取鼠标或执行动作
}
```

`u_control`只决定像素外观，不执行保存、打开等动作。`u_button`将应用提供的selected和disabled转换为共同位表，尚不读取鼠标。以后输入分派可以产生这些状态，现在观察程序明确枚举它们，使两种调色板先有稳定的比较输入。

## 同一内容分别绘制两种主题

新建 `labs/tutorial/controls.c`，先写数组和图片输出函数。它沿用前两章的 PPM 写入方式：本轮画完后读取同一 pixels 数组，保存成独立文件。两轮绘图之间数组继续有效。

<a id="code-labs-tutorial-controls-c-1"></a>

<!-- write: labs/tutorial/controls.c -->
```c
#include "render.h"  // 使用已实现的颜色、圆角、文字和明确控件状态
#include <stdio.h>   // 保存PPM并打印当前主题的实际RGB
#define WIDTH 760    // 六个按钮的排布所需像素宽度
#define HEIGHT 250   // 固定内容使两张主题图可以直接比较
static uint32_t pixels[WIDTH * HEIGHT];     // 数组在两次绘制之间复用，主题切换只改呈现
static bool save_image(const char *path) {  // 将本程序的32位RGB数组输出成可查看的PPM
    FILE *file = fopen(path, "wb");  // 以二进制写入打开路径，已有内容会截断
    if (!file)  // 父目录缺失或主机拒绝打开时没有可用文件句柄
        return false;  // 把失败交回main，不打印完成消息
    bool ok = fprintf(file, "P6\n%d %d\n255\n", WIDTH, HEIGHT) > 0;  // 写PPM魔数、像素尺寸和通道最大值
    for (unsigned i = 0; ok && i < WIDTH * HEIGHT; ++i) {  // 顺序输出数组，一次短写后停止
        unsigned char rgb[] = {  // 文件固定使用红、绿、蓝字节顺序，与主机端序无关
            (unsigned char)(pixels[i] >> 16),  // 红色右移后转换只保留低八位
            (unsigned char)(pixels[i] >> 8),   // 同样取得绿色字节
            (unsigned char)pixels[i]  // 同样取得蓝色字节
        };
        ok = fwrite(rgb, 1, 3, file) == 3;  // 每像素写三字节，短写使ok变false
    }
    if (fclose(file) != 0)  // 即使已经短写也关闭句柄，并检查最后的缓冲输出
        ok = false;  // 关闭失败同样不算完整文件
    return ok;  // 只有所有写入和关闭成功才报告完成
}
```

观察内容由 names 和 states 两个等长数组共同给出：下标相同的标签与状态组成一个按钮。六项依次为普通、悬停、按下、选中、焦点、禁用；最后一项同时有禁用和按下位。main 先借用数组建立画布，再在 Night、Day 两轮中依次选主题、清背景、绘制同样内容并输出当前文件。下方另画状态值 9 和半径 8 样本，分别观察组合状态与更明显的圆角。

继续追加 main。

<!-- append: labs/tutorial/controls.c -->
```c
int main(void) {  // 使用同一组内容分别绘制Night与Day
    struct airi_framebuffer canvas = {  // 描述本进程持有的数组，未指定字段按C规则置零
        .virtual_address = (uintptr_t)pixels,  // 地址属于主机进程，绘图借用而不释放
        .width = WIDTH, .height = HEIGHT,      // 宽高按像素计，读取者据此裁剪
        .pitch = WIDTH * 4, .bpp = 32,      // 当前观察每行紧排四字节像素
        .red_size = 8, .red_shift = 16,     // 红色使用数值的第16至23位
        .green_size = 8, .green_shift = 8,  // 绿色使用第8至15位
        .blue_size = 8, .blue_shift = 0     // 蓝色使用第0至7位
    };
    u_begin(&canvas);  // 本函数结束前数组与局部描述都保持有效
    const char *paths[] = {"build/controls-night.ppm", "build/controls-day.ppm"};  // 下标与主题枚举一致
    const char *names[] = {"Normal", "Hover", "Pressed", "Selected", "Focused", "Disabled"};  // 六个独立显示状态
    const unsigned states[] = {U_NORMAL, U_HOT, U_DOWN, U_SELECTED, U_FOCUSED, U_INACTIVE | U_DOWN};  // 最后组合禁用与按下，观察禁用优先
    for (unsigned mode = UI_NIGHT; mode <= UI_DAY; ++mode) {  // 先夜间后日间，内容与几何保持一致
        u_theme(mode);  // 选择当前绘图调色板，不重新创建观察内容
        u_rect(0, 0, WIDTH, HEIGHT, U_BG);  // 每轮先铺整幅背景，清除上一主题像素
        u_text(24, 14, "AiriOS / shared controls", U_TITLE, U_TEXT);  // 两张图使用完全相同的标题
        u_text(24, 54, "Same geometry, text and explicit states", U_BODY, U_MUTED);  // 说明当前图像比较的固定输入
        for (unsigned i = 0; i < 6; ++i)  // 按同一顺序绘制六种状态
            u_control((struct ui_rect){24 + (int)i * 120, 108, 108, 34}, names[i], states[i]);  // 34像素按钮高，间隔12像素
        u_control((struct ui_rect){24, 174, 188, 34}, "Selected + focused", U_SELECTED | U_FOCUSED);  // 数值9同时携带选中与焦点
        u_round((struct ui_rect){250, 174, 90, 34}, 8, U_ACCENT);    // 单独展示半径8的边缘覆盖率
        u_text(356, 179, "radius 8 / 16 samples", U_BODY, U_MUTED);  // 与默认2像素按钮圆角对照
        printf("%s: bg=%06x accent=%06x literal=%06x selected-focus=%u\n",  // 报告角色解析及状态组合的可观察数值
               mode == UI_DAY ? "Day" : "Night", (unsigned)u_color(U_BG),  // 主题名和当前背景的实际24位RGB
               (unsigned)u_color(U_ACCENT), (unsigned)u_color(0x123456),  // 粉色角色随主题变化，原始RGB原样通过
               (unsigned)(U_SELECTED | U_FOCUSED));  // 按位或的结果为1|8=9
        if (!save_image(paths[mode]))  // 两张输出分别检查写入和关闭
            return 1;  // 任一文件失败便停止，不宣称两主题完成
    }
    puts("wrote build/controls-night.ppm and build/controls-day.ppm");  // 两轮均成功才报告双图
    return 0;  // 主机观察完成
}
```

`paths[0]`、`paths[1]`分别接收 Night、Day 的结果。第二轮会覆盖数组里的第一轮像素，但第一张文件已在前一轮写完，因此两张图都留下来。两图的尺寸、标签、状态组合相同，颜色差异来自 u_color 的调色板选择。以后桌面也会把主题选择传给绘图；文件与窗口对象由桌面另外保存。

运行：

<!-- run -->
```sh
mkdir -p build  # 确保两张主题图的父目录存在
cc -std=c11 -O0 -g -Wall -Wextra -Werror -Iinclude -Ifrontend/include labs/tutorial/controls.c frontend/render/draw.c -o build/controls  # 链接当前明确状态的控件观察程序
./build/controls  # 两轮使用同内容，分别写Night与Day文件
```

输出如下。literal在两轮都是123456，selected-focus在两轮都是9；只有角色解析随主题改变。

```text
Night: bg=0d131c accent=e5a6c7 literal=123456 selected-focus=9
Day: bg=eff1f7 accent=a13f70 literal=123456 selected-focus=9
wrote build/controls-night.ppm and build/controls-day.ppm
```

查看`build/controls-night.ppm`和`build/controls-day.ppm`：选中按钮有粉色实面，焦点有外围蓝框，禁用按钮保持文字但没有按下位移；半径8样本比默认半径2具有更明显的角落过渡。绘图已经能组成界面，Files与Editor还需要能列出、读取和保存的真实内容，下一步建立主机内存文件服务。
