#!/bin/sh
set -eu # 未定义变量或命令失败都停止。
if [ "$(uname -s)" != Linux ] || [ "$(uname -m)" != x86_64 ]; then # 固定个人x86-64 Linux观察路径。
    echo 'x86-64 Linux is required for epoll/TCP observation.' >&2 # 不用Mac模拟Linux。
    exit 77 # 未验证。
fi # 结束环境检查。
mkdir -p artifacts # 本阶段独立证据目录。
printf '{"passed":false,"phase":"p5-epoll"}\n' > artifacts/result.json # 先撤销旧成功。
for required in cc strace timeout; do # 核心编译、观察和运行上界缺一不可。
    if ! command -v "$required" >/dev/null 2>&1; then # 先给具体修复方法，不运行半个实验。
        echo "Missing $required. Debian/Ubuntu: apt install build-essential strace coreutils; Arch/CachyOS: pacman -S base-devel strace coreutils." >&2 # 这里只提示，不擅自提权安装。
        exit 77 # 依赖未满足，不能写passed。
    fi # 当前工具存在才继续。
done # 所需工具都已找到。
uname -a > artifacts/environment.txt # 实际Linux与架构。
cc --version >> artifacts/environment.txt # 实际C11编译器。
strace --version >> artifacts/environment.txt # 跟踪器必须真实存在。
timeout --version >> artifacts/environment.txt # GNU coreutils总运行上界，缺工具不能无限等待。
getconf GNU_LIBC_VERSION >> artifacts/environment.txt 2>&1 || true # 非glibc系统可以缺此项，其他环境事实仍保存。
cc -std=c11 -O0 -g -Wall -Wextra -Werror epoll.c -o epoll # 编译真实Linux epoll路径。
if timeout -k 2 30 strace -f -o artifacts/epoll-syscalls.log -e trace=epoll_create1,epoll_ctl,epoll_wait,sendto,recvfrom,shutdown ./epoll > artifacts/epoll.log 2>&1; then # 跟踪本机TCP和就绪调用，整个观察最多三十秒。
    cat artifacts/epoll.log # 完成后回显真实输出。
else # 超时、程序错误和工具失败都保留证据。
    status=$? # 不被后续cat覆盖。
    cat artifacts/epoll.log >&2 # 具体失败原因。
    exit "$status" # 不发布passed。
fi # 程序正常退出。
grep -q '^\[LINUX_EPOLL_OK\]' artifacts/epoll.log # 不能仅以strace进程存在当成功。
printf '{"passed":true,"phase":"p5-epoll","airios_network_stack_implemented":false}\n' > artifacts/result.json # Linux观察不冒充AiriOS网络栈。
