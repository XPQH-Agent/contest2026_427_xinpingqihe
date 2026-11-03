/****************************************************************************
 * apps/examples/phywear/pw_btprobe.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 蓝牙阶段 A 探针（异步，不阻塞 GUI）。详见 pw_btprobe.c 顶部注释。
 ****************************************************************************/

#ifndef __APPS_EXAMPLES_PHYWEAR_BTPROBE_H
#define __APPS_EXAMPLES_PHYWEAR_BTPROBE_H

/* 对 /dev/ttyHCI0 发一次 HCI_Reset 并等回包。
 * 0 = 链路通；-1 = 端口打不开；-2 = 无回包；-3 = 读失败；-4 = 回包不是预期的
 * Command Complete。 */

int pw_bt_probe(void);

#endif /* __APPS_EXAMPLES_PHYWEAR_BTPROBE_H */
