/****************************************************************************
 * apps/examples/phywear/pw_btppp.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 任务 3 的第三条路线：**PPP over BLE**（NuttX 侧实现）
 *
 * 为什么走这条：本板没有无线网卡（`ifconfig: open failed: 2` 已证），
 * USB CDC-ACM + SLIP 又缺一根插不上的数据线 ⇒ **BLE 是唯一现成的物理通道**。
 * 而 NuttX 自带完整的 pppd（apps/netutils/pppd），主机侧 pppd 也早已安装 ——
 * 中间只差一条"BLE ↔ 伪终端(pty)"的**字节管道**，本文件就是这条管道。
 *
 * 结构（三段，各司其职）：
 *
 *   ┌─ BT 侧 ────────────────┐   ┌─ 本文件 ─────────────┐   ┌─ NuttX 网络 ─┐
 *   │ GATT 写 (a101)  ──────►│──►│ 环形缓冲 → pty master │──►│              │
 *   │ GATT 通知 (a102) ◄─────│◄──│ pty master → 通知      │◄──│ pppd (slave) │
 *   └────────────────────────┘   └──────────────────────┘   └──────┬───────┘
 *                                                                  ▼
 *                                                          ppp0 + IP（N1/N2）
 *
 * 为什么用 **pty** 而不是直接把 fd 交给 pppd：
 *   NuttX 的 pppd 只通过 `ppp_arch_getchar/putchar` 读写它自己 `open()` 的
 *   那个**字符设备**（见 apps/netutils/pppd/pppd.c）。pty 正好提供一对
 *   双向字节流：我们拿 master，pppd 拿 slave，双方都不用改一行 pppd 代码。
 *
 * 为什么用 **GATT 写+通知** 而不是 L2CAP CoC：
 *   CoC 吞吐更好，但 Linux 用户态走 LE CoC 的 socket API 很别扭；
 *   而 GATT 这条路我们已经有现成、验证过的宿主实现（pw_ble_central.py）。
 *   协商后 ATT MTU 是 517 ⇒ 单次可带 ~514 B，跑 LCP/IPCP/ping 绰绰有余。
 *   （CoC 留作后续优化，见 docs/16 §13.6e。）
 *
 * 跨线程约定（与 pw_btgatt.c 同一套，务必遵守）：
 *   * GATT 回调跑在 BT 接收线程 ⇒ 只往环形缓冲写字节、只改标志位，
 *     **不碰任何 fd、不碰 LVGL**；
 *   * pty 的 open/read/write 全部在**本文件自己的任务**里做 ——
 *     NuttX 的 fd 表挂在 task_group 上（B1 根因就是踩了这个），
 *     所以"谁 open 谁用"，绝不把 fd 跨组传；
 *   * 索引都是单字宽读写，单生产者单消费者 ⇒ 不加锁。满了丢字节并计数
 *     （PPP 有自己的 FCS，丢包会重传比在这里阻塞 BT 线程划算）。
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>

#include "netutils/pppd.h"
#include "pw_btppp.h"

/* ── 总开关：2026-09-18 起 **默认开** ──────────────────────────────────
 *
 * 2026-09-18 的状态是"功能跑通到 pty + pppd 启动，但装不进 SRAM"：
 *   · NuttX 的 pppd 需要 ~16 KB 栈（参考例子 apps/examples/pppd 用
 *     CONFIG_EXAMPLES_PPPD_STACKSIZE=16096）；
 *   · 加上桥任务 2 KB + pty 2×512 B，本功能要 ~21 KB SRAM；
 *   · 当时余量只有 ~4~5 KB（悬崖在 95.98% 能开机 / 96.76% 挂死之间），
 *     给 pppd 16 KB 时 SRAM 98.71%，板子连 ABCD 都打不出来。
 *
 * 2026-09-18 把 NuttX 的 hpwork/lpwork 栈由 16096 B 收到 **8192 B**
 * （defconfig 里 CONFIG_SCHED_HPWORKSTACKSIZE/CONFIG_SCHED_LPWORKSTACKSIZE；
 * 两者只是继承了板级 DEFAULT_TASK_STACKSIZE=16096，本身没有特殊需求）：
 *   SRAM 496,452 B (94.69%) → 480,644 B (91.68%)，净腾出 **15,808 B**。
 * 腾出的空间足够本功能按"设计值"跑（pppd 16 KB + 桥 2 KB）。
 * 8 KB 栈的安全性由真机完整负载验证（验收 18/18 + B3 文本往返 + GUI）。
 */
