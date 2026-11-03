#!/bin/bash
# install_desktop_shortcut.sh —— 在桌面放一个「PhyWear 蓝牙测试」图标（点一下就测）
#
# 为什么做成脚本而不是直接把 .desktop 提交进仓：
#   .desktop 里的 Exec 必须是**绝对路径**，而每个人的工作区路径不一样
#   （本机是 /home/xpqh/openvela）。提交一份写死路径的文件，换台机器就是死的。
#   所以这里按当前机器实际情况生成，重复执行是幂等的。
#
# 用法：
#   bash install_desktop_shortcut.sh              # 装到桌面
#   bash install_desktop_shortcut.sh --uninstall  # 删掉
#
# 本机实测（2026-09-18）：Ubuntu GNOME on Wayland。装完还需要把文件标成"可信任"，
# 否则 GNOME 双击会弹"不受信任的应用程序" —— 这里用 gio 设置 metadata::trusted，
# 并在末尾把怎么手动点「Allow Launching」也打出来（万一 gio 那条被忽略）。
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
GUI="$HERE/pw_bt_gui.py"

# 桌面目录：中文优先，其次英文
DESK=""
for d in "$HOME/桌面" "$HOME/Desktop"; do
  [ -d "$d" ] && { DESK="$d"; break; }
done
[ -n "$DESK" ] || { echo "找不到桌面目录（~/桌面 或 ~/Desktop）"; exit 1; }

NAME="PhyWear蓝牙测试.desktop"
TARGET="$DESK/$NAME"

if [ "${1:-}" = "--uninstall" ]; then
  rm -f "$TARGET"
  echo "已删除 $TARGET"
  exit 0
fi

[ -f "$GUI" ] || { echo "缺少 $GUI"; exit 1; }
command -v python3 >/dev/null || { echo "缺少 python3"; exit 1; }

# 依赖自检：GUI 要 tkinter，测蓝牙要 dbus-python。缺了先告诉人，别等点了没反应。
python3 -c "import tkinter" 2>/dev/null || {
  echo "⚠️  没有 tkinter：sudo apt install -y python3-tk"; exit 1; }
python3 -c "import dbus" 2>/dev/null || {
  echo "⚠️  没有 dbus-python：sudo apt install -y python3-dbus"; exit 1; }

chmod +x "$GUI"

cat > "$TARGET" <<EOF
[Desktop Entry]
Type=Application
Version=1.0
Name=PhyWear BT Test
Name[zh_CN]=PhyWear 蓝牙测试
Comment=Connect to the PhyWear watch and self-test the whole BLE link
Comment[zh_CN]=连手表做蓝牙全链路自检：扫描 → 连接 → 读传感器 → 收通知 → 写命令 → 文本往返
Exec=python3 $GUI
Icon=bluetooth-active
Terminal=false
Categories=Utility;
Keywords=PhyWear;Bluetooth;BLE;test;
EOF

chmod 711 "$TARGET"
# GNOME 要求桌面上的 .desktop 被标成可信才允许双击运行
gio set "$TARGET" metadata::trusted true 2>/dev/null || true

echo "✅ 已安装：$TARGET"
echo "   执行：python3 $GUI"
echo
echo "如果双击提示「不受信任的应用程序」：右键该图标 → Allow Launching（允许启动）一次即可。"
echo "命令行验证（等价于双击）：DISPLAY=:0 gio launch \"$TARGET\""
