# 口径与去重算法

## 1. 四个数

| 字段 | 含义 | 关系 |
|---|---|---|
| `input_tokens` | 未命中缓存的输入 | ≥ 0 |
| `cache_read_tokens` | 命中缓存的输入 | prompt 的子集 |
| `prompt_tokens` | 全部输入 | `input + cache_read + cache_write` |
| `output_tokens` | 模型输出 | 与输入独立 |
| `total_tokens` | 计费/用量总量 | `prompt + output` |

不同工具原生字段不同，扫描器统一折算成上面六列（另加 `cache_write_tokens`）。
**不要把 cached 再加到 prompt 上**：WorkBuddy / MiMo 的 `prompt_tokens` 本来就含 cache，扫描器用 `input = prompt - cache` 反算。

## 2. 每个工具怎么读

### DSH

- 文件：`~/.dsh/sessions/<cwd-slug>/<session-id>/session.v4.jsonl.zstd`（旧版可能是 `.v3` 或无版本）。
- 多帧 zstd：每个事件一帧，必须按 magic `28 b5 2f fd` 切开逐帧解压；Node 的 `zlib.zstdDecompressSync` 只解第一帧。
- 会话元数据在 `session` 事件的**顶层**：`id` / `cwd` / `parentSession`，不在 `data` 里。
- 用量：`assistant/message.data.usage.{inputTokens,outputTokens,cacheReadTokens,cacheWriteTokens}`。
- 消息 id：`assistant/message.data.message.id`。
- 模型：`assistant/message.data.message.source.{provider,model}`。

### WorkBuddy

- 数据库：`~/.workbuddy/workbuddy.db` 的 `sessions` 表（标题、cwd、模型）。
- transcript：`~/.workbuddy/projects/<cwd-slug>/<session-id>.jsonl`。
- 用量：`providerData.rawUsage.{prompt_tokens,completion_tokens,prompt_cache_hit_tokens,prompt_cache_miss_tokens}`。
- 去重键：`providerData.messageId`。同一请求的 rawUsage 会挂在多条 `function_call` 上，不去重会翻几倍。
- 首条用户消息常被 `<system-reminder>` 包裹；扫描器优先取 `<user_query>`。

### Claude Code

- `~/.claude/projects/<slug>/<session-id>.jsonl`。
- 只看 `type == "assistant"`；用量在 `message.usage.{input_tokens,cache_read_input_tokens,cache_creation_input_tokens,output_tokens}`。
- 去重键：`message.id`。

### Codex

- `~/.codex/sessions/**/rollout-*.jsonl`。
- 用量在 `event_msg.payload.type == "token_count"` 的累计 `total_token_usage`；取会话内最大值，不要逐条累加。
- `input_tokens` 含 cached，扫描器做 `input = total_input - cached`。
- 用 `turn_context` 的条数近似请求数。

## 3. 去重：为什么按文件求和一定错

DSH / WorkBuddy 在崩溃恢复、复制会话、子代理派生时，会把同一段对话写成多个文件。实测一个项目里 90 个会话文件只对应 56 个唯一对话，按文件求和虚高 **2.7 倍**。

扫描器做两层去重：

1. **消息级**（DSH）：全局 `message.id` 首次出现归属最早会话；后续副本里重复的消息丢弃并计入 `duplicate_requests`。
2. **会话级**（merge）：按 `session_key` 合并同一会话的不同采集批次；保留 token 更大的那一份。

> 不要用“首条 assistant message id”当合并键：续写副本共享开场消息，但带有各自独有的后续轮次，按首消息合并会丢量。

## 4. 项目归属评分

`classify()` 的现行权重：

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
- `review` 不计入总数，必须人工在 `decisions.json` 里裁决。

### 为什么要给标题/首问更高权重

同一台机器上的 DSH 会同时跑本项目、插件开发、视频台、作业等会话，`cwd` 全都一样。只看 cwd 会把无关会话全算进来；只看关键词出现次数又会把“顺手提了一句 openvela”的长会话算进来。标题和首问是意图最强的信号，后续消息只作弱证据。

## 5. 官方 LOGS 的特殊处理

官方日志里，Claude Code 的一次 API 响应会拆成多条 assistant 事件（thinking / text / tool），**每条都重复同一个 `tokens_in`**。如果逐条累加，输入会被放大 2–3 倍。

扫描器的做法：会话内连续 assistant 事件且 `tokens_in` 相同 → 只计一次输入，输出照常累加；遇到非 assistant 事件重置。这个值是**可核验下限**，不是账单全量。

官方证据行 `included_in_total=false`，不进入补充总量。它只用于：

- 验证 manifest 的 `event_count` 是否与文件一致；
- 给出官方口径的 in/out 参考；
- 标出时间戳异常、缺字段的会话。

## 6. 交叉校验

生成后至少跑这四条：

```text
manifest.totals.total_tokens == sum(agents[included_in_total].total_tokens)
report_data.totals == manifest.totals
report_data.included_sessions == len(manifest.sessions)
sum(summary[included].total_tokens) == totals.total_tokens
```

`tekon-scan.mjs validate --dir <out>` 覆盖前三条；第四条可用 Excel 或一句 Node 复核。

## 7. 平台账单为什么不加

MiMo 月度 xlsx、DSH 平台 CSV 是**账单视图**，本地 transcript 是**会话视图**，两者描述同一批请求。把它们相加会重复计费。正确做法：

- 选本地会话为主口径，平台账单放 `platform_ledger.csv` 作交叉校验；
- 或者反过来，选平台账单为主口径，本地只作明细；
- 在 `manifest.accounting.note` 里写清选了哪个。

## 8. 快照

会话随时在增长。`report` 把生成时间写进 `manifest.generated_at`；`sessions.csv` 每行有 `last_event_at`。引用数字时必须带采样时间，不能把 22:49 的快照说成“最终”。
