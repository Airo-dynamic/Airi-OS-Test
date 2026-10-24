#include <airi_libc.h> // 同一静态运行入口，不手工初始化TLS。
_Thread_local volatile unsigned char only = 7; // 唯一TLS变量迫使模板为1字节、对齐1。
int main(void) { // TLS数据应在16字节对齐TP之前最后一个字节。
    long base = airi_fs_base(); // 编译器地址计算还会通过FS:0读取self。
    if (base != 0x60000010 || (uintptr_t)&only != (uintptr_t)base - 1 || only != 7) // 不能错误地从块首复制后仅把TP向上挪15字节。
        return 220; // 小对齐不是64对齐测试的同义重复。
    only = 9; // 实际写入编译器解析出的负偏移。
    if (only != 9) // 硬件用户写必须能读回。
        return 221; // 不以模板常量替代访问。
    airi_puts("[CTLS_CHAR_OK] bytes=1 alignment=1 tp=16 value=9\n"); // 真实小模板布局正确。
    airi_exec("ctls-page"); // 最后覆盖4KiB纯零模板与第二页TCB。
    return 226; // 成功exec不回到这一份小模板。
}
