#include <string.h>  // 主机观察用独立数组复制与比较检查选择没有触发保存。
#include "ui.h"      // 取得共享桌面状态和输入事件接口。
#include <stdio.h>   // 取得主机 printf、文件输出与错误报告接口。
static struct ui_desktop desktop;      // 静态存储整份桌面；事件回调结束后状态仍保留。
static struct airi_console console;    // 静态保存终端命令与日志，不在每次重画时重建。
static bool observe_text_drag(void) {  // 在同一桌面验证窄窗口起拖和越过Save时的选择边界。
    ui_key(&desktop, UI_ESCAPE, false);  // 清反馈与查找焦点，本次文本输入不交给短字段。
    ui_open(&desktop, UI_EDITOR);  // 让原Editor重新成为输入接收者，不重建文档或服务。
    desktop.windows[UI_EDITOR].maximized = false;  // 使用明确普通外框，避免下一帧改成最大化宽度。
    desktop.windows[UI_EDITOR].rect = (struct ui_rect){100, 80, 800, 400};  // 外框800像素，内容宽798，不显示190像素项目栏。
    ui_key(&desktop, UI_SELECT_ALL, false);    // 本次完整替换当前工作正文，保存点仍独立持有。
    strcpy(desktop.clipboard, "print(42)\n");  // 十字节ASCII输入加NUL，已知可放入剪贴板。
    ui_key(&desktop, UI_PASTE, false);  // 形成实际未保存文档，dirty由已有编辑路径更新。
    char text[UI_DOC_SIZE], saved[UI_DOC_SIZE], stored[UI_DOC_SIZE];  // 观察者独立持有文字与保存点，stored稍后读回服务。
    memcpy(text, desktop.editor, sizeof(text));   // 保存本次工作数组全部字节，选择不能修改它。
    memcpy(saved, desktop.saved, sizeof(saved));  // 保存旧提交点，拖过Save也不能更新它。
    struct ui_rect body = u_window_body(desktop.windows[UI_EDITOR].rect);  // 用当前实际外框取得与绘图相同的内容范围。
    if (body.w != 798 || !desktop.dirty)  // 先确认临界宽度和实际未保存状态，才观察后续移动。
        return false;  // 尚未构造目标输入，不能宣称选择观察通过。
    int x = body.x + 61 + 10, y = body.y + 88 + 8;  // 无项目栏，第一行第1间隙在正文左缘之后十像素。
    ui_pointer(&desktop, x, y, 1);  // 在第一批文字列按下，不能误按外框宽度排除不存在的项目栏。
    if (desktop.drag != UI_DRAG_SELECT || desktop.anchor != 1)  // 必须真正开始拖选并锁定第1间隙。
        return false;  // 仅移动cursor而未登记持续动作仍算失败。
    unsigned anchor = desktop.anchor;  // 按下间隙随后保持不变，鼠标只移动另一端。
    ui_pointer(&desktop, body.x + 20, body.y + 16, 0);  // 将选择移到实际Save按钮内部，只走正文定位器。
    if (!desktop.dirty || desktop.anchor != anchor || desktop.cursor != 0)  // 顶部越界夹到文首，保存状态及起点不能被按钮分派改动。
        return false;  // 保存动作清dirty或改anchor时立即报告失败。
    if (memcmp(text, desktop.editor, sizeof(text)) || memcmp(saved, desktop.saved, sizeof(saved)))  // 两份数组字节都应与移动前相同。
        return false;  // 选择和外部位置不能提交或重写当前文档。
    if (!desktop.backend.read(desktop.backend.context, desktop.editor_id, stored, sizeof(stored)) || strcmp(stored, saved))  // 读回真实参考记录，确认服务仍持有旧保存点。
        return false;  // 不仅检查前端标记，也检查服务没有收到保存。
    ui_pointer(&desktop, body.x + 20, body.y + 16, 2);  // 在同一按钮上释放，只结束选择动作。
    if (desktop.drag != UI_DRAG_NONE || !desktop.dirty || desktop.anchor != anchor)  // 释放后不再续拖，未保存状态与固定起点继续保留。
        return false;  // 释放不能另行执行Save或留下旧拖动。
    puts("selection: body798 starts at column1; drag over Save preserves text, saved and anchor");  // 报告这组真实事件的结果。
    return true;  // 起拖、越界、服务回读与释放均符合当前选择规则。
}

