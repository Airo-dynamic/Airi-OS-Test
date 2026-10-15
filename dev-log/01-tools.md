# 01 · 让构建使用正确的一份工具

版本命令已经能运行，但“找到一个叫 clang 的程序”还不能确定它适合内核构建。macOS 自带的 Clang 和 Homebrew 安装的 LLVM 可以同时存在。Homebrew 是安装工具的软件包管理器，LLVM 是编译工具集合，Clang 负责 C 源码编译，LLD 负责链接。内核需要生成供虚拟 x86-64 CPU 执行的程序，而不是生成一个普通 Mac 应用。

这种在当前主机运行编译器、为另一种机器或运行环境生成代码的方式称为交叉编译。后面会明确传入目标架构；现在先把需要使用的编译器和链接器选出来。主机观察程序仍由 HOST_CC 构建，两种产物分别存放。

## 先确定源码与工具的位置

<a id="tool-paths"></a>

新模块 tools.py 保存项目根目录和查找函数。绝对路径从 `/` 起算，例如 `/Users/airi/AiriOS-work`；相对路径则从当前工作目录起算。终端在哪个目录调用脚本可以变化，源码根目录却应继续指向这份工程。

`__file__` 是 Python 为当前模块提供的源码文件名。`Path(__file__)` 将它包装成路径对象，`resolve()` 取得实际位置，`parents[0]` 是 scripts，`parents[1]` 是 AiriOS-work。ROOT 因而不依赖启动脚本时的工作目录。Path 的 `/` 被库定义为路径拼接：`Path("/opt/homebrew") / "opt" / "llvm" / "bin" / "clang"` 得到 `/opt/homebrew/opt/llvm/bin/clang`，这里没有做除法。

**新建 `scripts/tools.py`：**

<a id="code-scripts-tools-py-1"></a>

<!-- write: scripts/tools.py -->
```python
"""Resolve each executable without modifying the process PATH."""  # 查找函数返回所选路径，不改当前进程的 PATH
import os  # 读取环境变量并检查文件执行权限
from pathlib import Path  # 用路径对象保存和拼接目录、文件名
import platform  # 判断当前主机是否为 macOS
import shutil  # 复用标准库的 PATH 查找

ROOT = Path(__file__).resolve().parents[1]  # 当前文件在 scripts 中；第二层父目录就是工程根目录
```

以 Clang 为例，命令名是 `clang`，Homebrew 配方名是 `llvm`。查找函数分别接收这两个名字，按以下次序选择可执行文件：

1. 用户设置了对应 AIRI 环境变量时，采用其中的路径；路径无效则立即报错。
2. 当前平台为 macOS 且给出了配方时，依次检查 `/opt/homebrew` 与 `/usr/local` 下的配方目录。
3. 前两项均未选中时，使用 PATH 搜索。

命令名是启动时要找的文件名，配方名是 Homebrew 安装一组文件时使用的名称。安装 llvm 得到 clang，并不意味着应该启动名为 llvm 的文件。函数因此接收 `tool("clang", "llvm")`，分别用它们组成候选路径。

显式覆盖是一次用户选择，例如环境变量 AIRI_CLANG 保存 `/some/llvm/bin/clang`。函数若发现这个变量存在，就必须验证这条路径；否则设置写错时悄悄退回另一份编译器，日志显示成功却没有使用指定工具。

`formula=None` 表示没有 Homebrew 配方。环境变量名由命令名变换而来，例如 `ld.lld` 变成 `AIRI_LD_LLD`。`os.environ` 是保存“变量名 → 字符串值”的映射，`in` 检查键是否存在；空字符串也算一次显式设置，随后会被拒绝。`is_file()` 与 `os.access(..., os.X_OK)` 分别检查文件和执行权限，错误消息中的 `!r` 保留值的引号，便于看清空值。

**追加到 `scripts/tools.py` 末尾：**

<a id="code-scripts-tools-py-2"></a>

<!-- append: scripts/tools.py -->
```python
def tool(name, formula=None):  # name 是命令名；formula 默认 None，表示没有 Homebrew 配方
    variable = 'AIRI_' + name.upper().replace('-', '_').replace('.', '_')  # ld.lld -> AIRI_LD_LLD
    if variable in os.environ:  # 检查键是否存在，空字符串也算用户做过显式设置
        value = os.environ[variable]  # 从“变量名 -> 字符串”映射取出这一次覆盖值
        candidate = Path(value).expanduser()  # 将路径开头的 ~ 展开为当前用户目录
        if not value or not candidate.is_file() or not os.access(candidate, os.X_OK):  # 空值、非文件或不可执行均拒绝
            raise RuntimeError(f'{variable} must name an executable file: {value!r}')  # !r 保留引号，空值也能看清
        return str(candidate.absolute())  # 返回绝对路径字符串；保留符号链接名，不调用 resolve
    if platform.system() == 'Darwin' and formula:  # macOS 且已给配方名时才进入 Homebrew 搜索
        for prefix in ('/opt/homebrew', '/usr/local'):  # 元组按此顺序提供 Apple Silicon、Intel 两个候选前缀
            candidate = Path(prefix) / 'opt' / formula / 'bin' / name  # Path 的 / 拼成路径，不是数值除法
            if candidate.is_file() and os.access(candidate, os.X_OK):  # 候选必须实际存在且具有执行权限
                return str(candidate)  # 第一个有效候选结束搜索，后面的目录不再尝试
    path = shutil.which(name)  # 前两种策略未选中时，按 PATH 从左到右寻找命令
    if path is not None:  # which 的失败值为 None，成功值为路径字符串
        return path  # 将 PATH 选中的文件交给后续构建调用
    raise RuntimeError(f'Missing {name}. macOS: brew install llvm lld qemu xorriso mtools make; '  # 相邻字符串字面量会拼成同一消息
                       'Debian/Ubuntu: apt install clang lld llvm qemu-system-x86 ovmf xorriso mtools make; '  # Debian 系工具包名
                       'Arch/CachyOS: pacman -S clang lld llvm qemu-system-x86 edk2-ovmf xorriso mtools make. '  # Arch 系工具包名
                       'Firmware must also match config/deps.lock.json.')  # 工具安装之外，固件还须匹配项目锁
```

