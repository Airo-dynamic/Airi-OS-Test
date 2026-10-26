# 09 · 先让普通字符成为一条可执行命令

启动入口打印地图后就停下，无法再回答一次查询。现在保留一份会话状态，让 m、e、m 和回车先后进入，回车才触发 mem 查询；查询结果还要留下，稍后交给串口发送。先在主机送入同样事件，设备输入下一章再接入。

<a id="console-state"></a>

控制台在这里是保存命令与结果的程序对象，既不是 UART 设备，也不需要先有窗口。第一次调用后 input 为 `m\0`，三次后为 `mem\0`，此时没有查询；第四次回车将它当作完整命令执行。因而正在编辑的输入、已提交的历史与产生的输出不能共用同一块空间。

<a id="console-theme"></a>

本次会话能读 boot 地图执行 mem，也能按 echo 的参数产生文字。theme day 尚无处理者，会形成未知命令错误。它们都在同一个程序内同步分派，没有创建进程或用户程序。观察将连续提交这些不同输入，检查状态如何留下并供下一次调用使用。

## 输入与输出各保存自己的当前状态

console 的 input、历史项与 draft 各为固定数组：input 是尚未提交的内容，history 最多八条已提交副本，draft 是离开当前输入浏览历史前的备份。boot 借用只读启动结构，必须在会话使用期间有效。

lines 另保存 256 行、每行 128 字节。total_lines 累计逻辑行号，line_count 表示仍保留的数量；满后覆盖旧槽，逻辑号继续增加。这样一次 mem 的多行结果可在处理结束后被消费者逐行取出。

普通 ASCII 字符在 0..127，方向键等功能事件从 256 起编号。view 选择终端、内存、设备或日志，scroll 和 map_page 保存浏览位置。串口当前只输出生成的文字，不根据这些字段重新画屏；图形消费者以后可以读取同一模型。

**新建 `include/airi/console.h`：**

<a id="code-include-airi-console-h-1"></a>

