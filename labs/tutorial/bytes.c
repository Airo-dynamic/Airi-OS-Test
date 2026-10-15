#include <airi/base.h>  // 取得自有字节或格式函数声明，主机观察程序按这些签名调用
#include <stdio.h>      // 主机观察程序借用 printf 和 puts 显示结果；内核函数仍使用自己的实现
int main(void) {  // 主机 C 运行时调用此入口，返回值成为观察进程退出状态
    char right[] = "ABCDE", left[] = "ABCDE";  // 准备两个独立六字节数组，各含五个字符和一个 NUL
    airi_memmove(right + 1, right, 4);  // 把前四字节向高地址挪一格，逆序搬运后得到 AABCD
    airi_memmove(left, left + 1, 4);    // 把偏移 1 的四字节向低地址挪一格，得到 BCDEE
    printf("right=%s left=%s zero=%d\n", right, left, airi_memcmp(NULL, NULL, 0));  // 显示两数组并执行零长度比较；memcmp 不读 NULL 指向的字节
    return airi_memcmp(right, "AABCD", 6) || airi_memcmp(left, "BCDEE", 6);  // 比较包含 NUL 的六字节，任一结果不符就返回非零
}
