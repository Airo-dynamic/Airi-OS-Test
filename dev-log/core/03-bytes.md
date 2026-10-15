# 03 · 让字节复制在重叠时仍然正确

内核收到引导数据、编辑命令字符串和形成日志时，都需要搬动字节。libc 是提供常用 C 库函数实现的库，普通应用链接它就可以调用 memcpy 等函数；当前内核没有另一个操作系统提供这套运行环境，先写自己的字节函数。用当前电脑运行一个小程序，观察复制方向对同一数组的影响。

后面的命令控制台最多保存八条历史；存满后要把后七条向前挪一格，为新命令腾位置。源与目标都在同一数组里，选择错误方向就会覆盖尚未读出的内容。这里先用较短的 `ABCDE` 数组观察两个方向，随后让历史移动使用同一函数。

<a id="nul-string"></a>

NUL 是值为零的字节，通常写成 `\0`。`"ABCDE"` 的内容有五个可见字符和一个 NUL；strlen 找结束标记，memcpy/memmove 按给定数量搬运，不会自动补结束标记。

字符与字节按编码对应，当前使用 ASCII：A 的编号是 65，B 是 66，零字节保留作结束标记。复制操作只搬字节，不判断它们是不是文字。

## 声明字节操作接口

头文件公开五个函数的参数和返回类型，其他 C 文件包含它后才能检查调用。`#ifndef`、`#define`、`#endif` 组成包含保护，避免同一个翻译单元重复处理这些声明。`stddef.h`、`stdint.h` 和 `stdarg.h` 由工具链提供类型或宏，链接 libc 是另外一步。

<a id="byte-pointers"></a>

C 的 char 对象在本项目目标上占一个八位字节，unsigned char 能保存 0..255。stdint.h 提供 uint8_t、uint32_t、uint64_t 等明确位宽的整数名；uintptr_t 是可以保存对象指针转换结果的无符号整数类型。它们描述不同范围，不是给同一个值随意换名字。

一个指针保存对象地址。`src[i]` 在字节指针上从起点向后 i 字节读取，`dst[i]` 写目标对应位置；这两个地址不负责分配内存。调用方必须已准备好至少 n 个可读或可写字节。

`size_t` 是表示对象大小的无符号整数。`void *` 可以接收不同对象的地址，逐字节处理时再转为 `unsigned char *`；后者每推进一项就移动一个字节。源指针带 `const`，限制函数通过它写入，目标指针则允许写入。

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

先实现要求源、目标互不重叠的 `airi_memcpy`。它从低下标开始，依次读取源字节并写到目标。返回值仍为调用者最初给出的 `dst`，便于继续使用复制起点。

`n=0` 时循环不执行，函数也不会通过指针访问字节。允许重叠的复制需要单独决定方向，`airi_memmove` 根据两段地址决定方向。

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

对数组 `ABCDE\0` 执行 `dst=buffer+1`、`src=buffer`、`n=4`，目标比源高一个字节。正向复制的第一次写入会覆盖下一次要读的 B，重复后得到 AAAAA。倒着搬则先保存 D，再保存 C、B、A，结果是 `AABCD\0`。

`airi_memmove` 用地址的相对次序选择方向：目标较低时正向搬，目标较高时反向搬。重新从 `ABCDE\0` 开始，把 `buffer+1` 的四字节搬到 `buffer` 会得到 `BCDEE\0`。代码将地址转换为 `uintptr_t` 再比较，这依赖本项目目标上的平坦地址表示；C 对不相关对象的指针大小比较没有提供同样的通用保证。

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

复制方向确定后，再补齐填充、比较和字符串长度。填充将 `int` 转成 `unsigned char`，每项写入低八位；比较将两个无符号字节提升成 `int` 后相减，以返回值的正、负或零表示次序。字符串长度函数一直读到终止零 NUL，返回值不包含这一字节，因此输入必须在可读范围内以零结束。

例如字节比较把字符 A 和 B 当作数值 65 与 66，遇到第一个不同字节时返回负值，调用者据此判断左边在前；无需返回精确的“差几位”。`airi_memset(buffer,0,sizeof(buffer))` 则把整个已有数组填零，随后它既可以是空字符串，也可以是清零的状态结构，含义由接收者决定。

这些函数都操作调用者提供的内存，此时尚无分配器。

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

观察程序为左右两个方向分别提供 `ABCDE` 数组。字符串初始化会补 NUL，所以 `sizeof(right)` 为 6；移动四个字节后，原有尾部零仍使打印有效。主机程序用 `printf` 显示结果，再用已有比较函数检查数组，任何不符都通过逻辑或 `||` 转成非零退出。

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

编译命令同时列出观察程序和 `kernel/lib/base.c`，工具链将它们分别编译再链接。`cc` 是主机编译驱动；`-std=c11` 选择 C11，`-O0` 关闭优化，`-g` 保留调试信息，`-Wall -Wextra -Werror` 启用常见警告并将警告作为失败。

`-Iinclude` 使 `<airi/base.h>` 从项目 include 目录查找。`-o` 后的路径保存主机可执行文件。

**在学习目录根目录运行：**

<!-- run -->
```sh
mkdir -p build  # 建立主机观察产物目录，已有目录也可继续
cc -std=c11 -O0 -g -Wall -Wextra -Werror -Iinclude labs/tutorial/bytes.c kernel/lib/base.c  -o build/probe  # 把本轮观察源码和实现编译链接成指定主机可执行文件
./build/probe  # 运行刚生成的主机观察，非零退出表示实际结果不符
```

终端输出 `right=AABCD left=BCDEE zero=0`。两个方向都保住了尚未读取的源字节，零长度比较没有访问数据。这五个函数已经可以供后面的内核模块使用；日志还需要把整数转换成字符，接着增加有容量限制的格式化。
