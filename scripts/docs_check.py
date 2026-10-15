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
