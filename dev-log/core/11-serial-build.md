# K11 · 让串口构建独立于图形支线

从第 10 章继续，UART 收到 `mem` 后已经能返回真实内存统计。接下来报告异常、分配页面和处理 I/O 仍需要这条通路。日常构建因此先固定为只选择文本与设备实现；即使工程里没有字体和窗口代码，也能生成同一个串口内核。

本章会替换入口、控制台接口和 Makefile，并新建架构提示函数、串口显示实现及主机检查程序。保留第 10 章的输入驱动、启动适配、字节库和镜像脚本。完成全部文件后再构建，中途入口引用的新接口尚未全部声明和定义。

<a id="display-selection"></a>

<a id="arch-relax"></a>

## 让处理器提示留在架构实现中

空闲循环目前反复轮询设备，`pause` 只给 x86 处理器忙等提示，没有让出任务。通用入口改为调用普通 C 函数 `airi_arch_relax`，x86 指令的实现留在 arch/x86_64。这个边界不意味着已经支持其他架构；将来的页表、异常栈和上下文仍需要各架构实际实现。

**新建 `include/airi/arch.h`：**

<!-- write: include/airi/arch.h -->
```c
#ifndef AIRI_ARCH_H  // 避免重复声明架构操作。
#define AIRI_ARCH_H  // 标记本头文件已展开。
void airi_arch_relax(void);  // 在忙等的一次迭代间提示处理器；不是任务睡眠。
#endif  // 结束重复包含保护。
```

收到命令前，主循环的每次空转都会调用这一声明。它的 x86 实现只执行原有 `pause`，随后返回轮询循环；设备状态和命令缓冲都保持原样。

**新建 `kernel/arch/x86_64/cpu.c`：**

<!-- write: kernel/arch/x86_64/cpu.c -->
```c
#include <airi/arch.h>  // 通用循环只通过此声明使用处理器提示。
void airi_arch_relax(void) {  // 本实现只用于当前 x86-64 目标。
    __asm__ volatile("pause");  // 告知 x86 处理器正在忙等，不开启中断或切换任务。
}
```

<a id="serial-display-link"></a>

## 在链接时选择呈现消费者

第 10 章在入口里直接写 `graphics=false`。现在把“有没有完成画面”交给 `airi_console_draw` 返回，入口仍然负责输入和串口输出。默认目标为这个名字提供一个很小的函数：接受 console，直接返回 false。这样入口能按同一接口编译；选择可选图形实现时，再由另一个同名函数读取会话并绘图。

这里由 Make 决定把哪一份定义交给链接器。C 调用只认识函数名和签名，链接器将该引用连接到选中的定义；若两份都加入同一个目标，会产生重复定义。当前的串口实现称为占位实现，它只报告“未绘图”，实际发送文字仍由主循环调用 `airi_serial_write` 完成。BootInfo 即使包含有效显示描述，返回值也仍为 false。

`(void)console` 将形参作为明确不用的值处理，避免未使用参数的编译警告；它没有解引用地址。另一个 `airi_dashboard_draw` 同样保留可选显示接口，本章入口只调用前者。

**新建 `kernel/console/serial_display.c`：**

<!-- write: kernel/console/serial_display.c -->
```c
#include <airi/kernel.h>   // 使用启动描述和当前可选显示接口。
#include <airi/console.h>  // 接受同一文本会话；本目标不持有图形状态。

bool airi_console_draw(struct airi_console *console) {  // 串口目标没有像素消费者。
    (void)console;  // 明确本实现无需访问会话；不解引用可选帧缓冲。
    return false;   // 返回未绘图，使入口继续保留串口输入输出。
}
bool airi_dashboard_draw(const AiriBootInfo *info) {  // 与可选图形实现保持相同链接接口。
    (void)info;    // 串口运行不需要设备像素地址。
    return false;  // 不以设备存在冒充完成了一帧。
}
```

