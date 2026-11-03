/****************************************************************************
 * apps/examples/phywear/pw_traj.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * PhyWear 惯性轨迹（相对位移）：姿态 → 世界系去重力 → ZUPT → 梯形积分。
 * 纯 C、无 NuttX/LVGL 依赖、无动态内存 —— 可主机单测。
 *
 * ⚠️ 能力边界（必须与界面上的标注一致，不许夸大）：
 *   本模块给的是**以"起点归零"为原点的相对位移**，**不是全局定位**。
 *   消费级 IMU 的二重积分误差随时长增长：
 *     加速度零偏 ε  → 位置误差 ≈ ½εt²   （ε=1 mg → 10 s 约 49 cm、60 s 约 17.6 m）
 *     姿态失准 θ    → 假重力 g·sinθ 灌进位置（1° 失准 ≈ 17.5 mg）
 *     陀螺零偏 b    → 位置误差 ≈ g·b·t³/6（b=0.1°/s → 10 s 约 2.85 m）
 *   所以只有**短窗（1~2 s）+ 两端静止（ZUPT 锚定）**的相对位移可信（mm~cm 量级）；
 *   静止段速度强制归零，是本模块唯一能压制漂移的手段。
 *
 * 坐标与单位（与 pw_ahrs 对齐，调用方负责换算）：
 *   acc_g[]  : g      （传感器 mg/1000）
 *   gyro_rps[]: rad/s （传感器 mdps × 1.745329e-5）
 *   q[4]     : body→world 四元数（直接来自 pw_ahrs，world 为 ENU、Z 向上）
 *   输出 p[3] : 米（相对"起点归零"时刻）
 ****************************************************************************/

#ifndef __PHYWEAR_PW_TRAJ_H
#define __PHYWEAR_PW_TRAJ_H

