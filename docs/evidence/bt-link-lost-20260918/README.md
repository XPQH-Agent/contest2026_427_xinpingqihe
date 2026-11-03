# 证据：用户报"断开很久了还显示已连接" —— 根因与修复（2026-09-18 晚）

固件：`nuttx.bin`（本轮修复后）/ SRAM 496,412 B（94.68%）
用户原话："手表端的，链接蓝牙之后，即使蓝牙断开很久了，还是显示已连接。"

## 结论先说（两件事，一件是我的锅，一件是真 bug）

### ① 主因：**是桌面测试工具的锅，手表一直是对的**

桌面上那个「PhyWear 蓝牙测试」用了 `pw_ble_central.py`，而它**测完从不断开**：
BlueZ 是中心设备，**进程退出并不会断开 LE 链路**。所以链路一直挂着，
手表（外设侧）的 `connected` 就一直是真 —— 显示"已连接"**完全正确**。

实测取到的现场：

```
17:58 那次桌面测试跑完
18:0x  BlueZ 仍报 /org/bluez/hci0/dev_5A_58_62_96_E7_57
      Connected=True  ServicesResolved=True      ← 链路真的还在
```

用户把"关掉测试窗口"理解成"蓝牙断开了"，而手表看到的是一条真实存在的链路。
**这类"设备显示得和用户预期不一致"的 bug，先怀疑测试工具，别先怀疑固件。**

**修法**：`run_full_test()` 收尾显式 `Device1.Disconnect()`，并把它作为一条判定项：

```
PASS  测完主动断开（手表回到可被发现）
```

CLI 加了 `--keep-connected`，GUI 加了「测完保持连接」勾选框（要连续观察手表状态时才勾）。

### ② 顺带查出的真 bug：**断开后手表再也不会广播（搜不到、连不上）**

查这件事时做了空口实测，结果推翻了我在 §11.2h 里"读源码得出"的结论：

```
① Device1.Disconnect() 成功，BlueZ 侧 Connected=False
② 清掉 BlueZ 缓存对象后扫 15 s → 扫到 13 个别的设备，**没有 PhyWear**
③ 设备侧串口原文：直接 bt_le_adv_start() 回 **-114 = -EALREADY**
```

根因：链路建立时**控制器自己**把 legacy 广播停了，但 host 侧的
`BT_ADV_ENABLED` 标志位**没被清掉**；`adv.c` 在 `BT_ADV_ENABLED` 置位时
直接 `return -EALREADY`。而内置的 `bt_le_adv_resume()` 前置条件是
`BT_ADV_PERSIST && !BT_ADV_ENABLED` —— 第二个条件永远为假，
**所以 resume 永远不会生效，断开后设备就再也搜不到了**（要复位板子才能重连）。

**修法**：`pw_disconnected()` 里投一个工作项，遇到 `-EALREADY` 先
`bt_le_adv_stop()` 清标志位再 `bt_le_adv_start()`，两个 rc 都打日志。

## 修复后的实测（设备侧串口原文，`b3.log`）

```
   19.159  [bt] B3 connected: 04:EC:D8:F3:73:8D (public) err=0
   19.159  [bt] link up mtu=23
   22.570  [bt] echo tx rc=0: 'echo: hello-1789726262'
   25.379  [bt] B3 disconnected: 04:EC:D8:F3:73:8D (public) reason=0x13
   25.379  [bt] adv busy(EALREADY), stop rc=0, retry start
   25.379  [bt] adv restart rc=0 (ok)          ← 修好了
```

`reason=0x13` = remote user terminated ⇒ **断连回调本来就会触发**，
手表端的"状态翻转"这条路是通的（问题从来不在它）。

## 三张真机证据（`root_after_disconnect.png` / `btlink_after_disconnect.png`）

| 文件 | 内容 |
|---|---|
| `root_after_disconnect.png` | 主页，**经历了一次真实"连上→断开"之后**：灰点 + 灰字「蓝牙」。sha256 `e356689d…` 与**从未连接过的金标截图逐字节相同** |
| `btlink_after_disconnect.png` | 蓝牙页，同一时刻：`未连接`(灰) / `广播中，等待手机/电脑连接` / `RX 2 · TX 1 · echo 1 · **MTU 0**`；日志尾部 `[--] disconnected` → `[--] advertising again` |
| `rescan_after_disconnect.txt` | 断开后重新扫描：BlueZ `Connected=False`，清缓存后**能重新扫到 PhyWear** ✅ ⇒ 可以再连，不用复位板子 |

两张截图分别验的是**两条不同的刷新路径**：主页那枚点走 `ui_bt_tick`（250 ms），
蓝牙页状态走 `btp_tick`（300 ms）—— 都得单独证一遍，不能互相代替。

## 为什么截图要专门加两个取证入口

`phywear shot <page>` 是"开机后第一个 GUI"，拍完就 `break` 退出、**BT 栈随即随进程消失**
（实测：`shot` 跑完板子回到 `nsh>`，此时不会广播）。所以没法"先连一次、再重启、再拍照"。
于是加了两个只在 shot 模式下多等一拍的入口：

| 入口 | 语义 |
|---|---|
| `btafter` | 主页 + 起蓝牙 → **等连上 → 再等断开** → 静默 4 s → 出图 |
| `btlog` | 同上，但拍的是蓝牙页 |

（`btlink` / `bthome` 维持原语义：等连上 → 静默 → 出图。）

## 复核

```bash
cd docs/evidence/bt-link-lost-20260918 && sha256sum -c SHA256SUMS.txt

# 断开链路（设备侧会打 disconnected / adv busy / adv restart 三行）
python3 tools/phywear/pw_ble_central.py --timeout 30 --text-test

# 断连后是否回到可被发现（要在 phywear 还活着时跑；shot 跑完进程已退出、BT 都没了）
python3 tools/phywear/pw_bt_b3.py --central --text --out <dir>
```

## 附：本轮我自己犯的一个推断错误（记下来）

我曾用"`cmake_out/.../.config` 里 `CONFIG_BT_ADV_PERSIST` 计数为 0"当作"持久广播没开"，
差点据此把文档改成"文档写错了"。实际上 **zblue 里根本没有这个 Kconfig 符号**
（`BT_ADV_PERSIST` 只是 `hci_core.h` 里的一个运行时标志位）。
**`grep` 不到一个符号，只能说明"这个符号不在配置里"，不能推断"这个特性没编译进去"。**
真正的订正在 `docs/16 §11.2h`：结论是被**空口实测**推翻的，不是被 grep 推翻的。
