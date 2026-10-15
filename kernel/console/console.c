#include <airi/console.h>  // 引入控制台状态与逻辑键编号
#include <airi/base.h>     // 引入自有字节操作和有界格式化函数

void airi_console_print(struct airi_console *c, const char *fmt, ...) {  // 向console输出环新增一条有界格式化记录
    char *line = c->lines[c->total_lines % AIRI_CONSOLE_LINES];  // 逻辑行号取余得到可复用槽位，line借用该行的128字节数组
    va_list args;  // 准备遍历fmt之后实际传入的格式参数
    va_start(args, fmt);  // 从最后一个具名参数fmt之后开始读取格式实参
    airi_vsnprintf(line, AIRI_CONSOLE_WIDTH, fmt, args);  // 把本条记录格式化到输出环，容量包含终止零
    va_end(args);      // 结束本轮可变参数读取
    ++c->total_lines;  // 累计逻辑行号前进一行，不因环绕覆盖归零
    if (c->line_count < AIRI_CONSOLE_LINES)  // 环尚未装满时才增加仍保留的行数
        ++c->line_count;  // 记录环中又多了一条仍可读取的输出
}
const char *airi_console_line(const struct airi_console *c, uint64_t sequence) {  // 按逻辑行号借用仍保留的日志文字
    if (sequence >= c->total_lines || c->total_lines - sequence > c->line_count)  // 拒绝未来行号和已经被覆盖的历史行号
        return "";  // 无可读取记录时返回有效的空字符串
    return c->lines[sequence % AIRI_CONSOLE_LINES];  // 按逻辑行号定位槽位，返回借用指针，后续覆盖可能改变内容
}
static bool equal(const char *a, const char *b) {  // 按长度和内容判断两个NUL字符串相同
    size_t n = airi_strlen(a);  // 取得第一个待比较字符串的内容字节数，稍后要求第二个长度也相同
    return n == airi_strlen(b) && !airi_memcmp(a, b, n);  // 先确认长度相同，再比较全部内容字节；memcmp返回0才相等
}
unsigned airi_journal_count(void) {  // 返回当前固定启动记录数量
    return 8;  // 启动记录固定8项，调用者据此限制查询下标
}
void airi_journal_line(const struct airi_console *c, unsigned i, char *s, size_t n) {  // 把指定启动事件格式化到调用者输出缓冲
    const AiriBootInfo *b = c->boot;  // 借用控制台绑定的启动结构，后续记录从这里取真实字段
    switch (i) {  // 按调用者请求的记录下标选择当前文字内容
    case 0:  // 选择加载器身份记录
        airi_snprintf(s, n, "0000 boot     loader=%s", b->loader);  // 把启动信息中的加载器名称写成第0条记录，调用者随后保存或绘制它
        break;  // 本项文字已形成，结束switch而不继续生成下一项
    case 1:  // 选择交接合同版本与内存地图条数记录
        airi_snprintf(s, n, "0001 handoff  ABI v%u validated; %u memory regions", b->version,  // 记录本次启动合同版本与有效内存区域条数，两项都来自已校验的启动结构
                      b->region_count);  // 把内存地图有效区域条数作为本条记录的%u实参
        break;  // 本项文字已形成，结束switch而不继续生成下一项
    case 2:  // 选择内核物理装载范围记录
        airi_snprintf(s, n, "0002 image    physical=0x%llx bytes=%llu",  // 记录内核物理装载起点与镜像长度，地址按十六进制、长度按字节显示
                      (unsigned long long)b->kernel_physical_base,  // 内核物理起点用64位无符号格式打印，指向实际装载范围
                      (unsigned long long)b->kernel_size);  // 内核镜像字节长度按%llu要求的类型传入
        break;  // 本项文字已形成，结束switch而不继续生成下一项
    case 3:  // 选择直接映射偏移记录
        airi_snprintf(s, n, "0003 mapping  direct-map=0x%llx",  // 记录物理地址转直接映射虚拟地址时需加的固定偏移
                      (unsigned long long)b->direct_map_offset);  // 将直接映射固定偏移交给64位十六进制格式
        break;  // 本项文字已形成，结束switch而不继续生成下一项
    case 4:  // 选择可用物理内存量记录
        airi_snprintf(s, n, "0004 memory   usable=%llu KiB; reclaim regions held",  // 记录当前可用物理范围总量，转为KiB，并明确引导可回收范围仍未释放
                      (unsigned long long)(airi_memory_bytes(b, AIRI_MEMORY_USABLE) / 1024));  // 字节除1024转KiB，再按%llu约定提供unsigned long long
        break;  // 本项文字已形成，结束switch而不继续生成下一项
    case 5:  // 选择COM1串口访问方式记录
        airi_snprintf(s, n, "0005 serial   COM1 115200 8N1; polled I/O");  // 记录已配置的COM1速率、8位数据无校验1停止位及轮询访问方式
        break;  // 本项文字已形成，结束switch而不继续生成下一项
    case 6:  // 选择实际图形可用性记录
        airi_snprintf(s, n, "0006 display  %s",  // 按实际帧缓冲校验结果选择图形可用或仅串口仍活跃的说明
                      airi_framebuffer_valid(&b->framebuffer)  // 先用尺寸、位域与地址约束判断当前帧缓冲描述能否绘图
                          ? "mapped framebuffer available"  // 校验通过时报告有可访问的帧缓冲
                          : "framebuffer absent; serial console active");  // 校验失败时说明图形缺失，现有串口通路仍可输出
        break;  // 本项文字已形成，结束switch而不继续生成下一项
    default:  // 固定8项记录中的最后一项为本地命令输入状态
        airi_snprintf(s, n, "0007 console  local command interpreter; logical-key input");  // 最后一项说明本地命令解释器接收设备轮询转换后的逻辑键
        break;  // 本项文字已形成，结束switch而不继续生成下一项
    }
}
void airi_device_line(const struct airi_console *c, unsigned i, char *s, size_t n) {  // 把指定设备描述格式化到调用者输出缓冲
    const struct airi_framebuffer *f = &c->boot->framebuffer;  // 取启动信息内嵌帧缓冲描述的地址，未复制像素内存
    switch (i) {  // 按调用者请求的记录下标选择当前文字内容
    case 0:  // 选择已配置COM1串口设备
        airi_snprintf(s, n, "ttyS0   serial    COM1 0x03f8 / 115200 / 8N1 / polling");  // 形成串口设备记录，包含实际端口基址、速率与轮询方式
        break;  // 此设备说明已形成，结束switch分派
    case 1:  // 选择固定i8042键盘设备
        airi_snprintf(s, n, "kbd0    input     i8042 / ports 0x60,0x64 / US layout");  // 形成固定键盘控制器记录，说明数据与状态端口以及当前US键位范围
        break;  // 此设备说明已形成，结束switch分派
    default:  // 第三个固定设备项读取实际帧缓冲描述
        if (airi_framebuffer_valid(f))  // 描述满足绘图前提时才显示实际分辨率和布局
            airi_snprintf(s, n, "fb0     display   %llux%llu / %u bpp / pitch %llu",  // 形成实际帧缓冲设备记录：宽高为像素，bpp为每像素位数，pitch为每行字节数
                          (unsigned long long)f->width, (unsigned long long)f->height,  // 把软件画布像素宽度、软件画布像素高度转成格式要求的unsigned long long
                          (unsigned)f->bpp, (unsigned long long)f->pitch);  // 按本次格式或坐标计算的整数类型传入当前字段值
        else  // 前一分支未成立时执行下方替代操作
            airi_snprintf(s, n, "fb0     display   unavailable");  // 图形描述不满足绘图前提时明确写不可用，避免读取无效像素地址
    }
}
void airi_map_line(const struct airi_console *c, unsigned i, char *s, size_t n) {  // 把指定物理内存范围格式化成一行文字
    if (i >= c->boot->region_count) {  // 查询下标超过有效内存区域时生成空输出
        if (n)  // 输出容量非零时才允许写入首字节
            s[0] = 0;  // 把调用者输出标成空的NUL字符串
        return;  // 行号超出地图有效项，保留空输出并返回，不能读取数组越界项
    }
    const struct airi_memory_region *r = &c->boot->regions[i];  // 借用第i条实际内存范围，读取起点、长度和类别
    airi_snprintf(s, n, "%03u %016llx %016llx %s", i, (unsigned long long)r->base,  // 记录区域下标与物理半开区间左右端点，地址固定十六进制宽度方便逐列比较
                  (unsigned long long)(r->base + r->length), airi_memory_name(r->kind));  // 把当前64位字段转成格式要求的unsigned long long
}
static void system_info(struct airi_console *c) {  // 将启动身份和内核尺寸加入命令输出
    airi_console_print(c, "AiriOS 0.1 / x86_64 / bootstrap kernel");  // 将AiriOS 0.1 / x86_64 / bootstrap kernel保存为一条命令说明或身份记录
    airi_console_print(c, "Console: local    CPU: single    Boot: UEFI / %s", c->boot->loader);  // 把本次查询字段按Console: local    CPU: single    Boot: UEFI / %s的格式保存到输出环
    airi_console_print(c, "Kernel image: %llu KiB    Boot ABI: v%u",  // 把本次查询字段按Kernel image: %llu KiB    Boot ABI: v%u的格式保存到输出环
         (unsigned long long)(c->boot->kernel_size / 1024), c->boot->version);  // 字节除1024转KiB，再按%llu约定提供unsigned long long
}
void airi_console_init(struct airi_console *c, const AiriBootInfo *b) {  // 清命令状态并绑定只读启动结构，形成初始文字
    airi_memset(c, 0, sizeof(*c));  // 清零整个当前状态或快照，使计数和标记从确定初值开始
    c->boot = b;     // 将借用启动信息，描述与内存地图须持续有效置为b，后续读取使用新状态
    system_info(c);  // 从同一启动结构生成系统身份与内核镜像尺寸
    airi_console_print(c, "");  // 在输出环中保存一条空行，为后续显示保留间隔
    airi_console_print(c, "System console attached.");  // 将System console attached.保存为一条命令说明或身份记录
    airi_console_print(c, "help       Command reference");  // 将help       Command reference保存为一条命令说明或身份记录
    airi_console_print(c, "mem        Physical memory inventory");  // 将mem        Physical memory inventory保存为一条命令说明或身份记录
    airi_console_print(c, "devices    Console, input and display endpoints");  // 将devices    Console, input and display endpoints保存为一条命令说明或身份记录
    airi_console_print(c, "log        Boot journal");  // 将log        Boot journal保存为一条命令说明或身份记录
    airi_console_print(c, "");  // 在输出环中保存一条空行，为后续显示保留间隔
}
static void execute(struct airi_console *c) {  // 读取完整提交命令，更新历史并产生结果
    char command[AIRI_COMMAND_SIZE];  // 本次提交使用独立命令副本，避免清输入时丢失分派文字
    airi_memcpy(command, c->input, sizeof(command));  // 连同终止零复制完整输入数组到本次提交副本
    char *start = command;  // 先让命令起点指向副本首字节
    while (*start == ' ')   // 起点仍为空格时继续向命令正文前进
        ++start;  // 跳过一个前导空格，原数组保留但起点向后移动
    size_t length = airi_strlen(start);  // 测量去掉前导空格后的内容长度，不计终止零
    while (length && start[length - 1] == ' ')  // 有内容且末字节为空格时缩短有效命令
        start[--length] = 0;  // 先缩短长度，再在被移除的尾部空格处写NUL
    if (!length)  // 首尾去空格后为空命令时不提交
        return;  // 输入只有空白，返回而不增加命令计数或记录历史
    airi_console_print(c, "airi> %s", start);  // 将本次真正提交的命令保存为输出环的一行
    ++c->commands;  // 非空命令的累计提交次数增加一次
    if (c->history_count == AIRI_HISTORY_SIZE) {  // 8条历史已满时腾出最后一个槽
        airi_memmove(c->history, c->history + 1, (AIRI_HISTORY_SIZE - 1) * AIRI_COMMAND_SIZE);  // 后7条完整历史前移一槽，重叠区域用memmove保住待读字节
        --c->history_count;  // 覆盖最旧历史后暂记7条，为本条提交留位置
    }
    airi_snprintf(c->history[c->history_count++], AIRI_COMMAND_SIZE, "%s", start);  // 先在旧计数下标保存命令，再把历史计数加一
    c->view = AIRI_VIEW_TERMINAL;  // 切回Terminal显示当前输入或命令结果
    c->scroll = 0;  // 回到输出尾部，便于看到最新命令
    char line[AIRI_CONSOLE_WIDTH];  // 命令查询先在128字节局部数组形成一行，再由airi_console_print复制进输出环
    if (equal(start, "help")) {     // 提交文字完整等于help时生成命令列表
        airi_console_print(c, "COMMAND    OPERATION");  // 将COMMAND    OPERATION保存为一条命令说明或身份记录
        airi_console_print(c, "sys        Kernel and boot identity");  // 将sys        Kernel and boot identity保存为一条命令说明或身份记录
        airi_console_print(c, "mem        Usable / held physical memory summary");  // 将mem        Usable / held physical memory summary保存为一条命令说明或身份记录
        airi_console_print(c, "mem map    Complete physical address inventory");  // 将mem map    Complete physical address inventory保存为一条命令说明或身份记录
        airi_console_print(c, "devices    Registered console hardware endpoints");  // 将devices    Registered console hardware endpoints保存为一条命令说明或身份记录
        airi_console_print(c, "log        Boot journal (ordered events, no wall clock)");  // 将log        Boot journal (ordered events, no wall clock)保存为一条命令说明或身份记录
        airi_console_print(c, "echo TEXT  Write text to this console");  // 将echo TEXT  Write text to this console保存为一条命令说明或身份记录
        airi_console_print(c, "clear      Clear terminal scrollback");  // 将clear      Clear terminal scrollback保存为一条命令说明或身份记录
        airi_console_print(c, "F1-F4      Terminal / Memory / Devices / Journal");  // 将F1-F4      Terminal / Memory / Devices / Journal保存为一条命令说明或身份记录
        airi_console_print(c, "PgUp/PgDn  Scroll terminal or memory inventory");  // 将PgUp/PgDn  Scroll terminal or memory inventory保存为一条命令说明或身份记录
        airi_console_print(c, "Up/Down    Command history    Ctrl-L: clear");  // 将Up/Down    Command history    Ctrl-L: clear保存为一条命令说明或身份记录
    } else if (equal(start, "sys"))  // 完整匹配sys时查询同一启动身份
        system_info(c);  // 从同一启动结构生成系统身份与内核镜像尺寸
    else if (equal(start, "mem")) {  // 完整匹配mem时按启动地图类别统计字节数
        airi_console_print(c, "TYPE                  KiB");  // 将TYPE                  KiB保存为一条命令说明或身份记录
        airi_console_print(c, "usable                %llu",  // 把本次查询字段按usable                %llu的格式保存到输出环
             (unsigned long long)(airi_memory_bytes(c->boot, AIRI_MEMORY_USABLE) / 1024));  // 字节除1024转KiB，再按%llu约定提供unsigned long long
        airi_console_print(c, "boot reclaim (held)   %llu",  // 把本次查询字段按boot reclaim (held)   %llu的格式保存到输出环
             (unsigned long long)(airi_memory_bytes(c->boot, AIRI_MEMORY_BOOT_RECLAIM) / 1024));  // 字节除1024转KiB，再按%llu约定提供unsigned long long
        airi_console_print(c, "kernel / modules      %llu",  // 把本次查询字段按kernel / modules      %llu的格式保存到输出环
             (unsigned long long)(airi_memory_bytes(c->boot, AIRI_MEMORY_KERNEL) / 1024));  // 字节除1024转KiB，再按%llu约定提供unsigned long long
        if (c->read_memory) { // guest 内存服务初始化后提供真实分配状态。
            struct airi_memory_stats stats; // 本次查询只借用同步填好的数值。
            c->read_memory(&stats); // 回调返回前复制，不借出页池或堆块指针。
            airi_console_print(c, "page pool: total=%u free=%u held=%u", stats.pages_total, stats.pages_free, stats.pages_total - stats.pages_free); // 页表和堆backing都计入held。
            airi_console_print(c, "kernel heap: bytes=%zu free_payload=%zu", stats.heap_bytes, stats.heap_free); // 区分页级backing与对象级空闲。
        } else // 较早检查点与宿主纯模型尚未绑定分配器。
            airi_console_print(c, "%u firmware regions. No allocator accounting yet.", c->boot->region_count); // 未接入就明确不提供伪造统计。
    } else if (equal(start, "mem map")) {  // 完整匹配mem map时逐条输出实际物理范围
        airi_console_print(c, "ID  PHYSICAL START   PHYSICAL END     TYPE");  // 将ID  PHYSICAL START   PHYSICAL END     TYPE保存为一条命令说明或身份记录
        for (unsigned i = 0; i < c->boot->region_count; ++i) {  // 只遍历启动地图有效前缀，每项形成一行范围说明
            airi_map_line(c, i, line, sizeof(line));  // 把第i条地图范围写入128字节临时行，下一步复制进命令输出环
            airi_console_print(c, "%s", line);  // 把本次查询字段按%s的格式保存到输出环
        }
    } else if (equal(start, "devices")) {  // 完整匹配devices时显示当前固定设备端点
        airi_console_print(c, "NAME    CLASS     CONFIGURATION");  // 将NAME    CLASS     CONFIGURATION保存为一条命令说明或身份记录
        for (unsigned i = 0; i < 3; ++i) {  // 逐项输出当前约定的3个设备说明
            airi_device_line(c, i, line, sizeof(line));  // 把第i个固定设备描述写入临时行，随后airi_console_print复制进输出环
            airi_console_print(c, "%s", line);  // 把本次查询字段按%s的格式保存到输出环
        }
    } else if (equal(start, "log")) {  // 完整匹配log时逐条显示有序启动记录
        for (unsigned i = 0; i < airi_journal_count(); ++i) {  // 按8条启动记录的有效下标查询并显示
            airi_journal_line(c, i, line, sizeof(line));  // 把第i条启动事件格式化到临时行，再追加到输出环
            airi_console_print(c, "%s", line);  // 把本次查询字段按%s的格式保存到输出环
        }
    } else if (equal(start, "clear"))  // 完整匹配clear时清可见输出环而不重置累计逻辑号
        c->line_count = 0;  // 清除可见历史行数，累计逻辑行号仍保留
    else if (!airi_memcmp(start, "echo ", length < 5 ? length : 5) && length >= 5)  // 最多比较当前长度与5的较小值，再要求至少5字节，确认echo加空格前缀
        airi_console_print(c, "%s", start + 5);  // 略过echo和空格这5字节，将余部作为本条输出
    else if (c->extension && c->extension(c, start, c->extension_context)) {  // 同步调用可选消费者；它自己形成输出，不能保留局部命令指针。
        /* 扩展已处理本条命令，不再生成未知命令错误。 */
    } else  // 没有扩展或扩展不认识本命令时，保留原来的具体错误。
        airi_console_print(c, "%s: unknown command. Type help.", start);  // 把本次查询字段按%s: unknown command. Type help.的格式保存到输出环
    airi_console_print(c, "");  // 在输出环中保存一条空行，为后续显示保留间隔
}
bool airi_console_key(struct airi_console *c, int key) {  // 消费一个逻辑键并报告显示状态是否变化
    if (key >= AIRI_KEY_F1 && key <= AIRI_KEY_F4) {  // 连续功能键范围F1到F4用于选择四种命令视图
        c->view = (enum airi_view)(key - AIRI_KEY_F1);  // F1到F4连续编号减去起点，得到0到3的视图编号
        return true;  // 本逻辑事件已处理，调用者可重画当前状态
    }
    if (key == AIRI_KEY_PAGE_UP || key == AIRI_KEY_PAGE_DOWN) {  // 两个翻页键先按当前视图区分内存分页和终端滚动
        if (c->view == AIRI_VIEW_MEMORY) {  // Memory视图以每12条记录的一页为移动单位
            unsigned pages = (c->boot->region_count + 11) / 12;  // 每页12条，先加11实现整数向上取整
            if (key == AIRI_KEY_PAGE_UP && c->map_page)  // 向上翻页且当前不是第一页时才减页号，避免无符号下溢
                --c->map_page;  // 内存地图回到前一页
            if (key == AIRI_KEY_PAGE_DOWN && c->map_page + 1 < pages)  // 向下翻页且还有下一有效页时才加页号
                ++c->map_page;  // 内存地图前进一页
        } else if (c->view == AIRI_VIEW_TERMINAL) {  // Terminal视图改变文字滚动量，内存页号保持原值
            if (key == AIRI_KEY_PAGE_UP) {  // 终端向上翻时从最新输出回看更早文字
                c->scroll += 10;  // 终端向较早文字滚动10条显示行
                if (c->scroll > c->line_count * 4)  // 滚动量最多保留每条日志拆成4显示行的预留范围
                    c->scroll = c->line_count * 4;  // 把滚动量限制在为拆行预留的最大范围
            } else  // 前一分支条件未成立时转入随后的单条替代操作
                c->scroll = c->scroll > 10 ? c->scroll - 10 : 0;  // 向输出尾部回滚10行，剩余不足10时归零
        }
        return true;  // 本逻辑事件已处理，调用者可重画当前状态
    }
    if (key == AIRI_KEY_UP || key == AIRI_KEY_DOWN) {  // 上下键进入命令历史浏览，不提交新命令
        if (!c->history_count)  // 尚未有已提交历史时没有可浏览项
            return false;  // 当前事件没有可执行操作，调用者无需因此重画
        if (!c->history_back)  // 第一次离开当前输入时先保存草稿，后续历史浏览不反复覆盖它
            airi_memcpy(c->draft, c->input, sizeof(c->draft));  // 首次看旧命令前保存未提交输入，返回时可恢复
        if (key == AIRI_KEY_UP && c->history_back < c->history_count)  // 向上且还未到最旧历史时增加回看距离
            ++c->history_back;  // 向更早的一条提交命令移动
        if (key == AIRI_KEY_DOWN && c->history_back)  // 向下且仍在历史中时减少回看距离，最终0恢复草稿
            --c->history_back;  // 向更新历史或当前草稿移动
        airi_snprintf(c->input, sizeof(c->input), "%s",  // 将选定历史或草稿复制回64字节输入数组，随后重测输入长度
                      c->history_back ? c->history[c->history_count - c->history_back] : c->draft);  // 浏览历史时按从尾部反数下标取旧命令，回到0时恢复未提交草稿
        c->input_length = (unsigned)airi_strlen(c->input);  // 历史文本复制后重新计算当前输入字节数
        c->view = AIRI_VIEW_TERMINAL;  // 切回Terminal显示当前输入或命令结果
        return true;  // 本逻辑事件已处理，调用者可重画当前状态
    }
    if (key == AIRI_KEY_CLEAR) {  // 清屏功能键移除可见历史并回到Terminal，输入文字仍保留
        c->line_count = 0;  // 清除可见历史行数，累计逻辑行号仍保留
        c->scroll = 0;      // 回到输出尾部，便于看到最新命令
        c->view = AIRI_VIEW_TERMINAL;  // 切回Terminal显示当前输入或命令结果
        return true;  // 本逻辑事件已处理，调用者可重画当前状态
    }
    if (key == '\n' || key == '\r') {  // 换行或回车提交控制台正在积累的命令
        execute(c);  // 提交当前输入副本，更新历史并在同一console生成命令结果
        c->input_length = 0;  // 本次提交结束后输入长度归零
        c->input[0] = 0;      // 在输入首字节写NUL，为下一条命令准备空串
        c->history_back = 0;  // 退出历史浏览，回到当前输入
        c->draft[0] = 0;      // 提交后丢弃浏览历史之前的旧草稿
        return true;  // 本逻辑事件已处理，调用者可重画当前状态
    }
    if (key == 8 || key == 127) {  // 两种退格逻辑字节都删除当前输入尾部一个字符
        if (c->input_length)  // 输入非空才允许删除尾字符，避免长度减1下溢
            c->input[--c->input_length] = 0;  // 先退一字节再写NUL，删除最后一个输入字符
        c->view = AIRI_VIEW_TERMINAL;  // 切回Terminal显示当前输入或命令结果
        return true;  // 本逻辑事件已处理，调用者可重画当前状态
    }
    if (key >= 32 && key <= 126 && c->input_length + 1 < sizeof(c->input)) {  // 只加入可打印ASCII；新内容后必须仍留一字节给NUL
        c->input[c->input_length++] = (char)key;  // 在旧末尾写入ASCII字符，然后推进有效长度
        c->input[c->input_length] = 0;  // 给新输入尾部补NUL供字符串函数读取
        c->view = AIRI_VIEW_TERMINAL;   // 切回Terminal显示当前输入或命令结果
        c->scroll = 0;  // 回到输出尾部，便于看到最新命令
        return true;    // 本逻辑事件已处理，调用者可重画当前状态
    }
    return false;  // 当前事件没有可执行操作，调用者无需因此重画
}
