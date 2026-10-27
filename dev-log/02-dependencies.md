# 02 · 让外部引导材料有可验证的身份

<a id="boot-materials"></a>

上一章已经选好了编译工具。它们以后会把 C 和汇编源文件变成内核文件；接下来还需要一条启动路径，让虚拟 CPU 真正执行其中的指令。

平时在 Mac 上运行程序，已有的操作系统负责读取可执行文件、准备内存并进入程序。现在要运行的程序本身就是内核。它开始执行以前，装载工作需要由先运行的程序完成。CPU 复位后从体系结构规定的入口取指，机器预先保存的启动软件从这里接手；这类随机器提供的软件称为固件。QEMU 模拟 PC 硬件，我们还要为这台虚拟 PC 提供实际可执行的固件内容。

本项目采用 OVMF，它是适用于虚拟机器的 UEFI 固件实现。UEFI 规定固件向启动程序提供的接口和装载约定。OVMF 先初始化启动所需的环境，再从启动介质找到 EFI 可执行程序并运行它。这份 EFI 程序采用 Limine：它是独立于内核的引导程序，负责读取内核文件，安排代码和数据的内存位置，建立早期地址映射，最后进入内核入口。

这段分工让固件提供通用的启动环境，由引导器处理内核采用的文件格式与交接协议。进入内核时，Limine 还会留下内存地图等启动数据；内核适配代码读取这些描述，再逐步建立自己的内存管理和设备服务。第 05 章先设计自有启动描述，第 06–08 章把这条路径实际接通。

因此准备脚本要取得三类材料：虚拟机器首先执行的 OVMF 固件、随后执行的 Limine 引导程序，以及编译内核适配代码时读取的 C 协议头 `limine.h`。协议头声明双方约定的数据布局和标识常量；真正装载内核、填写响应的是运行中的 Limine。光盘启动还需要它提供的启动映像，第 07 章会把这些文件放进介质。

同一个文件名可能对应不同版本的字节。先固定 URL、版本和摘要，下载与复制都按同一组记录核对，后面的装载和响应格式才有确定输入。

## 锁文件描述材料，脚本据此取得真实文件

<a id="config-values"></a>

锁文件使用 JSON。花括号对象读入 Python 后成为字典，方括号数组成为列表。`lock["limine"]` 取得含 url、sha256、files 的内层字典，`lock["firmware"]` 则取得两条固件记录。这里先声明期望值；写好 JSON 并不会自动下载或运行其中的程序。

`frontend_fonts` 另记可选图形支线的资产身份。本章只准备引导材料，串口主线不读取字体。以下记录中的实际 URL 与摘要原样保存。

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

准备脚本用 `json.loads(path.read_text())` 将上面的文本变成对象。比如 `lock["limine"]["url"]` 先取一份内层字典，再取 URL 字符串；`lock["firmware"][0]["name"]` 先取列表，再取第零条字典，最后取文件名。每次索引得到的对象形状决定下一步按键还是按下标访问。

取得 URL 还不够，下载内容需要与 sha256 比较。SHA-256 消费文件的完整原始字节，给出固定长度摘要；期望摘要来自锁文件，实际摘要由脚本重新计算。文本读取用于解释 JSON，`read_bytes()` 用于核对材料身份，文件名和版本打印文字都不能代替字节检查。

归档与缓存分别检查：limine.sha256 核对本次展开所用的完整归档，files 中的摘要判断已有 BOOTX64.EFI 与 CD 启动映像能否复用；需要重新展开时，输入由归档摘要固定。protocol 的 commit 固定声明版本，url 定位头文件，sha256 核对返回内容。qemu_reference_version 提供寻找固件的参考版本，两条固件记录中的摘要才是最终采用条件。

图形字段的 manifest 指资产清单，atlas 指生成的 C 像素数组，atlas_sha256 核对其内容，generator 记录生成工具；选择图形支线后才消费它们。材料的来源与许可另保存在 `THIRD_PARTY.md`。

**新建 `scripts/deps.py`，先引入下载、摘要、归档与路径操作：**

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

同一份材料会在缓存复用、下载完成和展开后检查时被多次核对，先把共同动作写成 digest。输入可以是字符串或 Path，函数统一包装成路径，读取全部字节再返回摘要字符串；调用它只检查现有文件，不负责下载。

**追加到 `scripts/deps.py` 末尾：**

<a id="code-scripts-deps-py-2"></a>

<!-- append: scripts/deps.py -->
```python
def digest(path):  # 输入文件路径，返回其全部字节的 SHA-256 十六进制文字
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()  # 先读原始字节再计算摘要，hexdigest 将结果转为锁文件使用的字符串
```

现在考虑归档还不存在的第一次准备。download 先建立缓存目录，让 curl 将 URL 内容写入 `.download` 临时路径。`-f` 使 HTTP 错误失败，`-L` 跟随重定向，`--retry 3` 重试可重试的传输错误，`check=True` 把非零退出转为 Python 异常；这时后续校验不会继续。

