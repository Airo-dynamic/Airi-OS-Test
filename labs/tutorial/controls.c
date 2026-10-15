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
