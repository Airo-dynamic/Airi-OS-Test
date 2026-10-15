# G12 · 把八套字体转成可以绘制的字符

像素观察图已经能画矩形与线段，但把字符A的编号65写进像素内存不会形成字母。显示一行`AiriOS / iii WWW 0123`需要两份信息：每个字符的笔画覆盖哪些像素，以及下一个字符从哪里开始。现在生成八套固定字体数据，让同一字符串以小字、正文、标题、代码和大读数显示在一张图中。

<a id="glyph-model"></a>

TTF字体保存笔画轮廓及布局度量。一个字符的具体形状称为字形；按字号把轮廓转换成像素称为光栅化。Pillow在主机做这一步，共享 C 绘图随后只读取生成的固定数组，G24 才把这份绘图编进内核。字形边缘会部分覆盖一个像素，覆盖率用一字节保存：0不覆盖，255完全覆盖，中间值用来混合已有背景。

## 固定字体输入和八套生成参数

字体锁文件保存四份外部输入：两份TTF用于绘制，两份OFL文本保存许可。`assets`是记录数组，每项有`file`文件名、固定提交的`url`以及原始字节`sha256`；上层`commit`说明这些URL共同使用的Google Fonts提交。URL中的`%5B`、`%5D`编码字体文件名的方括号，不是Python索引。

新建`frontend/assets/fonts.lock.json`。JSON保持合法数据，不加入源码注释。

<a id="code-frontend-assets-fonts-lock-json-1"></a>

<!-- write: frontend/assets/fonts.lock.json -->
```json
{
  "commit": "2eb0b48d5f760f62e286216f0859a8c540dbc1bd",
  "assets": [
    {
      "file": "ibmplexsans.ttf",
      "url": "https://raw.githubusercontent.com/google/fonts/2eb0b48d5f760f62e286216f0859a8c540dbc1bd/ofl/ibmplexsans/IBMPlexSans%5Bwdth,wght%5D.ttf",
      "sha256": "3b031aa4216174205bd8471f88a49b91f093169e9e87bd5262242bc5967fe2e3"
    },
    {
      "file": "ibmplexmono.ttf",
      "url": "https://raw.githubusercontent.com/google/fonts/2eb0b48d5f760f62e286216f0859a8c540dbc1bd/ofl/ibmplexmono/IBMPlexMono-Regular.ttf",
      "sha256": "6a3412f058c7d8dfd9170c41e85ade48e5156ecb89356110ca57a0a27734af46"
    },
    {
      "file": "sans-OFL.txt",
      "url": "https://raw.githubusercontent.com/google/fonts/2eb0b48d5f760f62e286216f0859a8c540dbc1bd/ofl/ibmplexsans/OFL.txt",
      "sha256": "7e6b2818edbd8f6a01ae80641cc8f16a51080d08fb4e532be3a0b6f74adb07da"
    },
    {
      "file": "mono-OFL.txt",
      "url": "https://raw.githubusercontent.com/google/fonts/2eb0b48d5f760f62e286216f0859a8c540dbc1bd/ofl/ibmplexmono/OFL.txt",
      "sha256": "7e6b2818edbd8f6a01ae80641cc8f16a51080d08fb4e532be3a0b6f74adb07da"
    }
  ]
}
```

Sans字体文件带字重和字宽两条可变轴。同一轮廓可以按轴值取不同形态；该文件的轴顺序为Weight、Width，所以生成器传`[weight,100]`，100是标准字宽。Plex Mono输入是固定常规字体，不调用轴接口。字号是请求的像素字号，不保证每个字形包围盒都恰好有同样高度。

