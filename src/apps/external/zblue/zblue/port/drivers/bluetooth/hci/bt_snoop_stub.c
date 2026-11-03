/****************************************************************************
 * external/zblue/zblue/port/drivers/bluetooth/hci/bt_snoop_stub.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 为什么有这个小文件（PhyWear 蓝牙阶段 A 探针所需，本队新增）：
 *   h4.c 在收/发 HCI 包时会调用 btsnoop_log_capture()（嗅探日志），
 *   而它的唯一实现在 frameworks/connectivity/bluetooth/service/utils/btsnoop_log.c，
 *   那个 framework 被 CMakeLists 的 `if(CONFIG_BLUETOOTH)` 关着
 *   （本板 defconfig 里 CONFIG_BLUETOOTH is not set）。
 *   只把 h4.c 编进来必然 undefined reference，所以在端口层放一个空实现。
 *
 * 注意：这不是"打开嗅探功能"，而是明确地**不记录** HCI 日志（日志开关
 * CONFIG_BLUETOOTH_LOG 本来就是关的）。将来若真要 HCI 抓包，把这个文件删掉、
 * 改为打开 framework 即可。
 ****************************************************************************/

#include <stdint.h>

void btsnoop_log_capture(uint8_t is_receive, uint8_t *hci_pkt,
                         uint32_t hci_pkt_size)
{
  (void)is_receive;
  (void)hci_pkt;
  (void)hci_pkt_size;
}
