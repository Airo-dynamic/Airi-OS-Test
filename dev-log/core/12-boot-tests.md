# K12 · 用完成标记核对真实启动与输入

K11 的串口镜像已能持续处理命令。现在要自动确认两件事：本次启动确实走到就绪位置，送入的一条命令也确实处理完。仅看到 QEMU 进程存在，或仅知道主机已写出 mem 字节，都不足以证明它们。

启动与输入因此各有等待条件。启动检查等待 READY；输入检查先记旧输出尾部，送一条请求，再等新计数的 CONSOLE_UPDATE 和该项内容，完成才继续。UART 和虚拟键盘分别执行，让命令模型与两条设备通路都有实际观察。

normal 之外另构建 no-framebuffer、missing-memory、edit-experiment。它们分别验证无图形仍可交互、缺必需地图按原因拒绝、修改文字进入了本次产物；镜像、日志和汇总按配置隔离。

<a id="qmp-channel"></a>

## 公共机器参数不代替一次运行的独立资源

<a id="qemu-parameters"></a>

将已有 q35、单核、256 MiB 等设置集中进 command。每次调用仍接收自己的 profile/work，先复制可写固件变量，再返回参数列表；Popen 才启动机器。固定固件模板不被修改，不同观察也不借同一次进程。

后三个形参有默认值：gui=False 隐藏主机窗口，debug=False 正常开始执行，debug_port=1234 留给调试。`command(profile,work)` 使用默认值，具名参数可单独覆盖。这里 gui 只改变 QEMU 窗口，内核显示实现仍由构建 profile 选择。

**新建 `scripts/boot.py`：**

<!-- write: scripts/boot.py -->
```python
"""QEMU launch and bounded boot checks; success requires a kernel marker."""  # 固定虚拟机参数并按本轮内核标记判断启动结果
import json  # 在 Python 对象与 JSON 文本之间转换，不执行其中内容。
from pathlib import Path  # Path 表示主机路径对象，/ 在这里拼接路径片段。
import shutil      # 取得目录复制、文件复制与临时结果清理接口。
import socket      # 取得主机通信连接接口，QMP/调试器通过独立连接交接。
import subprocess  # 以参数列表启动已有外部程序，并读取输出或退出码。
import tempfile    # 创建互不冲突的临时路径，用完后显式回收。
import time  # 单调时钟计算截止时间，sleep 给设备与进程处理时间。
from tools import ROOT, tool  # ROOT 确定项目位置，tool 按已建立优先级寻找工具。

def command(profile, work, gui=False, debug=False, debug_port=1234):  # 返回 QEMU 参数列表；profile 选镜像，work 保存本轮可写固件与日志。
    work.mkdir(parents=True,exist_ok=True)  # 建立本轮输出目录，已有目录允许复用。
    firmware=ROOT/'third_party/firmware'    # 定位固定固件目录。
    variables=work/'ovmf-vars.fd'  # 本轮固件变量单独存放，避免改动锁定的原始模板。
    shutil.copyfile(firmware/'edk2-i386-vars.fd',variables)  # 复制变量模板，QEMU 可写副本而保持外部依赖原件。
    cmd=[tool('qemu-system-x86_64','qemu'),'-machine','q35','-accel','tcg',  # 先选 QEMU 路径与 q35/TCG，参数列表按元素边界传递。
         '-cpu','qemu64','-smp','1','-m','256M','-vga','std',  # 固定CPU型号、单核、256MiB及标准VGA。
         '-drive',f'if=pflash,format=raw,unit=0,readonly=on,file={firmware/"edk2-x86_64-code.fd"}',  # 只读代码固件是第0个 pflash 单元，f 字符串填入实际路径。
         '-drive',f'if=pflash,format=raw,unit=1,file={variables}',  # 第1个 pflash 单元使用本轮可写变量副本。
         '-monitor','none','-cdrom',str(ROOT/'build'/profile/'airios.iso'),'-boot','d','-no-reboot',  # 挂载对应配置镜像，从光盘启动，故障时不循环重启。
         '-serial',f'file:{work/"serial.log"}']  # 默认 UART 输出保存到本轮 serial.log。
    if not gui: cmd+=['-display','none']  # 无人操作时隐藏显示窗口，guest 仍可绘图并经 QMP 截帧。
    if debug: cmd+=['-S','-gdb',f'tcp:127.0.0.1:{debug_port}']  # -S 暂停CPU，GDB只监听回环地址指定端口。
    return cmd  # 仅返回参数列表，调用者决定何时启动与回收进程。
```

## 控制 QEMU 和给 guest 输入，是两条连接

<a id="qmp-messages"></a>

