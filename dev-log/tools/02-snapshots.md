# T02 · 固定导出来源，并安全替换完整快照

T01已经能从正文重建K13。现在要把一个版本留作对照：参考工程继续修改后，记录仍能说明它来自哪里；再次生成时，也不能把你改过的快照当缓存删掉。导出器因此承担两件不同的事：固定输入，保护输出目录。

<a id="snapshot-git-model"></a>

T01 能从当前正文重建，但如果导出期间参考资料被改动，不同文件就可能来自不同版本。先把“本次读哪一版”变成稳定输入，再谈输出目录怎样替换。Git 的每份提交记录指向一棵已经保存的文件树，完整SHA可以定位这份不可变记录；`main`和`gui`则是可以移到新提交的分支名。导出时先把需要的分支解析成SHA，后续读取这个提交，避免导出一半时分支前移而混入另一版材料。

参考工程的主线提交用消息末尾的`AiriOS-Checkpoint: K13`标明教学身份，GUI提交使用G编号。一次合并通常有两个父提交：在主线上合入GUI时，第一个父是合并前的主线位置，另一个父来自GUI。`--first-parent`只沿第一个父继续，主线身份因此按自己的顺序读取，GUI内部停点另从GUI分支取得。

GUI分支从00分出，只新增自己拥有的路径。因此把它合进03、K13或工具阶段，都不需要覆盖主线入口，也不把00之后的内核提交带进来。合并本身可以在任何已提供阶段进行；运行每个图形检查点仍要满足它声明的K11/K13接口前置。这套分支和尾注约定属于参考工程，自己的学习目录可以使用任意Git历史；本节末尾用只有课程资料的无Git目录观察导出，不要求重建参考工程的提交链。

<a id="snapshot-inputs"></a>
**新建`scripts/snapshots.py`，导入宿主标准库并定义生成器身份：** `fcntl`在macOS/Linux上提供文件锁，后面的生成入口用它避免两个进程同时交接同一个HISTORY。

<!-- write: scripts/snapshots.py -->
```python
"""Export protected route snapshots from fixed Git inputs or a plain source archive."""  # GUI与主线按各自身份定位。
import fcntl    # macOS/Linux的宿主文件锁串行化同一工程的生成器。
import hashlib  # 摘要保护生成内容与来源字节。
import io    # 用内存字节读取Git归档。
import json  # 保存声明、来源身份和内容摘要。
from pathlib import Path  # 组合目录并检查越界。
import shutil      # 复制声明的普通说明和外部字体资产。
import subprocess  # 用参数列表读取Git对象，不改引用。
import tarfile     # 检查每个归档成员再读取。
import tempfile    # 固定来源视图与新HISTORY先写临时目录。
from tools import ROOT  # 位置由脚本所在工程决定。

GENERATOR = 'airios-routes-v4'  # 新格式与旧生成目录分开识别。


```

先建立Git读取和历史声明入口。`git(*arguments, data=None)`中的`*arguments`收集所有位置实参，形成元组；调用`git('show', '-s', ...)`时，里面就是这一串独立参数。放进`['git', *arguments]`时，星号又把元组逐项展开为进程参数列表。`data`位于星号参数之后，只能按名字传入，用来给子进程标准输入提供bytes。

`history.json`只声明主线的章节身份，`schema`值3标明当前格式。实际读取到的SHA由运行结果记录，不写回这份课程资料。`specification`则读取T01的章节列表，用于只需要查看资料的调用。

**追加读取声明的函数：**

<!-- append: scripts/snapshots.py -->
```python
def git(*arguments, data=None):  # data只作为Git标准输入，不插入Shell文本。
    return subprocess.check_output(['git', *arguments], cwd=ROOT, input=data, stderr=subprocess.PIPE)  # 返回原始字节，由格式使用者解码。


def history():  # 跟踪资料只记录分支和检查点身份，避免提交自引用。
    data = json.loads((ROOT / '.codex/teaching/history.json').read_text())  # 这是主线历史声明。
    if data.get('schema') != 3:  # 不猜测旧endpoint与标签方案。
        raise RuntimeError('Expected branch-separated history schema3')  # 历史迁移错误需明确修复。
    return data  # 调用者在一次操作内固定此值。


def specification():  # 列表只需教程资料，不要求存在Git。
    from course import plan    # 与重建器共用选装GUI的规则。
    return plan()['chapters']  # 每项仍含track与requires。


```

<a id="snapshot-branches"></a>
目标 K13 是教学身份，main 是会移动的引用，两者不能互相替代。先定位本次 main 的终点，再在其历史中找 K13。读取 main 时，先找本地`refs/heads/main`，不存在才找`refs/remotes/origin/main`；GUI同理。`origin/gui`是此前取回并保存在本地的远端跟踪引用，读取它不会自动访问网络。`rev-parse --verify`要求引用存在，后面的`^{commit}`要求它解析为提交对象，成功后就固定这个SHA。

现在导出 K13。`wanted["K13"]` 先给出“要哪个教学停点”；读取器把 main 当前引用固定为 SHA，记作 `branch_commit`，再沿这条历史找到带 K13 尾注的提交，记作 `commit`。`records["K13"]` 保存这两个值和 `branch="main"`，让教学身份与本次资料版本同时可追溯。

