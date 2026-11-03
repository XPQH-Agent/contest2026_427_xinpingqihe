# PhyWear LVGL 界面风格与动效规范（LVGL 版 UI/UX Skill）

来源与改造说明：本 Skill 的结构与分类参考开源项目 **ui-ux-pro-max-skill**
（https://github.com/nextlevelbuilder/ui-ux-pro-max-skill ，MIT）的
「设计数据库」组织方式（风格 / 色板 / 字体配对 / 推理规则 / UX 指南）。
**数据层已按上游 v2.15.0 快照照搬并入库**：`third_party/ui-ux-pro-max/`（5 个数据 CSV + 2 个参考栈 + LICENSE）。
**机器可读版规则**见 `third_party/ui-ux-pro-max/stacks/lvgl.csv`（本队新增的 LVGL 栈：14 条，含 Severity / Status / Verified At）。

**但内容不是照搬**：那份 Skill 的产出是 HTML + Tailwind 片段，目标是浏览器；
本板是 **390×450 AMOLED + SF32LB52 + EPIC 硬件加速 + LVGL 9**，
渲染能力与输入方式完全不同（无 GPU 混合、无圆角加速、无键盘、字体是子集位图）。
所以这里**只保留"数据层"的分类骨架**，把栈层换成 LVGL，输出层换成 LVGL 属性，
并**裁掉全部 Web 专属**内容。改造清单见 `docs/10_LVGL动效与风格规范.md`。

> 使用前提（本板硬约束，违反即掉帧）：**圆角、阴影、模糊、变换、非 H/V 渐变
> 都会让 EPIC 拒绝该绘制任务并回退到 CPU 软件光栅**。详见 §4。

---

## 1. 风格清单（**上游实名**，已按本板翻译）

判定依据是上游 `styles.csv` 的三个结构化字段（`Performance` / `Effects & Animation` / `Complexity`），
**不是**扫整行文本 —— 实测教训：整行扫会把 `Dark Mode (OLED)`、`Bento Box Grid` 这类低负载风格误判成"禁用"。

| 上游风格 | 上游声明（cost / effects） | 本板翻译：要什么、砍什么 | 结论 |
|---|---|---|---|
| **Flat Design** | cost:low；无渐变无阴影；hover 只改颜色/透明度 | 全盘可用；hover → 按压时的颜色变化 | ✅ **默认风格** |
| **Brutalism** | cost:low；**sharp corners (0px)**、粗字重、可见网格 | 与 EPIC"圆角必须 0"天然一致 | ✅ 入口页/仪表页 |
| **Data-Dense Dashboard** | cost:low；复杂度 Medium | 只取"高信息密度 + 网格对齐"；图表按 10–15 Hz 重绘 | ✅ 读数/分析页 |
| **Real-Time Monitoring** | cost:low；实时图表 + 状态脉冲 | 取"状态点 + 数值"；**脉冲用透明度，不用 glow** | ✅ 实时页 |
| **Bento Box Grid** | cost:low；但效果是 `rounded-xl(16px) + subtle shadows + hover scale(1.02)` | **只取网格跨列/跨行**；圆角、阴影、缩放**三样全砍**（EPIC 拒绝） | ⚠️ 取布局、弃装饰 |
| **Minimal & Direct** | cost:low；复杂度 Medium | 可用（"平滑滚动"本板无意义，去掉） | ✅ 向导页 |
| Neumorphism / Glassmorphism / 3D & Hyperrealism / Claymorphism / Liquid Glass | 依赖阴影/模糊/渐变 | **不可用**：EPIC 会回退 CPU 软光栅 | ❌ 禁用 |

> 上游 `styles.csv` 共 **88 条**，其自带 `Performance` 字段分布为 **cost:low 62 / cost:moderate 19 / cost:high 7**；
> 本队只从 cost:low 里挑，且逐条核对 `Effects & Animation` 是否撞上 EPIC 的三个禁区（圆角/阴影/变换）。

## 2. 色板（**上游 colors.csv 是色值来源**，本仓 token 是落地形态）

上游 `data/colors.csv`：**192 行**（Product Type × 角色），角色含 Primary / Background / Foreground /
Card / Muted / Border / Accent / Destructive 等 **19 列**。一条命令即可得到"每个 Product Type 的色值 →
RGB565 + 对近黑底对比度"的报告：

```bash
python3 tools/phywear/import_ui_skill.py --in third_party/ui-ux-pro-max/data
```

下表是本仓现行 token（已落地，勿自创色值）：

| 用途 | token | 说明 |
|---|---|---|
| 背景 | `PW_COL_BG` | 近黑，省电（AMOLED） |
| 卡片 | `PW_COL_CARD` / `PW_COL_CARD_LT` | 两级层次 |
| 正文/次要/占位 | `PW_COL_TEXT` / `PW_COL_DIM` / `PW_COL_FAINT` | 对比度递减 |
| 网格/分割 | `PW_COL_GRID` | 1 px 线 |
| 强调（按语义分色） | `PW_ACC_RAW`（原始传感器）/ `PW_ACC_MECH`（力学）/ 声学色 | 同一屏**只用一种强调色** |

