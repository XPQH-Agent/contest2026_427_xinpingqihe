#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把上游 ui-ux-pro-max 的**网页数据**适配成**本板 LVGL 原生数据**（4 个文件）。

为什么需要（2026-09-16）：上游数据是给浏览器写的 —— `motion.csv` 的 Trigger 只有
hover / scroll / route change，Easing 是 GSAP 曲线名，还带 `GSAP Snippet` 列；
`colors.csv` 是 web hex 色板；`typography.csv` 是 Google Fonts 配对；`ui-reasoning.csv`
的 UI_Category 是 web 页面类型。只做"文档里的散文映射"不算适配，本脚本产出**可复现的 LVGL 数据文件**。

生成（写入上游数据同目录，便于对照）：
    data/motion-lvgl.csv        17 条动效 → 本板触发/曲线档/时长/属性/守卫
    data/colors-lvgl.csv        选定 Product Type 的角色色 → RGB565 + 对比度 + 本仓 token
    data/typography-lvgl.csv    配对 → 本板 5 档位图字号的层级映射
    data/ui-reasoning-lvgl.csv  上游 UI_Category → 本仓页面类型 → 风格/色板/字号/密度/动效

用法：
    python3 tools/phywear/gen_lvgl_adapt.py            # 生成
    python3 tools/phywear/gen_lvgl_adapt.py --check    # 只校验是否与上游一致（防漂移）