要检查键盘转换，主机不能直接替内核调用解码函数，而要请求 QEMU 产生按键。QMP 的 send-key 控制虚拟键盘，cont 使暂停 CPU 继续，内核随后只看到设备事件，不认识 QMP 的 JSON 对象。UART socket 则直接传 mem 等字节，两者的接收者与格式不同。

两条连接都可用主机 UNIX socket 的路径定位。AF_UNIX 选本机地址、SOCK_STREAM 选有序字节流；connect 建立连接，字节流没有自带消息边界。QMP 以换行分 JSON：dumps 形成文字，追加换行、encode 送字节，readline 收一行，loads 还原对象。

连接先消费服务器问候，再发送 qmp_capabilities 与本次请求，如 `{'execute':'cont'}`。回复可能夹有 event，只有 return/error 才结束当前请求。arguments=None 表示不提供字段，空字典仍是显式参数，不能一并略掉。

makefile('rb') 是 socket 的按行二进制包装，与构建 Makefile 无关。嵌套 with 先关包装再关连接，异常也一样。screendump 另给可选图形路线输出主机图像，本章串口不调用它。

**追加到 `scripts/boot.py` 末尾：**

<!-- append: scripts/boot.py -->
```python
def qmp_execute(sockpath, execute, arguments=None):  # 本机连接只执行本次指定的控制请求。
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:  # 外层拥有底层连接，最后关闭。
        sock.settimeout(5)  # 连接、发送和读取最多阻塞五秒。
        sock.connect(str(sockpath))  # 路径定位QEMU控制端点，不是普通输出文件。
        with sock.makefile('rb') as stream:  # 内层包装按行读取字节，先关闭包装再关闭连接。
            json.loads(stream.readline())  # 消费服务器问候，JSON库直接接受bytes。
            result = None  # 尚未收到动作结果，不与空字典混淆。
            request = {'execute': execute}  # 当前控制动作保存为请求字段。
            if arguments is not None:  # 空字典也是显式参数，只有None表示不提供。
                request['arguments'] = arguments  # 将本次参数加到同一请求。
            for req in ({'execute': 'qmp_capabilities'}, request):  # 先协商能力再发实际动作。
                sock.sendall((json.dumps(req) + '\n').encode())  # 直到全部消息字节交出或失败，不假设一次write写完。
                while True:  # 跳过异步事件，等待当前请求的结果。
                    reply = json.loads(stream.readline())  # 按消息换行取得完整JSON字节。
                    if 'error' in reply:  # QEMU拒绝了当前请求。
                        raise RuntimeError(str(reply['error']))  # 保留服务器的具体错误。
                    if 'return' in reply:  # event不满足完成条件。
                        result = reply['return']  # 取得当前请求结果。
                        break  # 此请求已完成，进入下一个或返回。
            return result  # 返回实际动作结果，两个with仍按由内到外关闭资源。

def qmp_screenshot(sockpath, output):  # 把本轮截图目标路径交给QEMU控制端点。
    qmp_execute(sockpath, 'screendump', {'filename': str(output)})  # 控制显示设备保存PPM，不向UART发送命令。
```

## 用配置的预期结果判断本次启动

<a id="boot-observation"></a>

每次先删旧 serial.log/result.json，再启动自己的 QEMU，避免旧 READY 或旧成功 JSON 留下误证。UNIX 端点放短临时目录，规避 macOS 路径长度限制；等待采用单调时钟，不受墙上时间校准影响。

normal、no-framebuffer、edit-experiment 都需完整换行的 READY 且无 PANIC，当前串口均为 graphics=0；修改版还需新文字。missing-memory 则相反，必须有具体缺地图 PANIC 且无 READY，拒绝启动才是预期结果。desktop- 只选择显示实现，去掉前缀仍按相同故障含义判断。

expected 据配置确定，逐行等到完整记录后才保存 result.json。半行继续等，超时或提前退出都失败，已收到的串口与 QEMU 诊断保留，不能写成功汇总。

**追加到 `scripts/boot.py` 末尾：**

