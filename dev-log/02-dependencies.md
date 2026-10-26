# 02 · 让外部引导材料有可验证的身份

<a id="boot-materials"></a>

工具已经选好，但编译得到内核文件以后，还没有任何程序负责把它装入虚拟机器。QEMU 启动后先执行固件，固件按 UEFI 接口找到启动介质中的 EFI 引导程序，后者才装入内核、建立早期映射并跳转到入口。本项目先借用 OVMF 固件和 Limine 引导程序，内核代码仍由后续章节实现。

因此主机准备脚本要交出三类用途不同的材料：Limine 的可执行文件和光盘启动映像，编译时读取的 C 协议头，以及虚拟机器启动时执行的 OVMF 固件。协议头只有结构与常量声明，不会执行装载。它们会分别进入编译命令、启动介质或 QEMU 参数，不能仅凭文件都在 third_party 中就当成同一种依赖。

同一个文件名也可能对应不同版本的字节。先固定 URL、版本和摘要，下载与复制都按同一组记录核对，后面的装载和响应格式才有确定输入。

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

检查也分两层：limine.sha256 核对整个归档，files 再核对展开后的 BOOTX64.EFI 与 CD 启动映像。protocol 的 commit 固定声明版本，url 定位头文件，sha256 核对返回内容。qemu_reference_version 提供寻找固件的参考版本，两条固件记录中的摘要才是最终采用条件。

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

prepare 调用 download 后，archive 指向已校验的压缩包。启动介质稍后需要的却是里面的 BOOTX64.EFI 和 CD 映像，所以还要检查 dest 中这些展开文件。`dep["files"]` 保存“文件名 → 期望摘要”，`items()` 每次提供一对值，例如 name 为 `"BOOTX64.EFI"`、sha 为它的摘要，`dest/name` 就是待检查路径。

先判断这份文件是否存在。缺失时 `not is_file()` 已为真，or 不再读取摘要；存在才比较实际字节。生成器按 `any(...)` 的需要逐项产生这些真假值，没有先建立完整结果列表。any 遇到第一个真值就决定重新展开，全部为假才复用已有展开文件。只缺一项也会触发整个归档的展开，而不是只下载那一项。

展开过程还必须使输出落到刚才检查的位置。假设成员名是 `发行目录/BOOTX64.EFI`，parts 将它拆为路径段，`parts[1:]` 去掉发行目录，`Path(*parts[1:])` 将剩余段逐个作为参数组成相对路径。修改 member.name 后，extract 写入 dest，得到的正是前面检查的文件；还有子目录时同样保留后续路径段。只有顶层目录本身的成员被跳过。

member 是归档成员描述，不是已展开的文件。`with tarfile.open(...)` 限制归档读取句柄的寿命，关闭它不删除展开结果；每次 extract 仍使用 `filter='data'` 限制危险路径和成员。随后按独立的 protocol 记录取得 limine.h，编译器读取它，引导器则执行前面展开的二进制程序。

## 固件需要匹配的字节，还要区分只读代码与变量初值

OVMF 分为只读机器码 code 和可写变量初始映像 vars。循环先检查工程缓存，正确就 continue 到下一项；否则按 QEMU 相邻 share 目录等候选位置查找。筛选生成器只交出存在且摘要符合的路径，`next(..., None)` 取首个，没有匹配就返回 None 并报错，不换用另一版固件。

这里对 QEMU 路径调用 resolve，只为找到安装目录旁的 share，不把解析后的名字当启动命令。匹配来源复制到工程 firmware 缓存；实际每次启动再复制一份 vars，启动时的变量写入不会修改可重复使用的初始映像。全部材料齐备后，prepare 返回最初读取的同一份 lock。

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

## 在学习目录准备材料，再检查结果

传输失败会在临时文件校验和解包之前停止，先前经过校验的正式缓存仍可保留。修复网络后运行同一准备命令，版本与摘要不随网络失败改变。需要代理时可在 shell 中设置 HTTPS_PROXY，curl 子进程继承它；参考环境的 `export HTTPS_PROXY=http://127.0.0.1:7897` 指本机 7897 端口，其他电脑应采用自己的实际通路。代理只改变传输，不改变摘要条件。

下面先从参考工程借用锁定的两份外部固件机器码，再调用自己的 prepare；不复制项目实现。若在参考工程本身执行，路径相同就跳过复制。Limine 归档和协议头仍按 URL 获取并核对，固件缓存也继续接受摘要检查。

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

出现 `Limine 12.9.2`，意味着这次 prepare 已经走完引导文件、协议头与两份固件的准备，而不是只读到了版本字段。若固件未缓存，程序会寻找本机 QEMU 的 share 目录；发行版固件字节可能不同，没有符合项就仍需把锁定文件放进 `third_party/firmware/` 后重跑。

固定协议声明见 [Limine 协议](https://github.com/Limine-Bootloader/limine-protocol/blob/3a0526b700e356f0eac1b71a77697b3fd1c707a3/PROTOCOL.md)。到这里准备的是以后装载内核的环境，还没有自己的内核入口。接下来先实现字节操作与格式化：它们先由主机观察入口调用，之后再进入同一工程构建的内核。
