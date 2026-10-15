#include "ui.h"      // 引入前端服务合同与桌面状态
#include <stdio.h>   // 主机观察使用标准文件输出与打印接口
#include <stdlib.h>  // 主机观察使用atoi转换应用参数
static uint32_t pixels[1280 * 800];  // 主机观察程序拥有静态像素数组，绘图描述只借用它
static struct ui_desktop desktop;    // 主机观察拥有整份静态桌面，多个步骤修改同一对象
static struct airi_console console;  // 主机观察拥有同一console输入输出状态
int main(int argc, char **argv) {    // 在主机准备输入、运行当前观察并以退出码报告结果
    struct ui_backend backend;     // 观察程序持有回调合同副本，初始化后才可调用
    ui_fixture_backend(&backend);  // 把服务回调写入本观察程序的 backend 结构，并重置参考静态记录
    airi_console_init(&console, backend.boot);  // 观察console与System借用同一参考启动结构，日志和面板来源一致
    ui_init(&desktop, backend, &console);  // 同一desktop借用console并复制后端合同，后续每次操作都修改这份状态
    int app = argc > 1 ? atoi(argv[1]) : UI_TERMINAL;  // 有参数时把十进制字符串变成应用编号，否则生成Terminal
    if (app < 0 || app >= UI_APP_COUNT)  // 检查转换结果的数组边界后才索引窗口
        return 1;  // 当前主机观察失败，以非零状态阻止后续误判通过
    if (app == UI_VIEWER)  // 图片进程需要自己加载数据，不能借用另一个进程的数组
        ui_open_document(&desktop, 1);  // 本参考快照第1项是series.csv，成功解析后发布六个点
    ui_open(&desktop, (enum ui_app)app);    // 打开或置顶一个应用，更新窗口次序和活动编号，本次实参取自当前桌面状态
    desktop.windows[app].maximized = true;  // 观察程序先把目标窗口设为最大化，绘图据此计算实际矩形
    struct airi_framebuffer fb = {  // 当前操作使用的airi_framebuffer记录布局
        .virtual_address = (uintptr_t)pixels, .width = 1280, .height = 800,  // 设置像素数组的主机虚拟地址、软件画布像素宽度、软件画布像素高度
        .pitch = 1280 * 4, .bpp = 32, .red_size = 8, .red_shift = 16,  // 设置每行实际字节跨度、每像素位数、红色通道位数、红色通道最低位位置
        .green_size = 8, .green_shift = 8, .blue_size = 8};  // 设置绿色通道位数、绿色通道最低位位置、蓝色通道位数
    ui_draw(&desktop, &fb);  // 按窗口层次画整帧，并将对话框留在最后，本次实参取自当前桌面状态
    FILE *file = fopen("build/desktop.ppm", "wb");  // 以二进制写并截断旧内容打开本次桌面PPM文件
    if (!file)  // 打开PPM输出失败时不继续写文件
        return 1;  // 当前主机观察失败，以非零状态阻止后续误判通过
    fprintf(file, "P6\n1280 800\n255\n");  // 把PPM魔数、尺寸和255通道上限写到主机输出文件
    for (unsigned i = 0; i < 1280 * 800; ++i) {  // 逐个像素向PPM输出1280×800个RGB三字节
        unsigned char rgb[] = {(pixels[i] >> 16) & 255, (pixels[i] >> 8) & 255, pixels[i] & 255};  // 从像素数值按红绿蓝顺序提取3个八位通道，形成与主机字节序无关的PPM字节
        if (fwrite(rgb, 1, 3, file) != 3)  // 每像素应写3个RGB字节，短写即报告失败
            return 1;  // 当前主机观察失败，以非零状态阻止后续误判通过
    }
    if (fclose(file))  // 关闭时检查缓冲输出是否真正交给主机文件系统接口
        return 1;  // 当前主机观察失败，以非零状态阻止后续误判通过
    printf("app=%d -> build/desktop.ppm\n", app);  // 向主机终端报告本次观察的实际状态或输出位置
    return 0;  // 观察完整通过，主机进程以成功状态退出
}