<!-- append: scripts/boot.py -->
```python
def check(profile='normal',timeout=35):  # 每次启动最多35秒，成功条件由配置对应完成标记决定。
    work=ROOT/'artifacts'/('boot-'+profile)  # 不同配置分开保存启动证据。
    work.mkdir(parents=True,exist_ok=True)   # 建立本轮输出目录，已有目录允许复用。
    (work/'result.json').unlink(missing_ok=True)  # 删除上次汇总，失败不能留下旧成功身份。
    serial=work/'serial.log'  # 当前检查只读取这次启动的串口文件。
    serial.unlink(missing_ok=True)  # 删去旧日志；不存在不报错，旧 READY 不会使新启动提前通过。
    # AF_UNIX paths are capped at 104 bytes on macOS. Source exports can be deep.
    endpoints=tempfile.TemporaryDirectory(prefix='airios-qmp-',dir='/tmp')  # 在短/tmp路径建立控制端点，避免 macOS UNIX socket 路径过长。
    sockpath=Path(endpoints.name)/'qmp.sock'  # 临时目录中的 qmp.sock 命名本次控制连接。
    cmd=command(profile,work)+['-qmp',f'unix:{sockpath},server=on,wait=off']  # 在固定启动参数后追加控制端点，QEMU不等待客户端才启动。
    (work/'command.json').write_text(json.dumps(cmd,indent=2))  # 保存实际参数列表供本轮复现。
    case = profile.removeprefix('desktop-')  # 图形前缀不改变故障注入含义。
    graphics = profile.startswith('desktop-') and case != 'no-framebuffer'  # 有绘图实现且设备存在才应生成画面。
    expected='[AIRI_PANIC] required Limine memory map is missing' if case=='missing-memory' else '[AIRI_P1_READY]'  # 缺内存响应要看具体 PANIC，其他配置要看 READY。
    started=time.monotonic()  # 单调秒数作为经过时间起点，不受系统时钟调整影响。
```

以本章 normal 调用为例，work 已定位 artifacts/boot-normal，旧结果和串口文件已删除，cmd 选择 build/normal/airios.iso；expected 为 READY。下面才启动这一实例。每次读取先取得完整行，遇到 expected 后再核对该配置的其他条件，最后形成结果；未满足条件时不能写入成功汇总。

**继续追加启动等待与 finally，闭合 check：**

<!-- append: scripts/boot.py -->
```python
    with (work/'qemu.log').open('w') as errors:  # 主机 QEMU 输出单独存档，with 保证文件最终关闭。
        process=subprocess.Popen(cmd,stdout=errors,stderr=errors)  # 启动后立即取得进程句柄，轮询 guest 标记而非等待进程退出。
        try:  # 受保护区域失败也必须进入后面的资源回收。
            while time.monotonic()-started<timeout:  # 按实际经过秒数限制整次启动等待。
                text=serial.read_text(errors='replace') if serial.exists() else ''  # 日志尚未创建时取空串；暂时不完整的字节以替代字符读取。
                complete_lines = text.splitlines(keepends=True)  # 保留行结束符，完整记录与写到一半的标记可区分。
                if any(line.endswith('\n') and line.startswith(expected) for line in complete_lines):  # 生成器逐行检查，至少一个完整且以预期标记开头才成立。
                    if case=='missing-memory':  # 失败配置只允许具体拒绝记录，不能同时宣布启动完成。
                        assert '[AIRI_P1_READY]' not in text  # 错误路径不得出现成功标记。
                    else:  # 按当前profile的另一条完整验证路径处理。
                        assert '[AIRI_PANIC]' not in text  # 正常完成路径不得混入内核停止记录。
                        assert f'graphics={int(graphics)}' in text  # 报告值须与本次选择的呈现实现一致。
                        assert '[MEMORY_SUMMARY]' in text  # 完成之前应已记录实际内存总计。
                        if case=='edit-experiment':  # 修改实验额外核对替换后的文字。
                            assert 'I changed the output and rebooted' in text  # 确认运行的是实验配置的输出行为。
                        if graphics:  # 有图形的完成路径才请求显示面截图。
                            qmp_screenshot(sockpath,work/'screen.ppm')  # 在完整 READY 之后读取对应已完成画面。
                    result={'profile':profile,'passed':True,'expected_marker':expected,  # 记录配置、成功与本次实际等待的标记。
                            'seconds':round(time.monotonic()-started,2)}  # 记录经过秒数，round(...,2) 保留两位小数。
                    (work/'result.json').write_text(json.dumps(result,indent=2)+'\n')  # 结果编码为便于阅读的 JSON，并补文件末尾换行。
                    print(f'PASS boot/{profile} ({result["seconds"]}s)')  # 打印本次配置和耗时，而非以QEMU仍运行判定通过。
                    return result  # 当前配置标记与截图核对成功，返回这次启动记录。
                if process.poll() is not None:  # poll 非 None 表示主机 QEMU 已退出，不能继续等 guest 标记。
                    raise RuntimeError(f'QEMU exited before marker; see {work}')  # 提前退出时报告证据目录。
                time.sleep(0.1)  # 给虚拟CPU和日志写入100毫秒处理时间，再检查。
            raise RuntimeError(f'Boot timeout after {timeout}s; see {work}')  # 耗尽截止时间后明确失败，并保留该配置的日志。
        finally:  # 成功返回与异常离开都进入收尾。
            process.terminate()  # 先请求主机 QEMU 进程结束。
            try:  # terminate只是结束请求，随后仍需收回子进程。
                process.wait(timeout=5)  # 最多等待五秒，防止清理本身无限卡住。
            except subprocess.TimeoutExpired:  # QEMU没有在正常终止期限内退出。
                process.kill()  # 强制结束本轮仍存活的QEMU。
                process.wait()  # 收回已结束子进程的退出状态和资源。
            sockpath.unlink(missing_ok=True)  # 删除本轮控制端点，端点已消失时允许继续。
            endpoints.cleanup()  # 回收临时目录，不删除保留在 artifacts 的证据。
```