随后从 `branch_commit` 取得同版的整套教程，再只应用 K13 和前置的编辑。因而修过的解释可以用于重建旧停点，结果仍停在 K13。若直接归档 `commit` 的文件树，取得的则是当时整个 Git 目录；它不是这里的路线重建。

每条提交消息交给Git的尾注解析器。没有教学尾注的维护提交跳过；有尾注时，同一提交只能有一项身份，声明中的同一身份也只能出现一次。字典保留插入顺序，最后比较`list(records)`和`list(wanted)`，同时核对声明身份是否齐全、顺序是否一致。

**追加单分支定位：**

<!-- append: scripts/snapshots.py -->
```python
def branch_records(branch, items):  # 只沿该分支第一父链识别有教学身份的提交。
    endpoint = None  # 尚未选择本地或origin的同名引用。
    for reference in (f'refs/heads/{branch}', f'refs/remotes/origin/{branch}'):  # 新克隆通常只有main是本地分支。
        try:  # 缺少本地gui时允许读取已经取回的origin/gui。
            endpoint = git('rev-parse', '--verify', reference + '^{commit}').decode().strip()  # 第一个存在的引用固定成SHA。
            break  # 后续Git读取都使用这个不可变对象。
        except subprocess.CalledProcessError:  # 此候选不存在，还可以尝试下一个明确引用。
            continue  # 不通过未知分支猜测GUI来源。
    if endpoint is None:  # 两个明确入口都不存在。
        raise RuntimeError(f'Missing history branch: {branch}')  # 提示先取回声明分支。
    wanted = {item['id']: item for item in items}  # 此路线允许出现的检查点集合。
    if len(wanted) != len(items):  # 同名身份不能被后项覆盖。
        raise RuntimeError('Duplicate history checkpoint')  # 在读取提交前停止。
    records = {}  # 按实际历史顺序收集已核对记录。
    for commit in git('rev-list', '--first-parent', '--reverse', endpoint).decode().splitlines():  # 合并GUI不会把GUI内部提交冒充主线进度。
        message = git('show', '-s', '--format=%B', commit)  # 固定SHA的消息不受后续引用移动影响。
        fields = git('interpret-trailers', '--parse', data=message).decode().splitlines()  # Git解析真实尾注，正文同名文字不算。
        markers = [line.removeprefix('AiriOS-Checkpoint: ') for line in fields if line.startswith('AiriOS-Checkpoint: ')]  # 保留全部尾注以发现重复。
        if not markers:  # 普通维护或合并提交不推进教学身份。
            continue  # 不要求每个维护提交都是课程。
        if len(markers) != 1:  # 同一提交不能冒充两个停点。
            raise RuntimeError(f'Duplicate checkpoint trailer: {commit}')  # 显示具体不可用来源。
        name = markers[0]  # 一条合法身份字段对应一个名称。
        if name not in wanted:  # GUI分支共祖00不属于GUI自己的节点。
            continue  # 其他已声明路线在自己的分支读取。
        if name in records:  # 重复身份不能静默取最新。
            raise RuntimeError(f'Duplicate checkpoint: {name}')  # 先修历史再导出。
        records[name] = dict(wanted[name], commit=commit, branch=branch, branch_commit=endpoint)  # SHA只进入生成记录。
    if list(records) != list(wanted):  # 身份顺序、缺口与声明必须一致。
        raise RuntimeError(f'{branch}: declared checkpoints differ from Git history')  # 不回退到旧历史或标签。
    return records  # 全部分支节点通过后才交出映射。


```

K13 的身份和本次资料终点都已固定，接下来才从这些提交取得字节；此时仍未创建 HISTORY。主线声明可以单独读取，只有课程资料中存在graphics.json时才定位GUI历史。`checkpoint_records`返回的外层字典以检查点身份为键，值是刚才补充来源字段的记录；后面的固定视图从这些记录取得本次main与gui的准确终点。

`git archive`把指定提交树编码为tar归档，`archive`接到的是bytes。`io.BytesIO(archive)`让tarfile像读取文件一样读取这些内存字节。归档中的每个成员都有路径、类型与权限；读取器拒绝绝对路径、上级跳转、文件路径别名、重复文件名，以及符号链接和其他特殊类型。合格文件放进`files`字典，值是`(data, mode)`二元组：data是原始字节，mode是权限整数。此时仍未创建输出目录。

**追加组合身份和归档读取：**

