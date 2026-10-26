# T03 · 核对参考工程，并分别记录路线位置

T01写出了重建器，T02保护了导出和完整快照。现在把它们接到已经存在的`./airios`入口：你可以只列主线、读取一章、在新目录复现，或者分别记录内核与GUI停留的位置。记录位置只保存你的选择，不根据运行成功推定掌握，也不会改写自己的源码。

<a id="tool-navigation"></a>
## 先核对内容，再核对跳转位置

检查应回答两个不同问题：正文能否形成声明的源码，读者能否沿链接到达解释位置。先验证前者，避免在版本已经不符时继续给出看似正常的链接结果。从 T01 保存的课程资料出发，内容检查先重建每个节点，再与当前自有文件比较；通过以后才检查文档里的本地文件链接和画像中的显式锚点。开始时先删除上一次`artifacts/docs-check.json`：如果本轮重建就失败，检查会立即退出，终端给出节点或文件，新报告还没有生成，旧成功结果也不会残留。

这个检查在自己的Git目录或普通源码包中都可以运行，它不核对个人提交历史。参考工程的分支与教学身份另由T02的历史读取器验证。

来源已核对以后，再把正文当作导航材料读取；源码中的类似路径只是字符串，不能拿来判断文档链接。链接检查先用 `re.sub` 去掉源码围栏，避免把源码内的字符串当成Markdown。`re.S | re.M`组合两个标志：点号可以跨行，`^/$`同时按每行的起止位置匹配。链接正则只有一个捕获组，findall因此返回目标字符串的列表。目标去掉`#`后的片段，再从当前文档目录定位文件；页内链接和带协议的外部地址跳过，不访问网络，也不检查普通Markdown标题的页内锚点。

画像中的`at`有更具体的检查。`partition('#')`把引用拆成文件名、分隔符和锚点三项，再到对应文件中寻找`id="锚点"`这一显式标记。这只能确认解释位置存在，解释是否建立了所需前提仍由正文阅读判断。

**新建`scripts/docs_check.py`：** 先调用T01的audit，再检查本地链接与画像位置。

