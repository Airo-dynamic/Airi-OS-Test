"""Run two actual QEMU boots against one dedicated, freshly created test disk.""" # 跨VM启动与模型crash分别验证。
import re # 从实际PID和完成记录关联等待区间，不固定猜PID。
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
    (work / 'async-throttle.json').unlink(missing_ok=True) # 失败不能留下上次QMP已接受的身份。
    disk = work / 'disk.raw' # 此测试每次从空盘开始，普通run目录不受影响。
    if disk.is_symlink() or (disk.exists() and (not disk.is_file() or disk.stat().st_size != 8 * 1024 * 1024)): # 未知对象不能当测试盘删除。
        raise RuntimeError('Invalid restart-test disk; refusing to replace') # 明确保留异常路径。
    disk.unlink(missing_ok=True) # 只删除本测试上次生成的8MiB盘，下一command排他创建新盘。
    started = time.monotonic() # 整组测试耗时。
    def boot_once(label, kernel_marker, request, user_marker, slow=False): # 同一work保存盘，端点和CPU每次新建。
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
                    text = wait('[AIRI_P5_READY] ') # 全部初始化和用户演示结束后才输入。
                    assert kernel_marker in text and '[AIRI_PANIC]' not in text # 当前新CPU必须扫描出指定前缀。
                    if slow: # 启动后才节流，前面的块/日志演示不受影响。
                        limits = {'device':'airi-disk', 'bps':0, 'bps_rd':0, 'bps_wr':0, 'iops':1, 'iops_rd':0, 'iops_wr':0} # 明确单IOPS且无额外突发，不限制吞吐字节。
                        qmp_execute(qmp, 'block_set_io_throttle', limits) # QEMU实际限速，不在guest里伪造完成延迟。
                        (work / 'async-throttle.json').write_text(json.dumps(limits, indent=2) + '\n') # 只在QMP接受后记录本次配置。
                    offset = len(text) # 用户命令只消费之后的新输出。
                    wire.sendall((request + '\n').encode()) # 通过实际UART提交控制台命令。
                    text = wait('[CONSOLE_UPDATE] view=0 commands=1 page=0', offset) # 等命令与用户会话全部收拢。
                    assert user_marker in text[offset:] and '[AIRI_PANIC]' not in text[offset:] # 标记来自本次ring3程序。
                    return text[offset:] # 交出本次请求的完整真实区间，finally仍先回收QEMU。
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
    tail = boot_once('async', '[STORAGE_RECOVERED] count=1 truncated=0', 'run async', '[ASYNC_USER_OK]', slow=True) # 同一正式日志保留，第三个新CPU单独验证慢设备上的CQ等待。
    match = re.search(r'\[FORK\] parent=(\d+) child=(\d+)', tail) # 当前命令第一次fork给出真实根PID。
    assert match is not None # 不把启动时旧PID或固定常量混入本次证据。
    pid = match.group(1) # 字符串PID用于精确字段匹配。
    progress = tail.index('[ASYNC_OTHER_PROGRESS]') # 另一个真正用户取得CPU后的进展。
    blocked = False # 必须找到一次父CQ阻塞覆盖这条进展。
    for registered in re.finditer(r'\[WAIT_REGISTER\] pid=' + pid + r' operation=cq\r?\n', tail): # 每个候选都是本次父请求的实际登记。
        wake = tail.find('[WAIT_WAKE] pid=' + pid + ' result=0\r\n', registered.end()) # 找此登记之后同PID的结果交付。
        if registered.start() < progress < wake: # 子任务进展发生在同一次父调用等待期间。
            blocked = True # 不是凭QEMU仍运行推断并发。
    assert blocked and 'phase=start' in tail and 'phase=complete' in tail # 真设备接收、完成和其他用户继续必须同时成立。
    result = {'passed':True, 'profile':profile, 'boots':3, 'restart_boots':2, 'recovered_records':1, 'throttled_async_iops':1, 'other_user_during_cq_wait':True, 'seconds':round(time.monotonic() - started, 2), 'model':'QEMU process restart, not physical power loss'} # 只记录实际完成的模型。
    result_path.write_text(json.dumps(result, indent=2) + '\n') # 两次均完成才覆盖成功。
    print('PASS storage sessions: two-boot recovery + QMP-throttled CQ wait with other user progress') # 不用宿主读取文件猜客体行为。
    return result # 统一入口可收集当前结果。
