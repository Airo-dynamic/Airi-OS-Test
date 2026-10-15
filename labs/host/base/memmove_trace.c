#include <airi/base.h>  // 取得自有字节或格式函数声明，主机观察程序按这些签名调用
#include <stdio.h>      // 主机观察程序借用 printf 和 puts 显示结果；内核函数仍使用自己的实现
#include <string.h>     // 主机观察用 strcmp/strcpy 检查或准备文字，不加入内核链接

int main(void) {  // 主机 C 运行时调用此入口，返回值成为观察进程退出状态
    char right[] = "ABCDE", left[] = "ABCDE", wrong[] = "ABCDE";  // 三个独立数组分别观察两个正确方向及错误正向算法
    airi_memmove(right + 1, right, 4);  // 把前四字节向高地址挪一格，逆序搬运后得到 AABCD
    airi_memmove(left, left + 1, 4);    // 把偏移 1 的四字节向低地址挪一格，得到 BCDEE
    for (unsigned i = 0; i < 4; ++i)    // 故意从低到高搬四字节，用于展示错误，不改变基础库
        wrong[i + 1] = wrong[i];  // 目标覆盖下一轮待读源字节，B/C/D 会先后丢失
    printf("right: ABCDE -> %s (expected AABCD)\n", right);  // 显示正确逆序搬运的实际结果和期望值
    printf("left:  ABCDE -> %s (expected BCDEE)\n", left);   // 显示正确正序搬运的实际结果和期望值
    printf("wrong forward loop: %s (source bytes overwritten)\n", wrong);  // 显示错误算法所得 AAAAA，将故障与正确实现并排观察
    char output[5];  // 另准备五字节数组，用于观察较长文字的截断
    int count = airi_snprintf(output, sizeof(output), "%s", "abcdef");  // 仅保存 abcd 和 NUL，但返回完整所需长度 6
    printf("format: cap=5 stored=%s required=%d\n", output, count);     // 同时显示截断结果与完整长度，防止混淆两种数量
    if (strcmp(right, "AABCD") || strcmp(left, "BCDEE") || strcmp(wrong, "AAAAA") ||  // 检查三个搬运结果，任一不符便进入失败出口
        strcmp(output, "abcd") || count != 6)  // 再检查截断文字与所需长度，两项都必须符合当前容量模型
        return 1;  // 本轮实际结果与期望不符，以非零状态结束主机观察
    puts("PASS lab/memmove: direction and truncation observed");  // 全部观察符合才打印本次成功标记
    return 0;  // 本轮所有结果均符合，向 shell 交回成功状态
}
