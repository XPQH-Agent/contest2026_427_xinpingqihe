# 证据：PPP over BLE（任务 3 第三条路线）—— 功能跑通到"pppd 起来"，但**装不进 SRAM**

日期：2026-09-18 深夜
被测：PhyWear 手表（SF32LB52-MOD-1-N16R8）/ 宿主 Intel AX201 + BlueZ

---

## 0. 结论先说

| 问题 | 答案 |
|---|---|
| PPP over BLE **做了吗** | ✅ 两端代码都写了：设备侧 `apps/examples/phywear/pw_btppp.c`（BLE↔pty 字节管道 + 拉起 pppd）、宿主侧 `tools/phywear/pw_bt_ppp.py`（BLE↔PTY 桥 + 给出 pppd 命令） |
| **跑通了吗** | ⚠️ **设备侧跑到"pty 建好 + pppd 启动"就被 SRAM 卡死**，没有走到"手表拿到 IP" |
| 联调到哪一步 | ⚠️ **两端没联调过**：设备侧那次烧录（`device_ppp_started.log`）**宿主一次都没连上**（全程 `conn=0`），所以"主机写 `…a101` / 收 `…a102` 通知"这条链路**只做到代码级，未做端到端实测**；宿主脚本 `pw_bt_ppp.py` 只验过 `--help`/参数解析能跑。**结论按 ❌/⚠️ 记，不写成"已打通"** |
| 卡在哪 | **SRAM**。NuttX 的 pppd 需要 ~16 KB 栈（它自己的参考例子 `apps/examples/pppd` 用 `CONFIG_EXAMPLES_PPPD_STACKSIZE=16096`），加桥任务 2 KB ≈ **18 KB**；而本板的悬崖在 **SRAM 95.98%（能开机）与 96.76%（开机挂死）之间** —— 只有约 4~5 KB 余量 |
| 当前固件状态 | `PW_BT_PPP` **默认 0**（功能未编入），SRAM 回到 **496,452 B（94.69%）**（= 出货固件 `nuttx.bin` 2,557,296 B 那一版，构建输出实测），板子正常开机；`phywear btppp` 会打印"为什么没有" |

## 1. 已经做出来并验证到的部分

设备侧串口原文（`device_ppp_started.log`）：

```
[bt] bt_enable rc=0 (host stack up) elapsed=1228ms
[bt] B2 adv start rc=0 (ok)
[ppp] gatt service register rc=0 (ok)
[ppp] bridge task up (stack 2048 B)
[ppp] pty master fd=4 slave=/dev/pts/0
[ppp] starting pppd on /dev/pts/0
```

也就是：**BLE 服务注册成功 → pty 建成（`/dev/pts/0`）→ pppd 拉起**，这三步都有原文。
再往后板子就不出声了（pppd 在自己的任务里跑，栈不够 → 硬故障 → 看门狗复位）。

## 2. SRAM 实测：悬崖在哪（全部为真机烧录 + 复位实测）

| 配置 | SRAM | 结果 |
|---|---|---|
| 基线（无 PPP） | 496,448 B（94.68%） | ✅ 正常开机，验收 18/18 |
| PPP + pppd 栈 6144 + 桥 2048 + pty 1024×2 | **507,308 B（96.76%）** | ❌ **开机挂死**：只打 `SFBL`/`ABCD`，到不了 NShell |
| PPP + pppd 栈 3072 + 桥 1024 + pty 512×2 | **503,212 B（95.98%）** | ✅ 能开机；但 `phywear btppp` 之后**整机静默**（桥任务栈 1 KB 里放了 512 B 局部数组 → 栈溢出） |
| PPP + pppd 栈 4096 + 桥 2048 + pty 512×2 | ~505 KB | ✅ 能开机，且 pppd **确实被拉起来**；但 pppd 侧栈不足，随即硬故障/复位 |
| **PPP + pppd 栈 16384（参考例子口径）** | **517,548 B（98.71%）** | ❌ **连 `ABCD` 都打不出来**（只有 5 字节 `SFBL`） |
| **出货（PW_BT_PPP=0，5 行 CONFIG 也注释掉）** | **496,452 B（94.69%）** | ✅ 正常开机（flash 2,557,296 B）+ `phywear btppp` 说明原因；一键验收 18/18 |

结论：**余量 ~4~5 KB，需求 ~18 KB ⇒ 差约 13~14 KB。** 这不是"调参"能解决的，
必须从别处腾出十几 KB SRAM（或砍功能）。

## 3. 宿主侧

`tools/phywear/pw_bt_ppp.py`：连手表 → 订阅 `…a102` 通知 → 开一对 PTY →
把 PTY 与两个 GATT 特征对搬；ATT MTU 从 `…a003` 的状态串读真实值（517），
据此分片（`MTU-3`）。跑起来后会打印：

