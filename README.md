# AiriOS

AiriOS 从自己的 x86-64 内核进入 Linux 存储与 I/O 机制，服务于理解和开发 BOSS、Taishan 这类系统。教程从空目录逐步写出内核、构建、验证与平台适配。内核和 Linux 对照是必修主线，图形工作站在独立的 gui 分支中推进。

从[教程入口](dev-log/README.md)继续。已到 day2 可以接 [03](dev-log/core/03-bytes.md)，00–02 的实现字节保持不变。K14–K17 完成异常与内存，K18–K23 完成用户进程，K24–K28 完成文件、管道、VirtIO 与日志，K29–K32 完成阻塞等待和有界异步 I/O；[K33](dev-log/core/33-shell-pipeline.md) 用真实用户态 shell 串起分配、文件、任务切换、设备完成与继续运行。L01–L08 提供同期 Linux 对照，当前个人主机 SSH255，Linux 实际运行仍未验证。

## 默认从串口运行

以下命令从 main 的完整参考工程执行；自己的带敲目录到 K13 时提供相同入口。

```sh
./airios doctor
./airios build
./airios run
./airios test
```

run 把 QEMU 的 UART 接到当前终端，可以输入 help、mem、mem map、devices、log、echo、programs 和 run hello/run processes/run files/run pipes/run waits/run async/run storage/run recover。输入 run sh 进入用户态 shell，再输入 programs、demo、storage、recover、exit。按 Ctrl-A 后按 X 退出 QEMU。test 运行八组 ASan/UBSan 主机检查、九种带超时启动、三条 UART/PS/2 输入路径、一组真实用户 shell 会话、十二代常驻 init 服务与三次存储/I/O 会话启动（含跨 QEMU 恢复和真实限速）。

主机需要 Python 3.12+、LLVM/LLD、GNU Make、QEMU、xorriso 和 mtools。macOS 可执行 `brew install python llvm lld qemu xorriso mtools make`。虚拟机固定为 q35、单 CPU、256 MiB 和 OVMF UEFI，Apple Silicon 使用 TCG。工具选择、外部来源与依赖校验由教程 00–02 展开。 Apple Silicon 上若 `python3 --version` 仍显示系统旧版，可先执行 `export PATH="/opt/homebrew/bin:$PATH"`，再运行上述命令。

## 两条分支各自推进

| 分支 | 检查点 | 内容与运行条件 |
| --- | --- | --- |
| main | 64 个：00–10、K11–K52、L01–L08、T01–T03 | 串口内核、必修 Linux 对照和作者工具；不包含图形课程、前端源码或图形资产 |
| gui | 15 个：G11–G24、G26 | 从 main 的 00 根提交分叉，只增加图形自己的路径；合入任一现有 main 阶段均不覆盖主线文件 |

合入源码与开始运行是两件事。图形代码可以先合入较早的 main 检查点，运行需主线到 K11；G24 的 guest 接入需 K13。G11–G23 先提供宿主观察，G26 提供完整图形检查。默认构建继续选择串口实现，不读取字体，也不链接 Cocoa。

在本仓库的参考main历史中，合入GUI使用下面两条命令。自己的空目录带敲工程继续按G11正文写入图形代码；这组参考Git命令要求共享本仓库的00祖先。

```sh
git fetch origin gui # 取回gui及其共同的00祖先。
git merge origin/gui # 只加入支线拥有的图形实现与资料。
```

