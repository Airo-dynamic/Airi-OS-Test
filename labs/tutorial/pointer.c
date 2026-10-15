#include "ui.h"      // 取得共享桌面状态和输入事件接口。
#include <string.h>  // 用主机memcmp核对独立文档副本
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

static bool observe_monitor_feedback(uint32_t *pixels, const struct airi_framebuffer *fb) {  // 观察资源区从窗口反馈状态中独立出来。
    ui_open(&desktop, UI_EDITOR);    // 保留一个后方窗口，避免空桌面绕过窗口绘制路径。
    ui_open(&desktop, UI_SETTINGS);  // Settings成为当前活动窗口，再通过真实关闭事件离开。
    desktop.monitor = true;  // 本观察必须绘制右侧资源区。
    ui_draw(&desktop, fb);   // 取得当前Settings实际外框，关闭坐标不能猜初始布局。
    struct ui_rect r = desktop.windows[UI_SETTINGS].rect;  // 读取同一帧回写的当前外框。
    ui_pointer(&desktop, r.x + r.w - 20, r.y + 20, 1);     // 点标题关闭，Settings没有文档保存确认。
    ui_pointer(&desktop, r.x + r.w - 20, r.y + 20, 2);     // 释放清按压与拖动，随后只观察hover。
    if (desktop.active != -1 || !desktop.windows[UI_EDITOR].open || desktop.windows[UI_SETTINGS].open)  // 无活动窗口，但后方Editor仍存在。
        return false;  // 尚未形成会暴露区域许可遗留的状态。
    ui_pointer(&desktop, 200, 200, 0);  // 先让鼠标离开资源按钮，保持无活动窗口。
    ui_draw(&desktop, fb);  // 后方Editor关闭自己的hover许可，资源区必须重新建立自己的许可。
    int left = desktop.width - UI_MONITOR_W + 24, top = desktop.height - 96;  // 资源按钮和monitor绘图使用同一内边距。
    unsigned at = (unsigned)(top + 3) * 1280 + (unsigned)(left + 3);  // 按钮底面取样避开文字和两像素圆角。
    uint32_t normal = pixels[at];  // 保存鼠标远离时本帧的实际底面颜色。
    ui_pointer(&desktop, left + 20, top + 16, 0);  // 普通移动进入资源按钮，不应打开任务窗口。
    ui_draw(&desktop, fb);  // 资源区重新开放许可，此像素应进入悬停色。
    if (desktop.active != -1 || pixels[at] == normal)  // 既检查外观变化，也检查移动没有执行动作。
        return false;  // 沿用底层窗口的false许可会使底面一直不变。
    ui_pointer(&desktop, left + 20, top + 16, 1);  // 真正按下才进入资源按钮动作分派。
    ui_pointer(&desktop, left + 20, top + 16, 2);  // 本次动作后仍完整结束按压。
    if (desktop.active != UI_TASKS_APP)  // 点击必须打开Tasks，不能只拥有悬停像素。
        return false;  // 绘图反馈与动作入口需分别满足对应结果。
    puts("monitor: no active window still permits hover and opens Tasks on click");  // 报告关闭、移动、按下的同一实际事件序列。
    return true;  // 资源区不继承底层窗口的反馈许可。
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
    static uint32_t pixels[1280 * 800];  // 本主机观察拥有像素，绘图仅借用它
    struct airi_framebuffer fb = {  // 画布使用已建立的32位RGB合同
        .virtual_address = (uintptr_t)pixels, .width = 1280, .height = 800,  // 地址指向本进程数组，尺寸以像素计
        .pitch = 1280 * 4, .bpp = 32, .red_size = 8, .red_shift = 16,  // 每行5120字节，红通道在16至23位
        .green_size = 8, .green_shift = 8, .blue_size = 8  // 绿通道8至15位，蓝通道0至7位
    };
    ui_open(&desktop, UI_EDITOR);  // 让Editor成为最前可操作窗口
    desktop.windows[UI_EDITOR].maximized = true;  // 固定观察几何，首次draw更新实际rect
    ui_pointer(&desktop, 1000, 600, 0);  // 指针先远离工具栏，普通移动不触发动作
    ui_draw(&desktop, &fb);  // 把实际布局回写并生成初始像素
    struct ui_rect body = u_window_body(desktop.windows[UI_EDITOR].rect);  // 命中使用绘图刚算出的内容矩形
    int bx = body.x + 32, by = body.y + 16;  // Save按钮内部远离边缘、文字和底线的采样像素
    int button_x = body.x + 12, button_y = body.y + 8;  // 同一Save按钮的左上位置供采样定位
    unsigned at = (unsigned)(button_y + 3) * 1280 + (unsigned)(button_x + 3);  // 用一行1280像素把二维点转换为数组下标
    uint32_t normal = pixels[at];  // 记录指针远离时的按钮底面实际颜色
    char original[UI_DOC_SIZE];    // 独立保存移动前正文，避免指针别名掩盖变化
    memcpy(original, desktop.editor, sizeof(original));  // 后续hover不得修改这些字节
    ui_pointer(&desktop, bx, by, 0);  // 仅把位置移到Save按钮，仍不产生点击
    ui_draw(&desktop, &fb);     // 新帧加入U_HOT，只改变绘图状态
    uint32_t hot = pixels[at];  // 此像素位于按钮底面，悬停应与normal不同
    if (normal == hot || memcmp(original, desktop.editor, sizeof(original)))  // 同时检查反馈存在、正文保留
        return 1;  // 只改了位置却误改正文或没有反馈都应失败
    desktop.pointer_down = true;  // 单独演示绘图按压值，不发送会真正保存的phase1事件
    ui_draw(&desktop, &fb);      // 当前按钮同时带U_HOT和U_DOWN，按压优先
    uint32_t down = pixels[at];  // 取得按压底面，后续与hover比较
    if (hot == down)  // 按压应产生独立于悬停的底面
        return 1;  // 当前绘图没有呈现按压状态
    ui_pointer(&desktop, bx, by, 2);  // 真正释放事件清pointer_down，但位置仍在Save内部
    ui_draw(&desktop, &fb);  // 释放后恢复悬停底面
    if (desktop.pointer_down || pixels[at] != hot)  // 不能保留按压；hover仍应有效
        return 1;  // 释放路径没有清掉原来的按压反馈
    struct ui_rect sample = {4, 4, 90, 30};  // 直接控件观察位于整屏角落，与工作区内容分开
    u_begin(&fb);  // 借用同一数组并恢复整屏裁剪
    u_theme(UI_NIGHT);  // 将直接控件观察固定为夜间，使颜色比较可重复
    u_interaction(8, 8, false, false);  // 下层区域没有响应资格，即使指针在按钮内也无悬停
    u_button(sample, "Test", false, false);   // 普通启用按钮只绘制明确的normal状态
    uint32_t blocked = pixels[8 * 1280 + 8];  // 读取区域禁止响应时的同一点
    u_interaction(8, 8, false, true);  // 相同位置开放响应，只改变当前绘制区域许可
    u_button(sample, "Test", false, false);  // 同一按钮现在加入U_HOT
    if (blocked == pixels[8 * 1280 + 8])     // 区域许可必须确实影响视觉反馈
        return 1;  // 没有区分可响应和被遮挡区域
    u_interaction(8, 8, true, true);  // 指针既在按钮内又按着，先形成最强输入状态
    u_button(sample, "Test", false, true);     // disabled仍覆盖hover/down，只显示禁用外观
    uint32_t disabled = pixels[8 * 1280 + 8];  // 保存禁用按钮这一点的颜色
    u_interaction(1000, 600, false, true);     // 移出按钮并解除按压，禁用身份保持
    u_button(sample, "Test", false, true);     // 禁用按钮不因位置或按压变化而改变底面
    if (pixels[8 * 1280 + 8] != disabled)      // 两种指针输入下的禁用底面应一致
        return 1;  // 禁用控件仍错误显示可操作反馈
    ui_open(&desktop, UI_SETTINGS);  // Settings置顶，原Editor成为不能响应按钮的底层窗口
    struct ui_rect overlap = desktop.windows[UI_EDITOR].rect;  // 将上层几何暂设为同样位置，明确构成遮挡
    desktop.windows[UI_SETTINGS].rect = overlap;    // 仅观察遮挡，不改变文档数组
    desktop.windows[UI_SETTINGS].maximized = true;  // 相同外框将在下一帧得到同样布局
    ui_pointer(&desktop, bx, by, 0);  // 位于旧Save屏幕点的普通移动，不能调用Editor保存
    ui_draw(&desktop, &fb);  // 窗口绘图依层次决定谁有响应许可并覆盖下层像素
    if (desktop.active != UI_SETTINGS || memcmp(original, desktop.editor, sizeof(original)))  // hover不得置顶Editor或修改它的正文
        return 1;  // 被遮挡内容在普通移动中产生了动作
    puts("hover/down/up, disabled and occluded feedback observed; hover changed no document");  // 输出各输入状态的实际观察结果
    if (!observe_monitor_feedback(pixels, &fb))  // 再关闭活动窗口，验证资源区不继承后方Editor的禁止反馈。
        return 1;  // 资源hover或真实打开Tasks任一失败都不能结束为成功。
    if (desktop.editor_id != 1 || !desktop.selection || desktop.cursor == anchor)  // 先确认此前文件与窗口观察，再改变外框测试临界宽度。
        return 1;  // 原来的选择尚未建立，不能用后一组操作替代其结果。
    return observe_text_drag() ? 0 : 1;  // 窄窗口起拖与越过Save同样必须通过，最终才返回成功。
}
