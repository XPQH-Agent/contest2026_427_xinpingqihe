#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""pw_bt_gui.py —— 桌面版「PhyWear 蓝牙测试」（点一下就测）

为什么要有它：
  命令行那条 `pw_ble_central.py` 已经能完整验一遍手表蓝牙，但它要求使用者会开
  终端、会敲参数、会读 PASS/FAIL 表。验收/演示时这层摩擦不值得 —— 所以做一个
  能放在桌面上双击的窗口：按「开始测试」，剩下的（扫描→连接→读传感器→收通知
  →写命令→文本往返）自动跑完，逐项打勾并给出大字结论。

**它自己不实现任何测试逻辑**：序列来自 `pw_ble_central.run_full_test()`，
与命令行共用同一份代码。否则 GUI 和 CLI 两份实现迟早跑偏，而两边都"看起来在
测同一件事" —— 那比没有 GUI 更糟。

依赖：python3-tk（Tk 8.6，本机已装）+ python3-dbus（已装）。
用法：
    python3 pw_bt_gui.py                 # 打开窗口并自动开始
    python3 pw_bt_gui.py --no-autostart  # 打开窗口，等按按钮
"""

import argparse
import os
import queue
import sys
import threading
import time
import tkinter as tk
from tkinter import font as tkfont
from tkinter import ttk

# 复用命令行那条序列（同目录）。
HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

import pw_ble_central as C  # noqa: E402

# 字体：Tk 默认字体（DejaVu Sans）没有汉字，会显示成方框。
# 按优先级挑一个系统里真的存在的 CJK 字体。
CJK_CANDIDATES = [
    "Noto Sans CJK SC", "Noto Sans SC", "Source Han Sans SC",
    "WenQuanYi Micro Hei", "WenQuanYi Zen Hei", "Droid Sans Fallback",
    "Microsoft YaHei", "DejaVu Sans",
]

COL_OK = "#1a7f37"
COL_FAIL = "#cf222e"
COL_DIM = "#6b7280"
COL_BG = "#f6f7f9"


def pick_font(root, size):
    fams = set(tkfont.families(root))
    for name in CJK_CANDIDATES:
        if name in fams:
            return (name, size)
    return ("TkDefaultFont", size)


class App:
    def __init__(self, root, autostart=True, timeout=20.0, text_test=True):
        self.root = root
        self.timeout = timeout
        self.text_test = text_test
        self.q = queue.Queue()
        self.worker = None
        self.stop_flag = False
        self.busy = False
        self.selftest = self._selftest_only()

        self.f_title = pick_font(root, 17)
        self.f_body = pick_font(root, 11)
        self.f_small = pick_font(root, 10)
        self.f_verdict = pick_font(root, 15)
        self.f_mono = ("DejaVu Sans Mono", 9)

        root.title("PhyWear 蓝牙测试")
        root.geometry("760x640")
        root.minsize(660, 520)
        root.configure(bg=COL_BG)

        self._build()

        # 把 pw_ble_central 内部的 log() 也接进窗口日志区 ——
        # 不然"扫描中/连接中"这些过程信息只打到 stdout，GUI 里一片安静。
        C.log = lambda m: self.q.put(("log", str(m)))

        self.root.after(100, self._pump)
        if autostart and not self.selftest:
            self.root.after(400, self.start)

    # ── 纯 GUI 自检：不碰蓝牙，只验证窗口本身能画出来 ──
    def _selftest_only(self):
        return "--gui-selftest" in sys.argv

    def _selftest_fill(self):
        demo = [
            ("找到蓝牙适配器", True, "/org/bluez/hci0"),
            ("扫描到 PhyWear", True, "/org/bluez/hci0/dev_5A_58_62_96_E7_57"),
            ("连接成功且服务已解析", True, ""),
            ("找到服务 e0f1a000-…", True, "service000e"),
            ("找到 sensor 特征 …a001", True, ""),
            ("找到 cmd 特征 …a002", True, ""),
            ("读 …a001 得到 16 B", True, "16 B"),
            ("解出加速度且 |a| 合理(300~3000mg)", True,
             "ax=-23 ay=37 az=-1013 mg |a|=1014 mg"),
            ("Notify 收到 >= 2 个包", True, "收到 2 个"),
            ("写 …a002 'ping' 成功", True, ""),
            ("找到 text 特征 …a003", True, ""),
            ("读 …a003 得到状态串", True,
             "PhyWear bt conn=1 sub=1 rx=2 tx=2 echo=1 mtu=23"),
            ("写文本后收到设备 echo（双向连通）", True,
             "写 'hello-1789723972' → 收到 ['echo: hello-1789723972']"),
            ("echo 内容与所写一致", True, "echo: hello-1789723972"),
        ]
        for row in demo:
            self.q.put(("row",) + row)
        self.q.put(("log", "[central] adapter = /org/bluez/hci0"))
        self.q.put(("log", "[central] 扫描到 'PhyWear' @ …"))
        # done 必须带 rows —— _finish() 是拿它算 npass/结论的。
        # 第一版这里只给了 info，于是窗口画得挺好看、结论横幅却一直是空的。
        self.q.put(("done", None, {"rows": demo,
                                   "info": {"peer": "dev_5A_58_62_96_E7_57",
                                            "mag": 1014.0,
                                            "echo": "echo: hello-1789723972",
                                            "packets": 2}}))

    # ── 界面 ──
    def _build(self):
        pad = {"padx": 12, "pady": 6}

        head = tk.Frame(self.root, bg=COL_BG)
        head.pack(fill="x", **pad)
        tk.Label(head, text="PhyWear 蓝牙测试", font=self.f_title,
                 bg=COL_BG, fg="#111827").pack(anchor="w")
        tk.Label(head,
                 text="被测设备：PhyWear 手表　|　宿主编译机通过蓝牙直接连手表做全链路自检",
                 font=self.f_small, bg=COL_BG, fg=COL_DIM).pack(anchor="w")

        bar = tk.Frame(self.root, bg=COL_BG)
        bar.pack(fill="x", **pad)

        self.btn_start = tk.Button(bar, text="开始测试", font=self.f_body,
                                   command=self.start, width=12,
                                   bg="#2563eb", fg="white",
                                   activebackground="#1d4ed8",
                                   activeforeground="white", relief="flat",
                                   padx=8, pady=6)
        self.btn_start.pack(side="left")

        self.btn_stop = tk.Button(bar, text="停止", font=self.f_body,
                                  command=self.stop, width=8, state="disabled",
                                  relief="flat", padx=8, pady=6)
        self.btn_stop.pack(side="left", padx=6)

        tk.Label(bar, text="扫描超时(秒)", font=self.f_small,
                 bg=COL_BG, fg=COL_DIM).pack(side="left", padx=(16, 4))
        self.spin_to = tk.Spinbox(bar, from_=5, to=90, increment=5, width=4,
                                  font=self.f_small)
        self.spin_to.delete(0, "end")
        self.spin_to.insert(0, str(int(self.timeout)))
        self.spin_to.pack(side="left")

        self.var_text = tk.IntVar(value=1 if self.text_test else 0)
        tk.Checkbutton(bar, text="含文本串口往返测试", variable=self.var_text,
                       font=self.f_small, bg=COL_BG,
                       activebackground=COL_BG).pack(side="left", padx=12)

        # 默认测完断开。为什么要有这个开关而不是写死：
        # 不断开时手表会一直显示"已连接"且不再广播 —— 那正是用户报的
        # "蓝牙断了很久还显示已连接"的根因之一。但要连续观察手表端状态时
        # 又确实想保持连接，所以给一个显式勾选。
        self.var_keep = tk.IntVar(value=0)
        tk.Checkbutton(bar, text="测完保持连接", variable=self.var_keep,
                       font=self.f_small, bg=COL_BG,
                       activebackground=COL_BG).pack(side="left", padx=4)

        self.lbl_state = tk.Label(bar, text="待测试", font=self.f_body,
                                  bg=COL_BG, fg=COL_DIM)
        self.lbl_state.pack(side="right")

        # 结论横幅
        self.lbl_verdict = tk.Label(self.root, text="",
                                    font=self.f_verdict, bg=COL_BG,
                                    fg=COL_DIM, anchor="w", padx=12)
        self.lbl_verdict.pack(fill="x")

        # 结果表
        box = tk.LabelFrame(self.root, text=" 检查项 ", font=self.f_small,
                            bg=COL_BG, fg="#374151")
        box.pack(fill="both", expand=False, padx=12, pady=(2, 6))

        cols = ("res", "item", "detail")
        self.tree = ttk.Treeview(box, columns=cols, show="headings", height=11)
        self.tree.heading("res", text="结果")
        self.tree.heading("item", text="检查项")
        self.tree.heading("detail", text="说明 / 实测值")
        self.tree.column("res", width=64, anchor="center", stretch=False)
        self.tree.column("item", width=250, anchor="w", stretch=False)
        self.tree.column("detail", width=390, anchor="w")
        self.tree.tag_configure("ok", foreground=COL_OK)
        self.tree.tag_configure("fail", foreground=COL_FAIL)
        self.tree.tag_configure("skip", foreground=COL_DIM)
        self.tree.pack(side="left", fill="both", expand=True)

        sb = ttk.Scrollbar(box, orient="vertical", command=self.tree.yview)
        sb.pack(side="right", fill="y")
        self.tree.configure(yscrollcommand=sb.set)

        # 日志
        box2 = tk.LabelFrame(self.root, text=" 过程日志 ", font=self.f_small,
                             bg=COL_BG, fg="#374151")
        box2.pack(fill="both", expand=True, padx=12, pady=(0, 12))
        self.txt = tk.Text(box2, height=8, font=self.f_mono, wrap="none",
                           bg="#0f172a", fg="#cbd5e1", relief="flat")
        self.txt.pack(side="left", fill="both", expand=True)
        sb2 = ttk.Scrollbar(box2, orient="vertical", command=self.txt.yview)
        sb2.pack(side="right", fill="y")
        self.txt.configure(yscrollcommand=sb2.set, state="disabled")

        tk.Label(self.root,
                 text="手表侧要做什么：开机 → 主页标题右侧点一下「蓝牙」进蓝牙页"
                      "（或直接停在主页即可，页面会自动开广播）",
                 font=self.f_small, bg=COL_BG, fg=COL_DIM).pack(pady=(0, 8))

    # ── 日志/结果写入 ──
    def _log(self, msg):
        self.txt.configure(state="normal")
        self.txt.insert("end", msg + "\n")
        self.txt.see("end")
        self.txt.configure(state="disabled")

    def _add_row(self, label, ok, detail):
        mark = "✅ 通过" if ok else "❌ 失败"
        self.tree.insert("", "end", values=(mark, label, detail),
                         tags=("ok" if ok else "fail",))
        self.tree.see(self.tree.get_children()[-1])

    # ── 测试线程 ──
    def start(self):
        if self.busy:
            return

        for item in self.tree.get_children():
            self.tree.delete(item)
        self.txt.configure(state="normal")
        self.txt.delete("1.0", "end")
        self.txt.configure(state="disabled")

        try:
            self.timeout = float(self.spin_to.get())
        except ValueError:
            self.timeout = 20.0

        self.text_test = bool(self.var_text.get())
        self.keep_connected = bool(self.var_keep.get())
        self.stop_flag = False
        self.busy = True
        self.t0 = time.time()
        self.btn_start.configure(state="disabled")
        self.btn_stop.configure(state="normal")
        self.lbl_verdict.configure(text="", fg=COL_DIM)
        self.lbl_state.configure(text="正在测试…", fg="#2563eb")

        self.worker = threading.Thread(target=self._run, daemon=True)
        self.worker.start()

    def stop(self):
        if not self.busy:
            return
        self.stop_flag = True
        self.lbl_state.configure(text="停止中（等当前步骤结束）…", fg=COL_DIM)
        self.btn_stop.configure(state="disabled")

    def _run(self):
        """工作线程：只做测试，不碰任何 Tk 控件（跨线程改控件必崩）。"""
        rows, info = [], {}
        err = None
        try:
            rows, info = C.run_full_test(
                lambda label, ok, detail: self.q.put(("row", label, ok, detail)),
                name="PhyWear", timeout=self.timeout,
                notify_count=2, text_test=self.text_test,
                disconnect_at_end=not self.keep_connected)
        except Exception as e:                      # noqa: BLE001
            err = f"{type(e).__name__}: {e}"
        self.q.put(("done", err, {"rows": rows, "info": info,
                                  "stopped": self.stop_flag}))

    def _pump(self):
        """GUI 线程：把工作线程塞进队列的东西搬进控件。"""
        try:
            while True:
                msg = self.q.get_nowait()
                kind = msg[0]

                if kind == "log":
                    self._log(msg[1])
                elif kind == "row":
                    self._add_row(msg[1], msg[2], msg[3])
                elif kind == "done":
                    self._finish(msg[1], msg[2])
        except queue.Empty:
            pass

        if self.busy:
            self.lbl_state.configure(
                text=f"正在测试… {time.time() - self.t0:.0f}s")
        self.root.after(100, self._pump)

    def _finish(self, err, payload):
        self.busy = False
        self.btn_start.configure(state="normal")
        self.btn_stop.configure(state="disabled")

        if err:
            self.lbl_state.configure(text="测试异常", fg=COL_FAIL)
            self.lbl_verdict.configure(text=f"⚠️ 测试脚本异常：{err}",
                                       fg=COL_FAIL)
            self._log(f"[gui] 异常：{err}")
            return

        if payload.get("stopped"):
            self.lbl_state.configure(text="已停止", fg=COL_DIM)
            self.lbl_verdict.configure(text="⏹ 已停止（判定不完整）",
                                       fg=COL_DIM)
            return

        # 逐项往表里加时最后一行的 see() 会把视图滚到底；结束时回到顶部，
        # 让人一眼看到"从第一条开始全是绿的"。
        try:
            self.tree.yview_moveto(0)
        except tk.TclError:
            pass

        rows = payload.get("rows") or []
        info = payload.get("info") or {}
        npass = sum(1 for _l, ok, _d in rows if ok)
        allok = bool(rows) and npass == len(rows)

        if allok:
            self.lbl_state.configure(text="全部通过", fg=COL_OK)
            self.lbl_verdict.configure(
                text=f"✅ 全部通过（{npass}/{len(rows)}）—— 手表蓝牙链路双向正常",
                fg=COL_OK)
        elif rows:
            self.lbl_state.configure(text="有未过项", fg=COL_FAIL)
            self.lbl_verdict.configure(
                text=f"❌ 有未过项（{npass}/{len(rows)}）—— 看下表红色那几行",
                fg=COL_FAIL)
        else:
            self.lbl_state.configure(text="未开始", fg=COL_DIM)

        bits = []
        if info.get("peer"):
            bits.append(f"对端 {info['peer']}")
        if info.get("mag") is not None:
            bits.append(f"|a|={info['mag']:.0f} mg")
        if info.get("packets"):
            bits.append(f"通知 {info['packets']} 包")
        if info.get("echo"):
            bits.append(f"回声 {info['echo']}")
        if bits:
            self._log("[gui] " + " · ".join(bits))

        # 把关键实测值也贴到结论行下面，方便截图/演示
        if rows and allok:
            self._log("[gui] 结论依据：上表全部为 PASS；"
                      "手表侧同一次连接会在蓝牙页留下对应的 [RX]/[TX] 记录。")


def main():
    ap = argparse.ArgumentParser(description="PhyWear 蓝牙测试（桌面 GUI）")
    ap.add_argument("--no-autostart", action="store_true",
                    help="打开窗口后不自动开始，等按按钮")
    ap.add_argument("--timeout", type=float, default=20.0,
                    help="扫描/等服务的超时（秒），默认 20")
    ap.add_argument("--no-text-test", action="store_true",
                    help="不跑文本串口 …a003 的往返测试")
    ap.add_argument("--gui-selftest", action="store_true",
                    help="只验窗口本身能不能画出来（填合成结果，不碰蓝牙）")
    args = ap.parse_args()

    root = tk.Tk()
    app = App(root,
              autostart=not args.no_autostart,
              timeout=args.timeout,
              text_test=not args.no_text_test)
    if app.selftest:
        root.after(300, app._selftest_fill)
    root.mainloop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
