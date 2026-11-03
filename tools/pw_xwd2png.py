#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""pw_xwd2png.py —— 把 XWD 截图转成 PNG（本机没有 ImageMagick，也不想装）

为什么需要它（2026-09-18 实测的本机约束）：
  * 桌面是 **GNOME on Wayland**，Tk 程序跑在 XWayland 上；
  * `xwd -root` 抓整个屏幕会 **BadMatch**（XWayland 的根窗口不给抓）；
  * GNOME Shell 的截图 D-Bus 接口对普通程序回 **AccessDenied**；
  * `gnome-screenshot` / `grim` / `scrot` / ImageMagick **一个都没装**。
  唯一能走通的路是 `xwd -id <窗口ID>`（抓具体那个窗口是允许的），
  而 XWD 这个格式又不是 PNG —— PIL 也读不了它。
  所以这里用 60 行把 XWD 解出来交给 PIL 存成 PNG。

XWD 格式（X11 的 XWDFileHeader，全部大端 CARD32）：
  前 25 个字段是头，随后是 ncolors × 12 字节的调色板，再往后是像素数据，
  像素数据从 header_size 字节处开始。实测 XWayland 给的是
  bits_per_pixel=32 的 TrueColor（ZPixmap，LSBFirst），红绿蓝各自
  由 red_mask/green_mask/blue_mask 指出位置。

用法：
    python3 pw_xwd2png.py in.xwd out.png
    python3 pw_xwd2png.py --grab "PhyWear" out.png     # 按窗口标题找 ID 再抓
"""

import argparse
import os
import re
import struct
import subprocess
import sys
import tempfile

from PIL import Image

FIELDS = [
    "header_size", "file_version", "pixmap_format", "pixmap_depth",
    "pixmap_width", "pixmap_height", "xoffset", "byte_order",
    "bitmap_unit", "bitmap_bit_order", "bitmap_pad", "bits_per_pixel",
    "bytes_per_line", "visual_class", "red_mask", "green_mask",
    "blue_mask", "bits_per_rgb", "colormap_entries", "ncolors",
    "window_width", "window_height", "window_x", "window_y",
    "window_bdrwidth",
]
LSB_FIRST = 0


def parse_xwd(path):
    with open(path, "rb") as fh:
        blob = fh.read()

    if len(blob) < 100:
        raise ValueError("文件太小，不是 XWD")

    vals = struct.unpack(">25I", blob[:100])
    h = dict(zip(FIELDS, vals))

    if h["file_version"] != 7:
        raise ValueError(f"不支持的 XWD 版本 {h['file_version']}（只认 7）")
    if h["pixmap_format"] != 2:
        raise ValueError(f"只支持 ZPixmap(2)，实际 {h['pixmap_format']}")

    w, ht = h["pixmap_width"], h["pixmap_height"]
    bpp = h["bits_per_pixel"]
    stride = h["bytes_per_line"]
    off = h["header_size"] + h["ncolors"] * 12

    need = stride * ht
    if len(blob) < off + need:
        raise ValueError(f"像素数据不足：需要 {need} B，只有 {len(blob) - off} B")

    raw = blob[off:off + need]

    def mask_shift(mask):
        if mask == 0:
            return 0, 0
        shift = (mask & -mask).bit_length() - 1
        return shift, mask >> shift

    rs, rm = mask_shift(h["red_mask"])
    gs, gm = mask_shift(h["green_mask"])
    bs, bm = mask_shift(h["blue_mask"])

    # 每像素取成整数（按 byte_order 决定字节序）
    if bpp == 32:
        fmt = "<I" if h["byte_order"] == LSB_FIRST else ">I"
        step = 4
    elif bpp == 16:
        fmt = "<H" if h["byte_order"] == LSB_FIRST else ">H"
        step = 2
    elif bpp == 8:
        fmt = "B"
        step = 1
    else:
        raise ValueError(f"没实现 bits_per_pixel={bpp}")

    px = bytearray(w * ht * 3)
    for y in range(ht):
        row = raw[y * stride:y * stride + w * step]
        o = y * w * 3
        for x in range(w):
            v = struct.unpack_from(fmt, row, x * step)[0]
            # 掩码取值后拉到 8 bit（rm/gm/bm 是该通道的最大值）
            px[o + x * 3 + 0] = ((v & h["red_mask"]) >> rs) * 255 // rm if rm else 0
            px[o + x * 3 + 1] = ((v & h["green_mask"]) >> gs) * 255 // gm if gm else 0
            px[o + x * 3 + 2] = ((v & h["blue_mask"]) >> bs) * 255 // bm if bm else 0

    return Image.frombytes("RGB", (w, ht), bytes(px)), h


def find_window_id(title_substr):
    """按窗口标题子串找 X11 窗口 ID（Tk 会开两个：mutter 的框 + Tk 自己那个）。"""
    out = subprocess.run(["xwininfo", "-root", "-tree"],
                         capture_output=True, text=True).stdout
    cands = []
    for line in out.splitlines():
        if title_substr not in line:
            continue
        m = re.match(r"\s*(0x[0-9a-f]+)\s", line)
        if not m:
            continue
        # 优先选真正的 Tk 窗口（类名里带 "Tk"），不是 mutter 的外框
        cands.append((m.group(1), '"Tk"' in line))
    if not cands:
        return None
    for wid, is_tk in cands:
        if is_tk:
            return wid
    return cands[0][0]


def grab(title_substr, out):
    wid = find_window_id(title_substr)
    if not wid:
        print(f"找不到标题含 {title_substr!r} 的窗口", file=sys.stderr)
        return 1
    with tempfile.NamedTemporaryFile(suffix=".xwd", delete=False) as tmp:
        xwd = tmp.name
    try:
        r = subprocess.run(["xwd", "-id", wid, "-silent"],
                           stdout=open(xwd, "wb"), stderr=subprocess.PIPE)
        if r.returncode != 0:
            print("xwd 失败：" + r.stderr.decode("utf-8", "replace"),
                  file=sys.stderr)
            return 1
        img, h = parse_xwd(xwd)
        img.save(out)
        print(f"窗口 {wid} {h['pixmap_width']}x{h['pixmap_height']} "
              f"-> {out}")
        return 0
    finally:
        os.unlink(xwd)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("src", nargs="?", help="输入 .xwd")
    ap.add_argument("dst", nargs="?", help="输出 .png")
    ap.add_argument("--grab", metavar="TITLE",
                    help="不走输入文件：按窗口标题抓屏并直接存成 dst")
    args = ap.parse_args()

    if args.grab:
        # 注意：`--grab T out.png` 里那个 out.png 会被 argparse 当成位置参数 src，
        # 所以这里 src 也当 dst 用（第一版就是在这里报"--grab 需要给出输出路径"）。
        out = args.dst or args.src
        if not out:
            ap.error("--grab 需要给出输出 png 路径")
        return grab(args.grab, out)

    if not (args.src and args.dst):
        ap.error("需要 <输入.xwd> <输出.png>，或用 --grab")
    img, h = parse_xwd(args.src)
    img.save(args.dst)
    print(f"{h['pixmap_width']}x{h['pixmap_height']} bpp={h['bits_per_pixel']} "
          f"-> {args.dst}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
