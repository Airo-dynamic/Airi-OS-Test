# 02 · 让外部引导材料有可验证的身份

<a id="boot-materials"></a>

把内核文件放在磁盘上，并不会使 CPU 自动开始执行它。机器先执行固件，再由固件找到启动介质中的引导程序，引导程序装入内核并跳到入口。UEFI 规定固件加载程序及提供早期设备服务的接口；本项目采用 OVMF 固件和 Limine 引导程序，后者完成装载与早期映射，再把执行权交给内核。

先准备固定版本的外部材料，后面的启动行为才能对应到同一组输入。

主机脚本要取得三类材料：Limine 的可执行文件、描述交接格式的 C 协议头，以及 OVMF 的固件文件。协议头只让编译器知道结构和常量；真正执行装载的是引导程序。把它们分开保存，后面才能看清哪个文件参与编译、哪个文件在启动时执行。

## 记录需要的文件

<a id="config-values"></a>

锁文件使用 JSON。花括号 `{}` 中的对象会读成 Python 字典，方括号 `[]` 中的数组会读成列表。`lock["limine"]` 取得一份内层字典，其中 `url` 和 `sha256` 是字符串；`lock["firmware"]` 取得两条固件记录，每条保存名称和摘要。`frontend_fonts` 保存可选图形支线需要的资产身份；当前下载流程只准备引导所需材料，串口主线不读取字体文件。

**新建 `config/deps.lock.json`：**

<a id="code-config-deps-lock-json-1"></a>

<!-- write: config/deps.lock.json -->
```json
{
  "limine": {
    "version": "12.9.2",
    "url": "https://github.com/Limine-Bootloader/Limine/releases/download/v12.9.2/limine-binary.tar.gz",
    "sha256": "047e7eeae7de4de62fca4fc3740b621aa37a2038b208f4d0ac993750a18eed86",
    "files": {
      "BOOTX64.EFI": "7513f15f174bb0d4028d6231b7e76570c4fa60321965846a33cab16dac718210",
      "limine-uefi-cd.bin": "a0f31cb9333ca1af07844307c770fa6ec650625e5ff76bc07c5be95bb8410a9a"
    }
  },
  "protocol": {
    "commit": "3a0526b700e356f0eac1b71a77697b3fd1c707a3",
    "url": "https://raw.githubusercontent.com/Limine-Bootloader/limine-protocol/3a0526b700e356f0eac1b71a77697b3fd1c707a3/include/limine.h",
    "sha256": "0aadf2633c85d8cb145344c968add158aeb1deb68af567d55ba8431c492b6d4c"
  },
  "qemu_reference_version": "11.1.2",
  "firmware": [
    {
      "name": "edk2-x86_64-code.fd",
      "sha256": "33090cc07675baa5190d9f1e84bf5176b33bcbfa9bacac522961150cdb6dbb2a"
    },
    {
      "name": "edk2-i386-vars.fd",
      "sha256": "5d2ac383371b408398accee7ec27c8c09ea5b74a0de0ceea6513388b15be5d1e"
    }
  ],
  "frontend_fonts": {
    "source_commit": "2eb0b48d5f760f62e286216f0859a8c540dbc1bd",
    "manifest": "frontend/assets/fonts.lock.json",
    "atlas": "frontend/assets/font-atlas.h",
    "atlas_sha256": "aaf2aa574cd663809d864aed4d712cc5790cb5b03d9a5935b8481f8091c29f27",
    "generator": "Pillow 12.3.0 / scripts/font_atlas.py"
  }
}
```

锁文件写入了预期值，准备脚本要按这些值决定缓存能否继续使用。`json.loads(path.read_text())` 把文件文本转换成字典，之后才能逐层按键读取。计算摘要则用 `read_bytes()`，保留下载文件的原始字节，交给 SHA-256 后得到十六进制字符串。

例如 `lock["limine"]["url"]` 先从外层字典取出 limine 对应的内层字典，再从内层取字符串 url；`lock["firmware"][0]["name"]` 则先取固件列表，再取第零条字典，最后取名称。每一步返回的对象决定下一步该按键还是按下标访问。

SHA-256 摘要是从文件全部字节计算出的固定长度结果。锁文件保存期望摘要，准备脚本重新计算下载内容的摘要并比较。它检测的是“这份字节是否符合预定输入”，不是根据文件名或显示的版本文字猜测身份。