finally 请求终止本次 Popen 进程，随后 wait；五秒未退出再 kill/wait。它回收的是自己启动的实例，不按全机进程名字结束其他 QEMU。成功与失败都走清理，保存的日志则留在 artifacts。

<a id="console-observation"></a>

## 发送以后，结果必须来自旧尾部之后

假设会话已处理两项，下一条命令只接受 commands=3。发送前记 offset，再在 `text[offset:]` 搜索完整记录和本条结果；原来 commands=2 的日志无论出现多少次都不能证明这项完成。

UART 模式先用 -S 暂停 CPU，连好设备 socket，再经 QMP cont，避免启动字节在接收方未准备时丢失。非阻塞 recv 暂无数据给 BlockingIOError，空 bytes 表示连接结束；读到的块加进 bytearray，再 decode 为文本。4096 是一次最多读取的字节数，after 则是解码文本里的字符偏移。

嵌套 output/wait 使用同一次 wire、received、serial、process。output 收取并保存累计字节，wait 反复检查新尾部，每次有自己的截止点，进程提前退出立即失败。它保留换行，只移除确定提示符和 CRLF，再按完整 CONSOLE_UPDATE 比较；page=1 不会误接受 page=10。READY 带后续字段，则检查标记后的空格边界，同样只用 after 之后记录。

**新建 `scripts/console_test.py`：**

<!-- write: scripts/console_test.py -->
```python
"""Exercise the running guest through QEMU keys and real UART receive bytes."""  # 两种设备输入均进入真正运行的内核控制台
import json    # 在 Python 对象与 JSON 文本之间转换，不执行其中内容。
import socket  # 取得主机通信连接接口，QMP/调试器通过独立连接交接。
import subprocess  # 以参数列表启动已有外部程序，并读取输出或退出码。
import tempfile    # 创建互不冲突的临时路径，用完后显式回收。
from pathlib import Path  # Path 表示主机路径对象，/ 在这里拼接路径片段。
import time  # 单调时钟计算截止时间，sleep 给设备与进程处理时间。
from boot import command, qmp_execute, qmp_screenshot  # 复用启动、QMP 请求与截图接口，输入检查继续等待 guest 完成记录。
from tools import ROOT  # 共享 ROOT 指向项目根目录，后续路径不依赖调用位置。

def check_console(profile='normal', uart_input=None):  # 每次启动独立guest，按uart_input选择UART字节或PS/2按键。
    graphics = profile.startswith('desktop-') and not profile.endswith('no-framebuffer')  # 根据实际构建选择有无图像断言。
    if uart_input is None:  # 没有显式选择设备时，串口目标使用 UART。
        uart_input = not graphics  # 默认串口目标走UART，桌面目标走PS/2；调用者可显式改选。
    work=ROOT/'artifacts'/('console-'+profile+('-ps2' if not uart_input else '-uart'))  # 不同输入路径保存独立本轮证据。
    work.mkdir(parents=True,exist_ok=True)  # 建立本轮输出目录，已有目录允许复用。
    (work/'result.json').unlink(missing_ok=True)  # 删除上次汇总，失败不能留下旧成功身份。
    serial=work/'serial.log'  # 只读取这次guest对应的日志。
    serial.unlink(missing_ok=True)  # 删除旧完成标记，不存在时允许继续。
    endpoints=tempfile.TemporaryDirectory(prefix='airios-console-',dir='/tmp')  # 短临时目录保存本次控制与UART端点，规避socket路径长度限制。
    qmp=Path(endpoints.name)/'qmp.sock'    # QMP端点只传主机控制请求，不是内核串口。
    uart=Path(endpoints.name)/'uart.sock'  # UART端点传设备收发字节，无图形时也能交互。
    cmd=command(profile,work)+['-qmp',f'unix:{qmp},server=on,wait=off']  # 复用启动参数，再添加本轮QMP服务端点。
    wire=None  # 尚未建立UART输入连接，PS/2路径保持None。
    received=bytearray()  # 可增长数组累积本次设备字节。
    if uart_input:  # 所选输入通路为UART时才建立或读取字节连接。
        cmd[cmd.index('-serial')+1]=f'unix:{uart},server=on,wait=off'  # 找到-serial参数后替换其值，把原文件输出改成设备socket。
        cmd+=['-S']  # 先暂停CPU，UART接收连接建好后再执行，以免丢早期输出。
    started=time.monotonic()  # 单调秒数作为经过时间起点，不受系统时钟调整影响。
```

