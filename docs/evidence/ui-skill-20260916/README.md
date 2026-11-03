# UI/UX Skill → LVGL 适配证据（2026-09-16）

## 1. 适配到什么程度（四层）

| 层 | 产物 | 状态 |
|---|---|---|
| 数据层（**本轮补齐**） | `data/{motion,colors,typography,ui-reasoning}-lvgl.csv` —— 由 `tools/phywear/gen_lvgl_adapt.py` 从上游数据可复现生成 | ✅ |
| 规则层 | `stacks/lvgl.csv`（14 条：11 verified + 3 needs-verify） | ✅ |
| 筛选器 | `tools/phywear/import_ui_skill.py`（按上游 Performance/Effects/Complexity 三列判负载） | ✅ |
| 落地层（UI 实际改版） | `docs/12` 的 P1–P3 | ⏳ 待执行 |

## 2. 本轮生成结果（数字）

- `motion-lvgl.csv`：上游 **17 条** → **保留 13 / 裁掉 4**
  （裁掉：`scroll (continuous scrub)` ×1、`Parallax Scroll` ×2、`Carousel/Auto-Rotation` ×1 —— 本板无滚动、无轮播、禁循环动画）
- `colors-lvgl.csv`：**39 行**（4 个 Product Type × 角色），全部带 RGB565 与对背景对比度
- `typography-lvgl.csv`：**74 行**，全部映射到本板 5 档位图字号
- `ui-reasoning-lvgl.csv`：**10 行**，把上游 192 个 web 页面类型收敛到本仓 10 个页面

## 3. 两处如实修正（都是本轮实测发现的）

1. **强调色不能取上游 `Primary`**：暗色档里 `Primary` 就是深色块（Smart Home 档 `#1E293B`，
   对底对比度仅 **1.22:1**），当强调色会糊成一片。改为**只取 `Accent`**（该档 `#22C55E`，**7.83:1** ✓），
   `Primary/Secondary` 标"（未采用）"。
2. **导入器不能按整行文本判负载**（上一轮）：会把 `Dark Mode (OLED)`/`Bento Box Grid`/`Flat Design`
   误判为禁用 → 改为按 `Effects & Animation` + `Performance` + `Complexity` 字段级判定。

## 4. 上游数据的"网页残留"（适配时逐项处理，未带入）

`Trigger` 里的 hover/scroll/route change、`Easing` 的 GSAP 名（`back.out(1.4)`/`expo.inOut`）、
`GSAP Snippet` 列、`Framework Notes` 的浏览器提示、`google-fonts.csv`、`icons.csv`（SVG）、
`react-performance.csv`、响应式断点与 hover/focus 态 —— 全部丢弃或翻译，见上述 4 个 `*-lvgl.csv`。
