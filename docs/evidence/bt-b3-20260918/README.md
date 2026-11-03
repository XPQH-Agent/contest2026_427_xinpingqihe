# 证据：蓝牙 B3（连接 / 读 / 订阅 / 写）—— 2026-09-18 晚，**全自动，不需要手机**

固件：`nuttx.bin` **2,609,752 B**（出货构建；诊断开关 `PW_H4_TRACE`/`PW_BT_NOTIFY_TRACE` 均为 0）
板子：立创黄山派 SF32LB52-MOD-1-N16R8
宿主：本机 Intel AX201（`hci0`，`04:EC:D8:F3:73:8D`）当 BLE 中心设备

## 一条命令

```bash
cd ~/openvela
python3 tools/phywear/pw_bt_b3.py --central --out docs/evidence/bt-b3-20260918
```

复位板子 → 发 `phywear cap bt`（**确认广播真起来了才继续**）→ 宿主侧
`pw_ble_central.py` 扫描/连接/读/订阅/写 → 两侧各自判定 → 落盘 sha256。**退出码 0。**

## 判定表（原文见 stdout；设备侧 = `b3.log`，宿主侧 = `central.log`）

| 侧 | 检查项 | 结果 |
|---|---|---|
| 设备 | 广播启动 rc=0 | PASS |
| 设备 | 服务端自检 PASS | PASS |
| 设备 | 已连接（对端 `04:EC:D8:F3:73:8D`） | PASS |
| 设备 | 订阅 Notify（`ccc changed -> notify ON`） | PASS |
| 设备 | 收到写命令（`cmd #2 (4 B): 'ping'`） | PASS |
| 设备 | 通知无报错 | PASS |
| 宿主 | 扫描到 PhyWear（UUID 命中） | PASS |
| 宿主 | 连接成功且服务已解析 | PASS |
| 宿主 | 找到服务 `e0f1a000-…` | PASS |
| 宿主 | 找到 sensor 特征 `…a001` | PASS |
| 宿主 | 找到 cmd 特征 `…a002` | PASS |
| 宿主 | 读 `…a001` 得到 16 B | PASS |
| 宿主 | 解出加速度且 \|a\| 在 300~3000 mg | PASS（`ax=-23 ay=37 az=-1013`，`\|a\|=1014 mg`） |
| 宿主 | **Notify 收到 ≥2 个包** | PASS（收到 2 个，首个 `ebff26000afc540027ff7e0000000100`） |
| 宿主 | 写 `…a002` 'ping' 成功 | PASS |
| 合判 | `pw_bt_b3.py --central` 退出码 | **0 ✅** |

## 关键串口原文（`b3.log`）

```
    4.483  [bt] B2 adv start rc=0 (ok)
    4.484  [bt] SELFTEST PASS (fails=0)
   19.146  [bt] B3 connected: 04:EC:D8:F3:73:8D (public) err=0
   20.951  [bt] ccc changed -> notify ON
   21.752  [bt] ccc changed -> notify OFF      ← 宿主收满 2 个包后主动 StopNotify
   21.953  [bt] cmd #2 (4 B): 'ping'           ← 宿主写入，设备侧收到
```

## 这次为什么能全自动

VM 的蓝牙启用后，宿主自己就是一台 BLE 中心设备：`pw_ble_central.py` 用
BlueZ + D-Bus 完成"扫描 → 连接 → 读特征 → StartNotify 收包 → 写特征"，
**与手机上用 nRF Connect 点的那几下完全对应**。手机路径仍然保留
（`pw_bt_b3.py --out <dir> --wait 240`），可作独立交叉验证 —— 但**不是通过的前提**。

## 复核

```bash
sha256sum -c SHA256SUMS.txt
grep -a "B3 connected\|ccc changed\|cmd #2" b3.log
grep -a "Notify 收到" central.log
python3 ../../../tools/phywear/pw_bt_b3.py --selftest    # 判定器本身 5/5
```

关联证据：`docs/evidence/bt-gatt-handle-20260918/`（通知帧 value handle
0x0000 → 0x0010 的字节级前后对照，这一跳曾让"设备发了、手机收不到"卡了一整轮）。
