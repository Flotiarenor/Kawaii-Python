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

The table that ships here currently holds 4814 literal keys and 20 patterns
(`lookup-table.txt` is 4917 lines, 83 of them blank or comments), and every entry
quoted in this file or in the READMEs decodes to exactly the text shown there —
including `'int' object has no attribute 'foo'`, which is not a key but the
pattern above rendered with `int` and `foo` substituted into `%1` and `%2`.

## Where it differs from upstream

Six upstream files touched, two added:

| File | Change |
| --- | --- |
| `Python/pythonrun.c` | hook in `_PyErr_Display()` (line 1149) |
| `Python/pylifecycle.c` | release the cached table in `_Py_Finalize()` (line 2401) |
| `Include/cpython/unicodeobject.h` | include the new header, last line of the file |
| `Makefile.pre.in`, `PCbuild/pythoncore.vcxproj`, `PCbuild/_freeze_module.vcxproj` | build the new source |
| **added** `Objects/message_table.c` | table loading, hex decoding, lookup, rewriting, and the "can this message be replaced" decision |
| **added** `Include/message_table.h` | declarations |

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

1. `$PYTHON_MESSAGE_TABLE`;
2. `$PYTHONHOME/lookup-table.txt`;
3. `lookup-table.txt` in the current directory.

The table in the repository root is the deliverable, tracked and distributed
with the source; the interpreter itself does not probe that path, so when you
run a build from `PCbuild/amd64/` either set `$PYTHON_MESSAGE_TABLE` or start the
process in the repository root.  The Windows installer does not place the table
next to the interpreter either — copy it to `sys.prefix` or point
`$PYTHON_MESSAGE_TABLE` at it.

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

Whether a message can be replaced is decided in two steps.  First the text to
look up is chosen: if the type uses the standard `__str__` (compared by `tp_str`,
no allocation) the message is `args[0]`; otherwise it is whatever the exception
displays.  A pattern key can therefore reach types whose display is not
`args[0]` at all.  Second, once a replacement is built, the copy is asked for its
display again: the rewrite is kept only if that display is exactly the new text
(`SyntaxError` is the one exception, it may continue with the location it
renders).  Without that second step the copy would print something nobody asked
for — a `KeyError` would show `repr()` of the new text, an `OSError` carrying
`errno` would show a tuple, and a `UnicodeDecodeError` copy would show nothing at
all.

| Exception | What happens | Why |
| --- | --- | --- |
| `ZeroDivisionError`, `IndexError`, `TypeError`, `AttributeError`, `NameError`, `RecursionError`, `ImportError`, … | replaced | the display is `args[0]` |
| subclasses that inherit the standard `__str__` | replaced | same |
| `SyntaxError` | replaced, location kept | text lives in `.msg`; `filename`, `lineno`, `offset`, `text` and the end positions are copied over, so the display is `new text (file, line N)` |
| `KeyError` | found in the table, then left alone | the display is `repr(args[0])`: the rewrite would print `'<new text>'` |
| `OSError` / `FileNotFoundError`, … with `errno` | found in the table, then left alone | the display is built from `errno`/`strerror`/`filename`; the copy would print a tuple and lose all three |
| `UnicodeDecodeError` | found in the table, then left alone | the display is built from `encoding/object/start/end/reason`, and a copy made with `__new__` has no `object` member, so it displays an empty string |
| `Exception(1, 2)` and other multi-argument exceptions | found in the table, then left alone | the display is the tuple repr |
| subclasses that override `__str__` | never matched, nothing to do | the display is fixed text that does not mention `args` |

"Found in the table, then left alone" is the intended outcome: the entry is
reachable, and the rewrite is discarded because it would print something worse
than the original.  `Objects/exceptions.c:3867` (empty `UnicodeDecodeError`
copy) and `:2927` (where `SyntaxError` renders its location) are the upstream
lines behind the two most surprising rows.

## Verified

Built and tested on Windows x64 Release (VS2022, MSC 19.44) against v3.15.0.
Every row below was re-checked on a Release x64 build of the current tree
(`MSC v.1944`, 3.15.0, `heads/main:a2a6516`), together with the wildcard pattern
engine, the display gate and the tab separator:

| Case | Result |
| --- | --- |
| no table configured | `ZeroDivisionError: division by zero` (unchanged) |
| `python -c "1/0"` | `ZeroDivisionError: 笨蛋才会把0放到除数吧！` (literal key) |
| `python -c "[][0]"` | `IndexError: list后面的地方，不……不可以再看了，会坏掉的，呜` |
| `python -c "(1).foo"` | `AttributeError: 'int' 对象没有属性 'foo'` (pattern key, captures `int` and `foo`) |
| interactive `>>> 1/0` and `>>> (1).foo` | both replaced — the interactive loop never calls `sys.excepthook` |
| `SyntaxError(..., (file, line, offset, text))` in the REPL | replaced, location kept |
| `compile('1 +', ...)` | message replaced where the table covers it, location kept either way |
| `raise ValueError('not in table')` | unchanged |
| `open('nope-xyz')` | `FileNotFoundError: [Errno 2] ...` (pattern matched, rewrite discarded by the gate) |
| `{}['missing']` | `KeyError: 'missing'` (same gate) |
| custom `__str__`, `OSError` subclass, `KeyError` | matched where a pattern covers them, rewrite discarded |
| `PYTHON_MESSAGE_TABLE` pointing at a missing file | `division by zero`, unchanged |

`Lib/test/test_message_table.py` encodes these as 10 cases and runs under
`python -m test test_message_table`; `test_exceptions`, `test_traceback`,
`test_sys` and `test_capi.test_exceptions` (684 tests) pass unchanged.

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
