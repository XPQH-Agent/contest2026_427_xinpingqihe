# 10 · LVGL 动效与风格规范（P1-2 落地记录 + ui-ux-pro-max-skill 吸纳）

> 阶段：**已开工并落盘**（2026-09-16）。前置审查见上一轮《实验性审查报告》；
> 回退锚点：参赛仓 git tag **`pre-motion-skill-20260916`**（在本次改动之前）。

## 0. 两个问题的一句话结论

- **问题 1（解析式 vs 查表）**：曲线形状固定、每秒被算几百次，**查表法可行且划算** ——
  离线按帧率采样存 flash、运行时只做"索引 + 线性插值"，实测主机上 **0.59 ns/次 vs 解析式 6.40 ns/次（×10.8）**，
  代价是 **516 B flash、0 B SRAM**，精度损失在 LVGL 的 0/1024 分辨率下约 **2.4 个步进（视觉不可辨）**。
- **问题 2（HTML 版 Skill → LVGL 版）**：**数据层可吸纳**（风格/色板/字体配对/推理规则/UX 指南的分类骨架），
  **栈层与输出层必须重写**（HTML+Tailwind → LVGL 样式属性与 obj 配置），**Web 专属全部裁掉**
  （Google Fonts 导入、响应式断点、键盘 focus、hover、React 等）。

---

## 1. 问题 1：查表法动效（已实现）

### 1.1 曲线与存储

两条曲线（都归一化到 u = t/duration ∈ [0,1]）：

| 曲线 | 公式 | 参数 | 用途 |
|---|---|---|---|
| `spring` | 欠阻尼二阶阶跃 `1 − e^(−ζωu)·[cos(ω_d u) + (ζω/ω_d)·sin(ω_d u)]`，`ω_d=ω√(1−ζ²)` | ζ=0.40、ω=12，过冲峰值 **1.2494** | 进入/回弹/归位（默认手感） |
| `decay` | `e^(−6u)·sin(2π·2u)` 按峰值归一化 | a=6、f=2 | "抖一下"的提示反馈 |

- 存储：**Q14 定点（1.0 = 16384）、`int16_t`、129 点/条**，两条共 **516 B，落 flash(.rodata)**。
  选 Q14 而非 Q16：spring 有过冲（峰值 1.25），Q16 表示不到 1.0 以上；Q16+uint32 会让表涨到 1 KB 且插值要走 64 位。
- **末帧严格到位**：原始 `s_raw(1)=1.0035703`，若直接制表，动画最后一拍会从 1.0036 跳回 end_value
  （位移 200 px 时约 0.7 px 的"哒"一下）。所以曲线整体除以 `s(1)`（系数 **0.9964424**，
  由生成器输出、解析式实现引用同一常量），并把 `u ≥ 1024` 强制返回 `1.0`。

### 1.2 精度（实测，不是估计）

`python3 tools/phywear/gen_motion_table.py` 的输出：

| 曲线 | 表长 | 插值最大误差 | 出现在 | 量化上界 |
|---|---|---|---|---|
| spring | 129×2 B | **1.06e-03** | u=0.004（起步段曲率最大处） | 3.1e-05 |
| decay | 129×2 B | **2.29e-03** | u=0.020（首个峰） | 3.1e-05 |

- 误差 ∝ h²：N 从 64 → 128，误差降到 1/4，flash 只多 260 B、**运行时成本完全不变**，所以取 128。
- 换算到 LVGL：path 内部把曲线映射到 0…1024，`2.29e-3 × 1024 ≈ **2.4 个步进**` —— 对位置动画不可辨。
- 主机单测 `pw_motion_selftest`：**rc=0，max_err=7.93e-04**（端点必须是 0 与严格 1.0、越界时间收敛到末值、
  过冲峰值必须落在 1.15~1.35 之间、查表与解析逐点误差 < 3e-3）。

### 1.3 计算节省量（实测 + 待补）