<!-- write: scripts/docs_check.py -->
```python
"""Check source reconstruction and links; neither is a readability score."""  # 核对重建和链接，结果不能当作读者已经理解的证据
import json  # 读写检查点清单和实际操作记录中的嵌套对象
from pathlib import Path  # 拼接工程路径，检查相对路径并读写实际文件
import re  # 按本教程标记提取代码、命令或Markdown链接。
import subprocess  # 识别Git读取失败的CalledProcessError，原因进入本次报告。
from tools import ROOT    # 工程根位置来自此模块源码，不由调用时工作目录决定
from course import audit  # 先用教程重建核对源码，再检查其他文档入口


def check_docs():  # 先核对代码重建，再检查普通正文中的本地文件链接
    output = ROOT / 'artifacts/docs-check.json'  # 这个文件只允许表示本轮内容检查结果。
    output.parent.mkdir(parents=True, exist_ok=True)  # 第一次检查也先建立证据目录。
    output.unlink(missing_ok=True)  # audit若抛错，不能留下旧成功报告。
    data = audit()  # 先核对参考教程与实现同版，再使用其内容重建
    errors = []     # 累计实际发现的缺文件链接或提交路线错误
    links = 0  # 只计需要核对的本地链接，网络地址不计入此数
    documents = list((ROOT / 'dev-log').rglob('*.md'))  # 先收集全部教程 Markdown 路径，list 固定本次扫描集合
    documents += list((ROOT / 'docs').glob('*.md'))     # 追加项目说明文档，一并检查其中跳转
    documents += [ROOT / 'README.md']  # 将根入口说明也纳入同一链接检查
    for document in documents:  # 逐篇读取实际正文，链接按各自父目录解释
        prose = re.sub(r'^```[^\n]*\n.*?^```[ \t]*$', '', document.read_text(), flags=re.S | re.M)  # 去掉行首围栏包围的代码，S 跨行、M 让行首行尾逐行生效
        for destination in re.findall(r'!?\[[^\]\n]*\]\(([^)\n]+)\)', prose):  # 只收集当前简单 Markdown 链接括号中的目标字符串
            destination = destination.strip('<>').split('#')[0]  # 去尖括号包裹和页内片段，剩下文件位置供 exists 检查
            if not destination or re.match(r'[a-zA-Z][\w+.-]*:', destination):  # 页内空路径或带协议地址无需在本地文件系统寻找
                continue  # 该目标是页内定位或外部协议链接，不作本地文件存在检查
            links += 1  # 计入一条实际需要检查的本地路径
            if not (document.parent / destination).resolve().exists():  # 从此文档所在目录拼接并求实际位置，确认目标文件存在
                errors.append(f'{document.relative_to(ROOT)}: missing {destination}')  # 保存起点文档和缺失路径，便于修正准确的跳转
    for chapter in data['chapters']:  # 每个发布节点都须有自己的读者上下文。
        reader = ROOT / chapter['reader']  # 清单直接指定真实资料，不从旧编号猜路径。
        if not reader.exists():  # 缺资料先记入同一报告。
            errors.append(f'{chapter["id"]}: missing reader context')  # 不以源码通过替代知识前提。
            continue  # 无法继续检查此文件的引用。
        for reference in re.findall(r'^\s*at:\s+([^\s]+)', reader.read_text(), re.M):  # 这些字段只保存简单本地路径。
            name, _, anchor = reference.partition('#')  # 文件位置与具体解释锚点分别取得。
            source = (reader.parent / name).resolve()   # 引用相对读者文件所在目录解释。
            if not source.exists() or (anchor and f'id="{anchor}"' not in source.read_text()):  # 明确锚点必须仍在实际正文。
                errors.append(f'{chapter["id"]}: reader reference missing: {reference}')  # 只验证位置，语义仍须阅读。
    report = {'checkpoints': len(data['chapters']), 'owned_files': len(data['owned']),  # 报告记录本次章节数量和自有源码数量
              'local_links': links, 'errors': errors,  # 同时保存已检查链接数及全部实际错误
              'limit': 'Content reconstruction is not proof of explanation quality.'}  # 报告明示内容相同不等于解释质量或真实读者理解
    output = ROOT / 'artifacts/docs-check.json'  # 报告写在运行产物目录，不混入自有源码
    output.parent.mkdir(exist_ok=True)  # 确保报告父目录存在，已有目录继续使用
    output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')  # 保持中文文字可读，以两格缩进保存 JSON 报告
    if errors:  # 有实际错误时在保存报告之后失败
        raise RuntimeError('\n'.join(errors))  # 每项错误占一行交回命令入口，不隐藏后续发现
    print(f'PASS documentation links: {links}')  # 内容与路径检查均通过后报告本地链接数
    return report  # 把完整检查结果交给调用方


if __name__ == '__main__':  # 直接执行此文件才进入命令入口，作为模块导入只建立定义
    check_docs()  # 直接运行此模块时执行文档检查
```

来源核对完成后，链接和画像错误先累积到`errors`列表，写入本轮JSON，再以非零状态退出；全部通过才打印PASS。来源不符时没有新JSON，链接错误时则有包含errors的新JSON，这两种失败位置要分别看待。

<a id="tool-progress"></a>
## 只更新一条路线的停留点

现在把同一份章节声明交给实际命令。`checkpoints()` 形成“身份 → 章节记录”的字典，后面的动作都从这里选中同一记录，再决定怎样使用：

| 动作 | 记录怎样被消费 | 结果留在哪里 |
| --- | --- | --- |
| list | 按 track 过滤记录 | 终端列表，不改变学习源码 |
| show | 用身份找到 document 并读取正文 | 终端中的教程内容 |
| check | 调用 T01 的 replay 并启用 run | mkdtemp 创建的新目录；日志与失败现场不会自动清除 |
| export | 调用 T02 固定来源并重建 | 指定的新目录，不执行章节命令 |

它们共享编号，并不共享一种副作用。接下来的 progress 又只保存位置，不调用 replay 或 export。

