/****************************************************************************
 * apps/examples/phywear/pw_btprobe.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 蓝牙阶段 A 探针：直接对 `/dev/ttyHCI0` 做一次 HCI_Reset，看 LCPU 答不答。
 *
 * 为什么自己写而不是用 zblue 的 shell/sample：阶段 A 要回答的是一个**可证伪**的
 * 问题 —— "片内 LCPU 的 HCI 到底通不通"。zblue 的 host 栈（bt_enable）会把
 * 传输层、缓冲区、事件线程全带上，任何一环没配好都失败，失败也说不清断在哪。
 * 这个探针只做三件事：开端口、发一条 HCI_Reset（0x03 0x0C）、等 Command Complete。
 *
 * 判据（写进 docs）：
 *   ① 打不开 /dev/ttyHCI0  → UART_BTH4 没生效（传输层问题）
 *   ② 打得开但无回包       → 端口在、LCPU 不应答
 *   ③ 回 04 0E 04 01 03 0C 00 → HCI 链路通（Command Complete, opcode 0x0C03, status 0）
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "pw_btprobe.h"

#define BT_HCI_DEV   "/dev/ttyHCI0"
#define BT_WAIT_MS   1500

static void bt_hex(const char *tag, const unsigned char *p, int n)
{
  int i;

  printf("[BTHCI] %s (%d B):", tag, n);
  for (i = 0; i < n; i++)
    {
      printf(" %02x", p[i]);
    }

  printf("\n");
}

int pw_bt_probe(void)
{
  static const unsigned char reset[] =
  {
    0x01, 0x03, 0x0c, 0x00        /* HCI_Reset：type=cmd, opcode=0x0C03(LE), len=0 */
  };

  unsigned char buf[64];
  struct pollfd pfd;
  int fd;
  int n;
  int i;

  fd = open(BT_HCI_DEV, O_RDWR | O_NONBLOCK);
  if (fd < 0)
    {
      printf("[BTHCI] open %s failed: %d (%s)\n", BT_HCI_DEV, errno,
             strerror(errno));
      printf("[BTHCI] 判据①：传输层没起来（CONFIG_UART_BTH4 / 驱动未注册）\n");
      return -1;
    }

  printf("[BTHCI] %s opened\n", BT_HCI_DEV);

  n = write(fd, reset, sizeof(reset));
  printf("[BTHCI] write HCI_Reset -> %d\n", n);
  bt_hex("tx", reset, sizeof(reset));

  pfd.fd = fd;
  pfd.events = POLLIN;
  n = poll(&pfd, 1, BT_WAIT_MS);

  if (n <= 0)
    {
      printf("[BTHCI] 判据②：%d ms 内无回包（端口在、LCPU 未应答）rc=%d\n",
             BT_WAIT_MS, n);
      close(fd);
      return -2;
    }

  n = read(fd, buf, sizeof(buf));
  if (n <= 0)
    {
      printf("[BTHCI] read failed: %d\n", errno);
      close(fd);
      return -3;
    }

  bt_hex("rx", buf, n);

  /* Command Complete: 04 0E 04 01 <opcode_lo> <opcode_hi> <status> */

  if (n >= 7 && buf[0] == 0x04 && buf[1] == 0x0e)
    {
      printf("[BTHCI] 判据③：HCI 链路通（Command Complete, opcode=0x%02x%02x, "
             "status=%d）\n", buf[5], buf[4], buf[6]);
      close(fd);
      return 0;
    }

  printf("[BTHCI] 收到非 Command Complete 事件，原始字节见上\n");
  close(fd);

  for (i = 0; i < 3; i++)
    {
      (void)0;
    }

  return -4;
}