| 环境 | 查表 | 解析式（expf+sinf+cosf） | 倍数 | 出处 |
|---|---|---|---|---|
| 主机 x86-64 `-O2`（20 万次） | **0.59 ns/次** | **6.40 ns/次** | **×10.8** | `pw_math_hosttest --bench` |
| **真机 SF32LB52**（2 万次） | **151.25 ns/次** | **2088.40 ns/次** | **×13.8** | `phywear motionbench 20000`（`docs/evidence/motion-20260916/`） |

> 真机实测已补齐（板子重新插拔后）。注意绝对值含循环与累加开销，**有意义的是比值 13.8×
> 与每次省下的 1937 ns**。

**必须说清的边界**：把 1937 ns 放回真实调用量级后，**查表法不是帧率杠杆** ——
LVGL 的 path 每个动画对象每帧只调一次，一次 320 ms 动效 ≈ 19 次调用 ≈ 省 **37 µs**
（一帧预算 16.7 ms 的 0.2%）；同屏 3 对象、每秒重起动一次也只有 **~0.1 ms/s**。
它的价值是**成本确定性**（无 libm 抖动、代码可控、表仅 516 B，且可复用于逐样本包络类计算），
**不是** fps 手段。
> **更正（2026-09-16 实测）**：本文档此前称「真正压帧率的是每帧全屏推屏 351 KB ≈ 14 ms 与圆角回退 SW」——
> **两条都被实测推翻**：① `fb_flush_start()` 送面板的窗口是 `p_fb->fb_clip`（脏区），脏区推屏 vendor 已实现；
> ② 圆角 14→0 的 A/B 在实时页与轨迹页均**零增益**。当前真正有效的是主循环休眠（+30.8%），见 `docs/evidence/p0-20260916/`。

### 1.4 实施步骤（已按此执行）

1. `tools/phywear/gen_motion_table.py` 离线生成曲线 + 精度报告 + 归一化系数（**唯一真值源**，表文件禁止手改）。
2. `pw_motion.{c,h}` 纯算法实现：整数查表 + 线性插值；**解析式实现始终保留**（对照计时 + 回退）。
3. `pw_motion_lvgl.{c,h}` 适配层：`pw_motion_path_spring` / `pw_motion_path_decay` 直接替换
   `lv_anim_path_linear`；`pw_motion_slide_in_y()` 便捷滑入。
4. 真机计时入口 `phywear motionbench [iters]`（用 `clock_gettime` 分别计时两条实现）。
5. 接入一处可见动效：切到「轨迹」页时图形卡以弹簧滑入（`phywear_imu.c` 的 `imu_set_page`，单对象 320 ms）。
6. 主机单测纳入 `pw_motion.c` + `pw_motion_table.c`（`hosttest_math.sh`）。

**成本实测**：固件 2,204,456 → **2,206,056 B（+1,600 B，含计时打印修正后的重建）**，md5 `0c9f8e90574cad28fba6f33a7b980f06`，flash 13.14% → **13.15%**，
**SRAM 不变（487,460 B / 92.98%）** —— 表在 flash，模块零静态 SRAM，符合"SRAM 不新增"。

---

## 2. 问题 2：ui-ux-pro-max-skill → LVGL 版（已实现）

来源：<https://github.com/nextlevelbuilder/ui-ux-pro-max-skill>（MIT）。
**本次开发环境无法访问 github/raw.githubusercontent（DNS 解析到非公网地址）**，因此改造是
**按其"设计数据库"的分类骨架**做的，不照抄具体条目；真实 CSV 的导入路径见 §2.4（已实现但**未用真数据实测**）。

### 2.1 可吸纳 / 需重写 / 裁掉

