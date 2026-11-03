#!/usr/bin/env python3
"""pw_serial.py - 无头串口取证器（PhyWear / 黄山派 SF32LB52）

为什么有这个小工具：
  `pwshot.py` 是**抓帧**用的（要解析 SHOT-* 协议），不适合"我只想把开发板
  从开机到某条命令的全部串口原文一字不漏地存成证据文件"这种场景。
  本工具只做三件事：开串口、按序发命令、把**原始字节**+**带时间戳的行**落盘，
  并打印两个文件的 sha256 —— 让"串口原文"这条证据可被第三方独立复核。

铁律（与项目 CLAUDE.md 一致）：
  * 打开后立刻 dtr=False / rts=False（CH340N 把 RTS 接到了 SoC 复位脚）；
  * 端口独占（exclusive=True）；
  * 一个 boot 只跑一个 GUI —— 本工具不负责这条，由调用方保证。

用法示例：
  python3 tools/phywear/pw_serial.py --out /tmp/bt --label ab \\
      --reset --run "phywear bthci" --run "phywear cap bt"
"""

import argparse
import hashlib
import os
import sys
import time

import serial

PORT = "/dev/ttyUSB0"
BAUD = 1000000


def log(msg):
    print(f"[pw_serial] {msg}", flush=True)


class Session:
    def __init__(self, port, baud, boot_timeout):
        self.ser = serial.Serial(port, baud, timeout=0.2, exclusive=True)
        # 铁律：CH340N 的 RTS 接 SoC 复位脚，DTR 也要保持释放
        self.ser.dtr = False
        self.ser.rts = False
        time.sleep(0.2)
        self.ser.reset_input_buffer()
        self.boot_timeout = boot_timeout
        self.raw = bytearray()
        self.lines = []          # (t_rel, text)
        self.t0 = time.monotonic()
        self._pending = b""

    # ---------- 底层：读 + 记账 ----------
    def _absorb(self, chunk):
        self.raw += chunk
        self._pending += chunk
        while b"\n" in self._pending:
            line, self._pending = self._pending.split(b"\n", 1)
            self.lines.append((time.monotonic() - self.t0, line))

    def pump(self, seconds):
        """读满 seconds 秒（或直到串口静默），返回期间收到的字节数。"""
        got = 0
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            chunk = self.ser.read(4096)
            if chunk:
                got += len(chunk)
                self._absorb(chunk)
        return got

    def wait_prompt(self, timeout, since=None):
        """等提示符（nsh> 或 vela>）。

        `since` = 从 self.raw 的哪个字节开始找 —— 必须传！
        否则上一次的提示符还留在缓冲区里，会立刻"假命中"。
        """
        if since is None:
            since = 0
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            chunk = self.ser.read(4096)
            if chunk:
                self._absorb(chunk)
                tail = bytes(self.raw[since:])
                if b"nsh>" in tail or b"vela>" in tail:
                    return True
            else:
                self.ser.write(b"\n")
                time.sleep(0.15)
        return False

    def reset(self):
        log("RTS 脉冲复位开发板")
        self.ser.rts = True
        time.sleep(0.2)
        self.ser.rts = False
        time.sleep(0.3)
        self.ser.reset_input_buffer()
        self.t0 = time.monotonic()
        self.raw = bytearray()
        self.lines = []
        self._pending = b""
        return self.wait_prompt(self.boot_timeout, since=0)

    def send(self, command, wait=True):
        log(f"$ {command}")
        marker = f"### SEND: {command}".encode()
        self.lines.append((time.monotonic() - self.t0, marker))
        self.raw += b"\n" + marker + b"\n"
        mark = len(self.raw)
        self.ser.write(command.encode("utf-8") + b"\n")
        if not wait:
            return True
        return self.wait_prompt(self.boot_timeout, since=mark)

    def close(self):
        try:
            self.ser.close()
        except Exception:
            pass


def dump(outdir, label, sess):
    os.makedirs(outdir, exist_ok=True)
    raw_path = os.path.join(outdir, f"{label}.raw")
    log_path = os.path.join(outdir, f"{label}.log")

    with open(raw_path, "wb") as fh:
        fh.write(bytes(sess.raw))

    with open(log_path, "w", encoding="utf-8", errors="replace") as fh:
        for t_rel, line in sess.lines:
            fh.write(f"{t_rel:9.3f}  {line.decode('utf-8', 'replace')}\n")

    for path in (raw_path, log_path):
        data = open(path, "rb").read()
        log(f"{path}  {len(data)} B  sha256={hashlib.sha256(data).hexdigest()}")
    return raw_path, log_path


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default=PORT)
    ap.add_argument("--baud", type=int, default=BAUD)
    ap.add_argument("--out", required=True)
    ap.add_argument("--label", required=True)
    ap.add_argument("--reset", action="store_true",
                    help="先做 RTS 复位脉冲（清掉上一次 GUI 的残留状态）")
    ap.add_argument("--boot-timeout", type=float, default=60.0)
    ap.add_argument("--run", action="append", default=[],
                    help="要执行的命令，可重复；每条执行完等提示符")
    ap.add_argument("--step", action="append", default=[],
                    help="定时步骤 SECONDS:COMMAND —— 发完命令后**定长**采集 N 秒。"
                         "为什么需要它：本板控制台被 NSH 与 ai_agent 的 cli_thread "
                         "同时读（提示符 nsh> / vela> 交替），靠提示符同步会假命中；"
                         "开机后先发 `quit` 让 ai_agent CLI 退出，再定时发命令。")
    ap.add_argument("--tail", type=float, default=3.0,
                    help="最后额外静默采集的秒数")
    args = ap.parse_args()

    if not os.path.exists(args.port):
        log(f"致命：{args.port} 不存在（板子掉线？烧录与串口都会静默无输出）")
        return 2

    sess = Session(args.port, args.baud, args.boot_timeout)
    try:
        if args.reset:
            sess.reset()
        else:
            sess.wait_prompt(5.0, since=0)

        for step in args.step:
            seconds, _, command = step.partition(":")
            if not command:
                # 纯等待步骤（例如等 ai_agent 的 30s 网络超时把控制台放开）
                sess.pump(float(seconds))
                log(f"  (等待 {seconds}s)")
                continue
            sess.send(command, wait=False)
            sess.pump(float(seconds))
            log(f"  (采集 {seconds}s 完成)")

        for command in args.run:
            sess.send(command)

        sess.pump(args.tail)
    finally:
        dump(args.out, args.label, sess)
        sess.close()

    return 0


if __name__ == "__main__":
    sys.exit(main())
