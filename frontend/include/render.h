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