从空进度开始，执行`./airios learn progress --record 02`后，字典成为`{"core": "02"}`；再执行`./airios learn progress --record L01`，只增加`"linux": "L01"`，core仍是02。`nodes[args.record]['track']`给出要更新的键，`args.record`给出新值。安装图形清单后，G13同样只更新graphics。不带 `--record` 时只读取并显示 `artifacts/learning-progress.json`。这个文件是命令的输出，不是课程前置或编译输入；下方 route_test 用临时 ROOT 实际检查两条路线先后记录，已有个人位置不会被测试覆盖。

记录到 02 只更新位置文件，不会创建一次 Git 提交。另一个 current 导出选择的则是实际提交树：HEAD 表示 Git 当前检出的提交位置。current取得这份提交的整个文件树，先要求没有未提交或未跟踪的改动，但不要求个人提交具有教学尾注；普通课程身份则按前置组合重建源码。前者需要Git，后者使用T02已经说明的固定来源规则。`current`不接受alongside，因为它记录的是一棵准确的提交树，不能同时声称还加入了另一份路线内容。

argparse的subparser为list、show、check、export等动作分别登记合法参数，动作名写入`args.action`。choices接收编号字典时检查它的键；`[*nodes, 'current']`把这些身份展开成一个列表，再加上current。`LABS['memmove']`保存`('lab-memmove', 'labs/README.md')`，分别是Make目标与观察说明，后面的`target, guide = ...`将两项取出。宿主实验把两路输出合入stdout并保存，即使Make失败也先保留日志，再交回非零状态。

**新建`scripts/learning.py`：**

<!-- write: scripts/learning.py -->
```python
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
```

<a id="tool-failures"></a>
## 用失败输入检查目录保护与进程回收

工具的成功路径能生成文件，仍不足以证明它拒绝覆盖个人编辑；需要把真实旧文件放在拒绝分支中检查。已有目录、悬空链接、依赖循环和超时都必须返回失败，而且失败之后原文件还应存在。下面的标准库测试程序完整构造这些输入并调用正式实现。`Routes`继承`unittest.TestCase`，运行器按`test_`方法名找到测试，每个方法的`self`指向本次测试实例；`self.assertEqual`等方法把实际结果与预期比较，失败会使整条命令返回非零。

`with self.assertRaisesRegex(RuntimeError, 'edited')`要求块内抛出RuntimeError，且错误文字匹配edited；不抛错、类型不同或文字不匹配都会使测试失败。`patch.object(learning, 'ROOT', 临时路径)`只在with期间替换learning模块的属性，离开后恢复；course模块仍读取真实课程资料，所以进度测试只改临时保存位置，不另造一套章节清单。

重建保护先由真实工具写出临时文件，再将其中一份改成`learner edit`，重复导出必须被拒绝，文件也必须仍保存这次修改。启动失败观察则把boot模块的QEMU参数换成两个真实Python子进程：`pass`立即正常退出，却没有启动标记；`sleep(10)`继续存在，但在0.3秒上限内也没有标记。等待器应分别报提前退出与超时，并进入同一套进程回收。

这些子进程观察检查主机等待工具，UART启动仍需要真实guest记录。未安装GUI时，组合用例使用`skipTest`明确报告跳过；进度独立性则用core与linux两条路线检查，不要求先写图形代码。

**新建`scripts/route_test.py`：** 所有写入都在标准库临时目录中，不操作个人学习目录。

