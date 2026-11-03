/****************************************************************************
 * apps/examples/phywear/pw_graph.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

#ifndef __APPS_EXAMPLES_PHYWEAR_GRAPH_H
#define __APPS_EXAMPLES_PHYWEAR_GRAPH_H

/* 可缩放/可平移的折线-散点图（phyphox graph 体验的手表单点适配版）。
 * 内部 = pw_scope 同款「CPU 光栅 + lv_image + EPIC blit」，缩放平移只改
 * 视图窗口重画，成本低、流畅。
 *
 * 交互：
 *   - 单指拖动 = 平移（X/Y 同时）
 *   - pw_graph_zoom(axis, factor)：X/Y 独立缩放（factor>1 放大）
 *   - pw_graph_fit()：自动适配数据范围
 *   - pw_graph_set_auto()：开启后每次 set_data 自动 fit
 */

#include <lvgl/lvgl.h>

#define PW_GRAPH_AXIS_X  0
#define PW_GRAPH_AXIS_Y  1

/* 每条序列的最大点数（调用方按此准备 x/y 数组）。
 * 调用方要用到上限的场合（轨迹页缓冲）必须用这个宏，别自己写 256。 */

#define PW_GRAPH_MAX_POINTS 256

struct pw_graph_s;

/* dots=1 散点模式，否则折线模式 */
struct pw_graph_s *pw_graph_create(lv_obj_t *parent, int w, int h,
                                   lv_color_t line, lv_color_t bg, int dots);

lv_obj_t *pw_graph_obj(struct pw_graph_s *g);

/* 单序列便捷 API：等价 begin + add_series(x,y,n,默认色) + end */

void pw_graph_set_data(struct pw_graph_s *g, FAR const float *x,
                       FAR const float *y, int n);

/* 多序列 API（roadmap §7.3 第 1 条，最多 PW_GRAPH_MAX_SERIES=4 条）：
 *   pw_graph_begin(g);                     清空序列集
 *   pw_graph_add_series(g,x,y,n,color);    逐条追加（每序列独立颜色）
 *   pw_graph_end(g);                       自动 fit + 重绘
 * 所有序列共用同一视图窗口与缩放/平移；fit 取全序列联合范围。 */

void pw_graph_begin(struct pw_graph_s *g);
int  pw_graph_add_series(struct pw_graph_s *g, FAR const float *x,
                         FAR const float *y, int n, lv_color_t color);
void pw_graph_end(struct pw_graph_s *g);

void pw_graph_set_auto(struct pw_graph_s *g, int on);

/* 直接指定视图窗口（并关掉 auto_fit）。用于"标尺必须固定"的图：
 * 例如轨迹页的 XY 平面——若每帧自动 fit，图形会随数据呼吸，
 * 位移大小就看不出比例了。 */

void pw_graph_set_range(struct pw_graph_s *g, float xmin, float xmax,
                        float ymin, float ymax);
void pw_graph_fit(struct pw_graph_s *g);
void pw_graph_set_grid(struct pw_graph_s *g, int on);
void pw_graph_zoom(struct pw_graph_s *g, int axis, float factor);
void pw_graph_pan(struct pw_graph_s *g, int dx, int dy);
void pw_graph_redraw(struct pw_graph_s *g);

/* 查询当前视图窗口（供状态栏反馈） */
void pw_graph_get_range(struct pw_graph_s *g, FAR float *xmin,
                        FAR float *xmax, FAR float *ymin, FAR float *ymax);

#endif /* __APPS_EXAMPLES_PHYWEAR_GRAPH_H */
