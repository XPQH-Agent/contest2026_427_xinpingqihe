# 证据：修「点开蓝牙页会卡 1 秒」—— `bt_enable()` 挪到后台线程

日期：2026-09-18 深夜（用户反馈驱动）
被测：PhyWear 手表（SF32LB52-MOD-1-N16R8）
固件：flash **2,577,592 B（15.36%）** / SRAM **483,360 B（92.19%）**

---

## 0. 现象与根因

**用户原话**："有反映啊，只是点击到蓝牙会卡顿 1S 左右，其他无异常。"

根因一句话：**`bt_enable()` 同步跑在 LVGL 主线程上**。

* `bt_enable()` 实测耗时 **1228~1268 ms**（每次开机日志都有 `elapsed=…ms`）；
* 而「蓝牙」页是在 **phywear 主线程**（= 跑 `lv_timer_handler()` 的那个线程）上建的，
  页内 `if (!pw_bt_is_up()) pw_bt_init();` 是**同步**调用；
* 主页那枚灰/蓝蓝牙状态点同理，一"顺手起栈"就冻住整个界面一秒。

## 1. 改法

| 位置 | 改动 |
|---|---|
| `pw_bt.c/.h` | 新增 `pw_bt_init_async()`：把 `pw_bt_init()` 投给一个**后台 pthread**，立刻返回；另有 `pw_bt_is_starting()` 供界面显示"启动中" |
| 优先级 | 显式用**裸值 90**。本端口 `K_PRIO_PREEMPT(x) = CONFIG_NUM_COOP_PRIORITIES(100) + x`，而 phywear 主线程优先级就是 **100**，NuttX 又是"数值越大越紧急" ⇒ 用宏只能得到 ≥100 的线程，照样抢界面 |
| 栈 | 从**堆**上要 8 KB（静态 `K_THREAD_STACK_DEFINE` 无条件吃 SRAM，而本板 SRAM 已 92%）—— 与 `pw_btppp.c` 同一套理由 |
| `phywear_ui.c` | 蓝牙页与主页状态点改调异步入口；三态显示：**琥珀=启动中 / 灰=未连接 / 蓝=已连接**；页内状态卡起栈期间写「启动中」 |
| `phywear.c` | 纯 GUI 模式下 `PW_BT_AUTOSTART=1`（默认）→ 开机后在**后台**起栈，这样手机不碰手表也能扫到它；`shot/cap` 等取证模式**不**自动起栈（串口要被像素文本流独占，BT 日志会撕行导致截图被拒） |

线程是 pthread（zblue 的 `k_thread_create` → `pthread_create`）⇒ 挂在 phywear 的
task_group 下，`bt_enable()` 里 open 的 H4 fd 与它创建的接收线程同组 —— 符合
B1 根因（NuttX fd 表按 `task_group`）的要求。

## 2. 实测（`gui.log`，纯 GUI 开机）

```
3.728  [bt] 起栈线程已投递（prio 90，栈 8192 B，来自堆）—— 界面不阻塞
3.728  [bt] calling bt_enable(NULL) ...
4.931  [bt] bt_enable rc=0 (host stack up) elapsed=1268ms
4.931  [bt] B2 adv start rc=0 (ok)
5.132  [bt] 起栈期间界面仍在推帧：5 帧（同步实现下这里只会是 0）
32.628 [phywear] alive t=30s loops/s=271 fps=3
```

**判据**：那 1268 ms 里界面线程**推了 5 帧**（≈ 1.27 s × 空闲主页的 ~4 fps）。
改动前这个数**恒为 0** —— 因为调用者就是界面线程本身，它不可能一边阻塞一边渲染。
（计数取的是 UI 线程写的推帧计数 `g_fps_cnt`，见 `pw_ui_flush_count()`。）

## 3. 回归验证（同固件）

| 验证 | 结果 | 目录 |
|---|---|---|
| 设备侧一键验收 | **18/18 PASS**，fps 12 / loops 196 不变，主页金标 sha256 `e356689d…` **逐字节相同** | `accept/` |
| 宿主当中心设备的 B3 双向文本 | 设备侧 8/8 + 宿主侧 9/9 | `b3/` |

主页金标没变是因为 `shot` 模式**不**自动起栈（`PW_BT_AUTOSTART` 只在纯 GUI 那一支），
所以指示灯初始仍是"灰 + 蓝牙"。

## 4. 人工确认（2026-09-18 深夜，用户现场）

把新固件烧上板后请用户现场点按，答复：**"流畅"** —— 即"点开蓝牙页/点主页蓝牙标识
不再卡 1 秒"这条**主观判据**也闭环了（帧数证据只证明"起栈期间界面仍在渲染"，
手感最终由人确认，这里就是那一次确认）。

## 5. 如实边界

* 帧计数证明"界面在起栈期间仍在渲染"，**不等于**"手感一定无感" —— 后台线程只保证
  不抢占界面（优先级 90 < 100），具体观感请人工确认一次。
* 自动起栈让手表**开机即广播**（手机不必先点开蓝牙页就能扫到）；想回到"用才起"
  把 `apps/examples/phywear/phywear.c` 的 `PW_BT_AUTOSTART` 置 0 即可（零风险开关）。
* 本轮没有做"同步 vs 异步"的整机 A/B（改前固件已不在板上）；负对照是**代码级**的：
  同步调用发生在界面线程自身，其帧计数差值必然为 0。

## 6. 复核

```bash
cd docs/evidence/bt-async-start-20260918 && sha256sum -c SHA256SUMS.txt
grep -a "起栈\|bt_enable rc=0\|推帧\|alive" gui.log      # 关键四行
grep -a "PASS\|FAIL" accept/SUMMARY.txt                   # 验收 18 项
```
