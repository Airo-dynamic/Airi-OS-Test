#include <airi/kernel.h>   // 取得启动信息、基础库与串口日志接口。
#include <airi/console.h>  // 取得命令环、按键与视图类型。
#include <assert.h>  // 取得条件不成立就中止的 assert；检查构建不能定义 NDEBUG。
#include <limits.h>  // 取得INT_MIN、UINT_MAX、LLONG_MIN，检查极值格式化。
#include <stdio.h>   // 取得主机 printf、文件输出与错误报告接口。
#include <stdlib.h>  // 取得主机分配、释放、退出与数值转换接口。
#include <string.h>  // 取得宿主字符串比较、复制和查找接口。

static void memory_tests(void) {  // 同一输入分别交自制函数和宿主libc，比较全数组而非仅搬动区。
    unsigned char ours[96], oracle[96], input[96];  // 三个独立96字节数组保存自制结果、宿主对照和原输入。
    for (size_t i = 0; i < sizeof(input); ++i)      // sizeof数组给出全部96字节，逐项构造输入。
        input[i] = (unsigned char)(i * 37);  // 把递增乘积截到字节，生成容易暴露偏移错误的图案。
    for (size_t n = 0; n <= 64; ++n) {  // 覆盖零长度到64字节，包括不做访问的零边界。
        airi_memset(ours, 0xa5, sizeof(ours));  // 自制填充先覆盖整个结果数组。
        memset(oracle, 0xa5, sizeof(oracle));   // 宿主以相同值和容量建立对照初态。
        airi_memcpy(ours + 8, input, n);  // 自制复制从偏移8开始，周围哨兵字节也参与后续比较。
        memcpy(oracle + 8, input, n);     // 对照采用相同源、目标偏移和长度。
        assert(memcmp(ours, oracle, sizeof(ours)) == 0);  // 全96字节必须相同，额外写到复制区外也会失败。
        for (size_t from = 0; from < 16; ++from)  // 源起点遍历0..15，覆盖多种相对距离。
            for (size_t to = 0; to < 16; ++to) {  // 目标起点同样遍历，产生左右重叠和同地址。
                airi_memcpy(ours, input, sizeof(ours));   // 每次恢复原输入，上一轮移动不污染本次。
                memcpy(oracle, input, sizeof(oracle));    // 对照也恢复相同原输入。
                airi_memmove(ours + to, ours + from, n);  // 在同一数组内移动n字节，自制函数必须保护重叠源。
                memmove(oracle + to, oracle + from, n);   // 宿主标准实现处理完全相同的重叠输入。
                assert(memcmp(ours, oracle, sizeof(ours)) == 0);  // 全96字节必须相同，额外写到复制区外也会失败。
            }
    }
    assert(airi_memcmp("abc", "abd", 3) < 0);   // 首个不同字节c小于d，返回符号应为负。
    assert(airi_memcmp("abd", "abc", 3) > 0);   // 交换参数后符号应为正，不要求固定返回差值。
    assert(airi_memcmp("abc", "abc", 3) == 0);  // 三个有效字节完全相同，结果为零。
    assert(airi_memcmp(NULL, NULL, 0) == 0);    // 零长度不得解引用NULL，比较结果为相等。
    assert(airi_strlen("") == 0 && airi_strlen("AiriOS") == 6);  // 文本长度不计NUL，分别为零与六字节。
}
static void format_tests(void) {  // 在已支持格式范围内，与宿主snprintf比较长度和每个写入字节。
    char ours[128], oracle[128];  // 独立128字节输出数组，未写区域也要保持相同。
    for (size_t cap = 0; cap < sizeof(ours); ++cap) {  // 逐一检查0..127容量，观察统计、终止和截断行为。
        memset(ours, 0x55, sizeof(ours));      // 预置整数组，后面可发现写过声明容量。
        memset(oracle, 0x55, sizeof(oracle));  // 对照初态一致，比较包含未被写区域。
        int a = airi_snprintf(ours, cap, "%s %c %d %u %08x %lld %zu %%", "hello", '!', INT_MIN,  // 自制格式化覆盖字符串、字符、最小int与多种整数格式。
                              UINT_MAX, 0xab, LLONG_MIN, (size_t)42);  // 后续实参类型与%u/%x/%lld/%zu对应，极值检查负数转换。
        int b = snprintf(oracle, cap, "%s %c %d %u %08x %lld %zu %%", "hello", '!', INT_MIN,  // 宿主处理同一格式、容量与参数，得到对照返回长度。
                         UINT_MAX, 0xab, LLONG_MIN, (size_t)42);  // 后续实参类型与%u/%x/%lld/%zu对应，极值检查负数转换。
        assert(a == b && memcmp(ours, oracle, sizeof(ours)) == 0);  // 应完整长度与全数组内容都相同，不能只比较可见前缀。
    }
    assert(airi_snprintf(NULL, 0, "hello %u", 42u) == 8);  // 容量零只统计八字节结果，不访问NULL目标。
    assert(airi_snprintf(ours, 1, "abc") == 3 && ours[0] == 0);  // 容量一只能写NUL，返回值仍为完整文本长三。
    airi_snprintf(ours, sizeof(ours), "%s %p", (char *)NULL, (void *)(uintptr_t)0x1234);  // 显式按变参要求传字符指针与void指针，检查空值和地址格式。
    assert(strcmp(ours, "(null) 0x1234") == 0);  // 当前教学格式器的明确空字符串/指针表示必须一致。
    airi_snprintf(ours, sizeof(ours), "bad %q end%");  // 不支持格式与孤立百分号使用当前约定的原样保留。
    assert(strcmp(ours, "bad %q end%") == 0);  // 不应吞掉未识别格式或越过字符串末尾。
}
static AiriBootInfo fixture(void) {  // 构造合法启动输入，各故障都从它的独立副本开始。
    AiriBootInfo b = {.magic = AIRI_BOOT_MAGIC,  // 指定magic识别协议，未指定成员按C规则清零。
                      .version = AIRI_BOOT_VERSION,  // 采用当前接口版本。
                      .size = sizeof(AiriBootInfo),  // 字节大小与编译的结构布局一致。
                      .direct_map_offset = UINT64_C(0xffff800000000000),  // UINT64_C明确64位常量，用作物理直接映射偏移。
                      .kernel_physical_base = 0x100000,  // 构造内核物理起点1MiB。
                      .kernel_virtual_base = UINT64_C(0xffffffff80000000),  // 内核虚拟起点与链接模型对应。
                      .kernel_size = 4096,  // 构造一页4096字节内核长度。
                      .region_count = 2};   // 只有两条内存区域有效。
    b.regions[0] =  // 第一项描述0..1MiB保留区域。
        (struct airi_memory_region){.base = 0, .length = 0x100000, .kind = AIRI_MEMORY_RESERVED};  // 复合字面量按字段赋值，物理范围不标为可分配。
    b.regions[1] = (struct airi_memory_region){  // 第二项从1MiB开始，紧邻首项且不重叠。
        .base = 0x100000, .length = 0x400000, .kind = AIRI_MEMORY_USABLE};  // 1..5MiB共4MiB构造可用范围。
    return b;  // 按值返回整个结构，调用者得到可独立损坏的副本。
}
static void boot_tests(void) {  // 每次只损坏一项，具体失败原因不会被前次残留掩盖。
    AiriBootInfo b = fixture();  // 在当前调用栈中取得合法完整副本。
    assert(airi_boot_validate(&b) == NULL);  // 合法输入返回NULL表示没有错误原因。
    assert(airi_memory_bytes(&b, AIRI_MEMORY_USABLE) == 0x400000);  // 可用字节必须仅累加4MiB可用项。
    assert(airi_boot_validate(NULL) != NULL);  // 缺整个描述时返回具体错误，不能解引用。
    b.version++;  // 后置自增使协议版本不匹配，测试未知布局拒绝路径。
    assert(airi_boot_validate(&b) != NULL);  // 本次损坏输入必须返回非空错误原因。
    b = fixture();  // 恢复合法原输入，下一次只改变紧随其后的字段。
    b.region_count = 129;  // 超过128条固定容量，必须先拒绝而不遍历越界。
    assert(airi_boot_validate(&b) != NULL);  // 本次损坏输入必须返回非空错误原因。
    b = fixture();  // 恢复合法原输入，下一次只改变紧随其后的字段。
    b.region_count = 0;  // 地图无条目，缺少所需可用区域信息。
    assert(airi_boot_validate(&b) != NULL);  // 本次损坏输入必须返回非空错误原因。
    b = fixture();  // 恢复合法原输入，下一次只改变紧随其后的字段。
    b.regions[1].base = UINT64_MAX - 1;      // 起点接近最大值，加长度会溢出，不能接受绕回范围。
    assert(airi_boot_validate(&b) != NULL);  // 本次损坏输入必须返回非空错误原因。
    b = fixture();  // 恢复合法原输入，下一次只改变紧随其后的字段。
    b.regions[1].base = 1;  // 第二项落入首项内部，制造物理范围重叠。
    assert(airi_boot_validate(&b) != NULL);  // 本次损坏输入必须返回非空错误原因。
    b = fixture();  // 恢复合法原输入，下一次只改变紧随其后的字段。
    b.regions[1].kind = AIRI_MEMORY_RESERVED;  // 没有任何可用项时应拒绝启动合同。
    assert(airi_boot_validate(&b) != NULL);    // 本次损坏输入必须返回非空错误原因。
    b = fixture();  // 恢复合法原输入，下一次只改变紧随其后的字段。
    b.kernel_physical_base = UINT64_MAX;     // 内核起点加长度溢出，不能宣称可读取。
    assert(airi_boot_validate(&b) != NULL);  // 本次损坏输入必须返回非空错误原因。
    b = fixture();  // 恢复合法原输入，下一次只改变紧随其后的字段。
    memset(b.loader, 'X', sizeof(b.loader));  // 填满名称数组但不留NUL，必须拒绝无界字符串。
    assert(airi_boot_validate(&b) != NULL);   // 本次损坏输入必须返回非空错误原因。
}
static void type(struct airi_console *c, const char *s) {  // 把完整命令按字节送入同一个console，再回车执行。
    while (*s)  // 当前字节不是NUL才继续输入。
        airi_console_key(c, *s++);  // 先取当前字节送键事件，再将源指针推进。
    airi_console_key(c, '\n');  // 命令末尾明确发送提交键，不把NUL当按键。
}
static bool contains(const struct airi_console *c, const char *s) {  // 在仍保留的逻辑日志范围内搜索目标子串。
    for (uint64_t i = c->total_lines - c->line_count; i < c->total_lines; ++i)  // 起点排除环中已被覆盖的行，终点为最新尾部。
        if (strstr(airi_console_line(c, i), s))  // 行接口转换逻辑下标，strstr非NULL表示包含目标。
            return true;  // 当前保留的日志行中至少有一行包含目标文本。
    return false;  // 全部仍保留的日志行都不含目标文本。
}
static void console_tests(void) {  // 检查命令、历史、环形输出、容量、分页和扫描码状态。
    AiriBootInfo b = fixture();  // 在当前调用栈中取得合法完整副本。
    struct airi_console c;      // 独立命令状态在当前检查期间有效。
    airi_console_init(&c, &b);  // 借用当前合法启动描述，初始化命令与日志。
    type(&c, "   mem  ");  // 命令含前后空格，解析应仍识别mem。
    assert(contains(&c, "4096"));  // mem汇总fixture的4MiB可用区域，按KiB输出4096。
    type(&c, "mem map");  // 输出同一启动描述中的完整物理地图。
    assert(contains(&c, "0000000000500000 USABLE"));  // 可用项结束地址是5MiB，种类必须匹配。
    type(&c, "does-not-exist");  // 明确走未知命令错误分支。
    assert(contains(&c, "unknown command"));  // 未识别输入应留下原因文字。
    type(&c, "echo ready");  // 按参数原样回显当前文字。
    assert(contains(&c, "ready"));  // echo结果确实进入日志环。
    type(&c, "devices");  // 查询fixture的设备可用状态。
    assert(contains(&c, "framebuffer") == false);  // fixture未提供图形信息，不应声称有framebuffer设备。
    assert(contains(&c, "unavailable"));  // 缺设备要明确报告不可用。
    airi_console_key(&c, 'x');  // 保存尚未提交的输入，供历史往返恢复。
    airi_console_key(&c, AIRI_KEY_UP);    // 向上回取最近devices命令。
    assert(!strcmp(c.input, "devices"));  // 当前输入应替换为历史文本。
    airi_console_key(&c, AIRI_KEY_DOWN);  // 返回当前未提交草稿。
    assert(!strcmp(c.input, "x"));  // 历史浏览不能丢掉先前草稿x。
    airi_console_key(&c, 8);      // ASCII退格删除草稿最后字节。
    assert(c.input_length == 0);  // 删除唯一字节后输入长度归零。
    for (unsigned i = 0; i < 100; ++i)  // 多次输入故意超过命令缓冲有效容量。
        airi_console_key(&c, 'a');  // 单个ASCII字符持续尝试追加。
    assert(c.input_length == AIRI_COMMAND_SIZE - 1 && c.input[AIRI_COMMAND_SIZE - 1] == 0);  // 容量包含NUL，截断时仍必须正确终止。
    airi_console_key(&c, '\n');  // 提交当前输入，后续检查从空命令字段继续。
    for (unsigned i = 0; i < 400; ++i)  // 产生多于日志环和历史容量的重复命令。
        type(&c, "echo ring");  // 每次提交将输出和命令历史同时推进。
    assert(c.line_count == AIRI_CONSOLE_LINES && c.history_count == AIRI_HISTORY_SIZE);  // 容量满后有效条数饱和，逻辑总计可继续增长。
    assert(!strcmp(airi_console_line(&c, c.total_lines - 2), "ring"));  // 最新命令结果的逻辑行仍能取到ring。
    assert(!strcmp(airi_console_line(&c, 0), ""));  // 已被环覆盖的早期逻辑行返回空串，不访问旧数组位置。
    type(&c, "clear");  // 执行清屏并保留当前提示规则。
    assert(c.line_count == 1 && c.input_length == 0);  // 清屏后只有新的命令视图行，输入为空。
    unsigned count = c.commands;  // 记录空白提交前累计命令数。
    type(&c, "    ");  // 全空白输入不构成实际命令。
    assert(c.commands == count);  // 空白提交不得增加命令计数。
    airi_console_key(&c, AIRI_KEY_F2);   // 键事件切到内存页面。
    assert(c.view == AIRI_VIEW_MEMORY);  // 功能键转换必须选择对应视图。
    airi_console_key(&c, AIRI_KEY_PAGE_DOWN);  // 内存页面尝试向后翻一页。
    assert(c.map_page == 0);  // 初始只有两条区域，分页上限零，不能继续翻出空页。
    b.region_count = 128;     // 扩大fixture的有效地图数量，检查页号上限算法。
    for (unsigned i = 0; i < 30; ++i)  // 重复翻页故意超过实际最大页号。
        airi_console_key(&c, AIRI_KEY_PAGE_DOWN);  // 内存页面尝试向后翻一页。
    assert(c.map_page == 10);  // 128条按当前每页容量应限制在页10。
    for (unsigned i = 0; i < 30; ++i)  // 重复翻页故意超过实际最大页号。
        airi_console_key(&c, AIRI_KEY_PAGE_UP);  // 从当前内存页向前翻页，首页面不能下溢。
    assert(c.map_page == 0);  // 128条地图连续向前翻页后回到第0页，不会向下回绕
    assert(airi_keyboard_decode(0x1e) == 'a');  // 普通A键按下扫描码应得到小写ASCII。
    assert(airi_keyboard_decode(0x9e) == 0);    // 对应释放扫描码不产生文本输入。
    airi_keyboard_decode(0x2a);  // 按下左Shift，登记其独立状态。
    airi_keyboard_decode(0x36);  // 按下右Shift，左右状态不能互相覆盖。
    airi_keyboard_decode(0xaa);  // 释放左Shift，但右Shift仍按住。
    assert(airi_keyboard_decode(0x1e) == 'A');  // 右Shift仍有效，A键应产生大写。
    airi_keyboard_decode(0xb6);  // 释放右Shift，两侧都不再按住。
    assert(airi_keyboard_decode(0x1e) == 'a');  // 普通A键按下扫描码应得到小写ASCII。
    airi_keyboard_decode(0xe0);  // 扩展前缀改变下一扫描码的解释方式。
    assert(airi_keyboard_decode(0x51) == AIRI_KEY_PAGE_DOWN);  // 扩展扫描码映射为分页功能值，不是ASCII字符。
    airi_keyboard_decode(0x1d);  // 按下Ctrl，后续L识别为清屏组合。
    assert(airi_keyboard_decode(0x26) == AIRI_KEY_CLEAR);  // Ctrl-L应返回清屏功能键值。
    airi_keyboard_decode(0x9d);  // Ctrl释放必须清掉修饰状态。
    assert(airi_keyboard_decode(0x3e) == AIRI_KEY_F4);  // 普通F4扫描码对应日志页面功能键。
}
int main(void) {  // 观察程序入口；返回非零表示预期状态未出现。
    memory_tests();   // 先运行字节移动与比较的宿主对照。
    format_tests();   // 检查支持格式、容量、极值及未知格式行为。
    boot_tests();     // 验证启动结构及独立损坏输入。
    console_tests();  // 检查纯命令状态、历史、分页和扫描码。
    puts("PASS host: memory oracle, overlap, formatting, boot validation, console "  // 只有所有assert完成才打印本轮PASS，字符串继续到下一行。
         "history/ring/paging, scancodes (ASan/UBSan)");  // 相邻字符串在C编译期连接，不是另一次输出。
    return 0;  // 正常结束当前程序，成功状态交给启动它的命令。
}
