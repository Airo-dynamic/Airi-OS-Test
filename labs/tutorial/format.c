#include <airi/base.h>  // 取得自有字节或格式函数声明，主机观察程序按这些签名调用
#include <stdio.h>      // 主机观察程序借用 printf 和 puts 显示结果；内核函数仍使用自己的实现
int main(void) {  // 主机 C 运行时调用此入口，返回值成为观察进程退出状态
    char text[5];  // 观察数组只有五字节，最多保存四个字符和一个 NUL
    int required = airi_snprintf(text, sizeof(text), "%s:%u", "IO", 123u);  // 传字符串与 unsigned 实参，保存 IO:1 并取得所需长度 6
    printf("stored=%s required=%d capacity=%zu\n", text, required, sizeof(text));  // 并排显示实际文字、完整长度和数组容量，分别使用匹配格式
    return required != 6 || airi_memcmp(text, "IO:1", 5) != 0;  // 长度不符或保存的五字节含 NUL 不符时，观察程序失败
}