**所有映射表都写在本文件里**，改映射=改这张表，然后重跑；生成物不要手改。
硬约束（来自本板实测，见 third_party/ui-ux-pro-max/stacks/lvgl.csv）：
单次动效 ≤320ms、只改几何或不透明度、不循环、同屏并发 ≤1、不碰圆角/阴影/transform。
"""

import argparse
import csv
import io
import os
import sys

# 上游数据（以及本脚本的生成物）都在**参赛仓**里，不在工作区。
# 与 sync_back.py 的约定一致：可用 PHYWEAR_REPO 覆盖，或用 --data 直接指定。
REPO = os.environ.get("PHYWEAR_REPO",
                      os.path.join(os.path.expanduser("~"), "work",
                                   "contest2026_427_xinpingqihe"))
DATA = os.environ.get("PHYWEAR_DATA",
                      os.path.join(REPO, "third_party", "ui-ux-pro-max", "data"))

# ── 触发方式：网页 → 本板（无 hover / 无滚动 / 无鼠标 / 无路由）─────────────
TRIGGER_MAP = {
    "hover": "press（手指按下）",
    "hover + mousemove": "press（手指按下）",
    "scroll (viewport enter)": "页面/子页进入",
    "load or scroll": "宫格/列表入场（错峰）",
    "route change": "屏幕切换（只动内容层）",
    "on mount / async wait": "进度反馈（一次性不循环）",
    "scroll (continuous)": "裁掉（本板无滚动）",
    "scroll (continuous scrub)": "裁掉（本板无滚动）",
    "timer / focus / hover / visibility": "裁掉（本板无轮播/无焦点态）",
}

# ── 缓动：GSAP → 本队查表曲线档（曲线参数见 tools/phywear/gen_motion_table.py）──
EASING_MAP = {
    "power1.out": ("C1 settle", "ζ=0.70 ω=12（无过冲）"),
    "power2.out": ("C1 settle", "ζ=0.70 ω=12（无过冲）"),
    "power1.inOut": ("C1 settle", "ζ=0.70 ω=12（无过冲）"),
    "power2.inOut": ("C1 settle", "ζ=0.70 ω=12（无过冲）"),
    "expo.out": ("C4 snap", "ζ=0.90 ω=16（短促）"),
    "expo.inOut": ("C4 snap", "ζ=0.90 ω=16（短促）"),
    "back.out(1.4)": ("C2 soft", "ζ=0.45 ω=12（过冲≈20%）"),
    "elastic.out(1,0.4)": ("C3 bounce", "ζ=0.25 ω=14（过冲≈45%）"),
    "sine.inOut": ("D1 pulse", "decay a=6 f=2（对称振荡，一次性）"),
    "linear (scrub)": ("—", "裁掉"),
    "none": ("—", "裁掉"),
    "none (scrub-driven)": ("—", "裁掉"),
}

# ── 属性：按上游 Category 决定动什么（本板只允许几何/不透明度）────────────────
PROPERTY_MAP = {
    "Hover Micro-interaction": "y ±2 px + 背景色（禁 transform/scale）",
    "Scroll Reveal": "y（+10 px → 0），可叠不透明度",
    "Stagger List": "y（+10 px → 0），逐块起始错峰 +50 ms",
    "Page Transition": "y 或不透明度（只动内容层，不做整屏转场）",
    "Loading / Skeleton": "不透明度（进度变化时淡入）",
    "Parallax Scroll": "裁掉",
    "Carousel / Auto-Rotation": "裁掉",
}

# ── 本仓页面 → 上游 UI_Category（192 个里挑对得上的）────────────────────────
PAGE_MAP = [
    ("手表主屏", "Alarm & World Clock", "Flat Design", "colors.csv: dark 档", "时间 XL(28) / 日期 BODY(14)", "中", "刻度+指针"),
    ("主页宫格入口", "Smart Home/IoT Dashboard", "Bento Box Grid（只取网格，砍圆角阴影缩放）", "colors.csv: dark 档", "标题 LARGE(24) / tile MED(20) / 说明 BODY(14)", "2×4", "宫格"),
    ("工具板", "Calculator & Unit Converter", "Flat Design + Minimal & Direct", "同主页", "行项 SMALL(16)", "松", "列表"),
    ("读数页（原始传感器/水平仪）", "Financial Dashboard", "Data-Dense Dashboard", "colors.csv: Analytics 档", "数值 MED(20) / 单位 BODY(14)", "紧凑", "高密度网格"),
    ("图表页（曲线/频谱）", "Analytics Dashboard", "Real-Time Monitoring", "同读数页", "数值 MED / 标注 BODY", "中", "10–15 Hz 重绘"),
    ("轨迹页", "Running & Cycling GPS", "Real-Time Monitoring + Data-Dense", "同读数页", "数值 MED / 诚实标注 BODY", "中", "10 Hz 图线"),
    ("秒表/计时", "Timer & Pomodoro", "Flat Design", "colors.csv: Productivity 档", "计时 XL(28)", "松", "大数字"),
    ("声学页", "White Noise & Ambient Sound", "Flat Design", "colors.csv: dark 档", "数值 MED", "中", "波形"),
    ("AI 教练/日志页", "Educational App", "Minimal & Direct", "同主页", "正文 BODY / 标题 LARGE", "松", "文本为主"),
    ("设置/关于", "Productivity Tool", "Flat Design", "同主页", "行项 SMALL(16)", "松", "列表"),
]

# ── 色板：本板选定的 Product Type（对应上表用到的档）────────────────────────
PALETTE_PICKS = ["Smart Home/IoT Dashboard", "Analytics Dashboard", "Financial Dashboard", "Timer & Pomodoro"]

# ── 字号：上游配对 → 本板 5 档位图字号 ────────────────────────────────────
SIZE_MAP = {
    "Display": ("XL(28)", "BODY(14)"),
    "Heading": ("LARGE(24)", "BODY(14)"),
    "Subheading": ("MED(20)", "SMALL(16)"),
    "Body": ("MED(20)", "BODY(14)"),
    "Caption": ("SMALL(16)", "SMALL(16)"),
}


def read(name):
    p = os.path.join(DATA, name)
    with io.open(p, encoding="utf-8", errors="replace", newline="") as f:
        return list(csv.DictReader(f))


def dur_ok(s: str):
    """上游时长字符串 → 本板取值（取下界并封顶 320ms）；不可映射返回 None。"""
    s = (s or "").strip().lower()
    if not s or not s[0].isdigit():
        return None
    lo = int("".join(ch for ch in s.split("-")[0] if ch.isdigit()) or 0)
    if lo <= 0:
        return None
    return min(lo, 320)


def gen_motion():
    rows = read("motion.csv")
    out = []
    for i, r in enumerate(rows, 1):
        cat = (r.get("Category") or "").strip()
        tier = (r.get("Intensity Tier") or "").strip()
        trig = (r.get("Trigger") or "").strip()
        eas = (r.get("Easing") or "").strip()
        ms = dur_ok(r.get("Duration"))
        curve, params = EASING_MAP.get(eas, ("—", "未映射（保守：不做动效）"))
        board_trig = TRIGGER_MAP.get(trig, "未映射（保守：不做动效）")
        prop = PROPERTY_MAP.get(cat, "未映射")
        dropped = board_trig.startswith("裁掉") or curve == "—"
        if dropped:
            ms = 0
        out.append({
            "No": i,
            "Upstream Category": cat,
            "Intensity Tier": tier,
            "Trigger (upstream)": trig,
            "Trigger (LVGL board)": board_trig,
            "Curve": curve,
            "Curve Params": params,
            "Duration (ms)": ms if ms else "—",
            "Property": prop,
            "Guard": "≤320ms；不循环；同屏并发≤1；禁 transform/圆角/阴影" if not dropped else "—",
            "Source Row": r.get("No", ""),
            "Status": "dropped" if dropped else ("adapted" if curve != "—" else "needs-mapping"),
        })
    return out, ["No", "Upstream Category", "Intensity Tier", "Trigger (upstream)",
                 "Trigger (LVGL board)", "Curve", "Curve Params", "Duration (ms)",
                 "Property", "Guard", "Source Row", "Status"]


def hex2rgb565(h):
    h = (h or "").strip().lstrip("#")
    if len(h) != 6:
        return None
    try:
        r, g, b = (int(h[i:i + 2], 16) for i in (0, 2, 4))
    except ValueError:
        return None
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3), r, g, b


def lum(r, g, b):
    def f(c):
        c /= 255.0
        return c / 12.92 if c <= 0.03928 else ((c + 0.055) / 1.055) ** 2.4
    return 0.2126 * f(r) + 0.7152 * f(g) + 0.0722 * f(b)


def contrast(a, b):
    la, lb = lum(*a), lum(*b)
    hi, lo = max(la, lb), min(la, lb)
    return (hi + 0.05) / (lo + 0.05)


# 角色 → 本仓 token。**注意**：上游的 Primary/Secondary 在暗色档里往往就是"深色块"
# （实测 Smart Home 档 Primary=#1E293B，对比度仅 1.22），**不能当强调色用**；
# 强调色只取上游的 Accent 角色，其余按对比度给"仅装饰/背景"结论并标（未采用）。
TOKEN = {"Background": "PW_COL_BG", "Foreground": "PW_COL_TEXT", "Card": "PW_COL_CARD",
         "Card Foreground": "PW_COL_TEXT", "Muted": "PW_COL_CARD_LT",
         "Muted Foreground": "PW_COL_DIM", "Border": "PW_COL_GRID",
         "Accent": "PW_ACC_RAW", "Primary": "（未采用）", "Secondary": "（未采用）"}


def gen_colors():
    rows = read("colors.csv")
    out = []
    for r in rows:
        pt = (r.get("Product Type") or "").strip()
        if pt not in PALETTE_PICKS:
            continue
        bg = hex2rgb565(r.get("Background"))
        for role, tok in TOKEN.items():
            v = hex2rgb565(r.get(role))
            if not v:
                continue
            c565, rr, gg, bb = v
            ct = contrast((rr, gg, bb), (bg[1], bg[2], bg[3])) if bg else 0.0
            out.append({
                "Product Type": pt, "Role": role, "Hex": r.get(role, "").strip(),
                "RGB565": "0x%04X" % c565,
                "Contrast vs BG": "%.2f" % ct,
                "Use": "正文可用" if ct >= 4.5 else ("大字/次要可用" if ct >= 3.0 else "仅装饰/背景"),
                "Token": tok if (tok != "（未采用）" or role in ("Primary", "Secondary")) else tok,
            })
    return out, ["Product Type", "Role", "Hex", "RGB565", "Contrast vs BG", "Use", "Token"]


def gen_typography():
    rows = read("typography.csv")
    out = []
    for r in rows:
        cat = (r.get("Category") or "").strip()
        head, body = SIZE_MAP.get(cat, ("MED(20)", "BODY(14)"))
        out.append({
            "Pairing": (r.get("Font Pairing Name") or "").strip(),
            "Category": cat,
            "Upstream Heading Font": (r.get("Heading Font") or "").strip(),
            "Upstream Body Font": (r.get("Body Font") or "").strip(),
            "LVGL Heading Size": head,
            "LVGL Body Size": body,
            "Hierarchy Intent": (r.get("Mood/Style Keywords") or "").strip()[:60],
            "Dropped": "字体名与 Google Fonts URL 裁掉（本板为位图子集）",
        })
    return out, ["Pairing", "Category", "Upstream Heading Font", "Upstream Body Font",
                 "LVGL Heading Size", "LVGL Body Size", "Hierarchy Intent", "Dropped"]


def gen_reasoning():
    ui = {(r.get("UI_Category") or "").strip(): r for r in read("ui-reasoning.csv")}
    out = []
    for page, cat, style, pal, typo, dense, note in PAGE_MAP:
        r = ui.get(cat, {})
        out.append({
            "Our Page": page,
            "Upstream UI_Category": cat,
            "Upstream Pattern": (r.get("Recommended_Pattern") or "").strip()[:50],
            "Style (LVGL)": style,
            "Palette Source": pal,
            "Typography": typo,
            "Density": dense,
            "Motion Tier": "C2 入场 + C4 按压（见 motion-lvgl.csv）",
            "Notes": note,
            "Status": "adapted" if r else "upstream-category-missing",
        })
    return out, ["Our Page", "Upstream UI_Category", "Upstream Pattern", "Style (LVGL)",
                 "Palette Source", "Typography", "Density", "Motion Tier", "Notes", "Status"]


JOBS = [("motion-lvgl.csv", gen_motion), ("colors-lvgl.csv", gen_colors),
        ("typography-lvgl.csv", gen_typography), ("ui-reasoning-lvgl.csv", gen_reasoning)]


def dump(path, header, rows):
    with io.open(path, "w", encoding="utf-8", newline="") as f:
        w = csv.DictWriter(f, fieldnames=header, lineterminator="\n")
        w.writeheader()
        w.writerows(rows)


def main():
    global DATA
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--data", default=None, help="上游数据目录（默认参赛仓内）")
    a = ap.parse_args()
    if a.data:
        DATA = a.data
    if not os.path.isdir(DATA):
        print("❌ 找不到上游数据目录：%s（用 --data 指定）" % DATA)
        return 2
    bad = 0
    for name, fn in JOBS:
        rows, header = fn()
        p = os.path.join(DATA, name)
        buf = io.StringIO()
        w = csv.DictWriter(buf, fieldnames=header, lineterminator="\n")
        w.writeheader()
        w.writerows(rows)
        want = buf.getvalue()
        if a.check:
            have = io.open(p, encoding="utf-8").read() if os.path.exists(p) else ""
            if have != want:
                print("❌ %s 与上游数据不一致（重跑本脚本）" % name)
                bad += 1
            else:
                print("✅ %s 一致（%d 行）" % (name, len(rows)))
            continue
        dump(p, header, rows)
        kept = sum(1 for r in rows if r.get("Status") != "dropped")
        extra = "（保留 %d / 裁掉 %d）" % (kept, len(rows) - kept) if "Status" in header else ""
        print("已生成 %-24s %4d 行  %s" % (name, len(rows), extra))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
