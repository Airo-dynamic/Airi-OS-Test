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
enum ui_color {  // 数值从24位RGB范围之外开始，名字表示用途而非固定像素
    U_BG = 0x1000000,  // 整幅画布背景，第一个角色的编号
    U_PANEL,    // 标题、工具栏和普通按钮的底面
    U_SURFACE,  // 正文内容区域
    U_RAISED,   // 被选择的列表背景
    U_EDGE,     // 轻分隔和不活动外框
    U_TEXT,     // 主要正文
    U_MUTED,    // 次要说明
    U_ACCENT,   // 粉色主动作与身份标记
    U_BLUE,     // 蓝色焦点与辅助强调
    U_RED,      // 错误和危险操作
    U_WARM,     // 警示与代码数值
    U_PINK,     // 独立身份色，区别于可操作强调
    U_CONTROL,  // 普通可操作边界
    U_SHADOW,     // 窗口或对话框阴影
    U_HOVER,      // 悬停按钮底面
    U_PRESSED,    // 按下按钮底面
    U_DISABLED,   // 禁用文字
    U_SUCCESS,    // 成功状态
    U_ON_ACCENT,  // 粉色实面上的文字
    U_CHROME,  // 标题身份片的表面
    U_INK,     // 标题身份片的前景
    U_COLOR_END  // 半开角色终点，只用于边界检查，不是可绘制颜色
};
enum ui_theme { UI_NIGHT, UI_DAY };  // 下标0夜间、1日间，零初始化自然选择夜间
enum ui_metric { UI_CONTROL_RADIUS = 2 };  // 本章只需要按钮的2像素圆角，不预置窗口尺寸
enum ui_control_state {  // 不同状态占不同二进制位，可以按位或组合
    U_NORMAL = 0,    // 没有任何额外状态位
    U_SELECTED = 1,  // 第0位：当前已选中的动作
    U_HOT = 2,      // 第1位：悬停反馈
    U_DOWN = 4,     // 第2位：按下反馈
    U_FOCUSED = 8,  // 第3位：键盘焦点
    U_INACTIVE = 16  // 第4位：服务不可用，覆盖其他交互强调
};
void u_theme(unsigned);      // 选择本轮绘图调色板，不拥有应用的会话数据
uint32_t u_color(uint32_t);  // 角色解析成RGB，0至0xffffff的真实RGB原样通过
void u_round(struct ui_rect, int, uint32_t);  // 圆角边缘按4×4样本混合，仍遵守裁剪
void u_control(struct ui_rect, const char *, unsigned);   // 按调用者明确给出的状态绘制按钮，不执行动作
void u_button(struct ui_rect, const char *, bool, bool);  // 将selected与disabled转换为共同状态表
#endif  // 结束包含保护
