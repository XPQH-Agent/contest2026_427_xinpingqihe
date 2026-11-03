#!/usr/bin/env python3
"""Boot the device and check whether ai_agent starts by itself, then run the
proactive chain WITHOUT starting the agent by hand."""
import time, serial
PORT="/dev/ttyUSB0"; BAUD=1000000
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
boot=buf.decode("utf-8","replace")
print("[boot %.1fs] agent 自启迹象:" % (time.time()-t0))
print("\n".join(l for l in boot.splitlines() if "agent" in l.lower())[-600:] or "  (无)")
def send(cmd, secs):
    s.reset_input_buffer(); s.write(cmd.encode()+b"\r\n"); s.flush()
    out=bytearray(); t0=time.time()
    while time.time()-t0<secs:
        d=s.read(4096)
        if d: out+=d
    return out.decode("utf-8","replace")
print("--- ps ---"); o=send("ps", 6)
print("\n".join(l for l in o.splitlines() if "ai_agent" in l)[:400] or "  (ps 里没有 ai_agent → 没自启)")
print("--- 起 GUI 后 coach 触发（不手动起 Agent）---")
send("phywear cap root &", 10)
o=send("phywear coach 检测到持续摆动 请跑单摆实验测重力加速度", 30)
keep=("coach","Executing tool","ai-coach","ALERT","Assertion","not delivered","not ready","not connected")
print("\n".join(l for l in o.splitlines() if any(k in l for k in keep))[-1800:])
s.close()
