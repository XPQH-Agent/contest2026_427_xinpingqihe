# ⑤-1 「主动 + 执行」场景证据（2026-09-15）

> 目标：让手表**自己**发现"有人在晃表"，然后**自己去跑实验并把结果说出来** ——
> 也就是赛道要求的 "proactive + acting"（纯对话机器人不算）。
>
> 本目录只放证据与复现脚本；结论口径以 `docs/03 §4.6` / `docs/06 §3.6` 为准。

---

## 1. 交付的链路

```
用户晃表
  │
  ├─(1) pw_watch_poll()   GUI 循环里 10Hz 读 IMU，窗口振幅 + 持续 2.5s + 60s 冷却
  │                       → "PhyWear 巡检：检测到持续摆动（窗口振幅 x.xx g，持续 x.x s）"
  │
  ├─(2) 事件写进手表 AI 消息日志（用户看得见），并 pw_ai_ask() 推给端侧 Agent
  │
  ├─(3) Agent 离线意图表命中 "检测到持续摆动" → phywear_run_experiment
  │                       {screen:"pendulum", seconds:10}
  │
  ├─(4) 工具经 pw_ai_request_open() 邮箱请求 GUI 线程开"单摆"页（LVGL 只在 GUI 线程动）
  │
  ├─(5) 单摆页自己 50Hz 采样 + 解算 g，算完把结果**登记**到 pw_ai 结果槽
  │                       （pw_ai_publish_result("pendulum", g, "m/s^2", T, detail)）
  │
  ├─(6) 工具**只等结果、读结果**（自己不再采样 IMU）→ 返回 value/unit/aux/detail/human
  │
  └─(7) 回复经 pw_ai_humanize() → 手表消息日志："AI: 已自动测重力加速度: g = 9.xx m/s^2 (T = x.xxx s)"
```

**架构要点（也是这次真正的修法）**：I2C 上**永远只有 GUI 那一路**在采样。
Agent 侧不再碰传感器 —— 这是 09-13 那句"实测十几秒整机卡住"的根因所在。

---

## 2. 本次定位并修掉的两个**真崩溃**（都是之前没人查到底的）

| # | 现象 | 根因（实测定位） | 修法 |
|---|---|---|---|
| 1 | Agent 没起来时，一推事件就 **panic 复位**（`task: phywear`） | `velaclaw_client_open()` 在总线未初始化时**也会成功**（tap 表是独立静态数据），随后 `velaclaw_ask → msg_queue_push → pthread_mutex_take` 撞上 `DEBUGASSERT(NXSEM_IS_MUTEX(sem))`（`nuttx/include/nuttx/semaphore.h:625`） | 新增 `message_bus_ready()`（`message_bus.c` 的 `g_initialized`），本地客户端在 `open()` 阶段就拒绝并返回 NULL；`pw_watch` 改走**受保护的 `pw_ai_ask()`**，不再自己开 velaclaw 客户端 |
| 2 | Agent 起来后，工具执行到末尾 **ASAN 报错**（`cJSON_Delete` 双重释放） | `pw_tool_emit()` 内部已经 `cJSON_Delete(root)`，我在新写的两条返回分支里又删了一次 | 去掉两处多余的 `cJSON_Delete(r)`（本文件既有分支的写法就是"emit 后只删 root"） |

> 崩溃 1 的严重性：它不只是主动场景的问题 —— **AI 教练页的按钮**同样会走
> `velaclaw_ask`，所以"Agent 没起来时点按钮"一样会把整机打崩。现在两条路都安全。

**回归证据**（模拟器，故意不起 Agent）：`sim-proactive-chain.log` 里可见
```
[velaclaw_client] message bus not ready (agent not running); refusing to open client
ai-coach: AI: not connected (start ai_agent first)
[phywear] proactive event not delivered (agent not running?); kept in the AI log anyway
```
**没有 assert，phywear 继续存活**（`alive t=61s loops/s=92`）。

---

## 3. 补上的**「Agent 开机自启」缺口**

文档一直写"端侧 loop 开机启动"，但**实际没有任何地方启动它**：