现在同时安装系统 Clang 和 Homebrew Clang 时，会选 Homebrew 那份；设置 `AIRI_CLANG` 后则采用显式路径。这是对上一章仅搜索 PATH 的策略扩展。Linux 跳过 Homebrew 检查，直接使用显式覆盖或 PATH；缺少工具时，错误消息给出 macOS、Debian/Ubuntu、Arch/CachyOS 的安装包名。

符号链接是一个指向另一条文件路径的文件系统对象。这里 `ld.lld` 可能指向 `lld`，但启动名称还参与链接模式选择。源文件位置可以跟随链接求实际目录；工具执行路径则保留 ld.lld 这个名字，只补成绝对路径。两个路径看似指向同一文件，交给程序启动时仍可能有不同含义。

## 将工具路径组成构建参数

Make 调用需要同时得到编译器、链接器、主机编译器和构建目标。`make_command` 先选 GNU Make，再把各项放进一张参数列表。若明确设置了 `AIRI_GMAKE`，路径错误会停止；否则先尝试 `gmake`，找不到再尝试 `make`。

**追加到 `scripts/tools.py` 末尾：**

<a id="code-scripts-tools-py-3"></a>

<!-- append: scripts/tools.py -->
```python
def make_command(profile='normal', target='all'):  # 两个默认参数分别选择构建配置和 Make 目标
    if 'AIRI_GMAKE' in os.environ:  # 用户明确指定 GNU Make 时，路径错误必须直接报告
        make = tool('gmake', 'make')  # 从显式覆盖取得 GNU Make 文件，不退回另一份 make
    else:  # 没有显式覆盖时允许尝试两种常见命令名
        try:  # 先尝试 Homebrew 常见的 gmake 名称
            make = tool('gmake', 'make')  # 找到后保留这条执行路径
        except RuntimeError:  # gmake 不存在时改试系统的 make
            make = tool('make')  # Linux 的 GNU Make 通常就叫 make
    return [make, '-j4', f'PROFILE={profile}', f'CC={tool("clang", "llvm")}',  # argv 前半：程序、并发数和配置/编译器赋值
            f'LD={tool("ld.lld", "lld")}', f'HOST_CC={tool("cc")}', target]  # argv 后半：链接器、主机编译器和目标；尚未执行
```

调用 `make_command("normal", "kernel")` 得到一个以 Make 文件路径开头的列表。`-j4` 允许最多四项构建任务并行，`PROFILE=normal` 和 `CC=...` 等元素是交给 Make 的变量赋值，末项 `kernel` 是目标。返回这张列表时尚未启动 Make，后面的脚本会用 `subprocess.run` 执行它。

例如本机找到 gmake、clang、ld.lld 和 cc 后，列表中的 `CC=/opt/homebrew/opt/llvm/bin/clang` 是一个完整元素。Make 读取 CC 变量决定调用哪个编译器，kernel 则选择后面将定义的构建目标。`make_command` 当前只返回这些数据；返回参数列表与执行构建仍是两个动作。

命令通过 `PYTHONPATH=scripts` 为这一次 Python 进程增加模块搜索目录，以便 `from tools import ...` 找到文件。这里的 PYTHONPATH 找 Python 模块，PATH 找可执行程序。

**在学习目录根目录运行：**

<!-- run -->
```sh
PYTHONPATH=scripts python3 - <<'PY'  # 标准输入中到下一独立 PY 行之前的内容交给 Python，不由 shell 展开
from tools import tool, make_command  # 分别取得可执行路径和完整的 Make 参数列表
print(tool('cc'))  # 打印实际找到的主机编译器路径，尚未运行编译器
print(make_command(target='kernel'))  # 打印内核构建参数，当前未启动 Make
PY
if AIRI_CLANG='' PYTHONPATH=scripts python3 -c 'from tools import tool; tool("clang", "llvm")'; then exit 1; fi  # 显式空路径必须失败；若意外返回成功则以非零中止
```

第一段打印主机编译器路径和内核构建参数，构建本身尚未发生。第二段设置 `AIRI_CLANG=''`，查找函数应指出它没有提供有效可执行文件，并停止在调用 Make 之前。

macOS 缺依赖时执行 `brew install llvm lld qemu xorriso mtools make`。安装后重跑第一段，参数列表会显示实际采用的路径。编译工具已经选定，装载内核还需要固定版本的引导器和虚拟固件。
