# Kawaii-Python — make Python's error messages cute

**Replace Python's error messages with words you wrote yourself.**

> **This is not the official Python distribution.** It is a personal modification of
> CPython v3.15.0, and it is not supported or endorsed by the Python Software Foundation.
> Except for the one thing below — the message text — everything is upstream and
> unmodified. The Chinese edition is [README-zh.md](README-zh.md).

It used to look like this:

```console
$ python -c "1/0"
Traceback (most recent call last):
  File "<string>", line 1, in <module>
    1/0
    ~^~
ZeroDivisionError: division by zero
```

With Kawaii-Python installed it looks like this:

```console
$ python -c "1/0"
Traceback (most recent call last):
  File "<string>", line 1, in <module>
    1/0
    ~^~
ZeroDivisionError: 笨蛋才会把0放到除数吧！
```

The text lives in `lookup-table.txt` in the repository root, one entry per line, and you change
whichever lines you like. The idea comes from [kawaii-gcc](https://github.com/Bill-Haku/kawaii-gcc),
which replaces GCC's localization file with a rewritten `gcc.mo`; that project in turn started with
the Chinese [gcc-hentai](https://github.com/Mosklia/gcc-hentai).

## Download and install

Get the package for your platform from
[Releases](https://github.com/Flotiarenor/Kawaii-Python/releases/latest):

| Platform | Artifact | How to install |
| --- | --- | --- |
| Windows x64 | `Kawaii-Python-3.15.0-windows-x64.exe` | Double-click, same as installing official Python |
| Windows x64 | `Kawaii-Python-3.15.0-windows-x64.zip` | Portable build, unzip and run |
| Linux x64 | `Kawaii-Python-3.15.0-linux-x64.tar.gz` | Unpack and run the `bin/python3` inside |
| Linux x64 | `Kawaii-Python-3.15.0-linux-x64.zip` | Same, different compression |

The Windows installer is produced by CPython's own WiX project, so it behaves like the official
one: it installs into `%LocalAppData%\Programs\Python\Python315` for the current user, offers the
"add to PATH" option, and can be uninstalled from Apps & features. It installs **alongside official
Python 3.15 and does not replace the Python you already have**.

> The installer is not code-signed, so Windows SmartScreen may block the first run: click
> "More info" → "Run anyway".

The installer puts the table where the interpreter looks, so an installed fork works **out of the
box**.  To use a table of your own, put it in one of these places — the first candidate that opens
wins:

1. the file named by the `PYTHON_MESSAGE_TABLE` environment variable;
2. `$PYTHONHOME/lookup-table.txt` (only reachable when you set `PYTHONHOME`);
3. `lookup-table.txt` in `sys.prefix` — the installation directory, next to `python.exe`;
4. `lookup-table.txt` in **the directory you start Python from**.

A candidate that cannot be read does not stop the walk, and a failed lookup is not remembered, so a
table that appears later takes effect.  `-X message-table=off` switches the feature off for a run,
`-E` skips the two environment candidates and `-I` skips the current directory.

```powershell
# Windows: point at the table in the repository root and check
> set PYTHON_MESSAGE_TABLE=%CD%\lookup-table.txt
> python -c "1/0"
Traceback (most recent call last):
  File "<string>", line 1, in <module>
    1/0
    ~^~
ZeroDivisionError: 笨蛋才会把0放到除数吧！
```

```bash
# Linux / macOS
$ export PYTHON_MESSAGE_TABLE=./lookup-table.txt
$ ./python -c "1/0"
Traceback (most recent call last):
  File "<string>", line 1, in <module>
    1/0
    ~^~
ZeroDivisionError: 笨蛋才会把0放到除数吧！
```

## Writing your own messages

Open `lookup-table.txt` in the repository root; one line is one entry:

```
original message<TAB>replacement text as hexadecimal
```

The separator is a **tab**, not a comma — an original message may well contain commas, spaces,
quotes and brackets. The right-hand side is the UTF-8 bytes of your text in hex; feed the sentence
you want to the command below to get it:

```bash
$ python -c "print(input().encode('utf-8').hex())"
```

Save, restart Python, and it takes effect (the table is read once, on first use). You are editing
the table that ships with the repository, so a pull request shares your wording with everyone —
see the end of this file.

Two things to remember about the table:

- **Keep `division by zero` and `list index out of range` working**; the documentation uses them as
  examples.
- A table you want for yourself only does not belong in the repository: keep it outside and point
  `PYTHON_MESSAGE_TABLE` at it.

## How an entry is written

- Key: the original message, which **must equal the text shown at run time character for
  character** — case and punctuation included. A key containing `*` is a **pattern** and covers a
  message that varies: `'*' object has no attribute '*'` → `'%1' 对象没有属性 '%2'`, where `%1` and
  `%2` are the first and second `*`.
- Value: the replacement text as hexadecimal UTF-8, which keeps the file pure ASCII so that no
  editor can damage it by saving.
- Blank lines and lines starting with `#` are ignored.

## What cannot be replaced

`KeyError`, `OSError` / `FileNotFoundError` with `errno`, `UnicodeDecodeError` and subclasses with
their own `__str__` are replaced now: none of them build their display from `args[0]`, so the copy is
made a subclass of the exception's own type whose `__str__` returns your text. The exception name
and any `isinstance` check stay the same.

The multi-argument `SyntaxError` the compiler produces, and multi-argument exceptions in general,
still keep the original — replacing the whole line would lose information. Every shape:

| Exception | Result | Why |
| --- | --- | --- |
| `ZeroDivisionError`, `IndexError`, `TypeError`, `AttributeError`, `NameError`, … | replaced | the display is the original message |
| `KeyError`, `OSError` / `FileNotFoundError` with `errno`, `UnicodeDecodeError`, subclasses with their own `__str__` | replaced | the display is not `args[0]`; it goes through the display proxy described above |
| `SyntaxError` **with a single argument** (e.g. `raise SyntaxError('msg')`) | replaced | the display is the original message |
| the `SyntaxError` the compiler produces (`compile()`, `exec()`, an imported file, REPL input) | original kept | known defect, see [FORK-NOTICE.md](FORK-NOTICE.md) |
| `Exception(1, 2)`, `ValueError('a', 'b')` and other multi-argument exceptions | original kept | the table looked up `args[0]`, while the display is the repr of the whole tuple |

## FAQ

**I edited the table and nothing changed.**
Start with where the table is: the interpreter tries `PYTHON_MESSAGE_TABLE`, then
`$PYTHONHOME/lookup-table.txt`, then the table in the installation directory, then one in **the
directory you started it from**, and takes the first that opens. Then check three more things:
② the separator is a **tab** — a comma makes the whole line be dropped; ③ your editor did not save
the file as UTF-8 with a BOM, which puts a few invisible characters in front of the first key;
④ no line is longer than 16384 bytes and none holds a NUL byte, because such a line is skipped as a
whole. Finally **restart Python**: the table is read once, on first use; run with `-X dev` to have
the skipped lines reported.

**Some entries are in the table but the message is still in English.**
First check that the key matches the text shown at run time character for character: one space or one
half-width punctuation mark off and the line becomes a dead key, silently. Two keys are easy to get
wrong: `KeyError` is keyed by the repr (`'missing'`, quotes included) and `OSError` by the whole line
(`[Errno 2] No such file or directory: 'x'`). If the exception is a multi-argument one, or a
`SyntaxError` the compiler produced, it is one of the two shapes listed in the section above.

**Will it replace the Python on my machine?**
No. The installer puts it in its own directory alongside official Python 3.15, and uninstalling
removes only its own files.

**Can I use it with the Python that pip installs into?**
No. The replacement logic is compiled into this fork's interpreter; official Python does not have
that code, and no external add-on can reach it.

**Is there an installer for Linux / macOS?**
There is a portable archive for Linux x64, in the table above. Nothing for macOS yet; the build
steps are in [docs/development.md](docs/development.md).

**The messages are cute now, but I want the original English back.**
Leave `PYTHON_MESSAGE_TABLE` unset and keep no table in **the directory you start Python from**;
the output is then exactly upstream's.

## Developers and further documentation

- [docs/development.md](docs/development.md): building from source, producing the installer, and the
  full format and matching rules of the lookup table (Chinese)
- [FORK-NOTICE.md](FORK-NOTICE.md): what differs from upstream, how the hook works, build and
  verification records
- [AGENTS.md](AGENTS.md): the conventions used in this repository (Chinese)

This fork has no issue tracker of its own. Problems with the table or the installers belong in this
repository's [issues](https://github.com/Flotiarenor/Kawaii-Python/issues); anything wrong with
Python itself belongs [upstream](https://github.com/python/cpython/issues).

## License

Both upstream CPython and this fork's changes are distributed under the **PSF License Version 2**:
`LICENSE` is unchanged, and the `LICENSE.txt` inside the installer is that license plus the
licenses of the bundled third-party libraries. See [LICENSE](LICENSE) for the terms and the history
of this software; the CPython copyright and license have not been modified by this fork.

This fork's own changes (listed in [FORK-NOTICE.md](FORK-NOTICE.md)) are Copyright Flotiarenor and
are offered under the same PSF-2.0 terms. "Python" is a trademark of the Python Software
Foundation; this fork is not affiliated with, or endorsed by, the PSF.
