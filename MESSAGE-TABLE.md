# 报错文案替换表（Message replacement table）

> **这是个人修改版 CPython，不是官方仓库。** 基线是官方 `v3.15.0`，
> 官方仓库在 <https://github.com/python/cpython>。

这个修改让 Python 的**报错文案可以被外部文本表替换**。表是一行一条的
「原始消息,十六进制 UTF-8 文案」：

```
division by zero,e7aca8e89b8be6898de4bc9ae68a8a30e694bee588b0e999a4e695b0e590a721
```

效果：

```console
$ PYTHON_MESSAGE_TABLE=./lookup-table.txt ./python -c "1/0"
Traceback (most recent call last):
  File "<string>", line 1, in <module>
ZeroDivisionError: 笨蛋才会把0放到除数吧!
```

交互式 REPL 同样生效——这一点很关键，因为 `sys.excepthook` 在交互式循环里
**不会被调用**，纯 Python 方案覆盖不到这个场景，所以需要 C 侧钩子。

## 改动了哪些文件

相对于官方 `v3.15.0`，共 3 处修改 + 3 个新文件：

| 文件 | 改动 |
| --- | --- |
| `Python/pythonrun.c` | 在 `_PyErr_Display()` 里接入替换（所有报错输出的唯一汇聚处） |
| `Python/pylifecycle.c` | 在 `_Py_Finalize()` 里释放缓存的表 |
| `Include/cpython/unicodeobject.h` | 引入新头文件 |
| `Makefile.pre.in` / `PCbuild/pythoncore.vcxproj` / `PCbuild/_freeze_module.vcxproj` | 把新源文件加入构建 |
| **新增** `Objects/message_table.c` | 表加载、hex 解码、查找、改写驱动、可替换性判定 |
| **新增** `Include/message_table.h` | 公共声明 |

全部是纯 C + 公共 API：**不 import 任何 Python 代码**，所以即使解释器的 import 机制当时已经损坏，
traceback 依然打得出来。

## 用法

表的查找顺序：

1. 环境变量 `PYTHON_MESSAGE_TABLE` 指定的路径；
2. `$PYTHONHOME/lookup-table.txt`；
3. 当前工作目录下的 `lookup-table.txt`。

表格式（UTF-8，每行一条，空行与 `#` 开头的注释会被忽略）：

```
# 原始消息,替换文案的 UTF-8 十六进制
division by zero,e7aca8e89b8be6898de4bc9ae68a8a30e694bee588b0e999a4e695b0e590a721
expected ':',e4b88de58aa0e58692e58fb7e79a84e5a4a7e7aca8e89b8be5b0b1e698afe4bda0e590a721
list index out of range,6c697374e5908ee99da2e79a84e59cb0e696b92ce4b88d2e2e2ee4b88de58fafe4bba5e5868de79c8be4ba862ce4bc9ae59d8fe68e89e79a842ce5919c
```

生成右侧十六进制：

```python
print(input().encode('utf-8').hex())
```

表在首次用到时读取一次并常驻进程。表缺失或损坏时**静默降级**，
永远回退到原文案，绝不会影响正常报错。

## 哪些异常能被替换

`BaseException.__str__` 从 `args[0]` 推导显示文案，但有些异常类型**自带 `__str__`**，
改 `args` 对它们没有效果。这些会被自动跳过，判定逻辑在 `Objects/message_table.c`
的 `replaceable_message()`，按**实例的实际行为**判断而不是写死类型名单：

| 异常 | 可替换 | 说明 |
| --- | --- | --- |
| `ZeroDivisionError` `IndexError` `TypeError` `AttributeError` `NameError` `RecursionError` `ImportError` | ✅ | 文案就是 `args[0]`，走快路径（只比 `tp_str` 指针，不构造字符串） |
| 继承 `BaseException.__str__` 的自定义子类 | ✅ | 同上 |
| `SyntaxError` | ✅ | 文案在 `.msg`，位置在 `args[1]`，两者都会被更新 |
| `KeyError` | ❌ | 显示时加引号 |
| `OSError` / `FileNotFoundError` 等 | ❌ | 显示为 `[Errno 2] ...`，前缀来自 `errno` |
| `UnicodeDecodeError` | ❌ | 文案由 `encoding/object/start/end/reason` 拼出 |
| 覆盖了 `__str__` 的子类 | ❌ | 显示固定文本 |

判定分两级：类型若用标准 `__str__` 就直接取 `args[0]`（零额外开销）；
否则实测 `str(exc)` 是否以 `args[0]` 开头——不以它开头就说明文案另有来源，跳过。

## 构建

```powershell
cd PCbuild
.\get_externals.bat --no-tkinter      # 取 OpenSSL/libffi/sqlite/xz/zlib-ng/zstd
msbuild python.vcxproj /p:Configuration=Release /p:Platform=x64 /m
```

Linux / macOS：

```bash
./configure --with-pydebug && make -j
```

**注意**：不要用 `msbuild pcbuild.sln /t:python`。解决方案里 `_tkinter` 被归在
`python` 目标下，跳过 Tcl/Tk 时它会失败并拖垮整个目标，尽管 `python.vcxproj`
本身只依赖 `pythoncore.vcxproj`。直接点项目文件即可。

## 性能

表只加载一次，每次报错做一次线性查找：

| | 每次报错 |
| --- | --- |
| 早期版本（每次报错都 `fopen` 并重建整张表） | 110.8 µs |
| 当前实现 | **0.307 µs** |

## 已验证

基线 **CPython v3.15.0**，Windows x64 Release（VS2022 17.14 / v143 / MSC 19.44）：

| 场景 | 结果 |
| --- | --- |
| 未配置表 | `ZeroDivisionError: division by zero`（原样） |
| `python -c "1/0"` | `ZeroDivisionError: 笨蛋才会把0放到除数吧!` |
| `python -c "[][0]"` | `IndexError: list后面的地方,不...不可以再看了,会坏掉的,呜` |
| 交互式 REPL `>>> 1/0` | 替换成功 |
| 交互式 REPL 里 tuple 形态 `SyntaxError` | 替换成功 |
| 表未命中 | 原样输出 |
| `open('/nope')` | `FileNotFoundError: [Errno 2] ...`（正确跳过） |
| 长时间运行 / 引用计数 | 桩测试确认堆对象归零 |
