# Kawaii-Python — 让 Python 的报错信息变得可爱

**把 Python 的报错文案换成你自己写的。**

> **这不是 Python 官方发行版。** 它是基于 CPython v3.15.0 的个人修改版，不受 Python
> Software Foundation 支持或认可。除了报错文案可以替换这一件事，其余全部是上游原样。
> 英文版见 [README.md](README.md)。

它原本长这样：

```console
$ python -c "1/0"
Traceback (most recent call last):
  File "<string>", line 1, in <module>
    1/0
    ~^~
ZeroDivisionError: division by zero
```

装上 Kawaii-Python 之后长这样：

```console
$ python -c "1/0"
Traceback (most recent call last):
  File "<string>", line 1, in <module>
    1/0
    ~^~
ZeroDivisionError: 笨蛋才会把0放到除数吧！
```

文案写在仓库根目录的 `lookup-table.txt` 里，一行一条，想改哪条改哪条。这个思路来自
[kawaii-gcc](https://github.com/Bill-Haku/kawaii-gcc)（把 GCC 的本地化文件换成改写过的
`gcc.mo`），源头是中文项目 [gcc-hentai](https://github.com/Mosklia/gcc-hentai)。

## 下载与安装

到 [Releases](https://github.com/Flotiarenor/Kawaii-Python/releases/latest) 下载对应平台的包：

| 平台 | 产物 | 怎么装 |
| --- | --- | --- |
| Windows x64 | `Kawaii-Python-3.15.0-windows-x64.exe` | 双击安装，和装官方 Python 一样 |
| Windows x64 | `Kawaii-Python-3.15.0-windows-x64.zip` | 解压即用的便携版，不用安装 |
| Linux x64 | `Kawaii-Python-3.15.0-linux-x64.tar.gz` | 解压后运行其中的 `bin/python3` |
| Linux x64 | `Kawaii-Python-3.15.0-linux-x64.zip` | 同上，只是压缩格式不同 |

Windows 安装包由 CPython 自带的 WiX 工程生成，行为和官方安装程序一致：默认装到当前用户的
`%LocalAppData%\Programs\Python\Python315`，可以勾选加入 PATH，可以在「应用和功能」里卸载。
它**与官方 Python 3.15 并存安装，不会覆盖你已经装好的 Python**。

> 安装包没有代码签名，Windows 首次运行可能被 SmartScreen 拦下：点「更多信息」→「仍要运行」。

装完之后让它生效，表要在解释器会去找的三个位置之一（按优先级）：

1. 环境变量 `PYTHON_MESSAGE_TABLE` 指向的那个文件；
2. `$PYTHONHOME/lookup-table.txt`（只在你设了 `PYTHONHOME` 时才有这一条）；
3. **启动 Python 时所在目录**下的 `lookup-table.txt`。

最省事的做法是把 `lookup-table.txt` 放进你平时启动 Python 的目录；否则就设环境变量。
**注意：把表复制到解释器自己所在的目录并不会生效**——`python -c "import sys; print(sys.prefix)"`
打印的是 `sys.prefix`，而上面第三条看的是当前工作目录，两者不是一回事。

```powershell
# Windows：指向仓库根目录那份表，然后验证
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

## 改成你自己想要的文案

打开仓库根目录的 `lookup-table.txt`，一行就是一条：

```
报错原文<TAB>十六进制写的替换文案
```

中间那个是**制表符**，不是逗号——因为报错原文里本来就可能出现逗号、空格、引号和括号。
右侧的十六进制由文案的 UTF-8 字节得来，把想写的话交给下面这行命令就能得到：

```bash
$ python -c "print(input().encode('utf-8').hex())"
```

改完保存，重开 Python 就生效（表在进程第一次用到时读一次）。把你改好的 `lookup-table.txt`
提成 Pull Request，大家就都能用上你的文案——参与方式见文末。

对表格的两点提醒：

- **`division by zero` 和 `list index out of range` 这两条是文档里的例子，请保持可用**；
- 想只给自己用、不想公开的表，放到仓库外，用 `PYTHON_MESSAGE_TABLE` 指过去就行，不必动仓库里这份。

## 表怎么写

- 键：报错原文，**必须和运行时显示的文本逐字符相同**，大小写和标点都不能改。含 `*` 的是
  **模式键**，用一条覆盖会变化的消息：
  `'*' object has no attribute '*'` → `'%1' 对象没有属性 '%2'`，`%1`、`%2` 对应第一个、第二个 `*`。
- 值：替换文案的十六进制 UTF-8，所以整份文件是纯 ASCII 的，任何编辑器打开、保存都不会弄坏它。
- 空行和 `#` 开头的行会被忽略。

## 哪些消息换不了

`KeyError`、带 `errno` 的 `OSError` / `FileNotFoundError`、`UnicodeDecodeError` 和自己写了
`__str__` 的子类现在都能替换：它们的显示文本不来自 `args[0]`，于是程序把副本做成一层次子类，
让它的 `__str__` 直接返回你的文案，异常名和 `isinstance` 都不变。

`SyntaxError` 由编译器产生的多参数形态，以及多参数异常，仍然**保留原文**——替掉整行反而会
丢信息。各类异常的结局：

| 异常 | 结果 | 原因 |
| --- | --- | --- |
| `ZeroDivisionError`、`IndexError`、`TypeError`、`AttributeError`、`NameError` 等 | 替换 | 显示的就是报错原文 |
| `KeyError`、带 `errno` 的 `OSError` / `FileNotFoundError`、`UnicodeDecodeError`、自定义 `__str__` 的子类 | 替换 | 显示不由 `args[0]` 决定，走上面说的显示代理 |
| `SyntaxError`，**只有单个参数**那种（如 `raise SyntaxError('msg')`） | 替换 | 显示的就是报错原文 |
| 编译器产生的 `SyntaxError`（`compile()`、`exec()`、导入的文件、REPL 输入） | 保留原文 | 已知缺陷，见 [FORK-NOTICE.md](FORK-NOTICE.md) |
| `Exception(1, 2)`、`ValueError('a', 'b')` 这类多参数异常 | 保留原文 | 表里查到的是 `args[0]`，显示的却是整个参数元组的 repr |

## 常见问题

**我改了表，为什么没生效？**
先看表的位置：解释器只读**启动时所在目录**里的表，或 `PYTHON_MESSAGE_TABLE` /
`$PYTHONHOME` 指向的表。把表复制到解释器自己所在的目录**不会**生效，这是最容易踩的一条。
再看另外两件事：② 分隔符是不是**制表符**，写成逗号这一行会被整行丢弃；③ 文件有没有被编辑器
存成带 BOM 的 UTF-8——那会让第一行的键多出几个看不见的字符。改完表要**重开 Python**，表只在
进程第一次用到时读一次。

**为什么有的条目明明在表里，显示出来还是英文？**
先确认键和运行时显示的文本一模一样——差一个空格、换了一个半角标点，这一行就成了死键，
而且不会报错。两种键最容易写错：`KeyError` 的键是 repr 之后的样子（`'missing'`，带引号），
`OSError` 的键是整行（`[Errno 2] No such file or directory: 'x'`）。如果碰上的是多参数异常
或编译器产生的 `SyntaxError`，那是上一节列出的两种保留原文的情形。

**它会替换掉我电脑上的 Python 吗？**
不会。安装包与官方 Python 3.15 并存安装，装在自己的目录里，卸载也只删自己的文件。

**能用在我 pip 里的那个 Python 上吗？**
不能。替换逻辑在这个 fork 编译出的解释器里，官方 Python 没有这段代码，外部插件做不到。

**Linux / macOS 有安装包吗？**
Linux x64 有便携压缩包，见上方的下载表格。macOS 暂无产物，构建步骤见
[docs/development.md](docs/development.md)。

**我想临时看回英文原文，怎么办？**
不设 `PYTHON_MESSAGE_TABLE`，并且不在**启动 Python 的目录**里放表，输出就和官方 Python 完全
一样。

## 开发者与更多文档

- [docs/development.md](docs/development.md)：从源码构建、生成安装包、查询表的格式与判定机制
- [FORK-NOTICE.md](FORK-NOTICE.md)：与上游的差异清单、实现要点、构建与验收记录
- [AGENTS.md](AGENTS.md)：本仓库的协作约定

本 fork 没有自己的问题跟踪。查询表与安装包的问题提到本仓库的
[Issue](https://github.com/Flotiarenor/Kawaii-Python/issues)；Python 本身的 bug 属于
[上游](https://github.com/python/cpython/issues)。

## 许可证

PSF License Version 2，未改动。条款与这份软件的历史见 [LICENSE](LICENSE)；CPython 自身的
版权与许可条款没有被本 fork 修改。