<!-- append: scripts/snapshots.py -->
```python
def checkpoint_records():  # 主线与GUI分别读取，GUI不存在时主线照常工作。
    data = history()  # 主线不依赖GUI历史才能定位。
    result = branch_records('main', data['checkpoints'])  # 第一父链保留主线进度。
    optional = ROOT / '.codex/teaching/graphics.json'     # 合并支线后才有额外声明。
    if optional.exists():  # 源码存在时还要检查对应分支身份。
        graphics = json.loads(optional.read_text())  # 每个G节点都由GUI分支承担。
        result.update(branch_records('gui', graphics['chapters']))  # 不把GUI合并进main历史表。
    return result  # 普通源码归档不会调用Git入口。


def archive_files(commit):  # 把一个不可变提交树转成路径到(bytes,mode)的字典。
    archive = git('archive', '--format=tar', commit)  # Git只交出跟踪文件，不包含.git或忽略缓存。
    files = {}  # 每个普通文件名只允许出现一次。
    with tarfile.open(fileobj=io.BytesIO(archive)) as contents:  # 归档流在with结束时关闭。
        for member in contents.getmembers():  # 检查路径、类型和重复之后才读取内容。
            path = Path(member.name)  # 拆分成员路径以识别绝对位置和上级段。
            if path.is_absolute() or '..' in path.parts or not (member.isfile() or member.isdir()):  # 拒绝目录外路径、链接和特殊成员。
                raise RuntimeError(f'Unsafe archive member: {member.name}')  # 不使用extractall直接展开未知树。
            if member.isfile():  # 目录由写文件时按需创建。
                if not path.parts or str(path) != member.name:  # 拒绝空名和a//b、./a等不同文本指向同一位置的别名。
                    raise RuntimeError(f'Noncanonical archive file: {member.name}')  # 不允许后项通过路径别名覆盖前项。
                if member.name in files:  # 同名成员不能以后项静默覆盖。
                    raise RuntimeError(f'Duplicate archive member: {member.name}')  # 冲突字节无法成为明确来源。
                with contents.extractfile(member) as source:  # 本成员的文件流只在读取期间有效。
                    files[member.name] = (source.read(), member.mode & 0o777)  # 八进制掩码保留普通读写执行位。
    if any(Path(name).parts[0] == 'HISTORY' for name in files):  # 生成快照不得又出现在来源树中。
        raise RuntimeError('HISTORY must never be tracked inside a checkpoint')  # 避免重复嵌套整条教程。
    return files  # 已核对的内容仍在内存，尚未改磁盘目标。


```

<a id="snapshot-output-protection"></a>
固定输入解决了读哪版，写到哪里仍要独立保护。不能因为输入可信，就覆盖一个可能含个人修改的旧目录。`write_archive` 只接受尚不存在的目录，按刚才核对过的成员创建普通文件。`export_checkpoint`再给这层写入增加目标位置保护：拒绝目标本身的链接、已有目录、参考工程内部和参考工程的祖先。它直接写出指定Git提交树，T03的current导出会使用这条路径。

重复生成HISTORY需要另一种判断。`check_previous`先检查目录中的`index.json`是否带有当前`GENERATOR`身份，再重新读取其余每个文件的摘要；实际内容与索引不相等就拒绝替换。索引本身不加入摘要，避免要求它记录自己的哈希。新增、缺失和内容编辑都能发现，权限变化与空目录变化不由这份字节索引记录。

`files.items()`迭代时，每项是`(name, (data, mode))`；循环或推导式里的两层括号把路径、字节和权限分别取出。`content_hashes`只用data计算摘要，`write_archive`才使用mode恢复权限。`member.mode & 0o777`保留普通读、写、执行位；`sorted(files.items())`按路径排序，使生成JSON的顺序稳定。

**追加摘要、写入和旧目录检查：**

<!-- append: scripts/snapshots.py -->
```python
def content_hashes(files):  # 文件字节与权限分开处理，摘要仅证明内容一致。
    return {name: hashlib.sha256(data).hexdigest() for name, (data, mode) in sorted(files.items())}  # 排序稳定JSON输出，不改变源码字节。


def write_archive(files, target):  # 目标必须是尚不存在的新目录。
    target.mkdir(parents=True)  # 不用exist_ok，出现已有目录便拒绝。
    for name, (data, mode) in files.items():  # 解包路径、原始字节和权限整数。
        destination = target / name  # 成员路径已检查安全，只能落在目标内。
        destination.parent.mkdir(parents=True, exist_ok=True)  # 多个兄弟文件共享父目录。
        destination.write_bytes(data)  # 不解码，不改变换行或文本编码。
        destination.chmod(mode)  # 恢复airios和Shell脚本的执行权限。


def json_file(path, value):  # 生成报告使用可读中文与固定缩进。
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n')  # JSON末尾补换行，文件内容可重复比较。


def export_checkpoint(record, target):  # 显式导出不替换用户目录，也不写入参考工程。
    raw = Path(target).expanduser().absolute()  # 先保留目标本身的路径身份，避免resolve隐藏悬空链接。
    if raw.is_symlink():  # 目标链接即使指向不存在位置也属于已有对象。
        raise RuntimeError(f'Export destination is a symlink: {raw}')  # 不写进链接指向的位置。
    target = raw.resolve()  # 跟随父链接再检查实际边界；macOS的/tmp本身就是系统链接。
    root = ROOT.resolve()   # 参考根位置使用相同规范化尺度。
    if target == root or target in root.parents or root in target.parents:  # 参考工程本身、祖先和内部都禁止导出。
        raise RuntimeError('Export outside the reference repository into a new directory')  # 不将导出混入实际参考源码。
    if target.exists():  # 空目录也属于已有用户对象。
        raise RuntimeError(f'Export destination already exists: {target}')  # 不把存在理解为可覆盖。
    files = archive_files(record['commit'])  # 已核对记录中的准确SHA不会随引用移动而变。
    write_archive(files, target)  # 本次来源全部检查后写完整树和权限。
    json_file(target / '.airi-checkpoint.json', record)  # 导出身份报告留存固定main与本步SHA。
    print(f'EXPORTED {record.get("id", "current")}: {record["commit"]}\n{target}')  # 显示准确来源，不显示已删除标签。


def check_previous(target):  # 只替换相同生成器产生且字节未被编辑的HISTORY。
    if target.is_symlink():  # 正式目录不得指向仓库外位置。
        raise RuntimeError('HISTORY cannot be a symlink')  # 不跨链接检查和替换用户对象。
    if not target.exists():  # 第一次生成没有旧树需要保护。
        return  # 后续直接建立新快照。
    if not target.is_dir():  # 同名文件不是生成目录。
        raise RuntimeError('HISTORY must be a directory')  # 保留这个现有文件。
    if not any(target.iterdir()):  # 空目录没有未知内容。
        return  # 可以将完整生成树放到该位置。
    index = target / 'index.json'  # 清单证明生成身份并记录原文件摘要。
    if not index.is_file():  # 非空未知目录不得当作缓存删除。
        raise RuntimeError('HISTORY contains non-generated files; move them elsewhere before generating snapshots')  # 需要先保存未知内容。
    record = json.loads(index.read_text())    # 重读实际旧索引，不复用内存假设。
    if record.get('generator') != GENERATOR:  # 旧格式或其他工具的输出均不自动迁移。
        raise RuntimeError('HISTORY was not created by this script')  # 拒绝没有本次保护契约的目录。
    actual = {}  # 当前文件内容映射稍后与旧记录整体比较。
    for path in target.rglob('*'):  # 嵌套链接和新增文件也参与检查。
        if path.is_symlink():  # 任何内部链接都可能读到树外内容。
            raise RuntimeError(f'Unexpected symlink in HISTORY: {path}')  # 指出需要移走的链接。
        if path.is_file() and path != index:  # 索引不保存自己的摘要，避免自引用。
            actual[str(path.relative_to(target))] = hashlib.sha256(path.read_bytes()).hexdigest()  # 只记录相对路径和真实字节，权限变动不由此检查。
    if actual != record['files']:  # 新增、丢失或修改任一文件都不相同。
        raise RuntimeError('HISTORY was edited; move your edited snapshot elsewhere before regenerating it')  # 保护学习者改动，不将其当成缓存覆盖。


```

