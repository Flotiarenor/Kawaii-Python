"""Tests for the replaceable-message table of this fork.

The table is an external file, read once per process and cached, so every case
runs the interpreter again with PYTHON_MESSAGE_TABLE pointing at a table written
for that case.  Exercising it in-process is not possible: the lookup happens
inside _PyErr_Display().

Three properties matter beyond "the replacement shows up":

* a message the table does not cover must come out exactly as upstream prints it;
* a type whose display is not derived from args[0] must still show the
  replacement -- it is carried by a display proxy, a subclass whose __str__
  returns the table's text -- while a display the table only saw part of (the
  repr of a multi-argument tuple, a SyntaxError carrying a location) must keep
  its original text;
* the interactive loop must be covered too, because it never calls
  sys.excepthook -- that is the whole reason this feature lives in C.
"""

import os
import subprocess
import sys
import tempfile
import unittest

from test.support import os_helper, requires_subprocess
from test.support.script_helper import assert_python_failure


def entry(key, value):
    """One table line: key, TAB, the replacement as hex-encoded UTF-8."""
    return f"{key}\t{value.encode('utf-8').hex()}"


@requires_subprocess()
class MessageTableTests(unittest.TestCase):

    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.addCleanup(os_helper.rmtree, self.dir)

    def write_table(self, *lines):
        path = os.path.join(self.dir, "lookup-table.txt")
        with open(path, "w", encoding="utf-8", newline="") as fh:
            fh.write("\n".join(lines) + "\n")
        return path

    def child_env(self, table):
        # CI sets FORCE_COLOR (.github/workflows/build.yml), which colorizes the
        # tracebacks below and would break every assertion on their text;
        # PYTHON_COLORS takes precedence over it.
        return dict(os.environ, PYTHON_MESSAGE_TABLE=table, PYTHON_COLORS="0")

    def run_script(self, table, code):
        """Run -c code with the table; returns (rc, stdout, stderr)."""
        env = self.child_env(table)
        proc = subprocess.run([sys.executable, "-c", code],
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              env=env)
        return (proc.returncode, proc.stdout.decode('utf-8', 'replace'),
                proc.stderr.decode('utf-8', 'replace'))

    def run_repl(self, table, source):
        """Feed stdin to the interactive interpreter; returns its output."""
        env = self.child_env(table)
        proc = subprocess.run([sys.executable, "-i", "-q"],
                              input=source.encode(), stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, env=env)
        return proc.stdout.decode('utf-8', 'replace')

    def test_literal_key(self):
        table = self.write_table(entry("division by zero", "笨蛋才会把0放到除数吧！"))
        rc, out, err = self.run_script(table, "1/0")
        self.assertNotEqual(rc, 0)
        self.assertIn("笨蛋才会把0放到除数吧！", err)
        self.assertNotIn("division by zero", err)

    def test_pattern_key_covers_every_value(self):
        table = self.write_table(
            entry("'*' object has no attribute '*'", "'%1' 对象没有属性 '%2'"))
        for code, expected in (("(1).foo", "'int' 对象没有属性 'foo'"),
                               ("'x'.bar", "'str' 对象没有属性 'bar'")):
            _, _, err = self.run_script(table, code)
            self.assertIn(expected, err)
            self.assertNotIn("has no attribute", err)

    def test_message_absent_from_the_table(self):
        table = self.write_table(entry("division by zero", "换掉了"))
        _, _, err = self.run_script(table, "raise ValueError('not in table')")
        self.assertIn("ValueError: not in table", err)

    def test_table_missing_is_not_fatal(self):
        missing = os.path.join(self.dir, "does-not-exist.txt")
        _, _, err = assert_python_failure(
            "-c", "1/0", PYTHON_MESSAGE_TABLE=missing, PYTHON_COLORS="0")
        self.assertIn(b"ZeroDivisionError: division by zero", err)

    def test_keyerror_display_is_replaced(self):
        # KeyError displays repr(args[0]).  Rewriting args would print the new
        # text with a pair of quotes around it, so the display proxy carries
        # the text instead.
        table = self.write_table(entry("'missing'", "换掉了"))
        rc, out, err = self.run_script(table, "{}['missing']")
        self.assertNotEqual(rc, 0)
        self.assertIn("KeyError: 换掉了", err)
        self.assertNotIn("KeyError: 'missing'", err)

    def test_oserror_with_errno_is_replaced(self):
        # The display is built from errno/strerror/filename, so args[0] is not
        # where the text comes from: the captures of the pattern fill the
        # value, and the display proxy shows it.
        table = self.write_table(
            entry("[Errno *] *: '*'", "[错误码 %1] %2: '%3'"))
        rc, out, err = self.run_script(table, "open('no-such-file-xyz')")
        self.assertNotEqual(rc, 0)
        self.assertIn("FileNotFoundError: [错误码 2] No such file or directory:"
                      " 'no-such-file-xyz'", err)

    def test_unicode_decode_error_is_replaced(self):
        table = self.write_table(entry(
            "'utf-8' codec can't decode byte 0xff in position 0:"
            " invalid start byte", "解码失败"))
        rc, out, err = self.run_script(table, "b'\\xff'.decode('utf-8')")
        self.assertNotEqual(rc, 0)
        self.assertIn("UnicodeDecodeError: 解码失败", err)
        self.assertNotIn("codec can't decode", err)

    def test_type_with_its_own_str_is_replaced(self):
        table = self.write_table(entry("weird message", "很奇怪的消息"))
        code = ("class Weird(Exception):\n"
                "    def __str__(self):\n"
                "        return 'weird message'\n"
                "raise Weird()\n")
        _, _, err = self.run_script(table, code)
        self.assertIn("Weird: 很奇怪的消息", err)

    def test_multi_argument_display_keeps_its_text(self):
        # The lookup runs on args[0], but the display is the repr of the whole
        # tuple, so replacing it would drop the other arguments.
        table = self.write_table(entry("a", "换掉了"))
        _, _, err = self.run_script(table, "raise ValueError('a', 'b')")
        self.assertIn("ValueError: ('a', 'b')", err)
        self.assertNotIn("换掉了", err)

    def test_syntax_error_with_a_location_keeps_its_text(self):
        # Multi-argument SyntaxError is a known defect (see FORK-NOTICE.md).
        # The display proxy must not turn it into a bare replacement either,
        # which would drop the file, the line and the caret.
        table = self.write_table(entry("invalid syntax", "语法无效"))
        _, _, err = self.run_script(table, "compile('1 +', '<demo>', 'exec')")
        self.assertIn("SyntaxError: invalid syntax", err)
        self.assertNotIn("语法无效", err)

    def test_key_may_start_with_a_hash(self):
        # A leading '#' is a comment only without a TAB; dropping this entry
        # because of the '#' would lose it silently.
        table = self.write_table(entry("# channels not specified", "频道没写啊"))
        _, _, err = self.run_script(
            table, "raise ValueError('# channels not specified')")
        self.assertIn("频道没写啊", err)

    def test_blanks_around_the_separator_are_trimmed(self):
        value = "空格外也认得"
        table = self.write_table(f"  division by zero \t {value.encode().hex()}  ")
        _, _, err = self.run_script(table, "1/0")
        self.assertIn(value, err)

    def test_old_comma_format_is_skipped(self):
        # The separator changed from ',' to TAB with no compatibility layer.
        table = self.write_table(
            "division by zero,{}".format("换掉了".encode().hex()))
        rc, out, err = self.run_script(table, "1/0")
        self.assertIn("division by zero", err)
        self.assertNotIn("换掉了", err)

    def test_repl_uses_the_table(self):
        # The interactive loop does not go through sys.excepthook.
        table = self.write_table(entry("division by zero", "REPL里也换掉了"))
        out = self.run_repl(table, "1/0\n")
        self.assertIn("REPL里也换掉了", out)
        self.assertNotIn("division by zero", out)


if __name__ == "__main__":
    unittest.main()
