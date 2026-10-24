#include <airi_libc.h> // 模板达到上限且没有文件初值，TCB必须另有映射。
_Thread_local volatile unsigned char page[4096]; // 真实.tbss整页，PT_TLS.filesz为0而memsz为4096。
int main(void) { // 编译器仍用local-exec地址，不手工设置FS。
    long base = airi_fs_base(); // TP应紧随4096字节本地数据，位于第二页起点。
    if (base != 0x60001000 || (uintptr_t)&page[0] != 0x60000000 || *(uintptr_t *)(uintptr_t)base != (uintptr_t)base) // TCB首字也必须可读且为self。
        return 223; // 只映射模板一页而漏掉TCB会在这里暴露。
    for (unsigned i = 0; i < sizeof(page); ++i) // 零初值不需要文件里提供4096个字节。
        if (page[i]) // 检查全范围而非仅首尾。
            return 224; // 不能复用旧TLS内容。
    page[0] = 0x31; page[4095] = 0x79; // 两个边界值都属于TLS数据，不覆盖下一页TCB。
    if (page[0] != 0x31 || page[4095] != 0x79) // 实际读回确认边界寻址。
        return 225; // 明确报告失败。
    airi_puts("[CTLS_PAGE_OK] bytes=4096 initialized=0 pages=2 tcb=second zeroed=all\n"); // 最大模板和额外TCB页均已实际访问。
    return 0; // 原shell的ctls命令至此结束。
}
