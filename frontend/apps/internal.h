#ifndef AIRI_APPS_INTERNAL_H  // 防止应用共享接口重复包含
#define AIRI_APPS_INTERNAL_H  // 标记应用共享接口已经包含
#include "ui.h"  // 引入前端服务合同与桌面状态
#include <airi/base.h>  // 引入自有字节操作和有界格式化函数

void ui_app_text(int x, int y, const char *s);   // 用正文体和主要颜色绘制应用文字
void ui_app_muted(int x, int y, const char *s);  // 用小字体和次要颜色绘制说明文字
void ui_app_field(struct ui_rect r, const char *s, bool focused);  // 画输入字段和当前焦点边框
bool ui_app_match(const char *s, const char *q);  // 按字节判断s是否包含q；Terminal识别提示符，Files和Tasks筛选各自使用结果
void ui_app_dialog(struct ui_desktop *s, int kind, int target, const char *title, const char *body,  // 把种类、目标身份和文案保存到桌面对话框状态
                   const char *value);  // value是对话框输入字段的初始文字，函数只复制状态
void ui_app_unavailable(struct ui_desktop *s, struct ui_rect r, const char *service);  // 按服务状态绘制加载、空或不可用提示
void ui_app_toolbar(struct ui_rect r, const char *label);  // 在应用内容顶部绘制44像素工具栏
void ui_app_status(struct ui_rect r, const char *left, const char *right);  // 在应用内容底部绘制27像素状态栏
void ui_terminal_draw(struct ui_desktop *, struct ui_rect);  // 把console逻辑行拆成窗口显示行并绘制输入
void ui_files_draw(struct ui_desktop *, struct ui_rect);     // 读取目录快照和排序索引，画路径、文件行和状态
void ui_editor_draw(struct ui_desktop *, struct ui_rect);    // 将文档字节偏移换成行列，画文字、选区和光标
void ui_tasks_draw(struct ui_desktop *, struct ui_rect);     // 绘制任务快照、CPU曲线和内存条
void ui_system_draw(struct ui_desktop *, struct ui_rect);    // 按当前页显示同一启动信息的内存、设备或日志
void ui_settings_draw(struct ui_desktop *, struct ui_rect);  // 按页面绘制当前会话配色、密度和接口说明
void ui_settings_click(struct ui_desktop *, struct ui_rect, int, int, bool);  // 分别命中页签、主题和密度矩形；只改变当前会话偏好
void ui_viewer_draw(struct ui_desktop *, struct ui_rect);  // 把已提交时间和测量值缩放到真实时间比例的像素曲线
void ui_open_series(struct ui_desktop *, const struct ui_file *);  // 读取CSV并在整份验证通过后提交成对时间数值数组
#endif  // 结束当前应用接口的重复包含保护
