---
name: tekon-usage-scan
description: 跨 Agent 统计 TEKON/token 用量：扫描 DSH、Claude Code、Codex、WorkBuddy 与 openvela 官方 LOGS，按 message id 去重，输出去重后的 summary/sessions/manifest/README/Excel，并区分官方可采集与补充台账。
whenToUse: 用户要求统计“这个项目一共花了多少 token / TEKON”、汇总多个 Agent（DSH 原体、VM DSH、K3DSH、WORKAGENT、MIMOCLAUDE、SUPERAGENT 等）的用量、核对 openvela 参赛 AI 日志，或要把本地会话沉淀成可审计的 token 台账时。
metadata:
  version: "1.1.0"
  owner: "DSH 原体"
  outputs: "summary.csv, sessions.csv, manifest.json, official_logs_audit.csv, platform_ledger.csv, README.md, TEKON_usage_report.xlsx"
---

# TEKON 用量扫描

## 这个技能解决什么

同一个项目往往同时有 Windows DSH、虚拟机 DSH、WSL 常驻 Agent、WorkBuddy、K3DSH、Claude Code 等多个 Agent。每个端的会话存储格式不同，而且**崩溃恢复、复制会话、子代理会把同一批 token 重复记很多遍**。直接按文件求和，数字会虚高几倍。

本技能做四件事：

1. **发现**：找出本机（以及可选远程主机）上所有 Agent 的会话存储；
2. **解码**：DSH 的多帧 zstd、WorkBuddy 的 SQLite+JSONL、Claude/Codex 的 transcript、官方 LOGS 的 JSONL；
3. **去重**：DSH 按 assistant `message.id` 全局去重，WorkBuddy 按 `providerData.messageId` 去重，官方日志按 manifest 文件去重；
4. **出账**：生成可审计的 CSV / JSON / Markdown / Excel，并把“官方可采集”与“补充台账”分开标注。

## 三条铁律

1. **只认官方 4 工具**：Claude Code（含 AIoT-IDE）、AIoT-IDE、OpenCode、Codex。DSH、WorkBuddy、K3DSH、SUPERAGENT 等一律 `official=false`、`collection_mode=local-session-store`，只能作为补充台账，**不能冒充官方工时**。
2. **同一批用量不能相加**：平台账单（MiMo / DSH 平台 CSV）与本地会话是同一批 token 的两个视图；本地 transcript 与官方 LOGS 也可能重叠。默认只选一个主口径，其余列为交叉校验。
3. **先快照，再解释**：所有数字都带 `generated_at` 和每端 `last_event_at`；会话仍在增长时，报告必须写“快照值”，不能写成最终值。

## 快速开始

需要 **Node >= 22.5**（DSH 的 zstd 解码依赖 `node:zlib`；WorkBuddy 依赖 `node:sqlite`）。Excel 需要 Python 3 + `openpyxl`。

```bash
# 0) 检查环境
node --version
python -c "import openpyxl; print(openpyxl.__version__)"

# 1) 初始化配置：自动发现本机 DSH / Claude / Codex / WorkBuddy
node scripts/tekon-scan.mjs init \
  --project-dir "E:/DSHGZKJ" \
  --out "E:/DSHGZKJ/phywear/token-usage"

# 2) 编辑 <out>/tekon.config.json：
#    project.roots / project.keywords / agent_rules / sources[].enabled / official_logs.path

# 3) 扫描：只读，不改任何原始会话
node scripts/tekon-scan.mjs scan --config "<out>/tekon.config.json"

# 4) 人工复核 <out>/candidates.csv 的 review 行，
#    把要纳入/排除的 session_key 写进 <out>/decisions.json
#    格式：{"<session_key>": "include"} 或 {"<session_key>": "exclude"}

# 5) 出账
node scripts/tekon-scan.mjs report --config "<out>/tekon.config.json"

# 6) 可选 Excel
python scripts/build_xlsx.py --dir "<out>"

# 7) 校验
node scripts/tekon-scan.mjs validate --dir "<out>"
```

> Windows PowerShell 5.1 下不要用 `&&` 连接命令；逐行执行。路径含空格时加引号。上面的正斜杠写法在 Windows / Linux 都可用。

## 推荐工作流

1. **先跑 `scan`，不急着下结论。** 看 `candidates.csv` 里 top 会话的标题、首问、cwd、score。
2. **校准关键词和 roots。** 如果明显是项目的会话落在 `review`，补关键词；如果无关会话被 `include`，补 `exclude_keywords`。
3. **用 `agent_rules` 拆平行 Agent。** 同一台机器上跑多个命名 Agent 时，不要靠路径区分，靠提示词/标题里的命名。
4. **远程端先 `collect` 再 `merge`。** 不要让扫描器直接写远程文件系统。
5. **报告和原始会话分开存放。** 会话仍在增长时，保留 `raw/` 快照，报告可重生成。
6. **最后跑 `validate`**，确认 totals、included sessions、manifest 三者一致。

