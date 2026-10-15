#include "ui.h"      // 取得会话、窗口与服务的共享接口
#include <stdio.h>   // 主机观察使用puts输出实际结果
#include <string.h>  // 主机比较文档副本和窗口状态的字节
static struct ui_desktop desktop;    // 观察前后修改同一桌面，不重新初始化
static struct airi_console console;  // 保存文本输入与日志；主题偏好由同一desktop持有
int main(void) {  // 在普通主机进程观察设置操作留下哪些状态
    struct ui_backend backend;     // 参考服务回调表，桌面按值复制
    ui_fixture_backend(&backend);  // 创建本会话参考文件和记录
    airi_console_init(&console, backend.boot);  // 控制台借用同一启动描述
    ui_init(&desktop, backend, &console);      // 读入main.lua并建立初始窗口
    ui_key(&desktop, UI_SELECT_ALL, false);    // 先在Editor选中整份初始文档
    strcpy(desktop.clipboard, "print(42)\n");  // 十字节替换内容放进独立剪贴板
    ui_key(&desktop, UI_PASTE, false);  // 替换工作正文，保存点仍为原文件
    ui_key(&desktop, UI_SELECT_ALL, false);  // 让主题切换时存在一个实际选区
    ui_open(&desktop, UI_SETTINGS);  // 设置成为活动窗口，编辑文档继续保留
    struct ui_rect body = u_window_body(desktop.windows[UI_SETTINGS].rect);  // 由同一外框扣除48像素标题与边框
    char text[UI_DOC_SIZE], saved[UI_DOC_SIZE];   // 两份独立观察副本，不借用会改变的数组
    memcpy(text, desktop.editor, sizeof(text));   // 保存切换前的工作文字全部字节
    memcpy(saved, desktop.saved, sizeof(saved));  // 保存切换前的服务保存点
    unsigned cursor = desktop.cursor, anchor = desktop.anchor;  // 记录选区的两个字节间隙
    bool dirty = desktop.dirty, selection = desktop.selection;  // 记录未保存标记和选区有效性
    struct ui_window windows[UI_APP_COUNT];  // 观察窗口位置、页签与可见状态
    memcpy(windows, desktop.windows, sizeof(windows));  // 保存全部窗口字节，设置点击不应改它们
    ui_apps_click(&desktop, UI_SETTINGS, body, body.x + 28 + 138 + 10, body.y + 166, false);  // 命中Day按钮内部，直接调用已接入的内容分派
    if (desktop.theme != UI_DAY)  // 桌面拥有的呈现偏好应立即变为Day
        return 1;  // 模式未改变，后续状态观察不能视为成功
    ui_apps_click(&desktop, UI_SETTINGS, body, body.x + 28 + 10, body.y + 166, false);  // 再命中Night按钮，沿同一内容接口返回夜间
    if (desktop.theme != UI_NIGHT)  // Night同样写入桌面自己持有的状态
        return 1;  // 第二次切换没有正确执行
    ui_apps_click(&desktop, UI_SETTINGS, body, body.x + 160, body.y + 266, false);  // 点两密度按钮之间的十二像素空隙，不能改变初始宽松模式。
    ui_apps_click(&desktop, UI_SETTINGS, body, body.x + body.w - 1, body.y + 266, false);  // 点同一高度的右侧空白，也不是Compact按钮。
    if (desktop.density)  // 两次未命中都必须保持0。
        return 1;  // 宽泛横条命中会在右侧空白处错误切到紧凑模式。
    ui_apps_click(&desktop, UI_SETTINGS, body, body.x + 176, body.y + 266, false);  // 真正点Compact矩形内部。
    if (desktop.density != 1)  // 正确按钮需要产生真实状态改变。
        return 1;  // 不能通过忽略全部输入来满足空白不变。
    ui_apps_click(&desktop, UI_SETTINGS, body, body.x + 38, body.y + 266, false);  // 再点Comfortable矩形内部返回宽松模式。
    if (desktop.density)  // 返回0，后面的文档比较沿原来的密度继续。
        return 1;  // 第二个实际按钮也需要正确分派。
    puts("density: gaps and blank preserve state; both buttons change it");  // 报告同一会话中四次真实点击的结果。
    if (memcmp(text, desktop.editor, sizeof(text)) || memcmp(saved, desktop.saved, sizeof(saved)))  // 两个文档数组必须保留每个字节
        return 1;  // 外观操作误改了文档内容或保存点
    if (cursor != desktop.cursor || anchor != desktop.anchor || dirty != desktop.dirty || selection != desktop.selection)  // 文档的选择和保存状态也应保留
        return 1;  // 模式以外的文档状态发生变化
    if (memcmp(windows, desktop.windows, sizeof(windows)))  // 全部窗口几何与页面字节仍应相同
        return 1;  // 主题按钮不应重新建立窗口
    puts("Day -> Night: text, saved, selection and windows preserved");  // 输出这次实际状态比较完成的结果
    return 0;  // 所有观察均符合当前设置合同
}