<a id="snapshot-freeze"></a>
## 从固定的教程输入重建主线与可选GUI

Git参考工程先要求没有未提交或未跟踪的改动，再从固定的分支终点读取文件树。`records.values()`提供各份来源记录，生成器表达式逐项选择branch为main的记录并给出branch_commit，next取得其中第一项。GUI使用带默认值None的next，未安装GUI时返回None，不进入它的组合分支。这种查找取得所需值就停止，不另外建立一份列表。

GUI相对00的差异必须全为新增路径，而且不能抢占主线`owned`。`--parents`的每行由当前提交SHA与父SHA组成；非根提交有一个父时是两项，超过两项就出现合并。GUI拒绝这种合并父链，main则可以保留合入GUI的提交。组合时，同名GUI文件与main中已有副本相同就保留，不同则明确报错，不能悄悄选一边覆盖。

学习目录也可能只是一个源码包，没有提交历史可定位。这时稳定性来自收集后的内存字节与再次读取比较，不能伪造一个 Git SHA。没有 `.git` 的普通源码包不查分支。`inputs`保存允许读取的实际文件路径，`files`保存“相对路径→(字节, 权限)”；第二次读取与内存字节比较，若读取期间出现变化就停止。`write_archive`随后把这份已收集的内容写到独立临时目录，此后各路线都从那里取材料，原目录的后续编辑不再改变本次输入。Git模式在报告中保存分支SHA，普通包保存每份输入的摘要。

允许材料包括课程正文、元数据、说明、外部协议和字体资产。除了锁定字体与配方生成的图集，内核、前端应用和Python脚本实现都不从文件树直接送入学习版本；真正的实现仍由写入、追加与精确替换动作产生。自己使用任意Git历史时，可以采用章末的无Git资料观察方式，继续调用同一导出函数。

**追加输入白名单和固定视图：**

