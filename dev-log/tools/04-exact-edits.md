# T04 · 只替换已确认的源码窗口

T03 可以从章节重建完整文件。继续修改调度器时，新增一个等待分支只涉及几十行，却需要重新写入已有的一千多行。现在让一次编辑同时携带旧窗口和新窗口：先确认旧窗口在前一状态中恰好出现一次，再把它替换。其他函数继续来自前文，不从完成版目录补入。

从 T03 的同一目录继续。下面的更改只影响教程编辑器与它的观察程序，不改变任何内核、用户程序或旧检查点。旧章节仍使用原来的 write/append。本节完成以后，后续章节才可使用 replace；精确定位失败时必须回到实际起点核对，不能选择最相近的函数继续。

<a id="exact-window-model"></a>
## 一份旧文本怎样确定修改位置

假设内存中的文件 x.c 是两行 `one`、`two`，每行各有末尾换行。替换动作中的 before 保存 `one` 及其换行，after 保存 `new`、一个空行。替换成功后，文件成为 `new`、空行、`two`。after 中的空行是新文件内容；标签和围栏只描述编辑，不写入文件。

这项定位依赖旧文本，而不是行号。前章插入其他函数以后，只要这段完整文本仍唯一，编辑就有确定位置；如果旧窗口消失或出现两次，就无法判断应修改哪里，整章停止。旧窗口包含换行和旁注，空白不能先被清理，否则检查的便不是跟敲留下的那份源码。只有空白的 before 不能提供有意义的位置，因此拒绝；空 after 则表示确实删除这个窗口。

协议按下面的形状书写。此围栏是协议示例，不是需要新建的 x.c：

```text
<!-- replace: x.c -->
<!-- before -->
```

before 标签后紧接一份完整源码围栏，接着是 after 标签及新源码围栏。编辑器不在普通正文中搜下一块代码来凑配对。下面实现会把每一份围栏读成完整字符串，再按正文次序应用。

<a id="exact-edit-parser"></a>
## 先找到围栏，再把它交给编辑动作

在 scripts/course.py 中替换为以下四块连续源码。第一块保留 T01 的资料读取和依赖排序，同时调整编辑标记的识别方式。BLOCK 现在只识别当前行的动作开头；FENCE 负责从指定位置读到独占一行的关闭围栏。S 让点号跨行，M 让行首锚点作用于每一行，二者共同保留代码内部的末尾换行。此时文件尚未写完，接上后面三块再运行。

<!-- write: scripts/course.py -->
```python
"""Reconstruct teaching routes from explicit write, append and exact replacement edits."""  # 从教程编辑动作重建，不复制终点实现。
import argparse  # 解析目标目录、路线与执行选项。
import hashlib   # 核对每个检查点的累计源码字节。
import json      # 读取依赖图并保存实际运行记录。
from pathlib import Path  # 安全组合工程和导出目录。
import re  # 提取完整源码块及运行块。
import subprocess  # 隔离执行教程命令并保留退出状态。
from tools import ROOT  # 根目录由本脚本位置决定。

BLOCK = re.compile(r'<!-- (write|append|replace): ([^\n]+) -->\n')  # 只识别独占一行的编辑起始标记。
FENCE = re.compile(r'^```[^\n]*\n(.*?)^```[ \t]*(?:\n|$)', re.S | re.M)  # 完整围栏包括代码的末尾换行。
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


```

一次 `source_block(document, position)` 从标记结束位置开始。允许跳过标记外的空行；进入围栏以后，空行属于源码，不再跳过。match 必须从当前位置匹配，因而普通正文不能被无声越过。返回的两项分别是源码字符串、围栏结束后的新位置，后者让调用方继续读取同一次编辑。

例如刚才的 before 读出 `one` 及换行后，position 已位于旧围栏之后。编辑解析器接下来必须在这里找到 after 标签，才能读取新文本。围栏未闭合时没有完整编辑，不能先交出前半段。

<!-- append: scripts/course.py -->
```python
def source_block(document, position):  # 从指定位置取得一份明确的源码围栏。
    while document.startswith('\n', position):  # 标记与围栏之间允许空行，不吞掉代码内空行。
        position += 1  # 只前进到本次内容的起点。
    match = FENCE.match(document, position)  # 不能越过普通正文去猜下一块源码。
    if not match:  # 缺围栏或未关闭都属于不完整编辑。
        raise RuntimeError('Teaching edit needs a closed source fence')  # 不静默跳过损坏动作。
    return match[1], match.end()  # 非空源码保留末尾换行，空的新文本可用于删除旧窗口。


```

