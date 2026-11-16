# 设计：在 Windows 上用户态驱动 Intel AX201 跑 AWDL

## 1. 为什么能拼起来

AirDrop 分四层：Wi-Fi 帧收发、AWDL 协议、BLE 唤醒、AirDrop 协议（mDNS + HTTPS）。

- **winawdl** 已经在 Windows 上把上面三层做完了（awdld = OWL 的 Windows 移植，`airdrop/` 是 omdrop 改出来的收发引擎，加上 App）。它跟最底层只通过一个命名管道 `\\.\pipe\winawdl-radio` 交流（`radio_link.h`）：收到的帧带一个 radiotap 头往上送，要发的帧带 radiotap 头往下送，另外有“设信道 / 设本机地址 / 设 BSSID 过滤”三条控制消息。
- winawdl 的最底层是 `winawdl_radio.exe`，靠 devourer（Realtek 用户态驱动）驱动 RTL8852CE。
- **ForbiddenItlwm** 证明了 Intel 22000 系列固件能当 AWDL 收发器：一个 `IWX_FW_MAC_TYPE_LISTENER` 类型的 MAC 上下文，节点地址设成 AWDL 地址，打开混杂过滤；再挂一个 monitor 站点和一个发送队列。这样 iPhone 发给我们的单播帧由固件按硬件时序回 ACK。winawdl 在 Realtek 上光解决 ACK 就花了很大力气。

所以只要写一个说同样管道协议、底下驱动 Intel 卡的 `winawdl_radio.exe`，两边就拼上了。

## 2. 组件

| 路径 | 内容 | 来源 |
|---|---|---|
| `driver/regproxy/` | UMDF 2 用户态驱动：映射网卡 BAR，按 IOCTL 批量读写寄存器；句柄关闭时重放预先登记的“停 DMA”写入 | winawdl，只改了 PCI ID 和 BAR 下限 |
| `iwxradio/src/proxy.*` | 寄存器代理客户端 | 新写 |
| `iwxradio/src/dma.*` | `AllocateUserPhysicalPages2` 拿锁定的 2 MiB 物理块，切成 DMA 缓冲区；物理地址直接当总线地址 | 照 devourer 的 Windows 后端改写 |
| `iwxradio/src/fw.*` | iwlwifi TLV 固件解析 | 照 itlwm `iwx_read_firmware` |
| `iwxradio/src/iwx.*` | 用户态 Intel 驱动 | 从 itlwm/ForbiddenItlwm 的 `ItlIwx.cpp` 移植 |
| `iwxradio/src/radio_main.cpp` | 管道服务端 + 设备线程 | 新写，协议与 winawdl 兼容 |
| `iwxradio/third_party/iwx/if_iwxreg.h` | 固件接口结构体与寄存器定义 | ForbiddenItlwm 原样拷贝（BSD/GPLv2 双许可） |

## 3. 用户态 Intel 驱动的启动流程

对应 itlwm 的函数名，方便对照阅读：

1. `identify`：读 `CSR_HW_REV`，按 Qu B0 / Qu C0 / QuZ 选固件（itlwm 的 attach 逻辑）。本机 AX201 是 `8086:43F0`，子系统 `4070`。
2. `alloc_rings`：一次拿 6 个 2 MiB 物理块（12 MiB），切出 context info、RX free/used 列表和状态字、512 个 4 KiB RX 缓冲、命令队列（32 个 TFD）、数据队列（256 个 TFD + 每槽 4 KiB 载荷）。22000 系列全程用 64 位地址，所以不需要 4 GiB 以下的低端内存，这点比 Realtek 宽松。
3. `start_hw`：`iwx_prepare_card_hw` → 清 persistence 位 → 软复位 → 集成型（CNVi）强制电源门控 → 再软复位 → `iwx_apm_init`。PCI 配置空间里的 ASPM 碰不到（UMDF 没有配置空间访问），只设 BAR 里的 L0s 位，和 winawdl 的结论一样。中断设成 MSI 模式（`UREG_CHICK`），然后**屏蔽全部中断**，所有事件都靠轮询。
4. `load_firmware`：把 LMAC/UMAC/paging 各分段拷进 DMA 内存，填 context info，写 `CSR_CTXT_INFO_BA`，设 LTR，写 `UREG_CPU_INIT_RUN` 让卡自己去拉固件。然后轮询 `CSR_INT` 等 ALIVE 位：此时固件已经配好接收引擎，立刻把空闲缓冲交给它（`RFH_Q0_FRBDCB_WIDX_TRG = 8`，itlwm 中断处理里的同一步），再从 RX 环里等 ALIVE 通知。
5. `run_init_sequence`：`INIT_EXTENDED_CFG(NVM)` → `NVM_ACCESS_COMPLETE` → 等 `INIT_COMPLETE` → `NVM_GET_INFO`（天线、LAR）→ `TX_ANT_CONFIGURATION` → `BT_CONFIG(WIFI)` → `SOC_CONFIGURATION`（Qu 长延迟晶振：12000、LTR 延迟 2500）→ `DQA_ENABLE` → `POWER_TABLE`（关省电）→ `MCC_UPDATE`（国家码）。
6. `start_monitor`：`PHY_CONTEXT(ADD)` → `MAC_CONTEXT(ADD, LISTENER)` → `BINDING(ADD)` → `ADD_STA(monitor)` → `SCD_QUEUE_CFG`（TVQM，队列号由固件分配）。也就是 itlwm 的 `iwx_auth` 里 monitor 分支加上 ForbiddenItlwm 的 listener 设定。

