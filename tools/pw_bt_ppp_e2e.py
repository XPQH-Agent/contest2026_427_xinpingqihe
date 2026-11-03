#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""pw_bt_ppp_e2e.py —— PPP over BLE 端到端验收（一条命令跑完，落盘证据）

它把两边按正确顺序串起来，顺手把**串口原文**和**宿主日志**都存成可复核的证据：

    1) 串口：复位 → `quit`（让 ai_agent 的 cli 放开 stdin）→ `phywear btppp &`
       （设备起来后会先等主机订阅，再拉 pppd —— 见 pw_btppp.c 里的等待）
    2) 起宿主对端：`pw_bt_ppp.py --peer`（用户态 LCP+IPCP+ICMP，**不需要 root**）
    3) 边等边 pump 串口；到点后在**设备上**跑
           ifconfig             → 期望 ppp0 UP 且 inet addr:192.168.7.2
           ping -c 3 192.168.7.1 → 期望 3 个回包（对端就是本进程）
    4) 落盘：dev.raw / dev.log（设备）、host.log（对端）、run.txt（本脚本记录）、
       SUMMARY.txt + SHA256SUMS.txt

用法：
    python3 tools/phywear/pw_bt_ppp_e2e.py --out docs/evidence/bt-ppp-e2e-<date>
退出码 0 = 三项都过（ppp0 起来了 / 拿到 IP / ping 通）。
"""

import argparse
import hashlib
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pw_serial as S            # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", required=True)
    ap.add_argument("--port", default="/dev/ttyUSB0")
    ap.add_argument("--peer-seconds", type=float, default=150.0,
                    help="宿主对端跑多久（要把 ifconfig/ping 罩在里面）")
    ap.add_argument("--ping-at", type=float, default=35.0,
                    help="起对端之后多少秒在设备上跑 ifconfig+ping")
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    notes = []

    def note(m):
        print(f"[e2e] {m}", flush=True)
        notes.append(m)

    sess = S.Session(args.port, 1000000, 60.0)
    host_path = os.path.join(args.out, "host.log")
    host_fh = open(host_path, "w", encoding="utf-8")
    proc = None
    try:
        if not sess.reset():
            note("❌ 板子没起来（等不到提示符）")
            return 2
        note("设备已启动到 NShell")

        sess.send("quit", wait=False)
        sess.pump(1.5)
        sess.send("phywear btppp &", wait=False)
        sess.pump(6.0)
        note("已下发 `phywear btppp &`")

        proc = subprocess.Popen(
            [sys.executable, os.path.join(HERE, "pw_bt_ppp.py"),
             "--peer", "--duration", str(args.peer_seconds)],
            stdout=host_fh, stderr=subprocess.STDOUT, text=True)
        note(f"宿主对端已起（pid={proc.pid}，跑 {args.peer_seconds:g}s）")

        t0 = time.monotonic()
        did_lcp = did_ip = False
        while True:
            sess.pump(0.5)
            el = time.monotonic() - t0
            tail = open(host_path, "r", encoding="utf-8",
                        errors="replace").read()
            if not did_lcp and "★ LCP UP" in tail:
                did_lcp = True
                note(f"t+{el:.1f}s 宿主看到 LCP UP")
            if not did_ip and "★ IPCP UP" in tail:
                did_ip = True
                note(f"t+{el:.1f}s 宿主看到 IPCP UP")
            if el >= args.ping_at:
                break
            if proc.poll() is not None:
                note(f"⚠️ 宿主对端提前退出（rc={proc.returncode}）")
                break

        note("在设备上跑 ifconfig")
        sess.send("ifconfig", wait=False)
        sess.pump(4.0)
        note("在设备上跑 ping -c 3 192.168.7.1")
        sess.send("ping -c 3 192.168.7.1", wait=False)
        sess.pump(8.0)
    finally:
        if proc and proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
        host_fh.close()
        raw_path, log_path = S.dump(args.out, "dev", sess)
        sess.close()

    dev_log = open(log_path, "r", encoding="utf-8", errors="replace").read()
    host_log = open(host_path, "r", encoding="utf-8", errors="replace").read()

    # ping 的判定要**解析数字**，不能用 "0% packet loss" 这种子串：
    # "100% packet loss" 里就含 "0% packet loss" —— 我第一版正是这么写的，
    # 于是 e2e_4 把 "3 packets transmitted, 0 received, 100% packet loss"
    # 判成了 PASS（负对照失效）。这里改成取 "N received"，N>0 才算通。
    m = re.search(r"(\d+) packets transmitted,\s*(\d+) received", dev_log)
    ping_ok = bool(m) and int(m.group(2)) > 0
    ping_txt = (f"{m.group(1)} sent / {m.group(2)} recv" if m else "没抓到 ping 统计")

    checks = [
        ("设备侧 ppp0 已创建", "ppp0" in dev_log),
        # NuttX 的 ifconfig 用的是 "at RUNNING"，不是 "UP"（第一版判据写错）
        ("设备侧 ppp0 是 RUNNING", "ppp0" in dev_log and "RUNNING" in dev_log),
        ("设备侧拿到 IP 192.168.7.2", "192.168.7.2" in dev_log),
        ("宿主对端 LCP UP", "★ LCP UP" in host_log),
        ("宿主对端 IPCP UP", "★ IPCP UP" in host_log),
        (f"设备 ping 有回包（{ping_txt}）", ping_ok),
    ]

    with open(os.path.join(args.out, "SUMMARY.txt"), "w", encoding="utf-8") as fh:
        fh.write("PPP over BLE 端到端验收（宿主用用户态 PPP 对端，不需要 root）\n")
        fh.write("=" * 72 + "\n")
        for name, ok in checks:
            fh.write(f"{'PASS' if ok else 'FAIL'}  {name}\n")
        fh.write("-" * 72 + "\n")
        for n in notes:
            fh.write(n + "\n")
        fh.write(f"\n结论：{'✅ 全过' if all(o for _, o in checks) else '❌ 有未通过项'}\n")

    with open(os.path.join(args.out, "SHA256SUMS.txt"), "w", encoding="utf-8") as fh:
        for fn in sorted(os.listdir(args.out)):
            if fn == "SHA256SUMS.txt" or not os.path.isfile(
                    os.path.join(args.out, fn)):
                continue
            fh.write(f"{sha256_file(os.path.join(args.out, fn))}  {fn}\n")

    print(open(os.path.join(args.out, "SUMMARY.txt"), encoding="utf-8").read())
    return 0 if all(o for _, o in checks) else 1


if __name__ == "__main__":
    sys.exit(main())