curl 正常结束也只说明传输结束。脚本还要读取临时文件计算摘要：相符才通过 replace 移到正式缓存，不符则删除临时文件并报错。下次已有正式文件且摘要符合时直接复用，否则重新下载。因此返回以后，调用方才能把正式路径作为符合预定输入的材料使用。

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

## 从已校验归档走到实际使用的展开文件

现在把 download 接到本章真正的材料准备过程。prepare 先读 `config/deps.lock.json`：json.loads 得到外层字典，`lock['limine']` 再取出 Limine 的那份内层记录。dep 保存的正是前面写下的 12.9.2 记录，其中有下载 URL、整个归档的摘要，以及展开文件各自的摘要。

本次调用把三项值交给 download：`dep['url']` 指定来源，archive 指定工程中的正式缓存位置，`dep['sha256']` 指定整个压缩包应有的摘要。archive 由 ROOT 拼接而来，终端当前目录改变也不会改掉这个目标。download 返回后，这个路径才对应符合摘要的归档；curl、校验或改名失败都会在此前中断。

**追加到 `scripts/deps.py` 末尾，开始编写 prepare；函数剩余部分随后接上，本章写完再运行：**

<a id="code-scripts-deps-py-4"></a>

<!-- append: scripts/deps.py -->
```python
def prepare():  # 按同一锁文件准备引导器、协议头和两份固件，最后返回锁内容
    lock=json.loads((ROOT/'config/deps.lock.json').read_text())  # 先读取文本，再转成外层字典；lock 保存键到内层对象的映射
    dep=lock['limine']  # 取得 Limine 的内层字典，包含 URL、归档摘要和展开后文件摘要
    archive=ROOT/'third_party/limine-binary.tar.gz'  # 把归档缓存放在这份工程的 third_party 内
    download(dep['url'],archive,dep['sha256'])  # 下载或复用已校验的 Limine 归档，再允许下面解包
    dest=ROOT/'third_party/limine'  # 展开后的引导文件保存在独立目录
```

归档已经就绪，制作启动介质时需要的却是其中的 BOOTX64.EFI 与 CD 启动映像。dest 指向 `ROOT/'third_party/limine'`，所以接下来检查的是展开文件。例如 `dep['files'].items()` 交出 BOOTX64.EFI 这一项时，name 取得文件名字符串，sha 取得该文件的期望摘要，`dest/name` 就定位到 `ROOT/'third_party/limine/BOOTX64.EFI'`。

先判断这份文件是否存在。缺失时 `not ...is_file()` 已为真，or 不再读取摘要；存在时才调用 digest 比较字节。生成器按 any 的需要逐项给出这些判断，any 遇到第一个真值便触发重新展开，两项全为假才复用已有文件。只缺一项时，重新展开的仍是整个归档，而非只下载这一项。

展开还要落到刚才检查的位置。当前归档中的真实成员名为 `limine-binary/BOOTX64.EFI`；parts 将它拆成 `('limine-binary', 'BOOTX64.EFI')`，`parts[1:]` 去掉第一段，`Path(*parts[1:])` 把留下的路径段逐个作为参数，组成 BOOTX64.EFI 这个相对路径。修改 member.name 后再向 dest 提取，最终文件正好成为前面检查的对象。有子目录时同样保留第一段以后的各段，只有顶层目录本身的成员被跳过。

member 保存归档成员描述，extract 才按这个描述写出文件。with 限制归档读取句柄的寿命，结束时关闭句柄，已展开的文件继续保留。每次 extract 仍用 `filter='data'` 限制危险路径和成员。

**继续追加 prepare 的展开部分，保持函数内缩进：**

<!-- append: scripts/deps.py -->
```python
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
```

到这里，archive 保存经过归档摘要校验的压缩包，dest 保存启动介质稍后要读取的展开文件。协议头来自另一份记录：`lock['protocol']` 给出它自己的 URL、固定 commit 与摘要，download 将匹配内容放到 `ROOT/'third_party/limine.h'`。编译器稍后读取声明，固件执行的则是前面展开的 EFI 程序。

**继续追加协议头准备，仍在 prepare 内：**

<!-- append: scripts/deps.py -->
```python
    header=lock['protocol']  # 取得协议头的 URL、固定提交和摘要记录
    download(header['url'], ROOT/'third_party/limine.h', header['sha256'])  # 把经过摘要校验的协议声明保存为后续编译使用的 limine.h
```

<a id="firmware-cache"></a>

## 固件需要匹配的字节，还要区分只读代码与变量初值

剩下两项来自 `lock['firmware']` 列表。每次 item 是含 name 与 sha256 的字典；处理 code 记录时，target 就是工程 firmware 目录下的 edk2-x86_64-code.fd，随后同样处理 vars。先检查工程缓存：存在且摘要相符便 continue 到下一项，否则才寻找本机安装中的匹配来源。

OVMF 的 code 是只读机器码，vars 是可写变量的初始映像。候选位置先从所选 QEMU 安装目录旁的 share 推导，再尝试列出的常见目录。对 QEMU 路径使用 resolve 是为了找到安装目录，不将解析后的名字当启动命令。筛选生成器只交出存在且摘要符合的路径，`next(..., None)` 取第一份；没有匹配便报错，不能改用同名但字节不同的固件。

