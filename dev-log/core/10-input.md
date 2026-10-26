# 10 · 把串口和键盘字节接入内核命令模型

命令模型已经能积累 mem 并生成结果，现在替换的是输入来源和输出消费者。内核不再打印后停机，而是持续轮询设备，将非零逻辑键交给 console_key，再把本次新增的文本写回 UART。

<a id="scan-code"></a>

两条输入路径携带的内容不同。PS/2 键盘报告位置与按下/释放，按 a 先得到扫描码，不是直接得到 ASCII 97；固定 PC 的 i8042 通过 0x60/0x64 数据与状态端口交出它。UART 则已经收到主机写来的 `mem\n` 字节，不必再按键盘布局翻译。最终两者都归为模型认识的字符或功能键。

## 设备适配只交事件，命令状态继续由会话持有

先在原控制台头文件加入 airi_input_poll 与扫描码转换器的声明，输入、历史和日志结构保持。poll 供 guest 主循环调用，转换器也可在主机用构造扫描码检查；声明只连接接口，下面的定义才实施设备处理。

完整替换 `include/airi/console.h`：

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
int airi_input_poll(void);  // 当前内核循环从UART或PS/2读取一个逻辑事件
int airi_keyboard_decode(uint8_t code);  // 主机观察和设备路径共用同一个扫描码转换器
#endif  // 结束当前条件编译范围
```

## 从一次设备字节恢复按键所需的前文

固定 q35 的固件使 i8042 交出 set-1 扫描码。a 按下为 0x1e、释放为 0x9e，最高位表示释放；E0 则是下一字节的前缀。若每次都从空状态解释，就无法知道下一字节是不是扩展键，也无法知道 Shift 是否仍按住，所以修饰状态与 extended 跨调用保存。

左右 Shift/Ctrl 各用一位，释放左侧只清左位，右侧仍可保持。`~mask` 经整数提升，转回 uint8_t 后才清对应位。转换器本身只处理数值，可在主机运行；真实 inb 稍后放进 AIRI_HOST_TEST 排除的部分。

**新建 `kernel/arch/x86_64/input.c`：**

<a id="code-kernel-arch-x86-64-input-c-1"></a>

<!-- write: kernel/arch/x86_64/input.c -->
```c
#include <airi/console.h>  // 引入控制台状态与逻辑键编号
/* QEMU q35 i8042: firmware leaves translation enabled, so port 0x60 returns
 * set-1 scancodes. No interrupts are enabled by this bootstrap driver. */
