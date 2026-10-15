#!/bin/sh
set -eu # 任一步失败即停止，未设置变量也失败。
if [ "$(uname -s)" != Linux ]; then # 当前内核必须是真正Linux。
    echo 'Linux is required; host execution is not a Linux validation.' >&2 # 不把Mac运行当对照证据。
    exit 77 # 明确未验证。
fi # 完成环境检查。
mkdir -p artifacts # 保存本次环境、输出和GDB记录。
printf '{"passed":false,"phase":"p2"}\n' > artifacts/result.json # 先覆盖旧成功，后续失败不能误用旧结果。
uname -a > artifacts/environment.txt # 记录实际内核和机器架构。
getconf PAGE_SIZE >> artifacts/environment.txt # 记录Linux实际页大小。
cc --version >> artifacts/environment.txt # 记录本轮宿主C编译器。
cc -std=c11 -O0 -g -Wall -Wextra -Werror memory.c -o memory # 普通libc程序，不使用AiriOS对象。
if ./memory > artifacts/memory.log 2>&1; then # 保存真实运行退出状态，不经pipeline丢失它。
    cat artifacts/memory.log # 将已完成输出交回调用者。
else # 程序失败也保留本轮输出。
    status=$? # 第一时间取得失败程序退出码。
    cat artifacts/memory.log >&2 # 回显具体失败位置。
    exit "$status" # 保留实际失败状态。
fi # 程序已确认映射和SIGSEGV路径。
command -v gdb > artifacts/gdb.log # 缺调试器时明确失败，不能跳过后宣称全通过。
gdb -q -batch -ex 'set pagination off' -ex 'break main' -ex run -ex 'bt' -ex 'info proc mappings' -ex continue ./memory >> artifacts/gdb.log 2>&1 # 真正停在main再运行完整实验。
printf '{"passed":true,"phase":"p2"}\n' > artifacts/result.json # 程序与调试器都正常结束后才发布成功。
