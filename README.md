# Kawaii-Python — CPython 3.15.0 的非官方修改版，异常信息可替换

> **这不是 Python 官方发行版。** 它是 CPython v3.15.0 的个人 fork，不受 Python Software
> Foundation 支持或认可。本仓库只改动与"异常信息可替换"有关的部分，其余一律是上游原样；
> 改动了哪些文件见 [FORK-NOTICE.md](FORK-NOTICE.md)。本文件的另一份在
> [README-zh.md](README-zh.md)。

**本仓库是叠加在 CPython 上的一组改动，不是一个 Python 发行版。** 关于 Python 本身的一切
——它能跑在哪些平台、怎么安装、这个版本改了什么——都由上游记录，见
[CPython 官方 README](https://github.com/python/cpython/blob/main/README.rst)。这份文件只讲
本 fork 加了什么。

## 这是什么

上游 CPython 的异常信息是写死在 C 源码里的字面量：`ZeroDivisionError: division by zero`
是一个固定字符串。在这个 fork 里，这段文本可以来自外部查询表，所以你在 traceback 里看到
的文案由你自己写。

交付物是一对东西：**能读表的解释器 + 表本身**（`lookup-table.txt`，随仓库分发）。这个思路
来自 [kawaii-gcc](https://github.com/Bill-Haku/kawaii-gcc)——它把 GCC 的本地化文件换成改写过的
`gcc.mo`；CPython 没有消息目录，所以同样的思路还需要在解释器里加一个钩子。

只有异常信息受影响。构建系统、标准库、测试套件与许可证都是上游原样。

## 用法

把 `PYTHON_MESSAGE_TABLE` 指向表，表里的消息就会被替换：

```bash
$ export PYTHON_MESSAGE_TABLE=./lookup-table.txt
$ ./python -c "1/0"
Traceback (most recent call last):
  File "<string>", line 1, in <module>
ZeroDivisionError: 笨蛋才会把0放到除数吧！
```

Windows 上同理，表放在构建产物旁边：

```powershell
> set PYTHON_MESSAGE_TABLE=%CD%\lookup-table.txt
> PCbuild\amd64\python.exe -c "1/0"
```

表的查找顺序（见 [Objects/message_table.c](Objects/message_table.c) 的 `message_table_path()`）：

1. `$PYTHON_MESSAGE_TABLE`；
2. `$PYTHONHOME/lookup-table.txt`；
3. 当前工作目录下的 `lookup-table.txt`。

注意这条顺序对「仓库根目录那份表」意味着什么：解释器**不会**主动去仓库根目录找它。所以要么
设 `PYTHON_MESSAGE_TABLE`，要么在仓库根目录启动进程，要么把表复制到解释器旁边。要和解释器
一起分发时，放到这个路径：

```bash
$ python -c "import sys; print(sys.prefix)"
```

## 表的格式

纯 UTF-8 文本，一行一条，空行与 `#` 开头的行忽略。键与替换文本之间是**制表符**，不是逗号
（这样键里才能自由出现逗号、空格、引号和括号）：

```
原文<TAB>十六进制 UTF-8 的替换文本
```

键分两种。**不含 `*` 的是字面键**，要与消息逐字符相等才命中。**含 `*` 的是模式键**：每个 `*`
代表任意长度的文本（可以为空），按出现顺序编号，替换文本用 `%1`、`%2` 引用对应捕获。字面键
优先于模式键；多条模式命中时取 `*` 最少的那条。随仓库分发的表两种都用：

```
division by zero<TAB>e7aca8e8...
'*' object has no attribute '*'<TAB>2725312720e5afb9...
```

右侧就是 `bytes.hex()` 的输出，生成自己的文本用：

```bash
$ python -c "print(input().encode('utf-8').hex())"
```

表在**第一次用到时读一次**并常驻内存，所以改完表要重开进程。表缺失或格式错误时按设计静默
退回原文，和上游输出完全一致。整行写错也是同样的表现，所以某条看起来"没生效"时，先查路径、
再查分隔符。

文件要像随仓库分发的那份一样保持**纯 ASCII**。这正是用十六进制的原因：编辑器若把它存成
UTF-16、或存成带 BOM 的 UTF-8，前者会让每一行都失效，后者会把 BOM 悄悄塞进第一个键里。

## 哪些异常能被替换

判定分两步。第一步选要查的文本：类型用的是标准 `__str__` 时取 `args[0]`，否则取异常真实
显示的文本——**这一步正是模式键能覆盖到"文本不是 `args[0]`"的类型的入口**。第二步，改写
完成后让副本再显示一次，只有显示结果**恰好等于**新文本才保留改写；过不了这一步的改写会被
丢弃，而不是打出来：

| 异常 | 结果 | 原因 |
| --- | --- | --- |
| `ZeroDivisionError`、`IndexError`、`TypeError`、`AttributeError`、`NameError`、`RecursionError`、`ImportError` 等 | 替换 | 显示文本就是 `args[0]` |
| 继承标准 `__str__` 的子类 | 替换 | 同上 |
| `SyntaxError` | 替换，位置保留 | 文本在 `.msg` 里，位置字段一并搬运 |
| `KeyError` | 表里查到了，但保留原文 | 显示文本是 `repr(args[0])`，改写后会多出引号 |
| 带 `errno` 的 `OSError` 及其子类 | 表里查到了，但保留原文 | 显示文本由 `errno`／`strerror`／`filename` 拼出 |
| `UnicodeDecodeError` | 表里查到了，但保留原文 | 显示文本由 `encoding/object/start/end/reason` 拼出 |
| 覆盖了 `__str__` 的子类 | 保留原文 | 显示文本是固定的 |
| `Exception(1, 2)` 这类多参数异常 | 保留原文 | 显示的是参数元组的 repr |

「表里查到了，但保留原文」是设计结果而不是故障：这些类型写任何条目都不会生效。

## 怎么构建

只有 Windows 构建与上游不同，差别在于**必须先编 `_freeze_module`**：

```powershell
cd PCbuild
.\get_externals.bat --no-tkinter
msbuild _freeze_module.vcxproj /p:Configuration=Release /p:Platform=x64 /m
msbuild python.vcxproj        /p:Configuration=Release /p:Platform=x64 /m
```

`pythoncore` 要 `Python/frozen_modules/*.h`，那些头文件只有该工程会生成、且不进版本控制，
所以单独编 `python.vcxproj` 会在 `Modules/getpath.c` 上报
`getpath.h: No such file or directory`。正常顺序靠解决方案级依赖保证，直接编工程就跳过了它。

**不要**用 `msbuild pcbuild.sln /t:python`：`_tkinter` 挂在这个目标里，跳过 Tcl/Tk 会让它
失败，连带整个目标失败，而 `python.vcxproj` 其实只需要 `pythoncore.vcxproj`。

要跑测试套件还得补编扩展模块（`msbuild pcbuild.proj`，其中 `_tkinter` 会因为缺 Tcl/Tk 失败，
那是预期的）。Unix、macOS 等其他平台，以及构建 Python 的其他一切，见上游的
[Build Instructions](https://github.com/python/cpython/blob/main/README.rst#build-instructions)。

## 验证记录与实现细节

构建矩阵、逐项验收结果、与上游的差异清单、以及哪些事不能做，都在
[FORK-NOTICE.md](FORK-NOTICE.md)。

## 参与

这是个人 fork，因此没有独立的 issue 跟踪。关于 Python 本身的问题属于
[上游](https://github.com/python/cpython/issues)；关于查询表的问题属于本仓库。本仓库使用的
约定——语言政策、表的写法、改 C 代码前必须验证什么——写在 [AGENTS.md](AGENTS.md) 与
[FORK-NOTICE.md](FORK-NOTICE.md) 里，后者也记录了这次改动的完整说明与验收结果。

## 许可证

PSF License Version 2，未改动。条款与这份软件的历史见 [LICENSE](LICENSE)；CPython 自身的
版权与许可条款没有被本 fork 修改。