字体枚举顺序同时决定生成二维数组的第一下标，不能只改变其中一边。完整替换`frontend/include/render.h`，保留已有绘图接口并加入八个名字及文字接口。

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
enum ui_font {  // 枚举下标与生成器faces的顺序一一对应
    U_SMALL,  // Plex Sans 12px、字重400，用于次要信息
    U_BODY,   // Plex Sans 14px、字重400，用于正文
    U_LABEL,  // Plex Sans 16px、字重500，用于标签
    U_TITLE,  // Plex Sans 22px、字重500，用于标题
    U_MONO,   // Plex Mono 14px常规字重，用于终端
    U_CODE,   // Plex Mono 16px常规字重，用于代码
    U_MEDIUM,  // Plex Sans 14px、字重500，用于紧凑按钮
    U_DISPLAY  // Plex Sans 32px、字重300，用于大读数
};
void u_text(int, int, const char *, enum ui_font, uint32_t);  // 用度量与覆盖率绘制可打印ASCII文字
int u_text_width(const char *, enum ui_font);  // 累加单行字形advance供文字布局
#endif  // 结束包含保护
```

U_MEDIUM是14像素、字重500，与U_BODY同字号但笔画更重；U_DISPLAY是32像素、字重300。标题与标签也使用500，普通正文和小字使用400。观察图在同一背景上绘制相同字符串，字号、字重及比例/等宽的差异才能单独辨认。

<a id="atlas-shape"></a>

图集把覆盖率顺序放进大数组，再让小记录定位每个字形。`offset`是数组下标而非指针：一个字形offset=100、宽2、高3，占六字节，像素`row=1,col=1`取`data[100+1*2+1]`，下一字形可从106开始。记录还保存x/y偏移与advance推进距离。空格可以不画任何像素，但仍有advance；把推进距离误当包围盒宽度会让字符间距失去空白。

生成器中的容器对应三个层次：`data`如`[0,128,255,...]`，每项是一字节覆盖率；`glyphs`保存当前字体的95个六元组`(offset,w,h,x,y,advance)`；`metrics`再保存八份完整的glyphs列表。`append`将一整个列表作为一项，`extend`则把bytes迭代出的每个整数加入data，不能互换。

新建 `scripts/font_atlas.py`。生成时，脚本从自身位置找到工程根目录，将四份外部资产逐个读成字节并核对摘要；随后才按八套参数分配字形、记录度量和收集覆盖率。输出文件是供 C 编译器包含的头文件，Python 的列表只在主机生成期间存在。

<a id="code-scripts-font_atlas-py-1"></a>

<!-- write: scripts/font_atlas.py -->
```python
"""Generate the checked-in A8 atlas from locked fonts with Pillow 12.3.0."""  # 在主机将固定字体转换为guest可读取的A8覆盖率数组
import hashlib  # 读取资产字节后计算SHA-256摘要
import json     # 导入JSON解析，将锁文件文本转成程序内字典
from pathlib import Path  # 导入Path，后续用路径对象组合观察输出位置
from PIL import Image, ImageDraw, ImageFont  # 借用Pillow创建灰度图片、画字形和取得字体度量

ROOT = Path(__file__).resolve().parents[1]  # 本脚本目录的上一级就是学习仓库根目录
assets = ROOT / 'frontend/assets'  # Path的/按路径段连接，得到字体与生成图集所在目录
lock = json.loads((assets / 'fonts.lock.json').read_text())  # 先读取UTF-8文本，再把assets键对应的数组变成记录列表
for record in lock['assets']:  # 每轮record是一个含file、url和sha256的资产字典
    actual = hashlib.sha256((assets / record['file']).read_bytes()).hexdigest()  # 读当前资产原始字节并取得十六进制摘要字符串
    if actual != record['sha256']:  # 摘要不等于锁定值时不能继续生成可信图集
        raise RuntimeError(f'Font checksum mismatch: {record["file"]}')  # f字符串带入失败文件名，异常使生成在写输出前停止

faces = [('ibmplexsans.ttf', 12), ('ibmplexsans.ttf', 14), ('ibmplexsans.ttf', 16),  # 列表的每项是字体文件名与字号二元组，顺序对应C字体枚举
         ('ibmplexsans.ttf', 22), ('ibmplexmono.ttf', 14), ('ibmplexmono.ttf', 16), ('ibmplexsans.ttf', 14), ('ibmplexsans.ttf', 32)]  # 补标题和两套等宽字体，再补14px中等字重与32px轻字重，共八项
