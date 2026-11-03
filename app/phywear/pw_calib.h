/****************************************************************************
 * apps/examples/phywear/pw_calib.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * PhyWear 标定数学库（纯 C、无平台依赖、可主机单测、无动态内存）。
 *
 * 设计要点 —— **全部用流式充分统计（sufficient statistics），不保存原始样本**：
 *   六面法只存 6 组累加；二维拟合只存一个 3×3 正规方程 + 两条右端项；
 *   磁椭球拟合只存一个 9×9 正规方程 + 一条右端项（90 个 float = 360 B）。
 *   因此标定过程的 SRAM 占用与采样点数**无关**，可以边采边算、跑几个小时也不涨。
 *
 * 真值从哪来（这决定了结论能不能叫"标定"）：
 *   六面法   ：真值是**重力方向**（理想读数 ±1 g）—— 不依赖任何外部仪器；
 *   二维位移 ：真值是**尺子/卷尺读数**（用户输入的已知距离）；
 *   磁        ：真值是**当地 WMM/IGRF 的 |B| 与倾角**，不是"典型 50 µT"。
 *   没有任何真值装置时，本库给不出"标定后精度"，只能给"重复性"。
 *
 * 单位约定：加速度一律 **g**（传感器 mg/1000）；磁场单位任意一致即可
 * （椭圆拟合只需一致性，椭球内部按 mG/1000 缩放以改善条件数）。
 ****************************************************************************/

#ifndef __PHYWEAR_PW_CALIB_H
#define __PHYWEAR_PW_CALIB_H

