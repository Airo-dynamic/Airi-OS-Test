"""Author-side regression for tutorial edit semantics; no kernel or remote run."""
import sys
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[3] / 'scripts'))
import course


def fence(code, language='c'):
    return '```' + language + '\n' + code + '```\n'


def edit(mode, name, code):
    return f'<!-- {mode}: {name} -->\n' + fence(code)


def replace(name, before, after):
    return f'<!-- replace: {name} -->\n<!-- before -->\n' + fence(before) + '<!-- after -->\n' + fence(after)


class CourseEdits(unittest.TestCase):
    def test_ordered_edits_preserve_full_bytes(self):
        state = {}
        course.apply(edit('write', 'x.c', 'one\n') + edit('append', 'x.c', 'two\n') + replace('x.c', 'one\n', 'new\n\n'), state)
        self.assertEqual(state, {'x.c': 'new\n\ntwo\n'})

    def test_empty_after_deletes_only_old_window(self):
        state = {'x.c': 'one\ntwo\n'}
        course.apply(replace('x.c', 'one\n', ''), state)
        self.assertEqual(state['x.c'], 'two\n')

    def test_legacy_empty_write_and_append(self):
        state = {}
        course.apply(edit('write', 'x.c', '') + edit('append', 'x.c', ''), state)
        self.assertEqual(state['x.c'], '\n\n')

    def test_missing_or_ambiguous_before_is_atomic(self):
        for contents, before in [('other\n', 'old\n'), ('old\nold\n', 'old\n'), ('a\na\na\n', 'a\na\n')]:
            with self.subTest(contents=contents):
                state = {'x.c': contents}; original = state.copy()
                with self.assertRaisesRegex(RuntimeError, 'requires one match'):
                    course.apply(edit('write', 'another.c', 'created\n') + replace('x.c', before, 'new\n'), state)
                self.assertEqual(state, original)

    def test_start_state_is_required(self):
        for document in [edit('append', 'x.c', 'new\n'), replace('x.c', 'old\n', 'new\n')]:
            with self.assertRaisesRegex(RuntimeError, 'before creation'):
                course.apply(document, {})

    def test_unsafe_paths(self):
        for name in ['/x.c', '../x.c', 'a/../x.c', 'a//x.c', './x.c']:
            with self.subTest(name=name), self.assertRaisesRegex(RuntimeError, 'Unsafe teaching path'):
                course.apply(edit('write', name, 'x\n'), {})

    def test_malformed_actions_cannot_disappear(self):
        valid = replace('x.c', 'old\n', 'new\n')
        samples = [valid.replace('<!-- before -->', '<!-- after -->', 1), valid.replace('<!-- after -->', '<!-- missing -->'), valid.replace('replace:', 'replace :'), '<!-- before -->\n', '<!-- write: x.c -->\n```c\nnot closed\n', replace('x.c', '', 'new\n')]
        for document in samples:
            with self.subTest(document=document), self.assertRaises(RuntimeError):
                course.apply(document, {'x.c': 'old\n'})

    def test_markers_in_display_fences_are_literal(self):
        document = fence('<!-- write: ghost.c -->\n', 'text') + edit('write', 'real.c', 'ok\n')
        state = {}; course.apply(document, state)
        self.assertEqual(state, {'real.c': 'ok\n'})

    def test_blank_lines_between_markers_are_not_source(self):
        document = replace('x.c', 'old\n', 'new\n').replace('<!-- before -->\n', '<!-- before -->\n\n').replace('<!-- after -->\n', '\n<!-- after -->\n\n')
        state = {'x.c': 'old\n'}; course.apply(document, state)
        self.assertEqual(state['x.c'], 'new\n')


if __name__ == '__main__':
    unittest.main()
