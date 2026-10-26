# 全教程解释性重构 TODO 与续点

本轮从已认可的五篇试点继续，逐篇处理当前 main 工作区的 45 个检查点，以及 14 份入口、路线和实验说明（含遍历时发现的 AiriBoot 独立计划）。所有正文修改保持未提交；既有试点与作者工具改动保留。源码和命令以本轮开始状态冻结，原始 Git 基线仍为 4c7aafa。

对照目录：`/Users/bilibili/Desktop/AiriOS 全教程重构对照 2026-10-24`。GUI 独立分支不在当前工作区。

## 执行规则

逐篇读取画像、实际前文和检查点新增实现；按读者第一次缺失的关系重组全部可见正文，不做统一模板追加。每篇完成后回看 writing 与对应连续范例，复查信息保全和累计 diff，再继续。成熟列表、代码、旁注、锚点和操作条件保留；仅凭重建或字数不判定可读。

## 章节 TODO

- [x] 00 · `dev-log/00-first-tool.md`
- [x] 01 · `dev-log/01-tools.md`
- [x] 02 · `dev-log/02-dependencies.md`
- [x] 03 · `dev-log/core/03-bytes.md`
- [x] 04 · `dev-log/core/04-format.md`
- [x] 05 · `dev-log/core/05-boot-contract.md`
- [x] 06 · `dev-log/core/06-entry.md`
- [x] 07 · `dev-log/core/07-first-boot.md`
- [x] 08 · `dev-log/core/08-real-boot-info.md`
- [x] 09 · `dev-log/core/09-console.md`
- [x] 10 · `dev-log/core/10-input.md`
- [x] K11 · `dev-log/core/11-serial-build.md`
- [x] K12 · `dev-log/core/12-boot-tests.md`
- [x] K13 · `dev-log/core/13-commands.md`
- [x] L01 · `dev-log/linux/01-entry.md`
- [x] T01 · `dev-log/tools/01-reconstruction.md`
- [x] T02 · `dev-log/tools/02-snapshots.md`
- [x] T03 · `dev-log/tools/03-check-and-progress.md`
- [x] K14 · `dev-log/core/14-exceptions.md`
- [x] K15 · `dev-log/core/15-physical-pages.md`
- [x] K16 · `dev-log/core/16-virtual-memory.md`
- [x] K17 · `dev-log/core/17-kernel-heap.md`
- [x] L02 · `dev-log/linux/02-memory.md`
- [x] K18 · `dev-log/core/18-timer.md`
- [x] K19 · `dev-log/core/19-kernel-tasks.md`
- [x] K20 · `dev-log/core/20-user-boundary.md`
- [x] K21 · `dev-log/core/21-user-elf.md`
- [x] K22 · `dev-log/core/22-processes.md`
- [x] K23 · `dev-log/core/23-exec-and-monitor.md`
- [x] L03 · `dev-log/linux/03-process.md`
- [x] K24 · `dev-log/core/24-files.md`
- [x] K25 · `dev-log/core/25-block-device.md`
- [x] K26 · `dev-log/core/26-journal.md`
- [x] K27 · `dev-log/core/27-user-storage.md`
- [x] L04 · `dev-log/linux/04-storage.md`
- [x] K28 · `dev-log/core/28-pipes.md`
- [x] K29 · `dev-log/core/29-wait-queues.md`
- [x] K30 · `dev-log/core/30-block-begin-poll.md`
- [x] K31 · `dev-log/core/31-async-queues.md`
- [x] K32 · `dev-log/core/32-user-async.md`
- [x] L05 · `dev-log/linux/05-threads.md`
- [x] L06 · `dev-log/linux/06-epoll.md`
- [x] L07 · `dev-log/linux/07-uring.md`
- [x] K33 · `dev-log/core/33-shell-pipeline.md`
- [x] L08 · `dev-log/linux/08-comparison.md`

## 入口与路线 TODO

