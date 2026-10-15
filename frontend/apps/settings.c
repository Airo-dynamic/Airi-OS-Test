#include "internal.h"  // 引入当前目录内部共享接口和基础函数

void ui_settings_draw(struct ui_desktop *s, struct ui_rect r) {  // 按页面绘制当前会话配色、密度和接口说明
    const char *tabs[] = {"Appearance", "Keyboard", "Display", "About"};  // Settings四页名称按tab编号排列
    unsigned tab = s->windows[UI_SETTINGS].tab;  // Settings自己的页下标选外观、输入说明或关于页，不复用System页状态
    for (unsigned i = 0; i < 4; ++i)  // 只画Settings四个有效页面按钮
        u_button((struct ui_rect){r.x + 16 + (int)i * 118, r.y + 9, 108, 28}, tabs[i], tab == i,  // 画当前页面按钮名按钮；其矩形供后续事件命中共用，当前仅输出像素
                 false);  // 当前按钮未禁用，实参只控制绘图外观
    int x = r.x + 28, y = r.y + 73;  // Settings正文从内容左边28像素、顶边73像素开始，标题和页签在上方
    u_text(x, y, tabs[tab], U_LABEL, U_TEXT);  // 在整屏x=x、整屏y=y显示已选页面名
    if (tab == 0) {  // Settings第0页显示当前会话日夜模式与界面密度
        ui_app_text(x, y + 49, "Appearance mode");  // 在整屏x=x、整屏y=y + 49显示文字"Appearance mode"
        const char *names[] = {"Night", "Day"};     // 日夜名称顺序与 ui_theme 一致
        for (unsigned i = 0; i < 2; ++i)  // 画两个可选择的会话模式按钮
            u_button((struct ui_rect){x + (int)i * 138, y + 83, 126, 34}, names[i], s->theme == i,  // 画当前模式名称按钮；其矩形供后续事件命中共用，当前仅输出像素
                     false);  // 当前按钮未禁用，实参只控制绘图外观
        ui_app_text(x, y + 148, "Interface density");  // 在整屏x=x、整屏y=y + 148显示文字"Interface density"
        u_button((struct ui_rect){x, y + 183, 126, 34}, "Comfortable", !s->density, false);  // 画文字"Comfortable"按钮；其矩形供后续事件命中共用，当前仅输出像素
        u_button((struct ui_rect){x + 138, y + 183, 126, 34}, "Compact", s->density, false);  // 画文字"Compact"按钮；其矩形供后续事件命中共用，当前仅输出像素
        ui_app_muted(x, y + 244, "Plex Sans / interface   -   Plex Mono / terminal and code");  // 在整屏x=x、整屏y=y + 244显示文字"Plex Sans / interface   -   Plex Mono / terminal and code"
    } else if (tab == 1) {  // Settings第1页显示当前键位与快捷动作说明
        ui_app_text(x, y + 48, "US keyboard layout");      // 在整屏x=x、整屏y=y + 48显示文字"US keyboard layout"
        const char *keys[] = {"Alt-Tab / switch windows",  // 按行保存快捷动作说明，这些文字不是事件处理实现
                              "Ctrl-S / save document",   // 独立快捷键说明数组项，逗号把它与下一项分开
                              "Ctrl-F / find or filter",  // 独立快捷键说明数组项，逗号把它与下一项分开
                              "Ctrl-A, C, X, V / select and clipboard",  // 独立快捷键说明数组项，逗号把它与下一项分开
                              "Ctrl-Z / undo",  // 独立快捷键说明数组项，逗号把它与下一项分开
                              "F5 / request program execution",  // 独立快捷键说明数组项，逗号把它与下一项分开
                              "Esc / close dialog or search"};   // 输入说明数组的最后一项，告知Escape关闭弹框或查找
        for (unsigned i = 0; i < 7; ++i)  // 逐行显示7条当前快捷键说明
            ui_app_text(x, y + 88 + (int)i * 32, keys[i]);  // 在整屏x=x、整屏y=y + 88 + (int)i * 32显示当前快捷键说明
    } else if (tab == 2) {  // Display页显示软件画布尺寸和显示方式
        char size[80];  // 80字节局部数组显示当前软件画布的像素尺寸和渲染方式
        airi_snprintf(size, 80, "%d x %d / software-rendered", s->width, s->height);  // Settings的Display页把软件画布像素宽高与绘图方式格式化成文字
        ui_app_text(x, y + 49, size);  // 在Display页显示刚格式化的软件画布宽高，不是文件字节数
        ui_app_text(x, y + 90, "1280 x 800 / baseline layout");  // 在整屏x=x、整屏y=y + 90显示文字"1280 x 800 / baseline layout"
        ui_app_text(x, y + 126, "1024 x 768 / compact layout");  // 在整屏x=x、整屏y=y + 126显示文字"1024 x 768 / compact layout"
        ui_app_text(x, y + 162, "1600 x 900 / expanded workspace");  // 在整屏x=x、整屏y=y + 162显示文字"1600 x 900 / expanded workspace"
        ui_app_muted(x, y + 225, "Window geometry adapts to the available work area.");  // 在整屏x=x、整屏y=y + 225显示文字"Window geometry adapts to the available work area."
    } else {  // 前一分支条件未成立时处理当前替代路径
        ui_app_text(x, y + 49, "AiriOS");  // 在整屏x=x、整屏y=y + 49显示文字"AiriOS"
        ui_app_text(x, y + 89, "Personal workstation");  // 在整屏x=x、整屏y=y + 89显示文字"Personal workstation"
        ui_app_muted(x, y + 142, "Original interface code: MIT");  // 在整屏x=x、整屏y=y + 142显示文字"Original interface code: MIT"
        ui_app_muted(x, y + 174, "IBM Plex fonts: SIL Open Font License");  // 在整屏x=x、整屏y=y + 174显示文字"IBM Plex fonts: SIL Open Font License"
    }
    ui_app_status(r, "Preferences apply to this session.", NULL);  // 在内容底部显示文字"Preferences apply to this session."；右侧说明按测得的文字宽度对齐
}
void ui_settings_click(struct ui_desktop *s, struct ui_rect r, int x, int y, bool twice) {  // 设置点击只修改共享界面选项，不写系统配置文件。
    (void)twice;  // 统一点击接口保留双击参数；这个应用当前只使用点击位置。
    enum ui_app app = UI_SETTINGS;  // 页签状态属于Settings窗口。
    if (y < r.y + 45) {  // 内容顶部45像素属于页签区。
        int tab = (x - r.x - 16) / 118;  // 按16像素内边距和118像素页签宽度定位。
        if (tab >= 0 && tab < 4)  // Settings只接受0..3页签。
            s->windows[app].tab = (unsigned)tab;  // 边界检查后再转无符号下标。
        return;  // 本次事件处理已结束，不再落入其他按钮或底层窗口。
    }
    if (s->windows[app].tab == 0) {  // 当前可修改主题选项位于首页。
        for (unsigned i = 0; i < 2; ++i)  // 用与绘制完全相同的矩形，间隙不属于按钮
            if (u_inside((struct ui_rect){r.x + 28 + (int)i * 138, r.y + 156, 126, 34}, x, y)) {  // Night与Day的真实点击区
                s->theme = i;  // 不调用初始化，不触碰文档和窗口状态
            }
        for (unsigned i = 0; i < 2; ++i)  // 密度也逐个命中真实按钮，不把整行空白当动作区。
            if (u_inside((struct ui_rect){r.x + 28 + (int)i * 138, r.y + 256, 126, 34}, x, y))  // 两按钮之间十二像素空隙不属于任何矩形。
                s->density = i;  // 下标0宽松、1紧凑，行高选择与实际命中对象一致。
    }
}
