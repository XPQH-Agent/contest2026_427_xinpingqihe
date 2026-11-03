#!/usr/bin/env python3
"""在模拟器上用注入数据验证 ③ 标定 UI 的成功路径（真值已知）。"""
import subprocess, time, os, sys

def sim_in(cmd):
    open("/tmp/sim.in","w").write(cmd + "\n")

def shot(name):
    subprocess.run(["python3","/tmp/fbshot.py",name,"shot"], capture_output=True)

sim_in("phywear imubench 0 &")          # 注入 + 从实时页开始
time.sleep(8)
for i, (page, wait) in enumerate([(1,8), (2,14), (3,16)], start=1):
    sim_in("phywear tap 354 418")       # 右箭头 → 下一页（bench 会在该页自动跑完）
    time.sleep(wait)
    shot("/tmp/bench_p%d.png" % page)
print("done")
