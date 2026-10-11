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
- `--test-marker` 给注册表键与安装位置加一个标记（README 里 "an installer that works
  side-by-side with an official Python release"），**fork 必须带上它**：本轮实测没有它时，
  安装包在 Windows Installer 眼里就是官方 Python 3.15.0；带上之后，安装目录是
  `%LocalAppData%\Programs\Python\Python315-test`、注册表是
  `HKCU\Software\Python\PythonCore\3.15-test`，与官方 3.15 完全隔离；
- 产物在 `PCbuild\amd64\en-us\`（[bundle.targets:17](../Tools/msi/bundle/bundle.targets#L17) 把
  `OutputPath` 追加了 `en-us\`）。

脚本会同时编 Debug 与 Release 两套（安装包的 Debug 组件需要），所以首次运行耗时较长。

### 构建前的两个前置条件

这两条不在 `Tools\msi\README.txt` 的"一步"流程里，缺了会在很靠后的步骤才失败：

1. **MSVC ARM64 生成工具**。[launcher.wixproj:38-43](../Tools/msi/launcher/launcher.wixproj#L38-L43)
   会为 x86、x64、**ARM64** 各构建一次 `pyshellext.vcxproj`，且不看目标架构，所以即使只出
   x64 安装包也必须有 ARM64 工具集，否则报 `MSB8020: 无法找到 v143 的生成工具`。缺它时用
   VS 安装器补（需管理员）：
   `VS Installer\setup.exe modify --installPath "<VS 路径>" --add Microsoft.VisualStudio.Component.VC.Tools.ARM64 --add Microsoft.VisualStudio.Component.VC.Tools.ARM64EC --quiet --norestart`。
   **运行前先关掉所有 MSBuild/devenv 进程**，否则安装器以 `VSProcessesRunning`（退出码 8006）
   拒绝执行。
2. **HTML 文档与 `NEWS.txt`**。`doc` 包引用 `doc_html` 组件组，它由 Sphinx 的 HTML 输出经
   `csv_to_wxs.py` 生成，所以要先 `cd Doc && .\make.bat html`（需要
   `pip install -r Doc\requirements.txt`，注意该文件里的 `-c constraints.txt` 要求**在 `Doc\`
   目录下执行 pip**）。`exe` 包则要 `NEWS.txt`：`Misc\NEWS` 不存在时它调用
   `blurb merge`，而 **blurb 靠"首行匹配 `This is \w+ version \d+\.\d+` 的 `README` 或
   `README.rst`"来定位仓库根**（`blurb/_cli.py` 的 `chdir_to_repo_root`）。本 fork 把
   `README.rst` 换成了 `README.md`，这个标记就不存在了，于是 blurb 报
   `You're not inside a CPython repo right now!`；构建期间临时放一个符合格式的 `README.rst`
   即可（它未被跟踪，构建后删掉）。

### 产物命名

