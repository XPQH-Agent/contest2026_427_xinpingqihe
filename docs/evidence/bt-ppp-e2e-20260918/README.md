# 证据：PPP over BLE **端到端打通**（任务 3 的 N1/N2）

日期：2026-09-18 深夜（继 `bt-ppp-20260918/` 那次"跑通但装不进 SRAM"之后）
被测：PhyWear 手表（SF32LB52-MOD-1-N16R8，固件 flash 2,576,888 B / SRAM 483,316 B = 92.19%）
宿主：Intel AX201 + BlueZ（**不需要 root** —— 宿主侧 PPP 对端是用户态 Python）

---

## 0. 结论

| 问题 | 答案 |
|---|---|
| **N1：手表拿到 IP 了吗** | ✅ 拿到了。设备侧 `ifconfig`：`ppp0 Link encap:TUN at RUNNING mtu 296` / `inet addr:192.168.7.2 DRaddr:192.168.7.1` |
| **N2：端到端通了吗** | ✅ 通了。设备上 `ping -c 3 192.168.7.1` → **3 packets transmitted, 3 received, 0% packet loss**（对端是宿主上的用户态 PPP 对端，ICMP Echo 由它应答） |
| 怎么做到不需要 root 的 | 宿主 `pppd` 建**内核网络接口**必须 root（本机 `sudo` 要密码）。所以另写了一个**用户态 PPP 对端**（LCP + IPCP + ICMP），跑在 `pw_bt_ppp.py --peer` 里；设备侧仍是**真正的 NuttX pppd**。要真正的内核接口仍可用 `--peer` 之外的桥模式 + `sudo pppd`（见 §4）。 |
| 两侧都验到了什么 | 链路层：LCP 双向 Configure-Request/Ack 成立；网络层：IPCP 分配 192.168.7.2；数据面：ICMP Echo-Request/Reply 双向穿过 BLE 管道 |

一份命令跑完：
```bash
python3 tools/phywear/pw_bt_ppp_e2e.py --out <证据目录>
# 退出码 0 = 6/6（ppp0 建好 / RUNNING / 拿到 IP / LCP UP / IPCP UP / ping 有回包）
```

## 1. 设备侧串口原文（`dev.log`）

```
[ppp] gatt service register rc=0 (ok)
[ppp] bridge task up (stack 8192 B, from heap)
[ppp] pty master fd=4 slave=/dev/pts/0
[ppp] pty 已设 raw（master+slave 两侧）slave fd=5
[ppp] probe open(/dev/tun) => 6 errno=0
[ppp] 等主机订阅 …a102（已等 0 s；主机侧跑 pw_bt_ppp.py --peer 或 pw_bt_ppp.py）
[ppp] host notify ON
[ppp] 主机订阅状态 sub=1（等了 9 s）→ 拉起 pppd
[ppp] starting pppd on /dev/pts/0
…
[netmgr] Found iface ppp0 addr 192.168.7.2
…
ifconfig
ppp0	Link encap:TUN at RUNNING mtu 296
	inet addr:192.168.7.2 DRaddr:192.168.7.1 Mask:0.0.0.0

ping -c 3 192.168.7.1
3 packets transmitted, 3 received, 0% packet loss, time 3003 ms
```

## 2. 宿主侧对端日志（`host.log`，节选）

```
[host-ppp] 已订阅设备→主机 通知；本进程作为 PPP 对端（192.168.7.1 ↔ 192.168.7.2）
[host-ppp] TX LCP Configure-Request id=1 opts=010405dc0506a1b2c3d4 [MRU=05dc] [Magic-Number=…]
[host-ppp] RX LCP Configure-Ack    id=1 opts=010405dc0506a1b2c3d4 [MRU=05dc] [Magic-Number=…]
[host-ppp] ★ LCP UP
[host-ppp] TX IPCP Configure-Request id=2 opts=0306c0a80701 [IP-Address=192.168.7.1]
[host-ppp] RX LCP Configure-Request id=0 opts=0206ffffffff [ACCM=ffffffff]
[host-ppp] TX LCP Configure-Ack     id=0 opts=0206ffffffff [ACCM=ffffffff]
[host-ppp] RX IPCP Configure-Ack    id=2 opts=0306c0a80701 [IP-Address=192.168.7.1]
[host-ppp] ★ IPCP UP（对端地址 192.168.7.1，设备应拿到 192.168.7.2）
[host-ppp] RX IPCP Configure-Request id=0 opts=030600000000 [IP-Address=0.0.0.0]
[host-ppp] TX IPCP Configure-Nak     id=0 opts=0306c0a80702 [IP-Address=192.168.7.2]
[host-ppp] RX IPCP Configure-Request id=1 opts=0306c0a80702 [IP-Address=192.168.7.2]
[host-ppp] TX IPCP Configure-Ack     id=1 opts=0306c0a80702 [IP-Address=192.168.7.2]
```