| 层 | 处理 | 说明 |
|---|---|---|
| 数据层：风格清单 | **吸纳**（重新定值） | 只保留低负载方向：Flat Dark、Bento 网格、Instrument；Neumorphism/Glassmorphism/3D 明确禁用（依赖阴影/模糊/渐变，EPIC 会回退 CPU） |
| 数据层：色板 | **吸纳** | 用本仓既有 token（`PW_COL_*`），新色必须给 RGB565 与对比度检查 |
| 数据层：字体配对 | **吸纳为"层级意图"** | 本板只有 5 个位图子集字号（`pw_font_14/16/20/24/28`）+ Montserrat 数字；Web 字体名无意义，且**新增中文必须重跑 `gen_fonts.sh`** |
| 数据层：推理规则 | **吸纳** | "什么场景用什么风格"改为按页面类型分派（读数页→Instrument、入口页→Bento…） |
| 数据层：UX 指南 | **吸纳**（阈值按本板改） | 触控目标 ≥44 px、>300 ms 操作必须有进度反馈、失败如实显示、状态色+文字双编码 |
| 栈层 | **重写** | HTML/Tailwind → LVGL 9：`bg-color/opa`、`radius`、`bg_grad`、`text_font`、`flex/grid`、`lv_anim` |
| 输出层 | **重写** | 不再产出 HTML 片段，改为 **LVGL 样式属性/obj 配置**（本仓风格：`pw_card_new`/`pw_label_new` 等封装） |
| Web 专属 | **裁掉** | Google Fonts 动态导入、`@media` 响应式断点、键盘 focus 环、hover 态、CSS 变量/预处理器、Tailwind JIT、React/Vue 组件、SVG 图标、浏览器惯性滚动 |

### 2.2 关键映射表（Web → 本板 LVGL）

| Web 概念 | LVGL 9 API | 本板约束（性能铁律） |
|---|---|---|
| `background-color` | `lv_obj_set_style_bg_color/opa` | 纯色，AMOLED 近黑省电 |
| `border-radius` | `lv_obj_set_style_radius` | **必须 0**：EPIC 的 FILL/BORDER 分支 `radius != 0 → return 0`（回退 CPU）；确需圆角须面积 ≤32×32 且不在每帧重绘路径 |
| `box-shadow` | `lv_obj_set_style_shadow_*` | **禁用**（`LV_DRAW_SW_SHADOW_CACHE_SIZE=0`，且属复杂路径） |
| `linear-gradient` | `lv_obj_set_style_bg_grad_*` | **仅 2 段、方向仅 HOR/VER**（EPIC 限制） |
| `filter: blur` | `lv_obj_set_style_blur_*` | **禁用** |
| `font-family` | `lv_obj_set_style_text_font` | 5 个位图子集字号 + Montserrat 数字；中文进表用 `gen_fonts.sh` |
| `flex` / `grid` | `lv_obj_set_flex_flow` / `grid_dsc` | 固定 390×450，优先绝对定位（`lv_obj_set_pos`），减少布局重排 |
| `transition` + `cubic-bezier` | `lv_anim` + `lv_anim_set_path_cb` | **用 `pw_motion_path_spring`（查表）**；单属性、≤320 ms、不循环 |
| `transform: scale/rotate` | `lv_obj_set_style_transform_*` | **避免**（走 SW 变换，EPIC 不支持） |
| `@media` 断点 | — | 裁掉（单一分辨率） |
| `:focus-visible` | `lv_group` | 裁掉（无键盘；触控为主） |

### 2.3 交付物

| 文件 | 内容 |
|---|---|
| `apps/examples/phywear/skills/phywear-lvgl-ui.md` | **LVGL 版 Skill 本体**（7 节：风格/色板/字体/性能铁律/UX/动效/裁掉清单），随固件安装到 `/data/agent/skills/`，AI Agent 可直接读到 |
| `pw_skill_blob.c` | 由 `gen_skill_blob.py` 重新生成（现在 **2 个 skill**，`--check` 通过） |
| `tools/phywear/import_ui_skill.py` | 数据层导入器：读上游 CSV → 列名自动归类 → 风格按"低负载/条件可用/禁用"三档筛选 → 色值转 RGB565 并算对比度 → 输出报告；`--sample` 自检已实测 |

