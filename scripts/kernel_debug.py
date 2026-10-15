"""Use the user's existing x86-64 GDB to inspect a local QEMU guest over SSH."""  # 调试器在远端，被观察的虚拟 CPU 留在本机
import argparse  # 命令行解析器把字符串选项转换为带名称的属性。
import io      # BytesIO 为内存字节提供文件式读写接口。
import json    # 在 Python 对象与 JSON 文本之间转换，不执行其中内容。
import shlex   # 按 Shell 引用规则拆分或引用文本，区别于实际执行。
import socket  # 取得主机通信连接接口，QMP/调试器通过独立连接交接。
import subprocess  # 以参数列表启动已有外部程序，并读取输出或退出码。
import tarfile     # 读取或构造 tar 归档，成员路径及类型还需单独检查。
import time  # 单调时钟计算截止时间，sleep 给设备与进程处理时间。
from tools import ROOT    # 共享 ROOT 指向项目根目录，后续路径不依赖调用位置。
from boot import command  # 复用 QEMU 参数组装，本模块只额外接入调试路径。

parser=argparse.ArgumentParser()  # 主机参数解析器提供--host选项与用法提示。
parser.add_argument('--host',default='airi@ssh.airi-dynamic.top')  # 个人Linux主机可以显式替换，不涉及公司机器。
args=parser.parse_args()  # 把进程参数转为Namespace，后续通过 args.host 取得目标主机字符串。
if args.host.startswith('-') or any(c.isspace() for c in args.host):  # 禁止选项形状和空白，避免host被外部工具误解析；生成器逐字符检查空白。
    raise RuntimeError('Invalid host')  # 拒绝无效主机字符串，尚未启动网络程序。
work=ROOT/'artifacts/kernel-debug'      # 本机QEMU与GDB会话证据路径。
work.mkdir(parents=True,exist_ok=True)  # 建立本轮输出目录，已有目录允许复用。
remote='AiriOS-labs/p1/kernel-debug'    # 远端调试器源码及ELF单独放在此目录。
base=['ssh','-o','BatchMode=yes','-o','ConnectTimeout=10']  # 共同SSH选项不包含host，后续各命令追加。
remote_home=subprocess.check_output(base+[args.host,'pwd'],text=True).strip()  # 查询远端真实工作目录，text转文本、strip去末尾换行。
source_dir=remote+'/source'  # GDB查找源文件的位置，不是guest内核加载位置。
subprocess.run(base+[args.host,f'mkdir -p {shlex.quote(source_dir)}'],check=True)  # 创建独立远端源码目录，shell目录文字引用完整。
archive=io.BytesIO()  # 空内存字节流承接待发送tar，不落临时磁盘文件。
with tarfile.open(fileobj=archive,mode='w') as tar:  # 以写归档模式向内存流装入源码。
    for name in ('kernel','include'):  # 只选当前调试涉及的源码与类型声明。
        tar.add(ROOT/name,arcname=name)  # 本机路径加入归档，归档内部保留相对name目录。
subprocess.run(base+[args.host,f'tar -xf - -C {shlex.quote(source_dir)}'],  # SSH把下一行input交到远端tar标准输入，在指定目录解包。
    input=archive.getvalue(),check=True)  # getvalue取得完整归档bytes，远端非零时立即失败。
subprocess.run(['scp','-q','-o','BatchMode=yes','-o','ConnectTimeout=10',  # scp安静且非交互传输，连接上限同SSH。
    str(ROOT/'build/normal/airios.elf'),f'{args.host}:{remote}/airios.elf'],check=True)  # 复制普通配置带符号ELF，仅供远端GDB解释地址与类型。
with socket.socket() as available:  # 临时TCP socket向主机申请当前可用端口。
    available.bind(('127.0.0.1',0))  # 回环地址端口0表示由系统选择空闲端口。
    port=available.getsockname()[1]  # 二元组第1项取得端口号，离开with后释放临时占用。
