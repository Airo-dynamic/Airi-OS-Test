# 2026-10-24 开发与 MBP 教程工作交接

本次按用户要求上传开发进度、同步 MBP，并采用 MBP 更新后的教程写作方法。这里只记录作者交接，不改变学习路线、课程起点或内核实现。

## 固定的开发状态

开发终点是 `75e9344`（`k52-shared-executions`）。相对于 MBP 教程工作的基线 `4c7aafa`，追加 K34–K52；00–02 的实现字节未变，没有重建既有 Git 历史。GitHub 的 main 和现有阶段标签已经上传。源码、教程草稿、画像与逐阶段验证记录都在该版本内。

当前清单包含 64 个检查点、167 份自有实现文件；单独重建 K52 路线得到 161 份文件。K34–K47 依次补充孤儿回收、用户页释放、init 监督、容量、内核堆、无人值守启动、块设备中断、用户 FPU/FS、静态 TLS、errno、向量 I/O、带符号 seek 与名义单调时钟。K48–K52 的直接续点如下。

| 节点 | 已实现能力 | 后续必须保留的边界 |
| --- | --- | --- |
| K48 | 用户态 malloc/calloc/realloc/free，有界堆与失败保持 | 单线程使用；释放对象不立即释放整个 arena 的页 |
| K49 | 地址空间及用户页所有权独立为 user_memory | exec 先切换 CR3，再销毁旧地址空间 |
| K50 | user_execution 独立保存 CPU 现场、栈与等待 | 进程资源与执行现场分开，尚不等于已经有线程 API |
| K51 | 正确区分 ELF TLS 初始化字节与零尾 | TLS 的内存尾部不能套用普通 LOAD 的文件字节范围 |
| K52 | 同一进程内共享地址空间与 FD 的工作执行体、私有 TLS/FPU、阻塞 join | 最多八个工作槽；仅主执行体创建/join；不是完整 pthread |

K52 的调用号为 39–42；worker 普通返回只结束自身，exit 或用户异常结束整个进程组。存在未回收 worker 时，fork/exec/waitpid/原始用户页释放返回 EBUSY。被同组执行体等待的 FD 暂不能关闭，以免复用后改变等待对象。用户栈和 TLS 槽可缓存到 exec/exit，内核栈与 FPU 页在离开目标栈后回收。

K53 尚未开始实现。下一候选是私有 futex compare-and-wait/wake，再以其支撑互斥与线程安全分配；尚无 K53 教程或已完成能力。网络栈、原生 Cloudflare 客户端、实际开发板和电源接线仍是后续阶段。

## 已有证据与未验证事项

源码终点的 `artifacts/k52/test-complete.log` 是完整默认测试的最终成功日志。包含八组宿主 sanitizer 检查、十个诊断启动、输入路径、原有用户程序、两次真实 threads 命令、init 监督与存储会话。每次 threads 根进程创建 35 个 worker，另有子进程场景；覆盖共享内存/FD/注册异步缓冲区、独立 TLS/FPU、八槽容量、复用、部分创建失败回滚、join 和整组退出/故障。

`artifacts/k52/replay-final.log`、`replay-build-final.log` 与 `replay-shell-final.log` 来自独立教程重建目录。161 份实现字节与清单相符，重建后实际编译和 shell 运行成功。早期失败和原型日志仍保留，不能用它们替换最终版本的证据。

本次交接又收取了先前启动的发布检查：`snapshots.log` 记录 64 个独立快照；`export.log` 记录保护式导出到新目录 `/tmp/airios-k52-release-20261024`。图形回归在隔离的组合仓库完成，167 份 main 实现文件逐字节相同；`graphics-test.log` 与 `ui-test.log` 通过，后者含 84 张宿主界面快照和 58 项对比度检查。本次已查看真实 guest 的 Day/Night 截图 `gui-day.png`、`gui-night.png`。这些图形结果没有把 GUI 合入 main。

Linux `p5-threads` 的实际尝试仍为 SSH 255，未在 Linux 上编译或运行；见 `artifacts/linux-p5-threads/remote.json`。本次连接成功的是 MBP，不能据此把 Linux 分支改为已验证。源码覆盖、旧方法的文字审查和旁注检查也不能证明新增课程符合本次更新的写作方法。

## MBP 工作区与交付位置

MBP 原工作区 `/Users/bilibili/Desktop/AiriOS` 正在基于 `4c7aafa` 修改 00、05、K19、K31、L07、画像及 course/snapshots 作者工具。它有未提交改动，本次没有 reset、stash、checkout 或覆盖这些文件，也没有合并其工作中的文档。

新的交付目录为 `/Users/bilibili/Desktop/AiriOS-dev-sync-20261024`：

- `source/`：独立 main 源码仓库，保留完整历史及标签；不借用原教程工作区的未提交文件。
- `development.bundle`：源码仓库与标签的离线传递包。
- `evidence.tar.gz`：K34–K52、最终 shell 和 Linux 失败记录的原始证据归档。
- `source/artifacts/`：展开后的证据，可直接按相对路径查看。
- `policy/`：本次通过隧道取得的教程技能快照及逐文件 SHA-256。
- `sync-receipt.json`：源码终点、文件摘要检查及原工作区保护检查结果。

若在原 MBP 工作区继续构造教程，可读取新目录的固定源码、验证记录和 Git 差异；待当前改稿被妥善保存后再组合两边的变更。不要用开发侧旧版 checkpoints.json 覆盖 MBP 对 T01–T03 作者工具摘要的修改。MBP 新增的精确 replace 动作及其 before/after 围栏需要配套 parser、工具教程和摘要一起迁移；当前开发终点尚未导入这组未提交修改。

## 后续采用的写作规范

规范来自 MBP `/Users/bilibili/Desktop/Airi-SKILLS/skills/tutorial-dev-log` 的实际工作树，而非仅其已提交 HEAD。本机同名技能已经同步，原有 grounding 修改保持原样；技能仍是作者环境，不成为学习者构建依赖。精确快照记录在交付的 `policy/tutorial-policy-sha256.json`，本仓库 AGENTS.md 同步了持续写作约定。

写作先从当前操作中找到读者第一次必须自行补入的关系，结合真实源码完成一个连续语义窗口，再展开整章。上一段建立的状态必须支撑下一段；同一个例子继续参与后文推理，实参与真实观察调用相符。普通调用、系统调用、设备操作、完成发布与结果收取分别说明。有效事实、错误条件、恢复方式和局部语法解释保留在作用处，不能只换成更短的故事。

每批复读完整累计 diff，核对原信息保全和段间承接；完成一篇后重读 writing 与相关 worked-examples，再继续下一篇。来源、行为、语义和连续阅读分别验证，重建成功与 99% 旁注覆盖只证明各自检查范围。后续新实现及其教程使用这套方法，既有 K34–K52 文字仍需按新标准逐篇评估，不能由上传同步宣布已经完成重写。

MBP 的具体试点、剩余问题和扩展顺序保留在原工作区 `.codex/teaching/validation/tutorial-prose-2026-10-24.md`。其中 04 的实际格式实参、07 的 ISO/FAT 关系和 L07 的错误完成诊断仍需处理，本次源码同步没有修复这些事项。
