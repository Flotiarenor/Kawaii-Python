"""Tests for the replaceable-message table of this fork.

The table is an external file, read once per interpreter and cached there, so
most cases run the interpreter again with PYTHON_MESSAGE_TABLE pointing at a
table written for that case.  Exercising the lookup in-process is not possible:
the replacement happens inside _PyErr_Display().

Properties covered here, beyond "the replacement shows up":

* a message the table does not cover must come out exactly as upstream prints
  it, and a table that cannot be read anywhere must not stop the interpreter
  from printing anything;
* a type whose display is not derived from args[0] must still show the
  replacement -- it is carried by a display proxy, a subclass whose __str__
  returns the table's text -- while a display the table only saw part of (the
  repr of a multi-argument tuple, a SyntaxError carrying a location) must keep
  its original text;
* the table is input from a path that may not be trusted, so its reader has to
  be robust: long lines, NUL bytes, 0x1A bytes, non-UTF-8 bytes, enormous files
  and hostile patterns are all handled without hanging, crashing or silently
  printing text nobody wrote;
* the interactive loop must be covered too, because it never calls
  sys.excepthook -- that is the whole reason this feature lives in C;
* the cache is per interpreter and holds no strong reference to user classes,
  so subinterpreters do not share a table and dynamically built exception
  classes are still collectable.
"""

import os
import subprocess
import sys
import tempfile
import textwrap
import unittest

from test.support import os_helper, requires_subprocess

# The table ships inside the installer, so an installed fork has a table in
# sys.prefix.  A source checkout has the same file at its root, but the
# interpreter skips the shipped-table candidate there -- it would otherwise
# rewrite the messages of this very test suite.  The marker is the one
# sysconfig.is_python_build() uses.
def _is_source_tree(prefix):
    return any(os.path.isfile(os.path.join(prefix, "Modules", name))
               for name in ("Setup", "Setup.local"))


PREFIX_TABLE = os.path.join(sys.prefix, "lookup-table.txt")
HAS_PREFIX_TABLE = (os.path.exists(PREFIX_TABLE)
                    and not _is_source_tree(sys.prefix))


def entry(key, value):
    """One table line: key, TAB, the replacement as hex-encoded UTF-8."""
    return f"{key}\t{value.encode('utf-8').hex()}"


def no_table_reachable():
    return HAS_PREFIX_TABLE


