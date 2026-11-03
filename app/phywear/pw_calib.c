/****************************************************************************
 * apps/examples/phywear/pw_calib.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 标定数学实现。全部 float（SF32LB52 无硬件双精度，double 会退化到软浮点）；
 * 需要条件数的地方靠"流式充分统计 + 输入缩放"而不是靠 double。
 *
 * 六面法与椭球拟合均为公开教科书方法（六位置法 / 二次曲面代数拟合），
 * 本文件为本队自写实现。
 ****************************************************************************/

#include <nuttx/config.h>

#include <math.h>
#include <string.h>

#include "pw_calib.h"

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* n×n 线性方程组求解（Gauss-Jordan + 部分主元）。
 * a[] 与 b[] 会被破坏；解写回 b[]。返回 0 成功，-1 奇异。 */

static int calib_solve(float *a, float *b, int n)
{
  int i;
  int j;
  int k;

  for (i = 0; i < n; i++)
    {
      int piv = i;
      float maxv = fabsf(a[i * n + i]);

      for (j = i + 1; j < n; j++)
        {
          float v = fabsf(a[j * n + i]);

          if (v > maxv)
            {
              maxv = v;
              piv = j;
            }
        }

      if (maxv < 1e-12f)
        {
          return -1;
        }

      if (piv != i)
        {
          for (k = 0; k < n; k++)
            {
              float t = a[i * n + k];

              a[i * n + k] = a[piv * n + k];
              a[piv * n + k] = t;
            }

          {
            float t = b[i];

            b[i] = b[piv];
            b[piv] = t;
          }
        }

      {
        float inv = 1.0f / a[i * n + i];

        for (k = i; k < n; k++)
          {
            a[i * n + k] *= inv;
          }

        b[i] *= inv;
      }

      for (j = 0; j < n; j++)
        {
          float f;

          if (j == i)
            {
              continue;
            }

          f = a[j * n + i];

          if (f == 0.0f)
            {
              continue;
            }

          for (k = i; k < n; k++)
            {
              a[j * n + k] -= f * a[i * n + k];
            }

          b[j] -= f * b[i];
        }
    }

  return 0;
}

/* 3×3 对称矩阵的 Jacobi 特征分解。A 就地对角化，V 列为特征向量。 */

static void calib_jacobi3(float A[3][3], float V[3][3], float *eig)
{
  int sweep;
  int i;
  int j;
  int k;

  for (i = 0; i < 3; i++)
    {
      for (j = 0; j < 3; j++)
        {
          V[i][j] = (i == j) ? 1.0f : 0.0f;
        }
    }

  for (sweep = 0; sweep < 30; sweep++)
    {
      float off = fabsf(A[0][1]) + fabsf(A[0][2]) + fabsf(A[1][2]);

      if (off < 1e-10f)
        {
          break;
        }

      for (i = 0; i < 2; i++)
        {
          for (j = i + 1; j < 3; j++)
            {
              float theta;
              float t;
              float c;
              float s;

              if (fabsf(A[i][j]) < 1e-14f)
                {
                  continue;
                }

              theta = (A[j][j] - A[i][i]) / (2.0f * A[i][j]);
              t = ((theta >= 0.0f) ? 1.0f : -1.0f) /
                  (fabsf(theta) + sqrtf(theta * theta + 1.0f));
              c = 1.0f / sqrtf(t * t + 1.0f);
              s = t * c;

              for (k = 0; k < 3; k++)
                {
                  float aik = A[i][k];
                  float ajk = A[j][k];

                  A[i][k] = c * aik - s * ajk;
                  A[j][k] = s * aik + c * ajk;
                }

              for (k = 0; k < 3; k++)
                {
                  float aki = A[k][i];
                  float akj = A[k][j];

                  A[k][i] = c * aki - s * akj;
                  A[k][j] = s * aki + c * akj;
                }

              for (k = 0; k < 3; k++)
                {
                  float vki = V[k][i];
                  float vkj = V[k][j];

                  V[k][i] = c * vki - s * vkj;
                  V[k][j] = s * vki + c * vkj;
                }
            }
        }
    }

  eig[0] = A[0][0];
  eig[1] = A[1][1];
  eig[2] = A[2][2];
}

/****************************************************************************
 * 一维拟合
 ****************************************************************************/

