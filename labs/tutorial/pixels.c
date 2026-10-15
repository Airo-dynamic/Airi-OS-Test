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