<!-- write: include/airi/console.h -->
```c
#ifndef AIRI_CONSOLE_H  // 第一次包含控制台接口时保留以下声明，重复包含时跳过
#define AIRI_CONSOLE_H  // 标记本编译单元已经读取控制台接口
#include <airi/boot.h>  // 引入启动结构、固定宽整数和内存范围接口
#define AIRI_CONSOLE_LINES 256  // 输出环最多保存256条文字记录
#define AIRI_CONSOLE_WIDTH 128  // 一条输出的数组容量为128字节，含终止零
#define AIRI_COMMAND_SIZE 64    // 命令与历史项各用64字节，最多63个内容字节
#define AIRI_HISTORY_SIZE 8     // 最多记住8条已提交的命令
enum airi_view { AIRI_VIEW_TERMINAL, AIRI_VIEW_MEMORY, AIRI_VIEW_DEVICES, AIRI_VIEW_JOURNAL };  // 按顺序编号当前airi_view可接受的状态或事件
enum airi_key {  // 按顺序编号当前airi_key可接受的状态或事件
    AIRI_KEY_UP = 256,  // 向上浏览命令历史或移动选择
    AIRI_KEY_DOWN,      // 向下浏览命令历史或移动选择
    AIRI_KEY_PAGE_UP,   // 向上滚动显示或翻内存页
    AIRI_KEY_PAGE_DOWN,  // 向下滚动显示或翻内存页
    AIRI_KEY_F1,    // 切到Terminal的逻辑事件
    AIRI_KEY_F2,    // 切到Memory的逻辑事件
    AIRI_KEY_F3,    // 切到Devices的逻辑事件
    AIRI_KEY_F4,    // 切到Journal的逻辑事件
    AIRI_KEY_CLEAR  // 清控制台可见历史的逻辑事件
};
/* Fixed storage owned by the kernel. No heap, process or filesystem dependency. */
struct airi_console {  // 独立保存文本输入、历史与输出环
    const AiriBootInfo *boot;  // 借用启动信息，描述与内存地图须持续有效
    enum airi_view view;  // 当前命令视图编号
    char lines[AIRI_CONSOLE_LINES][AIRI_CONSOLE_WIDTH];  // 256行文字环，每行容量128字节
    uint64_t total_lines;  // 累计逻辑输出行号，覆盖旧槽也继续增加
    unsigned line_count;   // 当前环中仍保存的有效行数
    unsigned scroll;    // 显示滚动量，单位由当前视图决定
    unsigned map_page;  // 内存地图每12条一页的页编号
    unsigned commands;  // 已提交非空命令次数
    char input[AIRI_COMMAND_SIZE];  // 尚未提交的NUL命令字符串
    char history[AIRI_HISTORY_SIZE][AIRI_COMMAND_SIZE];  // 最多8条已提交命令的独立文字副本
    unsigned input_length;   // 当前输入内容字节数，不含NUL
    unsigned history_count;  // 有效命令历史条数
    unsigned history_back;   // 离当前草稿有多远，0表示草稿而非历史
    char draft[AIRI_COMMAND_SIZE];  // 首次浏览历史前保存的未提交输入
};
void airi_console_init(struct airi_console *c, const AiriBootInfo *boot);  // 清命令状态并绑定只读启动结构，形成初始文字
bool airi_console_key(struct airi_console *c, int key);  // 消费一个逻辑键并报告显示状态是否变化
const char *airi_console_line(const struct airi_console *c, uint64_t sequence);  // 按逻辑行号借用仍保留的日志文字
unsigned airi_journal_count(void);  // 返回当前固定启动记录数量
void airi_journal_line(const struct airi_console *c, unsigned index, char *out, size_t size);  // 把指定启动事件格式化到调用者输出缓冲
void airi_device_line(const struct airi_console *c, unsigned index, char *out, size_t size);  // 把指定设备描述格式化到调用者输出缓冲
void airi_map_line(const struct airi_console *c, unsigned index, char *out, size_t size);  // 把指定物理内存范围格式化成一行文字
#endif  // 结束当前条件编译范围
```

<a id="ring-lines"></a>

## 文本已产生，还要等消费者取走

mem 会先生成表头、总量与说明，设备发送随后逐行读取。emit 因而把有界格式化结果复制进 lines，不保留临时格式参数或局部数组地址。256 个槽反复使用，累计逻辑号标明这次文字属于哪一行。

例如逻辑行 256 复用槽 0，不能让读取旧逻辑行 0 的请求拿到它。airi_console_line 先拒绝未来或已覆盖的行号，再用序号对 256 取模定位。返回的是会话内部数组的借用指针，后续环绕可能覆盖，消费者应在需要时及时使用。

**新建 `kernel/console/console.c`：**

<a id="code-kernel-console-console-c-1"></a>

<!-- write: kernel/console/console.c -->
```c
#include <airi/console.h>  // 引入控制台状态与逻辑键编号
#include <airi/base.h>     // 引入自有字节操作和有界格式化函数

static void emit(struct airi_console *c, const char *fmt, ...) {  // 向console输出环新增一条有界格式化记录
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
```

mem map、devices、log 需要把同一启动结构的不同字段变成一行文字。map_line 读取 base/length，输出半开区间右端；越过 region_count 就给空串，不能访问未用槽。device_line 输出固定 UART、键盘与本次显示描述，没有可用帧缓冲则明确 unavailable。

journal 当前是八项有序启动说明，尚无实时时钟，不把下标写成实际时间。每个助手都在调用者提供的数组里限容形成文字，再由调用方复制进输出环；生成记录与设备发送仍分开。

**追加到 `kernel/console/console.c` 末尾：**

<a id="code-kernel-console-console-c-2"></a>

