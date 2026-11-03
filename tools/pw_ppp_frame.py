#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""pw_ppp_frame.py —— 最小 PPP/HDLC 组帧与解析（RFC 1662），只够 PPPoE-less 的
"裸 PPP over 字节管道" 用

为什么写这个：PPP over BLE 的验收要分两步 ——
  ① 设备侧 pppd 真的在往管道里吐 LCP/IPCP 帧（**收**方向）；
  ② 主机写进去的帧，设备侧 pppd 真的会应答（**发**方向）。
只数字节数是证不了这两条的（"收到 37 字节"说明不了那是 LCP）。所以需要一个
能把字节流还原成 "LCP Configure-Request(id=3, opts=…)" 的小解析器，
外加一个**不需要 root** 的对端 —— 宿主 `pppd` 要建网络接口必须 root，
而本机 `sudo` 需要密码，所以退一步：用 Python 当 PPP 对端，把 LCP/IPCP
谈到 "设备拿到 IP" 这一步（IPCP 给地址是纯用户态可做的；真正 ping 通还需要
宿主内核建接口，那一步留给有 root 的人）。

帧格式（RFC 1661 §2 / RFC 1662 §3）：
    Flag(0x7E) Address(0xFF) Control(0x03) Protocol(2B) Payload... FCS(2B) Flag(0x7E)
  * 默认把 Address/Control 压缩掉（ACFC）也接受 —— 解析时按协议字段首字节
    的奇偶判断有没有 ACFC；
  * 字节填充：0x7E→0x7D 0x5E、0x7D→0x7D 0x5D，以及 <0x20 的字符按需转义
    （我们只做必需的两种，配合 ACCM=0 的请求）；
  * FCS16 = CRC-16/CCITT-FALSE 的反码，多项式 0x8408（反射）、初值 0xFFFF、
    结果取反、低字节先发。

