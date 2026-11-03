/****************************************************************************
 * apps/examples/phywear/pw_motion_lvgl.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * LVGL path 适配层。为什么单独一个文件：pw_motion.c 必须是纯算法（能在 PC 上单测），
 * 所以 LVGL 依赖全部隔离在这里。
 *
 * 数值约定（与 lv_anim_path_linear 一致，见 LVGL 9 lv_anim.c）：
 *   step = lv_map(act_time, 0, duration, 0, LV_ANIM_RESOLUTION)   // 0..1024
 *   返回值 = start + (end - start) × 曲线(step/1024)
 * 位移用 int64 中间量：end-start 可能到 ±几万像素级 × Q14，int32 会溢出。
 ****************************************************************************/

#include <nuttx/config.h>

#include "pw_motion.h"
#include "pw_motion_lvgl.h"

static lv_anim_value_t motion_path(const lv_anim_t *a, int32_t q14)
{
  int32_t step;
  int32_t span;
  int64_t v;

  step = lv_map(a->act_time, 0, a->duration, 0, PW_MOTION_RES);

  if (step >= PW_MOTION_RES)
    {
      return a->end_value;              /* 末帧严格到位 */
    }

  span = a->end_value - a->start_value;
  v = (int64_t)span * (int64_t)q14;     /* Q14 */
  v >>= PW_MOTION_Q;

  return (lv_anim_value_t)(a->start_value + (int32_t)v);
}

lv_anim_value_t pw_motion_path_spring(const lv_anim_t *a)
{
  return pw_motion_path_c2(a);
}

static lv_anim_value_t motion_path_tier(const lv_anim_t *a, int tier)
{
  return motion_path(a, pw_motion_spring_q14_t(
                       tier, (uint32_t)lv_map(a->act_time, 0, a->duration,
                                              0, PW_MOTION_RES)));
}

lv_anim_value_t pw_motion_path_c1(const lv_anim_t *a)
{
  return motion_path_tier(a, PW_MOTION_TIER_C1);
}

lv_anim_value_t pw_motion_path_c2(const lv_anim_t *a)
{
  return motion_path_tier(a, PW_MOTION_TIER_C2);
}

lv_anim_value_t pw_motion_path_c3(const lv_anim_t *a)
{
  return motion_path_tier(a, PW_MOTION_TIER_C3);
}

lv_anim_value_t pw_motion_path_c4(const lv_anim_t *a)
{
  return motion_path_tier(a, PW_MOTION_TIER_C4);
}

void pw_motion_press_y(lv_obj_t *obj, int base_y, int dy, uint32_t dur_ms)
{
#if PW_UI_MOTION
  lv_anim_t a;

  if (obj == NULL)
    {
      return;
    }

  if (dur_ms > 320)
    {
      dur_ms = 320;                    /* 硬顶：见 motion-lvgl.csv 的 Guard */
    }

  lv_anim_init(&a);
  lv_anim_set_var(&a, obj);
  lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_y);
  lv_anim_set_values(&a, lv_obj_get_y(obj), base_y + dy);
  lv_anim_set_duration(&a, dur_ms);
  lv_anim_set_path_cb(&a, pw_motion_path_c4);
  lv_anim_start(&a);
#else
  LV_UNUSED(obj);
  LV_UNUSED(base_y);
  LV_UNUSED(dy);
  LV_UNUSED(dur_ms);
#endif
}

#if PW_UI_MOTION
static void press_fb_down(lv_event_t *e)
{
  lv_obj_t *b = lv_event_get_target(e);
  int base = (int)(intptr_t)lv_event_get_user_data(e);

  pw_motion_press_y(b, base, 3, 90);
}

static void press_fb_up(lv_event_t *e)
{
  lv_obj_t *b = lv_event_get_target(e);
  int base = (int)(intptr_t)lv_event_get_user_data(e);

  lv_anim_delete(b, (lv_anim_exec_xcb_t)lv_obj_set_y);   /* 清残留，回位必须精确 */
  pw_motion_press_y(b, base, 0, 160);
}

static lv_anim_value_t path_pulse(const lv_anim_t *a)
{
  /* 三角波 255→100→255：上升段占 40%，回落段 60%（视觉上"闪一下"） */
  uint32_t p = (uint32_t)lv_map(a->act_time, 0, a->duration, 0, 1000);

  if (p <= 400)
    {
      return (lv_anim_value_t)lv_map((int32_t)p, 0, 400, LV_OPA_COVER, 100);
    }

  return (lv_anim_value_t)lv_map((int32_t)p, 400, 1000, 100, LV_OPA_COVER);
}

static void pulse_exec(void *o, int32_t v)
{
  lv_obj_set_style_opa((lv_obj_t *)o, (lv_opa_t)v, 0);
}
#endif