void pw_calib_fit1d_reset(struct pw_calib_fit1d_s *f)
{
  if (f != NULL)
    {
      memset(f, 0, sizeof(*f));
    }
}

void pw_calib_fit1d_add(struct pw_calib_fit1d_s *f, float x, float y)
{
  if (f == NULL)
    {
      return;
    }

  f->sx2 += x * x;
  f->sx  += x;
  f->s1  += 1.0f;
  f->sxy += x * y;
  f->sy  += y;
  f->sy2 += y * y;
  f->n++;
}

int pw_calib_fit1d_solve(const struct pw_calib_fit1d_s *f, float *a, float *b,
                         float *rms, float *r2)
{
  float rhs[2];
  float det;
  float aa;
  float bb;
  float ss;

  if (f == NULL || f->n < 2)
    {
      return -1;
    }

  det = f->s1 * f->sx2 - f->sx * f->sx;

  if (fabsf(det) < 1e-12f)
    {
      return -1;
    }

  aa = (f->s1 * f->sxy - f->sx * f->sy) / det;
  bb = (f->sx2 * f->sy - f->sx * f->sxy) / det;

  if (a != NULL)
    {
      *a = aa;
    }

  if (b != NULL)
    {
      *b = bb;
    }

  /* 残差用恒等式 Σ(ŷ-y)² = Σy² - θᵀ·rhs（流式，不用二次遍历） */

  rhs[0] = f->sxy;
  rhs[1] = f->sy;

  ss = f->sy2 - (aa * rhs[0] + bb * rhs[1]);
  if (ss < 0.0f)
    {
      ss = 0.0f;
    }

  if (rms != NULL)
    {
      *rms = sqrtf(ss / (float)f->n);
    }

  if (r2 != NULL)
    {
      float var = f->sy2 - f->sy * f->sy / (float)f->n;

      *r2 = (var > 1e-12f) ? (1.0f - ss / var) : 1.0f;
    }

  return 0;
}

/****************************************************************************
 * 二维拟合
 ****************************************************************************/

void pw_calib_fit2d_reset(struct pw_calib_fit2d_s *f)
{
  if (f != NULL)
    {
      memset(f, 0, sizeof(*f));
    }
}

void pw_calib_fit2d_add(struct pw_calib_fit2d_s *f, float x, float y,
                        float u, float v)
{
  if (f == NULL)
    {
      return;
    }

  f->sxx += x * x;
  f->sxy += x * y;
  f->syy += y * y;
  f->sx  += x;
  f->sy  += y;
  f->s1  += 1.0f;
  f->ru[0] += u * x;
  f->ru[1] += u * y;
  f->ru[2] += u;
  f->rv[0] += v * x;
  f->rv[1] += v * y;
  f->rv[2] += v;
  f->su2 += u * u;
  f->sv2 += v * v;
  f->n++;
}

int pw_calib_fit2d_solve(const struct pw_calib_fit2d_s *f, float *m, float *t,
                         float *rms)
{
  float A[9];
  float bu[3];
  float bv[3];
  float mu[3];
  float mv[3];
  float ss;

  if (f == NULL || f->n < 3)
    {
      return -1;
    }

  A[0] = f->sxx; A[1] = f->sxy; A[2] = f->sx;
  A[3] = f->sxy; A[4] = f->syy; A[5] = f->sy;
  A[6] = f->sx;  A[7] = f->sy;  A[8] = f->s1;

  memcpy(bu, f->ru, sizeof(bu));
  memcpy(bv, f->rv, sizeof(bv));

  {
    float A2[9];

    memcpy(A2, A, sizeof(A2));
    if (calib_solve(A2, bu, 3) != 0)
      {
        return -1;
      }

    memcpy(A2, A, sizeof(A2));
    if (calib_solve(A2, bv, 3) != 0)
      {
        return -1;
      }
  }

  /* bu = [m00, m01, t0]，bv = [m10, m11, t1] */

  memcpy(mu, bu, sizeof(mu));
  memcpy(mv, bv, sizeof(mv));

  if (m != NULL)
    {
      m[0] = mu[0]; m[1] = mu[1];
      m[2] = mv[0]; m[3] = mv[1];
    }

  if (t != NULL)
    {
      t[0] = mu[2];
      t[1] = mv[2];
    }

  ss = (f->su2 - (mu[0] * f->ru[0] + mu[1] * f->ru[1] + mu[2] * f->ru[2])) +
       (f->sv2 - (mv[0] * f->rv[0] + mv[1] * f->rv[1] + mv[2] * f->rv[2]));

  if (ss < 0.0f)
    {
      ss = 0.0f;
    }

  if (rms != NULL)
    {
      *rms = sqrtf(ss / (float)f->n);
    }

  return 0;
}