### 2.4 导入器的诚实边界

- `--sample`（内置样例）**已实测通过**：正确识别列、把 Glassmorphism/Neumorphism 判为禁用、
  把渐变判为"条件可用（仅 2 段 H/V）"、色值转 RGB565 并给出对比度与"当文字色是否可用"。
- `--in <上游目录>` 这条路径**未用真实 CSV 实测**（网络受限），且**上游列名未核实**，
  因此导入器对识别不到的列一律"如实列出、不猜测填充"。
- 拿到 CSV 后应做的事：先 `--in` 跑一遍看报告 → 人工筛出低负载风格 → 再决定是否写进
  `skills/phywear-lvgl-ui.md`（**不自动改代码**）。

---

## 2.5 【完成记录 2026-09-16】数据层照搬 + LVGL 栈层落地（本轮补齐）

上一轮只交付了"分类骨架"，**数据层从未导入、输出层从未落地**。本轮补齐：

### 2.5.1 数据层：上游真实 CSV 已入库并跑通

- 取件：本机无法直连 github，**走 jsDelivr 镜像**、**固定版本 `2.15.0`**：
  `https://cdn.jsdelivr.net/gh/nextlevelbuilder/ui-ux-pro-max-skill@2.15.0/.claude/skills/ui-ux-pro-max/<path>`
  （注意：`skills/...` 路径 404，真实路径在 `.claude/skills/...`）
- 入库：`third_party/ui-ux-pro-max/`（MIT，含 LICENSE + 取件说明）：`data/{styles,colors,typography,ui-reasoning,ux-guidelines}.csv`
  + `stacks/{html-tailwind,swiftui}.csv`（仅作结构参考）+ `SKILL.md`
- 导入实测（`tools/phywear/import_ui_skill.py --in third_party/ui-ux-pro-max/data`）：
  **styles 88 条**（`Performance` 分布 **cost:low 62 / moderate 19 / high 7**）、**colors 192 行 × 19 角色**、
  typography 50 KB、ui-reasoning 77 KB、ux-guidelines 28 KB —— 全部完成 RGB565/对比度换算与分档

### 2.5.2 【踩坑并修正】导入器不能"扫整行关键字"

第一版按整行文本判负载，**把 `Dark Mode (OLED)`、`Bento Box Grid`、`Flat Design` 判成了"禁用"**
（因为它们的 `Do Not Use For` / `Implementation Checklist` 列里出现 shadow/gradient 字样）。
改为**字段级判定**：只看 `Effects & Animation` + `Performance` + `Complexity` 三列（schema 指纹命中后走快路径）。
修正后：低负载 38 条、条件可用 10 条、禁用 59 条 —— 与上游自带 `Performance` 字段一致。

### 2.5.3 栈层：新增 `stacks/lvgl.csv`（这是"方向 2_2"的落地形态）

上游架构本是**数据驱动 + 可插拔栈**（`data/stacks/` 下每个技术栈一个 CSV：flutter / html-tailwind / nextjs /
vue / swiftui / jetpack-compose …）。所以"新增 LVGL 栈"= **新增一个 `stacks/lvgl.csv`**，
严格沿用上游 schema：`Category, Guideline, Description, Do, Don't, Code Good, Code Bad, Severity, Docs URL, Applies To, Status, Verified At`。

- 文件：`third_party/ui-ux-pro-max/stacks/lvgl.csv`，**14 条规则**
- 诚实标注：**11 条 `verified`**（附真机或代码证据与日期）+ **3 条 `needs-verify`**（来自上游 UX 指南、本板未实测）
- 内容全部来自本项目的实测事实：圆角必须 0（EPIC 判据 + 真机 A/B）、渐变仅 2 段 H/V、禁阴影/模糊/变换、
  图片只走普通混合、5 个位图子集字号（新增中文须重跑 `gen_fonts.sh`）、动效查表不做 PID
  （真机 151 vs 2088 ns）、图表 10–15 Hz（面板 60 Hz 规格、实测 48–49 FPS）、触控 ≥44×44、>300ms 必须有进度反馈

