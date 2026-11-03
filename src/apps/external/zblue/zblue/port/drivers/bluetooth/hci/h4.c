/******************************************************************************
 *
 * Copyright (C) 2024 Xiaomi Corporation
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
 *
 *****************************************************************************/

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/sys/util.h>

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <poll.h>
#include <unistd.h>
#include <limits.h>
#include <pthread.h>
#include <debug.h>
#include <nuttx/sched.h>
#include <zephyr/sys/byteorder.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/drivers/bluetooth.h>

extern void btsnoop_log_capture(uint8_t is_receive, uint8_t *hci_pkt, uint32_t hci_pkt_size);

#define LOG_LEVEL CONFIG_BT_HCI_DRIVER_LOG_LEVEL
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(bt_driver);

#define DT_DRV_COMPAT zephyr_bt_hci_ttyHCI
#define RX_THREAD_STACK_USED_MODE_SIZE    (3072)
#define RX_THREAD_STACK_DEBUG_MODE_SIZE   (4096)

struct h4_data {
	int fd;
	pthread_mutex_t mutex;
	bt_hci_recv_t recv;
	void *hci_data;
	struct k_thread rx_thread_data;
	/* PhyWear B1 修复：待发队列。
	 * 为什么需要它：bt_tx_irq_raise() 走 k_work_submit(&hdev->tx_work)，
	 * 于是 h4_send() 跑在 **Zephyr 系统工作队列线程**里；而 NuttX 的 fd 表
	 * 挂在 task_group 上（fs/inode/fs_files.c: file_get2 → nxsched_get_fdlist
	 * → tcb->group->tg_fdlist），系统工作队列是 z_sys_init() 在板级启动阶段
	 * 那个任务组里 pthread_create 出来的 —— 它的 fd 表里**没有** h4_open()
	 * 打开的 fd，write() 直接 EBADF（errno 9）。
	 * 所以：h4_send() 只把 buf 挂进这个队列，真正的 fd I/O 全部交给
	 * h4_rx_thread（它是 h4_open() 亲手创建的，与 open 同组、共享 fd 表）。 */
	struct k_fifo tx_fifo;

	/* RX 连续读错误计数（瞬时错误重试用；成功一次就清零） */
	unsigned int rx_errs;
#if (CONFIG_BLUETOOTH_SERVICE_LOG_LEVEL > 2 || CONFIG_BT_DEBUG_LOG > 2)
	K_KERNEL_STACK_DEFINE(rx_thread_stack, RX_THREAD_STACK_DEBUG_MODE_SIZE);
#else
	K_KERNEL_STACK_DEFINE(rx_thread_stack, RX_THREAD_STACK_USED_MODE_SIZE);
#endif //(CONFIG_BLUETOOTH_SERVICE_LOG_LEVEL > 2 || CONFIG_BT_DEBUG_LOG > 2)
	uint8_t frame[1026];
};

#define HCI_DEBUG 0

/* ── H4 层字节级收发日志开关（PhyWear） ────────────────────────────────
 * 定位 B1 根因时把它置 1，就能在串口上看到**真正写进 /dev/ttyHCI0 的 H4 帧**
 * 与**从 fd 上读回的原始字节**（例如 TX 01 03 0c 00 → RX 04 0e 04 06 03 0c 00）。
 * 置 0 时所有调用点被宏消掉，零 flash / 零 SRAM 开销。
 * 另外 `nxsched_get_fdlist()` 打印用于确认"谁在写、在哪个 task_group"——
 * 这正是本次根因的决定性证据（见 docs/16）。
 *
 * 2026-09-18：B3 又靠它抓到一个只在空口上看得见的 bug —— 通知帧的
 * value handle 是 0x0000（zblue gatt_notify_mc 漏 data.handle）。
 * 证据存档 docs/evidence/bt-gatt-handle-20260918/，正式固件归 0。
 */
#define PW_H4_TRACE 0

/* RX 连续读失败多少次才放弃线程（见 h4_rx_thread 里的错误处理） */
#define PW_H4_RX_ERR_MAX 50

#if PW_H4_TRACE
#define PW_H4_DUMP_MAX 64