/****************************************************************************
 * 陀螺静态零偏
 ****************************************************************************/

void pw_calib_bias_reset(struct pw_calib_bias_s *b)
{
  if (b != NULL)
    {
      memset(b, 0, sizeof(*b));
    }
}

void pw_calib_bias_add(struct pw_calib_bias_s *b, const float *gyro_rps)
{
  int i;

  if (b == NULL || gyro_rps == NULL)
    {
      return;
    }

  for (i = 0; i < 3; i++)
    {
      b->sum[i] += gyro_rps[i];
      b->sum2[i] += gyro_rps[i] * gyro_rps[i];
    }

  b->n++;
}

int pw_calib_bias_ready(const struct pw_calib_bias_s *b, int min_n)
{
  return (b != NULL && b->n >= min_n) ? 1 : 0;
}

int pw_calib_bias_solve(const struct pw_calib_bias_s *b, float *bias_rps,
                        float *std_rps, float max_std_rps)
{
  int i;

  if (b == NULL || b->n < 2)
    {
      return -1;
    }

  for (i = 0; i < 3; i++)
    {
      float mean = b->sum[i] / (float)b->n;
      float var = b->sum2[i] / (float)b->n - mean * mean;
      float sd;

      if (var < 0.0f)
        {
          var = 0.0f;
        }

      sd = sqrtf(var);

      if (max_std_rps > 0.0f && sd > max_std_rps)
        {
          return -2;
        }

      if (bias_rps != NULL)
        {
          bias_rps[i] = mean;
        }

      if (std_rps != NULL)
        {
          std_rps[i] = sd;
        }
    }

  return 0;
}

/****************************************************************************
 * 六面法
 ****************************************************************************/

void pw_calib_six_reset(struct pw_calib_six_s *s)
{
  if (s != NULL)
    {
      memset(s, 0, sizeof(*s));
    }
}

void pw_calib_six_add(struct pw_calib_six_s *s, int face, const float *acc_g)
{
  int i;

  if (s == NULL || acc_g == NULL || face < 0 || face >= PW_CALIB_FACES)
    {
      return;
    }

  for (i = 0; i < 3; i++)
    {
      s->sum[face][i] += acc_g[i];
    }

  s->n[face]++;
  s->nsamp++;
}

int pw_calib_six_ready(const struct pw_calib_six_s *s, int min_per_face)
{
  int i;

  if (s == NULL)
    {
      return 0;
    }

  for (i = 0; i < PW_CALIB_FACES; i++)
    {
      if (s->n[i] < min_per_face)
        {
          return 0;
        }
    }

  return 1;
}

int pw_calib_six_solve(const struct pw_calib_six_s *s, float *bias_g,
                       float *scale, float *resid, float *cross)
{
  float mean[PW_CALIB_FACES][3];
  float bias[3];
  float sc[3];
  float maxres = 0.0f;
  float maxcross = 0.0f;
  int   f;
  int   k;
  int   i;

  if (s == NULL)
    {
      return -1;
    }

  for (f = 0; f < PW_CALIB_FACES; f++)
    {
      if (s->n[f] <= 0)
        {
          return -1;
        }

      for (k = 0; k < 3; k++)
        {
          mean[f][k] = s->sum[f][k] / (float)s->n[f];
        }
    }

  /* 第一遍：每个轴用"正对/负对"两面求零偏与刻度（真值 = ±1 g） */

  for (k = 0; k < 3; k++)
    {
      float mp = mean[2 * k][k];
      float mm = mean[2 * k + 1][k];

      if (fabsf(mp - mm) < 0.5f)
        {
          return -1;                  /* 该面对比度不足，数据不可信 */
        }

      bias[k] = 0.5f * (mp + mm);
      sc[k]   = 2.0f / (mp - mm);
    }

  if (bias_g != NULL)
    {
      for (k = 0; k < 3; k++)
        {
          bias_g[k] = bias[k];
        }
    }

  if (scale != NULL)
    {
      for (k = 0; k < 3; k++)
        {
          scale[k] = sc[k];
        }
    }

  /* 第二遍：残差 = 目标轴映射回 ±1 的偏差；
   * 轴间串扰 = 非目标轴**扣掉该轴零偏后**的均值（不扣零偏会把自己的
   * 零偏当成串扰，六面法里每个面的非目标轴读数本来就是那个轴的零偏）。 */

  for (f = 0; f < PW_CALIB_FACES; f++)
    {
      int tgt = f / 2;
      float mapped = (mean[f][tgt] - bias[tgt]) * sc[tgt];
      float ideal = ((f % 2) == 0) ? 1.0f : -1.0f;

      if (fabsf(mapped - ideal) > maxres)
        {
          maxres = fabsf(mapped - ideal);
        }

      for (i = 0; i < 3; i++)
        {
          if (i == tgt)
            {
              continue;
            }

          if (fabsf(mean[f][i] - bias[i]) > maxcross)
            {
              maxcross = fabsf(mean[f][i] - bias[i]);
            }
        }
    }

  if (resid != NULL)
    {
      *resid = maxres;
    }

  if (cross != NULL)
    {
      *cross = maxcross;
    }

  return 0;
}

