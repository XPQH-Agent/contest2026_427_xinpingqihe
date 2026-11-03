#!/usr/bin/env python3
"""pw_accept.py —— PhyWear 蓝牙/回归 一键验收（设备侧，不需要手机）

把散落在 docs/16 里的每一条设备侧主张，收敛成**一条命令跑完、逐项打 PASS/FAIL**，
并把每一步的串口原文与 sha256 落盘。目的有两个：
  ① 让别人（评委/队友）不用读文档就能自己验一遍；
  ② 我自己改完东西也用它做冷启动回归 —— 免得"每一步都验过"变成"每一步都只验过一次"。

覆盖（全部为真机、冷启动复位后重跑）：
  A  阶段A裸探针      phywear bthci              → 判据③ HCI 链路通（status=0）
  B1 zblue host 栈    phywear cap bt             → [bt] bt_enable rc=0
  B2 GATT + 广播      （同上一条命令）            → gatt/conn cb/adv 三行 rc=0 + READY
  B2b 服务端自检      （同上）                    → SELFTEST PASS + 合矢量合理 + 打印采样耗时
  R1 帧率回归         phywear lang zh cap raw    → fps 与基线一致（默认 12）
  R2 主页截图         pwshot.py shot root        → >5 KB 且 sha256 与"金标"一致（UI 未变）
  R3 浸泡             phywear cap bt + 静置      → 无 panic/assert/重启

用法：
    python3 tools/phywear/pw_accept.py --out docs/evidence/accept-$(date +%Y%m%d-%H%M)
    python3 tools/phywear/pw_accept.py --out /tmp/acc --skip-soak      # 省掉最后 60 s

退出码：0 = 全过；1 = 有 FAIL（明细见 stdout 与 <out>/SUMMARY.txt）。
"""

import argparse
import hashlib
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from pw_serial import Session, dump, log  # noqa: E402

# UI 金标：主页 sha256。
#
# 2026-09-18 换过一次（这是**有意的改版**，不是回归）：
#   旧 d552c57b… = 加"蓝牙链路指示"之前（voice-20260918/02_home_with_voice_entry.png）
#   新 e356689d… = 加了标题右侧那枚"灰点 + 蓝牙"之后
# 未连接时是灰的、已连接时变蓝（蓝色状态见
# docs/evidence/btui-20260918/root_connected.png）。
# 这个检查的作用因此从"UI 没变"变成"主页与当前金标逐字节一致" ——
# 任何**非预期**的版式/配色改动仍然会被它抓住。
GOLDEN_ROOT_SHA = "e356689dc9e32387b5a9f121d23f6404ee52ca4e33bc4d8bd8267a535f0f7bfe"

PWSHOT = os.path.join(HERE, "pwshot.py")


def strip_ansi(b: bytes) -> str:
    return re.sub(r"\x1b\[[0-9;]*m", "", b.decode("utf-8", "replace"))


