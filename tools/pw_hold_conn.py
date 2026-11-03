#!/usr/bin/env python3
"""连上手表后**什么都不做**、只保持连接 —— 给真机截图留一个安静的窗口。

为什么需要：`phywear shot btlink` 会在"连上之后静默 4 s"才出图；而完整的
`pw_ble_central.py` 在连接后还要读传感器/订阅/写命令/文本往返，设备侧会连续
打十几行 `[bt] …` 日志 —— 那些日志与"以文本流出的像素"交叉就会撕行，
pwshot 逐行校验直接拒收整帧（实测复现两次）。所以截"已连接"这一张时，
用一个只连接、不活动的脚本把链路撑住即可。
"""
import sys
import time

sys.path.insert(0, "/home/xpqh/openvela/tools/phywear")
import pw_ble_central as C  # noqa: E402

HOLD = float(sys.argv[1]) if len(sys.argv) > 1 else 40.0

c = C.Central("PhyWear", 60.0)
if not c.find_adapter():
    sys.exit("no adapter")
dev = c.scan_for()
if not dev:
    sys.exit("watch not found")
if not c.connect(dev):
    sys.exit("connect failed")
print("connected, holding %.0fs quietly" % HOLD, flush=True)

# 只读一次状态串（一行设备日志），然后静默保持连接
svc, chrs = c.gatt(dev)
if chrs.get(C.TEXT_UUID):
    print("status:", c.read(chrs[C.TEXT_UUID]).decode("utf-8", "replace"),
          flush=True)
time.sleep(HOLD)