cmd=command('normal',work,debug=True,debug_port=port)  # 本地QEMU按该端口启动GDB stub并先暂停CPU。
with (work/'qemu.log').open('w') as log:  # QEMU主机输出写文件，离开with自动关闭。
    process=subprocess.Popen(cmd,stdout=log,stderr=log)  # 启动本地虚拟机并保留可轮询、终止的句柄。
    try:  # 等待监听和调试会话失败都需回收本地QEMU。
        deadline=time.monotonic()+5  # GDB监听准备最多等待五秒。
        while True:  # 重试本机监听探测，成功break，五秒截止时失败。
            if process.poll() is not None:  # poll 非 None 表示虚拟机已经结束，不能再等待监听
                raise RuntimeError('QEMU exited before debugger connection')  # 报告提前退出，把清理交给 finally
            try:  # 每次尝试连接，只把暂时的连接错误交给重试。
                with socket.create_connection(('127.0.0.1',port),timeout=0.2):  # 最多 200 毫秒试连接本机调试端口
                    pass  # 无须交换调试数据；退出 with 即关闭这次探测连接
                break  # 探测成功退出监听等待，开始实际GDB会话。
            except OSError:  # 尚未监听等连接失败暂时允许重试。
                if time.monotonic()>deadline:  # 重试已超过五秒总准备时间
                    raise RuntimeError('GDB listener timeout')  # 明确报告超时，不把等待中的 QEMU 算作成功
                time.sleep(0.1)  # 给虚拟CPU和日志写入100毫秒处理时间，再检查。
        expressions=[  # GDB命令列表按入口、换栈、C参数三段依次观察。
            'set pagination off', 'set confirm off',  # 批处理不分页且不等待交互确认。
            f'set substitute-path {ROOT} {remote_home}/{source_dir}',  # 把ELF内本机源路径映射到远端副本路径。
            f'target remote 127.0.0.1:{port}',  # GDB连接远端回环转发端口，最终到本机QEMU。
            'hbreak *_start', 'continue', 'info registers rip rsp cs', 'x/7i $rip',  # 入口硬件断点停CPU，读指令指针、栈和代码段，再看七条指令。
            'hbreak *airi_limine_start', 'continue', 'info registers rip rsp',  # 汇编换栈后停在适配C入口，观察当前栈指针。
            'printf "AIRI_STACK_OK=%d\\n", ($rsp>(unsigned long)&airi_stack_bottom)&&($rsp<(unsigned long)&airi_stack_top)',  # 地址比较核对RSP严格位于自有栈两端之间，&&要求同时成立。
            'hbreak *airi_kernel_main', 'continue',  # 在真正内核C入口停下，读取适配层交来的参数。
            'print ((AiriBootInfo*)$rdi)->version',  # RDI按System V保存首参数，转换为指针后读协议版本。
            'print ((AiriBootInfo*)$rdi)->region_count',  # 同一指针读取实际有效区域数。
            'print/x ((AiriBootInfo*)$rdi)->kernel_virtual_base',  # /x以十六进制输出内核虚拟起点。
            'print ((AiriBootInfo*)$rdi)->framebuffer.width',      # 读取实际图形宽度，不采用主机窗口配置。
            'printf "AIRI_KERNEL_ABI_OK=%d\\n", ((AiriBootInfo*)$rdi)->magic==0x41495249424f4f54',  # 参数必须指向有正确magic的启动描述，才说明当前ABI交接满足约定。
            'detach', 'quit']  # 脱离guest并结束批处理，进程清理由下面finally负责。
        gdb=['gdb','-q','-nx','-batch',f'{remote}/airios.elf']  # 不加载用户启动配置，按固定符号ELF执行批处理。
        for expr in expressions:  # 按已列出的顺序处理每条GDB文本命令。
            gdb += ['-ex', expr]  # 一对列表元素保持参数边界。
        try:  # 限时调试失败也应保留截止前已经收到的GDB输出。
            session=subprocess.run(base+['-o','ExitOnForwardFailure=yes',  # 转发端口建立失败必须让SSH非零退出。
                '-R',f'127.0.0.1:{port}:127.0.0.1:{port}',args.host,shlex.join(gdb)],  # 远端回环监听转回本机；join引用远端GDB各参数。
                capture_output=True,text=True,timeout=45)  # 两种输出均收集，整段会话最多45秒。
        except subprocess.TimeoutExpired as error:  # run超过45秒时先结束SSH子进程，再交出已收集输出。
            transcript=(error.stdout or b'')+(error.stderr or b'')  # TimeoutExpired持有bytes，没收到该路输出时以空bytes代替。
            (work/'gdb.txt').write_text(transcript.decode(errors='replace'))  # 解码本次部分记录，异常字节可见而不阻止保存。
            raise RuntimeError(f'Kernel debug timeout; see {work/"gdb.txt"}') from error  # 报告实际记录位置，finally继续回收本地QEMU。
        transcript=session.stdout+session.stderr  # 合并本轮GDB与SSH正常/错误输出。
        (work/'gdb.txt').write_text(transcript)   # 先保存实际记录，再判断成功。
        if session.returncode or 'AIRI_STACK_OK=1' not in transcript or 'AIRI_KERNEL_ABI_OK=1' not in transcript:  # 退出状态、栈边界或启动magic任一失败都不能通过。
            raise RuntimeError(f'Kernel debug validation failed; see {work/"gdb.txt"}')  # 报告已经保存的调试记录位置。
        (work/'result.json').write_text(json.dumps({'passed':True,'host':args.host,  # 将本轮真实调试主机和成功状态保存。
            'local_qemu':True,'owned_stack_verified':True,'boot_info_verified':True},indent=2)+'\n')  # 明确guest在本地，核对的是栈与启动参数，而非其他OS功能。
        print('PASS kernel GDB: x86-64 entry, owned stack, boot info v1 inspected')  # 三个入口和合同全部核对后才报告通过。
    finally:  # 成功返回与异常离开都进入收尾。
        process.terminate()  # 先请求主机 QEMU 进程结束。
        try:  # terminate 发出请求后仍须等待进程真正退出
            process.wait(timeout=5)  # 最多五秒，防止清理本身无限等待
        except subprocess.TimeoutExpired:  # 虚拟机未响应正常终止请求
            process.kill()  # 强制结束本机 QEMU 进程
            process.wait()  # 回收已经结束的进程，避免留下子进程资源
