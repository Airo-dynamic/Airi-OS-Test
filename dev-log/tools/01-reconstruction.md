# T01 · 从一组前置关系重建自己的源码

串口主线K13已经能独立构建，L01又写出了Linux对照程序和调试脚本；远端观察是否完成仍取决于实际连接结果。现在要另存一份K13源码作对照：工具从00开始读到K13，把每章写入与追加的内容合成文件，再写到新目录。以后显式组合K13和图形G13时，共享的旧K11也只能应用一次，避免把较新的入口覆盖回去。

从L01结束的源码继续新建工具文件。参考工程提供课程正文、说明、读者资料和检查点JSON；正文里明确的编辑动作是重建输入，脚本将它们解析出来，而不是复制参考工程中的实现目录。自己的学习目录继续保留原有修改，对照版本写入另一个尚不存在的目录。

<a id="route-reconstruction"></a>
<a id="t01-inputs"></a>
## 先把章节资料读成数据

先把目标缩到 02：导出器拿到字符串 "02"，必须知道它的前置、去哪里找正文，以及重建后怎样核对。它消费的是课程声明，而不是根据当前磁盘上已有几个文件猜进度。主线清单 `.codex/teaching/checkpoints.json` 最外层是字典，`chapters`对应章节记录组成的列表。以02为例，记录中的`id`是字符串`"02"`，`requires`是身份列表`["01"]`，`document`保存正文相对路径。`files`也是字典，把截至02已经形成的每个源码路径映射到SHA-256摘要字符串；它记录累计状态，不只记录02改过的文件。最外层的`owned`列出本集合负责的实现路径，`endpoints`列出要组合成完整参考工程的终点身份。

磁盘中的JSON是一份文本，`read_text()`先读成字符串，`json.loads`再把其中的对象、数组和字符串转换为Python字典、列表和字符串。它只读取数据，不像导入Python模块那样执行代码。反向保存声明时，`json.dumps`把这些Python值编码回JSON文本。

同样的声明方式也支持可选路线，但组合不能让两个所有者都负责同一实现。GUI 分支另外提供`.codex/teaching/graphics.json`。没有这份文件时，读取器只读主线清单；有它时，把两份清单的章节、源码归属和终点分别连接成新列表。`set(...)`将路径列表转换为不重复的集合，`&`取得两集合共同包含的路径。两份`owned`有交集，就意味着同一路径被两条路线同时归属，读取器会在创建输出目录前拒绝它。

取得正文路径以后，还不能把其中每个代码块都写盘：演算、错误演示和真正编辑混在同一篇文章中。重建器只处理正文中明确标注的编辑，不执行普通展示。`BLOCK` 识别独占一行的 write、append 或 replace 标记，`FENCE` 取得紧随其后的完整代码围栏；`RUN` 单独收集带 run 标记的 Shell 命令。`re.M` 让行首行尾按每行判断，`re.S` 使点号能收集多行源码；非贪婪的 `.*?` 在下一条合法收尾围栏停下。

编辑标记也可能作为文字出现在演算代码里，所以不能先搜索全篇所有标记。读取器从一行起点前进，遇到普通围栏就整块跳过，只在围栏外解释编辑。没有标 run 的展示命令不会自动执行。

**新建`scripts/course.py`，先写导入与两种围栏模式：**

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


```

**继续追加清单读取。** `source`省略时读本工程；之后的导出器可以传入已经固定的普通资料目录。`None`在这里表示“采用默认位置”，不是一条路径。

<!-- append: scripts/course.py -->
```python
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