`edits` 逐行推进同一份文章。普通正文中的动作标记确定 mode 和 name；replace 再读取 before、after。一次动作完整以后，yield 交出四项，让 apply 处理当前文件，随后从已保存的 position 继续。yield 暂停生成器时，局部位置仍在；下一轮迭代继续同一次文章扫描，不会从章首重新查找。

普通展示围栏也整体跳过。否则教程解释协议时写出的标记，或者 Python 字符串里的示例，会被误当成实际编辑。未位于围栏里的类似编辑行若格式损坏，则明确报错。这样“没有编辑”和“写坏了编辑”有不同结果，不能静默漏掉一项修改。

<!-- append: scripts/course.py -->
```python
def edits(document):  # 逐行走过正文，只在普通文本位置识别编辑标记。
    position = 0  # 下一次检查的行起点。
    while position < len(document):  # 文件中的编辑次序就是实际执行次序。
        marker = BLOCK.match(document, position)  # 模式不跨行，不能把路径后面的正文当参数。
        if marker:  # 找到write、append或replace。
            mode, name = marker.groups()  # 同一标记提供动作与目标相对路径。
            position = marker.end()  # 接下来必须是对应的内容。
            before = None  # write/append没有旧文本条件。
            if mode == 'replace':  # 替换需要两份有标签的源码，不能用行号或模糊匹配。
                if not document.startswith('<!-- before -->\n', position):  # 旧文本标签紧跟编辑标记。
                    raise RuntimeError('Replacement needs <!-- before -->')  # 拒绝漏写或调换标签。
                before, position = source_block(document, position + len('<!-- before -->\n'))  # 读取旧文本本身。
                while document.startswith('\n', position):  # 两块之间允许空行。
                    position += 1  # 不改变旧文本内的空白。
                if not document.startswith('<!-- after -->\n', position):  # 新文本必须有明确标签。
                    raise RuntimeError('Replacement needs <!-- after -->')  # 不把下一章动作当新文本。
                position += len('<!-- after -->\n')  # source_block将从新围栏开始读取。
                if not before.strip():  # 空白不能提供可靠的局部定位。
                    raise RuntimeError('Replacement before text must contain source')  # 增加真实上下文再编辑。
            code, position = source_block(document, position)  # 按项目原生换行约定保留内容。
            yield mode, name, before, code  # 调用方稍后检查路径与已有状态。
            continue  # 这次源码内的标记不再被当成正文动作。
        fence = FENCE.match(document, position)  # 普通演算或命令围栏也要整体跳过。
        if fence:  # 展示代码里可以出现字面量编辑标记。
            position = fence.end()  # 不执行展示中的嵌套内容。
            continue  # 继续检查围栏后面的正文。
        line_end = document.find('\n', position)  # 没有标记时只跳过当前一行。
        line = document[position:line_end if line_end >= 0 else len(document)]  # 最后一行可能没有换行。
        if re.match(r'<!-- (?:write|append|replace|before|after)\b', line):  # 看似编辑却未满足格式不能忽略。
            raise RuntimeError(f'Malformed teaching edit: {line}')  # 保留损坏行便于定位。
        position = line_end + 1 if line_end >= 0 else len(document)  # 前进到下一行或结束。


```

解析器现在可以交出 replace，但目标状态仍是一张“路径→完整字符串”的字典。apply 先复制这张字典为 pending：字符串不可变，后续连接和替换产生新的字符串，不会修改原值。只有整章动作都有效，才清空外部 state 并交入 pending；第二项失败时，第一项临时写入的另一个文件也不会残留。

沿 x.c 的两行继续。find 找到 before 的第一处位置，然后从下一字符再查一次。第二次不能从旧窗口末尾开始，否则会漏掉相互重叠的两处，例如三行 a 中的连续两行 a。唯一性成立后，replace 只改一处。路径仍按原规则检查，append/replace 都要求已有文件；新实现不会到参考工程磁盘里寻找缺少的起点。

以下追加 apply 及其后保留的摘要、审查、导出和命令入口。写完以后再运行观察。旧 write/append 的空围栏仍产生一个换行，新 replace 的空 after 则确实删除；二者不能因名字类似而共用空文本处理。

