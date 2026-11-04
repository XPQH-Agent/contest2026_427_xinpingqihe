/****************************************************************************
 * apps/examples/phywear/pw_store.c
 *
 * 掉电保留设置：NOR config 分区最后一个擦除块，magic + CRC32。
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <syslog.h>
#include <unistd.h>

#include <nuttx/mtd/mtd.h>

#include "pw_store.h"

#define PW_STORE_DEV    "/dev/config0"
#define PW_STORE_MAGIC  0x50575354u  /* 'P''W''S''T' */
#define PW_STORE_VER    1

struct pw_store_blob_s
{
  uint32_t magic;
  uint16_t ver;
  uint16_t len;
  struct pw_store_state_s st;
  uint32_t crc;                /* 覆盖前面全部字节 */
};

static uint32_t pw_store_crc32(const void *data, size_t len)
{
  const uint8_t *p = (const uint8_t *)data;
  uint32_t crc = 0xffffffffu;
  size_t i;
  int b;

  for (i = 0; i < len; i++)
    {
      crc ^= p[i];
      for (b = 0; b < 8; b++)
        {
          crc = (crc >> 1) ^ (0xedb88320u & (uint32_t)(-(int32_t)(crc & 1)));
        }
    }

  return ~crc;
}

/* 找一个可以安全使用的擦除块：
 *   从分区尾部往前扫最多 PW_STORE_SCAN 块，
 *   - 块首有我们的 magic  → 就是它（覆盖写）
 *   - 块首 256 B 全 0xFF → 候选空白块（首次使用）
 *   都找不到就返回 -ENOSPC，绝不碰别人的数据。
 * 2026-11-04 实测：config 分区的"最后一块"被 SiFli 自己的记录占着，
 * 原实现"只用最后一块"会一直写不进去，故改为向后扫。
 */

#define PW_STORE_SCAN  4096   /* 全分区扫（1024 块实测 32 块不够，尾部全被 SiFli 占了） */

static int pw_store_locate(int fd, const struct mtd_geometry_s *geo,
                           off_t *blkoff, bool *is_ours)
{
  uint8_t probe[256];
  off_t blank_off = -1;
  int i;

  for (i = 0; i < PW_STORE_SCAN && i < geo->neraseblocks; i++)
    {
      off_t off = (off_t)(geo->neraseblocks - 1 - i) * geo->erasesize;
      struct pw_store_blob_s head;
      size_t n = sizeof(probe);
      ssize_t got;
      bool blank = true;
      size_t k;

      if (n > geo->erasesize)
        {
          n = geo->erasesize;
        }

      got = pread(fd, probe, n, off);
      if (got <= 0)
        {
          continue;
        }

      for (k = 0; k < (size_t)got; k++)
        {
          if (probe[k] != 0xff)
            {
              blank = false;
              break;
            }
        }

      if (blank)
        {
          if (blank_off < 0)
            {
              blank_off = off;
            }

          continue;
        }

      if (pread(fd, &head, sizeof(head), off) == (ssize_t)sizeof(head) &&
          head.magic == PW_STORE_MAGIC)
        {
          *blkoff  = off;
          *is_ours = true;
          return 0;
        }
    }

  if (blank_off >= 0)
    {
      *blkoff  = blank_off;
      *is_ours = false;
      return 0;
    }

  return -ENOSPC;
}

/* 打开 config 分区并算好"最后一个擦除块"的偏移 */

static int pw_store_open(off_t *blkoff, size_t *blksize, bool *is_ours)
{
  struct mtd_geometry_s geo;
  int fd;
  int ret;

  fd = open(PW_STORE_DEV, O_RDWR);
  if (fd < 0)
    {
      return -errno;
    }

  memset(&geo, 0, sizeof(geo));
  if (ioctl(fd, MTDIOC_GEOMETRY, (unsigned long)((uintptr_t)&geo)) < 0)
    {
      int err = -errno;
      close(fd);
      return err;
    }

  if (geo.neraseblocks < 1 || geo.erasesize < sizeof(struct pw_store_blob_s))
    {
      close(fd);
      return -ENOSPC;
    }

  ret = pw_store_locate(fd, &geo, blkoff, is_ours);
  if (ret < 0)
    {
      close(fd);
      return ret;
    }

  *blksize = geo.erasesize;
  return fd;
}

static int pw_store_read_blob(struct pw_store_blob_s *blob)
{
  off_t off;
  size_t size;
  int fd;
  int ret;

  {
    bool ours;

    fd = pw_store_open(&off, &size, &ours);
    if (fd < 0)
      {
        return fd;
      }

    if (!ours)
      {
        close(fd);       /* 只有空白块：没有记录 */
        return -ENOENT;
      }
  }

  memset(blob, 0, sizeof(*blob));
  ret = pread(fd, blob, sizeof(*blob), off);
  close(fd);

  if (ret != (int)sizeof(*blob))
    {
      return -EIO;
    }

  if (blob->magic != PW_STORE_MAGIC || blob->ver != PW_STORE_VER ||
      blob->len != sizeof(struct pw_store_state_s))
    {
      return -ENOENT;
    }

  if (blob->crc != pw_store_crc32(blob, offsetof(struct pw_store_blob_s, crc)))
    {
      return -EIO;
    }

  return 0;
}

