#!/usr/bin/env python3
"""pw_ble_central.py —— 宿主侧 BLE 中心设备（BlueZ + D-Bus），用来验 PhyWear 的 B3

为什么有它：B3 原本设计成"人工用手机连一次"，但**本机现在有蓝牙控制器**
（VM 直通 Intel AX201，hci0）。既然能力具备，就不该再把可自动化的部分留给人。

它做的事与手机上用 nRF Connect 点的那几下**完全对应**：
  1. 扫描，按**名字**找 "PhyWear"（⚠️ 手表广播用随机地址，按 MAC 找不到）
  2. 连接，等 ServicesResolved
  3. 找到服务 e0f1a000-… 与它的两条特征
  4. 读 …a001（期望 16 B，解出 ax/ay/az）
  5. 对 …a001 打开 Notify，收 2 个包
  6. 往 …a002 写 "ping"
最后打 PASS/FAIL 表。

依赖：python3-dbus（本机已装）。不需要 bleak。
用法：
    python3 tools/phywear/pw_ble_central.py [--name PhyWear] [--timeout 30]
"""

import argparse
import sys
import time

import dbus
import dbus.mainloop.glib          # noqa: F401  (需要它才能收 D-Bus 信号)
from gi.repository import GLib

# 收 PropertiesChanged（通知值）必须把连接挂到一个主循环上，否则 add_signal_receiver
# 会抛 "D-Bus connections must be attached to a main loop"（第一版就栽在这里）。
dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)

BLUEZ = "org.bluez"
ADAPTER_IFACE = "org.bluez.Adapter1"
DEVICE_IFACE = "org.bluez.Device1"
GATT_SVC_IFACE = "org.bluez.GattService1"
GATT_CHR_IFACE = "org.bluez.GattCharacteristic1"
PROPS_IFACE = "org.freedesktop.DBus.Properties"
OM_IFACE = "org.freedesktop.DBus.ObjectManager"

SVC_UUID = "e0f1a000-1b2c-4d5e-8f90-a1b2c3d4e5f6"
SENSOR_UUID = "e0f1a001-1b2c-4d5e-8f90-a1b2c3d4e5f6"
CMD_UUID = "e0f1a002-1b2c-4d5e-8f90-a1b2c3d4e5f6"
TEXT_UUID = "e0f1a003-1b2c-4d5e-8f90-a1b2c3d4e5f6"


def log(m):
    print(f"[central] {m}", flush=True)


def norm(uuid):
    return str(uuid).lower()


