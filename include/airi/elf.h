#ifndef AIRI_ELF_H // 静态ELF解析边界只声明一次。
#define AIRI_ELF_H // 包含保护。
#include <stdbool.h> // 校验器使用布尔范围与入口状态。
#include <airi/base.h> // 固定宽度字段与自有字节复制。
#define AIRI_ELF_SEGMENTS 8u // 教学程序最多八个LOAD段。
#include <airi/user_limits.h> // ELF验证必须为实际初始栈预留容量。
#define AIRI_ELF_PAGES (AIRI_USER_PAGES - AIRI_USER_STACK_PAGES) // 段最多1008页，栈的16页单独建立。
struct airi_elf_segment { // 从外部字节校验后复制出的段事实。
    uint64_t address, offset, file_bytes, memory_bytes; // 用户VA、映像偏移、初始化长度和总内存长度。
    unsigned flags; // 原ELF PF_R/PF_W/PF_X，不能当作x86 PTE直接写入。
};
#define AIRI_ELF_TLS_LIMIT 4096u // 本轮单静态模块最多4KiB模板，运行块含TCB另算页。
struct airi_elf_tls { // PT_TLS描述模板，偏移与实际运行块布局分开。
    uint64_t address, offset, file_bytes, memory_bytes, alignment; // 模板VA、文件位置、初值/总长度与对齐。
    unsigned pointer_offset, data_offset, pages; // 运行块中TP位置、初值落点和含16字节TCB的页数。
};
struct airi_elf_view { // 只借用输入映像，装载完成前映像必须持续有效。
    const uint8_t *image; // 不拥有也不修改这些输入字节。
    size_t bytes; // 输入映像的实际可读取长度。
    uint64_t entry; // 入口必须位于可执行段的文件内容中。
    unsigned count, pages; // 已验证LOAD段数与向外舍入后的总页数。
    struct airi_elf_tls tls; // memory_bytes为0表示无需初始化FS；不改变无TLS程序的入口。
    struct airi_elf_segment segments[AIRI_ELF_SEGMENTS]; // 每项已通过范围、权限和重叠检查。
};
const char *airi_elf_inspect(const void *image, size_t bytes, struct airi_elf_view *view); // NULL表示通过；失败不修改输出view。
#endif // 结束包含保护。
