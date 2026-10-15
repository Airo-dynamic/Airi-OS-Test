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