串口显示实现已经给出了 false。入口初始化 console 后调用它，于是走 `DISPLAY_SERIAL` 分支并发布 `graphics=0`。随后收到 m、e、m 时只更新输入；回车时仍由原命令函数生成内存统计，主循环逐行写回 UART。只有 `graphics` 为真时才会调用重画，默认构建不会进入这条路径。

启动完成标记改用 `[AIRI_P1_READY]`，发布位置在显示接口返回之后；输入处理后的记录仍为 `[CONSOLE_UPDATE]`。第 12 章的等待器将使用这两个标记。

**完整替换 `kernel/main.c`：**

<!-- write: kernel/main.c -->
```c
#include <airi/arch.h>     // 通过架构边界使用忙等提示。
#include <airi/kernel.h>   // 取得启动信息、基础库与串口日志接口。
#include <airi/console.h>  // 取得命令环、按键与视图类型。

static struct airi_console console;  // 静态保存终端命令与日志，不在每次重画时重建。

#ifndef AIRI_DEMO_MESSAGE  // 允许修改实验在编译时替换输出文本。
#define AIRI_DEMO_MESSAGE "Hello from AiriOS P1"  // 普通配置默认输出的学习标记文字。
#endif  // 结束默认演示文字的条件定义
_Noreturn void airi_kernel_main(const AiriBootInfo *info) {  // 引导适配交入稳定启动描述；_Noreturn 声明内核不返回加载器。
    const char *error = airi_boot_validate(info);  // 校验必需启动字段，NULL 表示通过，字符串给出失败原因。
    if (error)  // 有具体原因则不能继续读取未经验证的数据。
        airi_panic(error);  // 输出失败原因并停止当前 CPU 执行。
    airi_log("[BOOT_INFO] version=%u regions=%u loader=%s", info->version, info->region_count,  // 打印协议版本和有效内存区域数，%u 与无符号参数匹配。
             info->loader);  // 输出适配层复制且有 NUL 终止的加载器名称。
    airi_log("[KERNEL] physical=0x%llx virtual=0x%llx size=%llu",  // 串口分别记录内核物理位置、虚拟地址和字节长度。
             (unsigned long long)info->kernel_physical_base,  // 转换到 %llx 要求的无符号 long long。
             (unsigned long long)info->kernel_virtual_base, (unsigned long long)info->kernel_size);  // 虚拟地址采用十六进制，内核大小按字节十进制输出。
    airi_log("[HHDM] offset=0x%llx", (unsigned long long)info->direct_map_offset);  // 记录把物理地址换成直接映射虚拟地址的偏移量。
    for (uint32_t i = 0; i < info->region_count; ++i) {  // 只遍历启动描述声明的有效区域，不读取固定数组剩余项。
        const struct airi_memory_region *r = &info->regions[i];  // 借用第 i 条区域描述。
        airi_log("[MEMORY] index=%u base=0x%llx length=0x%llx kind=%s", i,  // 逐项输出下标、物理起点、长度及用途。
                 (unsigned long long)r->base, (unsigned long long)r->length,  // 地址和长度都是64位范围，按 long long 适配格式。
                 airi_memory_name(r->kind));  // 将区域种类转成可读名称，不改变区域数据。
    }
    uint64_t usable = airi_memory_bytes(info, AIRI_MEMORY_USABLE);  // 按实际种类累加可用区域字节数，不等同虚拟机总配置内存。
    airi_log("[MEMORY_SUMMARY] usable_bytes=%llu boot_reclaim_bytes=%llu",  // 输出可用内存与暂由加载器占用的可回收内存两个值。
             (unsigned long long)usable,  // 可用字节数转换匹配 %llu。
             (unsigned long long)airi_memory_bytes(info, AIRI_MEMORY_BOOT_RECLAIM));  // 另行累计加载器可回收区域，不混入 usable。
    airi_log("[LESSON] %s", AIRI_DEMO_MESSAGE);  // 发送当前配置的学习文本，修改实验可据此核对版本。
    airi_console_init(&console, info);  // 初始化内核静态控制台，借用整个启动期间有效的 info。
    bool graphics = airi_console_draw(&console);  // 实际完成首帧后取得布尔结果，不能仅凭描述存在认定成功。
    if (graphics) {  // 有效图形路径才输出图形参数。
        const struct airi_framebuffer *fb = &info->framebuffer;  // 借用刚用于绘图的同一份帧缓冲描述。
        airi_log("[FRAMEBUFFER] width=%llu height=%llu pitch=%llu bpp=%u",  // 记录尺寸、字节行距和每像素位数，供截图与日志比对。
                 (unsigned long long)fb->width, (unsigned long long)fb->height,  // 宽高转为与 %llu 相同的参数类型。
                 (unsigned long long)fb->pitch, (unsigned)fb->bpp);  // pitch 按字节输出，bpp 按位数输出。
    } else  // 未完成图形时仍发送串口原因并继续启动。
        airi_log("[DISPLAY_SERIAL] no frame produced; using serial output");  // 图形不适用时记录原因路径，后面仍进入串口命令循环。
    /* Written AFTER all drawing, so tests can take a completed screenshot. */
    airi_log("[AIRI_P1_READY] graphics=%u usable_bytes=%llu", graphics ? 1u : 0u,  // 首帧调用结束后才发布完成标记，布尔转换为明确的0/1无符号值。
             (unsigned long long)usable);  // 完成记录附同一实际可用字节数。
    airi_serial_write("airi> ");  // 输出首个串口提示符。
    for (;;) {  // 没有退出条件，内核持续轮询输入。
        int key = airi_input_poll();  // 从虚拟键盘或 UART 取得一个共享键值，0 表示暂无输入。
        if (!key) {  // 没有新键时不处理命令和重画。
            airi_arch_relax();  // 提示当前架构正在忙等；这不是睡眠或调度。
            continue;  // 跳过当前不适用项，继续检查下一项。
        }
        uint64_t before = console.total_lines;  // 记录输入前日志尾，回车后只回显本次产生的新行。
        bool changed = airi_console_key(&console, key);  // 按键改变命令或页面状态，返回是否需要刷新。
        if (!changed)  // 被忽略的按键不产生新帧。
            continue;  // 跳过当前不适用项，继续检查下一项。
        if (key == '\n' || key == '\r') {  // 回车完成命令，才把新增输出回显到 UART。
            uint64_t first = console.total_lines - console.line_count;  // 日志环只保留 line_count 行，first 是尚可读取的最早逻辑行。
            if (before < first)  // 若本次命令挤掉旧行，先把读取起点限到仍存在的区域。
                before = first;  // 不访问已经被覆盖的逻辑行。
            for (uint64_t i = before; i < console.total_lines; ++i) {  // 逐行回显本次命令产生且仍保留的内容。
                airi_serial_write(airi_console_line(&console, i));  // 逻辑行号由环接口转换为数组下标。
                airi_serial_write("\n");  // 为该行补串口行结束。
            }
            airi_serial_write("airi> ");  // 输出首个串口提示符。
        }
        if (graphics)  // 只在启动时建立了图形路径的情况下重画。
            airi_console_draw(&console);  // 把变更后的命令与页面状态写成完整画面。
        /* Published after rendering: interaction tests observe completed frames. */
        if (key == '\n' || key == '\r' || key >= AIRI_KEY_PAGE_UP)  // 命令提交与功能键在绘图后发布可观察完成记录。
            airi_log("[CONSOLE_UPDATE] view=%u commands=%u page=%u", (unsigned)console.view,  // 输出当前页面、累计命令和内存页号，观察程序据此等待具体状态。
                     console.commands, console.map_page);  // 这两个计数来自刚处理并重画的同一命令对象。
    }
}
```

