"""Observe a real persistent user supervisor across repeated UART shell exits.""" # 不把宿主脚本当用户shell。
import hashlib # 只核对测试盘内容是否因自动启动改变，不代替掉电模型。
import json # 记录本轮真实结果。
from pathlib import Path # 短控制端点。
import re # 按实际PID关联等待和进展。
import socket # QEMU的UART双向字节流。
import subprocess # 只管理本测试子进程。
import tempfile # 避免UNIX路径长度上限。
import time # 单调总截止。
from boot import command, qmp_execute # 固定硬件与真实限速。
from tools import ROOT # 不依赖调用目录。
def check_init(profile='normal'): # 默认串口主线，不读取frontend或字体。
    automatic=profile.startswith('service') # 服务镜像直接启动init，normal仍从监控台显式启动。
    recovery=profile=='service-init-failure' # 本配置期待装载拒绝后回到monitor。
    work=ROOT/('artifacts/init-'+profile); work.mkdir(parents=True,exist_ok=True) # 本测试独占目录。
    result_path=work/'result.json'; result_path.write_text('{"passed":false}\n') # 旧成功立即撤销。
    disk=work/'disk.raw' # 仅测试盘可重置，不动交互run的正式实验盘。
    if disk.is_symlink() or (disk.exists() and (not disk.is_file() or disk.stat().st_size!=8*1024*1024)): # 未知对象不替换。
        raise RuntimeError('Invalid shell-test disk') # 保留异常路径供检查。
    disk.unlink(missing_ok=True) # 下一command排他创建空盘。
    with tempfile.TemporaryDirectory(prefix='airios-init-',dir='/tmp') as directory: # 本次端点随测试销毁。
        qmp=Path(directory)/'qmp.sock'; uart=Path(directory)/'uart.sock' # 控制与设备分开。
        cmd=command(profile,work); cmd[cmd.index('-serial')+1]=f'unix:{uart},server=on,wait=off' # 实际UART而非内存注入。
        cmd+=['-S','-qmp',f'unix:{qmp},server=on,wait=off'] # 连接UART后才继续CPU。
        (work/'command.json').write_text(json.dumps(cmd,indent=2)+'\n') # 固定本次QEMU参数。
        initial_disk=hashlib.sha256(disk.read_bytes()).hexdigest() # command已排他创建本测试盘，启动前保存完整内容摘要。
        with (work/'qemu.log').open('w') as errors: # QEMU诊断单独保存。
            process=subprocess.Popen(cmd,stdout=errors,stderr=errors); wire=None; data=bytearray() # 保留真实累积字节。
            try: # 任何失败都回收自己的QEMU。
                deadline=time.monotonic()+10 # 等待端点不能无限。
                while not uart.exists(): # CPU还未开始启动。
                    if process.poll() is not None or time.monotonic()>deadline: raise RuntimeError('Init UART unavailable') # 明确失败。
                    time.sleep(.05) # 给QEMU建立端点的机会。
                wire=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM); wire.connect(str(uart)); wire.setblocking(False) # 接收端在CPU之前准备好。
                qmp_execute(qmp,'cont') # 真实继续CPU。
                def output(): # 即使失败也保留已到达的日志。
                    while True: # 取到本轮暂时没有更多字节。
                        try: chunk=wire.recv(4096) # 流不保证一行一次到齐。
                        except BlockingIOError: break # 没字节不是EOF。
                        if not chunk: break # 进程状态由wait检查。
                        data.extend(chunk) # 保持串口真实顺序。
                    (work/'serial.log').write_bytes(data) # 及时落证据。
                    return data.decode(errors='replace') # 诊断不因单个字节丢失。
                def wait(marker,offset=0): # 只接受当前请求后已完整结束的记录。
                    end=time.monotonic()+35 # 每个操作有界。
                    while time.monotonic()<end: # 不重置超时预算。
                        text=output(); tail=text[offset:] # 旧启动输出不能满足新请求。
                        for line in tail.splitlines(keepends=True): # 半行不能被当成功。
                            record=line.removeprefix('airi> ').removesuffix('\n').removesuffix('\r') # 只移除已知提示符。
                            if line.endswith('\n') and (record==marker or (marker.endswith(' ') and record.startswith(marker))): return text # 明确完整记录。
                        if '[AIRI_PANIC]' in tail or process.poll() is not None: raise RuntimeError(f'Shell failed before {marker}') # 不把QEMU存活当成功。
                        time.sleep(.02) # 给真实设备与CPU推进。
                    raise RuntimeError(f'Shell timeout: {marker}; see {work}/serial.log') # 可定位失败证据。
                if automatic: # 不发送run init；服务必须从复位自行到达用户入口。
                    text=wait('[AIRI_SERVICE_RESOURCES_READY] ') # 此标记只声明内核资源，下面还需真实shell命令。
                    assert '[BOOT_MODE] name=service' in text and '[HEAP_READY] bytes=65536' in text # 没有借诊断模式扩堆或初始化。
                    assert all(marker not in text for marker in ('[TRAP_DEMO_OK]','[PAGES_DEMO_OK]','[HEAP_CAPACITY_OK]','[BLOCK_DEMO_OK]','[JOURNAL_DEMO_OK]','[USER_DEMO_OK]','[AIRI_P5_READY]')) # 跳过的演示不能发布完成。
                    expected_fb=0 if profile=='service-no-framebuffer' else 1 # 两个正常目标分别验证描述缺失与未绘图。
                    assert 'storage=recovered framebuffer='+str(expected_fb) in text # 从真实BootInfo而非宿主截图猜设备状态。
                    assert hashlib.sha256(disk.read_bytes()).hexdigest()==initial_disk # 自动启动没有更改新实验盘的内容。
                    if recovery: # 截断ELF必须明确拒绝，并保留可操作的串口恢复位置。
                        text=wait('[SERVICE_RECOVERY] init=broken result=-8 monitor=UART') # launch已验证失败资源恢复。
                        begin=len(text); wire.sendall(b'echo service-recovered\n'); text=wait('[CONSOLE_UPDATE] view=0 commands=1 page=0',begin) # 恢复路径仍能消费真实UART。
                        assert 'service-recovered' in text[begin:] # 不只是打印一个无法操作的提示符。
                        begin=len(text); wire.sendall(b'mem\n'); text=wait('[CONSOLE_UPDATE] view=0 commands=2 page=0',begin) # 失败装载没有留下大堆或页面借用。
                        assert 'kernel heap: bytes=65536' in text[begin:] # 服务保持最小缓存，未偷跑容量演示。
                        begin=len(text); wire.sendall(b'run hello\n'); text=wait('[CONSOLE_UPDATE] view=0 commands=3 page=0',begin) # 用户资源确实已初始化，不依赖user_demo。
                        assert 'program hello exited 0' in text[begin:] and '[ELF_USER_OK]' in text[begin:] # 故障之后仍可实际装载和回收。
                        result={'passed':True,'profile':profile,'automatic':True,'recovery':'bad ELF -8','monitor_echo_mem_and_user_hello':True,'boot_disk_unchanged':True} # 只记录此故障配置的实际能力。
                        result_path.write_text(json.dumps(result,indent=2)+'\n'); print('PASS service recovery: bad init ELF, UART echo/mem, independent user hello') # QEMU存活本身不足以通过。
                        return result # finally仍只结束自己的测试VM。
                    text=wait('[INIT_START] service=sh input=UART restart_delay=100') # 资源标记和此行可能在同一次读取到达。
                else: # 旧诊断目标行为保持，用户选择何时进入持续会话。
                    text=wait('[AIRI_P5_READY] ') # 完整演示已经结束。
                    wire.sendall(b'run init\n'); text=wait('[INIT_START] service=sh input=UART restart_delay=100',len(text)) # 显式启动同一个监督程序。
                supervisor=re.search(r'\[INIT_SESSION\] pid=(\d+) uart=exclusive',text) # 内核记录真实根身份，不假设PID1。
                assert supervisor # 该模式必须明确建立监督身份。
                pid=supervisor.group(1); shells=[]; offset=text.index('[INIT_START]') # 只看本模式之后的shell。
                for generation in range(12): # 超过八槽表容量，不能靠结束launch统一清理。
                    text=wait('[EXEC_COMMIT] ',offset) # 每代shell都必须成功替换成独立ELF。
                    matches=list(re.finditer(r'\[EXEC_COMMIT\] pid=(\d+) program=sh ',text[offset:])) # 只取当前代的真实sh提交。
                    assert matches # 其他程序的exec不能满足shell启动条件。
                    child=matches[-1].group(1); shells.append(child) # 保存每代新身份。
                    assert '[FORK] parent='+pid+' child='+child+' ' in text # 全部shell的父都是同一个init。
                    prompt_at=offset+matches[-1].start(); text=wait('[USER_SHELL_PROMPT]',prompt_at) # 前一代提示符不能提前满足。
                    begin=len(text); wire.sendall(b'hello\n'); text=wait('[SHELL_COMMAND_DONE] name=hello code=0',begin) # 每代都验证输入、子exec与返回。
                    assert '[ELF_USER_OK]' in text[begin:] # UART资格正确留给shell，程序没有偷走输入。
                    if generation==11: break # 第十二代保持服务，不让init自然结束。
                    offset=len(text); wire.sendall(b'exit\n'); text=wait('[INIT_REAP] result=normal',offset) # 用户logout只结束shell，不结束监督者。
                    text=wait('[INIT_RESTART] delay_elapsed=1',offset) # 用户已验证sleep至少经过100个真实tick。
                assert len(set(shells))==12 and process.poll() is None # init仍活着且每代使用新PID。
                assert text.count('[INIT_SESSION]')==1 and 'program init exited' not in text # 不能偷偷结束会话再重启。
                assert not re.search(r'\[USER_EXIT\] pid='+pid+r' ',text) # 监督者本身也不能已退出、只留下孤儿shell继续运行。
                assert len(re.findall(r'\[WAIT_REGISTER\] pid='+pid+r' operation=sleep ticks=100',text))==11 # 每次logout都真正登记退避等待。
                result={'passed':True,'generations':12,'same_supervisor_pid':int(pid),'shell_pids':[int(n) for n in shells],'restart_sleep_ticks':100,'launch_remained_active':True,'harness_stopped_vm':True,'profile':profile,'automatic':automatic,'boot_disk_unchanged':automatic} # 观察结束由宿主停测试VM，不冒充guest关机。
                result_path.write_text(json.dumps(result,indent=2)+'\n'); print(f'PASS init/{profile}: 12 UART shell generations, one supervisor, blocked restart delay, continued service') # 所有路径实际观察后发布。
                return result # 默认test可继续其他会话验证。
            finally: # 包括断言、超时和QEMU错误。
                if wire is not None: wire.close() # 只关闭自己的设备连接。
                process.terminate() # 正常终止测试CPU，不称掉电。
                try: process.wait(timeout=5) # 清理也有期限。
                except subprocess.TimeoutExpired: process.kill(); process.wait() # 不遗留测试进程。
