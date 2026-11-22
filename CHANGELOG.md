# Changelog / 更新日志

> Release channels / 发布通道：
> - **Stable（稳定版 / Latest）**：长期保留、经过稳定基线锁定的版本（客户端默认仅接收稳定版更新）。当前首个稳定版为 **`v0.1.22`**。
> - **Hourly（每时预览版 / Pre-release）**：持续迭代的自动构建预览版（客户端勾选「预览版更新」可收到）；GitHub Releases 仅滚动保留最新 4 个每时版 + 稳定版（最多 5 个 Release），历史 Git Tag 全部保留。

---

## v0.1.25 (Hourly / Pre-release) — 2026-11-22

### 中文
- **「更快发送到 iPhone」改为始终打开，设置里的开关已删除**：同时在途的 AWDL 帧从 8 个提高到 48 个。这个开关从 0.1.9 起就有，至今没有任何网卡报告它让情况变差：AX211 上 537 MB 以 3.4 MB/s 完整传完、没有放弃的帧（[#9](https://github.com/zheznanohana/nekodrop/issues/9)），AX210 略快（[#14](https://github.com/zheznanohana/nekodrop/issues/14)），AX200 速度不变（[#4](https://github.com/zheznanohana/nekodrop/issues/4)）。之前没开过的用户升级后就会用上它。
- 「聚合发送（A-MPDU）」仍有问题（固件只发单帧），保留开关，默认关闭。

### English
- **"Faster sending to iPhone" is always on and its switch is gone**: up to 48 AWDL frames in flight instead of 8. It was a switch since 0.1.9 and no card reported it making things worse: AX211 sent 537 MB at 3.4 MB/s with no frames given up ([#9](https://github.com/zheznanohana/nekodrop/issues/9)), AX210 a little faster ([#14](https://github.com/zheznanohana/nekodrop/issues/14)), AX200 the same speed ([#4](https://github.com/zheznanohana/nekodrop/issues/4)). Users who never turned it on get it after updating.
- Aggregated sending (A-MPDU) still has problems (the firmware sends single frames only), so it keeps its switch, off by default.

---

## v0.1.24 (Hourly / Pre-release) — 2026-11-22

### 中文
- **修复：开启实验性 A-MPDU 后找不到 iPhone（0.1.22 起的回归，[#4](https://github.com/zheznanohana/nekodrop/issues/4)）**：0.1.22 在 A-MPDU 模式下把监听的 BSSID 改成了 AWDL BSSID 并加了 ACK 速率，0.1.23 又在会话期间把接收过滤收窄到只收组播。AX200 实测 0.1.23 开 A-MPDU 后 3 分半钟都没发现设备（PSF 计数停住、`0 addressed to us`），而 0.1.20 同样开着 A-MPDU 还能发现。现在 MAC 上下文恢复到 0.1.20 的样子。默认路径（A-MPDU 关闭）从没用过这两处改动，不受影响。
- **修复：「按需」模式下在 nekodrop 里连接 Wi-Fi 会白白重启无线（[#11](https://github.com/zheznanohana/nekodrop/issues/11)）**：只有「同时联网和 AirDrop」模式会在 AirDrop 旁边连 Wi-Fi。以前在其他模式下点连接，服务会重启 radio 但什么也连不上，每点一次 AirDrop 发现就从头开始。现在会直接提示需要切换模式。
- **设置窗口标题显示版本号**（[#11](https://github.com/zheznanohana/nekodrop/issues/11)）。
- 另外包含 0.1.23 之后合入的：AirDrop 引擎发送优化（64 KiB DVZip 块、自适应不压缩直传、2 MiB 套接字缓冲、`TCP_NODELAY`），安装器按厂商识别网卡、`restore.ps1` 不再假设 Intel。

### English
- **Fix: no devices found with experimental A-MPDU on (regression since 0.1.22, [#4](https://github.com/zheznanohana/nekodrop/issues/4))**: 0.1.22 gave the A-MPDU listener AWDL's BSSID and ACK rates, and 0.1.23 narrowed its receive filter to group frames during a session. On an AX200, 0.1.23 found nothing for 3.5 minutes with A-MPDU on (PSF count frozen, `0 addressed to us`) while 0.1.20 still found the iPhone. The MAC context is back to 0.1.20's. The default path (A-MPDU off) never used either change.
- **Fix: joining Wi-Fi in nekodrop outside Concurrent mode restarted the radio for nothing ([#11](https://github.com/zheznanohana/nekodrop/issues/11))**: only "Wi-Fi and AirDrop together" runs Wi-Fi beside AirDrop. In the other modes, choosing a network restarted the radio and joined nothing, resetting discovery each time. The Wi-Fi window now says which mode is needed.
- **The settings window title shows the version** ([#11](https://github.com/zheznanohana/nekodrop/issues/11)).
- Also includes what landed after 0.1.23: AirDrop engine send tuning (64 KiB DVZip chunks with an adaptive stored bypass, 2 MiB socket buffers, `TCP_NODELAY`), vendor-aware card classification in the installer and a vendor-agnostic `restore.ps1`.

---

## v0.1.23 (Hourly / Pre-release) — 2026-11-22

### 中文
- **底层传输提速（Intel 实验性 A-MPDU TX + 全网卡通用 Named Pipe 批处理）**：
  - 修复 `IwxDevice::transmit_agg()` 每帧同步敲门铃的问题：改为批量入队（`kick = false`）+ 整批单次 `kick_agg()`，消除每帧 20–40 µs 的同步 UMDF IOCTL 开销（避免饿死固件 SCD 聚合调度器）。
  - `agg_setup()` 补充 TLC `LDPC` 与双天线 `STBC` 标志（VHT 80 MHz MCS 9 必需），对齐上游 `ForbiddenItlwm`。
  - 开启 `--aggregate` 并关闭混杂模式时将监听过滤掩码严格收缩为 `IWX_MAC_FILTER_ACCEPT_GRP`，避免干扰硬件 Block Ack 记账（默认非聚合路径 100% 不变）。
  - 聚合队列在途上限提升至 `min(128, ring - 16)`（22000 族 128 帧 / AX210 族 112 帧），并在收到对端 DELBA 时先发 `IWX_TXPATH_FLUSH` 冲刷 `sta_id=3` 防止 SSN 错位。
  - `PipeServer` 新增 RX 帧批量合并写入（`queue_send` / `flush_send`，单批达 32 KiB 或每轮 `dev.poll()` 结束统一一次 `WriteFile`），并将管道缓冲扩至 256 KiB，大幅降低高吞吐下的系统调用开销（适用于所有网卡与模式）。
- **多厂商驱动层基础（`driver/regproxy`）**：
  - `regproxy.c` 升级为自动选择**最大的** Memory BAR（≥ 8 KiB），使单一驱动原生兼容 Intel（BAR0 8 KiB）、Realtek RTL8852CE/BE（BAR2 1 MiB）与 MediaTek MT7921/MT7922（BAR0 1 MiB），Intel 行为完全不变。
- **文档与兼容矩阵**：
  - 新增本更新日志 `CHANGELOG.md`，以及独立的中英双语跨品牌网卡兼容性文档 [`docs/COMPATIBILITY.zh-CN.md`](docs/COMPATIBILITY.zh-CN.md) / [`docs/COMPATIBILITY.md`](docs/COMPATIBILITY.md)。

### English
- **Radio throughput improvements (Intel experimental A-MPDU TX + universal Named Pipe RX batching)**:
  - Batched A-MPDU doorbells (`transmit_agg(..., kick=false)` + one `kick_agg()` per batch), removing synchronous per-frame UMDF IOCTL overhead that could starve the firmware's SCD aggregator.
  - Enabled TLC `LDPC` (required for VHT 80 MHz MCS 9) and two-chain `STBC` flags in `agg_setup()`, matching upstream `ForbiddenItlwm`.
  - Narrowed non-promiscuous listener `filter_flags` to `IWX_MAC_FILTER_ACCEPT_GRP` when `--aggregate` is active (default non-aggregate path is 100% untouched).
  - Raised A-MPDU in-flight cap to `min(128, ring - 16)` and added `IWX_TXPATH_FLUSH` (`agg_flush()`) on DELBA before renegotiating to prevent SSN desync.
  - Added batched RX pipe writes (`PipeServer::queue_send` / `flush_send`, 32 KiB auto-flush + post-`dev.poll()` flush) and raised the Named Pipe buffer to 256 KiB for all cards and modes.
- **Multi-vendor register proxy (`driver/regproxy`)**:
  - Updated `regproxy.c` to select the largest memory BAR (≥ 8 KiB), supporting Intel (8 KiB BAR0), Realtek RTL8852CE/BE (1 MiB BAR2), and MediaTek MT7921/MT7922 (1 MiB BAR0) with zero change on Intel hardware.
- **Documentation**:
  - Added `CHANGELOG.md` and standalone multi-vendor compatibility matrices [`docs/COMPATIBILITY.md`](docs/COMPATIBILITY.md) / [`docs/COMPATIBILITY.zh-CN.md`](docs/COMPATIBILITY.zh-CN.md).

---

## v0.1.22 (Stable / 首个稳定版) — 2026-11-22

> SHA-256: `5d7545d61b8c546828bba2ad4fe13bd101cacfeaa7cf9b3853cf004eac9e77cf`

### 中文
- **客户端 UI（#11）**：修复 Windows 深色模式下主窗口与发送窗口附近设备名黑字看不清；修复对端 `model` 为空时 iPad / Mac 仍显示手机图标。
- **实验性 A-MPDU 发送（#4）**：开启 `--aggregate` 时为监听 MAC 设置 AWDL BSSID 与基础 ACK 速率；收紧 TLC 高 MCS 掩码；新增聚合单帧状态诊断日志及无 Block Ack 自动回退普通单帧队列保护（`iwx_selftest` 覆盖）。
- **模块化多网卡架构**：新增 `iwxradio/src/radio_backend.h` 后端能力契约与编译期校验，移除 `KernelDevice` 的 Intel 空桩，新增 `radio_backend_test.h` 离线测试。

### English
- **Client UI (#11)**: Fixed black device names in Windows dark mode in Main/Send windows; fixed iPad/Mac icon fallback when `model` is null.
- **Experimental A-MPDU sending (#4)**: Set AWDL BSSID and basic ACK rates on the `--aggregate` listener MAC; restricted TLC to high MCS; added per-single status/rate logging and automatic no-Block-Ack fallback (`iwx_selftest`).
- **Modular radio HAL**: Introduced `iwxradio/src/radio_backend.h` capability contract + compile-time checks, removed Intel stubs from `KernelDevice`, added `radio_backend_test.h`.

---

## v0.1.21 — 2026-11-22

> SHA-256: `58c72b9a0fe73dbe481edfbfbdf4a6a6b7ddbe1e49436cbec6e83801bdb1cd41`

- 新增 `app/service/CardCatalog.cs` 统一网卡档案与能力抽象，解耦 `Card.cs` / `Supervisor.cs` 的 Intel 硬编码，并将 `NekoDropService.exe --selftest` 接入每次 CI 构建。 / Introduced `CardCatalog.cs` multi-vendor adapter profiles & capability abstractions, decoupling `Card.cs` and `Supervisor.cs`, wired `NekoDropService.exe --selftest` into CI.

---

## v0.1.20 — 2026-11-22

> SHA-256: `16a19a5dd1790e7ef35601bb77fb4b6ff21caece31bc44c00e2ccbb5163c2e25`

- 修复 `#4` 代理环境下检查更新触发 GitHub API HTTP 403 限流（新增本地 `ETag` / `304` 缓存、15s 超时、`releases.atom` 回退及设置页「打开下载页」按钮）。 / Fixed #4 update check HTTP 403 rate-limit behind proxies (local `ETag` cache, 15s timeout, `releases.atom` fallback, Open Releases button).
- `installer/report.ps1` 新增全品牌无线网卡识别分类与 `-SelfTest`，新增 `docs/HARDWARE.md` 多网卡架构文档。 / Added multi-vendor Wi-Fi classification & `-SelfTest` to `installer/report.ps1`, plus `docs/HARDWARE.md`.

---

## v0.1.19 — 2026-11-22

> SHA-256: `6bba6037bb649b57c11aa3edeec5614a04864fbf798d3b28e647f3325ab12f4f`（已在 #11 AX210 真机验证）

- 修复 `#11`：`nekodrop` TAP 网卡跃点由 `75` 提升至 `500`，不再抢占笔记本有线以太网默认路由；修复被动信道 30 秒误判重启循环。 / Fixed #11: raised TAP metric `75 → 500` so Ethernet keeps routing priority; fixed 30s passive-channel bounce loop (hardware-verified on AX210).

---

## v0.1.18 — 2026-11-21

> SHA-256: `3c02105f7b4f7d3607e19cc46b9e217e0d08a4643f35852f8c7a3e7f3a1f58d1`（冷启动已在 #4 AX200 真机验证）

- 修复 `#4` 冷启动卡住 6 分钟问题（冷启动 `radio ready` 实测降至 2.1 秒）；修复 `#20` Win10 残留幽灵 `Not Present` TAP 适配器；确认 `#18` Win10 22H2 双向可用。 / Fixed #4 6-minute cold-boot stall (verified 2.1s on AX200); fixed #20 Win10 phantom TAP adapter; confirmed #18 Win10 22H2 working.

---

## v0.1.0 – v0.1.17 (2026-11-17 – 2026-11-21)

- 初始发布与早期实机验证里程碑：Intel AX201 / AX200 / AX210 / AX211 双向 AirDrop + Wi-Fi 共存、Windows 10 UMDF 2.31 兼容（#3/#12/#18）、AX210 族 PNVM DMA 预留与 `nic_config` 修复（#2/#7/#8）、128 项 TX 环与自动扩展 AWE 池（#11）、一键诊断报告 `report.ps1`（v0.1.7）、实验性高速发送 `--tx-window 48`（#9 实测提速 +41%）与实验性 A-MPDU 发送初版（v0.1.15）。