#ifndef PW_BT_PPP
#define PW_BT_PPP 1
#endif

#if PW_BT_PPP

/* ── 环形缓冲：BT 线程写，管道任务读 ─────────────────────────────────── */

#define PPP_RX_RING   2048      /* 主机 → 设备 */
#define PPP_TX_CHUNK  512       /* 设备 → 主机：单次通知上限（MTU 517 够用） */
#define PPP_PTY_RD    256

static uint8_t           g_rx[PPP_RX_RING];
static volatile unsigned g_rx_head;
static volatile unsigned g_rx_tail;
static volatile uint32_t g_rx_drop;      /* 丢弃字节数（缓冲满） */
static volatile uint32_t g_rx_total;     /* 累计收字节 */
static volatile uint32_t g_tx_total;     /* 累计发字节 */
static volatile uint8_t  g_ppp_sub;      /* 主机是否订阅了 TX 通知 */
static volatile uint8_t  g_ppp_up;       /* 管道是否已起来 */

static int  g_pty_master = -1;
static int  g_pty_slave_fd = -1;   /* 常开：见 pw_ppp_bridge_task 里的说明 */
static char g_pty_slave[32];
static volatile int g_pppd_rc = -1;      /* pppd 退出码（正常时永远不返回） */

/* 排障计数（2026-09-18 加的）：把"卡在哪一段"直接暴露在状态串里。
 *   g_dbg_tx_pty = 主机→设备方向、真正写进 pty 的字节数
 *   g_dbg_rx_pty = 设备→主机方向、从 pty 读出来的字节数
 *   g_trace      = 前若干条明细日志的配额（避免 ping 时刷屏） */
static volatile uint32_t g_dbg_tx_pty;
static volatile uint32_t g_dbg_rx_pty;
static volatile int      g_trace;

/* pppd 参数。两点注意：
 * ① `ttyname` 在 struct 里是**字符数组**（`char ttyname[TTYNAMSIZ]`），
 *    不是指针 —— 所以只能等 pty 建好、拿到 slave 名字之后再 strlcpy 进去，
 *    在这里没法"指过去"（第一版就是这么写的，编译器报
 *    "initializer element is not computable at load time"）。
 * ② connect/disconnect_script 都给 NULL：那两个脚本是给"AT 指令拨号猫"用的
 *    （走 chat）；我们是 pty 直连、对面就是宿主 pppd，没有猫可拨，给了反而
 *    会去等 AT 响应。pppd 里是 `if (settings->connect_script)` 才跑 chat。 */
static struct pppd_settings_s g_pppd_settings;

/* ── GATT：第二个服务，专门做字节管道 ─────────────────────────────────
 * 单独一个服务而不是塞进原来的服务里：原来的属性索引（sensor=2/ccc=3/
 * cmd=5/text=7）被自检与验收脚本断言着，加在后面容易牵一发动全身。 */

static const struct bt_uuid_128 pw_ppp_svc_uuid = BT_UUID_INIT_128(
  0xf6, 0xe5, 0xd4, 0xc3, 0xb2, 0xa1, 0x90, 0x8f,
  0x5e, 0x4d, 0x2c, 0x1b, 0x00, 0xa1, 0xf1, 0xe0);

static const struct bt_uuid_128 pw_ppp_rx_uuid = BT_UUID_INIT_128(
  0xf6, 0xe5, 0xd4, 0xc3, 0xb2, 0xa1, 0x90, 0x8f,
  0x5e, 0x4d, 0x2c, 0x1b, 0x01, 0xa1, 0xf1, 0xe0);

static const struct bt_uuid_128 pw_ppp_tx_uuid = BT_UUID_INIT_128(
  0xf6, 0xe5, 0xd4, 0xc3, 0xb2, 0xa1, 0x90, 0x8f,
  0x5e, 0x4d, 0x2c, 0x1b, 0x02, 0xa1, 0xf1, 0xe0);