static uint8_t shift, control;  // 左右修饰键分别用位记录，静态存储跨解码调用保留
static bool extended;  // 保存E0前缀，供下一扫描字节使用
int airi_keyboard_decode(uint8_t code) {  // 保留修饰键状态并把一个扫描字节变成逻辑键
    if (code == 0xe0) {  // E0是扩展键前缀，本字节先记录而不输出按键
        extended = true;  // 让下一扫描字节按扩展键解释
        return 0;  // 本次字节只改变按键状态或无有效输入，不产出逻辑键
    }
    bool ext = extended;  // 取出本字节之前留下的扩展前缀状态
    extended = false;     // 前缀只用于本字节，后续普通键不继续继承
    bool release = (code & 0x80) != 0;  // 最高位置位表示释放，按位与只取这一位
    code &= 0x7f;  // 清最高位后得到相同按键的按下编号
    if (!ext && (code == 0x2a || code == 0x36)) {  // 无E0前缀的0x2a和0x36分别是左右Shift
        uint8_t mask = code == 0x2a ? 1 : 2;  // 左Shift用位0，右Shift用位1，三目式按扫描码选择
        if (release)  // 释放当前这一侧Shift时只清对应状态位，保留另一侧。
            shift &= (uint8_t)~mask;  // 把该侧Shift位清零，转换回8位避免整数提升的高位
        else  // 前一分支未成立时执行下方替代操作
            shift |= mask;  // 置位该侧Shift，保留另一侧已经按下的状态
        return 0;  // 本次字节只改变按键状态或无有效输入，不产出逻辑键
    }
    if (code == 0x1d) {  // 0x1d表示Ctrl，再由E0状态区分左右
        uint8_t mask = ext ? 2 : 1;  // 扩展Ctrl代表右侧，普通Ctrl代表左侧
        if (release)  // 释放当前这一侧Ctrl时只清对应状态位，保留另一侧。
            control &= (uint8_t)~mask;  // 只清释放的这一侧Ctrl位
        else  // 前一分支未成立时执行下方替代操作
            control |= mask;  // 记住按下的这一侧Ctrl，保留其他状态位
        return 0;  // 本次字节只改变按键状态或无有效输入，不产出逻辑键
    }
    if (release)  // 普通键释放不生成字符；Shift与Ctrl的状态已在前面分支更新。
        return 0;  // 本次字节只改变按键状态或无有效输入，不产出逻辑键
    if (ext) {  // 有E0前缀时按扩展键编号分派
        switch (code) {  // 按已清释放位的扩展扫描码选择逻辑功能键
        case 0x48:  // 扫描码0x48进入其扩展逻辑键分支
            return AIRI_KEY_UP;  // 向上浏览命令历史或移动选择交给上一层
        case 0x50:  // 扫描码0x50进入其扩展逻辑键分支
            return AIRI_KEY_DOWN;  // 向下浏览命令历史或移动选择交给上一层
        case 0x49:  // 扫描码0x49进入其扩展逻辑键分支
            return AIRI_KEY_PAGE_UP;  // 向上滚动显示或翻内存页交给上一层
        case 0x51:  // 扫描码0x51进入其扩展逻辑键分支
            return AIRI_KEY_PAGE_DOWN;  // 向下滚动显示或翻内存页交给上一层
        case 0x1c:  // 扫描码0x1c进入其扩展逻辑键分支
            return '\n';  // 小键盘Enter转换成同样的命令提交换行
        default:  // 其他编号进入当前switch的默认结果
            return 0;  // 本次字节只改变按键状态或无有效输入，不产出逻辑键
        }
    }
    if (code >= 0x3b && code <= 0x3e)  // 扫描码0x3b至0x3e是F1至F4
        return AIRI_KEY_F1 + (code - 0x3b);  // 按相对扫描码偏移转换成连续逻辑功能键编号
```

收到 E0 时只置 extended 并返回 0；下一次把它取到局部 ext，再清静态前缀，影响只持续一个后续字节。Shift 按下也只改状态；随后 a 的按下码借该状态产生 A，a 释放不产字符，修饰键释放则更新各自位。

去掉前缀和释放含义以后，plain 才按扫描位置取普通字符。`[0x1e]='a'` 是指定下标初始化，未列槽为零；Shift 字母按 ASCII 差转换，数字/标点按 from、to 的相同下标配对，Ctrl-L 产生清屏事件。扩展键与 F1..F4 则直接返回功能编号。

映射限定 US ASCII。一次调用返回 0 可能只是记住了前缀或修饰变化，并不等于设备没有送过字节；模型只在实际取得字符/功能事件后接手。

**追加到 `kernel/arch/x86_64/input.c` 末尾：**

<a id="code-kernel-arch-x86-64-input-c-2"></a>

<!-- append: kernel/arch/x86_64/input.c -->
```c
    static const char plain[128] = {  // 按扫描码下标初始化US普通字符表，未指定槽自动为0
        [0x02] = '1', [0x03] = '2',  [0x04] = '3',  [0x05] = '4',  [0x06] = '5', [0x07] = '6',  // 按方括号扫描码下标指定US字符映射，未列出的数组槽保持0
        [0x08] = '7', [0x09] = '8',  [0x0a] = '9',  [0x0b] = '0',  [0x0c] = '-', [0x0d] = '=',  // 按方括号扫描码下标指定US字符映射，未列出的数组槽保持0
        [0x0e] = 8,   [0x0f] = ' ',  [0x10] = 'q',  [0x11] = 'w',  [0x12] = 'e', [0x13] = 'r',  // 按方括号扫描码下标指定US字符映射，未列出的数组槽保持0
        [0x14] = 't', [0x15] = 'y',  [0x16] = 'u',  [0x17] = 'i',  [0x18] = 'o', [0x19] = 'p',  // 按方括号扫描码下标指定US字符映射，未列出的数组槽保持0
        [0x1a] = '[', [0x1b] = ']',  [0x1c] = '\n', [0x1e] = 'a',  [0x1f] = 's', [0x20] = 'd',  // 按方括号扫描码下标指定US字符映射，未列出的数组槽保持0
        [0x21] = 'f', [0x22] = 'g',  [0x23] = 'h',  [0x24] = 'j',  [0x25] = 'k', [0x26] = 'l',  // 按方括号扫描码下标指定US字符映射，未列出的数组槽保持0
        [0x27] = ';', [0x28] = '\'', [0x29] = '`',  [0x2b] = '\\', [0x2c] = 'z', [0x2d] = 'x',  // 按方括号扫描码下标指定US字符映射，未列出的数组槽保持0
        [0x2e] = 'c', [0x2f] = 'v',  [0x30] = 'b',  [0x31] = 'n',  [0x32] = 'm', [0x33] = ',',  // 按方括号扫描码下标指定US字符映射，未列出的数组槽保持0
        [0x34] = '.', [0x35] = '/',  [0x39] = ' '};  // 按方括号扫描码下标指定US字符映射，未列出的数组槽保持0
    char c = plain[code];  // 按已清释放位的扫描码查普通字符表，未填槽默认为0
    if (control)  // 任一侧Ctrl按下时只保留当前支持的Ctrl-L组合
        return c == 'l' ? AIRI_KEY_CLEAR : 0;  // Ctrl-L产生清屏逻辑键，其他Ctrl组合本轮不输出
    if (shift) {  // 任一侧Shift按下时把普通字符转换成对应大写或标点
        if (c >= 'a' && c <= 'z')  // 小写ASCII字母可直接用编码差转换
            c -= 'a' - 'A';  // 减去32，把小写字母编码改成对应大写
        else {  // 当前不走上一特殊情况时采用下方替代处理
            const char *from = "1234567890-=[];'`,./\\", *to = "!@#$%^&*()_+{}:\"~<>?|";  // from与to同一下标配对普通标点和Shift标点，字符串末尾NUL结束查找
            for (unsigned i = 0; from[i]; ++i)  // 按Shift标点来源字符串的有效下标寻找匹配字符
                if (c == from[i]) {  // 当前普通字符匹配Shift来源串第i项时取目标串同一下标
                    c = to[i];  // 使用相同下标取Shift后应输出的数字或标点
                    break;      // 当前字符已转换成配对的 Shift 字符，结束来源字符串的查找循环
                }
        }
    }
    return c;  // 交出当前转换完成的ASCII字符编码
}
```

<a id="input-poll"></a>

## 状态允许读取时，才消费设备数据

poll 先读 i8042 的 0x64，位 0 表示输出缓冲有字节；读 0x60 消费后，再排除鼠标来源、超时或校验错误，只有有效键盘字节送入转换器。错误记录也已被消费，不能留在端口使下一轮重复遇到它。

键盘无数据时再看 UART 的 0x3fd 接收位，置位才读 0x3f8。该端口写入是发送，读取则取得接收字节；ASCII 12 转清屏，其余直接交模型，包括回车。没有事件返回 0，主循环继续；当前优先检查键盘，仍没有中断接收或设备等待队列。

**追加到 `kernel/arch/x86_64/input.c` 末尾：**

<a id="code-kernel-arch-x86-64-input-c-3"></a>

<!-- append: kernel/arch/x86_64/input.c -->
```c
#ifndef AIRI_HOST_TEST  // 主机测试排除后面的真实I/O端口访问，保留扫描码解码
static uint8_t in8(uint16_t port) {  // 以CPU端口指令读取设备的一字节寄存器
    uint8_t value;  // 为CPU的AL读端口结果准备一个8位接收对象
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));  // 从DX或可编码的立即端口读一字节到AL，=a把AL结果交给value
    return value;  // 把CPU端口读取的一字节交给设备轮询
}
int airi_input_poll(void) {  // 每轮从键盘或UART取一个事件，无事件返回0
    /* Process one pending byte, ignore mouse packets / parity / timeout errors. */
    uint8_t status = in8(0x64);  // 读取i8042状态寄存器，尚未取键盘数据字节
    if (status & 1) {  // 位0置位表示输出缓冲已有一个可读取字节
        uint8_t code = in8(0x60);  // 消费控制器当前数据字节，后面才判断其来源和有效性
        if (!(status & 0xe0))      // 排除鼠标来源、超时和奇偶校验错误的状态位
            return airi_keyboard_decode(code);  // 把有效扫描码转换成console理解的字符或功能键
        return 0;  // 本次字节只改变按键状态或无有效输入，不产出逻辑键
    }
    if (in8(0x3fd) & 1) {  // 键盘无数据时检查UART线路状态的接收就绪位
        int c = in8(0x3f8);  // 读取UART接收缓冲中的一个原始字节
        return c == 12 ? AIRI_KEY_CLEAR : c;  // ASCII控制字节12转换为清屏，其余接收字节直接交出
    }
    return 0;  // 本次字节只改变按键状态或无有效输入，不产出逻辑键
}
#endif  // 结束当前条件编译范围
```

## 模型已改变，还要把新行送给 UART

收到 m 只改变 input，不生成查询结果；回车执行 mem，才追加多行。主循环处理键之前保存 total_lines，处理之后从这个旧尾部读到新尾部，逐行发送，所以不会每按一次键就重发启动日志。若环已覆盖旧起点，先把起点提高到仍保留范围。

发送后写出 CONSOLE_UPDATE，包含新的命令计数与视图。主机因而可以用 commands=1 加查询结果证明一次回车已处理，不能只凭字节已写进 QEMU 就判完成。

无输入时 pause/continue 仍是忙等轮询，不是调度器睡眠；graphics 当前为 false，输出独立走串口。初始化 console、发布 READY 和后续处理均使用同一个静态会话。

**完整替换 `kernel/main.c`：**

<a id="code-kernel-main-c-1"></a>

<!-- write: kernel/main.c -->
```c
#include <airi/kernel.h>   // 引入启动日志、错误停机与串口接口
#include <airi/console.h>  // 引入控制台状态与逻辑键编号