`files` 字典给解包后的 BOOTX64.EFI 与 CD 启动映像分别保存摘要；只核对归档名不能代替这两份实际输入的检查。protocol 的 commit 固定头文件版本，url 取得该提交的声明，sha256 核对返回字节。qemu_reference_version 记录固件来源的参考版本，固件仍以两条 sha256 为最终判断。frontend_fonts 中的 manifest 是字体资产清单，atlas 是生成的 C 像素数组，atlas_sha256 核对其内容，generator 记录生成工具；选择图形支线后才会使用这组字段。

文件来源和许可保存在参考仓库的 `THIRD_PARTY.md` 中。

**新建 `scripts/deps.py`：**

<a id="code-scripts-deps-py-1"></a>

<!-- write: scripts/deps.py -->
```python
"""Fetch versioned artifacts and verify SHA-256 before using them."""  # 此模块只准备锁定的外部字节，成功后才允许构建者使用
import hashlib  # 使用 SHA-256 为文件原始字节计算身份摘要
import json  # 把锁文件中的 JSON 文本读成字典和列表
from pathlib import Path  # 将文件名包装为可拼接、可读取的路径对象
import shutil  # 把已核对摘要的本机固件复制到工程缓存
import subprocess  # 运行下载工具 curl，非零退出交回准备流程
import tarfile  # 读取并展开已通过摘要检查的 tar 归档
from tools import ROOT, tool  # 借用工程根路径及已确定的工具查找顺序
```

先补上 `digest`：它把传入的字符串或 Path 统一转换为路径对象，读取文件字节并返回摘要。后面的缓存检查和下载验证都使用这个函数。

**追加到 `scripts/deps.py` 末尾：**

<a id="code-scripts-deps-py-2"></a>

<!-- append: scripts/deps.py -->
```python
def digest(path):  # 输入文件路径，返回其全部字节的 SHA-256 十六进制文字
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()  # 先读原始字节再计算摘要，hexdigest 将结果转为锁文件使用的字符串
```

假设 Limine 归档尚未下载，`download` 先建立目标目录，再用 curl 写入以 `.download` 结尾的临时文件。`-f` 让 HTTP 错误返回失败，`-L` 跟随重定向，`--retry 3` 重试可重试的传输错误。`check=True` 将命令失败送回 Python，后续校验不会继续。

下载完成后计算临时文件摘要。相符才用 `replace` 将它移到正式路径；不符则删除临时文件并报告错误。如果正式缓存已经存在且摘要正确，跳过下载，使用已有文件。这个顺序使使用者只取得已通过校验的文件。

**追加到 `scripts/deps.py` 末尾：**

<a id="code-scripts-deps-py-3"></a>

<!-- append: scripts/deps.py -->
```python
def download(url, path, sha):  # 取得 URL 对应材料；只有摘要为 sha 的文件才进入正式缓存
    if not path.exists() or digest(path) != sha:  # 文件缺失便进入下载；or 短路使缺失文件不被读取
        path.parent.mkdir(parents=True, exist_ok=True)  # 创建正式缓存的父目录；已有目录继续使用
        temporary = path.with_suffix(path.suffix+'.download')  # 保留原扩展名并追加 .download，区分未校验内容与正式缓存
        subprocess.run(['curl','-fsSL','--retry','3',url,'-o',str(temporary)],check=True)  # curl 跟随重定向并写临时文件；启动或传输失败停止此函数
        if digest(temporary) != sha:  # 重新计算已下载字节的摘要，与锁文件的期望值比较
            temporary.unlink()  # 删除未通过校验的临时文件，不发布到正式缓存
            raise RuntimeError(f'SHA-256 mismatch: {url}')  # 把实际失败 URL 交回调用者，后续解包不会继续
        temporary.replace(path)  # 校验成功才将临时文件移到正式缓存位置
```

<a id="archive-preparation"></a>

download 完成后，归档路径对应一份已校验的文件，`prepare` 将它展开成具体引导文件，再取得协议头和固件。`files.items()` 每次给出一对文件名与摘要；`any(...)` 在其中任何文件缺失或不符时返回真，触发重新展开。括号中的生成器表达式逐项产生真假值，找到真值后可以停止。

`dep["files"]` 是“展开后文件名 → 期望摘要”的字典，例如某一项的键为 BOOTX64.EFI，值为一串摘要文字。items 把键和值成对给循环；`dest/name` 先组成待检查路径，is_file 返回真假，只有文件存在时才有必要继续算摘要。表达式里的 or 从左到右，左边已经为真便不计算右边。

