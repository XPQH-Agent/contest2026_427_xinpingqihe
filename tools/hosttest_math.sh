#!/usr/bin/env bash
# tools/phywear/hosttest_math.sh
#
# SPDX-License-Identifier: Apache-2.0
#
# 主机单测：把 PhyWear 的纯算法库（pw_ahrs 姿态解算 / pw_calib 标定数学 / pw_traj 轨迹）
# 直接编到 PC 上跑各自的自检，不需要 NuttX、不需要板子。
#
# 为什么要有它：这两个库是 ③ 惯性标尺/标定的数学核心，改一版就烧一次板子
# 太慢；在 PC 上跑（本脚本 <2s）可以把"实现是否正确"和"参数好不好"分开定位。
# 真机侧对应 `phywear ahrs` / `phywear calib` 两条子命令，同样的自检。
#
# 用法：
#   bash tools/phywear/hosttest_math.sh            # 编译并跑
#   CC=clang bash tools/phywear/hosttest_math.sh   # 换编译器

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WS="${PHYWEAR_WS:-$(cd "$HERE/../.." && pwd)}"
APP="$WS/apps/examples/phywear"
CC="${CC:-gcc}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

for f in pw_ahrs.c pw_calib.c pw_traj.c pw_motion.c pw_motion_table.c; do
  if [ ! -f "$APP/$f" ]; then
    echo "找不到算法库：$APP/$f" >&2
    exit 1
  fi
done

# 桩头文件：真实构建里 nuttx/config.h 由 NuttX 生成，这两个库并不使用其中任何宏
mkdir -p "$TMP/nuttx"
cat > "$TMP/nuttx/config.h" <<'EOF'
/* host 单测桩：真实构建由 NuttX 生成 */
#ifndef __PHYWEAR_HOST_STUB_CONFIG_H
#define __PHYWEAR_HOST_STUB_CONFIG_H
#endif
EOF

echo "[hosttest] CC=$CC"
"$CC" -O2 -Wall -Wextra -I"$TMP" -I"$APP" \
  -o "$TMP/pw_math_hosttest" \
  "$HERE/pw_math_hosttest.c" \
  "$APP/pw_ahrs.c" \
  "$APP/pw_calib.c" \
  "$APP/pw_traj.c" \
  "$APP/pw_motion.c" \
  "$APP/pw_motion_table.c" \
  -lm

"$TMP/pw_math_hosttest"
