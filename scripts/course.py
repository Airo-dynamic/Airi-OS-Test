"""Reconstruct independent teaching routes from complete write/append blocks."""  # 从教程编辑动作重建，不复制终点实现。
import argparse  # 解析目标目录、路线与执行选项。
import hashlib   # 核对每个检查点的累计源码字节。
import json      # 读取依赖图并保存实际运行记录。
from pathlib import Path  # 安全组合工程和导出目录。
import re  # 提取完整源码块及运行块。
import subprocess  # 隔离执行教程命令并保留退出状态。
from tools import ROOT  # 根目录由本脚本位置决定。

BLOCK = re.compile(r'<!-- (write|append): ([^\n]+) -->\n```[^\n]*\n(.*?)\n```', re.S)  # 每项依次捕获动作、路径和完整源码。
RUN = re.compile(r'<!-- run -->\n```sh\n(.*?)\n```', re.S)  # 只执行显式标注的命令，忽略展示输出。


def plan(source=None):  # 主线资料始终存在，GUI资料只在合并支线后出现。
    root = ROOT if source is None else Path(source)  # 默认读当前工程，固定来源导出可传临时只读视图。
    data = json.loads((root / '.codex/teaching/checkpoints.json').read_text())  # 主线清单不包含GUI文件或字体。
    optional = root / '.codex/teaching/graphics.json'  # GUI分支只增加自己的清单，不修改主线清单。
    if optional.is_file():  # 未合并GUI时不读取它的任何源码。
        graphics = json.loads(optional.read_text())  # 外层字典包含chapters、owned和endpoints三个集合。
        overlap = set(data['owned']) & set(graphics['owned'])  # 两条路线不能宣称拥有同一实现文件。
        if overlap:  # 覆盖主线文件会破坏随时合并与独立推进的前提。
            raise RuntimeError(f'GUI overwrites main-owned paths: {sorted(overlap)}')  # 在重建或写目录前报告冲突。
        for key in ('chapters', 'owned', 'endpoints'):  # 章节、路径和终点按各自列表组合。
            data[key] = data[key] + graphics[key]  # 加号形成新列表，不改变磁盘中的两份清单。
    return data  # 后续递归固定使用这次读取的同一组节点。


def ordered(names, data=None):  # 沿显式前置关系取得一次组合所需的章节次序。
    data = plan() if data is None else data  # 调用者可固定一次读取的清单。
    nodes = {item['id']: item for item in data['chapters']}  # 编号映射到所属路线、正文与依赖。
    if len(nodes) != len(data['chapters']):  # 重复身份会让后面的字典项覆盖前项，必须拒绝。
        raise RuntimeError('Duplicate checkpoint id')  # 不允许两份不同正文占用同一快照目录。
    result, visiting, visited = [], set(), set()  # 分别保存顺序、当前递归链和已完成节点。
    def visit(name):  # 深度优先访问一项及其全部前置。
        if name not in nodes:  # 未知编号不能默认为终点。
            raise RuntimeError(f'Unknown checkpoint: {name}')  # 在写入导出目录前报告拼写错误。
        if name in visiting:  # 同一递归链再次出现说明依赖成环。
            raise RuntimeError(f'Checkpoint dependency cycle: {name}')  # 不继续无限递归。
        if name in visited:  # 多条路线共享的基础只应用一次。
            return  # 避免后访问的图形路线覆盖较新的主线基础。
        visiting.add(name)  # 标记当前尚未闭合的节点。
        for parent in nodes[name]['requires']:  # 按声明顺序先处理必需版本。
            visit(parent)  # 递归闭合当前依赖。
        visiting.remove(name)  # 当前节点已不在递归栈中。
        visited.add(name)      # 后续共享依赖无需再次应用。
        result.append(nodes[name])  # 当前编辑动作排在全部前置之后。
    for name in names:  # 允许组合一个内核终点与一个图形终点。
        visit(name)  # 两条路线共用同一 visited 集合。
    return result  # 返回确定次序的章节记录。