找到来源以后，copyfile 将它复制到工程缓存。实际启动再复制一份 vars，使本次变量写入不修改下次还要用的初值。循环两项都完成后，prepare 才返回最初读取的同一份 lock。

**最后追加固件准备与返回，完成 prepare：**

<!-- append: scripts/deps.py -->
```python
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

## 在学习目录准备材料，再检查结果

传输失败会在临时文件校验和解包之前停止，先前经过校验的正式缓存仍可保留。修复网络后运行同一准备命令，版本与摘要不随网络失败改变。需要代理时可在 shell 中设置 HTTPS_PROXY，curl 子进程继承它；参考环境的 `export HTTPS_PROXY=http://127.0.0.1:7897` 指本机 7897 端口，其他电脑应采用自己的实际通路。代理只改变传输，不改变摘要条件。

<a id="firmware-inputs"></a>

QEMU 的固定源码版本也保存了本章所需的两份压缩固件。下面从 `qemu/qemu` 的 `v11.1.2/pc-bios` 取得它们，展开后的字节须与刚写好的锁文件一致。这个来源不依赖作者的电脑，也不要求本机软件包恰好附带相同固件；来源与许可见 [外部材料](../THIRD_PARTY.md)。

`curl -o` 保存压缩文件，`bzip2 -dc` 解压并将原始字节写到标准输出，shell 的 `>` 再把它们存入 `.download` 临时路径。bzip2 是 macOS 自带工具，Linux 需要安装 `bzip2` 包，传输仍需 curl。随后 Python 沿用已经写好的 digest，按锁文件核对解压后的完整内容；相符才用 replace 放入正式缓存，prepare 再继续取得 Limine、协议头并确认固件。

整个运行块交给独立的 `sh -eu`。`-e` 使未处理的失败立即结束这次 shell，`-u` 拒绝未定义变量；两条 SH 标记之间的命令由这个 shell 按顺序运行。传输、解压或摘要失败都会停在 prepare 以前，终端本身仍可继续使用。`<<'SH'` 的引号使外层 shell 将这些内容原样交入，内层的 PY 则界定那段 Python 程序。

**在学习目录根目录运行：**

<!-- run -->
```sh
sh -eu <<'SH'  # 在独立shell中连续准备；任一步失败即停止后续操作
mkdir -p third_party/firmware  # 为两份外部固件准备缓存目录
curl -fL --retry 3 https://raw.githubusercontent.com/qemu/qemu/v11.1.2/pc-bios/edk2-x86_64-code.fd.bz2 -o third_party/firmware/edk2-x86_64-code.fd.bz2  # 取得固定版本的固件代码压缩文件
curl -fL --retry 3 https://raw.githubusercontent.com/qemu/qemu/v11.1.2/pc-bios/edk2-i386-vars.fd.bz2 -o third_party/firmware/edk2-i386-vars.fd.bz2  # 取得同版变量区初值
bzip2 -dc third_party/firmware/edk2-x86_64-code.fd.bz2 > third_party/firmware/edk2-x86_64-code.fd.download  # 展开代码到待校验文件
bzip2 -dc third_party/firmware/edk2-i386-vars.fd.bz2 > third_party/firmware/edk2-i386-vars.fd.download  # 展开变量初值到待校验文件
PYTHONPATH=scripts python3 - <<'PY'  # 将以下完整程序交给学习工程的Python
import json  # 读取两份固件的期望摘要
from tools import ROOT  # 取得当前学习工程的稳定根目录
from deps import digest, prepare  # 使用本章自己写好的摘要与材料准备函数
lock = json.loads((ROOT / 'config/deps.lock.json').read_text())  # 将锁文件转为嵌套字典
for item in lock['firmware']:  # 每次处理一份含名称和摘要的固件记录
    target = ROOT / 'third_party/firmware' / item['name']  # 形成正式缓存路径
    temporary = target.with_suffix(target.suffix + '.download')  # 定位刚展开的待校验字节
    if digest(temporary) != item['sha256']:  # 与锁文件比较完整原始内容
        raise RuntimeError(f"firmware SHA-256 mismatch: {item['name']}")  # 摘要不符时结束，不发布错误固件
    temporary.replace(target)  # 仅将已匹配的文件放进正式缓存
print('Limine', prepare()['limine']['version'])  # 其余材料也准备成功后才打印版本
PY
SH
```

出现 `Limine 12.9.2`，表示两份公开来源固件已按锁文件核对，prepare 也完成了引导文件与协议头的准备。后续 prepare 会复用这些已校验缓存；重新执行上面的完整命令会重新取得并核对同一固定版本。失败时保留的压缩包和临时文件都还不是成功材料，按报错修复网络、工具或来源后重跑本块。

固定协议声明见 [Limine 协议](https://github.com/Limine-Bootloader/limine-protocol/blob/3a0526b700e356f0eac1b71a77697b3fd1c707a3/PROTOCOL.md)。到这里准备的是以后装载内核的环境，还没有自己的内核入口。接下来先实现字节操作与格式化：它们先由主机观察入口调用，之后再进入同一工程构建的内核。