data = []     # 大覆盖率数组起初为空，后续逐字节追加0到255整数
metrics = []  # 八套字体的度量列表起初为空，每项将是一整套glyphs
for index, (filename, size) in enumerate(faces):  # enumerate给出下标index与一项元组，内层括号再解包成文件名和字号
    font = ImageFont.truetype(str(assets / filename), size)  # 将Path转字符串交给Pillow，按当前字号读取TTF轮廓
    if filename.startswith('ibmplexsans'):  # 可变Sans保存同一套轮廓的字重与字宽轴
        weight = 400    # 正文和小字先使用常规字重
        if index == 7:  # 第八套用于32px大读数
            weight = 300  # 大字号使用较轻笔画
        elif index in (2, 3, 6):  # 标签、标题和14px按钮使用中等字重
            weight = 500  # 当前三个下标共享此轴值
        font.set_variation_by_axes([weight, 100])  # Pillow按本字体wght、wdth轴顺序取得字重和标准字宽
    glyphs = []  # 当前字体的95个字符度量暂存于新列表
    for code in range(32, 127):  # range含32不含127，覆盖空格到波浪号的95个ASCII编码
        character = chr(code)  # 把整数ASCII编码变为一字符Python字符串
        x0, y0, x1, y1 = font.getbbox(character)  # 四元组解包为当前字形包围盒左上和右下坐标
        width, height = x1 - x0, y1 - y0  # 用两端坐标差得到字形像素尺寸
        bitmap = Image.new('L', (max(width, 1), max(height, 1)))  # L是每像素一字节灰度，空字形也借至少1×1的Pillow图片
        ImageDraw.Draw(bitmap).text((-x0, -y0), character, font=font, fill=255)  # 抵消包围盒偏移后将轮廓画成0到255覆盖率
        glyphs.append((len(data), width, height, x0, y0, round(font.getlength(character))))  # 加入一条六元组，offset取当前覆盖率长度，advance将浮点间距取最近整数
        if width * height:  # 只有实际字形有像素时才把图片内容加入大覆盖率数组
            data.extend(bitmap.tobytes())  # bytes逐项迭代得到整数，extend逐字节加入而非嵌套一块bytes
    metrics.append(glyphs)  # append把整套字符度量列表作为八套字体列表的一项

lines = ['/* Generated from locked OFL fonts by scripts/font_atlas.py. */',  # 开始收集生成C文件的源码行，第一行说明固定字体来源
         'struct u_glyph { unsigned offset; unsigned char w,h; signed char x,y; unsigned char advance; };',  # 输出一条字形记录类型：覆盖率偏移、尺寸、文字偏移和推进间距
         'static const struct u_glyph u_glyphs[8][95] = {']  # 八套字体各95字符的二维度量数组开始
for glyphs in metrics:  # 依字体枚举顺序输出每套95条字符度量
    lines.append('{')     # 生成C二维数组中当前字体这一层的开始括号
    for glyph in glyphs:  # 当前glyph是offset、尺寸、偏移和推进量的六元组
        values = ','.join(str(value) for value in glyph)  # 生成器逐项将整数转字符串，join用逗号组成C初始化字段
        lines.append('{' + values + '},')  # 把本字符度量包在C结构初始化括号中，加尾逗号
    lines.append('},')  # 结束当前字体的95条度量初始化
lines.extend(['};', 'static const unsigned char u_coverage[] = {'])  # 结束度量数组，再打开每元素一字节的覆盖率数组
for offset in range(0, len(data), 32):  # 每32个覆盖率整数形成一行源码，最后一行可少于32项
    values = ','.join(str(value) for value in data[offset:offset + 32])  # 切片取本批前缀，逐个转成十进制C整数文本
    lines.append(values + ',')  # 输出这一批覆盖率，末尾逗号允许下一批继续