## 配置模型

`init` 会生成 `tekon.config.json`。最小可用结构：

```json
{
  "project": {
    "name": "phywear",
    "repo": "open-vela/contest2026_427_xinpingqihe",
    "roots": ["E:/DSHGZKJ", "/home/xpqh/work/contest2026_427_xinpingqihe"],
    "keywords": ["phywear", "openvela", "黄山派", "contest2026_427"],
    "exclude_keywords": ["无人机", "学习通"],
    "since": "2026-08-01T00:00:00+08:00",
    "until": null,
    "timezone": "Asia/Shanghai",
    "include_score": 50,
    "exclude_score": 0
  },
  "agent_rules": [
    {
      "match": "你命名为K3DSH",
      "agent_id": "k3dsh",
      "agent_name": "K3DSH（Windows 11 独立进程）",
      "platform": "Windows/local"
    }
  ],
  "sources": [
    {
      "id": "dsh-origin",
      "type": "dsh",
      "enabled": true,
      "path": "C:/Users/Administrator/.dsh",
      "agent_name": "DSH 原体（Windows）",
      "platform": "Windows 11",
      "host": "Administrator",
      "official": false
    },
    {
      "id": "official",
      "type": "official_logs",
      "enabled": true,
      "path": "E:/DSHGZKJ/repo/logs/XPQHyue",
      "login": "XPQHyue",
      "team_id": "contest2026_427_xinpingqihe",
      "official": true
    }
  ],
  "manual_agents": [],
  "output": { "dir": "E:/DSHGZKJ/phywear/token-usage" }
}
```

完整样例见 `assets/tekon.config.example.json`。

### 项目归属怎么判

每个会话会得到一个 `score` 和 `suggested`：

| 证据 | 分值 |
|---|---|
| `cwd` 落在 `project.roots` 下 | +40 |
| 关键词出现在标题 | 每个 +50，单关键词封顶 +50 |
| 关键词出现在首条用户消息 | 每个 +30，单关键词封顶 +50 |
| 关键词只出现在后续用户消息采样（前 8 KB） | 每个 +5，封顶 +20 |
| 命中 `exclude_keywords` | −100 |
| 早于 `since` / 晚于 `until` | 强制 −100 |

- 单会话总分封顶 150。
- `score >= include_score`（默认 50）→ `include`；`score <= exclude_score`（默认 0）→ `exclude`；中间 → `review`。
- `review` 不计入总数，只出现在 `candidates.csv`。**不要为了让总数变大而把 review 全部 include**；先看首条用户消息和标题。

### agent_rules：拆平行 Agent

同一台机器可能同时跑 DSH 原体、K3DSH、统计会话等。`agent_rules` 按子串匹配 `title + first_user + cwd + agent_name`，命中后覆盖 `agent_id` / `agent_name` / `platform`：

```json
"agent_rules": [
  { "match": "你命名为K3DSH", "agent_id": "k3dsh", "agent_name": "K3DSH", "platform": "Windows/local" }
]
```

规则要**足够具体**。只写 `"K3DSH"` 会把“统计 K3DSH 用量的会话”也误判成 K3DSH。

## 远程端（VM / WSL）

推荐“先采集、再合并”，不要让扫描器直接写远程。把 `scripts/` 和一份远程配置复制过去：

```bash
# 远程主机
node scripts/tekon-scan.mjs collect \
  --type dsh --path /home/xpqh/.dsh \
  --config /tmp/tekon/vm.config.json \
  --agent-id vm-dsh --agent-name "VM DSH" --platform "VMware/Linux" \
  --out /tmp/tekon-vm

# 回到本机
scp ubuntu:/tmp/tekon-vm/raw/sessions.json ./raw/vm-sessions.json
node scripts/tekon-scan.mjs merge --inputs "./raw/sessions.json;./raw/vm-sessions.json" --out ./raw/sessions.json
```

- `collect --config` 会用远程配置里的 `project.roots/keywords` 做归属评分；不传就全落 `exclude`。
- 远程配置里的 roots 必须是**远程路径**（`/home/...`），不能只写 Windows 路径。
- `merge` 按 `session_key` 合并同一会话的不同采集批次，保留 token 更大的那份；**不要**按首条 assistant message id 合并，续写副本会丢量。
- WSL 用 `--agent-id superagent --agent-name "SUPERAGENT" --platform WSL2` 单独成行。

## 人工裁决

`decisions.json` 是唯一的人工覆盖入口：

```json
{
  "dsh:57c94eb9-...": "include",
  "workbuddy:06a6238b-...": "include",
  "claude:8ed01d94-...": "exclude"
}
```

`report` 会重新应用它，所以配置改了也可以重跑，不会丢裁决。

## 输出契约

