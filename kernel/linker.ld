/* 请求节布局采用 Limine 官方 0BSD C 模板，许可来源见 THIRD_PARTY。 */
OUTPUT_FORMAT(elf64-x86-64)  /* 输出采用 x86-64 的 64 位 ELF 格式 */
ENTRY(_start)  /* 把符号 _start 的最终虚拟地址写入 ELF 入口字段 */
PHDRS {  /* 声明装载段表，下面的各节会显式归入这些段 */
    requests PT_LOAD FLAGS(4);  /* 引导请求段标为可读，固定协议仍允许引导器在交接前填写响应 */
    text PT_LOAD FLAGS(5);  /* 代码装载段可读可执行，4|1 得到权限数值 5 */
    rodata PT_LOAD FLAGS(4);  /* 只读常量装载段仅允许读，权限数值 4 */
    data PT_LOAD FLAGS(6);  /* 可写数据装载段允许读写，4|2 得到 6 */
}
SECTIONS {  /* 给输入节安排虚拟地址及其归属装载段 */
    . = 0xffffffff80000000;  /* 位置计数器 . 从固定的内核高地址起点开始 */
    __kernel_start = .;  /* 导出此处地址标签，适配层用它计算虚拟地址跨度 */
    .limine_requests : {  /* 建立一个输出请求节，内部按起点、请求、终点顺序收集 */
        KEEP(*(.limine_requests_start))  /* 保留所有输入对象中的请求起始标记节 */
        KEEP(*(.limine_requests))  /* 保留所有输入对象中的实际协议请求节 */
        KEEP(*(.limine_requests_end))  /* 保留所有请求结束标记，防止构建时删除 */
    } :requests  /* 将刚构成的输出请求节放入 requests 装载段 */
    . = ALIGN(4096);  /* 将下一位置向上补齐至 4096 字节边界，分开不同权限页 */
    .text : { *(.text .text.*) } :text  /* 收集 .text 及其子节，全部放入可执行 text 段 */
    . = ALIGN(4096);  /* 将下一位置向上补齐至 4096 字节边界，分开不同权限页 */
    .rodata : { *(.rodata .rodata.*) } :rodata  /* 收集只读数据及子节，放入 rodata 段 */
    . = ALIGN(4096);  /* 将下一位置向上补齐至 4096 字节边界，分开不同权限页 */
    .data : { *(.data .data.*) } :data  /* 收集有初值的可写数据，放入 data 段 */
    .bss : { *(.bss .bss.*) *(COMMON) } :data  /* 收集零初始化与 COMMON 符号，在 data 段保留内存空间 */
    . = ALIGN(4096);  /* 将下一位置向上补齐至 4096 字节边界，分开不同权限页 */
    __kernel_end = .;  /* 导出包含 BSS 及尾部页对齐的右端地址标签 */
    /DISCARD/ : { *(.eh_frame*) *(.note*) }  /* 丢弃此内核不消费的展开/说明元数据，不参与入口执行 */
}
