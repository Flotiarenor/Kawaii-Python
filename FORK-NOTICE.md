# Fork notice

**This is an unofficial, personally modified copy of CPython.** It is not the
official Python distribution, and it is not supported or endorsed by the
Python Software Foundation.

| | |
| --- | --- |
| Baseline | [CPython v3.15.0](https://github.com/python/cpython/releases/tag/v3.15.0) |
| This repository | https://github.com/Flotiarenor/Python |
| Official CPython | https://github.com/python/cpython |
| Official downloads / docs | https://www.python.org/ · https://docs.python.org |
| License | PSF License Version 2, unchanged ([LICENSE](LICENSE)) |

Everything except the one feature below is upstream and unmodified: the build
system, the standard library, the test suite, and the license.

## The feature

Exception messages can be replaced through an external text table.  The table
is a plain text file, one entry per line, `original message,hex-encoded UTF-8
replacement`:

```
division by zero,e7aca8e89b8be6898de4bc9ae68a8a30e694bee588b0e999a4e695b0e590a721
```

```console
$ PYTHON_MESSAGE_TABLE=./lookup-table.txt ./python -c "1/0"
Traceback (most recent call last):
  File "<string>", line 1, in <module>
ZeroDivisionError: 笨蛋才会把0放到除数吧!
```

The interactive REPL is covered too, which is the reason this needs a C hook:
`sys.excepthook` is **not** called by the interactive loop, so a pure-Python
implementation cannot reach that case.

## Where it differs from upstream

Three files touched, two added:

| File | Change |
| --- | --- |
| `Python/pythonrun.c` | hook in `_PyErr_Display()` |
| `Python/pylifecycle.c` | release the cached table in `_Py_Finalize()` |
| `Include/cpython/unicodeobject.h` | include the new header |
| `Makefile.pre.in`, `PCbuild/pythoncore.vcxproj`, `PCbuild/_freeze_module.vcxproj` | build the new source |
| **added** `Objects/message_table.c` | table loading, hex decoding, lookup, rewriting, and the "can this message be replaced" decision |
| **added** `Include/message_table.h` | declarations |

`_PyErr_Display()` is the single funnel every traceback goes through — the
REPL, `PyErr_Print`/`sys.excepthook`, `sys.__excepthook__`, and the C fallback
that runs when the `traceback` module is unavailable.  Hooking there covers all
of them at once.

The implementation is plain C on public API only: it does not import Python
code, so a traceback stays printable even if the import machinery is broken at
that moment.

## Using it

The table is looked up in this order:

1. `$PYTHON_MESSAGE_TABLE`;
2. `$PYTHONHOME/lookup-table.txt`;
3. `lookup-table.txt` in the current directory.

Format: UTF-8, one entry per line, blank lines and lines starting with `#` are
ignored.  Generate the hex side with:

```python
print(input().encode('utf-8').hex())
```

The table is read once, on first use, and stays in memory.  A missing or
malformed table degrades silently — the original message is always printed.

## What it cannot do

`BaseException.__str__` derives the displayed text from `args[0]`, but some
types build it from dedicated fields, and rewriting `args` there would change
nothing.  Those instances are detected and skipped:

| Exception | Replaceable | Why |
| --- | --- | --- |
| `ZeroDivisionError`, `IndexError`, `TypeError`, `AttributeError`, `NameError`, `RecursionError`, `ImportError`, … | yes | the display is `args[0]` |
| subclasses that inherit the standard `__str__` | yes | same |
| `SyntaxError` | yes | text lives in `.msg`, location in `args[1]`; both are updated |
| `KeyError` | no | the display adds quotes |
| `OSError` / `FileNotFoundError`, … | no | the display gains an `[Errno 2] ` prefix from `errno` |
| `UnicodeDecodeError` | no | text is built from `encoding/object/start/end/reason` |
| subclasses that override `__str__` | no | the display is fixed text |

The decision is made per instance, in two steps: if the type uses the standard
`__str__` (compared by `tp_str`, no allocation) the message is taken straight
from `args[0]`; otherwise the real display is compared, and a message that the
display is not derived from is left alone.

## Building

Windows:

```powershell
cd PCbuild
.\get_externals.bat --no-tkinter
msbuild python.vcxproj /p:Configuration=Release /p:Platform=x64 /m
```

Do not use `msbuild pcbuild.sln /t:python`: `_tkinter` sits inside the `python`
target, and skipping Tcl/Tk makes it fail, which fails the whole target even
though `python.vcxproj` only needs `pythoncore.vcxproj`.

Unix, Linux, BSD, macOS:

```bash
./configure
make -j
make test
```

Copying a source tree with `robocopy` on Windows will hang: CPython ships
symlinks (`Misc/mypy/*`), which `robocopy` follows as directories.  Use `tar`
or `git clone`.

Upstream build documentation is not copied here; it applies unchanged and lives
at [python/cpython:README.rst](https://github.com/python/cpython/blob/v3.15.0/README.rst).

## Verified

Built and tested on Windows x64 Release (VS2022, MSC 19.44) against
v3.15.0 — details in [patches/3.15/VERIFIED.txt](patches/3.15/VERIFIED.txt):

| Case | Result |
| --- | --- |
| no table configured | `ZeroDivisionError: division by zero` (unchanged) |
| `python -c "1/0"` | `ZeroDivisionError: 笨蛋才会把0放到除数吧!` |
| `python -c "[][0]"` | `IndexError: list后面的地方,不...不可以再看了,会坏掉的,呜` |
| interactive `>>> 1/0` | replaced |
| `SyntaxError(..., (file, line, offset, text))` in the REPL | replaced, location kept |
| message not in the table | unchanged |
| `open('/nope')` | `FileNotFoundError: [Errno 2] ...` (correctly skipped) |
| custom `__str__`, `OSError` subclass, `KeyError` | correctly skipped |

## Upgrading to a newer CPython

Do not merge this tree with upstream.  The feature is small, so the portable
route is to re-apply it on a fresh baseline; the ready-made patches live in
[patches/](patches/):

```bash
git fetch upstream --tags
git switch -c 3.16 v3.16.0
cp patches/3.15/message_table.c  Objects/message_table.c
cp patches/3.15/message_table.h  Include/message_table.h
git apply -p1 --3way patches/3.15/attach.patch
git apply -p1 --3way patches/3.15/build-integration.patch
```

When `_PyErr_Display` or the surrounding lines change upstream, the patch may
need a hand: it is one insertion at the top of that function plus one call in
`_Py_Finalize`, and the implementation file itself needs no changes.

## License

PSF License Version 2 — see [LICENSE](LICENSE) for the full text and the
history of the software.  Redistribution and modification are permitted; this
modified copy keeps the original license.  The file has not been altered.

## Reporting problems

This fork is not supported by the Python Software Foundation or the CPython
core team.  Problems with the message replacement feature belong in this
repository's tracker.  Anything else is an upstream issue:
https://github.com/python/cpython/issues
