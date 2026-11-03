/****************************************************************************
 * apps/examples/phywear/phywear_imu.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * ③ 惯性标尺 UI：水平仪/姿态 + 标定向导 + 轨迹（5 页横滑）。
 *
 * 设计取舍（都为了不撞 SF32LB52 的两个硬约束）：
 *   - **不用任何旋转控件**：水平仪的小球靠"改坐标"实现（lv_obj_set_pos），
 *     不走对象 rotate（那会退回软件光栅，EPIC 不加速）；
 *   - **不新增采样缓冲**：AHRS 状态 76 B、陀螺零偏累加 28 B、六面法累加 ~100 B、
 *     磁椭球正规方程 360 B —— 全是流式充分统计，与采样点数无关；
 *   - 采样沿用 GUI 自己的缓存 fd（`pw_sensors_read_imu/mag`），不走 oneshot
 *     （oneshot 是给别的任务用的，见 docs/03 §6.6）。
 *
 * 三个标定页的"真值"都不依赖外部仪器：
 *   零偏 ← 静止时角速度为零；重力 ← 重力本身（理想读数 ±1 g）；磁 ← 场强恒定（椭球）。
 ****************************************************************************/

#include <nuttx/config.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "phywear_imu.h"
#include "phywear_i18n.h"
#include "phywear_sensors.h"
#include "phywear_ui.h"
#include "pw_ahrs.h"
#include "pw_calib.h"
#include "pw_graph.h"
#include "pw_motion_lvgl.h"
#include "pw_traj.h"

/****************************************************************************
 * Private Definitions
 ****************************************************************************/

#define IMU_PAGES        5
#define IMU_PAGE_W       390
#define IMU_PAGE_H       342
#define IMU_TICK_MS      20        /* 50 Hz，与单摆页一致 */

#define BIAS_MIN_N       100       /* 静止 2s 才允许求解 */
#define BIAS_STILL_STD   0.02f     /* rad/s ≈1.1 dps，超过就判"没静止" */
#define SIX_FACE_N       50        /* 每个面取 1s */
#define MAG_MIN_N        200       /* 至少 200 个样本才拟合椭球 */

/* page 4 轨迹：默认视图 ±30 cm，超出 90% 就整档翻倍（不做逐帧自适应，
 * 否则"标尺"会随数据呼吸，位移比例就读不出来了） */

#define TJ_RANGE0        0.30f
#define TJ_DRAW_DIV      5         /* 50 Hz 采样 / 5 = 10 Hz 重绘 */
#define TJ_ALIGN_N       150       /* 开页后 3 s 姿态对准：这段时间只收敛姿态、不积分。
                                    * 两个真机实测依据：
                                    *   ① 不这么做时对准瞬态会被积成米级位移（4 s 内 X 走 2.4 m、Z 走 4.8 m）；
                                    *   ② AHRS 的陀螺零偏估计要 ~4 s 才收敛（0.2 → 2.48 dps），
                                    *      3 s 时已降到门限内，之后 ZUPT 能把静止稳住。 */
#define TJ_BENCH_A       2.0f      /* bench 推手峰值 m/s²（整周期 0.6 s → 每次推 ~11.5 cm） */

#define LVL_CARD_W       190   /* P0 实测：缩小卡片+读数聚拢把每帧脏区并集降到 ~55%，
                               * 但 fps 无变化（16.9 vs 17.1 ms）→ 回退观感 */
#define LVL_CARD_H       150
#define LVL_DOT          20
#define LVL_RANGE_X      72.0f
#define LVL_RANGE_Y      54.0f

struct imu_ui_s
{
  lv_obj_t *scr;
  lv_obj_t *scroller;
  lv_obj_t *dots[IMU_PAGES];
  lv_obj_t *arrow_btn[2];
  int       idx;

  /* page 0 实时数值 */

  lv_obj_t *lvl_card;
  lv_obj_t *lvl_h;
  lv_obj_t *lvl_v;
  lv_obj_t *lvl_dot;
  lv_obj_t *ax_name[3];
  lv_obj_t *ax_val[3];
  lv_obj_t *bias_lab;
  lv_obj_t *live_stat;
  lv_obj_t *live_hint;

  /* page 1 零偏标定 */

  lv_obj_t *bias_btn_lab;
  lv_obj_t *bias_prog;
  lv_obj_t *bias_res;
  lv_obj_t *bias_disp;
  lv_obj_t *bias_stat;

  /* page 2 重力标定（六面） */

  lv_obj_t *grav_face;
  lv_obj_t *grav_btn_lab;
  lv_obj_t *grav_prog;
  lv_obj_t *grav_res[3];
  lv_obj_t *grav_stat;

  /* page 3 磁标定 */

  lv_obj_t *mag_btn_lab;
  lv_obj_t *mag_prog;
  lv_obj_t *mag_res[3];
  lv_obj_t *mag_stat;

  /* page 4 轨迹（相对位移） */

  struct pw_graph_s *tj_graph;
  lv_obj_t *tj_val[3];
  lv_obj_t *tj_vel;
  lv_obj_t *tj_time;
  lv_obj_t *tj_stat;
  lv_obj_t *tj_scale;

  /* 状态 */

  /* 轨迹用**堆**（与 pw_graph 自己的画布同一策略）：本机静态 SRAM 很紧，
   * 页面级缓冲不该进 BSS。本页只多一个指针的静态开销。 */

  struct pw_traj_s *traj;
  float  *tjx;
  float  *tjy;
  int     tj_n;
  int     tj_tick;
  int     tj_align;            /* >0 = 姿态对准中（只收敛姿态，不积分） */
  float   tj_range;

  struct pw_ahrs_s       ahrs;
  struct pw_calib_bias_s bias_acc;
  struct pw_calib_six_s  six_acc;
  struct pw_calib_mag_s  mag_acc;

  int       bias_rec;
  int       six_face;          /* 下一个要取的面 0..5 */
  int       mag_rec;
  int       live_phase;        /* 错峰更新相位（PW_LIVE_STAGGER） */
  int       ahrs_inited;
  int       mag_seen;
  uint32_t  last_ms;
};

static struct imu_ui_s g_i;

/* ---- bench（注入合成数据）----
 * 真值固定，便于"标定能不能还原"这件事可验证：
 *   加速度零偏 BIAS_A g、刻度 SCALE_A；陀螺零偏 BIAS_G rad/s；磁硬铁中心 CENTER_M mG。
 * 不是测量：屏幕上会显示 [BENCH] 标记。 */

static int  g_imu_bench;
static long g_imu_bench_n;

