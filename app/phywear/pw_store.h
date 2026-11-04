/****************************************************************************
 * apps/examples/phywear/pw_store.h
 *
 * 极简"掉电保留设置"：把主题 / 语言 / 帧率表开关写进 NOR flash 的 config 区。
 *
 * 设计取舍（写清楚，免得后来人以为是随手写的）：
 *  - 本板没有文件系统，/data 是 tmpfs，重启即失；NVS/settings 后端在 NuttX
 *    移植层没有 devicetree 分区，开不起来（T-0014 已踩过）。
 *  - 所以直接开 /dev/config0（SiFli NOR 的 config 分区，MTD 字符设备），
 *    只使用**最后一个擦除块**，块内 0xFF 才写，带 magic + CRC32。
 *  - 写坏的最坏结果：本模块读不出 magic，设置回到默认值；不影响固件与蓝牙
 *    校准（它们在其它分区）。
 ****************************************************************************/

#ifndef __APPS_EXAMPLES_PHYWEAR_PW_STORE_H
#define __APPS_EXAMPLES_PHYWEAR_PW_STORE_H

#include <stdbool.h>
#include <stdint.h>

struct pw_store_state_s
{
  uint8_t theme;      /* 0 = 方格，1 = 单排 */
  uint8_t zh;         /* 0 = English，1 = 中文 */
  uint8_t fps_show;   /* 屏上帧率表 */
  uint8_t rsv;
  uint32_t backlight; /* 预留：亮度 0~100 */
};

/* 读回上次保存的设置；返回 0 表示读到了有效数据，-ENODEV 表示没有 flash
 * 设备（此时保持调用方传进来的默认值）。
 */

int pw_store_load(struct pw_store_state_s *st);

/* 保存当前设置；内容没变就直接返回 0。返回 0 成功，负值为 errno。
 */

int pw_store_save(const struct pw_store_state_s *st);

/* 打印一行状态，供 NSH `phywear store` 用。
 */

int pw_store_report(void);

#endif /* __APPS_EXAMPLES_PHYWEAR_PW_STORE_H */
