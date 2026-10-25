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
