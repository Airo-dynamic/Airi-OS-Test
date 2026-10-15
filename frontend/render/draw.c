#include "render.h"  // 取得矩形、画布描述与本文件提供的接口
#include "../assets/font-atlas.h"  // 借用主机生成的八套字形度量和覆盖率
static const struct airi_framebuffer *fb;  // 借用本次调用者持有的描述，不拥有像素内存
static bool standard;  // 三个8位通道是否恰好组成0xRRGGBB
static struct ui_rect clip;  // 所有图元共同使用的当前可写范围
static int pointer_x, pointer_y;  // 当前绘制区域借用的整屏指针坐标
static bool pointer_down, interactive;  // 按压状态与当前区域是否有资格显示反馈
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
static void blend(int x, int y, uint32_t rgb, unsigned alpha) {  // 将0至255覆盖率的前景混入原像素
    if (!alpha || !u_inside(clip, x, y))  // 零覆盖率和裁剪外像素不需要读背景
        return;  // 保持原像素不变
    rgb = u_color(rgb);  // 混合前把角色变成真实RGB，混合结果不会重新成为角色
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
    if (interactive && u_inside(r, pointer_x, pointer_y)) {  // 只有允许响应且指针在按钮半开矩形内时加入反馈
        state |= U_HOT;    // 按位或加入悬停位，不改变已有选择或禁用位
        if (pointer_down)  // 指针在此按钮内且鼠标仍按着
            state |= U_DOWN;  // 本帧加入按压反馈，服务动作仍由输入分派执行
    }
    u_control(r, label, state);  // 只把组合状态转换成像素，不调用任何应用或服务
}

int u_label_width(const char *s) {  // 身份标签使用Medium推进量，每个字符另加两像素，测量与绘图一致。
    int width = u_text_width(s, U_MEDIUM);  // 取得这条单行标签的全部字形推进量。
    for (const char *p = s; *p; ++p)  // 同u_label一样逐字节处理，ASCII标签每项留两像素字距。
        width += 2;  // 包括最后字形后的留白，使承载身份片有足够范围。
    return width;  // 返回软件画布中的像素距离，不是字符串字节数。
}

void u_label(int x, int y, const char *s, uint32_t color) {  // 标签按逐字推进加2像素字距，限于短分区名
    for (; *s; ++s) {  // 正常文件名与正文不经过这个函数
        char ch[2] = {*s, 0};  // 单字符形成合法NUL字符串复用字形绘制
        u_text(x, y, ch, U_MEDIUM, color);    // 使用已生成的14像素、字重500字体
        x += u_text_width(ch, U_MEDIUM) + 2;  // 字宽加固定空隙构成稳定标签节奏
    }
}
void u_plate(struct ui_rect r, uint32_t color) {  // 斜切片保持矩形左缘，右端随行逐步收缩
    for (int y = 0; y < r.h; ++y)  // 每行仍由原矩形图元执行裁剪
        u_rect(r.x, r.y + y, r.w - y / 3, 1, color);  // 约18度右缘使标题身份片区别于内容面板
}
void u_brand(int x, int y) {  // 两条斜边和悬浮横梁组成原创A形标识
    for (int i = 0; i < 3; ++i) {  // 三条相邻线提供系统标识所需的笔画重量
        u_line(x + i, y + 20, x + 10 + i, y, U_INK);  // 左侧上升边
        u_line(x + 10 + i, y, x + 20 + i, y + 20, U_INK);  // 右侧下降边
    }
    u_rect(x + 7, y + 13, 10, 3, U_PINK);  // 粉色横梁只承担产品身份
}

void u_interaction(int x, int y, bool down, bool enabled) {  // 每个绘制区域显式设置它可以响应的指针状态
    pointer_x = x;  // 下一次按钮绘制用这个整屏横坐标命中
    pointer_y = y;  // 纵坐标与按钮矩形采用同一整屏坐标系
    pointer_down = down;    // 当前按压由事件入口保存，绘图不产生按下事件
    interactive = enabled;  // 底层窗口或被对话框遮挡的区域关闭反馈
}
