# 03 · 让字节复制在重叠时仍然正确

引导环境已准备好，自己的内核还没有入口。现在先实现以后处理启动结构、日志和命令时都需要的字节操作。普通 C 应用可以链接 libc，取得 memcpy 等实现；内核将自行管理运行环境，所以这里写自有函数，再用主机程序检查它们的行为。

同一个实现能在主机运行，是因为这批函数只访问调用者提供的内存，不读取设备寄存器，也不要求内核页表。稍后观察入口借主机 libc 的 printf 显示结果，与基础函数是否依赖 libc 是两件事：链接得到的 Mac 程序包含自己的字节算法和主机显示入口，算法以后也会按内核目标重新编译。

命令控制台稍后要保存八条历史，满了就把后七条向前挪一格。它们在同一个数组中，目标可能覆盖还没读过的源内容。这里先用观察程序实际准备的 right、left 两份 `ABCDE` 处理同一问题：right 把前四字节向右挪一格，left 则把后四字节向左挪一格。两份输入独立，后面分别核对结果。

<a id="nul-string"></a>

字符串 `"ABCDE"` 占六个字节，五个字符后还有值为零的 NUL，通常写作 `\0`。当前按 ASCII 解释字符，A 的编号为 65、B 为 66；字节复制只搬这些数值，不判断它们是否构成文字。strlen 依靠 NUL 找到结尾，memcpy/memmove 则按给定数量搬运，不自动添加终止零。

## 调用者提供空间，函数只处理其中的字节

头文件先公开五个函数的参数与返回类型，让后面的 C 文件能检查调用。包含保护 `#ifndef/#define/#endif` 避免同一翻译单元重复声明；工具链头 stddef.h、stdint.h、stdarg.h 提供类型与宏，包含这些声明不等于链接主机 libc。

<a id="byte-pointers"></a>

在当前目标上，char 占一个八位字节，unsigned char 表示 0..255。把 `void *` 转成 `unsigned char *` 后，下标 i 就定位起点之后第 i 个字节；源带 const，限制函数通过它写入，目标允许写入。指针只定位已有对象，不分配空间，调用者必须保证前 n 个字节可读或可写。

n 用表示对象大小的无符号 size_t 保存；uint8_t、uint32_t、uint64_t 说明固定整数位宽，uintptr_t 用于保存对象指针转换得到的无符号地址数值。这些类型各有用途，尤其地址整数稍后只用于固定目标上的先后比较。

**新建 `include/airi/base.h`：**

<a id="code-include-airi-base-h-1"></a>

<!-- write: include/airi/base.h -->
```c
#ifndef AIRI_BASE_H  // 第一次包含时进入此范围，后续同一翻译单元跳过重复声明
#define AIRI_BASE_H  // 记录基础头文件已处理，配合末尾 endif 构成包含保护
#include <stddef.h>  // 取得 size_t、ptrdiff_t、NULL 等工具链定义
#include <stdint.h>  // 取得固定宽度整数及 uintptr_t，不会因此链接宿主 libc
#include <stdarg.h>  // 取得 va_list 和可变参数遍历宏，稍后的格式函数会使用

void *airi_memcpy(void *dst, const void *src, size_t n);   // 从 src 读 n 字节写入 dst，双方不得重叠，返回原目标地址
void *airi_memmove(void *dst, const void *src, size_t n);  // 按地址次序选择搬运方向，允许双方范围重叠
void *airi_memset(void *dst, int value, size_t n);  // 将 value 转成一个无符号字节，填入 dst 的 n 个位置
int airi_memcmp(const void *a, const void *b, size_t n);  // 按无符号字节比较 n 项，首个差异决定返回值正负
size_t airi_strlen(const char *s);  // 读到 NUL 才停止，返回前面的字符数，不计 NUL
#endif  // 结束头文件包含保护，后续使用同一公开声明
```

先处理源、目标互不重叠的情形。airi_memcpy 将两端看作字节数组，从下标 0 开始逐项读源、写目标；返回原来的 dst，调用者仍可沿该起点使用复制结果。n 保持总长度，循环中的 i 表示当前偏移。

n 为零时不进入循环，也不解引用任何地址。这个性质将在观察里用 NULL 与零长度比较。允许重叠的搬运还需要判断方向，不能让调用者用 memcpy 猜测覆盖结果。

**新建 `kernel/lib/base.c`：**

<a id="code-kernel-lib-base-c-1"></a>

