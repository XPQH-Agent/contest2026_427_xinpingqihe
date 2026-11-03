#!/usr/bin/env python3
"""pw_h4_notify_decode.py —— 从串口日志里把 H4 `[H4] TX` 的 ATT 通知帧解出来

为什么有它：B3 卡了整整一轮，症状是**设备侧一切正常**（`[bt] notify #N rc=0
len=16` 连打 60 条）而**宿主侧一个包都收不到**。这种"发出去没人收"的分叉，
只能靠空口原文定位 —— 于是先打开 h4.c 的 PW_H4_TRACE，再用本脚本把
`[H4] TX (… B): …` 的十六进制按 HCI ACL → L2CAP → ATT 逐层拆开，
把每条通知的 **value handle** 列出来。

结论（2026-09-18）：修复前 56/56 条通知的 value handle 都是 **0x0000**
（BlueZ 找不到句柄 0x0000 的特征，静默丢弃）；zblue 补上
`gatt_notify_mc()/gatt_indicate_mc()` 里漏掉的 `data.handle = handle;` 之后，
同一脚本解出 **0x0010**（= 特征 e0f1a001 的值句柄），宿主立刻收到。

帧格式（H4，无 HCI 传输层类型字节被日志剥掉，日志从 ACL 的 0x02 开始）：
    02 | handle(2) | dlen(2) | L2CAP: len(2) cid(2) | ATT: opcode(1) ...
cid 0x0004 = ATT；opcode 0x1b = Handle Value Notification
                  0x1d = Handle Value Indication
                  0x13 = Write Response
                  0x01 = Error Response

用法：
    python3 tools/phywear/pw_h4_notify_decode.py <日志文件> [--expect-handle 0x0010]
退出码：0 = 找到通知帧且（若给了 --expect-handle）句柄全部相符；否则 1。
"""

import argparse
import re
import sys
from collections import Counter

ATT_NOTIFY = 0x1B
ATT_INDICATE = 0x1D
CID_ATT = 0x0004


def deansi(text):
    return re.sub(r"\x1b\[[0-9;]*m", "", text)


def parse_hex(s):
    """日志里长帧会被截断成 `… (+188 B)`，只取截断点之前的字节。"""
    return [int(x, 16) for x in s.split("...(")[0].split()]


def read_log(path):
    with open(path, "rb") as fh:
        return deansi(fh.read().decode("utf-8", "replace"))


def sub_window(text):
    """返回 (订阅ON时刻, 订阅OFF时刻)，取不到就是 None。

    为什么要窗口：自检阶段（还没连手机）也会发一次通知探针，那不是"发给了
    订阅者"的证据，混在一起会把数字说大。
    """
    t_on = t_off = None
    for ln in text.splitlines():
        if "ccc changed -> notify ON" in ln:
            t_on = float(ln.split()[0])
        if "ccc changed -> notify OFF" in ln:
            t_off = float(ln.split()[0])
    return t_on, t_off


def collect(text):
    rows = []
    for ln in text.splitlines():
        m = re.match(r"\s*([\d.]+)\s+\[H4\] TX \((\d+) B\): (.*)$", ln)
        if m:
            rows.append((float(m.group(1)), int(m.group(2)), parse_hex(m.group(3))))
    return rows


def decode(rows):
    """拆出所有 ATT 层帧；返回 (时刻, opcode, value_handle|None, value bytes)。"""
    out = []
    for t, _n, b in rows:
        # 只认整帧都在日志里的（截断帧不解析，免得读错字节还以为发现了问题）
        if len(b) < 12 or b[0] != 0x02:
            continue
        if (b[7] | (b[8] << 8)) != CID_ATT:
            continue
        op = b[9]
        if op in (ATT_NOTIFY, ATT_INDICATE):
            out.append((t, op, b[10] | (b[11] << 8), bytes(b[12:])))
        else:
            out.append((t, op, None, bytes(b[10:])))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log")
    ap.add_argument("--expect-handle", default=None,
                    help="例如 0x0010；给出则要求订阅窗口内所有通知都用这个句柄")
    args = ap.parse_args()

    text = read_log(args.log)
    t_on, t_off = sub_window(text)
    print(f"订阅窗口: {t_on} -> {t_off}")

    frames = decode(collect(text))
    notif = [(t, op, h, v) for t, op, h, v in frames
             if op in (ATT_NOTIFY, ATT_INDICATE)
             and (t_on is None or t >= t_on)
             and (t_off is None or t <= t_off)]

    print(f"ATT 帧总数: {len(frames)}   订阅窗口内通知帧: {len(notif)}")
    if not notif:
        print("❌ 订阅窗口内没有任何 ATT 通知帧")
        return 1

    dist = Counter(f"0x{h:04x}" for _t, _op, h, _v in notif)
    print("通知 value handle 分布: " +
          ", ".join(f"{k}×{v}" for k, v in dist.items()))
    for t, op, h, v in notif[:3]:
        kind = "Notify" if op == ATT_NOTIFY else "Indicate"
        print(f"  t={t:<8} {kind} value_handle=0x{h:04x} value={v.hex(' ')}")
    if len(notif) > 3:
        print(f"  …… 其余 {len(notif) - 3} 条略")

    if args.expect_handle:
        want = int(args.expect_handle, 16)
        bad = [h for _t, _op, h, _v in notif if h != want]
        if bad:
            print(f"❌ 有 {len(bad)} 条通知的句柄不是 0x{want:04x}"
                  f"（例如 0x{bad[0]:04x}）—— 对端会静默丢弃")
            return 1
        print(f"✅ {len(notif)} 条通知全部使用 value handle 0x{want:04x}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