`report` 生成：

| 文件 | 用途 |
|---|---|
| `summary.csv` | 每个 Agent 一行：会话/请求/prompt/cached/input/output/total，含 `official` 与口径 |
| `sessions.csv` | 每个会话一行：cwd、时间、模型、token、评分、是否计入、来源文件 |
| `manifest.json` | 对齐 `logs/` 的清单：schema_version / team_id / github_login / accounting / totals / agents / sessions / unallocated |
| `official_logs_audit.csv` | 官方 LOGS 逐文件审计：事件数、assistant 数、in/out、manifest 时间差、event_count 是否一致 |
| `platform_ledger.csv` | 手工平台账单（若配置了 `manual_agents`） |
| `README.md` | 结论、各端明细、合规对照、异常清单、复现方法 |
| `TEKON_usage_report.xlsx` | 可选；`python scripts/build_xlsx.py --dir <out>` |

### 统一口径

| 字段 | 定义 |
|---|---|
| `input_tokens` | 非缓存输入 |
| `cache_read_tokens` | 缓存命中（prompt 的子集） |
| `prompt_tokens` | 全部输入 = input + cache_read + cache_write |
| `output_tokens` | 输出 |
| `total_tokens` | prompt + output |

WorkBuddy / MiMo 的原始 `prompt_tokens` 已含 cache，扫描器会反算 `input = prompt - cache`，保证六列可横向比较。

## 各工具的数据位置

| 类型 | 默认路径 | 关键字段 |
|---|---|---|
| DSH | `~/.dsh/sessions/<cwd-slug>/<sid>/session.v4.jsonl.zstd` | `assistant/message.data.usage`，`message.id` |
| Claude Code | `~/.claude/projects/<slug>/<sid>.jsonl` | `message.usage.input_tokens/cache_read_input_tokens/output_tokens` |
| Codex | `~/.codex/sessions/**/rollout-*.jsonl` | `event_msg.payload.type=token_count` |
| WorkBuddy | `~/.workbuddy/workbuddy.db` + `projects/**/*.jsonl` | `providerData.rawUsage`，`providerData.messageId` |
| 官方 LOGS | `logs/<login>/<YYYY-MM-DD>/<tool>__<sid>.jsonl` + `manifest.json` | `tokens_in`/`tokens_out`；manifest 的 `event_count` |

## 常见坑

- **DSH 的 zstd 是多帧拼接**：`zlib.zstdDecompressSync` 只解第一帧，必须按 magic `28 b5 2f fd` 切帧。`scripts/dsh-codec.mjs` 已处理。
- **DSH 的 session 元数据在事件顶层**：`cwd` / `id` / `parentSession` 不在 `data` 里；读错会让所有会话 `cwd` 为空、score 归零。
- **DSH 会话目录是两层**：`sessions/<cwd-slug>/<session-id>/session*.zstd`，不是 `sessions/<cwd-slug>/session*`。
- **不要按文件求和**：VM 里同一对话可能有 3–8 个崩溃恢复副本，文件总量能虚高 2–3 倍；必须按 message id 去重。
- **WorkBuddy 的 usage 会重复**：同一请求的 `rawUsage` 会挂在多条 `function_call` 上，按 `messageId` 去重。
- **WorkBuddy 首条消息带 system-reminder**：先取 `<user_query>`，否则首问全空、评分全 0。
- **官方 LOGS 的 in 会重复、out 经常是 0**：按“连续 assistant + 相同 tokens_in”去重输入；`tokens_out` 求和是下限。
- **平台账单与本地会话不能相加**：MiMo 月度 xlsx / DSH 平台 CSV 是账单视图，本地 transcript 是会话视图。
- **不要把 API key 写进报告**：WorkBuddy 的 `models.json` 可能有明文 key；扫描器只读 transcript，不输出配置。
- **PowerShell 5.1 会吃 `$` 和括号**：长命令写成 `.ps1` 或 `.sh` 文件再跑；不要把多行脚本塞进 `ssh "..."`。

## 质量检查

```bash
node scripts/tekon-scan.mjs validate --dir "<out>"
```

必须全部为 `true`：

- `manifest.totals.total_tokens == sum(included agents)`
- `report_data.totals == manifest.totals`
- `report_data.included_sessions == len(manifest.sessions)`

再人工抽查：

- `summary.csv` 的 TOTAL 行是否等于各 agent 之和；
- `sessions.csv` 里 `included_in_total=yes` 的行数是否等于 `totals.sessions`；
- `official_logs_audit.csv` 是否所有 `event_count_match=yes`；
- 报告里的 `generated_at` 是否写清。

## 参考

- `references/accounting.md`：去重算法、口径推导、交叉校验公式。
- `references/logs-compliance.md`：openvela 官方 LOGS 目录/字段/校验要求。
- `assets/tekon.config.example.json`：完整配置样例。
