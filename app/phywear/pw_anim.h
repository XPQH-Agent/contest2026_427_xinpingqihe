/****************************************************************************
 * apps/examples/phywear/pw_anim.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * P1-2 动效算法：解析式阻尼弹簧（闭式解）+ 三次贝塞尔缓动。
 *
 * 为什么不用 PID 逐帧调参（用户明确禁止，且技术上也不该）：
 *   阻尼弹簧有闭式解，采样它就是"每帧求一次解析式"，没有稳态误差、没有超调
 *   参数需要试凑、也不会因为帧率变化而改变手感（PID 三者都会）。
 *
 * 三种实现**编译期共存**（回退与 A/B 用，见 PW_ANIM_IMPL）：
 *   PW_ANIM_IMPL_REC     二阶递推（默认）：每帧 M33 目标码 11 条、零 libm、
 *                        精度 0.0002 px@450px、零 flash。仅要求等间隔 dt。
 *   PW_ANIM_IMPL_CLOSED  解析式直算：任意 t，13 条 + 2 次 libm(expf/sinf)。
 *   PW_ANIM_IMPL_LUT     查表 256 项 int16 + 线性插值：32 条/帧 + 512 B flash，
 *                        精度 0.25 px@450px；**只对表内那条预设曲线成立**。
 *
 * 选型依据（真机同架构目标码实测，`-Os -march=armv8-m.main -mfpu=fpv5-sp-d16`）：
 *   REC 11 条 < CLOSED 13 条+2 libm < LUT 32 条 —— 本板有硬浮点 FPU、且动效
 *   调用密度只有"每属性每帧 1 次"，查表法"免计算"的前提不成立（反而更贵、还占
 *   flash、还有 0.25 px 量化误差），因此默认 REC，LUT 只作可选实现保留。
 ****************************************************************************/

#ifndef __APPS_EXAMPLES_PHYWEAR_ANIM_H
#define __APPS_EXAMPLES_PHYWEAR_ANIM_H

#define PW_ANIM_IMPL_CLOSED  0    /* 解析式（任意 t，含 libm） */
#define PW_ANIM_IMPL_REC     1    /* 二阶递推（等间隔 dt，零 libm） */
#define PW_ANIM_IMPL_LUT     2    /* 查表（仅默认预设曲线） */

/* 默认实现：递推。改这一行即可整体切实现（回退/对比用，无需改其它代码）。 */

#ifndef PW_ANIM_IMPL
#  define PW_ANIM_IMPL  PW_ANIM_IMPL_REC
#endif

/* LUT 表的预设参数：换预设必须用 gen_pw_anim_lut.py 重新生成表，
 * 否则 pw_anim_selftest() 会失败（不会静默给错曲线）。 */

#define PW_ANIM_LUT_N     256
#define PW_ANIM_LUT_A     4.0f
#define PW_ANIM_LUT_W     18.0f
#define PW_ANIM_LUT_DUR   0.9f

struct pw_anim_spring_s
{
  float damp;        /* a：阻尼系数 1/s */
  float omega;       /* w：角频率 rad/s（0 = 临界阻尼） */
  float dur;         /* 动画时长 s */
  float c1;          /* 递推系数（等间隔用） */
  float c2;
  float u1;          /* 递推状态（u = 归一化归位量） */
  float u2;
  float dt;          /* 递推使用的等间隔（0 = 尚未用 step 推进过） */
};

/* 初始化：damp>0、omega>=0、dur>0。omega=0 时退化为临界阻尼（含 (1+a t) 项）。 */

void pw_anim_spring_init(struct pw_anim_spring_s *s, float damp, float omega,
                         float dur);

/* 归一化归位量 u(t) ∈ (0,1]：1 = 尚未开始，→0 = 已归位。
 * 界面常用 y = 1 - u 作为"进度"（0→1 平滑落位，无超调参数可调）。 */

float pw_anim_spring_u(const struct pw_anim_spring_s *s, float t);

/* 界面进度 y = 1 - u(t)，裁剪到 [0,1]。 */

float pw_anim_spring_progress(const struct pw_anim_spring_s *s, float t);

/* 等间隔推进（PW_ANIM_IMPL_REC 的零开销路径）。首次调用会锁定 dt；
 * dt 变化超过 1% 时内部自动回退到解析式求值并按该 t 重新同步状态
 * （避免"帧间隔抖动导致曲线发散"这一递推法的固有问题）。 */

float pw_anim_spring_step(struct pw_anim_spring_s *s, float dt);

/* 当前时间是否已结束（t >= dur，且归位量已足够小）。 */

int pw_anim_spring_done(const struct pw_anim_spring_s *s, float t);

/* 三次贝塞尔缓动（CSS cubic-bezier 语义：(0,0)-(p1)-(p2)-(1,1) 上按 x 求 y）。
 * 迭代次数固定（PW_ANIM_BEZIER_ITER），不引入不确定耗时的循环。 */

#define PW_ANIM_BEZIER_ITER 4

float pw_anim_bezier_ease(float p1x, float p1y, float p2x, float p2y,
                          float progress);

/* 自检：以解析式为真值，校验当前 PW_ANIM_IMPL 的实现精度。
 * 返回 0=通过；err_px 非空时写回最大误差（像素，按 450 px 满幅折算）。
 * 失败码：-1 递推超差 / -2 查表超差 / -3 查表预设不匹配 / -4 贝塞尔超差。 */

int pw_anim_selftest(float *err_px);

/* 当前编译进来的实现名（用于日志/证据，不参与逻辑）。 */

const char *pw_anim_impl_name(void);

#endif /* __APPS_EXAMPLES_PHYWEAR_ANIM_H */
