/****************************************************************************
 * apps/examples/phywear/pw_ahrs.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * PhyWear 姿态解算（AHRS）：Mahony 显式互补滤波（MARG，9 轴）。
 * 纯 C、无 NuttX/LVGL 依赖、无动态内存、无查表 —— 可主机单测。
 *
 * 为什么选 Mahony 而不是 Madgwick/EKF（见 docs/03 §4.8 的设计说明）：
 *   1) 它把"陀螺零偏"变成**显式的积分状态**，能把零偏估出来并报出来 ——
 *      本项目要的正是"零偏可标定、可复现"，EKF 要 15 维状态且需要建模噪声；
 *   2) 算力最低：约 100 flop/次，50Hz 下占 MCU 不到 0.1%；
 *   3) 磁力计只修偏航：磁残差按权重并入误差项，权重默认 0.2，并可限速。
 *
 * 状态只有 76 B（四元数 16 + 零偏 12 + 增益/计时），不新增任何缓冲。
 *
 * 坐标与单位约定（与 phywear_sensors.h 对齐，调用方负责换算）：
 *   加速度 acc_g[] : g      （传感器 mg / 1000）
 *   角速度 gyr[]   : rad/s  （传感器 mdps × 1.745329e-5）
 *   磁力计 mag[]   : 任意一致单位（传感器 mG 直接可用）
 *   四元数 q[4]    : body → world，(w,x,y,z)，world 为东北天(ENU)、Z 向上
 *   欧拉角         : 度，roll/pitch/yaw（ZYX，yaw 仅在有磁修正时才有绝对意义）
 ****************************************************************************/

#ifndef __PHYWEAR_PW_AHRS_H
#define __PHYWEAR_PW_AHRS_H

#ifdef __cplusplus
extern "C"
{
#endif

/****************************************************************************
 * 默认参数（真机用真值标定过再改；见 docs/03 §4.8 的实验设计）
 *
 *   KP     0.5..1.0   太小收敛慢，太大放大加速度噪声
 *   KI     0.01..0.05 负责把陀螺零偏估出来；过大在振动下会把真实角速度当零偏
 *   WMAG   0.1..0.3   磁只治偏航，权重给大了会把倾斜也拽歪
 *   MAG_HZ 10..20     磁修正限速：磁力计本来就不需要每次都参与
 ****************************************************************************/

#define PW_AHRS_KP       1.5f
#define PW_AHRS_KI       0.02f
#define PW_AHRS_WMAG     0.5f
#define PW_AHRS_KIMAG    0.0f    /* 磁项默认**不进积分**：磁噪声被积进零偏估计
                                  * 会让偏航缓慢游走（实测 σ_yaw 2.7°→12°），
                                  * 磁只做比例修正即可把偏航拉回来 */
#define PW_AHRS_MAG_HZ   0.0f    /* 0 = 每次更新都用磁（权重已经很小，不必再占空比限速；
                                  * 占空比限速会让冷启动收敛慢一个数量级） */

/* 加速度小于该值（g）视为"不可信"，本次不参与重力修正（自由落体/强振） */

#define PW_AHRS_ACC_MIN  0.30f

struct pw_ahrs_s
{
  float q[4];        /* 姿态四元数 body→world */
  float bias[3];     /* 陀螺零偏估计（rad/s） */
  float kp;          /* 比例增益 */
  float ki;          /* 积分增益 */
  float wmag;        /* 磁修正权重 0..1（0 = 纯 6 轴） */
  float kimag;       /* 磁项进入零偏积分的比例 0..1（默认 0） */
  float mag_dt;      /* 磁修正最小间隔（s），0 = 每次都修 */
  float mag_acc;     /* 磁修正计时累加 */
  int   n;           /* 已更新次数 */
  int   n_mag;       /* 磁修正参与次数 */
  int   n_rej;       /* 加速度被拒绝次数 */
};

/* 初始化（用默认增益）与复位（保留增益，清姿态/零偏）。安全传 NULL。 */

void pw_ahrs_init(struct pw_ahrs_s *a);
void pw_ahrs_reset(struct pw_ahrs_s *a);

/* 一次更新。acc_g/gyr 必须非空；mag 可为 NULL（退化为 6 轴）。
 * dt 为本次与上次之间的真实间隔（秒），必须 > 0（用真实 dt，不用固定标称值）。 */

void pw_ahrs_update(struct pw_ahrs_s *a, const float *acc_g, const float *gyr,
                    const float *mag, float dt);

/* 输出。调用方各自带缓冲，模块内部不返回指针。 */

void pw_ahrs_quat(const struct pw_ahrs_s *a, float *q4);
void pw_ahrs_euler(const struct pw_ahrs_s *a, float *roll_deg,
                   float *pitch_deg, float *yaw_deg);
void pw_ahrs_bias(const struct pw_ahrs_s *a, float *bias_rps);

/* 自检：用合成数据验证"从错误初值收敛到已知姿态 + 估出已知零偏"。
 * 成功返回 0，失败返回负值；err 若非空则写回最大姿态误差（度）。 */

int pw_ahrs_selftest(float *err_deg);

#ifdef __cplusplus
}
#endif

#endif /* __PHYWEAR_PW_AHRS_H */