- 真机 `ps` 里没有 `ai_agent`；模拟器同样没有（都要手敲 `ai_agent &`）。
- 板级 `src/etc/init.d/rcS` 与 `rc.sysinit` **都是 0 字节**；`CONFIG_NSH_SYSINITSCRIPT="init.d/rc.sysinit"`，
  且 `sf32lb52_lchspi_ulp/src/CMakeLists.txt:52-53` 明确把这两个文件打进 ROMFS `/etc`。

**修法**：把 `ai_agent &` 写进板级 `etc/init.d/rc.sysinit`。

⚠️ **坑**：该文件会被 **C 预处理器**处理（构建命令是 `arm-none-eabi-gcc -E -P -x c ... rc.sysinit`），
所以**行首不能用 `#` 注释**（会被当成非法预处理指令，构建直接失败）。本次用的是行尾注释。

证据：`real-boot-agent-autostart.log`（开机就有 `ai_agent [6:100]`、`[bus] Message bus initialized`）、
`real-ps-ai_agent.log`（`ps` 里 `ai_agent` 任务与其 6 个 pthread）。

---

## 4. 结果上报的**三道有效性门**（为什么需要）

主动场景若把垃圾数字报给 Agent，就是"结论不实"。真机实测（手表静止放在桌上/挂在线缆上轻微晃），
单摆页能连续给出 **1.43 / 2.39 / 2.76 / 3.93 / 4.08 / 5.36 / 6.09 / 16.31 / 22.99 m/s²** 这类值 ——
页面自身的 `MOTION_STD(180 mdps)` 只够"察觉在动"，噪声/振动也能越过它。

因此 `phywear_pend.c` 在**发布给 Agent 之前**加了三道门（**只影响"要不要上报"，页面上的原始数值一律不过滤、不隐藏**）：

| 门 | 判据 | 为什么 |
|---|---|---|
| ① 独立数据 | 距上次评估至少 **50 个新样本（1s）** | `pend_analyze()` 每 tick 都跑，而数据窗每 HIST 拍才前进 —— 连续两次分析的是**同一段数据**，结果必然全等，"稳定性"检查会形同虚设（实测出现过 `5.3615 / 5.3614` 两次全等但仍被当成稳定发布） |
| ② 可复现 | 连续两次估计相差 **≤8%** | 噪声驱动出来的周期凑不出两次一致 |
| ③ 合理性 | g ∈ **5.0 .. 15.0 m/s²** | 单摆实验测地球 g 落在这个很宽的带（约 ±50%）之外，说明**这次测量本身失败**了 |

**效果（真机静止 70s 实测）**：

| | 修复前 | 修复后 |
|---|---|---|
| 上报给 Agent 的垃圾结果 | **~3 条/秒**（`real-still-*` 之前那版固件，log 里成片刷） | **70 秒内：1 条被拦 + 1 条漏过** |

拦截是**可审计**的（`real-still-withheld.log`）：
```
[phywear] pendulum result withheld: g=3.93 out of 5.0..15.0 m/s^2 (implausible measurement)
```

> ⚠️ **残余风险（如实写）**：窗口**内**仍可能漏过噪声值（70s 内漏过 1 条 `7.5240 m/s^2`）。
> 桌面振动能连续产生各种垃圾值，任何窗口都会偶尔漏。
> **所以主动场景的"测量值"只有在用户真的摆动时才有意义**；要根治得改单摆页的运动判据与周期估计
> （见 §8）。

---

## 5. 验证矩阵（哪些环节在哪儿验的）

