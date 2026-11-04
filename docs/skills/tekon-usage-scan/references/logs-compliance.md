# openvela 官方 AI 日志合规对照

来源：`contest2026_427_xinpingqihe/docs/zh-cn/contest_2026/ai_coding_log_guide.md`（虚拟机内 openvela 文档）与仓库 `logs/README.md`。

## 1. 官方只自动采集四种工具

- **Claude Code**（含 AIoT-IDE 的 Claude Code 形态）
- **AIoT-IDE**
- **OpenCode**
- **Codex**

只有这四种工具的日志能被官方采集脚本识别，才可能计入有效工时。

> 仓库 `logs/README.md` 把第 4 种写成 `kiro`，与官方手册的 `AIoT-IDE` 不一致，属于文档笔误；以官方手册为准。

**DSH、WorkBuddy、K3DSH、SUPERAGENT、MiMo 直连、OpenAI 兼容端点等，都不在官方采集范围。** 它们的用量可以统计，但必须标 `official=false`、`collection_mode=local-session-store`，只能作为补充台账，不能冒充官方工时。

## 2. 目录与文件

```text
logs/
  <github_login>/
    manifest.json
    <YYYY-MM-DD>/
      claude-code__<session_id>.jsonl
      codex__<session_id>.jsonl
      aiot-ide__<session_id>.jsonl
      opencode__<session_id>.jsonl
```

- 一人一个目录，目录名必须是 GitHub login。
- 一天一个子目录，日期用本地时区（`Asia/Shanghai`）。
- 一个会话一个 JSONL，文件名 `<tool>__<session_id>.jsonl`（双下划线）。
- **只提交 JSONL 本身**；不要提交中间数据库、截图、密钥、完整 CLI transcript 原始包。
- 采集必须在含 `.repo/` 的 openvela 工作区内进行。

## 3. manifest.json

仓库现有 manifest 的关键字段：

```json
{
  "schema_version": "1.0",
  "team_id": "contest2026_427_xinpingqihe",
  "github_login": "XPQHyue",
  "generator": "manual-reconstruct@1.0",
  "sessions": [
    {
      "session_id": "...",
      "tool": "claude-code",
      "started_at": "2026-08-26T16:13:53.998Z",
      "last_event_at": "2026-08-27T00:37:57.182Z",
      "event_count": 488,
      "file_path": "2026-08-26/claude-code__....jsonl",
      "collection_mode": "cli",
      "health": "ok"
    }
  ]
}
```

审计时逐项核对：

1. `sessions[].file_path` 是否存在；
2. 文件实际行数是否等于 `event_count`；
3. 文件内最小/最大 `ts` 是否与 `started_at` / `last_event_at` 一致（>1 秒记异常）；
4. `health` / `collection_mode` 是否缺失。

扫描器把这些写进 `official_logs_audit.csv`，**不会修改官方文件**。

## 4. 事件字段

每行一个 JSON 事件，常用字段：

| 字段 | 说明 |
|---|---|
| `ts` | ISO 时间戳 |
| `seq` | 会话内序号，应连续 |
| `role` | `user` / `assistant` |
| `text` / `thinking` | 文本 / 思考 |
| `tool_name` / `input` / `output` | 工具调用 |
| `model` | 模型名 |
| `tokens_in` / `tokens_out` | 输入 / 输出 token |

注意：

- Claude Code 一次响应会拆成多条 assistant 事件，`tokens_in` 会重复；统计输入时必须按“连续 assistant + 相同 tokens_in”去重。
- `tokens_out` 很多事件为 0，只有末条或部分事件有值，求和是**下限**。
- Codex 日志可能完全没有 `tokens_in` / `tokens_out` 字段；此时 token 可计为 0，但事件数仍可审计。

## 5. 官方采集命令

在 openvela 工作区内：

```bash
# 安装/初始化采集（team-id 与 github-login 必填）
./install.sh --team-id contest2026_427_xinpingqihe --github-login <login>

# 列出可导出的会话
contest-snapshot --list

# 导出指定会话
contest-snapshot --session <session_id>

# 确认写入 logs/
contest-snapshot --confirm
```

校验：

```bash
python3 validate-log.py logs/<login>
```

校验内容通常包括：JSONL 每行可解析、`seq` 无断档、事件字段完整、manifest 的 `event_count` 与文件一致。

## 6. 本技能的映射

| 本技能字段 | 官方对应 |
|---|---|
| `summary.csv.official_tool` | 是否属于官方 4 工具 |
| `summary.csv.official_collectable` | 是否可被官方采集脚本识别 |
| `summary.csv.collection_mode` | `cli` / `official-logs` / `local-session-store` |
| `official_logs_audit.csv` | manifest + JSONL 的逐文件审计 |
| `manifest.json` | 对齐官方 manifest 的补充清单（不是官方 manifest） |

**不要**把本技能生成的 `manifest.json` 直接放进仓库 `logs/<login>/`。官方 manifest 有固定 schema 和采集来源；补充台账应单独存放（例如 `docs/token_usage/supplementary/`）。

## 7. 提交前检查清单

- [ ] 目录名是 GitHub login，不是显示名。
- [ ] 日期目录用本地时区。
- [ ] 文件名 `<tool>__<session_id>.jsonl`，双下划线。
- [ ] 每个 JSONL 每行一个事件，`seq` 连续。
- [ ] manifest 的 `event_count` 与文件行数一致。
- [ ] 没有 API key、token、内网 IP、个人信息。
- [ ] 没有被官方采集的工具混进 `logs/` 冒充官方日志。
- [ ] 补充台账单独放，并在 README 标明 `official=false`。