<!-- append: scripts/snapshots.py -->
```python
def teaching_input(name):  # 固定视图只包含教程资料与声明的外部资产。
    path = Path(name)  # 相对路径已经由归档或本机文件清单提供。
    if path.parts[0] in ('dev-log', 'docs') or name.startswith('.codex/teaching/'):  # 正文、元数据和读者上下文。
        return True  # 它们不直接提供终点实现。
    if name.startswith('frontend/assets/'):  # 字体锁、外部轮廓、许可与生成图集有明确来源。
        return True  # 这类资产按G12配方提供，不要求手写二进制。
    return name in ('README.md','AGENTS.md','THIRD_PARTY.md','LICENSE','boot/AiriBoot/README.md','labs/README.md','labs/linux/p1/README.md','third_party/limine.h','third_party/limine-LICENSE','third_party/edk2-licenses.txt')  # 不复制任何脚本或内核实现。


def frozen_view(destination):  # 每次导出都固定一次材料，普通源码包也不反复读取变化中的正文。
    sources = {}  # 保存Git身份或普通文件摘要，准确区分两种来源。
    if not (ROOT / '.git').exists():  # 普通源码归档没有作者Git或技能也能使用。
        inputs = []  # 不遍历build、HISTORY或作者技能链接。
        for directory in ('dev-log', 'docs', '.codex/teaching', 'frontend/assets'):  # 只有这几类目录提供纯资料。
            parent = ROOT / directory  # 不存在的GUI目录可以略过。
            if parent.is_symlink():    # 普通包也不能把外部目录伪装成自己的材料。
                raise RuntimeError(f'Teaching input directory is a symlink: {directory}')  # 不跟随未知边界。
            inputs.extend(path for path in parent.rglob('*') if path.is_file() and not path.is_symlink())  # 只读取普通文件。
        for name in ('README.md','AGENTS.md','THIRD_PARTY.md','LICENSE','boot/AiriBoot/README.md','labs/README.md','labs/linux/p1/README.md','third_party/limine.h','third_party/limine-LICENSE','third_party/edk2-licenses.txt'):  # 单独声明的说明与外部材料。
            path = ROOT / name  # 名称来自这份固定白名单。
            if path.is_file() and not path.is_symlink():  # 早期资料缺省时不读取其他路径。
                inputs.append(path)  # 后续统一固定成字节与摘要。
        files = {str(path.relative_to(ROOT)):(path.read_bytes(), 0o644) for path in inputs}  # 一次读取形成内存中的固定字节。
        for path in inputs:  # 写出视图前再确认本轮读取期间没有修改。
            if path.read_bytes() != files[str(path.relative_to(ROOT))][0]:  # 并行编辑不能产生混合版本。
                raise RuntimeError('Teaching inputs changed during export')  # 留原目录不动，重新检查后再导出。
        sources['documents_sha256'] = content_hashes(files)  # 普通归档用内容身份，不冒称固定Git提交。
    else:  # Git参考工程从明确提交对象读取。
        if git('status', '--porcelain').strip():  # 未提交内容不能混称已提交来源。
            raise RuntimeError('Snapshot export needs a clean committed reference tree')  # 在写学习目录前失败。
        records = checkpoint_records()  # 固定并校验各分支身份。
        main = next(item['branch_commit'] for item in records.values() if item['branch'] == 'main')  # 本次主线终点。
        files = archive_files(main)  # Git树包含权限与原始字节。
        sources['main'] = main  # SHA只进入生成报告。
        optional = next((item['branch_commit'] for item in records.values() if item['branch'] == 'gui'), None)  # 未合并GUI时没有可选来源。
        if optional:  # 图形分支必须只在共享00根之后增加自己的路径。
            roots = git('rev-list', '--first-parent', '--reverse', main).decode().splitlines()  # 主线第一父链的起点是00。
            root = roots[0]  # 共祖提交不可由工作目录推断。
            gui_chain = git('rev-list', '--first-parent', '--reverse', optional).decode().splitlines()  # GUI历史也从同一00开始。
            parent_rows = git('rev-list', '--first-parent', '--parents', optional).decode().splitlines()  # GUI自身不能合入较晚主线历史。
            if any(len(row.split()) > 2 for row in parent_rows):  # 合并父会把别的主线阶段带入GUI历史。
                raise RuntimeError('GUI history must remain linear')  # 保留GUI作为只添加自身路径的支线。
            if gui_chain[0] != root:  # GUI若另有起点，不能承诺任意主线阶段合并。
                raise RuntimeError('GUI must share the main00 root')  # 先修分支来源。
            gui_files = archive_files(optional)  # 图形分支终点固定为同一对象。
            core_owned = set(json.loads(files['.codex/teaching/checkpoints.json'][0])['owned'])  # 主线拥有的实现不可由GUI创建或修改。
            changes = git('diff', '--no-renames', '--name-status', '-z', root, optional).decode().split('\0')  # NUL边界保留空格和换行文件名。
            for index in range(0, len(changes) - 1, 2):  # 无重命名时每次恰好取得状态与文件名。
                status, name = changes[index:index + 2]  # 文件名不通过TAB或换行重新切分。
                if status != 'A' or name in core_owned:  # 修改、删除、移动或抢占主线文件都会破坏契约。
                    raise RuntimeError(f'GUI is not additive: {status} {name}')  # 不能静默丢弃图形或核心改动。
                value = gui_files[name]  # 从已固定的图形树取得新增文件。
                if name in files and files[name] != value:  # main合并过GUI时允许相同字节，拒绝分叉维护同一路径。
                    raise RuntimeError(f'GUI path diverged in main: {name}')  # 要求在GUI分支维护这份实现。
                files[name] = value  # 只新增图形独有内容或保留相同副本。
            sources['gui'] = optional  # 记录组合的第二个来源。
        files = {name:value for name,value in files.items() if teaching_input(name)}  # 读取正文不需要成品实现。
    write_archive(files, destination)  # 写出一次固定的普通输入视图。
    return destination, sources  # 所有路线复用这份视图，避免交错读取。


```

公共导出先保护目标，再建立一次固定视图。对这次 K13 导出，`source` 是工程外临时资料目录的 Path，正文以后从这里读取；`commits` 是本次来源字典，随结果保存而不参与写入源码；名字虽叫commits，普通包时其中保存的是`documents_sha256`。两者一起传给`write_route`，该函数不再重新解析分支。内部快照生成也复用它，所以开始写暂存目录后不会再次读取Git状态。

**追加公共路线导出入口：**

