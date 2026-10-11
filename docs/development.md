# Kawaii-Python 开发文档

> 本文件面向**开发者与贡献者**：从源码构建、生成安装包、查询表的格式与判定机制。
> 面向使用者的下载、安装与改文案，见 [README-zh.md](../README-zh.md)（英文版
> [README.md](../README.md)）；与上游的差异清单、验收记录见 [FORK-NOTICE.md](../FORK-NOTICE.md)。

## 需求

- Windows：Visual Studio 2022（含 C++ 桌面开发负载）。**即使只编 x64，也要勾选
  "MSVC v14x - VS 20xx C++ ARM64/ARM64EC build tools"**，因为安装包链路用的 WiX 3.14 需要它。
- 安装包额外需要 Python 3.10 或更高版本（WiX 工程的构建脚本会用到）与 WiX 工具集；
  WiX 没有现成安装时 `Tools\msi\get_externals.bat` 会自动下载到 `externals/`。
- Unix / Linux / BSD / macOS：C 编译器与常规构建工具，`./configure && make -j`。

## 从源码构建（Windows x64 Release）

最省事的一条命令，连扩展模块一起编（跑测试要用）：

```powershell
cd PCbuild
.\get_externals.bat --no-tkinter
.\build.bat -e --no-tkinter
```

