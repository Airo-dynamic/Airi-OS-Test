import shutil  # 标准库提供可执行文件的 PATH 查找，不启动外部 which 命令
import subprocess  # 启动子进程并等待版本命令结束
import sys  # 读取本次程序参数，并把错误输出到 stderr


def find_tool(name: str) -> str:  # 接收命令名；成功返回路径字符串，失败抛出异常
    path = shutil.which(name)  # 如 python3 -> /usr/bin/python3；找不到时结果为 None
    if path is None:  # None 表示没有找到文件，不能继续把它交给进程启动接口
        raise RuntimeError(f"找不到可执行文件: {name}")  # f 字符串把当前命令名填入错误消息
    return path  # 把已找到的路径交给 main；查找本身还未执行该程序
def main() -> int:  # 返回退出状态：0 成功，1 失败；-> int 是类型提示
    name = sys.argv[1] if len(sys.argv) == 2 else "python3"  # 一项额外参数选工具，否则默认 python3
    try:  # 此范围的查找与启动异常统一转成错误消息和退出状态
        path = find_tool(name)  # 如 name=python3，从查找函数取得实际文件路径
        command = [path, "--version"]  # 列表中首项是程序，第二项是独立参数，不经过 shell 拆分
        print(f"运行: {command}", flush=True)  # 先刷新当前进程输出，再让子进程打印版本
        subprocess.run(command, check=True, timeout=10)  # 等待完成；非零退出或超过十秒均抛出异常
    except (RuntimeError, OSError, subprocess.SubprocessError) as error:  # 捕获查找、系统启动及子进程失败
        print(f"错误: {error}", file=sys.stderr)  # file 参数将诊断送到标准错误通道
        return 1  # shell 用非零退出状态判断这次探测失败
    return 0  # 查找和版本执行都完成，交回成功状态


if __name__ == "__main__":  # 直接运行文件才进入此分支；作为模块导入不自动执行
    raise SystemExit(main())  # 用 main 的整数结果结束当前 Python 进程