`snapshot.wixproj` 默认产出 `python-3.15.0.<天数>-amd64.exe`，天数自 2001-01-01 起算
（[msi.props:72](../Tools/msi/msi.props#L72)）。换成 fork 的名字与唯一 `ReleaseUri`：

```powershell
msbuild Tools\msi\bundle\releaselocal.wixproj /p:Platform=x64 /p:BuildForRelease=true `
        /p:ReleaseUri=https://github.com/Flotiarenor/Kawaii-Python /p:UseTestMarker=true
```

产出 `PCbuild\amd64\en-us\Kawaii-Python.exe`。**`/p:OutputName` 不会变成最终文件名**：bundle
工程的属性会覆盖命令行传进去的值，最终名字取自工程自身；于是对外分发时把产物复制成
`Kawaii-Python-3.15.0-windows-x64.exe`（本轮就是这么做的）。

`releaselocal.wixproj` 是"内嵌默认安装所需文件"的发布用工程；`full.wixproj` 内嵌全部内容，
适合本机测试；`snapshot.wixproj` 是 `build.bat` 用的测试工程，其 bundle 名不会带上
`-3.15.0` 后缀（因为 `snapshot.wixproj` 未设 `BuildForRelease`）。三者的区别见
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
只做本地安装包、不发布时，用 `releaselocal.wixproj` 配合 `/p:ReleaseUri=` 即可（本轮采用这条），
无需复制 `buildrelease.bat`；`ReleaseUri` 缺省时是 `$(ComputerName)/{arch}/`，那也会与官方
冲突，所以本地构建同样要显式传。

要改的三处，都集中在 `buildrelease.bat` 开头的 `set` 段：

| 变量 | 官方默认值 | fork 该填什么 |
| --- | --- | --- |
| `RELEASE_URI` | `http://www.python.org/{arch}` | 本仓库地址，如 `https://github.com/Flotiarenor/Kawaii-Python` |
| `DOWNLOAD_URL_BASE` | python.org 的 ftp 目录 | 自己的产物托管地址；不提供按需下载时留空 |
| `DOWNLOAD_URL` | `https://www.python.org/ftp/python/{version}/{arch}{releasename}/{msi}` | 同上 |

`RELEASE_URI` 的 scheme **必须是 `http`**：改成 `https` 会让产物无法与官方版本互相升级
（[buildrelease.bat:15-16](../Tools/msi/buildrelease.bat#L15-L16)）。

安装程序的显示名来自安装包自己的本地化字符串，在 `Tools\msi\bundle\` 下的 `.wxl` 模板里。
**本 fork 尚未改这一处**：当前产物在"应用和功能"里显示为 `Python 3.15.0-test (64-bit)`，
发布者仍是 `Python Software Foundation`（见下方"已知未做"）。

### 交付物里的表

安装包只装 CPython 自己的文件，**不会自动带上 `lookup-table.txt`**——本轮构建的安装包，
安装目录里没有这份表，所以装完仍是原样的 CPython，直到用户自己提供表。随包分发时把根目录
那份表复制进 release 目录，或者在 `Tools\msi\` 的打包流程里加一个复制步骤。注意表放在安装
目录里也**不会**自动生效（见下文的查找路径）；要让它开箱即用，得同时满足查找路径之一。

## 从源码运行与验证

表怎么被找到（[Objects/message_table.c](../Objects/message_table.c) 的 `message_table_path()`），
按优先级：

1. 环境变量 `$PYTHON_MESSAGE_TABLE` 指向的路径；
2. `$PYTHONHOME/lookup-table.txt` —— 读的是**环境变量** `PYTHONHOME`，不是 `sys.prefix`；
   引导期没有把 `PYTHONHOME` 指向前缀，所以普通安装下这一条不成立；
3. 当前工作目录下的 `lookup-table.txt`（`message_table_open()` 用 `fopen` 打开相对路径）。

**这一点最容易写错**：把表复制到解释器所在的目录（`sys.prefix`）**不会**生效，除非
`PYTHONHOME` 恰好被设为该目录。实测（Linux 安装树）：表在
`/home/build/kawaii/kawaii-python-3.15.0/` 里、cwd 为 `/tmp` 时输出仍是
`division by zero`；把表放到 cwd 里才变成替换文本。便携包之所以"开箱即用"，是因为表就在
cwd（解压目录）里，走的是第三条路径。

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

## 已知未做

v3.15.0 的产物（Windows 安装包与便携 zip、Linux 便携包）已构建并逐项验收，以下几条是明确的
未完成项，不是遗漏：

- **安装包的产品名与发布者仍是官方的**：在"应用和功能"里显示为
  `Python 3.15.0-test (64-bit)`、发布者 `Python Software Foundation`。要改成 Kawaii-Python
  得改 `Tools\msi\bundle\` 下的 `.wxl` 本地化模板，那会让与上游的差异文件从 8 个变成 9 个
  （见 [FORK-NOTICE.md](../FORK-NOTICE.md) 的差异清单），需要先确认这个代价可以接受。
- **便携包不含 tkinter 之外的 Tcl/Tk 运行时资源校验**：Linux 包里的 `_tkinter` 已编入，
  但容器内的 Tcl/Tk 视发行版而定；Windows 包的 `tcl/` 目录由 `PC/layout` 放入，未单独验证
  图形界面启动。
- **Linux 包的体积还能再降**：当前 41 MiB（`tar.gz` 15 MB），已经去掉静态库、测试套件与
  字节码缓存；若需要更小，可考虑 `--without-ensurepip` 或按需裁剪 `idlelib`。
- **`buildrelease.bat` 的 fork 副本尚未落地**：本轮用 `releaselocal.wixproj` 配合
  `/p:ReleaseUri=` 完成，所以仓库里还没有一条可复现的发布脚本，只有本文记录的命令。
