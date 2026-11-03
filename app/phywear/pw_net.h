/****************************************************************************
 * apps/examples/phywear/pw_net.h
 *
 * 任务 3 的替代通路：USB CDC-ACM + SLIP（本板无无线网卡，见 pw_net.c）。
 ****************************************************************************/

#ifndef __APPS_EXAMPLES_PHYWEAR_NET_H
#define __APPS_EXAMPLES_PHYWEAR_NET_H

/* 注册 SLIP 网卡 sl0（挂在 /dev/ttyACM0 上）。
 * 0 = 已注册；负值 = slip_initialize 的 errno。 */

int pw_net_up(void);

#endif /* __APPS_EXAMPLES_PHYWEAR_NET_H */
