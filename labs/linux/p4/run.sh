#!/bin/sh
set -eu # 未定义变量或失败命令都停止。
if [ "$(uname -s)" != Linux ] || [ "$(uname -m)" != x86_64 ]; then # strace步骤固定个人x86-64 Linux。
    echo 'x86-64 Linux is required for this storage experiment.' >&2 # 不用Mac结果替代。
    exit 77 # 环境不符不是成功。
fi # 结束环境判断。
mkdir -p artifacts # 所有本次证据在独立实验目录。
printf '{"passed":false,"phase":"p4"}\n' > artifacts/result.json # 先撤掉旧成功身份。
uname -a > artifacts/environment.txt # 真正内核和架构。
cc --version >> artifacts/environment.txt # 实际编译器。
strace --version >> artifacts/environment.txt # 缺工具直接失败，不能伪造跟踪。
if command -v findmnt >/dev/null 2>&1; then # util-linux存在时补充/tmp实际文件系统。
    findmnt -T /tmp >> artifacts/environment.txt # 文件系统能力影响O_DIRECT与同步。
fi # 不是必须安装新工具才能进行核心实验。
cc -std=c11 -O0 -g -Wall -Wextra -Werror io.c -o io # 普通Linux C程序使用本机libc。
if strace -f -o artifacts/syscalls.log -e trace=openat,close,read,write,pread64,pwrite64,preadv,pwritev,fsync,fdatasync,renameat,unlinkat,fallocate,statx,alarm,rt_sigaction,mmap,msync,munmap,mincore ./io > artifacts/io.log 2>&1; then # 一次真实运行同时保留库输出与Linux边界。
    cat artifacts/io.log # 只有命令完成后回显结果。
else # strace或程序失败都不发布passed。
    status=$? # 在其他命令前保存失败码。
    cat artifacts/io.log >&2 # 具体错误供定位。
    exit "$status" # 交回实际失败。
fi # 核心程序已正常退出。
grep -q '^\[LINUX_P4_OK\]' artifacts/io.log # 还必须出现完整路径结束标记。
direct=false # 可选能力默认未验证。
if grep -q '^\[DIRECT_OK\]' artifacts/io.log; then # 仅实际执行成功才改状态。
    direct=true # JSON布尔字面量，不是带引号文本。
fi # 能力差异独立保存。
printf '{"passed":true,"phase":"p4","direct_verified":%s,"physical_power_loss_tested":false}\n' "$direct" > artifacts/result.json # 核心路径通过不等于所有FS能力通过。