### 2.5.4 输出层：Skill 已升级为"条目级"，UI 落地已于本轮执行（见 §6）

`app/phywear/skills/phywear-lvgl-ui.md` 已改为**引用上游实名风格**并给出逐条翻译
（Flat Design ✅ 默认 / Brutalism ✅ 圆角 0px / Data-Dense / Real-Time Monitoring / **Bento：只取网格、圆角阴影缩放全砍** /
Neumorphism 等禁用），并指向机器可读的 `stacks/lvgl.csv`；Skill blob 已重新生成（2 个 skill）。

**本轮已落地**（2026-09-16 用户批准"全面改进 UI 设计"后执行）：按上述 token **改了全局色板并接入 6 处动效**，
逐项清单、真机截图与 A/B 数字见 **§6**。仍**未**做的是 §4 的 **Bento 版式重排**（圆角保留 14 px ——
实测圆角 14→0 对 fps 零增益，见 `docs/evidence/p0-20260916/`）与部分页面级动效，清单同样在 §6.4。

## 3. 回退方案

| 层级 | 手段 | 具体 |
|---|---|---|
| 仓库 | **git tag** | `pre-motion-skill-20260916`（本次改动之前的 HEAD） |
| 仓库 | 独立提交 | 动效模块与 Skill 分成两个提交，可单独 `git revert` |
| 代码 | **宏开关** | `PW_MOTION_USE_TABLE=0` → 整模块切回解析式（调用点不变，行为等价） |
| 代码 | 单点回退 | 把 `lv_anim_set_path_cb(&a, pw_motion_path_spring)` 换回 `lv_anim_path_linear`；或删掉 `phywear_imu.c::imu_set_page` 里那三行动效 |
| 数据 | 表可重生成 | 曲线/参数在 `gen_motion_table.py`，改回即重跑脚本；表文件是生成物，不手改 |
| 固件 | 烧回上一版 | 上一版固件 md5 `2e00ecf22243b0bc942c3933f77f6660`（2,204,456 B，蓝牙阶段 A）；烧录走 `flash_with_ftab.sh` |

**回滚触发条件（任一命中即回退，不做"再调调看"）**：
1. 同口径 A/B 下 **fps 下降 >5%**（口径见 §4）；
2. 启动异常、页面卡死、动画末帧跳动或对象停在错误位置；
3. SRAM 越界（>512 KB）或 flash 越界；
4. `pw_motion_selftest` 或 `gen_skill_blob.py --check` 失败。

---

## 4. 验证口径（与上一轮《审查报告》P0-0 一致）

- **fps/loops**：30 s 心跳 `[phywear] alive t=Ns loops/s=… fps=…`（`fps` = `LV_EVENT_RENDER_READY` 计数/秒）。
- **同口径要求**：同页面、同注入状态（bench 开/关）、同是否跑 Agent、同窗口长度。
- 本次动效接入点的验证结果（真机，2026-09-16）：切到「轨迹」页触发弹簧滑入，
  动画结束后取帧 `docs/evidence/motion-20260916/imutraj-anim.png` —— 图形卡停在最终位置（y=46），
  无偏移/无残影/无卡死；轨迹页心跳 **fps 8→9、loops 57~59**（改动前基线 8），**测不到退化**。
  单次 320 ms 单对象动效在 30 s 窗口里本就会被平均掉，故不做"有动画 vs 无动画"的 fps 宣称。

---

## 5. 参考依据（只取思路，不照搬）

- **ui-ux-pro-max-skill**（数据层分类骨架、低负载风格取向）—— 见 §2；其 HTML/Tailwind 产出层不适用本板。
- **B站 b23_tv_Sc54nzn**（PID 调参易抖、UI 动画首选贝塞尔/解析式）—— 本模块据此**不做 PID**，
  改为一组固定解析曲线的查表。