输入观察也已取得自己的路径与参数。UART 分支改成设备 socket 并加 -S，PS/2 分支继续用文件收串口；两者都尚未启动进程。下面用同一个 process 管住这次运行，并在 UART 接好后才请求 cont。

**继续追加进程启动与 UART 连接准备；try 主体后面继续：**

<!-- append: scripts/console_test.py -->
```python
    with (work/'qemu.log').open('w') as errors:  # 主机 QEMU 输出单独存档，with 保证文件最终关闭。
        process=subprocess.Popen(cmd,stdout=errors,stderr=errors)  # 启动后立即取得进程句柄，轮询 guest 标记而非等待进程退出。
        try:  # 受保护区域失败也必须进入后面的资源回收。
            if uart_input:  # 所选输入通路为UART时才建立或读取字节连接。
                deadline=time.monotonic()+10  # 等待UART端点创建最多十秒。
                while not uart.exists():      # 端点尚未建立时短暂重试，不能立刻connect。
                    if process.poll() is not None or time.monotonic()>deadline:  # CPU暂停仍要求QEMU存活并及时创建端点。
                        raise RuntimeError('UART socket unavailable')  # 端点未建立时拒绝继续连接。
                    time.sleep(.05)  # 暂停50毫秒给设备或日志处理时间，再重复本次检查。
                wire=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM)  # 本机流式UART端点。
                wire.connect(str(uart))  # 接收连接在CPU继续之前建好。
                wire.setblocking(False)  # 暂无字节时立即抛BlockingIOError。
                qmp_execute(qmp,'cont')  # 接收端准备好后通过控制连接让虚拟CPU继续。
```

现在这一实例已经在运行。UART 时 wire 指向已连接的非阻塞 socket，received 从空 bytearray 开始；PS/2 时 wire 为 None，QEMU 自己写 serial.log。output 在两种情况下都返回累计文本，供同一等待器取新的尾部。它是嵌套函数，读取当前 check_console 调用里的这些对象，下一次调用会有另一组对象。

**继续追加累计收取与完整记录等待：**

<!-- append: scripts/console_test.py -->
```python
            def output():  # 取得同一次guest的累计串口文本，UART字节同时保存文件。
                if wire:  # 有UART连接时从该连接读取；否则读取QEMU写出的日志文件。
                    while True:  # 尽量取出当前已有UART字节，无更多数据或连接结束时停止。
                        try:  # 单次非阻塞读取可能暂时无字节。
                            chunk=wire.recv(4096)  # 最多接收4096设备字节。
                        except BlockingIOError:  # 暂无字节不表示设备故障。
                            break  # 停止本轮读取，wait稍后会再调用output。
                        if not chunk:  # 空bytes说明设备连接结束。
                            break  # 结束这一轮字节收集。
                        received.extend(chunk)  # 可变bytearray追加本次真实接收字节。
                    serial.write_bytes(received)  # 把累计设备输出保存在本轮日志，失败时仍可查看。
                    return received.decode(errors='replace')  # 字节解码为文本，异常编码以替代字符呈现而不丢整份记录。
                return serial.read_text(errors='replace') if serial.exists() else ''  # 文件尚未创建时返回空串，等待循环继续。
            def wait(marker,after=0,timeout=10):  # after是旧日志字符偏移，只在之后寻找这次完成标记。
                deadline=time.monotonic()+timeout  # 每次等待按单调时间设置独立截止秒数。
                while time.monotonic()<deadline:   # 剩余时间内读取新的实际guest输出。
                    text=output()  # 获取累计文本，长度可能随guest运行增加。
                    for line in text[after:].splitlines(keepends=True):  # 只检查本次动作之后的行，保留结束符以识别尚未写完的记录。
                        if not line.endswith('\n'):  # 最后一行可能只有半条状态，不能据此通过。
                            continue  # 等后续读取取得完整换行再检查这条记录。
                        record=line.removeprefix('airi> ').removesuffix('\n').removesuffix('\r')  # 只移除已知提示符与CRLF，保留字段本身。
                        matches=record==marker  # CONSOLE_UPDATE要求完整等值，page=1不会匹配page=10。
                        if marker=='[AIRI_P1_READY]':  # READY后还有graphics与内存字段，按标记后的空格确定边界。
                            matches=record.startswith(marker+' ')  # 接受带字段的完整READY行，不接受相似标记名称。
                        if matches:  # 本次输出中已取得完整且匹配的状态记录。
                            return text[after:]  # 把本次动作的新结果交回调用者继续核对正文。
                    if process.poll() is not None:  # 运行进程提前退出，即使旧日志有文字也不能当成本次输入成功。
                        raise RuntimeError('Guest exited during console test')  # 报告真实guest生命周期失败。
                    time.sleep(.05)  # 暂停50毫秒给设备或日志处理时间，再重复本次检查。
                raise RuntimeError(f'Console timeout waiting for {marker}; see {serial}')  # 明确给出缺少的标记和已有日志路径。
```