/* 主机 → 设备：整段塞进环形缓冲。
 * 注意只接受 offset==0 —— 长写（Prepare/Execute）在这里没有意义：
 * 宿主按 MTU 分片写就好，PPP 的 HDLC 帧本来就能拼回来。 */
static ssize_t pw_ppp_write(struct bt_conn *conn,
                            const struct bt_gatt_attr *attr, const void *buf,
                            uint16_t len, uint16_t offset, uint8_t flags)
{
  const uint8_t *p = buf;
  unsigned head;
  unsigned i;

  (void)conn;
  (void)attr;
  (void)flags;

  if (offset != 0)
    {
      return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }

  head = g_rx_head;

  for (i = 0; i < len; i++)
    {
      unsigned next = (head + 1u) % PPP_RX_RING;

      if (next == g_rx_tail)
        {
          /* 缓冲满：丢剩下的并记账。绝不在这里等 —— 这是 BT 接收线程。 */
          g_rx_drop += (len - i);
          break;
        }

      g_rx[head] = p[i];
      head = next;
    }

  g_rx_head = head;
  g_rx_total += i;
  return len;
}

static void pw_ppp_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
  (void)attr;
  g_ppp_sub = (value & BT_GATT_CCC_NOTIFY) ? 1 : 0;
  printf("[ppp] host notify %s\n", g_ppp_sub ? "ON" : "OFF");
}

/* 状态读：一行 ASCII，宿主读一次就知道管道/pppd 的现状 */
static ssize_t pw_ppp_status_read(struct bt_conn *conn,
                                  const struct bt_gatt_attr *attr, void *buf,
                                  uint16_t len, uint16_t offset)
{
  char st[192];
  int n;

  n = snprintf(st, sizeof(st),
               "PhyWear ppp up=%u sub=%u slave=%s rx=%u tx=%u drop=%u "
               "ptx=%lu prx=%lu pppd=%d",
               (unsigned)g_ppp_up, (unsigned)g_ppp_sub, g_pty_slave,
               (unsigned)g_rx_total, (unsigned)g_tx_total,
               (unsigned)g_rx_drop, (unsigned long)g_dbg_tx_pty,
               (unsigned long)g_dbg_rx_pty, g_pppd_rc);

  return bt_gatt_attr_read(conn, attr, buf, len, offset, st, (uint16_t)n);
}

static struct bt_gatt_attr pw_ppp_attrs[] =
{
  BT_GATT_PRIMARY_SERVICE(&pw_ppp_svc_uuid.uuid),

  /* 主机写进来的 PPP 字节 */
  BT_GATT_CHARACTERISTIC(&pw_ppp_rx_uuid.uuid,
                         BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
                         BT_GATT_PERM_WRITE,
                         NULL, pw_ppp_write, NULL),

