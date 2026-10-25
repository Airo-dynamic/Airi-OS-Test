# 内核主线：启动、命令与串口观察

从上一级的 [00](../00-first-tool.md)、[01](../01-tools.md)、[02](../02-dependencies.md) 继续自己的学习目录。03–08 先观察内存复制与有限输出，定义启动合同，再让自己的 ELF 进入 C 并读取真实内存图。09–10 把设备字节接入同一命令模型，以串口接收查询和返回结果。

- [03 · 重叠字节复制](03-bytes.md)
- [04 · 有限数组中的格式化日志](04-format.md)
- [05 · 可检查的启动合同](05-boot-contract.md)
- [06 · 汇编入口、栈与第一行串口输出](06-entry.md)
- [07 · UEFI镜像与首次真实启动](07-first-boot.md)
- [08 · 从Limine响应取得真实启动信息](08-real-boot-info.md)
- [09 · 命令输入、历史与输出环](09-console.md)
- [10 · UART与键盘进入同一命令模型](10-input.md)

K11 把文本会话与显示实现分开，默认构建只链接串口实现。K12 等待本轮完整标记，并向运行中的 guest 发送真实输入；K13 把这些动作收拢到统一命令入口。[可选图形课程](https://github.com/Airo-dynamic/Airi-OS-Test/blob/gui/dev-log/graphics/README.md) 维护在独立分支，guest 接入使用 K11 预留的接口，后续内核继续使用串口。

- [K11 · 独立的串口构建](11-serial-build.md)
- [K12 · 有超时的启动与输入验证](12-boot-tests.md)
- [K13 · 统一运行入口](13-commands.md)
- [K14 · CPU 异常现场与受控恢复](14-exceptions.md)
- [K15 · 物理页所有权与分配回收](15-physical-pages.md)
- [K16 · 四级页表、CR3 与真实权限故障](16-virtual-memory.md)
- [K17 · 内核堆、对象寿命与真实统计](17-kernel-heap.md)
- [K18 · PIT/PIC 时钟中断与 hlt 唤醒](18-timer.md)
- [K19 · 独立栈任务、主动切换与保护页](19-kernel-tasks.md)
- [K20 · ring3、系统调用与用户故障隔离](20-user-boundary.md)
- [K21 · 静态 ELF、用户入口与自有 libc](21-user-elf.md)
- [K22 · 用户抢占、复制 fork 与阻塞 wait](22-processes.md)
- [K23 · 事务 exec 与串口运行程序](23-exec-and-monitor.md)
- [K24 · RAM文件、FD引用与显式偏移读写](24-files.md)
- [K25 · VirtIO块设备、DMA与超时](25-block-device.md)
- [K26 · 追加日志、提交同步与坏尾恢复](26-journal.md)
- [K27 · 用户存储调用与跨QEMU重启恢复](27-user-storage.md)
- [K28 · 非阻塞管道、背压与端点引用](28-pipes.md)
- [K29 · 阻塞管道、条件等待与内核服务栈](29-wait-queues.md)
- [K30 · 块设备begin/poll与所有权](30-block-begin-poll.md)
- [K31 · 有界SQ/CQ、注册缓冲与晚完成](31-async-queues.md)
- [K32 · 用户异步I/O与进程资源轮次](32-user-async.md)
- [K33 · 用户shell与完整存储I/O演示](33-shell-pipeline.md)
- [K34 · 孤儿托管与持续会话资源回收](34-orphan-reaper.md)
- [K35 · 用户页归还与空洞复用](35-user-page-release.md)
- [K36 · 常驻用户init与UART服务重启](36-init-supervisor.md)
- [K37 · 用户内存扩容与大映像替换](37-user-capacity.md)
- [K38 · 内核堆按批扩展与缓存核对](38-kernel-heap-growth.md)
- [K39 · 自动进入init的服务启动与恢复](39-service-boot.md)
- [K40 · 块设备中断与used环后备收集](40-block-interrupts.md)
- [后续部署升级](next.md)

K13 的默认操作是 `./airios run` 和 `./airios test`。前者将 UART 接到当前终端，按 Ctrl-A 后按 X 退出；后者使用独立 QEMU 进程、日志和截止时间，检查启动与输入。默认构建只需主线源码、工具和引导材料。

接着完成必修的 [Linux 入口对照](../linux/01-entry.md)，把普通函数调用、Linux 系统调用与内核设备访问放到各自的执行环境里。后续阶段继续以串口与 Linux 实验作为必需反馈，图形接入作为可选消费者。

[K41 浮点与向量状态](41-user-fpu-state.md)给每个用户独立CPU状态页，沿抢占、fork、exec与退出保持所有权。

[K42 用户FS基址](42-user-fs-base.md)将实际FS寻址接入统一用户保存与恢复，验证不同基址的抢占、fork/exec及释放后缺页。

[K43 C11线程本地变量](43-static-tls.md)从静态ELF模板创建用户TLS副本，验证负偏移、初值/零尾部、fork继承、exec重建及超限拒绝。

[K44 errno与文件包装](44-posix-errno.md)提供显式链接的用户POSIX源码子集，保留短操作、EOF和原始ABI，验证本地错误跨fork/exec的生命周期。

[K45 显式偏移向量I/O](45-positioned-vectors.md)先复制描述与核对实际前缀，再以一次文件操作聚合/分散；保留短返回、共享偏移和坏后项无副作用。

[K46 有符号定位](46-signed-seek.md)在共享打开对象中一次完成SET/CUR/END，验证负差值、极值失败保持和写入后的零洞。

[K47 名义单调时钟](47-monotonic-clock.md)通过受检查的clock_gettime输出PIT时间，验证真实阻塞窗口并明确10ms量化与墙钟边界。

[K48 用户对象分配](48-user-malloc.md)在独立用户arena中提供malloc/free/calloc/realloc，验证原始页不足后的重试、复用清零、扩容失败保持和fork/exec生命周期。

[K49 用户内存所有权](49-user-memory-owner.md)将页表与页面记录单独持有，exec只转移新旧内存，保持内核栈、FD与进程身份，切根后再回收旧页。

[K50 用户执行现场](50-user-execution.md)将CPU状态、陷入栈与等待条件集中到execution，调度同时选择内存与现场，并保持离开栈后再销毁的顺序。

[K51 TLS零尾部](51-tls-zero-tail.md)以较短普通BSS复现真实LLD布局，修正过严的TLS总长检查，同时保持完整文件初值、孤立模板与页预算校验。

[K52 同进程工作执行者](52-shared-executions.md)共享用户内存、FD与异步buffer，独立保存栈、TLS和CPU环境，提供阻塞join并验证容量、复用和整个进程的退出故障。

[K53 私有内存等待](53-private-futex.md)加入条件比较与登记、显式有界唤醒、PIT超时与进程/VA私有键，并把真实程序目录分段输出。

[K54 用户互斥](54-owned-mutex.md)以原子TID所有权、固定截止及futex重试保护普通数据，验证带锁yield、错误所有者和非robust死亡边界。

[K55 共享分配器](55-shared-malloc.md)将公开调用与内部操作分开，保护唯一arena与一致快照，验证四执行者分配和跨执行者归还。

[K56 运行头身份](56-runtime-identity.md)由内核在启动、原始fork、exec及工作槽复用时发布版本化TCB，普通TID读取留在用户态，后备查询有独立陷入证据。

[K57 请求计数](../core/57-syscall-count.md)在真实128号入口按执行者计数，区分fork新建与exec保留，测出八对无竞争锁的八次通知基线。

[K58 通知快路径](../core/58-mutex-notification.md)用SC参与者登记、再次争取和统一撤销保留真实竞争等待，同一八对无竞争锁减少八次系统调用。

[K59 受控交错](../core/59-mutex-races.md)暂停同一次加锁的三个用户窗口，再与真实睡眠比较，区分省略通知、空通知、比较失败和真正唤醒。

[K60 变化序号](../core/60-sequence-events.md)把旧快照、原始通知与真正变化分开，验证两位worker重查、广播、迟到新快照与拒绝回绕。

[K61 有界工作队列](../core/61-bounded-queue.md)组合锁和变化通知，实际验证四槽背压、空满超时、双侧关闭排空及48项锁外计算。

[K62 线程源码入口](../core/62-pthread-entry.md)以正确函数类型交接启动包和指针结果，保留errno并返回正错误，验证容量和分配失败的归还。

[K63 嵌套创建](../core/63-nested-workers.md)允许worker创建同进程leaf，先收拢并复用creator槽，再确认仍活着的leaf保留独立TLS、TCB与结果。

[K64 对等join](../core/64-peer-join.md)在目标上唯一认领结果，登记前检查有界等待链，实测重复认领、闭环和坏输出保持。

[K65 pthread互斥](../core/65-pthread-mutex.md)映射默认ERRORCHECK，维护静态/动态寿命，并在内部200tick到期以后继续公开无总超时lock。

[K66 八字节等待](../core/66-futex64.md)保留完整64位期望值，把宽度纳入私有键，并用相同VA的两种通知顺序验证隔离。

[K67 完整宽快照](../core/67-wide-events.md)分开完整64位数据与错误状态，并分别观察推进版本、通知一人和通知所有等待者。

[K68 互斥对象存活](../core/68-mutex-lifetime.md)用真实worker验证暂时释放后仍须保留对象，先重取再归还引用，空闲可以继续使用但不能提前销毁。