static struct airi_console console;  // 内核静态对象保留同一控制台的输入缓冲和显示内容

#ifndef AIRI_DEMO_MESSAGE  // 构建参数未指定输出文字时采用下方默认值
#define AIRI_DEMO_MESSAGE "Hello from AiriOS P1"  // 给本次启动准备默认演示文字
#endif  // 结束当前条件编译范围
_Noreturn void airi_kernel_main(const AiriBootInfo *info) {  // 校验启动信息后持续轮询输入并回送新命令输出
    const char *error = airi_boot_validate(info);  // 验证启动合同，NULL表示有效，非NULL是可直接输出的只读错误原因
    if (error)  // 启动合同验证返回非NULL原因时，在使用信息之前停止
        airi_panic(error);  // 把当前失败原因送到UART并停止内核
    airi_log("[BOOT_INFO] version=%u regions=%u loader=%s", info->version, info->region_count,  // 把当前启动状态记录送到UART
             info->loader);  // 把启动适配器保存的引导器名称作为%s实参
    airi_log("[KERNEL] physical=0x%llx virtual=0x%llx size=%llu",  // 把当前启动状态记录送到UART
             (unsigned long long)info->kernel_physical_base,  // 把当前64位字段转成格式要求的unsigned long long
             (unsigned long long)info->kernel_virtual_base, (unsigned long long)info->kernel_size);  // 把当前64位字段转成格式要求的unsigned long long
    airi_log("[HHDM] offset=0x%llx", (unsigned long long)info->direct_map_offset);  // 把当前启动状态记录送到UART
    for (uint32_t i = 0; i < info->region_count; ++i) {  // 逐条记录实际地图有效范围，尚未对其进行分配
        const struct airi_memory_region *r = &info->regions[i];  // 取实际启动地图第i条范围的只读地址，后续记录其物理起点和长度
        airi_log("[MEMORY] index=%u base=0x%llx length=0x%llx kind=%s", i,  // 把当前启动状态记录送到UART
                 (unsigned long long)r->base, (unsigned long long)r->length,  // 把当前64位字段转成格式要求的unsigned long long
                 airi_memory_name(r->kind));  // 把当前用途编号转为记录中的可读分类文字
    }
    uint64_t usable = airi_memory_bytes(info, AIRI_MEMORY_USABLE);  // 沿实际启动地图累加可用范围长度，结果单位是字节
    airi_log("[MEMORY_SUMMARY] usable_bytes=%llu boot_reclaim_bytes=%llu",  // 把当前启动状态记录送到UART
             (unsigned long long)usable,  // 把当前64位字段转成格式要求的unsigned long long
             (unsigned long long)airi_memory_bytes(info, AIRI_MEMORY_BOOT_RECLAIM));  // 把当前64位字段转成格式要求的unsigned long long
    airi_log("[LESSON] %s", AIRI_DEMO_MESSAGE);  // 把当前启动状态记录送到UART
    airi_console_init(&console, info);  // console绑定已验证的同一启动结构，初始化命令历史和输出环
    bool graphics = false;  // 当前只接串口，图形状态尚未启用
    if (graphics) {  // 只有本次图形真正启用时才报告成功显示参数
        const struct airi_framebuffer *fb = &info->framebuffer;  // 借用适配器给出的实际帧缓冲描述
        airi_log("[FRAMEBUFFER] width=%llu height=%llu pitch=%llu bpp=%u",  // 把当前启动状态记录送到UART
                 (unsigned long long)fb->width, (unsigned long long)fb->height,  // 把软件画布像素宽度、软件画布像素高度转成格式要求的unsigned long long
                 (unsigned long long)fb->pitch, (unsigned)fb->bpp);  // pitch是字节行距，转unsigned long long配%llu；bpp转unsigned配%u。
    } else  // 前一分支条件未成立时转入随后的单条替代操作
        airi_log("[FRAMEBUFFER_UNAVAILABLE] using serial output");  // 把当前启动状态记录送到UART
    /* P1 input checkpoint: serial state is ready; no drawing is implemented yet. */
    airi_log("[AIRI_P1_READY] graphics=%u usable_bytes=%llu", graphics ? 1u : 0u,  // 把当前启动状态记录送到UART
             (unsigned long long)usable);  // 把当前64位字段转成格式要求的unsigned long long
    airi_log("[AIRI_LESSON_READY] serial command loop active");  // 把当前启动状态记录送到UART
    airi_serial_write("airi> ");  // 发送没有换行的提示符，等待下一条输入
    for (;;) {  // 本操作持续循环，只有明确终点或分支才能退出
        int key = airi_input_poll();  // 每轮从键盘或UART取一个事件，无事件返回0
        if (!key) {  // 当前轮询没有逻辑事件时保持输入输出状态并继续等待
            __asm__ volatile("pause");  // 无输入时向CPU发忙等提示，仍继续轮询而没有睡眠调度
            continue;  // 本项不参与当前结果，跳过本轮剩余操作进入下一项
        }
        uint64_t before = console.total_lines;  // 记录按键前逻辑行号，提交后只发送新增输出
        bool changed = airi_console_key(&console, key);  // 把一个逻辑事件交给命令模型，返回是否可能需要刷新显示
        if (!changed)  // 该事件未改变可显示状态时无需发送更新记录
            continue;  // 本项不参与当前结果，跳过本轮剩余操作进入下一项
        if (key == '\n' || key == '\r') {  // 回车或换行说明本轮已提交命令，向串口发布完成后的控制台输出
            uint64_t first = console.total_lines - console.line_count;  // 求环中最早仍有效的逻辑行号
            if (before < first)  // 按键前起点已经被新输出覆盖时，从仍有效的最旧行开始
                before = first;  // 原起点已被覆盖时提升到仍可读取的最旧记录
            for (uint64_t i = before; i < console.total_lines; ++i) {  // 按逻辑行号读取仍保留或本次新增的console输出
                airi_serial_write(airi_console_line(&console, i));  // 读取第i条仍保留文字并逐字节送给UART
                airi_serial_write("\n");  // 每发送一条环记录后补串口换行
            }
            airi_serial_write("airi> ");  // 发送没有换行的提示符，等待下一条输入
        }

        /* Published after command handling; this checkpoint reports serial completion only. */
        if (key == '\n' || key == '\r' || key >= AIRI_KEY_PAGE_UP)  // 提交或功能事件处理完后才发更新记录，普通逐字节输入不产生这条完成标记
            airi_log("[CONSOLE_UPDATE] view=%u commands=%u page=%u", (unsigned)console.view,  // 把当前启动状态记录送到UART
                     console.commands, console.map_page);  // 发布处理后的提交计数与内存页号，供主机确认动作已完成
    }
}
```

主循环现在引用 console 与 input 的实现，将它们加入 SOURCES，链接才闭合。构建目标、固定架构和介质打包流程仍沿用已有规则；新增设备循环没有把主机观察代码编进 guest。

**完整替换 `Makefile`：**

<a id="code-makefile-1"></a>

<!-- write: Makefile -->
```makefile
# 交叉编译C和汇编的LLVM驱动
CC := clang
# 把独立ELF对象链接成内核
LD := ld.lld
# 后续主机观察使用本机C编译器
HOST_CC := cc
# 当前构建变体，未指定时normal
PROFILE ?= normal
# 当前变体产物目录
OUT := build/$(PROFILE)
# 已接入的内核C文件清单
SOURCES := kernel/boot/limine.c kernel/lib/base.c kernel/lib/runtime.c kernel/arch/x86_64/serial.c kernel/boot/info.c kernel/main.c kernel/console/console.c kernel/arch/x86_64/input.c
# 每个C文件映射到对象路径，再加入汇编入口对象
OBJECTS := $(patsubst %.c,$(OUT)/%.o,$(SOURCES)) $(OUT)/kernel/arch/x86_64/entry.o
# 独立x86-64 ELF、C11和无宿主运行时等编译约束
CFLAGS := --target=x86_64-unknown-none-elf -std=c11 -O2 -g -Wall -Wextra -Werror \
          -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-pie \
          -mno-red-zone -mgeneral-regs-only -mcmodel=kernel -MMD -MP
