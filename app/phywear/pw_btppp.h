/****************************************************************************
 * apps/examples/phywear/pw_btppp.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * PPP over BLE（任务 3 的第三条路线）—— 对外只暴露一个启动函数。
 *
 * 前置：需要 CONFIG_PSEUDOTERM(+_SUSV1) 与 CONFIG_NETUTILS_PPPD（见 defconfig）。
 * BT 栈必须先起来（pw_bt_init()）。
 *
 * 用法（板子侧）：
 *     phywear cap bt      # 起 BT + 广播
 *     phywear btppp       # 建 pty、注册字节管道服务、起 pppd
 * 然后宿主侧跑 pw_bt_ppp.py 做 BLE↔pty 桥，再 sudo pppd 起对端。
 ****************************************************************************/

#ifndef __APPS_EXAMPLES_PHYWEAR_PW_BTPPP_H
#define __APPS_EXAMPLES_PHYWEAR_PW_BTPPP_H

/* 启动 BLE↔pty 字节管道并拉起 pppd。
 * 幂等：重复调用只打印当前状态。
 * 返回 0 成功；负值为 GATT 服务注册失败的错误码。 */
int pw_btppp_start(void);

#endif /* __APPS_EXAMPLES_PHYWEAR_PW_BTPPP_H */
