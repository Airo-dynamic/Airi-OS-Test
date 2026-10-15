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
