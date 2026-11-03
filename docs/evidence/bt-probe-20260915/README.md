# ④ 蓝牙探针（时间盒实验，2026-09-15）—— 结论：**暂不可交付（no-go），但差的是"构建接线"不是硬件**

> 用户问过"蓝牙是否可以提上日程"。本目录是**时间盒探针**的结论：**能不能起来**、
> 差在哪、要多少代价 —— 不是交付。全部结论都有命令与文件位置可复核。

---

## 1. 结论速览

| 结论 | 证据 |
|---|---|
| **片上蓝牙控制器是有的**（LCPU），不需要外挂固件 | `vendor/sifli/chips/sf32lb52/sf32lb52_bth4.c` 里 `lcpu_power_on()`、`sf32lb52_hci_register_callback()`；且 `vendor/sifli` 下**没有任何** BT controller 固件 `.bin`（本来也不需要） |
| **BT 主机栈在配置里是打开的** | `.config` 有 `CONFIG_BT_H4=y`、`CONFIG_BT_HCI_HOST=y`、`CONFIG_BT_CLASSIC=y`、`CONFIG_BT_EXT_ADV=y`、`CONFIG_BT_UART_ON_DEV_NAME="/dev/ttyHCI0"`，共 **107** 个 `CONFIG_BT*/BLUETOOTH*` 项 |
| **但固件里几乎没有 BT 代码** | 基线固件 `nm nuttx \| grep -ci "bt_\|bluetooth"` = **2** |
| **根因 1：`UART_BTH4` 没开** | defconfig 里 `# CONFIG_UART_BTH4 is not set`；板级 `sf32lb52_bt_initialize()` 正是 `#ifdef CONFIG_UART_BTH4` 才被调用 → `/dev/ttyHCI0` 从不注册 |
| **根因 2：NuttX 的 BT socket 层被 gated 掉** | `net/bluetooth/CMakeLists.txt` 全部源码在 `if(CONFIG_NET_BLUETOOTH)` 内，而该符号（及其依赖 `WIRELESS_BLUETOOTH`）未开 → **一个 BT 目标文件都没编**。⚠️ 2026-09-15 复核更正：**走 zblue 路径并不需要这两个**——`UART_BTH4` 只依赖 `DRIVERS_BLUETOOTH`（`nuttx/drivers/serial/Kconfig:892`），数据经 `uart_bth4_register` 的 `drv->receive` 进 circbuf 被 zblue `h4.c` 读走，不经过 `net/bluetooth`；`NET_BLUETOOTH` 只在用 NuttX 原生 socket 栈时才需要 |
| **根因 3（决定性）：端口自己的 H4 驱动没有任何 CMakeLists 编它** | 驱动源码在 `external/zblue/zblue/port/drivers/bluetooth/hci/h4.c`（`h4.c:48` `DT_DRV_COMPAT zephyr_bt_hci_ttyHCI`，其 `DEVICE_DT_DEFINE` 正好产出 `defines.c` 缺的 `__init___device_dts_ord_..._ttyHCI0/1_ORD`），但 `port/drivers/bluetooth/hci/` **目录下没有 CMakeLists.txt**；而 `port/sections/defines.c:543` 已经声明了 `__init___device_dts_ord_..._zephyr_bt_hci_ttyHCI0/1_ORD` → **声明了没人实现，必然链接失败** |

**探针实测**：把 `CONFIG_UART_BTH4=y` 加进 defconfig 后，设备树会生成 `zephyr_bt_hci_ttyHCI0/ttyHCI1` 节点，但链接在最后一步报：

```
apps/external/zblue/libzblue.a(defines.c.o): undefined reference to
  `__init___device_dts_ord_DT_N_INST_0_zephyr_bt_hci_ttyHCI0_ORD'
