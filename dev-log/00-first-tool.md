# 00 · 从空目录找到并运行一个程序

<a id="kernel-model"></a>

运行一个 C++ 程序时，macOS 或 Linux 已经负责加载它、提供内存和显示输出。AiriOS 要逐步接过这些职责。内核是操作系统中管理 CPU、内存与设备、向程序提供基础服务的那部分代码；后面我们会让自己的程序向这个内核请求服务。

这个内核将运行在 QEMU 创建的虚拟电脑里。QEMU 本身是主机上的普通程序，却向里面提供 x86-64 CPU、内存和设备。x86-64 是虚拟 CPU 使用的指令架构，Apple Silicon 的 ARM CPU 使用另一套指令。主机仍负责运行编译器和 QEMU，自己的内核在虚拟电脑中执行，两边的程序有不同的运行环境。

先从主机上的一次调用开始：脚本找到一个程序，再启动它的 `--version`。以后这个被调用的程序可以换成编译器或 QEMU；现在用版本输出观察查找和启动是否接通，还没有构建或启动内核。

## 准备学习目录和第一次调用

在终端执行 `mkdir -p ~/Desktop/AiriOS-work`，再执行 `cd ~/Desktop/AiriOS-work`。终端提供显示文字和输入命令的窗口，里面的 shell 解释命令。`~` 展开为当前用户的主目录；`mkdir -p` 建立目录并允许它已经存在，`cd` 改变后续相对路径的起点。后文新建文件前先建立父目录，例如 `mkdir -p scripts`。

先执行 `python3 --version` 和 `cc --version`，确认 Python 3.12+ 与 C 编译器存在。macOS 缺少编译器时执行 `xcode-select --install`。这些命令都运行在主机上。同一文件分块时按顺序连接，完成本章全部编辑后再运行；源码框写进文件，命令框交给 shell，输出不写进源码。

这次将运行 `python3 scripts/probe.py`。shell 先找到并启动 Python，Python 再读取我们写的脚本。脚本还会查找一个探测对象，启动另一个进程输出版本，然后等待它结束。默认探测对象也叫 `python3`，所以会有两次 Python 运行：外层解释器负责脚本，内层解释器只报告版本。执行同一个文件可以产生两次运行，并不表示脚本再次进入自己。

<a id="tool-process"></a>

## 查找程序只取得路径，还没有启动它

可执行文件是磁盘上的程序字节；进程是一次运行的状态，包括正在执行的位置、内存和打开的输入输出。脚本先取得文件路径，之后调用启动接口，主机操作系统才为被调用程序建立新的进程。

查找依靠 PATH。它是 shell 传给程序的一项具名字符串，也就是环境变量；例如 `/opt/homebrew/bin:/usr/bin:/bin` 用冒号分隔几个搜索目录。`shutil.which("python3")` 按这些目录查找，找到时返回路径字符串，找不到时返回 `None`。`None` 是 Python 表示这里没有值的对象，不能作为程序路径继续使用。

`find_tool` 把这两种结果整理成一个约定：成功便交回路径，失败便抛出错误。`import shutil` 引入标准库模块，点号从模块中取得 `which`；`def` 建立函数，缩进划定函数体。`name: str` 与 `-> str` 提示参数和返回值预期为字符串，不会让 Python 自动验证类型。失败分支中的 f 字符串把本次名称填入 `{name}`，`raise` 则把异常交给调用方处理。

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

查找函数写好了，但定义函数不会自动执行它。接下来由入口取得本次名称、调用 `find_tool`，再把返回路径交给启动接口。

<a id="process-arguments"></a>

## 入口把查找结果变成一次具体运行

如果执行 `python3 scripts/probe.py clang`，解释器交给脚本的 `sys.argv` 是 `['scripts/probe.py', 'clang']`：第零项是脚本名，第一项才是用户传给脚本的名称。默认调用只有脚本名这一项，入口便采用 `"python3"`。代码中的 `A if 条件 else B` 在恰好给出一项额外参数时选 `sys.argv[1]`，其他情况选默认名称；两个分支都把字符串交给查找函数。

