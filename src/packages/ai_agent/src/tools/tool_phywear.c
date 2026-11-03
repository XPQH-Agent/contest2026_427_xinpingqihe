/*
 * Copyright (C) 2026 Xiaomi Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* PhyWear tools: drive the wrist physics workshop from natural language.
 *
 * Design notes
 * ------------
 *  - LVGL is not thread safe and the PhyWear GUI owns the console, so screen
 *    switching is NOT done here.  We post a request into the PhyWear mailbox
 *    (pw_ai_request_open) and the GUI loop performs it on its next tick.
 *  - Sensor reads are plain character-device reads, so they may be issued
 *    from the agent thread directly.
 *  - Everything degrades gracefully: if the PhyWear app was not built or its
 *    UI is not running, each tool answers with a readable JSON error instead
 *    of failing the agent turn.
 */

#include "tools/tool_phywear.h"
#include "agent_compat.h"

#include <string.h>

#include "cJSON.h"

#ifdef CONFIG_EXAMPLES_PHYWEAR

/* Provided by apps/examples/phywear (declared here to avoid a hard build
 * dependency on the app's private headers). */

extern int         pw_ai_screen_count(void);
extern const char *pw_ai_screen_name(int idx);
extern const char *pw_ai_screen_desc(int idx);
extern int         pw_ai_request_open(const char *screen);
extern bool        pw_ai_gui_running(void);
extern const char *pw_ai_current_screen(void);

/* 实验结果登记（apps/examples/phywear/pw_ai.c）。实验页在 GUI 线程里算完后发布，
 * Agent 只读不采样 —— 避免 agent 任务与页面同时 50 Hz 抢同一个 I2C。 */

#define PW_AI_RESULT_KIND_MAX   16
#define PW_AI_RESULT_UNIT_MAX   12
#define PW_AI_RESULT_TEXT_MAX   80

struct pw_ai_result_s
{
  char  kind[PW_AI_RESULT_KIND_MAX];
  char  unit[PW_AI_RESULT_UNIT_MAX];
  char  detail[PW_AI_RESULT_TEXT_MAX];
  int   seq;
  float value;
  float aux;
};

extern int  pw_ai_result_seq(const char *kind);
extern bool pw_ai_result_get(const char *kind, struct pw_ai_result_s *out);
extern void pw_ai_note(const char *text);

struct pw_ai_imu_s
{
  int ax;
  int ay;
  int az;
  int gx;
  int gy;
  int gz;
};

struct pw_ai_mag_s
{
  int x;
  int y;
  int z;
};

struct pw_ai_light_s
{
  unsigned int lux;
  int ch0;
  int ch1;
};

extern int pw_sensors_read_imu(struct pw_ai_imu_s *out);
/* 在调用者任务里 open/read/close：AI Agent 是独立任务组，不能复用 GUI
 * 任务缓存的 fd（真机实测会回 accelerometer not available）。 */
extern int pw_sensors_read_imu_oneshot(struct pw_ai_imu_s *out);
extern int pw_sensors_read_mag(struct pw_ai_mag_s *out);
extern int pw_sensors_read_light(struct pw_ai_light_s *out);

#endif /* CONFIG_EXAMPLES_PHYWEAR */

/****************************************************************************
 * Helpers
 ****************************************************************************/

static void pw_tool_emit(cJSON *root, char *output, size_t output_size,
                         int status)
{
  char *s;

  (void)status;

  if (root == NULL)
    {
      snprintf(output, output_size, "{\"error\":\"out of memory\"}");
      return;
    }

  s = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);

  if (s != NULL)
    {
      strncpy(output, s, output_size - 1);
      output[output_size - 1] = '\0';
      free(s);
    }
  else
    {
      snprintf(output, output_size, "{\"error\":\"serialization failed\"}");
    }
}

static void pw_tool_emit_str(const char *key, const char *val, char *output,
                             size_t output_size)
{
  cJSON *r = cJSON_CreateObject();

  if (r != NULL)
    {
      cJSON_AddStringToObject(r, key, val);
    }

  pw_tool_emit(r, output, output_size, OK);
}

/****************************************************************************
 * phywear_list_experiments
 ****************************************************************************/