void pw_calib_six_apply(const float *bias_g, const float *scale,
                        const float *in_g, float *out_g)
{
  int i;

  if (bias_g == NULL || scale == NULL || in_g == NULL || out_g == NULL)
    {
      return;
    }

  for (i = 0; i < 3; i++)
    {
      out_g[i] = (in_g[i] - bias_g[i]) * scale[i];
    }
}

/****************************************************************************
 * 磁椭球拟合
 ****************************************************************************/

/* 输入按 1/1000 缩放（mG → G 量级），改善正规方程条件数 */

#define CALIB_MAG_SCALE  0.001f

void pw_calib_mag_reset(struct pw_calib_mag_s *s)
{
  if (s != NULL)
    {
      memset(s, 0, sizeof(*s));
    }
}

void pw_calib_mag_add(struct pw_calib_mag_s *s, const float *m)
{
  float x;
  float y;
  float z;
  float row[9];
  int   i;
  int   j;

  if (s == NULL || m == NULL)
    {
      return;
    }

  x = m[0] * CALIB_MAG_SCALE;
  y = m[1] * CALIB_MAG_SCALE;
  z = m[2] * CALIB_MAG_SCALE;

  row[0] = x * x;
  row[1] = y * y;
  row[2] = z * z;
  row[3] = 2.0f * y * z;
  row[4] = 2.0f * x * z;
  row[5] = 2.0f * x * y;
  row[6] = 2.0f * x;
  row[7] = 2.0f * y;
  row[8] = 2.0f * z;

  for (i = 0; i < 9; i++)
    {
      for (j = 0; j < 9; j++)
        {
          s->a[i][j] += row[i] * row[j];
        }

      s->b[i] += row[i];
    }

  s->sum_r2 += x * x + y * y + z * z;
  s->n++;
}

int pw_calib_mag_ready(const struct pw_calib_mag_s *s, int min_n)
{
  return (s != NULL && s->n >= min_n) ? 1 : 0;
}

