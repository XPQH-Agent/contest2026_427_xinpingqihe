#!/usr/bin/env python3
import sys, time, re, serial
cap = sys.argv[1]
s=serial.Serial("/dev/ttyUSB0",1000000,timeout=0.3); s.dtr=False; s.rts=False
time.sleep(0.3); s.reset_input_buffer()
s.rts=True; time.sleep(0.2); s.rts=False; time.sleep(0.3); s.reset_input_buffer()
buf=bytearray(); t0=time.time()
while time.time()-t0<50:
    d=s.read(4096)
    if d:
        buf+=d
        if b"nsh>" in buf: break
    else: s.write(b"\r\n")
def send(cmd,secs):
    s.reset_input_buffer(); s.write(cmd.encode()+b"\r\n"); s.flush()
    o=bytearray(); t0=time.time()
    while time.time()-t0<secs:
        d=s.read(4096)
        if d: o+=d
    return o.decode("utf-8","replace")
send("free",6)                      # 第一次可能被 Agent CLI 吃掉，丢掉
send("phywear cap %s &" % cap, 10)
out = send("free", 6)
line = [l for l in out.splitlines() if "Umem" in l]
print("cap=%-8s %s" % (cap, line[-1].strip() if line else "(未拿到)"))
s.close()