<!-- append: kernel/console/console.c -->
```c
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
        airi_snprintf(s, n, "0007 console  local command interpreter; logical-key input");  // 最后一项说明本地命令解释器接收逻辑键；下一章才接入设备轮询
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
```

这些查询已经有了文本来源，初始化先清零整个会话，绑定 boot，再写入系统身份和命令提示。空字符串也占一条逻辑行，会增加序号；观察入口逐行 puts 时它才成为终端里的空行。初始化生成的是待显示文字，此处仍没有访问 UART。

**追加到 `kernel/console/console.c` 末尾：**

<a id="code-kernel-console-console-c-3"></a>

<!-- append: kernel/console/console.c -->
```c
static void system_info(struct airi_console *c) {  // 将启动身份和内核尺寸加入命令输出
    emit(c, "AiriOS 0.1 / x86_64 / bootstrap kernel");  // 将AiriOS 0.1 / x86_64 / bootstrap kernel保存为一条命令说明或身份记录
    emit(c, "Console: local    CPU: single    Boot: UEFI / %s", c->boot->loader);  // 把本次查询字段按Console: local    CPU: single    Boot: UEFI / %s的格式保存到输出环
    emit(c, "Kernel image: %llu KiB    Boot ABI: v%u",  // 把本次查询字段按Kernel image: %llu KiB    Boot ABI: v%u的格式保存到输出环
         (unsigned long long)(c->boot->kernel_size / 1024), c->boot->version);  // 字节除1024转KiB，再按%llu约定提供unsigned long long
}
void airi_console_init(struct airi_console *c, const AiriBootInfo *b) {  // 清命令状态并绑定只读启动结构，形成初始文字
    airi_memset(c, 0, sizeof(*c));  // 清零整个当前状态或快照，使计数和标记从确定初值开始
    c->boot = b;     // 将借用启动信息，描述与内存地图须持续有效置为b，后续读取使用新状态
    system_info(c);  // 从同一启动结构生成系统身份与内核镜像尺寸
    emit(c, "");     // 在输出环中保存一条空行，为后续显示保留间隔
    emit(c, "System console attached.");      // 将System console attached.保存为一条命令说明或身份记录
    emit(c, "help       Command reference");  // 将help       Command reference保存为一条命令说明或身份记录
    emit(c, "mem        Physical memory inventory");  // 将mem        Physical memory inventory保存为一条命令说明或身份记录
    emit(c, "devices    Console, input and display endpoints");  // 将devices    Console, input and display endpoints保存为一条命令说明或身份记录
    emit(c, "log        Boot journal");  // 将log        Boot journal保存为一条命令说明或身份记录
    emit(c, "");  // 在输出环中保存一条空行，为后续显示保留间隔
}
```

## 回车让当前输入成为一次提交

execute 先把 input 复制为局部 command，本次分派便不依赖稍后要清空的输入数组。对 `  mem  `，start 跳过两个前导空格，尾空格改写为 NUL，再按 mem 完整匹配；全为空格则不增加 commands。非空提交先写输出中的提示行、计数与历史，再执行具体分支，所以未知命令也算一次提交。

历史满八条时，用 memmove 把后七项完整前移，为本次副本留位置。mem 将观察输入的 `0x400000` 字节除以 1024，产生 4096 KiB，再追加说明和空行。mem map 另逐项输出范围，echo 则匹配五字节 `echo ` 前缀并输出后缀，不能把它与 mem 的完整匹配混同。

查询助手的局部 line 在每次 emit 时被复制，execute 返回后不再借用。clear 清可见数量而不回退累计序号，后续新行仍有新的身份。整个分派保持在同一个会话调用中。

**追加到 `kernel/console/console.c` 末尾：**

<a id="code-kernel-console-console-c-4"></a>

