# 从BOSS与Taishan的问题进入系统机制

一次存储请求会跨过几个不同的完成时刻：程序提交字节，内核接受请求，设备报告完成，系统又可能在更晚时刻确认持久化。请求还没完成时，缓冲区能否重用、任务该在哪里等待、失败后从哪里继续，都会影响存储服务的接口设计。Linux实验沿实际调用观察这些边界，AiriOS再实现其中较小而完整的一段。

下面的连接点来自2026-10-15的本地源码调查，用于确定后续课程的问题。当前已提供L01入口、L02内存和L03进程实验，均明确记录远端未验证；L04已提供向量I/O、文件缓存与同步替换源码，Linux运行未验证；具体实验开始时会固定对应源码与运行环境。

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
