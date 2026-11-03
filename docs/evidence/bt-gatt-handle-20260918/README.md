# 证据：GATT 通知 value handle = 0x0000（B3 "设备发了、手机收不到"的根因）

日期：2026-09-18
固件：`nuttx.bin` 2,610,304 B（含 `PW_H4_TRACE=1`，仅诊断用；正式版已关）

## 一句话

zblue（小米 Zephyr 移植）把上游"就地遍历 CCC"的代码重构成了独立函数
`gatt_notify_mc()`，但**漏了 `data.handle = handle;`** —— 于是
`notify_cb()` 拿到的 `data->handle` 恒为 0，空口上真的发出
`… 1b 00 00 …`（ATT Notification，value handle = 0x0000）的通知帧。
BlueZ 找不到句柄 0x0000 的特征，**静默丢弃**：
设备侧 `[bt] notify #N rc=0 len=16` 一路正常，宿主侧一个包都收不到。

## 怎么定位的（可复现）

1. `external/zblue/zblue/port/drivers/bluetooth/hci/h4.c` 里 `PW_H4_TRACE=1`
   → 串口打出每一条 H4 收发的十六进制原文；
2. `tools/phywear/pw_h4_notify_decode.py <日志> --expect-handle 0x0010`
   把 `[H4] TX` 按 HCI ACL → L2CAP(CID 0x0004=ATT) → ATT 逐层拆开，
   列出每条通知的 value handle。

## 修复前 / 修复后（同一脚本、同一口径）

| | 订阅窗口内通知帧 | value handle | 宿主侧收到 |
|---|---|---|---|
| 修复前 `before-b3.log` | 56 | **0x0000 × 56** | 0 个包 |
| 修复后 `after-b3.log` | 2 | 0x0010 × 2 | 2 个包 ✅ |

原文（`before-decode.txt`）：

```
订阅窗口: 36.405 -> 66.521
ATT 帧总数: 68   订阅窗口内通知帧: 56
通知 value handle 分布: 0x0000×56
  t=36.606   Notify value_handle=0x0000 value=e7 ff 27 00 0a fc 3f 00 35 ff 31 00 00 00 01 00
❌ 有 56 条通知的句柄不是 0x0010（例如 0x0000）—— 对端会静默丢弃
```

原文（`after-decode.txt`）：

```
订阅窗口: 23.548 -> 24.354
ATT 帧总数: 15   订阅窗口内通知帧: 2
通知 value handle 分布: 0x0010×2
  t=23.752   Notify value_handle=0x0010 value=ef ff 26 00 0c fc 3f 00 35 ff 62 00 00 00 01 00
✅ 2 条通知全部使用 value handle 0x0010
```

同一 boot 的宿主侧（`central.log`，BlueZ）：

```
PASS  Notify 收到 >= 2 个包   收到 2 个，首个 e9ff25000bfc620004ff7e0000000100
```

注：两侧的 16 B 载荷数值不同是正常的 —— 通知每 500 ms 一发，
宿主侧 `StartNotify` 之后收到的是**它那个时刻**的采样。

## 为什么这是 zblue 的 bug 而不是应用写法问题

上游 Zephyr `subsys/bluetooth/host/gatt.c` 的 `bt_gatt_notify_cb()` 在
`conn == NULL` 时**就地**在已经填好 `data.handle` 的 `struct notify_data`
上做 CCC 遍历：

```c
	data.handle = bt_gatt_attr_get_handle(data.attr);
	...
	data.err = -ENOTCONN;
	data.type = BT_GATT_CCC_NOTIFY;
	data.nfy_params = params;

	bt_gatt_foreach_attr_type(data.handle, 0xffff, BT_UUID_GATT_CCC, NULL,
				  1, notify_cb, &data);
```

zblue 为了多控制器把它抽成了 `gatt_notify_mc(uint16_t handle, …)`，
新函数里 `struct notify_data data = { 0 };` 把句柄丢了
（`gatt_indicate_mc()` 同理）。修复就是两行 `data.handle = handle;`，
语义与上游完全一致。

## 文件

| 文件 | 说明 |
|---|---|
| `before-b3.log` / `before-b3.raw` | 修复前的完整串口原文（含 `[H4] TX` 字节级 trace） |
| `after-b3.log` | 修复后同一 harness 的串口原文 |
| `before-decode.txt` | `pw_h4_notify_decode.py before-b3.log --expect-handle 0x0010` 输出（退出码 1） |
| `after-decode.txt` | 同上，对 `after-b3.log`（退出码 0） |
| `central.log` | 修复后宿主侧 BlueZ 中心设备的 PASS 表 |
| `SHA256SUMS.txt` | 全部文件的 sha256 |

复核方式：

```bash
cd docs/evidence/bt-gatt-handle-20260918
sha256sum -c SHA256SUMS.txt
python3 ../../../tools/phywear/pw_h4_notify_decode.py after-b3.log --expect-handle 0x0010
```
