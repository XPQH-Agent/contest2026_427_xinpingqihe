/****************************************************************************
 * apps/examples/phywear/phywear_imu.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * ③ 惯性标尺：水平仪/姿态页 + 标定向导。
 *   page 0 实时数值（Mahony MARG 姿态 + 水平仪指示 + 零偏估计读数）
 *   page 1 零偏标定（静止 2s，pw_calib_bias_*）
 *   page 2 重力标定（六个面各取 1s 样本，pw_calib_six_*）
 *   page 3 磁标定（全方向转动，pw_calib_mag_*）
 *
 * 板块归属（生活 / 工具）待用户拍板，因此本批先只提供无头入口
 * （`phywear cap imu` / AI 的 open_screen{imu}），主页 tile 一行未接。
 ****************************************************************************/

#ifndef __APPS_EXAMPLES_PHYWEAR_IMU_H
#define __APPS_EXAMPLES_PHYWEAR_IMU_H

#include <lvgl/lvgl.h>

lv_obj_t *pw_imu_screen(void);

/* 跳到指定子页（截图/AI 切页用） */

void pw_imu_goto(int idx);

/* bench：注入**合成**数据并自动走完标定流程（性能/演示/自动化验证用；不是测量）。
 * 注入的真值：加速度零偏 (0.020,-0.015,0.030) g、刻度 (1.02,0.98,1.01)、
 *            陀螺零偏 (0.010,-0.008,0.012) rad/s、磁硬铁中心 (35,-120,60) mG。
 * 画面标题会带 [BENCH] 标记，避免把注入数据当测量。 */

void pw_imu_bench(int on);

#endif /* __APPS_EXAMPLES_PHYWEAR_IMU_H */
