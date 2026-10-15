#include <airi/base.h>  // 声明回归观察中使用的自有有界字符串复制。
#include "ui.h"      // 引入前端服务合同与桌面状态
#include <stdio.h>   // 主机观察使用标准文件输出与打印接口
#include <string.h>  // 主机观察使用字符串比较检查实验结果

static struct ui_desktop desktop;    // 主机观察拥有整份静态桌面，多个步骤修改同一对象
static struct airi_console console;  // 主机观察拥有同一console输入输出状态
static void observe(const char *step) {  // 打印同一桌面在当前步骤的光标、保存和错误状态
    printf("%s: cursor=%u dirty=%u dialog=%d editor_bytes=%zu\n", step, desktop.cursor,  // 向主机终端报告本次观察的实际状态或输出位置
           desktop.dirty ? 1u : 0u, desktop.dialog, strlen(desktop.editor));  // 将bool转换为%u所需unsigned，打印对话框种类与不含NUL的实际字节数
}
int main(void) {  // 在主机准备输入、运行当前观察并以退出码报告结果
    struct ui_backend backend;     // 观察程序持有回调合同副本，初始化后才可调用
    ui_fixture_backend(&backend);  // 把服务回调写入本观察程序的 backend 结构，并重置参考静态记录
    airi_console_init(&console, backend.boot);  // 观察console与System借用同一参考启动结构，日志和面板来源一致
    ui_init(&desktop, backend, &console);  // 同一desktop借用console并复制后端合同，后续每次操作都修改这份状态
    observe("open");  // 记录刚读取main.lua后的干净文档状态，作为后续比较起点
    ui_key(&desktop, UI_SELECT_ALL, false);    // 把当前文档从首间隙到文尾选中，false表示此动作不伴随Shift扩选
    strcpy(desktop.clipboard, "print(42)\n");  // 本观察用容量已确定的剪贴板数组保存十字节替换文字
    ui_key(&desktop, UI_PASTE, false);  // 用文字剪贴板替换整份选区，沿容量与ASCII检查路径完成一次修改
    observe("replace");  // 记录全选替换后的10字节正文、文尾光标与dirty=true
    if (!desktop.dirty || strcmp(desktop.editor, "print(42)\n"))  // 要求替换正文完全相同且已标为未保存，任一条件失败即停止观察
        return 1;  // 当前主机观察失败，以非零状态阻止后续误判通过
    ui_key(&desktop, UI_UNDO, false);  // 恢复全选替换之前的main.lua，它与读取时保存点相同
    observe("undo");  // 观察恢复原文后dirty=false，撤销不改服务正文
    if (desktop.dirty || !strcmp(desktop.editor, "print(42)\n"))  // 要求恢复原文且不再未保存，不能仍停留在替换文字
        return 1;  // 撤销观察不符合预期时立即失败
    ui_key(&desktop, UI_UNDO, false);  // 再交换一次唯一副本，回到尚未提交的十字节替换文字
    observe("restore replacement");    // 此时dirty再次为true，下一步保存这份替换结果
    if (!desktop.dirty || strcmp(desktop.editor, "print(42)\n"))  // 要求替换正文完全相同且已标为未保存，任一条件失败即停止观察
        return 1;  // 副本交换没有恢复替换结果时停止
    ui_key(&desktop, UI_SAVE_KEY, false);  // 请求将当前编辑文字提交给已绑定服务id，成功后才更新saved
    observe("save");    // 记录提交服务之后的dirty=false，下一步还要重新read核对服务正文
    char readback[64];  // 64字节局部数组承接服务重新读出的正文，和Editor副本互相独立
    if (desktop.dirty || !backend.read(NULL, desktop.editor_id, readback, sizeof(readback)) ||  // 要求保存点已更新且服务读取完整成功，短路或避免失败后使用未写入的readback
        strcmp(readback, "print(42)\n"))  // 保存成功之外还要求服务重新读出的文字完全等于十字节输入
        return 1;  // 当前主机观察失败，以非零状态阻止后续误判通过
    ui_key(&desktop, UI_RUN_KEY, false);  // 以当前文档发起运行请求，观察参考服务真实未连接错误
    observe("request execution");  // 记录Run返回错误框后的同一文档状态，保存成功不等于执行可用
    printf("error: %s\n", desktop.dialog_text);  // 向主机终端报告本次观察的实际状态或输出位置
    if (desktop.dialog != UI_DIALOG_ERROR || !strstr(desktop.dialog_text, "not connected"))  // 要求反馈确实是执行未连接错误，不能把失败请求伪装成成功
        return 1;  // 当前主机观察失败，以非零状态阻止后续误判通过
    ui_key(&desktop, UI_ESCAPE, false);  // 关闭Run错误，下一次输入回到同一Editor
    desktop.windows[UI_EDITOR].rect = (struct ui_rect){100, 80, 800, 400};  // 当前48px标题下只容纳九条完整24px文本行
    desktop.windows[UI_EDITOR].maximized = false;  // 固定普通外框，避免最大化替换这份几何
    ui_key(&desktop, UI_SELECT_ALL, false);  // 全选已有十字节，使以下输入替换同一文档
    strcpy(desktop.clipboard, "x\nx\nx\nx\nx\nx\nx\nx\nx\nx");  // 九个换行生成十行，光标在第十行末尾
    ui_key(&desktop, UI_PASTE, false);   // 插入会调用reveal_cursor，最后一行应完整滚入正文
    static uint32_t pixels[1280 * 800];  // 本进程拥有本次观察画布，渲染器只借用
    struct airi_framebuffer fb = {  // 使用同一路径验证实际像素，不只比较滚动公式
        .virtual_address = (uintptr_t)pixels, .width = 1280, .height = 800,  // 画布地址是主机静态数组，单位为像素
        .pitch = 1280 * 4, .bpp = 32, .red_size = 8, .red_shift = 16,  // 每行5120字节，红通道占16..23位
        .green_size = 8, .green_shift = 8, .blue_size = 8  // 其余两个八位通道形成标准RGB
    };
    ui_draw(&desktop, &fb);  // 按同一文档与当前滚动生成可检查的一帧
    struct ui_rect body = u_window_body(desktop.windows[UI_EDITOR].rect);  // 借用绘图回写的实际外框转换正文矩形
    int cursor_x = body.x + 61 + 10;  // 未出现项目栏，正文左缘加一字节推进量到第十行文尾
    int cursor_y = body.y + 88 + (9 - (int)desktop.editor_scroll) * 24 + 3;  // 第九逻辑下标减卷走行数，再到光标竖线顶端
    for (int i = 0; i < 19; ++i)  // 每个光标像素都应保留，不能只让竖线上半段露出
        if (pixels[(cursor_y + i) * 1280 + cursor_x] != u_color(U_BLUE))  // 对照渲染器当前蓝色角色的实际RGB
            return 1;  // 任一像素被底部裁剪说明光标尚未完整显露
    puts("cursor: all 19 pixels visible after the tenth line");  // 报告跨过当前正文末行后的实际绘图结果
    desktop.windows[UI_EDITOR].rect = (struct ui_rect){100, 80, 400, 400};  // 398像素body扣72像素行号与边距后，仍能显示32列
    ui_key(&desktop, UI_SELECT_ALL, false);  // 新观察继续替换同一文档，释放前一个选区
    strcpy(desktop.clipboard, "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");  // 31个ASCII字符，文尾间隙仍在当前32列可见范围
    ui_key(&desktop, UI_PASTE, false);  // 完整替换后应保持最左列，而不是提前滚走第一字符
    ui_draw(&desktop, &fb);  // 用本次宽度生成实际文尾光标
    body = u_window_body(desktop.windows[UI_EDITOR].rect);  // 更新刚变化的body，不能使用前一幅800像素窗口位置
    cursor_x = body.x + 61 + 31 * 10;  // 31字符的文尾间隙位于第31列，每列十像素
    cursor_y = body.y + 88 + 3;  // 单行文本已回到第一可见行，竖线从行顶下三像素开始
    if (desktop.editor_column_scroll)  // 当前完整输入及光标仍容纳得下，无需隐藏第一列
        return 1;  // 按旧外框常数提前水平滚动会在此暴露
    for (int i = 0; i < 19; ++i)  // 再逐像素确认文尾竖线确实写入当前正文裁剪区
        if (pixels[(cursor_y + i) * 1280 + cursor_x] != u_color(U_BLUE))  // 状态值正确之外还要满足最终可见像素
            return 1;  // 行尾光标被右边界或其他绘图覆盖时停止观察
    puts("cursor: column 31 visible without horizontal scroll");  // 报告宽度改变后无需提前滚动的实际行为
    ui_fixture_backend(&backend);  // 恢复原main.lua，下面local查找确实能匹配。
    ui_init(&desktop, backend, &console);  // 为字段路由观察建立新的桌面，绑定现有服务。
    ui_open_document(&desktop, 0);  // 打开main.lua，作为查找可能匹配的文档。
    ui_key(&desktop, UI_FIND_KEY, false);  // Editor查找栏保持打开。
    airi_snprintf(desktop.find_text, sizeof(desktop.find_text), "%s", "local");  // 准备能匹配文档的查找串。
    desktop.cursor = 2;  // 给文档留下与匹配范围不同的光标间隙。
    desktop.anchor = 1;  // 选择当前字节1至2，后续不应被Files筛选改变。
    desktop.selection = true;  // 当前文档选区有效，便于观察越界分派。
    ui_open(&desktop, UI_FILES_APP);  // 切到Files，Editor查找栏状态继续保留。
    ui_key(&desktop, UI_FIND_KEY, false);  // 本次焦点是Files筛选，不是Editor查找。
    ui_key(&desktop, 'm', false);   // 筛选字段接收当前字符。
    ui_key(&desktop, '\n', false);  // 回车只结束Files筛选输入。
    if (desktop.cursor != 2 || desktop.anchor != 1 || !desktop.selection)  // 核对Editor三个状态没有被另一字段搜索覆盖。
        return 1;  // 任何变化都说明本次事件越过了接收者边界。
    puts("fields: Files filter keeps Editor selection");  // 报告实际跨应用字段观察结果。
    puts("PASS lab/editor: reference save and failed execution are distinct");  // 向主机终端报告本次观察的实际状态或输出位置
    return 0;  // 观察完整通过，主机进程以成功状态退出
}
