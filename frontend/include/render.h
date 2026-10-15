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
