# Fork notice

**This is an unofficial, personally modified copy of CPython.** It is not the
official Python distribution, and it is not supported or endorsed by the
Python Software Foundation.

| | |
| --- | --- |
| Baseline | [CPython v3.15.0](https://github.com/python/cpython/releases/tag/v3.15.0) |
| This repository | https://github.com/Flotiarenor/Kawaii-Python |
| Official CPython | https://github.com/python/cpython |
| Official downloads / docs | https://www.python.org/ · https://docs.python.org |
| License | PSF License Version 2, unchanged ([LICENSE](LICENSE)) |

Everything except the one feature below is upstream and unmodified: the build
system, the standard library, the test suite, and the license.

The design follows [kawaii-gcc](https://github.com/Bill-Haku/kawaii-gcc), which
replaces GCC's localization file with a Japanese `gcc.mo` (the idea started with
the Chinese project [gcc-hentai](https://github.com/Mosklia/gcc-hentai)).  CPython
has no message catalogue, so the same idea needs a hook in the interpreter and a
lookup table; that table ships with this repository and is part of what you
install.

This file records what differs from upstream and what was verified.  Building the
interpreter, producing the Windows installer, and the full format of the table are
in [docs/development.md](docs/development.md) (Chinese); the user-facing
introduction is [README.md](README.md) / [README-zh.md](README-zh.md).

## The feature

Exception messages can be replaced through an external text table.  The table is
a plain text file, one entry per line, `original message<TAB>hex-encoded UTF-8
replacement`; the separator is a tab, and the replacement text is Chinese with
full-width punctuation:

```
division by zero<TAB>e7aca8e89b8be6898de4bc9ae68a8a30e694bee588b0e999a4e695b0e590a7efbc81
```

A key containing `*` is a pattern instead of literal text: it matches the message
the exception displays, every `*` captures a run of characters, and the
replacement refers to the captures as `%1`, `%2`.  One pattern therefore covers
every value the interpreter can put into a message:

```
'*' object has no attribute '*'<TAB>2725312720e5afb9e8b1a1e6b2a1e69c89e5b19ee680a72027253227
```

```console
$ PYTHON_MESSAGE_TABLE=./lookup-table.txt ./python -c "1/0"
Traceback (most recent call last):
  File "<string>", line 1, in <module>
ZeroDivisionError: 笨蛋才会把0放到除数吧！
```

The table that ships here currently holds 4814 literal keys and 21 patterns
(`lookup-table.txt` is 4918 lines, 83 of them blank or comments), and every entry
quoted in this file or in the READMEs decodes to exactly the text shown there —
including `'int' object has no attribute 'foo'`, which is not a key but the
pattern above rendered with `int` and `foo` substituted into `%1` and `%2`.

## Where it differs from upstream

Ten upstream files touched, four added:

| File | Change |
| --- | --- |
| `Python/pythonrun.c` | hook in `_PyErr_Display()` (line 1149) |
| `Python/pylifecycle.c` | release the cached table in `_Py_Finalize()` (line 2401) |
| `Include/cpython/unicodeobject.h` | include the new header, last line of the file |
| `Makefile.pre.in`, `PCbuild/pythoncore.vcxproj`, `PCbuild/_freeze_module.vcxproj` | build the new source |
| `Tools/msi/common_en-US.wxl_template`, `Tools/msi/msi.targets`, `Tools/msi/exe/exe_en-US.wxl_template`, `Tools/msi/freethreaded/freethreaded_en-US.wxl_template` | the installer's identity strings: display name `Kawaii-Python 3.15.0 (64-bit)`, publisher `Kawaii-Python`, and `SupportUrl` pointing at this repository, via a marker-free `{{PyVersion}}` token |
| **added** `Objects/message_table.c` | table loading, hex decoding, lookup, rewriting, the display proxy, and the "can this message be replaced" decision |
| **added** `Include/message_table.h` | declarations |
| **added** `lookup-table.txt` | the shipped table — the deliverable itself |
| **added** `Lib/test/test_message_table.py` | this fork's tests for the table, the gate, the display proxy and the REPL entry point |

`_PyErr_Display()` is the single funnel every traceback goes through — the
REPL, `PyErr_Print`/`sys.excepthook`, `sys.__excepthook__`, and the C fallback
that runs when the `traceback` module is unavailable.  Hooking there covers all
of them at once.  The interactive loop is the reason this cannot be done in
Python at all: it never calls `sys.excepthook`, so a pure-Python implementation
cannot reach that case.

The implementation is plain C on public API only: it does not import Python
code, so a traceback stays printable even if the import machinery is broken at
that moment.  The new symbols (`_PyErr_Message_Replace`, `_PyMessageTable_Reload`,
`_PyMessageTable_Fini`) carry the `_Py` prefix and stay internal; an exported
symbol without the underscore is treated by upstream's "Check if all C APIs are
documented" job as a new public C API, which is why the hex decoder was made
`static` in b41bab3.

## Using it

The table is looked up in this order:

1. the file named by `$PYTHON_MESSAGE_TABLE`;
2. `$PYTHONHOME/lookup-table.txt` — this reads the **environment variable** `PYTHONHOME`, not
   `sys.prefix`, so on an ordinary installation this step never matches;
3. `lookup-table.txt` in the current working directory (`message_table_open()` opens a relative
   path with `fopen`).

The table in the repository root is the deliverable, tracked and distributed with the source; the
interpreter itself does not probe that path, so when you run a build from `PCbuild/amd64/` either
set `$PYTHON_MESSAGE_TABLE` or start the process in the repository root.  **Copying the table into
the interpreter's own directory does not work** — that directory is `sys.prefix`, which step 2 only
consults if `PYTHONHOME` happens to point there.  The portable archives appear to pick the table up
automatically because it sits in the directory they are extracted to, which is the working
directory for anyone who starts the interpreter from there (step 3).

Format: UTF-8, one entry per line, blank lines and lines starting with `#` are
ignored, the key and the value are separated by a tab.  A key may contain
commas, spaces, quotes and brackets — everything except a tab or a line break,
because a key has to equal the message text exactly — so the two sides are
separated by the one character a message never contains.  The file is kept pure
ASCII: an editor that saves it as UTF-16 would invalidate every line, and one
that adds a BOM would prepend invisible characters to the first key.

A key containing `*` is a pattern: matching is leftmost and non-greedy (every `*`
takes the shortest run that lets the rest match), a literal key always wins over a
pattern, and among patterns the one with the fewest `*` wins — an equal count is
decided by the earlier line.  Generate the hex side with:

```python
print(input().encode('utf-8').hex())
```

The table is read once, on first use, and stays in memory.  A missing or
malformed table degrades silently — the original message is always printed.
Line-level mistakes fail the same way, so when an entry seems to do nothing,
check the path first and the separator second.

## What it cannot do

Whether a message can be replaced is decided in three steps.  First the text to
look up is chosen: if the type uses the standard `__str__` (compared by `tp_str`,
no allocation) the message is `args[0]`; otherwise it is whatever the exception
displays.  A pattern key can therefore reach types whose display is not
`args[0]` at all.  Second, once a replacement is built, the copy is asked for its
display again: the rewrite is kept only if that display is exactly the new text
(`SyntaxError` is the one exception, it may continue with the location it
renders).  Without that second step the copy would print something nobody asked
for — a `KeyError` would show `repr()` of the new text, an `OSError` carrying
`errno` would show a tuple, and a `UnicodeDecodeError` copy would show nothing at
all.  Third, for exactly those types, the text is carried by a **display proxy**:
a copy whose type is a subclass of the exception's own type whose `__str__`
returns the replacement.  A subclass rather than a fresh exception class keeps
the printed name (`__qualname__` and `__module__` for `traceback`, `tp_name` for
the C fallback) and every `isinstance()` check on the object that is displayed.
The subclass is created once per base type and cached, and it is built from
public C API only (`type()`, `PyDescr_NewMethod()`), with no import.

| Exception | What happens | Why |
| --- | --- | --- |
| `ZeroDivisionError`, `IndexError`, `TypeError`, `AttributeError`, `NameError`, `RecursionError`, `ImportError`, … | replaced | the display is `args[0]` |
| subclasses that inherit the standard `__str__` | replaced | same |
| `SyntaxError` **with a single argument** | replaced | the display is `args[0]`; the multi-argument form the compiler produces (`compile()`, `exec()`, an imported file) is **not** replaced — see "Known defect" below, the proxy is skipped there on purpose |
| `KeyError` | replaced through a display proxy | the display is `repr(args[0])`, which rewriting `args` cannot change |
| `OSError` / `FileNotFoundError`, … with `errno` | replaced through a display proxy | the display is built from `errno`/`strerror`/`filename`, none of which an `args` rewrite reaches |
| `UnicodeDecodeError` (and the other `UnicodeError` shapes with their own `__str__`) | replaced through a display proxy | the display is built from `encoding`/`object`/`start`/`end`/`reason`, and a copy made with `__new__` has no `object` member, so it displays an empty string |
| subclasses that override `__str__` | replaced through a display proxy | the display is fixed text that does not mention `args` |
| `Exception(1, 2)`, `ValueError('a', 'b')` and other multi-argument exceptions | found in the table, then left alone | the lookup saw `args[0]` (or the `str()` of the tuple when that is not a string), while the display is the repr of the whole tuple: replacing it would drop the arguments the key never covered |

"Found in the table, then left alone" is the intended outcome: the entry is
reachable, and the rewrite is discarded because it would print something worse
than the original.  `Objects/exceptions.c:3867` (empty `UnicodeDecodeError`
copy) and `:2927` (where `SyntaxError` renders its location) are the upstream
lines behind the two shapes that still keep their text.

## Known defect: multi-argument SyntaxError is never replaced

`SyntaxError` reaches the table in two shapes, and only one of them works:

| Shape | `args` | Replaced? |
| --- | --- | --- |
| `raise SyntaxError('invalid syntax')` | `('invalid syntax',)` | yes |
| `raise SyntaxError('invalid syntax', ('<demo>', 1, 4, '1 +'))`, and everything the parser produces — `compile()`, `exec()`, `import`, REPL input | `('invalid syntax', ('<demo>', 1, 4, ...))` | **no** |

Reproduce (table containing `invalid syntax<TAB>` plus the hex of any text):

```console
$ python -c "raise SyntaxError('invalid syntax')"
SyntaxError: 【替换】            # works
$ python -c "compile('1 +', '<demo>', 'exec')"
SyntaxError: invalid syntax     # not replaced
```

`_PyErr_Message_Replace()` returns `NULL` for the second shape.  Calling it
directly (ctypes, `python315.dll`) on a `SyntaxError` whose `filename` and
`lineno` are both non-`NULL` reproduces it; setting only `offset`, `text`,
`end_lineno` or `end_offset` still succeeds.  Replaying
`exception_with_message()`'s steps in Python — `__new__`, replace `args[0]`,
set `.msg`, copy the six location fields, then compare `str(copy)` with the
replacement — succeeds and yields `【替换】 (<demo>, line 1)`, so the divergence
is inside the C path and not in the gate's expectation.  Not diagnosed further;
no fix attempted.

Impact: syntax errors in everyday use are the multi-argument form, so the
documented `SyntaxError` replacement effectively does not happen for them.  The
single-argument form has no location to lose, so the earlier claim that a
rewrite keeps file and line was describing a case that cannot occur — the
README now states which shape is covered.

The display proxy that carries replacements for the other types deliberately
skips `SyntaxError`: its `__str__` also renders the file, the line and the
caret, so a proxy would print the bare replacement and drop all three.  The
multi-argument shape therefore still keeps its original text, location
included.

## Verified

Built and tested on Windows x64 Release (VS2022, MSC 19.44) against v3.15.0.
Every row below was re-checked on a Release x64 build of the current tree
(`MSC v.1944`, 3.15.0, `heads/main:a2a6516`), together with the wildcard pattern
engine, the display gate and the tab separator; the rows that go through the
display proxy were re-checked on the build that added it.

| Case | Result |
| --- | --- |
| no table configured | `ZeroDivisionError: division by zero` (unchanged) |
| `python -c "1/0"` | `ZeroDivisionError: 笨蛋才会把0放到除数吧！` (literal key) |
| `python -c "[][0]"` | `IndexError: list后面的地方，不……不可以再看了，会坏掉的，呜` |
| `python -c "(1).foo"` | `AttributeError: 'int' 对象没有属性 'foo'` (pattern key, captures `int` and `foo`) |
| interactive `>>> 1/0` and `>>> (1).foo` | both replaced — the interactive loop never calls `sys.excepthook` |
| `raise SyntaxError('msg')`, script or REPL | replaced (the single-argument shape) |
| `compile('1 +', '<demo>', 'exec')`, REPL `1 +` | `SyntaxError: invalid syntax` — the known defect above, the proxy is skipped there |
| `raise ValueError('not in table')` | unchanged |
| `raise ValueError('a', 'b')` with a key for `a` | `ValueError: ('a', 'b')` — the display is the tuple, not the matched text |
| `open('no-such-file-xyz')`, key `[Errno *] *: '*'` | `FileNotFoundError: [错误码 2] No such file or directory: 'no-such-file-xyz'` |
| `open('other-file')`, same pattern key | `FileNotFoundError: [错误码 2] No such file or directory: 'other-file'` — the captures come from the display |
| `class MyMissing(FileNotFoundError)` raised | `MyMissing: [错误码 2] ...` — the proxy is built from the raised type, not from `OSError` |
| `b'\xff'.decode('utf-8')`, shipped key `'*' codec can't decode byte * in position *: *` | `UnicodeDecodeError: 'utf-8' 解码器无法解码第 0 位的字节 0xff: invalid start byte` — captures `utf-8`, `0xff` and `0`, the reason stays as it came |
| `b'\x81'.decode('gbk')`, same shipped key | `UnicodeDecodeError: 'gbk' 解码器无法解码第 0 位的字节 0x81: incomplete multibyte sequence` |
| `{}['missing']`, key `'missing'` (quotes included) | `KeyError: 换掉了` — no quotes are added |
| subclass with its own `__str__`, key `weird message` | `Weird: 很奇怪的消息` |
| the same four proxy cases through `sys.__excepthook__(type(e), e, e.__traceback__)` | identical text — every entry point reaches the hook |
| a type whose `__init_subclass__` raises | original message kept, no error — building the proxy failed and was abandoned |
| `PYTHON_MESSAGE_TABLE` pointing at a missing file | `division by zero`, unchanged |

`Lib/test/test_message_table.py` encodes these as 14 cases and runs under
`python -m test test_message_table`; on this build `test_message_table`,
`test_exceptions`, `test_traceback`, `test_codecs` and `test_os` all pass
(1 420 tests run in total).  The proxy rows above come from the build that added
it: Release x64, 0 warnings and 0 errors.  The `OSError` and `UnicodeDecodeError`
rows ran with the shipped `lookup-table.txt` (its `[Errno *] *: '*'` and
`'*' codec can't decode byte * in position *: *` entries), the `KeyError` and
custom-`__str__` rows with a two-line table, because the shipped one has no key
for those two texts.

The whole suite was run once on that build (`-m test -j8`, 8 min 38 s, 50 017
tests, 547 of 558 files).  11 files failed, 15 assertions in total, and none of
them come from the message table:

- `test_asyncio.test_events`, `test_proactor_events`, `test_sendfile`,
  `test_socket`, `test_urllib`, `test_urllib2`, `test_urllib2_localnet` —
  loopback sockets; the log is full of `[WinError 10048]`, the address being in
  use, which is this machine's port and firewall situation;
- `test_embed` — wants an installed layout (`_testembed.exe`, a real
  `pybuilddir.txt`), not a `PCbuild/amd64` tree;
- `test_sysconfig.test_symlink` and `test_platform.test_architecture_via_symlink`
  — want the `win-amd64` symlink directory a full build creates in `PCbuild`;
- `test_venv.test_venvwlauncher` — `pythonw.exe` is not built by
  `python.vcxproj`.

Two checks support that reading: renaming `lookup-table.txt` away, which leaves
the replacement feature dormant, reproduced **exactly the same 11 files and 15
assertions**; and grepping the 72 163-line log for the table's own wording
(`笨蛋`, `喵～`, `长点心`, `人家`, `杂鱼`, `离谱`) finds nothing — the only CJK in
it are Windows' own localized `OSError` strings.

## Upgrading to a newer CPython

Do not merge this tree with upstream: the fork and upstream share no common
commit history.  The feature is small, so the portable route is to re-apply it
on a fresh baseline:

```bash
git fetch upstream --tags
git switch -c 3.16 v3.16.0
git diff --name-only v3.15.0 cc632c6   # cc632c6: the v3.15.0 import, feature included
```

The differences to re-apply are the rows of the table under
[Where it differs from upstream](#where-it-differs-from-upstream): copy
`Objects/message_table.c` and `Include/message_table.h`, re-add the `#include`
at the end of `Include/cpython/unicodeobject.h`, re-add the three build-system
entries, and re-add the two call sites — one insertion in `_PyErr_Display`, one
call in `_Py_Finalize`.  The implementation file itself needs no changes.

## License

PSF License Version 2 — see [LICENSE](LICENSE) for the full text and the
history of the software.  Redistribution and modification are permitted; this
modified copy keeps the original license.  The file has not been altered.

## Reporting problems

This fork is not supported by the Python Software Foundation or the CPython
core team.  Problems with the message replacement feature belong in this
repository's tracker.  Anything else is an upstream issue:
https://github.com/python/cpython/issues