int tool_phywear_list_execute(const char *input_json, char *output,
                              size_t output_size)
{
  (void)input_json;

#ifdef CONFIG_EXAMPLES_PHYWEAR
  cJSON *root = cJSON_CreateObject();
  cJSON *arr;
  int i;

  if (root == NULL)
    {
      return ERROR;
    }

  arr = cJSON_AddArrayToObject(root, "screens");

  for (i = 0; i < pw_ai_screen_count(); i++)
    {
      cJSON *item = cJSON_CreateObject();

      cJSON_AddStringToObject(item, "name", pw_ai_screen_name(i));
      cJSON_AddStringToObject(item, "description", pw_ai_screen_desc(i));
      cJSON_AddItemToArray(arr, item);
    }

  cJSON_AddBoolToObject(root, "ui_running", pw_ai_gui_running());
  cJSON_AddStringToObject(root, "current_screen", pw_ai_current_screen());
  cJSON_AddStringToObject(root, "hint",
                          "Use phywear_open_screen with one of these names "
                          "to bring that page up on the watch display.");

  pw_tool_emit(root, output, output_size, OK);
  return OK;
#else
  pw_tool_emit_str("error", "PhyWear is not built into this image", output,
                   output_size);
  return ERROR;
#endif
}

/****************************************************************************
 * phywear_open_screen
 ****************************************************************************/

int tool_phywear_open_execute(const char *input_json, char *output,
                              size_t output_size)
{
#ifdef CONFIG_EXAMPLES_PHYWEAR
  /* Copy the parsed string out of the cJSON tree immediately: the tree is
   * released before the value is reported back, and holding a pointer into it
   * would be a use-after-free (KASAN catches it on the emulator). */

  char screen[32];
  bool have_screen = false;
  cJSON *root;
  cJSON *item;
  int ret;

  screen[0] = '\0';

  root = cJSON_Parse(input_json);
  if (root != NULL)
    {
      item = cJSON_GetObjectItem(root, "screen");
      if (item != NULL && cJSON_IsString(item))
        {
          const char *v = cJSON_GetStringValue(item);

          if (v != NULL)
            {
              strncpy(screen, v, sizeof(screen) - 1);
              screen[sizeof(screen) - 1] = '\0';
              have_screen = true;
            }
        }

      cJSON_Delete(root);
    }

  if (!have_screen)
    {
      pw_tool_emit_str("error", "missing required parameter: screen", output,
                       output_size);
      return ERROR;
    }

  ret = pw_ai_request_open(screen);

  if (ret < 0)
    {
      cJSON *r = cJSON_CreateObject();

      if (r != NULL)
        {
          cJSON_AddStringToObject(r, "error",
                                  ret == -ENODEV
                                      ? "PhyWear UI is not running; start the "
                                        "phywear app first"
                                      : "unknown screen name");
          cJSON_AddStringToObject(r, "requested", screen);
          cJSON_AddBoolToObject(r, "ui_running", pw_ai_gui_running());
        }

      pw_tool_emit(r, output, output_size, ERROR);
      return ERROR;
    }

  {
    cJSON *r = cJSON_CreateObject();

    if (r != NULL)
      {
        cJSON_AddBoolToObject(r, "accepted", true);
        cJSON_AddStringToObject(r, "screen", screen);
        cJSON_AddStringToObject(r, "previous_screen", pw_ai_current_screen());
        cJSON_AddStringToObject(r, "note",
                                "The page is switched by the PhyWear UI loop "
                                "within about 100 ms.");
      }

    pw_tool_emit(r, output, output_size, OK);
  }

  return OK;
#else
  pw_tool_emit_str("error", "PhyWear is not built into this image", output,
                   output_size);
  return ERROR;
#endif
}

/****************************************************************************
 * phywear_read_sensor
 ****************************************************************************/

