# AiriOS 学习项目约定

读者有 C++ 网络库、编译器与全栈项目经验，目前从事分布式存储。首次出现的汇编、ABI、页表、固件及 Python/Make/Shell 语法须从当前操作讲清；具体背景见共享画像和项目 `.codex/teaching/readers/`。

## 课程和分支

- main 维护 18 个检查点：00–10、K11–K13、L01、T01–T03。内核与 Linux 实验是必修主线，作者工具也在 main，不要求先完成 GUI。
- gui 从 main 的 00 根提交分叉，维护 G11–G24、G26 共 15 个节点，只增加 GUI 独有路径。它可无冲突合入任一现有 main 阶段，不修改主线入口、Makefile、文本模型、主线清单或主线文档。
- 图形运行需主线 K11，G24 的 guest 接入需 K13。合并和运行前置分别处理，不能为早期合并把图形依赖加进默认构建。
- main 不含图形课程目录、前端源码、字体或图形图像。主线文档指向图形课程与设计时使用 origin 的 gui 分支外链，不能留下本地悬空链接。
- 主线 checkpoints.json 只保存 main；gui 以独立 graphics.json 增加章节、所属文件和终点。learn 默认纯 main，合并图形清单后才扩充。export 与 HISTORY 使用同一依赖关系组合，不能用较早图形基础回退主线。
- history.json 描述当前 main 检查点链。用户对 main/gui 历史重建或远程推送的明确授权持续有效，按已授权范围完成，不重复请求相同确认；未获授权时不主动发布或推送。
- 导出目标须是新目录；被编辑的 HISTORY 快照和已有学习目录必须保护。分支重建授权不等于可以覆盖这些目录。
- 用户自述已到 day2。00–02 保持实现字节和学习连续性，正文可作必要澄清；03 起继续同一目录。P2 从异常报告开始，不以图形章节为知识前置。

## 实现与观察

- 主线是自己写的 x86-64 内核；Linux、xv6 和官方资料作为参考，不用已有内核换名替代实现。
- 用户要逐字符写完全部自有代码，包括构建、验证和平台适配。教程从空目录增长，不预装完成实现，不设自测、留题、评分或掌握度门槛；必要修改与故障由正文完整演示。
- 固定 QEMU q35、单 CPU、256 MiB、OVMF UEFI，不扩大为生产系统。AiriBoot 是独立 UEFI 引导子项目，额外预算，初期与 Limine 共存。
- 默认 Linux 实验主机为 airi@ssh.airi-dynamic.top，目录 AiriOS-labs/p1。公司开发机不进入默认执行路径。
- 外部协议只进入适配层。启动数据说明物理地址、虚拟地址和内存所有权；主机文件、图片与脚本结果不能冒充 guest 功能。
- 默认构建与测试不得读取 frontend、字体或 Cocoa。图形状态、主题、字形与应用属于 gui；Cocoa 和 reference provider 不链接进 guest。七个应用各自在 frontend/apps/，不恢复为巨型 apps.c。
- GUI 的 Day/Night 采用粉色主色、蓝色辅助、斜切标题片和细线框架；界面不放 FRONTEND REFERENCE 等开发演示标签，能力限制由操作错误和文档准确说明。[图形设计](https://github.com/Airo-dynamic/Airi-OS-Test/blob/gui/docs/design.md)维护颜色、组件与数据语义。
- 修改启动与输入行为时运行 `./airios test`；显式影响 GUI 时再运行 `./airios test --graphics`、`./airios ui-test` 并检查截图。依赖变更同步锁文件、来源许可与验收记录。
- 不创建新聊天或向他人发送消息，除非用户明确要求；已有的仓库操作授权持续按当前会话执行。

## 教程维护

- 目录与运行边界见 docs/architecture.md。项目中的教程、画像和运行证据就地维护，集中 Skill 统一维护于 Airi-SKILLS。
- 教程作者先读取本机链接 `.codex/skills/tutorial-dev-log/SKILL.md`。相关作者链接不进入 Git；缺少时从 Airi-SKILLS 获取，不把作者环境变成学习者构建依赖。
- 重组先读取 tutorial-dev-log/references/docs-workflow.md，按结构、解释、润色、导航的顺序使用相关技能。完整源码覆盖不能替代机制核对和连续阅读。
- 承接前读取 baseline、本检查点画像及其指向的实际解释段落。already_explained 只记录解释存在及其范围，不由敲完或运行成功推断掌握。
- 每章源码块可由 scripts/course.py 从声明起点重建。实现变更同步正文、检查点、画像和相关运行证据，并执行 `./airios docs-check`；全程仍需顺序阅读和必要的独立复敲。
- 原生旁注属于教学源码，有效行解释覆盖至少 99%。解释贴近实际语句，说明当前对象、值、用途与必要语法；按用户要求可保守调整局部对齐，不能机械拆开语句与解释。作者侧 comment-review 显式指定 --docs-root，不修改其他 docs 工作树。
- docs 保存架构、路线、实践地图和当前验收；图形设计与资产在 gui，完整测试图库留 artifacts。运行证据区分本轮实测、历史记录和未验证环境。