<!-- write: scripts/route_test.py -->
```python
"""Check route composition and protection; no learner score is produced."""  # 作者检查依赖边界与文件保护。
import json  # 为隔离夹具保存清单。
from pathlib import Path  # 构造独立临时路径。
import tempfile  # 每项保护检查拥有新目录。
import unittest  # 断言失败使命令返回非零。
from unittest.mock import patch  # 只替换夹具来源和 QEMU 参数，不冒充设备成功。
import sys     # 本脚本由 scripts 目录执行，可直接导入同目录模块。
import course  # 使用实际正文重建器。
import boot    # 故障检查调用真实等待和进程回收逻辑。


class Routes(unittest.TestCase):  # 这些是作者工具检查，不是学习者答题。
    def test_core_has_no_graphics_source(self):  # 当前串口终点完全排除前端实现。
        state = course.state_for(['K13'])  # 只沿主线依赖重建。
        self.assertFalse(any(name.startswith('frontend/') for name in state))  # 不隐藏成品图形源码。
        self.assertNotIn('scripts/ui_preview.py', state)     # 默认命令也不依赖字体工具。
        self.assertNotIn('kernel/console/display.c', state)  # 链接使用串口实现。

    def test_every_graphics_stop_preserves_core(self):  # 任一已发布图形停点不改写主线文件。
        base = course.state_for(['K13'])  # 固定主线终点的全部源码字节。
        graphics = [item for item in course.plan()['chapters'] if item['track'] == 'graphics']  # 只在实际合并GUI后验证其全部停点。
        if not graphics:  # 纯主线没有图形实现可运行。
            self.skipTest('GUI branch is not installed')  # 明确记录未运行，不能用空循环冒充通过。
        for item in graphics:  # 逐项覆盖而非抽查两个停点。
            if item['track'] == 'graphics':  # 图形集合的全部节点参与组合检查。
                combined = course.state_for(['K13', item['id']])  # 共用基础只应用一次。
                for name, content in base.items():  # 所有主线文件都必须保持相同内容。
                    self.assertEqual(combined[name], content, (item['id'], name))  # 失败准确定位支线节点与文件。

    def test_dependency_cycle_and_unknown(self):  # 错误资料不能导致无限递归或静默回退。
        data = {'chapters': [{'id': 'A', 'requires': ['B']}, {'id': 'B', 'requires': ['A']}]}  # 最小循环夹具。
        with self.assertRaisesRegex(RuntimeError, 'cycle'):  # 必须走明确错误出口。
            course.ordered(['A'], data)  # 只测试排序，不写文件。
        with self.assertRaisesRegex(RuntimeError, 'Unknown'):  # 拼错编号也是失败。
            course.ordered(['missing'], data)  # 不选择任何默认版本。

    def test_duplicate_identity(self):  # 重复编号不能默默覆盖第一项。
        data = {'chapters': [{'id': 'A', 'requires': []}, {'id': 'A', 'requires': []}]}  # 两份声明占用同一身份。
        with self.assertRaisesRegex(RuntimeError, 'Duplicate'):  # 在排序前拒绝。
            course.ordered(['A'], data)  # 不生成重复快照。

    def test_route_export_outside_source(self):  # 防止将docs复制到自己的子目录。
        import snapshots  # 使用真实导出入口的位置保护。
        with tempfile.TemporaryDirectory() as directory:  # 夹具不触碰真实工程。
            with patch.object(snapshots, 'ROOT', Path(directory)):  # 让这个新目录代表来源。
                with self.assertRaisesRegex(RuntimeError, 'outside'):  # 必须在写入前拒绝。
                    snapshots.export_route('02', Path(directory) / 'docs/nested')  # 不进入递归复制。

    def test_edited_snapshot_is_preserved(self):  # 被编辑的生成目录不能当缓存覆盖。
        import snapshots  # 使用正式的摘要和身份检查。
        with tempfile.TemporaryDirectory() as directory:  # 不修改真实HISTORY。
            target = Path(directory)      # 夹具只保存一份普通文件与索引。
            file = target / 'source.txt'  # 这份文字代表原生成内容。
            file.write_text('original\n')  # 建立已知初态。
            record = {'generator': snapshots.GENERATOR, 'files': {'source.txt': course.digest('original\n')}}  # 索引自身不参与摘要。
            (target / 'index.json').write_text(json.dumps(record))  # 写入有效生成身份。
            snapshots.check_previous(target)   # 未修改时应允许后续生成。
            file.write_text('learner edit\n')  # 模拟学习者修改。
            with self.assertRaisesRegex(RuntimeError, 'edited'):  # 检查必须拒绝覆盖。
                snapshots.check_previous(target)  # 只读原目录，错误保留具体原因。
            self.assertEqual(file.read_text(), 'learner edit\n')  # 修改内容保持原样。

    def test_progress_tracks_are_independent(self):  # 图形记录不改写内核停留点。
        import learning  # 调用实际命令分派，不另造进度更新算法。
        with tempfile.TemporaryDirectory() as directory:  # 个人记录只写临时目录。
            with patch.object(learning, 'ROOT', Path(directory)):  # 课程资料仍由真实清单读取。
                learning.main(['progress', '--record', '02'])  # 明确记录内核位置。
                second = 'G13' if 'G13' in learning.checkpoints() else 'L01'  # 主线可用Linux记录验证两条路线独立。
                track = learning.checkpoints()[second]['track']  # 期待值来自该身份的实际路线。
                learning.main(['progress', '--record', second])  # 单独更新图形位置。
                value = json.loads((Path(directory) / 'artifacts/learning-progress.json').read_text())  # 读取实际磁盘结果。
                self.assertEqual(value, {'core': '02', track: second})  # 两项都保留，不产生掌握评分。

    def test_write_boundaries(self):  # 教程编辑动作不能越过导出目录。
        for name in ('../outside.py', '/tmp/outside.py', './a.py', 'a//b.py', '.'):  # 上级跳转与绝对路径分别拒绝。
            with self.assertRaisesRegex(RuntimeError, 'Unsafe'):  # 错误需在写入前发生。
                course.apply(f'<!-- write: {name} -->\n```python\nx=1\n```', {})  # 使用实际块解析器。
        with self.assertRaisesRegex(RuntimeError, 'Append'):  # 缺少文件起点时不能直接追加。
            course.apply('<!-- append: missing.py -->\n```python\nx=1\n```', {})  # 发现漏写前章。

    def test_export_preserves_existing_and_broken_link(self):  # 学习者目录与断链都保持原样。
        with tempfile.TemporaryDirectory() as directory:  # 全部副作用限于夹具目录。
            target = Path(directory) / 'work'  # 第一次导出使用新路径。
            course.replay(target, '02')  # 导出真实 day2 文件。
            file = target / 'scripts/probe.py'  # 模拟学习者修改一份源码。
            file.write_text('learner edit\n')   # 不修改参考工程。
            with self.assertRaisesRegex(RuntimeError, 'never overwrites'):  # 重复导出必须拒绝。
                course.replay(target, '02')  # 不接受隐式覆盖。
            self.assertEqual(file.read_text(), 'learner edit\n')  # 已有修改确实保留。
            broken = Path(directory) / 'broken'  # 断链也属于用户已创建的对象。
            broken.symlink_to(Path(directory) / 'absent')  # 不访问任何真实目录。
            with self.assertRaisesRegex(RuntimeError, 'never overwrites'):  # resolve 之后仍须识别原路径是链接。
                course.replay(broken, '02')  # 不沿断链创建隐藏目标。

    def test_missing_marker_and_timeout_fail(self):  # 真实子进程失败不能冒充启动通过。
        with tempfile.TemporaryDirectory() as directory:  # 启动证据写入隔离目录。
            for program in ('pass', 'import time; time.sleep(10)'):  # 分别模拟提前退出与不产生标记。
                with patch.object(boot, 'ROOT', Path(directory)), patch.object(boot, 'command', return_value=[sys.executable, '-c', program]):  # 只替换硬件启动参数，等待逻辑保持原样。
                    with self.assertRaisesRegex(RuntimeError, 'exited|timeout'):  # 两种失败都要求非成功路径。
                        boot.check('normal', timeout=.3)  # 超时仍执行实际进程终止和回收。


if __name__ == '__main__':  # 作者显式运行才启动这些检查。
    unittest.main()  # 全部断言结果交给进程退出码。
```