int pw_store_load(struct pw_store_state_s *st)
{
  struct pw_store_blob_s blob;
  int ret;

  if (st == NULL)
    {
      return -EINVAL;
    }

  ret = pw_store_read_blob(&blob);
  if (ret < 0)
    {
      return ret;
    }

  *st = blob.st;
  return 0;
}

int pw_store_save(const struct pw_store_state_s *st)
{
  struct pw_store_blob_s blob;
  struct pw_store_blob_s old;
  struct mtd_erase_s erase;
  struct mtd_geometry_s geo;
  bool blank = true;
  off_t off;
  size_t size;
  size_t i;
  int fd;
  int ret;

  if (st == NULL)
    {
      return -EINVAL;
    }

  /* 内容没变就不写，省一次擦除 */

  if (pw_store_read_blob(&old) == 0 &&
      memcmp(&old.st, st, sizeof(*st)) == 0)
    {
      return 0;
    }

  bool ours = false;

  fd = pw_store_open(&off, &size, &ours);
  if (fd < 0)
    {
      return fd;
    }

  /* 写之前再确认一次：定位到的块要么已经是我们的记录，要么是空白块。
   * 这份护栏保证即使分区布局变了，也绝不会覆盖 SiFli 的校准/蓝牙记录。
   */

  if (!ours)
    {
      static uint8_t probe[64];
      size_t left = size < sizeof(probe) ? size : sizeof(probe);
      ssize_t n = pread(fd, probe, left, off);

      if (n <= 0)
        {
          close(fd);
          return -EIO;
        }

      blank = true;
      for (i = 0; i < (size_t)n; i++)
        {
          if (probe[i] != 0xff)
            {
              blank = false;
              break;
            }
        }

      if (!blank)
        {
          close(fd);
          syslog(LOG_WARNING, "[store] 目标块非空白，放弃写入\n");
          return -EBUSY;
        }
    }

  /* 不论空白与否都先擦一次：实测"空白块直接 pwrite"返回成功但读回仍是 0xFF，
   * NOR 驱动看起来要求显式擦除后才允许编程。擦的是我们自己定位到的块，
   * 真空白块擦除也是无副作用的。
   */

  if (1)
    {
      memset(&geo, 0, sizeof(geo));
      if (ioctl(fd, MTDIOC_GEOMETRY, (unsigned long)((uintptr_t)&geo)) < 0 ||
          geo.erasesize == 0)
        {
          close(fd);
          return -EIO;
        }

      erase.startblock = (off_t)(off / geo.erasesize);
      erase.nblocks    = 1;

      if (ioctl(fd, MTDIOC_ERASESECTORS, (unsigned long)((uintptr_t)&erase)) < 0)
        {
          int err = -errno;
          close(fd);
          return err;
        }
    }

  memset(&blob, 0, sizeof(blob));
  blob.magic = PW_STORE_MAGIC;
  blob.ver   = PW_STORE_VER;
  blob.len   = sizeof(struct pw_store_state_s);
  blob.st    = *st;
  blob.crc   = pw_store_crc32(&blob, offsetof(struct pw_store_blob_s, crc));

  ret = pwrite(fd, &blob, sizeof(blob), off);
  close(fd);

  if (ret != (int)sizeof(blob))
    {
      return -EIO;
    }

  syslog(LOG_INFO,
         "[store] saved theme=%u zh=%u fps=%u\n",
         st->theme, st->zh, st->fps_show);
  return 0;
}

int pw_store_report(void)
{
  struct pw_store_blob_s blob;
  off_t off = 0;
  size_t size = 0;
  int fd;
  int ret;

  {
    bool ours;

    fd = pw_store_open(&off, &size, &ours);
  }

  if (fd < 0)
    {
      printf("[store] %s 不可用（%d），设置不会掉电保留\n",
             PW_STORE_DEV, fd);
      return fd;
    }

  close(fd);

  ret = pw_store_read_blob(&blob);
  if (ret < 0)
    {
      printf("[store] %s 末块（offset=%ld, %u B）没有有效记录（%d）\n",
             PW_STORE_DEV, (long)off, (unsigned)size, ret);
      return ret;
    }

  printf("[store] theme=%u zh=%u fps=%u bl=%u (offset=%ld)\n",
         blob.st.theme, blob.st.zh, blob.st.fps_show,
         (unsigned)blob.st.backlight, (long)off);
  return 0;
}

/* 体积探针：验证镜像大小是否触发启动挂死（T-0017 排查） */
const unsigned char pw_pad_blob[1024] __attribute__((used)) = {0x5a};