static const float BENCH_BA[3] = {0.020f, -0.015f, 0.030f};
static const float BENCH_SA[3] = {1.02f, 0.98f, 1.01f};
static const float BENCH_BG[3] = {0.010f, -0.008f, 0.012f};
static const float BENCH_CM[3] = {35.0f, -120.0f, 60.0f};

void pw_imu_bench(int on)
{
  g_imu_bench = on ? 1 : 0;
  g_imu_bench_n = 0;
}

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int imu_dps1000(float rps)
{
  return (int)(rps * 57.29578f * 1000.0f);
}

/* 合成一拍数据：dir 为"重力朝下的机体轴"(0..5，对应六面)，-1 表示平放 */
static void imu_bench_data(float *a, float *g, float *m, int dir)
{
  float u[3] = {0.0f, 0.0f, 1.0f};
  float ph;
  float ux;
  float uy;
  float uz;
  float n;
  int   k;

  if (dir >= 0 && dir < 6)
    {
      k = dir / 2;
      u[0] = u[1] = u[2] = 0.0f;
      u[k] = ((dir % 2) == 0) ? 1.0f : -1.0f;
    }

  a[0] = u[0] * BENCH_SA[0] + BENCH_BA[0];
  a[1] = u[1] * BENCH_SA[1] + BENCH_BA[1];
  a[2] = u[2] * BENCH_SA[2] + BENCH_BA[2];

  g[0] = BENCH_BG[0];
  g[1] = BENCH_BG[1];
  g[2] = BENCH_BG[2];

  /* 磁：让方向绕两轴转，覆盖球面（椭球拟合需要立体角覆盖） */

  ph = (float)g_imu_bench_n * 0.05f;
  ux = sinf(ph);
  uy = sinf(ph * 0.7f + 1.0f);
  uz = cosf(ph);
  n = sqrtf(ux * ux + uy * uy + uz * uz);
  if (n < 1e-6f)
    {
      n = 1.0f;
    }

  ux /= n;
  uy /= n;
  uz /= n;

  m[0] = 520.0f * ux + BENCH_CM[0];
  m[1] = 520.0f * uy + BENCH_CM[1];
  m[2] = 520.0f * uz + BENCH_CM[2];

  g_imu_bench_n++;
}

static void imu_sync_dots(void)
{
  int i;

  for (i = 0; i < IMU_PAGES; i++)
    {
      lv_obj_set_style_bg_color(g_i.dots[i],
                                (i == g_i.idx) ? PW_ACC_RAW : PW_COL_CARD_LT,
                                0);
      if (i == g_i.idx)
        {
          pw_motion_pulse_opa(g_i.dots[i], 220);   /* ④ 状态点脉冲（一次性） */
        }
    }
}

static void imu_set_page(int idx)
{
  if (idx < 0)
    {
      idx = 0;
    }
  else if (idx >= IMU_PAGES)
    {
      idx = IMU_PAGES - 1;
    }

  g_i.idx = idx;
  lv_obj_scroll_to_x(g_i.scroller, idx * IMU_PAGE_W, LV_ANIM_ON);   /* 切页转场 */
  imu_sync_dots();

  /* 动效（P1-2）：切到轨迹页时，图形卡用**查表弹簧**滑入。
   * 只改 y 坐标（几何变化→局部重绘），不碰样式，因此不会落进 SW 变换路径；
   * 时长 320 ms、单对象，成本有界。想关掉动画就把这一句删掉（或把 path 换回线性）。 */

  if (idx == 4 && g_i.tj_graph != NULL)
    {
      pw_motion_slide_in_y(pw_graph_obj(g_i.tj_graph), 18, 320);
    }
}

static void imu_arrow_cb(lv_event_t *e)
{
  int d = *(int *)lv_event_get_user_data(e);

  imu_set_page(g_i.idx + d);
}

static void imu_scroll_end_cb(lv_event_t *e)
{
  int32_t x = lv_obj_get_scroll_x(g_i.scroller);

  (void)e;

  g_i.idx = (int)((x + IMU_PAGE_W / 2) / IMU_PAGE_W);

  if (g_i.idx < 0)
    {
      g_i.idx = 0;
    }
  else if (g_i.idx >= IMU_PAGES)
    {
      g_i.idx = IMU_PAGES - 1;
    }

  imu_sync_dots();
}

/* 水平仪小球：跟着倾角走（正 roll 向下、正 pitch 向右），不旋转对象 */

/* 轨迹页的刷新/落点在 tick 之后才定义，这里先声明（本文件内静态） */

static void imu_traj_labels(void);
static void imu_traj_push(float x, float y);

static void imu_level_dot(float roll, float pitch)
{
  int   cx = LVL_CARD_W / 2 - LVL_DOT / 2;
  int   cy = LVL_CARD_H / 2 - LVL_DOT / 2;
  float dx = pitch * (LVL_RANGE_X / 45.0f);
  float dy = roll * (LVL_RANGE_Y / 45.0f);

  if (dx > LVL_RANGE_X)  dx = LVL_RANGE_X;
  if (dx < -LVL_RANGE_X) dx = -LVL_RANGE_X;
  if (dy > LVL_RANGE_Y)  dy = LVL_RANGE_Y;
  if (dy < -LVL_RANGE_Y) dy = -LVL_RANGE_Y;

  lv_obj_set_pos(g_i.lvl_dot, cx + (int)dx, cy + (int)dy);
}

/* P0 实验开关：实时页的更新**错峰**。
 * 动机：LVGL 局部渲染下，每个独立脏矩形都会触发一次 flush（拷贝 + 面板传输 + 同步握手），
 * 而每帧固定 ~17 ms 的成本与"画了多少像素"几乎无关 —— 所以先怀疑**每帧 flush 的次数**。
 * 置 1 = 每拍只更新一处（三个角度 / 小球 / 零偏 / 磁状态 轮转，各自 ~12~17 Hz），
 * 把每帧多处脏矩形压成一处。置 0 = 原始行为（全部每拍更新，50 Hz）。 */

#ifndef PW_LIVE_STAGGER
#  define PW_LIVE_STAGGER 0   /* 实测：错峰对 fps 无影响（47~49 与 48~49 相同），保持原行为 */
#endif