<!-- append: scripts/snapshots.py -->
```python
def export_route(name, destination, alongside=None):  # 路线快照从固定正文构造，不把成品源码当作输入。
    from course import replay, plan, ordered  # 三个入口使用同一依赖图。
    raw = Path(destination).expanduser().absolute()  # 保留断链与目标本身身份。
    if raw.exists() or raw.is_symlink():  # 已有目录即使空也不能覆盖。
        raise RuntimeError(f'Export destination already exists: {raw}')  # 不删除学习者改动。
    target = raw.resolve()  # 之后所有写入固定使用这个位置。
    root = ROOT.resolve()   # 与目标使用同一规范化尺度。
    if target == root or root in target.parents or target in root.parents:  # 不在参考树内或其祖先导出。
        raise RuntimeError('Export outside the reference repository')  # 防止把来源混进生成物。
    with tempfile.TemporaryDirectory(prefix='airios-frozen-') as temporary:  # 公共导出固定一次已校验来源。
        source, commits = frozen_view(Path(temporary) / 'source')  # 来源视图不位于当前工作树。
        write_route(name, target, source, commits, alongside)      # 低层只接收已固定的普通输入。
    print(f'EXPORTED route {name}: {target}')  # 完成后显示确切新目录。


```

<a id="snapshot-composition"></a>
暂存资料已固定，真正输出还应只描述本次选中的路线：带上后续 T03 的声明，却没有 T03 实现，会制造一个自相矛盾的包。以导出 K13 为例，`replay`先按固定资料重建到K13，并写出包含源码摘要的`.airi-checkpoint.json`。随后`selected`取得实际应用的全部章节记录，复制课程说明、许可和纯教学资料，再据selected生成这份输出专用的清单。原资料可能还描述后续T01–T03，输出清单只保留本次确实形成的前置与终点。

`core`保存非图形记录，`graphical`保存图形记录，列表推导式只筛选元素而不改变原顺序。`core[-1]`是最后一个主线记录，它的files字典描述这次主线累计源码；图形owned则从组合状态中减去这些路径。复制纯教学资料时可能带入来源的完整graphics.json，所以输出先删除这个副本，再按实际选择写出图形声明；未选择GUI就保持该文件不存在，图形实现和字体也不会加入。

字体阶段之后才复制锁中列明的TTF、许可与配方生成的数组；像素阶段没有字体锁，就不会取得这些后续材料。锁里的file也要逐项检查：绝对路径、`..`、空路径和`a//b`这样的非规范别名都拒绝，合格名称才拼接到源、目标各自的`frontend/assets`目录。`.airi-checkpoint.json`同时记录节点、源码摘要和本次固定来源。Git来源记SHA，普通归档记材料摘要，两者不混称。

**追加同一次固定输入上的实际写入：**

<!-- append: scripts/snapshots.py -->
```python
def write_route(name, target, source, commits, alongside=None):  # 公共导出与内部快照共用相同重建动作。
    from course import replay, plan, ordered  # 此函数不重新解析Git引用。
    replay(target, name, False, alongside, source=source)  # 只应用明确编辑动作，不运行Shell。
    data = plan(source)  # 使用同一组已固定的清单。
    selected = ordered([name] + ([alongside] if alongside else []), data)  # 共享前置只进入一次。
    for directory in ('dev-log', 'docs', '.codex/teaching'):  # 复制纯教程、画像和作者验证说明，不复制实现或技能。
        if (source / directory).is_dir():  # GUI材料未合并时可以不存在。
            shutil.copytree(source / directory, target / directory, dirs_exist_ok=True)  # 不复制任何实现目录或本机技能。
    for relative in ('README.md','AGENTS.md','THIRD_PARTY.md','LICENSE','boot/AiriBoot/README.md','labs/README.md','labs/linux/p1/README.md','third_party/limine.h','third_party/limine-LICENSE','third_party/edk2-licenses.txt'):  # 明确声明的纯说明与外部协议材料。
        path = source / relative  # 当前来源中实际可用的位置。
        if path.is_file():  # 早期停点尚无的说明可以缺省。
            out = target / relative  # 外部文件仍位于其正常工程位置。
            out.parent.mkdir(parents=True, exist_ok=True)  # 为独立说明创建父目录。
            shutil.copyfile(path, out)  # 不取得内核或工具实现补洞。
    metadata = target / '.codex/teaching'  # 输出清单只描述此次选择的源码状态。
    metadata.mkdir(parents=True, exist_ok=True)  # 早期源码没有教学元数据目录。
    state = json.loads((target / '.airi-checkpoint.json').read_text())['files']  # replay已保存每份实现的摘要。
    core = [item for item in selected if item['track'] != 'graphics']  # 主线资料独立保存。
    graphical = [item for item in selected if item['track'] == 'graphics']  # GUI资料只有实际选择后才出现。
    core_owned = sorted(core[-1]['files'])  # 前置顺序中最后一个主线节点描述本次主线源码。
    core_plan = {'schema':3, 'chapters':core, 'owned':core_owned, 'endpoints':[core[-1]['id']]}  # 输出不宣称包含未选择的后续工具。
    json_file(metadata / 'checkpoints.json', core_plan)   # 主线资料不会包含GUI自己的路径。
    (metadata / 'graphics.json').unlink(missing_ok=True)  # 清除来源中的完整GUI声明，不能要求此次未选的后续实现。
    if graphical:  # 独立记录可选路线，不覆盖上面的主线清单。
        own = sorted(set(state) - set(core[-1]['files']))  # 图形只拥有主线之外的实现路径。
        json_file(metadata / 'graphics.json', {'schema':3,'chapters':graphical,'owned':own,'endpoints':[graphical[-1]['id']]})  # 依赖仍指向同一K11/K13基础。
    if (source / '.codex/teaching/history.json').is_file():  # Git声明只是资料，不要求普通归档存在Git。
        shutil.copyfile(source / '.codex/teaching/history.json', metadata / 'history.json')  # 不复制任何.git内容。
    font_lock = target / 'frontend/assets/fonts.lock.json'  # 只有字体检查点以后才需要外部轮廓与生成数组。
    if font_lock.is_file():  # G11像素阶段不偷偷取得后续资产。
        assets = json.loads(font_lock.read_text())  # 锁中每项都含来源摘要。
        for item in assets['assets']:  # 只复制明确列出的TTF和OFL。
            relative = Path(item['file'])  # 锁中名称也必须是资产目录内的规范相对路径。
            if relative.is_absolute() or '..' in relative.parts or not relative.parts or str(relative) != item['file']:  # 不信任锁文件中的路径。
                raise RuntimeError('Unsafe font asset path')  # 写任何外部资产前拒绝越界。
            origin = source / 'frontend/assets' / item['file']  # 已发布外部资产输入。
            out = target / 'frontend/assets' / item['file']     # 归档保留相同文件名。
            shutil.copyfile(origin, out)  # 自有生成器仍已由正文写出。
        shutil.copyfile(source / 'frontend/assets/font-atlas.h', target / 'frontend/assets/font-atlas.h')  # 生成数组与锁定配方对应，不要求逐字手写字节。
    json_file(target / '.airi-checkpoint.json', {'checkpoints':[item['id'] for item in selected],'files':state,'sources':commits,'mode':'tutorial reconstruction from identified frozen inputs'})  # 记录组合身份和全部内容摘要。


```

