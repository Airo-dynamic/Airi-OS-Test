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
