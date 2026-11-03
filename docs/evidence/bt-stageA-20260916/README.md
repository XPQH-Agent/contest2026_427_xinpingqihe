# 蓝牙阶段 A 证据包 — 2026-09-16

**问题（可证伪）**：黄山派 SF32LB52 的**片内 LCPU 蓝牙控制器**，在 openvela 这一侧到底能不能通过
HCI 说上话？（此前结论是"蓝牙 no-go"，但那是基于"端口从未注册"的间接推断。）

**结论：能。** 传输层打通，控制器对 HCI_Reset 正常回 Command Complete。

## 1. 固件与代价

| 项 | 值 |
|---|---|
| 固件 | `2,204,456 B`，md5 `2e00ecf22243b0bc942c3933f77f6660` |
| 占用 | flash **13.14%**（基线 12.63%，**+84,760 B**）、SRAM **487,460 B / 512 KB（92.98%）**（基线 446,704 / 85.20%，**+40,756 B**）、PSRAM 不变 |
| 读回校验 | off=0 与末块逐字节一致 |
| 源码改动（3 处） | ① 板级 defconfig：`# CONFIG_UART_BTH4 is not set` → `CONFIG_UART_BTH4=y`；② `external/zblue/CMakeLists.default.txt`：把 `h4.c` 加入构建（default 分支漏了，3_0_1 分支本来就有）；③ 新增 `.../hci/bt_snoop_stub.c`（`btsnoop_log_capture` 空实现——它唯一的实现在被 `CONFIG_BLUETOOTH` 关掉的 framework 里，不加就 undefined reference） |

## 2. 判据与实测（`probe-device.txt` 原文）

```
/dev: ... ttyHCI0 ...                      ← 判据①：端口注册成功
[BTHCI] /dev/ttyHCI0 opened
[BTHCI] write HCI_Reset -> 4
[BTHCI] tx (4 B): 01 03 0c 00
[BTHCI] rx (7 B): 04 0e 04 06 03 0c 00
[BTHCI] 判据③：HCI 链路通（Command Complete, opcode=0x0c03, status=0）
```

`04 0E 04 06 03 0C 00` = HCI Event / Command Complete / plen=4 / 剩余命令数=6 /
opcode=`0x0C03`（HCI_Reset）/ status=**0（成功）**。这说明 LCPU 侧控制器活着且应答正常，
不是"端口在但对端是空的"。

复现命令（真机一个 boot 内，**GUI 只开一次**那条铁律对 CLI 子命令不适用，`phywear bthci` 不开 GUI）：
```bash
phywear bthci        # 设中文并用它可以这样：phywear lang zh bthci
```

## 3. 顺带查清的**阶段 B 阻塞点**（如实记录，不含糊）

要在这块板上跑 **zblue host 栈**（`bt_enable()`/GATT），当前**编不过**，两条实测路径：

| 试法 | 结果 |
|---|---|
| `CONFIG_BT_SHELL=y` | **CMake configure 直接失败**：`select SHELL` 在本端口 Kconfig 树里没有对应符号 → `target_link_libraries(zblue ...)` 报 "target zblue is not built" |
| `CONFIG_BT_SAMPLE=y + CONFIG_BT_SAMPLE_PERIPHERAL=y` | configure 过，**编译失败**：`host/keys.h:245: 'CONFIG_BT_MAX_PAIRED' undeclared`、`host/id.c:1673: 'struct bt_dev' has no member named 'irk'` —— 隐私/irk 与配对池相关 Kconfig 在 `ZEPHYR_REPO_DEFAULT` 分支未接 |

所以阶段 A 的探针刻意**绕开 host 栈**，直接对 `/dev/ttyHCI0` 收发 HCI —— 层层隔离，失败也知道断在哪。
阶段 B 若要做，第一件事就是补这两处 Kconfig 缺口（并重新核算 SRAM：host 栈还要几十 KB，
本次已把 `CONFIG_SYSTEM_WORKQUEUE_STACK_SIZE` 从 32768 降到 4096 腾出 28 KB，
**4 KB 对真正的 BT host 工作队列可能偏小，需重新评估**）。

## 4. 本证据包**不**主张什么

- **不主张"手表能上网/能用 BLE 应用"**：只证明了片内控制器与 HCI 传输通；
  host 栈、GATT、与手机的互联都还没跑起来（阶段 B）。
- 不主张 `phywear bthci` 之外的功能；这条探针只发一条 HCI_Reset，不做扫描/连接。
