# LVGL 官方 benchmark 跑分（真机）— 2026-09-16

设备：立创黄山派 SF32LB52-MOD-1-N16R8（390×450 CO5300 + EPIC 加速），LVGL **9.1.0**，
固件 `2,484,328 B` / md5 `2c8f73cb181d6f9056c3e51db1213790`。
入口：`phywear lvbench`（本队新增；调用 `lv_demo_benchmark()`，结果打印到串口 + 屏幕表格）。
原始输出：`lvbench-summary.txt`。

## 1. 跑分（官方 20 场景）

| 场景 | Avg. CPU | Avg. FPS | Avg. time | render | flush |
|---|---|---|---|---|---|
| Empty screen | 54% | **56** | 6 | 6 | 0 |
| Moving wallpaper | 83% | 53 | 14 | 14 | 0 |
| Single rectangle | 62% | **63** | 7 | 7 | 0 |
| Multiple rectangles | 81% | 60 | 11 | 11 | 0 |
| Multiple RGB images | 86% | 51 | 14 | 14 | 0 |
| Multiple ARGB images | 86% | 51 | 14 | 14 | 0 |
| Rotated ARGB images | 88% | 40 | 19 | 19 | 0 |
| Multiple labels | 87% | 47 | 17 | 16 | 1 |
| Screen sized text | 94% | **15** | 58 | 58 | 0 |
| Multiple arcs | 86% | 56 | 13 | 13 | 0 |
| Span text | 90% | 28 | 28 | 28 | 0 |
| BIN image | 90% | 35 | 23 | 23 | 0 |
| BIN I8 image | 91% | 31 | 27 | 27 | 0 |
| BIN A8 image | 81% | 60 | 11 | 11 | 0 |
| Containers | 66% | 28 | 20 | 20 | 0 |
| Containers with overlay | 85% | 35 | 22 | 22 | 0 |
| Containers with opa | 78% | 27 | 25 | 25 | 0 |
| Containers with opa_layer | 90% | 18 | 46 | 45 | 1 |
| Containers with scrolling | 91% | 27 | 30 | 30 | 0 |
| Widgets demo | 94% | 16 | 42 | 42 | 0 |
| **All scenes avg.** | **83%** | **39** | **22** | **22** | **0** |

## 2. 三条可引用结论

1. **画质极简的场景天花板 56~63 FPS**（`Empty screen` 56、`Single rectangle` 63、`BIN A8` 60）——
   说明**有效帧节拍约 16~18 ms**。注意：这是"轻负载上限"的实测，**不是**面板规格书数字；
   `co5300.c` 里 `frame rate=82` 只是注释里的 TODO，且 `syn_mode` 默认为 DISABLE，
   所以"锁在 60 Hz"属**推断**，本队不把它当结论。
2. **重场景是 CPU 软件光栅瓶颈**：`Screen sized text` 15 FPS / **58 ms 渲染**、
   `Widgets demo` 16 FPS / 42 ms、`Containers with opa_layer` 18 FPS / 45 ms。这些场景 EPIC 帮不上忙
   （满屏文字、图层透明度、容器叠加），全部落在 CPU 上。
3. **flush 列几乎全是 0**：与 `fb_flush_start()` 走脏区、且推屏与渲染重叠的事实一致 ——
   **瓶颈不在面板传输**。这条也再次否掉了"面板带宽是主要瓶颈"的猜测。

## 3. 与自研探针的交叉验证

`[PERF]`（每秒，`PW_PERF_PROBE=1`）在本队页面上的读数：实时页 48~49 FPS / 单帧窗口 17.1 ms、
图线页 13 / 19.2 ms、轨迹页 10~11 / 21.4 ms、单摆页 22 / 26.7 ms。
两套数据一致地指向同一件事：**轻负载约 17 ms 是节拍下限，重负载由 CPU 光栅决定**。

## 4. 本页成本提示

`lvbench` 入口会把 LVGL demos 链进固件：**+278 KB flash**（2,206,088 → 2,484,328 B，13.15% → 15.38%）。
不需要跑分时，删掉 `phywear.c` 里的 `lvbench` 分支即可（宏 `CONFIG_LV_USE_DEMO_BENCHMARK` 随之失效）。
