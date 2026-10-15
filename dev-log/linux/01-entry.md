# L01 · 对照 Linux 的函数调用、系统调用与内核入口

AiriOS 已直接在内核里写 UART。普通 Linux 程序输出同样一行文字，却不能像当前内核那样直接执行设备访问；它需要请求 Linux 内核代办。这里先观察 `./ordinary one two` 在 `main` 收到的参数，再让 `raw` 自己准备一次写请求并核对返回值，最后回到 AiriOS 的汇编入口，观察 C 函数赖以执行的栈。

源码起点是 K13。两份 Linux 程序在个人主机 `airi@ssh.airi-dynamic.top` 的 `~/AiriOS-labs/p1` 目录运行；AiriOS 内核仍在本机 QEMU 中执行。远端需要 x86-64 Linux、C 编译器 `cc`、GNU Binutils 的 `readelf/objdump/nm`、GDB 和 Python 3；本机继续使用前面建立的构建环境，并需 Python 3.12 以上、SSH 与 scp。Apple Silicon 上的 macOS 不能直接执行下面生成的 Linux ELF。

远端脚本使用已有的 SSH 密钥与访问配置，`BatchMode=yes` 禁止停下来询问密码。账户尚不能非交互登录、远端工具缺失或远端不是 x86-64 时，后面的运行会失败；失败状态与本轮实际输出决定是否完成观察。本章不把连接成功等同于程序实验成功。

<a id="runtime-main"></a>

## 普通 C 程序从运行时进入 main

