# 从BOSS与Taishan的问题进入系统机制

一次存储请求会跨过几个不同的完成时刻：程序提交字节，内核接受请求，设备报告完成，系统又可能在更晚时刻确认持久化。请求还没完成时，缓冲区能否重用、任务该在哪里等待、失败后从哪里继续，都会影响存储服务的接口设计。Linux实验沿实际调用观察这些边界，AiriOS再实现其中较小而完整的一段。

下面的连接点来自2026-10-15的本地源码调查，用于确定后续课程的问题。当前已提供L01–L08完整代码和教程，覆盖入口、内存、进程、文件、线程、epoll与io_uring；个人主机本轮SSH255，Linux实际编译和运行均未验证。项目引用表示当时调查的连接点，不替代各实验环境记录。

| 本地来源与具体操作 | Linux实验需要解释的关系 | AiriOS承担的机制 |
|---|---|---|
| BOSS storage_engine/common/eg_disk_io.cpp的preadv/pwritev与sync_fd | FD与打开文件对象、偏移、短读写、errno/EINTR、iovec各段的消费位置 | 文件对象、整数句柄、有界缓冲区与返回值 |
| BOSS storage_engine/common/dio_writer.cpp的对齐缓冲与写入 | O_DIRECT对地址/长度/偏移的要求，补齐字节、缓存路径与同步分别意味着什么 | 页与设备缓冲区、块请求；不重写Linux全部直接I/O |
| Taishan node/src/untar.cpp的WritableFile::open | posix_memalign、fallocate、实际写入、元数据与空间占用 | 页分配和块设备，对齐与容量失败的报告 |
| BOSS storage_engine/common/sequencial_file.cpp的MultiSeqFileReader::read | 提交记录SQE、完成记录CQE、user_data关联、部分提交、结果与缓冲区寿命 | 有限提交/完成队列，完成前不回收缓冲区 |
| BOSS common/thread_pool.cpp的入队、wait_for与notify | 谓词、互斥、睡眠、唤醒、退出与队列容量，pthread和bthread各由谁调度 | 任务状态、等待队列、阻塞和唤醒 |
| Taishan node/src/unpacker.cpp中的fsync | 脏页、回写、设备完成和错误；同步并不等于释放全部页缓存 | 设备同步与明确的完成条件 |
| Airi-Cpp-Server-Lib的FileStorage::atomicWrite | ofstream.flush、rename可见性、文件和目录fsync、进程退出与掉电模型 | 小型追加日志、校验与截断恢复 |
| 已写过的EpollPoller与BOSS/Taishan RPC路径 | LT/ET、非阻塞、socket缓冲、背压、半关闭、超时与取消 | 管道就绪；完整TCP/IP先在Linux实验 |