static void pw_h4_dump(const char *tag, const uint8_t *p, size_t n)
{
	size_t i;

	printf("[H4] %s (%u B):", tag, (unsigned int)n);
	for (i = 0; i < n && i < PW_H4_DUMP_MAX; i++) {
		printf(" %02x", p[i]);
	}

	if (n > PW_H4_DUMP_MAX) {
		printf(" ...(+%u B)", (unsigned int)(n - PW_H4_DUMP_MAX));
	}

	printf("\n");
}

static void pw_h4_whoami(const char *tag, const struct h4_data *h4)
{
	printf("[H4T] %-12s tid=%d fdlist=%p h4->fd=%d\n", tag, (int)gettid(),
	       (void *)nxsched_get_fdlist(), h4->fd);
}
#else
#define pw_h4_dump(tag, p, n)  do { } while (0)
#define pw_h4_whoami(tag, h4)  do { } while (0)
#endif

static void h4_data_dump(const char *tag, uint8_t type, uint8_t *data, uint32_t len)
{
#if HCI_DEBUG
	struct iovec bufs[2];

	bufs[0].iov_base = &type;
	bufs[0].iov_len = 1;
	bufs[1].iov_base = data;
	bufs[1].iov_len = len;

	lib_dumpvbuffer(tag, bufs, 2);
#endif
}

static int h4_send_data(struct h4_data *h4, uint8_t *buf, size_t count)
{
	ssize_t ret, nwritten = 0;

	while (nwritten != count) {
		ret = write(h4->fd, buf + nwritten, count - nwritten);
		if (ret < 0) {
			if (ret == -EAGAIN) {
				usleep(500);
				continue;
			} else {
				return ret;
			}
		}

		nwritten += ret;
	}

	return nwritten;
}

static struct net_buf *get_rx(const uint8_t *buf)
{
	bool discardable = false;
	k_timeout_t timeout = K_FOREVER;

	switch (buf[0]) {
	case BT_HCI_H4_EVT:
		if (buf[1] == BT_HCI_EVT_LE_META_EVENT &&
		    (buf[3] == BT_HCI_EVT_LE_ADVERTISING_REPORT)) {
			discardable = true;
			timeout = K_NO_WAIT;
		}

		return bt_buf_get_evt(buf[1], discardable, timeout);
	case BT_HCI_H4_ACL:
		return bt_buf_get_rx(BT_BUF_ACL_IN, K_FOREVER);
	case BT_HCI_H4_ISO:
		if (IS_ENABLED(CONFIG_BT_ISO)) {
			return bt_buf_get_rx(BT_BUF_ISO_IN, K_FOREVER);
		}
		__fallthrough;
	default:
		LOG_ERR("Unknown packet type: %u", buf[0]);
	}

	return NULL;
}

/**
 * @brief Decode the length of an HCI H4 packet and check it's complete
 * @details Decodes packet length according to Bluetooth spec v5.4 Vol 4 Part E
 * @param buf	Pointer to a HCI packet buffer
 * @param buf_len	Bytes available in the buffer
 * @return Length of the complete HCI packet in bytes, -1 if cannot find an HCI
 *         packet, 0 if more data required.
 */