lines.append('};')  # 结束覆盖率C数组
(assets / 'font-atlas.h').write_text('\n'.join(lines) + '\n')  # 按换行连接源码各行，再加末尾换行写生成头文件
print('Atlas:', len(data), 'bytes /', len(faces), 'faces')     # 报告真实生成覆盖率字节数和八套字体数量
```

生成器先核对全部输入摘要，任何TTF或许可不符都会在写输出前抛异常。`enumerate(faces)`同时给下标与当前二元组，下标选字重，二元组解包为文件名与字号。`getbbox`返回左上/右下四元组，差值给宽高；Pillow灰度模式L以一字节保存覆盖率。画字时传`(-x0,-y0)`抵消包围盒偏移，使临时图片只保存这一个字形的有效范围。advance来自`getlength`，Python的`round`取最近整数，恰好半整数时选择偶数。

C输出中的八行各保存95个可打印ASCII字形。六元组中的偏移用unsigned，尺寸和推进用unsigned char，相对偏移用signed char；这些固定字体的数值落在对应范围内。覆盖率每32项换一行只是输出排版，不是图集每字形的宽度。生成器不做整段文字的字偶距或复杂文字塑形，绘图按单字符取整后的advance推进，范围外的字节会显示问号。

在学习目录根目录下载已锁定的输入并生成数组。这里沿用[依赖下载](https://github.com/Airo-dynamic/Airi-OS-Test/blob/main/dev-log/02-dependencies.md#config-values)中实现的 `deps.download`：按锁文件取得字体与许可，再由本章生成器写出 `font-atlas.h`。

<!-- run -->
```sh
# 单引号heredoc结束词使Shell不展开内部Python，PYTHONPATH仅给此次进程补模块目录
PYTHONPATH=scripts python3 - <<'PY'  # 仅此进程搜索本目录scripts，单引号使随后文本保持原样
import json  # 把字体锁文本读成字典与资产记录列表
from deps import download  # 使用已实现的下载、摘要核对和临时文件替换
from tools import ROOT  # 字体与产物属于当前学习目录
assets = ROOT / 'frontend/assets'  # Path拼接出四份外部输入的保存目录
for record in json.loads((assets / 'fonts.lock.json').read_text())['assets']:  # 每轮取得file、url、sha256字典
    download(record['url'], assets / record['file'], record['sha256'])  # 下载或复用相同摘要的本目录缓存