wait 已能在 after 之后找完整记录。先用默认 after=0 等本轮 READY，才允许键输入。key 把名字转为 QMP 键事件，update 只形成期望文字，capture 则在图形配置下读取已完成画面；这些助手都操作当前 qmp 与 process。

**继续追加就绪等待和输入助手；后面仍在同一个 try 中：**

<!-- append: scripts/console_test.py -->
```python
            wait('[AIRI_P1_READY]',timeout=35)  # 输入前先等内核初始化与首帧结束，最多35秒。
            def key(*codes):  # 形参*把多个键名收集成元组，可同时发送ctrl或shift组合。
                qmp_execute(qmp,'send-key',{'keys':[{'type':'qcode','data':c} for c in codes],'hold-time':20})  # 每个键名转qcode字典，列表作为keys，按住时间单位毫秒。
                time.sleep(.12)  # 两次虚拟键事件间隔120毫秒，轮询内核有时间逐次消费。
            def update(view,commands,page=0):  # 组装与内核输出字段顺序一致的具体完成标记。
                return f'[CONSOLE_UPDATE] view={view} commands={commands} page={page}'  # f字符串把预期页面、累计命令数与内存页号代入文本。
            def capture(path):  # 只有实际图形目标才读取显示设备。
                if graphics:  # 串口或缺帧缓冲目标不产生截图。
                    qmp_screenshot(qmp, path)  # 在命令完成后保存本轮画面。
```

## 两条设备通路共用“发出、等待、核对”的过程

<a id="qmp-keys"></a>

key(*codes) 将多个键名收为元组，例如 shift/a，再用列表推导为每个名字生成 qcode 字典。spc、ret、shift 分别是空格、回车、修饰键，hold-time=20 以毫秒计，调用后再留 0.12 秒给轮询内核处理。通过主机控制产生的仍是虚拟键盘事件。

PS/2 分支稍后实际调用 `run('mem','No allocator accounting yet.')`。run 先以 len(output()) 记旧文本末尾，再把 m、e、m 和回车逐次交给 key。回车增加 guest 的提交计数，主机也通过 nonlocal commands 增加外层同一个预期计数；没有 nonlocal，赋值会另建局部变量。随后 update(0,commands) 形成这次完整状态，wait 用 offset 排除旧记录，返回的新尾部才用于检查 mem 的说明。

UART 分支直接送字节，依次检查 echo、devices、未知命令；第一项就必须同时有 commands=1 与 uart-receive-ok。PS/2 分支另查 mem、帮助、地图、历史、退格与 Shift。F1–F4/分页只核对模型编号，graphics=false 时 capture 不截图；以后图形目标才增加日夜切换和画面。

所有路径符合才将实际 cases 写入汇总。结束时先关 UART，终止并回收该进程，再删两个临时端点；一次调用的资源不会交给下一次观察继续使用。

**追加到 `scripts/console_test.py` 末尾：**

<!-- append: scripts/console_test.py -->
```python
            commands=0  # 本次guest已提交命令数，嵌套run每次递增。
            def run(text,expect):  # 把整条终端命令送虚拟键盘，等待这次回车对应记录并核对输出。
                nonlocal commands     # 修改check_console作用域变量，不在run内另建未初始化的局部计数。
                offset=len(output())  # 先记录旧文本长度，后续只消费它之后的完成与结果。
                for c in text:  # 字符串逐个字符迭代。
                    key('spc' if c==' ' else 'minus' if c=='-' else c)  # 空格/横线用QEMU键名，其余直接送字符名。
                key('ret')   # 回车使guest处理当前完整命令。
                commands+=1  # 预期累计提交数增加一。
                tail=wait(update(0,commands),offset)  # 终端view=0且命令数匹配后才得到本次输出尾部。
                assert expect in tail, f'Missing output: {expect}'  # 缺少当前命令预期文字时用assert明确中止。
```

run 已把一条 PS/2 命令闭合为取旧尾、发键、等新计数、核对文字。UART 不经 key，直接把 echo 字节交给 wire；但同样先记 offset，再等 commands=1 的新记录和回显，之后才能发送第二条。

