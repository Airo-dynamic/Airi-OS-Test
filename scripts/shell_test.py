"""Drive the actual ring-3 shell through UART and observe a complete I/O path.""" # 不把宿主脚本当用户shell。
import json # 记录本轮真实结果。
from pathlib import Path # 短控制端点。
import re # 按实际PID关联等待和进展。
import socket # QEMU的UART双向字节流。
import subprocess # 只管理本测试子进程。
import tempfile # 避免UNIX路径长度上限。
import time # 单调总截止。
from boot import command, qmp_execute # 固定硬件与真实限速。
from tools import ROOT # 不依赖调用目录。
def check_shell(): # 默认串口主线，不读取frontend或字体。
    work=ROOT/'artifacts/shell-normal'; work.mkdir(parents=True,exist_ok=True) # 本测试独占目录。
    result_path=work/'result.json'; result_path.write_text('{"passed":false}\n') # 旧成功立即撤销。
    disk=work/'disk.raw' # 仅测试盘可重置，不动交互run的正式实验盘。
    if disk.is_symlink() or (disk.exists() and (not disk.is_file() or disk.stat().st_size!=8*1024*1024)): # 未知对象不替换。
        raise RuntimeError('Invalid shell-test disk') # 保留异常路径供检查。
    disk.unlink(missing_ok=True) # 下一command排他创建空盘。
    with tempfile.TemporaryDirectory(prefix='airios-sh-',dir='/tmp') as directory: # 本次端点随测试销毁。
        qmp=Path(directory)/'qmp.sock'; uart=Path(directory)/'uart.sock' # 控制与设备分开。
        cmd=command('normal',work); cmd[cmd.index('-serial')+1]=f'unix:{uart},server=on,wait=off' # 实际UART而非内存注入。
        cmd+=['-S','-qmp',f'unix:{qmp},server=on,wait=off'] # 连接UART后才继续CPU。
        (work/'command.json').write_text(json.dumps(cmd,indent=2)+'\n') # 固定本次QEMU参数。
        with (work/'qemu.log').open('w') as errors: # QEMU诊断单独保存。
            process=subprocess.Popen(cmd,stdout=errors,stderr=errors); wire=None; data=bytearray() # 保留真实累积字节。
            try: # 任何失败都回收自己的QEMU。
                deadline=time.monotonic()+10 # 等待端点不能无限。
                while not uart.exists(): # CPU还未开始启动。
                    if process.poll() is not None or time.monotonic()>deadline: raise RuntimeError('Shell UART unavailable') # 明确失败。
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
                text=wait('[AIRI_P5_READY] ') # 完整内核启动后再进入用户shell。
                wire.sendall(b'run sh\n'); text=wait('[USER_SHELL_PROMPT]',len(text)) # 真正进入用户态shell。
                limits={'device':'airi-disk','bps':0,'bps_rd':0,'bps_wr':0,'iops':1,'iops_rd':0,'iops_wr':0} # 让物理完成晚于父等待。
                qmp_execute(qmp,'block_set_io_throttle',limits); (work/'throttle.json').write_text(json.dumps(limits)+'\n') # QMP接受后才记录。
                offset=len(text); wire.sendall(b'demo\n'); text=wait('[SHELL_COMMAND_DONE] name=demo code=0',offset) # 命令由shell解析、fork/exec/wait。
                tail=text[offset:]; match=re.search(r'\[USER_ALLOC\] pid=(\d+) ',tail) # 从实际新程序分配记录取得PID。
                assert match and '[PIPELINE_USER_OK]' in tail and '[EXEC_COMMIT]' in tail # 不用宿主复制或预期字符串替代运行。
                pid=match.group(1); progress=tail.index('[PIPELINE_OTHER_PROGRESS]'); blocked=False # 验证另一个用户在同一次CQ等待期间推进。
                for registered in re.finditer(r'\[WAIT_REGISTER\] pid='+pid+r' operation=cq\r?\n',tail): # 只匹配演示父进程。
                    wake=tail.find('[WAIT_WAKE] pid='+pid+' result=0\r\n',registered.end()) # 同PID真实结果交付。
                    blocked|=registered.start()<progress<wake # 不固定猜用户PID或时序。
                assert blocked and 'phase=start' in tail and 'phase=complete' in tail # 实际设备请求和完成均可见。
                qmp_execute(qmp,'block_set_io_throttle',dict(limits,iops=0)) # 后续同步日志不继承慢设备测试预算。
                for request,marker in [('storage','[STORAGE_USER_OK]'),('recover','[RECOVERY_USER_OK]'),('files','[FILES_USER_OK]'),('missing','exec failed: -2'),('broken','exec failed: -8'),('hello','[ELF_USER_OK]')]: # 正常、坏名字、坏ELF后仍可运行。
                    offset=len(text); wire.sendall((request+'\n').encode()); text=wait('[SHELL_COMMAND_DONE] name='+request+' ',offset) # 完整孩子退出才开始下一条。
                    assert marker in text[offset:] # 必须来自本条命令。
                offset=len(text); wire.sendall(b'memory\n'); text=wait('[SHELL_COMMAND_DONE] name=memory code=0',offset) # 同一地址空间内归还并复用，不靠exit恢复内存。
                assert '[USER_MEMORY_REUSE_OK] rounds=128 holes=first-fit zeroed=1 invalid=unchanged' in text[offset:] # 新旧对象内容和失败原子性都由用户实际核对。
                offset=len(text); wire.sendall(b'capacity\n'); text=wait('[SHELL_COMMAND_DONE] name=capacity code=0',offset) # 大BSS、跨页用户栈、耗尽恢复与exec都来自guest。
                capacity_output=text[offset:] # 只使用本次请求后的完整输出。
                assert '[USER_CAPACITY_OK] bss=393216 stack=32768 rounds=4 full_pages=1024 exec=worker guard=pf14' in capacity_output # 用户实际逐字节核对后才发布。
                assert len(re.findall(r'\[FORK\].*copied_pages=1024',capacity_output))==4 # 四轮都触及最后一个元数据页。
                assert len(re.findall(r'\[EXEC_COMMIT\].*program=worker old_pages=1024',capacity_output))==4 # 大旧空间在成功exec提交后完整清理。
                assert 'cr2=0x6fff0000' in capacity_output # 用户栈下方保护页真正触发硬件异常。
                offset=len(text); wire.sendall(b'simd\n'); text=wait('[SHELL_COMMAND_DONE] name=simd code=0',offset) # 真实C浮点与跨抢占的活寄存器不能靠宿主模拟。
                simd_output=text[offset:] # 只核对本次命令的实际用户身份。
                assert '[SIMD_USER_OK] fork=inherited timer_window=20 x87=private xmm15=private exec=reset double=6.5' in simd_output # 父子与exec路径均由guest核对。
                assert '[SIMD_FAULT_ISOLATED] fxsave=unaligned vector=13 parent=alive' in simd_output and 'vector=13 error=0x0' in simd_output # 未对齐状态保存只结束自己的真实子身份。
                assert '[SIMD_CHILD_OK]' in simd_output and '[SIMD_EXEC_RESET_OK]' in simd_output # 子必须进入新的真实ELF检查默认状态。
                parent=re.search(r'\[EXEC_COMMIT\] pid=(\d+) program=simd ',simd_output) # shell创建的目标用户PID。
                assert parent # 不能用其他启动程序的输出代替。
                fork=re.search(r'\[FORK\] parent='+parent.group(1)+r' child=(\d+) ',simd_output) # 探针fork产生另一真实身份。
                assert fork # 本次C程序并非仅在一个进程里比较两个数组。
                for identity in (parent.group(1),fork.group(1)): # 两个用户都必须实际经过PIT中断窗口。
                    ended=re.search(r'\[USER_EXIT\] pid='+identity+r' code=0 user_ticks=(\d+)',simd_output) # exec保留同一PID的用户tick统计。
                    assert ended and int(ended.group(1))>=2 # 活寄存器检查跨真实时间片，而非只有主动yield。
                offset=len(text); wire.sendall(b'tls\n'); text=wait('[SHELL_COMMAND_DONE] name=tls code=0',offset) # FS的读写必须跨真实父子抢占保持。
                tls_output=text[offset:] # 不复用启动或前一个用户程序的成功标记。
                assert '[TLS_USER_OK] fork=inherited timer_window=20 fs=private invalid=unchanged exec=reset released=pf14' in tls_output # 完整正常与释放后故障由guest检查。
                assert '[TLS_CHILD_OK]' in tls_output and '[TLS_EXEC_RESET_OK] fs_base=0 old_block=detached' in tls_output # 同一孩子成功换成新ELF。
                tls_parent=re.search(r'\[EXEC_COMMIT\] pid=(\d+) program=tls ',tls_output) # 本次命令的真实根PID。
                assert tls_parent # 不能用别的命令的fork满足条件。
                tls_fork=re.search(r'\[FORK\] parent='+tls_parent.group(1)+r' child=(\d+) ',tls_output) # 首个孩子参与不同基址的纯计算。
                assert tls_fork # 本次必须出现真实的两个可运行身份。
                for identity in (tls_parent.group(1),tls_fork.group(1)): # 二者都实际经过PIT窗口。
                    ended=re.search(r'\[USER_EXIT\] pid='+identity+r' code=0 user_ticks=(\d+)',tls_output) # exec仍使用同一身份累计值。
                    assert ended and int(ended.group(1))>=2 # 主动yield或单身份内存比较不算抢占证据。
                offset=len(text); wire.sendall(b'ctls\n'); text=wait('[SHELL_COMMAND_DONE] name=ctls code=0',offset) # 编译器_Thread_local必须在main前建立，不由宿主注入值。
                ctls_output=text[offset:] # 固定本次命令的实际记录。
                assert '[CTLS_USER_OK] c11=thread_local tdata=42 tbss=73 align=64 fork=copy exec=reset invalid=unchanged' in ctls_output # 父子修改后fork、真正抢占与坏PT_TLS保持旧映像。
                assert '[CTLS_CHILD_OK]' in ctls_output and '[CTLS_EXEC_RESET_OK]' in ctls_output and '[CTLS_CHAR_OK] bytes=1 alignment=1 tp=16 value=9' in ctls_output # 大小对齐与成功exec均实际运行。
                assert '[CTLS_PAGE_OK] bytes=4096 initialized=0 pages=2 tcb=second zeroed=all' in ctls_output # 最大纯零模板实际访问第二页TCB。
                assert 'ELF TLS size or alignment is unsupported' in ctls_output # 超限真实样本必须在装载器拒绝。
                offset=len(text); wire.sendall(b'ctls-tail\n'); text=wait('[SHELL_COMMAND_DONE] name=ctls-tail code=0',offset) # 真实LLD的TLS零尾部可以超过普通LOAD内存长度。
                assert '[CTLS_TAIL_OK] initialized=4 zero_tail=257 ordinary_bss=1 independent=1' in text[offset:] # 普通BSS与本地运行块必须分别可写且互不覆盖。
                offset=len(text); wire.sendall(b'libc\n'); text=wait('[SHELL_COMMAND_DONE] name=libc code=0',offset) # 用户库转换真实内核结果，errno来自编译器TLS。
                libc_output=text[offset:] # 不使用旧映像或启动期输出。
                assert '[LIBC_CLOCK_OK] monotonic=1 quantum_ns=10000000 sleep=blocked invalid=unchanged' in libc_output # 真实PIT窗口、输出权限和墙钟拒绝均由guest核对。
                assert '[LIBC_SEEK_OK] set=1 cur=1 end=1 signed=checked hole=zero failure=unchanged' in libc_output # 相对定位、极值失败和真实零洞均由guest观察。
                assert '[LIBC_IO_OK] short=256 pipe=64 eof=zero offset=preserved bad_buffer=unchanged' in libc_output # 正常、短操作和坏缓冲区均由guest验证。
                assert '[LIBC_ERRNO_OK] initial=0 fork=copied preempt=private success=preserved exec=reset' in libc_output and '[LIBC_EXEC_RESET_OK] errno=zero cloexec=closed' in libc_output # 同一身份成功exec重置错误并关闭表项。
                offset=len(text); wire.sendall(b'vector\n'); text=wait('[SHELL_COMMAND_DONE] name=vector code=0',offset) # 散布缓冲必须由真实内核边界校验后完成一次文件操作。
                assert '[VECTOR_USER_OK] gather=ordered scatter=short prefix=256 positioned=1 invalid=unchanged descriptors=checked' in text[offset:] # 坏后项、跨页描述和实际前缀均由guest检查。
                offset=len(text); wire.sendall(b'heap\n'); text=wait('[SHELL_COMMAND_DONE] name=heap code=0',offset) # 对象库运行在真实用户地址空间，先耗尽原始页再验证重试。
                heap_output=text[offset:] # 只接受本条命令与它实际exec的孩子记录。
                assert '[MALLOC_USER_OK] arena=262144 align=16 calloc=zero overflow=checked realloc=failure_preserved reuse=128 fork=private exec=reset' in heap_output # 对齐、复用、失败保留与父子隔离均由guest观察。
                assert '[MALLOC_EXEC_RESET_OK] arena=lazy objects=zero errno=zero' in heap_output # 成功exec必须重新建立BSS和TLS，不能继承旧arena。
                offset=len(text); wire.sendall(b'futex\n'); text=wait('[SHELL_COMMAND_DONE] name=futex code=0',offset) # 真正共享地址空间中的比较、登记与显式通知。
                futex_output=text[offset:] # 不接受其他命令的旧等待标记。
                assert '[FUTEX_PREWAKE_OK] old=1 published=2 wake=0 wait=-11' in futex_output # 通知先发生，旧判断不能使后续调用睡下。
                assert '[FUTEX_COUNT_OK] waiters=3 wake_one=1 wake_rest=2 unchanged=1' in futex_output # 数量与字不被修改分别由guest检查。
                assert '[FUTEX_USER_OK] compare=atomic prewake=checked timeout=pit counts=1,2 private=process,va exit=clean' in futex_output # 私有键、退出清理与真实PIT超时都已完成。
                assert len(re.findall(r'\[FUTEX_WAIT\] pid=',futex_output))==8 # 一次超时、三次同键、父子各一、退出孩子与复用者各一。
                offset=len(text); wire.sendall(b'mutex\n'); text=wait('[SHELL_COMMAND_DONE] name=mutex code=0',offset) # 真正共享普通数据在带锁yield时也不能半更新。
                mutex_output=text[offset:] # 当前程序而非旧日志提供结果。
                assert '[MUTEX_USER_OK] counter=64 critical_yield=1 foreign=denied self=deadlock timeout=pit owner_death=nonrobust' in mutex_output # 所有权、真实等待和非robust寿命都由guest核对。
                assert '[MUTEX_PARTICIPANTS_OK] blocked=2 timeout=withdrawn acquired=withdrawn dead_owner=retained' in mutex_output # 真实竞争和到期均撤销参与者，死亡owner仍保留。
                assert len(re.findall(r'\[FUTEX_WAIT\] pid=',mutex_output))>=4 and 'result=-110' in mutex_output # 两位争用者、超时者和死亡所有者观察确实进入等待。
                offset=len(text); wire.sendall(b'mutex-races\n'); text=wait('[SHELL_COMMAND_DONE] name=mutex-races code=0',offset) # 同一用户锁在三个受控窗口及一个真实睡眠位置继续。
                races_output=text[offset:] # 只使用当前命令的实际回调和内核日志。
                assert '[MUTEX_RACES_OK] stages=4 actual_wait=1 compare_mismatch=1 normal_library=unchanged' in races_output # guest已核对真实返回及参与者撤销。
                for stage in range(1,5): # 每轮有独立串口边界，不能互相借日志。
                    begin='[MUTEX_RACE_BEGIN] stage='+str(stage) # str把本轮整数放进固定标记。
                    end='[MUTEX_RACE_DONE] stage='+str(stage) # 只切当前轮结束以前的记录。
                    assert races_output.count(begin)==1 and races_output.count(end)==1 # 不接受缺轮或重复成功。
                    section=races_output.split(begin,1)[1].split(end,1)[0] # 两次最多切一刀，保留这轮真实事件。
                    waits=re.findall(r'\[FUTEX_WAIT\] pid=',section) # -11不登记，因此第三轮也没有WAIT记录。
                    wakes=re.findall(r'\[FUTEX_WAKE\] pid=\d+ address=0x[0-9a-f]+ count=1 woken=(\d+)',section) # 实际通知数量来自内核，不从用户标记猜测。
                    assert len(waits)==(1 if stage==4 else 0) # 只有第四轮真正进入睡眠集合。
                    assert wakes==([] if stage==1 else ['1' if stage==4 else '0']) # 第一轮省略，第二/三轮空通知，第四轮真实唤醒一人。
                offset=len(text); wire.sendall(b'events\n'); text=wait('[SHELL_COMMAND_DONE] name=events code=0',offset) # 变化序号与通知分开，旧快照、无变化通知和真实广播均运行。
                events_output=text[offset:] # 不能借其他命令的等待记录。
                assert '[EVENT_USER_OK] old_snapshot=changed timeout=pit false_wake=rechecked broadcast=2 late_wait=timeout saturation=refused' in events_output # guest已核对饱和与每位worker返回2。
                event_waits=re.findall(r'\[FUTEX_WAIT\] pid=(\d+) tid=(\d+) address=(0x[0-9a-f]+) expected=(\d+)',events_output) # 实际登记来自内核，零预算不登记。
                assert len(event_waits)==6 and len({(pid,address) for pid,tid,address,expected in event_waits})==1 # 两次主超时加两位各两次等待，同一私有键。
                assert [expected for pid,tid,address,expected in event_waits].count('1')==5 and event_waits[-1][3]=='2' # 最后新快照不会领取旧通知。
                event_wakes=re.findall(r'\[FUTEX_WAKE\] pid=\d+ address=0x[0-9a-f]+ count=16 woken=(\d+)',events_output) # 饱和对象末次发布、先发布、无变化通知、最后广播。
                assert event_wakes==['0','0','2','2'] # 拒绝回绕不发通知；两次真正广播分别唤醒两人。
                offset=len(text); wire.sendall(b'queue\n'); text=wait('[SHELL_COMMAND_DONE] name=queue code=0',offset) # 真正四槽生产消费、背压与关闭排空。
                queue_output=text[offset:] # 不能用宿主统计或旧命令满足。
                assert '[QUEUE_USER_OK] capacity=4 jobs=48 exactly_once=1 sum=1176 squares=38024 backpressure=wait close=drain timeout=preserved' in queue_output # guest位图、结果与错误返回已逐一核对。
                backpressure=queue_output.split('[QUEUE_BACKPRESSURE_BEGIN]',1)[1].split('[QUEUE_BACKPRESSURE_OK]',1)[0] # 满队列超时和第五项真实等待发生在这里。
                assert len(re.findall(r'\[FUTEX_WAIT\] pid=',backpressure))>=2 and 'result=-110' in backpressure and re.search(r'\[FUTEX_WAKE\].*woken=1',backpressure) # 不用主动yield冒充背压。
                closing=queue_output.split('[QUEUE_CLOSE_WAITERS_BEGIN]',1)[1].split('[QUEUE_CLOSE_WAITERS_OK]',1)[0] # 关闭必须唤醒两位空队列消费者。
                assert len(re.findall(r'\[FUTEX_WAIT\] pid=',closing))>=2 and re.search(r'\[FUTEX_WAKE\].*count=16 woken=2',closing) # 结束状态由真实等待返回后重查。
                producer_close=queue_output.split('[QUEUE_CLOSE_PRODUCER_BEGIN]',1)[1].split('[QUEUE_CLOSE_PRODUCER_OK]',1)[0] # 满队列关闭必须唤醒尚未接受的新项。
                assert re.search(r'\[FUTEX_WAIT\] pid=',producer_close) and re.search(r'\[FUTEX_WAKE\].*count=16 woken=1',producer_close) # 与空队列关闭的消费者路径分别实测。
                queue_owner=re.search(r'\[EXEC_COMMIT\] pid=(\d+) program=queue ',queue_output) # 只数本次队列进程的执行者。
                assert queue_owner # 工作链共享同一PID。
                assert len(re.findall(r'\[THREAD_CREATE\] pid='+queue_owner.group(1)+r' tid=',queue_output))==8 and len(re.findall(r'\[THREAD_RETURN\] pid='+queue_owner.group(1)+r' tid=',queue_output))==8 # 一背压、二关闭消费者、一关闭生产者及四负载均收拢。
                offset=len(text); wire.sendall(b'heap-threads\n'); text=wait('[SHELL_COMMAND_DONE] name=heap-threads code=0',offset) # 四个真实执行者共享唯一arena，观察对象交接。
                heap_threads=text[offset:] # 不用旧heap命令代替并发结果。
                assert '[MALLOC_THREADS_OK] workers=4 rounds=96 arena=262144 cross_free=4 snapshots=consistent errno=private critical_yield=1' in heap_threads # 每个工作者和主快照均由guest核对。
                assert '[FUTEX_WAIT]' in heap_threads # 观察编译明确在内部持锁yield，必须实际进入竞争等待。
                heap_owner=re.search(r'\[EXEC_COMMIT\] pid=(\d+) program=heap-threads ',heap_threads) # 固定本条命令的真实进程身份。
                assert heap_owner and len(re.findall(r'\[THREAD_CREATE\] pid='+heap_owner.group(1)+r' tid=',heap_threads))==4 and len(re.findall(r'\[THREAD_RETURN\] pid='+heap_owner.group(1)+r' tid=',heap_threads))==4 # join可以立即成功，不能强求特定阻塞时序。
                offset=len(text); wire.sendall(b'identity\n'); text=wait('[SHELL_COMMAND_DONE] name=identity code=0',offset) # 运行头必须在raw fork、exec和工作者复用时更新。
                identity_output=text[offset:] # 只核对这一条命令的真实查询。
                assert '[IDENTITY_USER_OK] tcb=version1 fast=local fork=raw exec=fresh workers=3 reused=fresh fallback=queried' in identity_output and '[IDENTITY_EXEC_OK] tls=42 tcb=current version=1' in identity_output # 数据字段与原始查询均由guest比较。
                identity_owner=re.search(r'\[EXEC_COMMIT\] pid=(\d+) program=identity ',identity_output) # 主执行者身份是本次命令真实PID。
                assert identity_owner # 不能拿其他程序的查询日志校准。
                queries=re.findall(r'\[THREAD_ID_QUERY\] pid=(\d+) tid=(\d+)',identity_output) # 原始42号入口记录实际陷入，不依赖用户计数。
                assert len(queries)==7 and queries.count((identity_owner.group(1),identity_owner.group(1)))==2 # 主参考加版本后备两次、孩子前后两次、三位worker各一次。
                identity_workers=re.findall(r'\[THREAD_CREATE\] pid='+identity_owner.group(1)+r' tid=(\d+) ',identity_output) # 三次创建中最后一次复用已join槽。
                assert len(identity_workers)==3 and len(set(identity_workers))==3 # 身份不复用，槽可以复用。
                for tid in identity_workers: # 每位跨PIT的计算执行者都只做一次参考42号查询。
                    assert queries.count((identity_owner.group(1),tid))==1 # 大量快速读取和互斥调用不增加查询陷入。
                    returned=re.search(r'\[THREAD_RETURN\] pid='+identity_owner.group(1)+r' tid='+tid+r' user_ticks=(\d+)',identity_output) # 独立用户tick来自真实IRQ。
                    assert returned and int(returned.group(1))>=2 # 不把主动yield当抢占隔离证据。
                counts={} # 以真实(pid,tid)为键，各自保存计数快照；其他工作者输出可以插入其间。
                for pid,tid,count in re.findall(r'\[THREAD_SYSCALLS\] pid=(\d+) tid=(\d+) count=(\d+)',identity_output): # 只消费本条命令真实45号记录。
                    counts.setdefault((pid,tid),[]).append(int(count)) # 第一次建立空列表，随后按该执行者顺序追加整数。
                root_id=identity_owner.group(1) # 与上面的42号参考使用同一次真实进程身份。
                child_match=re.search(r'\[FORK\] parent='+root_id+r' child=(\d+)',identity_output) # exec沿同一孩子继续，不能固定本轮数字。
                assert child_match # 必须确实走过原始fork。
                child_id=child_match.group(1) # 孩子的主TID等于它自己的PID。
                assert len(counts)==5 and sum(map(len,counts.values()))==18 # 父三次、孩子跨exec六次、三位worker各三次。
                root_counts=counts[(root_id,root_id)] # 父执行者已有shell exec请求，不硬编码第一项。
                child_counts=counts[(child_id,child_id)] # exec只换映像，仍积累同一执行者的请求。
                assert len(root_counts)==3 and root_counts[2]-root_counts[1]==1 # 八对锁不发请求，只有结束查询自身一次。
                assert len(child_counts)==6 and child_counts[0]==1 and child_counts[2]-child_counts[1]==1 and child_counts[5]-child_counts[4]==1 and child_counts[3]>child_counts[2] # fork清零、exec保留，两个相同局部锁窗口分别计数。
                for tid in identity_workers: # 工作者可以交替运行，仍分别验证各自三个值。
                    samples=counts[(root_id,tid)] # 共享PID不能合并不同TID的测量。
                    assert len(samples)==3 and samples[0]==1 and samples[2]-samples[1]==1 # 新执行者首查询为1，八对无竞争操作省去全部通知。
                assert identity_output.count('[MUTEX_PROFILE] loops=8 delta=1')==6 # guest与宿主按同一真实测量各自核对。
                assert '[FUTEX_WAIT]' not in identity_output and '[FUTEX_WAKE]' not in identity_output # 局部锁没有竞争者，不能用返回0的空通知冒充用户态路径。
                for repetition in range(2): # 同一shell连续完成两次线程命令，上一组退出回收后应能重新创建。
                    offset=len(text); wire.sendall(b'threads\n'); text=wait('[SHELL_COMMAND_DONE] name=threads code=0',offset) # 真正同地址空间执行，不能由fork复制或宿主线程模拟。
                    thread_output=text[offset:] # 所有身份与标记均限定在这条命令之后。
                    assert '[THREAD_USER_OK] shared=memory,fd,async tls=fresh cpu=private join=64bit capacity=8 reuse=24 busy=checked group_exit=77 worker_fault=pf14' in thread_output # 用户主动检查正常、容量、生命周期与故障。
                    owner=re.search(r'\[EXEC_COMMIT\] pid=(\d+) program=threads ',thread_output) # 进程PID与各工作TID分别读取。
                    assert owner # 不能拿其他程序的执行身份满足条件。
                    identities=re.findall(r'\[THREAD_CREATE\] pid='+owner.group(1)+r' tid=(\d+) ',thread_output) # 首两个是跨PIT的计算工作者。
                    assert len(identities)==35 and len(set(identities))==35 # 两计算、八容量、二十四复用和一个管道工作者全部真实创建。
                    for identity in identities[:2]: # 两个CPU/TLS观察都必须实际获得时间片。
                        returned=re.search(r'\[THREAD_RETURN\] pid='+owner.group(1)+r' tid='+identity+r' user_ticks=(\d+)',thread_output) # 每个结束记录保留真实IRQ数。
                        assert returned and int(returned.group(1))>=2 # 不能只有主执行者主动yield而没有工作抢占窗口。
                    assert '[THREAD_JOIN_WAIT]' in thread_output and re.search(r'\[WAIT_REGISTER\] pid='+owner.group(1)+r' operation=read ',thread_output) # join与工作管道都真实阻塞。
                offset=len(text); wire.sendall(b'orphans\n'); text=wait('[SHELL_COMMAND_DONE] name=orphans code=0',offset) # 同一用户shell持续服务，不靠返回monitor收拢孙进程。
                orphan_output=text[offset:] # 只核对本条命令的身份记录。
                assert '[ORPHAN_USER_OK] rounds=24 live=12 zombie=12 session=alive' in orphan_output # 超过八槽容量仍能连续fork。
                assert len(re.findall(r'\[ORPHAN_REAP\] pid=\d+ released=1',orphan_output))==24 # 不是launch收尾清空表冒充托管。
                assert len(re.findall(r'\[ORPHAN_ADOPT\].*finished=1',orphan_output))==12 # 先成为僵尸再失去父的路径实际运行。
                assert len(re.findall(r'\[ORPHAN_EXIT\]',orphan_output))==12 # 活孤儿放行后正常退出。
                offset=len(text); wire.sendall(b'programs\n'); text=wait('[USER_SHELL_PROMPT]',offset) # 用户查询来自真实注册目录。
                assert 'demo\r\n' in text[offset:] and 'async-reset\r\n' in text[offset:] # 不是内核监控台的静态帮助。
                offset=len(text); wire.sendall(b'x'*80+b'\n'); text=wait('[USER_SHELL_PROMPT]',offset) # 过长行必须整体丢弃。
                assert 'line too long; discarded' in text[offset:] and '[FORK]' not in text[offset:] # 不执行截断前缀。
                offset=len(text); wire.sendall(b'hellx\x7fo\r\n'); text=wait('[SHELL_COMMAND_DONE] name=hello code=0',offset) # 退格修正与CRLF真正走用户输入。
                offset=len(text); wire.sendall(b'exit\n'); text=wait('[CONSOLE_UPDATE] view=0 commands=1 page=0',offset) # shell退出后内核监控台才返回。
                assert '[USER_SHELL_EXIT]' in text[offset:] and 'program sh exited 0' in text[offset:] # 真实收拢页/FD/静态池由launch核对。
                offset=len(text); wire.sendall(b'echo after-shell\n'); text=wait('[CONSOLE_UPDATE] view=0 commands=2 page=0',offset) # 回到内核后输入仍完整。
                assert 'after-shell' in text[offset:] # 不只是退出而丢了后续UART。
                result={'passed':True,'ring3_shell':True,'ram_file_and_device_readback':True,'other_user_during_cq_wait':True,'qmp_iops':1,'error_exec_survived':True,'returned_to_monitor':True,'orphan_rounds':24,'memory_reuse_rounds':128,'capacity_rounds':4,'capacity_pages':1024,'simd_preempt_fork_exec':True,'fs_base_preempt_fork_exec':True,'compiler_static_tls':True,'posix_errno_io_subset':True,'positioned_vectors':True} # 只发布已观察事实。
                result.update(bounded_queue=True,queue_capacity=4,queue_jobs=48,queue_close_drains=True,sequence_event=True,event_waits=6,event_broadcast_workers=2,event_false_wake_rechecked=True,mutex_race_stages=4,mutex_race_actual_waits=1,mutex_race_mismatches=1,per_execution_syscall_count=True,uncontended_mutex_pairs=8,uncontended_mutex_syscalls=0,runtime_tcb_identity=True,identity_reference_queries=7,shared_malloc=True,malloc_workers=4,malloc_rounds=96,owned_mutex=True,mutex_critical_updates=64,private_futex=True,futex_registrations=8,user_heap=True,shared_executions=True,thread_commands=2,worker_capacity=8,worker_reuse_rounds=24) # 关键字把实际已验收字段加入结果字典，不替代上面的逐命令检查。
                result_path.write_text(json.dumps(result,indent=2)+'\n'); print('PASS user shell: alloc/file/async wait/other task/CQ/readback, failed exec, return to monitor') # 阶段演示实际完成。
                return result # 总入口可收集。
            finally: # 包括断言、超时和QEMU错误。
                if wire is not None: wire.close() # 只关闭自己的设备连接。
                process.terminate() # 正常终止测试CPU，不称掉电。
                try: process.wait(timeout=5) # 清理也有期限。
                except subprocess.TimeoutExpired: process.kill(); process.wait() # 不遗留测试进程。
