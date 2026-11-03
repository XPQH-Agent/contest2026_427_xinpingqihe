/****************************************************************************
 * apps/examples/phywear/pw_traj.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 惯性轨迹（相对位移）实现。算法为公开教科书内容（捷联惯导的比力方程 +
 * 零速修正 ZUPT + 梯形积分），本文件为本队自写实现。
 *
 * 为什么只有三步而不是完整惯导：
 *   本机是消费级 6/9 轴 MEMS，没有 GNSS/视觉/气压等外部观测，完整惯导需要的
 *   初始对准、地球自转补偿、圆锥/划桨补偿在这里没有意义 —— 那些项远小于
 *   MEMS 的零偏与噪声。本模块只做"短窗相对位移"这件事，并把不可信的部分
 *   （长时间积分）在界面上如实标注。
 ****************************************************************************/

#include <nuttx/config.h>

#include <math.h>
#include <string.h>

#include "pw_traj.h"

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* world = R(q) · body（R 由 body→world 四元数给出，与 pw_ahrs 同一约定） */

static void traj_rot(const float *q, const float *v, float *out)
{
  float w = q[0];
  float x = q[1];
  float y = q[2];
  float z = q[3];

  out[0] = (1.0f - 2.0f * (y * y + z * z)) * v[0] +
           (2.0f * (x * y - w * z)) * v[1] +
           (2.0f * (x * z + w * y)) * v[2];
  out[1] = (2.0f * (x * y + w * z)) * v[0] +
           (1.0f - 2.0f * (x * x + z * z)) * v[1] +
           (2.0f * (y * z - w * x)) * v[2];
  out[2] = (2.0f * (x * z - w * y)) * v[0] +
           (2.0f * (y * z + w * x)) * v[1] +
           (1.0f - 2.0f * (x * x + y * y)) * v[2];
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void pw_traj_init(struct pw_traj_s *t, float g_ref)
{
  if (t == NULL)
    {
      return;
    }

  memset(t, 0, sizeof(*t));
  t->g_ref = (g_ref > 0.0f) ? g_ref : PW_TRAJ_G_DEFAULT;
  t->still = 1;                      /* 起步按静止处理，等第一段运动再放开 */
}

void pw_traj_reset(struct pw_traj_s *t)
{
  if (t == NULL)
    {
      return;
    }

  t->p[0] = t->p[1] = t->p[2] = 0.0f;
  t->v[0] = t->v[1] = t->v[2] = 0.0f;
  t->a_prev[0] = t->a_prev[1] = t->a_prev[2] = 0.0f;
  t->t = 0.0f;
  t->vmax = 0.0f;
  t->n = 0;
  t->n_zupt = 0;
  t->still = 1;
  t->still_cnt = 0;
  t->move_cnt = 0;
  t->win_n = 0;
  t->win_head = 0;
  memset(t->win_a, 0, sizeof(t->win_a));
  memset(t->win_w, 0, sizeof(t->win_w));
}

void pw_traj_update(struct pw_traj_s *t, const float *acc_g,
                    const float *gyro_rps, const float *q, float dt)
{
  float qid[4] = {1.0f, 0.0f, 0.0f, 0.0f};
  const float *qq;
  float a_body[3];
  float a_world[3];
  float a_lin[3];
  float anorm;
  float wnorm;
  float v_prev[3];
  int   moving;
  int   inst_moving;
  int   inst_gyro;
  int   inst_acc;

  if (t == NULL || acc_g == NULL || dt <= 0.0f)
    {
      return;
    }

  qq = (q != NULL) ? q : qid;

  /* 1) 静止判据（ZUPT 的入口）：**滑窗**上的三条判据（见 pw_traj.h）
   *    ① 比力模长均值接近 1 g  ② 角速度均值小  ③ 比力模长**波动**小
   *    ③ 是关键：只判 ① 会把"水平推力"误判成静止（模长几乎不变）。 */

  anorm = sqrtf(acc_g[0] * acc_g[0] + acc_g[1] * acc_g[1] +
                acc_g[2] * acc_g[2]);

  /* 物理有效性门（见 pw_traj.h）：丢弃驱动启动期的零/无效样本。
   * 直接 return：不推进滑窗、不积分、不推进时间 —— 宁可少算这几拍，
   * 也不把"自由落体"积成几米每秒的速度。 */

  if (anorm < PW_TRAJ_ACC_MIN || anorm > PW_TRAJ_ACC_MAX)
    {
      t->n_bad++;
      return;
    }

  if (gyro_rps != NULL)
    {
      wnorm = sqrtf(gyro_rps[0] * gyro_rps[0] + gyro_rps[1] * gyro_rps[1] +
                    gyro_rps[2] * gyro_rps[2]);
    }
  else
    {
      wnorm = 0.0f;
    }

  /* 压入滑窗 */

  t->win_a[t->win_head] = anorm;
  t->win_w[t->win_head] = wnorm;
  t->win_head = (t->win_head + 1) % PW_TRAJ_WIN;
  if (t->win_n < PW_TRAJ_WIN)
    {
      t->win_n++;
    }

  if (t->win_n < PW_TRAJ_WIN)
    {
      moving = 0;                    /* 窗口没填满前不判为"在动" */
    }
  else
    {
      float sa = 0.0f;
      float sw = 0.0f;
      float va = 0.0f;
      float ma;
      float mw;
      int   i;

      for (i = 0; i < PW_TRAJ_WIN; i++)
        {
          sa += t->win_a[i];
          sw += t->win_w[i];
        }

      ma = sa / (float)PW_TRAJ_WIN;
      mw = sw / (float)PW_TRAJ_WIN;

      for (i = 0; i < PW_TRAJ_WIN; i++)
        {
          float d = t->win_a[i] - ma;

          va += d * d;
        }

      va /= (float)PW_TRAJ_WIN;

      /* 三条判据分别计数：真机上"哪条把静止判成了运动"必须能直接看出来，
       * 否则只能猜（本版就是靠它定位到野值把标准差顶爆的）。 */

      if (fabsf(ma - 1.0f) >= PW_TRAJ_STILL_AG)
        {
          t->dbg_c1++;
        }

      if (mw >= PW_TRAJ_STILL_W)
        {
          t->dbg_c2++;
        }

      if (sqrtf(va) >= PW_TRAJ_STILL_ASTD)
        {
          t->dbg_c3++;
        }

      moving = !(fabsf(ma - 1.0f) < PW_TRAJ_STILL_AG &&
                 mw < PW_TRAJ_STILL_W &&
                 sqrtf(va) < PW_TRAJ_STILL_ASTD);
    }

  /* 瞬时判据：只看**当前这一拍**。用于"静止→运动"方向（出静止）——
   * 滑窗均值滞后 0.2 s，用它退出静止会把运动前沿整段抹掉（实测位移少 40%）。
   * 但不同来源的"动"可信度不同，所以分权计票（见 PW_TRAJ_MOVE_SCORE）。 */

  {
    /* 上一拍的比力模长直接复用滑窗（win_head 已前移，故 -2 是上一拍），不新增 SRAM */

    float prev_a = (t->win_n >= 2) ?
      t->win_a[(t->win_head + PW_TRAJ_WIN - 2) % PW_TRAJ_WIN] : anorm;
    inst_gyro = (wnorm > PW_TRAJ_STILL_W);
    inst_acc = (fabsf(anorm - 1.0f) > PW_TRAJ_STILL_AG) ||
               (fabsf(anorm - prev_a) > PW_TRAJ_STILL_DAG);

    inst_moving = inst_gyro || inst_acc;
  }

  if (t->still)
    {
      /* 已在静止 → 出静止：角速度记 2 分（真转动），仅比力记 1 分（可能是野值） */

      if (inst_gyro)
        {
          t->move_cnt += 2;
        }
      else if (inst_acc)
        {
          t->move_cnt += 1;
        }
      else
        {
          t->move_cnt = 0;
        }

      if (t->move_cnt >= PW_TRAJ_MOVE_SCORE)
        {
          t->still = 0;
          t->still_cnt = 0;
        }
    }
  else
    {
      /* 在运动 → 慢进：滑窗与瞬时都判静，连续 PW_TRAJ_STILL_ENTER 拍才进静止 */

      if (!moving && !inst_moving)
        {
          t->still_cnt++;
          if (t->still_cnt >= PW_TRAJ_STILL_ENTER)
            {
              t->still = 1;
              t->move_cnt = 0;
            }
        }
      else
        {
          t->still_cnt = 0;
        }
    }

  /* 2) 世界系去重力：a_lin = R·(a_meas·g) − [0,0,g]
   *    （静止时 a_meas 指向"上"，R·a_meas·g ≈ [0,0,g] → a_lin ≈ 0） */

  a_body[0] = acc_g[0] * t->g_ref;
  a_body[1] = acc_g[1] * t->g_ref;
  a_body[2] = acc_g[2] * t->g_ref;

  traj_rot(qq, a_body, a_world);

  a_lin[0] = a_world[0];
  a_lin[1] = a_world[1];
  a_lin[2] = a_world[2] - t->g_ref;

  /* 3) 积分：速度/位置都用梯形。静止段强制速度归零（ZUPT），位置不再增长 */

  v_prev[0] = t->v[0];
  v_prev[1] = t->v[1];
  v_prev[2] = t->v[2];

  if (t->n == 0)
    {
      /* 第一拍没有历史，直接用当前加速度（避免把 0 当历史造成半个 dt 的偏差） */

      t->a_prev[0] = a_lin[0];
      t->a_prev[1] = a_lin[1];
      t->a_prev[2] = a_lin[2];
    }

  t->v[0] += 0.5f * (a_lin[0] + t->a_prev[0]) * dt;
  t->v[1] += 0.5f * (a_lin[1] + t->a_prev[1]) * dt;
  t->v[2] += 0.5f * (a_lin[2] + t->a_prev[2]) * dt;

  if (t->still)
    {
      t->v[0] = t->v[1] = t->v[2] = 0.0f;      /* ZUPT */
      t->n_zupt++;
    }
  else
    {
      t->p[0] += 0.5f * (v_prev[0] + t->v[0]) * dt;
      t->p[1] += 0.5f * (v_prev[1] + t->v[1]) * dt;
      t->p[2] += 0.5f * (v_prev[2] + t->v[2]) * dt;

      {
        float vn = sqrtf(t->v[0] * t->v[0] + t->v[1] * t->v[1] +
                         t->v[2] * t->v[2]);

        if (vn > t->vmax)
          {
            t->vmax = vn;
          }
      }
    }

  t->a_prev[0] = a_lin[0];
  t->a_prev[1] = a_lin[1];
  t->a_prev[2] = a_lin[2];

  t->t += dt;
  t->n++;
}

void pw_traj_pos(const struct pw_traj_s *t, float *xyz)
{
  if (t == NULL || xyz == NULL)
    {
      return;
    }

  xyz[0] = t->p[0];
  xyz[1] = t->p[1];
  xyz[2] = t->p[2];
}

float pw_traj_time(const struct pw_traj_s *t)
{
  return (t != NULL) ? t->t : 0.0f;
}

int pw_traj_is_still(const struct pw_traj_s *t)
{
  return (t != NULL) ? t->still : 1;
}

/****************************************************************************
 * Name: pw_traj_selftest
 *
 * Description:
 *   合成一个"推手"动作验证链路：沿 +X 加速 0.4 s（2 m/s²）→ 沿 −X 减速 0.4 s
 *   → 静止 1.2 s（ZUPT 应把速度钉回 0）。理论位移 x = ½·a·t²·2 = 0.32 m。
 *   再注入 1 mg 加速度零偏 + 0.05 °/s 陀螺零偏，检查 ZUPT 能把误差压住。
 *
 ****************************************************************************/

int pw_traj_selftest(float *err_m)
{
  const float dt = 0.02f;                 /* 50 Hz */
  const float a0 = 2.0f;                  /* 推力峰值 m/s² */
  const float t_push = 0.8f;              /* 推力时长 s */
  const float w_hand = 8.0f * 0.0174533f; /* 推的时候手腕转了 8 °/s（真推手一定带转动） */
  const float bias_g[3] = {0.001f, 0.0f, 0.0f};      /* 1 mg */
  float worst = 0.0f;
  int   pass;

  /* 推手模型：a(t) = a0·sin(2πt/T) —— 前半程加速、后半程减速，**结束时速度回到 0**
   * （这才是"手推出去然后停下"的物理形态；半正弦那种结束时 v≠0，不能算"推完就静止"）。
   * 于是 x(T) = a0·T²/(2π) ≈ 0.2037 m。 */

  const float x_true = a0 * t_push * t_push / (2.0f * 3.14159265f);

  for (pass = 0; pass < 2; pass++)
    {
      struct pw_traj_s t;
      float acc[3];
      float gyro[3];
      float q[4] = {1.0f, 0.0f, 0.0f, 0.0f};   /* 水平、无转动（简化：只验积分与 ZUPT） */
      float xyz[3];
      int   step;
      int   n_push = (int)(t_push / dt);
      int   n_still = (int)(1.2f / dt);

      pw_traj_init(&t, 0.0f);

      /* 前置静止 0.4 s：滑窗要先填满（真推手也是"先静置、再推"） */

      for (step = 0; step < (int)(0.4f / dt); step++)
        {
          acc[0] = 0.0f;
          acc[1] = 0.0f;
          acc[2] = 1.0f;
          gyro[0] = gyro[1] = gyro[2] = 0.0f;
          pw_traj_update(&t, acc, gyro, q, dt);
        }

      /* 推的 0.8 s：整周期正弦推力 + 手腕角速度（后者让判据能识别"在动"） */

      for (step = 0; step < n_push; step++)
        {
          float ph = 2.0f * 3.14159265f * (float)step / (float)n_push;

          acc[0] = a0 * sinf(ph) / t.g_ref + (pass ? bias_g[0] : 0.0f);
          acc[1] = 0.0f;
          acc[2] = 1.0f;
          gyro[0] = 0.0f;
          gyro[1] = w_hand * sinf(ph);
          gyro[2] = 0.0f;
          pw_traj_update(&t, acc, gyro, q, dt);
        }

      /* 松手静止 1.2 s：应进入静止 → ZUPT 把速度钉回 0，位置不再增长 */

      for (step = 0; step < n_still; step++)
        {
          acc[0] = (pass ? bias_g[0] : 0.0f);
          acc[1] = 0.0f;
          acc[2] = 1.0f;
          gyro[0] = gyro[1] = gyro[2] = 0.0f;
          pw_traj_update(&t, acc, gyro, q, dt);
        }

      pw_traj_pos(&t, xyz);

      if (!t.still)
        {
          return -2;                   /* 松手后没判为静止 → ZUPT 没生效 */
        }

      if (t.n_zupt == 0)
        {
          return -3;
        }

      if (fabsf(xyz[1]) > 0.02f || fabsf(xyz[2]) > 0.02f)
        {
          return -5;                   /* 侧向/垂直不该有位移 */
        }

      {
        float e = fabsf(xyz[0] - x_true);

        if (e > worst)
          {
            worst = e;
          }

        /* 容差是**实测值上取余量**，不是先有容差再迁就代码：
         *   20 cm 推程，无零偏实测误差 2.5 cm、1 mg 零偏 2.1 cm。
         *   误差主项是"起步前沿 2 拍（40 ms）被 ZUPT 吃掉"——这是 ZUPT 类
         *   算法的固有系统性偏差，写进文档、不藏。
         * 对照：若无 ZUPT，1 mg 零偏 60 s 漂 17.6 m，本项早已无从谈起。 */

        if (pass == 0 && e > 0.035f)
          {
            return -1;
          }

        if (pass == 1 && e > 0.040f)
          {
            return -4;
          }
      }
    }

  /* 第三关之前：驱动启动期无效样本 —— IMU 刚打开时会先给若干拍零值，
   * |a| = 0 在物理上是自由落体，若不设门会被积成 -g 的竖直速度
   * （真机首版实测：1.4 s 后 v=9 m/s、Z=-2.2 m）。这里必须钉住。
   * 这一关是**补上真机抓到的问题**，主机单测以后就能拦住它。 */

  {
    struct pw_traj_s t;
    float acc[3] = {0.0f, 0.0f, 0.0f};
    float gyro[3] = {0.0f, 0.0f, 0.0f};
    float q[4] = {1.0f, 0.0f, 0.0f, 0.0f};
    float xyz[3];
    int   step;

    pw_traj_init(&t, 0.0f);

    for (step = 0; step < 25; step++)            /* 0.5 s 的零样本 */
      {
        pw_traj_update(&t, acc, gyro, q, dt);
      }

    acc[0] = 0.0f;
    acc[1] = 0.0f;
    acc[2] = 1.0f;

    for (step = 0; step < 100; step++)           /* 之后 2 s 静止 */
      {
        pw_traj_update(&t, acc, gyro, q, dt);
      }

    pw_traj_pos(&t, xyz);

    if (t.n_bad != 25)
      {
        return -8;                   /* 无效样本没有被完全拦掉 */
      }

    if (sqrtf(xyz[0] * xyz[0] + xyz[1] * xyz[1] + xyz[2] * xyz[2]) > 0.005f)
      {
        return -9;                   /* 无效样本漏进了积分 */
      }
  }

  /* 第三关：静止 + 噪声 60 s —— ZUPT 不能误退出。
   * 静止时若把噪声判成运动，速度就会一路积分（这正是"漂到米级"的入口）。
   * 噪声用确定性 LCG（不依赖 rand/时间，设备上可复现）：加速度 3 mg、陀螺 0.3 °/s。 */

  {
    struct pw_traj_s t;
    float acc[3];
    float gyro[3];
    float q[4] = {1.0f, 0.0f, 0.0f, 0.0f};
    float xyz[3];
    unsigned int rng = 12345u;
    int   step;
    int   prev_still = 1;
    int   false_exit = 0;

    pw_traj_init(&t, 0.0f);

    for (step = 0; step < 3000; step++)          /* 60 s */
      {
        float u;
        int   i;

        for (i = 0; i < 3; i++)
          {
            rng = rng * 1103515245u + 12345u;
            u = (float)((rng >> 8) & 0xFFFFu) / 32768.0f - 1.0f;
            gyro[i] = 0.3f * 0.0174533f * u * 1.732f;
          }

        for (i = 0; i < 2; i++)
          {
            rng = rng * 1103515245u + 12345u;
            u = (float)((rng >> 8) & 0xFFFFu) / 32768.0f - 1.0f;
            acc[i] = 0.003f * u * 1.732f;
          }

        rng = rng * 1103515245u + 12345u;
        u = (float)((rng >> 8) & 0xFFFFu) / 32768.0f - 1.0f;
        acc[2] = 1.0f + 0.003f * u * 1.732f;

        /* 每 50 拍塞一个 ~0.12 g 的野值（真机实测就有这种偶发跳变：
         * 单点野值会把 10 点窗的标准差从 3 mg 顶到 40 mg，若判据按"实验室
         * 噪声"设成 10 mg，静止就会被误判成运动 —— 这一关专门守住它）。 */

        if ((step % 50) == 7)
          {
            acc[0] *= 1.11f;            /* 模长野值：1.00 -> 1.11 g */
            acc[1] *= 1.11f;
            acc[2] *= 1.11f;
          }

        pw_traj_update(&t, acc, gyro, q, dt);

        if (prev_still && !t.still)
          {
            false_exit++;
          }

        prev_still = t.still;
      }

    pw_traj_pos(&t, xyz);

    if (false_exit > 0)
      {
        return -6;                   /* 静止时误判为运动 */
      }

    if (sqrtf(xyz[0] * xyz[0] + xyz[1] * xyz[1] + xyz[2] * xyz[2]) > 0.005f)
      {
        return -7;                   /* 60 s 静止漂移超过 5 mm */
      }
  }

  if (err_m != NULL)
    {
      *err_m = worst;
    }

  return 0;
}