def apply(document, state):  # 把一章的完整编辑动作作用于累计源码。
    for mode, name, code in BLOCK.findall(document):  # 一项对应一个源码围栏。
        path = Path(name)  # 使用路径段判断越界，不把输入当 shell 命令。
        if path.is_absolute() or '..' in path.parts or not path.parts or str(path) != name:  # 禁止写出重建目录。
            raise RuntimeError(f'Unsafe teaching path: {name}')  # 保留具体非法路径。
        if mode == 'append' and name not in state:  # 追加必须已经有正文创建的文件。
            raise RuntimeError(f'Append before creation: {name}')  # 不用空文件掩盖缺少的步骤。
        state[name] = (state[name] if mode == 'append' else '') + code + '\n'  # 完整替换或继续追加，保留末尾换行。


def digest(text):  # 文本先按 UTF-8 形成确定字节。
    return hashlib.sha256(text.encode()).hexdigest()  # 返回可存入清单的摘要字符串。


def state_for(names, data=None):  # 从空状态重建指定终点或组合。
    state = {}  # 每次调用拥有独立文件映射。
    for chapter in ordered(names, data):  # 顺序由前置关系决定，不按文件名猜测。
        apply((ROOT / chapter['document']).read_text(), state)  # 源码只来自当前教程。
    return state  # 不把源码写入用户目录。


def implementation_files():  # 盘点完整项目的自有实现，不把生成字体数组算作手敲代码。
    names = set()  # 集合去除重复路径。
    suffixes = {'.c', '.h', '.S', '.py', '.sh', '.m', '.ld', '.mk'}  # 构建片段也属于需要解释的自有实现。
    for directory in ('kernel', 'include', 'frontend', 'scripts', 'tests', 'labs'):  # 明确源码所属目录。
        for path in (ROOT / directory).rglob('*'):  # 目录不存在时为空，便于串口归档运行。
            relative = path.relative_to(ROOT)  # 清单统一保存相对路径。
            if '__pycache__' in relative.parts or 'artifacts' in relative.parts:  # 跳过可重建缓存与证据。
                continue  # 不要求教程手写运行结果。
            if path.is_file() and path.suffix in suffixes and path.name != 'font-atlas.h':  # 只收自有文字实现。
                names.add(str(relative))  # 把当前路径加入唯一集合。
    names.update(name for name in ('Makefile', 'airios', '.clangd', '.clang-format', '.gitignore', 'boot/AiriBoot/Makefile', 'config/deps.lock.json', 'config/boot/limine.conf', 'frontend/assets/fonts.lock.json') if (ROOT / name).is_file())  # 无后缀入口与配置显式登记。
    return names  # 全项目审查与单路线检查使用不同范围。


def audit():  # 检查依赖、每节点摘要和完整终点覆盖。
    data = plan()  # 固定本次审查的清单。
    for chapter in data['chapters']:  # 每一项都从自己的前置起点重建。
        state = state_for([chapter['id']], data)  # 不借用另一条路线的偶然状态。
        actual = {name: digest(code) for name, code in state.items()}  # 累计文件也有确定摘要。
        if actual != chapter['files']:  # 正文、顺序或源码任一变化都会失配。
            raise RuntimeError(f'{chapter["id"]}: checkpoint source mismatch')  # 定位最早失败的检查点。
        text = (ROOT / chapter['document']).read_text()  # 命令与源码绑定同一篇正文。
        if RUN.findall(text) != chapter['commands']:     # 修改运行路径也须同步清单。
            raise RuntimeError(f'{chapter["id"]}: checkpoint commands mismatch')  # 不拿旧运行声明验证新正文。
    state = state_for(data['endpoints'], data)  # 合并全部已交付路线，用于最终来源覆盖。
    if set(data['owned']) != implementation_files():  # 新增自有文件不能逃过正文覆盖。
        raise RuntimeError('Owned source inventory differs from the tutorial manifest')  # 报告清单遗漏。
    for name in data['owned']:  # 逐个核对完整项目当前字节。
        if state.get(name) != (ROOT / name).read_text():  # 缺完整代码块或版本不同都失败。
            raise RuntimeError(f'Final source has no matching lesson: {name}')  # 显示具体文件。
    print(f'PASS reconstruction: {len(data["chapters"])} checkpoints, {len(data["owned"])} owned files')  # 只声明来源一致性。
    return data  # 链接检查复用同一清单。


