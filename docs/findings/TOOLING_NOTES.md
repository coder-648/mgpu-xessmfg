# 工具链问题：命令挂起的原因与规避规则

实测日期：2026-11-06　环境：VS Code Remote 1.143.1（`~/.vscode-server`）+ Ubuntu 24.04 + WSL2

---

## 1. 现象

Cline 执行命令后经常报：

```
[Shell integration did not report command completion (this happens e.g. inside ssh or
nested shells); output was captured with a timing heuristic, may be incomplete ...
```

命令其实**已经跑完了**，但 Cline 等不到"结束"标记，于是一直等到超时。

## 2. 诊断过程（结论：环境没问题）

| 检查项 | 结果 |
|---|---|
| `TERM_PROGRAM` / `TERM_PROGRAM_VERSION` | `vscode` / `1.143.1` |
| `VSCODE_SHELL_INTEGRATION` | `1` |
| `TMUX` / `STY` | 未设置（不是 tmux/ssh 嵌套） |
| `PROMPT_COMMAND` | `__vsc_prompt_cmd` ← **集成已加载** |
| `declare -F \| grep vsc` | `__vsc_prompt_cmd` / `__vsc_preexec` / `__vsc_command_complete` 等全部存在 |

**所以不是"没装集成"，而是特定命令会打断集成发出的结束标记。**

## 3. 真正的根因：命令里带换行

逐类实测：

| 命令特征 | 结果 |
|---|---|
| 单行 + 重定向到文件 | ✅ 正常返回 |
| 单行 `powershell.exe`（历史上最常挂的一类） | ✅ 正常返回 |
| **命令字符串含真实换行**（heredoc、多行脚本） | ❌ **必挂** |
| 超长复合命令（一条里 5–8 个 `;` 加大量引号） | ❌ 偶发 |

原因：命令里出现换行时，bash 进入 **PS2 续行状态**并回显 `>` 提示符，`__vsc_command_complete` 的结束序列与 Cline 的解析随之错位。

终端里能直接看到证据：

```
./scripts/rcmd.sh t_heredoc 'python3 - <<EOF
> print("heredoc ok")
> EOF'
```

那条命令**执行结果是对的**（`temp/cmd/t_heredoc.log` 里确实有 `heredoc ok`），只是 Cline 白等了。

## 4. 规避规则（硬性）

1. **`run_commands` 的每条命令必须是单行**，绝不出现真实换行。
2. 需要多步逻辑 → **用编辑器工具写成 `.sh` 文件**，再单行调用：
   ```
   ./scripts/rcmd.sh <名字> -f scripts/xxx.sh
   ```
3. 涉及 `cmd.exe` / `powershell.exe` / 长子进程 → 一律走 `scripts/rcmd.sh`，
   它用 `setsid` + `</dev/null` + 重定向让进程组脱离 pty。
4. 所有输出写到文件，再按需读取（这也正是 PLAN 补充 1 的要求）。

## 5. 辅助脚本：`scripts/rcmd.sh`

```bash
./scripts/rcmd.sh <name> '<single-line command>'   # 跑一条命令
./scripts/rcmd.sh <name> -f path/to/script.sh      # 跑一个脚本文件
```

行为：
- `setsid` + `< /dev/null` + `> temp/cmd/<name>.log 2>&1`，进程组与 pty 脱钩；
- 最多等 2 秒；完成则打印 `[rcmd] <name> finished rc=...`，未完成则提示稍后读日志；
- 退出码写入 `temp/cmd/<name>.status`。

## 6. 万一还是挂了怎么办

命令的工作**已经完成**，只是通知丢了。直接读输出文件即可，不要重跑：

```bash
cd <REPO> && cat temp/cmd/<name>.log
```