static void imu_live_update(void)
{
  float r = 0.0f;
  float p = 0.0f;
  float y = 0.0f;
  float b[3];
  char  buf[72];

  pw_ahrs_euler(&g_i.ahrs, &r, &p, &y);
  pw_ahrs_bias(&g_i.ahrs, b);

#if PW_LIVE_STAGGER
  /* 轮转：每拍只动一处，避免一帧里出现多个互不相邻的脏矩形 */

  g_i.live_phase = (g_i.live_phase + 1) & 3;

  if (g_i.live_phase == 0)
    {
      lv_label_set_text_fmt(g_i.ax_val[0], "%+.1f", (double)r);
    }
  else if (g_i.live_phase == 1)
    {
      lv_label_set_text_fmt(g_i.ax_val[1], "%+.1f", (double)p);
    }
  else if (g_i.live_phase == 2)
    {
      lv_label_set_text_fmt(g_i.ax_val[2], "%+.1f", (double)y);
    }
  else
    {
      imu_level_dot(r, p);
    }

  if ((g_i.live_phase & 1) == 0)
    {
      snprintf(buf, sizeof(buf), PW_STR(IMU_FMT_BIAS),
               imu_dps1000(b[0]), imu_dps1000(b[1]), imu_dps1000(b[2]));
      lv_label_set_text(g_i.bias_lab, buf);
    }
#else
  lv_label_set_text_fmt(g_i.ax_val[0], "%+.1f", (double)r);
  lv_label_set_text_fmt(g_i.ax_val[1], "%+.1f", (double)p);
  lv_label_set_text_fmt(g_i.ax_val[2], "%+.1f", (double)y);

  imu_level_dot(r, p);

  snprintf(buf, sizeof(buf), PW_STR(IMU_FMT_BIAS),
           imu_dps1000(b[0]), imu_dps1000(b[1]), imu_dps1000(b[2]));
  lv_label_set_text(g_i.bias_lab, buf);
#endif

  snprintf(buf, sizeof(buf), PW_STR(IMU_FMT_MAG),
           g_i.mag_seen ? PW_STR(IMU_V_YES) : PW_STR(IMU_V_NO));
  lv_label_set_text(g_i.live_stat, buf);
}

/* page 1：零偏标定 */

static void imu_bias_cb(lv_event_t *e)
{
  (void)e;

  if (!g_i.bias_rec)
    {
      pw_calib_bias_reset(&g_i.bias_acc);
      g_i.bias_rec = 1;
      lv_label_set_text(g_i.bias_btn_lab, PW_STR(IMU_BTN_STOP));
      lv_label_set_text(g_i.bias_prog, "");
      lv_label_set_text(g_i.bias_res, "");
      lv_label_set_text(g_i.bias_disp, "");
      lv_label_set_text(g_i.bias_stat, "");
      return;
    }

  g_i.bias_rec = 0;
  lv_label_set_text(g_i.bias_btn_lab, PW_STR(IMU_BTN_START));

  if (pw_calib_bias_ready(&g_i.bias_acc, BIAS_MIN_N))
    {
      float bias[3];
      float sd[3];

      /* 第三个参数是"够不够静"的判据：判不过就如实拒绝，不给数字 */

      if (pw_calib_bias_solve(&g_i.bias_acc, bias, sd,
                              BIAS_STILL_STD) == 0)
        {
          lv_label_set_text_fmt(g_i.bias_res, PW_STR(IMU_FMT_BIAS),
                                imu_dps1000(bias[0]), imu_dps1000(bias[1]),
                                imu_dps1000(bias[2]));
          lv_label_set_text_fmt(g_i.bias_disp, PW_STR(IMU_FMT_DISP),
                                imu_dps1000(sd[0]), imu_dps1000(sd[1]),
                                imu_dps1000(sd[2]));
          lv_label_set_text(g_i.bias_stat, PW_STR(IMU_ST_DONE));
          return;
        }
    }

  lv_label_set_text(g_i.bias_stat, PW_STR(IMU_ST_NONE));
}

/* page 2：重力标定（六面法） */

static void imu_grav_face_label(void)
{
  static const char axc[6] = {'X', 'X', 'Y', 'Y', 'Z', 'Z'};
  char tag[4];

  snprintf(tag, sizeof(tag), "%c%c",
           ((g_i.six_face % 2) == 0) ? '+' : '-', axc[g_i.six_face]);
  lv_label_set_text_fmt(g_i.grav_face, PW_STR(IMU_FACE_DOWN), tag);
}

static void imu_grav_solve(void)
{
  float bias[3];
  float scale[3];
  float resid = 0.0f;
  float cross = 0.0f;

  if (pw_calib_six_solve(&g_i.six_acc, bias, scale, &resid, &cross) == 0)
    {
      lv_label_set_text_fmt(g_i.grav_res[0], PW_STR(IMU_FMT_GBIAS),
                            (double)bias[0], (double)bias[1],
                            (double)bias[2]);
      lv_label_set_text_fmt(g_i.grav_res[1], PW_STR(IMU_FMT_SCALE),
                            (double)scale[0], (double)scale[1],
                            (double)scale[2]);
      lv_label_set_text_fmt(g_i.grav_res[2], PW_STR(IMU_FMT_DEV),
                            (double)resid);
      lv_label_set_text_fmt(g_i.grav_prog, PW_STR(IMU_FMT_AXIS),
                            (double)cross);
      lv_label_set_text(g_i.grav_stat, PW_STR(IMU_ST_DONE));
    }
  else
    {
      lv_label_set_text(g_i.grav_stat, PW_STR(IMU_ST_NONE));
    }
}

static void imu_grav_cb(lv_event_t *e)
{
  (void)e;

  if (g_i.six_face >= 6)
    {
      /* 已经全部取完：再点一次重新开始 */

      pw_calib_six_reset(&g_i.six_acc);
      g_i.six_face = 0;
      lv_label_set_text(g_i.grav_res[0], "");
      lv_label_set_text(g_i.grav_res[1], "");
      lv_label_set_text(g_i.grav_res[2], "");
      lv_label_set_text(g_i.grav_stat, "");
      lv_label_set_text(g_i.grav_prog, "");
      imu_grav_face_label();
      return;
    }

  g_i.six_face++;

  if (g_i.six_face >= 6)
    {
      lv_label_set_text(g_i.grav_prog, "");
      imu_grav_solve();
      lv_label_set_text_fmt(g_i.grav_face, PW_STR(IMU_FMT_FACE), 6,
                            SIX_FACE_N);
      return;
    }

  imu_grav_face_label();
}

/* page 3：磁标定 —— 只定形状（椭球→球），物理量纲另行按 WMM 缩放 */

