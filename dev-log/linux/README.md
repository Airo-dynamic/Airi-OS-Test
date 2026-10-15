# Linux对照：把内核机制接回实际开发

Linux实验与自制内核共同构成主线。以下各章提供完整源码、执行脚本、观察命令和错误出口，没有图形前置。每个阶段在个人x86-64 Linux主机airi@ssh.airi-dynamic.top的AiriOS-labs下使用独立目录。

| 章节 | 观察的问题 | 命令phase |
| --- | --- | --- |
| [L01 · 程序入口](01-entry.md) | 普通C与无libc、ELF、_start、寄存器与退出 | p1 |
| [L02 · 虚拟与驻留](02-memory.md) | mmap、触页、RSS、mprotect与故障 | p2 |
| [L03 · 进程替换](03-process.md) | COW、fork/exec/wait、FD与CLOEXEC | p3 |
| [L04 · 存储与同步](04-storage.md) | 向量短I/O、页缓存、直接I/O、预分配、同步替换 | p4 |
| [L05 · 线程和futex](05-threads.md) | 有界队列、条件谓词、原子发布与比较入睡 | p5-threads |
| [L06 · TCP就绪](06-epoll.md) | epoll LT/ET、排空、背压与半关闭 | p5-epoll |
| [L07 · io_uring完成](07-uring.md) | 部分提交、cookie、短读、错误CQ与缓冲寿命 | p5-uring |
| [L08 · 同负载对照](08-comparison.md) | 同一TCP回显的同步、epoll与io_uring等待位置 | p6 |

例如执行 `./airios linux-lab --phase p5-epoll`。GDB与内核停点操作在L01–L03，`./airios kernel-debug`使用同一主机观察本机QEMU。各章给出具体依赖包名；缺库、权限拒绝和超时都不发布成功。

2026-10-15至16的本轮个人SSH均返回255，错误为Connection closed by UNKNOWN port 65535。只读检查确认连接经cloudflared access ssh代理，未修改认证、网络或系统策略。Linux分支编译、程序、GDB/strace及文件系统能力尚未验证。Mac对环境拒绝分支的编译与返回77不能替代Linux运行，guest通过也不能替代Linux。每阶段remote.json在连接前撤销旧成功，connection.log保存实际失败；判断新运行时还需区分旧目录残留文件。

[验证记录](../../docs/verification.md)、[系统实践地图](../../docs/system-practice.md)与[知识地图](../../docs/knowledge-map.md)分别说明实际证据、项目问题和未覆盖范围。
