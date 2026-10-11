# Conventions —— 本仓库提交信息的机器可执行规则

依据是 CPython 上游的 [Making good commits](https://devguide.python.org/getting-started/pull-request-lifecycle/#making-good-commits)
与 [Accepting and merging a pull request](https://devguide.python.org/getting-started/git-boot-camp/#accepting-and-merging-a-pull-request)：
**只有语言换成中文**，结构、粒度、可复核性要求与上游一致。为什么这么定见
[AGENTS.md](../AGENTS.md)；键的写法与拒绝码见
[工具的键值参考](https://github.com/Flotiarenor/dsh-tool-git/blob/main/README.zh.md#conventions-keys)。

types: feat, fix, docs, style, refactor, perf, test, build, ci, chore, revert
language: zh
subjectMaxColumns: 72
subjectSoftColumns: 60
bodyMaxColumns: 100
requireBody: true
firstPerson: true
allowEmoji: false
banned: 幽灵修复, 莫名其妙, 暂时先, 先这样吧, 跑通了, 一大堆

## 自由文本（会原样进入工具的提示词）

**只有提交信息用中文**：C / Python 源码里的注释、docstring、命名一律英文（与上游一致）；
`FORK-NOTICE.md`、`AGENTS.md`、`.dsh/**` 是本 fork 自己的治理文档，用中文。

### 标题（subject）

- 中文祈使式短句，说明这个提交**做什么**，不写"现在…了"的描述式，不写现象。
  上游的写法是 "Make the spam module more spammy"，对应中文 "让 spam 模块更 spam"。
- 首行不加句号，不加 `!!!!`、`……` 之类的装饰标点；一个提交只做一件事，不夹带无关的
  格式化改动。
- 技术名词（CPython、REPL、PYTHON_MESSAGE_TABLE、msbuild、_PyErr_Display、
  PyUnicode_FromHex）保持原样，不翻译、不加引号。
- 移植上游补丁时，标题用本次改动的中文祈使句，并在正文注明原始 commit 与上游 issue。

### 类型前缀（本工具的要求，可选）

上游的提交标题没有 `type:` 前缀；本工具的 `git_commit` 要求 `type` 字段并把它拼在标题前，
所以标题形如 `fix(message-table): 修正 REPL 下不替换异常信息的情形`。类型取上面的白名单，
按"改动的性质"选，不要按"改了哪个文件"选。

若这次改动正好对应某个上游 issue，可以恢复 CPython 的 `gh-NNNNN: <标题>` 形式：把
`gh-NNNNN` 加进本文件的 `types` 列表，然后传 `type: gh-NNNNN`、省略 scope、`subject` 只留
标题。本 fork 没有自己的问题跟踪，**不要凭空编号**；普通改动的 issue 关联一律走
`Refs: gh-<编号>` footer。

### 正文（body）

- 用中文，第一段回答"为什么"：问题是什么、不这么改会怎样。首行已经说清"是什么"，
  不要用正文复述标题。
- 后续段落补细节、权衡与被否掉的方案；贴命令与输出时，命令本身保持英文原样。
- 过程记录不写入提交信息：先试 A 再试 B、rebase、merge from main、"根据评审意见调整"
  这类内容属于 PR 或 issue，上游把这类正文明确列为反面例子。
- 每条断言都要可复核：命令、文件与行号、构建配置（如 Release x64）、观察到的输出或数字。
  只写现象、过程或自我评价的正文不合规；写不出证据就写"未验证"。
- 正文按段换行（≤ 100 列），一个段落不要挤成一行；段落之间空一行。
