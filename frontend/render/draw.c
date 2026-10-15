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
