#!/bin/sh
set -eu  # -e使非零简单命令停止，-u拒绝未定义变量；管道状态仍按shell自己的规则。
cd "$(dirname "$0")"  # $0是当前脚本路径，dirname取所在目录，命令替换结果引用为单个目录参数。
mkdir -p artifacts  # 创建本次Linux输出目录，已有目录保留。
cc -std=c11 -O0 -g -Wall -Wextra -Werror ordinary.c -o ordinary  # 普通编译驱动加入C运行时和libc，O0/g便于观察main参数。
cc -nostdlib -static -no-pie -Wl,-e,_start -g raw_start.S -o raw  # 排除运行时/库，固定地址静态链接；-Wl把入口选项传给链接器。
./ordinary one two > artifacts/ordinary-output.txt  # 两个用户参数连同argv[0]形成argc=3，输出写文件。
./raw > artifacts/raw-output.txt  # fd=1被Shell重定向到文件，原始write不需知道它不是终端。
uname -srm > artifacts/environment.txt  # 保存系统名、版本和机器架构，便于限定实验环境。
cc --version >> artifacts/environment.txt  # >>追加编译器版本，不覆盖前一行环境。
readelf -h -l -d -s ordinary > artifacts/ordinary-elf.txt  # 保存普通程序ELF头、段、动态信息与符号。
readelf -h -l -d -s raw > artifacts/raw-elf.txt  # 保存无运行时程序的相同四类信息。
objdump -d raw > artifacts/raw-disassembly.txt  # 反汇编代码节，观察寄存器参数与syscall指令。
nm -u raw > artifacts/raw-undefined.txt  # 只保存未定义符号列表，当前目标应为空。
test ! -s artifacts/raw-undefined.txt  # -s检测非空，!取反，确认没有未解析符号。
if readelf -l raw | grep -q INTERP; then exit 1; fi  # 若程序段含解释器，则拒绝当前独立静态目标；grep -q只以状态报告匹配。
# 以下完整续行命令按-ex逐项送GDB；$寄存器文本置单引号，不由Shell展开；标准错误并入记录。
# 普通程序start停main；raw的starti停机器入口，先看初始栈，再停write前并si跨越syscall。
gdb -q -nx -batch ./ordinary \
    -ex 'set pagination off' -ex 'set startup-with-shell off' \
    -ex 'start one two' -ex 'info registers rip rsp rdi rsi' \
    -ex 'print argc' -ex 'backtrace' -ex 'continue' \
    > artifacts/ordinary-gdb.txt 2>&1
# 以下完整续行命令按-ex逐项送GDB；$寄存器文本置单引号，不由Shell展开；标准错误并入记录。
# 普通程序start停main；raw的starti停机器入口，先看初始栈，再停write前并si跨越syscall。
gdb -q -nx -batch ./raw \
    -ex 'set pagination off' -ex 'set startup-with-shell off' \
    -ex 'set displaced-stepping off' \
    -ex 'starti one two' -ex 'info registers rip rsp' -ex 'x/4gx $rsp' \
    -ex 'break airi_before_write' -ex 'continue' \
    -ex 'info registers rip rax rdi rsi rdx rcx r11' \
    -ex 'x/47cb $rsi' \
    -ex 'printf "AIRI_RAW_BEFORE number=%ld fd=%ld length=%ld\n", $rax, $rdi, $rdx' \
    -ex 'si' -ex 'info registers rip rax rcx r11' \
    -ex 'printf "AIRI_RAW_AFTER result=%ld next=%#lx expected=%#lx\n", $rax, $rip, &airi_after_write' \
    -ex 'continue' \
    > artifacts/raw-gdb.txt 2>&1
python3 - <<'PY'  # 带引号终止标记禁止Shell展开正文中的$或反斜杠，内容直接交Python。
from pathlib import Path  # Path把artifacts表示为可拼接的路径对象。
import json  # 成功结果字典编码为JSON文件。
p=Path('artifacts')  # 相对实验脚本目录定位本轮输出。
assert 'main(argc=3)' in (p/'ordinary-output.txt').read_text()  # 普通程序必须实际收到程序名和两个参数。
assert (p/'raw-output.txt').read_text()=='raw: _start -> syscall(write) -> syscall(exit)\n'  # 原始write必须写出完整且唯一的一行。
raw=(p/'raw-gdb.txt').read_text()  # 读取本次GDB机器入口观察文本。
length=len((p/'raw-output.txt').read_bytes())  # 输出文件按字节计算长度，匹配write的单位。
assert f'AIRI_RAW_BEFORE number=1 fd=1 length={length}' in raw  # syscall前应为write调用号、stdout与精确字节数。
assert f'AIRI_RAW_AFTER result={length}' in raw  # syscall后rax应报告完整写入的字节数。
import re  # 正则从GDB记录提取下一指令与预期标签地址。
match=re.search(r'AIRI_RAW_AFTER result=\d+ next=(0x[0-9a-f]+) expected=(0x[0-9a-f]+)',raw)  # 两个括号捕获十六进制地址，未找到返回None。
assert match and match.group(1)==match.group(2)  # 先确认匹配存在，再比较两个捕获组，保证单步停在预期下一指令。
assert 'exited normally' in raw  # 原始程序必须正常以零状态结束。
assert 'argc' in (p/'ordinary-gdb.txt').read_text()  # 普通程序记录应包含main参数观察。
result={'passed':True,'ordinary_argc':3,'raw_write_bytes':length,  # 仅全部断言通过才建立成功结果，字节数来自真实文件。
        'raw_has_interpreter':False,'raw_undefined_symbols':False,  # 保存前面ELF与nm实际核对的两个约束。
        'gdb_syscall_stepped':True}  # 地址和寄存器记录共同确认syscall单步观察。
(p/'result.json').write_text(json.dumps(result,indent=2)+'\n')  # 字典转JSON，保存这次实际验证结果。
print('PASS Linux P1: normal CRT/main and raw _start/syscall, ELF + GDB verified')  # 完整输出、ELF和GDB均成立后才打印。
PY
