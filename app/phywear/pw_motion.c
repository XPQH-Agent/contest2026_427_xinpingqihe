/****************************************************************************
 * apps/examples/phywear/pw_motion.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 动效数学核心实现（查表 + 解析式双实现）。曲线参数与生成脚本见
 * tools/phywear/gen_motion_table.py；表文件 pw_motion_table.c 是生成物。
 *
 * 整数插值为什么这样写：
 *   u1024 ∈ [0,1024] → x = u1024 × (N-1) ∈ [0, 16256]
 *   i = x >> 10（整数索引），f = x & 1023（小数，分母 1024）
 *   v = tab[i] + ((tab[i+1] - tab[i]) × f >> 10)
 *   全程整数、无除法；|Δ| ≤ 32767×2=65534，×1023 后 6.7e7，int32 装得下。
 *   用 1024 做分母而不是 (N-1)：省一次除法，代价只是"小数刻度"与表步长不完全对齐
 *   （引入的额外误差 < 1e-4，远小于插值误差本身）。
 ****************************************************************************/

#include <nuttx/config.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#include "pw_motion.h"

/* 曲线常量：必须与 gen_motion_table.py 一致 */

#define SPRING_ZETA   0.40f
#define SPRING_OMEGA  12.0f
#define DECAY_A       6.0f
#define DECAY_F       2.0f

/* 表查值：整数索引 + 线性插值。表已按 s(1) 归一化，末点严格为 ONE。 */

static int32_t motion_lookup(const int16_t *tab, uint32_t u1024)
{
  uint32_t x;
  uint32_t i;
  uint32_t f;
  int32_t  a;
  int32_t  b;

  if (u1024 >= PW_MOTION_RES)
    {
      return PW_MOTION_ONE;             /* 末帧严格到位，不留 1 像素的跳变 */
    }

  x = u1024 * (uint32_t)(PW_MOTION_N - 1);
  i = x >> 10;
  f = x & 1023u;

  if (i >= (uint32_t)(PW_MOTION_N - 1))
    {
      return PW_MOTION_ONE;
    }

  a = tab[i];
  b = tab[i + 1];

  return a + (int32_t)(((b - a) * (int32_t)f) >> 10);
}

/* 档位 → 查表（越界回 C2，保持与旧版一致） */

static const int16_t *motion_tier_tab(int tier)
{
  switch (tier)
    {
      case PW_MOTION_TIER_C1: return pw_motion_tab_c1;
      case PW_MOTION_TIER_C3: return pw_motion_tab_c3;
      case PW_MOTION_TIER_C4: return pw_motion_tab_c4;
      default:                return pw_motion_tab_c2;
    }
}

int32_t pw_motion_spring_q14_t(int tier, uint32_t u1024)
{
#if PW_MOTION_USE_TABLE
  return motion_lookup(motion_tier_tab(tier), u1024);
#else
  return pw_motion_spring_analytic_q14_t(tier, u1024);
#endif
}

int32_t pw_motion_spring_q14(uint32_t u1024)
{
  return pw_motion_spring_q14_t(PW_MOTION_TIER_C2, u1024);   /* 向后兼容：旧调用点 = C2 */
}

int32_t pw_motion_decay_q14(uint32_t u1024)
{
#if PW_MOTION_USE_TABLE
  return motion_lookup(pw_motion_tab_decay, u1024);
#else
  return pw_motion_decay_analytic_q14(u1024);
#endif
}

/****************************************************************************
 * 解析式实现（对照与回退用，始终编译）
 ****************************************************************************/

int32_t pw_motion_spring_analytic_q14_t(int tier, uint32_t u1024)
{
  float z;
  float w;

  switch (tier)
    {
      case PW_MOTION_TIER_C1: z = pw_motion_c1_zeta; w = pw_motion_c1_omega; break;
      case PW_MOTION_TIER_C3: z = pw_motion_c3_zeta; w = pw_motion_c3_omega; break;
      case PW_MOTION_TIER_C4: z = pw_motion_c4_zeta; w = pw_motion_c4_omega; break;
      default:                z = pw_motion_c2_zeta; w = pw_motion_c2_omega; break;
    }

  if (u1024 >= PW_MOTION_RES)
    {
      return PW_MOTION_ONE;
    }

  {
    float u = (float)u1024 / (float)PW_MOTION_RES;
    float wd = w * sqrtf(1.0f - z * z);
    float s = 1.0f - expf(-z * w * u) * (cosf(wd * u) +
                                         (z * w / wd) * sinf(wd * u));
    float s1 = 1.0f - expf(-z * w) * (cosf(wd) + (z * w / wd) * sinf(wd));

    s /= s1;                        /* 与表同一套归一化：末帧严格 1.0 */

    return (int32_t)(s * (float)PW_MOTION_ONE + 0.5f);
  }
}

int32_t pw_motion_spring_analytic_q14(uint32_t u1024)
{
  return pw_motion_spring_analytic_q14_t(PW_MOTION_TIER_C2, u1024);
}