```

<a id="t01-parents"></a>
## 先写完父节点，再写当前章

调用`ordered(["02"])`时，先把章节列表整理为按身份查找的`nodes`字典，`nodes["02"]`就是02的整份记录。字典推导式中的`item['id']: item`分别指定键和值；同名键会覆盖前项，所以还要比较字典与原列表的长度，拒绝重复身份。

02 的追加依赖 01 已有文件，01 又依赖 00 创建起点，因此读取当前章之前必须先走完前置。内部函数 `visit` 可以读取外层的 `nodes` 和三个结果容器。进入02后，它先访问01，再访问00；00没有前置，先加入结果，返回01后加入01，最后加入02。结果中依次是00、01、02三份记录。每次递归调用保留自己的`name`，这些调用共享外层创建的`result`、`visiting`和`visited`。

共享前置和依赖环看起来都像“又遇到同一编号”，处理却相反：前者应跳过，后者必须报错。`visiting` 保存尚未返回的身份。例如访问02、01、00的最深处，它包含三个身份；00处理完后从中移除，再进入`visited`。尚未返回就再次进入同一身份，才是依赖环；已经处理完的身份只需跳过。

组合K13和G13时，K13先完成共享K11；G13再走到K11会命中`visited`，于是这次不重复应用K11。排序只保证同一节点处理一次，各图形章节还必须遵守只添加自己文件的约定，才不会改写主线。`result`的元素始终是整份记录，下一步用其中的`document`读取正文。

**追加依赖排序：**

<!-- append: scripts/course.py -->
```python
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

<a id="t01-edits"></a>
## 让一次编辑改变一份完整文件

顺序已得到 00→01→02，接下来要让每章动作改变同一份累计文件。例如 00 首次写 probe.py，后面的 write 可以换整份实现，append 只能延长已经存在的文件。`state` 是“相对文件名 → 完整源码字符串”的字典。write 创建或完整替换一项，append 在已有内容后继续；replace 只改变已有文件中的一个确定窗口。三种动作都来自读者实际看到的编辑，不读取参考工程的终点实现。

精确替换按如下格式书写：独占一行的 `<!-- replace: 相对路径 -->`，下一行是 `<!-- before -->` 和旧源码围栏，再接 `<!-- after -->` 和新源码围栏。旧文本必须含真实源码，而且在当前文件里恰好匹配一次；新文本可以为空，表示删除旧窗口。两块之间只放标签和空行，定位与替换说明写在动作前。代码块最后一个换行也是匹配的一部分，不按行号或忽略空白寻找。

`source_block` 从指定位置读取一份完整围栏，`edits` 则按正文顺序交出动作、路径、旧文本与新文本。replace 缺标签、缺围栏或旧文本只有空白都会失败。看似编辑却拼错格式的标记也报错，避免一个必要修改被静默略过。

同一章可能先成功改一项，再在第二项发现旧文本不匹配。如果直接改 state，失败就会留下半章版本。apply 先浅复制 state 到 pending。字符串不可变，修改副本中的一项不会改原字典。追加和替换都要求文件已经存在；替换先找第一处旧文本，再从下一字符检查是否还有第二处，包括重叠匹配。零处表示起点版本不符，多处表示上下文不足，都应修正文案，不擅自选第一处。整章动作都有效以后才把 pending 交回 state，失败不留下半章编辑。

`Path(name)` 拒绝绝对路径、上级跳转和 `a//b` 一类非规范别名。源码围栏保留真实换行，write/append 的旧空块约定仍保留。摘要函数把完整字符串编码为 UTF-8，再用 SHA-256 的十六进制结果核对。`state_for` 从空字典按章节顺序应用这些动作，只构造内存内容，尚未写磁盘。

**追加编辑动作、摘要和累计状态构造：**

<!-- append: scripts/course.py -->
```python
def source_block(document, position):  # 从指定位置取得一份明确的源码围栏。
    while document.startswith('\n', position):  # 标记与围栏之间允许空行，不吞掉代码内空行。
        position += 1  # 只前进到本次内容的起点。
    match = FENCE.match(document, position)  # 不能越过普通正文去猜下一块源码。
    if not match:  # 缺围栏或未关闭都属于不完整编辑。
        raise RuntimeError('Teaching edit needs a closed source fence')  # 不静默跳过损坏动作。
    return match[1], match.end()  # 非空源码保留末尾换行，空的新文本可用于删除旧窗口。


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


```

<a id="t01-audit"></a>
## 按检查点核对内容，再盘点全部自有文件

核对02时，`state_for(["02"])`重新应用00、01、02，得到当前累计源码。`actual`字典把这些字符串逐份转换为摘要，与02记录中的`files`比较；路径多一项、少一项或内容不同，两个字典都会不相等。正文的run块也要与`commands`列表逐项相同，才能使源码版本与执行命令对应。

