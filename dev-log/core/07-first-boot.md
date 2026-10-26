# 07 · 把 ELF 放入 UEFI 镜像并看到第一行

已有内核 ELF，尚未有机器装载它。本章把载荷放进启动介质，交给 OVMF，再等待内核发送 `_start → C → UART` 的标记。主机的编译文件、介质的目录与固件实际读取的启动映像需要分清，否则同名 EFI 文件容易造成装载关系的误判。

ISO 表示光盘文件与启动信息，QEMU 把它当虚拟光盘。主机 `build/normal/airios.elf` 会复制到其中的 `/boot/airios.elf`，配置引用后者；guest 不会按这条路径去打开 Mac 工程目录。

本次 UEFI 光盘入口由 xorriso 登记的 `limine-uefi-cd.bin` 提供。它内部已有一份 FAT 文件系统，包含 `/EFI/BOOT/BOOTX64.EFI`；OVMF 从该启动映像取得 EFI 程序。外层 ISO 也保存同名 EFI 路径，但不能据此把它当成这次光盘启动的直接读取来源。Limine 固定版本的[构建规则](https://github.com/Limine-Bootloader/Limine/blob/v12.9.2/GNUmakefile.in#L229-L246)明确先建立 FAT，再把 EFI 目录放入其中。

EFI 程序开始运行以后，Limine 再找到介质上的 limine.conf，按配置读取外层 `/boot/airios.elf`，装入并跳到 `_start`。前一次装载启动引导程序，后一次装载自己的内核；编译 ELF 与打包介质分别为它们提供输入。

## 配置指向介质里的内核

`timeout: 0` 跳过菜单倒计时，verbose/serial 选择引导器自己的详细和串口输出，不是调用内核日志函数。`/AiriOS` 是菜单项，缩进字段归它；protocol 指交接协议，`boot():/boot/airios.elf` 指启动介质上的载荷。

resolution 只是请求尺寸，实际可用帧缓冲由响应决定。kaslr=no 关闭内核地址随机化，使固定链接和调试地址一致；随机化本用于增加猜测地址的难度，当前先保留可重复布局。

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

## 主机先组织目录树，制镜像工具再写入启动信息

image.py 接收 Make 传来的 profile，`sys.argv[1]` 选择 build/normal 等目录。它先删除旧 staging，再重建本次目录树，防止已不需要的旧文件混入新镜像；其他配置继续使用各自目录。

四次 copyfile 分别放入内核、根目录配置、外层 EFI 文件和已经含有 FAT/EFI 内容的 CD 映像。copyfile 只是主机文件复制，不会重新制作 CD 映像内部的 FAT。`xorriso -as mkisofs` 将 staging 写成 ISO，`--efi-boot` 等参数登记现成映像并提供相应启动布局，QEMU 随后才能使用它。

每个路径仍是 argv 的独立元素。至此增加的是磁盘数据，脚本成功打印 Image 只说明打包完成，还没有执行内核。

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

## 用本次串口输出确认装载后实际执行

<a id="host-guest-output"></a>

主机脚本运行 QEMU，guest 在其中执行内核；脚本不直接读取内核局部数组，而用 `-serial file:路径` 接收 COM1 字节到 serial.log。QEMU 自身 stdout/stderr 写 qemu.log，分别保存 guest 输出与主机虚拟机诊断。

`-display none` 关闭图形窗口，串口仍工作；`-monitor none` 关闭 QEMU 自己的监控台，避免批处理标准输入接错。连接方式见 [QEMU 字符设备选项](https://www.qemu.org/docs/master/system/invocation.html#character-device-options)。

<a id="qemu-machine"></a>

机器固定为 q35 主板、qemu64 CPU、单 CPU、256 MiB，采用 TCG 将 guest 指令翻译为主机操作，所以 Apple Silicon 也能运行 x86-64 内核。cdrom 选择新 ISO，boot d 优先光盘；pflash 两份 drive 分别放只读固件 code 与本轮可写 vars 副本，参考固件保持不变。

Popen 启动后立即交回进程对象，Python 可以继续读日志。启动前删除旧 serial.log，再用 monotonic 建立 90 秒截止点；单调时间不随墙上时钟校准倒退。poll 为 None 表示仍运行，整数表示已经退出，未取到标记就退出必须失败。

每 0.1 秒读一次可能遇到半行。splitlines 保留行尾，只接受以标记开头且换行已到的完整行；读到它才打印结果并 break。旧日志先删除、半行不接受，分别避免旧成功与尚未发完的输出被当成本轮完成。

while 的 else 在没有 break 时报告超时，finally 则无论成功或异常都回收 QEMU：先 terminate，等五秒，仍不退出再 kill/wait。guest 的 hlt 不会替它完成这项主机进程清理。

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

材料沿用第 02 章锁定版本。先执行 image，Make 会先满足 ELF 依赖，再打包本轮 ISO；随后 first_boot 才启动机器并等待设备输出。两步任一失败都不能用已有文件或旧标记替代。

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

应收到 `[AIRI_LESSON_READY] _start -> C -> UART`。它位于 C 入口的成功分支，说明本次已装入载荷、建立栈、进入 C 并通过 UART 送出记录，而不是仅仅成功生成了 ISO。

90 秒未有完整标记，或 QEMU 提前退出，脚本都失败并结束进程。查看 `build/first-boot/qemu.log` 与 serial.log，可区分主机虚拟机错误和 guest 已发送的内容。入口已能报告状态，下一章才把真实引导内存响应交给它。