<a id="tool-diff-coverage"></a>
## 给后续维护留下固定的变更范围

内容摘要指出最终字节不同，但维护者还需要知道哪些变更应有解释。差异覆盖工具提供固定范围与位置，不能替代正文正确性审查。后续修改 `scripts/course.py` 时，除了重建终点，还需要找出两份提交之间哪些说明需要同步。差异工具把base与target解析成准确SHA，再读取这两个端点之间的diff。`--no-renames`使移动按原路径删除与新路径增加呈现；`-z`使路径用NUL分隔，文件名中的空格和换行不会被拆成不同项。过滤器按源码后缀和明确文件名选择本次要记录的文件。

diff中的每个`@@ ... @@`开头是一块变更及其邻近上下文。`finditer`返回这些块头的匹配对象，`match.start()`给出它在diff字符串中的字符偏移；切片从当前块头取到下一个块头之前，最后一块取到字符串末尾。没有这种块头的文件差异仍作为一项保存，不从清单丢掉。

每项记录包含文件路径、块头、行数和整个差异块的摘要。身份由本次名称与两级序号组成，例如名称p2下的`p2-001-002`表示本次第一个入选文件的第二块。base设为EMPTY时，起点是空文件树；它不是普通提交，重算时不能把它交给要求提交对象的解析器。

