"""Run two actual QEMU boots against one dedicated, freshly created test disk.""" # 跨VM启动与模型crash分别验证。
import json # 记录本轮事实，不执行JSON内容。
from pathlib import Path # 构造项目内独立证据路径。
import socket # 实际UART收发。
import subprocess # 启动和回收真实QEMU。
import tempfile # 短端点路径避免macOS UNIX长度上限。
import time # 单调截止时间不受墙钟变化影响。
from boot import command, qmp_execute # 固定硬件与实际QMP继续。
from tools import ROOT # 不依赖调用者当前目录。

def check_storage_restart(profile='normal'): # 对当前构建正常或图形正常配置运行两次。
    work = ROOT / 'artifacts' / ('storage-restart-' + profile) # 仅此测试拥有的实验盘。
    work.mkdir(parents=True, exist_ok=True) # 证据父目录必须存在。
    result_path = work / 'result.json' # 本轮状态路径。
    result_path.write_text(json.dumps({'passed':False, 'status':'running'}) + '\n') # 旧成功不能覆盖新失败。
    disk = work / 'disk.raw' # 此测试每次从空盘开始，普通run目录不受影响。
    if disk.is_symlink() or (disk.exists() and (not disk.is_file() or disk.stat().st_size != 8 * 1024 * 1024)): # 未知对象不能当测试盘删除。
        raise RuntimeError('Invalid restart-test disk; refusing to replace') # 明确保留异常路径。
    disk.unlink(missing_ok=True) # 只删除本测试上次生成的8MiB盘，下一command排他创建新盘。
    started = time.monotonic() # 整组测试耗时。
    def boot_once(label, kernel_marker, request, user_marker): # 同一work保存盘，端点和CPU每次新建。
        with tempfile.TemporaryDirectory(prefix='airios-store-', dir='/tmp') as directory: # 短临时路径仅放通信端点。
            qmp = Path(directory) / 'qmp.sock' # QMP不是设备输出。
            uart = Path(directory) / 'uart.sock' # UART在CPU继续前连接。
            cmd = command(profile, work) # 复制本轮固件变量，保持同一个disk.raw。
            cmd[cmd.index('-serial') + 1] = f'unix:{uart},server=on,wait=off' # 串口改成真实收发端点。
            cmd += ['-S', '-qmp', f'unix:{qmp},server=on,wait=off'] # 先暂停，接收端准备好才继续。
            serial = work / (label + '.log') # 每个新CPU单独记录，不能读取上次READY。
            data = bytearray() # 只累积本次设备字节。
            with (work / (label + '-qemu.log')).open('w') as errors: # QEMU自身诊断与内核输出分开。
                process = subprocess.Popen(cmd, stdout=errors, stderr=errors) # 启动真实独立进程。
                wire = None # 连接失败也进入finally清理。
                try: # 每个失败都必须收回QEMU。
                    deadline = time.monotonic() + 10 # 端点等待另有总时间上限。
                    while not uart.exists(): # 等待服务端创建端点。
                        if process.poll() is not None or time.monotonic() > deadline: # 已退出或耗时过长。
                            raise RuntimeError('Restart-test UART unavailable') # 不能无限等。
                        time.sleep(.05) # 给QEMU建立端点的时间。
                    wire = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) # 本机UART流，不走网络服务。
                    wire.connect(str(uart)) # CPU仍暂停，早期输出不会丢失。
                    wire.setblocking(False) # 暂无字节不阻塞整个控制循环。
                    qmp_execute(qmp, 'cont') # 接收端存在后继续CPU。
                    def output(): # 累积本次所有已到设备字节。
                        while True: # 取到当前无更多数据为止。
                            try: # 非阻塞接收暂时无数据是正常情况。
                                chunk = wire.recv(4096) # 分块读，不假定一条消息一次到齐。
                            except BlockingIOError: # 稍后再试。
                                break # 本轮读完。
                            if not chunk: # 设备流关闭。
                                break # 后续进程状态给出失败。
                            data.extend(chunk) # 保留原字节顺序。
                        serial.write_bytes(data) # 失败前也保存已收到证据。
                        return data.decode(errors='replace') # 不因单个解码问题丢整份日志。
                    def wait(marker, offset=0): # 只在本次请求后的完整行内找标记。
                        deadline = time.monotonic() + 35 # 真实目标标记最多等待35秒。
                        while time.monotonic() < deadline: # 使用单调截止时间。
                            text = output() # 读取本次设备输出。
                            for line in text[offset:].splitlines(keepends=True): # 保留结束符以拒绝半行。
                                record = line.removeprefix('airi> ').removesuffix('\n').removesuffix('\r') # 只移除已知提示符与CRLF。
                                if line.endswith('\n') and (record == marker or (marker.endswith(' ') and record.startswith(marker))): # 完整等值或明确带空格字段边界，拒绝半行和相似命令计数。
                                    return text # 交回实际累计输出。
                            if process.poll() is not None: # 提前退出不能通过。
                                raise RuntimeError(f'Guest exited before {marker}') # 保留目标身份。
                            time.sleep(.05) # 等设备与CPU推进。
                        raise RuntimeError(f'Restart test timed out: {marker}; see {serial}') # 明确失败文件。
                    text = wait('[AIRI_P4_READY] ') # 全部初始化和用户演示结束后才输入。
                    assert kernel_marker in text and '[AIRI_PANIC]' not in text # 当前新CPU必须扫描出指定前缀。
                    offset = len(text) # 用户命令只消费之后的新输出。
                    wire.sendall((request + '\n').encode()) # 通过实际UART提交控制台命令。
                    text = wait('[CONSOLE_UPDATE] view=0 commands=1 page=0', offset) # 等命令与用户会话全部收拢。
                    assert user_marker in text[offset:] and '[AIRI_PANIC]' not in text[offset:] # 标记来自本次ring3程序。
                finally: # 正常和错误均收回设备与CPU。
                    if wire is not None: # 建立过才关闭。
                        wire.close() # 结束本次UART连接。
                    process.terminate() # 这是受控进程结束，不冒充真实主机掉电。
                    try: # 请求结束还需确认进程已回收。
                        process.wait(timeout=5) # 清理也有截止时间。
                    except subprocess.TimeoutExpired: # QEMU未及时退出。
                        process.kill() # 强制结束本测试子进程。
                        process.wait() # 不遗留僵尸。
    boot_once('first', '[STORAGE_RECOVERED] count=0 truncated=0', 'run storage', '[STORAGE_USER_OK]') # 空盘上由用户实际追加并同步。
    boot_once('second', '[STORAGE_RECOVERED] count=1 truncated=0', 'run recover', '[RECOVERY_USER_OK]') # 新CPU和新内存从同一盘恢复。
    result = {'passed':True, 'profile':profile, 'boots':2, 'recovered_records':1, 'seconds':round(time.monotonic() - started, 2), 'model':'QEMU process restart, not physical power loss'} # 只记录实际完成的模型。
    result_path.write_text(json.dumps(result, indent=2) + '\n') # 两次均完成才覆盖成功。
    print('PASS storage restart: ring3 append/flush -> new QEMU -> scan/read') # 不用宿主读取文件猜客体行为。
    return result # 统一入口可收集当前结果。