# 自有接口和协议头的包含目录
CPPFLAGS := -Iinclude -Ithird_party
# 按PROFILE构建变体选择下面的故障或输出宏
ifeq ($(PROFILE),no-framebuffer)
# 在已有预处理参数后追加当前变体宏
CPPFLAGS += -DAIRI_TEST_NO_FRAMEBUFFER
# 结束本次PROFILE条件分支
endif
# 按PROFILE构建变体选择下面的故障或输出宏
ifeq ($(PROFILE),missing-memory)
# 在已有预处理参数后追加当前变体宏
CPPFLAGS += -DAIRI_TEST_NO_MEMMAP
# 结束本次PROFILE条件分支
endif
# 按PROFILE构建变体选择下面的故障或输出宏
ifeq ($(PROFILE),edit-experiment)
# 在已有预处理参数后追加当前变体宏
CPPFLAGS += -DAIRI_PANEL_TITLE='"LOCAL / MAINTENANCE CONSOLE"' \
            -DAIRI_DEMO_MESSAGE='"I changed the output and rebooted"'
# 结束本次PROFILE条件分支
endif

# 这些名字是操作目标，即使存在同名文件也不判已完成
.PHONY: all kernel image host-test clean
# all为默认动作，依赖image，自身没有recipe。
all: image
# kernel为显式编译入口，ELF目标决定是否重建。
kernel: $(OUT)/airios.elf
# 声明本目标依赖哪些输入；源文件比对象新时重新执行recipe
$(OUT)/%.o: %.c
	# 先创建目标父目录；@隐藏命令回显，$(dir $@)取目标目录
	@mkdir -p $(dir $@)
	# 分别传预处理与编译参数；$<为首个源前提，$@为当前目标路径
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
# 声明本目标依赖哪些输入；源文件比对象新时重新执行recipe
$(OUT)/%.o: %.S
	# 先创建目标父目录；@隐藏命令回显，$(dir $@)取目标目录
	@mkdir -p $(dir $@)
	# 分别传预处理与编译参数；$<为首个源前提，$@为当前目标路径
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
# ELF任一对象或链接脚本更新时重新链接。
$(OUT)/airios.elf: $(OBJECTS) kernel/linker.ld
	# 独立静态链接，无宿主libc，链接脚本决定段与入口位置
	$(LD) -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 \
	    -z noexecstack -T kernel/linker.ld $(OBJECTS) -o $@