这里的生成器表达式没有先建立一整张结果列表。它在 any 请求下一项时才计算下一文件，any 取得一个真值就知道缓存需要重新展开。全部为假才表示每个要求的文件均已存在且匹配。

归档成员原路径的第一段是发行目录。`parts[1:]` 去掉这一段，`Path(*parts[1:])` 将剩余各段展开为参数并重新组成相对路径。`with` 使归档在退出作用域时关闭，`filter='data'` 限制危险的解包路径。归档句柄用于读取内容，关闭句柄不会删除已经展开的文件。

固件分成只读机器码 `code` 与可写变量初始镜像 `vars`。它们分别经过循环检查，`continue` 跳过已经正确的缓存；`next((...), None)` 从本机 QEMU 安装目录中选第一个摘要符合的候选，没有就返回 `None`。此处对 QEMU 路径 `resolve()` 只为定位相邻 share 目录。每次启动会另复制一份 vars，固定的初始镜像保持可再次使用。

**追加到 `scripts/deps.py` 末尾：**

<a id="code-scripts-deps-py-4"></a>

<!-- append: scripts/deps.py -->
```python
def prepare():  # 按同一锁文件准备引导器、协议头和两份固件，最后返回锁内容
    lock=json.loads((ROOT/'config/deps.lock.json').read_text())  # 先读取文本，再转成外层字典；lock 保存键到内层对象的映射
    dep=lock['limine']  # 取得 Limine 的内层字典，包含 URL、归档摘要和展开后文件摘要
    archive=ROOT/'third_party/limine-binary.tar.gz'  # 把归档缓存放在这份工程的 third_party 内
    download(dep['url'],archive,dep['sha256'])  # 下载或复用已校验的 Limine 归档，再允许下面解包
    dest=ROOT/'third_party/limine'  # 展开后的引导文件保存在独立目录
    if any(not (dest/name).is_file() or digest(dest/name)!=sha  # 任何必需文件缺失或摘要错误都触发重新展开；缺失时不算摘要
           for name,sha in dep['files'].items()):  # items 逐次给出文件名与期望摘要；生成器按 any 的请求计算
        dest.mkdir(parents=True,exist_ok=True)  # 确保展开目标存在，已存在的目录不报错
        # 每项解包都经过 Python 的 data 过滤器，不按成员名字盲目信任路径。
        with tarfile.open(archive) as tar:  # 打开归档；正常离开或异常退出此缩进范围时都关闭句柄
            for member in tar.getmembers():  # 逐个取得归档成员的名称和内容位置描述
                parts=Path(member.name).parts  # 将成员路径拆成目录段元组，例如发行目录、BOOTX64.EFI
                if len(parts)>1:  # 略过只有顶层目录名的成员，处理顶层目录中的文件
                    member.name=str(Path(*parts[1:]))  # 切片去掉顶层目录，星号将剩余段逐个作为 Path 参数传入
                    tar.extract(member,dest,filter='data')  # 在 dest 中展开成员；data 过滤器拒绝危险路径和不允许的成员
    header=lock['protocol']  # 取得协议头的 URL、固定提交和摘要记录
    download(header['url'], ROOT/'third_party/limine.h', header['sha256'])  # 把经过摘要校验的协议声明保存为后续编译使用的 limine.h
    firmware=ROOT/'third_party/firmware'  # 两份固件的正式缓存目录，与引导器材料分开
    firmware.mkdir(parents=True,exist_ok=True)  # 建立固件目录，保留其中已存在的缓存文件
    for item in lock['firmware']:  # 每次 item 为一条包含 name 和 sha256 的固件字典
        target=firmware/item['name']  # 按该条 name 组成学习工程中的目标文件路径
        if target.exists() and digest(target)==item['sha256']:  # 缓存存在且字节摘要相符才复用；and 短路避免读缺失文件
            continue  # 这份固件已合格，直接检查下一条固件记录
        qemu=Path(tool('qemu-system-x86_64','qemu')).resolve()  # 这里只跟随工具符号链接定位安装目录，不将 resolve 结果用于启动 QEMU
        candidates=[qemu.parent.parent/'share/qemu'/item['name'],  # 优先检查所选 QEMU 安装目录相邻的 share/qemu
                    Path('/opt/homebrew/opt/qemu/share/qemu')/item['name'],  # 再检查 Apple Silicon Homebrew 的常见固件目录
                    Path('/usr/share/qemu')/item['name'],  # 再检查 Linux 的常见 QEMU 数据目录
                    Path('/usr/share/OVMF')/item['name']]  # 最后检查常见 OVMF 目录；列表保留这个候选次序
        source=next((p for p in candidates if p.is_file() and digest(p)==item['sha256']),None)  # 生成器只给出实际存在且摘要匹配的路径；next 取首项，无项则返回 None
        if not source:  # 没有匹配固件时停止，不能悄悄换成另一版
            raise RuntimeError(f'Pinned firmware {item["name"]} not found. '  # 错误先指出本轮缺少的锁定固件名称，再由相邻字符串接上恢复提示
                f'Use QEMU {lock["qemu_reference_version"]}, or copy the documented matching '  # 接着给出已记录的 QEMU 参考版本及复制匹配文件的恢复方法
                'firmware into third_party/firmware; do not silently substitute it.')  # 相邻字符串拼接为同一条异常消息，指明固件缓存目录
        shutil.copyfile(source,target)  # 匹配来源的字节复制到正式缓存，后续启动只用此缓存
    return lock  # 三类引导材料准备成功后，把同一锁内容交给调用者
```