```
[host-ppp] PTY 就绪：/dev/pts/N
[host-ppp] sudo pppd /dev/pts/N 115200 noauth 192.168.7.1:192.168.7.2 local nodetach
```

⚠️ **这段输出是"设计目标"，不是实测记录** —— 本轮只做到"脚本能被 python3 解析并打印 `--help`"，
**没有拿它连过板子**（设备侧那次 boot 宿主全程 `conn=0`，见 `device_ppp_started.log` 末尾状态串）。
换句话说：设备侧 GATT 字节管道**只验到"服务注册 rc=0"**，主机→设备写、设备→主机通知这两个方向
都**没有端到端证据**。要真跑这条链路，得先解决下面的 SRAM 问题。

**第二步必须 root**（pppd 要建网络接口）—— 这一步只能由人来做。

## 4. 顺带查清一个长期被误判的坑：defconfig 里的注释字符

加 `CONFIG_PSEUDOTERM`/`CONFIG_NETUTILS_PPPD` 时，`cmake --build --target resetconfig`
报了这个项目**反复见过**的错：

```
nuttx_export_kconfig_by_value() ... string sub-command REGEX, mode MATCH
needs at least 5 arguments total to command
```

项目记忆里把它归因为"手改 `.config`"。**这次定位到真因**：我在 defconfig 里加了
**含半角空格的中文注释**。最小复现（`cmake -P`，5 个用例）：

| 注释内容 | 结果 |
|---|---|
| `# 中文注释带全角，无空格` | ✅ 正常 |
| `# only ascii spaces here` | ✅ 正常 |
| `# 纯中文注释（全角标点），没有装饰性符号` | ✅ 正常 |
| `# 中文 注释 带 半角空格` | ❌ 触发那个 REGEX 错误 |
| `# ── 框线 ── 箭头 ↔ ⇒ 破折号 ——` | ❌ 触发那个 REGEX 错误 |

⇒ **defconfig 里含非 ASCII 的注释行中不要出现半角空格，也别用框线/箭头类符号。**
（文件里原有的 14 行纯中文注释一直没出事，符合这条规律。）

## 5. 复核

```bash
cd docs/evidence/bt-ppp-20260918 && sha256sum -c SHA256SUMS.txt

# 复现 SRAM 悬崖（改 pw_btppp.c 的 PW_BT_PPP 与栈大小后重编烧录）
cmake --build cmake_out/sf32lb52_lchspi_ulp_nsh_ai -j16
./flash_with_ftab.sh cmake_out/sf32lb52_lchspi_ulp_nsh_ai/nuttx.bin /dev/ttyUSB0
# 出货态应看到：[ppp] 本固件未编入 PPP over BLE（PW_BT_PPP=0）

# 最小复现那个 cmake 报错
for f in plain ascii spaces nospace symbols; do
  printf "%-8s " $f
  cat > /tmp/one.cmake <<EOF
include("\$HOME/openvela/nuttx/cmake/nuttx_kconfig.cmake")
nuttx_export_kconfig_by_value("/tmp/kt/\${f}" "CONFIG_TEST")
EOF
  cmake -P /tmp/one.cmake >/dev/null 2>&1 && echo OK || echo "触发 REGEX 错误"
done
```

## 6. 要真跑起来，缺什么（给决策用）

需要腾出 **约 13~14 KB** SRAM。候选（都还没做，需要你点头）：

| 候选 | 预计能省 | 代价 |
|---|---|---|
| 把 pppd 栈从 16 KB 往下试（4096 已能启动、握手时崩） | 逐步试，最多 ~10 KB | 栈不够就是硬故障，需要慢慢逼近；**不能保证够** |
| `CONFIG_NET_TUN_PKTSIZE` 296 → 降到 128 | ~几百 B | 影响 PPP 侧 MTU |
| 砍/缩 `phywear` 里的大块静态缓冲（scope/曲线/字体缓存等） | 可观，但要逐项找 | 影响功能或帧率 |
| 关掉某个大功能（例如 demos / lvbench 相关） | 可能上 KB | 丢功能 |
| 换更小的 PPP 实现（自己写最小 IPCP？） | — | 工作量以天计，不划算 |

**我的建议**：先别急着砍功能。任务 3 已经有"BLE 承载测量数据到宿主"这条**已实测通过**的
路径（B3 的 Notify），N1 的"手表自己拿到 IP"属于加分项；把 SRAM 留给已交付的 UI/算法更划算。
如果你确实想要 IP，我建议先做"pppd 栈逐步下探"这一个实验（成本低、能立刻知道下限）。