<!-- append: scripts/course.py -->
```python
def apply(document, state):  # 整章检查完成后才把新状态交回调用者。
    pending = state.copy()  # 字符串不可变，浅复制足以隔离失败编辑。
    for mode, name, before, code in edits(document):  # 从正文顺序取得确定动作。
        path = Path(name)  # 路径不交给Shell解释。
        if path.is_absolute() or '..' in path.parts or not path.parts or str(path) != name:  # 保护重建目录。
            raise RuntimeError(f'Unsafe teaching path: {name}')  # 沿用原有路径约束。
        if mode != 'write' and name not in pending:  # 追加和替换都必须已有明确起点。
            raise RuntimeError(f'{mode.title()} before creation: {name}')  # 不读取终点文件补洞。
        if mode == 'replace':  # 旧文本包含围栏保留的换行，按字节形状核对。
            first = pending[name].find(before)  # 精确查找，不去空白、不猜相似函数。
            if first < 0 or pending[name].find(before, first + 1) >= 0:  # 第二次从下一字符查找，也拒绝重叠匹配。
                reason = 'missing' if first < 0 else 'ambiguous'  # 区分旧版本不符和缺少唯一定位。
                raise RuntimeError(f'Replacement requires one match in {name}: {reason}')  # 不选择第一个凑合继续。
            pending[name] = pending[name].replace(before, code, 1)  # 只改变唯一的已声明窗口。
        else:  # write完整赋值，append在已有文件后继续。
            pending[name] = (pending[name] if mode == 'append' else '') + (code or '\n')  # 保留旧write/append空围栏写入一个换行的约定。
    state.clear()  # 只有全部动作有效才替换外部累计状态。
    state.update(pending)  # 失败时原state保持，成功仍复用调用者字典。


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
        apply(text, state)     # 执行正文中的写入、追加或精确替换。
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
```

<a id="exact-edit-observation"></a>
## 让成功与拒绝都经过实际编辑器

新建 tests/course_edits_test.py。测试只构造内存文件映射，不写学习目录，也不启动 guest。第一项沿 one/two 运行真实编辑器；后续分别经过删除、旧空块、缺失与重叠、起点与路径、损坏格式、空定位和字面量。失败原子性场景先临时创建 new.c，再让替换失败，最后核对原字典仍只含 x.c。

辅助函数把 code 放进明确围栏。它接收的 code 已含末尾换行，所以不再补第二个换行。TestCase 的每个 test 方法由 unittest 建立新测试调用，self 用于比较实际结果或要求异常；这里的 require 由测试框架承担，读者无需另交练习或证明理解。

