# 2026-10-25：T04 与 K53–K55 开发交接

实现终点为 `ac9c234`（`k55-shared-malloc`），后续交接记录提交不改变实现字节。GitHub main 已保留追加历史，T04、K53、K54、K55 分别有阶段提交和标签。旧64个检查点没有回写；内核路线独立于作者工具及 GUI。

## 新增的可运行节点

| 节点 | 实现 | 教程与证据 |
| --- | --- | --- |
| T04 `cd6e13c` | 接入 MBP 已实现的精确 replace 编辑器；保留旧 write/append，整章失败不发布 | dev-log/tools/04-exact-edits.md；九项编辑器测试、52份独立重建与测试通过 |
| K53 `f0ccad9` | 私有进程/VA futex、比较与登记、有界通知及PIT截止；目录分段输出 | dev-log/core/53-private-futex.md；纯主线162份重建，完整test与独立shell通过 |
| K54 `7db09fa` | TID所有权CAS、有限截止重试、release/acquire互斥及非robust边界 | dev-log/core/54-owned-mutex.md；165份纯主线重建、64次带锁yield更新、错误所有者及死亡观察通过 |
| K55 `ac9c234` | 公开分配入口只加锁一次，内部操作不重入；共享arena与一致快照 | dev-log/core/55-shared-malloc.md；166份纯主线重建，四worker共96轮和四次跨执行者归还通过 |

`b86b5bf` 另补充 K54 的完整 freestanding C++ 头检查输入与命令，没有改锁实现。它只验证公共声明，未宣称 C++ 运行库已完成。

当前共68个检查点、173份自有实现；旁注位置检查684块，无低于99%项。每阶段完整默认测试均退出0，独立重建只借锁定的外部引导材料后实际编译、运行shell。原始日志在 artifacts/t04、k53、k54、k55；最新完整 shell 日志在 artifacts/shell-normal。68份独立快照及新目录 K55 导出通过；现有学习目录和 HISTORY 未被覆盖。

## 教程接续与已知边界

新章节按 MBP 2026-10-24 的连续语义窗口方法写作，before/after 保留精确旧字节，先核对当前动作及真实值，再接到下一消费者。T04独立，K53只依赖K52，K54只依赖K53，K55只依赖K54。纯内核重建目录没有 scripts/course.py，也能编译和完成真实shell观察。

T04把MBP编辑器作为追加节点引入，没有将MBP尚未提交的00、05、K19、K31、L07改稿覆盖到本机，也没有修改它们的旧源码摘要。后续组合文档时，请保留各自有效改稿，核对作者工具的起点，不用旧版整份清单覆盖新增节点。来源和旁注通过只证明对应范围，既有章节仍需逐篇按新方法审查。

K55普通malloc.o不主动yield；只有明确的malloc-observe.o启用AIRI_MALLOC_OBSERVE_YIELD，以真实guest调度触发竞争。分配器锁只保护元数据，应用仍管理对象交接和最后一次free；页缓存到进程exit/exec才回收。内部锁等待到期可重试，不把free静默丢弃；当前没有取消、信号或非robust allocator恢复。

互斥仍经42号取得TID，unlock仍通知内核，尚未实现零系统调用快速路径。下一候选是每执行者身份缓存与fork后运行库更新，再减少无等待通知；不能让fork复制的旧缓存冒充孩子身份，也不能只看函数输出就宣称减少了真实陷入。

Linux最近实际尝试为K53的p5-threads，仍SSH255，verified=false；K54/K55没有伪造新连接结果。GUI沿用K52的历史隔离组合，本轮未改图形或宣称新截图。开发板、原生网络、Server-Lib和Cloudflare仍在后续路线；私有凭据不进入本次同步。

## MBP 同步位置

增量交付目标为 `/Users/bilibili/Desktop/AiriOS-dev-sync-20261024/updates/K55-20261025`。其中 source 是独立Git副本，evidence 为上述日志，development.bundle 基于先前已交付的86afc3b，sync-receipt.json记录实际提交、173份实现校验及原工作目录的保护结果。

本次同步不在原 `/Users/bilibili/Desktop/AiriOS` 或先前交付的 source 目录 checkout、reset 或覆盖文档。它们可能仍承载教程工作；新的开发快照另有自己的路径。同步若受隧道影响未完成，应以实际回执为准，不以本文的目标路径推断交付已经成功。
