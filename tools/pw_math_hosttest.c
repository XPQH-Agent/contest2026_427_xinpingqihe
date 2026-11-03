/****************************************************************************
 * tools/phywear/pw_math_hosttest.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 主机侧单测入口：把 pw_ahrs / pw_calib / pw_traj 三个纯算法库编到 PC 上跑自检。
 * 这两个文件不依赖 NuttX/LVGL（只 include <nuttx/config.h>，主机用桩头文件），
 * 所以算法改动可以在 PC 上秒级迭代，不必每次烧板子。
 *
 * 用法：bash tools/phywear/hosttest_math.sh
 ****************************************************************************/

#include <stdio.h>
#include <string.h>

#include "pw_ahrs.h"
#include "pw_calib.h"
#include "pw_motion.h"
#include "pw_traj.h"

int main(int argc, char *argv[])
{
  /* 可选：--bench 跑动效查表的计时对照（P1-2 的计算节省量）。
   * 默认不跑，避免单测时间被计时污染。 */

  if (argc > 1 && strcmp(argv[1], "--bench") == 0)
    {
      pw_motion_bench(200000);
      return 0;
    }

  float err;
  int   rc;
  int   bad = 0;

  err = -1.0f;
  rc = pw_ahrs_selftest(&err);
  printf("pw_ahrs_selftest  rc=%d  max_attitude_err=%.3f deg   %s\n",
         rc, (double)err, rc == 0 ? "OK" : "FAIL");
  if (rc != 0)
    {
      bad++;
    }

  err = -1.0f;
  rc = pw_calib_selftest(&err);
  printf("pw_calib_selftest rc=%d  worst_rel_err=%.4f            %s\n",
         rc, (double)err, rc == 0 ? "OK" : "FAIL");
  if (rc != 0)
    {
      bad++;
    }

  /* 轨迹：整周期正弦推手 20 cm + 1 mg 零偏 + 静止抗噪三关。
   * 这一项最容易被"看起来像能跑"糊过去（naive ZUPT 会把推力当静止、
   * 位移恒为 0 却不报错），所以必须进常规单测。 */

  err = -1.0f;
  rc = pw_traj_selftest(&err);
  printf("pw_traj_selftest  rc=%d  max_disp_err=%.4f m          %s\n",
         rc, (double)err, rc == 0 ? "OK" : "FAIL");
  if (rc != 0)
    {
      bad++;
    }

  /* 动效查表：端点严格性 + 过冲形态 + 查表/解析一致性 */

  err = -1.0f;
  rc = pw_motion_selftest(&err);
  printf("pw_motion_selftest rc=%d  max_err=%.2e                  %s\n",
         rc, (double)err, rc == 0 ? "OK" : "FAIL");
  if (rc != 0)
    {
      bad++;
    }

  printf("%s\n", bad == 0 ? "ALL OK" : "SOME FAILED");
  return bad == 0 ? 0 : 1;
}