int main(void) {  // 观察程序入口；返回非零表示预期状态未出现。
    struct ui_backend backend;     // 局部回调表连接参考服务；桌面会复制这份表。
    ui_fixture_backend(&backend);  // 初始化参考文件和回调，把表写入 backend。
    airi_console_init(&console, backend.boot);  // 命令对象借用参考启动描述。
    ui_init(&desktop, backend, &console);  // 初始化桌面，复制回调表并借用稳定的 console。
    ui_open(&desktop, UI_FILES_APP);  // 置顶 Files，实验从目录操作开始。
    unsigned order[UI_FILES];  // 数组保存排序后的快照下标，不复制文件记录。
    unsigned count = ui_file_order(&desktop, order);  // 得到当前实际排序顺序及有效条数。
    unsigned row = 0;  // 从排序后的第一行开始查找 main.lua 的服务 id。
    while (row < count && desktop.files[order[row]].id != 1)  // 先确认行有效，再经排序下标读取服务 id，查找 id=1 的文件。
        ++row;  // 推进排序后行号，而不改变文件身份。
    if (row == count)  // 没有 id=1 时不能伪造后续鼠标成功。
        return 1;  // 观察失败，以非零状态让命令立即停止。
    struct ui_rect files = desktop.windows[UI_FILES_APP].rect;  // 读取 Files 的实际窗口位置作为事件坐标起点。
    ui_pointer(&desktop, files.x + 220, files.y + UI_TITLE_H + 1 + 138 + (int)row * (desktop.density ? 28 : 36) + 8, 3);  // 从49像素标题及外框下方定位首行138像素顶部，按实际行高选第row行内部；3表示双击。
    if (desktop.active != UI_EDITOR)  // 双击必须把该文件送进 Editor，否则停止实验。
        return 1;  // 观察失败，以非零状态让命令立即停止。
    struct ui_rect editor = desktop.windows[UI_EDITOR].rect;      // 打开后重新读取 Editor 矩形，不复用 Files 坐标。
    int x = editor.x + 260, y = editor.y + UI_TITLE_H + 1 + 100;  // 选取 Editor 正文内的一点，避开树与行号区。
    ui_pointer(&desktop, x, y, 1);     // 发送按下事件，让正文计算初始字节间隙。
    unsigned anchor = desktop.anchor;  // 保存实验起点，用于比较拖动后的光标。
    ui_pointer(&desktop, x + 40, y, 0);  // 横移40像素，模拟约四个字符的拖选。
    ui_pointer(&desktop, x + 40, y, 2);  // 释放鼠标，必须结束 drag 而保留已形成的选区。
    printf("editor_id=%d anchor=%u cursor=%u selected=%d drag=%d\n", desktop.editor_id,  // 打印实际身份、选区间隙与释放后的拖动状态。
           anchor, desktop.cursor, desktop.selection, desktop.drag);  // 布尔值经可变参数提升为 int，%d 打印0或1。
    ui_pointer(&desktop, x + 40, y, 2);  // 再释放一次仍保持无拖动，避免观察继承旧动作
    struct ui_rect before = desktop.windows[UI_EDITOR].rect;  // 保存普通窗口此刻的实际位置与尺寸
    int title_x = before.x + 210, title_y = before.y + 20;    // 标题中央避开最右侧三个系统按钮
    ui_pointer(&desktop, title_x, title_y, 1);  // 在标题按下，核心保存鼠标到窗口左上角的偏移
    ui_pointer(&desktop, title_x + 30, title_y + 25, 0);     // 移动事件沿同一拖动种类改变rect位置
    ui_pointer(&desktop, title_x + 30, title_y + 25, 2);     // 释放结束窗口移动，几何变化继续保留
    struct ui_rect moved = desktop.windows[UI_EDITOR].rect;  // 取得移动后而非原来的窗口矩形
    if (moved.x != before.x + 30 || moved.y != before.y + 25)  // 标题偏移必须转成同样的窗口位移
        return 1;  // 移动结果不符，不继续缩放观察
    ui_pointer(&desktop, moved.x + moved.w - 6, moved.y + moved.h - 6, 1);  // 右下16×16区域内按下开始缩放
    ui_pointer(&desktop, moved.x + moved.w + 24, moved.y + moved.h + 19, 0);  // 鼠标距窗口左上角的距离成为新宽高
    ui_pointer(&desktop, moved.x + moved.w + 24, moved.y + moved.h + 19, 2);  // 释放停止缩放，普通移动不再延续
    struct ui_rect resized = desktop.windows[UI_EDITOR].rect;  // 同一窗口的新尺寸与当前位置
    if (resized.w != moved.w + 24 || resized.h != moved.h + 19 || desktop.drag != UI_DRAG_NONE)  // 同时核对宽高与释放状态
        return 1;  // 有一项不符便停止，不能只以窗口仍存在判为成功
    puts("window moved by (30,25), resized by (24,19), released");  // 输出这组实际三事件操作留下的结果
    if (desktop.editor_id != 1 || !desktop.selection || desktop.cursor == anchor)  // 先确认此前文件与窗口观察，再改变外框测试临界宽度。
        return 1;  // 原来的选择尚未建立，不能用后一组操作替代其结果。
    return observe_text_drag() ? 0 : 1;  // 窄窗口起拖与越过Save同样必须通过，最终才返回成功。
}
