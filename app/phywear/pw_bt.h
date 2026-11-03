/****************************************************************************
 * apps/examples/phywear/pw_bt.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 蓝牙 host 栈的启动入口（B1）。
 ****************************************************************************/

#ifndef __APPS_EXAMPLES_PHYWEAR_PW_BT_H
#define __APPS_EXAMPLES_PHYWEAR_PW_BT_H

/* 起 zblue host 栈 + 注册 GATT/开广播（B1+B2）。
 *
 * **幂等**：重复调用直接返回 0，不会把 service 再注册一遍、也不会重开广播。
 * 返回 0 = 栈已就绪（含"本来就已经起来了"）。 */
int pw_bt_init(void);

/* 栈是否已经起来（1/0）。界面用它决定"要不要顺手把蓝牙打开"。 */
int pw_bt_is_up(void);

/* 异步起栈：**立刻返回**，真正的工作交给一个优先级更低的后台线程。
 *
 * 为什么要有它（2026-09-18 深夜，用户反馈）：`bt_enable()` 实测要 ~1.2 s，
 * 而主页标识与蓝牙页都在 **LVGL 主线程**上刷新 —— 同步调用会把界面整整冻住
 * 一秒（用户原话："点击到蓝牙会卡顿 1S 左右"）。
 *
 * 幂等：已经起来、或正在起，都直接返回 0，不会重复 `bt_enable()`。
 * 进度用 `pw_bt_is_starting()` 查（界面据此显示"启动中"）。
 * ⚠️ 不要在同一个进程里同时对它和 pw_bt_init() 并发调用（GUI 与子命令不会重叠）。 */
int pw_bt_init_async(void);

/* 1 = 后台线程正在起（还没起来）。界面用它显示"启动中"。 */
int pw_bt_is_starting(void);

#endif /* __APPS_EXAMPLES_PHYWEAR_PW_BT_H */
