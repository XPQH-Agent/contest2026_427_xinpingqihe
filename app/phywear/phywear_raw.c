/****************************************************************************
 * apps/examples/phywear/phywear_raw.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/* 原始传感器板块：6 页横滑（Accelerometer / Gyroscope / Magnetometer /
 * Light / Microphone / Speaker）。三个三轴页（加速度/陀螺/地磁）为
 * "一行三列数值 + 迷你实时曲线"：数值列（轴字母用 X/Y/Z 系列色，单位见右上角）
 * 下方是一块 358×186 的曲线区，竖直切成 X/Y/Z 三条泳道，与数值列左右对齐。
 * 麦克风页由后台线程读 /dev/mic0 显示电平，扬声器页可直接放测试音。
 * 数据经 phywear_sensors 层换算为物理单位（g / dps / mG / lux）。
 *
 * 曲线实现：复用 pw_scope（CPU 光栅化 RGB565 → lv_image → EPIC 硬件 blit），
 * 3 条泳道共用 1 块缓冲、每页只占 1 个 scope 槽位；历史样本以 int16 存在
 * struct raw_page_s 里（3×80×2 B/页），不新增 lv_malloc 缓冲。
 *
 * 布局（390×450）：
 *   0..54       顶栏（pw_topbar：返回 + "Raw Sensors"）
 *   54..396     横滑内容区（每页 390×342）
 *   396..450    指示点 + 左右箭头
 *
 * 三轴页内部（390×342）：
 *   4..34       页名 + 单位
 *   36..98      三列数值（轴字母 20px / 数值 28px）
 *   112..298    迷你实时曲线（3 条泳道 × 62 px）
 *   306..324    左下纵轴满量程 / 右下 8 s 时间窗
 */

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <lvgl/lvgl.h>

#include "phywear_ui.h"
#include "phywear_sensors.h"
#include "phywear_raw.h"
#include "phywear_i18n.h"
#include "pw_scope.h"
#include "pw_tone.h"

/****************************************************************************
 * Private Definitions
 ****************************************************************************/

#define RAW_PAGES      6
#define RAW_PAGE_W     390
#define RAW_PAGE_H     342

#define RAW_CONTENT_X  0
#define RAW_CONTENT_Y  PW_TOPBAR_H
#define RAW_CONTENT_H  (PW_SCREEN_H - PW_TOPBAR_H - 54)   /* 342 */

/* 三轴页有两种视图（轻点页面 / 右上角图标切换；默认数值视图 = 改动前原版风格）
 *
 *   RAW_VIEW_NUM   数值：3 行「轴字母 + 48px 大数字（带单位）」，同环境光页风格
 *   RAW_VIEW_CURVE 图线：一行三列数值 + 358×186 三泳道迷你曲线（每泳道独立量程）
 *
 * 两种视图**复用同一批标签对象**（只改字体/位置/宽度/颜色），不新增控件。
 * 默认回到数值视图是用户 2026-09-15 的明确要求。
 */

#define RAW_VIEW_NUM   PW_RAW_VIEW_NUM
#define RAW_VIEW_CURVE PW_RAW_VIEW_CURVE

/* 数值视图（原版版式） */

#define RAW_ROW_H      90
#define RAW_ROW_Y0     58
#define RAW_VAL_X      72
#define RAW_TAG_X      18

/* 图线视图 */

#define RAW_COL_X0     6
#define RAW_COL_W      126
#define RAW_TAG_Y      36
#define RAW_VAL_Y      62

#define RAW_SCOPE_X    16
#define RAW_SCOPE_Y    112
#define RAW_SCOPE_W    358
#define RAW_SCOPE_H    186                    /* 3 条泳道 × 62 px */
#define RAW_SCOPE_LANES 3

#define RAW_RANGE_Y    306

/* 曲线历史：80 × 100 ms = 8.0 s 时间窗（与 RAW_WIN_TXT 保持一致） */

#define RAW_HIST_N     80
#define RAW_WIN_TXT    "8 s"

/* 视图切换按钮（右下角，自绘图标，零字体成本：
 * 画不了"曲线"二字 —— U+66F2「曲」不在本队 CJK 字体子集里，用了会显示方框，
 * 而重生成字体子集有约 0.5 天流程成本） */

#define RAW_TOG_W      46
#define RAW_TOG_H      36
#define RAW_TOG_X      (PW_SCREEN_W - RAW_TOG_W - 12)
#define RAW_TOG_Y      (RAW_CONTENT_Y + 300)

/* 文本格式串（图线视图的数值不带单位，单位见页眉；列宽 126 放不下
 * "+1234.5 dps" 这样的 7 字符组合） */

#define FMT_G   "%+.2f"        /* accel：mg → g */
#define FMT_MG  "%+d"          /* mag：mG 整数 */

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct raw_page_s
{
  lv_obj_t *tag[3];     /* X/Y/Z 轴标签 */
  lv_obj_t *val[3];     /* 数值标签（3 轴页：轴值；光页：主数值） */
  lv_obj_t *sub[2];     /* 次级小字（光页：CH0/CH1） */

  /* 迷你实时曲线（仅 3 轴页；光/麦克风/扬声器页不用） */

  lv_obj_t *scope;      /* pw_scope 图像对象（3 条泳道共用 1 个槽位） */
  lv_obj_t *unitlab[3]; /* 数值右侧的单位列（大数字视图；曲线视图隐藏） */
  lv_obj_t *numline[2]; /* 数值行之间的 1px hairline 分隔（大数字视图） */
  lv_obj_t *rangelab;   /* 左下：纵轴满量程（自动量程） */
  lv_obj_t *winlab;     /* 右下：横轴时间窗 */
  const char *unit;     /* 单位串（"g"/"dps"/"mG"，静态存储） */
  float     k;          /* 物理单位 → int16 的换算系数 */
  float     fs[3];      /* **每泳道各自的**满量程（物理单位） */
  float     fs_lo;      /* 满量程下限（也是初值） */
  float     fs_hi;      /* 满量程上限（受 int16 量程约束） */
  int       nfill;      /* 已填入样本数；< N 时整条铺当前值 */
};

