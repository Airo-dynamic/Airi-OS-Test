#include "ui.h"      // 取得共享桌面状态和输入事件接口。
#include <assert.h>  // 取得条件不成立就中止的 assert；检查构建不能定义 NDEBUG。
#include <stdio.h>   // 取得主机 printf、文件输出与错误报告接口。
#include <stdlib.h>  // 取得主机分配、释放、退出与数值转换接口。
#include <string.h>  // 取得宿主字符串比较、复制和查找接口。
static struct ui_desktop desk;  // 测试进程持有一份桌面，每个检查用 start 重新初始化。
static struct airi_console console;  // 静态保存终端命令与日志，不在每次重画时重建。
static struct ui_backend backend;    // 测试进程保存回调表，读取与动作针对同一参考服务。
static void start(void) {  // 每个独立检查重建服务与桌面，前一检查状态不会污染下一项。
    ui_fixture_backend(&backend);  // 初始化参考文件和回调，把表写入 backend。
    airi_console_init(&console, backend.boot);  // 命令对象借用参考启动描述。
    ui_init(&desk, backend, &console);  // 桌面复制这一份回调表，借用同一console。
}
static void typing(const char *s) {  // 把NUL字符串转换为连续ASCII键入事件。
    while (*s)  // 当前字节非NUL才继续键入。
        ui_key(&desk, *s++, false);  // 先读取当前字节送键事件，再推进指针一字节。
}
static void workflow(void) {  // 沿同一文档修改、保存、撤销、剪贴板与服务失败走完动作。
    start();  // 以重新初始化的参考服务和桌面开始本项检查。
    assert(desk.active == UI_EDITOR && desk.editor_id == 1);  // 初始活动Editor必须打开服务id=1的文档。
    ui_key(&desk, UI_SELECT_ALL, false);  // 建立整个文档选区，下一次键入替换旧内容。
    typing("print(42)\n");  // 逐字节键入完整Lua文本，包含末尾换行。
    assert(desk.dirty && !strcmp(desk.editor, "print(42)\n"));  // 修改后工作文本匹配输入且尚未保存。
    ui_key(&desk, UI_SAVE_KEY, false);  // 请求当前文件id保存工作文本。
    assert(!desk.dirty && desk.dialog == UI_DIALOG_NONE);  // 成功保存后dirty清除且没有错误框。
    char saved[UI_DOC_SIZE];  // 独立数组接收服务实际保存的文本。
    assert(backend.read(NULL, 1, saved, sizeof(saved)));  // 参考服务上下文可为NULL，读取id=1并检查回调成功。
    assert(!strcmp(saved, "print(42)\n"));  // 成功保存必须可从服务重新读出同一文本。
    ui_key(&desk, UI_UNDO, false);  // 使用撤销快照交换前后文本，不重新读取服务。
    assert(desk.dirty);  // 当前工作文本与saved不同，必须标为未保存。
    ui_key(&desk, UI_UNDO, false);  // 使用撤销快照交换前后文本，不重新读取服务。
    assert(!strcmp(desk.editor, "print(42)\n"));  // 操作后工作文本必须仍为这份完整字节串。
    ui_key(&desk, UI_SELECT_ALL, false);  // 建立整个文档选区，下一次键入替换旧内容。
    ui_key(&desk, UI_COPY, false);  // 把当前选区复制到独立剪贴板数组。
    assert(!strcmp(desk.clipboard, "print(42)\n"));  // 剪贴板必须持有完整选区文本。
    ui_key(&desk, UI_CUT, false);  // 复制当前选区后删除工作文本中的选中字节。
    assert(!desk.editor[0]);  // 全选剪切后首字节为NUL，文档为空。
    ui_key(&desk, UI_PASTE, false);  // 以容量和ASCII合同插入完整剪贴板。
    assert(!strcmp(desk.editor, "print(42)\n"));  // 操作后工作文本必须仍为这份完整字节串。
    ui_key(&desk, UI_RUN_KEY, false);  // 执行请求由当前服务明确返回未连接。
    assert(desk.dialog == UI_DIALOG_ERROR && strstr(desk.dialog_text, "not connected"));  // 错误框存在且原因包含服务未连接，不能假装脚本已运行。
    ui_key(&desk, UI_ESCAPE, false);      // 关闭当前消息框或取消字段，回到原编辑对象。
    ui_service_state(&desk, UI_OFFLINE);  // 切为离线状态，后续服务动作应失败。
    ui_key(&desk, UI_SAVE_KEY, false);    // 请求当前文件id保存工作文本。
    assert(desk.dialog == UI_DIALOG_ERROR);  // 失败动作必须留下错误框。
    assert(!strcmp(desk.editor, "print(42)\n"));  // 操作后工作文本必须仍为这份完整字节串。
    ui_key(&desk, UI_ESCAPE, false);     // 关闭当前消息框或取消字段，回到原编辑对象。
    ui_service_state(&desk, UI_NORMAL);  // 恢复普通服务状态，下一动作重新访问参考服务。
    ui_command(&desk, UI_CREATE_FILE, 0, "test.lua", "");  // 在显式当前目录创建空文件，id=0表示尚未分配身份。
    assert(desk.file_count == 7);  // 六条初始目录记录加一次创建应变成七条。
    int id = 0;  // 零表示尚未找到新建文件服务身份。
    for (unsigned i = 0; i < desk.file_count; ++i)  // 遍历有效目录快照，不读取容量剩余项。
        if (!strcmp(desk.files[i].name, "test.lua"))  // strcmp为零表示名称完全相等。
            id = desk.files[i].id;  // 取稳定文件身份，不把i作为服务目标。
    assert(id);  // 新建记录必须分配非零id。
    ui_command(&desk, UI_RENAME, id, "renamed.lua", "");  // 同一id重命名，位置或名称变化不更换身份。
    ui_command(&desk, UI_DELETE, id, "", "");  // 删除该id，无需额外名称或正文。
    assert(desk.file_count == 6);  // 删除新文件后恢复六条初始记录。
    ui_command(&desk, UI_CREATE_FILE, 0, "main.lua", "");  // 尝试同目录重名创建，必须返回错误。
    assert(desk.dialog == UI_DIALOG_ERROR);  // 失败动作必须留下错误框。
    ui_key(&desk, UI_ESCAPE, false);  // 关闭当前消息框或取消字段，回到原编辑对象。
    ui_open_document(&desk, 1);  // 快照下标1是初始CSV，打开后转入Viewer。
    assert(desk.active == UI_VIEWER && desk.series_count == 6 && desk.series_values[5] == 37);  // 核对窗口、六个有效点和最后一个纵轴值。
    unsigned before = desk.task_count;  // 记录停止动作之前的任务记录数。
    ui_command(&desk, UI_STOP, 14, "", "");  // 删除允许停止的参考任务pid=14。
    assert(desk.task_count == before - 1);   // 成功停止只减少一条服务记录。
    ui_command(&desk, UI_STOP, 1, "", "");   // 受保护的pid=1停止请求必须失败。
    assert(desk.dialog == UI_DIALOG_ERROR);  // 失败动作必须留下错误框。
    ui_key(&desk, UI_ESCAPE, false);  // 关闭当前消息框或取消字段，回到原编辑对象。
    for (int i = 0; i < UI_APP_COUNT; ++i)  // 打开每个有效应用观察顺序数组容量。
        ui_open(&desk, (enum ui_app)i);  // 将整数身份转枚举，打开并置顶该应用。
    assert(desk.order_count == UI_APP_COUNT);  // 重复打开已有应用不能增加重复顺序项。
    ui_open(&desk, UI_EDITOR);  // 将Editor再次置顶，不创建第二个Editor窗口。
    assert(desk.order_count == UI_APP_COUNT);  // 重复打开已有应用不能增加重复顺序项。
    ui_key(&desk, UI_NEXT_WINDOW, false);      // 活动应用沿窗口顺序切换。
    assert(desk.active != UI_EDITOR);  // 切换后不能仍停在原Editor。
}
static void input_boundaries(void) {  // 沿查找、容量、滚动、坏CSV和对话框检查输入边界。
    start();  // 以重新初始化的参考服务和桌面开始本项检查。
    ui_key(&desk, UI_FIND_KEY, false);  // 查找栏获得输入焦点。
    strcpy(desk.clipboard, "local values");  // 准备查找要粘贴的ASCII文本。
    ui_key(&desk, UI_PASTE, false);  // 以容量和ASCII合同插入完整剪贴板。
    assert(!strcmp(desk.find_text, "local values"));  // 查找字段应接收粘贴，不能误改文档。
    ui_key(&desk, '\n', false);  // 回车确认当前框或查找，具体用途由当前focus/dialog决定。
    assert(desk.selection);      // 查找匹配应在文档中建立选区。
    ui_key(&desk, UI_ESCAPE, false);    // 关闭当前消息框或取消字段，回到原编辑对象。
    ui_key(&desk, UI_FIND_KEY, false);  // 重新打开Editor查找，保留会话中的find状态。
    strcpy(desk.find_text, "local");    // 准备能匹配当前main.lua的文本。
    desk.cursor = 2;  // 文档光标间隙与匹配结果不同。
    desk.anchor = 1;  // 建立需要保留的选区起点。
    desk.selection = true;  // 当前文档选择字节1至2。
    ui_open(&desk, UI_FILES_APP);  // 转到Files但不销毁Editor状态。
    ui_key(&desk, UI_FIND_KEY, false);  // 获取Files筛选焦点。
    ui_key(&desk, 'm', false);   // 只改变Files筛选文本。
    ui_key(&desk, '\n', false);  // 提交筛选不能触发仍打开的Editor查找栏。
    assert(desk.cursor == 2 && desk.anchor == 1 && desk.selection);  // 跨应用事件不得覆盖文档选择。
    ui_open(&desk, UI_EDITOR);  // 返回原文档继续后面的编辑边界检查。
    ui_key(&desk, UI_SELECT_ALL, false);  // 建立整个文档选区，下一次键入替换旧内容。
    typing("one\n");  // 全选后用短文替换原内容。
    ui_key(&desk, UI_SAVE_KEY, false);  // 请求当前文件id保存工作文本。
    ui_key(&desk, UI_UNDO, false);      // 使用撤销快照交换前后文本，不重新读取服务。
    assert(desk.dirty);  // 当前工作文本与saved不同，必须标为未保存。
    ui_key(&desk, UI_UNDO, false);  // 使用撤销快照交换前后文本，不重新读取服务。
    assert(!desk.dirty);  // 工作文本与最近成功保存相同，dirty必须清除。
    ui_key(&desk, UI_SELECT_ALL, false);  // 建立整个文档选区，下一次键入替换旧内容。
    for (unsigned i = 0; i < UI_DOC_SIZE - 1; ++i)  // 为8191有效字节填内容，保留最后一字节给NUL。
        desk.clipboard[i] = 'a';  // 每项填一个ASCII字节，构造满容量文本。
    desk.clipboard[UI_DOC_SIZE - 1] = 0;  // 最后写NUL，后续strlen和插入才有明确终点。
    ui_key(&desk, UI_PASTE, false);  // 以容量和ASCII合同插入完整剪贴板。
    assert(strlen(desk.editor) == UI_DOC_SIZE - 1);  // 满容量插入后的有效文本长度应为8191。
    ui_key(&desk, 'b', false);  // 追加一个字节故意超过可保存长度。
    assert(strlen(desk.editor) == UI_DOC_SIZE - 1 && strstr(desk.toast, "limit"));  // 超限必须保留原长度并显示拒绝原因。
    assert(desk.editor_column_scroll > 0);  // 长行光标移到尾部应推动水平滚动。
    ui_key(&desk, UI_SELECT_ALL, false);    // 建立整个文档选区，下一次键入替换旧内容。
    typing("short\n");  // 全选替换短文释放容量，不是向满文档追加。
    assert(!strcmp(desk.editor, "short\n"));  // 替换后的工作文本必须完整匹配短文。
    for (unsigned i = 0; i < 50; ++i)  // 连续追加五十行以触发垂直滚动。
        typing("row\n");  // 一次写入完整四字节行。
    assert(desk.editor_scroll > 0);     // 末尾光标应促使文档视口向下卷动。
    ui_key(&desk, UI_SAVE_KEY, false);  // 请求当前文件id保存工作文本。
    ui_open_document(&desk, 1);      // 快照下标1是初始CSV，打开后转入Viewer。
    assert(desk.series_count == 6);  // 当前有效曲线仍有六个点。
    assert(backend.action(NULL, desk.directory, UI_SAVE, 2, "series.csv", "seconds,load\n0,12\n1,invalid\n",  // 将文件id=2保存为可读取但第二行数值损坏的CSV。
                          desk.dialog_text, sizeof(desk.dialog_text)));  // 服务错误缓冲独立有界；本次写入本身应成功。
    ui_open_document(&desk, 1);  // 快照下标1是初始CSV，打开后转入Viewer。
    assert(desk.dialog == UI_DIALOG_ERROR && desk.series_count == 6);  // 坏CSV解析失败，错误框出现但旧六点曲线保留。
    ui_key(&desk, UI_ESCAPE, false);  // 关闭当前消息框或取消字段，回到原编辑对象。
    desk.pending_close = true;  // 模拟保存后继续关闭的待办状态。
    desk.dialog = UI_DIALOG_SAVE_AS;  // 当前显示另存为输入框。
    ui_key(&desk, UI_ESCAPE, false);  // 关闭当前消息框或取消字段，回到原编辑对象。
    assert(!desk.pending_close);      // 取消另存为必须连待关闭意图一起取消。
    ui_open(&desk, UI_FILES_APP);     // 把后续键事件转到Files视图。
    ui_key(&desk, UI_DELETE_KEY, false);      // 列表删除键先建立确认框。
    assert(desk.dialog == UI_DIALOG_DELETE);  // 不能在确认前直接删除选中记录。
    ui_key(&desk, UI_ESCAPE, false);   // 关闭当前消息框或取消字段，回到原编辑对象。
    desk.dialog = UI_DIALOG_NEW_FILE;  // 模拟新文件名称输入框。
    desk.field_all = true;  // 输入字段全选，粘贴应替换旧名称。
    strcpy(desk.dialog_input, "untitled");  // 初始化旧名称，供替换后比较。
    strcpy(desk.clipboard, "paste.lua");    // 名称剪贴板独立保存完整新名称。
    ui_key(&desk, UI_PASTE, false);  // 以容量和ASCII合同插入完整剪贴板。
    assert(!strcmp(desk.dialog_input, "paste.lua"));  // 字段全选粘贴必须完整替换名称。
}
static void explicit_contracts(void) {  // 核对拒绝非ASCII、失败关闭、显式目录和可见选择三组合同。
    start();  // 以重新初始化的参考服务和桌面开始本项检查。
    char original[UI_DOC_SIZE];     // 独立数组保留操作前的文档字节，供后面比较。
    strcpy(original, desk.editor);  // 当前有界文档复制到同容量快照，不能只借用工作数组。
    strcpy(desk.clipboard, "\xc3\xa9");  // 构造é的两个UTF-8字节，当前ASCII合同应拒绝它们。
    ui_key(&desk, UI_PASTE, false);      // 以容量和ASCII合同插入完整剪贴板。
    assert(!strcmp(original, desk.editor) && !desk.undo_valid && !desk.dirty);  // 拒绝整次插入，不改变工作文本、撤销或dirty。
    assert(strstr(desk.toast, "ASCII"));  // 输入拒绝必须说明ASCII限制。
    typing("changed");  // 制造尚未保存的修改，供关闭失败观察。
    ui_service_state(&desk, UI_OFFLINE);  // 切为离线状态，后续服务动作应失败。
    desk.dialog = UI_DIALOG_CLOSE;  // 未保存关闭要先确认保存或放弃。
    ui_key(&desk, '\n', false);     // 回车确认当前框或查找，具体用途由当前focus/dialog决定。
    assert(desk.dialog == UI_DIALOG_ERROR && !desk.pending_close && desk.dirty);  // 离线保存失败应保留修改并取消待关闭，窗口不被误关。
    ui_key(&desk, '\n', false);  // 回车确认当前框或查找，具体用途由当前focus/dialog决定。
    ui_service_state(&desk, UI_NORMAL);  // 恢复普通服务状态，下一动作重新访问参考服务。
    ui_key(&desk, UI_SAVE_KEY, false);   // 请求当前文件id保存工作文本。
    assert(desk.windows[UI_EDITOR].open && !desk.dirty);  // 恢复服务并保存后Editor仍打开且内容已保存。
    struct ui_file other[UI_FILES];  // 独立快照承接另一个目录查询，不替换桌面当前目录。
    backend.files(NULL, "/home/airi", other, UI_FILES);  // 显式查询主目录，后续动作仍应使用desk.directory。
    ui_command(&desk, UI_CREATE_FILE, 0, "explicit.txt", "");  // 创建目标来自桌面显式目录，不受刚才查询副作用影响。
    assert(desk.file_count == 7);  // 六条初始目录记录加一次创建应变成七条。
    ui_open(&desk, UI_FILES_APP);  // 把后续键事件转到Files视图。
    strcpy(desk.filter[UI_FILES_APP], ".lua");  // 筛选只保留Lua名称，后续上下键应沿可见索引选择。
    unsigned order[UI_FILES];  // 数组保存排序后的快照下标，不复制文件记录。
    unsigned count = ui_file_order(&desk, order);  // 获取筛选排序后的有效快照下标。
    assert(count == 1);  // 此输入应只留下一个匹配项。
    desk.selection_file = UI_FILES - 1;   // 故意放入不对应有效可见项的旧选择。
    ui_key(&desk, AIRI_KEY_DOWN, false);  // 从旧选择恢复到实际可见项，不能用容量下标访问记录。
    assert(desk.selection_file == order[0]);  // 修正选择后应指向唯一匹配的原快照下标。
    char error[160];  // 服务失败原因写入这个有界数组。
    assert(backend.action(NULL, "/home/airi", UI_CREATE_FILE, 0, "separate.txt", "", error, sizeof(error)));  // 在另一显式目录建立记录，必须不受桌面当前目录影响。
    unsigned n = backend.files(NULL, "/home/airi", other, UI_FILES);  // 在同一显式目录重新查询并取得有效条数。
    int id = -1;  // -1表示尚未找到目标文件。
    for (unsigned i = 0; i < n; ++i)  // 仅遍历这次查询返回的有效项。
        if (!strcmp(other[i].name, "separate.txt"))  // 完整名称相等时取稳定服务id。
            id = other[i].id;  // 保存跨排序和名称修改仍可使用的身份。
    assert(id > 0);  // 新创建项必须分配有效正id。
    assert(backend.action(NULL, "/home/airi", UI_RENAME, id, "main.lua", "", error, sizeof(error)));  // 不同目录可以有同名main.lua，显式目录与id决定此次目标。
}
static void appearance(void) {  // 用真实设置点击检查换色不损坏文档和窗口。
    start();  // 每个用例重建独立会话。
    typing("unsaved");  // 制造真实未保存文本。
    ui_key(&desk, UI_SELECT_ALL, false);  // 建立必须保留的光标与选择。
    char document[UI_DOC_SIZE];     // 只复制被测文本，不重建整份状态。
    strcpy(document, desk.editor);  // 保存换色前内容作为独立预期。
    char saved[UI_DOC_SIZE];    // 独立保存最近一次成功保存的字节，换色不能重设保存点。
    strcpy(saved, desk.saved);  // 复制保存点，不借用稍后可能被修改的原数组。
    unsigned cursor = desk.cursor, anchor = desk.anchor;     // 记录选择两端。
    struct ui_rect position = desk.windows[UI_EDITOR].rect;  // 编辑窗口不因 Settings 打开而移动。
    ui_open(&desk, UI_SETTINGS);  // 通过正常入口激活设置。
    struct ui_rect body = u_window_body(desk.windows[UI_SETTINGS].rect);  // 点击复用公共布局度量。
    for (unsigned mode = 0; mode < 2; ++mode) {  // 先点击 Day，再切回 Night。
        unsigned expected = mode == 0 ? UI_DAY : UI_NIGHT;  // 明确本次按钮含义。
        int x = body.x + 28 + (int)expected * 138 + 30, y = body.y + 166;  // 按下点落在按钮内部。
        ui_pointer(&desk, x, y, 1);      // 正常桌面命中路由执行设置。
        assert(desk.theme == expected);  // 主题由桌面拥有，命令通过扩展消费它。
        assert(desk.pointer_down);   // 按下状态供公共绘图使用。
        ui_pointer(&desk, x, y, 2);  // 释放不重复执行设置。
        assert(!desk.pointer_down && desk.dirty && desk.selection);  // 未保存和选择状态不能被清空。
        assert(desk.cursor == cursor && desk.anchor == anchor);      // 光标与选择两端不变。
        assert(!strcmp(document, desk.editor));  // 工作文本逐字节保留。
        assert(!strcmp(saved, desk.saved));      // 最近成功保存的字节也保留，dirty含义不能因换色改变。
        assert(!memcmp(&position, &desk.windows[UI_EDITOR].rect, sizeof(position)));  // 窗口位置和大小都不变。
    }
    ui_pointer(&desk, body.x + 159, body.y + 166, 1);  // 两个按钮间的空隙不属于任何操作。
    assert(desk.theme == UI_NIGHT);  // 间隙不能切换为 Day。
    ui_pointer(&desk, body.x + 159, body.y + 166, 2);  // 恢复释放状态。
    const char *command = "theme day\n";  // 控制台提供相同设置入口。
    for (const char *p = command; *p; ++p) airi_console_key(&console, *p);  // 逐键送入命令模型，不绕过解析。
    assert(desk.theme == UI_DAY && !strcmp(document, desk.editor));  // 命令成功也不触碰编辑器。
    for (const char *p = "theme invalid\n"; *p; ++p)  // 沿完整命令字节递进至NUL。
        airi_console_key(&console, *p);  // 未知主题经真实解析被拒绝，不覆盖偏好。
    assert(desk.theme == UI_DAY);  // 无效输入保留已有模式。
    u_theme(UI_DAY);  // 下方检查颜色边界与双主题映射。
    uint32_t day = u_color(U_BG);  // 日间桌面应不同于夜间。
    assert(u_color(0x123456) == 0x123456);  // 字形混合生成的真实 RGB 不得再映射。
    u_theme(UI_NIGHT);  // 切换渲染器模式。
    assert(day != u_color(U_BG));  // 确认不是仅更改设置标签。
}
static void boundary_render(void) {  // 在多尺寸、多位布局和哨兵内存上调用同一绘图实现。
    const unsigned widths[] = {640, 1024, 1280, 1600}, heights[] = {480, 768, 800, 900};  // 同下标配对四种画布尺寸。
    for (unsigned mode = UI_NIGHT; mode <= UI_DAY; ++mode)  // 所有像素边界用例在两种主题下执行。
    for (unsigned k = 0; k < 4; ++k)  // 遍历四组宽高组合。
        for (unsigned bpp = 24; bpp <= 32; bpp += 8) {  // 每个尺寸分别检查三字节和四字节像素。
            unsigned bytes = bpp / 8, pitch = widths[k] * bytes + 16;  // 位数除八换字节；每行故意多留16字节填充。
            size_t size = (size_t)pitch * heights[k];  // 先扩展成size_t再相乘，得到全画布字节数。
            unsigned char *p = malloc(size + 64);      // 两端各额外32字节，像素区域放在中间。
            assert(p);  // 分配成功后才能初始化和绘图。
            memset(p, 0xa5, size + 64);  // 连同两端与行填充置为哨兵值0xa5。
            struct airi_framebuffer fb = {.virtual_address = (uintptr_t)(p + 32),  // 真实像素地址跳过前端32字节哨兵，uintptr_t保存地址值。
                                          .width = widths[k],    // 本次像素宽度来自第k组。
                                          .height = heights[k],  // 高度取同组配对值。
                                          .pitch = pitch,  // 每行跨度包含16字节填充，绘图必须按它寻址。
                                          .bpp = bpp,      // 当前采用24或32位像素。
                                          .red_size = 8,   // 红通道占八位。
                                          .red_shift = 16,   // 红通道放在位16..23，读写需平移16位。
                                          .green_size = 8,   // 绿通道占八位。
                                          .green_shift = 8,  // 绿通道放在位8..15。
                                          .blue_size = 8};   // 蓝通道占八位，未指定 blue_shift 时默认零。
            start();  // 当前尺寸、位数与主题使用独立重置后的服务。
            desk.theme = mode;  // 桌面绘制从会话模型获取当前主题。
            for (int app = 0; app < UI_APP_COUNT; ++app) {  // 每个应用在同一布局分别绘制。
                ui_open(&desk, (enum ui_app)app);    // 把当前检查应用打开置顶。
                desk.windows[app].maximized = true;  // 用全工作区测试实际布局边界。
                for (unsigned tab = 0; tab < (app == UI_SYSTEM     ? 5u  // System检查五个有效页签。
                                              : app == UI_SETTINGS ? 4u  // Settings检查四个页签。
                                                                   : 1u);  // 其他应用只有一个内容页。
                     ++tab) {  // 每次推进一个页签下标。
                    desk.windows[app].tab = tab;  // 把页签状态交给实际绘图。
                    ui_draw(&desk, &fb);  // 在有哨兵和填充的本次像素数组内绘制。
                }
            }
            for (int state = UI_EMPTY; state <= UI_OFFLINE; ++state) {  // 分别检查空、加载、错误和离线四种服务状态。
                ui_service_state(&desk, (enum ui_state)state);  // 把整数状态转枚举并刷新状态显示。
                ui_open(&desk, UI_FILES_APP);  // 把后续键事件转到Files视图。
                ui_draw(&desk, &fb);  // 在有哨兵和填充的本次像素数组内绘制。
                ui_open(&desk, UI_TASKS_APP);  // Tasks也需在各服务状态下绘制。
                ui_draw(&desk, &fb);  // 在有哨兵和填充的本次像素数组内绘制。
            }
            desk.dialog = UI_DIALOG_SAVE_AS;  // 当前显示另存为输入框。
            strcpy(desk.dialog_title, "Save document");  // 构造另存为标题，覆盖确认框文字布局。
            strcpy(desk.dialog_text, "A long path or document name stays inside the dialog.");  // 完整说明仍应限制在对话框矩形内。
            memset(desk.dialog_input, 'x', 127);  // 填满输入框有效内容，观察长名称裁剪。
            desk.dialog_input[127] = 0;  // 保留第128字节终止NUL。
            ui_draw(&desk, &fb);  // 在有哨兵和填充的本次像素数组内绘制。
            u_round((struct ui_rect){-4, -3, 14, 12}, 6, U_ACCENT);  // 圆角越过左上边界仍只能写有效像素。
            u_round((struct ui_rect){(int)widths[k] - 3, (int)heights[k] - 4, 12, 14}, 6, U_BLUE);  // 同样验证右下裁剪。
            for (unsigned i = 0; i < 32; ++i)  // 检查数组首尾各32字节的写边界。
                assert(p[i] == 0xa5 && p[size + 32 + i] == 0xa5);  // 前后哨兵均未变化才说明这组绘图没越界。
            for (unsigned y = 0; y < heights[k]; ++y)  // 按真实行数检查每一行填充。
                for (unsigned x = widths[k] * bytes; x < pitch; ++x)  // 只遍历可见像素后到实际pitch之间的字节。
                    assert(p[32 + (size_t)y * pitch + x] == 0xa5);  // 行填充未变化，才能排除将pitch误当宽度的写错。
            free(p);  // 全部绘图和检查完成后才释放整块主机内存。
        }
}
static void mouse_windows(void) {  // 通过 ui_pointer 发送 C 事件序列，观察移动、最大化、最小化与关闭。
    start();  // 以重新初始化的参考服务和桌面开始本项检查。
    struct ui_window *w = &desk.windows[UI_EDITOR];  // 借用同一Editor窗口对象，后续事件直接修改它。
    int ox = w->rect.x, oy = w->rect.y;  // 保存初始左上坐标供位移比较。
    ui_pointer(&desk, ox + 220, oy + 15, 1);  // 在标题栏普通位置按下，启动移动并保存偏移。
    ui_pointer(&desk, ox + 250, oy + 40, 0);  // 鼠标横移30、纵移25，核心应保持按下偏移。
    ui_pointer(&desk, ox + 250, oy + 40, 2);  // 释放停止移动，保留新的窗口矩形。
    assert(w->rect.x == ox + 30 && w->rect.y == oy + 25);  // 实际窗口位移必须与鼠标位移一致。
    ui_pointer(&desk, w->rect.x + w->rect.w - 55, w->rect.y + 16, 1);  // 标题栏最大化按钮坐标，用当前实际矩形定位。
    assert(w->maximized);  // 首次点击进入最大化状态。
    ui_pointer(&desk, w->rect.x + w->rect.w - 55, w->rect.y + 16, 1);  // 标题栏最大化按钮坐标，用当前实际矩形定位。
    assert(!w->maximized);  // 再点击应恢复普通窗口。
    ui_pointer(&desk, w->rect.x + w->rect.w - 85, w->rect.y + 16, 1);  // 最小化按钮把窗口暂时移出命中/绘制。
    assert(w->minimized);  // 最小化必须保留对应状态。
    ui_open(&desk, UI_EDITOR);  // 将Editor再次置顶，不创建第二个Editor窗口。
    assert(!w->minimized);      // ui_open重新显示同一窗口，不创建副本。
    typing("changed");  // 制造尚未保存的修改，供关闭失败观察。
    ui_pointer(&desk, w->rect.x + w->rect.w - 25, w->rect.y + 16, 1);  // 点击关闭，Editor有修改时必须先出确认框。
    assert(desk.dialog == UI_DIALOG_CLOSE);  // 未保存内容不能直接丢失或自动关闭。
    ui_pointer(&desk, desk.width / 2 + 36, desk.height / 2 + 78, 1);  // 命中关闭确认框的放弃按钮。
    assert(!w->open && !desk.dirty);  // 放弃后窗口关闭且工作文本回到已保存状态。
    assert(!strcmp(desk.editor, desk.saved));  // 放弃动作恢复saved字节，不写新文本到服务。
}
static void density_hit_boxes(void) {  // 密度状态只由两个当前可见按钮改变，空白不冒充Compact。
    start();  // 独立重建参考服务与桌面，不继承前一项的模式或对话框。
    ui_open(&desk, UI_SETTINGS);  // Settings成为当前内容点击接收者。
    struct ui_rect body = u_window_body(desk.windows[UI_SETTINGS].rect);  // 按同一标题与边框度量取得内容原点。
    ui_apps_click(&desk, UI_SETTINGS, body, body.x + 160, body.y + 266, false);  // 两126像素按钮之间的十二像素间隙。
    assert(desk.density == 0);  // 未命中任何按钮，宽松模式保留。
    ui_apps_click(&desk, UI_SETTINGS, body, body.x + body.w - 1, body.y + 266, false);  // 同高度右侧空白不是Compact。
    assert(desk.density == 0);  // 宽泛横条判断会在此错误切换。
    ui_apps_click(&desk, UI_SETTINGS, body, body.x + 176, body.y + 266, false);  // Compact矩形内部产生有效动作。
    assert(desk.density == 1);  // 有效按钮必须可切到紧凑模式。
    ui_apps_click(&desk, UI_SETTINGS, body, body.x + 38, body.y + 266, false);  // Comfortable矩形内部回到宽松模式。
    assert(desk.density == 0);  // 两个方向都通过实际内容分派执行。
}

int main(void) {  // 观察程序入口；返回非零表示预期状态未出现。
    density_hit_boxes();  // 先核对空白、间隙与两个真实密度按钮。
    appearance();  // 换色验收先于通用应用回归。
    explicit_contracts();  // 先检查输入拒绝、目录目标与可见选择合同。
    workflow();  // 以独立重置后的状态检查完整文件编辑过程。
    input_boundaries();  // 运行容量、查找、滚动及坏CSV边界。
    mouse_windows();     // 检查跨阶段鼠标窗口动作。
    boundary_render();   // 最后覆盖应用、状态和多布局绘图哨兵。
    puts("PASS frontend: Day/Night state preservation, document workflow, provider failures, CRUD, task actions, window input, 7 "  // 所有assert均通过后才打印最终结果，字符串继续到下一行。
         "applications/states, 24/32 bpp guarded rendering (ASan/UBSan)");  // 相邻C字符串字面量在编译时合并，不是第二次输出。
    return 0;  // 正常结束当前程序，成功状态交给启动它的命令。
}
