# Linux对照：把内核机制接回实际开发

Linux实验与内核实现共同构成主线。自制内核让机制可见，Linux实验提供实际工作中的接口、错误与观察工具；两者结论分别标明环境。

当前已提供[L01 · 程序入口、函数调用与系统调用](01-entry.md)，从K13继续，无图形前置。`./airios linux-lab`在个人x86-64 Linux主机的独立目录构建两个程序，`./airios kernel-debug`用同一主机的GDB观察本机QEMU。远端需已有SSH访问与编译、ELF检查和调试工具；具体要求及输出路径在L01相应操作前说明。

2026-10-15的作者运行在SSH阶段返回255，本版尚无远端程序与GDB停点证据。P1旧记录的连接失败可能只在终端；当前执行器在连接前即记录verified=false，连接失败保存connection.log；已进入实验或调试会话后的输出才会写入对应记录；判断本次结果时还要区分复用目录里的旧文件。当前状态见[验证说明](../../docs/verification.md)。

[L02 · 虚拟范围、驻留和权限](02-memory.md)已提供完整mmap/mprotect/故障子进程程序与GDB操作；`./airios linux-lab --phase p2`选择独立目录，本轮仍因SSH255未验证Linux运行。后续与内核阶段同期提供完整程序、命令和解释，范围见[系统实践地图](../../docs/system-practice.md)：内存与缺页、文件和向量I/O、同步与原子替换、线程等待、epoll与io_uring。当前已提供L01、L02与L03的实现，不把这些计划实验列为已交付。

[L03 · 进程替换、COW与FD继承](03-process.md)从K23继续，观察同一PID换映像、共享打开文件偏移及CLOEXEC；本轮远端仍为SSH255未验证。

[L04 · 向量I/O、文件缓存、直接I/O与同步替换](04-storage.md)从K27继续。Linux分支、strace和FS能力本轮仍因SSH255未验证；Mac只编译拒绝分支，不能替代Linux编译。