static int32_t hci_packet_complete(const uint8_t *buf, uint16_t buf_len)
{
	uint16_t payload_len = 0;
	const uint8_t type = buf[0];
	uint8_t header_len = sizeof(type);
	const uint8_t *hdr = &buf[sizeof(type)];

	switch (type) {
	case BT_HCI_H4_CMD: {
		if (buf_len < header_len + BT_HCI_CMD_HDR_SIZE) {
			return 0;
		}
		const struct bt_hci_cmd_hdr *cmd = (const struct bt_hci_cmd_hdr *)hdr;

		/* Parameter Total Length */
		payload_len = cmd->param_len;
		header_len += BT_HCI_CMD_HDR_SIZE;
		break;
	}
	case BT_HCI_H4_ACL: {
		if (buf_len < header_len + BT_HCI_ACL_HDR_SIZE) {
			return 0;
		}
		const struct bt_hci_acl_hdr *acl = (const struct bt_hci_acl_hdr *)hdr;

		/* Data Total Length */
		payload_len = sys_le16_to_cpu(acl->len);
		header_len += BT_HCI_ACL_HDR_SIZE;
		break;
	}
	case BT_HCI_H4_SCO: {
		if (buf_len < header_len + BT_HCI_SCO_HDR_SIZE) {
			return 0;
		}
		const struct bt_hci_sco_hdr *sco = (const struct bt_hci_sco_hdr *)hdr;

		/* Data_Total_Length */
		payload_len = sco->len;
		header_len += BT_HCI_SCO_HDR_SIZE;
		break;
	}
	case BT_HCI_H4_EVT: {
		if (buf_len < header_len + BT_HCI_EVT_HDR_SIZE) {
			return 0;
		}
		const struct bt_hci_evt_hdr *evt = (const struct bt_hci_evt_hdr *)hdr;

		/* Parameter Total Length */
		payload_len = evt->len;
		header_len += BT_HCI_EVT_HDR_SIZE;
		break;
	}
	case BT_HCI_H4_ISO: {
		if (buf_len < header_len + BT_HCI_ISO_HDR_SIZE) {
			return 0;
		}
		const struct bt_hci_iso_hdr *iso = (const struct bt_hci_iso_hdr *)hdr;

		/* ISO_Data_Load_Length parameter */
		payload_len = bt_iso_hdr_len(sys_le16_to_cpu(iso->len));
		header_len += BT_HCI_ISO_HDR_SIZE;
		break;
	}
	/* If no valid packet type found */
	default:
		LOG_WRN("Unknown packet type 0x%02x", type);
		return -1;
	}

	/* Request more data */
	if (buf_len < header_len + payload_len) {
		return 0;
	}

	return (int32_t)header_len + payload_len;
}

#if 1
static bool h4_ready(struct h4_data *h4)
{
	struct pollfd pollfd = { .fd = h4->fd, .events = POLLIN };

	return (poll(&pollfd, 1, 0) == 1);
}
#endif

static int h4_hw_tx(struct h4_data *h4, struct net_buf *buf);