<!-- append: kernel/console/console.c -->
```c
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
    emit(c, "airi> %s", start);  // 将本次真正提交的命令保存为输出环的一行
    ++c->commands;  // 非空命令的累计提交次数增加一次
    if (c->history_count == AIRI_HISTORY_SIZE) {  // 8条历史已满时腾出最后一个槽
        airi_memmove(c->history, c->history + 1, (AIRI_HISTORY_SIZE - 1) * AIRI_COMMAND_SIZE);  // 后7条完整历史前移一槽，重叠区域用memmove保住待读字节
        --c->history_count;  // 覆盖最旧历史后暂记7条，为本条提交留位置
    }
    airi_snprintf(c->history[c->history_count++], AIRI_COMMAND_SIZE, "%s", start);  // 先在旧计数下标保存命令，再把历史计数加一
    c->view = AIRI_VIEW_TERMINAL;  // 切回Terminal显示当前输入或命令结果
    c->scroll = 0;  // 回到输出尾部，便于看到最新命令
    char line[AIRI_CONSOLE_WIDTH];  // 命令查询先在128字节局部数组形成一行，再由emit复制进输出环
    if (equal(start, "help")) {     // 提交文字完整等于help时生成命令列表
        emit(c, "COMMAND    OPERATION");  // 将COMMAND    OPERATION保存为一条命令说明或身份记录
        emit(c, "sys        Kernel and boot identity");  // 将sys        Kernel and boot identity保存为一条命令说明或身份记录
        emit(c, "mem        Usable / held physical memory summary");  // 将mem        Usable / held physical memory summary保存为一条命令说明或身份记录
        emit(c, "mem map    Complete physical address inventory");  // 将mem map    Complete physical address inventory保存为一条命令说明或身份记录
        emit(c, "devices    Registered console hardware endpoints");  // 将devices    Registered console hardware endpoints保存为一条命令说明或身份记录
        emit(c, "log        Boot journal (ordered events, no wall clock)");  // 将log        Boot journal (ordered events, no wall clock)保存为一条命令说明或身份记录
        emit(c, "echo TEXT  Write text to this console");  // 将echo TEXT  Write text to this console保存为一条命令说明或身份记录
        emit(c, "clear      Clear terminal scrollback");  // 将clear      Clear terminal scrollback保存为一条命令说明或身份记录
        emit(c, "F1-F4      Terminal / Memory / Devices / Journal");  // 将F1-F4      Terminal / Memory / Devices / Journal保存为一条命令说明或身份记录
        emit(c, "PgUp/PgDn  Scroll terminal or memory inventory");  // 将PgUp/PgDn  Scroll terminal or memory inventory保存为一条命令说明或身份记录
        emit(c, "Up/Down    Command history    Ctrl-L: clear");  // 将Up/Down    Command history    Ctrl-L: clear保存为一条命令说明或身份记录
    } else if (equal(start, "sys"))  // 完整匹配sys时查询同一启动身份
        system_info(c);  // 从同一启动结构生成系统身份与内核镜像尺寸
    else if (equal(start, "mem")) {  // 完整匹配mem时按启动地图类别统计字节数
        emit(c, "TYPE                  KiB");  // 将TYPE                  KiB保存为一条命令说明或身份记录
        emit(c, "usable                %llu",  // 把本次查询字段按usable                %llu的格式保存到输出环
             (unsigned long long)(airi_memory_bytes(c->boot, AIRI_MEMORY_USABLE) / 1024));  // 字节除1024转KiB，再按%llu约定提供unsigned long long
        emit(c, "boot reclaim (held)   %llu",  // 把本次查询字段按boot reclaim (held)   %llu的格式保存到输出环
             (unsigned long long)(airi_memory_bytes(c->boot, AIRI_MEMORY_BOOT_RECLAIM) / 1024));  // 字节除1024转KiB，再按%llu约定提供unsigned long long
        emit(c, "kernel / modules      %llu",  // 把本次查询字段按kernel / modules      %llu的格式保存到输出环
             (unsigned long long)(airi_memory_bytes(c->boot, AIRI_MEMORY_KERNEL) / 1024));  // 字节除1024转KiB，再按%llu约定提供unsigned long long
        emit(c, "%u firmware regions. No allocator accounting yet.", c->boot->region_count);  // 把本次查询字段按%u firmware regions. No allocator accounting yet.的格式保存到输出环
    } else if (equal(start, "mem map")) {  // 完整匹配mem map时逐条输出实际物理范围
        emit(c, "ID  PHYSICAL START   PHYSICAL END     TYPE");  // 将ID  PHYSICAL START   PHYSICAL END     TYPE保存为一条命令说明或身份记录
        for (unsigned i = 0; i < c->boot->region_count; ++i) {  // 只遍历启动地图有效前缀，每项形成一行范围说明
            airi_map_line(c, i, line, sizeof(line));  // 把第i条地图范围写入128字节临时行，下一步复制进命令输出环
            emit(c, "%s", line);  // 把本次查询字段按%s的格式保存到输出环
        }
    } else if (equal(start, "devices")) {  // 完整匹配devices时显示当前固定设备端点
        emit(c, "NAME    CLASS     CONFIGURATION");  // 将NAME    CLASS     CONFIGURATION保存为一条命令说明或身份记录
        for (unsigned i = 0; i < 3; ++i) {  // 逐项输出当前约定的3个设备说明
            airi_device_line(c, i, line, sizeof(line));  // 把第i个固定设备描述写入临时行，随后emit保存
            emit(c, "%s", line);  // 把本次查询字段按%s的格式保存到输出环
        }
    } else if (equal(start, "log")) {  // 完整匹配log时逐条显示有序启动记录
        for (unsigned i = 0; i < airi_journal_count(); ++i) {  // 按8条启动记录的有效下标查询并显示
            airi_journal_line(c, i, line, sizeof(line));  // 把第i条启动事件格式化到临时行，再追加到输出环
            emit(c, "%s", line);  // 把本次查询字段按%s的格式保存到输出环
        }
    } else if (equal(start, "clear"))  // 完整匹配clear时清可见输出环而不重置累计逻辑号
        c->line_count = 0;  // 清除可见历史行数，累计逻辑行号仍保留
    else if (!airi_memcmp(start, "echo ", length < 5 ? length : 5) && length >= 5)  // 最多比较当前长度与5的较小值，再要求至少5字节，确认echo加空格前缀
        emit(c, "%s", start + 5);  // 略过echo和空格这5字节，将余部作为本条输出
    else  // 前一分支未成立时执行下方替代操作
        emit(c, "%s: unknown command. Type help.", start);  // 把本次查询字段按%s: unknown command. Type help.的格式保存到输出环
    emit(c, "");  // 在输出环中保存一条空行，为后续显示保留间隔
}
```