```

> 注：`external/zblue/zblue/drivers/bluetooth/hci/h4.c`（上游那份，`DT_DRV_COMPAT zephyr_bt_hci_uart`）
> 虽然存在，但它**不匹配**本端口的设备树节点（节点是 `zephyr_bt_hci_ttyHCI`）；我曾经把这一份
> 加进 CMake 试过，仍然缺符号（因为匹配不上），已回退。**真正该编的是 `port/drivers/bluetooth/hci/h4.c`**（2026-09-15 复核：该文件有 `DT_DRV_COMPAT zephyr_bt_hci_ttyHCI`，而 `h4_uart.c` **没有** `DT_DRV_COMPAT`，它是旧的 `CONFIG_BT_H4_ENABLE` 独立 app，还引用了全仓 Kconfig 里不存在的 `CONFIG_BT_UART_H4_*`，编它会额外报错）。

---

## 2. 探针的边界（我做了什么、没做什么）

**做了**（都在工作区，**已全部回退并验证复原**）：
1. 记录基线固件（`md5 9ed0d480…`，2,076,244 B）；
2. 在 `nsh-ai` defconfig 追加 `CONFIG_WIRELESS_BLUETOOTH=y / CONFIG_NET_BLUETOOTH=y / CONFIG_UART_BTH4=y`（后两者事后证明对 zblue 路径并非必需，见上表根因 2 的更正）；
3. `rm .config` + 重新 configure + 全量构建 → 得到上表的链接失败；
4. 试补 `h4.c` 到上游 CMake → 仍缺符号 → 回退；
5. 回退 defconfig 与 CMake，重新 configure + 构建，**用 md5 验证固件回到基线**。

**没做**（超出时间盒，需要用户先定）：
- 给 `port/drivers/bluetooth/hci/` 写 CMakeLists、打开 `CONFIG_BT_UART_H4_*`、处理 zblue 的
  `net_buf` 静态池内存（见下）、真机跑 HCI 命令、写 GATT 服务；
- 因此**没有任何"蓝牙能用"的结论**，也没有证明 LCPU 控制器一定会应答。

---

## 3. 如果要做，代价与风险（估）

| 项 | 说明 |
|---|---|
| 工作量 | **0.5~1.5 天**到"能在手表上跑通 HCI reset/scan"；再做 GATT 服务与手机侧对接另算 |
| SRAM（**最大风险**） | zblue 的 `net_buf` 池是**静态数组**（`NET_BUF_POOL_DEFINE`），没有 PSRAM 支持。本板 SRAM 已用 **93.7%**（探针前累计已超"不超过 ~2 KB"额度），开 BT 极可能直接**链接失败**（SRAM 100% 会链接报错，`.config` 里 `CONFIG_BT_*_TX_STACK_SIZE`、RX/TX 线程栈都要吃 SRAM） |
| 已知坑 | ① 该 defconfig 由 CMake 列表解析，**行内注释里的引号/全角括号会让 configure 直接报 `string sub-command REGEX ... needs at least 5 arguments`**（本次踩到，去掉注释即恢复）；② `CONFIG_BT_DEVICE_NAME="Zephyr"`，手机扫描会看到 "Zephyr"，要改；③ `CONFIG_BT_CLASSIC=y`（BR/EDR）对穿戴设备基本无用，先关掉省资源 |
| 是否需要外挂控制器固件 | **不需要**（控制器在片内 LCPU），但**未经验证**：探针没走到"发一条 HCI 命令看回答"这一步 |

---

## 3.5 补充：**"手机蓝牙共享网络"这条路在手表侧接不住**（2026-09-15 复核）

用户问过"手机做网关不就是打开蓝牙共享网络吗"。**手机侧确实零开发**（系统设置里打开即可），
但**卡点在手表侧**：

| 事实 | 证据 |
|---|---|
| 它是**经典蓝牙 BR/EDR 的 PAN/NAP**，数据面 = BNEP over L2CAP PSM `0x000F`，之上才跑 IP | 标准 |
| **zblue 完全没有 PAN/BNEP** | 全树只有 2 个 UUID 常量（`external/zblue/zblue/include/zephyr/bluetooth/uuid.h:5247-5248`、`.../classic/sdp.h:60`），`subsys/bluetooth/` 下无 bnep/pan/ipsp 实现 |
| **NuttX 原生栈也没有** | `nuttx/wireless/bluetooth/` 无 bnep 文件；只有预留管道 `netdev_register.c:59`（`"bnep%d"`）与 `include/netpacket/bluetooth.h:88,228`（`BTPROTO_BNEP` / `BT_PSM_BNEP=0x000f`） |
| openvela 框架有 PAN 服务，但**在 zblue 上编不起来** | `frameworks/connectivity/bluetooth/service/profiles/pan/panu_service.c` 依赖的 `sal_pan_interface.h` **全仓不存在**（zblue 的 SAL 没有 PAN 适配；那套是给 Bluelet 栈的） |
| Bluelet 无源码；Fluoride 有完整 BNEP 但只当独立 app | `external/bluelet/` 只有构建文件；`external/fluoride/.../stack/bnep/` 无板级接入 |

→ 手表要接住 = **从零写 PANU + BNEP + netdev/IP 绑定，估 5~15 人日**，比走自己的 **BLE GATT**（1.5~3 人日）
大一个量级；且 **iOS 的"个人热点"经蓝牙基本只服务 Apple 设备**（自制设备通常连不上，未验证）。

**路线排序（若目标只是"真机能上网/在线 LLM"）**：① 手机 App 当 GATT↔云网关（手表只用现成 GATT，+2~4 人日）→
② USB + SLIP（`docs/09` 已规划）→ ③ 蓝牙共享网络（+5~15 人日，**不建议在截止前做**）。

## 4. 给用户的建议

**建议：不在本轮排期蓝牙。** 理由：
1. 它卡在**构建接线 + SRAM**两道关，不是"配置一开就有"；
2. 当前 SRAM 已 93.7% 且**已超用户给的 ~2 KB 新增额度**，BT 的静态 `net_buf` 池大概率直接放不下；
3. 赛道评分里"主动 + 执行"（⑤-1 已交付）与"演示视频"的权重明显更高，而 ③ 的 UI 也还没落地。

**要重启它需要用户先定两件事**：① SRAM 额度是否放宽（或接受关掉某些现有功能换 BT）；
② 是否接受 0.5~1.5 天投入只为"能跑 HCI"（不含 GATT 业务）。

复现探针（不要在生产固件上做）：

```bash
cd ~/openvela
cp vendor/sifli/boards/sf32lb52/sf32lb52_lchspi_ulp/configs/nsh-ai/defconfig /tmp/defconfig.bak
printf 'CONFIG_WIRELESS_BLUETOOTH=y\nCONFIG_NET_BLUETOOTH=y\nCONFIG_UART_BTH4=y\n' >> \
  vendor/sifli/boards/sf32lb52/sf32lb52_lchspi_ulp/configs/nsh-ai/defconfig
rm -f cmake_out/sf32lb52_lchspi_ulp_nsh_ai/.config
cmake -B cmake_out/sf32lb52_lchspi_ulp_nsh_ai -S nuttx -GNinja \
  -DBOARD_CONFIG=../vendor/sifli/boards/sf32lb52/sf32lb52_lchspi_ulp/configs/nsh-ai \
  -DEXTRA_FLAGS="-Wno-cpp -Wno-deprecated-declarations"
cmake --build cmake_out/sf32lb52_lchspi_ulp_nsh_ai -j16 2>&1 | tail -20   # 预期：ttyHCI0 init 未定义
# 复原：cp /tmp/defconfig.bak <defconfig>; rm .config; 重新 configure + build；核对 md5
```