static void imu_mag_cb(lv_event_t *e)
{
  (void)e;

  if (!g_i.mag_rec)
    {
      pw_calib_mag_reset(&g_i.mag_acc);
      g_i.mag_rec = 1;
      lv_label_set_text(g_i.mag_btn_lab, PW_STR(IMU_BTN_STOP));
      lv_label_set_text(g_i.mag_prog, "");
      lv_label_set_text(g_i.mag_res[0], "");
      lv_label_set_text(g_i.mag_res[1], "");
      lv_label_set_text(g_i.mag_res[2], "");
      lv_label_set_text(g_i.mag_stat, "");
      return;
    }

  g_i.mag_rec = 0;
  lv_label_set_text(g_i.mag_btn_lab, PW_STR(IMU_BTN_START));

  if (pw_calib_mag_ready(&g_i.mag_acc, MAG_MIN_N))
    {
      float center[3];
      float w[9];
      float mean = 0.0f;
      float eig[3];

      if (pw_calib_mag_solve(&g_i.mag_acc, center, w, &mean, eig) == 0)
        {
          float lo = eig[0];
          float hi = eig[0];
          int   i;

          for (i = 1; i < 3; i++)
            {
              if (eig[i] < lo) lo = eig[i];
              if (eig[i] > hi) hi = eig[i];
            }

          lv_label_set_text_fmt(g_i.mag_res[0], PW_STR(IMU_FMT_MAGC),
                                (int)center[0], (int)center[1],
                                (int)center[2]);
          lv_label_set_text_fmt(g_i.mag_res[1], PW_STR(IMU_FMT_MAGMEAN),
                                (int)mean);
          lv_label_set_text_fmt(g_i.mag_res[2], PW_STR(IMU_FMT_MAGDISP),
                                (double)((lo > 0.0f) ? (hi / lo - 1.0f) : 0.0f));
          lv_label_set_text(g_i.mag_stat, PW_STR(IMU_ST_DONE));
          return;
        }
    }

  lv_label_set_text(g_i.mag_stat, PW_STR(IMU_ST_NONE));
}

/****************************************************************************
 * Tick：一路采样，按当前页分派（50 Hz）
 ****************************************************************************/

