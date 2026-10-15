# 07 · 把 ELF 放入 UEFI 镜像并看到第一行

内核 ELF 已经包含 `_start → airi_limine_start → UART`。启动它还需要把 ELF、Limine 和配置放在同一介质上。OVMF 从 UEFI 介质入口 BOOTX64.EFI 启动 Limine，Limine 再按配置找到内核路径并交接。

ISO 是表示一张光盘全部文件及启动信息的镜像文件。QEMU 把它作为虚拟光盘读取，里面的路径与 Mac 工程目录不是同一个文件系统。Mac 上的 `build/normal/airios.elf` 将被复制成介质里的 `/boot/airios.elf`；Limine 配置引用后者，不能让虚拟机去打开 Mac 的工程路径。

这次介质有两次装载：OVMF 先取得 `/EFI/BOOT/BOOTX64.EFI` 这个 UEFI 启动程序，也就是 Limine；Limine 再读 limine.conf，找到内核 ELF，装入并跳到 `_start`。把源码编译成 ELF 完成代码生成，把 ELF 放进介质才让这两次装载有文件可读。

## 将内核放入启动介质

`timeout: 0` 让 Limine 不等待菜单倒计时，verbose 和 serial 请求详细信息与引导器自身的串口记录；它们不是内核的日志函数。`/AiriOS` 为启动菜单项名称，缩进字段属于这一项。protocol 选择 Limine 交接协议；path 中 `boot():` 指启动介质，后面的 `/boot/airios.elf` 指介质内文件。resolution 请求显示尺寸，实际帧缓冲仍由响应决定。kaslr 关闭内核地址随机化，使当前固定链接地址和调试观察一致。KASLR 原本会让内核加载地址在不同启动中变化，以增加猜测地址的难度。

**新建 `config/boot/limine.conf`：**

<a id="code-config-boot-limine-conf-1"></a>

<!-- write: config/boot/limine.conf -->
```text
timeout: 0
verbose: yes
serial: yes

/AiriOS
    protocol: limine
    path: boot():/boot/airios.elf
    resolution: 1280x800x32
    kaslr: no
```

staging 是打包之前的临时目录树。脚本让它先具有介质希望看到的目录结构，再让 xorriso 把这棵树写入一个文件。重新构建前删除旧 staging，旧文件就不会因为本次没再复制而混进镜像；待输出目录则继续由 profile 区分。

Make 把 profile 作为脚本第一个参数传入，`sys.argv[1]` 因此决定使用 `build/normal` 等目录。

四次 copyfile 放入内核、配置、BOOTX64.EFI 与引导 CD 映像。`xorriso -as mkisofs` 使用制镜像接口，efi-boot 等参数将 UEFI 映像登记到 ISO。它生成磁盘数据，之后 QEMU 才会读取这份介质。路径各占参数列表的一项。

**新建 `scripts/image.py`：**

<a id="code-scripts-image-py-1"></a>

<!-- write: scripts/image.py -->
```python
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
shutil.copyfile(ROOT/'third_party/limine/BOOTX64.EFI',staging/'EFI/BOOT/BOOTX64.EFI')  # 放入 OVMF 按 UEFI 默认路径启动的 Limine 程序
shutil.copyfile(ROOT/'third_party/limine/limine-uefi-cd.bin',  # 选择已校验的 UEFI 光盘启动映像作为复制来源
                staging/'boot/limine/limine-uefi-cd.bin')  # 将光盘启动映像放进 xorriso 稍后登记的介质路径
subprocess.run([tool('xorriso','xorriso'),'-as','mkisofs','-R','-r','-J',  # 选择 xorriso，使用 mkisofs 参数风格与目录命名扩展建立 ISO
    '--efi-boot','boot/limine/limine-uefi-cd.bin','-efi-boot-part',  # 把这份映像登记为 UEFI 启动项，同时提供 EFI 启动分区形式
    '--efi-boot-image','--protective-msdos-label',str(staging),  # 登记启动映像及保护标签，最后传入待打包目录树
    '-o',str(out/'airios.iso')],check=True)  # 写入此 profile 的 ISO；制镜像工具失败会停止脚本
print(f'Image: {out/"airios.iso"}')  # 打印实际生成的主机镜像路径供启动脚本使用
```

