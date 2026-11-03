# 人工确认：触摸屏点按无异常（关掉 hpwork 8 KB 栈的残余风险）

日期：2026-09-18 深夜
类型：**人工现场确认**（不是自动抓取）—— 由用户在本机、板上固件为
`build: Sep 18 2026 23:01:55`（flash 2,576,888 B / SRAM 483,316 B / `PW_BT_PPP=1`）
时，直接在 **AMOLED 屏上点按**，观察后答复"**我点击了屏幕，未发现异常**"。

## 为什么需要人工

把 `CONFIG_SCHED_HPWORKSTACKSIZE` / `CONFIG_SCHED_LPWORKSTACKSIZE` 由 16,096 B
收到 8,192 B 之后，本固件里跑在 **hpwork** 队列上的只有两个 worker：

| worker | 出处 | 怎么验的 |
|---|---|---|
| BT HCI 接收 | `vendor/sifli/chips/sf32lb52/sf32lb52_bt_adapter.c` | **自动**：B3 双向文本往返压满（设备侧 8/8 + 宿主侧 9/9） |
| 触摸屏 | `vendor/sifli/boards/sf32lb52/drivers/input/ft6146.c` | **只能人工**：`ft6146_irq_handler()` 是真 IRQ 里 `work_queue(HPWORK, …)`，自动脚本造不出真实触摸事件 |

驱动确认是**真的在这块板上跑**（不是编进来没用）：

```
$ grep -n "FT6146" cmake_out/sf32lb52_lchspi_ulp_nsh_ai/.config
1389:CONFIG_INPUT_FT6146=y
1390:CONFIG_TOUCH_IRQ_PIN=41
```
且板级 `sf32lb52_lchspi_ulp/src/sifli_ap.c:398` 在 `#ifdef CONFIG_INPUT_FT6146`
下调用 `ft6146_touch_initialize()`（LCD 电源起来后再初始化，PR #31 之后加的处理）。

## 人工确认时用户粘贴的串口文本（节选）

> 说明：这是**用户现场粘贴**的控制台文本，不是本仓库脚本抓取的证据文件；
> 放在这里只是给"人工确认无异常"留一个旁证。原文里的乱码
> （`��验测重力加速度`、`nsh: oactive: command not found`）是串口并发写的已知现象
> （phywear printf 与 ai_agent syslog 交叉撕行，docs/07 坑 ⑦）。

```
SFBL
ABCDADC calibration data missing, use defaults
INFO: LSM6DS3 test device registered as /dev/lsm6dsl0, INT=31
NuttShell (NSH)
[agent] AI Agent - Vela AI Agent starting (build: Sep 18 2026 23:01:55)
Found lcd co5300 id:331100h
lcd_fb_init done.
Auto turn on display.
[phywear] t=18s PhyWear 巡检：检测到持续摆动 …
[phywear] alive t=30s loops/s=156 fps=6
[phywear] alive t=60s loops/s=197 fps=16
[phywear] alive t=90s loops/s=251 fps=7
[phywear] alive t=120s loops/s=278 fps=7
[bt] B3 connected: 04:EC:D8:F3:73:8D (public) err=0
[bt] exchange mtu done err=0
[bt] cmd #2 (4 B): 'ping'   →   [bt] cmd: pong
[bt] msg in #2 (16 B): 'hello-1789744411'  →  [bt] echo tx rc=0: 'echo: hello-1789744411'
[bt] B3 disconnected: … reason=0x13
[bt] adv busy(EALREADY), stop rc=0, retry start   →   [bt] adv restart rc=0 (ok)
[phywear] alive t=180s loops/s=259 fps=6
[phywear] alive t=210s loops/s=291 fps=3
[phywear] alive t=240s loops/s=292 fps=3
```

**可读出的信息**：点按之后设备**没有** panic / 断言 / 复位 —— 心跳一路打到
`t=240s`（在**同一 boot** 里），fps/loops 与改动前同区间。

## 追加确认（同日，更强的一条）

用户随后又补了一句："**有反映啊**，只是点击到蓝牙会卡顿 1S 左右，其他无异常。"

"**有反映**"这个信息很关键：它说明点按**确实产生了触摸事件**，也就是
`ft6146_irq_handler()` → `work_queue(HPWORK, …)` → `ft6146_data_worker()` →
`touch_event()` 这条链**真的执行了**（不只是"设备没崩"）。⇒ 8 KB 栈上跑这个
worker 是**实测通过**的，不是"没被触发所以没出事"。

（顺便：那句"点击到蓝牙会卡顿 1S"是**另一个** bug，根因是 `bt_enable()` 同步跑在
LVGL 主线程上，已修，见 `docs/evidence/bt-async-start-20260918/` 与 `docs/16 §13.6f`。
它和触摸栈无关。）

## 如实边界

* 这条确认的强度是"**点按后设备继续正常运行**"，**不是**"实测触摸 worker 的栈高水位"。
  本项目这份 NuttX 里没有 `nxsched_get_stackusage()`、`CONFIG_STACK_COLORATION`
  也没开，因此**拿不到**高水位数字 —— 要拿数字得另开一个配置（代价是再烧一轮）。
* 但这条风险的形态很明确：栈溢出在这块板上的表现是**整机静默**（2026-09-18 桥任务
  栈 2 KB 那次就是这样，没有 panic、串口直接停）。用户点按后心跳继续 ⇒ 没有发生。
