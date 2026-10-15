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