<!-- write: kernel/lib/base.c -->
```c
#include <airi/base.h>  // 按公开声明编译自己的字节函数，保证定义与调用使用同一签名
#include <limits.h>     // 取得 INT_MAX，为格式化返回值的饱和处理提供上限

void *airi_memcpy(void *dst, const void *src, size_t n) {  // 本函数只处理互不重叠的两个已准备范围，不分配内存
    unsigned char *d = dst;  // 目标转成字节指针，每个下标对应一个可写字节
    const unsigned char *s = src;   // 源转成只读字节指针，此函数不通过它写入
    for (size_t i = 0; i < n; ++i)  // 从低下标搬运前 n 个字节，n 为零时不访问源和目标
        d[i] = s[i];  // 先读取当前源字节，再写同一偏移的目标位置
    return dst;  // 交回调用者最初提供的目标起点，便于继续处理该范围
}
```

<a id="overlap-copy"></a>

## 重叠时，先保护下一次还要读的源字节

观察里的第一条调用是 `airi_memmove(right + 1, right, 4)`。right 初值为 `ABCDE\0`，所以 dst 接住 right+1，src 接住 right，n 为 4；目标从 B 的位置开始，源却从 A 开始。若仍从左到右，第一次写入就把 B 覆盖成 A，下一轮读到的也是 A，四轮以后得到 AAAAA。

这次必须从末尾搬：先把源中的 D 写到 E 的位置，再搬 C、B、A。每个源字节都在将被覆盖以前读出，四轮后 right 成为 `AABCD\0`。n=4 只规定这四次搬运，原数组最后的 NUL 没有移动，也没有被覆盖。

第二条调用是 `airi_memmove(left, left + 1, 4)`，用另一份新的 `ABCDE\0`。这次 dst 是 left，src 是 left+1，正向先读 B 写到开头，不会破坏后面的 C、D、E。结果是 `BCDEE\0`；最后一个 E 与 NUL 都不在写入范围内，仍保留原值。搬运不会自动擦掉源范围剩下的旧字节。

airi_memmove 据地址次序选择方向：目标不高于源时正向，否则反向。代码将地址转为 uintptr_t 再比较，依赖本项目 x86-64 与主机观察的平坦地址表示；C 不为不相关对象的一般指针大小比较提供同样保证。逆序循环从 n 开始使用 i−1，n=0 不进入循环，也不会发生无符号下溢。

**追加到 `kernel/lib/base.c` 末尾：**

<a id="code-kernel-lib-base-c-2"></a>

<!-- append: kernel/lib/base.c -->
```c
void *airi_memmove(void *dst, const void *src, size_t n) {  // 在调用者提供的范围中搬字节，重叠时保护尚未读取的源内容
    unsigned char *d = dst;  // 目标转成字节指针，每个下标对应一个可写字节
    const unsigned char *s = src;  // 源转成只读字节指针，此函数不通过它写入
    /* 地址转换为整数后比较；本实现依赖 x86-64 和主机实验的平坦地址表示。 */
    if ((uintptr_t)d <= (uintptr_t)s) {  // 在本目标的平坦地址表示下比较地址整数，目标不高于源时向前搬
        for (size_t i = 0; i < n; ++i)  // 从低下标搬运前 n 个字节，n 为零时不访问源和目标
            d[i] = s[i];  // 先读取当前源字节，再写同一偏移的目标位置
    } else {  // 目标高于源时改为逆序，先保存靠近末尾的源字节
        for (size_t i = n; i != 0; --i)  // 先从数量 n 开始，每轮使用 i-1；零长度不会下溢
            d[i - 1] = s[i - 1];  // 依次写最后一个到第一个字节，避免覆盖后续待读源字节
    }
    return dst;  // 交回调用者最初提供的目标起点，便于继续处理该范围
}
```

## 同一份内存怎样被清零、比较和判定字符串长度

搬运解决位置变化，初始化和核对还需要另外三项操作。memset 将 int 转为 unsigned char，把低八位重复写入已有范围；填零后的数组可表示空字符串或初始状态，具体含义由消费者决定。

memcmp 从第一个字节开始比较，将两个无符号字节提升到 int 再相减。比如 A=65、B=66，首个不同处返回负值，调用方据符号判断次序；不必依赖具体差值。全部相同返回 0，n=0 则直接相等而不读取指针。

strlen 的结束条件不同：它没有 n，只能读到 NUL 停止，返回前面的字节数，不含终止零。调用者必须保证可读范围中确有这个标记，不能把任意二进制范围直接当字符串传入。这五个函数都借用现有空间，当前仍没有分配器。

**追加到 `kernel/lib/base.c` 末尾：**

<a id="code-kernel-lib-base-c-3"></a>