# image是.PHONY动作，每次调用都运行镜像制作；依赖先更新ELF。
image: $(OUT)/airios.elf
	# 把当前PROFILE传给镜像脚本，复用已锁定固件与引导器
	python3 scripts/image.py $(PROFILE)
# 加载编译器生成的头文件依赖；缺失.d不会中断第一次构建
-include $(OBJECTS:.o=.d)
```

## 主机送出一条请求，等待同一次处理的结果

<a id="uart-input-path"></a>

本次改用 `-serial stdio`：QEMU 标准输入进入 guest UART 接收端，发送端进入主机标准输出。这里 stdio 属于主机进程，AiriOS 还没有自己的标准文件描述符。`-monitor none` 关闭 QEMU 监控台，避免它和 UART 竞争输入。

父 Python 用 stdin=PIPE 建主机字节管道，写 `b"mem\n"` 再 flush，将字节交给 QEMU；后者送到虚拟 UART，poll 读取、模型积累并在换行时执行，再沿 UART 的另一方向返回结果。b 前缀表示 bytes，读回日志则解码为 str，两种表示分别服务传输和文本比较。

脚本先等本轮完整 READY，再发送一项。requests 的每项是 `(输入bytes,预期str)`，二元组解包交给 request/expected；offset 记发送前日志长度，后续只在新尾部检查该项结果与递增计数。收到完成再发下一条，避免把主机管道容量误当有限 UART 的接收容量，也避免拿旧输出证明新请求。

输出文件本轮截断，错误另存 qemu.log。列表推导按 splitlines 的候选行筛出有换行者；`airi> ` 本身无换行，可能连在记录前，removeprefix 只去这一个确定前缀，再比较完整字段，不任意剥空白。六次请求共享 90 秒预算，继承固定 QEMU 参数与 finally 清理。

**新建 `scripts/first_input.py`：**

<a id="code-scripts-first-input-py-1"></a>

<!-- write: scripts/first_input.py -->
```python
"""Observe UART queries, echo and unknown commands before the full test suite."""  # 本脚本观察真实UART命令，尚未使用QMP控制协议
from pathlib import Path  # 导入Path，后续用路径对象组合观察输出位置
import shutil      # 提供主机文件复制，给本次QEMU独立的可写固件变量
import subprocess  # 导入子进程接口，用于执行本次构建
import time  # 提供单调时钟与短暂等待，约束本轮观察期限
from tools import ROOT, tool  # 读取学习仓库根目录与已建立的工具查找函数