**继续追加两种设备的实际输入序列：**

<!-- append: scripts/console_test.py -->
```python
            if wire:  # 有UART连接时从该连接读取；否则读取QEMU写出的日志文件。
                offset=len(output())  # 记录第一条UART命令之前的日志末尾。
                wire.sendall(b'echo uart-receive-ok\r')  # bytes中的回车提交这条实际设备命令。
                tail=wait(update(0,1),offset)  # 第一条UART命令完成后取得新增输出。
                assert 'uart-receive-ok' in tail and 'graphics=0' in output()  # 无图形启动仍须实际处理UART输入与echo。
                offset=len(output())  # 第二条命令不复用第一条完成标记。
                wire.sendall(b'devices\n')     # 实际UART字节末尾换行提交。
                tail=wait(update(0,2),offset)  # 等第二条UART命令的完成记录。
                if profile.endswith('no-framebuffer'):  # 缺设备与未启用绘图是两个独立事实。
                    assert 'fb0     display   unavailable' in tail  # 设备查询明确图形不可用。
                offset=len(output())  # 新命令只观察之后的完成记录。
                wire.sendall(b'theme day\n')  # 串口目标没有主题扩展，应明确拒绝。
                wait(update(0,3),offset)      # 等待第三次真实提交。
                assert 'unknown command' in output()[offset:]  # 不把无消费者的主题设置伪装成功。
                cases=['UART receive / command dispatch','serial-only device report']  # 结果只记录此路径实际运行的两项观察。
            else:  # 按当前profile的另一条完整验证路径处理。
                capture(work/'terminal.ppm')  # READY已到达后保存初始终端画面。
                run('help','mem map    Complete physical address inventory')  # 验证帮助输出包含完整物理地图命令说明。
                run('mem','No allocator accounting yet.')     # 验证实际内存描述，不假装已有分配器占用统计。
                run('unknown','unknown command. Type help.')  # 未知命令必须得到具体错误文本。
                run('echo ready','ready')  # 检查文字参数到输出的正常命令通路。
                for code,view,name in [('f2',1,'memory'),('f3',2,'devices'),('f4',3,'journal'),('f1',0,'terminal-active')]:  # 每个三元组同时指定按键、预期视图和截图名称，循环解包三项。
                    offset=len(output())  # 记录切页前日志尾。
                    key(code)  # 按本三元组指定的功能键。
                    wait(update(view,commands),offset)  # 等对应视图新完成记录，命令数不变。
                    capture(work/(name+'.ppm'))  # 对应视图完成后才保存画面。
                    if code=='f2':  # 只有内存页面具有物理区域分页操作。
                        offset=len(output())  # 记录翻页之前的日志尾。
                        key('pgdn')  # 请求向后翻内存地图页。
                        wait(update(1,commands,1),offset)  # 等内存view=1/page=1完成。
                        capture(work/'memory-page2.ppm')   # 保存第二页画面，与刚等待的page=1对应。
                        offset=len(output())  # 此次向前翻页排除刚才page=1记录。
                        key('pgup')  # 请求返回首个内存页。
                        wait(update(1,commands,0),offset)  # 等page=0新完成。
                if graphics:  # 日夜命令属于图形扩展。
                    for mode in ('day', 'night'):  # 两个真实命令都走 PS/2 输入路径。
                        run('theme ' + mode, 'Appearance: ' + mode.title())  # 等待命令完成标记，不以进程仍在运行代替成功。
                        capture( work / ('theme-' + mode + '.ppm'))  # 保存真实 guest 帧缓冲，供与主机样板比较。
                run('mem map','PHYSICAL END')  # 完整地图命令输出应包含区域结束地址列。
                offset=len(output())  # 清屏前排除已有完成记录。
                key('ctrl','l')  # 同时发送Ctrl-L功能组合。
                wait(update(0,commands),offset)  # 等清屏完成，累计命令数保持。
                capture(work/'cleared.ppm')      # 清屏完成后再截帧。
                offset=len(output())  # 记录历史操作开始位置。
                key('up')    # 取回最近的mem map历史文本。
                key('ret')   # 回车形成新的命令提交。
                commands+=1  # 等待新的累计命令数。
                wait(update(0,commands),offset)  # 等待新增命令计数的完成标记。
                assert 'airi> mem map' in output()[offset:]  # 历史回取必须实际提交前一条地图命令。
                offset=len(output())  # 先记录旧文本长度，后续只消费它之后的完成与结果。
                for c in 'syx':  # 先输入错误命令草稿。
                    key(c)  # 将当前字符送到虚拟键盘。
                key('backspace')  # 删除草稿末尾x。
                key('s')     # 补成sys。
                key('ret')   # 提交修复后的命令。
                commands+=1  # 本次实际提交也计入预期数。
                assert 'bootstrap kernel' in wait(update(0,commands),offset)  # 修复后的sys命令应报告当前内核身份。
                offset=len(output())  # 先记录旧文本长度，后续只消费它之后的完成与结果。
                for c in 'echo ':     # 逐个字符准备echo前缀。
                    key('spc' if c==' ' else c)  # 空格转spc键名。
                key('shift','a')  # 同时按Shift和a得到大写A。
                key('9')  # 普通数字9。
                key('shift','1')  # Shift-1转换为标点!。
                key('ret')   # 提交echo A9!。
                commands+=1  # 与guest累计提交计数同步。
                assert 'A9!' in wait(update(0,commands),offset)  # 等待本次完成后核对大写与标点扫描码转换。
                capture(work/'terminal-session.ppm')  # 保存全部命令序列结束时的实际终端画面。
                cases=['PS/2 command dispatch','help/mem/map/echo/errors','F1-F4 guest views','memory paging',  # 记录本次PS/2通路已执行的命令、切页和分页。
                       'Ctrl-L clear','history recall','backspace','shift letters/punctuation']  # 继续记录清屏、历史、退格和Shift观察。
```