- [x] `dev-log/README.md`
- [x] `dev-log/tools/README.md`
- [x] `dev-log/core/next.md`
- [x] `dev-log/core/README.md`
- [x] `dev-log/linux/README.md`
- [x] `docs/architecture.md`
- [x] `docs/knowledge-map.md`
- [x] `docs/system-practice.md`
- [x] `docs/verification.md`
- [x] `docs/roadmap.md`
- [x] `README.md`
- [x] `labs/README.md`
- [x] `labs/linux/p1/README.md`

- [x] `boot/AiriBoot/README.md`

## 本轮批次记录

当前续点：45 个检查点与 14 份入口资料全部完成逐篇处理；已认可五篇保留其有效组织并复查新前后文。源码保护、导航、旁注与累计 diff 交付核对已完成。

### 01

从主机观察与内核两种产物说明 CC/LD/HOST_CC 的消费关系；工具选择、参数组成与 Make 执行分别成立。保留覆盖优先级、符号链接与错误条件。

### 02

锁文件记录经字典访问成为归档、展开目标与固件输入；同一个 BOOTX64.EFI 路径贯穿缓存判定和解包，with 结束仅关闭归档句柄。

### 03

共享算法只访问借用内存，主机观察入口另借 printf；同一重叠范围的读写方向解释结果，保留 NUL、零长度、地址表示和编译选项。

### 04

按实际的 %s:%u、IO、123u 追踪两项实参；output 在入口创建，格式指针、args 与 count 各自推进，截断仍继续统计。已修正原追踪与观察不同的路径。

### 06

先给出实际运行顺序，再将 UART 参数、寄存器约束、栈准备、ELF 链接与 Make 消费接回同一标记。保留所有端口、ABI、构建选项和失败限制。

### 07

区分主机 staging、外层 ISO 与 CD 映像内部 FAT；固件读启动映像的 EFI，Limine 再取配置与内核。固定版本构建规则已核实，完整行、超时与主机回收条件保留。

### 08

memory_request 从 ELF 零指针到装入内存、引导器写回、C 读取，再进入静态自有副本。区分地图/名称复制与像素地址借用，验证后打印和分类统计保留。

### 09

同一 mem 从三次字符输入到回车分派、历史副本和输出环，再由主机显示；逻辑号与槽位、草稿与历史、状态变化与命令执行分别解释。

### 10

键盘前缀与修饰状态跨解码调用保留，UART 则直接交字节；回车产生新行后才发送和发布计数。父进程一项一等，区分主机管道与设备容量。

### K11

以同一 mem 保留串口主线，链接时选显示定义；回调函数/数据指针和命令栈副本寿命分别解释。Make 条件只读取必要清单，主机检查不冒充设备证据。

### K12

一次请求用发送前 offset 与新计数关联完成，QMP 控制和 UART 数据分别建模；正常/拒绝成功含义、JSON 分帧、端点与进程寿命完整保留。

### K13

从一次 run/test 的真实动作串起参数解析、依赖准备、Make、ELF 检查与 QEMU；编辑器数据库只是同规则消费者，保留懒导入、Shell 参数边界及 AiriBoot 失败语义。

### L01

输出一行的三种执行环境分别展开；argc 初始栈、raw 寄存器与返回地址、远端 GDB 对本地 guest 的观察逐步对应，保留证据限制和 SSH255 未验证状态。

### K14

沿 ud2 故障到 IDT、统一异常现场、C 精确登记与 iretq 恢复组织全文；静态表和局部描述符寿命分开，GP 错误码与未登记停止条件保留，既有实测不冒称本轮。

### K15

从地图不随借出变化推导 managed/allocated；同一页经历向内取整、占位、HHDM 写读、拒绝非法归还及复用。主机编号和 guest 真地址保持区别。

### K16

同一数据页贯穿 HHDM、四级别名、硬件权限故障与回收。先建立当前执行资源映射再切根；软件查表不读目标数据，NX 内部返回地址、失败回滚及共享表寿命完整保留。

### K17

十六 backing 页与小对象分别持有；80+32+128 的合并支撑 208 请求，第三项持续有效。初始化失败回滚、同步快照和 P2 发布先后贯通，保留碎片及旧指针限制。