class Central:
    def __init__(self, name, timeout):
        self.name = name
        self.timeout = timeout
        self.bus = dbus.SystemBus()
        self.om = dbus.Interface(self.bus.get_object(BLUEZ, "/"), OM_IFACE)
        self.adapter_path = None
        self.adapter = None

    def find_adapter(self):
        for path, ifaces in self.om.GetManagedObjects().items():
            if ADAPTER_IFACE in ifaces:
                self.adapter_path = path
                self.adapter = dbus.Interface(
                    self.bus.get_object(BLUEZ, path), ADAPTER_IFACE)
                log(f"adapter = {path}")
                return True
        return False

    def props(self, path, iface):
        return dbus.Interface(self.bus.get_object(BLUEZ, path), PROPS_IFACE)

    def forget_stale(self):
        """扫描前清掉 BlueZ 里缓存的目标设备记录。

        为什么必须做：手表广播用**随机地址**，每次上电可能换一个；而 BlueZ 会把
        上一次的 `dev_XX_XX_…` 连同 `Name="PhyWear"` 一起缓存下来。于是
        GetManagedObjects() 里存在一个**名字对得上、但早就不广播**的对象，
        扫描循环会立刻命中它（第一版就差点连到一个死地址上）。
        清掉之后，命中的一定是本轮真正扫到的那个。
        """
        try:
            objects = self.om.GetManagedObjects()
        except dbus.DBusException as e:
            log(f"读对象表失败，跳过清理：{e.get_dbus_name()}")
            return
        for path, ifaces in objects.items():
            if DEVICE_IFACE not in ifaces:
                continue
            if not str(path).startswith(self.adapter_path + "/dev_"):
                continue
            d = ifaces[DEVICE_IFACE]
            nm = str(d.get("Name", d.get("Alias", "")))
            if self.name.lower() not in nm.lower():
                continue
            if bool(d.get("Connected", False)):
                log(f"已有连接中的 {nm!r} @ {path}，保留")
                continue
            try:
                self.adapter.RemoveDevice(dbus.ObjectPath(path))
                log(f"清掉缓存记录 {nm!r} @ {path}（随机地址可能已变）")
            except dbus.DBusException as e:
                log(f"  清理 {path} 失败：{e.get_dbus_name()}（继续）")

    def scan_for(self):
        """扫描并按名字（或服务 UUID）找到目标设备。"""
        try:
            self.adapter.SetDiscoveryFilter(
                dbus.Dictionary({"Transport": dbus.String("le")},
                                signature="sv"))
        except dbus.DBusException as e:
            log(f"SetDiscoveryFilter 跳过（{e.get_dbus_name()}）")

        self.forget_stale()

        log(f"开始扫描，找名字含 {self.name!r} 的设备 ……")
        self.adapter.StartDiscovery()
        deadline = time.time() + self.timeout
        found = None
        seen = {}
        try:
            while time.time() < deadline:
                for path, ifaces in self.om.GetManagedObjects().items():
                    if DEVICE_IFACE not in ifaces:
                        continue
                    if not str(path).startswith(self.adapter_path + "/dev_"):
                        continue
                    d = ifaces[DEVICE_IFACE]
                    nm = str(d.get("Name", d.get("Alias", "")))
                    seen[str(path)] = nm
                    uuids = [norm(u) for u in d.get("UUIDs", [])]
                    if self.name.lower() in nm.lower() or SVC_UUID in uuids:
                        found = path
                        log(f"找到：{nm!r} @ {path}"
                            f"（UUID 命中={SVC_UUID in uuids}）")
                        break
                if found:
                    break
                time.sleep(1.0)
        finally:
            try:
                self.adapter.StopDiscovery()
            except dbus.DBusException:
                pass
        if not found:
            log(f"扫描 {self.timeout}s 没找到。期间看到的设备：")
            for p, n in list(seen.items())[:12]:
                log(f"    {n!r}  {p}")
        return found

    def connect(self, dev_path):
        """连接并等服务解析。

        为什么要重试：BlueZ 自己也会去 auto-connect，两条路撞上时
        `Device1.Connect()` 会直接抛 `NoReply`（第一版就撞了一次）。
        所以这里把 Connect 包在重试里，并且**以属性为准**判断成功。
        """
        p = self.props(dev_path, DEVICE_IFACE)
        dev = dbus.Interface(self.bus.get_object(BLUEZ, dev_path), DEVICE_IFACE)
        deadline = time.time() + self.timeout
        attempt = 0
        while time.time() < deadline:
            if bool(p.Get(DEVICE_IFACE, "Connected")) and \
                    bool(p.Get(DEVICE_IFACE, "ServicesResolved")):
                log(f"已连接且服务已解析（{str(p.Get(DEVICE_IFACE, 'Address'))}）")
                return True
            if attempt < 3 and not bool(p.Get(DEVICE_IFACE, "Connected")):
                attempt += 1
                log(f"发起连接（第 {attempt} 次）……")
                try:
                    dev.Connect()
                except dbus.DBusException as e:
                    # NoReply / InProgress：多半是 BlueZ 自己正在连，继续等属性变化
                    log(f"  Connect() 抛 {e.get_dbus_name()}，继续等属性……")
            time.sleep(0.5)
        log("连接或服务解析超时")
        return False

    def disconnect(self, dev_path):
        """显式断开。

        为什么必须有这一步（2026-09-18 用户报的 bug）：
          BlueZ 是中心设备，**它不会因为我们的进程退出就断开 LE 链路**。
          早先的脚本测完直接退出，链路就一直挂着 ⇒ 手表端（外设）那边
          `connected` 永远为真，主页一直显示"已连接"，断开很久也不变；
          而且它还占着连接、不再广播，手机随后也连不上。
          用户看到的就是"蓝牙早断了，手表还显示已连接"。
          所以测完必须主动 Disconnect，把手表放回可被发现状态。
        """
        try:
            dev = dbus.Interface(self.bus.get_object(BLUEZ, dev_path),
                                 DEVICE_IFACE)
            dev.Disconnect()
            log("已主动断开（手表回到可被发现）")
            return True
        except dbus.DBusException as e:
            log(f"断开失败：{e.get_dbus_name()}（手表可能仍显示已连接）")
            return False

    def gatt(self, dev_path):
        """返回 (service_path, {uuid: chr_path})"""
        svc, chrs = None, {}
        for path, ifaces in self.om.GetManagedObjects().items():
            if GATT_SVC_IFACE in ifaces:
                if str(ifaces[GATT_SVC_IFACE].get("Device", "")) != str(dev_path):
                    continue
                if norm(ifaces[GATT_SVC_IFACE]["UUID"]) == SVC_UUID:
                    svc = path
            if GATT_CHR_IFACE in ifaces and svc is None:
                chrs[norm(ifaces[GATT_CHR_IFACE]["UUID"])] = path
        # 第二遍：拿到 svc 之后再按 Service 归属挑特征
        if svc:
            chrs = {}
            for path, ifaces in self.om.GetManagedObjects().items():
                if GATT_CHR_IFACE in ifaces and \
                        str(ifaces[GATT_CHR_IFACE].get("Service", "")) == str(svc):
                    chrs[norm(ifaces[GATT_CHR_IFACE]["UUID"])] = path
        return svc, chrs

    def gatt_all(self, dev_path):
        """返回本设备**所有服务**下的特征 {uuid: chr_path}（跨服务）。

        为什么需要单独一个方法：`gatt()` 只收**主传感器服务**（SVC_UUID）下的
        特征 —— B2/B3 的断言都是针对那一组句柄写的，不能动。但 PPP over BLE
        用的是**第二个服务**（…a1xx），属于别的 GATT service 路径，用 `gatt()`
        永远找不到（2026-09-18 实测：脚本报"没找到 PPP 特征"，其实是过滤掉的）。
        """
        objs = self.om.GetManagedObjects()
        chrs = {}
        for path, ifaces in objs.items():
            if GATT_CHR_IFACE not in ifaces:
                continue
            svc_path = str(ifaces[GATT_CHR_IFACE].get("Service", ""))
            svc_iface = objs.get(svc_path, {}).get(GATT_SVC_IFACE, {})
            if str(svc_iface.get("Device", "")) != str(dev_path):
                continue
            chrs[norm(ifaces[GATT_CHR_IFACE]["UUID"])] = path
        return chrs

    def read(self, chr_path):
        iface = dbus.Interface(self.bus.get_object(BLUEZ, chr_path),
                               GATT_CHR_IFACE)
        val = iface.ReadValue(dbus.Dictionary({}, signature="sv"))
        return bytes(bytearray(val))

    def write(self, chr_path, data):
        iface = dbus.Interface(self.bus.get_object(BLUEZ, chr_path),
                               GATT_CHR_IFACE)
        iface.WriteValue(dbus.Array([dbus.Byte(b) for b in data],
                                    signature="y"),
                         dbus.Dictionary({}, signature="sv"))

    def notify(self, chr_path, want, timeout):
        """订阅并收集 want 个包（按 handle/时间窗去重）。"""
        got = []
        iface = dbus.Interface(self.bus.get_object(BLUEZ, chr_path),
                               GATT_CHR_IFACE)

        def on_props(interface, changed, invalidated):
            if interface != GATT_CHR_IFACE:
                return
            if "Value" in changed:
                got.append(bytes(bytearray(changed["Value"])))

        self.bus.add_signal_receiver(
            on_props, dbus_interface=PROPS_IFACE,
            signal_name="PropertiesChanged", path=chr_path)

        iface.StartNotify()
        deadline = time.time() + timeout
        ctx = GLib.MainContext.default()
        while time.time() < deadline and len(got) < want:
            # 把主循环里已排队的信号处理掉（非阻塞 iteration）
            while ctx.pending():
                ctx.iteration(False)
            time.sleep(0.1)
        try:
            iface.StopNotify()
        except dbus.DBusException:
            pass
        self.bus.remove_signal_receiver(on_props,
                                        dbus_interface=PROPS_IFACE,
                                        signal_name="PropertiesChanged",
                                        path=chr_path)
        return got

    def perf(self, chr_path, n_lat=20, n_bulk=40, bulk_len=12, window=6.0):
        """链路性能测量：**应用层往返延迟** + 定长载荷的**往返吞吐**。

        为什么用"写一条文本 → 等设备把 echo 发回来"当尺子：
        这是**端到端**的量（含 ATT 写、设备处理、通知回程），而且设备侧
        `pw_text_write()` 是立刻回 echo 的 —— 中间没有人为延时，所以这条
        往返时间基本就是「BLE 连接间隔 × 往返跳数」。连接间隔一变，它立刻跟着变，
        正是我们做 A/B 要看的量。

        返回 (rtts, bulk_ok, bulk_secs, err)：
          rtts  : 每条往返耗时（秒），未收到为 None
          bulk_ok / bulk_secs : window 秒内成功往返的条数与其用时
        """
        got = []
        iface = dbus.Interface(self.bus.get_object(BLUEZ, chr_path),
                               GATT_CHR_IFACE)

        def on_props(interface, changed, invalidated):
            if interface != GATT_CHR_IFACE:
                return
            if "Value" in changed:
                got.append(bytes(bytearray(changed["Value"])))

        self.bus.add_signal_receiver(
            on_props, dbus_interface=PROPS_IFACE,
            signal_name="PropertiesChanged", path=chr_path)

        ctx = GLib.MainContext.default()

        def drain(seconds):
            """把已排队的 D-Bus 信号处理掉（非阻塞 iteration）。"""
            t_end = time.time() + seconds
            while time.time() < t_end:
                while ctx.pending():
                    ctx.iteration(False)
                time.sleep(0.002)

        rtts = []
        try:
            iface.StartNotify()
            drain(1.2)                      # 等 CCC 写下去、链路稳一点

            # ── 延迟：一条一条来，互不干扰 ──
            for i in range(n_lat):
                msg = ("p%03d" % i).encode()
                want = b"echo: " + msg
                got.clear()
                t0 = time.time()
                try:
                    self.write(chr_path, msg)
                except dbus.DBusException as e:
                    return rtts, 0, 0.0, e.get_dbus_name()

                while time.time() - t0 < 3.0:
                    drain(0.05)
                    if want in got:
                        break
                rtts.append((time.time() - t0) if want in got else None)

            # ── 吞吐：连着灌 n_bulk 条定长文本，数回程 ──
            # 定长是有意的：这样"往返条数/秒"可以直接换成 B/s 比较。
            ok = 0
            payload = ("x" * bulk_len).encode()
            t0 = time.time()
            while time.time() - t0 < window and ok < n_bulk:
                got.clear()
                self.write(chr_path, payload)
                t1 = time.time()
                while time.time() - t1 < 2.0:
                    drain(0.05)
                    if b"echo: " + payload in got:
                        ok += 1
                        break
                else:
                    break
            bulk_secs = time.time() - t0
        finally:
            try:
                iface.StopNotify()
            except dbus.DBusException:
                pass
            self.bus.remove_signal_receiver(on_props,
                                            dbus_interface=PROPS_IFACE,
                                            signal_name="PropertiesChanged",
                                            path=chr_path)

        return rtts, ok, bulk_secs, None

    def text_roundtrip(self, chr_path, timeout):
        """文本特征（…a003）的双向连通性测试 —— 这就是"蓝牙串口"的判据。

        做的是：订阅 → 写一条带时间戳的文本 → **等设备把它原样 echo 回来**。
        为什么用"echo 回来"当判据而不是"写成功"：BlueZ 的 WriteValue 返回成功
        只说明本地协议栈把包发出去了，空口上丢没丢、设备侧收没收到、
        设备能不能主动发回来，它一概不知道。收到 echo 才证明
        「手机→手表」和「手表→手机」**两个方向**都通。
        """
        got = []
        iface = dbus.Interface(self.bus.get_object(BLUEZ, chr_path),
                               GATT_CHR_IFACE)

        def on_props(interface, changed, invalidated):
            if interface != GATT_CHR_IFACE:
                return
            if "Value" in changed:
                got.append(bytes(bytearray(changed["Value"])))

        self.bus.add_signal_receiver(
            on_props, dbus_interface=PROPS_IFACE,
            signal_name="PropertiesChanged", path=chr_path)

        iface.StartNotify()

        ctx = GLib.MainContext.default()
        # 先把订阅本身的 CCC 写下去（StartNotify 会自动写 CCC），给它一点时间
        t0 = time.time()
        while time.time() - t0 < 1.0:
            while ctx.pending():
                ctx.iteration(False)
            time.sleep(0.1)

        msg = "hello-%d" % int(time.time())
        try:
            self.write(chr_path, msg.encode("utf-8"))
        except dbus.DBusException as e:
            iface.StopNotify()
            return msg, None, got, e.get_dbus_name()

        want = ("echo: " + msg).encode("utf-8")
        echo = None
        deadline = time.time() + timeout
        while time.time() < deadline:
            while ctx.pending():
                ctx.iteration(False)
            for pkt in got:
                if pkt.startswith(b"echo: "):
                    echo = pkt
                    break
            if echo is not None:
                break
            time.sleep(0.1)

        try:
            iface.StopNotify()
        except dbus.DBusException:
            pass
        self.bus.remove_signal_receiver(on_props,
                                        dbus_interface=PROPS_IFACE,
                                        signal_name="PropertiesChanged",
                                        path=chr_path)
        return msg, want, got, (None if echo is not None else "no echo")


