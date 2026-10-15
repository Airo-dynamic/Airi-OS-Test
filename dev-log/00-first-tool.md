# 00 · 从空目录找到并运行一个程序

<a id="kernel-model"></a>

在终端运行一个 C++ 程序时，当前 macOS 或 Linux 已经负责加载它、提供内存和显示输出。AiriOS 要逐步接过这些职责。内核是操作系统中管理 CPU、内存与设备、向程序提供基础服务的那部分代码；当前先写这部分，后续才让用户程序向它请求服务。

内核将运行在 QEMU 创建的虚拟电脑里：QEMU 是当前电脑上的普通程序，却向里面提供一颗 x86-64 CPU、内存和设备。x86-64 是这颗 CPU 使用的指令架构，Apple Silicon 的 ARM CPU 则使用另一套指令。

准备这台虚拟电脑仍要调用主机上的编译器、链接器和 QEMU。第一步先只做一次可观察的调用：把名字 `python3` 变成可执行文件路径，再执行它的 `--version`。这个脚本成功以后，相同的查找与启动方式才会用于构建内核。

在终端执行 `mkdir -p ~/Desktop/AiriOS-work`，再执行 `cd ~/Desktop/AiriOS-work`。这里的终端是显示文字的窗口，里面的 shell 解释输入的命令。`~` 由 shell 展开为当前用户的主目录；`mkdir -p` 建立目录并允许它已经存在，`cd` 改变后续相对路径的起点。后文新建文件前建立父目录，例如 `mkdir -p scripts`；同一文件分块时按顺序连接，完成本章全部编辑后再运行。源码框写入文件，命令框在终端执行，输出不写进源码。

先执行 `python3 --version` 和 `cc --version`，确认 Python 3.12+ 与 C 编译器存在。macOS 缺少编译器时执行 `xcode-select --install`。这几条命令运行在主机；现在尚未启动 AiriOS。

## 从名字找到可执行文件

<a id="tool-process"></a>

可执行文件是磁盘上的程序字节；进程是操作系统为一次运行建立的状态，包括正在执行的位置、内存和打开的输入输出。两次执行同一个 python3 文件会创建两次运行。查找路径只是取得文件名，直到启动接口被调用，才有新的进程。

PATH 是环境变量，也就是 shell 传给程序的一项具名字符串。它用冒号连接搜索目录，例如 `/opt/homebrew/bin:/usr/bin:/bin`。`shutil.which("python3")` 按这些目录查找可执行文件，成功返回路径字符串，未找到返回 `None`。`None` 是 Python 表示“这里没有值”的专用对象，不能把它当路径继续执行。

`import shutil` 引入标准库模块，点号表示从模块中取得 which 函数。`def find_tool(name: str) -> str` 的标注说明输入与返回值预期为字符串，Python 不因此自动验证类型。函数体用缩进划定，`is None` 判断搜索有没有结果；失败时 `raise` 抛出异常，把错误交给调用方。f 字符串中的 `{name}` 在运行时插入本次名称。

**新建 `scripts/probe.py`：**

<a id="code-scripts-probe-py-1"></a>

<!-- write: scripts/probe.py -->
```python
import shutil  # 标准库提供可执行文件的 PATH 查找，不启动外部 which 命令
import subprocess  # 启动子进程并等待版本命令结束
import sys  # 读取本次程序参数，并把错误输出到 stderr


def find_tool(name: str) -> str:  # 接收命令名；成功返回路径字符串，失败抛出异常
    path = shutil.which(name)  # 如 python3 -> /usr/bin/python3；找不到时结果为 None
    if path is None:  # None 表示没有找到文件，不能继续把它交给进程启动接口
        raise RuntimeError(f"找不到可执行文件: {name}")  # f 字符串把当前命令名填入错误消息
    return path  # 把已找到的路径交给 main；查找本身还未执行该程序
```

<a id="process-arguments"></a>

查找函数现在能返回文件路径。脚本入口还需要决定查哪个程序，并将路径组成一次具体调用。运行 `python3 scripts/probe.py clang` 时，`sys.argv` 保存 `['scripts/probe.py', 'clang']`。表达式 `A if 条件 else B` 根据参数是否存在来选择名称。

`sys.argv[0]` 是这次脚本的文件名，后面的元素才是用户交给它的参数；下标从零开始。默认运行时列表只有一项，入口采用 python3；给出 clang 时列表有两项，入口取字符串 clang。两个分支都将同一种字符串交给 find_tool。

