#include <airi_libc.h> // 新ELF先查询基址，不解引用尚未建立的TLS。
int main(void) { // 成功exec必须从停用的FS环境开始。
    if (airi_fs_base() != 0) // PID可保留，旧用户本地块不能作为新映像的默认环境。
        return 191; // 父仍能收到具体失败位置。
    airi_puts("[TLS_EXEC_RESET_OK] fs_base=0 old_block=detached\n"); // 只在真实查询完成后报告。
    return 0; // 原父wait同一孩子身份。
}