void pw_motion_press_feedback(lv_obj_t *obj, int base_y, int dy)
{
#if PW_UI_MOTION
  if (obj == NULL)
    {
      return;
    }

  lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(obj, press_fb_down, LV_EVENT_PRESSED,
                      (void *)(intptr_t)base_y);
  lv_obj_add_event_cb(obj, press_fb_up, LV_EVENT_RELEASED,
                      (void *)(intptr_t)base_y);
  lv_obj_add_event_cb(obj, press_fb_up, LV_EVENT_PRESS_LOST,
                      (void *)(intptr_t)base_y);
#else
  LV_UNUSED(obj);
  LV_UNUSED(base_y);
  LV_UNUSED(dy);
#endif
}

void pw_motion_settle_y(lv_obj_t *obj, int base_y, int amp, uint32_t dur_ms)
{
#if PW_UI_MOTION
  lv_anim_t a;

  if (obj == NULL)
    {
      return;
    }

  if (dur_ms > 320)
    {
      dur_ms = 320;
    }

  lv_anim_delete(obj, (lv_anim_exec_xcb_t)lv_obj_set_y);
  lv_obj_set_y(obj, base_y + amp);          /* 先落到回弹起点，再弹回 */

  lv_anim_init(&a);
  lv_anim_set_var(&a, obj);
  lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_y);
  lv_anim_set_values(&a, base_y + amp, base_y);
  lv_anim_set_duration(&a, dur_ms);
  lv_anim_set_path_cb(&a, pw_motion_path_c3);
  lv_anim_start(&a);
#else
  LV_UNUSED(obj); LV_UNUSED(base_y); LV_UNUSED(amp); LV_UNUSED(dur_ms);
#endif
}

void pw_motion_pulse_opa(lv_obj_t *obj, uint32_t dur_ms)
{
#if PW_UI_MOTION
  lv_anim_t a;

  if (obj == NULL)
    {
      return;
    }

  if (dur_ms > 320)
    {
      dur_ms = 320;
    }

  lv_anim_delete(obj, pulse_exec);
  lv_anim_init(&a);
  lv_anim_set_var(&a, obj);
  lv_anim_set_exec_cb(&a, pulse_exec);
  lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_COVER);
  lv_anim_set_duration(&a, dur_ms);
  lv_anim_set_path_cb(&a, path_pulse);
  lv_anim_start(&a);
#else
  LV_UNUSED(obj); LV_UNUSED(dur_ms);
#endif
}

lv_anim_value_t pw_motion_path_decay(const lv_anim_t *a)
{
  return motion_path(a, pw_motion_decay_q14(
                       (uint32_t)lv_map(a->act_time, 0, a->duration,
                                        0, PW_MOTION_RES)));
}

void pw_motion_slide_in_y_at(lv_obj_t *obj, int base_y, int from_dy,
                             uint32_t dur_ms, uint32_t delay_ms)
{
#if PW_UI_MOTION
  lv_anim_t a;

  if (obj == NULL)
    {
      return;
    }

  if (dur_ms > 320)
    {
      dur_ms = 320;                    /* 硬顶（motion-lvgl.csv Guard） */
    }

  /* 先把对象落到起点（写样式 Y，立即生效），延时期间它就一直停在起点，
   * 不会出现"先出现在终点、延时到了再跳一下"的抖动。 */

  lv_obj_set_y(obj, base_y + from_dy);

  lv_anim_init(&a);
  lv_anim_set_var(&a, obj);
  lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_y);
  lv_anim_set_values(&a, base_y + from_dy, base_y);
  lv_anim_set_duration(&a, dur_ms);
  lv_anim_set_delay(&a, delay_ms);
  lv_anim_set_path_cb(&a, pw_motion_path_c2);      /* 入场默认 C2 soft */
  lv_anim_start(&a);
#else
  LV_UNUSED(obj); LV_UNUSED(base_y); LV_UNUSED(from_dy);
  LV_UNUSED(dur_ms); LV_UNUSED(delay_ms);
#endif
}

void pw_motion_slide_in_y(lv_obj_t *obj, int from_dy, uint32_t dur_ms)
{
#if PW_UI_MOTION
  lv_anim_t a;
  int32_t   y0;

  if (obj == NULL)
    {
      return;
    }

  if (dur_ms > 320)
    {
      dur_ms = 320;
    }

  /* 用**样式 Y**（lv_obj_get_y_aligned）而不是已布局 coords：
   * 这个函数在"对象刚建好"的路径上也会被调用。 */

  y0 = lv_obj_get_y_aligned(obj);

  lv_anim_init(&a);
  lv_anim_set_var(&a, obj);
  lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_y);
  lv_anim_set_values(&a, y0 + from_dy, y0);
  lv_anim_set_duration(&a, dur_ms);
  lv_anim_set_path_cb(&a, pw_motion_path_c2);
  lv_anim_start(&a);
#else
  LV_UNUSED(obj); LV_UNUSED(from_dy); LV_UNUSED(dur_ms);
#endif
}
