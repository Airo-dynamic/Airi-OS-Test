#include <airi_libc.h> // 单执行者观察TLS零尾部，不依赖线程库或errno。
static _Thread_local volatile unsigned initial = 7; // 四个文件初值字节，位于tdata。
static _Thread_local volatile unsigned char zeros[257]; // 较长的tbss，运行副本需要清零但普通LOAD不为它预留全部范围。
static volatile unsigned char ordinary; // 普通BSS只有一字节，明显短于TLS零尾部。
int main(void) { // 装载器应接受真实LLD的独立初值与零尾部长度。
    if (initial != 7 || ordinary) // TLS初值和普通LOAD零字节各自正确。
        return 51; // 不把两种存储误作同一个对象。
    for (unsigned i = 0; i < sizeof(zeros); ++i) // 全部运行TLS零尾部都由单独副本提供。
        if (zeros[i]) // 普通LOAD的memsz短不能导致TLS运行块遗漏清零。
            return 52; // 任何旧内容都属于装载错误。
    zeros[0] = 0x31; zeros[256] = 0x72; ordinary = 0x53; // 分别写两个TLS边界和普通BSS。
    if (initial != 7 || zeros[0] != 0x31 || zeros[256] != 0x72 || ordinary != 0x53) // 相似模板VA不代表运行对象互相覆盖。
        return 53; // 普通BSS不能别名到真正TLS运行副本。
    airi_puts("[CTLS_TAIL_OK] initialized=4 zero_tail=257 ordinary_bss=1 independent=1\n"); // 所有真实用户读写完成后发布。
    return 0; // shell继续，用户内存仍按既有退出路径整体归还。
}