@requires_subprocess()
class MessageTableTests(unittest.TestCase):

    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.addCleanup(os_helper.rmtree, self.dir)
        self.table = os.path.join(self.dir, "lookup-table.txt")

    def write_table(self, *lines, name="lookup-table.txt", directory=None):
        path = os.path.join(directory or self.dir, name)
        with open(path, "w", encoding="utf-8", newline="") as fh:
            fh.write("\n".join(lines) + "\n")
        return path

    def write_bytes(self, payload, name="lookup-table.txt"):
        path = os.path.join(self.dir, name)
        with open(path, "wb") as fh:
            fh.write(payload)
        return path

    def child_env(self, table, **extra):
        # CI sets FORCE_COLOR (.github/workflows/build.yml), which colorizes the
        # tracebacks below and would break every assertion on their text;
        # PYTHON_COLORS takes precedence over it.
        env = dict(os.environ, PYTHON_COLORS="0", **extra)
        if table is None:
            env.pop("PYTHON_MESSAGE_TABLE", None)
        else:
            env["PYTHON_MESSAGE_TABLE"] = table
        return env

    def run_script(self, table, code, cwd=None, flags=(), **extra):
        """Run -c code with the table; returns (rc, stdout, stderr)."""
        proc = subprocess.run([sys.executable, *flags, "-c", textwrap.dedent(code)],
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              cwd=cwd or self.dir,
                              env=self.child_env(table, **extra))
        return (proc.returncode, proc.stdout.decode('utf-8', 'replace'),
                proc.stderr.decode('utf-8', 'replace'))

    def run_repl(self, table, source, cwd=None):
        """Feed stdin to the interactive interpreter; returns its output."""
        proc = subprocess.run([sys.executable, "-i", "-q"], input=source.encode(),
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              cwd=cwd or self.dir, env=self.child_env(table))
        return proc.stdout.decode('utf-8', 'replace')

    # --- the basics --------------------------------------------------

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
        code = """\
            class Weird(Exception):
                def __str__(self):
                    return 'weird message'
            raise Weird()
            """
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

    def test_empty_replacement_silences_the_message(self):
        # An empty value is an entry, not a malformed line: it maps the message
        # to nothing, which is how a table silences a message on purpose.
        table = self.write_table("division by zero\t")
        _, _, err = self.run_script(table, "1/0")
        self.assertIn("ZeroDivisionError", err)
        self.assertNotIn("division by zero", err)

    def test_repl_uses_the_table(self):
        # The interactive loop does not go through sys.excepthook.
        table = self.write_table(entry("division by zero", "REPL里也换掉了"))
        out = self.run_repl(table, "1/0\n")
        self.assertIn("REPL里也换掉了", out)
        self.assertNotIn("division by zero", out)

    # --- where the table is looked for -------------------------------

    def test_missing_env_table_falls_through(self):
        # The candidates are a real fallback chain: an environment variable that
        # points at a file which cannot be read does not stop the walk.  Which
        # of the later candidates wins depends on the layout (a build tree has
        # the table in sys.prefix, an installed fork ships it there), so this
        # only asks that the message was replaced by something.
        self.write_table(entry("division by zero", "从当前目录找到"))
        missing = os.path.join(self.dir, "does-not-exist.txt")
        _, _, err = self.run_script(missing, "1/0")
        self.assertNotIn("division by zero", err)
        self.assertIn("ZeroDivisionError", err)

    @unittest.skipIf(HAS_PREFIX_TABLE, "sys.prefix holds a table of its own")
    def test_cwd_table_is_used_without_the_variable(self):
        self.write_table(entry("division by zero", "当前目录"))
        _, _, err = self.run_script(None, "1/0")
        self.assertIn("当前目录", err)

    def test_env_table_wins_over_cwd(self):
        env_table = self.write_table(entry("division by zero", "环境变量"),
                                     name="env-table.txt")
        self.write_table(entry("division by zero", "当前目录"))
        _, _, err = self.run_script(env_table, "1/0")
        self.assertIn("环境变量", err)
        self.assertNotIn("当前目录", err)

    def test_non_ascii_path_in_the_variable(self):
        # The variable is read from the C environment, which on Windows holds
        # bytes in the ANSI code page; decoding it as UTF-8 would silently drop
        # the table for any path outside ASCII.
        directory = os.path.join(self.dir, "表目录-ünïcode")
        os.mkdir(directory)
        table = self.write_table(entry("division by zero", "非 ASCII 路径"),
                                 directory=directory)
        _, _, err = self.run_script(table, "1/0")
        self.assertIn("非 ASCII 路径", err)

    def test_table_may_appear_after_the_first_lookup(self):
        # A lookup that found no table must not be remembered as a failure: the
        # next one has to look again.  The variable is pointed at a file that
        # does not exist yet and is written between the two displays.
        late = os.path.join(self.dir, "late-table.txt")
        code = """\
            import ctypes, os

            api = ctypes.pythonapi
            api._PyErr_Message_Replace.argtypes = [ctypes.py_object]
            api._PyErr_Message_Replace.restype = ctypes.c_void_p

            def show():
                # NULL means "nothing was replaced", which ctypes would report
                # as an error if the restype were py_object.
                address = api._PyErr_Message_Replace(ValueError('boom'))
                text = (ctypes.cast(address, ctypes.py_object).value
                        if address else 'unchanged')
                print('display:', text)

            show()
            with open(os.environ['PYTHON_MESSAGE_TABLE'], 'w',
                      encoding='utf-8') as fh:
                fh.write('boom\\t' + '后来才写好的'.encode().hex() + '\\n')
            show()
            """
        rc, out, err = self.run_script(late, code)
        self.assertEqual(rc, 0, err)
        self.assertIn("display: unchanged", out)        # nothing there yet
        self.assertIn("后来才写好的", out)                # picked up later

    @unittest.skipIf(HAS_PREFIX_TABLE, "this build ships a table in sys.prefix")
    def test_no_table_reachable_is_not_fatal(self):
        missing = os.path.join(self.dir, "does-not-exist.txt")
        _, _, err = self.run_script(missing, "1/0")
        self.assertIn("ZeroDivisionError: division by zero", err)

    @unittest.skipIf(HAS_PREFIX_TABLE, "this build ships a table in sys.prefix")
    def test_isolated_mode_ignores_the_current_directory(self):
        # -I must not have its error messages rewritten by a file somebody
        # else may have written into the working directory.
        self.write_table(entry("division by zero", "当前目录"))
        _, _, err = self.run_script(None, "1/0", flags=("-I",))
        self.assertIn("division by zero", err)

    def test_xoption_turns_the_feature_off(self):
        table = self.write_table(entry("division by zero", "换掉了"))
        _, _, err = self.run_script(table, "1/0",
                                    flags=("-X", "message-table=off"))
        self.assertIn("division by zero", err)
        self.assertNotIn("换掉了", err)

    # --- robustness of the reader ------------------------------------

    def test_long_line_is_skipped_not_truncated(self):
        # A line past the limit is dropped whole; the entries around it still
        # work.  Truncating it would print the first half of a replacement.
        filler = "x" * 20000
        table = self.write_table(entry("division by zero", filler),
                                 entry("list index out of range", "还在"))
        _, _, err = self.run_script(table, "[][0]")
        self.assertIn("还在", err)

    def test_long_but_legal_line_is_read(self):
        value = "长" * 1000                      # 3000 bytes, 6000 hex digits
        table = self.write_table(entry("division by zero", value))
        _, _, err = self.run_script(table, "1/0")
        self.assertIn(value, err)

    def test_line_with_a_nul_is_skipped(self):
        payload = (b"division by zero\t61\x00" b"62\n"
                   b"list index out of range\t" + "还在".encode().hex().encode() + b"\n")
        table = self.write_bytes(payload)
        _, _, err = self.run_script(table, "[][0]")
        self.assertIn("还在", err)
        _, _, err = self.run_script(table, "1/0")
        self.assertIn("division by zero", err)

    def test_substitute_byte_is_not_the_end_of_the_file(self):
        # 0x1A ends the file in the C runtime's text mode; the table is opened
        # in binary mode so the entries after it survive.
        payload = (b"division by zero\t" + "前".encode().hex().encode() + b"\n"
                   b"\x1a\n"
                   b"list index out of range\t" + "后".encode().hex().encode() + b"\n")
        table = self.write_bytes(payload)
        _, _, err = self.run_script(table, "[][0]")
        self.assertIn("后", err)

    def test_invalid_utf8_value_drops_the_entry(self):
        # A value that is not UTF-8 is a broken entry, not text to print with
        # replacement characters in it.
        table = self.write_table("division by zero\t61ff62")
        _, _, err = self.run_script(table, "1/0")
        self.assertIn("division by zero", err)
        self.assertNotIn("\ufffd", err)

    def test_oversized_table_is_refused(self):
        # The loader runs while an exception is displayed, so a table far
        # larger than a message table can be is refused before it is read.
        path = os.path.join(self.dir, "lookup-table.txt")
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(entry("division by zero", "换掉了") + "\n")
            fh.write("# padding\n" * (2 * 1024 * 1024))    # > 16 MiB
        _, _, err = self.run_script(path, "1/0")
        # Whichever candidate wins afterwards, the oversized table's own entry
        # must not be the one that is shown.
        self.assertNotIn("换掉了", err)

    def test_entry_cap_keeps_the_first_entries(self):
        lines = [entry("division by zero", "第一")]
        lines += [entry(f"never printed {i}", "x") for i in range(70000)]
        lines += [entry("list index out of range", "最后")]
        table = self.write_table(*lines)
        _, _, err = self.run_script(table, "1/0")
        self.assertIn("第一", err)
        _, _, err = self.run_script(table, "[][0]")
        self.assertIn("list index out of range", err)   # past the cap

    def test_malformed_lines_are_reported_in_dev_mode(self):
        table = self.write_table(entry("division by zero", "换掉了"),
                                 "no separator here",
                                 "bad hex\tzz")
        _, _, err = self.run_script(table, "1/0", flags=("-X", "dev"))
        self.assertIn("malformed line(s)", err)
        _, _, err = self.run_script(table, "1/0")
        self.assertNotIn("malformed line(s)", err)

    def test_hostile_patterns_stay_fast(self):
        # The pattern budget is per lookup, not per pattern: a table full of
        # patterns that backtrack must not add up to minutes of work.
        pattern = "*a" * 40 + "*b"
        lines = [entry(pattern, "x") for _ in range(500)]
        table = self.write_table(*lines)
        code = "raise ValueError('a' * 200)"
        _, _, err = self.run_script(table, code)
        self.assertIn("ValueError", err)

    # --- the display proxy -------------------------------------------

    def test_stateful_str_cannot_bypass_the_table(self):
        # A __str__ that answers differently on every call used to pass the
        # equality check and then print text the table never asked for.  A type
        # with its own __str__ now always goes through the display proxy, whose
        # __str__ is deterministic.
        table = self.write_table(entry("boom", "REPLACEMENT-OK"))
        code = """\
            class Sneaky(Exception):
                calls = 0
                def __str__(self):
                    Sneaky.calls += 1
                    if Sneaky.calls == 1:
                        return 'boom'
                    if Sneaky.calls == 2:
                        return 'REPLACEMENT-OK'
                    return 'GATE-BYPASSED'
            raise Sneaky()
            """
        _, _, err = self.run_script(table, code)
        self.assertIn("REPLACEMENT-OK", err)
        self.assertNotIn("GATE-BYPASSED", err)

    def test_dynamic_exception_classes_are_not_kept_alive(self):
        # The proxy cache holds weak references, so a class built at run time
        # is collectable once the program drops it.
        table = self.write_table(*[entry(f"boom {i}", f"替换 {i}")
                                   for i in range(80)])
        code = """\
            import ctypes, gc, weakref

            def show(exc):
                try:
                    raise exc
                except Exception:
                    ctypes.pythonapi.PyErr_Print()

            refs = []
            for i in range(80):
                cls = type(f'Dynamic{i}', (Exception,),
                           {'__str__': lambda self, i=i: f'boom {i}'})
                refs.append(weakref.ref(cls))
                show(cls())
                del cls
            gc.collect()
            print('alive', sum(1 for ref in refs if ref() is not None))
            """
        rc, out, err = self.run_script(table, code)
        alive = int(out.split("alive")[-1].strip().splitlines()[0])
        self.assertLessEqual(alive, 2, out + err)

    # --- interpreter state -------------------------------------------

    def test_subinterpreter_does_not_share_the_table(self):
        # The cache belongs to the interpreter that built it: a table read in a
        # subinterpreter must not be handed to the main one (and the str objects
        # in it must not cross the boundary either).
        try:
            import _testcapi
        except ImportError:
            self.skipTest("_testcapi is not available")
        if not hasattr(_testcapi, "run_in_subinterp"):
            self.skipTest("run_in_subinterp is not available")
        table = self.write_table(entry("boom", "SUB"))
        code = """\
            import ctypes, os
            import _testcapi

            api = ctypes.pythonapi
            api._PyErr_Message_Replace.argtypes = [ctypes.py_object]
            api._PyErr_Message_Replace.restype = ctypes.c_void_p

            def replace(exc):
                address = api._PyErr_Message_Replace(exc)
                if not address:
                    return None
                return ctypes.cast(address, ctypes.py_object).value

            sub = '''
            import ctypes
            api = ctypes.pythonapi
            api._PyErr_Message_Replace.argtypes = [ctypes.py_object]
            api._PyErr_Message_Replace.restype = ctypes.c_void_p
            address = api._PyErr_Message_Replace(ValueError("boom"))
            print("sub:", ctypes.cast(address, ctypes.py_object).value
                  if address else None)
            '''
            _testcapi.run_in_subinterp(sub)
            with open(os.environ['PYTHON_MESSAGE_TABLE'], 'w',
                      encoding='utf-8') as fh:
                fh.write('boom\\t' + 'MAIN'.encode().hex() + '\\n')
            print('main:', replace(ValueError('boom')))
            """
        rc, out, err = self.run_script(table, code)
        self.assertEqual(rc, 0, err)
        self.assertIn("sub: SUB", out)          # the subinterpreter's own table
        self.assertIn("main: MAIN", out)        # rebuilt here, not inherited

    def test_threads_can_display_at_the_same_time(self):
        # The caches are reached from every thread that displays an exception;
        # this does not prove the absence of a race, but it does exercise the
        # paths that used to grow and free a shared array.
        table = self.write_table(*[entry(f"boom {i}", f"替换 {i}")
                                   for i in range(40)])
        code = """\
            import ctypes, sys, threading

            sys.setswitchinterval(1e-6)

            def worker():
                for i in range(40):
                    try:
                        raise ValueError(f'boom {i}')
                    except ValueError:
                        ctypes.pythonapi.PyErr_Print()

            threads = [threading.Thread(target=worker) for _ in range(8)]
            for thread in threads:
                thread.start()
            for thread in threads:
                thread.join()
            print('done')
            """
        rc, out, err = self.run_script(table, code)
        self.assertIn("done", out)

    def test_the_contract_leaves_no_exception_behind(self):
        # _PyErr_Message_Replace() hands the error indicator back the way it
        # found it; ctypes raises if the indicator is set when the call returns,
        # which is how a leftover UnicodeDecodeError from a bad environment
        # value used to show up.
        directory = os.path.join(self.dir, "表目录-ünïcode")
        os.mkdir(directory)
        table = self.write_table(entry("boom", "换掉了"), directory=directory)
        code = """\
            import ctypes

            api = ctypes.pythonapi
            api._PyErr_Message_Replace.argtypes = [ctypes.py_object]
            api._PyErr_Message_Replace.restype = None
            api._PyErr_Message_Replace(ValueError('boom'))
            api._PyErr_Message_Replace(ValueError('not in the table'))
            print('no leftover exception')
            """
        rc, out, err = self.run_script(table, code)
        self.assertEqual(rc, 0, err)
        self.assertIn("no leftover exception", out)


if __name__ == "__main__":
    unittest.main()
