#!/usr/bin/env python3
import time, serial
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
print(send("phywear calib", 6).strip()[-200:])
print(send("phywear ahrs", 6).strip()[-200:])
print(send("phywear ahrs 20", 30).strip()[-1600:])
s.close()
