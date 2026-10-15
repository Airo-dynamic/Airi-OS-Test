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