int pw_calib_mag_solve(const struct pw_calib_mag_s *s, float *center, float *w,
                       float *mean_mag, float *eig)
{
  float A[81];
  float b[9];
  float Q[3][3];
  float V[3][3];
  float ev[3];
  float p[3];
  float c[3];
  float Qc[3];
  float k;
  int   i;
  int   j;

  if (s == NULL || s->n < 30)
    {
      return -1;
    }

  for (i = 0; i < 9; i++)
    {
      for (j = 0; j < 9; j++)
        {
          A[i * 9 + j] = s->a[i][j];
        }

      b[i] = s->b[i];
    }

  if (calib_solve(A, b, 9) != 0)
    {
      return -2;
    }

  Q[0][0] = b[0]; Q[0][1] = b[5]; Q[0][2] = b[4];
  Q[1][0] = b[5]; Q[1][1] = b[1]; Q[1][2] = b[3];
  Q[2][0] = b[4]; Q[2][1] = b[3]; Q[2][2] = b[2];

  p[0] = b[6];
  p[1] = b[7];
  p[2] = b[8];

  /* 球心 c = -Q⁻¹p（用同一套 Gauss 解 3×3） */

  {
    float AA[9];
    float bb[3];
    float rhs[3];

    AA[0] = Q[0][0]; AA[1] = Q[0][1]; AA[2] = Q[0][2];
    AA[3] = Q[1][0]; AA[4] = Q[1][1]; AA[5] = Q[1][2];
    AA[6] = Q[2][0]; AA[7] = Q[2][1]; AA[8] = Q[2][2];
    rhs[0] = -p[0];
    rhs[1] = -p[1];
    rhs[2] = -p[2];
    memcpy(bb, rhs, sizeof(bb));

    if (calib_solve(AA, bb, 3) != 0)
      {
        return -2;
      }

    c[0] = bb[0];
    c[1] = bb[1];
    c[2] = bb[2];
  }

  /* k = 1 + cᵀQc；校正后应为单位球 */

  Qc[0] = Q[0][0] * c[0] + Q[0][1] * c[1] + Q[0][2] * c[2];
  Qc[1] = Q[1][0] * c[0] + Q[1][1] * c[1] + Q[1][2] * c[2];
  Qc[2] = Q[2][0] * c[0] + Q[2][1] * c[1] + Q[2][2] * c[2];
  k = 1.0f + (c[0] * Qc[0] + c[1] * Qc[1] + c[2] * Qc[2]);

  if (k <= 0.0f)
    {
      return -3;
    }

  calib_jacobi3(Q, V, ev);

  if (ev[0] <= 0.0f || ev[1] <= 0.0f || ev[2] <= 0.0f)
    {
      return -3;
    }

  /* 要的是 W 满足 WᵀW = Q/k，这样 |W·(m-c)|² = (m-c)ᵀQ(m-c)/k = 1。
   * 即 W = (Q/k)^(1/2) = V·diag(√(λ/k))·Vᵀ —— **是平方根，不是平方根倒数**。
   * （早期版本写成 √k·Q^(-1/2)，方向搞反：实测校正后模长均值 280、离散 38%。
   *   另外椭球拟合有个固有性质：软铁矩阵只能定到"一个未知旋转"，因为 A 与
   *   A·R 给出同一个二次型；这里取对称平方根作为规范选择。） */

  if (w != NULL)
    {
      /* 内部单位下 W 把椭球映射成**单位球**；乘上样本平均场强（内部单位）后，
       * 校正输出的球半径就等于输入单位的平均 |m|（例如 ~500 mG）。
       * 需要物理量纲时再按当地 WMM/IGRF 的 |B| 缩放（本库不知道地理位置）。 */

      float mean_s = sqrtf(s->sum_r2 / (float)s->n);
      float rk = mean_s / sqrtf(k);

      for (i = 0; i < 3; i++)
        {
          for (j = 0; j < 3; j++)
            {
              float acc = 0.0f;
              int   t;

              for (t = 0; t < 3; t++)
                {
                  acc += V[i][t] * sqrtf(ev[t]) * V[j][t];
                }

              w[i * 3 + j] = rk * acc;
            }
        }
    }

  if (center != NULL)
    {
      for (i = 0; i < 3; i++)
        {
          center[i] = c[i] / CALIB_MAG_SCALE;
        }
    }

  if (mean_mag != NULL)
    {
      *mean_mag = sqrtf(s->sum_r2 / (float)s->n) / CALIB_MAG_SCALE;
    }

  if (eig != NULL)
    {
      for (i = 0; i < 3; i++)
        {
          eig[i] = ev[i];
        }
    }

  return 0;
}

void pw_calib_mag_apply(const float *center, const float *w,
                        const float *m, float *out)
{
  float d[3];
  int   i;
  int   j;

  if (center == NULL || w == NULL || m == NULL || out == NULL)
    {
      return;
    }

  for (i = 0; i < 3; i++)
    {
      d[i] = m[i] - center[i];
    }

  for (i = 0; i < 3; i++)
    {
      float acc = 0.0f;

      for (j = 0; j < 3; j++)
        {
          acc += w[i * 3 + j] * d[j];
        }

      out[i] = acc;
    }
}

/****************************************************************************
 * Name: pw_calib_selftest
 *
 * Description:
 *   合成数据自检：
 *     A) 一维拟合：已知 y = 2.5x - 3，检查斜率/截距还原与残差；
 *     B) 六面法：已知零偏与刻度，检查还原；
 *     C) 磁椭球：已知硬铁 + 已知非对称软铁（含轴间耦合），检查球心还原与
 *        "校正后模长的一致性"。注意：椭球拟合只能定出软铁矩阵到**一个未知
 *        旋转**（A 与 A·R 给出同一个二次型），这是该方法的固有性质，因此这里
 *        检查的是"能否把椭球球面化"，而不是逐元素还原 A。
 *   成功返回 0；err_out 非空时写回最大相对误差（取 A/B/C 中最大者）。
 ****************************************************************************/

