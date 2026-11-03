#!/usr/bin/env python3
"""A/B: pendulum page fps with the on-device agent running vs killed."""
import time, re, serial
PORT="/dev/ttyUSB0"; BAUD=1000000

def boot_and_run(kill_agent):
    s=serial.Serial(PORT,BAUD,timeout=0.3); s.dtr=False; s.rts=False
    time.sleep(0.3); s.reset_input_buffer()
    s.rts=True; time.sleep(0.2); s.rts=False; time.sleep(0.3); s.reset_input_buffer()
    buf=bytearray(); t0=time.time()
    while time.time()-t0<50:
        d=s.read(4096)
        if d:
            buf+=d
            if b"nsh>" in buf: break
        else: s.write(b"\r\n")
    def send(cmd, secs):
        s.reset_input_buffer(); s.write(cmd.encode()+b"\r\n"); s.flush()
        out=bytearray(); t0=time.time()
        while time.time()-t0<secs:
            d=s.read(4096)
            if d: out+=d
        return out.decode("utf-8","replace")
    ps = send("ps", 6)
    pid=None
    for l in ps.splitlines():
        if re.search(r"\bai_agent\b", l):
            pid=l.split()[0]; break
    if kill_agent and pid:
        send("kill "+pid, 3)
    send("phywear cap pendulum &", 8)
    out = send("", 45)
    s.close()
    hb = re.findall(r"alive t=(\d+)s loops/s=(\d+) fps=(\d+)", out)
    return pid, hb

pid, hb_with = boot_and_run(False)
print("A) agent 在跑  : pid=%s heartbeats=%s" % (pid, hb_with))
pid, hb_without = boot_and_run(True)
print("B) agent 已 kill: pid=%s heartbeats=%s" % (pid, hb_without))