<a id="snapshot-lock"></a>
## 先完成新树，再交接旧树

一次 K13 导出只需新目录；HISTORY 却要一次构造全部快照，再替换旧树。若两个生成器同时交接，它们可能互相移动正式目录，把对方的暂存状态当成自己的旧状态。宿主文件锁使用工程真实路径的摘要区分不同仓库；`LOCK_EX | LOCK_NB`表示独占且不等待，已经有人持有时直接失败。锁文件留在临时目录不代表仍被锁，文件描述符关闭或进程退出后，内核会释放锁。

**追加外层生成入口：**

<!-- append: scripts/snapshots.py -->
```python
def generate():  # 文件锁在进程结束时释放，不因残留文件永久阻塞后续运行。
    identity = hashlib.sha256(str(ROOT.resolve()).encode()).hexdigest()  # 不同工程使用不同锁名。
    lock = Path(tempfile.gettempdir()) / ('airios-history-' + identity + '.lock')  # 锁不写进来源Git树。
    with lock.open('a') as held:  # 文件存在不代表仍被锁，打开后由内核判断当前持有者。
        try:  # 不等待另一个生成器，以明确失败结束这次调用。
            fcntl.flock(held, fcntl.LOCK_EX | fcntl.LOCK_NB)  # EX独占，NB使已有持有者时立即报错。
        except BlockingIOError as error:  # 已有生成器持有同一工程的锁。
            raise RuntimeError('Another snapshot generator is running') from error  # 不创建第二份交接树。
        generate_locked()  # with离开后描述符关闭，内核同时释放这把锁。


```

`target`始终是正式HISTORY，`stage`是本轮正在构造的新树，`previous`是交接时保存的旧树。生成器先在与target相同的文件系统中构造stage，逐个写出全部路线及索引，期间不移动旧HISTORY。新树完成后再核对旧内容，确认构造期间没有个人编辑。

stage 已完整构造，才允许旧路径暂时离开正式位置。交接先把 target 整体移到 previous，再把stage移到target。previous来自`mkdtemp`，不会在离开with时自动清理；这样即使恢复失败，旧树仍保存在工程下的`.airios-history-backup-.../previous`。`except BaseException`同时接住普通错误和Ctrl-C的KeyboardInterrupt，在正式目标仍不存在时尝试把旧树移回。

成功后才删除已经验证未修改的旧生成物，`finally`只删除空的备用目录。两次移动各自是目录rename，整个交接并不是一次不可分割的操作；进程若在中间被强制结束，正式路径可能暂时缺失，旧树仍可在备用位置找到。恢复时先处理错误报告的权限或文件系统问题，再在HISTORY不存在的条件下把previous移回，保留旧副本直至正式路径恢复。

被编辑快照触发拒绝时，先把修改版移到单独学习目录，再重新生成。工具不丢弃也不合并这些编辑；文件锁只排除另一个生成器，不阻止编辑器修改文件，所以交接前和移开后仍需核对摘要。

**追加持锁后的构造、交接与直接执行入口：**