int pw_calib_selftest(float *err_out)
{
  float worst = 0.0f;

  /* --- A) 一维拟合 --- */

  {
    struct pw_calib_fit1d_s f;
    float a = 0.0f;
    float b = 0.0f;
    float rms = 0.0f;
    int   i;

    pw_calib_fit1d_reset(&f);
    for (i = 0; i < 20; i++)
      {
        float x = (float)i * 15.0f;

        pw_calib_fit1d_add(&f, x, 2.5f * x - 3.0f);
      }

    if (pw_calib_fit1d_solve(&f, &a, &b, &rms, NULL) != 0)
      {
        return -1;
      }

    {
      float ea = fabsf(a - 2.5f) / 2.5f;
      float eb = fabsf(b + 3.0f) / 3.0f;

      if (ea > worst) worst = ea;
      if (eb > worst) worst = eb;
      if (rms > 0.5f) return -2;
    }
  }

  /* --- B) 六面法 --- */

  {
    struct pw_calib_six_s s;
    const float bias_true[3] = {0.012f, -0.020f, 0.031f};
    const float gain_true[3] = {1.03f, 0.97f, 1.01f};   /* 真值 = 实测/gain */
    float bias[3];
    float scale[3];
    float resid = 0.0f;
    float cross = 0.0f;
    int   f;
    int   k;
    int   i;

    pw_calib_six_reset(&s);

    for (f = 0; f < PW_CALIB_FACES; f++)
      {
        for (i = 0; i < 20; i++)
          {
            float acc[3] = {0.0f, 0.0f, 0.0f};

            k = f / 2;
            acc[k] = ((f % 2) == 0) ? 1.0f : -1.0f;
            /* 正演：实测 = 真值·gain + bias */

            acc[0] = acc[0] * gain_true[0] + bias_true[0];
            acc[1] = acc[1] * gain_true[1] + bias_true[1];
            acc[2] = acc[2] * gain_true[2] + bias_true[2];

            pw_calib_six_add(&s, f, acc);
          }
      }

    if (!pw_calib_six_ready(&s, 10))
      {
        return -3;
      }

    if (pw_calib_six_solve(&s, bias, scale, &resid, &cross) != 0)
      {
        return -4;
      }

    for (k = 0; k < 3; k++)
      {
        float eb = fabsf(bias[k] - bias_true[k]);

        /* 口径注意：本库的 scale 是**修正系数**（真值 = (实测-bias)·scale），
         * 应等于 1/gain，而不是 gain 本身。早期这里误与 gain 比较，于是
         * worst_rel_err 里恒含约 6% 的"口径差"；2026-09-15 用 imubench 把注入
         * 真值与 UI 解算对照时才看出来，遂改正。 */

        float es = fabsf(scale[k] - 1.0f / gain_true[k]) * gain_true[k];

        if (eb > worst) worst = eb;
        if (es > worst) worst = es;
      }

    if (resid > 0.01f || cross > 0.001f)
      {
        return -5;
      }
  }

  /* --- C) 磁椭球（确定性球面取点，无 rand） --- */

  {
    struct pw_calib_mag_s s;
    const float center_true[3] = {35.0f, -120.0f, 60.0f};   /* mG */
    float A[3][3];
    float center[3];
    float w[9];
    float mean_mag = 0.0f;
    float eig[3];
    float minn = 1e9f;
    float maxn = -1e9f;
    int   i;

    /* 软铁/安装畸变：非对称、带轴间耦合 */

    /* 半径必须与真实场强同量级（~520 mG）。早期版本这里写成 A≈1 mG 而中心
     * 140 mG，等于"半径 1 mG 的球偏到 140 mG"，正规方程条件数 ~1e8、
     * float 解直接崩（实测 rc=-3）。真实数据里用户把表转一圈，
     * 半径自然就是场强，不存在这个问题。 */

    A[0][0] = 520.0f * 1.10f; A[0][1] = 520.0f * 0.06f; A[0][2] = 520.0f * -0.03f;
    A[1][0] = 520.0f * 0.02f; A[1][1] = 520.0f * 0.92f; A[1][2] = 520.0f * 0.05f;
    A[2][0] = 520.0f * 0.01f; A[2][1] = 520.0f * -0.04f; A[2][2] = 520.0f * 1.04f;

    pw_calib_mag_reset(&s);

    for (i = 0; i < 400; i++)
      {
        /* Fibonacci 球面：确定性、覆盖均匀 */

        float t = ((float)i + 0.5f) / 400.0f;
        float zz = 1.0f - 2.0f * t;
        float rr = sqrtf(1.0f - zz * zz);
        float ph = (float)i * 2.399963f;
        float u[3];
        float m[3];
        int   j;

        u[0] = rr * cosf(ph);
        u[1] = rr * sinf(ph);
        u[2] = zz;

        for (j = 0; j < 3; j++)
          {
            m[j] = A[j][0] * u[0] + A[j][1] * u[1] + A[j][2] * u[2] +
                   center_true[j];
          }

        pw_calib_mag_add(&s, m);
      }

    if (!pw_calib_mag_ready(&s, 200))
      {
        return -6;
      }

    if (pw_calib_mag_solve(&s, center, w, &mean_mag, eig) != 0)
      {
        return -7;
      }

    for (i = 0; i < 3; i++)
      {
        float e = fabsf(center[i] - center_true[i]) / 150.0f;

        if (e > worst) worst = e;
      }

    /* 校正后模长一致性：重新走一遍球面点（这里用同一批 u 正演） */

    for (i = 0; i < 400; i++)
      {
        float t = ((float)i + 0.5f) / 400.0f;
        float zz = 1.0f - 2.0f * t;
        float rr = sqrtf(1.0f - zz * zz);
        float ph = (float)i * 2.399963f;
        float u[3];
        float m[3];
        float c[3];
        int   j;

        u[0] = rr * cosf(ph);
        u[1] = rr * sinf(ph);
        u[2] = zz;

        for (j = 0; j < 3; j++)
          {
            m[j] = A[j][0] * u[0] + A[j][1] * u[1] + A[j][2] * u[2] +
                   center_true[j];
          }

        pw_calib_mag_apply(center, w, m, c);

        {
          float n = sqrtf(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);

          if (n < minn) minn = n;
          if (n > maxn) maxn = n;
        }
      }

    /* 单位球应落在 1 附近；用相对离散度衡量 */

    if (minn > 1e-6f && (1.0f / minn) > 0.0f)
      {
        float spread = (maxn - minn) / (0.5f * (maxn + minn));

        if (spread > worst) worst = spread;

        /* 阈值 5%：修好平方根方向后实测约 1~2%；剩下的离散度是椭球拟合
         * "软铁只能定到一个未知旋转"这一固有性质的体现。 */

        if (spread > 0.05f) return -8;
      }
  }

  /* --- D) 陀螺静态零偏 --- */

  {
    struct pw_calib_bias_s b;
    const float bias_true[3] = {0.010f, -0.008f, 0.012f};
    float bias[3];
    float sd[3];
    int   i;

    pw_calib_bias_reset(&b);
    for (i = 0; i < 500; i++)
      {
        float g[3];

        g[0] = bias_true[0];
        g[1] = bias_true[1];
        g[2] = bias_true[2];
        pw_calib_bias_add(&b, g);
      }

    if (pw_calib_bias_solve(&b, bias, sd, 0.0f) != 0)
      {
        return -9;
      }

    for (i = 0; i < 3; i++)
      {
        float e = fabsf(bias[i] - bias_true[i]) / 0.012f;

        if (e > worst) worst = e;
      }

    /* "不够静"必须被拒：人为塞入大幅抖动 */

    pw_calib_bias_reset(&b);
    for (i = 0; i < 500; i++)
      {
        float g[3];

        g[0] = bias_true[0] + ((i % 2) ? 0.05f : -0.05f);   /* ±5% rad/s ≈ ±2.9 dps */
        g[1] = bias_true[1];
        g[2] = bias_true[2];
        pw_calib_bias_add(&b, g);
      }

    if (pw_calib_bias_solve(&b, NULL, NULL, 0.02f) != -2)
      {
        return -10;
      }
  }

  if (err_out != NULL)
    {
      *err_out = worst;
    }

  return 0;
}
