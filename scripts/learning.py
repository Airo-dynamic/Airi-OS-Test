"""Inspect or combine explicit learning routes without changing a learning workspace."""  # 学习进度独立于 Git 分支和是否掌握知识。
import argparse  # 为列出、阅读、复现、导出和进度建立子命令。
import json      # 保存路线进度和读取清单。
from pathlib import Path  # 处理目标目录与输出路径。
import subprocess  # 执行当前选择的宿主实验。
import tempfile    # 为隔离复现建立独立父目录。
from tools import ROOT, make_command      # 共用主机工具选择和工程位置。
from course import plan, replay, ordered  # 使用同一依赖图重建，避免第二套学习顺序。

LABS = {'memmove': ('lab-memmove', 'labs/README.md'), 'boot-info': ('lab-boot-info', 'labs/README.md'), 'editor': ('lab-editor', 'labs/README.md')}  # 实验名映射到已有构建动作。


def checkpoints():  # 清单中的身份不依赖 Git 或目录名。
    return {item['id']: item for item in plan()['chapters']}  # 新路线可以有各自编号。


def export_checkpoint(name, destination, alongside=None):  # 课程导出组合正文，current才使用整个HEAD树。
    from snapshots import export_checkpoint as export_git, export_route  # 两种来源有不同的声明。
    if name == 'current':  # 当前跟踪树不代表某个独立学习阶段。
        dirty = subprocess.check_output(['git', 'status', '--porcelain'], cwd=ROOT, text=True)  # 固定来源前确认未提交改动。
        if alongside or dirty.strip():  # 当前树不能与另一路线混称同一提交。
            raise RuntimeError('Current export needs a clean tree and no alongside route')  # 保留学习者修改。
        commit = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip()  # 只固定一次HEAD。
        export_git({'id':'current', 'commit':commit}, destination)  # 原语检查路径、归档和已有目录。
    else:  # 正常检查点通过完整正文与显式前置重建。
        export_route(name, destination, alongside)  # GUI只添加自己的源码，不能回退主线。


def main(argv=None):  # 统一入口可以显式传入后续参数。
    nodes = checkpoints()  # 一次解析使用固定的节点集合。
    parser = argparse.ArgumentParser(description=__doc__)  # 帮助说明来自本模块职责。
    actions = parser.add_subparsers(dest='action', required=True)  # 必须选择一个动作。
    listing = actions.add_parser('list')  # 列表不运行或写入实现。
    listing.add_argument('--track', choices=('core','graphics','linux','tools'))  # 可只看当前路线。
    show = actions.add_parser('show')  # 阅读一篇完整正文。
    show.add_argument('day', choices=nodes)  # 接受检查点身份，不从文件名前两位猜编号。
    check = actions.add_parser('check')      # 从空目录实际执行指定路线。
    check.add_argument('day', choices=nodes)  # 终点必须已经发布。
    check.add_argument('--alongside', choices=nodes)  # 可核对两线组合。
    export = actions.add_parser('export')  # 导出不执行命令，继续保护已有目标。
    export.add_argument('name', choices=[*nodes, 'current'])  # 只列出当前课程身份与明确的当前树。
    export.add_argument('destination')  # 目标必须是尚不存在的新目录。
    export.add_argument('--alongside', choices=nodes)  # 指定图形半成品时不影响主线文件。
    progress = actions.add_parser('progress')  # 仅记录学习者选择的停留位置。
    progress.add_argument('--record', choices=nodes)  # 不传值时只读取，不自动推进。
    lab = actions.add_parser('lab')  # 保留已有主机观察入口。
    lab.add_argument('name', choices=LABS)  # 未声明实验在解析阶段拒绝。
    args = parser.parse_args(argv)  # 一次取得当前动作和对应参数。
    if args.action == 'list':  # 按清单顺序列出路线与前置。
        for item in nodes.values():  # 不使用全局课程编号排序。
            if args.track and item['track'] != args.track:  # 过滤只影响显示。
                continue  # 不改变依赖图。
            print(f'{item["id"]} [{item["track"]}] {item["title"]}\n  {item["document"]}  requires: {", ".join(item["requires"]) or "empty"}')  # 显示实际阅读入口和起点。
    elif args.action == 'show':  # 正文按元数据定位。
        print((ROOT / nodes[args.day]['document']).read_text())  # 输出完整章节。
    elif args.action == 'check':  # 隔离复现不使用用户学习目录。
        target = Path(tempfile.mkdtemp(prefix='airios-learning-')) / 'work'  # 子目录尚不存在。
        replay(target, args.day, True, args.alongside)  # 实际运行记录保存在目标内。
        print(f'Rebuilt: {target}')  # 无论读者是否理解，工具只声明已复现。
    elif args.action == 'export':  # 按所选来源写入新目录。
        export_checkpoint(args.name, args.destination, args.alongside)  # 覆盖保护由底层入口执行。
    elif args.action == 'progress':  # 各路线各有一个停留点，不相互重置。
        path = ROOT / 'artifacts/learning-progress.json'  # 个人进度不进入 Git 或共享画像。
        value = json.loads(path.read_text()) if path.exists() else {}  # 第一次使用得到空记录。
        if args.record:  # 仅明确记录动作才修改文件。
            value[nodes[args.record]['track']] = args.record  # 更新一条路线，保留其他路线位置。
            path.parent.mkdir(parents=True, exist_ok=True)    # 个人记录目录可以已经存在。
            path.write_text(json.dumps(value, ensure_ascii=False, indent=2)+'\n')  # 不写 mastered、passed 或评分。
        print(json.dumps(value, ensure_ascii=False, indent=2))  # 读取和记录都展示实际保存内容。
    else:  # 剩下的合法动作是一个已声明宿主实验。
        target, guide = LABS[args.name]  # 解包构建动作和观察说明。
        result = subprocess.run(make_command(target=target), cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)  # 合并输出但保留退出状态。
        output = ROOT / 'artifacts/labs' / (args.name+'.txt')  # 日志不进入源码目录。
        output.parent.mkdir(parents=True, exist_ok=True)  # 准备本次证据目录。
        output.write_text(result.stdout)  # 保存实际输出，失败也保留。
        print(result.stdout, end='')      # 显示同一份结果。
        print(f'Log: {output}\nGuide: {ROOT / guide}')  # 给出实际文件位置。
        if result.returncode:  # 非零状态必须交回外层。
            raise RuntimeError(f'Lab {args.name} failed: {result.returncode}')  # 不以日志生成代替通过。


if __name__ == '__main__':  # 导入模块只建立定义。
    main()  # 直接执行才运行学习工具。
