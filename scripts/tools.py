"""Resolve each executable without modifying the process PATH."""  # 查找函数返回所选路径，不改当前进程的 PATH
import os  # 读取环境变量并检查文件执行权限
from pathlib import Path  # 用路径对象保存和拼接目录、文件名
import platform  # 判断当前主机是否为 macOS
import shutil  # 复用标准库的 PATH 查找

ROOT = Path(__file__).resolve().parents[1]  # 当前文件在 scripts 中；第二层父目录就是工程根目录
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