以默认调用为例，假设查到 `/usr/bin/python3`，入口组成 `['/usr/bin/python3', '--version']`。Python 列表按顺序保存对象，这里两项都是字符串。`subprocess.run` 把第一项作为可执行文件，第二项作为一个完整参数，默认直接启动，不再请 shell 拆分一段命令文字；路径中的空格因此也不会变成多个参数。组成列表时尚未执行程序。

入口先打印这张列表，`flush=True` 使这条提示立即写出，再由 subprocess.run 启动版本进程。子进程继承本次脚本的标准输入、标准输出和标准错误通路，所以它的版本文字也出现在同一个终端。脚本在 run 中等待，子进程结束后才继续。

等待的结果决定 main 接下来走哪条路。正常退出就越过 except、返回 0；`check=True` 使非零退出变成异常，`timeout=10` 则限制等待十秒。查找或启动阶段的失败也进入 except，由同一错误出口返回 1。

main 先完成名称选择、查找与运行，最后交回整数状态。这个函数稍后由文件末尾的入口调用；现在先把函数本身接在 find_tool 后面。

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


```

main 内部现在能走完正常路径：取得名称，找到路径，组成列表，打印提示，启动并等待，最后返回 0。名称是 clang 时流程相同，只是查到的路径与版本进程不同。接下来还要把这个返回值交给 Python 的进程退出过程。

`main()` 返回 0 或 1，供外层入口结束当前脚本进程时使用。Python 不会因为函数名叫 main 就自动调用它：直接执行文件时，解释器令 `__name__` 为 `"__main__"`，末尾判断才进入 `main()`；作为模块导入时使用模块名，不会自动运行版本探测。末行 `raise SystemExit(main())` 先计算 main 的返回值，用它构造退出异常，再由 raise 触发解释器退出。函数返回值由此成为 shell 能收到的进程退出码，约定 0 成功、非零失败。

**最后追加文件入口：**

<!-- append: scripts/probe.py -->
```python
if __name__ == "__main__":  # 直接运行文件才进入此分支；作为模块导入不自动执行
    raise SystemExit(main())  # 用 main 的整数结果结束当前 Python 进程
```

失败可能停在不同位置。`find_tool` 找不到文件时抛出 `RuntimeError`，尚未组成命令，也不会出现“运行:”提示；有路径却无法启动时可能得到 `OSError`；子进程非零退出或超过十秒则由 `subprocess` 报告失败。`except` 的括号列出这些异常类型，`as error` 保存本次异常对象，然后把具体消息写到 `sys.stderr` 并返回 1。标准错误与正常输出是不同通路，后续收集正常输出时仍可以分别处理诊断。

## 用成功与查找失败观察这条过程

先用默认名称运行，再传入一个不存在的名称。第二条命令中的 shell `if` 按退出码决定是否进入 `then`：若不存在的程序意外被判为成功，`exit 1` 会使这组观察失败；按当前实现，脚本返回非零，then 分支不执行。

**在学习目录根目录运行：**

<!-- run -->
```sh
python3 scripts/probe.py  # 执行脚本默认的 python3 查找和版本探测
if python3 scripts/probe.py airi_no_such_tool; then exit 1; fi  # 故障应返回非零；若意外成功，then 以 1 中止此组观察
```

第一次先出现“运行:”和包含路径的参数列表，然后是内层 Python 的版本。具体路径由本机 PATH 决定。第二次报告“找不到可执行文件: airi_no_such_tool”，退出码为 1；查找失败处已经转入 except，因此既没有“运行:”提示，也没有启动报告版本的子进程。

重新执行第一条仍会正常运行。探测没有修改持久配置，每次从当前环境重新查找。现在已经能从一个名称走到一次运行；下一章会处理同名编译器安装了多份时，构建该选择哪一份。