找到路径后，`[path, "--version"]` 把可执行文件和一个参数保存为列表。`subprocess.run` 默认直接启动这个文件，每个列表元素保持一个参数的边界，因此路径中的空格也能正确传递。`check=True` 把非零退出转换为异常，`timeout=10` 将等待限制为十秒。

以找到 `/usr/bin/python3` 为例，参数列表为 `['/usr/bin/python3', '--version']`。列表是一组按顺序保存的对象，这里两项都是字符串；启动接口把第一项作为文件路径，第二项作为一个完整参数。列表不是需要 shell 再拆开的命令文字。

子进程通常继承父进程的标准输入、标准输出和标准错误三个通路。本次没有改输出去向，Python 的版本文字直接出现在同一个终端。脚本自己的错误写到 sys.stderr，后面即使把正常输出收集到文件，仍能分别处理错误。

查找失败、文件无法执行和子进程失败都在 `except` 中处理。括号列出可捕获的异常类型，`as error` 保存本次异常对象。错误打印到标准错误流 `sys.stderr`，入口返回 1；成功则返回 0。`flush=True` 使“运行”提示先写出，随后子进程的版本文字才出现在终端。

`main` 需要串起“取名称 → 找文件 → 启动 → 等待”的过程，还需要在直接执行脚本时调用它。Python 为直接运行的文件设置 `__name__ = "__main__"`；作为模块导入时则使用模块名。入口判断据此只在直接执行时调用 `main()`。

函数返回整数只把结果交回当前 Python 调用者，还没有结束进程。退出码是进程结束时交给父进程的整数，约定 0 为成功，非零为失败。shell 的 if、后续构建脚本和 subprocess 的 check 都通过它决定是否继续。

`SystemExit(main())` 将返回的整数作为进程退出码交给 Python 解释器。这样终端和后续构建脚本都能区分成功与失败。

**追加到 `scripts/probe.py` 末尾：**

<a id="code-scripts-probe-py-2"></a>

<!-- append: scripts/probe.py -->
```python
def main() -> int:  # 返回退出状态：0 成功，1 失败；-> int 是类型提示
    name = sys.argv[1] if len(sys.argv) == 2 else "python3"  # 一项额外参数选工具，否则默认 python3
    try:  # 此范围的查找与启动异常统一转成错误消息和退出状态
        path = find_tool(name)  # 如 name=python3，从查找函数取得实际文件路径
        command = [path, "--version"]  # 列表中首项是程序，第二项是独立参数，不经过 shell 拆分
        print(f"运行: {command}", flush=True)  # 先刷新当前进程输出，再让子进程打印版本
        subprocess.run(command, check=True, timeout=10)  # 等待完成；非零退出或超过十秒均抛出异常
    except (RuntimeError, OSError, subprocess.SubprocessError) as error:  # 捕获查找、系统启动及子进程失败
        print(f"错误: {error}", file=sys.stderr)  # file 参数将诊断送到标准错误通道
        return 1  # shell 用非零退出状态判断这次探测失败
    return 0  # 查找和版本执行都完成，交回成功状态


if __name__ == "__main__":  # 直接运行文件才进入此分支；作为模块导入不自动执行
    raise SystemExit(main())  # 用 main 的整数结果结束当前 Python 进程
```

用默认名称运行一次，再给出一个不存在的名称。第二条 shell 命令中的 `if` 根据退出码判断：若不存在的工具意外返回成功，`exit 1` 会中止这组观察；按当前实现，脚本报告查找失败，`then` 分支不会执行。

**在学习目录根目录运行：**

<!-- run -->
```sh
python3 scripts/probe.py  # 执行脚本默认的 python3 查找和版本探测
if python3 scripts/probe.py airi_no_such_tool; then exit 1; fi  # 故障应返回非零；若意外成功，then 以 1 中止此组观察
```

第一次运行先打印包含绝对路径的参数列表，再打印当前 Python 的版本。第二次只打印“找不到可执行文件: airi_no_such_tool”，退出码为 1。它在搜索阶段已经停止，因此没有“运行:”提示，也没有启动子进程。

再次执行第一条命令仍会正常运行。程序名没有改变任何持久状态；脚本每次都重新搜索。同名编译器可能安装多份，构建还需要决定采用哪条路径。
