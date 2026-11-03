#!/usr/bin/env python3
# tools/phywear/make_storyboard.py
#
# SPDX-License-Identifier: Apache-2.0
#
# 生成"演示视频分镜静帧对照表"（contact sheet）：把已采到的真机截图与关键串口日志
# 排成一张图，供实拍时照着走。**每张素材都标注来源（真机 / 模拟器 / 串口日志）**，
# 避免把模拟器画面当真机、把日志当截图。
#
# 用法：
#   python3 tools/phywear/make_storyboard.py --repo ~/work/contest2026_427_xinpingqihe \
#       --out <repo>/docs/evidence/demo-20260915/storyboard.png
#
# 依赖：Pillow + 任一 CJK 字体（默认找 Noto Serif CJK）。

import argparse
import os
import sys

from PIL import Image, ImageDraw, ImageFont

CJK_CANDIDATES = [
    "/usr/share/fonts/opentype/noto/NotoSerifCJK-Bold.ttc",
    "/usr/share/fonts/opentype/noto/NotoSerifCJK-Regular.ttc",
    "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
]

BG = (22, 24, 29)
FG = (232, 234, 240)
DIM = (150, 156, 168)
ACCENT = (120, 190, 255)
WARN = (255, 190, 90)

TW, TH = 300, 400          # tile image area
LAB = 74                   # label strip height
COLS = 3
PAD = 14


def find_font(size):
    for p in CJK_CANDIDATES:
        if os.path.exists(p):
            return ImageFont.truetype(p, size)
    return ImageFont.load_default()


def wrap(draw, text, font, maxw):
    lines = []
    for para in text.split("\n"):
        cur = ""
        for ch in para:
            if draw.textlength(cur + ch, font=font) <= maxw:
                cur += ch
            else:
                lines.append(cur)
                cur = ch
        lines.append(cur)
    return lines


