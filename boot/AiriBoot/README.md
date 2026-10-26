# AiriBoot：可视化 UEFI 引导实验室

这是 AiriOS 的独立引导子项目。当前启动时，是 OVMF 执行 Limine，再由 Limine装入内核、准备响应并交出控制权。AiriBoot 要逐步接替的是中间这段工作：先能观察固件环境，再能按 ELF 描述装载内核，最后准备好数据与映射，退出固件服务并交接。当前状态为 **设计与共享接口已建立，UEFI 加载器待实现**。P1 的运行路径使用 Limine，不将它计作自己的引导器。

独立预算初估 30–50 小时；不扣减 AiriOS 的首轮学习预算。先完成 AiriOS P1，再按下面三个里程碑推进。

## A1：启动观察器

先让固件成功执行我们自己的 EFI 程序，此时还不必进入内核。使用 C 与 x86-64 UEFI 调用约定生成 `BOOTX64.EFI`。由 OVMF 加载，使用 Loaded Image、Simple File System、Graphics Output 与内存服务。

在图形画面显示实际启动步骤、内存地图和 kernel ELF 文件信息；通过固件键盘逐步查看，选择继续或报告错误。独立运行时即使没有内核文件也应清楚报告原因。

## A2：ELF 装载

观察器可以读文件以后，再把文件描述转换为可执行内存。ELF 文件里的字节与运行时 RAM 需求不同，BSS 就可能只有长度而没有文件数据。只接受 x86-64、小端、静态 ET_EXEC ELF，校验头与段表边界、文件/内存大小、整数溢出、重叠与允许的目标地址范围。按 PT_LOAD 分配、复制与清零 BSS。

准备内核栈、启动页表和版本化 AiriBootInfo，显示每个段的文件位置、物理分配和虚拟目标。首版不实现动态重定位、网络启动或 Secure Boot。

## A3：交接到 AiriOS

装载完成仍不能直接跳转：内核需要继续执行的栈、映射和启动描述，而且固件服务将在交接中结束。共享数据结构为 `include/airi/boot.h`。UEFI 入口使用固件的 x64 ABI；进入 AiriOS 的适配入口后使用 System V x86-64 ABI。固件服务调用与内核调用必须分别标注，不能混用。

分配页或准备数据会改变固件的内存地图，所以最终地图必须在准备性分配结束后取得。随后使用它对应的 key 退出 Boot Services；地图 key 变化时按 UEFI 规则重新获取并重试。成功退出后不能再调用固件的启动服务。

内核入口适配负责将传入数据复制到自己保留的存储，再进入 `airi_kernel_main`。需要同时映射交接代码、栈、内核、数据与帧缓冲，并标记加载器仍占用的区域。自主引导入口的具体二进制交接实现随 A3 完成；P1 只有共享数据接口，不能直接将现有 ELF 的 Limine 入口当成自主入口调用。

## 验收

- 缺少文件、截断 ELF、非法段范围都产生可观察错误，不能跳到未经验证地址。
- 检查文件内容复制、BSS 清零和栈映射；固定记录实际占用的物理页。
- 模拟内存地图变化，验证退出固件服务的重试。
- AiriBoot 与 Limine 两条路径进入同一套内核功能验证，保留各自日志。
- 图形步骤来自真实装载事件，不使用预制进度动画冒充状态。

规范来源：[UEFI 2.11](https://uefi.org/specs/UEFI/2.11/)、[Intel 手册](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html)、[OVMF 项目](https://github.com/tianocore/edk2/tree/master/OvmfPkg)。