def replay(destination, through, execute=False, alongside=None, source=None):  # 独立导出一条路线，或与另一支线组合。
    names = [through] + ([alongside] if alongside else [])  # 先应用主线，再加入支线自己的编辑。
    source = ROOT if source is None else Path(source)  # 固定来源可以是已校验Git树的临时普通目录。
    chapters = ordered(names, plan(source))     # 先验证未知编号与循环，再创建目录。
    requested = Path(destination).expanduser()  # 先保留原路径，断开的链接也属于已存在对象。
    if requested.exists() or requested.is_symlink():  # 任何已有对象都不覆盖，包括断开的链接。
        raise RuntimeError('Replay needs a new directory; it never overwrites a workspace')  # 学习者改动保持原样。
    target = requested.resolve()  # 完成原路径保护后才固定实际位置。
    target.mkdir(parents=True)    # 所有前置检查完成后才创建新目录。
    state, evidence = {}, []      # 文件状态和本轮实际命令结果分别保存。
    for chapter in chapters:      # 按依赖顺序逐章写入完整状态。
        text = (source / chapter['document']).read_text()  # 读取该检查点正文。
        before = state.copy()  # 字符串不可变，浅复制足以保留前态。
        apply(text, state)     # 执行正文中的 write/append 动作。
        for name, code in state.items():  # 只更新本章改变的文件。
            if before.get(name) == code:  # 支线不会重写未改变的基础文件。
                continue  # 保持该文件原字节和时间戳。
            path = target / name  # apply 已拒绝越界路径。
            path.parent.mkdir(parents=True, exist_ok=True)  # 准备子目录。
            path.write_text(code)  # 写入当前检查点源码。
            if name == 'airios' or name.endswith('.sh'):  # 文本围栏无法携带执行位。
                path.chmod(0o755)  # 仅给明确脚本恢复可执行权限。
        if execute:  # 默认导出只写源码，不访问远端或启动虚拟机。
            for command in RUN.findall(text):  # 只执行本章已展示的完整命令块。
                try:  # 保留正常返回与超时的不同证据。
                    result = subprocess.run(['sh', '-eu', '-c', command], cwd=target, capture_output=True, text=True, timeout=600)  # 任一子命令失败即结束当前块。
                    record = {'checkpoint': chapter['id'], 'command': command, 'exit': result.returncode, 'output': result.stdout + result.stderr}  # 记录实际状态和输出。
                except subprocess.TimeoutExpired as error:  # 超时输出可能仍是 bytes。
                    out = error.stdout or b''  # 尚无输出时保留空字节。
                    err = error.stderr or b''  # 标准错误单独取得。
                    record = {'checkpoint': chapter['id'], 'command': command, 'exit': None, 'timed_out': True, 'output': (out.decode(errors='replace') if isinstance(out, bytes) else out) + (err.decode(errors='replace') if isinstance(err, bytes) else err)}  # 统一保存可读文字。
                evidence.append(record)  # 失败也先进入记录。
                (target / 'replay.json').write_text(json.dumps(evidence, ensure_ascii=False, indent=2))  # 及时留下可恢复的证据。
                if record['exit'] != 0:  # 超时的 None 也不能算通过。
                    raise RuntimeError(f'Replay failed at {chapter["id"]}: {target}/replay.json')  # 保留隔离目录供检查。
        print(f'REPLAY {chapter["id"]}: {len(state)} source files')  # 报告实现进度，不推断读者理解。
    (target / '.airi-checkpoint.json').write_text(json.dumps({'checkpoints': names, 'files': {name: digest(code) for name, code in state.items()}}, indent=2))  # 固定本次导出的路线与内容身份。
    return target  # 调用者可以在该目录构建或继续带敲。


def main():  # 直接执行提供来源检查或隔离复现。
    parser = argparse.ArgumentParser()  # 建立本模块参数解析器。
    parser.add_argument('destination', nargs='?')    # 不给目录时只审查参考工程。
    parser.add_argument('--through', default='K13')  # 默认选择已发布的串口主线终点。
    parser.add_argument('--alongside')  # 可显式组合一个图形检查点。
    parser.add_argument('--run', action='store_true')  # 执行需显式选择，默认不启动命令。
    args = parser.parse_args()  # 将实际参数转换成属性。
    if args.destination:  # 导出不依赖全项目审查，缺图形目录也不阻止串口路线。
        replay(args.destination, args.through, args.run, args.alongside)  # 按指定路线写入新目录。
    else:  # 没有目录时检查全项目源码与正文。
        audit()  # 不在当前工程生成实现文件。


if __name__ == '__main__':  # 导入本模块只建立函数。
    main()  # 只有直接执行才解析命令行。
