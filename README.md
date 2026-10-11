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
| Linux x64 | `Kawaii-Python-3.15.0-linux-x64.tar.gz` | Unpack and run the `python3` inside |

> [!NOTE]
> The table above is the naming convention for release artifacts; **until the first release is
> published those packages do not exist yet** — build from source as described in
> [docs/development.md](docs/development.md).

The Windows installer is produced by CPython's own WiX project, so it behaves like the official
one: it installs into `%LocalAppData%\Programs\Python\Python315` for the current user, offers the
"add to PATH" option, and can be uninstalled from Apps & features. It installs **alongside official
Python 3.15 and does not replace the Python you already have**.

> The installer is not code-signed, so Windows SmartScreen may block the first run: click
> "More info" → "Run anyway".

To make it work, the table has to be somewhere the interpreter looks (any one of these):

- copy `lookup-table.txt` into the interpreter directory, the one printed by
  `python -c "import sys; print(sys.prefix)"`;
- or set the environment variable `PYTHON_MESSAGE_TABLE` to the full path of the table;
- or keep a `lookup-table.txt` in a directory and start Python from that directory.

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

For a few exceptions your entry is found, but rewriting it would display something worse than the
original, so the original is kept. This is by design, not a failure:

| Exception | Result | Why |
| --- | --- | --- |
| `ZeroDivisionError`, `IndexError`, `TypeError`, `AttributeError`, `NameError`, … | replaced | the display is the original message |
| `SyntaxError` | replaced, line and column kept | the text is stored separately; the location is carried over |
| `KeyError` | original kept | the display is the argument itself, and a rewrite would add a pair of quotes |
| `OSError` / `FileNotFoundError`, … with `errno` | original kept | the display is built from the error number and the file name, which a rewrite would drop |
| `UnicodeDecodeError` | original kept | the display is built from the encoding and positions |
| `Exception(1, 2)` and other multi-argument exceptions | original kept | the display is the arguments themselves |
| exception subclasses with their own `__str__` | original kept | the display text is fixed |

## FAQ

**I edited the table and nothing changed.**
Check three things in order: ① the table is in a place the interpreter looks (the three options
above); ② the separator is a **tab** — a comma makes the whole line be dropped; ③ your editor did
not save the file as UTF-8 with a BOM, which puts a few invisible characters in front of the first
key. Then **restart Python**: the table is read once, on first use.

**Some entries are in the table but the message is still in English.**
See the section above: `KeyError`, `OSError` with `errno`, `UnicodeDecodeError` and types with a
custom `__str__` are "found, then left alone". No entry can take effect for them.

**Will it replace the Python on my machine?**
No. The installer puts it in its own directory alongside official Python 3.15, and uninstalling
removes only its own files.

**Can I use it with the Python that pip installs into?**
No. The replacement logic is compiled into this fork's interpreter; official Python does not have
that code, and no external add-on can reach it.

**Is there an installer for Linux / macOS?**
Source only for now; the build steps are in [docs/development.md](docs/development.md).

**The messages are cute now, but I want the original English back.**
Leave `PYTHON_MESSAGE_TABLE` unset and keep no table in the interpreter directory; the output is
then exactly upstream's.

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

PSF License Version 2, unchanged. See [LICENSE](LICENSE) for the terms and the history of this
software; the CPython copyright and license have not been modified by this fork.