- **GitHub feitangyuan_motion-web**（物理阻尼/弹簧）—— 取"用带物理参数的闭式解"这一点，
  在本板把它固化成 flash 表（Web 端的求解器思路不需要，也不该带到 MCU）。
- **B站 b23_tv_SPZ0czSz / GitHub RealNee_lvgl_demo**（DMA/双缓冲/局部刷新）—— 属 P0 面板流量问题，
  与本文档的动效层无直接关系，留待 P0 动工时按同口径实测。

---

## 6. 本轮 UI 全面改版执行记录（2026-09-16）

### 6.1 色板 token（来源 `data/colors-lvgl.csv`；一处改、全页生效）

| token | 改动前 | 改动后 | colors-lvgl.csv 里的角色 |
|---|---|---|---|
| `PW_COL_BG` | `0x101418` | **`0x0F172A`** | Background |
| `PW_COL_CARD` | `0x1A222C` | **`0x1B2336`** | Card |
| `PW_COL_CARD_LT` | `0x26333F` | **`0x272F42`** | Muted（按压/高亮） |
| `PW_COL_TEXT` | `lv_color_white()` | **`0xF8FAFC`** | Foreground（不用纯白） |
| `PW_COL_DIM` | `0x8A97A5` | **`0x94A3B8`** | Muted Foreground（对底 6.96:1） |
| `PW_COL_GRID` | — | **`0x475569`** | Border（网格/描边） |

未动：8 个板块强调色 `PW_ACC_*` 与 `PW_COL_FAINT`。**色板是全局共享 token**，所以"逐页风格铺开"
在色板这一层是自动生效的（已用真机截图核对 8 页）。token 处留有来源注释。

### 6.2 动效接入点（本轮落地 6 处）

| # | 位置 | 曲线 / 时长 | 实现 |
|---|---|---|---|
| ① | 主页 tile 按压（P1） | C4 / 150 ms，y+2 px + 变色 | `ui_tile_press_cb` |
| ② | 工具板列表错峰入场 | C2 / 280 ms，逐项 +50 ms | `pw_motion_slide_in_y_at(row, 10, 280, i*50)` |
| ③ | 轨迹页「归零」回弹 | C3 / 260 ms，y 0→3→0 | `pw_motion_settle_y(btn, 0, 3, 260)` |
| ④ | 页指示点激活脉冲 | 三角波 220 ms，100%→40%→100% | `pw_motion_pulse_opa`（`imu_sync_dots`） |
| ⑤ | IMU / 标定 / 向导各按钮按压 | C4 / 90 ms 下压 + 160 ms 回位 | `pw_motion_press_feedback(btn, base_y, 3)` |
| ⑥ | 秒表页 Clear 与阈值 ± 按钮按压 | 同上（dy=2） | 同上，共 3 处 |

新增共享 API（`pw_motion_lvgl.{c,h}`）：`pw_motion_press_feedback` / `pw_motion_settle_y` /
`pw_motion_pulse_opa` —— 均满足"单次 ≤320 ms、一次性不循环、单对象、只改几何或不透明度"。

**④ 的如实说明**：方案原文是"标定完成状态点脉冲"，但代码里标定页并**没有**独立的"完成状态点"对象
（只有页指示点 `g_i.dots[]` 与水平仪气泡 `g_i.lvl_dot`）。本轮回退为"**页指示点激活时脉冲**"——
语义从"标定完成"变成"切页确认"；要真正的标定完成指示得先新增该对象（未做）。

### 6.3 帧率 A/B（同页、同命令、30 s 心跳；动效层是唯一变量）

| 页面 | `PW_UI_MOTION=1` | `PW_UI_MOTION=0` | 判定 |
|---|---|---|---|
| 主页（静置） | loops/s **282**，fps **3** | loops/s **284**，fps **3** | 无退化 |
| `cap raw` 实时页 | loops/s **208**，fps **12** | loops/s **209**，fps **12** | 无退化 |

