# AiriOS知识地图：实现、实验与未覆盖范围

“亲手实现”在这里表示教程给出完整自有参考代码，作者在QEMU或宿主模型验证过；不表示读者已经敲完或掌握。“Linux实验待实测”表示代码和操作已提供，但本轮SSH255，Linux分支尚未编译与运行。官方资料用于理解规范，不自动成为实测证据。

| 路径 | 教程中的实现 | Linux对照状态 | 当前简化 |
| --- | --- | --- | --- |
| 入口与调用 | [00–K13](../dev-log/core/README.md)：独立ELF、引导合同、串口、字节库 | [L01](../dev-log/linux/01-entry.md)普通C/_start、GDB/反汇编，待实测 | Limine引导；AiriBoot另列 |
| 异常与内存 | [K14–K17](../dev-log/core/14-exceptions.md)：IDT/TSS、页池、四级表、权限、堆 | [L02](../dev-log/linux/02-memory.md)mmap、RSS、缺页、权限，待实测 | 低1GiB页池、4KiB页、64KiB堆；无COW/SMP |
| 任务与进程 | [K18–K23](../dev-log/core/18-timer.md)：PIT/PIC、独立栈、ring3、int80、ELF、fork/exec/wait | [L03](../dev-log/linux/03-process.md)COW、PID、FD继承，待实测 | 单CPU、单线程、静态ELF、复制fork；无信号/FPU现场 |
| 文件与流 | [K24](../dev-log/core/24-files.md)、[K28–K29](../dev-log/core/28-pipes.md)：FD/OFD、共享偏移、RAM文件、管道与等待 | [L04](../dev-log/linux/04-storage.md)向量短I/O、EINTR、缓存与直接I/O，待实测 | 平坦8文件、24打开对象、4条64字节管道；非完整POSIX |
| 设备与恢复 | [K25–K27](../dev-log/core/25-block-device.md)：VirtIO DMA、FLUSH、有界超时、CRC提交日志与恢复 | [L04](../dev-log/linux/04-storage.md)预分配、文件/目录同步、替换，待实测 | legacy PCI、单在途、轮询；模型故障和QEMU进程重启不等于物理掉电 |
| 等待与通知 | [K29](../dev-log/core/29-wait-queues.md)：保存请求、阻塞、唤醒、有限条件等待 | [L05](../dev-log/linux/05-threads.md)条件变量/futex/原子、[L06](../dev-log/linux/06-epoll.md)LT/ET/TCP背压，待实测 | guest不实现futex、pthread、socket；单CPU内核不可抢占 |
| 提交与完成 | [K30–K32](../dev-log/core/30-block-begin-poll.md)：begin/poll、SQ/CQ、cookie、handle、epoch、退出晚完成 | [L07](../dev-log/linux/07-uring.md)liburing、部分提交、短读、错误CQ，待实测 | 注册buffer是内核副本，非Linux ABI或零拷贝固定页 |
| 完整工作流 | [K33](../dev-log/core/33-shell-pipeline.md)：真实用户shell、页/FD/块请求、等待切换、继续和回收 | [L08](../dev-log/linux/08-comparison.md)相同TCP回显的三种等待路径，待实测 | UART独占，无参数/重定向/作业控制；不发布吞吐排名 |

自有用户libc支持头文件明确列出的字节函数和系统调用包装，错误直接返回负值，没有完整errno/stdio/locale/malloc语义；内核基础库与用户库分开链接。[用户头文件](../labs/airios/include/airi_libc.h)是当前支持范围，不能据“libc”名称推定完整C标准库。

图形支线已提供绘图、输入、主题、控件与窗口参考，但窗口不是独立进程，参考Files/Tasks不是guest持久文件或调度数据。图形TTY、IPC和共享表面属于后续接入；无屏主线不受其进度影响。

尚未实现或实测覆盖的部分包括完整TCP/IP、页缓存/回写实现、信号、SMP、抢占内核、FPU/SIMD上下文、futex、多线程内存模型在真实Linux上的观察、IOMMU/通用DMA分配、设备中断完成、完整文件系统和物理掉电持久性。第二架构和开发板仍未选定；现有arch/驱动边界减少耦合，不自动完成移植。ss、vmstat、iostat、perf尚无本轮实际输出，不能因课程提到名称就记为掌握或验证。

[验证记录](verification.md)记录作者实际结果，[系统实践地图](system-practice.md)把每项机制接回BOSS、Taishan与旧网络库的问题。

P7 已亲手实现的新增路径：[K34](../dev-log/core/34-orphan-reaper.md)孤儿托管、[K35](../dev-log/core/35-user-page-release.md)整页归还与空洞复用、[K36](../dev-log/core/36-init-supervisor.md)常驻用户init、单一UART消费身份与阻塞重启、[K37](../dev-log/core/37-user-capacity.md)按需页记录、大映像与跨页用户栈，以及[K38](../dev-log/core/38-kernel-heap-growth.md)堆扩展和缓存/活对象核对、[K39](../dev-log/core/39-service-boot.md)初始化与诊断分离、自动init与坏初始映像恢复、[K40](../dev-log/core/40-block-interrupts.md)PCI/PIC设备提示与普通used收集。真实板、网络、线程与C++ runtime仍按[部署路线](deployment-roadmap.md)继续，已提供解释不代表读者已掌握。
