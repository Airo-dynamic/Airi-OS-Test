# K11 · 让串口构建独立于图形支线

第 10 章的一条 mem 已能经 UART 进入会话并返回统计。接下来报告异常、分配页面和操作设备都要复用这条通路，而这些能力不需要字体或窗口。因此默认构建只选择启动、文本与设备实现，缺少图形目录也应得到相同的串口内核。

仍从一次 mem 输入出发：架构层提供轮询间的忙等提示，文本会话积累命令并生成结果，入口将新行发送回 UART。显示函数另外报告本次是否产生画面，扩展回调另外处理内置命令以外的请求。默认串口路径为这些可选消费者保留接口，输入与输出继续使用第 10 章已经建立的对象。

接下来替换入口、控制台接口和 Makefile，增加架构提示、串口显示实现与主机检查；输入驱动、启动适配、基础库和镜像脚本保留。完成接口和各实现后再统一构建。主机检查则在最后直接调用基础库、启动验证、命令模型和扫描码转换器，检查它们各自在普通内存上的行为。

<a id="display-selection"></a>

<a id="arch-relax"></a>

## 空闲时做什么，由架构实现提供

主循环暂无设备事件时仍轮询，pause 只提示 x86 CPU 正在忙等，没有切换任务。把这条指令藏在 airi_arch_relax 后，通用入口只调用一个普通 C 接口，架构实现再决定具体指令。当前仍只有 x86-64，页表、异常与上下文并未因此自动支持其他架构。

**新建 `include/airi/arch.h`：**

<!-- write: include/airi/arch.h -->
```c
#ifndef AIRI_ARCH_H  // 避免重复声明架构操作。
#define AIRI_ARCH_H  // 标记本头文件已展开。
void airi_arch_relax(void);  // 在忙等的一次迭代间提示处理器；不是任务睡眠。
#endif  // 结束重复包含保护。
```

声明给出了空闲分支的调用形式，下面的定义才执行原来的 pause。它返回后仍回到同一次轮询，设备与命令状态不变；不会把“函数返回”解释成“已经等到输入”。

**新建 `kernel/arch/x86_64/cpu.c`：**

<!-- write: kernel/arch/x86_64/cpu.c -->
```c
#include <airi/arch.h>  // 通用循环只通过此声明使用处理器提示。
void airi_arch_relax(void) {  // 本实现只用于当前 x86-64 目标。
    __asm__ volatile("pause");  // 告知 x86 处理器正在忙等，不开启中断或切换任务。
}
```

<a id="serial-display-link"></a>

## 同一个调用名，在构建时选定一份消费者

第 10 章入口直接设 graphics=false。现在入口先调用 airi_console_draw，由选中的实现报告是否完成绘图。默认定义接受 console 后直接返回 false，文字仍由入口调用 airi_serial_write；即便 BootInfo 带有效帧缓冲，也不把设备存在当作已绘图。

选择发生在 Make 与链接阶段：C 只产生同一函数名的引用，链接器把它连到 serial_display.c 的定义；图形路线可选另一份同名实现，但两份不能同时进入一个目标，否则重复定义。这样会话不必先持有图形状态，入口也无需通过运行时加载另一套代码。

`(void)console` 明确本定义不用形参，避免警告，并不读取该地址。airi_dashboard_draw 也保留相同的可选接口，本章入口实际调用前者。

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

初始化会话后，入口得到 false，发 DISPLAY_SERIAL 与 graphics=0，再发布 `[AIRI_P1_READY]`。之后 m、e、m 只改输入，回车仍由原命令分支形成统计，入口将新行写回 UART；只有 graphics 为真才重画。