<!-- append: kernel/lib/base.c -->
```c
void *airi_memset(void *dst, int value, size_t n) {  // 把调用者已有范围填成相同字节，不创建新对象
    unsigned char *d = dst;  // 目标转成字节指针，每个下标对应一个可写字节
    for (size_t i = 0; i < n; ++i)  // 依次填充目标前 n 个字节，n 为零时不访问目标
        d[i] = (unsigned char)value;  // 转换后的低八位写入当前位置，例如 value=0 清零
    return dst;  // 交回调用者最初提供的目标起点，便于继续处理该范围
}
int airi_memcmp(const void *a, const void *b, size_t n) {  // 首个不同字节确定次序；n=0 时两指针都不解引用
    const unsigned char *x = a, *y = b;  // 把两个输入都按无符号字节查看，避免高位字符产生负值
    for (size_t i = 0; i < n; ++i)  // 依次比较前 n 个字节，n 为零时不读取任一输入
        if (x[i] != y[i])  // 只有当前字节不相等才结束比较，前面各项均已相等
            return (int)x[i] - (int)y[i];  // 先提升成 int 再相减，返回负、零、正值表达字节次序
    return 0;  // 前 n 项全部相同，返回相等结果
}
size_t airi_strlen(const char *s) {  // 输入必须在可读范围内存在 NUL，本函数没有额外容量参数
    size_t n = 0;  // 从字符串第零字节开始累计可见字符长度
    while (s[n])   // 当前字节非零时继续，NUL 使条件为假
        ++n;  // 计入一个非零字节，下一轮检查紧随其后的字节
    return n;  // 交回 NUL 前的字节数量，终止零本身未被计入
}
```

## 让两份独立输入分别经过两个方向

现在把前面两条调用放进同一个观察入口。right、left 各自从 `ABCDE` 初始化成六字节数组，两个参数组合分别触发逆序与正序分支。第一项的修改不会成为第二项的输入；两边原尾部 NUL 都保留，所以 printf 能把得到的两份结果作为字符串显示。

最后比较六字节，连终止零一起核对，任一不符就由逻辑或 `||` 产生非零退出。另一次 `airi_memcmp(NULL,NULL,0)` 专门利用零长度不访问输入的约定，并不意味着非零长度也能读取 NULL。

**新建 `labs/tutorial/bytes.c`：**

<a id="code-labs-tutorial-bytes-c-1"></a>

<!-- write: labs/tutorial/bytes.c -->
```c
#include <airi/base.h>  // 取得自有字节或格式函数声明，主机观察程序按这些签名调用
#include <stdio.h>      // 主机观察程序借用 printf 和 puts 显示结果；内核函数仍使用自己的实现
int main(void) {  // 主机 C 运行时调用此入口，返回值成为观察进程退出状态
    char right[] = "ABCDE", left[] = "ABCDE";  // 准备两个独立六字节数组，各含五个字符和一个 NUL
    airi_memmove(right + 1, right, 4);  // 把前四字节向高地址挪一格，逆序搬运后得到 AABCD
    airi_memmove(left, left + 1, 4);    // 把偏移 1 的四字节向低地址挪一格，得到 BCDEE
    printf("right=%s left=%s zero=%d\n", right, left, airi_memcmp(NULL, NULL, 0));  // 显示两数组并执行零长度比较；memcmp 不读 NULL 指向的字节
    return airi_memcmp(right, "AABCD", 6) || airi_memcmp(left, "BCDEE", 6);  // 比较包含 NUL 的六字节，任一结果不符就返回非零
}
```

下面把观察入口与 `kernel/lib/base.c` 一起交给主机 cc，分别编译后链接成 build/probe。入口使用主机运行时与 printf，调用的 airi_memmove/memcmp 则来自刚写好的文件；内核以后会重新编译同一实现。

`-std=c11` 选择 C11，`-O0` 关闭优化，`-g` 保留调试信息，`-Wall -Wextra -Werror` 启用常见警告并把警告当失败。`-Iinclude` 指定项目头文件目录，`-o` 指定可执行产物路径。mkdir 只准备输出目录，真正运行发生在最后一条命令。

整个命令块交给独立的 `sh -eu` 执行。编译失败时立即结束，不会接着运行上一次留下的程序；结束行 `SH` 单独顶格输入。

**在学习目录根目录运行：**

<!-- run -->
```sh
sh -eu <<'SH'  # 构建失败便结束，不继续运行先前的主机产物。
mkdir -p build  # 建立主机观察产物目录，已有目录也可继续
cc -std=c11 -O0 -g -Wall -Wextra -Werror -Iinclude labs/tutorial/bytes.c kernel/lib/base.c  -o build/probe  # 把本轮观察源码和实现编译链接成指定主机可执行文件
./build/probe  # 运行刚生成的主机观察，非零退出表示实际结果不符
SH
```

应输出 `right=AABCD left=BCDEE zero=0`，并以 0 结束。前两项说明两个方向都保住了待读源字节，第三项说明零长度比较没有要求输入空间；六字节核对还检查了字符串结束位置。

这些实现只搬动或检查已有字节。下一章让整数 123 产生字符 1、2、3，并在容量不足时继续报告完整长度，为内核日志准备可发送的文字。
