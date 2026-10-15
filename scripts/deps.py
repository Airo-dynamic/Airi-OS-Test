"""Fetch versioned artifacts and verify SHA-256 before using them."""  # 此模块只准备锁定的外部字节，成功后才允许构建者使用
import hashlib  # 使用 SHA-256 为文件原始字节计算身份摘要
import json  # 把锁文件中的 JSON 文本读成字典和列表
from pathlib import Path  # 将文件名包装为可拼接、可读取的路径对象
import shutil  # 把已核对摘要的本机固件复制到工程缓存
import subprocess  # 运行下载工具 curl，非零退出交回准备流程
import tarfile  # 读取并展开已通过摘要检查的 tar 归档
from tools import ROOT, tool  # 借用工程根路径及已确定的工具查找顺序
def digest(path):  # 输入文件路径，返回其全部字节的 SHA-256 十六进制文字
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()  # 先读原始字节再计算摘要，hexdigest 将结果转为锁文件使用的字符串
def download(url, path, sha):  # 取得 URL 对应材料；只有摘要为 sha 的文件才进入正式缓存
    if not path.exists() or digest(path) != sha:  # 文件缺失便进入下载；or 短路使缺失文件不被读取
        path.parent.mkdir(parents=True, exist_ok=True)  # 创建正式缓存的父目录；已有目录继续使用
        temporary = path.with_suffix(path.suffix+'.download')  # 保留原扩展名并追加 .download，区分未校验内容与正式缓存
        subprocess.run(['curl','-fsSL','--retry','3',url,'-o',str(temporary)],check=True)  # curl 跟随重定向并写临时文件；启动或传输失败停止此函数
        if digest(temporary) != sha:  # 重新计算已下载字节的摘要，与锁文件的期望值比较
            temporary.unlink()  # 删除未通过校验的临时文件，不发布到正式缓存
            raise RuntimeError(f'SHA-256 mismatch: {url}')  # 把实际失败 URL 交回调用者，后续解包不会继续
        temporary.replace(path)  # 校验成功才将临时文件移到正式缓存位置
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