处理后的记录继续使用 CONSOLE_UPDATE。启动与命令完成各有发布位置，第 12 章等待器将分别使用它们，而不是用“QEMU 仍在运行”判断就绪。

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
```

启动描述和地图日志先沿旧路径完成。现在初始化同一静态 console，再将它交给刚写的显示实现；默认返回 false，所以 DISPLAY_SERIAL 和 READY 记录 graphics=0。

**继续追加会话初始化与就绪发布：**

<!-- append: kernel/main.c -->
```c
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
```

本次会话已就绪，持续输入仍交给原 console_key。新边界只改变两处消费者：空轮询调用 airi_arch_relax，成功图形路径才调用 draw；回车后的新增行继续由入口逐条发给 UART。

**最后追加输入循环，闭合主函数：**

<!-- append: kernel/main.c -->
```c
    for (;;) {  // 没有退出条件，内核持续轮询输入。
        int key = airi_input_poll();  // 从虚拟键盘或 UART 取得一个共享键值，0 表示暂无输入。
        if (!key) {  // 没有新键时不处理命令和重画。
            airi_arch_relax();  // 提示当前架构正在忙等；这不是睡眠或调度。
            continue;  // 本轮没有输入，回到下一次设备轮询。
        }
        uint64_t before = console.total_lines;  // 记录输入前日志尾，回车后只回显本次产生的新行。
        bool changed = airi_console_key(&console, key);  // 按键改变命令或页面状态，返回是否需要刷新。
        if (!changed)  // 被忽略的按键不产生新帧。
            continue;  // 按键未改变可见状态，本轮不重画。
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

## 内置分派之外，才同步交给可选处理者

mem 已由文本模型实现，主题等额外状态则由可选图形方持有。若 console 直接保存主题，就会让默认构建依赖图形。这里只在内置分支之后留一个可选函数地址，默认不安装，theme day 仍报告未知命令。

例如，输入 `theme day` 先经过与 mem 相同的字符积累、回车和名称整理。内置分派没有接受它时，execute 才询问已安装的扩展；默认串口目标没有安装扩展，继续形成原来的未知命令结果。将来图形方可在这个位置安装自己的主题处理函数。

字段 `bool (*extension)(struct airi_console *,const char *,void *)` 保存可调用的函数地址：bool 是返回类型，后面的三项规定调用参数。执行 `c->extension(c,start,c->extension_context)` 时，CPU 调用这个地址所指的函数，分别交出当前会话、已去首尾空格的命令和安装方的数据地址。

数据指针 void* 不带对象类型，也不复制对象。安装者必须持有那份对象，并在回调中恢复成匹配类型；会话只借地址，对象要活到最后一次可能调用。函数地址决定执行谁，数据地址决定交给它哪个对象，二者不能互换。

回调在当前执行流同步完成：true 表示已处理，可通过 airi_console_print 将文字复制进同一输出环；false 继续原未知命令路径。command 只是 execute 的栈副本，回调不能保存其地址供将来读取。初始化清零两个字段，`&&` 先检查函数非空才调用，所以默认路线安全地略过扩展。

**局部更新 `include/airi/console.h`，加入扩展字段、公开输出和显示声明：**

在该文件依次找到以下旧窗口，各用后面的新文本完整替换；其他定义保留。

<!-- replace: include/airi/console.h -->
<!-- before -->
```c
};
/* Fixed storage owned by the kernel. No heap, process or filesystem dependency. */
struct airi_console {  // 独立保存文本输入、历史与输出环
    const AiriBootInfo *boot;  // 借用启动信息，描述与内存地图须持续有效
    enum airi_view view;  // 当前命令视图编号
```
<!-- after -->
```c
};
/* Fixed storage owned by the kernel. No heap, process or filesystem dependency. */
struct airi_console {  // 独立保存文本输入、历史与输出环。
    bool (*extension)(struct airi_console *, const char *, void *);  // 可选命令消费者；true 表示已经处理，NULL 表示没有扩展。
    void *extension_context;   // 借用扩展拥有者的数据，必须活过回调调用。
    const AiriBootInfo *boot;  // 借用启动信息，描述与内存地图须持续有效
    enum airi_view view;  // 当前命令视图编号
```

继续替换下一处窗口：

<!-- replace: include/airi/console.h -->
<!-- before -->
```c
    char draft[AIRI_COMMAND_SIZE];  // 首次浏览历史前保存的未提交输入
};
void airi_console_init(struct airi_console *c, const AiriBootInfo *boot);  // 清命令状态并绑定只读启动结构，形成初始文字
bool airi_console_key(struct airi_console *c, int key);  // 消费一个逻辑键并报告显示状态是否变化
```
<!-- after -->
```c
    char draft[AIRI_COMMAND_SIZE];  // 首次浏览历史前保存的未提交输入
};
void airi_console_print(struct airi_console *c, const char *fmt, ...);  // 把扩展结果复制到有界文本环，不借用格式化实参。
void airi_console_init(struct airi_console *c, const AiriBootInfo *boot);  // 清命令状态并绑定只读启动结构，形成初始文字
bool airi_console_key(struct airi_console *c, int key);  // 消费一个逻辑键并报告显示状态是否变化
```

继续替换下一处窗口：

<!-- replace: include/airi/console.h -->
<!-- before -->
```c
void airi_device_line(const struct airi_console *c, unsigned index, char *out, size_t size);  // 把指定设备描述格式化到调用者输出缓冲
void airi_map_line(const struct airi_console *c, unsigned index, char *out, size_t size);  // 把指定物理内存范围格式化成一行文字
int airi_input_poll(void);  // 当前内核循环从UART或PS/2读取一个逻辑事件
int airi_keyboard_decode(uint8_t code);  // 主机观察和设备路径共用同一个扫描码转换器
#endif  // 结束当前条件编译范围
```
<!-- after -->
```c
void airi_device_line(const struct airi_console *c, unsigned index, char *out, size_t size);  // 把指定设备描述格式化到调用者输出缓冲
void airi_map_line(const struct airi_console *c, unsigned index, char *out, size_t size);  // 把指定物理内存范围格式化成一行文字
bool airi_console_draw(struct airi_console *c);  // 把当前会话和启动描述同步绘到真实帧缓冲，返回此帧是否完成
int airi_input_poll(void);  // 每轮从键盘或UART取一个事件，无事件返回0
int airi_keyboard_decode(uint8_t code);  // 保留修饰键状态并把一个扫描字节变成逻辑键
#endif  // 结束当前条件编译范围
```

扩展产生的文字和内置命令需要相同的环与序号。把原文件内的 emit 改为公开 airi_console_print，保留容量和覆盖规则；已识别的 mem/echo 等仍在原分支结束，只有末尾才尝试扩展。

历史与计数在分派前已经更新，所以一次成功查询或一次未知命令都只记一次提交。公开输出接口没有增加异步借用，也没有把局部命令转成长期对象。

**局部更新 `kernel/console/console.c` 的输出名称与扩展分支；查询助手和键处理算法保持：**

在该文件依次找到以下旧窗口，各用后面的新文本完整替换；其他定义保留。

<!-- replace: kernel/console/console.c -->
<!-- before -->
```c
#include <airi/base.h>     // 引入自有字节操作和有界格式化函数

static void emit(struct airi_console *c, const char *fmt, ...) {  // 向console输出环新增一条有界格式化记录
    char *line = c->lines[c->total_lines % AIRI_CONSOLE_LINES];  // 逻辑行号取余得到可复用槽位，line借用该行的128字节数组
    va_list args;  // 准备遍历fmt之后实际传入的格式参数
```
<!-- after -->
```c
#include <airi/base.h>     // 引入自有字节操作和有界格式化函数

void airi_console_print(struct airi_console *c, const char *fmt, ...) {  // 向console输出环新增一条有界格式化记录
    char *line = c->lines[c->total_lines % AIRI_CONSOLE_LINES];  // 逻辑行号取余得到可复用槽位，line借用该行的128字节数组
    va_list args;  // 准备遍历fmt之后实际传入的格式参数
```

继续替换下一处窗口：

<!-- replace: kernel/console/console.c -->
<!-- before -->
```c
        break;  // 本项文字已形成，结束switch而不继续生成下一项
    default:  // 固定8项记录中的最后一项为本地命令输入状态
        airi_snprintf(s, n, "0007 console  local command interpreter; logical-key input");  // 最后一项说明本地命令解释器接收逻辑键；下一章才接入设备轮询
        break;  // 本项文字已形成，结束switch而不继续生成下一项
    }
```
<!-- after -->
```c
        break;  // 本项文字已形成，结束switch而不继续生成下一项
    default:  // 固定8项记录中的最后一项为本地命令输入状态
        airi_snprintf(s, n, "0007 console  local command interpreter; logical-key input");  // 最后一项说明本地命令解释器接收设备轮询转换后的逻辑键
        break;  // 本项文字已形成，结束switch而不继续生成下一项
    }
```

继续替换下一处窗口：

<!-- replace: kernel/console/console.c -->
<!-- before -->
```c
}
static void system_info(struct airi_console *c) {  // 将启动身份和内核尺寸加入命令输出
    emit(c, "AiriOS 0.1 / x86_64 / bootstrap kernel");  // 将AiriOS 0.1 / x86_64 / bootstrap kernel保存为一条命令说明或身份记录
    emit(c, "Console: local    CPU: single    Boot: UEFI / %s", c->boot->loader);  // 把本次查询字段按Console: local    CPU: single    Boot: UEFI / %s的格式保存到输出环
    emit(c, "Kernel image: %llu KiB    Boot ABI: v%u",  // 把本次查询字段按Kernel image: %llu KiB    Boot ABI: v%u的格式保存到输出环
         (unsigned long long)(c->boot->kernel_size / 1024), c->boot->version);  // 字节除1024转KiB，再按%llu约定提供unsigned long long
}
```
<!-- after -->
```c
}
static void system_info(struct airi_console *c) {  // 将启动身份和内核尺寸加入命令输出
    airi_console_print(c, "AiriOS 0.1 / x86_64 / bootstrap kernel");  // 将AiriOS 0.1 / x86_64 / bootstrap kernel保存为一条命令说明或身份记录
    airi_console_print(c, "Console: local    CPU: single    Boot: UEFI / %s", c->boot->loader);  // 把本次查询字段按Console: local    CPU: single    Boot: UEFI / %s的格式保存到输出环
    airi_console_print(c, "Kernel image: %llu KiB    Boot ABI: v%u",  // 把本次查询字段按Kernel image: %llu KiB    Boot ABI: v%u的格式保存到输出环
         (unsigned long long)(c->boot->kernel_size / 1024), c->boot->version);  // 字节除1024转KiB，再按%llu约定提供unsigned long long
}
```

继续替换下一处窗口：

<!-- replace: kernel/console/console.c -->
<!-- before -->
```c
    c->boot = b;     // 保存调用者提供的启动信息地址；会话继续借用结构和地图，使用期间须有效
    system_info(c);  // 从同一启动结构生成系统身份与内核镜像尺寸
    emit(c, "");     // 在输出环中保存一条空行，为后续显示保留间隔
    emit(c, "System console attached.");      // 将System console attached.保存为一条命令说明或身份记录
    emit(c, "help       Command reference");  // 将help       Command reference保存为一条命令说明或身份记录
    emit(c, "mem        Physical memory inventory");  // 将mem        Physical memory inventory保存为一条命令说明或身份记录
    emit(c, "devices    Console, input and display endpoints");  // 将devices    Console, input and display endpoints保存为一条命令说明或身份记录
    emit(c, "log        Boot journal");  // 将log        Boot journal保存为一条命令说明或身份记录
    emit(c, "");  // 在输出环中保存一条空行，为后续显示保留间隔
}
static void execute(struct airi_console *c) {  // 读取完整提交命令，更新历史并产生结果
```
<!-- after -->
```c
    c->boot = b;     // 保存调用者提供的启动信息地址；会话继续借用结构和地图，使用期间须有效
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
```

继续替换下一处窗口：

<!-- replace: kernel/console/console.c -->
<!-- before -->
```c
    if (!length)  // 首尾去空格后为空命令时不提交
        return;  // 输入只有空白，返回而不增加命令计数或记录历史
    emit(c, "airi> %s", start);  // 将本次真正提交的命令保存为输出环的一行
    ++c->commands;  // 非空命令的累计提交次数增加一次
    if (c->history_count == AIRI_HISTORY_SIZE) {  // 8条历史已满时腾出最后一个槽
```
<!-- after -->
```c
    if (!length)  // 首尾去空格后为空命令时不提交
        return;  // 输入只有空白，返回而不增加命令计数或记录历史
    airi_console_print(c, "airi> %s", start);  // 将本次真正提交的命令保存为输出环的一行
    ++c->commands;  // 非空命令的累计提交次数增加一次
    if (c->history_count == AIRI_HISTORY_SIZE) {  // 8条历史已满时腾出最后一个槽
```

继续替换下一处窗口：

<!-- replace: kernel/console/console.c -->
<!-- before -->
```c
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
bool airi_console_key(struct airi_console *c, int key) {  // 消费一个逻辑键并报告显示状态是否变化
```
<!-- after -->
```c
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
```

<a id="serial-build-rules"></a>

## 让实际源文件选择与调用接口一致

入口新增 airi_arch_relax/draw 引用，SOURCES 必须给出定义。共同启动、文本与设备源码先列出，再按 PROFILE 选择显示实现，而不是先打开图形清单再决定不用。

`$(filter desktop-%,$(PROFILE))` 用逗号前模式匹配后面的文字。normal 得空，desktop-normal 得自身；ifneq 仅在结果非空时读取 frontend/sources.mk，默认则加入 serial_display.c。它决定实际编入哪些定义。

CASE 用 patsubst 去掉 desktop- 前缀，desktop-no-framebuffer 得 no-framebuffer，用来选故障宏。产物目录仍用完整 PROFILE，所以 build/normal 与 build/desktop-normal 不共用对象。另一个 filter 检查 MAKECMDGOALS，只在 ui-preview、ui-test、lab-editor 这些显式动作时读取图形动作规则；本章 all/host-test 均不会进入。

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

```

源码与宏选择已确定本次目标里包含哪些定义。下面的编译、链接规则把这份清单变成独立对象和镜像；host-test 另选纯算法输入，不调用显示函数。

**继续追加构建与主机检查规则，随后补齐文件末尾的可选动作：**

<!-- append: Makefile -->
```make
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


```

普通 all 与 host-test 的规则已经写齐。文件末尾仍保留显式图形动作的条件包含和两个已有主机观察目标；它们不会使默认构建读取 frontend。

**最后追加其余动作与观察规则：**

<!-- append: Makefile -->
```make
# 手动选择图形动作时才读取支线目标；默认构建不检查该目录。
ifneq ($(filter ui-preview ui-test lab-editor,$(MAKECMDGOALS)),)
include frontend/targets.mk # 仅显式图形动作读取相应规则。
endif # 结束当前可选图形分支。

# 两个观察名称均是动作，即使有同名文件也运行。
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

## 主机核对算法，guest 才证明设备路径

同一条 mem 可以先在主机检查文本与历史，便于把数组错误与设备错误分开。host-test 只链接基础库、启动验证、会话和扫描码转换，AIRI_HOST_TEST 排除端口指令；它不调用显示接口，无需显示占位或图形源码。

<a id="host-checks"></a>

assert 条件为假就报告位置并中止进程，Make 收到非零；全部成立才到 PASS。NDEBUG 会关闭断言，当前不定义它。ASan 查越界等访问，UBSan 查可检测的未定义行为，两者只验证这次主机程序，不能替代 UART 实测。

四组检查各自恢复相同起点，再观察不同边界：

- 字节函数与主机 libc 处理相同数组，比较全部字节，范围外的误写也会暴露。
- 格式化遍历容量，比较完整长度和完整数组；容量零只统计，一只写 NUL。
- 每次由 fixture 返回合法启动副本，只损坏一项，避免先前错误遮住当前拒绝条件。
- 会话从 4 MiB 地图提交 mem 得 4096 KiB，再查未知命令、草稿/历史、容量、环覆盖、分页与扫描状态。分页只改模型，未绘制画面。

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
```

字节组每轮先恢复相同输入，整数组比较也能看到范围外误写。格式组沿相同对照方法遍历 cap：同一格式和实参进入两份数组，返回长度与未写区域一起比较。

**继续追加格式检查：**

<!-- append: tests/unit/base_test.c -->
```c
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
```

前两组已经核对字节和格式。接下来为启动检查建立按值返回的 fixture，得到独立副本后每次只损坏一项；这样具体拒绝不被上一项错误遮住。

**继续追加启动夹具与检查：**

<!-- append: tests/unit/base_test.c -->
```c
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
```

合法启动副本也供会话组使用。type 将一条名称逐字送入再回车，contains 只查仍保留的逻辑行；下面的 console_tests 由这两个助手观察同一会话的连续变化。

**继续追加命令与扫描状态检查：**

<!-- append: tests/unit/base_test.c -->
```c
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
```

四组函数现在各有完整输入与断言。主机 main 依次调用它们，只有全部返回才打印一条 PASS；这里没有启动 guest 或读取 UART。

**最后追加主机入口：**

<!-- append: tests/unit/base_test.c -->
```c
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

all 先构建默认串口 ELF 与镜像，host-test 再编译运行上述检查。仍由 01 章的工具选择函数组成真实 Make 调用，统一 airios 命令尚未实现。

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

构建输入应只有共同源码与 serial_display.c，主机检查全部通过才打印 PASS host。串口入口的 graphics=0 不影响 mem 的设备路径。

READY 现已改为 AIRI_P1_READY，第 07、10 章早期观察脚本属于当时版本，不再用它们等待新标记。下一章建立匹配当前入口的启动、输入等待器，把主机算法检查和真实设备证据一起组织起来。