规则：**强调色 = 语义**，不用于装饰；一屏最多 2 种（含状态色）；状态色仅用于「成功/失败/警告」。
新增色值必须同时给出 RGB565 值与对比度检查（正文与背景 ≥ 4.5:1）。

## 3. 字体配对与排版

| 用途 | token | 字号 |
|---|---|---|
| 页面标题 | `PW_FNT_LARGE` | 24 |
| 区块标题/读数 | `PW_FNT_XL` / `PW_FNT_MED` | 28 / 20 |
| 正文/说明 | `PW_FNT_BODY` | 14 |
| 单位/次要数值 | `PW_FNT_SMALL` | 16 |
| 超大读数 | `PW_FNT_XXL` | 48（Montserrat 数字） |

**中文是子集位图字体**：任何新增中文文案都要跑 `tools/phywear/gen_fonts.sh` 重生成字体，
否则真机会显示缺字方框（已踩过）。文案尽量短（390 px 宽，14 px 中文约 24 字/行）。

## 4. 性能铁律（本板特有，优先级高于观感）

1. **圆角 `radius` 必须为 0**：EPIC 的 FILL/BORDER 分支明确写着
   `/* EPIC doesn't support rounded corners */ if(radius != 0) return 0;`
   → 圆角填充/边框一律 CPU 软渲染。确需圆角时：面积 ≤ 32×32 px 且**不在每帧重绘路径上**。
2. **渐变只允许 2 段、方向只允许 HOR/VER**（EPIC 限制），其余回退 CPU。
3. **禁用阴影与模糊**（`LV_DRAW_SW_SHADOW_CACHE_SIZE=0`，且属复杂路径）。
4. **图片**走 EPIC IMAGE 时不得：旋转/缩放（transform）、`recolor`、
   `clip_radius`、`blend_mode != NORMAL`。否则回退 CPU。
5. **动效只改几何（x/y/width/height）或不透明度**，不要用 `transform_*`（走 SW 变换）。
6. **动效频率有界**：单次 ≤ 320 ms、同屏并发 ≤ 1 个、不循环（循环动画会持续吃帧）。
7. **一帧只做必要的事**：50 Hz 采样但图形重绘降到 10–15 Hz（数值可以高频刷新）。

## 5. 交互与 UX 指南（照搬"无障碍/反馈"分类，阈值按本板定）

- 触控目标 ≥ 44×44 px（手指，AMOLED 390×450）。
- 任何 >300 ms 的操作必须有进度反馈（进度条/样本计数/「对准中」状态字）。
- 失败必须**如实显示**（如"未标定"而不是假装成功）；不确定的量在界面上标注口径（例：轨迹页写「相对位移，非全局定位」）。
- 状态色 + 文字双编码（不要只靠颜色区分，色弱可用）。
- 页面切换不加转场动画（整屏重绘成本高），改为**内容元素滑入**（见 §6）。

## 6. 动效规范（物理感，禁止 PID 迭代调参）

- **两条曲线，离线算好存 flash**，运行时查表 + 线性插值：
  - `pw_motion_path_spring`：欠阻尼弹簧阶跃（ζ=0.40，过冲 ≈25%，末帧严格到位）——**默认**，用于"进入/回弹/归位"。
  - `pw_motion_path_decay`：衰减振荡 e^(-6u)·sin(4πu)——用于"抖一下"的提示反馈。
- 用法：
  ```c
  lv_anim_set_path_cb(&a, pw_motion_path_spring);   /* 替换 lv_anim_path_linear */
  pw_motion_slide_in_y(obj, 18, 320);               /* 从下方 18 px 弹入 */
  ```
- **不要**用逐帧 PID 迭代去"调弹性"：同一形状每秒被算几百次，属于纯浪费；
  解析式只在需要连续变化的场合用（如随实时数据变化的指针），且要用 `pw_motion_*_analytic_*` 做对照。
- 回退：把 path 换回 `lv_anim_path_linear` 即可（不动其他代码）；整模块可用
  `PW_MOTION_USE_TABLE=0` 切回解析式。

## 7. 明确裁掉的 Web 专属内容

Google Fonts 动态导入、响应式断点（`@media`）、CSS 变量/预处理器、
Tailwind 类名与 JIT 生成、React/Vue 组件、键盘 focus 环、hover 态、
CSS `transition`/`@keyframes`、SVG 图标、浏览器滚动与惯性滚动、
Web 字体回退链（本板只有 5 个固定字号的位图子集）。

## 8. 机器可读版本（与本文档同源，改一处必须改两处）

上述规则同时以上游**栈层 schema** 落成 `third_party/ui-ux-pro-max/stacks/lvgl.csv`：
`Category, Guideline, Description, Do, Don't, Code Good, Code Bad, Severity, Docs URL, Applies To, Status, Verified At`。
共 **14 条 = 11 条 verified（有真机或代码证据）+ 3 条 needs-verify（来自上游 UX 指南，本板尚未实测）**。
新增规则请同时改本文件与 `stacks/lvgl.csv`，并在 `Verified At` 写清证据与日期；**没有证据的一律标 needs-verify**。