PY
python3 -m venv build/font-env  # 独立依赖目录隔开Pillow与其他主机Python项目
build/font-env/bin/python -m pip install 'Pillow==12.3.0'  # 固定光栅化库，不向guest安装Python
build/font-env/bin/python scripts/font_atlas.py  # 从锁定轮廓重新生成八套度量和覆盖率
```

虚拟环境是带独立依赖目录的解释器环境，不模拟CPU。生成后的C数组编译进应用或内核；运行guest不需要Pillow。下载使用已有curl及Shell环境，如果环境要求 HTTPS 代理，可按依赖下载中的配置设置 `HTTPS_PROXY`，再重跑同一命令；连接错误发生在取得字节阶段，不是摘要已经通过。生成器输出：

```text
Atlas: 94785 bytes / 8 faces
```

`frontend/assets/font-atlas.h`的SHA-256为`aaf2aa574cd663809d864aed4d712cc5790cb5b03d9a5935b8481f8091c29f27`，与依赖锁中的图集摘要相同。它是生成资产，手敲内容是生成器和锁文件，覆盖率数组由上述输入与配方产生。

## 在已有背景上混合字形

<a id="alpha-blend"></a>

混合函数读取背景，再按覆盖率计算每个通道：`(前景*alpha + 背景*(255-alpha)+127)/255`。例如白色通道255覆盖黑色通道0，alpha=128时结果128；对半覆盖的边缘呈灰色，而非把128当作整幅像素的颜色。三个通道各自计算，127使整数除法就近取整。

读取背景与写回仍使用 G11 的实际 pitch 与位域布局；设备里的五位红先缩放为八位，再混合，再编码回设备位置。alpha为0时既不读也不写，255时直接走矩形写入。文字因此必须在背景铺好之后绘制，否则后来铺背景会把笔画盖掉。

追加到`frontend/render/draw.c`末尾。头文件在这里包含是合法C预处理：字形类型和数组只需在下面首次使用之前出现，前面的矩形函数不依赖它们。

<a id="code-frontend-render-draw-c-1"></a>

<!-- append: frontend/render/draw.c -->
```c
#include "../assets/font-atlas.h"  // 读取主机生成的八套字形度量和覆盖率
static void blend(int x, int y, uint32_t rgb, unsigned alpha) {  // 将0至255覆盖率的前景混入原像素
    if (!alpha || !u_inside(clip, x, y))  // 零覆盖率和裁剪外像素不需要读背景
        return;  // 保持原像素不变
    if (alpha == 255) {  // 完整覆盖可以直接使用既有写入路径
        u_rect(x, y, 1, 1, rgb);  // 写前景而不读取背景
        return;  // 当前像素已经处理完
    }
    unsigned bytes = fb->bpp / 8;  // 与矩形相同，24位用三字节，32位用四字节
    volatile uint8_t *p = (volatile uint8_t *)(uintptr_t)fb->virtual_address +  // 将借用的数值地址还原成字节指针
                          (uint64_t)y * fb->pitch + (uint64_t)x * bytes;  // 按实际行距定位待混合像素
    uint32_t old = 0;  // 从零开始恢复设备布局中的原像素数值
    for (unsigned k = 0; k < bytes; ++k)  // 读取当前像素各字节，不读取行尾填充
        old |= (uint32_t)p[k] << (k * 8);  // 提升到32位后按小端位置组合字节
    const unsigned shifts[] = {fb->red_shift, fb->green_shift, fb->blue_shift};  // 当前设备的红绿蓝最低位
    const unsigned sizes[] = {fb->red_size, fb->green_size, fb->blue_size};  // 与最低位按同下标对应的位宽
    unsigned channels[3];  // 保存混合后的三个八位通道
    for (unsigned k = 0; k < 3; ++k) {  // 每个通道独立混合，避免颜色位之间串入
        unsigned maximum = (1u << sizes[k]) - 1;  // 当前设备通道的最大值，例如五位为31
        unsigned before = ((old >> shifts[k]) & maximum) * 255u / maximum;  // 从设备位域还原成背景八位通道
        unsigned after = (rgb >> ((2 - k) * 8)) & 255u;  // 从0xRRGGBB取得前景八位通道
        channels[k] = (after * alpha + before * (255 - alpha) + 127) / 255;  // 覆盖率加权，加127使结果就近取整
    }
    uint32_t mixed = (channels[0] << 16) | (channels[1] << 8) | channels[2];  // 把三个八位结果组回RGB数值
    uint32_t color = native(mixed);  // 再编码成当前设备真正的位域排列
    for (unsigned k = 0; k < bytes; ++k)  // 将完整混合结果写回同一个像素
        p[k] = (uint8_t)(color >> (k * 8));  // 保持与矩形写入相同的小端字节次序
}
void u_text(int x, int y, const char *s, enum ui_font font, uint32_t color) {  // 将字符串叠到已经铺好的背景
    int origin = x;  // 换行时回到此次文字的起始列
    while (*s) {     // 遇到NUL结束，不读取字符串之后的内存
        unsigned char code = (unsigned char)*s;  // 按0至255读取当前字节，避免有符号char成为负下标
        ++s;  // 当前字节已经取出，指向下一个字符
        if (code == '\n') {  // 换行只改变下一字形的原点
            x = origin;  // 回到本段文字起始列
            y += font == U_CODE ? 24 : 20;  // 代码文字用24像素行距，其余用20像素
            continue;  // 换行不取覆盖率数组
        }
        if (code < 32 || code > 126)  // 固定图集只有可打印ASCII
            code = '?';  // 对未收录的字节使用同一问号字形
        const struct u_glyph *g = &u_glyphs[font][code - 32];  // 二维下标先选字体，再将ASCII32映到第0项
        if ((int64_t)x > (int64_t)clip.x + clip.w) {  // 当前行原点已超过右界，64位计算避免坐标相加溢出
            while (*s && *s != '\n')  // 只跳过本行余下字节，遇NUL或换行停止
                ++s;  // 当前行余下字符仍向右，无需继续读取字形覆盖率
            continue;  // 保留换行给外层下一轮处理，下一行仍回到origin
        }
        for (unsigned row = 0; row < g->h; ++row)  // 按字形局部行遍历包围盒
            for (unsigned col = 0; col < g->w; ++col) {  // 按字形局部列遍历覆盖率
                unsigned alpha = u_coverage[g->offset + row * g->w + col];  // 大数组偏移加局部行列位置
                blend(x + g->x + (int)col, y + g->y + (int)row, color, alpha);  // 原点加字形偏移才是屏幕位置
            }
        x += g->advance;  // 下一字符按推进间距开始，不按包围盒宽度开始
    }
}
int u_text_width(const char *s, enum ui_font font) {  // 返回单行字符串的推进距离，单位为软件像素
    int width = 0;     // 尚未处理任何字形时宽度为零
    for (; *s; ++s) {  // 初始指针已由调用者给出，每轮走到下一字节
        unsigned char code = (unsigned char)*s;  // 与绘制保持相同的无符号编码范围
        if (code < 32 || code > 126)  // 宽度计算也只能访问已收录字形
            code = '?';  // 替代规则与绘制一致，避免布局和外观不符
        width += u_glyphs[font][code - 32].advance;  // 累加原点之间的距离，空格也有advance
    }
    return width;  // 调用者用总推进距离进行居中或对齐
}
```

文字函数以字体下标和`code-32`定位字形，局部行列经offset取得覆盖率；文字原点加记录的x/y偏移才是要混合的屏幕位置。字形完成后按advance移动原点，换行回到起始x并增加行距。原点超过裁剪右界只说明本行后面的字形不可见，不能结束整个字符串：例如窄区域中的`WWWWWWWWW\nO`，第一行虽然超宽，第二行O仍应回到起始列。内层while把s推进到换行或NUL，保留换行给外层处理；因此裁剪不等于自动换行，也不会丢掉后面的显式新行。

当前 U_CODE 用 24 像素行距，其余字体用 20 像素；大字体观察单独为每套留50像素，不能依赖默认行距排32像素多行文本。

宽度函数用于单行布局，累加与绘制相同的advance。它不测量笔画最左/右的包围盒，也不计算多行最大宽度。下面粉线长度表示原点推进总量，字符实际笔画可能在原点之后才开始或略微伸出。

## 比较同一输入的字号、字重和推进距离

新建`labs/tutorial/fonts.c`。观察程序读取生成图集中A的真实度量，统计其0与255之间的边缘像素；这些读数由本次生成决定，而不是手写展示数据。它先在带两端哨兵的小画布中画超宽第一行和第二行O，逐像素确认裁剪外没有写入、第二行确实出现，再回到八套字体比较。小画布的描述只借用本函数栈内数组；返回后main立即用主画布重新调用u_begin，后续不再读取这个借用。

<a id="code-labs-tutorial-fonts-c-1"></a>

<!-- write: labs/tutorial/fonts.c -->
```c
#include "render.h"  // 使用上一章图元和本章文字接口
#include "../../frontend/assets/font-atlas.h"  // 观察程序只读同一生成数组以报告A的真实度量
#include <stdio.h>  // 输出逐字体数据和PPM
#define WIDTH 800   // 留出八行文字与度量标记的宽度
#define HEIGHT 420  // 每套字体占一行，避免大字相互覆盖
static uint32_t pixels[WIDTH * HEIGHT];     // 主机观察拥有像素数组
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
static bool clipped_lines(void) {  // 超宽第一行不能阻止第二行显式换行后的O出现
    enum { TEST_WIDTH = 64, TEST_HEIGHT = 64 };      // 小画布只用于这一项裁剪观察，尺寸单位为像素
    uint32_t guarded[TEST_WIDTH * TEST_HEIGHT + 2];  // 可见像素两端各预留一个四字节哨兵
    for (unsigned i = 0; i < TEST_WIDTH * TEST_HEIGHT + 2; ++i)  // 连同前后哨兵建立统一初态
        guarded[i] = 0xa5a5a5a5;  // 任何像素变化都能与未写区域区分
    struct airi_framebuffer canvas = {  // 描述只借用本函数数组的小画布
        .virtual_address = (uintptr_t)(guarded + 1),  // 跳过首哨兵，使绘图地址指向第一个有效像素
        .width = TEST_WIDTH, .height = TEST_HEIGHT,   // 字段使用上面的实际宽高
        .pitch = TEST_WIDTH * 4, .bpp = 32,  // 每行紧排64个四字节像素
        .red_size = 8, .red_shift = 16, .green_size = 8, .green_shift = 8,  // 当前RGB布局与主观察画布相同
        .blue_size = 8  // 未指定blue_shift时为零，蓝色占低八位
    };
    struct ui_rect region = {8, 8, 20, 42};  // 窄裁剪只允许列8..27、行8..49被修改
    u_begin(&canvas);  // 本项绘图期间描述和数组都仍在当前调用栈中
    u_clip(region);    // 先与画布求交，再由所有字形像素使用这块范围
    u_text(8, 8, "WWWWWWWWW\nO", U_BODY, 0xffffff);  // 第一行超宽，换行后O回到x=8且y=28
    unsigned visible = 0;  // 计数第二行实际改变的像素，而非只检查函数返回
    for (int y = 0; y < TEST_HEIGHT; ++y)  // 逐行观察整个小画布，包括裁剪外区域
        for (int x = 0; x < TEST_WIDTH; ++x) {  // 当前行逐列读取主机自己的有界数组
            bool changed = guarded[1 + y * TEST_WIDTH + x] != 0xa5a5a5a5;  // 行列换成数组下标，并跳过首哨兵
            if (!u_inside(region, x, y) && changed)  // 裁剪外出现修改说明字形越过了当前边界
                return false;  // 本观察失败，main将返回非零状态
            if (y >= 28 && changed)  // 第一行字形已结束，y=28开始只能来自第二行O
                ++visible;  // 至少一个可见字形像素才说明没有丢掉后续新行
        }
    if (guarded[0] != 0xa5a5a5a5 || guarded[TEST_WIDTH * TEST_HEIGHT + 1] != 0xa5a5a5a5)  // 图元也不能写到像素数组两端
        return false;  // 任一哨兵变化都使本观察失败
    printf("clipped newline: second-line pixels=%u; guards=unchanged\n", visible);  // 保存真正绘出的第二行像素数量
    return visible > 0;  // 零像素说明超宽第一行仍阻断了第二行
}
int main(void) {  // 每套字体显示同一字符串以比较间距和字形
    if (!clipped_lines())  // 先观察超宽行、显式换行和裁剪边界
        return 1;  // 图元行为不同于声明合同时不继续报告字体观察完成
    struct airi_framebuffer canvas = {  // 描述本进程持有的数组，未指定字段按C规则置零
        .virtual_address = (uintptr_t)pixels,  // 地址属于主机进程，绘图借用而不释放
        .width = WIDTH, .height = HEIGHT,      // 宽高按像素计，读取者据此裁剪
        .pitch = WIDTH * 4, .bpp = 32,      // 当前观察每行紧排四字节像素
        .red_size = 8, .red_shift = 16,     // 红色使用数值的第16至23位
        .green_size = 8, .green_shift = 8,  // 绿色使用第8至15位
        .blue_size = 8, .blue_shift = 0     // 蓝色使用第0至7位
    };
    u_begin(&canvas);  // 本函数结束前数组与局部描述都保持有效
    const char *names[] = {"Small 12/400", "Body 14/400", "Label 16/500", "Title 22/500",  // 比例字体的实际字号和字重
                           "Mono 14", "Code 16", "Medium 14/500", "Display 32/300"};  // 等宽字体及新增中等和轻字重
    const char *sample = "AiriOS / iii WWW 0123";  // 同样输入让字重与字体差异可比较
    u_rect(0, 0, WIDTH, HEIGHT, 0x0d131c);  // 背景先形成，文字混合读取这个背景
    for (unsigned index = 0; index < 8; ++index) {  // 每轮观察生成器的一套字体
        enum ui_font face = (enum ui_font)index;  // 已限定0至7，下标可作为字体枚举传入
        int y = 14 + (int)index * 50;  // 当前字体的文字原点，每套相距50像素
        int width = u_text_width(sample, face);  // 取得相同样本文字的总推进距离
        u_text(12, y + 8, names[index], U_SMALL, 0xa9b2c6);  // 左侧小字标明当前字体参数
        u_text(200, y, sample, face, 0xe7ebf5);  // 八套字体画同样字符串，颜色一致
        u_line(200, y + 40, 200 + width, y + 40, 0xe5a6c7);    // 粉线长度对应推进量而非笔画像素的包围盒
        const struct u_glyph *g = &u_glyphs[index]['A' - 32];  // 固定观察ASCII65，对应每套的第33项
        unsigned partial = 0;  // 累计A字形中部分覆盖的像素数
        for (unsigned n = 0; n < (unsigned)g->w * g->h; ++n) {  // 只访问A自己的覆盖率范围
            unsigned alpha = u_coverage[g->offset + n];  // 从同一大数组取得当前覆盖率
            if (alpha > 0 && alpha < 255)  // 边缘像素既非空白也非完全覆盖
                ++partial;  // 一个部分覆盖像素贡献一次计数
        }
        printf("%s: width=%d A=%ux%u advance=%u partial=%u\n", names[index], width,  // 报告本次真实生成与布局数据
               (unsigned)g->w, (unsigned)g->h, (unsigned)g->advance, partial);  // 小整数字段显式提升成与%u匹配的unsigned
    }
    if (!save_image("build/fonts.ppm"))  // 检查全部像素写入和关闭结果
        return 1;  // 不完整的图像使主机观察失败
    puts("wrote build/fonts.ppm");  // 报告八套字体比较文件
    return 0;  // 主机字体观察完成
}
```

clipped_lines把图元地址指向guarded+1，首尾两个uint32_t只用作哨兵。裁剪矩形是半开范围，列8到27、行8到49；U_BODY行距20，使第二行原点从(8,8)回到(8,28)。先确认全部裁剪外像素不变，再统计第二行实际改变的像素。visible不是字形包围盒面积，零覆盖率的空白位置不会改变背景。

每套字体都有同样的样本，左侧名称使用U_SMALL，右侧才使用正在比较的face。字形A的ASCII值65，数组下标65−32=33；`partial`遍历它自己的宽×高覆盖率范围，只计部分覆盖像素。U_MONO和U_CODE采用等宽字体，`iii`与`WWW`每个字符的advance一致；比例字体会为不同字符保留不同距离。

运行：

<!-- run -->
```sh
mkdir -p build  # 保留当前学习目录的生成与观察产物
cc -std=c11 -O0 -g -Wall -Wextra -Werror -Iinclude -Ifrontend/include labs/tutorial/fonts.c frontend/render/draw.c -o build/fonts  # 文字实现与观察程序共同读取刚生成的图集
./build/fonts  # 输出真实度量并保存八套字体比较图
```

输出如下，width表示完整样本的推进距离，A尺寸表示一个字形的包围盒，partial是边缘部分覆盖像素数。

```text
clipped newline: second-line pixels=58; guards=unchanged
Small 12/400: width=121 A=8x8 advance=8 partial=37
Body 14/400: width=137 A=9x10 advance=9 partial=41
Label 16/500: width=166 A=11x11 advance=11 partial=51
Title 22/500: width=223 A=15x15 advance=15 partial=66
Mono 14: width=168 A=8x10 advance=8 partial=42
Code 16: width=210 A=10x11 advance=10 partial=47
Medium 14/500: width=141 A=9x10 advance=9 partial=38
Display 32/300: width=314 A=20x22 advance=20 partial=114
wrote build/fonts.ppm
```

`build/fonts.ppm`中有八行相同文字，粉线对应各自推进距离。14/400与14/500的字号相同，但度量与笔画覆盖率不同。接着为一组按钮同时选择底面、文字与焦点颜色，绘图接口需要表示这些用途，再在日夜两套颜色中解析。