### L02

同一 64MiB 范围历经保留、逐页触碰、首页只读、孩子故障、解除。补明 mprotect 拆 VMA 后快照范围变小的含义，保留 RSS/缺页统计边界与远端未验证。

### K18

至少五次事件连接设备、控制器和 CPU 三环节；初始化后才开 IF，EOI 后才可继续交付。保留屏蔽故障、APIC 前提、周期等待与未来防丢唤醒区别。

### 00

保留已认可完整稿，复查外层解释脚本、默认内层探测、入口异常和退出码，并接到新 01 的工具选择；实现、命令和局部语法说明保持。

### 05

保留已认可完整稿，复查其与新 04/06/08 的交接。两段地图、地址别名、验证内部统计、独立显示检查和主机观察仍完整成立。

### K19

保留已认可完整稿，复查 K18 时钟只计数与主动换栈的边界，再接用户态。六寄存器、首次栈、0→1→2→0、DONE 后离栈回收和 DF 演示完整保留。

### K20

从内核任务缺少隔离引入权限边界；同一用户字符串跨页验证、TSS 交接、所选现场和离栈回收连续展开，保留四项受控故障及 ABI 精度。

### K21

以 initialized、8192 字节 zeroed 和用户 main 串起文件描述、整份校验、用户库、初始栈及两次链接，明确 view 尚未分配页。

### K22

以 42/99 的父子数据、同一 fork 返回点、等待调用和僵尸占槽组织进程机制；当前执行者与状态写入目标分开，保留离栈后回收条件。

### K23

沿旧名称副本、局部 next、提交与原内核栈转移解释 exec，再把退出结果副本交给 console service，保留失败后继续和 PID 不变。

### L03

以同一匿名页、PID 和共享偏移解释 Linux 对照，修正真实 --child 参数与临时坏 ELF 的保留期，保留早晚 exec 失败区别及未验证状态。

### K24

以 abcdef 的共享 offset 和 /notes 失败 truncate 解释三层对象，读取先保证用户可接收，再推进偏移；FD 引用转移与页所有权保持区分。

### K25

沿最后扇区的一次 read 连接端口、DMA、链首身份、idx 发布和完成；超时返回不等于 DMA 停止，保留静态隔离、三种故障与 FLUSH 边界。

### K26

以 alpha 后追加和旧坏尾引出介质提交证据、三段同步和连续前缀；错误返回的副作用不确定性接到 poisoned，真实位损坏与 live/durable 模型分开。

### K27

由新内核无法取得旧 count 引出正式日志服务和双启动；用户副本、静态服务与 DMA 逐层交接，纠正标题将 QEMU 误称 CPU 进程，保留同步等待边界。

### L04

由分散数据的一次总返回数进入前缀消费，再区分缓存可见、驻留、预留与同步；旧 FD/新名字支撑发布顺序，保留真实 EINTR 和 FS 能力边界。

### K28

同一 child 数据流引出环中未读状态和端点存活；100 字节容量演算参与回绕，空/EAGAIN/EOF 与 HUP 按写者生命周期展开，保留原子创建和范围限制。

### K29

父 read10 与孩子 awake 贯穿登记、目标页表重试、结果先发布再唤醒；有限就绪等待和原请求完成分开，启动服务续点的栈寿命接到纯计算者超时。

### K30

由 perform 不交还控制权引出 begin/poll；同一扇区请求的状态跨调用保存，同步适配只收自己提交，日志忙拒绝发生在事务前。

### K31

保留已认可完整稿并复查 K30→K31→K32；cookie30、四份 CQ 信用、BUSY 到 pop、epoch/handle 晚完成及唯一硬件消费者保持连续，精确替换配方不变。

### K32

用户局部描述与 BUSY 数据分开；fork 整数不授予资源、exec 同 PID 换 epoch、CQ 输出先验后 pop 连续解释，限速证据按同一等待区间核对。

### L05