  /* 设备发出去的 PPP 字节（通知）+ 状态读 */
  BT_GATT_CHARACTERISTIC(&pw_ppp_tx_uuid.uuid,
                         BT_GATT_CHRC_NOTIFY | BT_GATT_CHRC_READ,
                         BT_GATT_PERM_READ,
                         pw_ppp_status_read, NULL, NULL),
  BT_GATT_CCC(pw_ppp_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
};

/* 属性索引必须**数清楚**（BT_GATT_CHARACTERISTIC 会展开成两条：
 * 特征声明 + 特征值）：
 *   [0] 主服务
 *   [1] rx 声明   [2] rx 值        （主机写进来的 PPP 字节）
 *   [3] tx 声明   [4] tx 值        ← 通知要发给**这一条**
 *                 [5] tx 的 CCC
 * 2026-09-18 实测教训：最初写的是 2（= rx 值），而 rx 特征只有 WRITE、
 * 没有 NOTIFY ⇒ `bt_gatt_notify()` 每次回 -22 = -EINVAL，设备侧看着
 * "读到了 17/32 B 但发不出去"，宿主一个字节都收不到。 */
#define PW_PPP_ATTR_TX 4

static struct bt_gatt_service pw_ppp_svc = BT_GATT_SERVICE(pw_ppp_attrs);

/* ── pppd 任务：它自己 open(slave)，然后永远循环 ─────────────────────── */

static void pw_ppp_pppd_task(void *a, void *b, void *c)
{
  (void)a;
  (void)b;
  (void)c;

  strlcpy(g_pppd_settings.ttyname, g_pty_slave,
          sizeof(g_pppd_settings.ttyname));
  printf("[ppp] starting pppd on %s\n", g_pppd_settings.ttyname);
  g_pppd_rc = pppd(&g_pppd_settings);
  printf("[ppp] pppd returned %d (0 才是正常退出)\n", g_pppd_rc);
}

/* 两个任务的栈都从**堆**上要（本板堆在 PSRAM，空闲 7.3 MB），不再用
 * K_THREAD_STACK_DEFINE 静态占 SRAM。
 *
 * 为什么改：静态栈是**无条件**占 SRAM 的，而 SRAM 只有 512 KB、本固件已
 * 95%+（实测悬崖在 96~97% 之间）。放堆上以后，"平时不跑 PPP"就不付这份钱；
 * 而且 2048 B 的桥栈**明显不够**（2026-09-18 实测：桥任务在第一次
 * write(pty) 之后整机静默、tx 计数一直为 0 —— 和当初 1 KB 栈那次同型）。
 * 对齐 32 B：pthread_attr_setstack() 对栈指针有对齐要求。 */
#ifndef PW_PPP_BRIDGE_STACK
#define PW_PPP_BRIDGE_STACK 8192
#endif
#ifndef PW_PPP_PPPD_STACK
#define PW_PPP_PPPD_STACK 16384   /* 与 apps/examples/pppd 的 16096 同量级 */
#endif

static void *g_pppd_stack;
static struct k_thread g_pppd_thread;

/* ── 管道任务：pty master 与环形缓冲/通知之间的搬运工 ───────────────── */

static void *g_bridge_stack;
static struct k_thread g_bridge_thread;

/* 任务优先级 —— 注意本端口的 K_PRIO_PREEMPT 是**反的**：
 *   port/include/zephyr/kernel.h: K_PRIO_PREEMPT(x) = CONFIG_NUM_COOP_PRIORITIES + x
 * 而 NuttX 里「数值越大越紧急」，于是 x 越大反而越紧急（与 Zephyr 语义相反）。
 * 桥任务必须**没有** pppd 紧急，否则它那个搬运循环会把 pppd 饿死：
 * 2026-09-18 实测，桥 109 / pppd 108 ⇒ pppd 一行日志都没跑出来
 * （ppp0 也没建），而桥在 slave 未打开时 poll 立刻返回 HUP，转成死循环。
 * 这里把桥压到 pppd 之下，循环里再有 usleep 兜底。 */
#define PW_PPP_PPPD_PRIO    K_PRIO_PREEMPT(8)    /* 108 */
#define PW_PPP_BRIDGE_PRIO  K_PRIO_PREEMPT(6)    /* 106 < 108，让 pppd 先跑 */

static void *pw_ppp_alloc_stack(size_t size)
{
  uintptr_t p = (uintptr_t)malloc(size + 32);

  if (p == 0)
    {
      return NULL;
    }

  return (void *)((p + 31u) & ~(uintptr_t)31u);
}

/* 把 pty 变成**透明字节管道**。
 *
 * NuttX 的 pty 默认是"终端"语义（drivers/serial/pty.c:1079 附近初始化）：
 *   master: pd_oflag = OPOST | OCRNL
 *   slave : pd_oflag = OPOST | ONLCR, pd_lflag = ECHO | ICANON
 * PPP 在这种终端上根本走不通：
 *   · slave 的 ICANON 会把 master 写进去的数据**按行缓存**，而 HDLC 帧里
 *     没有换行符 ⇒ pppd 永远读不到我们发的帧；
 *   · ECHO 会把主机写进去的帧**回显**回主机（对端看到自己的帧）；
 *   · OPOST/ONLCR 把 0x0A 变成 0x0D 0x0A，直接破坏 PPP 的 FCS。
 * 所以 master 与 slave 两侧都要设成 raw。 */
static void pw_ppp_set_raw(int fd)
{
  struct termios tio;

  if (tcgetattr(fd, &tio) < 0)
    {
      printf("[ppp] tcgetattr(fd=%d) FAILED errno=%d\n", fd, errno);
      return;
    }

  tio.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR |
                   ICRNL | IXON | IXOFF);
  tio.c_oflag &= ~OPOST;
  tio.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
  tio.c_cflag &= ~(CSIZE | PARENB);
  tio.c_cflag |= CS8;