下载连接失败时，`check=True` 会在临时文件校验与解包之前停止；已有经过校验的正式缓存保持原样。先修复主机网络，再重跑同一条准备命令，不更换锁中的版本或摘要。

需要本机代理的环境可在 shell 中设置 `HTTPS_PROXY`，curl 子进程继承这个环境变量。当前参考环境使用 `export HTTPS_PROXY=http://127.0.0.1:7897`，其中 export 让后续子进程收到此值，127.0.0.1 指当前主机，7897 是该环境代理的端口。其他电脑应使用自己的实际代理地址，已经可以直接下载时无需设置。代理只改变下载通路，下载结果仍接受同一份 SHA-256 校验。

运行准备函数前，先将参考工程中锁定的两份固件复制到学习目录。它们是外部机器码输入。Limine 归档和协议头仍由准备函数按 URL 下载并核对。

**在学习目录根目录运行：**

<!-- run -->
```sh
PYTHONPATH=scripts python3 - <<'PY'  # 标准输入中到下一独立 PY 行之前的内容交给 Python，不由 shell 展开
from pathlib import Path  # 将文件名包装为可拼接、可读取的路径对象
import shutil  # 复制文件及移除旧的打包目录树
from tools import ROOT  # 取得这份学习工程的绝对根路径
# 固件是外部二进制材料；仅从已校验的参考缓存取得这两份，不复制任何项目源码。
source = Path('/Users/bilibili/Desktop/AiriOS/third_party/firmware')  # 参考工程中只有这两份外部固件可借用，项目源码仍手敲
if source.is_dir() and source.resolve() != (ROOT / 'third_party/firmware').resolve():  # 仅从另一个已存在目录借用外部固件，避免把同一文件复制到自己
    target = ROOT / 'third_party/firmware'  # 本次学习工程的固件目录，与参考缓存不同
    target.mkdir(parents=True, exist_ok=True)  # 建立目标固件目录，允许已有缓存
    for name in ('edk2-x86_64-code.fd', 'edk2-i386-vars.fd'):  # 元组限定可复制的两个外部文件名
        shutil.copyfile(source / name, target / name)  # 将当前固件字节放入学习目录，prepare 仍会重新校验
from deps import prepare  # 取得刚写好的材料准备函数，不在 import 时自动准备
print('Limine', prepare()['limine']['version'])  # 全部准备成功才从返回字典取出版本，打印 Limine 12.9.2
PY
```

输出 `Limine 12.9.2` 时，引导文件、协议头和两份固件已符合锁文件。缺少固件时，准备函数会继续检查本机 QEMU 的 share 目录；如果其中版本不同，仍需将锁定的外部文件放入 `third_party/firmware/` 后重跑。各 Linux 发行版自带固件可能不同。

固定版本的接口定义见 [Limine 协议](https://github.com/Limine-Bootloader/limine-protocol/blob/3a0526b700e356f0eac1b71a77697b3fd1c707a3/PROTOCOL.md)。下一步开始写内核自己的字节操作，它们将先在主机上运行，再进入刚准备好引导环境的虚拟机。
