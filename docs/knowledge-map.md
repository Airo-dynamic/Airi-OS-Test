# AiriOS知识地图：实现、实验与未覆盖范围

“亲手实现”在这里表示教程给出完整自有参考代码，作者在QEMU或宿主模型验证过；不表示读者已经敲完或掌握。“Linux实验待实测”表示代码和操作已提供，但本轮SSH255，Linux分支尚未编译与运行。官方资料用于理解规范，不自动成为实测证据。

| 路径 | 教程中的实现 | Linux对照状态 | 当前简化 |
| --- | --- | --- | --- |
| 入口与调用 | [00–K13](../dev-log/core/README.md)：独立ELF、引导合同、串口、字节库 | [L01](../dev-log/linux/01-entry.md)普通C/_start、GDB/反汇编，待实测 | Limine引导；AiriBoot另列 |
| 异常与内存 | [K14–K17](../dev-log/core/14-exceptions.md)：IDT/TSS、页池、四级表、权限、堆 | [L02](../dev-log/linux/02-memory.md)mmap、RSS、缺页、权限，待实测 | 低1GiB页池、4KiB页、K38堆按64KiB增长至1MiB；无COW/SMP |
| 任务与进程 | [K18–K23](../dev-log/core/18-timer.md)：PIT/PIC、独立栈、ring3、int80、ELF、fork/exec/wait | [L03](../dev-log/linux/03-process.md)COW、PID、FD继承，待实测 | 单CPU、单线程、静态ELF、复制fork；无信号；K41增加x87/SSE2现场 |
| 文件与流 | [K24](../dev-log/core/24-files.md)、[K28–K29](../dev-log/core/28-pipes.md)：FD/OFD、共享偏移、RAM文件、管道与等待 | [L04](../dev-log/linux/04-storage.md)向量短I/O、EINTR、缓存与直接I/O，待实测 | 平坦8文件、24打开对象、4条64字节管道；非完整POSIX |
| 设备与恢复 | [K25–K27](../dev-log/core/25-block-device.md)：VirtIO DMA、FLUSH、有界超时、CRC提交日志与恢复 | [L04](../dev-log/linux/04-storage.md)预分配、文件/目录同步、替换，待实测 | legacy PCI、单在途；K40增加IRQ提示、used收集与PIT后备；模型故障和QEMU进程重启不等于物理掉电 |
| 等待与通知 | [K29](../dev-log/core/29-wait-queues.md)：保存请求、阻塞、唤醒、有限条件等待 | [L05](../dev-log/linux/05-threads.md)条件变量/futex/原子、[L06](../dev-log/linux/06-epoll.md)LT/ET/TCP背压，待实测 | guest不实现futex、pthread、socket；单CPU内核不可抢占 |
| 提交与完成 | [K30–K32](../dev-log/core/30-block-begin-poll.md)：begin/poll、SQ/CQ、cookie、handle、epoch、退出晚完成 | [L07](../dev-log/linux/07-uring.md)liburing、部分提交、短读、错误CQ，待实测 | 注册buffer是内核副本，非Linux ABI或零拷贝固定页 |
| 完整工作流 | [K33](../dev-log/core/33-shell-pipeline.md)：真实用户shell、页/FD/块请求、等待切换、继续和回收 | [L08](../dev-log/linux/08-comparison.md)相同TCP回显的三种等待路径，待实测 | UART独占，无参数/重定向/作业控制；不发布吞吐排名 |

自有用户libc支持头文件明确列出的字节函数和系统调用包装，错误直接返回负值，原始包装不含errno；K44另有显式链接的errno及POSIX文件子集，仍无stdio/locale/malloc；内核基础库与用户库分开链接。[用户头文件](../labs/airios/include/airi_libc.h)是当前支持范围，不能据“libc”名称推定完整C标准库。

图形支线已提供绘图、输入、主题、控件与窗口参考，但窗口不是独立进程，参考Files/Tasks不是guest持久文件或调度数据。图形TTY、IPC和共享表面属于后续接入；无屏主线不受其进度影响。

尚未实现或实测覆盖的部分包括完整TCP/IP、页缓存/回写实现、信号、SMP、抢占内核、AVX/XSAVE上下文、futex、多线程内存模型在真实Linux上的观察、IOMMU/通用DMA分配、通用共享设备IRQ与IOAPIC/MSI-X、完整文件系统和物理掉电持久性。第二架构和开发板仍未选定；现有arch/驱动边界减少耦合，不自动完成移植。ss、vmstat、iostat、perf尚无本轮实际输出，不能因课程提到名称就记为掌握或验证。