static void h4_rx_thread(void *p1, void *p2, void *p3)
{
	const struct device *dev = p1;
	struct h4_data *h4 = dev->data;

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

#if PW_H4_TRACE
	printf("[H4] rx thread started\n");
#endif
	pw_h4_whoami("h4_rx_thread", h4);
	ssize_t frame_size = 0;

	while (1) {
		struct net_buf *buf;
		size_t buf_tailroom;
		size_t buf_add_len;
		ssize_t len;
		const uint8_t *frame_start = h4->frame;

		/* 先清 TX 队列：本线程与 h4_open() 同组，只有它能合法写这个 fd。 */
		while ((buf = k_fifo_get(&h4->tx_fifo, K_NO_WAIT)) != NULL) {
			h4_hw_tx(h4, buf);
		}

#if 1
		if (!h4_ready(h4)) {
			usleep(1000);
			continue;
		}
#endif

		pw_h4_whoami("rx read", h4);
		LOG_DBG("calling read()");

		len = read(h4->fd, h4->frame + frame_size, sizeof(h4->frame) - frame_size);
		if (len < 0) {
			if (errno == EINTR) {
				continue;
			}

			/* ⚠️ 原代码这里是 `if (len == -EAGAIN)` —— **永远为假**：
			 * read() 出错返回 -1，EAGAIN 在 errno 里。于是任何一次瞬时读错误
			 * 都会直接掉到下面的 close(fd) + return。
			 * 而本驱动现在把 **TX 也交给这个线程**（见 h4_send 的注释），
			 * 所以一旦走到那里：RX 线程没了、fd 关了、TX 也发不出去 ——
			 * 整个蓝牙栈静默死亡。更糟的是出货配置没开 CONFIG_BT_DEBUG_LOG，
			 * LOG_ERR 被编译成空语句，连一行提示都没有，B3 时只会表现为
			 * "手机突然连不上/读不到"，极难归因。
			 * 现在：EAGAIN 重试；其它错误只累计"连续失败"，先重试，
			 * 连续 PW_H4_RX_ERR_MAX 次才放弃，并且用**无条件 printf** 留原文。 */
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				usleep(500);
				continue;
			}

			if (++h4->rx_errs < PW_H4_RX_ERR_MAX) {
				printf("[H4] rx read errno=%d (第 %u 次)，重试\n", errno,
				       (unsigned int)h4->rx_errs);
				usleep(1000);
				continue;
			}

			printf("[H4] rx read 连续失败 %u 次 (errno=%d) —— 放弃并退出 RX 线程；"
			       "此后 RX / TX 都会停\n", (unsigned int)h4->rx_errs, errno);
			close(h4->fd);
			h4->fd = -1;
			return;
		}

		h4->rx_errs = 0;

		frame_size += len;

		/* PhyWear 诊断：fd 上真实收到的原始字节（未分帧） */
		if (len > 0) {
			pw_h4_dump("RX raw", h4->frame + frame_size - len, (size_t)len);
		}

		while (frame_size > 0) {
			const uint8_t *buf_add;
			const uint8_t packet_type = frame_start[0];
			const int32_t decoded_len = hci_packet_complete(frame_start, frame_size);

			if (decoded_len == -1) {
				LOG_ERR("HCI Packet type is invalid, length could not be decoded");
				frame_size = 0; /* Drop buffer */
				break;
			}

			if (decoded_len == 0) {
				if (frame_size == sizeof(h4->frame)) {
					LOG_ERR("HCI Packet (%d bytes) is too big for frame (%d "
						"bytes)",
						decoded_len, sizeof(h4->frame));
					frame_size = 0; /* Drop buffer */
					break;
				}
				if (frame_start != h4->frame) {
					memmove(h4->frame, frame_start, frame_size);
				}
				/* Read more */
				break;
			}

			buf_add = frame_start + sizeof(packet_type);
			buf_add_len = decoded_len - sizeof(packet_type);

			buf = get_rx(frame_start);

			btsnoop_log_capture(1, (uint8_t *)frame_start, decoded_len);

			frame_size -= decoded_len;
			frame_start += decoded_len;

			if (!buf) {
				LOG_DBG("Discard adv report due to insufficient buf");
				continue;
			}

			buf_tailroom = net_buf_tailroom(buf);
			if (buf_tailroom < buf_add_len) {
				LOG_ERR("Not enough space in buffer %zu/%zu", buf_add_len,
					buf_tailroom);
				net_buf_unref(buf);
				continue;
			}

			net_buf_add_mem(buf, buf_add, buf_add_len);

			LOG_DBG("Calling bt_recv(%p)", buf);

			h4_data_dump("BT RX", packet_type, buf->data, buf_add_len);
			h4->recv(dev, buf, h4->hci_data);
		}
	}
}

/* 真正把一帧 H4 写进 fd 的地方。**只允许 h4_rx_thread 调用**：
 * 只有它和 h4_open() 在同一个 NuttX task_group 里、共享 fd 表。 */
static int h4_hw_tx(struct h4_data *h4, struct net_buf *buf)
{
	int len = (int)buf->len;
	int ret;

	pw_h4_whoami("h4_hw_tx", h4);
	/* PhyWear 诊断：真正写进 /dev/ttyHCI0 的 H4 帧（type + payload） */
	pw_h4_dump("TX", buf->data, (size_t)buf->len);
	btsnoop_log_capture(0, buf->data, buf->len);

	ret = h4_send_data(h4, buf->data, buf->len);
#if PW_H4_TRACE
	printf("[H4] h4_send_data -> %d (want %d, errno %d)\n", ret, len, errno);
#endif

	net_buf_unref(buf);
	return ret == len ? 0 : -EINVAL;
}