struct raw_ui_s
{
  lv_obj_t   *scr;      /* 整屏 */
  lv_obj_t   *scroller; /* 横滑容器 */
  lv_obj_t   *dots[RAW_PAGES];
  int         idx;      /* 当前页 */
  struct raw_page_s pg[RAW_PAGES];
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct raw_ui_s g_raw;

/* 三轴页当前视图：0=数值（默认） 1=图线。三个三轴页共用（跨页一致）。 */

static int g_raw_view = RAW_VIEW_NUM;

/* 视图切换按钮与自绘图标 */

static lv_obj_t *g_tog_btn;
static lv_obj_t *g_tog_line;                 /* 折线图标 */
static lv_obj_t *g_tog_bar[3];               /* 三横条图标 */
static lv_point_precise_t g_tog_pt[5];

/* 曲线历史：任何时刻只有"当前页"在采样，且切页时 raw_set_page() 会把
 * nfill 清零（整条重铺），所以三轴页共用这一份 3×RAW_HIST_N 的 int16 即可。
 * 若按页各存一份，struct raw_page_s 有 6 个实例，会白占 3 倍 SRAM
 * （实测 2 880 B vs 480 B）。 */

static int16_t g_raw_hist[3][RAW_HIST_N];

static const int g_delta_prev = -1;
static const int g_delta_next = 1;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* 麦克风采集线程：定义在本文件后部，这里先声明（raw_set_page 会用到） */

static void raw_mic_start(void);
static void raw_mic_stop(void);

/* 视图切换相关（定义在文件中部，tick 与页面构建都会用到） */

static void raw_toggle_cb(lv_event_t *e);
static void raw_toggle_icon_update(void);

/****************************************************************************
 * Name: raw_sync_dots
 ****************************************************************************/

static void raw_sync_dots(void)
{
  int i;
  lv_color_t on  = PW_ACC_RAW;
  lv_color_t off = PW_COL_CARD_LT;

  for (i = 0; i < RAW_PAGES; i++)
    {
      lv_obj_set_style_bg_color(g_raw.dots[i], (i == g_raw.idx) ? on : off, 0);
    }
}

/****************************************************************************
 * Name: raw_set_page
 ****************************************************************************/

static void raw_set_page(int idx)
{
  if (idx < 0)
    {
      idx = 0;
    }

  if (idx >= RAW_PAGES)
    {
      idx = RAW_PAGES - 1;
    }

  g_raw.idx = idx;
  raw_sync_dots();

  /* 视图切换按钮只在三个三轴页显示（光/麦克风/扬声器页没有第二种视图） */

  if (g_tog_btn != NULL)
    {
      if (idx <= 2)
        {
          lv_obj_remove_flag(g_tog_btn, LV_OBJ_FLAG_HIDDEN);
        }
      else
        {
          lv_obj_add_flag(g_tog_btn, LV_OBJ_FLAG_HIDDEN);
        }
    }

  /* 进入新页即清空该页曲线历史：曲线只表示"本次进入后连续采样的 8 s"，
   * 不把上次来访的旧样本接在时间轴前面（传感器只在当前页被读取）。 */

  g_raw.pg[idx].nfill = 0;

  /* 只有麦克风页需要采集线程（离开即停，避免与频谱页抢 /dev/mic0） */

  if (idx == 4)
    {
      /* 麦克风与扬声器共用片内 AUDCODEC：真机实测两者同时工作会卡死
       * （喇叭放音 + 频谱页采样，约几分钟后看门狗复位），
       * 所以进入麦克风页先停掉正在播放的声音。 */

      pw_tone_stop();
      raw_mic_start();
    }
  else
    {
      raw_mic_stop();
    }
}

/****************************************************************************
 * Name: raw_scroll_to
 ****************************************************************************/

static void raw_scroll_to(int idx, bool anim)
{
  if (idx < 0 || idx >= RAW_PAGES)
    {
      return;
    }

  raw_set_page(idx);
  lv_obj_scroll_to_x(g_raw.scroller, idx * RAW_PAGE_W,
                     anim ? LV_ANIM_ON : LV_ANIM_OFF);
}

/****************************************************************************
 * Name: raw_del_cb
 ****************************************************************************/

static void raw_del_cb(lv_event_t *e)
{
  (void)e;

  raw_mic_stop();
}

/****************************************************************************
 * Name: raw_arrow_cb
 ****************************************************************************/

static void raw_arrow_cb(lv_event_t *e)
{
  FAR const int *dp = lv_event_get_user_data(e);

  raw_scroll_to(g_raw.idx + *dp, true);
}

/****************************************************************************
 * Name: raw_scroll_end_cb
 ****************************************************************************/

static void raw_scroll_end_cb(lv_event_t *e)
{
  int32_t x = lv_obj_get_scroll_x(g_raw.scroller);
  int idx = (int)((x + RAW_PAGE_W / 2) / RAW_PAGE_W);

  raw_set_page(idx);
}

/****************************************************************************
 * Name: raw_axis_colors
 *
 * Description:
 *   X/Y/Z 三轴固定配色（红/绿/蓝惯例）；加速度/陀螺/地磁三页一致，
 *   便于跨页对照。
 *
 ****************************************************************************/

static void raw_axis_colors(FAR lv_color_t *c)
{
  c[0] = PW_SER_RED;
  c[1] = PW_SER_GREEN;
  c[2] = PW_SER_BLUE;
}

/****************************************************************************
 * Name: raw_fs_str
 *
 * Description:
 *   满量程转字符串。量程按 1-2-5 步进，可能小于 1（如 0.2 g），
 *   直接 (int) 会打成 0。
 *
 ****************************************************************************/

static void raw_fs_str(float fs, char *out, size_t n)
{
  if (fs >= 1.0f)
    {
      snprintf(out, n, "%d", (int)fs);
    }
  else
    {
      snprintf(out, n, "%.2f", (double)fs);
    }
}

/****************************************************************************
 * Name: raw_axis_q
 *
 * Description:
 *   物理量 → int16 存储（四舍五入并饱和，满量程外直接贴边）。
 *
 ****************************************************************************/

static int16_t raw_axis_q(float v, float k)
{
  float q = v * k;

  if (q > 32767.0f)
    {
      q = 32767.0f;
    }
  else if (q < -32767.0f)
    {
      q = -32767.0f;
    }

  return (int16_t)(q + ((q >= 0.0f) ? 0.5f : -0.5f));
}

/****************************************************************************
 * Name: raw_nice_fs
 *
 * Description:
 *   取 {1,2,5}×10^n 中最小的 ≥ v（且不小于下限 lo）者作为满量程标称值。
 *   三轴页的量程下限都 ≥ 1，因此返回值恒为整数，可直接 %d 打印。
 *
 ****************************************************************************/

static float raw_nice_fs(float v, float lo)
{
  float p = 1.0f;

  if (v < lo)
    {
      v = lo;
    }

  while (p < v && p < 1.0e6f)
    {
      p *= 10.0f;
    }

  if (p * 0.2f >= v)
    {
      return p * 0.2f;
    }

  if (p * 0.5f >= v)
    {
      return p * 0.5f;
    }

  return p;
}

/****************************************************************************
 * Name: raw_axis_curve_update
 *
 * Description:
 *   把一帧三轴数值压入历史并重画迷你曲线（仅当前可见页真正重画）。
 *   自动量程：涨快落慢（回落需低于半量程），避免曲线随噪声忽大忽小。
 *
 ****************************************************************************/

static void raw_axis_curve_update(int pi, FAR const float *v)
{
  struct raw_page_s *p = &g_raw.pg[pi];
  int i;
  int j;

  /* 1) 压历史：进入本页后的第一帧把整条铺成当前值（否则会看到一条零线），
   * 之后每帧左移一格、从右端追加 —— 曲线立刻从右侧长出来，
   * 而不是先整条"定格"8 秒再开始滚动。 */

  if (p->nfill == 0)
    {
      p->nfill = 1;

      for (i = 0; i < 3; i++)
        {
          int16_t q = raw_axis_q(v[i], p->k);

          for (j = 0; j < RAW_HIST_N; j++)
            {
              g_raw_hist[i][j] = q;
            }
        }
    }
  else
    {
      for (i = 0; i < 3; i++)
        {
          memmove(&g_raw_hist[i][0], &g_raw_hist[i][1],
                  (RAW_HIST_N - 1) * sizeof(int16_t));
          g_raw_hist[i][RAW_HIST_N - 1] = raw_axis_q(v[i], p->k);
        }
    }

  /* 2) **每轴独立**自动量程：各轴取自己那条窗口的峰值，而不是三轴共用一个 ——
   * 共用时量程会被最大的那根顶住（重力让 Z≈1g → fs=2g），于是 0.12g 的 Y 轴
   * 在 62px 泳道里只剩 ~3px 摆动，看着就是直线（用户 2026-09-15 的原话：
   * "曲线优化不行，变化不明显"）。同时仍取整条窗口峰值而非瞬时值，
   * 否则正弦过零点会掉档（曾出现"数值写 ±50 dps、曲线按 20 画"）。 */

  for (i = 0; i < 3; i++)
    {
      float peak = 0.0f;
      float target;

      for (j = 0; j < RAW_HIST_N; j++)
        {
          float a = fabsf((float)g_raw_hist[i][j]);

          if (a > peak)
            {
              peak = a;
            }
        }

      target = raw_nice_fs(peak / p->k * 1.1f, p->fs_lo);
      if (target > p->fs_hi)
        {
          target = p->fs_hi;
        }

      if (target > p->fs[i])
        {
          p->fs[i] = target;               /* 超量程：立即抬高 */
        }
      else if (target < p->fs[i] * 0.5f)
        {
          p->fs[i] = target;               /* 回落：低于半量程才降 */
        }
    }

  /* 3) 重画（离屏页等切回来时再画，省一次 EPIC blit） */

  if (g_raw.idx != pi || p->scope == NULL)
    {
      return;
    }

  {
    lv_color_t c[3];
    float inv[3];
    char buf[32];

    raw_axis_colors(c);

    for (i = 0; i < 3; i++)
      {
        inv[i] = 1.0f / (p->fs[i] * p->k);
      }

    pw_scope_set_lanes_i16(p->scope, &g_raw_hist[0][0], RAW_SCOPE_LANES,
                           RAW_HIST_N, inv, c);

    /* 每轴一个满量程，按 X/Y/Z 顺序报出来（泳道颜色与轴字母一致，可对照） */

    {
      char f0[12];
      char f1[12];
      char f2[12];

      raw_fs_str(p->fs[0], f0, sizeof(f0));
      raw_fs_str(p->fs[1], f1, sizeof(f1));
      raw_fs_str(p->fs[2], f2, sizeof(f2));
      snprintf(buf, sizeof(buf), "+/-%s / %s / %s %s", f0, f1, f2, p->unit);
      lv_label_set_text(p->rangelab, buf);
    }
  }
}

/****************************************************************************
 * Name: raw_toggle_icon_update
 *
 * Description:
 *   切换按钮上的图标表示"点下去会看到什么"：
 *     数值视图 → 折线图标（点它去看图线）
 *     图线视图 → 三横条图标（点它去看数值）
 *   全部用 LVGL 图元自绘 —— 字体子集里没有"曲"字（U+66F2），写"曲线"会显示方框。
 *
 ****************************************************************************/

static void raw_toggle_icon_update(void)
{
  bool curve = (g_raw_view == RAW_VIEW_CURVE);
  int i;

  if (g_tog_btn == NULL)
    {
      return;
    }

  if (curve)
    {
      lv_obj_add_flag(g_tog_line, LV_OBJ_FLAG_HIDDEN);
    }
  else
    {
      lv_obj_remove_flag(g_tog_line, LV_OBJ_FLAG_HIDDEN);
    }

  for (i = 0; i < 3; i++)
    {
      if (curve)
        {
          lv_obj_remove_flag(g_tog_bar[i], LV_OBJ_FLAG_HIDDEN);
        }
      else
        {
          lv_obj_add_flag(g_tog_bar[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/****************************************************************************
 * Name: raw_apply_view
 *
 * Description:
 *   按当前视图重排三个三轴页。两种视图**复用同一批标签对象**（只改字体/
 *   位置/宽度/颜色），因此不新增控件、不额外吃 LVGL 堆。
 *
 ****************************************************************************/

static void raw_apply_view(void)
{
  bool curve = (g_raw_view == RAW_VIEW_CURVE);
  lv_color_t acol[3];
  int pi;
  int i;

  raw_axis_colors(acol);

  for (pi = 0; pi < 3; pi++)
    {
      struct raw_page_s *p = &g_raw.pg[pi];

      for (i = 0; i < 3; i++)
        {
          if (curve)
            {
              int colx = RAW_COL_X0 + i * RAW_COL_W;

              lv_obj_set_style_text_font(p->tag[i], PW_FNT_MED, 0);
              lv_obj_set_style_text_color(p->tag[i], acol[i], 0);
              lv_obj_set_width(p->tag[i], RAW_COL_W);
              lv_obj_set_style_text_align(p->tag[i],
                                          LV_TEXT_ALIGN_CENTER, 0);
              lv_obj_set_pos(p->tag[i], colx, RAW_TAG_Y);

              lv_obj_set_style_text_font(p->val[i], PW_FNT_XL, 0);
              lv_obj_set_width(p->val[i], RAW_COL_W);
              lv_obj_set_style_text_align(p->val[i],
                                          LV_TEXT_ALIGN_CENTER, 0);
              lv_obj_set_pos(p->val[i], colx, RAW_VAL_Y);
            }
          else
            {
              int rowy = RAW_ROW_Y0 + i * RAW_ROW_H;

              lv_obj_set_style_text_font(p->tag[i], PW_FNT_XL, 0);
              lv_obj_set_style_text_color(p->tag[i], PW_COL_FAINT, 0);
              lv_obj_set_width(p->tag[i], LV_SIZE_CONTENT);
              lv_obj_set_pos(p->tag[i], RAW_TAG_X, rowy + 34);

              lv_obj_set_style_text_font(p->val[i], PW_FNT_XXL, 0);
              lv_obj_set_width(p->val[i], LV_SIZE_CONTENT);
              lv_obj_set_pos(p->val[i], RAW_VAL_X, rowy + 8);
            }
        }

      /* 曲线相关对象只在图线视图显示 */

      if (p->scope != NULL)
        {
          if (curve)
            {
              lv_obj_remove_flag(p->scope, LV_OBJ_FLAG_HIDDEN);
            }
          else
            {
              lv_obj_add_flag(p->scope, LV_OBJ_FLAG_HIDDEN);
            }
        }

      for (i = 0; i < 3; i++)
        {
          if (p->unitlab[i] != NULL)
            {
              if (curve)
                {
                  lv_obj_add_flag(p->unitlab[i], LV_OBJ_FLAG_HIDDEN);
                }
              else
                {
                  lv_obj_remove_flag(p->unitlab[i], LV_OBJ_FLAG_HIDDEN);
                }
            }
        }

      for (i = 0; i < 2; i++)
        {
          if (p->numline[i] != NULL)
            {
              if (curve)
                {
                  lv_obj_add_flag(p->numline[i], LV_OBJ_FLAG_HIDDEN);
                }
              else
                {
                  lv_obj_remove_flag(p->numline[i], LV_OBJ_FLAG_HIDDEN);
                }
            }
        }

      if (p->rangelab != NULL)
        {
          if (curve)
            {
              lv_obj_remove_flag(p->rangelab, LV_OBJ_FLAG_HIDDEN);
            }
          else
            {
              lv_obj_add_flag(p->rangelab, LV_OBJ_FLAG_HIDDEN);
            }
        }

      if (p->winlab != NULL)
        {
          if (curve)
            {
              lv_obj_remove_flag(p->winlab, LV_OBJ_FLAG_HIDDEN);
            }
          else
            {
              lv_obj_add_flag(p->winlab, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }

  raw_toggle_icon_update();
}

/****************************************************************************
 * Name: raw_toggle_cb
 *
 * Description:
 *   切换三轴页视图。挂在右下角按钮与每个三轴页对象上（轻点页面任意处
 *   也能切；LVGL 区分短按与拖动，不会抢横滑翻页）。
 *
 ****************************************************************************/

static void raw_toggle_cb(lv_event_t *e)
{
  (void)e;

  g_raw_view = (g_raw_view == RAW_VIEW_NUM) ? RAW_VIEW_CURVE : RAW_VIEW_NUM;
  raw_apply_view();

  /* 切过去立刻补一帧，不必等下一个 100ms 节拍 */

  if (g_raw.idx <= 2)
    {
      struct pw_imu_s imu;

      if (g_raw.idx == 2)
        {
          struct pw_mag_s mag;

          if (pw_sensors_read_mag(&mag) == 0)
            {
              float v[3];
              int i;

              v[0] = (float)mag.x;
              v[1] = (float)mag.y;
              v[2] = (float)mag.z;

              for (i = 0; i < 3; i++)
                {
                  lv_label_set_text_fmt(g_raw.pg[2].val[i], "%+d", (int)v[i]);
                }

              raw_axis_curve_update(2, v);
            }
        }
      else if (pw_sensors_read_imu(&imu) == 0)
        {
          float v[3];
          int i;

          if (g_raw.idx == 0)
            {
              v[0] = imu.ax / 1000.0f;
              v[1] = imu.ay / 1000.0f;
              v[2] = imu.az / 1000.0f;
            }
          else
            {
              v[0] = imu.gx / 1000.0f;
              v[1] = imu.gy / 1000.0f;
              v[2] = imu.gz / 1000.0f;
            }

          for (i = 0; i < 3; i++)
            {
              lv_label_set_text_fmt(g_raw.pg[g_raw.idx].val[i], "%+.1f", v[i]);
            }

          raw_axis_curve_update(g_raw.idx, v);
        }
    }
}

/****************************************************************************
 * Name: raw_toggle_build
 *
 * Description:
 *   建右下角的视图切换按钮 + 两个自绘图标（折线 / 三横条）。
 *
 ****************************************************************************/

static void raw_toggle_build(lv_obj_t *parent)
{
  int i;

  g_tog_btn = lv_obj_create(parent);
  lv_obj_set_size(g_tog_btn, RAW_TOG_W, RAW_TOG_H);
  lv_obj_set_pos(g_tog_btn, RAW_TOG_X, RAW_TOG_Y);
  lv_obj_remove_flag(g_tog_btn, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_radius(g_tog_btn, 6, 0);          /* 小面积圆角 */
  lv_obj_set_style_bg_color(g_tog_btn, PW_COL_CARD, 0);
  lv_obj_set_style_border_width(g_tog_btn, 0, 0);
  lv_obj_set_style_pad_all(g_tog_btn, 0, 0);
  lv_obj_add_event_cb(g_tog_btn, raw_toggle_cb, LV_EVENT_CLICKED, NULL);

  /* 折线图标：一条 5 点折线 */

  g_tog_line = lv_line_create(g_tog_btn);
  lv_obj_set_size(g_tog_line, 30, 20);
  lv_obj_set_pos(g_tog_line, 8, 8);
  lv_obj_remove_flag(g_tog_line, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_opa(g_tog_line, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(g_tog_line, 0, 0);
  lv_obj_set_style_pad_all(g_tog_line, 0, 0);
  lv_obj_set_style_line_width(g_tog_line, 2, 0);
  lv_obj_set_style_line_color(g_tog_line, PW_ACC_RAW, 0);
  lv_obj_set_style_line_rounded(g_tog_line, true, 0);

  g_tog_pt[0].x = 0;
  g_tog_pt[0].y = 16;
  g_tog_pt[1].x = 8;
  g_tog_pt[1].y = 6;
  g_tog_pt[2].x = 15;
  g_tog_pt[2].y = 12;
  g_tog_pt[3].x = 22;
  g_tog_pt[3].y = 2;
  g_tog_pt[4].x = 30;
  g_tog_pt[4].y = 9;
  lv_line_set_points(g_tog_line, g_tog_pt, 5);

  /* 三横条图标：3 个小矩形（填充走 EPIC 加速路径，不占字体） */

  for (i = 0; i < 3; i++)
    {
      lv_obj_t *b = lv_obj_create(g_tog_btn);

      lv_obj_set_size(b, 22 - i * 5, 3);
      lv_obj_set_pos(b, 8 + i * 5, 9 + i * 7);
      lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
      lv_obj_set_style_radius(b, 0, 0);
      lv_obj_set_style_bg_color(b, PW_COL_TEXT, 0);
      lv_obj_set_style_border_width(b, 0, 0);
      lv_obj_set_style_pad_all(b, 0, 0);
      g_tog_bar[i] = b;
    }

  raw_toggle_icon_update();
}

/****************************************************************************
 * Name: raw_build_axis_page
 *
 * Description:
 *   构建 3 轴数值页（加速度/陀螺仪/地磁共用）：一行三列数值 + 三泳道
 *   迷你实时曲线。
 *
 *   k / fs_lo / fs_hi 为曲线存储与自动量程参数：
 *     accel  k=4000（±8.19 g）   fs 1..8 g
 *     gyro   k=16  （±2048 dps） fs 20..2000 dps
 *     mag    k=1   （±32767 mG） fs 100..30000 mG
 *
 ****************************************************************************/

static void raw_build_axis_page(int pi, int x, const char *name,
                                const char *unit, lv_color_t accent,
                                float k, float fs_lo, float fs_hi)
{
  lv_obj_t *pg = lv_obj_create(g_raw.scroller);
  lv_obj_t *sub;
  lv_obj_t *uni;
  static const char ax_char[3] = {'X', 'Y', 'Z'};
  lv_color_t acol[3];
  int i;

  lv_obj_set_size(pg, RAW_PAGE_W, RAW_PAGE_H);
  lv_obj_set_pos(pg, x, 0);
  lv_obj_set_style_bg_opa(pg, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(pg, 0, 0);
  lv_obj_set_style_pad_all(pg, 0, 0);
  lv_obj_remove_flag(pg, LV_OBJ_FLAG_SCROLLABLE);

  raw_axis_colors(acol);

  /* 轻点页面任意处 = 切换视图（与右下角按钮同一回调）。
   * LVGL 会区分"短按"与"拖动"，所以不会抢横滑翻页。 */

  lv_obj_add_event_cb(pg, raw_toggle_cb, LV_EVENT_CLICKED, NULL);

  /* 页眉：传感器名 + 单位（单位紧跟名字，把右上角/右下角让给切换按钮） */

  /* 章节标签：3px 强调条 + 名称（v2 观感） */

  {
    lv_obj_t *bar = lv_obj_create(pg);

    lv_obj_set_size(bar, 3, 18);
    lv_obj_set_pos(bar, 14, 8);
    lv_obj_set_style_bg_color(bar, accent, 0);
    lv_obj_set_style_radius(bar, 2, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    pw_deco(bar);
  }

  sub = pw_label_new(pg, name, PW_FNT_LARGE, accent);
  lv_obj_set_pos(sub, 24, 4);

  uni = pw_label_new(pg, unit, PW_FNT_MED, PW_COL_DIM);
  lv_obj_align_to(uni, sub, LV_ALIGN_OUT_RIGHT_MID, 8, 0);

  /* 6 个标签（3 轴字母 + 3 数值）：两种视图复用，仅初始按图线视图建好，
   * 随后由 raw_apply_view() 按当前视图重排。 */

  for (i = 0; i < 3; i++)
    {
      int colx = RAW_COL_X0 + i * RAW_COL_W;
      char tagtxt[2];

      tagtxt[0] = ax_char[i];
      tagtxt[1] = '\0';

      g_raw.pg[pi].tag[i] = pw_label_new(pg, tagtxt, PW_FNT_MED, acol[i]);
      lv_obj_set_width(g_raw.pg[pi].tag[i], RAW_COL_W);
      lv_obj_set_style_text_align(g_raw.pg[pi].tag[i],
                                  LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_set_pos(g_raw.pg[pi].tag[i], colx, RAW_TAG_Y);

      g_raw.pg[pi].val[i] = pw_label_new(pg, "--", PW_FNT_XL, PW_COL_TEXT);
      lv_obj_set_width(g_raw.pg[pi].val[i], RAW_COL_W);
      lv_obj_set_style_text_align(g_raw.pg[pi].val[i],
                                  LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_set_pos(g_raw.pg[pi].val[i], colx, RAW_VAL_Y);
    }

  /* 数字视图增强（v2）：数值右侧固定单位列 + 行间 1px hairline 分隔。
   * 单位放在固定 x（310）而不是 align_to：数值宽度每帧都在变，align_to 会反复重排。
   * 两者都只在"大数字"视图显示，切到曲线视图时隐藏（见 raw_apply_view）。 */

  for (i = 0; i < 3; i++)
    {
      int rowy = RAW_ROW_Y0 + i * RAW_ROW_H;

      g_raw.pg[pi].unitlab[i] = pw_label_new(pg, unit, PW_FNT_MED, PW_COL_DIM);
      lv_obj_set_pos(g_raw.pg[pi].unitlab[i], 310, rowy + 24);

      if (i > 0)
        {
          lv_obj_t *ln = lv_obj_create(pg);

          lv_obj_set_size(ln, 358, 1);
          lv_obj_set_pos(ln, 16, rowy - 16);
          lv_obj_set_style_bg_color(ln, PW_COL_LINE, 0);
          lv_obj_set_style_bg_opa(ln, LV_OPA_50, 0);
          lv_obj_set_style_border_width(ln, 0, 0);
          lv_obj_remove_flag(ln, LV_OBJ_FLAG_SCROLLABLE);
          pw_deco(ln);
          g_raw.pg[pi].numline[i - 1] = ln;
        }
    }

  /* 顶部"实时"指示点：本页正在采样（raw 页 10~15 Hz 持续采集） */

  {
    lv_obj_t *dot = lv_obj_create(pg);

    lv_obj_set_size(dot, 8, 8);
    lv_obj_set_pos(dot, 352, 14);
    lv_obj_set_style_bg_color(dot, PW_COL_LIVE, 0);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    pw_deco(dot);
  }

  /* 迷你实时曲线：一块 358×186 的 pw_scope 缓冲竖直切成 3 条泳道 */

  g_raw.pg[pi].unit = unit;
  g_raw.pg[pi].k = k;
  g_raw.pg[pi].fs[0] = fs_lo;
  g_raw.pg[pi].fs[1] = fs_lo;
  g_raw.pg[pi].fs[2] = fs_lo;
  g_raw.pg[pi].fs_lo = fs_lo;
  g_raw.pg[pi].fs_hi = fs_hi;

  g_raw.pg[pi].scope = pw_scope_create(pg, RAW_SCOPE_W, RAW_SCOPE_H,
                                       acol[0], PW_COL_CARD);
  if (g_raw.pg[pi].scope != NULL)
    {
      lv_obj_set_pos(g_raw.pg[pi].scope, RAW_SCOPE_X, RAW_SCOPE_Y);
      pw_scope_axes(g_raw.pg[pi].scope, PW_COL_GRID);
    }

  /* 左下：纵轴满量程（自动量程）；右下：横轴时间窗 */

  g_raw.pg[pi].rangelab = pw_label_new(pg, "", PW_FNT_BODY, PW_COL_DIM);
  lv_obj_set_pos(g_raw.pg[pi].rangelab, 20, RAW_RANGE_Y);

  /* 先按量程下限显示：传感器就绪前（或模拟器无该传感器时）也不留空 */

  {
    char buf[24];

    char f[12];

    raw_fs_str(fs_lo, f, sizeof(f));
    snprintf(buf, sizeof(buf), "+/-%s / %s / %s %s", f, f, f, unit);
    lv_label_set_text(g_raw.pg[pi].rangelab, buf);
  }

  /* 时间窗靠右但在切换按钮左侧，避免压在一起 */

  g_raw.pg[pi].winlab = pw_label_new(pg, RAW_WIN_TXT, PW_FNT_BODY,
                                     PW_COL_FAINT);
  lv_obj_align(g_raw.pg[pi].winlab, LV_ALIGN_TOP_RIGHT,
               -(20 + RAW_TOG_W + 8), RAW_RANGE_Y);
}


/****************************************************************************
 * 麦克风：后台线程读 /dev/mic0（每次 read 阻塞约 64 ms），只保留电平
 * 与最近 96 个样本给页面画柱状图。离开本页即停线程。
 ****************************************************************************/

#define RAW_MIC_N      1024
#define RAW_MIC_BARS   32

static pthread_t g_raw_mic_tid;
static volatile int g_raw_mic_run;
static volatile int g_raw_mic_dbm;      /* 峰值 dBFS（负值，越小越安静） */
static int16_t g_raw_mic_peak[RAW_MIC_BARS];
static lv_obj_t *g_raw_mic_bar[RAW_MIC_BARS];

static void *raw_mic_thread(void *arg)
{
  int fd;
  int16_t buf[RAW_MIC_N];

  (void)arg;

  fd = open("/dev/mic0", O_RDONLY);
  if (fd < 0)
    {
      g_raw_mic_run = 0;
      return NULL;
    }

  while (g_raw_mic_run)
    {
      ssize_t r = read(fd, buf, sizeof(buf));
      int i;
      int peak = 0;

      if (r != (ssize_t)sizeof(buf))
        {
          usleep(10 * 1000);
          continue;
        }

      /* 峰值电平 + 32 段柱状（每段 32 个样本取最大） */

      for (i = 0; i < RAW_MIC_N; i++)
        {
          int v = buf[i] < 0 ? -buf[i] : buf[i];

          if (v > peak)
            {
              peak = v;
            }

          if ((i % (RAW_MIC_N / RAW_MIC_BARS)) == 0 &&
              (i / (RAW_MIC_N / RAW_MIC_BARS)) < RAW_MIC_BARS)
            {
              g_raw_mic_peak[i / (RAW_MIC_N / RAW_MIC_BARS)] = (int16_t)v;
            }
        }

      for (i = 0; i < RAW_MIC_BARS; i++)
        {
          int seg = 0;
          int j;

          for (j = 0; j < RAW_MIC_N / RAW_MIC_BARS; j++)
            {
              int v = buf[i * (RAW_MIC_N / RAW_MIC_BARS) + j];

              v = v < 0 ? -v : v;
              if (v > seg)
                {
                  seg = v;
                }
            }

          g_raw_mic_peak[i] = (int16_t)seg;
        }

      /* dBFS = 20*log10(peak/满量程)，静音时给 -99 下限 */

      g_raw_mic_dbm = (peak > 0)
                          ? (int)(20.0f * log10f((float)peak / 32768.0f))
                          : -99;

      if (g_raw_mic_dbm < -99)
        {
          g_raw_mic_dbm = -99;
        }
    }

  close(fd);
  return NULL;
}

static void raw_mic_start(void)
{
  if (g_raw_mic_run)
    {
      return;
    }

  g_raw_mic_run = 1;
  g_raw_mic_dbm = -99;
  memset(g_raw_mic_peak, 0, sizeof(g_raw_mic_peak));

  if (pthread_create(&g_raw_mic_tid, NULL, raw_mic_thread, NULL) != 0)
    {
      g_raw_mic_run = 0;
    }
}

static void raw_mic_stop(void)
{
  g_raw_mic_run = 0;
}

/****************************************************************************
 * 扬声器页按钮
 ****************************************************************************/

static void raw_spk_play_cb(lv_event_t *e)
{
  (void)e;

  pw_tone_play(440, INT32_MIN);
}

static void raw_spk_stop_cb(lv_event_t *e)
{
  (void)e;

  pw_tone_stop();
}

/****************************************************************************
 * Name: raw_build_mic_page
 *
 * Description:
 *   麦克风页：大号 dBFS 电平 + 32 段电平柱 + 提示。
 *
 ****************************************************************************/

static void raw_build_mic_page(int pi, int x)
{
  lv_obj_t *pg = lv_obj_create(g_raw.scroller);
  lv_obj_t *sub;
  lv_obj_t *uni;
  int i;

  lv_obj_set_size(pg, RAW_PAGE_W, RAW_PAGE_H);
  lv_obj_set_pos(pg, x, 0);
  lv_obj_set_style_bg_opa(pg, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(pg, 0, 0);
  lv_obj_set_style_pad_all(pg, 0, 0);
  lv_obj_remove_flag(pg, LV_OBJ_FLAG_SCROLLABLE);

  sub = pw_label_new(pg, PW_STR(RAW_MIC), PW_FNT_LARGE, PW_ACC_ACOU);
  lv_obj_set_pos(sub, 20, 4);

  uni = pw_label_new(pg, "dBFS", PW_FNT_SMALL, PW_COL_DIM);
  lv_obj_align(uni, LV_ALIGN_TOP_RIGHT, -20, 8);

  g_raw.pg[pi].val[0] = pw_label_new(pg, "--", PW_FNT_XXL, PW_COL_TEXT);
  lv_obj_set_pos(g_raw.pg[pi].val[0], 72, 70);

  /* 32 段电平柱：用一排小方块表示 */

  for (i = 0; i < RAW_MIC_BARS; i++)
    {
      lv_obj_t *bar = lv_obj_create(pg);

      lv_obj_set_size(bar, 8, 8);
      lv_obj_set_pos(bar, 18 + i * 11, 170);
      lv_obj_set_style_bg_color(bar, PW_COL_CARD_LT, 0);
      lv_obj_set_style_radius(bar, 2, 0);
      lv_obj_set_style_border_width(bar, 0, 0);
      lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
      g_raw_mic_bar[i] = bar;
    }

  g_raw.pg[pi].sub[1] = pw_label_new(pg, PW_STR(RAW_MIC_HINT),
                                     PW_FNT_BODY, PW_COL_FAINT);
  lv_obj_set_pos(g_raw.pg[pi].sub[1], 18, 300);
}

/****************************************************************************
 * Name: raw_build_spk_page
 *
 * Description:
 *   扬声器页：状态文本 + 播放/停止按钮。
 *
 ****************************************************************************/

static void raw_build_spk_page(int pi, int x)
{
  lv_obj_t *pg = lv_obj_create(g_raw.scroller);
  lv_obj_t *sub;
  lv_obj_t *btn;
  lv_obj_t *lab;

  lv_obj_set_size(pg, RAW_PAGE_W, RAW_PAGE_H);
  lv_obj_set_pos(pg, x, 0);
  lv_obj_set_style_bg_opa(pg, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(pg, 0, 0);
  lv_obj_set_style_pad_all(pg, 0, 0);
  lv_obj_remove_flag(pg, LV_OBJ_FLAG_SCROLLABLE);

  sub = pw_label_new(pg, PW_STR(RAW_SPK), PW_FNT_LARGE, PW_ACC_ACOU);
  lv_obj_set_pos(sub, 20, 4);

  g_raw.pg[pi].val[0] = pw_label_new(pg, "--", PW_FNT_MED, PW_COL_TEXT);
  lv_obj_set_pos(g_raw.pg[pi].val[0], 24, 60);

  btn = pw_card_new(pg, 160, 64, PW_COL_CARD);
  lv_obj_set_pos(btn, 24, 140);
  lv_obj_add_event_cb(btn, raw_spk_play_cb, LV_EVENT_CLICKED, NULL);
  lab = pw_label_new(btn, PW_STR(SPK_PLAY), PW_FNT_MED, PW_ACC_ACOU);
  lv_obj_center(lab);

  btn = pw_card_new(pg, 160, 64, PW_COL_CARD);
  lv_obj_set_pos(btn, 200, 140);
  lv_obj_add_event_cb(btn, raw_spk_stop_cb, LV_EVENT_CLICKED, NULL);
  lab = pw_label_new(btn, PW_STR(SPK_STOP), PW_FNT_MED, PW_COL_TEXT);
  lv_obj_center(lab);

  g_raw.pg[pi].sub[0] = pw_label_new(pg, PW_STR(RAW_SPK_HINT),
                                     PW_FNT_BODY, PW_COL_FAINT);
  lv_obj_set_pos(g_raw.pg[pi].sub[0], 24, 300);
}

/****************************************************************************
 * Name: raw_build_light_page
 ****************************************************************************/

static void raw_build_light_page(int pi, int x)
{
  lv_obj_t *pg = lv_obj_create(g_raw.scroller);
  lv_obj_t *sub;
  lv_obj_t *uni;
  lv_obj_t *hint;

  lv_obj_set_size(pg, RAW_PAGE_W, RAW_PAGE_H);
  lv_obj_set_pos(pg, x, 0);
  lv_obj_set_style_bg_opa(pg, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(pg, 0, 0);
  lv_obj_set_style_pad_all(pg, 0, 0);
  lv_obj_remove_flag(pg, LV_OBJ_FLAG_SCROLLABLE);

  sub = pw_label_new(pg, PW_STR(RAW_LIGHT), PW_FNT_LARGE, PW_ACC_LIGHT);
  lv_obj_set_pos(sub, 20, 4);

  uni = pw_label_new(pg, PW_STR(RAW_LUX), PW_FNT_SMALL, PW_COL_DIM);
  lv_obj_align(uni, LV_ALIGN_TOP_RIGHT, -20, 8);

  /* 主数值 48 */

  g_raw.pg[pi].val[0] = pw_label_new(pg, "--", PW_FNT_XXL, PW_COL_TEXT);
  lv_obj_set_pos(g_raw.pg[pi].val[0], 72, 84);

  /* CH0/CH1 次级行 */

  g_raw.pg[pi].sub[0] = pw_label_new(pg, PW_STR(RAW_CH0_HINT),
                                      PW_FNT_MED, PW_COL_DIM);
  lv_obj_set_pos(g_raw.pg[pi].sub[0], 24, 216);

  g_raw.pg[pi].sub[1] = pw_label_new(pg, PW_STR(RAW_CH1_HINT),
                                      PW_FNT_MED, PW_COL_DIM);
  lv_obj_set_pos(g_raw.pg[pi].sub[1], 24, 256);

  hint = pw_label_new(pg, PW_STR(RAW_LIGHT_HINT),
                      PW_FNT_BODY, PW_COL_FAINT);
  lv_obj_set_pos(hint, 24, 310);
}

/****************************************************************************
 * Name: raw_show_vals
 *
 * Description:
 *   按当前视图把三轴数值写进标签：
 *     数值视图 → 带单位（"%+.2f g" 这样，和改动前的原版一致）
 *     图线视图 → 不带单位（列宽 126px 放不下 "+1234.5 dps"）
 *
 ****************************************************************************/

static void raw_show_vals(int pi, FAR const float *v)
{
  int i;

  for (i = 0; i < 3; i++)
    {
      if (pi == 0)
        {
          lv_label_set_text_fmt(g_raw.pg[pi].val[i],
                                (g_raw_view == RAW_VIEW_CURVE)
                                    ? FMT_G : "%+.2f", v[i]);
        }
      else if (pi == 1)
        {
          /* ≥1000 dps 用整数（列宽放不下 7 字符） */

          bool big = (v[i] <= -1000.0f || v[i] >= 1000.0f);

          if (g_raw_view == RAW_VIEW_CURVE)
            {
              lv_label_set_text_fmt(g_raw.pg[pi].val[i],
                                    big ? "%+.0f" : "%+.1f", v[i]);
            }
          else
            {
              lv_label_set_text_fmt(g_raw.pg[pi].val[i],
                                    big ? "%+.0f" : "%+.1f", v[i]);
            }
        }
      else
        {
          lv_label_set_text_fmt(g_raw.pg[pi].val[i],
                                (g_raw_view == RAW_VIEW_CURVE)
                                    ? FMT_MG : "%+d", (int)v[i]);
        }
    }
}

/****************************************************************************
 * Name: raw_tick_cb
 *
 * Description:
 *   每 100ms 只刷新当前页对应的传感器（省 I2C 流量）：三轴页同时更新
 *   数值标签与迷你曲线的历史样本。
 *
 ****************************************************************************/

static void raw_tick_cb(lv_timer_t *timer)
{
  struct pw_imu_s imu;
  struct pw_mag_s mag;
  struct pw_light_s light;
  int pi = g_raw.idx;
  int i;

  /* 仅活动屏刷新 */

  if (lv_screen_active() != g_raw.scr)
    {
      return;
    }

  switch (pi)
    {
      case 0:   /* Accelerometer */
        if (pw_sensors_read_imu(&imu) == 0)
          {
            float v[3];

            v[0] = imu.ax / 1000.0f;
            v[1] = imu.ay / 1000.0f;
            v[2] = imu.az / 1000.0f;

            raw_show_vals(pi, v);
            raw_axis_curve_update(pi, v);
          }
        break;

      case 1:   /* Gyroscope */
        if (pw_sensors_read_imu(&imu) == 0)
          {
            float v[3];

            v[0] = imu.gx / 1000.0f;
            v[1] = imu.gy / 1000.0f;
            v[2] = imu.gz / 1000.0f;

            raw_show_vals(pi, v);
            raw_axis_curve_update(pi, v);
          }
        break;

      case 2:   /* Magnetometer */
        if (pw_sensors_read_mag(&mag) == 0)
          {
            float v[3];

            v[0] = (float)mag.x;
            v[1] = (float)mag.y;
            v[2] = (float)mag.z;

            raw_show_vals(pi, v);
            raw_axis_curve_update(pi, v);
          }
        break;

      case 4:   /* Microphone */
        {
          lv_label_set_text_fmt(g_raw.pg[pi].val[0], "%d dBFS",
                                (int)g_raw_mic_dbm);

          for (i = 0; i < RAW_MIC_BARS; i++)
            {
              /* 每段 0..32767 → 4 级颜色（越亮越接近满刻度） */

              int v = g_raw_mic_peak[i];
              int db = (v > 0)
                           ? (int)(20.0f * log10f((float)v / 32768.0f))
                           : -99;
              lv_color_t c = PW_COL_CARD_LT;

              if (db > -20)
                {
                  c = PW_ACC_ACOU;
                }
              else if (db > -40)
                {
                  c = PW_COL_TEXT;
                }
              else if (db > -60)
                {
                  c = PW_COL_DIM;
                }

              lv_obj_set_style_bg_color(g_raw_mic_bar[i], c, 0);
            }
        }
        break;

      case 5:   /* Speaker */
        lv_label_set_text_fmt(g_raw.pg[pi].val[0], "%s%s",
                              pw_tone_available() ? "" : PW_STR(SPK_NO_HW),
                              pw_tone_playing()
                                  ? (pw_tone_available() ? PW_STR(SPK_PLAYING)
                                                         : "")
                                  : (pw_tone_available() ? PW_STR(SPK_IDLE)
                                                         : ""));
        break;

      case 3:   /* Light */
        if (pw_sensors_read_light(&light) == 0)
          {
            lv_label_set_text_fmt(g_raw.pg[pi].val[0], "%d lux",
                                  light.lux);
            lv_label_set_text_fmt(g_raw.pg[pi].sub[0], PW_STR(RAW_CH0_VISIR_FMT),
                                  light.ch0);
            lv_label_set_text_fmt(g_raw.pg[pi].sub[1], PW_STR(RAW_CH1_IR_FMT),
                                  light.ch1);
          }
        break;
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: pw_raw_goto
 *
 * Description:
 *   直接跳到原始传感器板块的第 idx 页（0=加速度 1=陀螺 2=磁力 3=光
 *   4=麦克风 5=扬声器）。供命令行截图与 AI Agent 使用。
 ****************************************************************************/

void pw_raw_goto(int idx)
{
  if (g_raw.scroller == NULL)
    {
      return;
    }

  raw_scroll_to(idx, false);
}

void pw_raw_set_view(int view)
{
  if (g_raw.scroller == NULL)
    {
      return;
    }

  g_raw_view = (view == RAW_VIEW_CURVE) ? RAW_VIEW_CURVE : RAW_VIEW_NUM;
  raw_apply_view();
}

lv_obj_t *pw_raw_screen(void)
{
  lv_obj_t *cont;
  lv_obj_t *btn;
  lv_obj_t *lab;
  int i;

  memset(&g_raw, 0, sizeof(g_raw));

  /* 每次进入「原始传感器」都从数值视图开始（用户要求"默认大数字风格"） */

  g_raw_view = RAW_VIEW_NUM;

  g_raw.scr = pw_scr_new();

  /* 顶栏 + 内容容器（横滑区） */

  cont = pw_topbar(g_raw.scr, PW_STR(UI_RAW_SENSORS));
  lv_obj_set_size(cont, PW_SCREEN_W, RAW_CONTENT_H);
  lv_obj_set_pos(cont, RAW_CONTENT_X, RAW_CONTENT_Y);
  g_raw.scroller = cont;

  lv_obj_set_scroll_dir(cont, LV_DIR_HOR);
  lv_obj_set_scroll_snap_x(cont, LV_SCROLL_SNAP_CENTER);
  lv_obj_set_scrollbar_mode(cont, LV_SCROLLBAR_MODE_OFF);
  lv_obj_add_flag(cont, LV_OBJ_FLAG_SCROLL_ONE);
  lv_obj_add_event_cb(cont, raw_scroll_end_cb, LV_EVENT_SCROLL_END, NULL);

  /* 三轴页：加速度 / 陀螺 / 地磁（k=存储系数，fs_lo..fs_hi=自动量程范围） */

  /* 量程下限刻意压低：三轴各自量程后，小信号也能占满泳道（下限太高会把
   * 小波动压回一条直线）。上限受 int16 存储量程约束。 */

  raw_build_axis_page(0, 0,          PW_STR(RAW_ACCEL), "g",
                      PW_ACC_ACC,  4000.0f, 0.2f, 8.0f);
  raw_build_axis_page(1, RAW_PAGE_W, PW_STR(RAW_GYRO), "dps",
                      PW_ACC_GYRO, 16.0f, 5.0f, 2000.0f);
  raw_build_axis_page(2, RAW_PAGE_W * 2, PW_STR(RAW_MAG), "mG",
                      PW_ACC_MAG,  1.0f, 20.0f, 30000.0f);
  raw_build_light_page(3, RAW_PAGE_W * 3);
  raw_build_mic_page(4, RAW_PAGE_W * 4);
  raw_build_spk_page(5, RAW_PAGE_W * 5);

  /* 底部：左箭头 + 指示点 + 右箭头 */

  btn = pw_card_new(g_raw.scr, 64, 44, PW_COL_CARD);
  lv_obj_set_pos(btn, 4, RAW_CONTENT_Y + RAW_CONTENT_H + 4);
  lv_obj_add_event_cb(btn, raw_arrow_cb, LV_EVENT_CLICKED,
                      (void *)&g_delta_prev);
  lab = pw_label_new(btn, "<", PW_FNT_XL, PW_COL_DIM);
  lv_obj_center(lab);

  for (i = 0; i < RAW_PAGES; i++)
    {
      lv_obj_t *d = lv_obj_create(g_raw.scr);

      lv_obj_set_size(d, 12, 12);
      lv_obj_set_style_bg_color(d, PW_COL_CARD_LT, 0);
      lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
      lv_obj_set_style_border_width(d, 0, 0);
      lv_obj_remove_flag(d, LV_OBJ_FLAG_SCROLLABLE);
      g_raw.dots[i] = d;
      lv_obj_align(d, LV_ALIGN_BOTTOM_MID, (i - (RAW_PAGES - 1) / 2) * 18,
                   -34);
    }

  btn = pw_card_new(g_raw.scr, 64, 44, PW_COL_CARD);
  lv_obj_set_pos(btn, PW_SCREEN_W - 4 - 64,
                 RAW_CONTENT_Y + RAW_CONTENT_H + 4);
  lv_obj_add_event_cb(btn, raw_arrow_cb, LV_EVENT_CLICKED,
                      (void *)&g_delta_next);
  lab = pw_label_new(btn, ">", PW_FNT_XL, PW_COL_DIM);
  lv_obj_center(lab);

  /* 右下角视图切换按钮（自绘图标）+ 按默认视图重排三轴页 */

  raw_toggle_build(g_raw.scr);
  raw_apply_view();

  /* 屏幕销毁时确保采集线程退出 */

  lv_obj_add_event_cb(g_raw.scr, raw_del_cb, LV_EVENT_DELETE, NULL);

  /* 初始状态 + 周期刷新 */

  raw_set_page(0);
  pw_scr_set_tick(g_raw.scr, raw_tick_cb, 100);

  return g_raw.scr;
}
