"""Observe UART queries, echo and unknown commands before the full test suite."""  # 本脚本观察真实UART命令，尚未使用QMP控制协议
from pathlib import Path  # 导入Path，后续用路径对象组合观察输出位置
import shutil      # 提供主机文件复制，给本次QEMU独立的可写固件变量
import subprocess  # 导入子进程接口，用于执行本次构建
import time  # 提供单调时钟与短暂等待，约束本轮观察期限
from tools import ROOT, tool  # 读取学习仓库根目录与已建立的工具查找函数

work = ROOT / 'build/first-input'  # 为这次UART观察选择独立产物目录
work.mkdir(parents=True, exist_ok=True)   # 自动建缺少的父目录，已有目录不会报错
firmware = ROOT / 'third_party/firmware'  # 固定UEFI固件输入所在目录
shutil.copyfile(firmware / 'edk2-i386-vars.fd', work / 'vars.fd')  # 复制可写变量区，使每次QEMU运行从同一固件状态开始
serial = work / 'serial.log'  # guest发送文字将由QEMU标准输出写入这个新日志
command = [tool('qemu-system-x86_64', 'qemu'), '-machine', 'q35', '-accel', 'tcg',  # 参数列表首项是实际QEMU路径，固定q35和TCG模拟执行
           '-cpu', 'qemu64', '-smp', '1', '-m', '256M', '-vga', 'std', '-display', 'none', '-no-reboot',  # 固定CPU模型、单核、256MiB、无图形窗口且禁止重启
           '-monitor', 'none', '-serial', 'stdio',  # 关闭QEMU文字监控，标准输入输出只连接guest UART
           '-drive', f'if=pflash,format=raw,unit=0,readonly=on,file={firmware / "edk2-x86_64-code.fd"}',  # 只读固件代码区用固定路径，f字符串将Path转进参数文字
           '-drive', f'if=pflash,format=raw,unit=1,file={work / "vars.fd"}',  # 可写变量区用本轮副本，避免污染锁定固件
           '-cdrom', str(ROOT / 'build/normal/airios.iso'), '-boot', 'd']  # 把Path转为进程参数字符串，从本轮光盘镜像启动
with serial.open('wb') as output, (work / 'qemu.log').open('wb') as errors:  # wb按字节写并截断旧日志，with退出时关闭两份文件
    process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=output, stderr=errors)  # 启动QEMU，父进程保留输入管道，UART输出与QEMU错误分文件保存
    try:  # 观察主体失败后也必须进入下面的QEMU清理
        requests = [(b'mem\n', 'No allocator accounting yet.'),  # 先查询真实启动库存。
                    (b'echo serial-mainline\n', 'serial-mainline'),  # 参数经 UART 进入同一命令模型。
                    (b'theme day\n', 'unknown command'),  # 尚未选择图形消费者时不提供主题功能。
                    (b'devices\n', 'ttyS0'),  # 当前固定设备端点可从串口查询。
                    (b'help\n', 'mem map'),   # 帮助与已实现命令对应。
                    (b'sys\n', 'bootstrap kernel')]  # 最后查询同一内核身份。
        index = 0      # 当前请求下标，六项下标为0到5
        ready = False  # 尚未观察到本轮内核启动完成
        waiting = False  # 当前没有已发送却未完成的请求
        offset = 0  # 本次请求发出前已有文本的字符长度
        deadline = time.monotonic() + 90    # 本次启动和六次交互共同最多90秒
        while time.monotonic() < deadline:  # 在截止前继续检查真实输出，运行中的进程并不代表成功
            text = serial.read_text(errors='replace')  # 输出文件字节解码为str，异常编码用替代字符保留其他结果
            lines = [line for line in text.splitlines(keepends=True) if line.endswith('\n')]  # 只处理有完整换行的记录，尾部未写完的行暂不使用
            if not ready:  # 启动阶段只寻找本轮独立日志中的READY
                ready = any(line.startswith('[AIRI_LESSON_READY]') for line in lines)  # 生成器逐行提供判断，any发现一个真值便停止查找
            if ready and not waiting:  # 上次请求已完成才发送下一条，设备接收缓冲不是无限队列
                request, expected = requests[index]  # 二元组解包得到本条输入bytes和预期结果str
                offset = len(text)  # 记录发出前旧文本尾，旧结果不用于证明本次完成
                process.stdin.write(request)  # 本条命令末尾换行提交，下一条等待它的完成记录
                process.stdin.flush()  # 把父进程缓冲交给主机管道，再由QEMU递交给UART
                waiting = True  # 已有一条请求等待，后续轮询不能重复发送
            tail = text[offset:]  # 切片从本次发送前记录的字符偏移取得新结果
            records = [line for line in tail.splitlines(keepends=True) if line.endswith('\n')]  # 新尾部同样只使用完整串口记录
            marker = f'[CONSOLE_UPDATE] view=0 commands={index + 1} page=0\n'  # 第index条请求是第index+1次提交，f字符串代入累计计数
            if waiting and any(line.removeprefix('airi> ') == marker for line in records):  # 限定新结果并去确定提示符前缀，再比较完整完成行
                if expected not in tail:  # 完成计数以外还检查本条查询或错误的实际内容
                    raise RuntimeError(f'UART request {index + 1} missed: {expected}')  # 指出哪次请求缺少什么结果，不能继续报告成功
                print('UART', index + 1, request.decode().strip(), '->', expected)  # 输入bytes转str并去换行，仅用于主机观察显示
                index += 1  # 当前一条已核对，下一请求下标前进一
                waiting = False  # 此后才能向同一UART发送下一条命令
                if index == len(requests):  # 六条全部核对后完成本轮观察
                    print('PASS first UART input: mem / echo / unknown / devices / help / sys')  # 实际交互全部成功才显示可检查的成功文字
                    break  # 结束等待，随后统一回收QEMU
            if process.poll() is not None:  # None表示仍运行，其余是已退出的状态值
                raise RuntimeError('QEMU exited before the UART response')  # 未完成全部请求却退出，立即报告失败
            time.sleep(0.1)  # 轮询间隔100毫秒，不在主机占满CPU忙等
        else:  # 没有成功break而耗尽截止时间时进入超时分支
            raise RuntimeError('UART command timed out')  # 未出现应收记录不能当作成功
    finally:  # 不论观察成败都执行本轮资源清理
        try:  # 关闭写端可能遇到QEMU已退出导致的管道错误。
            process.stdin.close()  # 释放父进程写端，不再提交设备字节。
        finally:  # close即使抛错，也必须回收本轮子进程。
            process.terminate()  # 请求结束当前QEMU，不等待其自行永久退出。
            try:  # 正常结束最多等待五秒。
                process.wait(timeout=5)  # 收回退出状态，避免遗留子进程。
            except subprocess.TimeoutExpired:  # 请求终止后仍未退出，采用强制结束。
                process.kill()  # 强制结束本轮QEMU。
                process.wait()  # 再收回已结束进程状态。