  if (tcsetattr(fd, TCSANOW, &tio) < 0)
    {
      printf("[ppp] tcsetattr(fd=%d) FAILED errno=%d\n", fd, errno);
    }
}

static void pw_ppp_bridge_task(void *a, void *b, void *c)
{
  uint8_t buf[PPP_PTY_RD];
  struct pollfd pfd;

  (void)a;
  (void)b;
  (void)c;

  /* 第一行就打"我起来了"：上一版桥任务栈只有 1 KB，而下面 buf[] 就 512 B，
   * 直接栈溢出**整机静默**（没有 panic、没有 assert，串口就停了）——
   * 有这么一行"活着"的标志，下次一眼能分辨"任务没起来"还是"起来后崩了"。 */
  printf("[ppp] bridge task up (stack %u B, from heap)\n",
         (unsigned)PW_PPP_BRIDGE_STACK);

  /* 1) 建 pty。master 归我们，slave 名字交给 pppd 去 open。
   *    NuttX 没实现 grantpt()（include/stdlib.h 里明确写了），跳过它。 */
  g_pty_master = posix_openpt(O_RDWR | O_NOCTTY);
  if (g_pty_master < 0)
    {
      printf("[ppp] posix_openpt FAILED errno=%d\n", errno);
      return;
    }

  if (unlockpt(g_pty_master) < 0)
    {
      printf("[ppp] unlockpt FAILED errno=%d\n", errno);
      return;
    }

  {
    FAR char *name = ptsname(g_pty_master);

    if (name == NULL)
      {
        printf("[ppp] ptsname FAILED errno=%d\n", errno);
        return;
      }

    strncpy(g_pty_slave, name, sizeof(g_pty_slave) - 1);
    g_pty_slave[sizeof(g_pty_slave) - 1] = '\0';
  }

  printf("[ppp] pty master fd=%d slave=%s\n", g_pty_master, g_pty_slave);

  /* 1a) 两侧都设 raw（理由见 pw_ppp_set_raw 的注释）。
   *     slave 由我们先 open 一次设好 termios 再关掉 —— termios 存在 devpair
   *     里，pppd 之后自己 open 时仍是 raw。 */
  pw_ppp_set_raw(g_pty_master);
  g_pty_slave_fd = open(g_pty_slave, O_RDWR | O_NOCTTY);
  if (g_pty_slave_fd < 0)
    {
      printf("[ppp] 打不开 slave %s errno=%d\n", g_pty_slave, errno);
    }
  else
    {
      pw_ppp_set_raw(g_pty_slave_fd);
      printf("[ppp] pty 已设 raw（master+slave 两侧）slave fd=%d\n",
             g_pty_slave_fd);

      /* 这个 slave fd **一直开着不关**。两点原因：
       *  ① pty 的 master 在「对端没有开着的 slave」时，write() 直接回
       *     EPIPE（实测 errno=32）—— pppd 起来之前我们写的每一帧都会失败；
       *  ② 我们只是**持着它**，从不 read()，所以不会跟 pppd 抢数据
       *     （NuttX 的 pty 是 devpair 内部一条管道，谁 read 谁消费）。 */
    }

  /* 打 pppd 之前先把两个 open 都试一遍并打印 errno：pppd() 失败只回一个
   * 光秃秃的 2（tun 或 tty 打不开都回 2），不打这两行就没法判断卡在哪。 */
  {
    int tfd = open("/dev/tun", O_RDWR);

    printf("[ppp] probe open(/dev/tun) => %d errno=%d\n",
           tfd, tfd < 0 ? errno : 0);
    if (tfd >= 0)
      {
        close(tfd);
      }
  }

  /* 1b) 等主机订阅再拉 pppd。
   *
   * 为什么必须等：pppd 一起来就会往 pty 里灌 LCP Configure-Request，而本桥
   * 在**没人订阅**时是直接丢的（PPP 会重传，堆着没意义）。NuttX pppd 的
   * LCP 是 5 s 一次、共 5 次（ppp_conf.h: LCP_TIMEOUT/LCP_RETRY_COUNT），
   * 也就是说主机若在 ~25 s 内没连上并订阅，设备侧 LCP 就进 LCP_TX_TIMEOUT
   * 不再发请求 —— 那之后再连也没用了。所以顺序反过来：**先等订阅，再拉
   * pppd**，把这段竞态从"要抢时间"变成"不可能发生"。
   *
   * 等待期间每 2 s 打一行，方便取证时确认"到底卡在哪一步"。
   * 超时（180 s）也照样往下走：万一主机就是想手动试，不该被设备卡住。 */
  {
    int waited = 0;

    while (!g_ppp_sub && waited < 180)
      {
        if ((waited % 10) == 0)
          {
            printf("[ppp] 等主机订阅 …a102（已等 %d s；主机侧跑 "
                   "pw_bt_ppp.py --peer 或 pw_bt_ppp.py）\n", waited);
          }

        usleep(1000000);
        waited++;
      }

    printf("[ppp] 主机订阅状态 sub=%d（等了 %d s）→ 拉起 pppd\n",
           (int)g_ppp_sub, waited);
  }

  /* 2) pppd 自己的任务（阻塞式无限循环）。名字先填好再建任务。
   *    栈从堆上要（见 pw_ppp_alloc_stack 的注释）。 */
  g_pppd_stack = pw_ppp_alloc_stack(PW_PPP_PPPD_STACK);
  if (g_pppd_stack == NULL)
    {
      printf("[ppp] pppd 栈 malloc(%d) 失败 errno=%d\n",
             PW_PPP_PPPD_STACK, errno);
      return;
    }

  k_thread_create(&g_pppd_thread, g_pppd_stack, PW_PPP_PPPD_STACK,
                  pw_ppp_pppd_task, NULL, NULL, NULL,
                  PW_PPP_PPPD_PRIO, 0, K_NO_WAIT);
  k_thread_name_set(&g_pppd_thread, "pw_pppd");

  g_ppp_up = 1;
  pfd.fd = g_pty_master;
  pfd.events = POLLIN;

  /* 3) 双向搬运 */
  for (; ; )
    {
      int progress = 0;

      /* 3a) 主机 → 设备：把环形缓冲抽干写进 pty */
      while (g_rx_tail != g_rx_head)
        {
          unsigned tail = g_rx_tail;
          unsigned n = 0;
          ssize_t w;

          while (n < sizeof(buf) && tail != g_rx_head)
            {
              buf[n++] = g_rx[tail];
              tail = (tail + 1u) % PPP_RX_RING;
            }

          w = write(g_pty_master, buf, n);
          if (w <= 0)
            {
              if (g_trace < 20)
                {
                  printf("[ppp] pty write FAILED n=%u rc=%d errno=%d\n",
                         n, (int)w, errno);
                  g_trace++;
                }

              break;                    /* 写不进去就下一轮再试，别丢 */
            }

          g_rx_tail = (g_rx_tail + (unsigned)w) % PPP_RX_RING;
          progress = 1;
          g_dbg_tx_pty += (uint32_t)w;

          if (g_trace < 20)
            {
              printf("[ppp] pty write n=%u rc=%d（累计 %lu B）\n",
                     n, (int)w, (unsigned long)g_dbg_tx_pty);
              g_trace++;
            }
        }

      /* 3b) 设备 → 主机：pty 有数据就通知出去 */
      pfd.revents = 0;
      if (poll(&pfd, 1, progress ? 0 : 20) > 0 && (pfd.revents & POLLIN))
        {
          ssize_t r = read(g_pty_master, buf, sizeof(buf));

          if (r > 0)
            {
              progress = 1;
              g_dbg_rx_pty += (uint32_t)r;

              if (g_trace < 40)
                {
                  printf("[ppp] pty read r=%d sub=%u（累计 %lu B）\n",
                         (int)r, (unsigned)g_ppp_sub,
                         (unsigned long)g_dbg_rx_pty);
                  g_trace++;
                }

              if (g_ppp_sub)
                {
                  ssize_t sent = 0;

                  while (sent < r)
                    {
                      size_t chunk = (size_t)(r - sent);
                      int nrc;

                      if (chunk > PPP_TX_CHUNK)
                        {
                          chunk = PPP_TX_CHUNK;
                        }

                      nrc = bt_gatt_notify(NULL, &pw_ppp_attrs[PW_PPP_ATTR_TX],
                                           buf + sent, chunk);
                      if (nrc < 0)
                        {
                          if (g_trace < 60)
                            {
                              printf("[ppp] notify FAILED rc=%d\n", nrc);
                              g_trace++;
                            }

                          break;
                        }

                      sent += (ssize_t)chunk;
                    }

                  g_tx_total += (uint32_t)sent;
                }
              else
                {
                  /* 没人订阅就先丢：PPP 会重传，比在这儿堆积更划算 */
                  g_rx_drop += 0;
                }
            }
        }

      /* 空转必须**让出 CPU**：pty 的 slave 还没被 pppd 打开时，poll() 会因为
       * HUP 立刻返回（不阻塞），这个 for(;;) 就变成满速死循环 —— 实测会把
       * 优先级更低的 pppd 饿死到一行都跑不出来。这里兜底 sleep 2 ms。 */
      if (!progress)
        {
          usleep(2000);
        }
    }
}