<a id="logical-keys"></a>

## 一次键调用可以只改状态，而不执行命令

设备将来报告扫描码，主机也可直接调用逻辑键接口，模型只消费统一事件：普通字符是 ASCII 值，F1、方向键等用 256 起的编号。每次调用只处理一个事件，跨次保存的 input 才把三个字符连为 mem。

F1..F4 改 view；内存按十二项一页计算，PgDn 不能越过最后一页，终端翻页只改 scroll。第一次向上看历史时先保存 draft，之后只改变回看距离；向下回到 history_back=0，恢复的仍是进入历史前那份未提交文字。

回车调用 execute 后才清 input、退出历史并清旧草稿；退格先检查长度，普通字符则要求新增后仍留一格 NUL。不支持或放不下的事件返回 false，true 只表示状态可能变化，调用方再决定发送新行或更新可选画面，不能把它当成“命令已执行”。

**追加到 `kernel/console/console.c` 末尾：**

<a id="code-kernel-console-console-c-5"></a>

<!-- append: kernel/console/console.c -->
```c
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
```

## 让同一会话接住六条连续输入

主机程序持有静态 console，局部 boot 在 main 的整个观察期间也有效。boot 只填命令查询要用的字段，没有调用完整 boot_validate；这份小夹具不用于实际引导。