设备第一次 IPCP 请求的是 `0.0.0.0`（它还不知道自己该用什么地址）——
按 RFC 1332 由对端用 **Configure-Nak** 把 192.168.7.2 告诉它，这正是 NuttX
`ipcp.c` 的 `CONF_NAK` 分支（会 `netlib_set_ipv4addr()` + `netlib_ifup()`）。

## 3. 这一轮真正踩到的 7 个坑（都写进 docs/07 了）

| # | 现象 | 根因 | 修法 |
|---|---|---|---|
| 1 | `phywear btppp` 打印完 "starting pppd" 后整机静默 | `phywear` 主任务 `return 0` ⇒ **task_group 被销毁**，桥/pppd 两个 pthread（zblue 的 `k_thread_create` 就是 `pthread_create`）一起没了。**2026-09-18 那次把它归因成"pppd 栈不足→硬故障"，是错的** | 入口**不返回**；用 `phywear btppp &` 后台跑 |
| 2 | 设备侧一直 `notify FAILED rc=-22` | 通知发给了 `attrs[2]`（**rx 值**，只有 WRITE 没有 NOTIFY）⇒ EINVAL。正确索引是 `attrs[4]`（`BT_GATT_CHARACTERISTIC` 展开成"声明+值"两条） | `PW_PPP_ATTR_TX 2 → 4` |
| 3 | pppd 一个字节也读不到主机发的帧 | NuttX pty 默认是终端语义（master `OPOST\|OCRNL`；slave `OPOST\|ONLCR` + `ECHO\|ICANON`）⇒ **ICANON 按行缓存**，HDLC 帧里没有换行符 | master/slave 两侧都设 raw（`pw_ppp_set_raw()`） |
| 4 | `write(pty master)` 回 `errno=32 (EPIPE)` | 对端还没有开着的 slave fd 时，pty master 写不进去 | 桥自己 `open()` 一个 slave fd 并**一直开着**（只持不读，不抢数据） |
| 5 | 桥任务第一次 `write(pty)` 之后静默 | 桥栈 2048 B 不够（栈溢出无 panic） | 两个任务的栈改成**从堆上要**（8192 / 16384），既修栈又省 SRAM |
| 6 | pppd 一行都跑不出来、`ppp0` 都没建 | ①本端口 `K_PRIO_PREEMPT(x) = CONFIG_NUM_COOP_PRIORITIES + x`，而 **NuttX 是数值越大越紧急** ⇒ 桥(109) 反而比 pppd(108) 高；②slave 未开时 `poll()` 因 HUP 立刻返回 ⇒ 桥变满速死循环 | 桥压到 106（低于 pppd），循环里空转 `usleep(2 ms)` 兜底 |
| 7 | `nsh: ping: command not found` | 两个 ping 应用一个都没编进来 | defconfig 加 `CONFIG_SYSTEM_PING=y` |

顺带一个**误判纠正**：`pppd()` 只回一个光秃秃的 `2`（tun 打不开或 tty 打不开都回 2）。
第 4 条那个 EPIPE 与 `pppd returned 2` 是同一个原因（slave 没开着）；
修掉"常开 slave fd"之后 pppd 才真正起来。

## 4. 想用**真正的宿主网络接口**（能路由、能被别的程序用）

```bash
# ① 普通用户：只做桥（打印出 /dev/pts/N）
python3 tools/phywear/pw_bt_ppp.py
# ② 另一个终端，root：
sudo pppd /dev/pts/N 115200 noauth 192.168.7.1:192.168.7.2 local nodetach
```
这条路上设备侧完全一样，宿主侧换成内核 PPP；`ifconfig` 里会出现真的 `ppp0`。
**这一步必须 root**，所以本次验收走的是 §0 的用户态对端。

## 5. 复核

```bash
cd docs/evidence/bt-ppp-e2e-20260918 && sha256sum -c SHA256SUMS.txt
python3 tools/phywear/pw_ppp_frame.py --selftest   # HDLC/FCS 解析器自测（8 项）
python3 tools/phywear/pw_bt_ppp.py   --selftest    # 对端状态机自测（7 项，含负对照）
```