## 等待这次启动的串口记录

<a id="host-guest-output"></a>

主机是运行脚本与 QEMU 的当前电脑，guest 是 QEMU 内那台正在执行 AiriOS 的机器。脚本不能直接读取内核局部数组，必须通过约定的设备通路取得结果。`-serial file:路径` 将 guest COM1 的发送字节写入主机 serial.log；QEMU 自身的错误另接到 qemu.log。两份日志分别来自不同执行者。

`-display none` 关闭 QEMU 的图形窗口，串口通路仍工作。`-monitor none` 关闭 QEMU 自己的文字监控台，使批量观察脚本的标准输入不再接到监控台。脚本读到内核发送的完整标记后才打印日志。QEMU 的输出连接方式见[字符设备与串口选项](https://www.qemu.org/docs/master/system/invocation.html#character-device-options)。

<a id="qemu-machine"></a>

观察脚本固定 q35、qemu64、单 CPU、256 MiB 和 TCG。cdrom 接刚制好的 ISO，boot d 选择光盘。

q35 选择模拟的 PC 主板与设备组合，qemu64 选择虚拟 CPU 型号，smp=1 是一颗虚拟 CPU。TCG 是 QEMU 将 guest 指令翻译成主机可执行操作的机制，Apple Silicon 因而能运行这份 x86-64 内核。pflash 表示固件使用的闪存设备，两份 drive 分别装入 code 和本次 vars 副本。

`Popen` 启动 QEMU 后立即返回进程对象，脚本可以同时检查串口日志。启动前删除旧日志，随后设置 90 秒截止时间。读取时保留行尾换行，只接受完整的内核标记行；poll 返回整数则说明 QEMU 已退出，本次启动不再能产生结果。

单调时钟 monotonic 用于测量经过时间，它不随墙上时钟校准倒退。`deadline=当前值+90`，循环只在此之前继续等待；QEMU 仍运行时 poll 返回 None。完整标记连换行都已到达，才说明发送循环提供了这条记录。

日志每 0.1 秒读取一次，所以最后一行可能仍只有前半段。splitlines(keepends=True) 保留换行，检查 endswith 换行排除尚未收全的行。启动前删除旧 serial.log 则排除上一次运行留下的成功。

循环的 else 在没有 break 时执行，负责报告超时。finally 在成功或异常后均终止 QEMU，先等待五秒，必要时 kill 并 wait 回收进程。

**新建 `scripts/first_boot.py`：**

<a id="code-scripts-first-boot-py-1"></a>

<!-- write: scripts/first_boot.py -->
```python
"""Bounded serial-only observation before the full QMP test harness exists."""  # 只按本次串口标记观察启动，超过截止时间即失败
from pathlib import Path  # 将文件名包装为可拼接、可读取的路径对象
import shutil      # 复制文件及移除旧的打包目录树
import subprocess  # 启动主机工具，取得退出状态或保存进程对象
import time  # 用单调时钟等待启动，并在两次检查之间暂停
from tools import ROOT, tool  # 借用工程根路径及已确定的工具查找顺序

work = ROOT / 'build/first-boot'  # 把本轮启动副本与观察日志放在独立目录
work.mkdir(parents=True, exist_ok=True)   # 建立观察目录，已有目录中保留其他文件
firmware = ROOT / 'third_party/firmware'  # 固定固件缓存仅作为输入，不直接修改变量镜像
shutil.copyfile(firmware / 'edk2-i386-vars.fd', work / 'vars.fd')  # 为这轮 QEMU 复制可写变量文件，原始固件缓存保持固定
serial = work / 'serial.log'    # 主机用此文件接收 guest COM1 发出的字节
serial.unlink(missing_ok=True)  # 先删旧串口记录；不存在时不报错，避免旧成功被误认
command = [tool('qemu-system-x86_64', 'qemu'), '-machine', 'q35', '-accel', 'tcg',  # 选择 QEMU 并固定 q35 PC 与跨架构 TCG 执行
           '-cpu', 'qemu64', '-smp', '1', '-m', '256M', '-vga', 'std', '-display', 'none', '-monitor', 'none', '-no-reboot',  # 固定 CPU、单核和内存；关闭图形窗口，复位时不自动重启
           '-drive', f'if=pflash,format=raw,unit=0,readonly=on,file={firmware / "edk2-x86_64-code.fd"}',  # unit 0 只读装入固件机器码，f 字符串嵌入实际文件路径
           '-drive', f'if=pflash,format=raw,unit=1,file={work / "vars.fd"}',  # unit 1 装入本轮可写固件变量副本
           '-cdrom', str(ROOT / 'build/normal/airios.iso'), '-boot', 'd',  # 把当前 normal 镜像作为光盘，并优先从光盘启动
           '-serial', f'file:{serial}']  # 将 COM1 输出连接到此轮主机日志文件，列表到此闭合
with (work / 'qemu.log').open('w') as errors:  # 打开 QEMU 自身诊断文件；退出此范围时关闭主机文件句柄
    process = subprocess.Popen(command, stdout=errors, stderr=errors)  # 启动 QEMU 后立即取得进程对象；其 stdout/stderr 都写诊断文件
    try:  # 把本轮等待异常与 finally 中的进程清理关联起来
        deadline = time.monotonic() + 90    # 单调时钟的当前值加 90 秒，得到本轮等待截止点
        while time.monotonic() < deadline:  # 只在截止点之前继续读本轮串口记录
            text = serial.read_text(errors='replace') if serial.exists() else ''  # 日志存在才读取；未完整解码的字节替换成提示字符，尚无文件则用空串
            if any(line.endswith('\n') and line.startswith('[AIRI_LESSON_READY]')  # 同时要求完整换行和内核标记前缀，残缺行不能判断成功
                   for line in text.splitlines(keepends=True)):  # 保留每行尾部换行，逐行给 any 检查，直到取得一条完整标记
                print(text, end='')  # 显示本轮串口文字；不额外添加换行，因为日志已有行尾
                break  # 已取得完整成功标记，结束等待并进入 finally 回收 QEMU
            if process.poll() is not None:  # poll 返回退出状态时说明 QEMU 已结束，无法继续等待新标记
                raise RuntimeError('QEMU exited before the complete serial marker')  # 未看到标记就退出，报告启动失败而非把进程结束当成功
            time.sleep(0.1)  # 两次读日志之间暂停 0.1 秒，避免主机持续忙等
        else:  # while 未通过成功标记执行 break，说明等到截止时间仍无完整标记
            raise RuntimeError('Boot timed out after 90 seconds')  # while 无 break 地到达截止点，报告本轮等待超时
    finally:  # 无论取得标记还是异常，都进入 QEMU 清理路径
        process.terminate()  # 先请求主机 QEMU 进程正常终止，guest 的 hlt 不会代替这个动作
        try:  # 先尝试带五秒上限的等待，超时才进入 kill 路径
            process.wait(timeout=5)  # 最多等五秒并回收 QEMU 的退出状态
        except subprocess.TimeoutExpired:  # QEMU 未在清理期限内退出，升级为强制结束
            process.kill()  # 强制结束仍未退出的 QEMU 进程
            process.wait()  # 回收被强制结束的进程，避免留下未收取的子进程状态
```

沿用第 02 章已经准备好的固定依赖，先执行 image 目标，再运行观察脚本。脚本取得新串口记录后打印成功行，结束本次虚拟机。

**在学习目录根目录运行：**

<!-- run -->
```sh
PYTHONPATH=scripts python3 - <<'PY'  # 标准输入中到下一独立 PY 行之前的内容交给 Python，不由 shell 展开
import subprocess  # 启动主机工具，取得退出状态或保存进程对象
from tools import make_command  # 取得指向当前工具和目标的 Make 参数列表
subprocess.run(make_command(target='all'), check=True)  # 按当前源文件生成 ELF 与 ISO；任一步非零退出停止此观察
PY
python3 scripts/first_boot.py  # 启动本轮虚拟机并等待完整串口标记，成功或失败后都回收进程
```

输出应包含 `[AIRI_LESSON_READY] _start -> C -> UART`。CPU 已经过汇编入口，建立栈，调用 C，并将文字写到设备端口。

超过 90 秒仍未取得完整标记时，脚本失败并结束 QEMU。`build/first-boot/qemu.log` 保存虚拟机错误，`serial.log` 保存内核输出，可据此区分失败发生在哪一侧。入口已经可观察，内存信息仍需从引导响应取得。