<!-- write: tests/course_edits_test.py -->
```python
"""Exercise exact teaching edits without starting a guest or changing a learner tree.""" # 仅验证作者编辑协议。
from pathlib import Path # 从测试文件位置找到同一份工程脚本。
import sys # 将当前工程scripts加入模块搜索位置。
import unittest # 每个正常和错误场景都保留具体断言。
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts')) # 不依赖运行时所在目录。
import course # 使用真实编辑器，不在测试内重写解析算法。

def edit(mode, name, code): # 生成一个完整write或append动作。
    return f'<!-- {mode}: {name} -->\n```c\n{code}```\n' # code含末尾换行，围栏外不补源码字节。

def replace(name, before, after): # 旧窗口与新窗口有各自明确标签。
    return f'<!-- replace: {name} -->\n<!-- before -->\n```c\n{before}```\n<!-- after -->\n```c\n{after}```\n' # 空after直接关闭围栏，表示删除。

class CourseEdits(unittest.TestCase): # 每项使用新的内存字典，不操作真实源码目录。
    def test_order(self): # 写入、追加、替换按文章次序组合。
        state = {} # 章节起点还没有文件。
        course.apply(edit('write', 'x.c', 'one\n') + edit('append', 'x.c', 'two\n') + replace('x.c', 'one\n', 'new\n\n'), state) # 新文本中的空行须保留。
        self.assertEqual(state, {'x.c': 'new\n\ntwo\n'}) # 追加部分不在替换窗口内，保持原样。
    def test_delete(self): # 删除只能拿走声明的旧窗口。
        state = {'x.c': 'one\ntwo\n'} # 两段已经存在。
        course.apply(replace('x.c', 'one\n', ''), state) # 空新文本合法。
        self.assertEqual(state['x.c'], 'two\n') # 第二段和末尾换行保留。
    def test_empty_legacy(self): # 旧write/append空块行为继续兼容。
        state = {} # 不借最终工程文件。
        course.apply(edit('write', 'x.c', '') + edit('append', 'x.c', ''), state) # 旧动作每个空块生成一个换行。
        self.assertEqual(state['x.c'], '\n\n') # 与旧检查点逐字节相同。
    def test_missing_and_overlap(self): # 唯一定位同时排除缺失、多处和重叠命中。
        for contents, before in [('other\n', 'old\n'), ('old\nold\n', 'old\n'), ('a\na\na\n', 'a\na\n')]: # 最后一个窗口相互重叠。
            state = {'x.c': contents}; original = state.copy() # 保存本章开始前的完整状态。
            with self.assertRaisesRegex(RuntimeError, 'requires one match'): # 不能选中第一处凑合继续。
                course.apply(edit('write', 'new.c', 'created\n') + replace('x.c', before, 'new\n'), state) # 前一个动作也不能在整章失败后残留。
            self.assertEqual(state, original) # 失败原子性覆盖全部文件。
    def test_requires_start(self): # 替换和追加都需要已创建文件。
        for document in [replace('x.c', 'old\n', 'new\n'), edit('append', 'x.c', 'new\n')]: # 两个错误入口共享起点规则。
            with self.assertRaisesRegex(RuntimeError, 'before creation'): # 不读取仓库中的终点补洞。
                course.apply(document, {}) # 空状态不能凭空产生文件。
    def test_paths(self): # 新动作沿用已有目录保护。
        for name in ['/tmp/x.c', '../x.c', 'a//b.c']: # 绝对、越界和非规范路径都不接受。
            with self.assertRaisesRegex(RuntimeError, 'Unsafe teaching path'): # 在写磁盘之前拒绝。
                course.apply(edit('write', name, 'text\n'), {}) # 输入仍完整，失败仅来自路径。
    def test_format(self): # 编辑标记损坏不能静默遗漏。
        for document in ['<!-- replace: x.c -->\n```c\nold\n```\n', '<!-- write: x.c -->\n```c\nopen\n', '<!-- write: x.c --> trailing\n']: # 分别缺标签、未关闭和标记尾随正文。
            with self.assertRaises(RuntimeError): # 三者都必须明确失败。
                course.apply(document, {}) # 原状态仍为空。
    def test_whitespace_before(self): # 空白不能承担唯一定位。
        with self.assertRaisesRegex(RuntimeError, 'must contain source'): # 不用一行空行选中任意位置。
            course.apply(replace('x.c', '\n', 'new\n'), {'x.c': '\n'}) # 即使只有一次也拒绝空定位。
    def test_literals(self): # 展示围栏里的协议文字不是编辑指令。
        document = '```text\n<!-- write: ignored.c -->\n```\n' + edit('write', 'x.c', '/* <!-- replace: literal --> */\n') # 正文展示和源码字符串都含类似标记。
        state = {} # 实际只应产生一个文件。
        course.apply(document, state) # 解析器按行走过围栏，不递归解释源码。
        self.assertEqual(state, {'x.c': '/* <!-- replace: literal --> */\n'}) # 字面量完整保留。

if __name__ == '__main__': # 直接运行测试文件时才启动unittest。
    unittest.main() # 任一断言失败交回非零退出码。
```

运行：

<!-- run -->
```sh
python3 tests/course_edits_test.py  # 在当前工程运行九个正常和拒绝场景，不启动内核或远程主机
```

本轮观察为九项测试通过，结尾显示 OK。把“缺失旧窗口”的场景代入 apply：pending 已有临时 new.c，第二项 find 返回 -1，异常沿调用链返回，state.clear 尚未执行，因此原状态完整保留。测试继续读取 state 时，new.c 不存在。这是整章失败不发布的行为，不表示已经写入的磁盘目录可以任意覆盖；T02 对新导出目录和已编辑 HISTORY 的保护仍保持。

后续 K53 使用这里的 before/after 在既有调度器中增加内存等待。普通旧章节的源码字节与运行命令不因编辑器升级改变；旁注检查只把 after 当作新的待写源码，before 保留原字节供定位，不能单独批量对齐其中一侧。
