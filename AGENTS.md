# AGENTS.md —— 本仓库（CPython 个人 fork）内的 Agent 工作约定

本文件由支持 `AGENTS.md` 的工具（含 DSH）在工作区根目录自动加载，是本仓库对 agent 的
完整工作约定；机器可执行的提交规则在 [.dsh/commit-conventions.md](.dsh/commit-conventions.md)，
本文件解释为什么这么定。仓库自身的说明见 [FORK-NOTICE.md](FORK-NOTICE.md)，
面向使用者的介绍见 [README-zh.md](README-zh.md)（英文版 [README.md](README.md)），开发流程见
[docs/development.md](docs/development.md)。

## 项目要点

- 这是**非官方**的 CPython 修改版，基线 v3.15.0，只有一个自加功能：异常信息可用外部文本表
  替换。上游的构建系统、标准库、测试套件、许可证一律未改。
- 与上游的差异：上游文件改动 10 个，本 fork 新增 4 个，改动前先认清边界：

  | 文件 | 作用 |
  | --- | --- |
  | [Python/pythonrun.c](Python/pythonrun.c) | `_PyErr_Display()`（约 1135 行起）里调用 `_PyErr_Message_Replace()` |
  | [Python/pylifecycle.c](Python/pylifecycle.c) | `_Py_Finalize()` 里调用 `_PyMessageTable_Fini()` 释放缓存表 |
  | [Include/cpython/unicodeobject.h](Include/cpython/unicodeobject.h) | 文件末尾 `#include "message_table.h"` |
  | [Include/message_table.h](Include/message_table.h) | 三个内部函数的声明 |
  | [Objects/message_table.c](Objects/message_table.c) | 表的加载、hex 解码、查找、改写、显示代理与"能否替换"的判定 |
  | lookup-table.txt（仓库根目录） | **交付物本体**：替换用的查询表，受版本控制、随仓库分发 |
  | [Makefile.pre.in](Makefile.pre.in)、[PCbuild/pythoncore.vcxproj](PCbuild/pythoncore.vcxproj)、[PCbuild/_freeze_module.vcxproj](PCbuild/_freeze_module.vcxproj) | 把新源文件编进 `pythoncore` |
  | [Tools/msi/common_en-US.wxl_template](Tools/msi/common_en-US.wxl_template)、[Tools/msi/msi.targets](Tools/msi/msi.targets)、[Tools/msi/exe/exe_en-US.wxl_template](Tools/msi/exe/exe_en-US.wxl_template)、[Tools/msi/freethreaded/freethreaded_en-US.wxl_template](Tools/msi/freethreaded/freethreaded_en-US.wxl_template) | 安装包的身份字符串：显示名 `Kawaii-Python 3.15.0 (64-bit)`、发布者 `Kawaii-Python`、`SupportUrl` 指向本仓库（新增不带 `-test` 的 `{{PyVersion}}` 占位符） |
  | [Lib/test/test_message_table.py](Lib/test/test_message_table.py) | 本 fork 自己的测试（表、闸门、显示代理、REPL 入口） |

- 钩子选在 `_PyErr_Display()`：REPL、`PyErr_Print`、`sys.excepthook`、`sys.__excepthook__`
  与 `traceback` 模块不可用时的 C 回退都从这里出去，一处覆盖全部。**REPL 不走
  `sys.excepthook`**，所以纯 Python 实现覆盖不到这一条，钩子必须在 C 里。
- 实现只用公开 C API、不 import Python 代码：import 机制损坏时 traceback 仍要打得出来。
- `_PyErr_Message_Replace()` 返回新引用（改写后的副本）或 `NULL`（表里没有这条，或改写无法
  显示出来）。调用方必须把 `NULL` 当作"用原来的"。
