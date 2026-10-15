# Linux P1：main 与原始 syscall

完整程序：[ordinary.c](ordinary.c)、[raw_start.S](raw_start.S)；执行与 GDB 检查：[run.sh](run.sh)。需要 x86-64 Linux、cc、readelf、objdump、nm、GDB、Python3。macOS 不可直接运行这个 Linux raw ELF。

从主仓库运行 `./airios linux-lab`，默认个人主机 airi@ssh.airi-dynamic.top。脚本把这些源码放入独立 `~/AiriOS-labs/p1`，构建、运行、单步 syscall 并取回 artifacts/linux-p1 记录。若已经在 x86-64 Linux 本地，进入此目录运行 `sh run.sh`。

ordinary 输出 main(argc=3)；raw 输出 `raw: _start -> syscall(write) -> syscall(exit)`。run.sh 还验证 raw 无解释器/未定义符号，GDB 单步 write 的返回值等于写入字节数，下一 RIP 对应 airi_after_write。正常程序入口来自运行时，main 的返回由运行时转成退出行为。

详细寄存器、初始栈、调试与完整操作见 [Linux对照](../../../dev-log/28-linux-debug.md)。这两个程序运行于 Linux，不是 AiriOS 用户程序。
