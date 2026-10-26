# 01 · 让构建使用正确的一份工具

上一章已能找到一个程序并运行版本命令。接下来要让编译器处理我们写的 C 文件，但同一份工程会产生两类程序：主机观察程序直接在当前 Mac 或 Linux 上运行，内核则交给 QEMU 中的 x86-64 CPU。即使源码相同，目标环境也不同，不能把生成 Mac 应用的默认选项直接用于内核。

编译器本身仍运行在主机上，为另一种机器或环境生成代码，这称为交叉编译。LLVM 是一组编译工具，Clang 把 C 变成目标代码，LLD 将目标文件连成可装载的程序。macOS 系统 Clang 和 Homebrew 安装的 LLVM 可以同时存在；Homebrew 是主机的软件包管理器，本章用它的安装位置优先选择内核工具，目标架构与具体选项稍后进入构建规则。

两类产物需要把工具角色分开保存：CC 是内核 C 编译器，LD 是内核链接器，HOST_CC 构建直接在主机运行的观察程序。Make 将根据所选目标消费这些变量；当前先组成它将收到的路径和参数，尚不编译内核。

## 先让脚本定位到自己的工程

<a id="tool-paths"></a>

查找工具以后还要读取锁文件、写 build 目录，因而脚本需要知道工程在哪里。终端的当前工作目录可以改变，模块自己的源码位置却仍属于这份工程。`tools.py` 将从后者建立 ROOT，其他脚本再复用它。

绝对路径从 `/` 起算，如 `/Users/airi/AiriOS-work`；相对路径从当前工作目录起算。`__file__` 是 Python 给当前模块的源码文件名，`Path(__file__)` 将它包装为路径对象，`resolve()` 找到实际位置。若文件是工程下的 `scripts/tools.py`，`parents[0]` 就是 scripts，`parents[1]` 才是工程根目录。

Path 还负责组合后续候选位置：`Path("/opt/homebrew") / "opt" / "llvm" / "bin" / "clang"` 得到 `/opt/homebrew/opt/llvm/bin/clang`。这里 `/` 是路径拼接操作，不执行文件，也不做数值除法。

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

## 同名工具存在多份时，明确采用哪一份

现在选择 Clang。启动文件名是 clang，Homebrew 安装这一组文件的配方却叫 llvm，因此调用为 `tool("clang", "llvm")`。两个参数共同形成上面的候选路径；没有配方时，默认的 `formula=None` 就跳过这一步。

查找顺序决定了两份工具同时存在时的结果：

1. 对应 AIRI 环境变量已经设置时，只验证并使用这条路径；无效就报错。
2. macOS 且给出了配方时，依次检查 `/opt/homebrew`、`/usr/local` 下的配方目录。
3. 尚未选中才交给 PATH 搜索。

这扩展了上一章仅按 PATH 查找的策略。比如 PATH 先出现系统 Clang，Homebrew 中另有可执行的 Clang，本章仍优先采用后者；设置 `AIRI_CLANG=/some/llvm/bin/clang` 后，则连 Homebrew 也不再尝试。Linux 跳过 Homebrew 分支。

`os.environ` 是“环境变量名 → 字符串值”的映射，`in` 判断键是否存在。变量名由命令名转换，`ld.lld` 对应 `AIRI_LD_LLD`。空字符串也表示设置过变量，所以必须拒绝，而不能悄悄改选另一份工具。候选路径用 `is_file()` 与 `os.access(..., os.X_OK)` 分别检查文件和执行权限；错误中的 `!r` 保留引号，让空值也能看清。

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

沿 `tool("clang", "llvm")` 的一次正常查找看结果：没有 AIRI_CLANG 覆盖，且 `/opt/homebrew/opt/llvm/bin/clang` 存在并可执行时，循环就在这里返回。函数不再检查第二个 Homebrew 前缀，也不继续搜 PATH；返回值只保存选中的路径，编译器还没有运行。显式覆盖和 Linux 路径按前面同一顺序处理。

返回工具路径时还要保留启动名称。例如 ld.lld 可能是指向通用 lld 的符号链接，启动时使用 ld.lld 这个名字会参与链接模式选择，因此这里只补成绝对路径。ROOT 则需要追溯 tools.py 的真实源码位置，才适合使用 resolve；两种处理分别服务启动工具和定位工程。缺少工具的异常继续列出 macOS、Debian/Ubuntu 与 Arch/CachyOS 包名。

## 把三种角色交给同一次 Make 调用

工具分别选好以后，要把它们带到构建执行器。GNU Make 读取 Makefile 中的目标和规则：目标说明这次要形成哪种产物，规则中的变量决定采用哪些工具。调用 `make_command("normal", "kernel")`，意味着选择 normal 配置下的 kernel 目标；随后规则用 CC 编译内核、用 LD 链接，主机观察规则则用 HOST_CC。一个参数列表可以提供三者，不表示三者都在每个目标中执行。

make_command 首先需要启动这次构建的 Make。显式设置 AIRI_GMAKE 就只接受指定路径，无效时停止；没有设置才先找 gmake，失败后改找 make，后者是 Linux 常见的 GNU Make 命令名。取得 Make 路径后，再调用同一个 tool 函数分别选 Clang、LLD 和主机 cc。

这些结果接下来进入 argv 列表。比如前面选中 Homebrew Clang，`f'CC={tool("clang", "llvm")}'` 就形成一个 `CC=/opt/homebrew/opt/llvm/bin/clang` 字符串。它是交给 Make 的一项参数，不会在 Python 中创建名为 CC 的变量。LD 与 HOST_CC 按同样方式形成各自的一项，最后接上目标名。

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

现在 `make_command("normal", "kernel")` 的结果已经包含本次构建所需的信息：第零项是 Make 路径，`-j4` 允许最多四项构建任务并行，后面是 PROFILE、CC、LD、HOST_CC 四项赋值，末项是 kernel。Make 运行以后才会按规则使用这些值；本章的打印只让我们看到将交出的调用数据，还没有编译产物。

下面先打印这份调用数据，再给 Clang 设置一个空路径观察拒绝分支。`PYTHONPATH=scripts` 只为这一次 Python 进程增加模块搜索目录，使 `from tools import ...` 找到刚写好的模块；PATH 搜索可执行程序，两者不能互换。Shell 的 heredoc 把两条 PY 标记间的完整程序交给 Python，末条 if 则检查失败退出码。

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

第一段应显示实际找到的主机编译器路径与内核构建参数；这证明工具选择和 argv 组成已经完成，仍未执行构建。第二段的 `AIRI_CLANG=''` 应报告无效可执行路径，停止在调用 Make 之前；如果它意外成功，shell 的 then 才以 1 中止这组观察。

macOS 缺少依赖时用 `brew install llvm lld qemu xorriso mtools make` 安装，再运行同一段观察。下一步将准备编译之外的输入：CPU 启动时先执行的固件、装入内核的引导程序，以及双方共享的协议声明。
