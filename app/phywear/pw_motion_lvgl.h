/****************************************************************************
 * apps/examples/phywear/pw_motion_lvgl.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 把 pw_motion 的两条曲线接到 LVGL 的动画 path 上（P1-2）。
 *
 * 用法（与 LVGL 原生 path 完全一致，替换即可）：
 *     lv_anim_set_path_cb(&a, pw_motion_path_spring);
 * 想整体退回原生线性动效，把 path_cb 换回 lv_anim_path_linear 即可（不改别处）。
 ****************************************************************************/

#ifndef __APPS_EXAMPLES_PHYWEAR_MOTION_LVGL_H
#define __APPS_EXAMPLES_PHYWEAR_MOTION_LVGL_H

#include <lvgl/lvgl.h>

/* P1 动效总开关（0 = 全关，UI 回到"瞬变"现状）。
 * 依据 third_party/ui-ux-pro-max/data/motion-lvgl.csv：单次 ≤320ms、只改几何/不透明度、
 * 不循环、同屏并发 ≤1、禁 transform/圆角/阴影。 */

#ifndef PW_UI_MOTION
#  define PW_UI_MOTION 1
#endif

/* 物理弹簧手感（欠阻尼，约 25% 过冲，末帧严格落在 end_value） */
lv_anim_value_t pw_motion_path_spring(const lv_anim_t *a);      /* = C2（向后兼容） */

/* 分档 path：C1 settle / C2 soft / C3 bounce / C4 snap（语义见 motion-lvgl.csv） */
lv_anim_value_t pw_motion_path_c1(const lv_anim_t *a);
lv_anim_value_t pw_motion_path_c2(const lv_anim_t *a);
lv_anim_value_t pw_motion_path_c3(const lv_anim_t *a);
lv_anim_value_t pw_motion_path_c4(const lv_anim_t *a);

/* 按压反馈：y 从当前位置动到 base_y+dy（按下 dy=+2，抬起 dy=0），C4 snap。
 * 只改 y —— 不缩放、不透明度也不动，符合本板约束。 */

void pw_motion_press_y(lv_obj_t *obj, int base_y, int dy, uint32_t dur_ms);

/* 衰减振荡（用于"抖一下"的反馈；末值回到 end_value） */
lv_anim_value_t pw_motion_path_decay(const lv_anim_t *a);

/* 便捷封装：对 obj 做一次"从 from_dy 像素外滑入到位"的弹簧动画。
 * 只改 y 坐标（几何变化 → 局部重绘），不动样式，不触发 SW 变换路径。 */
void pw_motion_slide_in_y(lv_obj_t *obj, int from_dy, uint32_t dur_ms);

/* 同上，但可指定起始延时（用于宫格/列表逐项错峰；延时只排队不并发）。
 *
 * ⚠️ base_y 必须由调用方显式给出。原因（2026-09-17 真机定位的真 BUG）：
 * LVGL v9 的 lv_obj_set_y() 只写**样式 Y**，而 lv_obj_get_y() 读的是**已布局的 coords**
 * —— 刚创建、布局尚未刷新的对象两者不一致，早期版本在函数内部读 get_y() 当动画终点，
 * 结果同一批新建行的终点全被算成同一个 y，5 行叠在一起只剩最后一行可见
 * （真机复现：工具板块只显示"水平仪"一行）。 */
void pw_motion_slide_in_y_at(lv_obj_t *obj, int base_y, int from_dy,
                             uint32_t dur_ms, uint32_t delay_ms);

/* 通用按压反馈：按下 y+dy(90ms C4)，抬起回到 base_y(160ms C4)。
 * base_y 由调用方在建对象时给出（这些按钮不会被别处移动）→ 无需存状态。 */
void pw_motion_press_feedback(lv_obj_t *obj, int base_y, int dy);

/* 归零/重置回弹：base_y+amp → base_y，C3 轻弹一次（≤320ms） */
void pw_motion_settle_y(lv_obj_t *obj, int base_y, int amp, uint32_t dur_ms);

/* 状态点脉冲：不透明 100%→40%→100%，一次性不循环（点状小对象专用） */
void pw_motion_pulse_opa(lv_obj_t *obj, uint32_t dur_ms);

#endif /* __APPS_EXAMPLES_PHYWEAR_MOTION_LVGL_H */