static void imu_tick_cb(lv_timer_t *timer)
{
  struct pw_imu_s imu;
  struct pw_mag_s mag;
  float a[3];
  float g[3];
  float m[3];
  float dt;
  uint32_t now;
  int   have_mag;

  (void)timer;

  now = lv_tick_get();
  dt = (float)(now - g_i.last_ms) / 1000.0f;
  g_i.last_ms = now;

  if (dt <= 0.0f || dt > 0.5f)
    {
      dt = (float)IMU_TICK_MS / 1000.0f;
    }

  if (g_imu_bench)
    {
      /* 注入：六面页按"当前要取的那一面"给朝向，其余页平放 */

      imu_bench_data(a, g, m, (g_i.idx == 2) ? g_i.six_face : -1);
      have_mag = 1;
      g_i.mag_seen = 1;

      if (g_i.idx == 4)
        {
          /* 轨迹页注入：合成"推 0.6 s → 静 1.4 s"的循环（竖轴仍 1 g）。
           * 必须带静止段：ZUPT 才有机会把速度钉回 0 —— 若一直"在动"，
           * 纯惯性积分几十秒就会漂到公里级（第一版注入正是如此，屏幕上是 5.5 km，
           * 那是死算的教科书结果，不是页面 bug）。
           * 真机这一页靠人推；注入只为截图/回归，标题上的 [BENCH] 标明不是测量。 */

          int k = (int)(g_imu_bench_n % 100);      /* 100 拍 = 2.0 s */

          if (k < 30)                              /* 30 拍 = 0.6 s：整周期正弦推手 */
            {
              float ph = 6.2831853f * (float)k / 30.0f;

              a[0] = TJ_BENCH_A * sinf(ph) / 9.80665f;
              a[1] = 0.0f;
              a[2] = 1.0f;
              g[0] = 0.0f;
              g[1] = 0.3f * sinf(ph);              /* 带转动的推，像真推手 */
              g[2] = 0.0f;
            }
          else                                     /* 70 拍 = 1.4 s：真静止 → ZUPT */
            {
              a[0] = 0.0f;
              a[1] = 0.0f;
              a[2] = 1.0f;
              g[0] = 0.0f;
              g[1] = 0.0f;
              g[2] = 0.0f;
            }
        }
    }
  else
    {
      if (pw_sensors_read_imu(&imu) < 0)
        {
          return;
        }

      a[0] = imu.ax / 1000.0f;
      a[1] = imu.ay / 1000.0f;
      a[2] = imu.az / 1000.0f;
      g[0] = imu.gx * 1.745329e-5f;
      g[1] = imu.gy * 1.745329e-5f;
      g[2] = imu.gz * 1.745329e-5f;

      have_mag = (pw_sensors_read_mag(&mag) == 0);
      if (have_mag)
        {
          m[0] = (float)mag.x;
          m[1] = (float)mag.y;
          m[2] = (float)mag.z;
          g_i.mag_seen = 1;
        }
    }

  switch (g_i.idx)
    {
      case 0:
        if (!g_i.ahrs_inited)
          {
            pw_ahrs_init(&g_i.ahrs);
            g_i.ahrs_inited = 1;
          }

        pw_ahrs_update(&g_i.ahrs, a, g, have_mag ? m : NULL, dt);
        imu_live_update();
        break;

      case 1:
        if (g_imu_bench && !g_i.bias_rec && g_i.bias_acc.n == 0)
          {
            imu_bias_cb(NULL);            /* bench：自动开始 */
          }

        if (g_i.bias_rec)
          {
            pw_calib_bias_add(&g_i.bias_acc, g);
            lv_label_set_text_fmt(g_i.bias_prog, PW_STR(IMU_FMT_SAMPLES),
                                  g_i.bias_acc.n);

            if (g_imu_bench && g_i.bias_acc.n >= 150)
              {
                imu_bias_cb(NULL);        /* bench：自动停 + 求解 */
              }
          }
        break;

      case 2:
        if (g_i.six_face < 6)
          {
            if (g_i.six_acc.n[g_i.six_face] < SIX_FACE_N)
              {
                pw_calib_six_add(&g_i.six_acc, g_i.six_face, a);
                lv_label_set_text_fmt(g_i.grav_prog, PW_STR(IMU_FMT_FACE),
                                      g_i.six_face + 1,
                                      g_i.six_acc.n[g_i.six_face]);
              }
            else if (g_imu_bench)
              {
                imu_grav_cb(NULL);        /* bench：自动翻到下一面 / 求解 */
              }
          }
        break;

      case 3:
        if (g_imu_bench && !g_i.mag_rec && g_i.mag_acc.n == 0)
          {
            imu_mag_cb(NULL);             /* bench：自动开始 */
          }

        if (g_i.mag_rec && have_mag)
          {
            float mean;

            pw_calib_mag_add(&g_i.mag_acc, m);
            mean = sqrtf(g_i.mag_acc.sum_r2 / (float)g_i.mag_acc.n) / 0.001f;

            lv_label_set_text_fmt(g_i.mag_prog, PW_STR(IMU_FMT_SAMPLES),
                                  g_i.mag_acc.n);
            lv_label_set_text_fmt(g_i.mag_res[1], PW_STR(IMU_FMT_MAGMEAN),
                                  (int)mean);

            if (g_imu_bench && g_i.mag_acc.n >= 400)
              {
                imu_mag_cb(NULL);         /* bench：自动停 + 拟合 */
              }
          }
        break;

      case 4:
        if (g_i.traj == NULL)
          {
            break;
          }

        /* 姿态是"去重力"必需的（roll/pitch 决定重力方向；绕竖轴转不影响），
         * 所以本页也跑 AHRS，保证从别的页直接滑过来时姿态已收敛。 */

        if (!g_i.ahrs_inited)
          {
            pw_ahrs_init(&g_i.ahrs);
            g_i.ahrs_inited = 1;
          }

        pw_ahrs_update(&g_i.ahrs, a, g, have_mag ? m : NULL, dt);

        /* 用**零偏改正后**的角速度喂 ZUPT 判据：真机实测未标定陀螺零偏约 2.9 dps，
         * 而"静止"门限是 3 dps —— 拿原始值会让状态在静止/运动之间来回抖，
         * 每抖一次就把零偏与刻度误差积进位置（实测 9 s 漂 1.3 m）。
         * AHRS 自己的 bias 就在收敛估计这个零偏，直接用它，不另起一套。 */

        {
          float gb[3];
          int   k;

          for (k = 0; k < 3; k++)
            {
              gb[k] = g[k] - g_i.ahrs.bias[k];
            }

          /* 姿态对准期只收敛姿态、不积分（见 TJ_ALIGN_N） */

          if (g_i.tj_align > 0)
            {
              g_i.tj_align--;

              if ((g_i.tj_align % 25) == 0)
                {
                  float bg = sqrtf(g_i.ahrs.bias[0] * g_i.ahrs.bias[0] +
                                   g_i.ahrs.bias[1] * g_i.ahrs.bias[1] +
                                   g_i.ahrs.bias[2] * g_i.ahrs.bias[2]);

                  printf("[TRAJ] aligning n=%d bias=%.2f dps\n",
                         g_i.ahrs.n, (double)(bg * 57.29578f));
                }

              if (g_i.tj_align == 0)
                {
                  pw_traj_init(g_i.traj, 0.0f);      /* 对准结束：从这一刻起算轨迹 */
                  g_i.tj_n = 0;
                  g_i.tj_tick = 0;
                  g_i.tj_range = TJ_RANGE0;
                  pw_graph_set_data(g_i.tj_graph, g_i.tjx, g_i.tjy, 0);
                }
              else
                {
                  if (g_i.tj_align == TJ_ALIGN_N - 1)
                    {
                      lv_label_set_text(g_i.tj_stat, PW_STR(IMU_TJ_ALIGN));
                    }
                  break;
                }
            }

          pw_traj_update(g_i.traj, a, gb, g_i.ahrs.q, dt);
        }

        g_i.tj_tick++;

        if (g_i.tj_tick >= TJ_DRAW_DIV)
          {
            float xyz[3];

            g_i.tj_tick = 0;
            pw_traj_pos(g_i.traj, xyz);

            if (!pw_traj_is_still(g_i.traj))
              {
                imu_traj_push(xyz[0], xyz[1]);

                /* 超出视图 90% 就整档翻倍（0.3→0.6→1.2 m），保证推远了还看得见 */

                if (fabsf(xyz[0]) > 0.9f * g_i.tj_range ||
                    fabsf(xyz[1]) > 0.9f * g_i.tj_range)
                  {
                    g_i.tj_range *= 2.0f;
                    pw_graph_set_range(g_i.tj_graph, -g_i.tj_range,
                                       g_i.tj_range, -g_i.tj_range,
                                       g_i.tj_range);
                  }
              }

            pw_graph_set_data(g_i.tj_graph, g_i.tjx, g_i.tjy, g_i.tj_n);
            imu_traj_labels();

            /* 每 5 次重绘（≈0.5 s）往控制台打一行，内容与屏幕一致。
             * 为什么要有它：真机没有 /dev/fb0、也不能拍照时，靠这行就能把
             * "手推了多远"变成可复核的数字（真值测量协议 Experiment A 用它）。
             * 频率用已有的 tj_tick 决定，不新增任何状态。 */

            /* 每 25 拍（0.5 s）一行。判据用 traj->n 而不是 tj_tick：
             * tj_tick 在上面已被清零，用它判断会永远不成立（踩过）。 */

            if ((g_i.traj->n % (TJ_DRAW_DIV * 5)) == 0)
              {
                float amag = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
                float wmag = sqrtf(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);

                float bg = sqrtf(g_i.ahrs.bias[0] * g_i.ahrs.bias[0] +
                                 g_i.ahrs.bias[1] * g_i.ahrs.bias[1] +
                                 g_i.ahrs.bias[2] * g_i.ahrs.bias[2]);

                printf("[TRAJ] t=%.2f x=%+.1f y=%+.1f z=%+.1f cm v=%.1f cm/s "
                       "still=%d n=%d |a|=%.3fg |w|=%.1fdps bad=%d "
                       "bias=%.2fdps\n",
                       (double)pw_traj_time(g_i.traj),
                       (double)(xyz[0] * 100.0f), (double)(xyz[1] * 100.0f),
                       (double)(xyz[2] * 100.0f),
                       (double)(sqrtf(g_i.traj->v[0] * g_i.traj->v[0] +
                                      g_i.traj->v[1] * g_i.traj->v[1] +
                                      g_i.traj->v[2] * g_i.traj->v[2]) * 100.0f),
                       pw_traj_is_still(g_i.traj), g_i.traj->n,
                       (double)amag, (double)(wmag * 57.29578f),
                       g_i.traj->n_bad, (double)(bg * 57.29578f));
              }
          }
        break;

      default:
        break;
    }
}

/****************************************************************************
 * Builders
 ****************************************************************************/

