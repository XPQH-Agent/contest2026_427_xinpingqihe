/****************************************************************************
 * apps/examples/phywear/pw_bounce.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 弹跳测距（T-0016）：把小球抛到桌面/表壳上，用两次落地冲击的间隔算弹起高度。
 *
 *   h = g * T^2 / 8      （T = 相邻两次落地的时间间隔）
 *
 ****************************************************************************/

#ifndef __APPS_EXAMPLES_PHYWEAR_PW_BOUNCE_H
#define __APPS_EXAMPLES_PHYWEAR_PW_BOUNCE_H

#include <stdbool.h>
#include <lvgl/lvgl.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 一次测量最多记多少个冲击：3 次连测需要 4 个冲击点（3 个间隔） */

#define PW_BOUNCE_MAX_IMPACT 8

struct pw_bounce_state_s
{
  float    h_cm;                  /* 中位数高度，cm */
  int      t_ms;                  /* 对应的飞行时间，ms */
  float    a_max_g;               /* 本次测量里最大的 |a|，g */
  int      n_impacts;             /* 已经记到的冲击数 */
  bool     valid;                 /* 三个间隔都拿到、结果可用 */
  bool     running;               /* 采样线程在跑 */
};

/* 开始一次测量：清空计数、起采样线程（已有线程则只清计数）。
 * 采样率约 200~500 Hz（取决于 LSM6DSL 的 I2C 读取耗时），
 * 判据：|a| 越过 2.5 g 记一次冲击，之后回落到 1.2 g 以下才允许下一次。 */

void pw_bounce_start(void);

/* 停止采样并释放 IMU（页面退出、或 NSH 关掉时调） */

void pw_bounce_stop(void);

int  pw_bounce_active(void);

/* 取当前结果（线程写、UI 读；都是 4 字节对齐的标量，单写单读够用） */

void pw_bounce_get(struct pw_bounce_state_s *out);

/* 串口输出：h_cm / T_ms / a_max_g / n_impacts 四个量 + 每个间隔的原始值 */

void pw_bounce_report(void);

/* LVGL 测量页（力学板块第 4 项） */

lv_obj_t *pw_bounce_screen(void);

#ifdef __cplusplus
}
#endif

#endif /* __APPS_EXAMPLES_PHYWEAR_PW_BOUNCE_H */
