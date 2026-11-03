#!/usr/bin/env python3
"""Collect the CJK code points that PhyWear actually renders.

Sources:
  * every string in `g_pw_str_zh[]` (phywear_i18n_tables.inc),
  * Chinese string literals passed directly to LVGL in the app sources
    (comments are stripped first, they are not rendered).

不参与渲染的文件被跳过（2026-09-18 加的）：
  * `*_blob.c` —— 自动生成的**数据块**（如 pw_skill_blob.c 内嵌整篇 skill
    markdown 的十六进制转义）。它的"字面量"是一篇文档的正文，
    实测一个文件就贡献 565 个汉字，占全部 725 个里的 78%；
    这些字永远不会画到屏上，却会让 5 个字号各多出上百 KB 字形位图。
  * `*_test_host.c` —— 主机侧单元测试，根本不进固件。

Prints the lv_font_conv `-r` range list, e.g. "0x3001-0x3002,0x4E00,...".
"""

import glob
import os
import re
import sys


def strip_comments(src):
    src = re.sub(r"/\*.*?\*/", " ", src, flags=re.S)
    return re.sub(r"//[^\n]*", " ", src)


# 只修饰别的字形、自己**没有独立字形**的格式字符。它们必须被排除：
#   * 变体选择符 U+FE00–U+FE0F（"⚠️" = U+26A0 + U+FE0F，后者就是这个块里的）
#   * 零宽空格/连接符/方向标记
# 为什么必须显式排除（2026-09-18 真踩）：harvest() 的判据是"码点 ≥ 0x2E80"，
# U+FE0F 恰好落在这个区间里，于是被当成一个"要渲染的字"塞进 lv_font_conv 的
# -r 列表；而 DroidSansFallback 根本没有这个字形 ⇒ lv_font_conv 报
#   Font "…/DroidSansFallbackFull.ttf" doesn't have any characters included
#   in range 0xfe0f-0xfe0f
# 然后**直接以退出码 1 结束**，五个字体一个都不生成。
# 触发它只需要在源码字面量里出现一个 "⚠️" —— 本项目的
# phywear_skill_blob.c（自动生成，内嵌 skill markdown 的十六进制转义）
# 里就有一个，所以整个字体流水线会莫名其妙地失败。
FORMAT_CHARS = set(range(0xFE00, 0xFE10)) | {
    0x200B, 0x200C, 0x200D, 0x200E, 0x200F, 0x2060, 0xFEFF,
}


def decode_c_literal(lit):
    out = bytearray()
    i = 0
    while i < len(lit):
        c = lit[i]
        if c == "\\" and i + 1 < len(lit):
            n = lit[i + 1]
            if n == "x" and re.match(r"[0-9a-fA-F]{2}", lit[i + 2:i + 4]):
                out.append(int(lit[i + 2:i + 4], 16))
                i += 4
                continue
            simple = {"n": 0x0A, "t": 0x09, '"': 0x22, "\\": 0x5C, "r": 0x0D}
            if n in simple:
                out.append(simple[n])
                i += 2
                continue
            i += 2
            continue

        o = ord(c)
        if o < 0x100:
            out.append(o)
        else:
            out.extend(c.encode("utf-8"))
        i += 1

    return out.decode("utf-8", "ignore")


def harvest(text, chars):
    for m in re.finditer(r'"((?:[^"\\]|\\.)*)"', text):
        for ch in decode_c_literal(m.group(1)):
            o = ord(ch)
            if o >= 0x2E80 and o not in FORMAT_CHARS:
                chars.add(o)


def main():
    app = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(
        os.path.abspath(__file__))
    chars = set()

    tables = os.path.join(app, "phywear_i18n_tables.inc")
    src = open(tables, encoding="utf-8", errors="ignore").read()
    harvest(src.split("static const char * const g_pw_str_zh[]", 1)[1], chars)

    for path in glob.glob(os.path.join(app, "*.c")) + \
            glob.glob(os.path.join(app, "*.h")):
        base = os.path.basename(path)
        if base.endswith("_blob.c") or base.endswith("_test_host.c"):
            continue
        harvest(strip_comments(open(path, encoding="utf-8",
                                    errors="ignore").read()), chars)

    points = sorted(chars)
    ranges = []
    start = prev = None
    for p in points:
        if start is None:
            start = prev = p
            continue
        if p == prev + 1:
            prev = p
            continue
        ranges.append((start, prev))
        start = prev = p
    if start is not None:
        ranges.append((start, prev))

    print(",".join("0x%X" % a if a == b else "0x%X-0x%X" % (a, b)
                   for a, b in ranges))
    print("%d CJK code points" % len(points), file=sys.stderr)


if __name__ == "__main__":
    main()
