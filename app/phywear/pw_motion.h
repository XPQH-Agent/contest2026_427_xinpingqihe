/****************************************************************************
 * apps/examples/phywear/pw_motion.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * PhyWear 动效数学核心（P1-2）。**纯算法，不依赖 LVGL**，所以能在 PC 上单测。
 *
 * 设计（2026-09-16）：
 *   · 动效形状只有两条固定曲线（物理弹簧阶跃响应 / 衰减振荡），一秒钟被算几百次，
 *     没必要每帧调 expf/sinf —— 离线算好存 flash，运行时做**索引 + 线性插值**。
 *   · 表是 const，落 .rodata/flash，不占 SRAM（129×2 B × 2 条 = 516 B）。
 *   · **解析式实现一直保留**（pw_motion_*_analytic_*），两个用途：
 *       ① 主机/真机上做"查表 vs 解析"的对照计时（计算节省量必须有实测）；
 *       ② 回退：把 PW_MOTION_USE_TABLE 置 0 即可整模块切回解析式，不改调用点。
 *
 * 与 LVGL 的关系：本文件不 include LVGL；LVGL 的 path 回调在 pw_motion_lvgl.c。
 ****************************************************************************/

#ifndef __APPS_EXAMPLES_PHYWEAR_MOTION_H
#define __APPS_EXAMPLES_PHYWEAR_MOTION_H

#include <stdint.h>

/* 曲线参数（与 pw_motion_table.c 的生成参数一致；改曲线要重跑
 * tools/phywear/gen_motion_table.py，不得手改表）：
 *   spring：欠阻尼二阶阶跃响应 ζ=0.40 ω=12，过冲峰值 ≈1.25，u=1 严格落 1.0
 *   decay ：e^(-6u)·sin(2π·2u)，按峰值归一化到 ±1
 */

#define PW_MOTION_N      129            /* 采样点（含两端），u = i/128 */
#define PW_MOTION_Q      14             /* Q14：1.0 = 16384 */
#define PW_MOTION_ONE    (1 << PW_MOTION_Q)
#define PW_MOTION_RES    1024           /* 归一化时间分辨率：u1024 ∈ [0,1024] */

/* 编译期开关：1 = 查表（默认，省算力）；0 = 解析式（对照/回退用）。
 * 两种实现的曲线形状与精度不同（表有 1e-3 量级插值误差），但调用点完全一致。 */

#ifndef PW_MOTION_USE_TABLE
#  define PW_MOTION_USE_TABLE 1
#endif

/* 曲线档（对应上游 motion.csv 的 easing 语义，见 third_party/.../data/motion-lvgl.csv）
 *   C1 settle ← power1/2.out（ζ=0.70，无过冲）  按压回位/数值更新/进度
 *   C2 soft   ← back.out(1.4)（ζ=0.45，过冲≈20%）卡片/宫格入场
 *   C3 bounce ← elastic.out（ζ=0.25，过冲≈45%）明确操作回弹
 *   C4 snap   ← expo.out（ζ=0.90，短促）        即时反馈 */

#define PW_MOTION_TIER_C1  0
#define PW_MOTION_TIER_C2  1
#define PW_MOTION_TIER_C3  2
#define PW_MOTION_TIER_C4  3
#define PW_MOTION_TIER_N   4

extern const int16_t pw_motion_tab_c1[PW_MOTION_N];
extern const int16_t pw_motion_tab_c2[PW_MOTION_N];
extern const int16_t pw_motion_tab_c3[PW_MOTION_N];
extern const int16_t pw_motion_tab_c4[PW_MOTION_N];
extern const float   pw_motion_c1_zeta;
extern const float   pw_motion_c1_omega;
extern const float   pw_motion_c2_zeta;
extern const float   pw_motion_c2_omega;
extern const float   pw_motion_c3_zeta;
extern const float   pw_motion_c3_omega;
extern const float   pw_motion_c4_zeta;
extern const float   pw_motion_c4_omega;

extern const int16_t pw_motion_tab_spring[PW_MOTION_N];
extern const int16_t pw_motion_tab_decay[PW_MOTION_N];

/* 曲线归一化系数（由 gen_motion_table.py 生成，解析式实现必须引用同一常量，
 * 否则"查表 vs 解析"的对照就不成立） */

extern const float pw_motion_spring_k;
extern const float pw_motion_decay_k;

/****************************************************************************
 * 取值接口：u1024 为归一化时间 ∈[0,1024]，返回 Q14 定点值。
 * spring 允许过冲（返回值可 > PW_MOTION_ONE）；u1024 >= 1024 时**严格返回 ONE**。
 ****************************************************************************/

int32_t pw_motion_spring_q14(uint32_t u1024);

/* 分档取值：tier = PW_MOTION_TIER_C1..C4；越界按 C2 处理 */
int32_t pw_motion_spring_q14_t(int tier, uint32_t u1024);
int32_t pw_motion_decay_q14(uint32_t u1024);

/* 解析式实现（始终编译，供对照计时与回退验证） */

int32_t pw_motion_spring_analytic_q14(uint32_t u1024);
int32_t pw_motion_spring_analytic_q14_t(int tier, uint32_t u1024);
int32_t pw_motion_decay_analytic_q14(uint32_t u1024);

/****************************************************************************
 * 自检：与解析式逐点比对查表结果，检查端点与过冲。
 *   成功返回 0，err 非空时写回最大归一化绝对误差；失败码 -1..-5。
 ****************************************************************************/

int pw_motion_selftest(float *err);

/* 真机/主机计时：两种实现各跑 iters 次，打印 ns/次与倍数。
 * 存在的意义：P1-2 的"计算节省量"必须是实测数字，不是估计。 */

void pw_motion_bench(int iters);

#endif /* __APPS_EXAMPLES_PHYWEAR_MOTION_H */
