# 从首轮演示继续到长驻设备

P2–P6 已由 K14–K33 提供异常、内存、用户进程、文件、设备、日志、等待、异步队列和真实用户 shell。自己的 day2 目录仍从 03 连续增长。旧停点保留，升级按新的编号追加，图形支线可以暂停。

P7 从 [K34 孤儿托管](34-orphan-reaper.md) 开始：同一用户 shell 内运行二十四轮子孙进程，父退出后仍保留活孩子，并回收无人领取的僵尸。[K35](35-user-page-release.md)增加整页释放、连续虚拟空洞复用与释放后的真实故障观察；[K36](36-init-supervisor.md)让用户 init 在同一会话内持续监督 UART shell，退出后真阻塞退避再重启；[K37](37-user-capacity.md)接着扩大用户数据、ELF与栈容量，并让页记录按需分配；[K38](38-kernel-heap-growth.md)提供按批扩展的内核堆；下一步继续正式启动策略与设备中断服务。

[部署路线](../../docs/deployment-roadmap.md)将后续工作分成 P7–P14：长驻、POSIX 源码兼容与 C++、自有网络、Server-Lib、文件与真实设备、原生 Cloudflare、隔离按键与部署，再进入 SMP 和异步强化。WebUI 与桌面只消费系统服务，不成为这些机制的前置。

[L01–L08](../linux/README.md) 仍需在个人 Linux 实际运行。新机器本轮 SSH 握手失败，保留未验证状态；完整程序和 Mac 拒绝分支都不能代替 Linux 运行证据。内核开发与环境恢复继续独立推进，真实记录见[验证记录](../../docs/verification.md)。