逐节点摘要能发现内容不符，却还可能遗漏一份从未分配给章节的新文件。因此还要从整个工程反查归属。整个参考工程先把全部 `endpoints` 组合成最终状态，再盘点磁盘中的自有源码，与最外层`owned`比较。新增文件如果没有分配给任何章节，就会在这一步被发现；最后逐份核对最终状态与当前实现字节。内容相同只说明正文确实可以形成当前源码，尚未判断这些代码的行为和解释是否正确。

普通源码后缀由目录扫描收集；`airios`、Makefile和配置等无常规源码后缀的输入显式列出。生成字体数组由G12的锁与生成配方解释，不作为手写字节盘点。选装GUI未出现时，清单和实际文件集合都不含GUI路径。

**追加盘点与来源核对：**

<!-- append: scripts/course.py -->
```python
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


```

<a id="t01-replay"></a>
## 写入新目录，并保留失败发生的位置

内存 state 核对成立以后，才把它落实到独立目录；学习者正在修改的原目录不是输出目标。`replay` 先算好章节顺序，再检查目标。悬空符号链接指向一个不存在的位置，`exists()`会为它返回False，但链接本身仍存在，所以必须再用`is_symlink()`检查。已有文件、目录和链接都拒绝写入；通过检查后才用`resolve()`固定实际输出位置并创建目录。

每章开始时，`before = state.copy()`保留上一章的字典。两个字典暂时引用相同字符串，但字符串不可变，apply给`state[name]`赋一个新字符串不会改变`before[name]`。于是只要比较两边的值，就能找出本章需要写盘的文件；未变化的文件保留原字节与时间戳。`chmod(0o755)`为airios和Shell脚本恢复执行权限，`0o`表示后面的数字按八进制解释。

不带`--run`时只执行正文编辑；带上它才启动run块。`sh -eu -c`在普通命令失败或读取未定义变量时停止，`if`条件等Shell例外仍然适用。外层600秒截止限制它等待整个命令块的时间，真实QEMU观察器还使用K12自己的较短截止。外层结束等待并不替代每个子工具的进程清理，QEMU仍由观察器的finally负责回收。

文件已写出与命令已成功是两种结果。启用 --run 后，还需为每次真实命令保留结果；不能只留下最终一行 REPLAY。`evidence` 是按执行顺序保存记录的列表，每项包含章节身份、完整命令、退出状态与实际输出。超时的输出可能仍是bytes，即使正常返回指定了text模式；脚本按实际类型解码，把`exit: null`、`timed_out: true`及已收到的输出写入`replay.json`，再报告失败。失败目录留在原处，修正原因后换一个新目录重跑，原目录中的记录和修改继续保留。

**追加实际重建：**

<!-- append: scripts/course.py -->
```python
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


```

前面的函数既供后续导出器调用，也供直接命令使用。最后给直接执行脚本建立入口。`nargs='?'`使目标目录可以省略，省略时只核对参考工程；`action='store_true'`使`--run`成为默认False的布尔选项。导入模块只建立函数，`if __name__ == '__main__'`才启动参数解析。

**追加命令入口：**