| 环节 | 模拟器 | 真机 | 手指 |
|---|---|---|---|
| 持续摆动检测（窗口振幅/持续/冷却） | ✅ 合成摆动，t=45.9s 触发 | ✅ 早期会话已验真实晃动触发（日志含振幅/持续） | ⏳ 可复验 |
| 事件进手表 AI 日志 | ✅ | ✅ | — |
| Agent 命中意图 → 执行工具 | ✅ | ✅ | — |
| 工具请求开页（GUI 线程执行） | ✅ | ✅ | — |
| 页面解算 → 结果登记 | ✅ | ✅（注入事件触发） | — |
| 回复 humanize 进手表日志 | ✅ `AI: 已自动测重力加速度: g = 18.31 m/s^2 (T = 1.038 s)` | ✅ `AI: 已自动测重力加速度: g = 6.09 m/s^2 (T = 1.800 s)` | — |
| 三道有效性门 | ✅ 稳定合成信号照常发布 | ✅ 静止时拦截 + 如实回 unavailable | — |
| **真实晃表 → 得到可信的 g** | ❌ 合成波形不是真单摆（解出 g≈18 属无效测量） | ❌ 板子静止 | **⏳ 只能由用户挥手确认** |
| Agent 不在时不 panic | ✅ 无 assert、phywear 存活 | ✅ 由 §2 的守卫覆盖（真机 Agent 已自启，直接验证的是模拟器） | — |

**真机"无结果"时的诚实回执**（静止桌面实测）：
```
[tools] Executing tool: phywear_run_experiment
ai-coach: AI: no experiment result (device not moved, or this page has no readable result yet)
```

---

## 6. 数字

| 项 | 值 |
|---|---|
| 固件 | md5 `7dd26b346a001d0b536430489e363d7c`，**2,066,040 B**（flash 12.31%） |
| SRAM | **491,320 B / 512 KB（93.71%）**；⑤-1 净增 **+496 B**（490,824 → 491,320） |
| 相对最初基线 | +1,880 B（489,440 → 491,320）—— 已用掉"不超过 ~2 KB"额度的约 92% |
| 读回校验 | 6 段 16 KB 采样窗口逐字节一致（另有 3 段复验通过） |
| 帧率（单摆页，Agent 在跑） | 实测落在 **fps 8..16 / loops 19..39**（同一页重复采样波动大：该页 fps 取决于当前是否在解算/是否检测到运动） |
| 「Agent 在/不在」帧率 A/B | **未建立起来（如实写）**：两次尝试都失败 —— 第一次 `ps` 解析错了 pid（`pid=c`），第二次 `kill 6` 后 `ps` 里 `ai_agent` **仍然存在**（`agent still present: True`），所以拿不到干净基线。**因此不宣称任何帧率差**。可依据的是架构事实：本次是**删掉** Agent 侧 50Hz 采样（卡死根因），页面侧只多了一次加锁结构体拷贝 + 每秒 ≤1 条 syslog；SRAM 实测 **+496 B** |

---

## 7. 复现命令

```bash
# 真机：烧录后 Agent 应自动起来（无需手敲 ai_agent）
python3 docs/evidence/proactive-20260915/repro-dev-chain.py      # 开机自启 + 全链路 + 拦截证据
python3 docs/evidence/proactive-20260915/repro-fps-ab.py         # Agent 在/不在 的帧率 A/B

# 模拟器（需要先 ai_agent &，模拟器板级没有 rc.sysinit 机制）
ai_agent &          # NSH
phywear cap root &  # 合成摆幅 0.8Hz/600mg → 约 4s 后自动触发
# 只看链路：grep -aE "检测到持续摆动|client opened|Executing tool|ai-coach" sim.log
```

---

## 8. 已知限制与后续建议（不隐藏）

1. **真实晃表得到可信 g** 尚未验证（需要人手）。这是本项唯一没被机器验掉的环节。
2. **有效性门有残余漏洞**：窗口内的噪声值仍可能被上报（实测 1/70s）。根治方向：
   改单摆页的运动判据（要真正的"摆动"特征而不是角速度标准差）与周期估计
   （现在用自相关，受低频漂移影响大）。**这是一项独立的、值得单独排期的任务**。
3. **工具不再采样 IMU 是个取舍**：好处是消灭了两路 50Hz 抢 I2C；代价是"没有登记结果的页面"
   （例如频谱类页）现在只能回 `unavailable`，不再返回一段原始 accel 统计。
4. `ai_agent` 常驻会占一份 32KB 栈（Agent 任务）+ 若干 pthread 栈；真机 `ps` 可见 6 个线程。