static lv_obj_t *imu_page_new(int pi)
{
  lv_obj_t *pg = lv_obj_create(g_i.scroller);

  lv_obj_set_size(pg, IMU_PAGE_W, IMU_PAGE_H);
  lv_obj_set_pos(pg, pi * IMU_PAGE_W, 0);
  lv_obj_set_style_bg_opa(pg, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(pg, 0, 0);
  lv_obj_set_style_pad_all(pg, 0, 0);
  lv_obj_remove_flag(pg, LV_OBJ_FLAG_SCROLLABLE);

  return pg;
}

static void imu_button(lv_obj_t *parent, int x, int y, int w, int h,
                       lv_event_cb_t cb, const char *text,
                       lv_obj_t **lab_out)
{
  lv_obj_t *btn = pw_card_new(parent, w, h, PW_COL_CARD);
  lv_obj_t *lab;

  lv_obj_set_pos(btn, x, y);
  lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

  lab = pw_label_new(btn, text, PW_FNT_MED, PW_COL_TEXT);
  lv_obj_center(lab);

  if (lab_out != NULL)
    {
      *lab_out = lab;
    }
}

static void imu_build_live(void)
{
  lv_obj_t *pg = imu_page_new(0);
  lv_obj_t *lab;
  static const char *nm[3] = {"roll", "pitch", "yaw"};
  int i;

  lab = pw_label_new(pg, PW_STR(IMU_LIVE), PW_FNT_LARGE, PW_ACC_RAW);
  lv_obj_set_pos(lab, 20, 2);

  g_i.lvl_card = pw_card_new(pg, LVL_CARD_W, LVL_CARD_H, PW_COL_CARD);
  lv_obj_set_pos(g_i.lvl_card, 14, 36);
  lv_obj_remove_flag(g_i.lvl_card, LV_OBJ_FLAG_SCROLLABLE);

  g_i.lvl_h = lv_obj_create(g_i.lvl_card);
  lv_obj_set_size(g_i.lvl_h, LVL_CARD_W - 20, 1);
  lv_obj_set_style_radius(g_i.lvl_h, 0, 0);
  lv_obj_set_style_bg_color(g_i.lvl_h, PW_COL_GRID, 0);
  lv_obj_set_style_border_width(g_i.lvl_h, 0, 0);
  lv_obj_remove_flag(g_i.lvl_h, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_center(g_i.lvl_h);

  g_i.lvl_v = lv_obj_create(g_i.lvl_card);
  lv_obj_set_size(g_i.lvl_v, 1, LVL_CARD_H - 20);
  lv_obj_set_style_radius(g_i.lvl_v, 0, 0);
  lv_obj_set_style_bg_color(g_i.lvl_v, PW_COL_GRID, 0);
  lv_obj_set_style_border_width(g_i.lvl_v, 0, 0);
  lv_obj_remove_flag(g_i.lvl_v, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_center(g_i.lvl_v);

  g_i.lvl_dot = lv_obj_create(g_i.lvl_card);
  lv_obj_set_size(g_i.lvl_dot, LVL_DOT, LVL_DOT);
  lv_obj_set_style_radius(g_i.lvl_dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(g_i.lvl_dot, PW_ACC_RAW, 0);
  lv_obj_set_style_border_width(g_i.lvl_dot, 0, 0);
  lv_obj_remove_flag(g_i.lvl_dot, LV_OBJ_FLAG_SCROLLABLE);
  imu_level_dot(0.0f, 0.0f);

  for (i = 0; i < 3; i++)
    {
      g_i.ax_name[i] = pw_label_new(pg, nm[i], PW_FNT_BODY, PW_COL_DIM);
      lv_obj_set_pos(g_i.ax_name[i], 220, 40 + i * 66);

      g_i.ax_val[i] = pw_label_new(pg, "--", PW_FNT_XL, PW_COL_TEXT);
      lv_obj_set_pos(g_i.ax_val[i], 220, 58 + i * 66);
    }

  g_i.bias_lab = pw_label_new(pg, "", PW_FNT_BODY, PW_COL_DIM);
  lv_obj_set_pos(g_i.bias_lab, 14, 200);

  g_i.live_stat = pw_label_new(pg, "", PW_FNT_BODY, PW_COL_DIM);
  lv_obj_set_pos(g_i.live_stat, 14, 222);

  g_i.live_hint = pw_label_new(pg, PW_STR(IMU_HINT_LIVE), PW_FNT_BODY,
                               PW_COL_FAINT);
  lv_obj_set_pos(g_i.live_hint, 14, 244);
}

static void imu_build_bias(void)
{
  lv_obj_t *pg = imu_page_new(1);
  lv_obj_t *lab;

  lab = pw_label_new(pg, PW_STR(IMU_BIAS_CAL), PW_FNT_LARGE, PW_ACC_RAW);
  lv_obj_set_pos(lab, 20, 2);

  lab = pw_label_new(pg, PW_STR(IMU_HINT_FLAT), PW_FNT_BODY, PW_COL_DIM);
  lv_obj_set_pos(lab, 20, 40);
  lv_obj_set_width(lab, 350);
  lv_label_set_long_mode(lab, LV_LABEL_LONG_WRAP);

  imu_button(pg, 20, 86, 170, 48, imu_bias_cb, PW_STR(IMU_BTN_START),
             &g_i.bias_btn_lab);

  g_i.bias_prog = pw_label_new(pg, "", PW_FNT_MED, PW_COL_TEXT);
  lv_obj_set_pos(g_i.bias_prog, 20, 152);

  g_i.bias_res = pw_label_new(pg, "", PW_FNT_SMALL, PW_COL_TEXT);
  lv_obj_set_pos(g_i.bias_res, 20, 190);

  g_i.bias_disp = pw_label_new(pg, "", PW_FNT_SMALL, PW_COL_DIM);
  lv_obj_set_pos(g_i.bias_disp, 20, 214);

  g_i.bias_stat = pw_label_new(pg, "", PW_FNT_SMALL, PW_ACC_MECH);
  lv_obj_set_pos(g_i.bias_stat, 20, 244);
}

static void imu_build_grav(void)
{
  lv_obj_t *pg = imu_page_new(2);
  lv_obj_t *lab;

  lab = pw_label_new(pg, PW_STR(IMU_GRAV_CAL), PW_FNT_LARGE, PW_ACC_RAW);
  lv_obj_set_pos(lab, 20, 2);

  lab = pw_label_new(pg, PW_STR(IMU_HINT_GRAV), PW_FNT_BODY, PW_COL_DIM);
  lv_obj_set_pos(lab, 20, 34);
  lv_obj_set_width(lab, 350);
  lv_label_set_long_mode(lab, LV_LABEL_LONG_WRAP);

  g_i.grav_face = pw_label_new(pg, "", PW_FNT_XL, PW_COL_TEXT);
  lv_obj_set_pos(g_i.grav_face, 20, 84);

  imu_button(pg, 20, 130, 190, 48, imu_grav_cb, PW_STR(IMU_BTN_TAKE),
             &g_i.grav_btn_lab);

  g_i.grav_prog = pw_label_new(pg, "", PW_FNT_BODY, PW_COL_DIM);
  lv_obj_set_pos(g_i.grav_prog, 20, 190);

  g_i.grav_res[0] = pw_label_new(pg, "", PW_FNT_SMALL, PW_COL_TEXT);
  lv_obj_set_pos(g_i.grav_res[0], 20, 218);
  g_i.grav_res[1] = pw_label_new(pg, "", PW_FNT_SMALL, PW_COL_TEXT);
  lv_obj_set_pos(g_i.grav_res[1], 20, 240);
  g_i.grav_res[2] = pw_label_new(pg, "", PW_FNT_SMALL, PW_COL_DIM);
  lv_obj_set_pos(g_i.grav_res[2], 20, 262);

  g_i.grav_stat = pw_label_new(pg, "", PW_FNT_SMALL, PW_ACC_MECH);
  lv_obj_set_pos(g_i.grav_stat, 20, 292);

  imu_grav_face_label();
}

static void imu_build_mag(void)
{
  lv_obj_t *pg = imu_page_new(3);
  lv_obj_t *lab;

  lab = pw_label_new(pg, PW_STR(IMU_MAG_CAL), PW_FNT_LARGE, PW_ACC_RAW);
  lv_obj_set_pos(lab, 20, 2);

  lab = pw_label_new(pg, PW_STR(IMU_HINT_MAG), PW_FNT_BODY, PW_COL_DIM);
  lv_obj_set_pos(lab, 20, 40);
  lv_obj_set_width(lab, 350);
  lv_label_set_long_mode(lab, LV_LABEL_LONG_WRAP);

  imu_button(pg, 20, 86, 170, 48, imu_mag_cb, PW_STR(IMU_BTN_START),
             &g_i.mag_btn_lab);

  g_i.mag_prog = pw_label_new(pg, "", PW_FNT_BODY, PW_COL_TEXT);
  lv_obj_set_pos(g_i.mag_prog, 20, 152);

  g_i.mag_res[0] = pw_label_new(pg, "", PW_FNT_SMALL, PW_COL_TEXT);
  lv_obj_set_pos(g_i.mag_res[0], 20, 190);
  g_i.mag_res[1] = pw_label_new(pg, "", PW_FNT_SMALL, PW_COL_TEXT);
  lv_obj_set_pos(g_i.mag_res[1], 20, 214);
  g_i.mag_res[2] = pw_label_new(pg, "", PW_FNT_SMALL, PW_COL_DIM);
  lv_obj_set_pos(g_i.mag_res[2], 20, 238);

  g_i.mag_stat = pw_label_new(pg, "", PW_FNT_SMALL, PW_ACC_MECH);
  lv_obj_set_pos(g_i.mag_stat, 20, 268);
}

/* ---- page 4：轨迹（相对位移）----
 * 画的是**世界系水平面内的相对位移**（X-Y），Z 只给数值不上图（竖向误差最大，
 * 上图容易让人以为"高度也准"）。留痕只在运动时落点：静止段 ZUPT 已把位置钉住，
 * 再落点只是同一处堆点。 */

static void imu_traj_labels(void)
{
  /* PW_STR() 是 token-paste 宏，参数里不能带下标表达式，
   * 所以这里用运行时 pw_str(id)：id 必须是编译期常量枚举。 */

  static const int axis_id[3] =
  {
    PW_STR_IMU_TJ_X, PW_STR_IMU_TJ_Y, PW_STR_IMU_TJ_Z
  };

  float xyz[3];
  int   i;

  if (g_i.traj == NULL)
    {
      return;
    }

  pw_traj_pos(g_i.traj, xyz);

  for (i = 0; i < 3; i++)
    {
      lv_label_set_text_fmt(g_i.tj_val[i], pw_str(axis_id[i]),
                            (double)(xyz[i] * 100.0f));
    }

  lv_label_set_text_fmt(g_i.tj_vel, PW_STR(IMU_FMT_TJ_V),
                        (double)(sqrtf(g_i.traj->v[0] * g_i.traj->v[0] +
                                       g_i.traj->v[1] * g_i.traj->v[1] +
                                       g_i.traj->v[2] * g_i.traj->v[2]) *
                                 100.0f));
  lv_label_set_text_fmt(g_i.tj_time, PW_STR(IMU_FMT_TJ_T),
                        (double)pw_traj_time(g_i.traj));
  lv_label_set_text_fmt(g_i.tj_scale, PW_STR(IMU_FMT_TJ_SCALE),
                        (double)(g_i.tj_range * 50.0f));

  if (pw_traj_is_still(g_i.traj))
    {
      lv_label_set_text(g_i.tj_stat, PW_STR(IMU_TJ_STILL));
      lv_obj_set_style_text_color(g_i.tj_stat, PW_COL_DIM, 0);
    }
  else
    {
      lv_label_set_text(g_i.tj_stat, PW_STR(IMU_TJ_MOVING));
      lv_obj_set_style_text_color(g_i.tj_stat, PW_ACC_RAW, 0);
    }
}

static void imu_traj_push(float x, float y)
{
  if (g_i.tj_n >= PW_GRAPH_MAX_POINTS)
    {
      memmove(g_i.tjx, g_i.tjx + 1, sizeof(float) * (PW_GRAPH_MAX_POINTS - 1));
      memmove(g_i.tjy, g_i.tjy + 1, sizeof(float) * (PW_GRAPH_MAX_POINTS - 1));
      g_i.tj_n = PW_GRAPH_MAX_POINTS - 1;
    }

  g_i.tjx[g_i.tj_n] = x;
  g_i.tjy[g_i.tj_n] = y;
  g_i.tj_n++;
}

static void imu_traj_reset_cb(lv_event_t *e)
{
  lv_obj_t *btn = lv_event_get_target(e);

  if (g_i.traj == NULL)
    {
      return;
    }

  pw_traj_init(g_i.traj, 0.0f);      /* 只归零，不修漂移（界面已如实标注） */
  g_i.tj_n = 0;
  g_i.tj_tick = 0;
  g_i.tj_range = TJ_RANGE0;

  pw_graph_set_data(g_i.tj_graph, g_i.tjx, g_i.tjy, 0);
  imu_traj_labels();

  /* ③ 归零回弹：按钮轻弹一次（C3），告知"已经清零" */
  pw_motion_settle_y(btn, 0, 3, 260);   /* 该按钮建在 y=0；先清抬起动画再弹 */
}

static void imu_build_traj(void)
{
  lv_obj_t *pg = imu_page_new(4);
  lv_obj_t *lab;
  int i;

  lab = pw_label_new(pg, PW_STR(IMU_TRAJ), PW_FNT_LARGE, PW_ACC_RAW);
  lv_obj_set_pos(lab, 20, 2);

  imu_button(pg, 282, 0, 96, 44, imu_traj_reset_cb,
             PW_STR(IMU_TJ_BTN_RESET), NULL);

  /* 页面级缓冲全部走堆：失败就整页降级（显示提示），不留半截界面 */

  if (g_i.traj == NULL)
    {
      g_i.traj = lv_malloc_zeroed(sizeof(struct pw_traj_s));
      g_i.tjx = lv_malloc(sizeof(float) * PW_GRAPH_MAX_POINTS);
      g_i.tjy = lv_malloc(sizeof(float) * PW_GRAPH_MAX_POINTS);
    }

  if (g_i.traj == NULL || g_i.tjx == NULL || g_i.tjy == NULL)
    {
      lab = pw_label_new(pg, PW_STR(IMU_TJ_NOMEM), PW_FNT_BODY, PW_COL_FAINT);
      lv_obj_set_pos(lab, 20, 60);
      return;
    }

  pw_traj_init(g_i.traj, 0.0f);
  g_i.tj_n = 0;
  g_i.tj_tick = 0;
  g_i.tj_align = TJ_ALIGN_N;
  g_i.tj_range = TJ_RANGE0;

  g_i.tj_graph = pw_graph_create(pg, 236, 236, PW_ACC_RAW, PW_COL_CARD, 0);
  if (g_i.tj_graph == NULL)
    {
      lab = pw_label_new(pg, PW_STR(IMU_TJ_NOMEM), PW_FNT_BODY, PW_COL_FAINT);
      lv_obj_set_pos(lab, 20, 60);
      return;
    }

  lv_obj_set_pos(pw_graph_obj(g_i.tj_graph), 10, 46);
  pw_graph_set_range(g_i.tj_graph, -TJ_RANGE0, TJ_RANGE0,
                     -TJ_RANGE0, TJ_RANGE0);

  for (i = 0; i < 3; i++)
    {
      g_i.tj_val[i] = pw_label_new(pg, "", PW_FNT_SMALL, PW_COL_TEXT);
      lv_obj_set_pos(g_i.tj_val[i], 252, 56 + i * 24);
    }

  g_i.tj_vel = pw_label_new(pg, "", PW_FNT_BODY, PW_COL_DIM);
  lv_obj_set_pos(g_i.tj_vel, 252, 140);

  g_i.tj_time = pw_label_new(pg, "", PW_FNT_BODY, PW_COL_DIM);
  lv_obj_set_pos(g_i.tj_time, 252, 162);

  g_i.tj_stat = pw_label_new(pg, "", PW_FNT_BODY, PW_COL_DIM);
  lv_obj_set_pos(g_i.tj_stat, 252, 192);

  g_i.tj_scale = pw_label_new(pg, "", PW_FNT_BODY, PW_COL_FAINT);
  lv_obj_set_pos(g_i.tj_scale, 252, 216);

  lab = pw_label_new(pg, PW_STR(IMU_TJ_NOTE), PW_FNT_BODY, PW_COL_FAINT);
  lv_obj_set_pos(lab, 14, 290);

  lab = pw_label_new(pg, PW_STR(IMU_HINT_TRAJ), PW_FNT_BODY, PW_COL_FAINT);
  lv_obj_set_pos(lab, 14, 312);

  imu_traj_labels();
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

lv_obj_t *pw_imu_screen(void)
{
  lv_obj_t *btn;
  lv_obj_t *lab;
  static int dm = -1;
  static int dp = 1;
  int i;

  memset(&g_i, 0, sizeof(g_i));
  g_i.six_face = 0;

  g_i.scr = pw_scr_new();

  /* bench 时标题带 [BENCH]：屏幕上一眼能看出"这是注入数据、不是测量" */

  {
    static char title[40];

    if (g_imu_bench)
      {
        snprintf(title, sizeof(title), "%s [BENCH]", PW_STR(IMU_TITLE));
        g_i.scroller = pw_topbar(g_i.scr, title);
      }
    else
      {
        g_i.scroller = pw_topbar(g_i.scr, PW_STR(IMU_TITLE));
      }
  }
  lv_obj_set_size(g_i.scroller, IMU_PAGE_W, IMU_PAGE_H);
  lv_obj_set_scroll_dir(g_i.scroller, LV_DIR_HOR);
  lv_obj_set_scroll_snap_x(g_i.scroller, LV_SCROLL_SNAP_CENTER);
  lv_obj_set_scrollbar_mode(g_i.scroller, LV_SCROLLBAR_MODE_OFF);
  lv_obj_add_flag(g_i.scroller, LV_OBJ_FLAG_SCROLL_ONE);
  lv_obj_add_event_cb(g_i.scroller, imu_scroll_end_cb, LV_EVENT_SCROLL_END,
                      NULL);

  imu_build_live();
  imu_build_bias();
  imu_build_grav();
  imu_build_mag();
  imu_build_traj();

  btn = pw_card_new(g_i.scr, 64, 44, PW_COL_CARD);
  lv_obj_set_pos(btn, 4, 396);
  lv_obj_add_event_cb(btn, imu_arrow_cb, LV_EVENT_CLICKED, &dm);
  lab = pw_label_new(btn, "<", PW_FNT_XL, PW_COL_DIM);
  lv_obj_center(lab);
  g_i.arrow_btn[0] = btn;

  for (i = 0; i < IMU_PAGES; i++)
    {
      lv_obj_t *d = lv_obj_create(g_i.scr);

      lv_obj_set_size(d, 12, 12);
      lv_obj_set_style_bg_color(d, PW_COL_CARD_LT, 0);
      lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
      lv_obj_set_style_border_width(d, 0, 0);
      lv_obj_remove_flag(d, LV_OBJ_FLAG_SCROLLABLE);
      g_i.dots[i] = d;
      lv_obj_align(d, LV_ALIGN_BOTTOM_MID, (i - (IMU_PAGES - 1) / 2) * 18, -24);
    }

  btn = pw_card_new(g_i.scr, 64, 44, PW_COL_CARD);
  lv_obj_set_pos(btn, IMU_PAGE_W - 4 - 64, 396);
  lv_obj_add_event_cb(btn, imu_arrow_cb, LV_EVENT_CLICKED, &dp);
  lab = pw_label_new(btn, ">", PW_FNT_XL, PW_COL_DIM);
  lv_obj_center(lab);
  g_i.arrow_btn[1] = btn;

  imu_set_page(0);

  g_i.last_ms = lv_tick_get();
  pw_scr_set_tick(g_i.scr, imu_tick_cb, IMU_TICK_MS);

  return g_i.scr;
}

void pw_imu_goto(int idx)
{
  if (g_i.scroller == NULL)
    {
      return;
    }

  imu_set_page(idx);
}