<!-- append: scripts/course.py -->
```python
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

<a id="t01-materials"></a>
## 用02验证顺序，再导出串口终点

重建器本身已经写在学习目录，但它读取的正文与清单还需一起接入，否则只有工具却没有输入。先把课程资料复制到学习目录，正文、检查点JSON和说明随后供工具读取，参考工程中的实现目录不参与复制。`AIRI_TUTORIAL_SOURCE`可由环境指定；省略时采用命令中给出的桌面参考项目路径。来源与接收目录相同时跳过复制。

复制`.codex/teaching`时，`ignore_patterns('graphics.json')`生成一个忽略规则，copytree据此跳过来源的完整图形清单，目标中已经保存的图形声明继续保留。复制其他目录时，`ignore=None`表示不设过滤；`dirs_exist_ok=True`允许合入已有资料目录，其中同名资料会更新，手写实现目录不在复制范围内。

只有显式设置`AIRI_TUTORIAL_GRAPHICS=G13`这样的身份，脚本才重写图形声明；变量未设置时，`os.environ.get`返回None，不新增也不推进图形位置。选择G13时，ordered先取得它及前置的记录，随后只把图形记录写入chapters，用最后图形记录的累计文件减去核心前置路径取得owned，endpoints只登记G13。这一步声明已有源码停点，不替人完成G13，也不通过扫描某个文件是否存在来猜进度。

<!-- run -->
```sh
PYTHONPATH=scripts python3 - <<'PYDATA'  # 当前进程可导入刚写好的course，heredoc交入完整程序。
import json  # 可选GUI停点需要写出准确的局部声明。
import os  # 读取显式参考资料位置与可选GUI停点。
from pathlib import Path  # 固定来源和当前学习目录。
import shutil  # 只复制已声明的纯资料。
from course import ordered, plan  # 使用刚写好的依赖闭合规则。
source = Path(os.environ.get('AIRI_TUTORIAL_SOURCE', '/Users/bilibili/Desktop/AiriOS')).resolve()  # 来源不提供成品实现。
target = Path.cwd().resolve()  # 接收者仍是自己的手写工程。
if source != target:  # 避免把同一文件复制到自己。
    for name in ('dev-log', 'docs', '.codex/teaching'):  # 这些目录只提供正文、画像和验证说明。
        ignore = shutil.ignore_patterns('graphics.json') if name == '.codex/teaching' else None  # 不把来源完整G26声明强加到学习停点。
        shutil.copytree(source / name, target / name, dirs_exist_ok=True, ignore=ignore)  # 已有GUI声明保留；不写入实现目录。
    for name in ('README.md', 'AGENTS.md', 'THIRD_PARTY.md', 'LICENSE', 'boot/AiriBoot/README.md', 'labs/README.md', 'labs/linux/p1/README.md'):  # 后续链接检查所需的纯说明。
        origin = source / name  # 不从未声明的位置补文件。
        if origin.is_file():  # 当前资料包没有的可选说明不读取。
            out = target / name  # 同名说明保留正常相对位置。
            out.parent.mkdir(parents=True, exist_ok=True)  # 说明文件也可能位于多层目录。
            shutil.copyfile(origin, out)  # 不复制scripts、kernel或frontend中的终点代码。
choice = os.environ.get('AIRI_TUTORIAL_GRAPHICS')  # 只有实际完成支线时，才显式声明例如G13；默认不选择GUI。
if choice:  # 声明源码停点不代表已经掌握，也不根据文件存在猜进度。
    selected = ordered([choice], plan(source))  # 未知编号或缺少GUI资料会明确报错。
    graphics = [item for item in selected if item['track'] == 'graphics']  # 只取该停点实际闭合到的图形章节。
    core = [item for item in selected if item['track'] != 'graphics']  # 核心前置用于划分路径所有者。
    if not graphics:  # 普通主线编号不能冒充GUI声明。
        raise RuntimeError('AIRI_TUTORIAL_GRAPHICS must name a graphics checkpoint')  # 保留已有手写实现，不替读者猜测。
    own = sorted(set(graphics[-1]['files']) - set(core[-1]['files']))  # 支线只拥有核心前置之外的源码路径。
    declaration = {'schema': 3, 'chapters': graphics, 'owned': own, 'endpoints': [choice]}  # 清单只描述显式选择的支线终点。
    (target / '.codex/teaching/graphics.json').write_text(json.dumps(declaration, ensure_ascii=False, indent=2) + '\n')  # 元数据与本次停点一致。
PYDATA
PYTHONPATH=scripts python3 - <<'PYORDER'  # 从自己刚写好的模块读取实际排序结果。
import tempfile  # 每次观察使用独立输出目录。
from pathlib import Path  # 构造尚不存在的work子目录。
from course import ordered, replay  # 使用同一正式实现。
print([item['id'] for item in ordered(['02'])])  # 实际输出00、01、02。
with tempfile.TemporaryDirectory() as directory:  # 这个只读对照观察不保存个人修改。
    replay(Path(directory) / 'work', 'K13')  # 从正文写出主线，不执行其Shell块。
PYORDER
```

第一行列表应为`['00', '01', '02']`，随后逐章输出REPLAY，最后停在K13。这里验证的是排序与完整编辑；没有再次启动guest，也没有运行Linux实验。下一节将把这份重建结果连到固定Git来源，并保护可长期保留的完整快照。