int tool_phywear_sensor_execute(const char *input_json, char *output,
                                size_t output_size)
{
#ifdef CONFIG_EXAMPLES_PHYWEAR
  const char *sensor = NULL;
  cJSON *root;
  cJSON *item;
  cJSON *r;

  root = cJSON_Parse(input_json);
  if (root != NULL)
    {
      item = cJSON_GetObjectItem(root, "sensor");
      if (item != NULL && cJSON_IsString(item))
        {
          sensor = cJSON_GetStringValue(item);
        }
    }

  if (sensor == NULL)
    {
      cJSON_Delete(root);
      pw_tool_emit_str("error", "missing required parameter: sensor", output,
                       output_size);
      return ERROR;
    }

  r = cJSON_CreateObject();
  if (r == NULL)
    {
      cJSON_Delete(root);
      return ERROR;
    }

  if (strcmp(sensor, "accel") == 0 || strcmp(sensor, "acceleration") == 0)
    {
      struct pw_ai_imu_s imu;

      memset(&imu, 0, sizeof(imu));
      if (pw_sensors_read_imu_oneshot(&imu) < 0)
        {
          cJSON_AddStringToObject(r, "error", "accelerometer not available");
          pw_tool_emit(r, output, output_size, ERROR);
          cJSON_Delete(root);
          return ERROR;
        }

      cJSON_AddStringToObject(r, "sensor", "accel");
      cJSON_AddStringToObject(r, "unit", "mg");
      cJSON_AddNumberToObject(r, "ax", (double)imu.ax);
      cJSON_AddNumberToObject(r, "ay", (double)imu.ay);
      cJSON_AddNumberToObject(r, "az", (double)imu.az);
      cJSON_AddNumberToObject(r, "gx", (double)imu.gx);
      cJSON_AddNumberToObject(r, "gy", (double)imu.gy);
      cJSON_AddNumberToObject(r, "gz", (double)imu.gz);
      cJSON_AddStringToObject(r, "gyro_unit", "mdps");
    }
  else if (strcmp(sensor, "mag") == 0 ||
           strcmp(sensor, "magnetometer") == 0)
    {
      struct pw_ai_mag_s mag;

      memset(&mag, 0, sizeof(mag));
      if (pw_sensors_read_mag_oneshot(&mag) < 0)
        {
          cJSON_AddStringToObject(r, "error", "magnetometer not available");
          pw_tool_emit(r, output, output_size, ERROR);
          cJSON_Delete(root);
          return ERROR;
        }

      cJSON_AddStringToObject(r, "sensor", "mag");
      cJSON_AddStringToObject(r, "unit", "mG");
      cJSON_AddNumberToObject(r, "x", (double)mag.x);
      cJSON_AddNumberToObject(r, "y", (double)mag.y);
      cJSON_AddNumberToObject(r, "z", (double)mag.z);
    }
  else if (strcmp(sensor, "light") == 0)
    {
      struct pw_ai_light_s light;

      memset(&light, 0, sizeof(light));
      if (pw_sensors_read_light_oneshot(&light) < 0)
        {
          cJSON_AddStringToObject(r, "error", "light sensor not available");
          pw_tool_emit(r, output, output_size, ERROR);
          cJSON_Delete(root);
          return ERROR;
        }

      cJSON_AddStringToObject(r, "sensor", "light");
      cJSON_AddStringToObject(r, "unit", "lux");
      cJSON_AddNumberToObject(r, "lux", (double)light.lux);
    }
  else
    {
      cJSON_AddStringToObject(r, "error", "unknown sensor");
      cJSON_AddStringToObject(r, "requested", sensor);
      cJSON_AddStringToObject(r, "supported", "accel | mag | light");
      pw_tool_emit(r, output, output_size, ERROR);
      cJSON_Delete(root);
      return ERROR;
    }

  pw_tool_emit(r, output, output_size, OK);
  cJSON_Delete(root);
  return OK;
#else
  pw_tool_emit_str("error", "PhyWear is not built into this image", output,
                   output_size);
  return ERROR;
#endif
}

/****************************************************************************
 * phywear_run_experiment
 *
 * Opens the requested experiment page, then waits for the page itself to
 * register a result (see pw_ai_publish_result) and returns those numbers.
 *
 * The agent deliberately does NOT sample the IMU here: the page already
 * samples at 50 Hz, and two 50 Hz readers on the same I2C bus used to freeze
 * the whole device after ~10 s (that is why the proactive scenario was
 * disabled). Only the GUI/experiment path touches the sensor now.
 ****************************************************************************/