从检查后再睡的空窗引出条件变量与 futex；通知不持有任务、WAKE 不发布普通数据，队列锁、relaxed 统计和 join 各自支持不同关系。

### L06

以 abc 剩余 bc 区分 LT/ET，再由 send 接受前缀引出应用后缀、订阅和半关闭；epoll 条件与 CQ 操作结果分开，保留真实背压和未验证边界。

### L07

保留已认可完整稿并复查 L05/L06/K32 交接；cookie103 的六字节有效前缀、提交数量、CQ 槽与应用数组寿命以及 liburing 错误诊断限制仍完整。

### K33

沿输入 demo→父 shell 保留→孩子 exec→同一页内容经过 RAM 文件与注册副本→父恢复组织整章；UART 唯一消费者、FD0 CLOEXEC 和观察区间保留。

### L08

固定同一消息与 done 前缀后比较三个等待位置；CQ 结果不需再补读写，缓冲寿命相同，补明固定 CQ 等待与循环预算不是严格总两秒。

### T01

02 的导出目标贯穿声明、前置排序、累计 state、整章事务和独立输出；区分重复前置与循环、编辑围栏与展示、源码核对与真实命令。

### T02

由移动资料混版引出 SHA/内存固定，再分别保护新输出和 HISTORY 交接；K13 身份、分支终点、输出清单与暂存树各自的用途连续展开。

### T03

内容成立后再查导航；同一声明支撑入口、路线位置与 current 的区别，真实失败输入保护旧文件，差异块身份只负责固定覆盖范围。

### dev-log/README.md

入口按工具→guest→用户 shell 建立阅读目标，再区分 Linux 环境与导出参考，不把分支历史当学习顺序。

### dev-log/core/README.md

目录分组按缺口形成前后承接，并给出 Linux 穿插位置；原有逐章索引完整保留。

### dev-log/linux/README.md

入口先解释两套接口为何对照，再交代 phase 的真实执行与取证过程，历史未验证状态保留。

### dev-log/tools/README.md

以累计编辑与最新参考的差别引出作者工具，三章分别接住前一产物，不把重建等同教学质量。

### dev-log/core/next.md

以最终一条实际工作流回收首轮成果，由当前限制引出扩展而非仅列术语。

### docs/architecture.md

完整保留分层精度，按环境→所有权→内存/进程/设备/完成→文本消费者重排附加章节；纠正 service/extension 双层分派，补明确对象寿命。

### docs/knowledge-map.md

地图从可回查的具体疑问进入，保留全部实现/实验/限制表格与尚未覆盖清单。

### docs/system-practice.md

将过时的将来时接到已发布章节；真实批量读身份联系自制 handle/epoch，内存统计与回写能力分开。

### docs/roadmap.md

阶段表按新增需要解释，顺序以真实 requires 为准；图形/WebUI 的服务前提与工时边界保留。

### README.md

根入口以 demo 执行者建立系统目标，区分主机终端、内核 monitor、用户 shell 语法和各种工具环境。

### docs/verification.md

验证文件按日期和检查点分层，旧尚未实现不再被误认为当前限制；新的文字核对与历史行为证据分别定位。

### labs/README.md

以纯内存算法为何能在主机观察解释环境，原有两项程序、输出入口和 GUI 限制保留。

### labs/linux/p1/README.md

从相同输出区分 CRT/main 与 raw syscall，修复旧不存在的 28-linux-debug 跳转并保留参数条件。

### boot/AiriBoot/README.md

未实现的 AiriBoot 仍按计划而非成果描述；沿固件执行→读文件→装载 RAM→地图与退出说明三个里程碑。

## 累计复读中的回修

后半程首次改稿仍有一些段落只是增加了进入问题，原密集语言未充分展开。复读累计 diff 后再次组织 K20 用户根/门/ABI、K21 文件与校验、K22 轮转和 fork/wait、K24 三层对象和用户副作用边界、K26 提交、K29 原调用等待、K32 owner 生命周期、K33 同一页数据等完整窗口。原有精确条件留在各自作用位置，连续因果分段，成熟并列结果适当列出，没有用概述取代局部解释。