def text_tile(title, src, body, font_t, font_b, color=FG):
    """把一段日志/数字做成卡片（不是截图，必须标"串口日志"）"""
    img = Image.new("RGB", (TW, TH), (30, 33, 40))
    d = ImageDraw.Draw(img)
    d.rectangle([0, 0, TW - 1, TH - 1], outline=(60, 66, 78))
    d.text((12, 10), title, font=font_t, fill=color)
    y = 34
    for line in wrap(d, body, font_b, TW - 24):
        if y > TH - 18:
            break
        d.text((12, y), line, font=font_b, fill=FG)
        y += 20
    return img


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", default=os.path.expanduser("~/work/contest2026_427_xinpingqihe"))
    ap.add_argument("--out", required=True)
    a = ap.parse_args()

    ev = os.path.join(a.repo, "docs", "evidence")
    f_t = find_font(19)
    f_b = find_font(15)
    f_l = find_font(14)
    f_h = find_font(22)

    def shot(rel):
        return os.path.join(ev, rel)

    tiles = []

    def add_image(path, title, src, tone=FG):
        if not os.path.exists(path):
            tiles.append((text_tile(title, src, "（素材缺失：" + os.path.basename(path) + "）",
                                    f_t, f_b, WARN), title, src, tone))
            return
        im = Image.open(path).convert("RGB")
        im.thumbnail((TW, TH))
        canvas = Image.new("RGB", (TW, TH), (16, 18, 22))
        canvas.paste(im, ((TW - im.width) // 2, (TH - im.height) // 2))
        tiles.append((canvas, title, src, tone))

    def add_text(title, src, body, tone=WARN):
        tiles.append((text_tile(title, src, body, f_t, f_b, tone), title, src, tone))

    # ---- 第 2 段：界面与文案 ----
    add_image(shot("realboard-20260912/00_root.png"), "主菜单（真机）", "真机截图")
    add_image(shot("ui-batch2-20260915/rev-real-home.png"),
              "主页文案已修：6 页 / 2+2（真机）", "真机截图", ACCENT)
    add_image(shot("realboard-20260912/01_raw.png"), "原始传感器页（真机）", "真机截图")

    # ---- 第 3 段：双视图 ----
    add_image(shot("ui-batch2-20260915/rev-real-raw-num.png"),
              "① 默认数值视图（真机）", "真机截图", ACCENT)
    add_image(shot("ui-batch2-20260915/rev-real-raw-curve.png"),
              "① 轻点后图线视图（真机）", "真机截图", ACCENT)
    add_image(shot("ui-batch2-20260915/rev-sim-both-views.png"),
              "① 两视图并排（模拟器）", "模拟器截图", WARN)

    # ---- 第 4 段：指针盘 / 秒表环（当前只有模拟器素材）----
    add_image(shot("ui-batch2-20260915/sim-incline-dial.png"),
              "倾角指针盘（模拟器）", "模拟器截图", WARN)
    add_image(shot("ui-batch2-20260915/sim-stopwatch-ring.png"),
              "秒表环形进度（模拟器）", "模拟器截图", WARN)
    add_image(shot("realboard-20260912/02_pendulum.png"), "单摆测 g（真机）", "真机截图")

    # ---- 第 6 段：主动场景（真机串口日志）----
    add_text("⑤-1 主动 + 执行（真机串口日志）", "串口日志 · 非截图",
             "检测到持续摆动（窗口振幅 0.54 g，持续 2.5 s）\n"
             "[tools] Executing tool: phywear_run_experiment\n"
             "ai-coach: AI: 已自动测重力加速度:\n"
             "          g = 6.09 m/s^2 (T = 1.800 s)\n\n"
             "静止时如实回：\n"
             "AI: no experiment result\n"
             "(device not moved, ...)")

    # ---- 第 7 段：姿态 ----
    add_text("③ 姿态解算（真机串口日志）", "串口日志 · 非截图",
             "roll=+178.12 pitch=+23.63 yaw=+75.63\n"
             "accel-tilt r=+178.74 p=+21.94\n"
             "→ 两条独立路径一致 ~0.5°\n\n"
             "mag 关：yaw ~2°/s 漂（零偏 −2.18 dps）\n"
             "mag 开：yaw 稳定 75.6° 不漂")

    # ---- 第 8 段：性能与内存 ----
    add_text("性能 / 内存（真机实测）", "真机实测数字",
             "LVGL benchmark 41 FPS (render 22)\n"
             "原始页 fps 12 / loops 57（两个视图同）\n"
             "倾角 25 · 秒表 9 · 单摆 8~16\n\n"
             "固件 2,076,244 B (flash 12.4%)\n"
             "SRAM 491,576 B / 93.76%\n"
             "累计新增 +2,136 B（超 ~2 KB 额度 88 B）")

    # ---- 第 9 段：蓝牙结论 ----
    add_text("④ 蓝牙探针：no-go", "结论 · 非截图",
             "片上控制器(LCPU)与协议栈都在，\n"
             ".config 里 107 个 BT 项已开，\n"
             "但固件里只有 2 个 BT 符号。\n\n"
             "断点：UART_BTH4 未开 ·\n"
             "NET_BLUETOOTH 未开 ·\n"
             "port 的 h4_uart.c 无 CMakeLists\n"
             "→ 链接缺 ttyHCI0 初始化入口")

    # ---- 排版 ----
    rows = (len(tiles) + COLS - 1) // COLS
    cellw = TW + PAD
    cellh = TH + LAB + PAD
    W = COLS * cellw + PAD
    H = 92 + rows * cellh + PAD
    sheet = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(sheet)
    d.text((PAD + 4, 18), "PhyWear 演示视频 · 分镜静帧对照表（v4 / 2026-09-15）",
           font=f_h, fill=FG)
    d.text((PAD + 4, 52),
           "⚠️ 标注「模拟器」的帧不是真机；标注「串口日志」的是文字证据不是画面。"
           "实拍请替换为手机拍摄的真机画面。",
           font=f_l, fill=WARN)

    for i, (img, title, src, tone) in enumerate(tiles):
        r, c = divmod(i, COLS)
        x = PAD + c * cellw
        y = 92 + r * cellh
        sheet.paste(img, (x, y))
        d.text((x + 2, y + TH + 4), title, font=f_l, fill=tone)
        d.text((x + 2, y + TH + 24), "来源：" + src, font=f_l, fill=DIM)
        d.text((x + 2, y + TH + 44), "第 %d 格" % (i + 1), font=f_l, fill=DIM)

    os.makedirs(os.path.dirname(a.out), exist_ok=True)
    sheet.save(a.out)
    print("storyboard: %s  (%dx%d, %d tiles)" % (a.out, W, H, len(tiles)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