int tool_phywear_run_execute(const char *input_json, char *output,
                             size_t output_size)
{
#ifdef CONFIG_EXAMPLES_PHYWEAR
  const char *screen = NULL;
  int seconds = 5;
  int ret;
  cJSON *root;
  cJSON *item;
  cJSON *r;

  root = cJSON_Parse(input_json);
  if (root != NULL)
    {
      item = cJSON_GetObjectItem(root, "screen");
      if (item != NULL && cJSON_IsString(item))
        {
          screen = cJSON_GetStringValue(item);
        }

      item = cJSON_GetObjectItem(root, "seconds");
      if (item != NULL && cJSON_IsNumber(item))
        {
          seconds = (int)item->valuedouble;
        }
    }

  if (screen == NULL)
    {
      cJSON_Delete(root);
      pw_tool_emit_str("error", "missing required parameter: screen", output,
                       output_size);
      return ERROR;
    }

  if (seconds < 1)
    {
      seconds = 1;
    }
  else if (seconds > 30)
    {
      seconds = 30;
    }

  ret = pw_ai_request_open(screen);
  if (ret < 0)
    {
      cJSON *e = cJSON_CreateObject();

      if (e != NULL)
        {
          cJSON_AddStringToObject(e, "error",
                                  ret == -ENODEV
                                      ? "PhyWear UI is not running; start the "
                                        "phywear app first"
                                      : "unknown screen name");
          cJSON_AddStringToObject(e, "requested", screen);
          cJSON_AddBoolToObject(e, "ui_running", pw_ai_gui_running());
        }

      pw_tool_emit(e, output, output_size, ERROR);
      cJSON_Delete(root);
      return ERROR;
    }

  /* 不再在 agent 任务里采样 IMU。原先这里按 50 Hz 采 N 秒，而屏幕上的实验页
   * 同时也在 50 Hz 采同一颗传感器 —— 两路 50 Hz 抢同一个 I2C（oneshot 每样本还
   * open/ioctl/close 一次），真机实测十几秒整机卡死，主动场景因此被关。
   *
   * 现在只"等页面把结果登记出来"：I2C 永远只有 GUI 那一路。等不到就如实报
   * unavailable（不编造数字）。 */

  {
    int before = pw_ai_result_seq(screen);
    int waited = 0;

    while (waited < seconds * 1000)
      {
        usleep(100 * 1000);
        waited += 100;

        if (pw_ai_result_seq(screen) != before)
          {
            break;
          }
      }

    struct pw_ai_result_s res;

    memset(&res, 0, sizeof(res));

    if (pw_ai_result_get(screen, &res))
      {
        r = cJSON_CreateObject();

        if (r == NULL)
          {
            cJSON_Delete(root);
            return ERROR;
          }

        cJSON_AddBoolToObject(r, "accepted", true);
        cJSON_AddStringToObject(r, "screen", screen);
        cJSON_AddStringToObject(r, "result", "ok");
        cJSON_AddNumberToObject(r, "value", (double)res.value);
        cJSON_AddStringToObject(r, "unit", res.unit);
        cJSON_AddNumberToObject(r, "aux", (double)res.aux);
        cJSON_AddStringToObject(r, "detail", res.detail);
        cJSON_AddNumberToObject(r, "waited_ms", (double)waited);
        cJSON_AddStringToObject(r, "source",
                                "experiment page (GUI thread)");
        /* human 字段：手表上要显示的那一行。pw_ai_humanize() 优先用它，
         * 这样纯 UI 文案不用散落在工具里（且用字已核对在本队 CJK 字体子集内：
         * "完"字缺，所以写"已自动测…"而不是"…完成"）。 */

        {
          char msg[128];

          snprintf(msg, sizeof(msg),
                   "已自动测重力加速度: g = %.2f %s (T = %.3f s)",
                   (double)res.value, res.unit, (double)res.aux);
          cJSON_AddStringToObject(r, "human", msg);
        }

        pw_tool_emit(r, output, output_size, OK);   /* 注意：emit 内部会 free(r) */
        cJSON_Delete(root);
        return OK;
      }
  }

  /* 等到超时都没有新结果：如实报告，不编数字。 */

  r = cJSON_CreateObject();
  if (r == NULL)
    {
      cJSON_Delete(root);
      return ERROR;
    }

  cJSON_AddBoolToObject(r, "accepted", true);
  cJSON_AddStringToObject(r, "screen", screen);
  cJSON_AddStringToObject(r, "result", "unavailable");
  cJSON_AddStringToObject(r, "human",
                          "no experiment result (device not moved, or this "
                          "page has no readable result yet)");
  cJSON_AddNumberToObject(r, "waited_ms", (double)(seconds * 1000));
  cJSON_AddStringToObject(r, "note",
                          "The experiment page did not register a result in "
                          "time. Either the watch was not moved, or this page "
                          "does not expose a machine-readable result yet. "
                          "For a single raw reading use phywear_read_sensor.");
  pw_tool_emit(r, output, output_size, ERROR);   /* emit 内部会 free(r) */
  cJSON_Delete(root);
  return ERROR;
#else
  pw_tool_emit_str("error", "PhyWear is not built into this image", output,
                   output_size);
  return ERROR;
#endif
}
