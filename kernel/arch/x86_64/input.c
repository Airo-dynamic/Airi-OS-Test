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
