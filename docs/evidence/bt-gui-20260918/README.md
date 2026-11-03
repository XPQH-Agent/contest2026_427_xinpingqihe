# 证据：桌面版「PhyWear 蓝牙测试」GUI（点一下就测）

日期：2026-09-18
宿主：Ubuntu GNOME on **Wayland**（Tk 跑在 XWayland 上），Intel AX201 `hci0`
被测：PhyWear 手表（立创黄山派 SF32LB52），固件 `nuttx.bin` 2,554,544 B

## 一句话

桌面上放一个图标，双击 → 自动跑完「扫描 → 连接 → 读传感器 → 收通知 → 写命令 →
文本往返」，逐项打勾并给大字结论。**它不是新写一遍测试**，序列来自
`pw_ble_central.run_full_test()`，与命令行共用同一份代码。

## 交付物

| 文件 | 作用 |
|---|---|
| `~/openvela/tools/phywear/pw_bt_gui.py` | Tk 图形界面（760×640）。工作线程跑测试、GUI 线程刷控件，两者用 `queue` 通信 |
| `~/openvela/tools/phywear/install_desktop_shortcut.sh` | 在桌面生成 `PhyWear蓝牙测试.desktop`（`--uninstall` 删除）。幂等，路径按当前机器算 |
| `~/openvela/tools/phywear/pw_xwd2png.py` | 本机截图链路：Wayland 下 `xwd -root` 会 BadMatch、GNOME 截图 D-Bus 回 AccessDenied、又没有 ImageMagick ⇒ 只能 `xwd -id <窗口>` 抓单窗口，再用这 60 行把 XWD 解成 PNG |
| `~/桌面/PhyWear蓝牙测试.desktop` | 桌面图标（`Exec=python3 …/pw_bt_gui.py`，`Icon=bluetooth-active`，`metadata::trusted=true`） |

## 截图

| 文件 | 说明 |
|---|---|
| `gui_layout_selftest.png` | **布局自检**（`--gui-selftest`）：填的是**合成结果**，只用来证明"窗口能画对、中文字体不缺字"。⚠️ 这一张的数值不是真机测量 |
| `gui_real_run_1.png` | **真机真跑**：由 `gio launch`（= 双击的等价调用）拉起，自动测完 **14/14**，`\|a\|=1020 mg`，`echo: hello-1789725471` |
| `gui_real_run_2.png` | 同上，用安装脚本重新生成图标后再跑一次：**14/14**，`\|a\|=1009 mg`，`echo: hello-1789725507` |

两张真机图的日志区里能看到 `pw_ble_central.py` 的原始输出，例如
`已有连接中的 "PhyWear" @ … 保留` / `找到：'PhyWear' @ …（UUID 命中=True）` /
`[gui] 对端 … · |a|=1009 mg · 通知 2 包 · 回声 echo: hello-1789725507`。

## 怎么复核

```bash
# 1) 装/重装桌面图标
bash ~/openvela/tools/phywear/install_desktop_shortcut.sh

# 2) 等价于双击（命令行验证，便于无鼠标复核）
DISPLAY=:0 gio launch ~/桌面/PhyWear蓝牙测试.desktop

# 3) 只想看窗口能不能画出来（不碰蓝牙）
python3 ~/openvela/tools/phywear/pw_bt_gui.py --gui-selftest

# 4) 抓窗口存 PNG（本机唯一走得通的截图路径）
DISPLAY=:0 python3 ~/openvela/tools/phywear/pw_xwd2png.py \
    --grab "PhyWear 蓝牙测试" /tmp/out.png

# 5) 不开 GUI 的同一条序列（命令行）
python3 ~/openvela/tools/phywear/pw_ble_central.py --timeout 30 --text-test
```

## 本机踩到的三个环境坑（记下来，换机器会再遇到）

1. **Wayland 下抓不到整个屏幕**：`xwd -root` → `BadMatch`（XWayland 根窗口不给抓）；
   GNOME Shell 的 `org.gnome.Shell.Screenshot` 对普通程序回 **AccessDenied**
   （Ubuntu 只信任 gnome-screenshot 一类程序）；`grim`/`scrot`/ImageMagick 都没装。
   ⇒ 唯一可行是 `xwd -id <窗口ID>`，而 XWD 又不是 PNG、PIL 读不了 ⇒ 才写了 `pw_xwd2png.py`。
2. **Tk 默认字体没有汉字**（DejaVu Sans），中文会显示成方框。程序里按
   Noto Sans CJK SC → 文泉驿 → Droid Sans Fallback 的顺序挑一个真实存在的字体
   （`tkfont.families()` 里查）。
3. **GNOME 不允许双击运行未标"可信"的 .desktop**。安装脚本会
   `gio set … metadata::trusted true`；万一被忽略，右键图标 → Allow Launching 一次。

## 主线程/工作线程的约定（GUI 版）

Tk 的控件**只能在主线程改**。所以：
* 工作线程只做测试，把 `("row", …)` / `("log", …)` / `("done", …)` 塞进 `queue.Queue`；
* 主线程用 `root.after(100, pump)` 轮询队列并更新控件；
* `pw_ble_central` 内部的 `log()` 被接到同一个队列上，所以窗口里的"过程日志"
  就是命令行那条链路打出来的原文。

「停止」按钮是**协作式**的：它只置一个标志，等当前步骤（如一次扫描）结束后生效，
界面会显示"停止中（等当前步骤结束）"。这样不会在 D-Bus 调用中途抛异常导致
`StopNotify`/`remove_signal_receiver` 被跳过。
