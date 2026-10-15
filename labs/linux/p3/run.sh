#!/bin/sh
set -eu # 命令失败或未定义变量都停止。
if [ "$(uname -s)" != Linux ] || [ "$(uname -m)" != x86_64 ]; then # 本节GDB寄存器步骤固定x86-64 Linux。
    echo 'x86-64 Linux is required for this process/GDB experiment.' >&2 # 其他环境不假装通过。
    exit 77 # 明确未验证。
fi # 完成环境检查。
mkdir -p artifacts # 仅本实验输出目录。
printf '{"passed":false,"phase":"p3"}\n' > artifacts/result.json # 先抹掉旧成功身份。
uname -a > artifacts/environment.txt # 实际内核与架构。
cc --version >> artifacts/environment.txt # 实际编译器。
gdb --version >> artifacts/environment.txt # 实际调试器版本。
cc -std=c11 -O0 -g -Wall -Wextra -Werror process.c -o process # 普通Linux程序链接Linux运行时。
readelf -h -l process > artifacts/elf.txt # 保存Linux程序真正的入口和装载段。
if ./process > artifacts/process.log 2>&1; then # 不经pipeline丢失程序退出码。
    cat artifacts/process.log # 回显已完成的真实输出。
else # 失败也保留日志。
    status=$? # 立即保存失败码。
    cat artifacts/process.log >&2 # 给出具体失败位置。
    exit "$status" # 不写成功。
fi # 正常运行已验证父子与FD路径。
: > artifacts/observe.gdb # 冒号是无操作命令，重定向先清空旧GDB脚本。
printf '%s\n' 'set pagination off' >> artifacts/observe.gdb # 输出不分页，批量模式不等待按键。
printf '%s\n' 'set follow-fork-mode child' >> artifacts/observe.gdb # 跟随孩子，父结果由独立正常运行验证。
printf '%s\n' 'catch exec' >> artifacts/observe.gdb # 只在真正成功exec后停。
printf '%s\n' 'commands' >> artifacts/observe.gdb # 开始为刚建立的捕捉点定义动作。
printf '%s\n' 'silent' >> artifacts/observe.gdb # 省去重复停止提示。
printf '%s\n' 'info inferiors' >> artifacts/observe.gdb # 观察被跟随孩子的实际PID。
printf '%s\n' 'info registers rip rsp rdi rsi' >> artifacts/observe.gdb # 停点可能在动态装载器，RDI不能直接当argc。
printf '%s\n' 'x/8gx $rsp' >> artifacts/observe.gdb # 读初始栈的argc及后续参数指针。
printf '%s\n' 'info proc mappings' >> artifacts/observe.gdb # 保存新程序与解释器映射。
printf '%s\n' 'printf "[GDB_EXEC_STOP]\n"' >> artifacts/observe.gdb # 只有真正命中并完成观察才发布标记。
printf '%s\n' 'continue' >> artifacts/observe.gdb # 恢复孩子执行直到退出。
printf '%s\n' 'end' >> artifacts/observe.gdb # 结束捕捉动作定义。
printf '%s\n' 'run' >> artifacts/observe.gdb # 启动这份独立调试运行。
# 固定GDB输出语言；目标exec的envp仍由C程序自己提供。
LC_ALL=C gdb -q -batch -x artifacts/observe.gdb ./process > artifacts/gdb.log 2>&1 # 批量执行明确写出的观察脚本。
# 该精确单行只能来自真正命中exec后的动作，不接受只有catchpoint设置成功。
grep -Fxq '[GDB_EXEC_STOP]' artifacts/gdb.log # -F字面匹配，-x整行，-q只交退出码。
printf '{"passed":true,"phase":"p3"}\n' > artifacts/result.json # 只有正常程序和调试命令都完成才发布。
