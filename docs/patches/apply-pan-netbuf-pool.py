#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把应用侧 pw_pan_pool 注册进 zblue NuttX port 的 _net_buf_pool_list[]。

背景（181 队同板复盘 + 我方逐行核查）：
  zblue 的 NuttX port 不用 Zephyr 的 linker section 收集 net_buf 池，而是手写数组
  external/zblue/zblue/port/sections/defines.c:162 起的 _net_buf_pool_list[]。
  pool_id() 反查不到时 __ASSERT 在 release 下被编掉 → 静默 return 0 →
  该 pool 的 buffer 被当成 _net_buf_pool_list[0]（discardable_pool，258B）的成员：
  unref 时尺寸与 data_pool 基址全错 → 内存污染。

  我们的 apps/examples/phywear/pw_bnep.c:54 用自己的池：
      NET_BUF_POOL_FIXED_DEFINE(pw_pan_pool, 4, BT_L2CAP_BUF_SIZE(1600), 8, NULL);
  :138 net_buf_alloc(&pw_pan_pool, ...)，但数组里没有它 → "pan on" 即腐败。

用法：
  python3 apply_pool_register.py --check
  python3 apply_pool_register.py --apply
  python3 apply_pool_register.py --revert
  python3 apply_pool_register.py --tree repo|build|all
"""
import argparse
import os
import sys

REL = "external/zblue/zblue/port/sections/defines.c"

EXTERN_ANCHOR = "extern struct net_buf_pool bt_a2dp_tx_pool; // for openvela sal"
EXTERN_ADD = (
    EXTERN_ANCHOR
    + "\n"
    + "extern struct net_buf_pool pw_pan_pool;   /* phywear PANU 应用池 */"
)

ENTRY = (
    "#if defined(CONFIG_EXAMPLES_PHYWEAR)\n"
    "\t&pw_pan_pool,\n"
    "#endif /* CONFIG_EXAMPLES_PHYWEAR */\n"
)

ARRAY_TAIL_ANCHOR = "\tNULL,\n};"
ARRAY_START = "_net_buf_pool_list[]"


def _insert_into_array(src):
    """在 _net_buf_pool_list[] 的结尾 NULL 之前插入 ENTRY（只认该数组内的第一个）。"""
    i = src.find(ARRAY_START)
    if i < 0:
        return None, "找不到 _net_buf_pool_list[] 定义"
    j = src.find(ARRAY_TAIL_ANCHOR, i)
    if j < 0:
        return None, "数组内找不到结尾 NULL"
    return src[:j] + ENTRY + src[j:], "ok"


def _remove_from_array(src):
    i = src.find(ARRAY_START)
    if i < 0:
        return None, "找不到 _net_buf_pool_list[] 定义"
    j = src.find(ENTRY + ARRAY_TAIL_ANCHOR, i)
    if j < 0:
        return None, "数组内找不到我们插入的 ENTRY"
    return src[:j] + ARRAY_TAIL_ANCHOR + src[j + len(ENTRY) + len(ARRAY_TAIL_ANCHOR):], "ok"


def patch_once(path, applied):
    src = open(path, encoding="utf-8", errors="surrogateescape").read()
    if applied:
        if EXTERN_ANCHOR not in src:
            return src, "extern 锚点未找到"
        if "pw_pan_pool" in src:
            return src, "文件里已经出现 pw_pan_pool，拒绝重复插入"
        src2 = src.replace(EXTERN_ANCHOR, EXTERN_ADD, 1)
        out, msg = _insert_into_array(src2)
        if out is None:
            return src, msg
        return out, "ok"
    else:
        if EXTERN_ADD not in src or ENTRY not in src:
            return src, "未找到已应用的补丁"
        out, msg = _remove_from_array(src)
        if out is None:
            return src, msg
        return out.replace(EXTERN_ADD, EXTERN_ANCHOR, 1), "ok"


def trees(which):
    out = []
    if which in ("repo", "all"):
        out.append(os.path.expanduser("~/work/contest2026_427_xinpingqihe"))
    if which in ("build", "all"):
        out.append(os.path.expanduser("~/openvela"))
    return out


def main():
    ap = argparse.ArgumentParser()
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--check", action="store_true")
    g.add_argument("--apply", action="store_true")
    g.add_argument("--revert", action="store_true")
    ap.add_argument("--tree", default="all", choices=["repo", "build", "all"])
    a = ap.parse_args()

    rc = 0
    for root in trees(a.tree):
        path = os.path.join(root, REL)
        if not os.path.exists(path):
            print("[MISS] %s" % path)
            rc = 1
            continue
        src = open(path, encoding="utf-8", errors="surrogateescape").read()
        has_extern = EXTERN_ADD in src
        has_entry = ENTRY in src
        has_pool_ref = "&pw_pan_pool," in src

        if a.check:
            state = "已注册" if (has_extern and has_entry) else "未注册"
            print("[%s] %s  -> %s (extern=%s entry=%s)" %
                  ("OK" if state == "未注册" else "WARN", path, state,
                   has_extern, has_entry))
            if has_pool_ref and not has_entry:
                print("      注意：&pw_pan_pool 出现但不在我们插入的形式里，人工确认")
            continue

        if a.apply:
            new, msg = patch_once(path, applied=True)
            if msg != "ok":
                print("[FAIL] %s : %s" % (path, msg))
                rc = 1
                continue
            if new == src:
                print("[SKIP] %s 已是目标状态" % path)
                continue
            open(path, "w", encoding="utf-8", errors="surrogateescape").write(new)
            print("[APPLIED] %s (+2 处：extern 声明、数组项)" % path)
        else:
            new, msg = patch_once(path, applied=False)
            if msg != "ok":
                print("[FAIL] %s : %s" % (path, msg))
                rc = 1
                continue
            open(path, "w", encoding="utf-8", errors="surrogateescape").write(new)
            print("[REVERTED] %s" % path)
    return rc


if __name__ == "__main__":
    sys.exit(main())