submit 逐字节调用 console_key，字符串中的换行是提交事件，NUL 只结束送入循环。mem 先得到 4096 KiB，echo serial-mainline 形成参数回显，theme day 留下未知命令错误，再查询 devices、help、sys；六次都更新同一个历史和输出环，最后才按逻辑行号显示。

input 每次提交后清空，历史仍保留已提交版本；draft 只保存浏览前尚未提交的文字，不能用历史替代。比如正输入 x 时上翻到 devices，再下翻应恢复 x，而不是丢掉草稿。下面的连续观察建立同一对象跨调用的寿命，后面的主机检查再核对这些浏览边界。

**新建 `labs/tutorial/console.c`：**

<a id="code-labs-tutorial-console-c-1"></a>

<!-- write: labs/tutorial/console.c -->
```c
#include <airi/console.h>  // 引入命令状态和逻辑键接口
#include <stdio.h>  // 主机用puts和printf观察同一控制台的输出
static struct airi_console console;     // 静态存储拥有输入、历史与输出
static void submit(const char *text) {  // 借用一条带换行的NUL命令，只在调用期间读取
    while (*text)  // 遇终止零结束，字符串中的换行仍作为提交事件送入
        airi_console_key(&console, *text++);  // 先取当前字节，调用命令入口，再把指针推进一字节
}
int main(void) {  // 用真实命令调用观察查询、回显与未知命令
    AiriBootInfo boot = {.version = 1, .region_count = 1};  // 当前命令观察只需要一条可用区域，不调用完整引导验证
    boot.regions[0] = (struct airi_memory_region){0x100000, 0x400000, AIRI_MEMORY_USABLE, 0};  // 物理1MiB起提供4MiB可用范围
    airi_console_init(&console, &boot);  // 初始化输出与历史，并借用当前启动结构
    submit("mem\n");  // 当前构造输入提供4096KiB可用内存。
    submit("echo serial-mainline\n");  // 文本参数经同一命令路径回显。
    submit("theme day\n");  // 未安装图形消费者时报告未知命令。
    submit("devices\n");    // 查询固定设备端点。
    submit("help\n");  // 列出当前已实现命令。
    submit("sys\n");   // 查询相同启动结构的身份。
    for (uint64_t i = 0; i < console.total_lines; ++i)  // 沿有效逻辑行号读取本次全部实际输出
        puts(airi_console_line(&console, i));  // 输出环中的每条NUL文字显示在主机终端
    printf("commands=%u\n", console.commands);  // 报告同一会话的累计提交数
    return console.commands != 6;  // 六次提交全部处理后退出0
}
```

将观察入口、命令模型、基础库和启动统计实现链接为主机程序。console 不依赖真实设备，结果到这里才由主机 puts/printf 显示；换成设备输入时，模型本身不必重写。

**在学习目录根目录运行：**

<!-- run -->
```sh
mkdir -p build  # 创建本学习目录的产物目录，已有目录可复用
cc -std=c11 -O0 -g -Wall -Wextra -Werror -Iinclude labs/tutorial/console.c kernel/console/console.c kernel/lib/base.c kernel/boot/info.c  -o build/probe  # 按当前检查点源码链接主机观察程序，-I提供接口目录，-o指定输出文件
./build/probe  # 运行刚构建的本检查点观察程序，退出码和输出共同核对结果
```

应看到 `airi> mem`、usable 4096、`No allocator accounting yet.`，随后还有 echo 的文字、theme day 的错误以及 `commands=6`。计数说明六次非空提交经过同一解释器，4096 仍来自传入的启动分类，不是实时分配器统计。

现在字符到命令、命令到保存文本已连通。下一章由 UART 或键盘提供事件，再把本次新增文字交回 UART，补齐实际设备交互。
