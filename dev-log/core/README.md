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
- [后续阶段 · 块设备、日志与等待](next.md)

K13 的默认操作是 `./airios run` 和 `./airios test`。前者将 UART 接到当前终端，按 Ctrl-A 后按 X 退出；后者使用独立 QEMU 进程、日志和截止时间，检查启动与输入。默认构建只需主线源码、工具和引导材料。

接着完成必修的 [Linux 入口对照](../linux/01-entry.md)，把普通函数调用、Linux 系统调用与内核设备访问放到各自的执行环境里。后续阶段继续以串口与 Linux 实验作为必需反馈，图形接入作为可选消费者。
