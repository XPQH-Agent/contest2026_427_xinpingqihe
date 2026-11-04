# 平台侧补丁（不在本仓库内，但缺了就重建不出同一份固件）

本目录放的是**两处平台级修复**的补丁与复现脚本。它们不在本仓库的源码树里，原因写在下面第 1 节；
放进来是为了让评委能按图索骥，把固件重建到与真机验证时**逐字节一致**。

---

## 1. 为什么这两处补丁不直接进本仓库

| 补丁 | 上游归属 | 本仓库里有没有这个文件 |
|---|---|---|
| `0001-vendor-sifli-bth4-whole-frame-write.patch` | openvela 主树 `vendor/sifli`（独立 git 仓库，SiFli 厂商代码） | ❌ 没有：本仓库只镜像了 `src/vendor/sifli/boards/...` 的板级配置，不含芯片驱动 |
| `apply-pan-netbuf-pool.py`（对应 zblue 的 `port/sections/defines.c`） | openvela 主树 `external/zblue`（构建时下载，**被 `.gitignore` 的 `/*/` 规则忽略、非 git 跟踪**） | ❌ 没有 |

即：这两处在 openvela 主树里改，本仓库（`app/` + 板级配置 + 文档 + 日志）里没有对应文件可改。
按赛事"不修改上游厂商代码、以 PR 方式回馈"的原则，这里以**补丁 + 脚本**形式随作品提交，而不是把厂商文件复制进本仓库。

---

## 2. `0001`：H4 帧必须一次整帧写进邮箱 ring

- 文件：`vendor/sifli/chips/sf32lb52/sf32lb52_bt_adapter.c` 的 `sf32lb52_bt_tx_chunk_len()`
- 症状：蓝牙吞吐一上来就 `Hardware error, hardware code: 0x00`，随后 NoCP / 看门狗复位
- 根因：原来在 `offset == 0 && remaining > 1` 时返回 1（按 1 字节切块）。`up_clean_dcache` 会把同一条 D-cache line 里**陈旧的 `read_idx_mirror`** 一起写回，LCPU 侧的读指针因此回退，邮箱协议崩
- 修复：整帧一次交出去（`return remaining;`）；单帧 ACL 上限 487 B（ring 492 B = (512−20) & ~3，再减 1 字节 H4 类型 + 4 字节头）
- 应用：`git apply 0001-vendor-sifli-bth4-whole-frame-write.patch`（在 `vendor/sifli` 根目录执行）

## 3. `0002`：新增 net_buf 池必须登记进 `_net_buf_pool_list[]`

- 文件：`external/zblue/zblue/port/sections/defines.c`
- 症状：接收方向 `tailroom` 莫名其妙只有 249、改 MTU 也没用；释放时踩坏别人的池
- 根因：这个 NuttX 端口的池表是**手写数组**（不用 linker section）。`port/lib/net_buf/buf.c` 的 `pool_id()` 查不到时 `__ASSERT` 在 release 构建里被编掉，**静默返回 0**，于是拿到别人的 `max_alloc_size` 与 `data_pool`
- 修复：`extern struct net_buf_pool pw_pan_pool;` + 在数组末尾加 `&pw_pan_pool,`
- 应用：`python3 apply-pan-netbuf-pool.py --apply`（幂等，`--check` 只检查，`--revert` 回退；自动定位 `external/zblue/zblue/port/sections/defines.c`）

---

## 4. 与仓库内代码的关系

仓库内的 `app/phywear/` 已经包含应用侧全部修复（提交 `a05c8cf`）：
`pw_bnep.c`（栈缓冲 / 重复 unref / `rx.mtu=1691`）、`pw_bnep_frame.c`（BNEP filter 长度字段）、
`pw_bt.c`（经典可见性 1/1）、板级 `defconfig`（`BT_MAX_CONN=2`、`BT_BUF_ACL_RX_SIZE=1605`、`DHCPC_BOOTP_FLAGS=0x8000`）。
**只有上面这两处属于平台树**，因此单独放补丁。

> 说明：这两处修复是我们在排查中独立定位并验证的；同期另有参赛队（181 队）在其公开文档中记录了同类平台坑，
> 我们在 `docs/22` §4 与 `docs/14` 复盘里如实注明"复用其规则、独立实现与验证"，不宣称首创。