def run_full_test(report, name="PhyWear", timeout=30.0, notify_count=2,
                  text_test=False, disconnect_at_end=True, perf=False):
    """跑完整测试序列，逐项通过 `report(label, ok, detail)` 回调。

    **为什么要把序列抽出来**：这条序列原本内联在 main() 里，只服务命令行。
    后来要在桌面上放一个"点一下就测"的 GUI —— 如果 GUI 自己再实现一遍，
    两份实现迟早会跑偏（改了一处忘了另一处，而两边都"看起来在测同一件事"）。
    所以序列只有这一份：CLI 传一个打印回调，GUI 传一个往队列里塞的回调。

    返回 (rows, info)：
      rows = [(label, ok, detail), …]     供判定/展示
      info = {…}                          给 GUI 展示的摘要（对端地址、|a|、echo 等）
    """
    rows = []
    info = {"peer": "", "mag": None, "echo": "", "packets": 0, "adapter": ""}

    def check(label, ok, detail=""):
        rows.append((label, ok, detail))
        report(label, ok, detail)

    c = Central(name, timeout)
    if not c.find_adapter():
        check("找到蓝牙适配器", False, "宿主没有蓝牙控制器（hciconfig 为空？）")
        return rows, info

    info["adapter"] = c.adapter_path or ""
    check("找到蓝牙适配器", True, info["adapter"])

    dev = c.scan_for()
    check("扫描到 PhyWear", dev is not None,
          str(dev) if dev else "没找到 —— 手表开机了吗？主页显示「蓝牙」了吗？")
    if not dev:
        return rows, info

    info["peer"] = str(dev).rsplit("/", 1)[-1]
    check("连接成功且服务已解析", c.connect(dev))
    svc, chrs = c.gatt(dev)
    check("找到服务 e0f1a000-…", svc is not None, str(svc) if svc else "")
    sensor = chrs.get(SENSOR_UUID)
    cmd = chrs.get(CMD_UUID)
    check("找到 sensor 特征 …a001", sensor is not None)
    check("找到 cmd 特征 …a002", cmd is not None)
    if not (svc and sensor and cmd):
        return rows, info

    try:
        val = c.read(sensor)
        ok = len(val) == 16
        ax = int.from_bytes(val[0:2], "little", signed=True) if ok else 0
        ay = int.from_bytes(val[2:4], "little", signed=True) if ok else 0
        az = int.from_bytes(val[4:6], "little", signed=True) if ok else 0
        mag = (ax * ax + ay * ay + az * az) ** 0.5
        info["mag"] = mag
        check("读 …a001 得到 16 B", ok, f"{len(val)} B")
        check("解出加速度且 |a| 合理(300~3000mg)", 300 <= mag <= 3000,
              f"ax={ax} ay={ay} az={az} mg |a|={mag:.0f} mg")
    except dbus.DBusException as e:
        check("读 …a001 得到 16 B", False, e.get_dbus_name())

    try:
        packets = c.notify(sensor, notify_count, timeout)
        info["packets"] = len(packets)
        check(f"Notify 收到 >= {notify_count} 个包",
              len(packets) >= notify_count,
              f"收到 {len(packets)} 个，首个 {packets[0].hex() if packets else '-'}")
    except dbus.DBusException as e:
        check(f"Notify 收到 >= {notify_count} 个包", False, e.get_dbus_name())

    try:
        c.write(cmd, b"ping")
        check("写 …a002 'ping' 成功", True)
    except dbus.DBusException as e:
        check("写 …a002 'ping' 成功", False, e.get_dbus_name())

    # ── 文本串口（…a003）：手表版蓝牙串口的连通性测试 ──
    if text_test:
        text = chrs.get(TEXT_UUID)
        check("找到 text 特征 …a003", text is not None)

        if text:
            try:
                st = c.read(text).decode("utf-8", "replace")
                check("读 …a003 得到状态串", st.startswith("PhyWear bt conn=1"),
                      st)
            except dbus.DBusException as e:
                check("读 …a003 得到状态串", False, e.get_dbus_name())

            try:
                msg, want, got, err = c.text_roundtrip(text, timeout)
                got_s = [p.decode("utf-8", "replace") for p in got]
                info["echo"] = got_s[0] if got_s else ""
                check("写文本后收到设备 echo（双向连通）", err is None,
                      f"写 '{msg}' → 收到 {got_s}")

                # 顺带验证手表**主动**发的那条也走同一条特征
                #（"发送测试"按钮走的就是 pw_bt_send_text → notify）
                if err is None and got:
                    check("echo 内容与所写一致",
                          want in got, want.decode("utf-8", "replace"))
            except dbus.DBusException as e:
                check("写文本后收到设备 echo（双向连通）", False,
                      e.get_dbus_name())

    if perf:
        text = chrs.get(TEXT_UUID)
        if text:
            rtts, ok, secs, perr = c.perf(text)
            good = [r for r in rtts if r is not None]
            if perr:
                check("性能测量", False, perr)
            elif not good:
                check("性能测量", False, "一次往返都没测到")
            else:
                good.sort()
                med = good[len(good) // 2]
                check("往返延迟（中位/最小/最大）", True,
                      f"{med*1000:.1f} / {good[0]*1000:.1f} / "
                      f"{good[-1]*1000:.1f} ms（{len(good)}/{len(rtts)} 次成功）")
                if secs > 0:
                    check("定长往返吞吐", ok > 0,
                          f"{ok} 条 / {secs:.2f} s = {ok/secs:.1f} 条/s"
                          f"（每条 12 B 载荷）")

    # 收尾：主动断开。测完不断开的话，手表会一直显示"已连接"（而它是对的——
    # 链路真还在），并且不再广播，手机随后也连不上。见 Central.disconnect()。
    if disconnect_at_end:
        check("测完主动断开（手表回到可被发现）", c.disconnect(dev))

    return rows, info


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--name", default="PhyWear")
    ap.add_argument("--timeout", type=float, default=30.0)
    ap.add_argument("--notify-count", type=int, default=2)
    ap.add_argument("--text-test", action="store_true",
                    help="额外跑文本串口（…a003）的连通性测试："
                         "写一条带时间戳的文本，等设备把 echo 原样发回来")
    ap.add_argument("--perf", action="store_true",
                    help="链路性能测量：应用层往返延迟分布 + 定长载荷往返吞吐"
                         "（做连接参数/MTU 的 A/B 用）")
    ap.add_argument("--keep-connected", action="store_true",
                    help="测完**不**断开（默认会主动断开，否则手表一直显示"
                         "已连接且不再广播）")
    args = ap.parse_args()

    rows, _info = run_full_test(
        lambda label, ok, detail: log(
            f"{'PASS' if ok else 'FAIL'}  {label}  {detail}"),
        name=args.name, timeout=args.timeout,
        notify_count=args.notify_count, text_test=args.text_test,
        disconnect_at_end=not args.keep_connected, perf=args.perf)

    if not rows:
        return 2

    print()
    w = max(len(r[0]) for r in rows)
    print(f"{'判定':<6}{'检查项':<{w}}  说明")
    print("-" * 78)
    for n, ok, d in rows:
        print(f"{'PASS' if ok else 'FAIL':<6}{n:<{w}}  {d}")
    print("-" * 78)
    allok = all(r[1] for r in rows)
    print("结论：" + ("✅ 宿主侧 BLE 中心设备全过（B3 设备侧 + 空口均成立）"
                    if allok else "❌ 有未过项"))
    return 0 if allok else 1


if __name__ == "__main__":
    sys.exit(main())