#ifdef __cplusplus
extern "C"
{
#endif

/****************************************************************************
 * 一维线性拟合：y = a·x + b（例：实测位移 → 真实距离）
 ****************************************************************************/

struct pw_calib_fit1d_s
{
  float sx2;
  float sx;
  float s1;
  float sxy;
  float sy;
  float sy2;
  int   n;
};

void pw_calib_fit1d_reset(struct pw_calib_fit1d_s *f);
void pw_calib_fit1d_add(struct pw_calib_fit1d_s *f, float x, float y);

/* 输出斜率 a、截距 b、残差 RMS（与被拟合量同单位）、R²。
 * 返回 0 成功；-1 样本不足（<2）或 x 无变化。 */

int pw_calib_fit1d_solve(const struct pw_calib_fit1d_s *f, float *a, float *b,
                         float *rms, float *r2);

/****************************************************************************
 * 二维线性拟合：[u,v] = M·[x,y] + t（例：二维轨迹/位移的比例+旋转+平移）
 *
 * 只解 6 个参数（4 个线性项 + 2 个平移），两个输出共用同一个 3×3 正规方程。
 ****************************************************************************/

struct pw_calib_fit2d_s
{
  float sxx;
  float sxy;
  float syy;
  float sx;
  float sy;
  float s1;
  float ru[3];    /* [Σu·x, Σu·y, Σu] */
  float rv[3];    /* [Σv·x, Σv·y, Σv] */
  float su2;
  float sv2;
  int   n;
};

void pw_calib_fit2d_reset(struct pw_calib_fit2d_s *f);
void pw_calib_fit2d_add(struct pw_calib_fit2d_s *f, float x, float y,
                        float u, float v);

/* 输出 2×2 矩阵 m[4]（行主序）与平移 t[2]，以及合成残差 RMS（单位与 u,v 同）。
 * 返回 0 成功；-1 样本不足或设计矩阵奇异。 */

int pw_calib_fit2d_solve(const struct pw_calib_fit2d_s *f, float *m, float *t,
                         float *rms);

/****************************************************************************
 * 陀螺静态零偏标定（真值 = 静止时角速度为零）
 *
 * 这是 AHRS 的**主用**零偏来源：Mahony 的积分项只能慢慢学零偏（几十秒且欠观测，
 * 实测 60s 才恢复 20~90%、偏航轴最差），所以正确做法是先静止标定出零偏、
 * 预置进滤波器，积分项只做微调。
 *
 * 同时给出"够不够静"的判据：静止时三轴标准差应很小，用户手一抖就作废。
 ****************************************************************************/

struct pw_calib_bias_s
{
  float sum[3];
  float sum2[3];
  int   n;
};

void pw_calib_bias_reset(struct pw_calib_bias_s *b);
void pw_calib_bias_add(struct pw_calib_bias_s *b, const float *gyro_rps);

int pw_calib_bias_ready(const struct pw_calib_bias_s *b, int min_n);

/* 输出零偏（rad/s）与三轴标准差（rad/s）。
 * 返回 0 成功；-1 样本不足；-2 不够静止（标准差超过 max_std_rps）；-3 参数错。 */

int pw_calib_bias_solve(const struct pw_calib_bias_s *b, float *bias_rps,
                        float *std_rps, float max_std_rps);

/****************************************************************************
 * 六面法加速度标定（真值 = 重力）
 *
 * 面编号约定（与 UI 提示一致）：
 *   0: +X 朝下   1: -X 朝下   2: +Y 朝下
 *   3: -Y 朝下   4: +Z 朝下   5: -Z 朝下
 * 每个面静置采样，理想读数为该轴 ±1 g。
 ****************************************************************************/

#define PW_CALIB_FACES  6

struct pw_calib_six_s
{
  float sum[PW_CALIB_FACES][3];
  int   n[PW_CALIB_FACES];
  int   nsamp;
};

void pw_calib_six_reset(struct pw_calib_six_s *s);
void pw_calib_six_add(struct pw_calib_six_s *s, int face, const float *acc_g);

/* 每个面至少 min_per_face 个样本才允许求解 */

int pw_calib_six_ready(const struct pw_calib_six_s *s, int min_per_face);

/* 输出：
 *   bias_g[3]   —— 零偏（g）
 *   scale[3]    —— 刻度修正系数（真值 = (实测 - bias)·scale）
 *   resid       —— 最大 |映射后读数 - 理想 ±1|（g），衡量标定质量
 *   cross       —— 最大轴间串扰（|非目标轴均值|，g），衡量安装/非正交
 * 返回 0 成功；-1 某面对比度不足（该面数据不可信）。 */

int pw_calib_six_solve(const struct pw_calib_six_s *s, float *bias_g,
                       float *scale, float *resid, float *cross);

/* 应用标定结果 */

void pw_calib_six_apply(const float *bias_g, const float *scale,
                        const float *in_g, float *out_g);

/****************************************************************************
 * 磁力计椭球拟合（硬铁 + 软铁）
 *
 * 代数拟合一般二次曲面：mᵀQm + 2·pᵀm = 1（9 个未知量，最小二乘）。
 * 由 Q、p 解出球心（硬铁）c = -Q⁻¹p，再对 Q 做 3×3 对称特征分解得到
 * 软铁矩阵 W = diag(1/√λ)·Vᵀ，使 |W·(m - c)| ≈ 1（单位球）。
 *
 * 物理量纲：本库只保证"单位球"，真实 |B| 要用当地 WMM/IGRF 换算
 * （docs/03 §4.8 明确要求不得使用"典型 50 µT"这类拍脑袋值）。
 *
 * 输入样本按 mG/1000 缩放后再累加（改善条件数）；输出已还原为输入单位。
 ****************************************************************************/

struct pw_calib_mag_s
{
  float a[9][9];      /* 正规方程（对称） */
  float b[9];         /* 右端项 */
  float sum_r2;       /* Σ|m|²（缩放后），用于报告平均场强 */
  int   n;
};

void pw_calib_mag_reset(struct pw_calib_mag_s *s);

/* 输入单位任意一致（建议 mG）；点数建议 ≥ 200 且覆盖各个朝向 */

void pw_calib_mag_add(struct pw_calib_mag_s *s, const float *m);

int pw_calib_mag_ready(const struct pw_calib_mag_s *s, int min_n);

/* 输出：
 *   center[3]       —— 硬铁偏置（输入单位）
 *   w[9]            —— 软铁矩阵（行主序），校正后 |W·(m-center)| ≈ 1
 *   mean_mag        —— 样本平均场强（输入单位），用于与 WMM 对照
 *   eig[3]          —— Q 的三个特征值（诊断：相差过大说明数据覆盖不好）
 * 返回 0 成功；-1 样本不足/-2 矩阵奇异/-3 拟合不是椭球（特征值非正）。 */

int pw_calib_mag_solve(const struct pw_calib_mag_s *s, float *center, float *w,
                       float *mean_mag, float *eig);

/* 应用校正：out = W·(m - center) */

void pw_calib_mag_apply(const float *center, const float *w,
                        const float *m, float *out);

/* 自检：合成"已知硬铁 + 已知软铁 + 已知倾斜安装"的椭球数据，检查拟合
 * 能不能把硬铁/软铁还原出来。成功返回 0；err_out 非空时写回最大相对误差。 */

int pw_calib_selftest(float *err_out);

#ifdef __cplusplus
}
#endif

#endif /* __PHYWEAR_PW_CALIB_H */
