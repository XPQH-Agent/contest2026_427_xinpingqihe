#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""pw_bt_ppp.py —— 宿主侧：把 BLE 与一个 PTY 接起来（给 pppd 用），或者**自己当
PPP 对端**（不需要 root）

板子侧（`phywear btppp &`）会注册两个 GATT 特征做**字节管道**：
    …a101  WRITE  主机 → 设备（PPP 字节）
    …a102  NOTIFY 设备 → 主机（PPP 字节）

三种用法：

① `--peer`（**推荐，本机可跑通全部验收**）：本脚本就是 PPP 对端。
   它自己做 LCP + IPCP 协商，把设备侧 `ppp0` 拉起来并给它 192.168.7.2；
   还会应答 ICMP Echo-Request —— 于是手表上 `ping 192.168.7.1` 能通。
   **不需要 root**（不需要建宿主的网络接口，因为"对端"就在本进程里）。

② 默认（桥模式）：把两个 GATT 特征接到一对 PTY 上，让**真正的 pppd** 用。
   ```
   python3 tools/phywear/pw_bt_ppp.py              # 打印 /dev/pts/N
   sudo pppd /dev/pts/N 115200 noauth 192.168.7.1:192.168.7.2 local nodetach
   ```
   这条能建真正的宿主网络接口（`ifconfig` 里出现 ppp0、可路由），
   但**第二步必须 root**。

③ `--sniff`：只解帧、不回应，用来确认"设备到底往管道里写了什么"。

板子侧顺序（三者都一样）：
    phywear btppp &        # 后台跑；设备会**先等主机订阅**再拉 pppd（防超时竞态）
    然后在设备上： ifconfig   → ppp0 应为 UP 且 inet addr:192.168.7.2
                  ping 192.168.7.1

