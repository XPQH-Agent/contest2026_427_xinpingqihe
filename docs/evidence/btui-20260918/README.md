# 证据：蓝牙连接标识 + 手表版蓝牙串口（2026-09-18 晚）

固件：`nuttx.bin` **2,554,544 B**（flash 15.23%）/ SRAM **496,372 B（94.68%）**
板子：立创黄山派 SF32LB52-MOD-1-N16R8　宿主：Intel AX201（`hci0`，`04:EC:D8:F3:73:8D`）

## 做了什么

| 项 | 内容 |
|---|---|
| **主界面标识** | 标题右侧一枚圆点 + 「蓝牙」：**未连接=灰**（`0x5B6875`）、**已连接=蓝**（`0x3B82F6`，带一点静态外发光）。灰→蓝由 250 ms 的 LVGL 定时器轮询 `pw_bt_link()` 自动切换，点一下直接进蓝牙页 |
| **GATT 文本特征** | 新增 `e0f1a003-1b2c-4d5e-8f90-a1b2c3d4e5f6`（READ \| WRITE \| WRITE_NR \| NOTIFY + CCC）。手机写自由文本 → 设备记 `[RX]` 并**回一条 `echo: …`**；手表「发送测试」按钮 → notify 文本给手机（`[TX]`） |
| **手表版蓝牙串口页** | 状态（未连接/已连接/已连接·已订阅）+ 对端地址 + `RX/TX/echo/MTU` 计数 + **10 行 TX/RX 滚动日志** + 「发送测试」「清空」。打开页面时若 host 栈未起会自己把它起起来（`pw_bt_init()` 幂等） |
| 入口 | 主页那枚点即入口（工具板块列表已 5 行占满内容区，第 6 行要滚动，故不塞那里） |

## 真机截图（`pwshot.py`，与宿主的 BLE 中心设备同时跑）

| 文件 | 内容 |
|---|---|
| `root_grey.png` | 主页，**未连接**：灰点 + 灰字「蓝牙」 |
| `root_connected.png` | 主页，**已连接**：蓝点 + 蓝字「蓝牙」（`shot bthome`） |
| `btlink_connected.png` | 蓝牙页，**已连接**：`已连接`（蓝）、`对端 04:EC:D8:F3:73:8D (public)`、`RX 2 · TX 1 · echo 1 · MTU 23`，日志里 `[RX] hello-1789723972` → `[TX] echo: hello-1789723972` |
| `btlink_idle.png` | 蓝牙页，未连接：`未连接`（灰）、`广播中，等待手机/电脑连接`、`RX 1 · TX 0 · echo 0 · MTU 0`（那 1 条 RX 是设备侧自检写的 `selftest`） |

## 空口连通性测试（宿主 `pw_ble_central.py --text-test`，13/13 PASS）

```
PASS  找到 text 特征 …a003
PASS  读 …a003 得到状态串      PhyWear bt conn=1 sub=0 rx=1 tx=0 echo=0 mtu=23
PASS  写文本后收到设备 echo（双向连通）  写 'hello-1789723972' → 收到 ['echo: hello-1789723972']
PASS  echo 内容与所写一致      echo: hello-1789723972
```

为什么判据是"**收到 echo**"而不是"写成功"：BlueZ 的 `WriteValue` 返回成功只说明
本地协议栈把包发出去了 —— 空口丢没丢、设备收没收到、设备能不能主动发回来，
它一概不知道。收到原样 echo 才同时证明「手机→手表」与「手表→手机」两个方向。

设备侧对应的串口原文（`docs/evidence/bt-b3-20260918-btui/b3.log`）：

```
[bt] B3 connected: 04:EC:D8:F3:73:8D (public) err=0
[bt] link up mtu=23
[bt] text ccc -> notify ON
[bt] msg in #2 (15 B): 'hello-1789723972'
[bt] echo tx rc=0: 'echo: hello-1789723972'
```

## 回归（同一固件，一键验收 18/18）

| 项 | 值 |
|---|---|
| 设备侧验收 | **18/18 全过**（`docs/evidence/accept-20260918-btui/`，172 s） |
| fps（30 s 同口径心跳） | **12 不变**（loops 200，基线 198~200） |
| 主页截图 | `e356689dc9e32387…`（= 新金标；**有意改版**：多了那枚状态点） |
| SRAM | **496,372 B（94.68%）**，+684 B（环形日志 10×36 + 链路状态 + `k_work`） |
| flash | **2,554,544 B（15.23%）** —— 比上一版（2,609,752 B）**少 55,208 B**（见下） |

## 顺手修掉的一个字体流水线 bug（否则这一步根本走不通）

加了中文文案后按项目规矩重跑 `tools/phywear/gen_fonts.sh`，结果**整个字体生成失败**：

```
Font "…/DroidSansFallbackFull.ttf" doesn't have any characters included
in range 0xfe0f-0xfe0f
```

根因在 `gen_font_ranges.py`：它把所有"码点 ≥ 0x2E80"的字面量字符都当成要渲染的字，
而 **U+FE0F（变体选择符-16）** 恰好落在这个区间。它只修饰前一个字形的呈现、
自己没有独立字形，DroidSansFallback 里当然没有 ⇒ `lv_font_conv` 直接退出 1，
**五个字号一个都不生成**。触发条件低到离谱：源码/内嵌文档里出现一个 `⚠️`
（= U+26A0 + U+FE0F）就够了 —— 本项目的 `pw_skill_blob.c`（自动生成，内嵌
skill markdown 的十六进制转义）里就有一个。

同时修掉第二处：该脚本把 `*_blob.c` / `*_test_host.c` 也当 UI 源码扫。
`pw_skill_blob.c` 是一篇 markdown 文档的数据块，**从不参与渲染**（它被写进
`/data/agent/skills/*.md` 喂给模型），却贡献了 565 个汉字 —— 占全部 725 个的 78%。

| | 采集到的字 | pw_font_14.c | 5 个字号合计（源文件） |
|---|---|---|---|
| 修前（含 blob + 变体选择符） | 725 | 431,344 B | 3,995,314 B |
| 修后（排除后） | **508** | **390,801 B** | **3,610,177 B** |

所以本版虽然加了整个功能，**flash 反而少了 55 KB**。新文案用到的字一个不少
（`蓝牙`/`对端`/`未连接`/`已连接`/`收发日志`/`发送测试`/`清空`/`广播中，等待手机/电脑连接`
全部真机渲染正常，截图可见）。

## 复核

```bash
cd docs/evidence/btui-20260918 && sha256sum -c SHA256SUMS.txt

# 设备侧 18 项
python3 tools/phywear/pw_accept.py --out <dir>

# 双向连通性（设备侧 8 项 + 宿主侧 13 项合判）
python3 tools/phywear/pw_bt_b3.py --selftest          # 判定器 8/8（含文本串口正反例）
python3 tools/phywear/pw_bt_b3.py --central --text --out <dir>
```