#ifdef __cplusplus
extern "C"
{
#endif

/* 静止判据（ZUPT）—— 三条同时满足才判"静止"，全部在**滑窗**上统计：
 *   ① |mean|a| − 1| < 50 mg     （比力模长接近重力）
 *   ② mean|ω| < 3 °/s          （角速度小）
 *   ③ std(|a|) < 10 mg         （比力**波动**小）
 *
 * ⚠️ ③ 是必须的，而且是踩过坑才加的：只判 ① 时，**水平推力几乎不改变比力模长**
 *   （2 m/s² 的推只让模长变 20 mg，小于 50 mg 阈值）→ 推手被误判成静止 → ZUPT 把
 *   速度一路清零 → **轨迹永远不动**（主机单测 rc=-1 抓到的就是这个）。
 *   本机主测例（`pw_traj_selftest`）因此改成"整周期正弦推力（推出去再停下，v(T)=0）
 *   + 手腕转动"的真实形态，并且前置 0.4 s 静置（真实推手也是先静置再推）。
 *
 * 物理边界（写在文档里，界面上也标注）：**恒定且纯平移的加速度**在 IMU 上与"倾斜"
 *   不可区分（这是单加速度计的固有不唯一性），所以那种理想化的运动本模块不保证能测到。
 */

/* 输入有效性门（**物理**门，不是调参）：比力模长必须落在 [0.3, 2.5] g。
 * 为什么必须有：真机实测（2026-09-15 轨迹页首版）抓到 IMU 驱动刚打开时会先给
 * 若干拍零/无效值，|a|=0 —— 在物理上那是"自由落体"，会被积成 -g 的竖直加速度，
 * 1.4 s 就把速度推到 9 m/s、Z 推到 -2.2 m。手推手表不可能让 |a| 掉到 0.3 g 以下，
 * 所以这类样本一律丢弃（不积分、不进滑窗、不改状态）。 */

#define PW_TRAJ_ACC_MIN     0.30f        /* g */
#define PW_TRAJ_ACC_MAX     2.50f        /* g */

#define PW_TRAJ_STILL_AG    0.06f        /* 60 mg（真机实测静止时 |a| 在 1.010~1.018 g） */
#define PW_TRAJ_STILL_W     0.0524f      /* 3 °/s。不要因为「真机角速度偏大」就放宽它：
                                          * 真机偏大是陀螺零偏（实测 2.48 dps），由零偏改正解决；
                                          * 一旦放宽到 6 dps，8 dps 的手腕转动就漏检（主机单测 rc=-1）。 */
#define PW_TRAJ_STILL_ASTD  0.045f       /* 比力模长滑动标准差 45 mg。
                                          * 原来按"实验室噪声"设 10 mg，真机偶发 ~0.11 g 的模长野值
                                          * 会把 10 点窗标准差顶到 ~36 mg，于是静止被判成运动、
                                          * 零偏被一路积分（实测 9 s 漂 1.3 m）。45 mg 既容得下野值，
                                          * 又拦得住真运动（真运动的角速度判据本来就先触发）。 */
#define PW_TRAJ_STILL_DAG   0.030f       /* 比力模长**一拍差分** 30 mg —— 运动起始检测：
                                          * 手推 2 m/s² 整周期正弦，起始斜率 15.7 m/s²/s，
                                          * 20 ms 内模长即变化 ~32 mg，第一拍就能抓到，
                                          * 而滑窗均值要滞后 0.2 s。静止时它也只受噪声驱动。 */
#define PW_TRAJ_WIN         10           /* 滑窗 10 拍（50Hz 下 0.2 s） */
#define PW_TRAJ_STILL_ENTER 5            /* 连续 5 拍满足才进入静止（进静止慢：宁可少 ZUPT，不可误 ZUPT） */
#define PW_TRAJ_STILL_EXIT  3            /* 连续 3 拍不满足才退出（滞回防抖） */
#define PW_TRAJ_MOVE_SCORE  4            /* 静止→运动：票数够 4 分才退出（**两速**，见下）。
                                          * 滑窗均值天然滞后 0.2 s，只用滑窗判据会把"起步"那 0.2 s
                                          * 的速度当静止抹掉（实测少 40% 位移）；但只看瞬时又会被
                                          * 单点野值骗（真机实测偶发 0.11 g 模长跳变）。
                                          * 于是分权：角速度超限 = 真转动，记 2 分（2 拍即出静止）；
                                          * 仅比力超限 = 可能是野值，记 1 分（要 4 拍才出）。
                                          * 野值只持续 2 拍 → 2 分 < 4，不会误退出。 */

/* 参考重力：默认 9.81；可用实测值覆盖（例如单摆页测得的 g_local） */

#define PW_TRAJ_G_DEFAULT   9.80665f

struct pw_traj_s
{
  float p[3];          /* 相对位移（m），以归零时刻为原点 */
  float v[3];          /* 速度（m/s） */
  float a_prev[3];     /* 上一拍世界系线加速度（梯形积分用） */
  float t;             /* 自归零以来累计时间（s） */
  float vmax;          /* 期间最大速度（诊断"到底动没动"） */
  int   n;             /* 更新次数 */
  int   n_zupt;        /* 累计 ZUPT 次数（静止段计数） */
  int   n_bad;         /* 被有效性门丢弃的样本数（驱动启动期/异常） */
  int   dbg_c1;        /* 诊断：判据①（|a| 均值偏离 1 g）失败次数 */
  int   dbg_c2;        /* 诊断：判据②（角速度均值偏大）失败次数 */
  int   dbg_c3;        /* 诊断：判据③（|a| 标准差偏大）失败次数 */
  int   still;         /* 当前是否判为静止 */
  int   still_cnt;     /* 连续静止拍数 */
  int   move_cnt;      /* 连续运动拍数 */
  float g_ref;         /* 参考重力（m/s²） */
  float win_a[PW_TRAJ_WIN];   /* 滑窗：比力模长（g） */
  float win_w[PW_TRAJ_WIN];   /* 滑窗：角速度模长（rad/s） */
  int   win_n;
  int   win_head;
};

/* 初始化（g_ref <= 0 时取默认 9.80665） */

void pw_traj_init(struct pw_traj_s *t, float g_ref);

/* 起点归零：位置/速度清零、时间归零（**只归零，不修漂移**） */

void pw_traj_reset(struct pw_traj_s *t);

/* 一次更新。q 为 body→world 四元数（来自 pw_ahrs），dt 必须为真实间隔（秒）。
 * 姿态为零向量时按"单位姿态"处理（仅用于自检/降级）。 */

void pw_traj_update(struct pw_traj_s *t, const float *acc_g,
                    const float *gyro_rps, const float *q, float dt);

/* 读位置（m）与状态 */

void pw_traj_pos(const struct pw_traj_s *t, float *xyz);
float pw_traj_time(const struct pw_traj_s *t);
int   pw_traj_is_still(const struct pw_traj_s *t);

/* 自检：合成"加速 0.4s → 减速 0.4s → 静止 1s"的推手动作，检查相对位移能否还原
 * （真值 0.32 m），并在注入 1 mg 零偏后检查 ZUPT 是否把误差压在厘米级。
 * 成功返回 0；err_m 非空时写回最大绝对误差（m）。 */

int pw_traj_selftest(float *err_m);

#ifdef __cplusplus
}
#endif

#endif /* __PHYWEAR_PW_TRAJ_H */