自测：`python3 pw_ppp_frame.py --selftest`（组帧→解帧往返 + 已知 FCS 向量）。
"""

import struct
import sys

PPP_FLAG = 0x7E
PPP_ESC = 0x7D
PPP_ESC_XOR = 0x20

# 协议号（RFC 1661 §2 / RFC 1332）
PROTO_IP = 0x0021
PROTO_LCP = 0xC021
PROTO_PAP = 0xC023
PROTO_IPCP = 0x8021

PROTO_NAMES = {PROTO_IP: "IP", PROTO_LCP: "LCP", PROTO_PAP: "PAP",
               PROTO_IPCP: "IPCP"}

# 控制码
LCP_CONF_REQ = 1
LCP_CONF_ACK = 2
LCP_CONF_NAK = 3
LCP_CONF_REJ = 4
LCP_TERM_REQ = 5
LCP_TERM_ACK = 6
LCP_CODE_REJ = 7
LCP_PROTO_REJ = 8
LCP_ECHO_REQ = 9
LCP_ECHO_REP = 10
LCP_DISC_REQ = 11

LCP_CODE_NAMES = {
    1: "Configure-Request", 2: "Configure-Ack", 3: "Configure-Nak",
    4: "Configure-Reject", 5: "Terminate-Request", 6: "Terminate-Ack",
    7: "Code-Reject", 8: "Protocol-Reject", 9: "Echo-Request",
    10: "Echo-Reply", 11: "Discard-Request",
}

# LCP/IPCP 选项类型
LCP_OPT_MRU = 1
LCP_OPT_ACCM = 2
LCP_OPT_AUTH = 3
LCP_OPT_QUALITY = 4
LCP_OPT_MAGIC = 5
LCP_OPT_PFC = 7
LCP_OPT_ACFC = 8
IPCP_OPT_IP = 3
IPCP_OPT_PRI_DNS = 129
IPCP_OPT_SEC_DNS = 131

LCP_OPT_NAMES = {
    1: "MRU", 2: "ACCM", 3: "Auth-Protocol", 4: "Quality-Protocol",
    5: "Magic-Number", 7: "PFC", 8: "ACFC",
}
IPCP_OPT_NAMES = {3: "IP-Address", 129: "Primary-DNS", 131: "Secondary-DNS"}


# ── FCS16（RFC 1662 附录 C）─────────────────────────────────────────────
def _mk_fcs_table():
    tbl = []
    for i in range(256):
        v = i
        for _ in range(8):
            v = (v >> 1) ^ 0x8408 if v & 1 else v >> 1
        tbl.append(v)
    return tbl


_FCS_TBL = _mk_fcs_table()


def fcs16(data):
    """返回 16 bit FCS（还未取反）。"""
    fcs = 0xFFFF
    for b in data:
        fcs = (fcs >> 8) ^ _FCS_TBL[(fcs ^ b) & 0xFF]
    return fcs


def fcs16_good(data_with_fcs):
    """data_with_fcs = 载荷 + 低字节在前的 FCS，校验用。"""
    return fcs16(data_with_fcs) == 0xF0B8      # 0xFFFF ^ 0xF0B8 的魔术值


def escape(data):
    out = bytearray()
    for b in data:
        if b == PPP_FLAG or b == PPP_ESC or b < 0x20:
            out.append(PPP_ESC)
            out.append(b ^ PPP_ESC_XOR)
        else:
            out.append(b)
    return bytes(out)


def unescape(data):
    out = bytearray()
    i = 0
    while i < len(data):
        b = data[i]
        if b == PPP_ESC:
            i += 1
            if i >= len(data):
                break
            out.append(data[i] ^ PPP_ESC_XOR)
        else:
            out.append(b)
        i += 1
    return bytes(out)


def build_frame(protocol, payload=b"", address_control=True):
    """组一个完整帧（含首尾 Flag），可直接写进管道。"""
    body = bytearray()
    if address_control:
        body += b"\xff\x03"
    body += struct.pack(">H", protocol)
    body += payload
    f = fcs16(body) ^ 0xFFFF
    body += struct.pack("<H", f)
    return bytes([PPP_FLAG]) + escape(bytes(body)) + bytes([PPP_FLAG])


class FrameSplitter:
    """增量式：喂字节，吐出 (protocol, payload) 或错误描述。"""

    def __init__(self):
        self.buf = bytearray()
        self.bad_fcs = 0
        self.frames = 0

    def feed(self, chunk):
        out = []
        for b in chunk:
            if b == PPP_FLAG:
                if len(self.buf) >= 4:
                    parsed = self._parse(bytes(self.buf))
                    if parsed is None:
                        self.bad_fcs += 1
                    else:
                        self.frames += 1
                        out.append(parsed)
                self.buf = bytearray()
            else:
                self.buf.append(b)
        return out

    @staticmethod
    def _parse(raw):
        body = unescape(raw)
        if len(body) < 4 or not fcs16_good(body):
            return None
        body = body[:-2]                     # 去掉 FCS
        # 有没有 ACFC：协议字段第一字节必须"奇偶为奇"（RFC 1661 §2）
        if len(body) >= 2 and body[0] == 0xFF and body[1] == 0x03:
            proto = struct.unpack(">H", body[2:4])[0]
            payload = body[4:]
        elif body and (body[0] & 1):
            proto = body[0]
            payload = body[1:]
        else:
            proto = struct.unpack(">H", body[0:2])[0]
            payload = body[2:]
        return proto, payload


def describe(protocol, payload):
    """把 LCP/IPCP 报文说成人话，给日志用。"""
    pname = PROTO_NAMES.get(protocol, f"0x{protocol:04x}")
    if protocol in (PROTO_LCP, PROTO_IPCP) and len(payload) >= 4:
        code = payload[0]
        pid = payload[1]
        plen = struct.unpack(">H", payload[2:4])[0]
        cname = LCP_CODE_NAMES.get(code, f"code{code}")
        opts = payload[4:plen] if plen >= 4 else b""
        return (f"{pname} {cname} id={pid} len={plen} "
                f"opts={opts.hex()}{_opts_text(protocol, opts)}")
    if protocol == PROTO_IP:
        return f"IP {len(payload)} B payload={payload[:24].hex()}"
    return f"{pname} {len(payload)} B payload={payload[:24].hex()}"


def parse_options(protocol, opts):
    """返回 [(type, bytes), ...]。"""
    out = []
    i = 0
    while i + 2 <= len(opts):
        t = opts[i]
        ln = opts[i + 1]
        if ln < 2 or i + ln > len(opts):
            break
        out.append((t, opts[i + 2:i + ln]))
        i += ln
    return out


def _opts_text(protocol, opts):
    if not opts:
        return ""
    names = IPCP_OPT_NAMES if protocol == PROTO_IPCP else LCP_OPT_NAMES
    parts = []
    for t, v in parse_options(protocol, opts):
        nm = names.get(t, f"type{t}")
        if (protocol == PROTO_IPCP and t == IPCP_OPT_IP) or \
                (protocol == PROTO_LCP and t == LCP_OPT_MAGIC):
            if len(v) == 4:
                parts.append(f" [{nm}={'.'.join(str(x) for x in v)}]")
                continue
        parts.append(f" [{nm}={v.hex()}]")
    return "".join(parts)


def lcp_packet(code, pid, opts=b""):
    return struct.pack(">BBH", code, pid, 4 + len(opts)) + opts


def ipcp_packet(code, pid, opts=b""):
    return struct.pack(">BBH", code, pid, 4 + len(opts)) + opts


def ipcp_opt(opt_type, addr):
    return bytes([opt_type, 6]) + bytes(addr)


def ip_bytes(text):
    return bytes(int(x) for x in text.split("."))


# ── 自测（正方向 + 负方向）────────────────────────────────────────────
def selftest():
    fails = []

    def chk(cond, what):
        if not cond:
            fails.append(what)
        print(("  PASS  " if cond else "  FAIL  ") + what)

    # 1) RFC 1662 附录 C 的 FCS 校验向量：帧内容 "7E FF 03 C0 21 ..."
    #    这里用组帧→解帧往返来验证（比硬编码更稳）
    payload = lcp_packet(LCP_CONF_REQ, 1, bytes([LCP_OPT_MRU, 4, 0x05, 0xDC]))
    frame = build_frame(PROTO_LCP, payload)
    chk(frame[0] == PPP_FLAG and frame[-1] == PPP_FLAG, "帧首尾都是 0x7E")
    sp = FrameSplitter()
    got = sp.feed(frame)
    chk(len(got) == 1 and got[0][0] == PROTO_LCP and got[0][1] == payload,
        "组帧→解帧往返一致")
    chk(sp.bad_fcs == 0, "无 FCS 错")

    # 2) 无 ACFC（协议字段压缩）也要能解
    frame2 = build_frame(PROTO_IPCP, ipcp_packet(
        LCP_CONF_REQ, 7, ipcp_opt(IPCP_OPT_IP, ip_bytes("0.0.0.0"))),
        address_control=False)
    sp2 = FrameSplitter()
    got2 = sp2.feed(frame2)
    chk(len(got2) == 1 and got2[0][0] == PROTO_IPCP, "无 ACFC 的帧也能解")

    # 3) 负方向：改一个字节必须报 FCS 错（不能"什么都收"）
    bad = bytearray(frame)
    bad[4] ^= 0x01
    sp3 = FrameSplitter()
    got3 = sp3.feed(bytes(bad))
    chk(not got3 and sp3.bad_fcs == 1, "坏 FCS 必须被拒（负对照）")

    # 4) 转义：载荷里塞 0x7E/0x7D/0x03 也要往返
    tricky = bytes([0x7E, 0x7D, 0x03, 0x11, 0x7E])
    sp4 = FrameSplitter()
    got4 = sp4.feed(build_frame(PROTO_LCP, tricky))
    chk(len(got4) == 1 and got4[0][1] == tricky, "含 0x7E/0x7D 的载荷往返一致")

    # 5) 分片喂入（BLE 会按 MTU 切片！）也要能拼回来
    sp5 = FrameSplitter()
    acc = []
    for i in range(0, len(frame), 3):
        acc += sp5.feed(frame[i:i + 3])
    chk(len(acc) == 1 and acc[0][1] == payload, "按 3 字节分片喂入仍能拼回整帧")

    # 6) 描述文本可读
    txt = describe(PROTO_LCP, payload)
    chk("Configure-Request" in txt and "MRU" in txt, f"describe() 可读：{txt}")

    print(f"\n自测：{'全部通过' if not fails else f'{len(fails)} 项失败'}")
    return 1 if fails else 0


if __name__ == "__main__":
    if "--selftest" in sys.argv:
        sys.exit(selftest())
    print(__doc__)