int32_t pw_motion_decay_analytic_q14(uint32_t u1024)
{
  float u;
  float v;

  if (u1024 >= PW_MOTION_RES)
    {
      return 0;
    }

  u = (float)u1024 / (float)PW_MOTION_RES;
  v = expf(-DECAY_A * u) * sinf(6.2831853f * DECAY_F * u);

  v *= pw_motion_decay_k;

  return (int32_t)(v * (float)PW_MOTION_ONE);
}

/****************************************************************************
 * 自检
 ****************************************************************************/

int pw_motion_selftest(float *err)
{
  static const uint32_t probe[] =
  {
    0, 1, 2, 8, 32, 64, 128, 256, 384, 512, 640, 768, 896, 1023, 1024
  };

  float worst = 0.0f;
  int   i;

  /* 端点：起步必须是 0，末帧必须严格是 1.0（LVGL 里就是"落在 end_value"） */

  if (pw_motion_spring_q14(0) != 0)
    {
      return -1;
    }

  if (pw_motion_spring_q14(PW_MOTION_RES) != PW_MOTION_ONE)
    {
      return -2;
    }

  if (pw_motion_spring_q14(100000) != PW_MOTION_ONE)
    {
      return -3;                        /* 越界时间也要收敛到末值 */
    }

  /* 过冲必须存在（否则弹簧手感没了）：峰值应落在 1.15~1.35 之间 */

  {
    int32_t peak = 0;
    uint32_t u;

    for (u = 0; u <= PW_MOTION_RES; u++)
      {
        int32_t v = pw_motion_spring_q14(u);

        if (v > peak)
          {
            peak = v;
          }
      }

    if (peak < (int32_t)(1.15f * PW_MOTION_ONE) ||
        peak > (int32_t)(1.35f * PW_MOTION_ONE))
      {
        return -4;
      }
  }

  /* 查表 vs 解析：逐点比，误差应 < 3e-3（表长 128 + 线性插值） */

  for (i = 0; i < (int)(sizeof(probe) / sizeof(probe[0])); i++)
    {
      float a = (float)pw_motion_spring_analytic_q14(probe[i]) /
                (float)PW_MOTION_ONE;
      float b = (float)pw_motion_spring_q14(probe[i]) / (float)PW_MOTION_ONE;
      float e = fabsf(a - b);

      if (e > worst)
        {
          worst = e;
        }
    }

  if (worst > 3.0e-3f)
    {
      return -5;
    }

  if (err != NULL)
    {
      *err = worst;
    }

  return 0;
}

/****************************************************************************
 * 计时：查表 vs 解析（真机 / 主机同一份代码）
 ****************************************************************************/

static uint64_t motion_now_ns(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);

  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void pw_motion_bench(int iters)
{
  volatile int32_t sink = 0;
  uint64_t t0;
  uint64_t t1;
  uint64_t t2;
  uint64_t t3;
  int      i;

  if (iters <= 0)
    {
      iters = 20000;
    }

  /* 查表：整模块走的路径 */

  t0 = motion_now_ns();
  for (i = 0; i < iters; i++)
    {
      sink += pw_motion_spring_q14((uint32_t)((i * 37) & 1023));
    }

  t1 = motion_now_ns();

  /* 解析式：expf + sinf + cosf */

  t2 = motion_now_ns();
  for (i = 0; i < iters; i++)
    {
      sink += pw_motion_spring_analytic_q14((uint32_t)((i * 37) & 1023));
    }

  t3 = motion_now_ns();

  printf("[MOTION] iters=%d\n", iters);
  /* 每次调用可能远小于 1 ns（查表路径在 x86 上就是），整数除法会打成 0，
   * 所以按 0.01 ns 定标后再打印。 */

  printf("[MOTION] table    : %llu ns total, %u.%02u ns/call\n",
         (unsigned long long)(t1 - t0),
         (unsigned)(((t1 - t0) * 100u / (uint64_t)iters) / 100u),
         (unsigned)(((t1 - t0) * 100u / (uint64_t)iters) % 100u));
  printf("[MOTION] analytic : %llu ns total, %u.%02u ns/call\n",
         (unsigned long long)(t3 - t2),
         (unsigned)(((t3 - t2) * 100u / (uint64_t)iters) / 100u),
         (unsigned)(((t3 - t2) * 100u / (uint64_t)iters) % 100u));
  printf("[MOTION] speedup  : x%u (analytic/table)\n",
         (unsigned)((t3 - t2 + 1) / (t1 - t0 + 1)));
  printf("[MOTION] compile-time switch PW_MOTION_USE_TABLE=%d, "
         "table bytes=%d\n", PW_MOTION_USE_TABLE,
         (int)(sizeof(pw_motion_tab_spring) + sizeof(pw_motion_tab_decay)));
  printf("[MOTION] sink=%ld (防止被优化掉)\n", (long)sink);
}