<!-- append: scripts/snapshots.py -->
```python
def generate_locked():  # 调用者已经排除了两个生成器互相覆盖。
    from course import plan    # GUI可选资料由同一入口识别。
    target = ROOT / 'HISTORY'  # 正式生成目录不会进入跟踪源码。
    check_previous(target)     # 最开始发现已有学习者编辑就停止。
    with tempfile.TemporaryDirectory(prefix='airios-frozen-') as frozen:  # 来源放在Git树外。
        source, commits = frozen_view(Path(frozen) / 'source')  # 所有节点复用同一main/gui组合。
        with tempfile.TemporaryDirectory(prefix='.airios-history-', dir=ROOT) as temporary:  # 新树与目标位于同一文件系统。
            stage = Path(temporary) / 'next'  # 完整构造成功前不触碰HISTORY。
            stage.mkdir()  # 每个节点在内部创建自己的目录。
            rows = []      # 已完成的快照身份。
            for chapter in plan(source)['chapters']:  # 实际前置由重建器闭合。
                write_route(chapter['id'], stage / chapter['id'], source, commits)  # 内部路径不重复执行公共外部导出检查。
                rows.append({'id':chapter['id'],'directory':chapter['id']})  # 保存相对目录名。
            (stage / 'README.md').write_text('# 完整路线快照\n\n' + ''.join(f'- [{row["id"]}]({row["directory"]}/README.md)\n' for row in rows))  # 导航指向已生成目录。
            hashes = {str(path.relative_to(stage)):hashlib.sha256(path.read_bytes()).hexdigest() for path in stage.rglob('*') if path.is_file()}  # 不将index自己的摘要写进自身。
            json_file(stage / 'index.json', {'generator':GENERATOR,'checkpoints':rows,'files':hashes})  # 下一次用这些字节识别用户编辑。
            check_previous(target)  # 构造期间用户若编辑旧树，不能用开头的检查结果覆盖它。
            backup = Path(tempfile.mkdtemp(prefix='.airios-history-backup-', dir=ROOT))  # 旧树移到不会被TemporaryDirectory自动清除的位置。
            previous = backup / 'previous'  # 恢复失败时这里的内容仍保留。
            try:  # 覆盖中断、文件系统错误以及用户Ctrl-C。
                if target.exists():  # 首次生成没有旧目录。
                    target.rename(previous)   # 整体移开，尚未删除任何原内容。
                    check_previous(previous)  # 复查紧邻rename的编辑竞态；变化时走同一恢复路径。
                stage.rename(target)  # 新树已完整，通过原子目录移动成为正式结果。
            except BaseException:  # KeyboardInterrupt也必须先尝试恢复，不能让旧树随临时目录消失。
                if previous.exists() and not target.exists():  # 不覆盖任何已存在的新目标。
                    previous.rename(target)  # 若此操作也失败，backup仍位于持久路径供恢复。
                raise  # 原始失败保持非成功状态。
            else:  # 只有完整交接成功才清理已验证的旧生成物。
                if previous.exists():  # 首次生成没有可删除的旧树。
                    shutil.rmtree(previous)  # 只删除本次移入的、已核对未编辑的旧生成目录。
            finally:  # 恢复失败不能递归删除backup。
                if not any(backup.iterdir()):  # 空目录已经没有需要恢复的内容。
                    backup.rmdir()  # 只删除空的自有备用目录。
    print(f'PASS snapshots: {len(rows)} independent route checkpoints')  # 所有节点和正式交接都完成后报告。


if __name__ == '__main__':  # 导入仅建立函数，直接执行才生成。
    generate()  # 默认不覆盖任何被编辑目录。
```

## 在隔离资料目录观察导出

最后用同一输入导出两份 K13，比较实际生成字节，观察的是内容稳定性，不是重复操作自己的学习目录。下面的源目录只有课程正文与教学 JSON，没有`.git`或终点实现。给`snapshots.ROOT`赋值，只改变当前Python进程中这个模块的路径属性；`course.replay`接收的source则由固定视图显式传入，因此仍从同一份资料读正文。两个导出各自写到一个新目录，随后比较files摘要字典。临时目录在观察结束后释放，自己的学习目录保持原样。

<!-- run -->
```sh
PYTHONPATH=scripts python3 - <<'PYEXPORT'  # 使用自己写出的导出器完成真实文件写入。
from pathlib import Path  # 组成隔离路径。
import tempfile  # 独立于个人源码的临时观察。
import shutil  # 只复制纯资料。
import json  # 比较两次实际生成的摘要。
import snapshots  # 导入时不自动生成HISTORY。
with tempfile.TemporaryDirectory() as directory:  # 观察副作用限于此目录。
    parent = Path(directory)  # 字符串路径转换为可拼接对象。
    source = parent / 'source'  # 固定输入前的纯资料来源。
    for name in ('dev-log', '.codex/teaching'):  # 不复制scripts、kernel和frontend。
        shutil.copytree(Path.cwd() / name, source / name)  # 每个目标子目录都尚不存在。
    snapshots.ROOT = source  # 本进程显式选择无Git资料包，之后不会改当前源码。
    snapshots.export_route('K13', parent / 'one')  # 第一次从正文重建。
    snapshots.export_route('K13', parent / 'two')  # 第二次仍使用相同输入。
    one = json.loads((parent / 'one/.airi-checkpoint.json').read_text())  # 读取实际输出摘要。
    two = json.loads((parent / 'two/.airi-checkpoint.json').read_text())  # 取得第二份独立结果。
    print('same source files:', one['files'] == two['files'])  # 应为True，不比较临时绝对位置。
PYEXPORT
```

输出包含两次EXPORTED，最后为`same source files: True`。这次比较核对的是K13实现文件的重建字节；临时源没有复制完整项目说明，不在这里运行导航检查，也没有再次启动QEMU。下一节在学习工程中接入内容检查、路线位置记录，并运行目录保护的失败示范。