- 资源：flash **2,486,288 B**（开）vs **2,485,144 B**（关）⇒ 动效层 **+1,144 B**，关掉即回收；
  **SRAM 489,136 B 两侧完全相同（新增 0 B）**。
- **口径限制（如实）**：心跳 fps 是整数计数，差 1 分辨不了；单次 ≤320 ms 的动效在 30 s 窗口内会被平均掉，
  因此**不能**据此宣称"动效期间不掉帧"——那需要 `PW_PERF_PROBE=1` 的逐帧窗口（本轮未测）。

### 6.4 本轮未做（如实清单）

1. **§4 Bento 版式重排**（不等宽跨列、直角卡片）：圆角保留 `PW_CARD_RADIUS=14`——真机 A/B 圆角 14→0
   **零 fps 增益**（`docs/evidence/p0-20260916/`），纯观感取舍，未动。
2. 批 2 的 ⑤读数页 C1 淡入、⑥图表页 C2 滑入、⑦向导步骤点 C1：**未接**。
3. ⑧秒表"开始/停止"按钮：该页未找到独立的开始/停止按钮对象，只接了 Clear 与阈值 ±。
4. 逐页截图 **8/16**：`pwshot.py sweep` 在**持续重绘页**上会被"行一致性"校验拒绝（同一行两次读到不同内容即判失败），
   并因连续失败提前中止。主页/原始/摆/弹簧/离心/倾角/标尺/频谱 8 页已取到；声学 4 页、秒表、光门、
   掌声、设置、关于**尚无真机截图**——这是取图工具的限制，不是页面缺陷。
5. 按压手感（①③④⑤⑥ 的实际观感）**需人手指确认**——无法远程按屏。

### 6.5 证据与顺带发现

- 证据目录：`docs/evidence/ui-motion-20260916/`（真机 PNG + §6.3 的 A/B 数字）。
- **`flash_with_ftab.sh` 的读回校验存在假警报**：`sftool read_flash` 失败时（本次实测
  `timeout while waiting for reading flash at 0x1201C000`）脚本因 stderr 被 `>/dev/null` 吞掉，
  直接报"固件 ❌ 不一致"。实际写入正常（板子能正常启动并跑 GUI）。脚本应区分"读取失败"与"逐字节不同"，
  本轮**未改**（不在交付前动烧录脚本）。

---

## 7. 第二轮：BUG 修复 + 交互补强 + v2 视觉（2026-09-17）

> 用户反馈三条：① 动效太少、**按键无反应**、切屏无动效；② **多个界面"可用内容缺失到只剩一个"**（BUG）；
> ③ 整体不够好看，要求按 ui-ux-pro-max 出 HTML 设计稿后重构。
> 完整证据包：**`docs/evidence/ui-v2-20260917/README.md`**；设计稿 `docs/design/phywear-ui-v2-mockup.html`。
> 回退锚点 **`git tag pre-ui-redesign-20260917`**。

### 7.1 ② 的真 BUG（我自己上一轮引入的）

`pw_board_list()` 的错峰入场里，早期 `pw_motion_slide_in_y_at()` **在函数内部读 `lv_obj_get_y()` 当动画终点**。
LVGL v9 的 `lv_obj_set_y()` 只写**样式 Y**，`lv_obj_get_y()` 读的是**已布局 coords** —— 刚创建、尚未布局
刷新的行两者不一致，于是**同批 N 行的终点全被算成同一个 y**，N 行完全重叠，只剩最后绘制的一行可见
（真机现象：工具板块只显示"水平仪"）。修法：API 改**显式传 base y** + 动画前先落到起点；无延时版改用
`lv_obj_get_y_aligned()`。**教训**：LVGL 里"刚创建的对象"不能读 coords，只能读样式值。
顺带把板块行高 76→64 px（5 行 360 px 才放得进 396 px 内容区）。

### 7.2 ① 的交互补强

