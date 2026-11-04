/****************************************************************************
 * apps/examples/phywear/pw_settings.h
 *
 * 设置的"读回 / 落盘"入口（主题、语言、帧率表开关）。
 * 实现在 phywear.c：它同时能看到 g_fps_show 与 UI 侧的 getter。
 ****************************************************************************/

#ifndef __APPS_EXAMPLES_PHYWEAR_PW_SETTINGS_H
#define __APPS_EXAMPLES_PHYWEAR_PW_SETTINGS_H

/* 开机时调用一次：有 flash 记录就套用，没有就保持默认值 */

void pw_settings_load(void);

/* 任何设置变化后调用：写 flash（内容未变时内部直接返回，不擦除） */

void pw_settings_save(void);

#endif /* __APPS_EXAMPLES_PHYWEAR_PW_SETTINGS_H */