/* ── 对外入口 ───────────────────────────────────────────────────────── */

int pw_btppp_start(void)
{
  int rc;

  if (g_ppp_up)
    {
      printf("[ppp] already running (slave=%s)\n", g_pty_slave);
      return 0;
    }

  rc = bt_gatt_service_register(&pw_ppp_svc);
  printf("[ppp] gatt service register rc=%d %s\n", rc,
         rc == 0 ? "(ok)" : "(FAILED)");
  if (rc != 0)
    {
      return rc;
    }

  g_bridge_stack = pw_ppp_alloc_stack(PW_PPP_BRIDGE_STACK);
  if (g_bridge_stack == NULL)
    {
      printf("[ppp] 桥栈 malloc(%d) 失败 errno=%d\n",
             PW_PPP_BRIDGE_STACK, errno);
      return -ENOMEM;
    }

  k_thread_create(&g_bridge_thread, g_bridge_stack, PW_PPP_BRIDGE_STACK,
                  pw_ppp_bridge_task, NULL, NULL, NULL,
                  PW_PPP_BRIDGE_PRIO, 0, K_NO_WAIT);
  k_thread_name_set(&g_bridge_thread, "pw_btppp");

  /* ── 必须留在这个 task_group 里：本函数**不能返回** ──────────────────
   *
   * zblue 的 `k_thread_create()` 是 `pthread_create()`（port/kernel/thread.c），
   * 所以桥线程与 pppd 线程都挂**当前 task_group** 下；而 NuttX 的 fd 表也是
   * 按 group 走的（`nxsched_get_fdlist()` → `&group->tg_fdlist`）。
   * `phywear` 主任务一旦从 main() 返回，整个 group 就被销毁 —— 桥与 pppd
   * 两个 pthread 随之消失。表现就是：pty 建好、pppd 打印完
   * "starting pppd" 之后串口**再无任何输出**。
   *
   * 2026-09-18 那一轮我把它归因成"pppd 栈不足→硬故障"，是**没有对照的猜测**
   * （当时 main 直接 return 0）。现在改成不返回，用来判定是不是这个原因。
   *
   * 代价：本命令会一直占着调用它的任务，所以要用**后台**方式跑：
   *     phywear btppp &
   * NSH 打印 PID 后立刻回到提示符，`ifconfig` / `ping` 继续可用。 */
  printf("[ppp] 提示：本命令不会返回（PPP 要长期挂着跑），请用 "
         "'phywear btppp &' 后台运行\n");

  for (; ; )
    {
      usleep(1000000);
    }
}

#else  /* !PW_BT_PPP */

/* 没编进来时也留一个入口：`phywear btppp` 能给出"为什么没有"，而不是静默无事发生 */
int pw_btppp_start(void)
{
  printf("[ppp] 本固件未编入 PPP over BLE（PW_BT_PPP=0）\n");
  printf("[ppp] 原因：NuttX pppd 需要 ~16 KB 栈 + 桥 2 KB ≈ 18 KB SRAM，\n");
  printf("[ppp]       而本板在 SRAM 96.76%% 时就会开机挂死（实测悬崖）。\n");
  printf("[ppp] 详见 docs/16 §13.6e 与 docs/evidence/bt-ppp-20260918/\n");
  return -ENOSYS;
}

#endif /* PW_BT_PPP */