- 按压反馈**改成样式**（`pw_press_style()` 挂在 `pw_card_new()` 上，按下 translate_y +3px + 底色 Muted +
  120 ms C4 过渡）→ 全 App 卡片/按钮/行/宫格**一次性统一**，不再"有的有有的没有"。
- 修掉"按图标没反应"：`lv_obj_create()` 默认 CLICKABLE，**装饰性子件会抢走卡片的按压状态** →
  新增 `pw_deco()` 并用于宫格图标块与板块行色条。
- 切屏转场：`pw_topbar()` 内容区 +16px 落位（C2 260 ms）；主页 8 宫格错峰入场（40 ms 步进）；
  IMU / 原始传感器多页屏改**动画滚动**。

### 7.3 ③ 的视觉

HTML 设计稿（`docs/design/`，headless Chrome 出图）→ 落地：卡片 **1px hairline 描边**
（`PW_COL_LINE`，替代 EPIC 禁用的阴影）、宫格图标块圆形→**圆角方块**、原始页 **3px 强调条章节标签**；
并修掉子代理审图发现的排版缺陷（加速度频谱页 "x: 频率 0.25 Hz" 压在按钮上 → 移到卡片右上与 Y 轴标签同行）。

### 7.3.1 读数页 v2（本轮追加，最常见的页面）

原始传感器页（6 页实时数据）按设计稿改版：**章节条 + 大数字 + 右侧单位列 + 行间 1px hairline + 顶部实时点**。
两个细节：

- 单位从数值串里**挪到独立单位列**（`"%+.2f g"` → `"%+.2f"`），否则会和单位列重复显示；
  曲线视图不使用单位列，那里仍保留内嵌单位。
- 单位列用**固定 x（310）**而不是 `align_to`：数值宽度每帧都变，`align_to` 会反复触发重排。

### 7.3.2 其余页面版式铺开 + D1 状态脉冲（本轮追加）

- `pw_section_bar()`（3px 强调条）批量接入 **24 处页头**（单摆/弹簧/向心力/倾角/标尺/计时器/频谱/生活），
  机械替换只落在 `PW_STR(...)` 页头，已核对未误伤 `"g"`/`"L"` 这类单位标签。
- **D1**：磁性标尺计数**只在增加时**脉冲一次（200 ms）——`ruler_recount()` 每次采样都会调用，
  无条件脉冲会变成常驻动画，违反"单次/不循环"。

### 7.3.3 设置页与图表卡（本轮追加）

- 设置页「语言」卡标题接入章节条；**倾角图卡**修掉与频谱页同型的"文字压控件条"缺陷（X 轴时间标签
  `(12,248)` → 卡片右上 `LV_ALIGN_TOP_RIGHT, -12, 6`）。单摆/弹簧卡的「最近 2.4 s」本就在右上，未动。

### 7.4 数字（同页同命令 30 s 心跳，`cap raw`）

| 构建 | loops/s | fps |
|---|---|---|
| 上一轮（色板 v1 + 动效 6 处） | 208 | 12 |
| 本轮（含 1px 描边） | 200 | 12 |
| 本轮 + 关描边（`PW_CARD_LINE=0`） | 203 | 12 |
| 本轮 + 读数页 v2（单位列/hairline/实时点） | **195** | 12 |
| 本轮 + 页头章节条 24 处 + 标尺 D1 脉冲 | **197** | 12 |
| 本轮 + 设置页章节条 + 倾角图卡标签修复 | **197** | 12 |

**fps 六档全部相同（12）**；累计 loops/s −6.3%（描边 ≈1.5%、读数页版式 ≈2.5%、其余为切屏转场等）。
flash 2,485,140 → **2,487,452 B**，**SRAM 489,136 B 不变**。

### 7.5 仍未做

逐页版式铺开（读数页 KV 行 + 状态点 + 图表留白）、数值变化脉冲 D1、读数页淡入 C1、
**按压手感需手指确认**（真机无触摸注入）、sweep 在实时页仍被行一致性校验拒绝（逐页证据仍是单页抓取）。
