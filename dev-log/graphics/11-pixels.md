# G11 · 按像素地址画出矩形和线段

从 K11 的文本与启动基础继续，主机程序已经能保存命令状态，内核也有独立的串口输出。图形画面需要另一种输出：在可写内存中保存每个位置的颜色，让显示端按这些像素呈现画面。先在主机生成一张320×200的图片，铺夜间底色、画被裁剪的粉色条带和一条蓝线。绘图函数只接收像素描述，后面接入guest时可以沿用这些函数。

<a id="pixel-canvas"></a>

像素是画面中的一个颜色采样位置。这里取左上为原点，x向右、y向下增加；一个矩形用左上位置和宽高表示。`{20,20,280,160}`包含列20至299、行20至179；列300和行180是矩形之后的位置。这个左上包含、右下排除的范围称为半开范围，列数恰好是300−20=280。

## 描述画布，并限制允许写入的区域

内核基础中的[启动合同](https://github.com/Airo-dynamic/Airi-OS-Test/blob/main/dev-log/core/05-boot-contract.md#boot-layout)已经建立帧缓冲描述`airi_framebuffer`：它保存可写虚拟地址、像素宽高、每行字节跨度和通道布局。它只描述内存，不包含全部像素。主机观察程序持有实际数组，绘图借用这个数组与描述；一轮绘图结束前，两者都必须保持有效。内核使用前文验证过的引导描述，主机观察则构造固定且有效的布局。`u_begin`只建立绘图状态，不负责分配或再次验证内存。

在自己的学习目录中建立 `frontend/include`、`frontend/render` 和 `labs/tutorial` 目录，再新建 `frontend/include/render.h`。颜色暂时直接传 `0xRRGGBB`，接口的坐标和宽高均按画布像素计数。`u_rect` 的前四项依次为左上 x、y、宽、高；`u_line` 的前四项则为起点 x、y 和终点 x、y。两者最后一项都是颜色。

<a id="code-frontend-include-render-h-1"></a>

<!-- write: frontend/include/render.h -->
```c
#ifndef AIRI_RENDER_H   // 同一编译单元只展开一次绘图接口
#define AIRI_RENDER_H   // 记住本头文件已经处理
#include <airi/boot.h>  // 借用前文的帧缓冲描述、固定宽整数和bool
struct ui_rect {  // 用像素数描述半开矩形，右下边界不属于矩形
    int x;  // 左边所在的整屏列号，可为负以表示部分在屏外
    int y;  // 顶边所在的整屏行号，向下增加
    int w;  // 横向像素数，不是右边坐标
    int h;  // 纵向像素数，不是底边坐标
};
void u_begin(const struct airi_framebuffer *);  // 借用已验证的描述与可写像素数组，直到本次绘图结束
void u_clip(struct ui_rect);  // 请求矩形与整幅画布求交，替换当前裁剪区
void u_reset_clip(void);      // 恢复整幅画布的可写范围
void u_rect(int, int, int, int, uint32_t);  // 按左上、宽高及0xRRGGBB填充裁剪内像素
void u_line(int, int, int, int, uint32_t);  // 用整数误差将含两端点的线段变成像素
void u_frame(struct ui_rect, uint32_t);     // 用四条一像素宽矩形绘制边框
bool u_inside(struct ui_rect, int, int);    // 判断整屏坐标是否在半开矩形内
#endif  // 结束包含保护
```

这一轮先用 `u_begin(&canvas)` 把主机数组交给渲染器，再调用 `u_rect(0,0,320,200,...)` 铺背景。后面的粉色条带只允许写到内容区，所以 `u_clip((struct ui_rect){20,20,280,160})` 会先保存一个较小的可写范围。

渲染器只保存画布描述指针和当前裁剪矩形。`u_clip` 每次将请求与整屏求交，替换此前范围；再次调用时以这次请求为准。真正绘制矩形还要再与当前裁剪求交。因此请求画在 (10,10)、尺寸 300×50 的粉色条带，最终只留下列 20 至 299、行 20 至 59；负尺寸或完全位于画布外的请求没有可写像素。

粉色条带传入的颜色数值 `0xe5a6c7` 保存红 229、绿 166、蓝 199，每个通道占八位。本章主机画布恰好使用这种布局，可以直接保存这个数值。设备描述可能让通道使用不同的位宽和位置；例如红色五位时，`229*31/255=27`，再移到设备规定的最低位。三个通道不重叠，按位或可以将它们组合。描述验证已将位宽限制为1至8，`1u<<size`据此生成通道最大值时不会越过整数范围。

新建 `frontend/render/draw.c`。先完成颜色编码与画布绑定：`native` 形成可供设备保存的颜色数值，`u_begin` 取得调用者描述并选择是否需要转换；实际像素写入在后面的矩形函数中进行。

<a id="code-frontend-render-draw-c-1"></a>

<!-- write: frontend/render/draw.c -->
```c
#include "render.h"  // 取得矩形、画布描述与本文件提供的接口
static const struct airi_framebuffer *fb;  // 借用本次调用者持有的描述，不拥有像素内存
static bool standard;  // 三个8位通道是否恰好组成0xRRGGBB
static struct ui_rect clip;  // 所有图元共同使用的当前可写范围
static uint32_t native(uint32_t rgb) {  // 把八位RGB数值转换为设备的实际通道布局
    if (standard)  // 常见布局不需要逐通道缩放和移位
        return rgb;  // 直接写入已有0xRRGGBB数值
    const unsigned shifts[] = {fb->red_shift, fb->green_shift, fb->blue_shift};  // 通道在设备像素中的最低位
    const unsigned sizes[] = {fb->red_size, fb->green_size, fb->blue_size};  // 同下标保存该通道的1至8位宽度
    uint32_t result = 0;  // 三个不重叠的通道从零组合
    for (unsigned k = 0; k < 3; ++k) {  // 按红、绿、蓝顺序逐个转换
        unsigned channel = (rgb >> ((2 - k) * 8)) & 255u;  // 右移16、8、0位取得当前八位颜色
        unsigned maximum = (1u << sizes[k]) - 1;     // 例如5位通道最大值为31
        unsigned scaled = channel * maximum / 255u;  // 将0至255映射为设备通道范围，整数除法向下取整
        result |= scaled << shifts[k];  // 移到设备位域后按位或加入结果
    }
    return result;  // 返回可按小端字节次序写入的设备像素值
}
void u_begin(const struct airi_framebuffer *f) {  // 调用者必须保证描述及整幅像素内存有效
    fb = f;  // 保存借用，之后的图元通过它定位像素
    standard = f->red_size == 8 && f->green_size == 8 && f->blue_size == 8 &&  // 先确认三色都占八位
               f->red_shift == 16 && f->green_shift == 8 && f->blue_shift == 0;  // 再确认各通道恰好位于RGB数值的位置
    u_reset_clip();  // 新画布从整屏范围开始，不继承此前裁剪
}
```

画布指针已经有了保存位置，接下来让 `u_begin` 调用的 `u_reset_clip` 取得整屏范围。局部裁剪仍保存同一种 x/y/w/h 记录，右边和底边先在 64 位整数中求和，再收窄到屏幕；空交集留下零尺寸，后续绘制因此可以直接结束。`u_inside` 使用相同的半开边界，后面的输入定位也会沿用它。

**追加到 `frontend/render/draw.c`：**

<!-- append: frontend/render/draw.c -->
```c
void u_reset_clip(void) {  // 恢复当前画布的整屏半开范围
    clip = (struct ui_rect){0, 0, (int)fb->width, (int)fb->height};  // 复合字面量按x、y、w、h构造矩形
}
void u_clip(struct ui_rect r) {  // 将请求限制到当前画布，不与上一次裁剪叠加
    int64_t right = (int64_t)r.x + r.w;   // 先提升为64位再相加，避免两个int的边界和溢出
    int64_t bottom = (int64_t)r.y + r.h;  // 保存请求底边，负起点仍能保留原来的终点
    int left = r.x < 0 ? 0 : r.x;    // 左边至少是画布第0列
    int top = r.y < 0 ? 0 : r.y;     // 顶边至少是画布第0行
    if (right > (int64_t)fb->width)  // 超出整屏右边时收窄终点
        right = (int64_t)fb->width;  // width是最后一列之后的半开边界
    if (bottom > (int64_t)fb->height)  // 超出整屏底边时收窄终点
        bottom = (int64_t)fb->height;  // height是最后一行之后的半开边界
    if (r.w <= 0 || r.h <= 0 || right <= left || bottom <= top) {  // 空请求或完全在屏外的请求没有可写交集
        clip = (struct ui_rect){0, 0, 0, 0};  // 保存空范围，随后图元直接返回
        return;  // 不把负尺寸转换成巨大的无符号范围
    }
    clip = (struct ui_rect){left, top, (int)(right - left), (int)(bottom - top)};  // 交集在已验证的屏幕尺寸内，可转回int
}
bool u_inside(struct ui_rect r, int x, int y) {  // 绘图与后续命中测试共用边界规则
    return r.w > 0 && r.h > 0 && x >= r.x && y >= r.y &&  // 左顶包含，空或负尺寸永不命中
           (int64_t)x < (int64_t)r.x + r.w && (int64_t)y < (int64_t)r.y + r.h;  // 右下排除，加法先提升以免int回绕
}
```

内容区已经限制为 `[20,300)×[20,180)`。矩形函数先把粉色请求收窄到 `[20,300)×[20,60)`，再为每行的首个可写像素计算地址。右边与底边继续在 64 位中求和，避免两个 32 位坐标相加回绕；没有交集便在形成地址前返回。

<a id="pixel-pitch"></a>

像素地址由行和列共同定位：`base + y*pitch + x*(bpp/8)`。`pitch`表示两行起点之间的字节距离，可能包含行末填充。对于24位、宽3、pitch=12的画布，每行9字节像素之后还有3字节填充；像素`(2,1)`位于`base+12+2*3=base+18`。用宽度代替pitch会落错位置。

交集给出行列，`native` 给出设备颜色，两者到这里才组合成写入。四字节且对齐的像素可以用 `uint32_t` 写一次；三字节或非对齐地址按字节写。地址的低两位检查为零，即 `p&3` 为零时，地址是 4 的倍数。

**追加到 `frontend/render/draw.c`，写出矩形填充：**

<!-- append: frontend/render/draw.c -->
```c
void u_rect(int x, int y, int w, int h, uint32_t rgb) {  // 写绘制矩形与当前裁剪区的交集
    if (w <= 0 || h <= 0)  // 非正尺寸没有任何像素
        return;  // 在计算地址前结束空绘制
    int a = x > clip.x ? x : clip.x;  // 交集左边取两者较大值
    int b = y > clip.y ? y : clip.y;  // 交集顶边取两者较大值
    int64_t right = (int64_t)x + w;   // 绘制右边在64位中计算
    int64_t bottom = (int64_t)y + h;  // 绘制底边在64位中计算
    int64_t clip_right = (int64_t)clip.x + clip.w;   // 当前裁剪的半开右边
    int64_t clip_bottom = (int64_t)clip.y + clip.h;  // 当前裁剪的半开底边
    if (right > clip_right)  // 写入不能越过裁剪右边
        right = clip_right;  // 交集右边取较小终点
    if (bottom > clip_bottom)  // 写入不能越过裁剪底边
        bottom = clip_bottom;  // 交集底边取较小终点
    if (right <= a || bottom <= b)  // 求交后没有行或列需要写入
        return;  // 不形成任何像素地址
    int c = (int)right;   // 有效交集受画布限制，转换为循环半开列终点
    int d = (int)bottom;  // 同样取得半开行终点
    uint32_t color = native(rgb);  // 将输入RGB编码为当前设备像素值
    unsigned bytes = fb->bpp / 8;  // 24位像素用3字节，32位用4字节
    for (int row = b; row < d; ++row) {  // 从交集顶行逐行写到d之前
        volatile uint8_t *p = (volatile uint8_t *)(uintptr_t)fb->virtual_address +  // 将数值地址还原成可读写的像素字节指针
                              (uint64_t)row * fb->pitch + (uint64_t)a * bytes;  // 行距用字节计算，再加入首列的像素字节偏移
        if (bytes == 4 && !((uintptr_t)p & 3u)) {  // 四字节像素且地址低两位为零时采用对齐写入
            volatile uint32_t *q = (volatile uint32_t *)p;  // 当前行首可按一个uint32_t一个像素访问
            for (int col = a; col < c; ++col) {  // 写交集中的每一列，不碰行尾填充
                *q = color;  // 将一个完整设备像素值写入当前位置
                ++q;  // uint32_t指针推进一个元素，跨过四字节
            }
        } else {  // 三字节或非对齐像素使用字节访问
            for (int col = a; col < c; ++col) {  // 仍只写同一交集列范围
                for (unsigned k = 0; k < bytes; ++k)  // 每次写像素内的一个字节
                    p[k] = (uint8_t)(color >> (k * 8));  // 先写低八位，按x86-64的小端次序保存数值
                p += bytes;  // 跨过当前像素，到本行的下一列
            }
        }
    }
}
```

矩形函数逐行写完交集后，主机数组对应位置已经保存粉色数值；它仍需稍后的输出函数才能成为可查看的图片。每行都重新按 pitch 定位，因此行末填充保持原样。

当前x86-64数值使用小端字节序。三十二位`0x00e5a6c7`在地址递增方向保存`c7 a6 e5 00`；二十四位只写前三字节。PPM文件要求的却是`e5 a6 c7`，观察程序会显式提取通道，而不直接转存原数组。设备格式中没有用于颜色的额外位写为零，本渲染器不使用它们作为透明度。`volatile`使映射范围的访问实际发生；裁剪和地址有效性仍由代码与调用契约保证。

## 用同一轮误差决定线段的下一像素

蓝线从`(0,70)`到`(319,190)`，多数位置没有整数坐标恰好落在线上。整数线段算法在相邻格子之间选位置，用横纵距离累积误差；每轮先写一个像素，再决定向终点移动哪一轴。候选像素仍经过`u_rect`裁剪，蓝线不能越过先前设置的内容区。

追加到`frontend/render/draw.c`末尾。

<a id="code-frontend-render-draw-c-2"></a>

<!-- append: frontend/render/draw.c -->
```c
void u_line(int x0, int y0, int x1, int y1, uint32_t color) {  // 包含起终点的整数栅格线段
    int64_t dx = x1 > x0 ? (int64_t)x1 - x0 : (int64_t)x0 - x1;  // 横向距离的绝对值，先提升避免差值溢出
    int64_t dy = y1 > y0 ? (int64_t)y0 - y1 : (int64_t)y1 - y0;  // 纵向距离的相反数，进入同一误差式
    int sx = x0 < x1 ? 1 : -1;  // 当前列向终点每次移动一格的方向
    int sy = y0 < y1 ? 1 : -1;  // 当前行向终点每次移动一格的方向
    int64_t error = dx + dy;    // 以横距减纵距作为初始整数误差
    for (;;) {  // 只有绘制完终点才退出
        u_rect(x0, y0, 1, 1, color);  // 每个候选点仍经过矩形裁剪，不直接访问数组
        if (x0 == x1 && y0 == y1)     // 当前写入的点已经是终点
            break;  // 保留终点像素后结束线段
        int64_t twice = error * 2;  // 两轴判断共享同一轮的旧误差
        if (twice >= dy) {  // 误差达到横向前进条件
            error += dy;  // 横向前进累计负纵距
            x0 += sx;     // 当前列向终点移动一格
        }
        if (twice <= dx) {  // 此判断独立，同一轮可以同时沿两轴走
            error += dx;  // 纵向前进累计横距
            y0 += sy;     // 当前行向终点移动一格
        }
    }
}
void u_frame(struct ui_rect r, uint32_t color) {  // 以一像素厚边围住传入矩形
    if (r.w <= 0 || r.h <= 0)  // 空矩形不计算底边和右边
        return;  // 保留当前画布
    u_rect(r.x, r.y, r.w, 1, color);  // 顶边覆盖r.w个像素
    u_rect(r.x, r.y + r.h - 1, r.w, 1, color);  // 底边位于半开底边前一行
    u_rect(r.x, r.y, 1, r.h, color);  // 左边覆盖r.h个像素
    u_rect(r.x + r.w - 1, r.y, 1, r.h, color);  // 右边位于半开右边前一列
}
```

线段函数的两个移动条件必须各自判断，并共用修改前的`twice`。以更小的`(0,0)`到`(3,2)`为例，横距dx=3、负纵距dy=−2，误差从1开始。绘制位置与下一次移动依次为：

| 当前写入点 | 本轮twice | 移动后的位置与误差 |
|---|---:|---|
| (0,0) | 2 | 两轴都走，得到(1,1)，error=2 |
| (1,1) | 4 | 只走x，得到(2,1)，error=0 |
| (2,1) | 0 | 两轴都走，得到(3,2)，error=1 |
| (3,2) | — | 终点已写，循环结束 |

若把第二个条件改成`else if`，第一条件成立后便不能走y，同一轮的对角移动会消失。误差采用64位整数，避免端点差值与两倍误差在32位中溢出；当前界面使用有限的屏幕坐标，算法仍会遍历从起点到终点的候选像素。裁剪限制写入范围，不缩短很长线段的遍历时间。

## 将主机数组写成图片

观察程序用静态数组拥有320×200个32位像素，局部`canvas`仅描述它。先铺底色，再收窄裁剪，粉色矩形因此只留下`x=[20,300)、y=[20,60)`；蓝线使用同一裁剪。恢复整屏范围之后，边框才可以画到靠近屏幕边缘的位置。

主机观察程序负责把数组交给文件接口。`save_image` 写出 PPM 的 P6 头，记录魔数、尺寸和 255 通道上限，再按红、绿、蓝顺序保存每个像素的三个字节。它借用主机 C 标准库；共享绘图模块继续只处理像素，不依赖 FILE。

一次短写会停止输出循环，但仍要关闭文件句柄；fclose 的最后缓冲输出失败也返回 false。成功表示像素文件已交给主机文件接口，不作掉电持久性承诺。

新建 `labs/tutorial/pixels.c`，先写数组与输出函数。

<a id="code-labs-tutorial-pixels-c-1"></a>

<!-- write: labs/tutorial/pixels.c -->
```c
#include "render.h"  // 使用本章刚实现的像素接口
#include <stdio.h>   // 主机观察通过文件与终端呈现结果
#define WIDTH 320    // 当前观察画布的像素列数
#define HEIGHT 200   // 当前观察画布的像素行数
static uint32_t pixels[WIDTH * HEIGHT];     // 程序拥有整幅画布，初始为零
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

输出函数已有了，main 再建立描述这份静态数组的局部 canvas。`u_begin` 借用它以后，依次铺背景、限制内容区、画粉色条带和蓝线；恢复整屏裁剪后才画外框，外框便可以写到内容区之外。最后把同一数组交给 `save_image("build/pixels.ppm")`。函数返回之前，canvas 与 pixels 都保持有效。

**追加到 `labs/tutorial/pixels.c`：**

<!-- append: labs/tutorial/pixels.c -->
```c
int main(void) {  // 普通主机程序，不作为内核入口
    struct airi_framebuffer canvas = {  // 描述本进程持有的数组，未指定字段按C规则置零
        .virtual_address = (uintptr_t)pixels,  // 地址属于主机进程，绘图借用而不释放
        .width = WIDTH, .height = HEIGHT,      // 宽高按像素计，读取者据此裁剪
        .pitch = WIDTH * 4, .bpp = 32,      // 当前观察每行紧排四字节像素
        .red_size = 8, .red_shift = 16,     // 红色使用数值的第16至23位
        .green_size = 8, .green_shift = 8,  // 绿色使用第8至15位
        .blue_size = 8, .blue_shift = 0     // 蓝色使用第0至7位
    };
    u_begin(&canvas);  // 本函数结束前数组与局部描述都保持有效
    u_rect(0, 0, WIDTH, HEIGHT, 0x0d131c);  // 用新版夜间背景的原始RGB铺满画布
    u_clip((struct ui_rect){20, 20, 280, 160});  // 将写入限制为x=[20,300)、y=[20,180)
    u_rect(10, 10, 300, 50, 0xe5a6c7);  // 与裁剪求交，留下x=[20,300)、y=[20,60)的粉色矩形
    u_line(0, 70, 319, 190, 0x83aff7);  // 蓝线同样遵守裁剪，屏外部分不写入
    u_reset_clip();  // 后面的边框可以覆盖整幅画布
    u_frame((struct ui_rect){4, 4, WIDTH - 8, HEIGHT - 8}, 0x46566b);  // 用细边框核对右下半开边界
    if (!save_image("build/pixels.ppm"))  // 图像输出失败不继续报告成功
        return 1;  // 让Shell获得失败退出码
    printf("clip inside=%d outside=%d\n", pixels[20 * WIDTH + 20] == 0xe5a6c7,  // 交集首像素应为粉色
           pixels[19 * WIDTH + 20] == 0x0d131c);  // 交集上一行仍保留背景
    puts("wrote build/pixels.ppm");  // 明确刚写入的文件位置
    return 0;  // 主机观察完成
}
```

在学习目录根目录运行：

<!-- run -->
```sh
mkdir -p build  # 保存当前学习目录的主机产物
cc -std=c11 -O0 -g -Wall -Wextra -Werror -Iinclude -Ifrontend/include labs/tutorial/pixels.c frontend/render/draw.c -o build/pixels  # 两个C文件一起链接，-I提供已敲入的头文件目录
./build/pixels  # 运行本章的像素观察，失败通过退出码交回Shell
```

终端输出如下，两个1分别表示交集首像素是粉色、上一行保留背景。

```text
clip inside=1 outside=1
wrote build/pixels.ppm
```

macOS可用`open build/pixels.ppm`查看，或用`sips -s format png build/pixels.ppm --out build/pixels.png`转成PNG；Linux使用支持PPM的图片查看器。画面已有细边框、粉色条带和蓝线，尚没有文字。把字符放进这块数组，还需要笔画像素和字符间距。