链接普通 C 程序时，编译驱动还加入运行时启动代码和 C 标准库 libc。启动代码负责把进程刚装载好的状态转换成 C 函数可以使用的状态。如果生成的 ELF 带有 `PT_INTERP` 段，Linux 先把执行权交给其中指定的动态装载器；装载器准备所需共享库，再进入程序自己的 ELF 入口。没有解释器的静态程序则直接从自己的入口开始。这一分支可以在后面保存的 `ordinary-elf.txt` 中核对，规则见 [execve 手册](https://man7.org/linux/man-pages/man2/execve.2.html)。

程序的运行时从初始栈取得参数，建立 `argc/argv`，再按 C 调用约定进入 `main`。`main` 不是内核最初交出执行权的位置；它返回后，运行时还会处理退出工作并将状态交给内核。

`argc` 是参数个数，`argv` 是字符串指针数组，每个字符串用 NUL 结束，数组的 `argv[argc]` 则是空指针。本次命令 `./ordinary one two` 让 `main` 收到 `argc=3`，前三项分别指向 `"./ordinary"`、`"one"`、`"two"`。`argv[0]` 通常由启动者写成命令名称，并不是内核从可执行文件路径强制推导出来的值。

`printf` 按 `%d/%s/%ld` 分别消费整数、字符串指针和 `long`，格式化后交给 libc 的标准输出流 `stdout`。这个流可能先缓存字节，再请求内核写出，所以一次 `printf` 不必对应一次原始系统调用。程序还调用 `getpid` 取得当前 Linux 进程号；它只用于辨认这次运行，与 AiriOS 尚未实现的任务编号无关。

**新建 `labs/linux/p1/ordinary.c`：**

<a id="code-labs-linux-p1-ordinary-c-1"></a>

<!-- write: labs/linux/p1/ordinary.c -->
```c
#include <stdio.h>   // 取得主机 printf、文件输出与错误报告接口。
#include <unistd.h>  // 取得 Linux getpid 声明。

int main(int argc, char **argv) {  // 宿主运行时传入参数数量和 NUL 字符串指针数组。
    printf("ordinary: main(argc=%d), argv[0]=%s, pid=%ld\n",  // 宿主libc格式化参数数、启动名称和当前Linux进程号。
           argc, argv[0], (long)getpid());  // argc/argv来自运行时，getpid返回值转long匹配%ld。
    puts("ordinary: libc -> Linux write -> stdout");  // libc写出这行并补换行，实际系统调用数量取决于缓冲。
    return 0;  // 正常结束当前程序，成功状态交给启动它的命令。
}
```

第二行由 `puts` 固定写出，用来标记程序采用的输出路径。`printf/puts` 实际触发了几次内核写请求，需要系统调用跟踪才能观察；本章的寄存器单步针对后面自行准备请求的 `raw`。

## 自定义入口直接请求 Linux 写出字节

<a id="syscall-boundary"></a>

普通函数调用在当前用户程序的权限下转入另一段代码。系统调用则让用户程序请求内核服务：`syscall` 指令进入内核预设的入口，Linux 根据调用号和寄存器参数选择服务，再把结果交回用户程序。本次请求是“将这 47 个字节写到标准输出”；程序不需要自己驱动终端，也不需要知道标准输出后来被接到了文件还是管道。

文件描述符 fd 是当前进程持有的整数句柄。1 通常指标准输出，Linux 据此找到已打开的输出对象；它不是 UART 端口 0x3f8，也不是内存地址。write 的字符串地址仍在程序地址空间，内核按长度读取字节并写到句柄对应对象，返回成功字节数；原始系统调用出错时返回负的错误编号。这是直接执行指令时读取的结果，C 库接口还会转换错误表示，区别见 [Linux 系统调用概述](https://man7.org/linux/man-pages/man2/intro.2.html)。

Linux x86-64 原始入口的 RSP 指向 `argc`，后面依次保存 `argv` 的指针；C 参数寄存器尚未自动设置。本例不调用 `main`，也不读取参数，而是让自定义 `_start` 将 write 号 1 放 RAX，标准输出 fd=1 放 RDI，文本地址放 RSI，长度 47 放 RDX，再执行 `syscall`。Linux x86-64 的寄存器分配见 [syscall 手册](https://man7.org/linux/man-pages/man2/syscall.2.html)。

Linux 内核返回结果到 RAX，RCX/R11 被调用机制改写。普通 System V C 调用第四整数参数使用 RCX，Linux syscall 第四参数使用 R10，本例仅使用三项。`.ascii` 按给定文字保存字节，write 用明确长度。AiriOS 的 outb 则直接写设备端口，三种调用各有执行位置。

写完后用调用号 60 请求退出。这个原始调用终止当前线程；本例没有创建其他线程，所以整个进程随之结束。它和 C 库负责终止所有线程的退出接口存在范围差异，见 [Linux退出调用说明](https://man7.org/linux/man-pages/man2/_exit.2.html)。

**新建 `labs/linux/p1/raw_start.S`：**

<a id="code-labs-linux-p1-raw-start-s-1"></a>

<!-- write: labs/linux/p1/raw_start.S -->
```asm
/* x86-64 Linux only. No libc, no main, no CRT startup. */
.section .rodata  /* 文字字节放在只读数据节，由ELF段映射供write读取。 */
message:  /* 标签绑定下一字节地址，本身不输出字节。 */
    .ascii "raw: _start -> syscall(write) -> syscall(exit)\n"  /* 原样存字节，含末尾换行但不自动加NUL。 */
.set message_length, .-message  /* .为当前汇编位置，减起点得到消息字节数；.set绑定这个常量。 */

.section .text  /* 后续机器指令放入可执行代码节。 */
.global _start, airi_before_write, airi_after_write  /* 入口与观察标签导出，链接器/GDB可按名称找到。 */
.type _start, @function  /* 符号表把_start标为函数，供反汇编和调试工具识别。 */
_start:  /* ELF入口从此开始；RSP仍指向Linux准备的argc和指针数组。 */
    mov $1, %eax  /* $表示立即数，写eax会清零rax高32位；调用号1选择write。 */
    mov $1, %edi  /* 第一整数参数rdi取fd=1，通常是当前进程标准输出。 */
    lea message(%rip), %rsi  /* 以RIP相对地址形成message指针，lea不读取消息内容。 */
    mov $message_length, %edx  /* 第三个参数为需写出的字节数，不靠NUL找末尾。 */
airi_before_write:  /* GDB在这里停下，寄存器已经准备好而请求尚未进入内核。 */
    syscall  /* 转入Linux服务入口；rax变为返回值，rcx/r11被机制改写。 */
airi_after_write:  /* 返回后下一指令地址，用于核对GDB单步确实跨过syscall。 */
    cmp $message_length, %rax  /* 比较实际成功字节数与要求长度，短写或负错误都不相等。 */
    jne failed  /* 不相等跳到失败退出，此小演示拒绝短写而不实现重试循环。 */
    xor %edi, %edi  /* 同一寄存器异或得到零，第一退出参数为成功状态0。 */
    jmp exit_now  /* 成功也进入统一退出调用，不返回未知调用者。 */
failed:  /* write没完整完成时从这里建立失败状态。 */
    mov $1, %edi  /* 退出状态1交给Shell，不能因打印部分文字就算成功。 */
exit_now:  /* 两种结果在此共享Linux退出请求。 */
    mov $60, %eax  /* x86-64 Linux调用号60为exit。 */
    syscall  /* 内核结束当前进程，此调用正常情况下不返回。 */
.size _start, .-_start  /* 当前位置减入口得到函数机器字节长度，保存为符号元数据。 */
.section .note.GNU-stack,"",@progbits  /* 空标记节声明不需要可执行栈，供链接器形成栈权限。 */
```

现在将两份源码变成 Linux 可执行文件。普通程序使用编译驱动默认加入的运行时；`raw` 的 `-nostdlib` 排除启动文件和默认库，`-static` 不使用共享库，`-no-pie` 生成固定地址的可执行文件，`-Wl,-e,_start` 将入口选择传给链接器。`readelf` 读取 ELF 的头和装载信息，`objdump` 把机器指令显示为汇编，`nm -u` 只列未解决的外部符号。

脚本先构建和直接运行，再启动 GDB 重新运行两份程序。直接运行 `raw` 时，Shell 把 fd=1 接到 `artifacts/raw-output.txt`；GDB 内的那次运行则把同一行文字送进调试记录。两次执行共用代码，参数与进程状态各自重新建立。

**新建 `labs/linux/p1/run.sh`：**

<a id="code-labs-linux-p1-run-sh-1"></a>

<!-- write: labs/linux/p1/run.sh -->
```sh
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
```

直接执行的 `raw` 必须写出完整 47 字节才以 0 退出。返回负错误或只写出一部分时，它都选择状态 1；脚本因 `set -e` 在这个普通命令失败处停止，后续 GDB 不会继续执行。这里还没有一般用途的短写重试循环。`set -u` 拒绝读取未定义变量；`-e` 在 `if` 条件等位置有例外，例如上面的 `grep` 未匹配时会进入正常的下一步，不能把 `-e` 理解为任何非零状态都会退出。

接下来让 GDB 观察同一写请求的边界。`-q` 减少启动文字，`-nx` 不读取个人初始化文件，`-batch` 执行 `-ex` 给出的各条命令后退出。`start one two` 在 `main` 处停下，参数仍是 `one two`；`starti one two` 在本次执行的第一条机器指令停下。由于 `raw` 没有解释器，后者就是自己的 `_start`。动态程序的 `starti` 可能先停在装载器中，这两种停法的区别见 [GDB 启动命令](https://sourceware.org/gdb/current/onlinedocs/gdb.html/Starting.html)。

`info registers` 读取指定寄存器的当前值，`print argc` 则用调试信息找到 C 参数，`backtrace` 显示当前能够还原的函数调用帧。`break airi_before_write` 安装断点，`continue` 让程序继续到断点或结束。`set startup-with-shell off` 让 GDB 直接启动目标程序，避免个人 Shell 的初始化和展开规则介入参数。分页关闭后，批处理也不会停下来等待翻页。

`x/4gx $rsp` 从初始栈顶读四个八字节值，`4` 是数量，`g` 是八字节单位，`x` 是十六进制格式。本次第一个值是 3，随后三个值是参数字符串的地址。到 `airi_before_write` 时，`x/47cb $rsi` 按字符格式 `c` 读取 47 个单字节 `b`；`.ascii` 没有追加 NUL，定长观察避免越过消息尾端继续寻找零字节。

`si` 执行一条机器指令。在 Linux 用户进程里单步 `syscall`，这里观察的是内核处理完请求后返回用户态的位置，不会进入 Linux 内核源码逐条停下。返回时 RAX 应为 47，RIP 应指向 `airi_after_write`。脚本关闭 `displaced-stepping`，让 GDB 越过断点时不改为在别处执行该指令的副本；这项设置的含义见 [GDB 单步设置](https://sourceware.org/gdb/current/onlinedocs/gdb.html/Maintenance-Commands.html)。Shell 单引号保护 `$rsp/$rsi` 等 GDB 表达式，`2>&1` 把标准错误也写进同一记录。

**追加到 `labs/linux/p1/run.sh` 末尾：**

<a id="code-labs-linux-p1-run-sh-2"></a>

<!-- append: labs/linux/p1/run.sh -->
```sh
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
```

最后把输出、寄存器与停点一起核对。`re.search` 从一行记录中取得 `next` 和 `expected` 两个十六进制地址；正则里的两个括号形成捕获组，`group(1)/group(2)` 分别取值。匹配失败返回 `None`，`match and ...` 会先拒绝它，再比较两个地址。只有所有断言成立才写 `result.json`。

Python 正文通过 here-doc 送入标准输入；`<<'PY'` 中的引号使 Shell 不展开正文里的 `$`、反斜杠等内容。结尾 `PY` 必须独占一行，并从行首开始。它属于 Shell 的结束标记，不是 Python 语句。

**追加到 `labs/linux/p1/run.sh` 末尾：**

<a id="code-labs-linux-p1-run-sh-3"></a>

<!-- append: labs/linux/p1/run.sh -->
```sh
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
```

两个程序与观察脚本已经齐全，本机还需要把它们送到 Linux。`linux_lab.py` 先通过 SSH 创建实验目录，再用 scp 传三份源码；它不从本机上传已经编译好的二进制。SSH 的参数列表让本机进程保持实参边界，但列表最后的远端命令仍由远端 Shell 解释，目录文字还需要 `shlex.quote`。

**新建 `scripts/linux_lab.py`：**

<a id="code-scripts-linux-lab-py-1"></a>

<!-- write: scripts/linux_lab.py -->
```python
"""Run the authorized P1 programs on the user's personal Linux host."""  # 从本机发起个人 Linux 主机上的两份实验
import argparse  # 命令行解析器把字符串选项转换为带名称的属性。
import io     # BytesIO 为内存字节提供文件式读写接口。
import json   # 在 Python 对象与 JSON 文本之间转换，不执行其中内容。
import shlex  # 按 Shell 引用规则拆分或引用文本，区别于实际执行。
import subprocess  # 以参数列表启动已有外部程序，并读取输出或退出码。
import tarfile     # 读取或构造 tar 归档，成员路径及类型还需单独检查。
from tools import ROOT  # 共享 ROOT 指向项目根目录，后续路径不依赖调用位置。

parser=argparse.ArgumentParser()  # 主机参数解析器提供--host选项与用法提示。
parser.add_argument('--host',default='airi@ssh.airi-dynamic.top')  # 个人Linux主机可以显式替换，不涉及公司机器。
args=parser.parse_args()  # 把进程参数转为Namespace，后续通过 args.host 取得目标主机字符串。
if args.host.startswith('-') or any(c.isspace() for c in args.host):  # 禁止选项形状和空白，避免host被外部工具误解析；生成器逐字符检查空白。
    raise RuntimeError('Invalid SSH host')  # 无效目标在启动SSH之前拒绝。
ssh=['ssh','-o','BatchMode=yes','-o','ConnectTimeout=10',args.host]  # 非交互认证，连接最多十秒，最后元素是单个host参数。
# Relative to SSH's login directory; does not overwrite any other project.
remote='AiriOS-labs/p1'  # 相对个人主机登录目录的独立实验目录。
subprocess.run(ssh+[f'mkdir -p {shlex.quote(remote)}'],check=True)  # 本地参数列表启动SSH，远端命令文本中的目录由quote保护。
sources=[ROOT/'labs/linux/p1'/name for name in ('ordinary.c','raw_start.S','run.sh')]  # 列表推导按三个名称构成三项Path对象。
subprocess.run(['scp','-q','-o','BatchMode=yes','-o','ConnectTimeout=10',  # scp安静且非交互传输，连接上限同SSH。
                *[str(p) for p in sources],f'{args.host}:{remote}/'],check=True)  # 列表内*展开三个路径字符串，最后一项是远端目标。
execution=subprocess.run(ssh+[f'cd {shlex.quote(remote)} && sh run.sh'])  # 远端先切实验目录，再执行shell；暂不check，失败也先下载证据。
```

`execution` 保存远端 Shell 的退出状态。这里暂不使用 `check=True`，因为程序可能已经运行到一半，仍需要取回已经产生的输出。下一段让远端 `tar` 将整个 `artifacts` 目录编码为归档字节，SSH 把这些字节带回本机；`io.BytesIO` 为字节提供文件式读取接口，供 `tarfile` 解析。

本机解包位置是 `artifacts/linux-p1`，归档本身仍含一层 `artifacts/`，所以输出文件实际在 `artifacts/linux-p1/artifacts/`。`filter='data'` 按 Python 的数据归档规则限制解包成员。若远端非零，取回记录后仍报告失败，不会读取旧 `result.json` 作为成功。

**追加到 `scripts/linux_lab.py` 末尾：**

<a id="code-scripts-linux-lab-py-2"></a>

<!-- append: scripts/linux_lab.py -->
```python
data=subprocess.check_output(ssh+[f'cd {shlex.quote(remote)} && tar -cf - artifacts'])  # tar从标准输出交回归档bytes，-f -表示不写远端归档文件。
dest=ROOT/'artifacts/linux-p1'  # 本机证据保存目录与远端独立。
dest.mkdir(parents=True,exist_ok=True)  # 确保证据父目录存在。
with tarfile.open(fileobj=io.BytesIO(data)) as tar:  # 把内存归档作为文件交给tarfile，with结束关闭归档。
    tar.extractall(dest,filter='data')  # data过滤限制危险成员，解包到本轮证据目录。
if execution.returncode:  # 非零远端状态意味着实验失败，即使tar传输成功。
    raise RuntimeError(f'Linux experiment failed; evidence downloaded to {dest}')  # 原始证据已经带回，再报告失败位置。
result=json.loads((dest/'artifacts/result.json').read_text())  # 实际远端程序写出的JSON恢复成字典。
if result.get('passed') is not True:  # get缺键返回None，只有布尔True才接受。
    raise RuntimeError('Remote validation failed')  # 远端记录不满足成功合同。
(dest/'remote.json').write_text(json.dumps({'host':args.host,'directory':remote},indent=2)+'\n')  # 记录本次实际host和目录，不把另一主机记录混用。
print(f'Remote evidence downloaded: {dest}')  # 显示本轮证据可查看位置。
```

## 沿自己的栈进入内核 C 函数

<a id="remote-debug"></a>

GDB 是远端 Linux 上的主机程序，实际栈和寄存器属于本机 QEMU 模拟的 CPU。QEMU 的 GDB stub 只在本机回环地址 `127.0.0.1` 监听，接受寄存器、内存和断点请求；SSH 反向转发负责把远端 GDB 的连接送到它。GDB 不作为 guest 内的线程执行。

调试时使用带符号的 ELF，GDB 才能把数值地址对应到函数和 C 类型。远端源码映射为调试器找到源文件，实际机器码仍由本机的启动镜像装入内存。

`./airios kernel-debug` 先在本机构建 normal 配置，随后脚本复制这份 ELF 与对应源码。`set substitute-path` 将 ELF 调试信息中的本机源码目录映射到远端 `source`，供 GDB 显示对应行。CPU 执行的仍是本机镜像里的机器码；若 ELF 与镜像不是同次构建，符号指向的地址就可能失配。

准备路径与复制完成后，脚本才启动本地 QEMU。前半段只建立远端观察材料，尚未启动虚拟机。

**新建 `scripts/kernel_debug.py`：**

<a id="code-scripts-kernel-debug-py-1"></a>

<!-- write: scripts/kernel_debug.py -->
```python
"""Use the user's existing x86-64 GDB to inspect a local QEMU guest over SSH."""  # 调试器在远端，被观察的虚拟 CPU 留在本机
import argparse  # 命令行解析器把字符串选项转换为带名称的属性。
import io      # BytesIO 为内存字节提供文件式读写接口。
import json    # 在 Python 对象与 JSON 文本之间转换，不执行其中内容。
import shlex   # 按 Shell 引用规则拆分或引用文本，区别于实际执行。
import socket  # 取得主机通信连接接口，QMP/调试器通过独立连接交接。
import subprocess  # 以参数列表启动已有外部程序，并读取输出或退出码。
import tarfile     # 读取或构造 tar 归档，成员路径及类型还需单独检查。
import time  # 单调时钟计算截止时间，sleep 给设备与进程处理时间。
from tools import ROOT    # 共享 ROOT 指向项目根目录，后续路径不依赖调用位置。
from boot import command  # 复用 QEMU 参数组装，本模块只额外接入调试路径。

parser=argparse.ArgumentParser()  # 主机参数解析器提供--host选项与用法提示。
parser.add_argument('--host',default='airi@ssh.airi-dynamic.top')  # 个人Linux主机可以显式替换，不涉及公司机器。
args=parser.parse_args()  # 把进程参数转为Namespace，后续通过 args.host 取得目标主机字符串。
if args.host.startswith('-') or any(c.isspace() for c in args.host):  # 禁止选项形状和空白，避免host被外部工具误解析；生成器逐字符检查空白。
    raise RuntimeError('Invalid host')  # 拒绝无效主机字符串，尚未启动网络程序。
work=ROOT/'artifacts/kernel-debug'      # 本机QEMU与GDB会话证据路径。
work.mkdir(parents=True,exist_ok=True)  # 建立本轮输出目录，已有目录允许复用。
remote='AiriOS-labs/p1/kernel-debug'    # 远端调试器源码及ELF单独放在此目录。
base=['ssh','-o','BatchMode=yes','-o','ConnectTimeout=10']  # 共同SSH选项不包含host，后续各命令追加。
remote_home=subprocess.check_output(base+[args.host,'pwd'],text=True).strip()  # 查询远端真实工作目录，text转文本、strip去末尾换行。
source_dir=remote+'/source'  # GDB查找源文件的位置，不是guest内核加载位置。
subprocess.run(base+[args.host,f'mkdir -p {shlex.quote(source_dir)}'],check=True)  # 创建独立远端源码目录，shell目录文字引用完整。
archive=io.BytesIO()  # 空内存字节流承接待发送tar，不落临时磁盘文件。
with tarfile.open(fileobj=archive,mode='w') as tar:  # 以写归档模式向内存流装入源码。
    for name in ('kernel','include'):  # 只选当前调试涉及的源码与类型声明。
        tar.add(ROOT/name,arcname=name)  # 本机路径加入归档，归档内部保留相对name目录。
subprocess.run(base+[args.host,f'tar -xf - -C {shlex.quote(source_dir)}'],  # SSH把下一行input交到远端tar标准输入，在指定目录解包。
    input=archive.getvalue(),check=True)  # getvalue取得完整归档bytes，远端非零时立即失败。
subprocess.run(['scp','-q','-o','BatchMode=yes','-o','ConnectTimeout=10',  # scp安静且非交互传输，连接上限同SSH。
    str(ROOT/'build/normal/airios.elf'),f'{args.host}:{remote}/airios.elf'],check=True)  # 复制普通配置带符号ELF，仅供远端GDB解释地址与类型。
```

端口 0 让本机为临时 socket 选择一个空闲端口；关闭临时 socket 后，再让 QEMU 使用这个编号。这个分配并不同时预留 Linux 那一端的端口，反向转发若遇到占用会因 `ExitOnForwardFailure=yes` 失败。`-R 127.0.0.1:端口:127.0.0.1:端口` 让远端 GDB 连接远端回环地址，SSH 再把连接送回本机 QEMU 的 GDB stub。

`hbreak *名称` 按符号的准确指令地址设硬件断点。第一次停在 `_start` 时还在使用引导器给的栈；继续到 `airi_limine_start` 后，汇编已经把 RSP 换到自己的 64 KiB 数组。进入 C 的 `call` 还压入了返回地址，所以此时 RSP 应严格小于 `airi_stack_top`、大于 `airi_stack_bottom`。这个比较确认当前栈指针的位置，没有建立溢出保护。

第三个断点停在 `airi_kernel_main` 的首条指令，参数还保存在 ABI 规定的 RDI 中。`((AiriBootInfo*)$rdi)->version` 将这个寄存器值解释为启动描述指针，再按结构字段偏移读取 `version`；`print/x` 让地址显示为十六进制。默认串口内核同样可能收到帧缓冲描述，读取 `framebuffer.width` 只说明引导数据里有什么，并不表示内核已绘图。

QEMU 的 `-S` 在 CPU 复位时暂停，GDB 连接后才让固件与引导器继续执行，直到命中内核断点。这个调试入口和虚拟地址观察行为见 [QEMU 的 GDB 说明](https://www.qemu.org/docs/master/system/gdb.html)。会话结束或失败后，`finally` 都会请求结束 QEMU，再等待并回收进程；GDB 的 `detach` 本身并不负责退出 QEMU。

**追加到 `scripts/kernel_debug.py` 末尾：**

<a id="code-scripts-kernel-debug-py-2"></a>

<!-- append: scripts/kernel_debug.py -->
```python
with socket.socket() as available:  # 临时TCP socket向主机申请当前可用端口。
    available.bind(('127.0.0.1',0))  # 回环地址端口0表示由系统选择空闲端口。
    port=available.getsockname()[1]  # 二元组第1项取得端口号，离开with后释放临时占用。
cmd=command('normal',work,debug=True,debug_port=port)  # 本地QEMU按该端口启动GDB stub并先暂停CPU。
with (work/'qemu.log').open('w') as log:  # QEMU主机输出写文件，离开with自动关闭。
    process=subprocess.Popen(cmd,stdout=log,stderr=log)  # 启动本地虚拟机并保留可轮询、终止的句柄。
    try:  # 等待监听和调试会话失败都需回收本地QEMU。
        deadline=time.monotonic()+5  # GDB监听准备最多等待五秒。
        while True:  # 重试本机监听探测，成功break，五秒截止时失败。
            if process.poll() is not None:  # poll 非 None 表示虚拟机已经结束，不能再等待监听
                raise RuntimeError('QEMU exited before debugger connection')  # 报告提前退出，把清理交给 finally
            try:  # 每次尝试连接，只把暂时的连接错误交给重试。
                with socket.create_connection(('127.0.0.1',port),timeout=0.2):  # 最多 200 毫秒试连接本机调试端口
                    pass  # 无须交换调试数据；退出 with 即关闭这次探测连接
                break  # 探测成功退出监听等待，开始实际GDB会话。
            except OSError:  # 尚未监听等连接失败暂时允许重试。
                if time.monotonic()>deadline:  # 重试已超过五秒总准备时间
                    raise RuntimeError('GDB listener timeout')  # 明确报告超时，不把等待中的 QEMU 算作成功
                time.sleep(0.1)  # 给虚拟CPU和日志写入100毫秒处理时间，再检查。
        expressions=[  # GDB命令列表按入口、换栈、C参数三段依次观察。
            'set pagination off', 'set confirm off',  # 批处理不分页且不等待交互确认。
            f'set substitute-path {ROOT} {remote_home}/{source_dir}',  # 把ELF内本机源路径映射到远端副本路径。
            f'target remote 127.0.0.1:{port}',  # GDB连接远端回环转发端口，最终到本机QEMU。
            'hbreak *_start', 'continue', 'info registers rip rsp cs', 'x/7i $rip',  # 入口硬件断点停CPU，读指令指针、栈和代码段，再看七条指令。
            'hbreak *airi_limine_start', 'continue', 'info registers rip rsp',  # 汇编换栈后停在适配C入口，观察当前栈指针。
            'printf "AIRI_STACK_OK=%d\\n", ($rsp>(unsigned long)&airi_stack_bottom)&&($rsp<(unsigned long)&airi_stack_top)',  # 地址比较核对RSP严格位于自有栈两端之间，&&要求同时成立。
            'hbreak *airi_kernel_main', 'continue',  # 在真正内核C入口停下，读取适配层交来的参数。
            'print ((AiriBootInfo*)$rdi)->version',  # RDI按System V保存首参数，转换为指针后读协议版本。
            'print ((AiriBootInfo*)$rdi)->region_count',  # 同一指针读取实际有效区域数。
            'print/x ((AiriBootInfo*)$rdi)->kernel_virtual_base',  # /x以十六进制输出内核虚拟起点。
            'print ((AiriBootInfo*)$rdi)->framebuffer.width',      # 读取实际图形宽度，不采用主机窗口配置。
            'printf "AIRI_KERNEL_ABI_OK=%d\\n", ((AiriBootInfo*)$rdi)->magic==0x41495249424f4f54',  # 参数必须指向有正确magic的启动描述，才说明当前ABI交接满足约定。
            'detach', 'quit']  # 脱离guest并结束批处理，进程清理由下面finally负责。
        gdb=['gdb','-q','-nx','-batch',f'{remote}/airios.elf']  # 不加载用户启动配置，按固定符号ELF执行批处理。
        for expr in expressions:  # 按已列出的顺序处理每条GDB文本命令。
            gdb += ['-ex', expr]  # 一对列表元素保持参数边界。
        try:  # 限时调试失败也应保留截止前已经收到的GDB输出。
            session=subprocess.run(base+['-o','ExitOnForwardFailure=yes',  # 转发端口建立失败必须让SSH非零退出。
                '-R',f'127.0.0.1:{port}:127.0.0.1:{port}',args.host,shlex.join(gdb)],  # 远端回环监听转回本机；join引用远端GDB各参数。
                capture_output=True,text=True,timeout=45)  # 两种输出均收集，整段会话最多45秒。
        except subprocess.TimeoutExpired as error:  # run超过45秒时先结束SSH子进程，再交出已收集输出。
            transcript=(error.stdout or b'')+(error.stderr or b'')  # TimeoutExpired持有bytes，没收到该路输出时以空bytes代替。
            (work/'gdb.txt').write_text(transcript.decode(errors='replace'))  # 解码本次部分记录，异常字节可见而不阻止保存。
            raise RuntimeError(f'Kernel debug timeout; see {work/"gdb.txt"}') from error  # 报告实际记录位置，finally继续回收本地QEMU。
        transcript=session.stdout+session.stderr  # 合并本轮GDB与SSH正常/错误输出。
        (work/'gdb.txt').write_text(transcript)   # 先保存实际记录，再判断成功。
        if session.returncode or 'AIRI_STACK_OK=1' not in transcript or 'AIRI_KERNEL_ABI_OK=1' not in transcript:  # 退出状态、栈边界或启动magic任一失败都不能通过。
            raise RuntimeError(f'Kernel debug validation failed; see {work/"gdb.txt"}')  # 报告已经保存的调试记录位置。
        (work/'result.json').write_text(json.dumps({'passed':True,'host':args.host,  # 将本轮真实调试主机和成功状态保存。
            'local_qemu':True,'owned_stack_verified':True,'boot_info_verified':True},indent=2)+'\n')  # 明确guest在本地，核对的是栈与启动参数，而非其他OS功能。
        print('PASS kernel GDB: x86-64 entry, owned stack, boot info v1 inspected')  # 三个入口和合同全部核对后才报告通过。
    finally:  # 成功返回与异常离开都进入收尾。
        process.terminate()  # 先请求主机 QEMU 进程结束。
        try:  # terminate 发出请求后仍须等待进程真正退出
            process.wait(timeout=5)  # 最多五秒，防止清理本身无限等待
        except subprocess.TimeoutExpired:  # 虚拟机未响应正常终止请求
            process.kill()  # 强制结束本机 QEMU 进程
            process.wait()  # 回收已经结束的进程，避免留下子进程资源
```

在学习目录运行下面两条完整命令。第一条执行 Linux 程序，第二条调试本地虚拟机；它们需要 SSH 通路，会写上述独立实验目录。

```sh
./airios linux-lab  # 向个人主机传输并实际运行两份程序，保存输出与GDB记录。
./airios kernel-debug  # 远端GDB连接本地QEMU，观察自有栈及C启动参数。
```

成功时，`artifacts/linux-p1/artifacts/ordinary-output.txt` 包含 `main(argc=3)`，`raw-output.txt` 包含 `raw: _start -> syscall(write) -> syscall(exit)` 并以换行结束。同目录的 `raw-gdb.txt` 应记录写前的调用号 1、fd 1、长度 47，写后的结果 47，以及相同的 `next/expected` 地址。数值地址随实际构建变化，检查的是两者关系，不是某个固定地址。

`artifacts/kernel-debug/gdb.txt` 是另一条观察路径：其中的 `AIRI_STACK_OK=1` 表示调试时 RSP 在自有栈内，`AIRI_KERNEL_ABI_OK=1` 表示 RDI 指向的数据具有预期的 magic。其他 `print` 命令把版本、内存区域数和地址留下供查看；当前成功条件没有逐项断言这些字段的具体值，也没有跟进 `airi_kernel_main` 内部的完整合同校验。

<a id="linux-evidence"></a>

两条命令的失败位置不同。SSH 尚不能登录时，错误只出现在当前终端，脚本还来不及生成新的实验记录；进入远端脚本后，已经写出的输出保留在远端 `artifacts`，本机脚本会尝试打包取回。若取回本身也失败，记录仍需从远端取得。内核调试只有进入 GDB 会话后才会写 `gdb.txt`；会话超时则保存异常中已收到的字节，随后回收本机 QEMU。

这些脚本复用上述证据目录，失败也可能留下以前的 `result.json` 或尚未被本次执行覆盖的文件。判断本轮结果应以刚运行命令的退出状态、实际输出与文件时间对应，不能仅凭目录里有一份成功 JSON。修复 SSH 访问或缺失工具后，重新执行同一条命令；连接失败本身不提供任何 Linux 寄存器或 guest 断点证据。

2026-10-15 的作者运行中，两条命令都因 SSH 返回 255 停止，尚未获得本版的远端输出与 GDB 停点；具体环境和状态保留在[验证说明](../../docs/verification.md)。下面的本地连续重建只检查脚本语法，不会调用上述远程命令。

**在学习目录根目录运行：**

<!-- run -->
```sh
python3 -m py_compile scripts/linux_lab.py scripts/kernel_debug.py  # 本地仅核对Python可解析，不能代替远端实测。
sh -n labs/linux/p1/run.sh  # 只做Shell语法检查，不执行Linux二进制或GDB。
```

本地检查成功只表示这些脚本可以解析。三条路径各自有了明确的观察位置：运行时进入 `main`，用户态通过 `syscall` 请求 Linux，AiriOS 则在自己的栈上进入 C 并直接访问设备。后续用户态阶段将继续观察地址空间与文件描述符在启动程序时怎样变化；文件与内存实验的具体场景见[项目对照](../../docs/system-practice.md)。
