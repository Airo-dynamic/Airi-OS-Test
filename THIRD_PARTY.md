# 外部来源、版本与许可

## Limine

- 引导器版本：12.9.2；下载 URL 和 SHA-256 见 config/deps.lock.json。
- 引导器及工具许可保留在 third_party/limine-LICENSE，下载目录中的 3RDPARTY.md 列出进一步的第三方来源。
- 协议头来自 limine-protocol 的固定提交 3a0526b700e356f0eac1b71a77697b3fd1c707a3，完整头与 0BSD 许可保留在 third_party/limine.h。
- 请求段排列及标记方式参考官方 C 模板，参考提交 d2a2607b0f7a48498b13f02a89aebf5c2417d1b8。入口、内核逻辑、标准化启动结构、基础库、串口与面板由本项目编写。旧 5×7 字形为原始实现；当前桌面采用下面列出的 IBM Plex 字体。

官方来源：[引导器](https://github.com/Limine-Bootloader/Limine/releases/tag/v12.9.2)、[协议](https://github.com/Limine-Bootloader/limine-protocol/tree/3a0526b700e356f0eac1b71a77697b3fd1c707a3)、[C 模板](https://github.com/Limine-Bootloader/limine-c-template-x86-64/tree/d2a2607b0f7a48498b13f02a89aebf5c2417d1b8)。

## QEMU 与 OVMF

验证版本：QEMU 11.1.2。固件来自该版本 Homebrew QEMU 包中的 edk2-x86_64-code.fd 和 edk2-i386-vars.fd，SHA-256 分别锁定。

QEMU 为 GPL-2.0；EDK2/固件组件的许可集合复制在 third_party/edk2-licenses.txt。本仓库不将引导器与固件二进制作为自制内核源码提交。

来源：[QEMU](https://www.qemu.org/)、[EDK2 OVMF](https://github.com/tianocore/edk2/tree/master/OvmfPkg)、[Homebrew QEMU](https://formulae.brew.sh/formula/qemu)。

## 原理资料

CPU 特权、寄存器与页表参考 [Intel SDM](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html)。教学结构参考 [MIT xv6 课程与书](https://pdos.csail.mit.edu/6.828/2025/xv6.html)，没有导入 xv6 内核。

后续设备依据 [VirtIO 1.3](https://docs.oasis-open.org/virtio/virtio/v1.3/virtio-v1.3.html)。真实接口实验依据 [epoll](https://man7.org/linux/man-pages/man7/epoll.7.html)、[io_uring](https://man7.org/linux/man-pages/man7/io_uring.7.html)、[fsync](https://man7.org/linux/man-pages/man2/fsync.2.html) 与 [Linux VFS](https://docs.kernel.org/filesystems/vfs.html) 官方项目资料。

## IBM Plex 前端字体

界面使用 IBM Plex Sans，代码和终端使用 IBM Plex Mono。来源为 Google Fonts 固定提交 `2eb0b48d5f760f62e286216f0859a8c540dbc1bd`，逐文件 URL 与 SHA-256 在 `frontend/assets/fonts.lock.json`；`config/deps.lock.json` 同时锁定生成图集。

字体采用 SIL Open Font License 1.1，原始字体及 `sans-OFL.txt` / `mono-OFL.txt` 一并保留在 `frontend/assets/`。生成的 A8 字形仍受字体许可约束，不能把原始项目 MIT 许可当成字体许可。来源：[IBM Plex](https://github.com/IBM/plex)、[Google Fonts 固定版本](https://github.com/google/fonts/tree/2eb0b48d5f760f62e286216f0859a8c540dbc1bd/ofl)。图集生成使用 Pillow 12.3.0，常规构建只使用已生成图集，不要求安装 Pillow。

## K40的q35接线参考

固定QEMU v11.1.2的普通根槽PIRQ映射与寄存器事实参考[ICH9 LPC模型](https://github.com/qemu/qemu/blob/v11.1.2/hw/isa/lpc_ich9.c)及[芯片声明](https://github.com/qemu/qemu/blob/v11.1.2/include/hw/southbridge/ich9.h)。前者文件保留MIT许可说明；本项目自行编写小范围路由、PIC操作和IRQ分派，没有导入QEMU设备模型作为内核实现或新增运行依赖。真实主板不能套用该模型公式。

K41 的CPU状态与调用约定参考 Intel SDM FXSAVE/FXRSTOR条目（https://cdrdv2-public.intel.com/812383/253666-sdm-vol-2a.pdf）及x86-64 psABI的寄存器约定（https://gitlab.com/x86-psABIs/x86-64-ABI/-/blob/master/x86-64-ABI/low-level-sys-info.tex）；没有导入其代码。QEMU v11.1.2 fpu_helper.c（https://github.com/qemu/qemu/blob/v11.1.2/target/i386/tcg/fpu_helper.c）用于对照本轮TCG保留位故障差异和FXSAVE对齐检查，未复制实现或增加运行依赖。

K42 的FS寻址、Linux用法和FSGSBASE允许边界参考 Linux内核文档（https://docs.kernel.org/arch/x86/x86_64/fsgs.html）与Intel指导（https://www.intel.com/content/www/us/en/developer/articles/technical/software-security-guidance/best-practices/guidance-enabling-fsgsbase.html），只用于机制核对，未导入实现或新增依赖。
