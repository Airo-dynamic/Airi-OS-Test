import shutil      # 复制文件及移除旧的打包目录树
import subprocess  # 启动主机工具，取得退出状态或保存进程对象
import sys  # 取得脚本文件名之后的 profile 参数
from tools import ROOT, tool  # 借用工程根路径及已确定的工具查找顺序

profile=sys.argv[1]  # 读取 Make 传入的第一个用户参数，例如 normal
out=ROOT/'build'/profile  # 当前 profile 的 ELF 输入和 ISO 输出共用此构建目录
staging=out/'iso'     # 暂存介质目录树，稍后整体交给 xorriso
if staging.exists():  # 本轮打包前判断旧 staging 是否存在，避免删除缺失路径
    shutil.rmtree(staging)  # 递归删除旧介质目录，下一步只放入本轮选定的文件
(staging/'boot/limine').mkdir(parents=True)  # 创建介质中的引导材料目录，并连同缺失的父目录建立
(staging/'EFI/BOOT').mkdir(parents=True)     # 创建 UEFI 默认启动路径的父目录
shutil.copyfile(out/'airios.elf',staging/'boot/airios.elf')  # 把本次 ELF 放到 Limine 配置将读取的介质路径
shutil.copyfile(ROOT/'config/boot/limine.conf',staging/'limine.conf')  # 把内核路径及交接协议配置放到介质根目录
shutil.copyfile(ROOT/'third_party/limine/BOOTX64.EFI',staging/'EFI/BOOT/BOOTX64.EFI')  # 在外层 ISO 保留同名 EFI 文件；本次光盘入口由登记的 bin 提供
shutil.copyfile(ROOT/'third_party/limine/limine-uefi-cd.bin',  # 选择已校验的 UEFI 光盘启动映像作为复制来源
                staging/'boot/limine/limine-uefi-cd.bin')  # 将光盘启动映像放进 xorriso 稍后登记的介质路径
subprocess.run([tool('xorriso','xorriso'),'-as','mkisofs','-R','-r','-J',  # 选择 xorriso，使用 mkisofs 参数风格与目录命名扩展建立 ISO
    '--efi-boot','boot/limine/limine-uefi-cd.bin','-efi-boot-part',  # 把这份映像登记为 UEFI 启动项，同时提供 EFI 启动分区形式
    '--efi-boot-image','--protective-msdos-label',str(staging),  # 登记启动映像及保护标签，最后传入待打包目录树
    '-o',str(out/'airios.iso')],check=True)  # 写入此 profile 的 ISO；制镜像工具失败会停止脚本
print(f'Image: {out/"airios.iso"}')  # 打印实际生成的主机镜像路径供启动脚本使用
