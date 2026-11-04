/****************************************************************************
 * apps/examples/phywear/pw_bounce.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 弹跳测距（T-0016）。
 *
 * 物理：小球离开桌面再落回，空中时间 T 与弹起高度 h 的关系
 *
 *     T = 2 * sqrt(2h / g)        =>        h = g * T^2 / 8
 *
 * 所以只要量准"两次落地冲击的时间间隔"，高度就出来了 —— 不需要知道球的质量、
 * 不需要标定加速度绝对精度，这也是 phyphox 的 bounce 用的同一套思路。
 *
 * 为什么单独起线程：GUI 主循环按 LVGL 的下一个定时器休眠（空闲时能睡 250 ms），
 * 拿它当采样源会漏掉整个飞行段。这里自己开线程死等 ioctl，实测 200~500 Hz。
 *
 * 采样来源：/dev/lsm6dsl0 的 SNIOC_LSM6DSLSENSORREAD（单位 mg，±8 g 量程）。
 *
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <nuttx/sensors/lsm6dsl.h>

#include <lvgl/lvgl.h>

#include "phywear_ui.h"
#include "pw_bounce.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define PW_BOUNCE_G         9.80665f

/* 冲击判据（mg）：落地瞬间加速度远大于 1 g；阈值 2.5 g 是"硬币/钢球从
 * 10 cm 落下"在桌面上的典型峰值，再低会把走路的晃动算进来。 */

#define PW_BOUNCE_A_HIT_MG  2500
#define PW_BOUNCE_A_RLS_MG  1200

/* 两次冲击的最小间隔：低于 120 ms 的连续尖峰是同一次碰撞的抖动 */

#define PW_BOUNCE_MIN_GAP_MS 120

/* 采样间隔：LSM6DSL 在 400 kHz I2C 上一次读约 1~2 ms，2 ms 让出 CPU */

#define PW_BOUNCE_SAMPLE_US  2000

#define PW_BOUNCE_NEED      3        /* 连测 3 次（3 个间隔 = 4 个冲击） */

/****************************************************************************
 * Private Data
 ****************************************************************************/

static pthread_t g_thread;
static volatile bool g_run;          /* 线程继续跑 */
static volatile bool g_running;      /* 对外：正在测量 */
static volatile int  g_fd = -1;

/* 结果区（线程写、UI/NSH 读） */

static volatile float g_h_cm;
static volatile int   g_t_ms;
static volatile float g_a_max_g;
static volatile int   g_n_impacts;
static volatile bool  g_valid;

/* 线程内部状态 */