所选设备的序列全部通过后，cases 才描述本次实际做过的观察。下面保存结果并返回；finally 仍会关闭本次连接、终止回收同一 process 和短端点，下一轮不继承它们。

**最后追加结果与资源清理，闭合 check_console：**

<!-- append: scripts/console_test.py -->
```python
            result={'profile':profile,'passed':True,'cases':cases,'seconds':round(time.monotonic()-started,2)}  # 各观察成立后才形成结果，耗时来自本次单调时钟。
            (work/'result.json').write_text(json.dumps(result,indent=2)+'\n')  # 结果编码为便于阅读的 JSON，并补文件末尾换行。
            print(f'PASS console/{profile} ({result["seconds"]}s)')  # 按实际路径与耗时报告本轮输入通过。
            return result  # 当前输入路径全部观察成功，返回实际case记录。
        finally:  # 成功返回与异常离开都进入收尾。
            if wire:  # 只有UART通路建立了设备socket。
                wire.close()  # 正常和失败都关闭本轮UART连接。
            process.terminate()  # 先请求主机 QEMU 进程结束。
            try:  # terminate只是请求结束，仍需收回进程。
                process.wait(timeout=5)  # 最多五秒等待正常退出。
            except subprocess.TimeoutExpired:  # QEMU没有在截止时间前退出。
                process.kill()  # 强制结束仍存活的本轮进程。
                process.wait()  # 收回已结束进程，不留下僵尸状态。
            qmp.unlink(missing_ok=True)   # 清理本轮QMP端点。
            uart.unlink(missing_ok=True)  # 清理UART端点，PS/2通路未创建也允许。
            endpoints.cleanup()  # 回收临时目录，不删除保留在 artifacts 的证据。
```

## 分开运行正常、受限与故障配置

先为四种 profile 分别构建并检查启动，再创建三次独立输入观察：normal UART、normal PS/2、no-framebuffer UART。每个 guest 的会话计数从零开始，宏与产物只属于所选目录，不能跨实例拼接成功记录。

**在学习目录根目录运行：**

<!-- run -->
```sh
PYTHONPATH=scripts python3 - <<'PY'  # 从当前工程导入模块，Shell不展开Python正文。
import subprocess  # 按配置生成独立目标。
from tools import make_command  # 使用同一套已查明的工具路径。
from boot import check  # 必须等待本轮完整标记。
from console_test import check_console  # 输入真正运行的guest。
for profile in ('normal', 'no-framebuffer', 'missing-memory', 'edit-experiment'):  # 故障场景分别观察。
    subprocess.run(make_command(profile), check=True)  # 编译并制作当前镜像。
    check(profile)  # 正常与拒绝路径均有具体完成条件。
check_console('normal')  # UART收发。
check_console('normal', uart_input=False)  # PS/2转换与命令输出。
check_console('no-framebuffer')  # 没有显示设备仍可输入。
PY
```

应得到四次 PASS boot 和三次 PASS console，各 artifacts 目录保留对应 serial.log/result.json。missing-memory 的通过表示正确拒绝，no-framebuffer 的通过表示串口仍可处理命令，它们的成功含义不同。

mem 的 usable 仍由 BootInfo 分类长度相加；256 MiB 是整机容量，还包含内核、固件和保留范围。现在启动、命令与拒绝路径都有可调用观察，下一章将它们收拢到日常入口，避免每次手工组合 Python 调用。