work = ROOT / 'build/first-input'  # 为这次UART观察选择独立产物目录
work.mkdir(parents=True, exist_ok=True)   # 自动建缺少的父目录，已有目录不会报错
firmware = ROOT / 'third_party/firmware'  # 固定UEFI固件输入所在目录
shutil.copyfile(firmware / 'edk2-i386-vars.fd', work / 'vars.fd')  # 复制可写变量区，使每次QEMU运行从同一固件状态开始
serial = work / 'serial.log'  # guest发送文字将由QEMU标准输出写入这个新日志
command = [tool('qemu-system-x86_64', 'qemu'), '-machine', 'q35', '-accel', 'tcg',  # 参数列表首项是实际QEMU路径，固定q35和TCG模拟执行
           '-cpu', 'qemu64', '-smp', '1', '-m', '256M', '-vga', 'std', '-display', 'none', '-no-reboot',  # 固定CPU模型、单核、256MiB、无图形窗口且禁止重启
           '-monitor', 'none', '-serial', 'stdio',  # 关闭QEMU文字监控，标准输入输出只连接guest UART
           '-drive', f'if=pflash,format=raw,unit=0,readonly=on,file={firmware / "edk2-x86_64-code.fd"}',  # 只读固件代码区用固定路径，f字符串将Path转进参数文字
           '-drive', f'if=pflash,format=raw,unit=1,file={work / "vars.fd"}',  # 可写变量区用本轮副本，避免污染锁定固件
           '-cdrom', str(ROOT / 'build/normal/airios.iso'), '-boot', 'd']  # 把Path转为进程参数字符串，从本轮光盘镜像启动