- 上游风格照旧生效：C 遵守 [PEP 7](https://peps.python.org/pep-0007/)、Python 遵守
  [PEP 8](https://peps.python.org/pep-0008/)。**新增符号一律加 `_Py` 前缀并保持内部可见**：
  [b41bab3](https://github.com/Flotiarenor/Kawaii-Python/commit/b41bab3) 把 `PyUnicode_FromHex` 改成
  `static` 就是这个原因 —— 非下划线开头的导出符号会被上游的「Check if all C APIs are
  documented」判成新的公开 C API，于是要么补 `Doc/c-api/`，要么多一份 stable ABI 报告。
- 本项目的形态对齐 [kawaii-gcc](https://github.com/Bill-Haku/kawaii-gcc)（把 GCC 的本地化文件换成日语化的
  `gcc.mo`，源头是中文项目 [gcc-hentai](https://github.com/Mosklia/gcc-hentai)）：**交付物是"译文表 + 能
  读它的解释器"**。所以根目录的 `lookup-table.txt` 是项目资产，要提交、要随源码分发，它不是本地
  用户数据，不要写进 `.gitignore`。
- `MESSAGE_TABLE_STUB_BUILD` 分支（[Objects/message_table.c](Objects/message_table.c) 第 73、1141 行）
  引用的 `py_stub_api.h` 不在仓库里，也没有任何构建目标定义这个宏：它是从 3.10 fork 带过来的
  遗留路径，当前无人编译，不要照着它做验证。

## 语言政策（先看这一条）

| 位置 | 语言 | 说明 |
| --- | --- | --- |
| C / Python 源文件里的**注释、docstring、命名** | 英文 | 与上游一致；本 fork 新增的 C 文件与头文件目前是 0 行中文 |
| `Doc/**`、`Lib/**` 等上游文件 | 英文 | 不要为了"本地可读"把上游文件译成中文 |
| `AGENTS.md`、`.dsh/**`、`README-zh.md`、`docs/**` | 中文 | 本 fork 自己的治理与开发文档，写给本项目的人看 |
| `README.md`、`FORK-NOTICE.md` | 英文 | 面向仓库外读者的两份：使用者页与与上游的关系声明；`README.md` 与 `README-zh.md` 章节一一对应 |
| **提交信息（subject 与正文）** | 中文 | 只此一处：`git_commit` 校验 `language: zh` |
| 测试里的 Unicode 字面量（`Lib/test/**`） | 按测试需要 | 上游本来就有 CJK 数据，属于被测内容，不是注释 |
| `lookup-table.txt` 的**值**（替换文本） | 中文，标点全角 | 见下方"表的写法约定" |
| `lookup-table.txt` 的**键**（消息原文） | 保持 CPython 原样 | **一个字符都不许改**，包括它用的半角标点 |

### 表的写法约定

- **分隔符是制表符**（`\t`），不是逗号。键是消息原文，可以含逗号、空格、引号、括号；
  唯一不能出现的是制表符与换行。这一条是为了不再出现"键里的逗号把整行吃掉"的静默失败。
- **键必须与运行期显示的文本逐字符相等**，标点、大小写、空格都不许规范化。改写标点只会让
  条目变成死键。
- **值的标点用中文全角**，由 `local/错误信息统计/punct_tool.py` 统一转换（该目录不进仓库，
  工具随用随取）。转换表只有六个字符，语料实测（5787 条消息里共 32 种非字母数字字符）够用：

  | 半角 | 全角 |
  | --- | --- |
  | `,` `!` `?` `:` `;` | `，` `！` `？` `：` `；` |
  | `...` 或单个 `…` | `……`（U+2026 ×2） |

  其余 26 种（`% _ ' . ( ) { } " - = / > ] [ < \` * \ | + # $ ~ @ & ^`）一律不动：它们是
  占位符或代码/协议的一部分。某处句读在满足"前一个字符是中日韩字符""后一个非空格字符是
  中日韩字符""位于句末"三者之一时才转换，**转换后紧跟的一个空格要去掉**（全角标点自带间距）。
  这样 `range(0, 256)`、`int()`、`[Errno 2]` 里的标点不会被误转。工具必须**幂等**
  （`--check` 每次都会自检），否则重跑一次就会把 `……` 变成 `………`。
- 值用十六进制 UTF-8 表示（`"文本".encode("utf-8").hex()`），所以空白与不可见字符是显式的。

违反这一条最典型的做法：给 `Objects/message_table.c` 新写的函数配一段中文注释。**不要**。

## 不得做的事

- **不要 merge / rebase 上游**。fork 与上游没有共同提交历史，功能又很小，升级方式是在新基线
  上重新施加改动（[FORK-NOTICE.md](FORK-NOTICE.md) 的 "Upgrading to a newer CPython"），不是合并。
- 不要提交构建产物：`PCbuild/**/x64/`、`build/`、`*.obj`、`*.pdb`、`python.exe`、生成出来的
  `PCbuild/pyconfig.h`、`Makefile`、`configure` 产物都在 `.gitignore` 里，`git status` 里出现
  就说明用错了路径。
- 不要把 `lookup-table.txt` 当本地数据：它是本项目的交付物，要提交、要随仓库分发。只为自用、
  不想公开的私人表请放在仓库外，用 `PYTHON_MESSAGE_TABLE` 指过去，不要塞进根目录。
- 不要对 `main` 执行 force push；不要移动或删除已发布的 tag。
- 不要为了"让门禁安静"而放宽检查（例如把新符号改名成不触发检查的样子），要在提交信息里
  写清楚为什么这么改。

## 改动前必须执行的检查

1. 先跑与改动相关的最小集，再按下表决定要不要出构建：

   | 改动落在 | 必须做 |
   | --- | --- |
   | `Objects/`、`Include/`、`Python/`、`Makefile.pre.in`、`PCbuild/**` | **真的构建一次并实跑**（见下），C 改动没有"不构建就能验证"的门禁 |
   | `AGENTS.md`、`.dsh/`、`FORK-NOTICE.md`、`README.md`、`README-zh.md`、`docs/**` | 只改文档，核对引用的路径、行号、命令仍然存在 |
   | `Lib/`、`Doc/` | 上游门禁照旧：`python -m test` / `make -C Doc check` |

2. 构建（Windows x64 Release，VS2022）：

   ```powershell
   cd PCbuild
   .\get_externals.bat --no-tkinter
   msbuild _freeze_module.vcxproj /p:Configuration=Release /p:Platform=x64 /m
   msbuild python.vcxproj        /p:Configuration=Release /p:Platform=x64 /m
   ```

   **`_freeze_module` 必须先编**：`pythoncore` 要 `Python/frozen_modules/*.h`，那些头文件由该
   工程生成，**只有解决方案级的依赖才会保证顺序**；直接编 `python.vcxproj` 会在
   `Modules/getpath.c:28` 报 `getpath.h: No such file or directory`（生成的头文件不进版本控制）。
   跑测试还需要扩展模块，用 `msbuild pcbuild.proj` 全量编一次；`_tkinter` 会因为没有 Tcl/Tk
   而失败，那是预期的。

   **不要用** `msbuild pcbuild.sln /t:python`：`_tkinter` 挂在 `python` 目标里，跳过 Tcl/Tk
   会让它失败，连带整个目标失败，而 `python.vcxproj` 其实只需要 `pythoncore.vcxproj`。

   Unix / macOS：`./configure && make -j`（跑测试再加 `make test`）。

   Windows 上**不要用 `robocopy` 复制源码树**：CPython 带符号链接（`Misc/mypy/*`），
   `robocopy` 会把链接当目录跟进去，直接卡死。用 `tar` 或 `git clone`。

3. 用一个临时表验证行为，四类情形都要覆盖：

   ```powershell
   # 表内容：division by zero<TAB><十六进制 UTF-8>（分隔符是制表符）
   $env:PYTHON_MESSAGE_TABLE = "$PWD\lookup-table.txt"
   .\PCbuild\amd64\python.exe -c "1/0"        # 字面键命中：显示替换后的文本
   .\PCbuild\amd64\python.exe -c "(1).foo"    # 模式键命中：'*' object has no attribute '*'
   .\PCbuild\amd64\python.exe -c "raise ValueError('not in table')"  # 未命中：保持原文
   .\PCbuild\amd64\python.exe -c "open('/nope')"   # 显示不来自 args[0]：走显示代理
   .\PCbuild\amd64\python.exe -c "b'\xff'.decode('utf-8')"  # 同上，表里有键时
   .\PCbuild\amd64\python.exe -c "raise ValueError('a', 'b')"  # 多参数：按闸门保留原文
   .\PCbuild\amd64\python.exe                   # REPL 里 >>> 1/0 也必须被替换
   ```

   十六进制那一侧用 `print(input().encode('utf-8').hex())` 生成；表缺失或格式错误时按设计
   静默退回原文，所以"没替换"要先确认表路径，再怀疑代码。**表怎么找到**（
   [Objects/message_table.c](Objects/message_table.c) 的 `message_table_path()`）：
   `$PYTHON_MESSAGE_TABLE` 优先，其次是 `$PYTHONHOME/lookup-table.txt`，最后才是当前工作目录的
   `lookup-table.txt`。注意第二条读的是**环境变量** `PYTHONHOME`，不是 `sys.prefix`：把表复制到
   解释器所在目录**不会**生效（v3.15.0 实测，Linux 安装树：表在 prefix 里、cwd 在 /tmp 时仍输出
   `division by zero`）。所以验证时要么设 `PYTHON_MESSAGE_TABLE`，要么在仓库根目录执行——仓库
   根目录那份表不会被 `PCbuild\amd64\python.exe` 自动读到，除非 cwd 就在那里。便携包"开箱即用"
   是因为表在解压目录里，而用户从那里启动，走的正是第三条。上游的补充检查是
   `.\python.bat Tools\patchcheck\patchcheck.py` 与 `.\python.bat -m test`。改动表内容时，
   `division by zero` 与 `list index out of range` 这两条要保持可用（README 与
   [FORK-NOTICE.md](FORK-NOTICE.md) 的示例以它们为依据；注意 `[][0]` 不是键，`IndexError` 的键是
   `list index out of range`）。
   **验收要看"副本显示成什么"**：改写只在副本的显示恰好等于新文本时才生效。过不了这道闸门的
   类型——`KeyError`、带 errno 的 `OSError`、`UnicodeDecodeError`、自定义 `__str__` 的子类——
   改走**显示代理**：副本是原子类的一层子类，`__str__` 直接返回新文本，异常名与 `isinstance`
   不变；代理建不出来（例如基类的 `__init_subclass__` 会抛）时才保留原文。`SyntaxError` 仍走
   "以新文本开头即放行"的分支（它会在文案后面追加 `(<file>, line N)`），代理刻意跳过它；**多参数
   SyntaxError 根本走不到那一步**，见下方的已知缺陷。
4. `git status` 确认没把构建产物与日志带进提交。

## 提交信息规范

规则依据是上游的
[Making good commits](https://devguide.python.org/getting-started/pull-request-lifecycle/#making-good-commits)
与 [Accepting and merging a pull request](https://devguide.python.org/getting-started/git-boot-camp/#accepting-and-merging-a-pull-request)，
**只有语言换成中文**：上游用英文祈使句，这里用中文祈使式短句，结构与粒度不变。

提交信息由 DSH 的 `git_commit` 工具从结构化字段拼装并逐条校验，不合规直接拒绝并给出改法，
不经过 shell —— `pwsh` / `bash` 里的 `git commit` 被 shell guard 拦下，不要绕。改规则时改
[.dsh/commit-conventions.md](.dsh/commit-conventions.md)（键值决定工具拦什么），本节的表格
解释为什么。工具的两个硬性形状与上游略有出入，先把它们说清楚：

1. 工具要求 `type` 字段并把它拼在主题前面（`type(scope): subject`），**上游没有 type 前缀**。
   所以本仓库的主题是「type 前缀 + CPython 式标题」，type 取上文那 11 个值、可选；
   这与当前 `git log` 的三条提交一致。
2. 若这次改动对应的正是某个上游 issue，保留 CPython 的关联方式：把主题写成
   `gh-NNNNN: <标题>`，即 `type=gh-NNNNN`、`scope` 省略、`subject` 只留标题，并新增
   `gh-NNNNN` 到 [.dsh/commit-conventions.md](.dsh/commit-conventions.md) 的 `types` 里。
   本 fork 没有自己的问题跟踪，所以**不要凭空编号**：只在上游确有该 issue 时才这么写。
   普通改动用类型前缀即可，issue 关联一律走 `Refs:` footer。

### 标题（subject）

- 中文祈使式短句，说明这个提交**做什么**，不写"现在…了"的描述式。
- **不写现象**：「REPL 不替换」是现象，「修正 REPL 下不替换异常信息的情形」才是改动本身；
  查过上游修复的话，移植 PR 的标题也按这个位置放，并保留必要上下文（如 `gh-NNNN` 与函数名）。
- 首行**不加句号**；不加 `!!!!`、`……` 之类的装饰标点；一个提交只做一件事。
- 单行 ≤ 60 列（约 30 字）为宜，硬上限 72 列。一个汉字算 2 列，所以 36 个汉字就已接近硬上限；
  长标题的正确解法是缩短措辞，工具不会替你截断。
- 技术名词（CPython、REPL、`PYTHON_MESSAGE_TABLE`、msbuild、`_PyErr_Display`）保持原样，不翻译。

### 正文（body）与 footer

- 正文用中文，第一段回答"**为什么**"：问题是什么、不这么改会怎样。首行已经说清"是什么"，
  不要用正文复述标题。
- 后续段落再补细节、权衡、被否掉的方案；正文里贴命令与输出时，命令本身保持英文原样。
- 与上游一致的"可复核"要求：每条断言都要落回命令、文件、行号、构建配置或数字。
- 破坏性变更用 `breaking` 字段（工具补 `!` 与 `BREAKING CHANGE:` trailer，不要自己塞进 footer）；
  关联 issue 用 `Refs: gh-<编号>`；移植上游补丁时在正文注明原始 commit。
- 过程记录（先试 A 再试 B、rebase、merge from main）属于 PR 或 issue，**不写入提交信息** ——
  上游把这类正文明确列为反面例子。

### 措辞排除表（写完逐条自查）

| 类别 | 排除 | 改为 |
| --- | --- | --- |
| 情绪与口语 | 搞一下、顺手、试试、其实就是、反正、貌似、差不多 | 客观陈述事实与结果 |
| 未量化的夸张 | 彻底解决、大幅提升、完全避免 | 给出数字；无法量化时写明作用范围与限制 |
| 不确定的断言 | 应该没问题、可能是这里 | "未验证"或"待确认" |
| 描述式标题 | "REPL 下异常信息没有被替换" | "修正 REPL 下不替换异常信息的情形" |
| 占位式标题 | 修改、更新、修复、优化（单独成句） | 写出这次到底改了什么 |

工具的填充词清单按**子串**匹配中文，上面这些词出现在主题或正文里都会直接拒绝；占位式标题
另有一份"整体相等才判失败"的清单。项目既有术语（如"幽灵 API"、上游检查的英文原名）沿用
原词，不另造说法。

### 对照（不合格 → 合格）

| 不合格 | 问题 | 合格 |
| --- | --- | --- |
| `fix: Fix REPL message replacement` | 英文主题（`language: zh` 拒绝） | `fix(message-table): 修正 REPL 下不替换异常信息的情形` |
| `fix: 修正 REPL 下异常信息没有被替换` | 描述式标题，写的是现象 | `fix: 让 REPL 也应用查询表中的替换文本` |
| `fix: 修正 _PyErr_Display() 的改写结果在 REPL 下被丢弃，因为 tb 为 NULL 时没有挂回异常` | 标题当正文用，超 72 列 | 标题同上；把 `tb` 为 `NULL` 的原因写进正文首段 |
| `chore: 更新代码` | 占位式标题，等于没写 | `build(pcbuild): 把 message_table.c 编进 pythoncore` |

正文对照：

```
不合格：复述标题、过程记录、"顺手"式附注
  REPL 下异常信息没有被替换。
  先试了改 sys.excepthook，没用，又顺手把 Include 里的声明也删了。

合格：先讲为什么，再讲怎么改与怎么验证
  背景：REPL 不走 sys.excepthook，且 _PyErr_Display() 只在 tb 非空时把改写后的副本
  挂回异常；交互式输入没有 traceback 对象，替换结果被丢弃。
  改动：改写后无条件保留 traceback；补充脚本与 REPL 两种入口的测试。
  验证：Release x64 构建后 python -c "1/0" 与 >>> 1/0 均显示替换文本；表里没有的
  消息保持原文。
```

### 提交前检查清单

- [ ] 标题是中文祈使式短句，说"做什么"而不是"什么现象"，不加句号
- [ ] 标题 ≤ 60 列（约 30 字），技术名词未翻译
- [ ] 正文首段讲清楚"为什么"，每条断言都可复核（命令、文件、行号、数字）
- [ ] 无情绪化、描述式、占位式、不确定、未量化的措辞
- [ ] 改动的是 C 代码时，已按上面第 2、3 步真实构建并实跑四类情形
- [ ] 新增的注释、docstring 是英文（见"语言政策"）
- [ ] `git status` 中无构建产物与日志，且 `lookup-table.txt` 的改动确实是要分发的表内容
- [ ] 破坏性变更已用 `breaking` 字段标注，移植自上游的改动已在正文注明原 commit

示例：

```
fix(message-table): 修正 REPL 下不替换异常信息的情形

背景：REPL 不走 sys.excepthook，且 _PyErr_Display() 只在 tb 非空时把改写后的
副本挂回异常；交互式输入没有 traceback 对象，替换结果被丢弃。
改动：改写后无条件保留原 traceback；补充脚本与 REPL 两种入口的测试。
验证：Release x64 构建后 python -c "1/0" 与 >>> 1/0 均显示替换文本；表里没有的
消息保持原文。
```

## 已知文档债（改到相关文件时顺手对齐）

- **多参数 `SyntaxError` 不会被替换**（`compile()`、`exec()`、`import`、REPL 输入产生的都是这种；
  只有 `raise SyntaxError('msg')` 那种单参数形态会）。v3.15.0 实测：`_PyErr_Message_Replace()`
  对 `filename` 与 `lineno` 同时非 `None` 的 `SyntaxError` 返回 `NULL`，而逐步复刻
  `exception_with_message()` 却全部通过，说明分歧在 C 路径内部；未定位、未修复。完整复现与影响
  见 [FORK-NOTICE.md](FORK-NOTICE.md) 的 "Known defect" 与
  [docs/development.md](docs/development.md) 的"已知缺陷"。改这条路径前先读那两节，别照着
  README 里"SyntaxError 能替换"的旧说法做验证。
- [Include/message_table.h](Include/message_table.h) 第 4 行的注释写着"见 `attach.patch`"，这个
  文件在仓库里不存在，指向它的句子应当改掉。改这句话要动 C 头文件，按上面第 1 步必须真实
  构建一次，不要只为文案顺手改。
- [docs/development.md](docs/development.md) 的安装包一节已按 v3.15.0 的真实构建回填（MSVC ARM64
  工具集与 VSProcessesRunning 8006、临时的 `README.rst` 标记、`doc` 包依赖 Sphinx 输出）。
  两条已纠正：`/p:OutputName` **确实**决定 bundle 的最终名字（命令行全局属性优先于工程里的
  `<OutputName>`，不带它就是 `python-3.15.0-amd64.exe`）；安装包的身份字符串已全部改为
  Kawaii-Python（显示名、发布者、`SupportUrl`）。仍未落地的只剩 `buildrelease.bat` 的 fork
  副本，见该文件末尾的"已知未做"。
- 上游文件自带的 CJK 只出现在 `Lib/test/**` 的测试数据与 `Doc/**` 的示例里，属于既定内容，
  不要顺手"清理"。