static int h4_send(const struct device *dev, struct net_buf *buf)
{
	struct h4_data *h4 = dev->data;

	LOG_DBG("buf %p type %u len %u", buf, bt_buf_get_type(buf), buf->len);
	pw_h4_whoami("h4_send", h4);

	switch (bt_buf_get_type(buf)) {
	case BT_BUF_ACL_OUT:
		net_buf_push_u8(buf, BT_HCI_H4_ACL);
		break;
	case BT_BUF_CMD:
		net_buf_push_u8(buf, BT_HCI_H4_CMD);
		break;
	case BT_BUF_ISO_OUT:
		if (IS_ENABLED(CONFIG_BT_ISO)) {
			net_buf_push_u8(buf, BT_HCI_H4_ISO);
			break;
		}
		__fallthrough;
	default:
		LOG_ERR("Unknown buffer type");
		return -EINVAL;
	}

	h4_data_dump("BT TX", buf->data[0], buf->data + 1, buf->len - 1);

	/* 注意：**不要**在这里 write()。本函数跑在 Zephyr 系统工作队列线程里，
	 * 那个 NuttX task_group 的 fd 表里没有 h4->fd（实测 write() → EBADF）。
	 * 挂进 tx_fifo，由 h4_rx_thread（与 h4_open 同组）真正写出去。 */
	k_fifo_put(&h4->tx_fifo, buf);

	return 0;
}

static int h4_open(const struct device *dev, bt_hci_recv_t recv, void *hci_data)
{
	int ret;
	struct h4_data *h4 = dev->data;
	char dev_name[32];

	if (dev->name == NULL) {
		LOG_ERR("No device name");
		return -EINVAL;
	}

	ret = snprintf(dev_name, sizeof(dev_name), "%s", dev->name);
	if (ret < 0 || ret >= sizeof(dev_name)) {
		LOG_ERR("dev_name:%s snprintf failed, ret %d, ", dev->name, ret);
		return -EINVAL;
	}

	ret = open(dev_name, O_RDWR | O_BINARY | O_CLOEXEC);
	if (ret < 0) {
		goto bail;
	}

	h4->fd = ret;
#if PW_H4_TRACE
	printf("[H4] open %s -> fd %d\n", dev_name, h4->fd);
#endif
	pw_h4_whoami("h4_open", h4);

	k_fifo_init(&h4->tx_fifo);
	LOG_DBG("H4: %s opened as fd %d", dev_name, h4->fd);

	ret = (int)k_thread_create(&h4->rx_thread_data, h4->rx_thread_stack,
				   K_THREAD_STACK_SIZEOF(h4->rx_thread_stack), h4_rx_thread, (void *)dev, NULL,
				   NULL, K_PRIO_COOP(CONFIG_BT_RX_PRIO), 0, K_NO_WAIT);

	if (ret < 0) {
		goto bail;
	} else {
		ret = 0;
	}

	h4->recv = recv;
	h4->hci_data = hci_data;

	ret = snprintf(dev_name, sizeof(dev_name), "BT Driver %s", dev->name);
	if (ret < 0 || ret >= sizeof(dev_name)) {
		LOG_ERR("dev_name:%s snprintf failed, ret %d, ", dev->name, ret);
		return -EINVAL;
	}

	k_thread_name_set(&h4->rx_thread_data, dev_name);
	LOG_DBG("returning");

	return 0;

bail:
#if PW_H4_TRACE
	printf("[H4] open %s FAILED ret=%d errno=%d\n", dev_name, ret, errno);
#endif
	close(h4->fd);
	h4->fd = -1;

	return ret;
}

static int h4_close(const struct device *dev)
{
	struct h4_data *h4 = dev->data;

	LOG_DBG("close h4");

	k_thread_abort(&h4->rx_thread_data);

	close(h4->fd);
	h4->fd = -1;

	return 0;
}

static const struct bt_hci_driver_api h4_drv_api = {
	.open = h4_open,
	.close = h4_close,
	.send = h4_send,
};

static int h4_init(const struct device *dev)
{
	LOG_INF("Bluetooth H4 driver %s", dev->name);

	return 0;
}

#define DT_HCI_INST(node, inst) DT_CAT(node, inst)

#define H4_DEVICE_INIT(inst)                                                                       \
	static struct h4_data h4_data_##inst = {                                                   \
		.fd = -1,                                                                             \
		.mutex = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP,                                   \
	};                                                                                         \
	DEVICE_DT_DEFINE(DT_HCI_INST(DT_DRV_INST(inst), inst), h4_init, NULL, &h4_data_##inst, NULL, POST_KERNEL,             \
			      CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &h4_drv_api)

H4_DEVICE_INIT(0);
H4_DEVICE_INIT(1);
