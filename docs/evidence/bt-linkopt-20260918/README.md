# 证据：蓝牙链路层优化尝试（连接参数 + ATT MTU）—— 含一次**被自己推翻**的结论

日期：2026-09-18 深夜
固件：`nuttx.bin` 2,556,508 B（15.24%）/ SRAM 496,420 B（94.68%）
被测：PhyWear 手表（SF32LB52）　中心设备：宿主 Intel AX201（BlueZ）

---

## 0. 结论先说（三件事，第二件是自我纠错）

| # | 事项 | 结论 |
|---|---|---|
| ① | **ATT MTU 显示错误** | ✅ **真 bug，已修**：手表页显示 `MTU 23` —— 那是**连接瞬间的快照**，不是协商值。现在监听 `att_mtu_updated`，显示**真实值 `MTU 517`**（tx=517 / rx=253） |
| ② | **连接参数请求** | ❌ **实测为 no-op**：请求 15~30 ms / latency 0，但 BlueZ 协商出来的是 `itv=24 → 30.0 ms`、`lat=0`、`sto=400`；**把请求关掉，读回来的参数一模一样**。我最初看到"往返延迟 135→90 ms"以为是它的功劳 —— **同代码开关对照证明不是**（见 §2） |
| ③ | **往返延迟 135→90 ms** | ⚠️ **差异真实（方差极小）但归因不明**：既不是连接参数（②），也不是 MTU 交换（关掉 MTU 交换后 MTU 仍是 517、延迟仍是 91 ms —— BlueZ 自己会交换）。**本报告不把它算作本次优化的收益** |

**所以这次真正拿到的是**：① 一个显示 bug 的修复（有真机截图为证）、②
链路参数从"谁都看不见"变成**设备侧可见 + 空口可读**（`itv/lat/sto`），
③ 一套可复现的**测量方法**（方差 ±1 ms，足以评估后续任何链路层改动）。
**没有拿到**可归因的延迟/吞吐提升。

---

## 1. 测量方法（新增 `pw_ble_central.py --perf`）

应用层**往返延迟**：宿主往文本特征 `…a003` 写一条短文本 → 等设备把
`echo: <原文>` 通过通知发回来。设备侧 `pw_text_write()` 是立刻回 echo 的，
中间没有人为延时，所以这条往返基本就是"BLE 连接间隔 × 往返跳数"。

吞吐（代理指标）：连着灌定长（12 B）文本，数 window 秒内成功往返的条数。

**为什么可信**：同一配置重复测，中位延迟抖动只有 **±1 ms**（§2 的 off1/off2/off3），
所以"135 vs 90"这种差异不可能是噪声。

---

## 2. 全部原始数据（`perf_*.txt` 是完整输出）

| 配置 | 设备上报 | 往返延迟 中位/最小/最大 (ms) | 定长吞吐 (条/s) |
|---|---|---|---|
| **优化前固件**（`perf_before`） | `mtu=23`（快照，假值） | **135.0** / 109.4 / 178.9 | 7.3 |
| 连接参数请求 **ON**（`perf_on1`） | `mtu=517 itv=24 lat=0 sto=400` | 89.5 / 87.4 / 113.5 | 9.9 |
| 连接参数请求 **OFF**（`perf_off1/2/3`） | **同上，完全一致** | 90.5 / 91.1 / 91.3 | 11.1 / — / — |
| MTU 交换也 **OFF**（`perf_base1`） | **仍是 `mtu=517`** | 91.0 / 87.8 / 119.9 | 10.8 |
| 宿主适配器**断电重启后**（`perf_cold1`） | `mtu=517 itv=24 …` | 90.1 / 86.7 / 122.5 | — |
| 恢复 1/1 后的复验（`perf_final`） | `mtu=517 itv=24 lat=0 sto=400` | 90.1 / 87.3 / 180.9 | 10.7 |

**从这张表能读出的三件事**：

1. **连接参数请求对 BlueZ 无效**：ON 与 OFF 三次，`itv/lat/sto` 与延迟都在同一水平
   （89.5~91.3 ms）。→ ② 成立。
2. **MTU 不是我们拉上去的**：把 `PW_BT_MTU_EXCH` 关掉（`#if` 为 0，交换代码根本没编进去），
   读回来仍是 `mtu=517` ⇒ **BlueZ 自己会做 MTU 交换**。我一开始把 `mtu updated tx=517`
   当成自己交换的成果，是**误判**。
3. **135 ms 不可复现**：优化后的所有配置（含"等价优化前"的 base1、含主机适配器冷启动的
   cold1）都在 90~91 ms。也就是说 135 那一次**没有被任何一个受控变量解释**。
   可能是那一次连接有我没识别到的条件（首次连接/主机状态），但**我没有证据**，
   所以不写成"优化带来的收益"。

---

## 3. 设备侧真机原文

```
[bt] mtu updated tx=23 rx=23                         ← 连接瞬间：默认 23
[bt] link up mtu=23
[bt] conn param update req rc=0 (interval 12~24 x1.25ms)   ← 请求已入队
[bt] exchange mtu rc=0
[bt] mtu updated tx=517 rx=253                       ← 协商后的真实值
[bt] exchange mtu done err=0
```

⚠️ **注意没有 `le param updated` 这一行** —— `bt_conn_cb.le_param_updated`
在这条链路（中心设备是 BlueZ）上**一次都没触发过**。这正是"不能只看回调"的教训：
如果只依赖它，界面永远显示不出连接间隔。所以改成在周期工作里用
`bt_conn_get_info()` 查**实时**参数（BT 工作队列线程上，每 500 ms，代价可忽略）。

---

## 4. 真机截图（`btlink_mtu_interval.png`）

手表「蓝牙」页在**已连接**状态下显示：

```
已连接
对端 04:EC:D8:F3:73:8D (public)
RX 1 · TX 0 · echo 0 · MTU 517 · 30.0 ms     ← 修复前这里是 "MTU 23"，且没有间隔
```

日志区还能看到三次 `[--] mtu updated`（`att_mtu_updated` 回调确实在跑）。

> 取证小坑：截这一张必须让链路**安静**。完整的 `pw_ble_central.py` 连上后还要
> 读/订阅/写，设备侧连续打十几行 `[bt] …`，与"以文本流出的像素"交叉会撕行、
> 被 pwshot 逐行校验拒收（复现两次）；而 pwshot 的重试是在**同一个 boot** 里再发一次
> `phywear shot` ⇒ 第二次 GUI 挂死 ⇒ 超时。所以另写了 `tools/phywear/pw_hold_conn.py`：
> 只连接、读一次状态、然后**静默保持**，给截图留出干净的 4 s 窗口。

---

## 5. 复核

```bash
cd docs/evidence/bt-linkopt-20260918 && sha256sum -c SHA256SUMS.txt

# 同代码 A/B：把 pw_btgatt.c 里的 PW_BT_FAST_CONN / PW_BT_MTU_EXCH 改 0/1
# 重编 + 烧录 + 用下面的命令测（每次约 1 分钟）
python3 tools/phywear/pw_ble_central.py --timeout 60 --text-test --perf --keep-connected

# 只读参数、不测性能（最快）
python3 tools/phywear/pw_ble_central.py --timeout 60 --text-test
```

**下一次想真正改善延迟，该动的是**：`2M PHY`（`bt_conn_le_phy_update`）与
`Data Length Extension`（`bt_conn_le_data_len_update`）—— 这两项直接减少空口时间，
而连接间隔在 BlueZ 下我们已经动不了。动手前先读控制器的能力
（`Read Local Supported Features`），别又白做一轮。