## 4. 收发

- **收**：卡把接收到的东西写进 RX 缓冲，再把进度写进状态字（`closed_rb_num`）。这两样都在我们的 DMA 内存里，所以轮询收包**完全不需要寄存器访问**，只在处理完一批后写一次 `FRBDCB_WIDX_TRG`。每个缓冲里可能有多个包，按 64 字节对齐依次解析；MPDU 去掉 Intel 描述符和头后填充，再加上 winawdl 格式的 radiotap 头（flags + dBm）发给上层。
- **发**：`TX_CMD`（gen2 格式）里固定速率（`TX_FLAGS_CMD_RATE`）、关加密，802.11 头拷进命令，载荷单独一个 TB；更新字节计数表后写 `HBUS_TARG_WRPTR`。完成通知里的 SSN 决定回收到哪个槽。上层没给速率时：单播数据帧用 `--data-rate`（默认 24 Mb/s），其余用 6 Mb/s OFDM。
- 寄存器访问每次约 30 µs（winawdl 测的），每发一帧只需 1 次。

## 5. 安全

- 网卡交给代理驱动期间，Intel 官方驱动被卸下，Windows 没有 Wi-Fi。`take-card.ps1` 会检查是否还有别的网络连接，没有就拒绝执行。
- 用户态 DMA 的风险和 winawdl 一样：管理员进程可以让网卡写任意物理内存。这里有三层防护：正常退出时 `stop()` 先停接收 DMA、再停总线主控、再复位；固件加载前先往代理驱动登记“停总线主控 + 软复位”，进程被杀时内核关句柄，驱动会重放这两步；最后 DMA 池在复位之后才释放。
- 不覆盖的情况：代理驱动宿主进程本身崩溃，或者在收发途中睡眠 / 休眠。

## 6. 已知风险，按会碰到的顺序

1. **固件起不来**：Intel 固件是卡自己按 context info 去拉的，从没在 AWE 内存上试过。失败时会打印 SecBoot 状态和 UMAC/LMAC PC。
2. **MAC 类型**：ForbiddenItlwm 在 macOS 上是 listener 跟一个关联着的 STA 并存（coex 模式）。这里没有 STA，只有 listener；如果 iPhone 发来的帧没人回 ACK（重传率高），用 `--mac-type ibss` 或 `sta` 试试，ForbiddenItlwm 的 BA 实验就用过这两种。
3. **5 GHz 发送**：world 域（`ZZ`）下 5 GHz 一般是被动信道，固件可能拒发。先在信道 6 上验证，再用 `--country CN` 之类试 149 / 44。
4. **Block Ack**：iPhone 发来 ADDBA 时这一版不回应，它会退回逐帧发送，慢但能用。后续可以照 ForbiddenItlwm 的 stage 4 给对端建一个固件 BA 会话。
5. **App**：winawdl 的 App 写死了 Realtek 的硬件 ID 和驱动名，现阶段用脚本启动；改 App 要装 .NET 10 SDK 重新编译。
