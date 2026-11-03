# ui-ux-pro-max-skill（上游设计数据库 · 固定版本快照）

**来源**：<https://github.com/nextlevelbuilder/ui-ux-pro-max-skill>，**MIT**（见同目录 `LICENSE`）。
**版本**：`2.15.0`（jsDelivr 缓存包的最新稳定版）。
**取件方式**：**用户提供的 main 分支压缩包** `ui-ux-pro-max-skill-main.zip`（2026-09-16 下载），
已全量解包入库；早期 5 个核心数据文件曾用 jsDelivr 镜像（固定版本 2.15.0）取回，
**与 zip 内同名文件逐字节一致**（以 `styles.csv` 为例：149,478 B / 89 行，md5 相同），故两来源等价。

```
ja 早期镜像路径（备用）：https://cdn.jsdelivr.net/gh/nextlevelbuilder/ui-ux-pro-max-skill@2.15.0/.claude/skills/ui-ux-pro-max/<path>
   注意：`skills/...` 路径会 404，真实路径在 `.claude/skills/` 下
```

入库日期：2026-09-16。

## 为什么把数据落进参赛仓

1. **可复现**：`tools/phywear/import_ui_skill.py` 的"真实 CSV"路径需要这些文件；
   上游一旦改版，导入结论会漂移，故固定版本快照 + vendoring。
2. **归属如实**：只搬**数据层**（风格/色板/字体配对/推理规则/UX 指南）与两个**参考栈**；
   产出层（HTML/Tailwind/React 代码生成）**一律不用**。改造与裁剪见 `docs/10 §2`。

## 文件清单

| 文件 | 大小 | 用途 |
|---|---|---|
| `data/styles.csv` | 149 KB | 风格库 88 条（自带 `Performance` 成本字段，是"低负载筛选"的输入） |
| `data/colors.csv` | 38 KB | 色板 192 行 × 19 角色（Primary/Background/Foreground/Card/Muted/Border…） |
| `data/typography.csv` | 50 KB | 字体配对（Heading/Body + Google Fonts URL，后者本板裁掉） |
| `data/ui-reasoning.csv` | 77 KB | 推理规则（UI_Category → Recommended_Pattern / Style_Priority / Decision_Rules） |
| `data/ux-guidelines.csv` | 28 KB | UX 指南（Do/Don't/Severity） |
| **`data/motion.csv`** | 17 条 | **动效库**（Category/Intensity Tier/Trigger/Duration/Easing/Performance Notes）—— 本队动效体系的直接依据 |
| `data/app-interface.csv` | 32 条 | 移动端界面问题（触屏相关条目并入 Skill） |
| `data/charts.csv` | — | 图表选型与可访问性（仅取实时曲线相关） |
| `data/google-fonts.csv`、`data/icons.csv`、`data/landing.csv`、`data/products.csv`、`data/react-performance.csv` | — | **裁掉**（Web 专属或本板无场景），保留仅为完整性与可追溯 |
| `data/stacks/*.csv` | 22 个 | 上游各技术栈（flutter/vue/swiftui/uno/jetpack-compose…），**仅作结构与 Severity 约定对照** |
| `scripts/*.py` | 5 个 | 上游引擎（core/design_system/search/reasoning_contract/validate_data），需要时离线用于"选风格" |
| `stacks/html-tailwind.csv` | 17 KB | **仅参考**：上游栈层 CSV 的结构样例（内容不采纳） |
| `stacks/swiftui.csv` | 15 KB | **仅参考**：原生 GUI 栈样例（离嵌入式最近，但仍非 LVGL） |
| `stacks/lvgl.csv` | 本队新增 | **LVGL 栈**：14 条规则（11 verified + 3 needs-verify），本板实测事实的机器可读版 |
| `SKILL.md` | 16 KB | 上游 Skill 定义（用于对齐我们 LVGL 版的结构） |

## 【LVGL 适配层】本队生成的 4 个原生数据文件（2026-09-16）

上游数据是**给浏览器写的**（hover/scroll 触发、GSAP easing、GSAP 片段列、Google Fonts 配对、web 页面类型），
只做文档级映射不算适配。故新增生成器 `tools/phywear/gen_lvgl_adapt.py`，
**从上游数据可复现地**产出 4 个 LVGL 原生文件（映射表写在脚本里，`--check` 防漂移）：

| 文件 | 行数 | 内容 |
|---|---|---|
| `data/motion-lvgl.csv` | 17（**保留 13 / 裁掉 4**） | 触发翻译（hover→press、scroll→页面进入、错峰入场、内容层切换）· 曲线档 **C1 settle / C2 soft / C3 bounce / C4 snap / D1 pulse** · 时长（≤320 ms 封顶）· 属性（y/不透明度，**禁 transform**）· 守卫（不循环/并发 1）· `Source Row` 指回上游 |
| `data/colors-lvgl.csv` | 39 | 4 个选定 Product Type × 角色 → **RGB565** + 对背景对比度 + 可用性结论 + 本仓 token（强调色只取上游 `Accent`；`Primary/Secondary` 在暗色档里是深色块，实测对比度 1.22，标"（未采用）"） |
| `data/typography-lvgl.csv` | 74 | 上游字体配对 → **本板 5 档位图字号**的层级映射（字体名与 Google Fonts URL 丢弃） |
| `data/ui-reasoning-lvgl.csv` | 10 | 上游 192 个 web 页面类型 → **本仓 10 个页面**（主屏/主页宫格/工具板/读数/图表/轨迹/秒表/声学/AI 日志/设置）→ 风格档 + 色板 + 字号 + 密度 + 动效档 |

生成/校验：`python3 tools/phywear/gen_lvgl_adapt.py [--check]`（默认读写参赛仓内数据目录，可用 `--data` 覆盖）。

## 统计（导入实测，2026-09-16）

- `styles.csv`：**88 条**；自带 `Performance` 分布 **cost:low 62 / cost:moderate 19 / cost:high 7**
- `colors.csv`：**192 行 × 19 角色**，全部完成 RGB565 与对近黑底对比度换算
- 本队只从 cost:low 里挑，并逐条核对 `Effects & Animation` 是否撞上 EPIC 三个禁区（圆角/阴影/变换）

## 我们**不**采纳的部分（如实声明）

HTML/Tailwind/React/Vue/Next/Nuxt/Svelte/Flutter/SwiftUI/Jetpack Compose 的代码生成、
Google Fonts 动态导入与 CSS Import 列、Tailwind Config 列、响应式断点、浏览器交互（hover/focus/滚动）。
本板替换为：LVGL 9 样式属性 + EPIC 硬件加速约束 + 位图子集字体
（见 `app/phywear/skills/phywear-lvgl-ui.md` 与 `stacks/lvgl.csv`）。
