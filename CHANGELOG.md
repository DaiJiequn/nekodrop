# Changelog / 更新日志

> Release channels / 发布通道：
> - **Stable（稳定版 / Latest）**：长期保留、经过稳定基线锁定的版本（客户端默认仅接收稳定版更新）。当前稳定版为 **`v0.1.30`**（首个稳定基线为 `v0.1.22`）。
> - **Hourly（每时预览版 / Pre-release）**：持续迭代的自动构建预览版（客户端勾选「预览版更新」可收到）；GitHub Releases 仅滚动保留最新 4 个每时版 + 稳定版（最多 5 个 Release），历史 Git Tag 全部保留。

---

## v0.1.31 (Hourly / Pre-release) — 2026-11-22

### 中文
- **修复 AX211 / AX411 / Killer 1690 上 iPhone、Mac 看不到这台电脑（[#24](https://github.com/zheznanohana/nekodrop/issues/24)、[#25](https://github.com/zheznanohana/nekodrop/issues/25)）**：radio 让固件在每个收到的帧后面附上 FCS（校验和），而 RxFrame 约定交给 awdld 的帧不带 FCS。AX210 系列这几张卡的固件确实会附上，awdld（OWL）看到 AWDL 动作帧最后一个 TLV 后面多出 4 个字节，就在记下“收到过 MIF”之前把整帧丢掉，于是 iPhone 一直是 `not valid yet`：电脑不会跟随它的同步，也不会被它列为接收方。发送不受影响，所以“能发不能收”。现在 radio 只在帧尾 4 字节确实等于这个帧的 CRC-32 时才把它去掉（不带 FCS 的帧被误判的概率是 2⁻³²），对所有网卡都安全。
- **修复 A-MPDU 第一次发送成功后，第二次发送失败、iPhone 从列表消失（[#4](https://github.com/zheznanohana/nekodrop/issues/4)，AX200 实测）**：发送结束后 Block Ack 会话一直停在 `agreed`。这期间监听只收组播帧，iPhone 结束会话时发来的单播 DELBA 被固件丢掉，下一次发送仍按旧会话发 A-MPDU，iPhone 不再接收，于是连接被拒（`WinError 10061`），随后 `peer_gone`，只有重启 radio 才能恢复。现在批量发送结束、队列清空 1.5 秒后，电脑主动发 DELBA 结束会话、恢复完整接收，下一次发送重新协商（约 0.3 秒）。
- **诊断**：radio 的 `rx:` 统计行新增 `FCS removed`、`MIF bad tail`（awdld 仍会丢掉的 MIF）、`MIF without version`（缺少 awdld 需要的版本 TLV）三项计数，前 3 个尾部异常的 MIF 会记下最后 16 个字节。
- **还没有在 AX211 真机上验证**，请 #24、#25 的用户帮忙测试。

### English
- **Fixed iPhone/Mac never listing the PC on AX211 / AX411 / Killer 1690 ([#24](https://github.com/zheznanohana/nekodrop/issues/24), [#25](https://github.com/zheznanohana/nekodrop/issues/25))**: the radio asks the firmware to append the FCS to received frames, while RxFrame promises awdld frames without it. These AX210-family cards do append it, and awdld (OWL) drops an AWDL action frame with 4 bytes after its last TLV before it records the MIF, so the iPhone stayed `not valid yet`: the PC never followed its sync and was never listed as a receiver. Sending was unaffected. The radio now removes the last 4 bytes only when they are the frame's valid CRC-32 (a frame without an FCS matches by chance with odds of 2^-32), which is safe on every card.
- **Fixed the second A-MPDU send failing and the iPhone vanishing after a successful first one ([#4](https://github.com/zheznanohana/nekodrop/issues/4), measured on an AX200)**: the Block Ack session stayed `agreed` after the send. Meanwhile the listener takes group frames only, so the iPhone's unicast DELBA was dropped by the firmware, and the next send still went out as A-MPDUs the iPhone no longer accepted: connection refused (`WinError 10061`), then `peer_gone` until the radio restarted. The PC now ends the session itself with a DELBA 1.5 s after a bulk send ends and its queue drains, restoring full receive; the next send negotiates again (~0.3 s).
- **Diagnostics**: the radio's `rx:` line adds `FCS removed`, `MIF bad tail` (MIFs awdld would still drop) and `MIF without version` (no version TLV, which awdld needs); the first 3 MIFs with a bad tail log their last 16 bytes.
- **Not yet verified on an AX211**; the reporters of #24 and #25 are asked to test it.

---

## v0.1.30 (Stable / 稳定版) — 2026-11-22

### 中文
- **🚀 传输速度十倍级跃升（AX201 并发模式实测验证）**：
  - **PC → iPhone 实测达到 `30.8 MB/s`**（104.8 MB 文件仅用 **3.4 秒**传完，对比此前单帧发送的 1–3 MB/s 提升 **10× 以上**）。开启实验性 A-MPDU 后 **0.304 秒**完成 Block Ack 握手，单次聚合高达 **63 帧**、确认率 **99.69%**（81,451 / 81,707），稳定跑满 **VHT 80 MHz 双流（2SS）MCS 9 + LDPC + SGI（866.7 Mbps）**。
  - **iPhone → PC 实测平均 `13.5 MB/s`（后半段峰值 ~30 MB/s）**，104.8 MB 安装包 **8.1 秒**收完落盘。
- **重点修复实验性 A-MPDU 协商与聚合路径（[#4](https://github.com/zheznanohana/nekodrop/issues/4)、[#9](https://github.com/zheznanohana/nekodrop/issues/9)）**：
  - 修复 0.1.28 导致 TX AddBA 一直停在 `asking`（`asks 80, agreed 0`）的回归：此前在 `asking` 阶段就切到组播过滤，把 iPhone 返回的单播 ADDBA Response 丢掉了。现在 `asking` 阶段保持混杂与管理帧接收，仅在 `agg_enable(true)` 拿到对端同意后原子切换到 ForbiddenItlwm 的 BA 监听状态（AWDL BSSID + 基础 ACK 速率 + 组播过滤），退出时立即恢复。
  - 修复 A-MPDU 响应路径（`handle_tx_resp`）在 `frame_count > 1` 时提前回收队列索引的隐患（改为仅由 `IWX_BA_NOTIF` 回收），并清零 26 字节 QoS 帧头后的 2 字节对齐填充。
  - 更激进的 A-MPDU 触发与协商：250 ms 内收到 24 帧大包即触发（此前为 1 s / 200 帧），每 100 ms 发一次 ADDBA（最多 20 次 / 5 s 超时即干净回退单帧，60 s 后允许再试），`asking` 期间不再开启发送门控（避免 `gate holds` 堆积与阻塞管理帧），回退时同步 `TXPATH_FLUSH` 清空聚合队列避免与单帧队列乱序竞争，同时开启 20/40/80 MHz SGI 与尊重对端协商窗口大小。
- **整体发送提速（A-MPDU 关/开均生效）**：Intel 无线发送窗口从 48 提到 64（`--tx-window 64`）；单帧速率阶梯改为 32 帧窗口（爬升快 2 倍）、上限新增 VHT 80 MHz 2SS MCS 8，并启用 VHT LDPC + SGI；主循环每轮读取管道帧数上限从 64 提到 128；`--fast-unlock` 在信道允许发送后立刻停掉剩余扫描。
- **诊断报告保留每次发送的 A-MPDU 快照（[#4](https://github.com/zheznanohana/nekodrop/issues/4)）**：在每次批量发送结束、回退或收到 DELBA 时立即写入一条 `aggregate:` 快照，诊断报告的 Bring-up 表优先展示活跃期（`asking`/`agreed`）记录而不再被末尾 `idle` 覆盖。

### English
- **🚀 10x+ Transfer Speed Leap (verified on AX201 Concurrent mode)**:
  - **PC → iPhone measured at `30.8 MB/s`** (104.8 MB transferred in **3.4 s**, up **10x+** from 1–3 MB/s single-frame TX). With experimental A-MPDU enabled, Block Ack negotiation completes in **0.304 s**, reaching **63-frame** aggregates with **99.69%** ACK rate (81,451 / 81,707) at full **VHT 80 MHz 2SS MCS 9 + LDPC + SGI (866.7 Mbps)**.
  - **iPhone → PC measured at `13.5 MB/s` average (~30 MB/s peak)**, receiving the 104.8 MB installer in **8.1 s**.
- **Major fix and tuning for experimental A-MPDU TX ([#4](https://github.com/zheznanohana/nekodrop/issues/4), [#9](https://github.com/zheznanohana/nekodrop/issues/9))**:
  - Fixed the 0.1.28 regression where TX AddBA stayed stuck in `asking` (`asks 80, agreed 0`): switching to group-only filtering during `asking` caused the firmware to drop unicast ADDBA Response management frames. Promiscuous/management receive now stays open throughout `asking`, switching atomically to ForbiddenItlwm's BA listener state (AWDL BSSID + basic ACK rates + group-only filter) inside `agg_enable(true)` only after the peer agrees, and restoring immediately on teardown.
  - Fixed `handle_tx_resp` for the aggregation queue so multi-frame A-MPDU responses (`frame_count > 1`) are reclaimed solely by `IWX_BA_NOTIF` (matching Linux iwlwifi / ForbiddenItlwm), and zeroed the 2-byte alignment padding after 26-byte QoS headers.
  - More aggressive A-MPDU trigger and bounded fallback: triggers at 24 payload frames in 250 ms (down from 200 in 1 s), asks every 100 ms up to 20 asks / 5 s before cleanly falling back with a 60 s cooldown, stops applying the send gate during `asking` (and never gates management frames), calls `TXPATH_FLUSH` on fallback to avoid reordering against the single-frame queue, enables 20/40/80 MHz SGI in TLC, and honors the peer's negotiated BA window.
- **Faster sending overall (with or without A-MPDU)**: Intel `--tx-window` raised from 48 to 64; single-frame rate ladder steps up over 32-frame windows (2x faster ramp), adds VHT 80 MHz 2SS MCS 8, and enables VHT LDPC + SGI; radio main loop drains up to 128 pipe frames per tick; `--fast-unlock` cancels remaining scans as soon as transmitting is allowed.
- **Per-send A-MPDU snapshots in diagnostics ([#4](https://github.com/zheznanohana/nekodrop/issues/4))**: logs an immediate `aggregate:` snapshot at the end of every bulk send, fallback, or DELBA, and `report.ps1` prefers the last active (`asking`/`agreed`) snapshot over a trailing `idle` line.

---

## v0.1.29 (Hourly / Pre-release) — 2026-11-22

### 中文
- **内置支持 Realtek RTL8852CE（`10EC:C852`）**：和 Intel 网卡一样按 PCI ID 自动识别、自动接管，不需要开关。用的是 winawdl 的 radio（基于 raph559/devourer 的 Windows PCIe 后端），打包为 `radio\nekodrop-radio-rtl.exe`。这张卡的驱动不支持同时连 Wi-Fi，所以按需模式运行。**还没有真机数据**。
- **自动识别失败时可以手动选择网卡**：*设置 → 网卡* 会列出本机的 Intel/Realtek 网卡和可选型号（Intel AX201/9560、AX200、AX210、AX211/AX411、Realtek RTL8852CE）。手动选择只对驱动包里列出的硬件 ID 有效。
- **网卡后端模块化**：服务端按 `IRadioBackend` 启动各个网卡的 radio；适配新网卡的接口和步骤写在 [docs/ADDING-A-CARD.md](docs/ADDING-A-CARD.md)，里面也列了下一批候选网卡和各自可以移植的驱动来源。

### English
- **Realtek RTL8852CE (`10EC:C852`) built in**: recognized and taken by its PCI ID like the Intel cards, no switch. It runs winawdl's radio on raph559/devourer's Windows PCIe backend, shipped as `radio\nekodrop-radio-rtl.exe`. Its driver has no station, so it runs on demand. **No hardware data yet**.
- **Manual card choice when detection fails**: *Settings > Wi-Fi card* lists the PC's Intel/Realtek network controllers and the models (Intel AX201/9560, AX200, AX210, AX211/AX411, Realtek RTL8852CE). Only for hardware IDs the driver package lists.
- **Pluggable radio backends**: the service starts each card's radio through `IRadioBackend`; the interfaces and checklist for the next card family are in [docs/ADDING-A-CARD.md](docs/ADDING-A-CARD.md), with the candidates and the drivers they could be ported from.

---

## v0.1.28 (Hourly / Pre-release) — 2026-11-22

### 中文
- **修复：连接 Wi-Fi 认证失败后网卡固件崩溃（[#21](https://github.com/zheznanohana/nekodrop/issues/21)）**：移除 station 前没有先取消会话保护、清空它的发送队列，结果移除命令超时，固件随之崩溃（LMAC 0x125F），#21 里连续发生了 3 次。现在按 OpenBSD/itlwm 的顺序断开：取消会话保护 → drain + TXPATH_FLUSH → 移除 station → binding → MAC → PHY。
- **实验性 A-MPDU：会话期间改用 ForbiddenItlwm 的监听配置（[#4](https://github.com/zheznanohana/nekodrop/issues/4)、[#9](https://github.com/zheznanohana/nekodrop/issues/9)）**：发送命令、QoS 帧、station、队列和速率控制都已经和 ForbiddenItlwm 逐字节一致，只剩会话期间的监听 MAC 不同。现在在 Block Ack 会话期间（混杂模式关闭时）改用 AWDL BSSID、基础 ACK 速率和只收组播的过滤器，和 ForbiddenItlwm 在 Mac → iPhone 跑到 23 MB/s 时的配置一样。发现阶段不变。A-MPDU 仍默认关闭，**还没有真机数据**。

### English
- **Fix: firmware crash after a failed Wi-Fi authentication ([#21](https://github.com/zheznanohana/nekodrop/issues/21))**: the station was removed without cancelling its session protection or flushing its queue; REMOVE_STA timed out and the firmware asserted (LMAC 0x125F), three times in a row in #21. Teardown now follows OpenBSD/itlwm: cancel session protection, drain + TXPATH_FLUSH, then station, binding, MAC, PHY.
- **Experimental A-MPDU: ForbiddenItlwm's listener state during the session ([#4](https://github.com/zheznanohana/nekodrop/issues/4), [#9](https://github.com/zheznanohana/nekodrop/issues/9))**: the TX command, QoS frame, peer station, queue and rate control already matched ForbiddenItlwm byte for byte; only the listener's MAC context differed. While the Block Ack session runs (promiscuous receive off) it now carries AWDL's BSSID, the basic ACK rates and a group-only filter, as ForbiddenItlwm does where Mac -> iPhone reaches 23 MB/s. Discovery is unchanged. A-MPDU stays off by default; **no hardware data yet**.

---

## v0.1.27 (Hourly / Pre-release) — 2026-11-22

### 中文
- **修复：传输途中被切信道打断（[#9](https://github.com/zheznanohana/nekodrop/issues/9)）**：iPhone 公告别的信道后，服务会等到 10 秒没有数据再切过去，但以前最多只等 120 秒，等满就不管有没有数据直接切。#9 里一个 537 MB 的发送在信道 44 上跑得好好的（1.8 MB/s），120 秒后被切到 149，在 37.7% 处以 TLS EOF 断开。现在只要还有数据就一直等；iPhone 又报回当前信道时取消切换；同时只保留一个切换任务。
- **修复：更新时 `libcrypto-3.dll` 复制失败（[#22](https://github.com/zheznanohana/nekodrop/issues/22)）**：安装脚本结束 `nekodrop.exe` 时，它启动的 AirDrop 引擎（`python.exe`）还占着文件。现在会等服务真正停下，结束所有从安装目录运行的进程，复制失败会重试，最后失败时会说明是哪个进程占着。
- **卸载不再静默留下残留（[#22](https://github.com/zheznanohana/nekodrop/issues/22)）**：每一步的失败都会记进 `%ProgramData%\nekodrop-uninstall.log`；会删除 `nekodrop` TAP 网卡（保留 TAP 驱动）和 `%LocalAppData%\nekodrop`；服务程序缺失时改用 `restore.ps1` 归还网卡。
- 构建时会用 PowerShell 5.1 解析所有安装器脚本，语法错误会让构建失败。

### English
- **Fix: sends cut by a channel move ([#9](https://github.com/zheznanohana/nekodrop/issues/9))**: after the iPhone named another channel, the service waited for 10 s without data but gave up after 120 s and moved anyway. In #9 a 537 MB send running at 1.8 MB/s on channel 44 was moved to 149 after 120 s and died with TLS EOF at 37.7%. Now it waits as long as data flows, cancels the move when the iPhone names our channel again, and runs one mover at a time.
- **Fix: updates failing to copy `libcrypto-3.dll` ([#22](https://github.com/zheznanohana/nekodrop/issues/22))**: the setup killed `nekodrop.exe` but not the AirDrop engine (`python.exe`) it starts, which kept the file open. Now it waits for the service to stop, stops everything running from the install folder, retries the copy and names what still holds it.
- **Uninstall no longer fails silently ([#22](https://github.com/zheznanohana/nekodrop/issues/22))**: each failed step goes to `%ProgramData%\nekodrop-uninstall.log`; it removes the `nekodrop` TAP adapter (not the TAP driver) and `%LocalAppData%\nekodrop`, and gives the card back with `restore.ps1` when the service exe is missing.
- The build parses every installer script under PowerShell 5.1 and fails on a syntax error.

---

## v0.1.26 (Hourly / Pre-release) — 2026-11-22

本版来自「nekodrop 自主进化」第 6 轮：Gemini 提案 → Claude 实现 → Codex QA 通过，风险等级 EXPERIMENTAL。 / From self-evolution round 6: proposal by Gemini, implementation by Claude, Codex QA passed, risk EXPERIMENTAL.

### 中文
- **新的实验性开关「更快解锁被动 5 GHz 信道」（默认关闭，[#11](https://github.com/zheznanohana/nekodrop/issues/11)）**：网卡不接受国家码时（#11 的 AX210 + TW），5 GHz AirDrop 信道一开始是被动的，在听到 iPhone 或附近路由器之前电脑一帧都不发，#11 里每次重启要等约 60 秒。打开后会在 1.5、12、30 秒各扫一次 11 个常用信道（原来是 10 秒扫一次全部信道），让网卡按自己的规则从附近路由器学到国家码。被拒绝的国家码会在 iPhone 报出同一国家时再问一次。不会在被动信道上强行发射。关闭时扫描时机和以前完全一样。**还没有真机数据**，有用户证明没问题后会改成始终打开。
- **发送统计改正**：`tx: N submitted` 只统计 AirDrop 的帧，发给路由器的帧移到 `wifi:` 那一行。以前在「同时联网」模式下 submitted 比 done 多出一大截，看上去像丢包，其实不是。
- **发现耗时诊断**：radio 日志新增「第一次收到 AWDL 帧 / MIF / 发给本机的帧用了多少秒」，诊断报告里有对应的时间表（[#4](https://github.com/zheznanohana/nekodrop/issues/4)、[#11](https://github.com/zheznanohana/nekodrop/issues/11)）。
- 诊断报告不再把「更快发送」显示成关闭（0.1.25 起它始终打开）。

### English
- **New experimental switch "Faster unlock of a passive 5 GHz channel" (off by default, [#11](https://github.com/zheznanohana/nekodrop/issues/11))**: where the card refuses the country (#11: AX210, TW), the 5 GHz AirDrop channel starts passive and the PC sends nothing until it hears an iPhone or a router; #11 waited ~60 s after each restart. With the switch on, the radio scans the eleven common channels at 1.5, 12 and 30 s (instead of all channels once at 10 s) so the card learns the country from nearby routers by its own rules, and asks again for a refused country when an iPhone names it. It never transmits on a passive channel. Off, the scan timing is exactly as before. **No hardware data yet**; it becomes always-on once users confirm it causes no problems.
- **TX counters fixed**: `tx: N submitted` counts AirDrop frames only; frames to the access point moved to the `wifi:` line. Under Concurrent the gap between submitted and done looked like loss and was not.
- **Discovery timing**: the radio logs how many seconds until the first AWDL frame, MIF and frame addressed to us, with a timing table in the diagnostic report ([#4](https://github.com/zheznanohana/nekodrop/issues/4), [#11](https://github.com/zheznanohana/nekodrop/issues/11)).
- The diagnostic report no longer shows faster sending as off (it is always on since 0.1.25).

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