正文用`<!-- covers: 块身份 -->`把差异项定位到说明。audit先按同一SHA重算完整清单，拒绝被删改的输入，再收集各Markdown文件中的covers位置。`found.setdefault(key, [])`取得这个身份已有的路径列表，首次出现时先创建空列表，随后append当前文档路径。一个身份没有位置就是missing，出现多次就是duplicate；标记只能证明位置对应，说明是否正确仍需结合变更阅读。新增特殊脚本还要进入T01的完整文件盘点，生成资产继续保留配方与许可。

**新建`scripts/tutorial_coverage.py`：** inventory生成固定差异清单，audit核对这份清单与正文标记。

<!-- write: scripts/tutorial_coverage.py -->
```python
"""Read-only snapshot diff inventory and tutorial coverage audit (Python stdlib)."""  # 从固定 Git 对象读取差异，不修改源码和提交
import argparse  # 把作者或学习者的命令行选择转为具名参数
import hashlib   # 为源码、差异块或归档字节计算 SHA-256 摘要
import json      # 读写检查点清单和实际操作记录中的嵌套对象
from pathlib import Path  # 拼接工程路径，检查相对路径并读写实际文件
import re  # 按本教程标记提取代码、命令或 Markdown 链接
import subprocess  # 运行 Git、Make 或教程命令并读取其退出状态

CODE_SUFFIXES = {'.c', '.h', '.cpp', '.hpp', '.cc', '.S', '.s', '.m', '.mm', '.py', '.sh', '.ld',  # 差异清单仅选择这些源码后缀，不能据此代替完整自有文件盘点
                 '.json', '.toml', '.yaml', '.yml', '.conf', '.java', '.js', '.ts', '.tsx', '.jsx',  # 继续列出配置与前端语言后缀，集合成员不要求有固定次序
                 '.rs', '.go', '.cu', '.cuh', '.proto', '.css', '.html', '.vue', '.sql', '.ipp', '.tpp', '.cmake'}  # 补充其他可能需要记录差异的语言和模板后缀
CODE_NAMES = {'Makefile', 'CMakeLists.txt', 'airios', '.gitignore', '.clangd', '.clang-format'}  # 无后缀或特殊配置文件以完整名字补入识别范围


def git(repo, *args, data=None):  # repo 指定仓库，星号收集 Git 参数，data 仅作具名输入字节参数
    return subprocess.check_output(['git', '-C', str(repo), *args], input=data)  # -C 选择仓库，data 交给子进程 stdin，返回原始输出 bytes


def revision(repo, ref):  # 把外部给定分支或修订名解析成准确提交 SHA
    return git(repo, 'rev-parse', ref + '^{commit}').decode().strip()  # 要求对象能解析为提交，解码 Git 输出并去掉换行


def inventory(repo, base, target, name):  # 固定两个比较端点，为每个变更块生成内容身份记录
    if base == 'EMPTY':  # 从零构建时起点没有文件，使用 Git 的空目录树对象
        before = git(repo, 'hash-object', '-t', 'tree', '--stdin', data=b'').decode().strip()  # 空 bytes 给 stdin，取得空 tree 的 SHA
    else:  # 已有版本作为起点时必须解析成提交
        before = revision(repo, base)  # 固定起点提交，不继续使用可变修订名
    after = revision(repo, target)  # 将目标修订也固定为提交 SHA，之后不再依赖分支可变指向
    paths = git(repo, 'diff', '--no-renames', '--name-only', '-z', before, after).decode().split('\0')  # 路径列表以 NUL 分隔，避免空格或换行被当作普通名字分界
    records = []  # 按路径和块次序保存实际变更记录
    for file_index, path in enumerate(p for p in paths if p and  # enumerate 从零编号，只继续处理非空且属于代码范围的路径
                                      (Path(p).suffix in CODE_SUFFIXES or Path(p).name in CODE_NAMES)):  # 后缀或完整文件名符合一项，就纳入此次差异盘点
        patch = git(repo, 'diff', '--no-ext-diff', '--no-textconv', '--no-color', '--no-renames', '--unified=3', before, after, '--', path).decode()  # 禁用外部转换、着色和重命名推断，用三行上下文取得本文件差异
        starts = list(re.finditer(r'^@@ .*?@@[^\n]*\n', patch, re.M))  # finditer 给每个 hunk 头的匹配对象，M 允许逐行识别开头 @@
        hunks = []  # 每项将保存一个完整差异块文本
        for index, match in enumerate(starts):  # 匹配对象和从零编号共同定位当前块
            end = starts[index + 1].start() if index + 1 < len(starts) else len(patch)  # 下一头位置或整份差异末尾是右端
            hunks.append(patch[match.start():end])  # 字符串切片复制当前完整块，不含下一块头
        if not hunks:  # 某些差异没有 @@ 文本块头，仍不能从清单丢掉文件
            hunks = [patch]  # 将整份文件差异保留为一个记录
        for hunk_index, hunk in enumerate(hunks):  # 为本文件的每个实际块从零编号
            records.append({'id': f'{name}-{file_index+1:03}-{hunk_index+1:03}', 'path': path,  # 生成名加一后索引组成稳定标识，03 用三位补零十进制
                            'header': hunk.splitlines()[0] if hunk else '',  # 保存首行块头，空差异时使用空文字
                            'sha256': hashlib.sha256(hunk.encode()).hexdigest(),  # 为整个块文本的 UTF-8 字节计算摘要，不只核对行数
                            'lines': len(hunk.splitlines())})  # 记录块的物理行数，供作者定位而非评分
    return {'schema': 1, 'name': name, 'base': before, 'target': after, 'items': records}  # 交回格式版本、清单名称、固定端点及全部块记录


def audit(repo, manifest, documents):  # 重算固定差异，随后核对每个块的正文覆盖标记
    # Recompute the entire immutable diff; changed inventory cannot silently omit a file/hunk.
    empty_tree = git(repo, 'hash-object', '-t', 'tree', '--stdin', data=b'').decode().strip()  # 求出本仓库使用的空目录树对象身份
    if manifest['base'] == empty_tree:  # 空树不是普通提交，不能走 revision 的提交解析
        fresh = inventory(repo, 'EMPTY', manifest['target'], manifest['name'])  # 明确用从零分支重算原清单
    else:  # 其余起点已经是提交 SHA
        fresh = inventory(repo, manifest['base'], manifest['target'], manifest['name'])  # 以同一固定端点重算全部块
    if fresh != manifest:  # 来源或记录被删改都会与重新计算的清单不同
        raise ValueError('Inventory differs from the immutable source diff')  # 不接受未经固定差异支持的覆盖清单
    found = {}  # 标记字典保存“块 id -> 出现该标记的文档路径列表”
    for path in sorted(documents.rglob('*.md')):  # 递归按路径次序扫描指定文档目录中的 Markdown
        for key in re.findall(r'<!-- covers: ([\w.-]+) -->', path.read_text()):  # 正则只提取 covers 注释中允许的块标识字符
            found.setdefault(key, []).append(str(path))  # 首次遇键先放空列表，之后把本次文档位置追加进去
    missing = [item['id'] for item in manifest['items'] if not found.get(item['id'])]  # 清单中没有任何正文标记对应的块列为遗漏
    duplicate = [item['id'] for item in manifest['items'] if len(found.get(item['id'], [])) > 1]  # 一个块在多处声明则列为重复，缺键用空列表安全计数
    if missing or duplicate:  # 有漏块或重复定位时覆盖检查失败
        raise ValueError(f'Missing coverage: {missing}\nDuplicate claims: {duplicate}')  # 同时显示两类问题，不把标记数量作为质量结论
    print(f'PASS coverage/{manifest["name"]}: {len(manifest["items"])} immutable changed blocks')  # 全部来源与定位一致才报告此次变更块数


def main():  # 建立此模块直接运行入口，解析参数后进入对应动作
    parser = argparse.ArgumentParser()  # 建立本模块命令行解析器，后面的声明决定合法输入
    commands = parser.add_subparsers(dest='command', required=True)  # 必须选择 inventory 或 audit，名称保存在 args.command
    create = commands.add_parser('inventory')  # 为生成固定差异清单建立参数分支
    create.add_argument('--repo', type=Path, required=True)  # 仓库参数必须给出，并转换成 Path 对象
    create.add_argument('--base', required=True)    # 起点为修订名或 EMPTY，保持原字符串供 inventory 处理
    create.add_argument('--target', required=True)  # 终点修订必须给出，随后解析为准确提交
    create.add_argument('--name', required=True)    # 清单名参与每个块的稳定 id
    create.add_argument('--output', type=Path, required=True)  # 输出位置转换为 Path，由创建分支写入
    verify = commands.add_parser('audit')  # 为重算来源并核对正文标记建立独立分支
    verify.add_argument('--repo', type=Path, required=True)  # 核对仍需指定拥有对应 Git 对象的仓库
    verify.add_argument('--inventory', type=Path, required=True)  # 指定待核对的清单文件，不从终点源码猜差异
    verify.add_argument('--docs', type=Path, required=True)  # 指定需要扫描 covers 标记的文档根目录
    args = parser.parse_args()  # 读取真实 argv，并按本模块声明形成具名参数对象
    if args.command == 'inventory':  # 生成分支按已解析端点写入新清单
        record = inventory(args.repo, args.base, args.target, args.name)  # 用同一次参数建立固定来源记录
        args.output.parent.mkdir(parents=True, exist_ok=True)  # 输出清单的父目录可多层创建，已有目录继续使用
        args.output.write_text(json.dumps(record, ensure_ascii=False, indent=2) + '\n')  # 以可读缩进保存完整清单，不只保存统计数字
        print(f'INVENTORY {record["name"]}: {len(record["items"])} changed blocks')  # 清单写完后报告生成名和实际块数
    else:  # 生成分支已排除，剩下的合法选择为清单核对
        audit(args.repo, json.loads(args.inventory.read_text()), args.docs)  # 读取给定清单并在同仓库重算，再扫描指定文档标记


if __name__ == '__main__':  # 直接执行此文件才进入命令入口，作为模块导入只建立定义
    try:  # 将可能失败的生成或目录交接操作放入对应异常恢复范围
        main()  # 执行本模块参数解析和动作选择，异常由相应外层处理
    except (ValueError, subprocess.CalledProcessError) as error:  # 来源验证或 Git 子进程失败统一作为命令失败
        raise SystemExit(str(error))  # 字符串退出向标准错误显示实际原因，进程返回非零
```

## 在当前实现上核对，并观察路线记录

内容、导航和位置已经各自有了消费者。最后先运行作者工具自身的观察，再从同一入口核对已安装路线和读取目录；位置写入则已经由临时目录中的 progress 测试覆盖。`learn list --track core`只改变列表显示，不改变源码或图形记录。检查点JSON和教程来自T01已经接入的同一份资料；若来源不符，报告具体节点或文件，不继续运行后面的命令。

<!-- run -->
```sh
set -eu  # 任一检查失败就停止，不能让最后的列表成功掩盖前面的错误。
python3 scripts/route_test.py  # 真实调用工具保护与失败路径；无GUI时明确记录skip。
./airios docs-check  # 核对当前所有已安装源码、命令、链接和画像位置。
./airios learn list --track core  # 只显示内核集合和其前置。
python3 scripts/tutorial_coverage.py --help  # 核对差异工具的实际参数入口，不把help当语义验收。
```

此时内核主线仍通过`./airios build`和`./airios test`独立工作；安装GUI后才增加图形清单及其命令。后续主线或GUI新增检查点，要同步对应完整源码、文档、画像和累计摘要，各自保持自己的文件归属。你可以今天推进内核、另一天再继续GUI，不需要为了记录一个位置先写完整桌面。
