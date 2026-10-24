#ifndef AIRI_USER_LIMITS_H // 装载器与进程页记录使用同一预算。
#define AIRI_USER_LIMITS_H // 避免重复宏定义。
#define AIRI_USER_PAGES 1024u // 每进程最多4MiB用户数据；页表、元数据和内核栈另计。
#define AIRI_USER_STACK_PAGES 16u // 静态ELF初始栈64KiB，下面保持未映射保护页。
#define AIRI_USER_REQUEST_PAGES 64u // 一次页面申请或归还最多256KiB，内核操作保持有界。
#endif // 结束预算声明。