class Report:
    def __init__(self):
        self.rows = []

    def add(self, name, ok, detail=""):
        self.rows.append((name, ok, detail))
        log(f"{'PASS' if ok else 'FAIL'}  {name}  {detail}")

    @property
    def ok(self):
        return all(r[1] for r in self.rows)

    def text(self):
        w = max(len(r[0]) for r in self.rows) if self.rows else 10
        out = [f"{'判定':<6}{'检查项':<{w}}  说明", "-" * 78]
        for n, ok, d in self.rows:
            out.append(f"{'PASS' if ok else 'FAIL':<6}{n:<{w}}  {d}")
        out.append("-" * 78)
        out.append("结论：" + ("✅ 设备侧验收全过" if self.ok else "❌ 有未过项（见上表）"))
        return "\n".join(out)


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", required=True)
    ap.add_argument("--port", default="/dev/ttyUSB0")
    ap.add_argument("--baud", type=int, default=1000000)
    ap.add_argument("--fps-expect", type=int, default=12)
    ap.add_argument("--soak-sec", type=float, default=60.0)
    ap.add_argument("--skip-soak", action="store_true")
    ap.add_argument("--skip-shot", action="store_true")
    args = ap.parse_args()

    if not os.path.exists(args.port):
        log(f"致命：{args.port} 不存在（板子掉线？烧录与串口都会静默无输出）")
        return 2

    os.makedirs(args.out, exist_ok=True)
    rep = Report()
    t_start = time.time()

    def run_phase(label, steps, settle=0.5):
        """一个"冷启动 + 若干定时步骤"的采集单元，返回 (text, log_path)。

        ⚠️ settle 别乱加：`phywear` 的 30 s 心跳是**窗口量**
        （`fps = 本窗口实际推屏帧数 / 30`，窗口从 GUI 启动起算），
        所以"命令发出时刻"决定窗口盖住哪段后台活动 ——
        **实测**：复位后 t≈3 s 发出 → loops/s=204 fps=12；
        先静置 3 s、t≈6 s 发出 → loops/s=183 fps=11（可重复，不是抖动）。
        这里保持 0.5 s，与历史基线同一口径。"""
        sess = Session(args.port, args.baud, boot_timeout=60.0)
        try:
            sess.reset()
            sess.pump(settle)
            for secs, cmd in steps:
                if cmd:
                    sess.send(cmd, wait=False)
                sess.pump(float(secs))
        finally:
            lp, _ = dump(args.out, label, sess)
            txt = strip_ansi(bytes(sess.raw))
            sess.close()
        return txt, lp

    # ── A：阶段 A 裸探针 ───────────────────────────────────────────────
    txt_a, log_a = run_phase("A_bthci", [(8.0, "phywear bthci")])
    m = re.search(r"\[BTHCI\] rx \((\d+) B\): ([0-9a-f ]+)", txt_a)
    ok = bool(m) and m.group(2).strip().startswith("04 0e 04 06 03 0c 00")
    rep.add("A 阶段A裸探针 HCI 通",
            ok,
            f"rx={m.group(2).strip() if m else 'N/A'}  ({os.path.basename(log_a)})")

    # ── B1/B2/B2b：host 栈 + GATT + 广播 + 服务端自检 ────────────────────
    # ⚠️ 串口是**多线程共享**的（phywear 的 printf 与 ai_agent 的 syslog 同时在写），
    # 实测偶尔会把一行的中间冲掉、甚至整行丢（本轮就抓到一次：自检的
    # `read sensor -> 16 B` 与 `decode ...` 两行没了，只剩半行 `[bt] St" ->`）。
    # 那种情况下**功能是好的**（`SELFTEST PASS` 仍在），只是证据没抓到 ——
    # 所以这里必须重试，绝不能把"没抓到证据"判成"功能失败"。
    need = ["[bt] bt_enable rc=0", "[bt] B2 gatt register rc=0",
            "[bt] B2 conn cb register rc=0", "[bt] B2 adv start rc=0",
            "[bt] B2 READY name=PhyWear", "[bt] SELFTEST PASS",
            "SELFTEST decode", "sample took", "SELFTEST notify"]
    for attempt in range(1, 4):
        label = "B_b1b2" if attempt == 1 else f"B_b1b2_try{attempt}"
        txt_b, log_b = run_phase(label, [(30.0, "phywear cap bt")])
        miss = [k for k in need if k not in txt_b]
        if not miss:
            break
        log(f"  B 阶段第 {attempt} 次采集缺 {len(miss)} 项证据 {miss}（多半是串口并发写冲掉了），重试")
    rep.add("B1 bt_enable rc=0", "[bt] bt_enable rc=0" in txt_b,
            (re.search(r"elapsed=(\d+)ms", txt_b).group(0) if "elapsed=" in txt_b else ""))
    rep.add("B2 gatt register rc=0", "[bt] B2 gatt register rc=0" in txt_b)
    rep.add("B2 conn cb register rc=0", "[bt] B2 conn cb register rc=0" in txt_b)
    rep.add("B2 adv start rc=0", "[bt] B2 adv start rc=0" in txt_b)
    rep.add("B2 READY name=PhyWear", "B2 READY name=PhyWear" in txt_b,
            (re.search(r"identity=\S+", txt_b).group(0) if "identity=" in txt_b else ""))
    rep.add("B2b 服务端自检 PASS", "[bt] SELFTEST PASS" in txt_b)

    m = re.search(r"\|a\|=(\d+) mg", txt_b)
    mag = int(m.group(1)) if m else 0
    rep.add("B2b 合矢量合理(300~3000mg)", 300 <= mag <= 3000, f"|a|={mag} mg")

    m = re.search(r"sample took (\d+) ms \(budget (\d+) ms\)", txt_b)
    rep.add("B2b 采样耗时已实测且在预算内",
            bool(m) and int(m.group(1)) <= int(m.group(2)),
            m.group(0) if m else "未打印")

    # 通知路径演练：没有订阅者时 bt_gatt_notify 必须回 -ENOTCONN。
    # 这条链（CCC 订阅 → 周期采样 → notify）只在有订阅者时才跑，实验室里没有手机
    # ⇒ 靠这个演练证明"属性句柄能解析 + 调用链通"，并钉住 B3 的期望值。
    m = re.search(r"SELFTEST notify\(无订阅者\) rc=(-?\d+)", txt_b)
    rep.add("B2b notify 演练 rc=-ENOTCONN(-107)",
            bool(m) and int(m.group(1)) == -107,
            m.group(0) if m else "未打印")

    # 句柄结构：注册后 handle 必须非 0，且 CCC 紧跟特征值（通知靠这条解析）
    m = re.search(r"handles: value=0x([0-9a-f]+) ccc=0x([0-9a-f]+)", txt_b)
    if m:
        v, c = int(m.group(1), 16), int(m.group(2), 16)
        rep.add("B2b 句柄结构 value!=0 且 ccc==value+1", v != 0 and c == v + 1,
                f"value=0x{v:04x} ccc=0x{c:04x}")
    else:
        rep.add("B2b 句柄结构 value!=0 且 ccc==value+1", False, "未打印")
    rep.add("B2b 有属性 handle=0", "有属性 handle=0" not in txt_b)

    rep.add("B2b 全程无 RX 读错误", "rx read errno" not in txt_b)

    # ── R1：帧率 ───────────────────────────────────────────────────────
    # 实测量提醒：30 s 心跳的 loops/s 会随后台（ai_agent 的网络超时、控制台流量）
    # 在 183~205 之间浮动，fps 偶尔掉到 11（本轮验收首跑就撞上一次 183/11，
    # 紧接着单独重测 3 次全是 204/12）。所以这里**允许重测**：
    # 任意一次达标即 PASS；只有**每次都不到**才判 FAIL（那才是真回归）。
    vals = []
    hit = None
    for attempt in range(1, 4):
        label = "R1_fps" if attempt == 1 else f"R1_fps_try{attempt}"
        txt_r, log_r = run_phase(label, [(40.0, "phywear lang zh cap raw")])
        m = re.search(r"loops/s=(\d+) fps=(\d+)", txt_r)
        if m:
            vals.append(f"{m.group(1)}/{m.group(2)}")
            if int(m.group(2)) >= args.fps_expect:
                hit = m
                break
        else:
            vals.append("无心跳")
    rep.add(f"R1 帧率 >= {args.fps_expect}（同口径 30s 心跳，最多测 3 次）",
            hit is not None,
            (f"命中 {hit.group(0)}；" if hit else "3 次都没达标；")
            + f"观测 loops/fps = {', '.join(vals)}")

    # ── R2：主页截图 ───────────────────────────────────────────────────
    if not args.skip_shot:
        shotdir = os.path.join(args.out, "shot")
        got = None
        for attempt in range(1, 4):
            r = subprocess.run(
                [sys.executable, PWSHOT, "shot", "root", "--out", shotdir,
                 "--label", "root", "--settle", "12000", "--timeout", "140",
                 "--retries", "3"],
                capture_output=True, text=True)
            png = os.path.join(shotdir, "root.png")
            if os.path.exists(png):
                got = png
                break
            log(f"  截图第 {attempt} 次没出（pwshot 逐行校验，周期重绘会撕行），重试")
        if got:
            data = open(got, "rb").read()
            sha = hashlib.sha256(data).hexdigest()
            rep.add("R2 主页截图 >5KB", len(data) > 5120, f"{len(data)} B")
            rep.add("R2 主页 sha256 == 金标", sha == GOLDEN_ROOT_SHA,
                    sha[:16] + "…")
        else:
            rep.add("R2 主页截图 >5KB", False, "3 次都没抓到")
            rep.add("R2 主页 sha256 == 金标", False, "无图可比")

    # ── R3：浸泡 ───────────────────────────────────────────────────────
    if not args.skip_soak:
        txt_s, log_s = run_phase("R3_soak", [(args.soak_sec, "phywear cap bt")])
        hb = len(re.findall(r"\[phywear\] alive t=", txt_s))
        bad = re.search(r"panic|Assertion failed|Unhandled|SIGSEGV|watchdog", txt_s)
        rep.add("R3 浸泡期间有存活心跳", hb >= 1, f"{hb} 次心跳 / {args.soak_sec:.0f}s")
        rep.add("R3 浸泡无 panic/断言/重启", bad is None,
                bad.group(0) if bad else "")

    # ── 汇总 ───────────────────────────────────────────────────────────
    body = rep.text()
    print()
    print(body)
    print(f"\n用时 {time.time() - t_start:.0f}s；证据目录 {args.out}")
    with open(os.path.join(args.out, "SUMMARY.txt"), "w") as fh:
        fh.write(body + "\n")
    return 0 if rep.ok else 1


if __name__ == "__main__":
    sys.exit(main())