with serial.open('wb') as output, (work / 'qemu.log').open('wb') as errors:  # wb按字节写并截断旧日志，with退出时关闭两份文件
    process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=output, stderr=errors)  # 启动QEMU，父进程保留输入管道，UART输出与QEMU错误分文件保存
    try:  # 观察主体失败后也必须进入下面的QEMU清理
        requests = [(b'mem\n', 'No allocator accounting yet.'),  # 先查询真实启动库存。
                    (b'echo serial-mainline\n', 'serial-mainline'),  # 参数经 UART 进入同一命令模型。
                    (b'theme day\n', 'unknown command'),  # 尚未选择图形消费者时不提供主题功能。
                    (b'devices\n', 'ttyS0'),  # 当前固定设备端点可从串口查询。
                    (b'help\n', 'mem map'),   # 帮助与已实现命令对应。
                    (b'sys\n', 'bootstrap kernel')]  # 最后查询同一内核身份。
        index = 0      # 当前请求下标，六项下标为0到5
        ready = False  # 尚未观察到本轮内核启动完成
        waiting = False  # 当前没有已发送却未完成的请求
        offset = 0  # 本次请求发出前已有文本的字符长度
        deadline = time.monotonic() + 90    # 本次启动和六次交互共同最多90秒
        while time.monotonic() < deadline:  # 在截止前继续检查真实输出，运行中的进程并不代表成功
            text = serial.read_text(errors='replace')  # 输出文件字节解码为str，异常编码用替代字符保留其他结果
            lines = [line for line in text.splitlines(keepends=True) if line.endswith('\n')]  # 只处理有完整换行的记录，尾部未写完的行暂不使用
            if not ready:  # 启动阶段只寻找本轮独立日志中的READY
                ready = any(line.startswith('[AIRI_LESSON_READY]') for line in lines)  # 生成器逐行提供判断，any发现一个真值便停止查找
            if ready and not waiting:  # 上次请求已完成才发送下一条，设备接收缓冲不是无限队列
                request, expected = requests[index]  # 二元组解包得到本条输入bytes和预期结果str
                offset = len(text)  # 记录发出前旧文本尾，旧结果不用于证明本次完成
                process.stdin.write(request)  # 本条命令末尾换行提交，下一条等待它的完成记录
                process.stdin.flush()  # 把父进程缓冲交给主机管道，再由QEMU递交给UART
                waiting = True  # 已有一条请求等待，后续轮询不能重复发送
            tail = text[offset:]  # 切片从本次发送前记录的字符偏移取得新结果
            records = [line for line in tail.splitlines(keepends=True) if line.endswith('\n')]  # 新尾部同样只使用完整串口记录
            marker = f'[CONSOLE_UPDATE] view=0 commands={index + 1} page=0\n'  # 第index条请求是第index+1次提交，f字符串代入累计计数
            if waiting and any(line.removeprefix('airi> ') == marker for line in records):  # 限定新结果并去确定提示符前缀，再比较完整完成行
                if expected not in tail:  # 完成计数以外还检查本条查询或错误的实际内容
                    raise RuntimeError(f'UART request {index + 1} missed: {expected}')  # 指出哪次请求缺少什么结果，不能继续报告成功
                print('UART', index + 1, request.decode().strip(), '->', expected)  # 输入bytes转str并去换行，仅用于主机观察显示
                index += 1  # 当前一条已核对，下一请求下标前进一
                waiting = False  # 此后才能向同一UART发送下一条命令
                if index == len(requests):  # 六条全部核对后完成本轮观察
                    print('PASS first UART input: mem / echo / unknown / devices / help / sys')  # 实际交互全部成功才显示可检查的成功文字
                    break  # 结束等待，随后统一回收QEMU
            if process.poll() is not None:  # None表示仍运行，其余是已退出的状态值
                raise RuntimeError('QEMU exited before the UART response')  # 未完成全部请求却退出，立即报告失败
            time.sleep(0.1)  # 轮询间隔100毫秒，不在主机占满CPU忙等
        else:  # 没有成功break而耗尽截止时间时进入超时分支
            raise RuntimeError('UART command timed out')  # 未出现应收记录不能当作成功
    finally:  # 不论观察成败都执行本轮资源清理
        try:  # 关闭写端可能遇到QEMU已退出导致的管道错误。
            process.stdin.close()  # 释放父进程写端，不再提交设备字节。
        finally:  # close即使抛错，也必须回收本轮子进程。
            process.terminate()  # 请求结束当前QEMU，不等待其自行永久退出。
            try:  # 正常结束最多等待五秒。
                process.wait(timeout=5)  # 收回退出状态，避免遗留子进程。
            except subprocess.TimeoutExpired:  # 请求终止后仍未退出，采用强制结束。
                process.kill()  # 强制结束本轮QEMU。
                process.wait()  # 再收回已结束进程状态。
