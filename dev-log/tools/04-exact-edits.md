# T04 · 只替换已确认的源码窗口

T01 已经写出的编辑器支持 write、append 和精确 replace，T03 又把它接进学习命令。现在继续核对这项合同：调度器只改一个等待分支时，旧窗口怎样确定位置；如果它出现两次，或者同章下一项失败，已有状态又会怎样。本章新增独立观察程序，使用已有解析器，不重复写入相同的 course.py。

从 T03 的同一目录继续，保留已经完成的 scripts/course.py。下面只新增 tests/course_edits_test.py；内核、用户程序和旧检查点保持。旧章节的 write/append 与已经使用的 replace 都继续有效。定位失败时应核对实际起点，不能选择最相近的函数继续。

<a id="exact-window-model"></a>
## 一份旧文本怎样确定修改位置

假设内存中的文件 x.c 是两行 `one`、`two`，每行各有末尾换行。替换动作中的 before 保存 `one` 及其换行，after 保存 `new`、一个空行。替换成功后，文件成为 `new`、空行、`two`。after 中的空行是新文件内容；标签和围栏只描述编辑，不写入文件。

这项定位依赖旧文本，而不是行号。前章插入其他函数以后，只要这段完整文本仍唯一，编辑就有确定位置；如果旧窗口消失或出现两次，就无法判断应修改哪里，整章停止。旧窗口包含换行和旁注，空白不能先被清理，否则检查的便不是跟敲留下的那份源码。只有空白的 before 不能提供有意义的位置，因此拒绝；空 after 则表示确实删除这个窗口。

协议按下面的形状书写。此围栏是协议示例，不是需要新建的 x.c：

```text
<!-- replace: x.c -->
<!-- before -->
```

before 标签后紧接一份完整源码围栏，接着是 after 标签及新源码围栏。编辑器不在普通正文中搜下一块代码来凑配对。已有实现把每份围栏读成完整字符串，再按正文次序应用。下面沿它的返回值接到新增测试。

<a id="exact-edit-parser"></a>
## 先找到围栏，再把它交给编辑动作

先看 T01 已有的匹配器。BLOCK 只识别当前行的动作开头；FENCE 从指定位置读到独占一行的关闭围栏。S 让点号跨行，M 让行首锚点作用于每一行，二者保留代码内部的末尾换行。依赖排序、摘要、导出和命令入口继续使用原实现；这里依次跟踪读取围栏、形成编辑和暂存应用，不把展示片段再追加到文件。

下面摘出已完成解析器的相应部分，仅用于跟踪，不写入文件：

```python
BLOCK = re.compile(r'<!-- (write|append|replace): ([^\n]+) -->\n')  # 只识别独占一行的编辑起始标记。
FENCE = re.compile(r'^```[^\n]*\n(.*?)^```[ \t]*(?:\n|$)', re.S | re.M)  # 完整围栏包括代码的末尾换行。
RUN = re.compile(r'<!-- run -->\n```sh\n(.*?)\n```', re.S)  # 只执行显式标注的命令，忽略展示输出。
```

一次 `source_block(document, position)` 从标记结束位置开始。允许跳过标记外的空行；进入围栏以后，空行属于源码，不再跳过。match 必须从当前位置匹配，因而普通正文不能被无声越过。返回的两项分别是源码字符串、围栏结束后的新位置，后者让调用方继续读取同一次编辑。

例如刚才的 before 读出 `one` 及换行后，position 已位于旧围栏之后。编辑解析器接下来必须在这里找到 after 标签，才能读取新文本。围栏未闭合时没有完整编辑，不能先交出前半段。

下面摘出已完成解析器的相应部分，仅用于跟踪，不写入文件：

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

下面摘出已完成解析器的相应部分，仅用于跟踪，不写入文件：

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

解析器交出的 replace 最终作用于一张“路径→完整字符串”的字典。apply 先复制这张字典为 pending：字符串不可变，后续连接和替换产生新的字符串，不会修改原值。只有整章动作都有效，才清空外部 state 并交入 pending；第二项失败时，第一项临时写入的另一个文件也不会残留。

沿 x.c 的两行继续。find 找到 before 的第一处位置，然后从下一字符再查一次。第二次不能从旧窗口末尾开始，否则会漏掉相互重叠的两处，例如三行 a 中的连续两行 a。唯一性成立后，replace 只改一处。路径仍按原规则检查，append/replace 都要求已有文件；解析器不会到参考工程磁盘里寻找缺少的起点。

apply 的关键交接如下。摘要、审查、导出和命令入口不需要再次写入。write/append 的空围栏产生一个换行，replace 的空 after 则确实删除；两种合同仍分别保留。

下面摘出已完成解析器的相应部分，仅用于跟踪，不写入文件：

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

实际运行完成后应看到九项测试通过，结尾显示 OK。把“缺失旧窗口”的场景代入 apply：pending 已有临时 new.c，第二项 find 返回 -1，异常沿调用链返回，state.clear 尚未执行，因此原状态完整保留。测试继续读取 state 时，new.c 不存在。这是整章失败不发布的行为，不表示已经写入的磁盘目录可以任意覆盖；T02 对新导出目录和已编辑 HISTORY 的保护仍保持。

K53 使用同一 before/after 合同 在既有调度器中增加内存等待。普通旧章节的源码字节与运行命令不因编辑器升级改变；旁注检查只把 after 当作新的待写源码，before 保留原字节供定位，不能单独批量对齐其中一侧。