<a id="command-extension"></a>

## 将额外命令交给同步消费者

`mem` 的处理仍属于文本会话；主题偏好则由可选图形状态持有。如果把主题字段和处理函数直接写进 console，默认构建也会依赖这些状态。因此为内置命令之外的输入保留一个可选处理入口，主线不安装它。

结构字段 `bool (*extension)(struct airi_console *, const char *, void *)` 保存函数地址。括号中的 `*extension` 表示它是函数指针，最前面的 bool 是被调用函数的返回类型，后面的括号给出三个形参类型。调用 `c->extension(c, start, c->extension_context)` 时，三个实参依次为当前会话、去掉首尾空格的命令和安装者的数据地址。

`void *extension_context` 可以保存不同对象的地址，但不携带对象类型，也不复制对象。以后安装处理函数的一方必须传入自己实际持有的对象，并在函数中恢复为匹配的具体指针类型后才能读取成员。会话只保存地址；该对象必须在所有可能的回调调用期间有效。函数指针与这个数据指针各有用途，不能互换。

回调返回 true 表示已处理，并可通过 `airi_console_print` 把结果复制到环中；false 表示不认识，仍由解释器报告原有错误。`command` 是 execute 栈上的副本，回调只能在调用期间读取它，不能保存地址供以后使用。整个回调是同一内核执行流里的普通函数调用。

