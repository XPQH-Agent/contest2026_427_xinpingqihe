# 评委复现指南（PhyWear · contest2026_427_xinpingqihe）

> **先说结论（如实）**：本仓**不能单独编译**。它是一个**作品仓**——放着本队原创的应用代码（`app/phywear/`）、
> 对 openvela 公共仓的**改动快照**（`src/`）、板级配置（`board/`）、文档与证据（`docs/`）、AI 编码日志（`logs/`）。
> 要编译运行，需要先有一个 **openvela 基线工作区**（多仓检出），再把本仓的内容落到工作区里。
> **本页给出三条路径**，按"想花多少时间"挑一条即可；完整细节见 [`docs/07_构建烧录与复现指南.md`](docs/07_构建烧录与复现指南.md)。

---

## 路径 A：只看代码与文档（约 5 分钟，不用编译）

| 先看 | 为什么 |
|---|---|
| [`docs/01_项目描述_如实版.md`](docs/01_项目描述_如实版.md) | 逐条"有就是有、没有就是没有"的事实清单（含 **未实现项**） |
| [`app/phywear/`](app/phywear) | **本队原创**作品源码（LVGL 手表应用，约 1.4 万行 C） |
| [`src/MANIFEST.md`](src/MANIFEST.md) | 公共仓改动**逐文件来源 + 归属**（哪些是官方 PR、哪些是本队原创） |
| [`docs/03_技术报告.md`](docs/03_技术报告.md) | 架构 + **10 个关键问题的"现象→定位→修复→验证"** |
| [`docs/evidence/`](docs/evidence) | 真机截图、声学/技能/主动场景/LLM 联调证据 |
| [`logs/XPQHyue/`](logs/XPQHyue) | AI Coding 日志（官方格式，`validate-log.py` 校验通过） |

---

## 路径 B：真机复现（推荐；有板子，约 1–2 小时）

**前置**：一块立创·黄山派（SF32LB52-MOD-1-N16R8）、CH340N USB 串口线、Linux 主机。

```bash
# ① 取 openvela 基线工作区（官方 manifest，分支 dev-ai-contest-2026）
mkdir -p ~/openvela && cd ~/openvela
repo init -u https://gitee.com/open-vela/manifests.git -b dev-ai-contest-2026 -m openvela.xml
#   本队工作区实际用的是 SSH：ssh://git@gitee.com/open-vela/manifests.git（两种都可）
repo sync -c -j8

# ② 把本仓的改动落到工作区（脚本在本仓内，先演练后执行；--all 含官方基线快照）
cd <本仓路径>
python3 .claude/skills/phywear-reproduce/check_progress.py     # 看当前进度（应为大量 ❌，属正常）
python3 .claude/skills/phywear-reproduce/restore_code.py       # 演练：只打印要写哪些文件
python3 .claude/skills/phywear-reproduce/restore_code.py --execute --all
python3 .claude/skills/phywear-reproduce/check_progress.py --brief   # 复检

# ③ 编译真机固件（AI Agent + PhyWear + 声学）
cd ~/openvela
export PATH="$PWD/prebuilts/tools/linux/x86_64:$PWD/prebuilts/gcc/linux-x86_64/aarch64-none-elf/bin:$PWD/vendor/artinchip/tools/scripts:$PATH"
rm -f cmake_out/sf32lb52_lchspi_ulp_nsh_ai/.config
cmake -B cmake_out/sf32lb52_lchspi_ulp_nsh_ai -S nuttx -GNinja \
  -DBOARD_CONFIG=../vendor/sifli/boards/sf32lb52/sf32lb52_lchspi_ulp/configs/nsh-ai \
  -DEXTRA_FLAGS="-Wno-cpp -Wno-deprecated-declarations"
cmake --build cmake_out/sf32lb52_lchspi_ulp_nsh_ai -j16

# ④ 烧录（本仓 board/ 里就有脚本与 ftab，不依赖本队电脑路径）
cd <本仓路径> && ./board/flash_with_ftab.sh ~/openvela/cmake_out/sf32lb52_lchspi_ulp_nsh_ai/nuttx.bin /dev/ttyUSB0

# ⑤ 串口验收
picocom -b 1000000 --noreset --lower-rts --lower-dtr /dev/ttyUSB0
```

串口里输入：

```text
nsh> ai_agent &          # 起 AI Agent（真机无互联网出口，走端侧离线意图 + 工具）
nsh> phywear lang zh     # 起中文界面（GUI 会占住控制台，之后用触摸操作）
```

**5 分钟验收清单**：主菜单 8 个板块有图标 → 进「AI 教练」显示"已就绪" → 点「有哪些实验」出现
`AI: list: 20 screens` → 点「打开单摆」**手表自己跳到单摆页** → 进「声学 → 音频发生器」能出声 →
「原始传感器 → 麦克风」说话柱状图跟着动。

> ⚠️ 串口铁律：`/dev/ttyUSB0` **独占**；`picocom` 必须带 `--noreset --lower-rts --lower-dtr`
> （CH340N 的 RTS 直连 SoC 复位）；**一个 boot 只能跑一个 `phywear` GUI**（第二个会挂在 LCD 驱动里）；
> 打开串口即复位一次，属正常现象。**烧录前先关掉 picocom**，否则 sftool 抢不到串口。

---

## 路径 C：模拟器跑起来（没有板子也能看界面；约 30 分钟）

同路径 B 的 ①②，然后把目标换成模拟器配置：

```bash
cd ~/openvela
cmake -B cmake_out/vela_goldfish-arm64-v8a-ap-phywear -S nuttx -GNinja \
  -DBOARD_CONFIG=../vendor/openvela/boards/vela/configs/goldfish-arm64-v8a-ap-phywear/ \
  -DEXTRA_FLAGS="-Wno-error -Wno-cpp -Wno-deprecated-declarations"
cmake --build cmake_out/vela_goldfish-arm64-v8a-ap-phywear -j16
./emulator.sh cmake_out/vela_goldfish-arm64-v8a-ap-phywear          # 需要图形界面时；也可加 -no-window
```