```

先构建本轮镜像，再分别启动启动观察与输入观察。它们各自创建 QEMU、各从命令计数 0 开始，不能把第一次机器的 READY 与第二次机器的回复拼成一次成功。

**在学习目录根目录运行：**

<!-- run -->
```sh
# 通过声明的PYTHONPATH从本学习目录导入构建模块，再执行下方Python命令
PYTHONPATH=scripts python3 - <<'PY'  # 仅此进程从 scripts 查找模块，单引号阻止 Shell 展开随后 Python 文本
import subprocess  # 导入子进程接口，用于执行本次构建
from tools import make_command  # 取得带已查明工具路径的Make参数构造函数
subprocess.run(make_command(target='all'), check=True)  # 构造all目标并执行，非零退出码使本轮操作失败
PY
python3 scripts/first_boot.py  # 启动独立QEMU并等完整启动成功标记
python3 scripts/first_input.py  # 启动另一份QEMU，通过真实UART提交六条命令并等最终完成记录
```

启动标记应变为 `serial command loop active`。first_input 经真实 UART 顺次提交 mem、echo serial-mainline、theme day、devices、help、sys，每项都带换行。第三项应报告未知命令，其余取得相应查询或回显；核对该项计数和内容以后才继续，六项全部符合才成功。

设备字节、持续会话、命令结果和设备发送至此连通。下一步固定串口构建与自动观察入口，让后续异常、内存和 I/O 实现都能复用它。字体、像素和窗口由独立图形路线接入。