为什么用 GATT 而不是 L2CAP CoC：CoC 吞吐更好，但 Linux 用户态走 LE CoC 的
socket API 很别扭；而 GATT 这条路已有现成且验证过的实现。ATT MTU 协商到 517
⇒ 单次可带 ~514 B，跑 LCP/IPCP/ping 足够（CoC 留作后续优化）。
"""

import argparse
import errno
import os
import pty
import re
import select
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pw_ble_central as C            # noqa: E402
import pw_ppp_frame as F              # noqa: E402

PPP_RX_UUID = "e0f1a101-1b2c-4d5e-8f90-a1b2c3d4e5f6"
PPP_TX_UUID = "e0f1a102-1b2c-4d5e-8f90-a1b2c3d4e5f6"

LOCAL_IP = "192.168.7.1"              # 本脚本（PPP 对端）的地址
PEER_IP = "192.168.7.2"               # 给手表分配的地址


def log(m):
    print(f"[host-ppp] {m}", flush=True)


def ip_checksum(data):
    """标准 16 bit 反码和（IPv4 头 / ICMP 都用它）。"""
    if len(data) % 2:
        data += b"\x00"
    s = 0
    for i in range(0, len(data), 2):
        s += (data[i] << 8) | data[i + 1]
    while s >> 16:
        s = (s & 0xFFFF) + (s >> 16)
    return (~s) & 0xFFFF


class Peer:
    """最小 PPP 对端：LCP + IPCP + ICMP Echo 应答（RFC 1661 / 1332）。"""

    def __init__(self, send, quiet=False):
        self.send = send                      # send(bytes) -> None（已按 MTU 分片）
        self.splitter = F.FrameSplitter()
        self.quiet = quiet
        self.lcp_up = False
        self.ipcp_up = False
        self.lcp_id = 1
        self.ipcp_id = 1
        self.local = F.ip_bytes(LOCAL_IP)
        self.peer = F.ip_bytes(PEER_IP)
        self.lcp_req = (bytes([F.LCP_OPT_MRU, 4]) + struct.pack(">H", 1500) +
                        bytes([F.LCP_OPT_MAGIC, 6]) + b"\xa1\xb2\xc3\xd4")
        # 只请求 IP 地址（设备侧 ppp_conf.h 没开 DNS 选项）
        self.ipcp_req = F.ipcp_opt(F.IPCP_OPT_IP, self.local)
        self.t_lcp = 0.0
        self.t_ipcp = 0.0
        self.rx_frames = 0
        self.tx_frames = 0
        self.icmp_replied = 0

    # ── 发送 ──────────────────────────────────────────────────────────
    def _tx(self, protocol, payload):
        self.tx_frames += 1
        self.send(F.build_frame(protocol, payload))
        if not self.quiet:
            log(f"TX {F.describe(protocol, payload)}")

    # ── 接收 ──────────────────────────────────────────────────────────
    def feed(self, data):
        for protocol, payload in self.splitter.feed(data):
            self.rx_frames += 1
            if not self.quiet:
                log(f"RX {F.describe(protocol, payload)}")
            if protocol == F.PROTO_LCP:
                self._lcp(payload)
            elif protocol == F.PROTO_IPCP:
                if self.lcp_up:
                    self._ipcp(payload)
                elif not self.quiet:
                    log("  (LCP 未 up，IPCP 先忽略)")
            elif protocol == F.PROTO_IP:
                self._ip(payload)

    def _lcp(self, p):
        code, pid = p[0], p[1]
        opts = p[4:struct.unpack(">H", p[2:4])[0]]
        if code == F.LCP_CONF_REQ:
            # 设备只请求 ACCM=0xffffffff；我们的发送端本来就转义所有 <0x20，
            # 所以照抄回去 Ack 即可。
            self._tx(F.PROTO_LCP, F.lcp_packet(F.LCP_CONF_ACK, pid, opts))
        elif code == F.LCP_CONF_ACK:
            if not self.lcp_up:
                self.lcp_up = True
                log("★ LCP UP")
                self._ipcp_open()
        elif code in (F.LCP_CONF_NAK, F.LCP_CONF_REJ):
            log("  我方 LCP 请求被 Nak/Rej ⇒ 退成「只请求 MRU」再试")
            self.lcp_req = bytes([F.LCP_OPT_MRU, 4]) + struct.pack(">H", 1500)
            self.t_lcp = 0.0
        elif code == F.LCP_ECHO_REQ:
            self._tx(F.PROTO_LCP, F.lcp_packet(F.LCP_ECHO_REP, pid, p[4:]))
        elif code == F.LCP_TERM_REQ:
            self._tx(F.PROTO_LCP, F.lcp_packet(F.LCP_TERM_ACK, pid))
            self.lcp_up = self.ipcp_up = False

    def _ipcp_open(self):
        self.ipcp_id = (self.ipcp_id % 250) + 1
        self._tx(F.PROTO_IPCP, F.ipcp_packet(F.LCP_CONF_REQ, self.ipcp_id,
                                             self.ipcp_req))
        self.t_ipcp = time.monotonic()

    def _ipcp(self, p):
        code, pid = p[0], p[1]
        plen = struct.unpack(">H", p[2:4])[0]
        opts = p[4:plen]
        if code == F.LCP_CONF_REQ:
            # 设备第一轮会请求 0.0.0.0（它还不知道自己该用什么地址）⇒
            # 按 RFC 1332 用 **Configure-Nak** 把 192.168.7.2 告诉它；
            # 设备侧 ipcp.c 的 CONF_NAK 分支会 netlib_set_ipv4addr() + ifup()。
            req = dict(F.parse_options(F.PROTO_IPCP, opts)).get(F.IPCP_OPT_IP)
            if req == self.peer:
                self._tx(F.PROTO_IPCP, F.ipcp_packet(F.LCP_CONF_ACK, pid, opts))
            else:
                want = F.ipcp_opt(F.IPCP_OPT_IP, self.peer)
                self._tx(F.PROTO_IPCP, F.ipcp_packet(F.LCP_CONF_NAK, pid, want))
                if req is not None:
                    log(f"  设备请求 IP={'.'.join(str(x) for x in req)} "
                        f"⇒ Nak 成 {PEER_IP}")
        elif code == F.LCP_CONF_ACK:
            if not self.ipcp_up:
                self.ipcp_up = True
                log(f"★ IPCP UP（对端地址 {LOCAL_IP}，设备应拿到 {PEER_IP}）")
        elif code == F.LCP_CONF_NAK:
            for t, v in F.parse_options(F.PROTO_IPCP, opts):
                if t == F.IPCP_OPT_IP and len(v) == 4:
                    self.local = v
                    self.ipcp_req = F.ipcp_opt(F.IPCP_OPT_IP, v)
                    log(f"  我方 IP 被 Nak 成 {'.'.join(str(x) for x in v)}")
            self.t_ipcp = 0.0
        elif code == F.LCP_CONF_REJ:
            log("  我方 IPCP 请求被 Rej（不常见）")
            self.t_ipcp = 0.0

    # ── ICMP：让设备能 ping 通"宿主"（其实是本进程）──────────────────
    def _ip(self, pkt):
        if len(pkt) < 20 or (pkt[0] >> 4) != 4:
            return
        ihl = (pkt[0] & 0x0F) * 4
        proto = pkt[9]
        src, dst = pkt[12:16], pkt[16:20]
        if proto != 1:
            if not self.quiet:
                log(f"  IP proto={proto} {len(pkt)} B（不处理）")
            return
        icmp = bytearray(pkt[ihl:])
        if len(icmp) < 8 or icmp[0] != 8:
            return
        icmp[0] = 0                                   # Echo Request → Reply
        icmp[2:4] = b"\x00\x00"
        icmp[2:4] = struct.pack(">H", ip_checksum(bytes(icmp)))
        hdr = bytearray(pkt[:ihl])
        hdr[8] = 64                                   # TTL
        hdr[12:16], hdr[16:20] = dst, src             # 交换源/目的
        hdr[10:12] = b"\x00\x00"
        hdr[10:12] = struct.pack(">H", ip_checksum(bytes(hdr)))
        self.icmp_replied += 1
        self._tx(F.PROTO_IP, bytes(hdr) + bytes(icmp))
        if not self.quiet:
            log(f"  ICMP Echo-Request ← {'.'.join(str(x) for x in src)} "
                f"⇒ 已回 Echo-Reply")

    # ── 定时重传（RFC：协商期每 3 s 重发一次 Configure-Request）──────
    def tick(self):
        now = time.monotonic()
        if not self.lcp_up and now - self.t_lcp >= 2.0:
            self._tx(F.PROTO_LCP, F.lcp_packet(F.LCP_CONF_REQ, self.lcp_id,
                                               self.lcp_req))
            self.t_lcp = now
        elif self.lcp_up and not self.ipcp_up and now - self.t_ipcp >= 2.0:
            self._ipcp_open()


def _open(central, args):
    if not central.find_adapter():
        return None
    dev = central.scan_for()
    if not dev:
        log("没扫到手表 —— 板子上先跑过 `phywear btppp &` 了吗？")
        return None
    if not central.connect(dev):
        log("连接失败")
        return None
    chrs = central.gatt_all(dev)          # PPP 特征在**第二个服务**里
    rx = chrs.get(PPP_RX_UUID)
    tx = chrs.get(PPP_TX_UUID)
    if not rx or not tx:
        log("没找到 PPP 特征（…a101/…a102）—— 固件里 PW_BT_PPP=0 或服务没注册成功。")
        return None
    return dev, rx, tx


def _chunk_for(central, chrs, args):
    chunk = args.chunk
    if chunk > 0:
        return chunk
    chunk = 180
    s3 = chrs.get(C.TEXT_UUID)
    if s3:
        try:
            st = central.read(s3).decode("utf-8", "replace")
            m = re.search(r"mtu=(\d+)", st)
            if m:
                mtu = int(m.group(1))
                chunk = max(20, mtu - 3)
                log(f"设备上报 MTU={mtu} ⇒ 每次写 {chunk} B")
        except Exception as e:                        # noqa: BLE001
            log(f"读 MTU 失败（用默认 {chunk} B）：{e}")
    return chunk


def run_peer(central, args, chrs, rx, tx, chunk):
    """用户态 PPP 对端：不需要 root，也不需要 PTY。"""
    from gi.repository import GLib

    pending = bytearray()
    peer = Peer(lambda b: _ble_write(central, rx, b, chunk),
                quiet=args.quiet)

    dbus_ok = _subscribe(central, tx, pending)
    if not dbus_ok:
        return 1

    log(f"已订阅设备→主机 通知；本进程作为 PPP 对端（{LOCAL_IP} ↔ {PEER_IP}）")
    log("设备侧现在可以跑： phywear btppp &  然后 ifconfig / ping 192.168.7.1")

    ctx = GLib.MainContext.default()
    deadline = time.monotonic() + args.duration
    try:
        while time.monotonic() < deadline:
            while ctx.pending():
                ctx.iteration(False)
            if pending:
                data = bytes(pending)
                del pending[:]
                peer.feed(data)
            peer.tick()
            time.sleep(0.02)
    except KeyboardInterrupt:
        log("退出")
    finally:
        log(f"统计：收 {peer.rx_frames} 帧 / 发 {peer.tx_frames} 帧；"
            f"坏 FCS {peer.splitter.bad_fcs}；ICMP 回包 {peer.icmp_replied}；"
            f"LCP={'UP' if peer.lcp_up else 'down'} "
            f"IPCP={'UP' if peer.ipcp_up else 'down'}")
        _unsubscribe(central, tx)
    return 0 if (peer.lcp_up and peer.ipcp_up) else 1


def _ble_write(central, rx, data, chunk):
    for i in range(0, len(data), chunk):
        central.write(rx, data[i:i + chunk])


def _subscribe(central, tx, sink):
    import dbus
    import dbus.mainloop.glib

    dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)

    def on_props(interface, changed, invalidated):
        if interface == C.GATT_CHR_IFACE and "Value" in changed:
            sink.extend(bytes(bytearray(changed["Value"])))

    central.bus.add_signal_receiver(on_props, dbus_interface=C.PROPS_IFACE,
                                    signal_name="PropertiesChanged", path=tx)
    iface = dbus.Interface(central.bus.get_object(C.BLUEZ, tx),
                           C.GATT_CHR_IFACE)
    try:
        iface.StartNotify()
    except Exception as e:                            # noqa: BLE001
        log(f"StartNotify 失败：{e}")
        return False
    return True


def _unsubscribe(central, tx):
    import dbus
    try:
        iface = dbus.Interface(central.bus.get_object(C.BLUEZ, tx),
                               C.GATT_CHR_IFACE)
        iface.StopNotify()
    except Exception:                                 # noqa: BLE001
        pass


def run_bridge(central, chrs, rx, tx, chunk):
    """默认模式：把 BLE 接到一个 PTY，给真正的 pppd 用（宿主侧需要 root）。"""
    pending = bytearray()
    if not _subscribe(central, tx, pending):
        return 1

    master, slave = pty.openpty()
    slave_name = os.ttyname(slave)
    log("=" * 66)
    log(f"PTY 就绪：{slave_name}")
    log("另开一个终端，用 root 起 pppd（对面就是它）：")
    log(f"    sudo pppd {slave_name} 115200 noauth "
        f"{LOCAL_IP}:{PEER_IP} local nodetach")
    log("然后在**板子**上： ifconfig  /  ping 192.168.7.1")
    log("=" * 66)

    from gi.repository import GLib
    ctx = GLib.MainContext.default()
    n_rx = n_tx = 0
    try:
        while True:
            while ctx.pending():
                ctx.iteration(False)
            if pending:
                try:
                    w = os.write(master, bytes(pending))
                    n_rx += w
                    del pending[:w]
                except OSError as e:
                    if e.errno not in (errno.EAGAIN, errno.EWOULDBLOCK):
                        raise
            r, _, _ = select.select([master], [], [], 0.05)
            if r:
                data = os.read(master, chunk)
                if data:
                    _ble_write(central, rx, data, chunk)
                    n_tx += len(data)
    except KeyboardInterrupt:
        log("退出")
    finally:
        log(f"累计：主机→设备 {n_tx} B，设备→主机 {n_rx} B")
        _unsubscribe(central, tx)
        os.close(master)
        os.close(slave)
    return 0


def selftest():
    """不碰板子：喂合成帧，检查对端状态机与它回出去的字节。

    为什么要有：判定器/对端本身也要有**正方向**自测（本项目的规矩）。
    这里逐条对照 RFC 1661/1332 的应有动作，顺带用真实校验和验证 ICMP 回包。"""
    fails = []

    def chk(cond, what):
        if not cond:
            fails.append(what)
        print(("  PASS  " if cond else "  FAIL  ") + what)

    sent = []
    peer = Peer(sent.append, quiet=True)
    peer.t_lcp = time.monotonic()          # 抑制首次 tick 重传

    def frames():
        out = []
        for raw in sent:
            out += F.FrameSplitter().feed(raw)
        return out

    # 1) 设备 LCP CONF_REQ(ACCM=ffffffff) ⇒ 必须回 CONF_ACK 且选项照抄
    accm = bytes([F.LCP_OPT_ACCM, 6]) + b"\xff\xff\xff\xff"
    peer.feed(F.build_frame(F.PROTO_LCP,
                            F.lcp_packet(F.LCP_CONF_REQ, 3, accm)))
    f = frames()
    chk(len(f) == 1 and f[0][0] == F.PROTO_LCP and
        f[0][1][0] == F.LCP_CONF_ACK and f[0][1][1] == 3 and
        f[0][1][4:] == accm, "LCP CONF_REQ ⇒ CONF_ACK（id/选项照抄）")

    # 2) 收到自己的 LCP CONF_ACK ⇒ LCP UP，并且**立刻**发 IPCP CONF_REQ
    sent.clear()
    peer.feed(F.build_frame(F.PROTO_LCP,
                            F.lcp_packet(F.LCP_CONF_ACK, peer.lcp_id)))
    chk(peer.lcp_up, "LCP CONF_ACK ⇒ lcp_up")
    f = frames()
    chk(len(f) == 1 and f[0][0] == F.PROTO_IPCP and f[0][1][0] == F.LCP_CONF_REQ
        and F.parse_options(F.PROTO_IPCP, f[0][1][4:]) ==
        [(F.IPCP_OPT_IP, F.ip_bytes(LOCAL_IP))],
        f"LCP UP 后立刻发 IPCP CONF_REQ(IP={LOCAL_IP})")

    # 3) 设备 IPCP CONF_REQ(0.0.0.0) ⇒ 必须 Nak 成 192.168.7.2
    sent.clear()
    peer.feed(F.build_frame(F.PROTO_IPCP, F.ipcp_packet(
        F.LCP_CONF_REQ, 9, F.ipcp_opt(F.IPCP_OPT_IP, F.ip_bytes("0.0.0.0")))))
    f = frames()
    chk(len(f) == 1 and f[0][1][0] == F.LCP_CONF_NAK and
        F.parse_options(F.PROTO_IPCP, f[0][1][4:]) ==
        [(F.IPCP_OPT_IP, F.ip_bytes(PEER_IP))],
        f"IPCP 请求 0.0.0.0 ⇒ CONF_NAK({PEER_IP})")

    # 4) 设备按 Nak 重问 192.168.7.2 ⇒ 这次必须 Ack
    sent.clear()
    peer.feed(F.build_frame(F.PROTO_IPCP, F.ipcp_packet(
        F.LCP_CONF_REQ, 10, F.ipcp_opt(F.IPCP_OPT_IP, F.ip_bytes(PEER_IP)))))
    f = frames()
    chk(len(f) == 1 and f[0][1][0] == F.LCP_CONF_ACK, "IPCP 请求正确地址 ⇒ ACK")

    # 5) 收到自己 IPCP 请求的 ACK ⇒ ipcp_up
    peer.feed(F.build_frame(F.PROTO_IPCP,
                            F.ipcp_packet(F.LCP_CONF_ACK, peer.ipcp_id)))
    chk(peer.ipcp_up, "IPCP CONF_ACK ⇒ ipcp_up")

    # 6) ICMP Echo-Request ⇒ Echo-Reply，地址互换、校验和自洽
    sent.clear()
    icmp = bytearray([8, 0, 0, 0, 0x12, 0x34, 0x00, 0x01]) + b"phywear"
    icmp[2:4] = struct.pack(">H", ip_checksum(bytes(icmp)))
    iph = bytearray(20)
    iph[0] = 0x45
    iph[8] = 64
    iph[9] = 1
    iph[12:16] = F.ip_bytes(PEER_IP)
    iph[16:20] = F.ip_bytes(LOCAL_IP)
    iph[2:4] = struct.pack(">H", 20 + len(icmp))
    iph[10:12] = struct.pack(">H", ip_checksum(bytes(iph)))
    peer.feed(F.build_frame(F.PROTO_IP, bytes(iph) + bytes(icmp)))
    f = frames()
    ok = len(f) == 1 and f[0][0] == F.PROTO_IP
    if ok:
        rp = f[0][1]
        rihl = (rp[0] & 0x0F) * 4
        ricmp = rp[rihl:]
        ok = (rp[12:16] == F.ip_bytes(LOCAL_IP) and
              rp[16:20] == F.ip_bytes(PEER_IP) and
              ricmp[0] == 0 and ricmp[8:] == b"phywear" and
              ip_checksum(bytes(rp[:rihl])) == 0 and
              ip_checksum(bytes(ricmp)) == 0)
    chk(ok, "ICMP Echo-Request ⇒ Echo-Reply（地址互换 + 两个校验和都为 0）")

    # 7) 负对照：坏 FCS 的帧不能被当成有效帧
    bad = bytearray(F.build_frame(F.PROTO_IPCP, F.ipcp_packet(
        F.LCP_CONF_REQ, 11, F.ipcp_opt(F.IPCP_OPT_IP, F.ip_bytes(PEER_IP)))))
    bad[4] ^= 0x01
    before = peer.splitter.frames
    peer.feed(bytes(bad))
    chk(peer.splitter.frames == before, "坏 FCS 帧被丢弃（负对照）")

    print(f"\n自测：{'全部通过' if not fails else f'{len(fails)} 项失败'}")
    return 1 if fails else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--selftest", action="store_true",
                    help="不碰板子，跑对端状态机自测")
    ap.add_argument("--name", default="PhyWear")
    ap.add_argument("--timeout", type=float, default=60.0)
    ap.add_argument("--chunk", type=int, default=0,
                    help="每次 GATT 写的字节数；0 = 按协商 MTU 自动（MTU-3）")
    ap.add_argument("--peer", action="store_true",
                    help="自己当 PPP 对端（LCP+IPCP+ICMP），**不需要 root**")
    ap.add_argument("--sniff", action="store_true",
                    help="只解帧不回应（诊断用）")
    ap.add_argument("--duration", type=float, default=3600.0,
                    help="--peer/--sniff 跑多少秒后自动收尾（默认 1 h）")
    ap.add_argument("--quiet", action="store_true",
                    help="不逐帧打印（只打状态与统计）")
    args = ap.parse_args()

    if args.selftest:
        return selftest()

    c = C.Central(args.name, args.timeout)
    got = _open(c, args)
    if not got:
        return 1
    dev, rx, tx = got
    chrs = c.gatt_all(dev)
    chunk = _chunk_for(c, chrs, args)

    if args.sniff:
        args.peer = True
        args.quiet = args.quiet  # sniff 只是 peer 的"不回应"版

    if args.peer:
        if args.sniff:
            # sniff：不回应任何东西，只看设备写了什么
            peer = Peer(lambda b: None, quiet=args.quiet)
            pending = bytearray()
            if not _subscribe(c, tx, pending):
                return 1
            from gi.repository import GLib
            ctx = GLib.MainContext.default()
            deadline = time.monotonic() + args.duration
            while time.monotonic() < deadline:
                while ctx.pending():
                    ctx.iteration(False)
                if pending:
                    data = bytes(pending)
                    del pending[:]
                    peer.feed(data)
                time.sleep(0.02)
            log(f"sniff 结束：收 {peer.rx_frames} 帧，坏 FCS {peer.splitter.bad_fcs}")
            _unsubscribe(c, tx)
            return 0
        return run_peer(c, args, chrs, rx, tx, chunk)

    return run_bridge(c, chrs, rx, tx, chunk)


if __name__ == "__main__":
    sys.exit(main())