`FileStorage::atomicWrite`提供一个具体的持久性问题：临时文件替换为正式名字后，其他进程看到完整的新版本，并不自动说明掉电后也能恢复这个版本。旧注释混合了这两个结果；后续示范将依次写临时文件、同步文件内容、替换名字、同步目录，并在每个失败位置保留可以解释的结果。文件`fsync`不单独保证所在目录项也已持久化，规则见[fsync手册](https://man7.org/linux/man-pages/man2/fsync.2.html)。

## 每组实验都回到同一请求

例如一次批量读先在Linux中打开文件并分配若干缓冲区。程序为每个读取操作准备提交记录SQE，其中写入目标缓冲区、长度与用于辨认请求的`user_data`，再将记录提交给内核。内核完成后产生完成记录CQE，程序用它带回的身份找到原请求，再读取返回字节数或错误结果，最后才重用相应缓冲区。完成顺序可能不同于提交顺序，记录的身份把这两端重新关联起来，接口规则见[io_uring手册](https://man7.org/linux/man-pages/man7/io_uring.7.html)。

AiriOS会从较小的有界队列实现同一段生命周期：谁持有请求、谁暂时借用缓冲区、等待者何时被唤醒，以及错误如何交回提交者。再回到`MultiSeqFileReader::read`，就可以对照其批量提交、完成处理和缓冲区回收之间的约束。

内存实验会分别观察申请虚拟地址、第一次访问页面、实际物理占用与回收。持久性实验分别说明进程退出、虚拟机重启和掉电模型，因为它们保留的内核与设备状态不同。AiriOS只实现范围受控的缓存、等待与恢复过程；Linux的完整回写和调度策略继续通过实际接口观察。futex、原子操作、内存序、信号与网络先进入Linux实验，再根据具体用途扩展内核。

每个实验提供完整程序、命令、正常和故障结果，并记录OS、内核、文件系统、工具与权限条件。观察工具在需要辨认某个状态时加入，例如区分虚拟申请和物理占用时再解释进程内存记录；操作和参数跟着同一请求出现。L01当前的真实连接结果与尚未验证范围见[验证说明](verification.md)。

## 已发布入口

[L04存储](../dev-log/linux/04-storage.md)对应向量I/O、直接I/O和同步替换，[L05线程](../dev-log/linux/05-threads.md)对应队列谓词与futex，[L06就绪](../dev-log/linux/06-epoll.md)对应Reactor背压，[L07完成](../dev-log/linux/07-uring.md)对应批量读取的身份和缓冲寿命。[K33](../dev-log/core/33-shell-pipeline.md)把用户分配、FD、等待与设备完成串起来，[L08](../dev-log/linux/08-comparison.md)在同一TCP负载中区分等待位置，不提供未经实测的性能优劣结论。

K52将线程池所需的共享执行落到自有内核：工作者共享FD表和进程注册buffer，栈/TLS/CPU状态独立；主执行者创建和join，进程退出收拢整个组。回看BOSS时可先区分进程资源与每条执行链，再继续L05的条件谓词与futex。当前用户malloc仍单线程，实际请求对象不能无保护地由多个工作者同时分配；下一步建立内核比较入睡和用户互斥，不以yield循环替代最终线程库。

K53把共享执行链接到私有futex，沿先通知后WAIT的-11、三人按1+2唤醒及父子相同VA隔离追踪。BOSS任务队列后续仍需要互斥/条件变量维护业务谓词；当前wake只恢复调度资格。Linux对照继续使用p5-threads，未连接不能计为实测。

K54以持有者TID和CAS把futex组合成用户互斥。普通counter/checksum的半更新主动yield仍受同一锁保护；队列里的数据发布需要release/acquire，通知只恢复争取资格。非robust死亡与尚存系统调用成本明确保留，分配器同步留待下一阶段。

K55给共享分配器增加元数据串行化，应用仍须明确移交指针和决定最后一次free。BOSS异步读取中的buffer存活期不因分配器有锁而结束；完成收取以前仍不能归还可能被设备或请求借用的对象。观察程序以release/acquire作单次对象交接，不把静态地址槽当成可无限覆盖的队列。

K56减少运行库反复取得TID的陷入，并检验fork复制普通TLS时不能复制旧身份这项区别。BOSS/线程池中的普通函数调用、线程库记录与内核线程身份不是同一个对象；快速读取也不意味着Linux pthread_t可当内核TID。后续继续用实际计数核对释放通知成本。

[K57](../dev-log/core/57-syscall-count.md)先测量同一执行者实际发出的请求：八对局部互斥仍有八次解锁通知。下一步通知优化用同一窗口比较；查询自身计一次、打印移出窗口，次数不能直接换算延迟。

[K58](../dev-log/core/58-mutex-notification.md)沿K57同一八对局部锁观察，把差值9改为1，扣结束查询后通知数从8到0；有意争用的互斥和共享分配仍真实等待。这里只减少请求数，没有测量耗时。

[K59](../dev-log/core/59-mutex-races.md)用同一个实际锁控制四个交错位置。第3轮真实43号返回-11但不登记睡眠，第4轮才有WAIT及woken=1；观察耗时不作为普通库性能。

[K60](../dev-log/core/60-sequence-events.md)使两个实际worker在通知后回到相同条件循环，重试共用原预算；版本变化不等于得到任务，下一步把真正队列谓词放进互斥保护。

[K61](../dev-log/core/61-bounded-queue.md)实际两生产者/两消费者处理48项，以位图排除重复并核对和值1176、平方和38024。四槽限制排队不限制所有在途计算，关闭先拒绝新增再排空，线程结果体现工作完成。

[K62](../dev-log/core/62-pthread-entry.md)验证同一用户参数经过真实线程返回给join，34位执行者的包在回调前归还，容量失败不泄漏，malloc失败也不改调用者errno。主/worker资源能力仍有明确边界。

[K63](../dev-log/core/63-nested-workers.md)让worker真实创建leaf，creator已join且旧槽运行过replacement以后leaf仍能恢复。普通函数返回与进程组exit分开，参数不得借用已结束创建者的局部栈。

[K64](../dev-log/core/64-peer-join.md)让worker真实创建并join child；另一组A/B证明主争抢B不偷结果，B反向等待A报35，原A等待继续成功，资源收拢后fork仍能推进。

[K65](../dev-log/core/65-pthread-mutex.md)真实持锁205tick触发底层200tick到期，公开两worker仍取得锁并完成64次普通数据更新；无竞争八对公共操作仍无额外系统调用。

[K66](../dev-log/core/66-futex64.md)将1改为仅高32位变化的0x1122334400000001，旧1宽等待立即不符；同址32/64各一等待者，两种通知顺序均隔离。它为宽变化序号提供机制，不是Linux ABI或旧event已升级的证明。