[验证记录](verification.md)记录作者实际结果，[系统实践地图](system-practice.md)把每项机制接回BOSS、Taishan与旧网络库的问题。

P7 已亲手实现的新增路径：[K34](../dev-log/core/34-orphan-reaper.md)孤儿托管、[K35](../dev-log/core/35-user-page-release.md)整页归还与空洞复用、[K36](../dev-log/core/36-init-supervisor.md)常驻用户init、单一UART消费身份与阻塞重启、[K37](../dev-log/core/37-user-capacity.md)按需页记录、大映像与跨页用户栈，以及[K38](../dev-log/core/38-kernel-heap-growth.md)堆扩展和缓存/活对象核对、[K39](../dev-log/core/39-service-boot.md)初始化与诊断分离、自动init与坏初始映像恢复、[K40](../dev-log/core/40-block-interrupts.md)PCI/PIC设备提示与普通used收集。真实板、网络、线程与C++ runtime仍按[部署路线](deployment-roadmap.md)继续，已提供解释不代表读者已掌握。

P8已提供[K41](../dev-log/core/41-user-fpu-state.md)用户x87/SSE2状态：每次陷入保存、返回恢复、fork继承、exec重建、GP隔离。[K42](../dev-log/core/42-user-fs-base.md)已提供用户FS基址与硬件寻址隔离，[K43](../dev-log/core/43-static-tls.md)提供静态ELF TLS模板与C11变量，[K44](../dev-log/core/44-posix-errno.md)再提供显式用户errno和文件源码包装，[K45](../dev-log/core/45-positioned-vectors.md)提供显式偏移向量复制与前缀检查，[K46](../dev-log/core/46-signed-seek.md)补齐有符号定位，用户堆由K48接入，线程、futex与C++ runtime继续追加；不是完整POSIX或AVX环境。

[K47](../dev-log/core/47-monotonic-clock.md)提供受检查的名义单调查询：PIT量化10ms，真实sleep前后比较；没有可信日期或独立高精度时钟源。

[K48](../dev-log/core/48-user-malloc.md)亲手实现用户对象包装：页面缓存与活请求分别统计，零长度策略、calloc溢出与每次清零、realloc峰值失败和fork/exec状态；当前固定256KiB且单线程，不等于生产分配器。

[K49](../dev-log/core/49-user-memory-owner.md)亲手区分用户内存与执行状态：小描述转移不复制页面，fork仍深复制，exec保留进程资源并在切根后回收旧页；共享线程和最后引用退出规则尚待实现。

[K50](../dev-log/core/50-user-execution.md)亲手归属执行现场与阻塞状态：页表归进程，CPU现场与内核栈归执行者，返回时二者必须匹配；销毁仍要求先离开目标栈。

[K51](../dev-log/core/51-tls-zero-tail.md)亲手区分TLS初值、运行总长与普通LOAD长度；真实LLD样本和宿主损坏输入分别验证合法零尾部与缺失初值。

[K52](../dev-log/core/52-shared-executions.md)亲手实现真正同进程共享执行：TID与PID、TLS初值和FPU环境继承、独立陷入栈、join与延后回收、C11发布、容量/页不足恢复和整组退出。已实测注册buffer共享，未计为多消费者SQ/CQ压力；互斥/futex及POSIX运行库继续后续。

[K53](../dev-log/core/53-private-futex.md)亲手实现比较到等待的不可分割边界、私有进程/VA键、显式通知与截止、无等待信用和整组退出清理；C11发布与裸通知职责分开。互斥、条件变量、线程安全分配仍待后续。

[K54](../dev-log/core/54-owned-mutex.md)亲手组合用户锁：CAS失败观察值交给WAIT，通知后重新争取，release/acquire保护两项普通状态，重试不延长截止；核对非持有者、自重入与非robust死亡，以及原子指令和C++头兼容的证据范围。

[K55](../dev-log/core/55-shared-malloc.md)亲手区分共享分配元数据和应用对象所有权：外层一次锁、内部复合操作、错误出口释放、一致但不持续冻结的快照；四执行者96轮及四次跨执行者归还，普通和观察编译的证据分开。