[图形课程入口](https://github.com/Airo-dynamic/Airi-OS-Test/blob/gui/dev-log/graphics/README.md)和[视觉设计](https://github.com/Airo-dynamic/Airi-OS-Test/blob/gui/docs/design.md)位于 gui 分支。图形可暂停在任一检查点，继续内核无需推进它。G24 提供 guest 接入与资产校验后可以选择图形运行，G26 再提供统一的预览与图形检查工具：

```sh
./airios run --graphics # 完成 G24 后运行 guest 图形。
./airios ui-preview # 完成 G26 后打开宿主参考窗口。
./airios ui-test # 完成 G26 后检查图形模型与宿主画面。
./airios test --graphics # 完成 G26 后执行完整图形回归。
```

ui-preview 是 macOS 原生参考窗口。Files/Editor 使用参考进程的会话内存文件，退出后重置；Run 报告执行服务未接入。guest 图形读取真实启动信息和文本控制台，F1–F4 切换视图，theme day/theme night 由图形消费者处理。串口命令没有主题功能。图形 Terminal 当前没有用户 TTY；run sh 会提示从串口入口运行，避免窗口等待未接入的输入。

## Linux 对照与检查点导出

```sh
./airios linux-lab --phase p5-threads # p1/p2/p3/p4/p5-threads/p5-epoll/p5-uring/p6分别保存证据。
./airios kernel-debug
./airios learn list
./airios learn export K13 /tmp/airios-core-reference
./airios snapshots
./airios docs-check
```

Linux/GDB 默认只连接个人主机 airi@ssh.airi-dynamic.top。运行证据与连接限制见[验证记录](docs/verification.md)。learn 从参考工程执行，默认只读 main 当前发布的检查点；导出以教程和明确前置关系重建源码，要求目标目录尚不存在。

合并 gui 后，工具通过独立的 `.codex/teaching/graphics.json` 增加图形节点。此时可以列出图形课程，或把任一主线进度与图形停点导出到新目录：

```sh
./airios learn list --track graphics
./airios learn export K13 /tmp/airios-core-with-g13 --alongside G13
```

组合只应用一次共享基础，图形停点不会把主线文件退回旧版。snapshots 在 HISTORY 中生成独立检查点目录；有图形清单时也生成包含所需主线基础的图形快照。已有导出目录不覆盖，HISTORY 中被编辑的快照会阻止重新生成。个人学习位置单独记录，不推断理解程度。

[架构](docs/architecture.md)解释服务、文本与显示的边界，[系统实践地图](docs/system-practice.md)连接项目场景，[路线](docs/roadmap.md)列出已实现阶段和扩展边界，[知识地图](docs/knowledge-map.md)区分实现、待实测实验与尚未覆盖内容。[AiriBoot](boot/AiriBoot/README.md)是独立的 UEFI 引导子项目。原始代码采用 MIT 许可，外部来源见 [THIRD_PARTY](THIRD_PARTY.md)。

P2 带敲入口：[K14 异常](dev-log/core/14-exceptions.md) → [K15 物理页](dev-log/core/15-physical-pages.md) → [K16 页表与权限](dev-log/core/16-virtual-memory.md) → [K17 内核堆](dev-log/core/17-kernel-heap.md) → [L02 Linux 内存](dev-log/linux/02-memory.md)。`./airios linux-lab --phase p2` 保存本轮实际连接与运行状态，连接失败不记为 Linux 验证成功。

P7 升级从 [K34 孤儿托管与持续回收](dev-log/core/34-orphan-reaper.md) 接到 [K35 用户页归还与复用](dev-log/core/35-user-page-release.md)。最终部署目标与自有网络、Server-Lib、Cloudflare 和多核阶段见[部署路线](docs/deployment-roadmap.md)。

[K36 常驻 init](dev-log/core/36-init-supervisor.md)提供 `run init`：用户 shell 退出后等待 100tick 再启动下一代；`run sh` 仍在退出后返回监控台。

[K37 用户容量](dev-log/core/37-user-capacity.md)提供 `capacity` 程序：每进程上限4MiB、64KiB初始用户栈，四轮耗尽/归还并在满预算上fork与exec。页归属记录按需分配，默认构建继续独立于图形。

[K38 内核堆扩展](dev-log/core/38-kernel-heap-growth.md)将64KiB起步的对象堆按批扩展到1MiB上限，保留活对象，并区分缓存页面与未归还对象。

[K39 服务启动](dev-log/core/39-service-boot.md)提供 `./airios run --profile service`，开机直接进入用户init与UART shell；诊断演示仍由默认normal运行，坏初始映像可退回恢复监控台。

[K40 块设备中断](dev-log/core/40-block-interrupts.md)接通q35 PCI/PIC到IRQ43，普通服务仍核对used完成；`block-irq-masked`保留真实DMA并验证PIT后备收集。

[K41 用户浮点状态](dev-log/core/41-user-fpu-state.md)允许普通C使用SSE2，独立保存x87/XMM与控制环境，实际验证抢占、fork、失败/成功exec及对齐故障隔离；P8从此继续。

[K42 用户FS基址](dev-log/core/42-user-fs-base.md)将实际FS寻址接入统一用户保存与恢复，验证不同基址的抢占、fork/exec及释放后缺页。

[K43 C11线程本地变量](dev-log/core/43-static-tls.md)从静态ELF模板创建用户TLS副本，验证负偏移、初值/零尾部、fork继承、exec重建及超限拒绝。

[K44 errno与文件包装](dev-log/core/44-posix-errno.md)提供显式链接的用户POSIX源码子集，保留短操作、EOF和原始ABI，验证本地错误跨fork/exec的生命周期。

[K45 显式偏移向量I/O](dev-log/core/45-positioned-vectors.md)先复制描述与核对实际前缀，再以一次文件操作聚合/分散；保留短返回、共享偏移和坏后项无副作用。

[K46 有符号定位](dev-log/core/46-signed-seek.md)在共享打开对象中一次完成SET/CUR/END，验证负差值、极值失败保持和写入后的零洞。

[K47 名义单调时钟](dev-log/core/47-monotonic-clock.md)通过受检查的clock_gettime输出PIT时间，验证真实阻塞窗口并明确10ms量化与墙钟边界。

[K48 用户对象分配](dev-log/core/48-user-malloc.md)在独立用户arena中提供malloc/free/calloc/realloc，验证原始页不足后的重试、复用清零、扩容失败保持和fork/exec生命周期。

[K49 用户内存所有权](dev-log/core/49-user-memory-owner.md)将页表与页面记录单独持有，exec只转移新旧内存，保持内核栈、FD与进程身份，切根后再回收旧页。

[K50 用户执行现场](dev-log/core/50-user-execution.md)将CPU状态、陷入栈与等待条件集中到execution，调度同时选择内存与现场，并保持离开栈后再销毁的顺序。

[K51 TLS零尾部](dev-log/core/51-tls-zero-tail.md)以较短普通BSS复现真实LLD布局，修正过严的TLS总长检查，同时保持完整文件初值、孤立模板与页预算校验。

[K52 同进程工作执行者](dev-log/core/52-shared-executions.md)共享用户内存、FD与异步buffer，独立保存栈、TLS和CPU环境，提供阻塞join并验证容量、复用和整个进程的退出故障。

[K53 私有内存等待](dev-log/core/53-private-futex.md)加入条件比较与登记、显式有界唤醒、PIT超时与进程/VA私有键，并把真实程序目录分段输出。