static int64_t g_imp_ms[PW_BOUNCE_MAX_IMPACT];
static int64_t g_arm_ms;            /* 起测后 500 ms 内的冲击不算（点屏那一下） */
static int     g_n_imp;
static int64_t g_last_ms;
static bool    g_in_hit;
static float   g_a_max;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int64_t pw_bounce_now_ms(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* 三个间隔 → 三个高度 → 中位数。用中位数而不是平均：球没弹直、
 * 桌面接缝、手碰桌子都会让某一次偏大，中位数对这种离群值不敏感。 */

static void pw_bounce_solve(void)
{
  float h[PW_BOUNCE_NEED];
  float tmp;
  int i;
  int j;

  if (g_n_imp < PW_BOUNCE_NEED + 1)
    {
      return;
    }

  for (i = 0; i < PW_BOUNCE_NEED; i++)
    {
      int64_t dt = g_imp_ms[i + 1] - g_imp_ms[i];
      float t = dt / 1000.0f;

      h[i] = PW_BOUNCE_G * t * t / 8.0f * 100.0f;   /* m -> cm */
    }

  /* 插入排序，3 个元素 */

  for (i = 1; i < PW_BOUNCE_NEED; i++)
    {
      tmp = h[i];

      for (j = i - 1; j >= 0 && h[j] > tmp; j--)
        {
          h[j + 1] = h[j];
        }

      h[j + 1] = tmp;
    }

  g_h_cm = h[PW_BOUNCE_NEED / 2];
  g_t_ms = (int)(g_imp_ms[PW_BOUNCE_NEED / 2 + 1] -
                 g_imp_ms[PW_BOUNCE_NEED / 2]);
  g_a_max_g = g_a_max;
  g_valid = true;
  g_n_impacts = g_n_imp;

  printf("[bounce] RESULT h=%d.%d cm  T=%d ms  a_max=%d.%02d g  n=%d\n",
         (int)g_h_cm, (int)(g_h_cm * 10.0f + 0.5f) % 10, g_t_ms,
         (int)g_a_max_g, (int)(g_a_max_g * 100.0f + 0.5f) % 100, g_n_impacts);
  fflush(stdout);
}

static void *pw_bounce_thread(void *arg)
{
  struct lsm6dsl_sensor_data_s d;
  int64_t t0;
  int idle_ms;

  (void)arg;

  g_fd = open("/dev/lsm6dsl0", O_RDONLY);
  if (g_fd < 0)
    {
      printf("[bounce] ERROR open /dev/lsm6dsl0: %d\n", errno);
      fflush(stdout);
      g_running = false;
      return NULL;
    }

  ioctl(g_fd, SNIOC_START, 0);

  t0 = pw_bounce_now_ms();
  idle_ms = 0;

  printf("[bounce] 采样开始（判据 %d.%d g / 释放 %d.%d g / 最短间隔 %d ms）\n",
         PW_BOUNCE_A_HIT_MG / 1000, (PW_BOUNCE_A_HIT_MG % 1000) / 100,
         PW_BOUNCE_A_RLS_MG / 1000, (PW_BOUNCE_A_RLS_MG % 1000) / 100,
         PW_BOUNCE_MIN_GAP_MS);
  fflush(stdout);

  while (g_run)
    {
      int64_t now;
      float mag;
      float g;

      if (ioctl(g_fd, SNIOC_LSM6DSLSENSORREAD, (unsigned long)&d) != 0)
        {
          usleep(PW_BOUNCE_SAMPLE_US * 5);
          continue;
        }

      mag = sqrtf((float)d.x_data * d.x_data +
                  (float)d.y_data * d.y_data +
                  (float)d.z_data * d.z_data);
      g = mag / 1000.0f;

      if (g > g_a_max)
        {
          g_a_max = g;
        }

      now = pw_bounce_now_ms();

      if (now < g_arm_ms)
        {
          /* 刚点屏/刚下发命令，手指和线的抖动先放过去 */

          usleep(PW_BOUNCE_SAMPLE_US);
          continue;
        }

      if (g_in_hit)
        {
          if (mag < PW_BOUNCE_A_RLS_MG && now - g_last_ms > 30)
            {
              g_in_hit = false;
            }
        }
      else if (mag > PW_BOUNCE_A_HIT_MG &&
               now - g_last_ms > PW_BOUNCE_MIN_GAP_MS)
        {
          g_in_hit = true;
          g_last_ms = now;

          if (g_n_imp < PW_BOUNCE_MAX_IMPACT)
            {
              g_imp_ms[g_n_imp++] = now;
              printf("[bounce] impact %d @ %d ms (a=%d.%02d g)\n",
                     g_n_imp, (int)(now - t0), (int)g,
                     (int)(g * 100.0f + 0.5f) % 100);
              fflush(stdout);
            }

          if (g_n_imp >= PW_BOUNCE_NEED + 1)
            {
              pw_bounce_solve();
              break;                    /* 测够了就停，别继续占 IMU */
            }
        }

      /* 60 s 没有任何冲击就自己退出，避免线程永远挂着占 IMU */

      if (g_n_imp == 0 && ++idle_ms > (60000 / (PW_BOUNCE_SAMPLE_US / 1000)))
        {
          printf("[bounce] 60 s 未检测到冲击，停止\n");
          fflush(stdout);
          break;
        }

      usleep(PW_BOUNCE_SAMPLE_US);
    }

  close(g_fd);
  g_fd = -1;
  g_running = false;
  g_a_max_g = g_a_max;
  g_n_impacts = g_n_imp;
  return NULL;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void pw_bounce_start(void)
{
  if (g_running)
    {
      /* 重复开始 = 重新测一次：只清计数，线程继续跑 */

      g_n_imp = 0;
      g_in_hit = false;
      g_valid = false;
      g_a_max = 0.0f;
      g_arm_ms = pw_bounce_now_ms() + 500;
      printf("[bounce] 重新开始\n");
      fflush(stdout);
      return;
    }

  g_n_imp = 0;
  g_last_ms = 0;
  g_in_hit = false;
  g_a_max = 0.0f;
  g_h_cm = 0.0f;
  g_t_ms = 0;
  g_a_max_g = 0.0f;
  g_n_impacts = 0;
  g_valid = false;
  g_arm_ms = pw_bounce_now_ms() + 500;
  g_run = true;
  g_running = true;

  if (pthread_create(&g_thread, NULL, pw_bounce_thread, NULL) != 0)
    {
      printf("[bounce] ERROR pthread_create: %d\n", errno);
      fflush(stdout);
      g_running = false;
      g_run = false;
    }
}

void pw_bounce_stop(void)
{
  if (!g_running && !g_run)
    {
      return;
    }

  g_run = false;
  pthread_join(g_thread, NULL);
  g_running = false;
}

int pw_bounce_active(void)
{
  return g_running ? 1 : 0;
}

void pw_bounce_get(struct pw_bounce_state_s *out)
{
  if (out == NULL)
    {
      return;
    }

  out->h_cm = g_h_cm;
  out->t_ms = g_t_ms;
  out->a_max_g = g_a_max_g;
  out->n_impacts = g_n_impacts;
  out->valid = g_valid;
  out->running = g_running;
}

void pw_bounce_report(void)
{
  printf("[bounce] status running=%d n=%d h=%d.%d cm T=%d ms a_max=%d.%02d g %s\n",
         g_running, g_n_impacts, (int)g_h_cm,
         (int)(g_h_cm * 10.0f + 0.5f) % 10, g_t_ms, (int)g_a_max_g,
         (int)(g_a_max_g * 100.0f + 0.5f) % 100,
         g_valid ? "valid" : "not-ready");
  fflush(stdout);
}

/****************************************************************************
 * 测量页（LVGL）
 *
 * 大字 h（cm）、两行小字 T / a_max、底部操作提示。点屏任意位置 = 重新测一次。
 * 页面退出时一定要 stop()：采样线程占着 IMU，留着会让"原始数据"页读不到。
 ****************************************************************************/

static void pw_bounce_ui_refresh(lv_timer_t *t)
{
  lv_obj_t **labs = lv_timer_get_user_data(t);
  struct pw_bounce_state_s st;
  char buf[48];
  int tenths;
  int hundredths;

  pw_bounce_get(&st);

  /* 这里**不用 %f**：本工程的 printf 对 float 支持不全（实测屏上直接显示
   * "inf"，用户第一眼就看到了）。改成整数定点：tenths = h*10，再手拼小数。 */

  if (st.valid && isfinite(st.h_cm) && st.h_cm >= 0.0f && st.h_cm < 300.0f)
    {
      tenths = (int)(st.h_cm * 10.0f + 0.5f);
      snprintf(buf, sizeof(buf), "%d.%d", tenths / 10, tenths % 10);
    }
  else
    {
      snprintf(buf, sizeof(buf), "--");
    }

  lv_label_set_text(labs[0], buf);

  snprintf(buf, sizeof(buf), "T %d ms", st.t_ms);
  lv_label_set_text(labs[1], buf);

  if (isfinite(st.a_max_g))
    {
      hundredths = (int)(st.a_max_g * 100.0f + 0.5f);
      snprintf(buf, sizeof(buf), "a_max %d.%02d g", hundredths / 100,
               hundredths % 100);
    }
  else
    {
      snprintf(buf, sizeof(buf), "a_max -- g");
    }

  lv_label_set_text(labs[2], buf);

  snprintf(buf, sizeof(buf), "%d / %d", st.n_impacts, PW_BOUNCE_MAX_IMPACT);
  lv_label_set_text(labs[3], buf);
}

static void pw_bounce_ui_del_cb(lv_event_t *e)
{
  lv_timer_t *t = lv_event_get_user_data(e);

  if (t != NULL)
    {
      lv_timer_delete(t);
    }

  pw_bounce_stop();
}

static void pw_bounce_ui_click_cb(lv_event_t *e)
{
  (void)e;
  pw_bounce_start();
}

lv_obj_t *pw_bounce_screen(void)
{
  lv_obj_t *scr;
  lv_obj_t *cont;
  lv_obj_t *lab;
  lv_obj_t *unit;
  static lv_obj_t *labs[4];

  scr = pw_scr_new();
  cont = pw_topbar(scr, "弹跳测距");

  /* 大读数：h（cm，一位小数），居中偏上 */

  labs[0] = pw_label_new(cont, "--", PW_FNT_XXL, PW_COL_TEXT);
  lv_obj_align(labs[0], LV_ALIGN_TOP_MID, -18, 34);

  unit = pw_label_new(cont, "cm", PW_FNT_BODY, PW_COL_DIM);
  lv_obj_align(unit, LV_ALIGN_TOP_MID, 52, 68);

  labs[1] = pw_label_new(cont, "T 0 ms", PW_FNT_MED, PW_COL_DIM);
  lv_obj_align(labs[1], LV_ALIGN_TOP_MID, 0, 104);

  labs[2] = pw_label_new(cont, "a_max 0.00 g", PW_FNT_BODY, PW_COL_DIM);
  lv_obj_align(labs[2], LV_ALIGN_TOP_MID, 0, 132);

  /* 文案用字都查过 pw_font_14 的 512 字子集：球/抛/击 都不在，别写回去 */

  lab = pw_label_new(cont, "落地次数", PW_FNT_BODY, PW_COL_FAINT);
  lv_obj_align(lab, LV_ALIGN_TOP_MID, -46, 164);

  labs[3] = pw_label_new(cont, "0 / 8", PW_FNT_BODY, PW_COL_DIM);
  lv_obj_align(labs[3], LV_ALIGN_TOP_MID, 46, 164);

  lab = pw_label_new(cont, "点一下开始，轻弹落地 3 次出结果", PW_FNT_BODY,
                     PW_COL_FAINT);
  lv_obj_align(lab, LV_ALIGN_BOTTOM_MID, 0, -46);

  lab = pw_label_new(cont, "把表放桌面上，在表边轻弹落地", PW_FNT_BODY,
                     PW_COL_FAINT);
  lv_obj_align(lab, LV_ALIGN_BOTTOM_MID, 0, -22);

  /* 点屏重测 + 退出时停线程 */

  lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(scr, pw_bounce_ui_click_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_add_event_cb(scr, pw_bounce_ui_del_cb, LV_EVENT_DELETE, NULL);

  {
    lv_timer_t *t = lv_timer_create(pw_bounce_ui_refresh, 100, labs);
    lv_obj_add_event_cb(scr, pw_bounce_ui_del_cb, LV_EVENT_DELETE, t);
  }

  /* 进页面不自动起采样：500 Hz 读 I2C 会把 GUI 拖慢（截图取证直接丢半帧），
   * 而且用户还没准备好。点一下屏再开始。 */

  return scr;
}
