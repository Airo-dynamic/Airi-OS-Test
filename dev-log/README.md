# 从串口进入内核与 Linux I/O

从空的 `~/Desktop/AiriOS-work` 目录开始，沿代码块逐步写出自己的系统。每章在已有结果上增加一项操作，并给出完整编辑范围、机制解释和运行观察。检查点表示源码位置，可以按自己的节奏停下和继续；课程不设答题、评分或掌握度门槛。

已到 day2 可以继续 03，无需重新开始。00–02 的实现字节保持不变，正文只作必要澄清。03–10 接着完成基础库、启动、串口命令和真实输入，K11–K13 形成日常构建与验证入口，然后完成 Linux 对照 L01。T01–T03 提供课程重建、来源检查与快照工具，它们都属于 main，不依赖图形课程。

| 集合 | 入口与前置 | 怎样继续 |
| --- | --- | --- |
| [内核](core/README.md) | 00–10、K11–K13 | 在同一学习目录继续，以串口观察结果 |
| [Linux 对照](linux/README.md) | L01 承接 K13 | 必修；分别观察普通程序、系统调用和内核入口 |
| [作者工具](tools/README.md) | T01–T03 接在 L01 后 | 沿 main 写出重建、校验和导出工具，无需 GUI |
| [图形课程](https://github.com/Airo-dynamic/Airi-OS-Test/blob/gui/dev-log/graphics/README.md) | 独立 gui 分支，运行需 K11，G24 需 K13 | G11–G23 先作宿主观察，G24 接 guest，G26 检查图形行为 |

## 从 day2 继续

- [00 · 从空目录找到并运行一个程序](00-first-tool.md)
- [01 · 让构建使用正确的一份工具](01-tools.md)
- [02 · 让外部引导材料有可验证的身份](02-dependencies.md)
- [03 · 让字节复制在重叠时仍然正确](core/03-bytes.md)

编号不对应日历天数。03–10 沿 02 的文件基础继续；像素、字体、主题和窗口由可选图形课程引入，不成为异常、内存和 I/O 学习的前置。

## 在参考工程中查阅和导出

```sh
./airios learn list # 默认列出main当前已发布节点。
./airios learn list --track core # 只显示内核课程及其前置。
./airios learn show K11 # 阅读这一检查点的完整正文。
./airios learn progress --record 02 # 记录自己的主线停留位置。
./airios learn export K13 /tmp/airios-core-reference # 在新目录重建纯串口对照源码。
./airios snapshots # 在 HISTORY 中生成各检查点的独立对照目录。
```

这些命令从参考工程执行。导出源码用于对照，带敲目录仍从章首声明的起点连续增长。progress 只记录所选路线的位置，不修改学习源码；export 拒绝已有目标目录。HISTORY 由固定来源和教程重建，快照被编辑后不会直接覆盖。

## 组合可选图形停点

main 的基础18节点之后已追加 K14–K58、L02–L08 和 T04，共71节点。gui 从共同的 00 根提交分叉，有 G11–G24、G26 共 15 个节点，并只增加自己的文件。因此 gui 可以无冲突合入任一现有 main 阶段，既不替换主线入口和 Makefile，也不修改文本模型。

图形的运行前置另行声明：运行需主线到 K11，G24 的 guest 接入需 K13。合并 gui 后出现独立的 graphics.json，learn 才增加图形节点；未合并时，主线列表、导出和快照只处理 main。

```sh
./airios learn list --track graphics # 合并 gui 后查看可选课程。
./airios learn progress --record G13 # 单独记录图形停留位置。
./airios learn export K13 /tmp/airios-core-with-g13 --alongside G13 # 组合当前主线与图形停点。
```

组合会闭合所选检查点的前置，只应用一次共享基础。G13 的图形文件不会把 K13 的内核退回 K11；HISTORY 中的图形检查点也包含其需要的主线基础。图形进度不推进或重置内核位置，G11–G23 的宿主结果也不代表 guest 已经显示图形。

[当前验证](../docs/verification.md)记录实际运行和未验证平台；[系统实践地图](../docs/system-practice.md)说明这些机制怎样回到 BOSS、Taishan 和以前的网络库。

完成 K13 和 L01 后，从 [K14 异常入口](core/14-exceptions.md) 连续到 [K17 内核堆](core/17-kernel-heap.md)，再读 [L02 Linux 内存](linux/02-memory.md)。K18–K23继续用户进程，K24–K28完成FD、管道和存储，K29–K33继续等待、异步与用户shell；L03–L08补进程、Linux文件、线程、epoll和io_uring对照。源码与教程每节都有独立停点，自己的 day2 目录仍按03开始，不需要导入新内核成品。

[首轮完整演示](core/33-shell-pipeline.md)与[知识地图](../docs/knowledge-map.md)可用于回查整条路径，Linux实际环境限制见[Linux集合](linux/README.md)。

P7 从 [K34 孤儿托管](core/34-orphan-reaper.md) 继续。后续按[部署路线](../docs/deployment-roadmap.md)追加升级检查点，旧课程与个人学习位置保留。

[K35 用户页归还](core/35-user-page-release.md)继续处理同一地址空间内的资源复用与解除映射故障。

[K36 常驻 init](core/36-init-supervisor.md)把用户前台服务的退出、阻塞退避与重启串成持续运行路径。

[K37 用户内存扩容](core/37-user-capacity.md)让大BSS、跨页用户栈与1024页的fork/exec进入同一条可重复路径。

[K38 内核堆扩展](core/38-kernel-heap-growth.md)接入有界页面增长，并同步任务与用户会话的资源核对。

[K39 服务启动](core/39-service-boot.md)将资源初始化与教学观察分开，支持无需输入run init的自动监督。

[K40 设备中断](core/40-block-interrupts.md)把硬件提示交给普通完成收集，不以中断次数代替请求结果。

[K41 浮点与向量状态](core/41-user-fpu-state.md)给每个用户独立CPU状态页，沿抢占、fork、exec与退出保持所有权。

[K42 用户FS基址](core/42-user-fs-base.md)将实际FS寻址接入统一用户保存与恢复，验证不同基址的抢占、fork/exec及释放后缺页。

[K43 C11线程本地变量](core/43-static-tls.md)从静态ELF模板创建用户TLS副本，验证负偏移、初值/零尾部、fork继承、exec重建及超限拒绝。

[K44 errno与文件包装](core/44-posix-errno.md)提供显式链接的用户POSIX源码子集，保留短操作、EOF和原始ABI，验证本地错误跨fork/exec的生命周期。

[K45 显式偏移向量I/O](core/45-positioned-vectors.md)先复制描述与核对实际前缀，再以一次文件操作聚合/分散；保留短返回、共享偏移和坏后项无副作用。

[K46 有符号定位](core/46-signed-seek.md)在共享打开对象中一次完成SET/CUR/END，验证负差值、极值失败保持和写入后的零洞。

[K47 名义单调时钟](core/47-monotonic-clock.md)通过受检查的clock_gettime输出PIT时间，验证真实阻塞窗口并明确10ms量化与墙钟边界。

[K48 用户对象分配](core/48-user-malloc.md)在独立用户arena中提供malloc/free/calloc/realloc，验证原始页不足后的重试、复用清零、扩容失败保持和fork/exec生命周期。

[K49 用户内存所有权](core/49-user-memory-owner.md)将页表与页面记录单独持有，exec只转移新旧内存，保持内核栈、FD与进程身份，切根后再回收旧页。

[K50 用户执行现场](core/50-user-execution.md)将CPU状态、陷入栈与等待条件集中到execution，调度同时选择内存与现场，并保持离开栈后再销毁的顺序。

[K51 TLS零尾部](core/51-tls-zero-tail.md)以较短普通BSS复现真实LLD布局，修正过严的TLS总长检查，同时保持完整文件初值、孤立模板与页预算校验。

[K52 同进程工作执行者](core/52-shared-executions.md)共享用户内存、FD与异步buffer，独立保存栈、TLS和CPU环境，提供阻塞join并验证容量、复用和整个进程的退出故障。

[T04 精确编辑](tools/04-exact-edits.md)增加 before/after 局部替换，核对唯一旧窗口，并保护整章失败前的源码状态。

[K53 私有内存等待](core/53-private-futex.md)加入条件比较与登记、显式有界唤醒、PIT超时与进程/VA私有键，并把真实程序目录分段输出。

[K54 用户互斥](core/54-owned-mutex.md)以原子TID所有权、固定截止及futex重试保护普通数据，验证带锁yield、错误所有者和非robust死亡边界。

[K55 共享分配器](core/55-shared-malloc.md)将公开调用与内部操作分开，保护唯一arena与一致快照，验证四执行者分配和跨执行者归还。

[K56 运行头身份](core/56-runtime-identity.md)由内核在启动、原始fork、exec及工作槽复用时发布版本化TCB，普通TID读取留在用户态，后备查询有独立陷入证据。

[K57 请求计数](core/57-syscall-count.md)在真实128号入口按执行者计数，区分fork新建与exec保留，测出八对无竞争锁的八次通知基线。

[K58 通知快路径](core/58-mutex-notification.md)用SC参与者登记、再次争取和统一撤销保留真实竞争等待，同一八对无竞争锁减少八次系统调用。
