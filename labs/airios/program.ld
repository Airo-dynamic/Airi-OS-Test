OUTPUT_FORMAT(elf64-x86-64) /* 生成独立静态用户ELF。 */
ENTRY(_start) /* 用户入口由自己的start.S提供。 */
PHDRS { /* 各权限段单独装载，避免共页权限冲突。 */
    text PT_LOAD FLAGS(5); /* 读、执行，不写。 */
    rodata PT_LOAD FLAGS(4); /* 只读常量。 */
    tls PT_TLS FLAGS(4); /* 静态本地变量的只读模板描述，普通LOAD仍承载模板字节。 */
    data PT_LOAD FLAGS(6); /* 读写数据和零填充，不执行。 */
}
SECTIONS { /* 固定低地址，当前不支持PIE重定位。 */
    . = 0x400000; /* 与用户装载范围一致的低半区。 */
    .text : { *(.text .text.*) } :text /* 用户入口和函数代码。 */
    . = ALIGN(4096); /* 下一权限段从新页开始。 */
    .rodata : { *(.rodata .rodata.*) } :rodata /* 文字与只读常量。 */
    . = ALIGN(4096); /* 不让常量与可写数据共页。 */
    .data : { *(.data .data.*) } :data /* 文件提供的初始化字节。 */
    . = ALIGN(64); /* 初值模板起点与当前支持的普通静态布局一致。 */
    .tdata : { *(.tdata .tdata.*) } :data :tls /* 带初值的线程本地模板。 */
    .tbss : { *(.tbss .tbss.*) } :data :tls /* 零填充TLS紧随初值，运行时单独复制。 */
    .bss : { *(.bss .bss.*) *(COMMON) } :data /* 只增加内存长度，装载器必须清零。 */
    /DISCARD/ : { *(.eh_frame*) *(.note*) } /* 本节不提供异常展开或辅助note消费。 */
}