模拟器里 `phywear cap root|raw|incline|tone|mic|ai ...` 可逐页打开。**模拟器 IMU 是合成波形，不可当测量结果。**

---

## 已知限制与"做不到"的地方（如实）

1. **本仓不能单独编译**：改动分散在 `apps / nuttx / vendor_sifli / vendor_openvela / packages_ai_agent`
   五个子仓里，必须叠在 openvela 基线上（这正是路径 B 第 ② 步的作用）。
2. **团队 manifest 的局限**：本仓根目录的 `contest2026_427_xinpingqihe.xml` 只把 `app/phywear`
   链接进工作区，**不含** `src/**` 的公共仓改动 —— 所以仍要跑第 ② 步的恢复脚本（它已覆盖全部 169 个文件）。
3. **真机没有网络栈 —— 且本板没有无线网卡**（2026-09-18 实测判定）：真机上 LLM 对话不可用，
   走端侧离线意图 + 工具；**端云 LLM 对话在模拟器演示**（小米 MiMo Token Plan；
   实测记录见 `docs/evidence/llm-20260913/`）。
   关于"用 `set_wifi` 连 WiFi 拿 DHCP IP"：黄山派 SF32LB52 **没有无线网卡** ——
   运行时 `ifconfig` 直接 `ifconfig: open failed: 2`（ENOENT：命令在、网卡不在），
   `/dev` 里没有任何 wlan/eth 设备，defconfig 无 `CONFIG_WIFI`/`CONFIG_DRIVERS_IEEE80211`，
   厂商 BSP 也没有任何 WiFi 芯片驱动。所以**网络通路在本板物理不可达**，不是软件没写完；
   `set_wifi` 的实现走 `ifup wlan0` + `wapi`，需要真实网卡。详见 `docs/16 §11.3`。
4. **"主动+执行"场景已交付并默认开启**（`PW_WATCH_PROACTIVE 1`，2026-09-15）：晃表 → Agent 自动开单摆实验 →
   结果进手表 AI 日志，真机已验证（`docs/evidence/proactive-20260915/`）；两个真崩溃已修（`velaclaw_ask` 断言、
   cJSON 双重释放），`ai_agent` 已开机自启。**残余**：静止/振动桌面下单摆页仍可能给出无效 g（三道有效性门
   有漏过，约 1 条/70 s），根治需改页面运动判据 —— 详见 `docs/03 §7`。
5. **蓝牙：B1/B2/B3 全部打通**（2026-09-18）：片内 LCPU 蓝牙控制器 + zblue host 栈已跑起来 ——
   `phywear cap bt` 打印 `[bt] bt_enable rc=0`，注册好 GATT 服务（`e0f1a000-…`）、
   开好可被发现广播（名字 `PhyWear`），服务端自检 `SELFTEST PASS`。
   **B3 也已实测通过**：宿主 BlueZ（Intel AX201）当 BLE 中心设备，完成
   连接 → 读 `…a001`（16 B，解出 ax/ay/az，|a|≈1014 mg）→ 订阅 Notify（收到包）→
   写 `…a002` 'ping'，**设备侧 6/6 + 宿主侧 9/9 全过**，一条命令无人化复现：
   `python3 tools/phywear/pw_bt_b3.py --central --out <dir>`。
   **手表端也能直接看**：主页标题右侧圆点**未连接=灰 / 已连接=蓝**（点一下进「蓝牙」页），
   页内有状态/对端地址/MTU/收发计数 + 10 行 TX·RX 滚动日志 + 「发送测试」；
   另有文本特征 `…a003` 做**空口连通性测试**（手机写文本 → 手表回 `echo: …`，
   设备侧 8/8 + 宿主侧 13/13，`--central --text`）。
   **不想开终端**：桌面上双击「PhyWear 蓝牙测试」图标即自动测完（图形界面，14 项打勾
   + 实测值 + 大字结论）；图标由 `tools/phywear/install_desktop_shortcut.sh` 生成。
   **如实说明**：本队**没有在真手机（Android/iOS）上点过**，用的是宿主 BlueZ 中心设备，
   动作与手机上用 nRF Connect 完全对应；手机路径（`--wait 240`）保留可作独立交叉验证。
   ⚠️ 广播用**随机地址**（EXT_ADV + PRIVACY），**请按名字 `PhyWear` 找，不要按 MAC 找**。
   两个根因① NuttX fd 表按 `task_group` ⇒ H4 TX 恒 `EBADF`、② zblue `gatt_notify_mc()` 漏
   `data.handle` ⇒ 通知帧句柄 0x0000 被对端静默丢弃，见 `docs/03 §4.10b/§4.10c` 与 `docs/16`。
6. **EPIC 硬件加速来自官方 PR**（vendor_sifli #31 / lvgl #41 / nuttx-apps #121），**非本队原创**；
   本队做的是集成 + UI/算法层优化 + 驱动/应用。
7. **模拟器数据是合成的**；模拟器帧率（22–23）与真机（约 41）不可混用。
8. 若某步在您的环境失败，请把 `check_progress.py` 的输出发给我们（它会指出缺哪一项）。
9. **演示视频**：`docs/evidence/demo-20260915/phywear-demo-v1.mp4`（4:44，1280×720，H.264，无音轨）。
   片内常驻声明画面来源：真机截图 / 真机采集数据 / 宿主实录 + 后制；**不是实拍动态画面**
   （真机无视频输出）。重编命令与校验见同目录 `README.md`。