`--no-tkinter` 只在给出 `-e` 时有效（[PCbuild/build.bat:47-51](../PCbuild/build.bat#L47-L51)），
没有 Tcl/Tk 时不加它会在 `_tkinter` 上失败。构建产物在 `PCbuild\amd64\python.exe`。

只编解释器本体时用工程文件，但**顺序不能反**：

```powershell
cd PCbuild
.\get_externals.bat --no-tkinter
msbuild _freeze_module.vcxproj /p:Configuration=Release /p:Platform=x64 /m
msbuild python.vcxproj        /p:Configuration=Release /p:Platform=x64 /m
```

**`_freeze_module` 必须先编**：`pythoncore` 需要 `Python/frozen_modules/*.h`，这些头文件只有该
工程会生成、且不进版本控制（见 `.gitignore`），正常顺序靠解决方案级依赖保证。单独编
`python.vcxproj` 会在 `Modules/getpath.c:28` 报 `getpath.h: No such file or directory`。

**不要用** `msbuild pcbuild.sln /t:python`：`_tkinter` 挂在 `python` 目标里，跳过 Tcl/Tk 会让它
失败，连带整个目标失败，而 `python.vcxproj` 只需要 `pythoncore.vcxproj`。

Unix / Linux / BSD / macOS：

```bash
./configure
make -j
make test
```

Windows 上**不要用 `robocopy` 复制源码树**：CPython 带符号链接（`Misc/mypy/*`），`robocopy`
会把链接当目录跟进去，直接卡死。用 `tar` 或 `git clone`。

## 生成 Windows 安装包

CPython 自带完整的 WiX 安装包工程（[Tools/msi](../Tools/msi)），本 fork 直接复用，没有自写
打包脚本。系统要求与工程结构见 [Tools/msi/README.txt](../Tools/msi/README.txt)。

### 一步生成可直接分发的安装程序

```powershell
cd Tools\msi
.\get_externals.bat
.\build.bat -x64 --pack --test-marker
```

- `--pack` 把全部 MSI 收进安装程序本体，得到一个不依赖旁边文件的单文件 `.exe`（
  `snapshot.wixproj` 的 `/p:Pack=true`）；
- `--test-marker` 给注册表键与安装位置加一个标记，使其**与官方 Python 并存安装**，不会互相
  覆盖（默认关闭，见 [Tools/msi/README.txt](../Tools/msi/README.txt) 的 `--test-marker` 一节）；
- 产物在 `PCbuild\amd64\en-us\`（[bundle.targets:17](../Tools/msi/bundle/bundle.targets#L17) 把
  `OutputPath` 追加了 `en-us\`）。

脚本会同时编 Debug 与 Release 两套（安装包的 Debug 组件需要），所以首次运行耗时较长。

### 产物命名

默认名称由 [bundle.targets:9-15](../Tools/msi/bundle/bundle.targets#L9-L15) 拼出：非发布构建是
`python-3.15.0.<天数>-amd64.exe`，其中天数自 2001-01-01 起算（
[msi.props:72](../Tools/msi/msi.props#L72)）；`BuildForRelease=true` 时是 `python-3.15.0-amd64.exe`。
仓库对外分发的名称是 `Kawaii-Python-3.15.0-windows-x64`，在构建时用属性覆盖：

```powershell
msbuild Tools\msi\bundle\releaselocal.wixproj /p:Platform=x64 /p:BuildForRelease=true `
        /p:OutputName=Kawaii-Python /p:ReleaseUri=https://github.com/Flotiarenor/Kawaii-Python
```

`releaselocal.wixproj` 是"内嵌默认安装所需文件"的发布用工程；`full.wixproj` 内嵌全部内容，
适合本机测试；`snapshot.wixproj` 是 `build.bat` 用的测试工程。三者的区别见
[Tools/msi/README.txt](../Tools/msi/README.txt)。

### fork 必须改的三处 URI

**不要直接跑 `Tools\msi\buildrelease.bat`。** 它的 [`RELEASE_URI`](../Tools/msi/buildrelease.bat#L17)
默认是 `http://www.python.org/{arch}`，而 `ReleaseUri` 决定全部组件的 GUID
（[msi.props:219](../Tools/msi/msi.props#L219) 由它派生 UUID）：用默认值构建，我们的安装包在
Windows Installer 眼里与官方 Python 3.15.0 是同一个产品，会去升级或卸载用户已装的官方版本。
官方文档对这一点的说明是 "Forks of Python can use the same installer infrastructure by
providing a unique URI for this property"（
[Tools/msi/README.txt:159-165](../Tools/msi/README.txt#L159-L165)），
[buildrelease.bat:4-6](../Tools/msi/buildrelease.bat#L4-L6) 的注释也要求 fork 复制该脚本再改。

要改的三处，都集中在文件开头的 `set` 段：

| 变量 | 官方默认值 | fork 该填什么 |
| --- | --- | --- |
| `RELEASE_URI` | `http://www.python.org/{arch}` | 本仓库地址，如 `https://github.com/Flotiarenor/Kawaii-Python` |
| `DOWNLOAD_URL_BASE` | python.org 的 ftp 目录 | 自己的产物托管地址；不提供按需下载时留空 |
| `DOWNLOAD_URL` | `https://www.python.org/ftp/python/{version}/{arch}{releasename}/{msi}` | 同上 |

`RELEASE_URI` 的 scheme **必须是 `http`**：改成 `https` 会让产物无法与官方版本互相升级
（[buildrelease.bat:15-16](../Tools/msi/buildrelease.bat#L15-L16)）。只做本地安装包、不发布时，
用 `releaselocal.wixproj` 配合 `/p:ReleaseUri=` 即可，无需复制 `buildrelease.bat`。

安装程序的显示名（"Python 3.15.0" 之类）来自安装包自己的本地化字符串，在
`Tools\msi\bundle\` 下的 `.wxl` 模板里，需要一并改成 Kawaii-Python。

### 交付物里的表

安装包只装 CPython 自己的文件，**不会自动带上 `lookup-table.txt`**。随包分发时把根目录那份表
复制进 release 目录，或在 `Tools\msi\` 的打包流程里加一个复制步骤；表被找到的三个位置见下文。

## 从源码运行与验证

表怎么被找到（[Objects/message_table.c](../Objects/message_table.c) 的 `message_table_path()`），
按优先级：

1. `$PYTHON_MESSAGE_TABLE`；
2. `$PYTHONHOME/lookup-table.txt`；
3. 当前工作目录下的 `lookup-table.txt`。

仓库根目录那份表**不会**被 `PCbuild\amd64\python.exe` 自动读到：它不在 `$PYTHONHOME` 下，
也不在构建产物的旁边。所以验证时要么设 `PYTHON_MESSAGE_TABLE`，要么把工作目录切到仓库根目录
（第三条查找路径就是当前工作目录）。

改动落到 C 代码后，四类情形都要实跑（表缺失或格式错误时按设计静默退回原文，所以"没替换"要
先确认表路径，再怀疑代码）：

```powershell
$env:PYTHON_MESSAGE_TABLE = "$PWD\lookup-table.txt"
.\PCbuild\amd64\python.exe -c "1/0"        # 字面键命中：显示替换后的文本
.\PCbuild\amd64\python.exe -c "(1).foo"    # 模式键命中：'*' object has no attribute '*'
.\PCbuild\amd64\python.exe -c "raise ValueError('not in table')"  # 未命中：保持原文
.\PCbuild\amd64\python.exe -c "open('/nope')"   # 命中模式键但显示不成立：按闸门保留原文
.\PCbuild\amd64\python.exe                   # REPL 里 >>> 1/0 也必须被替换
```

改动表内容时，`division by zero` 与 `list index out of range` 两条要保持可用——README 与
[FORK-NOTICE.md](../FORK-NOTICE.md) 的示例都以它们为依据。相关测试：

```powershell
.\python.bat -m test test_message_table
.\python.bat -m test test_exceptions test_traceback test_sys test_capi.test_exceptions
.\python.bat Tools\patchcheck\patchcheck.py
```

提交前 `git status` 确认没把构建产物与日志带进提交（`PCbuild/**/x64/`、`build/`、`*.obj`、
`*.pdb`、生成的 `Python/frozen_modules/*.h` 都在 `.gitignore` 里）。

## 查询表的格式

纯 UTF-8 文本，一行一条，空行与 `#` 开头的行忽略。分隔符是**制表符**：

```
原文<TAB>十六进制 UTF-8 的替换文本
```

制表符而不是逗号，是因为键里可以自由出现逗号、空格、引号和括号，唯一不能出现的只有制表符
与换行；这条规则是为了避免"键里的逗号把整行吃掉"的静默失败。十六进制那一侧由
`"文本".encode("utf-8").hex()` 生成：

```bash
$ python -c "print(input().encode('utf-8').hex())"
```

文件保持**纯 ASCII** 也是为此：编辑器若把它存成 UTF-16，每一行都会失效；存成带 BOM 的
UTF-8，BOM 会被悄悄塞进第一个键里。

键分两种：

- **字面键**（不含 `*`）：与消息逐字符相等才命中，标点、大小写、空格都不许规范化。
- **模式键**（含 `*`）：每个 `*` 匹配任意长度的文本（可为空），按出现顺序编号，替换文本用
  `%1`、`%2` 引用对应捕获。字面键优先于模式键；多条模式命中时取 `*` 最少的那条。

随仓库分发的表两种都用：

```
division by zero	e7aca8e89b8be6898de4bc9ae68a8a30e694bee588b0e999a4e695b0e590a7efbc81
'*' object has no attribute '*'	2725312720e5afb9e8b1a1e6b2a1e69c89e5b19ee680a72027253227
```

表在第一次用到时读一次并常驻内存，所以改完表要重开进程。

### 值的标点约定

替换文案的标点用中文全角，半角只保留六个需要转换的字符：

| 半角 | 全角 |
| --- | --- |
| `,` `!` `?` `:` `;` | `，` `！` `？` `：` `；` |
| `...` 或单个 `…` | `……`（U+2026 ×2） |

其余 26 种（`% _ ' . ( ) { } " - = / > ] [ < \` * \ | + # $ ~ @ & ^`）一律不动：它们是占位符
或代码、协议的一部分。某处句读在满足"前一个字符是中日韩字符""后一个非空格字符是中日韩字符"
"位于句末"三者之一时才转换，转换后紧跟的一个空格要去掉。这样 `range(0, 256)`、`int()`、
`[Errno 2]` 里的标点不会被误转。批量转换用 `local/错误信息统计/punct_tool.py`（该目录不进仓库，
工具随用随取），它必须保持幂等。

## 哪些消息能被替换

判定分两步。

第一步选要查的文本：类型用标准 `__str__`（按 `tp_str` 比较，不分配对象）时取 `args[0]`，
否则取异常真实显示的文本——**这一步正是模式键能覆盖到"文本不是 `args[0]`"的类型的入口**。

第二步，改写完成后让副本再显示一次：只有显示结果**恰好等于**新文本才保留改写
（`SyntaxError` 按"以新文本开头"放行，位置字段一并搬运）。过不了这一步的改写会被丢弃，而不是
打出来。没有第二步，副本会显示成没人要的东西——`KeyError` 会显示新文本的 `repr()`，带 `errno`
的 `OSError` 会显示一个元组，`UnicodeDecodeError` 的副本什么都显示不出来。

| 异常 | 结果 | 原因 |
| --- | --- | --- |
| `ZeroDivisionError`、`IndexError`、`TypeError`、`AttributeError`、`NameError`、`RecursionError`、`ImportError` 等 | 替换 | 显示文本就是 `args[0]` |
| 继承标准 `__str__` 的子类 | 替换 | 同上 |
| `SyntaxError` | 替换，位置保留 | 文本在 `.msg` 里，`filename`、`lineno`、`offset`、`text` 与结束位置一并搬运 |
| `KeyError` | 表里查到了，但保留原文 | 显示文本是 `repr(args[0])`，改写后会多出引号 |
| 带 `errno` 的 `OSError` 及其子类 | 表里查到了，但保留原文 | 显示文本由 `errno`／`strerror`／`filename` 拼出 |
| `UnicodeDecodeError` | 表里查到了，但保留原文 | 显示文本由 `encoding/object/start/end/reason` 拼出，用 `__new__` 造的副本没有 `object` 成员 |
| `Exception(1, 2)` 这类多参数异常 | 表里查到了，但保留原文 | 显示的是参数元组的 repr |
| 覆盖了 `__str__` 的子类 | 永远不匹配 | 显示文本是固定的 |

「表里查到了，但保留原文」是设计结果而不是故障：条目可达，但改写会打出比原文更糟的东西，所以
丢弃。两行最令人意外的结论来自上游的
[Objects/exceptions.c:3867](../Objects/exceptions.c#L3867)（`UnicodeDecodeError` 的空副本）与
[Objects/exceptions.c:2927](../Objects/exceptions.c#L2927)（`SyntaxError` 渲染位置之处）。

## 文档分工与政策

| 文件 | 读者 | 语言 |
| --- | --- | --- |
| [README-zh.md](../README-zh.md) | 使用者 | 中文（主用） |
| [README.md](../README.md) | 使用者 | 英文 |
| 本文件 | 开发者与贡献者 | 中文 |
| [FORK-NOTICE.md](../FORK-NOTICE.md) | 想确认与上游关系的人 | 英文 |
| [AGENTS.md](../AGENTS.md) | 自动化 agent 与协作者 | 中文 |

写作约定（语言政策、表的写法、不得做的事、提交信息规范）在
[AGENTS.md](../AGENTS.md) 与 [.dsh/commit-conventions.md](../.dsh/commit-conventions.md)：
C / Python 源码里的注释、docstring、命名一律英文，`Doc/**`、`Lib/**` 等上游文件不翻译，
只有治理文档与本文件用中文。
