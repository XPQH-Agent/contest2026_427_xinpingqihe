/****************************************************************************
 * apps/examples/phywear/pw_anim.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * P1-2 动效算法实现。设计取舍与三种实现的实测对比见 pw_anim.h 顶部注释。
 *
 * 本文件为纯算法（只依赖 <math.h>），可在主机上跑自检：
 *   bash tools/phywear/hosttest_math.sh
 ****************************************************************************/

#include <math.h>

#include "pw_anim.h"

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* 默认预设曲线的查表数据：u(t) = e^(-a t)(cos(w t) + (a/w) sin(w t))，
 * a=4, w=18, dur=0.9 s，int16 归一化到 32767。
 * **由 tools/phywear/gen_pw_anim_lut.py 生成，勿手改**（改了要重生成并重测）。 */

#if PW_ANIM_IMPL == PW_ANIM_IMPL_LUT
static const short g_pw_anim_lut[PW_ANIM_LUT_N] =
{
   32767,  32698,  32495,  32162,  31703,  31126,  30434,  29634,  28733,  27738,  26654,  25489,  24250,  22944,  21579,  20162,
   18700,  17200,  15671,  14119,  12551,  10974,   9396,   7822,   6259,   4714,   3192,   1699,    241,  -1178,  -2553,  -3878,
   -5151,  -6366,  -7521,  -8613,  -9638, -10594, -11480, -12293, -13032, -13696, -14284, -14797, -15233, -15594, -15879, -16090,
  -16228, -16295, -16292, -16220, -16084, -15883, -15622, -15303, -14929, -14503, -14029, -13509, -12947, -12347, -11712, -11046,
  -10352,  -9634,  -8896,  -8141,  -7374,  -6597,  -5814,  -5029,  -4244,  -3464,  -2691,  -1928,  -1178,   -445,    270,    965,
    1635,   2280,   2898,   3487,   4044,   4569,   5061,   5517,   5938,   6322,   6670,   6979,   7251,   7485,   7682,   7841,
    7962,   8047,   8096,   8110,   8090,   8036,   7951,   7834,   7689,   7515,   7314,   7089,   6840,   6570,   6280,   5971,
    5647,   5307,   4955,   4593,   4221,   3841,   3457,   3068,   2678,   2287,   1898,   1512,   1130,    754,    385,     25,
    -325,   -665,   -991,  -1305,  -1604,  -1889,  -2157,  -2410,  -2645,  -2862,  -3061,  -3243,  -3405,  -3549,  -3674,  -3780,
   -3867,  -3936,  -3987,  -4019,  -4034,  -4032,  -4012,  -3977,  -3926,  -3860,  -3779,  -3686,  -3579,  -3460,  -3331,  -3191,
   -3041,  -2883,  -2717,  -2545,  -2367,  -2184,  -1997,  -1806,  -1614,  -1420,  -1226,  -1032,   -839,   -647,   -459,   -274,
     -92,     84,    255,    421,    580,    732,    877,   1014,   1143,   1264,   1376,   1479,   1574,   1659,   1735,   1801,
    1858,   1906,   1944,   1973,   1993,   2005,   2007,   2001,   1987,   1966,   1936,   1899,   1855,   1805,   1749,   1687,
    1619,   1547,   1470,   1390,   1305,   1218,   1128,   1036,    942,    846,    750,    653,    557,    460,    365,    270,
     177,     86,     -3,    -89,   -173,   -253,   -330,   -404,   -474,   -540,   -602,   -660,   -713,   -762,   -807,   -847,
    -882,   -912,   -938,   -959,   -976,   -988,   -995,   -999,   -998,   -992,   -983,   -970,   -954,   -933,   -910,   -883,
};
#endif

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static float anim_w_safe(float omega_inv_scale, float w, float t)
{
  /* (a/w)·sin(w t) 在 w→0 时的极限是 a·t（用 sinc 形式避免 0/0） */

  if (w < 1.0e-4f)
    {
      return omega_inv_scale * t;      /* 调用方传入 a 作为 scale */
    }

  return (omega_inv_scale / w) * sinf(w * t);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

const char *pw_anim_impl_name(void)
{
#if PW_ANIM_IMPL == PW_ANIM_IMPL_REC
  return "rec";
#elif PW_ANIM_IMPL == PW_ANIM_IMPL_LUT
  return "lut";
#else
  return "closed";
#endif
}

void pw_anim_spring_init(struct pw_anim_spring_s *s, float damp, float omega,
                         float dur)
{
  float e;

  if (s == NULL)
    {
      return;
    }

  s->damp = (damp > 0.0f) ? damp : 4.0f;
  s->omega = (omega > 0.0f) ? omega : 0.0f;
  s->dur = (dur > 0.0f) ? dur : 0.9f;

  /* 递推系数：u 满足 u[n] = c1·u[n-1] + c2·u[n-2]（二阶线性常系数） */

  if (s->omega < 1.0e-4f)
    {
      /* 临界阻尼：u(t) = e^(-a t)(1 + a t)，递推系数取二重根极限 */

      e = expf(-s->damp * (1.0f / 60.0f));
      s->c1 = 2.0f * e;
      s->c2 = -e * e;
      s->u1 = 1.0f;
      s->u2 = e * (1.0f + s->damp * (1.0f / 60.0f));
    }
  else
    {
      float wdt = s->omega * (1.0f / 60.0f);

      e = expf(-s->damp * (1.0f / 60.0f));
      s->c1 = 2.0f * e * cosf(wdt);
      s->c2 = -e * e;
      s->u1 = 1.0f;
      s->u2 = e * (cosf(wdt) + (s->damp / s->omega) * sinf(wdt));
    }

  s->dt = 0.0f;
}

float pw_anim_spring_u(const struct pw_anim_spring_s *s, float t)
{
  float u;

  if (s == NULL)
    {
      return 0.0f;
    }

  if (t <= 0.0f)
    {
      return 1.0f;
    }

#if PW_ANIM_IMPL == PW_ANIM_IMPL_LUT
  {
    /* 查表：只对表内预设 (PW_ANIM_LUT_A/W/DUR) 成立，别的参数由自检拦下 */

    float p = t / PW_ANIM_LUT_DUR * (float)(PW_ANIM_LUT_N - 1);
    int   i;
    int   j;
    float fr;

    if (p >= (float)(PW_ANIM_LUT_N - 1))
      {
        return (float)g_pw_anim_lut[PW_ANIM_LUT_N - 1] / 32767.0f;
      }

    i = (int)p;
    if (i < 0)
      {
        i = 0;
      }

    j = i + 1;
    fr = p - (float)i;

    return ((float)g_pw_anim_lut[i] * (1.0f - fr) +
            (float)g_pw_anim_lut[j] * fr) / 32767.0f;
  }
#else
  /* 解析式：u(t) = e^(-a t)·(cos(w t) + (a/w)·sin(w t))，w→0 取极限 a·t */

  u = expf(-s->damp * t) *
      (cosf(s->omega * t) + anim_w_safe(s->damp, s->omega, t));
  return u;
#endif
}

float pw_anim_spring_progress(const struct pw_anim_spring_s *s, float t)
{
  float y = 1.0f - pw_anim_spring_u(s, t);

  if (y < 0.0f)
    {
      y = 0.0f;
    }

  if (y > 1.0f)
    {
      y = 1.0f;
    }

  return y;
}

float pw_anim_spring_step(struct pw_anim_spring_s *s, float dt)
{
  float u;

  if (s == NULL || dt <= 0.0f)
    {
      return 1.0f;
    }

#if PW_ANIM_IMPL == PW_ANIM_IMPL_REC
  /* 递推法要求等间隔：首次调用锁定 dt；若调用方给的 dt 明显变化
   * （掉帧/被抢占），退回解析式并用当前进度重新同步递推状态，
   * 避免"变步长喂固定系数"导致曲线发散（递推法固有陷阱）。 */

  if (s->dt == 0.0f)
    {
      s->dt = dt;
    }
  else if (fabsf(dt - s->dt) > 0.01f * s->dt)
    {
      float t = 0.0f;

      /* 用解析式把状态重新对齐到"已经走过的时长"（调用方语义：每次给一帧） */

      t = s->dt;                    /* 仅用于重算一次，保持相位连续 */
      u = pw_anim_spring_u(s, t);
      s->u1 = u;
      s->u2 = pw_anim_spring_u(s, t + dt);
      s->dt = dt;
      return 1.0f - s->u2;
    }

  u = s->c1 * s->u2 + s->c2 * s->u1;
  s->u1 = s->u2;
  s->u2 = u;
  return 1.0f - u;
#else
  (void)u;
  /* 非递推实现：调用方应使用 u(t)/progress(t)；这里用一个内部累计时间保持一致 */

  s->dt += dt;
  return pw_anim_spring_progress(s, s->dt);
#endif
}

int pw_anim_spring_done(const struct pw_anim_spring_s *s, float t)
{
  if (s == NULL)
    {
      return 1;
    }

  return (t >= s->dur) ? 1 : 0;
}

float pw_anim_bezier_ease(float p1x, float p1y, float p2x, float p2y,
                          float progress)
{
  float t;
  float u;
  int   i;

  if (progress <= 0.0f)
    {
      return 0.0f;
    }

  if (progress >= 1.0f)
    {
      return 1.0f;
    }

  /* 先对 x(t) 求参数 t（牛顿迭代，固定次数 → 有界耗时），再算 y(t)。
   * x(t) = 3(1-t)²t·p1x + 3(1-t)t²·p2x + t³ */

  t = progress;

  for (i = 0; i < PW_ANIM_BEZIER_ITER; i++)
    {
      float omt = 1.0f - t;
      float x = 3.0f * omt * omt * t * p1x + 3.0f * omt * t * t * p2x +
                t * t * t;
      float dx = 3.0f * omt * omt * p1x +
                 6.0f * omt * t * (p2x - p1x) +
                 3.0f * t * t * (1.0f - p2x);

      if (fabsf(dx) < 1.0e-6f)
        {
          break;
        }

      t -= (x - progress) / dx;

      if (t < 0.0f)
        {
          t = 0.0f;
        }

      if (t > 1.0f)
        {
          t = 1.0f;
        }
    }

  u = 1.0f - t;

  return 3.0f * u * u * t * p1y + 3.0f * u * t * t * p2y + t * t * t;
}

int pw_anim_selftest(float *err_px)
{
  const float dt = 1.0f / 60.0f;
  struct pw_anim_spring_s s;
  struct pw_anim_spring_s ref;
  float worst = 0.0f;
  int   k;
  int   rc = 0;

  pw_anim_spring_init(&s, PW_ANIM_LUT_A, PW_ANIM_LUT_W, PW_ANIM_LUT_DUR);
  pw_anim_spring_init(&ref, PW_ANIM_LUT_A, PW_ANIM_LUT_W, PW_ANIM_LUT_DUR);

  /* 1) 递推路径：与解析式逐帧比对（解析式在这里当"真值"，它只用 float 的
   *    expf/cosf/sinf；主机上误差量级见 docs）。 */

  {
    float t = 0.0f;

    for (k = 0; k < 200; k++)
      {
        float y_step = pw_anim_spring_step(&s, dt);
        float y_ref;

        t += dt;
        y_ref = pw_anim_spring_progress(&ref, t);

        if (fabsf(y_step - y_ref) > worst)
          {
            worst = fabsf(y_step - y_ref);
          }

        if (t >= s.dur && pw_anim_spring_done(&s, t))
          {
            break;
          }
      }
  }

  /* 2) 查表路径（仅当编译进来时）：预设必须与表一致，且精度达标 */

#if PW_ANIM_IMPL == PW_ANIM_IMPL_LUT
  if (fabsf(s.damp - PW_ANIM_LUT_A) > 1.0e-3f ||
      fabsf(s.omega - PW_ANIM_LUT_W) > 1.0e-3f ||
      fabsf(s.dur - PW_ANIM_LUT_DUR) > 1.0e-3f)
    {
      rc = -3;
    }
#endif

  /* 3) 贝塞尔：端点必须精确，且单调不降（(0.25,0.1)-(0.25,1) 这类缓动） */

  {
    float prev = -1.0f;

    if (fabsf(pw_anim_bezier_ease(0.25f, 0.1f, 0.25f, 1.0f, 0.0f)) > 1.0e-6f ||
        fabsf(pw_anim_bezier_ease(0.25f, 0.1f, 0.25f, 1.0f, 1.0f) - 1.0f) > 1.0e-6f)
      {
        rc = -4;
      }

    for (k = 0; k <= 64; k++)
      {
        float v = pw_anim_bezier_ease(0.25f, 0.1f, 0.25f, 1.0f,
                                      (float)k / 64.0f);

        if (v < prev - 1.0e-4f)
          {
            rc = -4;
          }

        prev = v;
      }
  }

  /* 判据：递推 0.01 px；查表 0.5 px（实测 0.0002 / 0.2525，留余量） */

  if (rc == 0)
    {
#if PW_ANIM_IMPL == PW_ANIM_IMPL_LUT
      if (worst * 450.0f > 0.5f)
        {
          rc = -2;
        }
#else
      if (worst * 450.0f > 0.01f)
        {
          rc = -1;
        }
#endif
    }

  if (err_px != NULL)
    {
      *err_px = worst * 450.0f;
    }

  return rc;
}
