# 下一阶段：从时钟中断到任务与用户程序

P2已经由[K14](14-exceptions.md)、[K15](15-physical-pages.md)、[K16](16-virtual-memory.md)、[K17](17-kernel-heap.md)完成异常、物理页、四级页表和内核堆。继续读[L02](../linux/02-memory.md)区分Linux虚拟范围与实际驻留；当前个人主机SSH255，远端运行未验证，不阻止继续内核。

P3按时钟IRQ、任务执行、用户态与系统调用、静态ELF和最小fork/exec/wait逐步推进。每个检查点继续串口验证，图形不成为前置。用户页权限与非法指针检查必须在ring3程序接入时一并实现，不能拿K16的supervisor演示冒充进程隔离。
