# 当前路线的验证

P4内核已推进到K28：七组ASan/UBSan主机观察、九种启动、三条实际UART/PS2输入和双QEMU恢复全部通过。VirtIO验证真实扇区读写、FLUSH、只读写拒绝、缺设备及漏发布索引后的真实PIT超时；日志验证真实位损坏恢复与16条明确缓存模型故障；管道验证短写、FIFO回绕、EOF、EPIPE及父子端点引用。正常启动只扫描正式日志，不自动追加。K24–K27各自正文导出后编译、启动，K27另外从导出源码跑双启动恢复。

L04本轮仍为SSH255。Linux分支编译、strace、O_DIRECT与文件系统能力均未验证；Mac只编译环境拒绝分支并返回77。当前尚无阻塞管道、就绪订阅、异步提交/完成队列或用户shell；后续P5–P6继续。图形回归按独立合并记录，不把宿主参考Files当持久文件系统。

## P3与P4初始历史验证

2026-10-15继续完成K18–K23、L03和K24。P3内核已实际运行时钟IRQ、独立栈任务、ring3、用户指针拒绝、静态ELF、自有用户库、真实用户抢占、复制fork、事务exec和阻塞wait；K24验证RAM文件短读写、零洞、共享偏移、CLOEXEC和对象回收。`./airios test`通过六组ASan/UBSan主机检查、六种启动配置和三条真实输入；K24正文在新目录独立重建并启动。

P3的隔离GUI合并通过六种图形启动、三条输入、84张宿主快照和58项对比，实际查看Day/Night guest画面，现有设计保留。完整日志及截图在artifacts/p3-graphics-regression，图形分支未修改。K24不改变呈现接口，其新增文件路径按串口验证。

L03程序在Mac严格编译、Shell语法检查通过，非Linux运行返回77；本轮个人主机SSH255，Linux COW、exec/FD与GDB捕捉点仍未验证。当前磁盘、管道、异步队列和完整串口用户shell尚未完成。各检查点源哈希和证据在`.codex/teaching/validation/`，运行或覆盖通过不表示读者已经掌握。

## P2历史验证

2026-10-15新增K14–K17与L02。P2内核部分已在固定QEMU中运行：自有GDT/IDT/TSS、UD/GP恢复和未处理故障停止、usable物理页池、自有四级页表和CR3切换、WP/NX/未映射三种真实PF、64KiB小对象堆。物理页、映射和对象所有权分别核对，临时映射/对象演示结束后统计恢复。

`./airios test`通过四组ASan/UBSan主机观察、五种带超时启动（含trap-fatal）和三条真实UART/PS2输入。K14–K17各自从正文在新目录重建、编译并启动，主线源码与23节点清单一致；新增教程旁注检查通过99%阈值，正文另作代码伴读与连续阅读。它们不证明读者已掌握。

在隔离目录把gui合入K17，`test --graphics`通过五种启动和三条输入；`ui-test`通过84张宿主快照、58项对比及Day/Night状态与显示哨兵检查。实际查看guest夜间截图，原有视觉布局保留，Terminal显示真实page pool与heap数值。主线未加入任何frontend、字体或Cocoa，图形分支未改写。

L02源在Mac以C11及严格警告编译通过，Mac运行返回77，Shell语法通过；个人主机SSH返回255，connection.log记录`Connection closed by UNKNOWN port 65535`，remote.json明确verified=false。Linux RSS、SIGSEGV和GDB停点均未验证。没有使用公司机替代，也不拿本地编译作为Linux结果。

该P2快照当时尚无用户态、进程、文件系统、磁盘或异步I/O；后续进展以上文为准。证据见`.codex/teaching/validation/K14.json`至`K17.json`和`L02.json`；本轮原始日志在artifacts与各隔离重建目录。图形回归摘要和Day/Night截图保存在artifacts/p2-graphics-regression。

## P1与课程分流的历史验证

本轮主线18个检查点、GUI支线15个检查点分别重建。独立代理只根据教程和声明资料，从空目录完成33个检查点的本地操作；92份自有源码与正文逐字核对。00–02代码围栏保持原字节。该记录对应迁移时的P1快照，当前新增结果以上文为准。

串口与图形完整测试分别通过四种带超时启动及三条真实UART/PS2输入路径。基础库、文本模型、图形和显示适配通过ASan/UBSan；原生程序生成84张双主题快照，58项颜色组合通过。真实Cocoa键鼠还核对了编辑、保存、运行失败反馈和切换主题保留未保存文档。参考文件与任务服务属于宿主进程内存，不能作为guest文件系统或调度证据。

main与gui的270种已发布阶段组合保留主线文件；12个已有内核构建入口的主线阶段合入GUI后默认构建通过。GUI仅增加自己的路径，实际运行仍遵守K11/K13前置。局部注释整理有组长、空白和宽度上限，不折行；覆盖与幂等检查通过后仍做了代码伴读和正文阅读。

Linux/GDB两次实际连接均返回SSH255，本轮仍未验证远端程序与停点。Debian/Ubuntu、Arch/CachyOS及第二架构未实测。环境是macOS Apple Silicon、LLVM/LLD、QEMU TCG、q35、单CPU、256MiB和锁定UEFI固件。

[作者记录](../.codex/teaching/validation/route-rebuild.md)说明逐节点来源、验收器诊断失败与恢复、实际运行和工具边界。工具成功、源码相同和代理复敲不证明真实读者已经理解。