初始化会清零整个会话，所以默认 `extension` 和 `extension_context` 都是 NULL。`mem` 仍在内置分支完成；`theme day` 走到末尾时，`&&` 先检查函数地址，为 NULL 就不计算右边的调用，随后生成未知命令错误。

**完整替换 `include/airi/console.h`：**

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
struct airi_console {  // 独立保存文本输入、历史与输出环。
    bool (*extension)(struct airi_console *, const char *, void *);  // 可选命令消费者；true 表示已经处理，NULL 表示没有扩展。
    void *extension_context;   // 借用扩展拥有者的数据，必须活过回调调用。
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
void airi_console_print(struct airi_console *c, const char *fmt, ...);  // 把扩展结果复制到有界文本环，不借用格式化实参。
void airi_console_init(struct airi_console *c, const AiriBootInfo *boot);  // 清命令状态并绑定只读启动结构，形成初始文字
bool airi_console_key(struct airi_console *c, int key);  // 消费一个逻辑键并报告显示状态是否变化
const char *airi_console_line(const struct airi_console *c, uint64_t sequence);  // 按逻辑行号借用仍保留的日志文字
unsigned airi_journal_count(void);  // 返回当前固定启动记录数量
void airi_journal_line(const struct airi_console *c, unsigned index, char *out, size_t size);  // 把指定启动事件格式化到调用者输出缓冲
void airi_device_line(const struct airi_console *c, unsigned index, char *out, size_t size);  // 把指定设备描述格式化到调用者输出缓冲
void airi_map_line(const struct airi_console *c, unsigned index, char *out, size_t size);  // 把指定物理内存范围格式化成一行文字
bool airi_console_draw(struct airi_console *c);  // 把当前会话和启动描述同步绘到真实帧缓冲，返回此帧是否完成
int airi_input_poll(void);  // 每轮从键盘或UART取一个事件，无事件返回0
int airi_keyboard_decode(uint8_t code);  // 保留修饰键状态并把一个扫描字节变成逻辑键
#endif  // 结束当前条件编译范围
```

扩展的输出也需要进入同一文本环。将原来只在 console.c 内可见的 `emit` 改为公开名字 `airi_console_print`，保留原有容量、逻辑行号和覆盖规则；内置命令改用这个名字。`execute` 的末尾才增加扩展检查，已识别的 `mem`、`echo` 等命令不会再交给它。历史记录仍在分派之前更新，所以成功查询与未知命令都各计一次提交。

**完整替换 `kernel/console/console.c`：**

<!-- write: kernel/console/console.c -->
```c
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
        airi_console_print(c, "%u firmware regions. No allocator accounting yet.", c->boot->region_count);  // 把本次查询字段按%u firmware regions. No allocator accounting yet.的格式保存到输出环
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
```

<a id="serial-build-rules"></a>

## 让默认目标完全不读取前端

入口现在引用了 `airi_arch_relax` 和 `airi_console_draw`，Make 必须加入对应实现。`SOURCES` 先列出共同的启动、文本和设备源码，再按 profile 追加一份显示实现。

`$(filter desktop-%,$(PROFILE))` 有两个参数：逗号前是匹配模式，逗号后是要检查的文字。`%` 匹配剩余部分；PROFILE 为 normal 时结果为空，为 desktop-normal 时结果是 desktop-normal。`ifneq (结果,)` 判断它是否与空值不同，只有非空才读取 `frontend/sources.mk`。因此默认目标选择 serial_display.c，且不会尝试打开图形清单。

`$(patsubst desktop-%,%,$(PROFILE))` 则替换前缀，产生 CASE：normal 仍为 normal，desktop-no-framebuffer 得到 no-framebuffer。CASE 决定故障宏，完整 PROFILE 决定产物目录，所以两种实现分别写入 `build/normal` 与 `build/desktop-normal`，不会共用对象。

文件末尾的另一个 filter 检查 `MAKECMDGOALS`，即命令行明确请求的目标名。只有 ui-preview、ui-test 或 lab-editor 才读取图形动作规则。当前执行 all 和 host-test，两个图形条件都不成立。

**完整替换 `Makefile`：**

<!-- write: Makefile -->
```make
# 用既有make_command取得交叉工具参数；统一airios入口在K13加入。
# 内核C编译器由调用参数覆盖；此处默认clang，不借宿主libc。
CC := clang
# ELF链接器默认LLD，调用参数可传入真实找到的路径。
LD := ld.lld
# 主机实验用宿主编译器，可以链接宿主libc。
HOST_CC := cc
# ?=仅在调用者未赋值时取normal，各测试配置独立构建。
PROFILE ?= normal
# $(PROFILE)展开当前配置名称，OUT指向这一配置的产物目录。
OUT := build/$(PROFILE)
# 基础目标只编译文本会话与设备；不读取 frontend 的目录或字体。
SOURCES := kernel/main.c kernel/boot/limine.c kernel/boot/info.c \
    kernel/lib/base.c kernel/lib/runtime.c kernel/arch/x86_64/serial.c \
    kernel/console/console.c kernel/arch/x86_64/input.c kernel/arch/x86_64/cpu.c
# 只有 desktop- 前缀目标才读取图形支线自己的构建片段。
ifneq ($(filter desktop-%,$(PROFILE)),)
include frontend/sources.mk # 本分支取得图形自己的源码清单。
# guest 图形适配与纯 C 前端进入同一内核，参考服务和 Cocoa 不进入。
SOURCES += kernel/console/display.c $(UI_SOURCES)
else # 没有desktop-前缀时只选择串口实现。
# 串口实现没有字体、绘图与桌面状态。
SOURCES += kernel/console/serial_display.c
endif # 结束当前可选图形分支。
# 去掉图形前缀后，两种呈现方式使用同一故障配置含义。
CASE := $(patsubst desktop-%,%,$(PROFILE))
# patsubst把每个.c相对路径改为当前OUT下的.o，汇编入口对象另加入。
OBJECTS := $(patsubst %.c,$(OUT)/%.o,$(SOURCES)) $(OUT)/kernel/arch/x86_64/entry.o
# 交叉目标为无宿主ELF；C11/O2/调试信息/警告作为错误。freestanding与fno-builtin关闭宿主假设；禁栈保护和PIC/PIE；不使用red zone与浮点寄存器；kernel地址模型；MMD/MP生成头文件依赖。
CFLAGS := --target=x86_64-unknown-none-elf -std=c11 -O2 -g -Wall -Wextra -Werror \
          -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-pie \
          -mno-red-zone -mgeneral-regs-only -mcmodel=kernel -MMD -MP
# 预处理器从自有include、固定协议头third_party和前端include取得声明。
CPPFLAGS := -Iinclude -Ithird_party
# 只有显式图形配置需要前端头文件。
ifneq ($(filter desktop-%,$(PROFILE)),)
CPPFLAGS += -Ifrontend/include # 为已选择的图形实现提供头文件搜索位置。
endif # 结束当前可选图形分支。
# Make条件只在no-framebuffer配置加入下方测试宏。
ifeq ($(CASE),no-framebuffer)
# +=追加预处理选项，让启动适配模拟缺少图形响应。
CPPFLAGS += -DAIRI_TEST_NO_FRAMEBUFFER
endif # 结束 no-framebuffer 的编译条件
# 缺内存响应配置采用独立构建目录和下面宏。
ifeq ($(CASE),missing-memory)
# 模拟必需内存地图缺失，内核应打印具体PANIC。
CPPFLAGS += -DAIRI_TEST_NO_MEMMAP
endif # 结束 missing-memory 的编译条件
# 修改实验仅改变标题与输出文本，普通配置不继承这两个宏。
ifeq ($(CASE),edit-experiment)
# 单引号由shell保护内部双引号，编译器取得C字符串宏值；下一续行同时设置学习日志文字。
CPPFLAGS += -DAIRI_PANEL_TITLE='"LOCAL / MAINTENANCE CONSOLE"' \
            -DAIRI_DEMO_MESSAGE='"I changed the output and rebooted"'
endif # 结束 edit-experiment 的文字覆盖条件

# 这些名称是动作目标，即使同名文件存在也执行其规则。
.PHONY: all kernel image host-test clean
# 默认动作依赖镜像，触发内核编译、链接和镜像制作。
all: image
# 只构建独立内核ELF，不制作启动镜像。
kernel: $(OUT)/airios.elf
# 模式规则将相同相对路径的C源文件编成当前配置对象；%匹配路径主体。
$(OUT)/%.o: %.c
# @隐藏命令回显；$@是目标路径，dir取得其目录并确保存在。
	@mkdir -p $(dir $@)
# $<取第一项依赖即当前源文件，$@取输出对象；-c只编译不链接。
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
# 同一对象布局下编译带预处理的汇编入口。
$(OUT)/%.o: %.S
# @隐藏命令回显；$@是目标路径，dir取得其目录并确保存在。
	@mkdir -p $(dir $@)
# $<取第一项依赖即当前源文件，$@取输出对象；-c只编译不链接。
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
# ELF依赖全部对象与链接脚本，任一变化会重新链接。
$(OUT)/airios.elf: $(OBJECTS) kernel/linker.ld
# 这一完整续行命令用ELF x86-64、无标准库、静态链接、4KiB页对齐和不可执行栈；-T指定自有布局，最后-o写目标。
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 \
	    -z noexecstack -T kernel/linker.ld $(OBJECTS) -o $@
# 镜像先要求独立ELF成功。
image: $(OUT)/airios.elf
# 把同一配置的ELF装入UEFI启动镜像。
	python3 scripts/image.py $(PROFILE)
# 主机字节算法、启动合同与命令状态检查，保留assert并启用ASan/UBSan。
host-test:
# 先建立主机检查产物目录。
	@mkdir -p build/tests
# 以下完整续行命令编译主机检查；fno-builtin避免优化绕过被测实现，sanitizer检查内存和未定义行为，AIRI_HOST_TEST避开真实端口指令。
	$(HOST_CC) -std=c11 -g -O1 -Wall -Wextra -Werror -fno-builtin \
	    -fsanitize=address,undefined -DAIRI_HOST_TEST -Iinclude tests/unit/base_test.c \
	    kernel/lib/base.c kernel/boot/info.c \
	    kernel/console/console.c kernel/arch/x86_64/input.c \
		    -o build/tests/base_test
# 只有全部assert成立才打印PASS，失败状态返回Make。
	./build/tests/base_test
# 清理仅删除可重建产物，保留运行证据artifacts。
clean:
# 调用shutil删除build；ignore_errors允许尚不存在目录。
	python3 -c 'import shutil; shutil.rmtree("build", ignore_errors=True)'
# 后缀替换得到自动依赖文件；前缀-允许首次构建尚无.d。
-include $(OBJECTS:.o=.d)


# 手动选择图形动作时才读取支线目标；默认构建不检查该目录。
ifneq ($(filter ui-preview ui-test lab-editor,$(MAKECMDGOALS)),)
include frontend/targets.mk # 仅显式图形动作读取相应规则。
endif # 结束当前可选图形分支。

# 三个观察名称均是动作，即使有同名文件也运行。
.PHONY: lab-memmove lab-boot-info
# 重新构建重叠移动与截断输出观察。
lab-memmove:
# 建立观察程序目录，保持所有可重建产物在build。
	@mkdir -p build/labs
# 下面续行按O0保留便于调试的步骤，链接当前观察所需的同一自有实现。
	$(HOST_CC) -std=c11 -O0 -g -Wall -Wextra -Werror -Iinclude \
	    labs/host/base/memmove_trace.c kernel/lib/base.c -o build/labs/memmove
# 打印左/右重叠及截断结果。
	./build/labs/memmove
# 重新构建合法/损坏启动数据观察。
lab-boot-info:
# 建立观察程序目录，保持所有可重建产物在build。
	@mkdir -p build/labs
# 下面续行按O0保留便于调试的步骤，链接当前观察所需的同一自有实现。
	$(HOST_CC) -std=c11 -O0 -g -Wall -Wextra -Werror -Iinclude \
	    labs/host/boot/boot_info_probe.c kernel/boot/info.c -o build/labs/boot-info
# 打印启动合同的正常与独立失败原因。
	./build/labs/boot-info
```

同一条 `mem` 输入还需要在主机上检查数组容量与历史变化，才能较快定位文本算法错误。host-test 只链接基础库、启动校验、文本会话和扫描码转换；`AIRI_HOST_TEST` 排除真实端口指令。它不会调用显示接口，因此不需要链接占位显示函数或图形代码。

<a id="host-checks"></a>

检查程序使用 `assert(条件)`：条件为假时显示所在位置并中止进程，Make 随之收到非零退出；全部成立后才到达末尾的 PASS。NDEBUG 会关闭这些断言，所以当前编译参数不定义它。ASan 在运行中检测越界等内存访问，UBSan 检查它能捕获的未定义行为；它们都只作用于本次主机程序。

四组检查分别沿已有操作展开：

- 字节移动把同一输入交给自制函数与主机 libc，比较整个数组，连未移动的周边字节也一起核对。
- 格式化遍历输出容量，比较完整所需长度与全部保存字节；零容量仍只统计，容量一只存 NUL。
- 启动验证每次从 `fixture()` 返回的合法结构副本开始，只损坏一个字段，避免前一个错误遮住后一个。
- 命令检查向含 4 MiB 可用区域的会话提交 `mem`，结果应含 4096 KiB；随后继续检查未知命令、草稿与历史、输入容量、输出环和扫描码状态。分页部分只改变模型状态，未绘制画面。

**新建 `tests/unit/base_test.c`：**

<!-- write: tests/unit/base_test.c -->
```c
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
```

运行 all 先取得串口 ELF 与镜像，host-test 再编译并执行刚写出的检查程序。此处保留第 01 章的工具选择入口，尚未使用统一 airios 命令。

**在学习目录根目录运行：**

<!-- run -->
```sh
PYTHONPATH=scripts python3 - <<'PY'  # 从当前工程导入模块，Shell不展开Python正文。
import subprocess  # 用已写出的 Make 参数执行两个明确目标。
from tools import make_command  # 工具选择仍使用第01节的同一实现。
subprocess.run(make_command(target='all'), check=True)  # 生成只含串口实现的独立内核。
subprocess.run(make_command(target='host-test'), check=True)  # 检查基础函数和命令模型。
PY
```

编译命令中只出现共同源码与 serial_display.c，主机检查完成后输出 PASS host。当前入口将报告 `graphics=0`，命令仍沿原 UART 路径处理。新的启动标记已替换旧教学标记；第 07、10 章的 first_boot.py 和 first_input.py 对应当时的版本，本章之后改用 K12 的启动与输入等待器。