源码符号候选检查只用于定位可能丢失的语言。旧内联字符串因空格、格式或错误纠正变化而消失，不直接判为信息丢失；已逐项回查参数、宏、汇编操作数、数值、状态和调试位置。04 改用真实 %s:%u 调用，07 明确外层 ISO 与 FAT 启动映像；06–18 保留了语言语法、硬件布局及边界的就地说明。工具的来源检查不承担阅读质量结论。

本轮没有新增作者接口或改写已有教学实现。上轮精确替换功能和五篇试点保持为已有工作，冻结时已含这些变化。所有正文仍是当前 AiriOS 工作区中的未提交修改，没有另建执行副本、改历史或推送。

## 本轮保留的实际限制

- Linux L01–L08 的真实编译运行、GDB/strace、文件系统能力和 liburing 支持仍未验证；2026-10-15/16 的 SSH255 保持历史结果，没有重新尝试连接。
- L07 的 require/perror 可能把旧 errno 附在意外 CQ 的诊断后，失败项 cookie/res 也可能尚未打印。正文继续明示，源码未修；行为改动需要另行同步源码摘要和实际验证。
- L08 的外层消息预算与单次 CQ 固定等待不能合称严格两秒总截止，现已就地解释实际范围。
- 固定平台、单 CPU、不可抢占内核、静态 ELF、固定副本池、单硬件在途、UART 单输入等限制仍保留。GUI 分支不在当前工作区，本轮没有检出或修改。
- 本轮按当前授权逐篇自行审读，没有新增独立代理审阅；既有五篇的独立试点审阅属于上一轮。机械检查、作者复读都不推定真实读者已经理解。

## 处理经验

需要分别判断“新段提供进入关系”和“原段是否仍压缩太多动作”。在原段前补一句需求，不一定解决原段的组织；应沿同一对象把现状、动作、结果和下一需要连起来，再保留各处符号、例外与失败。现有 tutorial-dev-log 的完整语义窗口规则已覆盖这一要求，本轮通过实际整窗回修落实，不再叠加另一套技能或模板。

架构与入口也需要回收正文已经改变的状态。将旧追加的机制章节放回环境、所有权和调用关系中，修正已过期的将来时与旧跳转，同时以日期区分历史运行和当前文字修改。AiriBoot 仍是未实现的独立计划，不因重写说明就变成已有引导器。

## 最终实际检查与交付

- 45 个检查点逐节点重建与 125 份自有文件核对通过。
- 对本轮 accepted-start 比较，全部 45 个源码状态、源代码及命令围栏、运行命令、显式锚点、编辑动作顺序完全一致；14 份辅助文档围栏也一致，checkpoints.json 保持本轮开始字节。
- `python3 scripts/docs_check.py` 通过，126 处本地链接与画像锚点可定位。
- `comment-review.mjs audit ... --check` 只读检查 414 块，0 块低于既定说明位置阈值。它只核对旁注位置，不能说明文章可读。
- `.codex/teaching/validation/test_course_edits.py` 的 9 项精确编辑测试通过；未增加新的作者编辑接口。
- Airi-SKILLS 的 `python3 scripts/validate.py` 通过 10 个技能；本轮复用既有规则，未修改技能仓库，原有未提交修改保留。
- 两仓库 `git diff --check` 通过。当前源码与命令没有变化，因此不追加无关的 guest、Linux 或图形运行。

[桌面对照入口](</Users/bilibili/Desktop/AiriOS 全教程重构对照 2026-10-24/查看说明.md>) 提供原版到当前累计差异，以及已认可试点到当前的本轮差异。原版 Git 基线仍为 4c7aafa；普通 `git diff` 始终可看当前未提交结果。

此前 04 调用路径不符、07 ISO/FAT 混同及未改的早期章节，已由本轮逐篇修复。上一轮试点报告按历史保留，其“尚未改写”不代表当前状态。当前待解决的是上述真实 Linux 证据与明确保留的实现限制，不把这次全文重构表述为真实学习者已经无阅读困难。
