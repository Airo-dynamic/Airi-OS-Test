# L01 · 对照 Linux 的函数调用、系统调用与内核入口

AiriOS 已在内核里直接写 UART。普通 Linux 程序也能输出文字，却没有同样的设备权限；它把写请求交给 Linux，输出对象由内核管理。这里保持同一项“输出一行”的目标，分别观察普通 main、无运行时的原始系统调用，再回到自己的内核栈。

源码从 K13 继续。ordinary/raw 在个人 x86-64 Linux 主机 `airi@ssh.airi-dynamic.top` 的 `~/AiriOS-labs/p1` 执行，AiriOS 仍在本机 QEMU 中运行。远端要有 cc、GNU Binutils 的 readelf/objdump/nm、GDB 与 Python 3；本机保留构建环境、Python 3.12+、SSH/scp。Apple Silicon 的 macOS 不能直接运行这些 Linux ELF。

SSH 采用现有密钥和配置，BatchMode=yes 不等待密码。连接、远端架构或工具不符都会失败；即使登录成功，也还没有证明程序或调试实验完成。运行命令和证据分别留在后面对应路径。

<a id="runtime-main"></a>

## main 收到参数之前，运行时已经做过什么

执行 `./ordinary one two` 时，Linux 按 ELF 建立进程初始状态。普通编译驱动还链接启动代码与 libc：若 ELF 有 PT_INTERP，内核先交给指定动态装载器，装载器准备共享库再进入程序入口；无解释器的静态程序直接进入自身入口。ordinary-elf.txt 将显示实际采用哪条路径，规则见 [execve](https://man7.org/linux/man-pages/man2/execve.2.html)。

程序运行时从初始栈取出参数，按 C ABI 调用 main；main 返回以后，运行时还处理退出并将状态交回内核。因此 main 不是最初入口。本次 argc=3，argv 是指针数组，前三项指向 `"./ordinary"`、`"one"`、`"two"`，各字符串以 NUL 结束，argv[argc] 才是空指针。argv[0] 通常由启动者给出，不是内核强制从文件路径推导。

下面 printf 按 `%d/%s/%ld` 读取参数，写入 libc 的 stdout；流可以缓存，所以一次 printf 不必等于一次内核 write。getpid 取得这次 Linux 进程号，只用于辨认运行，不是自制内核的任务编号。

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

puts 再写一行固定说明，但这两次库调用实际形成几次内核请求，要用系统调用跟踪才能判断。本章接下来直接准备原始 write，把要观察的寄存器边界固定下来。

## 没有 CRT 的入口，自己组成一次 Linux 请求

<a id="syscall-boundary"></a>

普通函数调用仍在用户程序权限内执行。syscall 则转入 Linux 预设入口，内核按调用号与寄存器参数选择服务，再返回结果。本次服务要求“向 fd=1 写出这 47 字节”，不要求程序知道输出最后是终端、文件还是管道。

fd 是当前进程的一项整数句柄，1 通常是标准输出。它既不是 0x3f8 设备端口，也不是消息地址；RSI 另外给出用户地址，RDX 另外给长度，内核按这些参数读取并写到句柄对应对象。原始指令返回成功字节数或负错误编号，libc 包装还会转换错误，见 [系统调用概述](https://man7.org/linux/man-pages/man2/intro.2.html)。

本例 `_start` 不调用 main，RSP 仍指初始 argc/argv 栈，C 参数寄存器尚未自动准备。它自己把 write 号 1 放 RAX、fd=1 放 RDI、消息地址放 RSI、47 放 RDX，再执行 syscall；返回结果在 RAX，RCX/R11 被机制改写。普通 SysV C 第四参数用 RCX，Linux syscall 用 R10，这次只用前三项，见 [syscall 寄存器约定](https://man7.org/linux/man-pages/man2/syscall.2.html)。

.ascii 保存消息字节，不追加 NUL，write 依靠明确长度。返回后仅完整 47 字节算成功，短写或负错误都选择退出状态 1，尚无通用重试循环。最后调用号 60 退出当前线程；本例单线程，因此进程结束。它与 libc 结束整个线程组的包装范围不同，见 [退出调用](https://man7.org/linux/man-pages/man2/_exit.2.html)。AiriOS 的 outb 则直接触达设备，不经过 Linux 服务入口。

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

## 同一份源码在直接运行和调试中各有一次进程

ordinary 由 cc 默认加入启动代码；raw 的 `-nostdlib -static -no-pie` 排除启动文件和默认库，选择固定地址，`-Wl,-e,_start` 将入口选项传给链接器。readelf 读 ELF，objdump 显示机器指令，nm -u 列未解决引用；raw 不应有解释器或外部符号。

脚本先直接运行，再由 GDB 各自重新启动。直接 raw 的 fd=1 由 Shell 重定向到 raw-output.txt，GDB 那次则把同样字节送入调试记录。它们共用代码，不共用一次运行的寄存器或参数状态。

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

直接运行结束以后，先以完整 47 字节和退出 0 核对写请求，再进入 GDB；这不会把一次 stdout 文件观察当成另一次调试的寄存器证据。raw 必须完整写出才退出成功。返回负错误或只写出一部分时，它都选择状态 1；脚本因 `set -e` 在这个普通命令失败处停止，后续 GDB 不会继续执行。这里还没有一般用途的短写重试循环。`set -u` 拒绝读取未定义变量；`-e` 在 `if` 条件等位置有例外，例如上面的 `grep` 未匹配时会进入正常的下一步，不能把 `-e` 理解为任何非零状态都会退出。

GDB 的停点将把“参数已准备”和“内核已返回”分别显示出来。`-q` 减少启动文字，`-nx` 不读取个人初始化文件，`-batch` 执行 `-ex` 给出的各条命令后退出。`start one two` 在 `main` 处停下，参数仍是 `one two`；`starti one two` 在本次执行的第一条机器指令停下。由于 `raw` 没有解释器，后者就是自己的 `_start`。动态程序的 `starti` 可能先停在装载器中，这两种停法的区别见 [GDB 启动命令](https://sourceware.org/gdb/current/onlinedocs/gdb.html/Starting.html)。

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

输出文件已证明完整字节，调试记录还需证明这次 syscall 前后的参数和返回位置对应。最后把两种证据一起核对。`re.search` 从一行记录中取得 `next` 和 `expected` 两个十六进制地址；正则里的两个括号形成捕获组，`group(1)/group(2)` 分别取值。匹配失败返回 `None`，`match and ...` 会先拒绝它，再比较两个地址。只有所有断言成立才写 `result.json`。

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

## 本机传源码，Linux 构建和运行，再取回证据

程序与观察脚本已写好，linux_lab.py 在个人主机创建独立目录，用 scp 只传 ordinary.c、raw_start.S、run.sh，不上传本机二进制；Linux 自己按 run.sh 编译。

本机启动 SSH 用 argv 保留参数边界，但最后一项仍是远端 Shell 命令文字。目录因而还需 shlex.quote，不能把本机列表等同远端没有 Shell。连接失败发生在运行前，不能从该错误推断系统调用行为。

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

execution 先保存远端退出状态，暂不 check=True，是为了失败后也尝试带回已经产生的记录。远端 tar 将 artifacts 编为 stdout 归档字节，SSH 返回 bytes，BytesIO 提供文件式接口交给 tarfile。

本机解到 artifacts/linux-p1，但归档还有 artifacts 这一层，所以实际文件在 `artifacts/linux-p1/artifacts/`。data 过滤限制成员路径。取回成功只证明传输完成，execution 非零仍报实验失败；仅远端执行和 passed 都符合才保存对应远端身份，不能用旧 JSON 代替。

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

## 回到本机虚拟 CPU，观察自己建立的栈

<a id="remote-debug"></a>

第三条路径的 GDB 仍是远端主机程序，被观察 CPU 却属于本机 QEMU。QEMU 的 GDB stub 只监听本机回环 127.0.0.1，SSH 反向转发把远端连接带回；调试器不是 guest 线程，也不替 CPU 执行内核代码。

kernel-debug 先构建 normal，再复制同次 ELF 与源码供 GDB 解释符号。substitute-path 把 ELF 中的本机源码路径映到远端 source，CPU 实际载荷仍由本机 ISO 装入。若符号 ELF 与镜像不同次，地址可能失配，所以这两份材料必须对应。

下面先准备远端观察文件，后半段才启动 QEMU 和 GDB。复制成功只说明调试材料就绪，尚未命中任何入口。

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

本机临时 socket 绑定端口 0，取得空闲编号后释放，再由 QEMU 使用。这个编号没有同时预留远端端口，SSH 的 `ExitOnForwardFailure=yes` 使远端占用明确失败；`-R 127.0.0.1:端口:127.0.0.1:端口` 将远端回环连接带回本机 stub。

GDB 连接后从三个停点观察不同结果。hbreak 按准确符号指令地址设硬件断点：`_start` 尚用引导器栈，airi_limine_start 已执行换栈与 call，airi_kernel_main 首指令则仍在 RDI 保存第一参数。前者的 RSP 应在自有 bottom/top 之间，因为 call 已从 top 压入返回地址；这只检查位置，尚无保护页。

`((AiriBootInfo*)$rdi)->version` 把参数值作为结构地址读字段，print/x 显示十六进制。显示 width 只说明响应携带了描述，串口内核也可收到它，不表示已绘图。脚本断言的是栈边界和 magic，其余打印项供观察，不扩展为完整合同校验。

-S 在 CPU 复位时暂停，连接后继续固件直到断点，见 [QEMU GDB](https://www.qemu.org/docs/master/system/gdb.html)。detach 只断调试连接，最后 finally 仍须结束、等待并回收 QEMU；会话超时也保存已收到记录再清理。

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

源码、符号和观察器现在已经各有位置，再执行下面两条命令。第一条把 ordinary/raw 的源码交给 Linux，由 Linux 构建并运行；第二条把同次内核 ELF 与源码交给远端 GDB，CPU 仍在本机 QEMU。两者都使用个人 SSH 通路，连接成功以后要核对的对象却不同。

| 操作 | 真正执行被观察代码的位置 | 本次要取得的结果 |
| --- | --- | --- |
| linux-lab | 个人 x86-64 Linux 进程 | ordinary 参数与 raw syscall 的寄存器、输出、退出 |
| kernel-debug | 本机 QEMU 中的 guest CPU | 自有栈、RDI 启动参数及对应符号停点 |

下面分别写入上述独立目录；不能拿前一个实验的输出补成后一个实验的停点。

```sh
./airios linux-lab  # 向个人主机传输并实际运行两份程序，保存输出与GDB记录。
./airios kernel-debug  # 远端GDB连接本地QEMU，观察自有栈及C启动参数。
```

Linux 路径成功时，ordinary-output.txt 应有 main(argc=3)，raw-output.txt 应恰好为带换行的 47 字节。raw-gdb.txt 还要显示写前 number=1、fd=1、length=47，写后 result=47，且 next 与 expected 地址相同；具体地址随构建变化，比较的是本次关系。

内核路径另在 `artifacts/kernel-debug/gdb.txt` 保存停点。AIRI_STACK_OK=1 只说明 RSP 在自有栈，AIRI_KERNEL_ABI_OK=1 只说明 RDI 指向预期 magic；其他字段虽打印，没有逐项断言，也没有跟完主函数内部验证。

<a id="linux-evidence"></a>

证据能否留下取决于失败阶段。SSH 未登录时只在当前终端报错，进入 run.sh 后远端 artifacts 才逐项形成，本机会尝试取回，取回失败仍需从远端取得。内核调试到 GDB 会话才有 gdb.txt，超时则保存异常已收到字节并清理本机进程。

这些目录会复用，旧 result.json 或本次尚未覆盖的文件可能仍在。以刚执行命令的状态、实际输出和文件时间对应判断，不凭成功 JSON 的存在。修复访问或工具后仍重跑同一命令，连接失败不提供寄存器或断点证据。

2026-10-15 的既有运行中，两条命令均 SSH255，本版远端输出与停点尚未验证，详见[验证说明](../../docs/verification.md)。下面的本地命令只检查脚本语法，不执行上述 SSH 操作。

**在学习目录根目录运行：**

<!-- run -->
```sh
python3 -m py_compile scripts/linux_lab.py scripts/kernel_debug.py  # 本地仅核对Python可解析，不能代替远端实测。
sh -n labs/linux/p1/run.sh  # 只做Shell语法检查，不执行Linux二进制或GDB。
```

本地语法检查通过，仍不等于 Linux 程序或 guest 断点已运行。三条路径分别说明了已有与待验证的边界：运行时调用 main，用户程序以 syscall 请求 Linux，自制内核则在自有栈执行 C 并访问设备。

后续用这些区别继续理解地址空间、进程和 fd，而不是把不同环境的同名函数视为同一个机制。具体业务对照见[项目实践](../../docs/system-practice.md)。
