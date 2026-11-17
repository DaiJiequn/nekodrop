<p align="center">
  <img src="app/assets/nekodrop.png" width="128" alt="nekodrop">
</p>

<h1 align="center">nekodrop</h1>

<p align="center">
  <b>用笔记本自带的 Intel 网卡在 Windows 上收发 AirDrop，Wi-Fi 照常上网。</b><br>
  不需要 USB 网卡，不需要 Linux 虚拟机，也不需要 Mac。
</p>

<p align="center">
  <a href="README.md">English</a> · 简体中文
</p>

<p align="center">
  <img alt="platform" src="https://img.shields.io/badge/platform-Windows%2010%20%7C%2011%20x64-0078d4">
  <img alt="card" src="https://img.shields.io/badge/card-Intel%20Wi--Fi%206%20AX201-6c5ce7">
  <img alt="license" src="https://img.shields.io/badge/license-GPL--2.0-blue">
  <img alt="status" src="https://img.shields.io/badge/status-experimental-orange">
</p>

---

## 功能

- **接收 iPhone、iPad、Mac 发来的文件**：电脑会像苹果设备一样出现在 AirDrop 列表里。文件存到你指定的文件夹（默认 `下载\nekodrop`），每次传输都可以在通知里选择接受或拒绝。
- **发送到苹果设备**：在资源管理器里右键文件，选 **用 nekodrop 发送**（Windows 11 第一级菜单、经典菜单和“发送到”里都有），再选附近的设备。
- **AirDrop 时照常上网**：在“同时联网和 AirDrop”模式下，nekodrop 自己驱动网卡，一个虚拟接口连你的 Wi-Fi，另一个跑 AWDL（苹果无线直连协议）。Windows 里上网走一块名叫 **nekodrop** 的“有线网卡”。
- **完整的 Wi-Fi 管理**：扫描、连接、隐藏网络、已保存网络和自动连接。会导入 Windows 里已保存的 Wi-Fi，还能识别酒店、机场、校园网这类需要网页登录的网络，并自动在浏览器里打开登录页。
- **自动恢复**：后台服务盯着 radio、AWDL 守护进程和 AirDrop 引擎，哪个停了就重启哪个。开机自动启动。
- **托盘程序**：中英双语，浅色界面，有系统通知和设置窗口。

## 支持的网卡

nekodrop 直接驱动网卡芯片，只有写了驱动的芯片才能用。

| 网卡 | 状态 |
|---|---|
| **Intel Wi-Fi 6 AX201**（160 MHz，CNVi），PCI ID `8086:02F0`、`06F0`、`34F0`、`3DF0`、`4DF0`、`43F0`、`A0F0` | ✅ 支持，已实机测试（固件 `Qu/QuZ-hr-68`） |
| Intel Wireless-AC 9560 / 9461 / 9462（同样的 PCI ID，旧款射频） | ❌ 不支持（固件系列不同） |
| Intel AX200、AX210、AX211、AX411、BE200 | ❌ 暂不支持。上游驱动支持这些卡，以后可以移植 |
| Realtek、MediaTek、高通、博通 | ❌ 不支持 |

怎么看自己的网卡：打开 *设备管理器 → 网络适配器*，应该显示 **Intel(R) Wi-Fi 6 AX201 160MHz**。其他网卡上安装程序会直接拒绝安装。

大多数 Intel 第 10、11 代笔记本（Comet Lake、Ice Lake、Tiger Lake、Jasper Lake）用的都是 AX201。

## 系统要求

- Windows 10 2004（版本 19041）或更新的 64 位系统，推荐 Windows 11。
- Intel AX201 网卡（见上表）。
- 安装需要管理员权限。
- 发送文件需要蓝牙：AirDrop 用蓝牙低功耗信标唤醒附近的苹果设备。
- 苹果设备那边把 AirDrop 设为 **所有人（10 分钟）**。“仅限联系人”需要 Apple ID 证书，nekodrop 没有。

## 安装

1. 从 [Releases](../../releases) 下载 `nekodrop-<版本>-setup.exe` 并运行。
2. 安装程序会：
   - 把文件复制到 `C:\Program Files\nekodrop`；
   - **在你的电脑上**生成一个代码签名证书，用它给小小的寄存器代理驱动签名并信任它，然后**删除私钥**，这个证书以后再也签不了别的东西；
   - 创建名为 **nekodrop** 的 TAP 虚拟网卡（TAP-Windows，OpenVPN 签名）；
   - 添加两条只允许链路本地地址的入站防火墙规则（TCP 8771-8779、UDP 5353）；
   - 安装并启动 **NekoDropService** 服务。
3. 托盘里会出现紫色小猫图标。打开它，确认 **允许接收** 已勾选。

第一次启动时，nekodrop 会从 Intel 驱动手里接管网卡，并自动连回 Windows 原来连着的网络。之后 Windows 自带的 Wi-Fi 图标会消失，改用 nekodrop 托盘菜单里的 **Wi-Fi…** 窗口管理网络。

### 卸载

在 *设置 → 应用 → nekodrop → 卸载*，或者运行安装目录里的 `uninstall.ps1`。卸载时会先把网卡还给 Intel 驱动，再删除服务、驱动包、证书信任和防火墙规则。

## 使用

| 想做什么 | 怎么做 |
|---|---|
| 接收 | 保持 **允许接收** 勾选。在 iPhone 上点 分享 → AirDrop，选你电脑的名字 |
| 发送 | 右键文件 → **用 nekodrop 发送** → 选设备；也可以把文件拖到主窗口 |
| 换 Wi-Fi | 托盘 → **Wi-Fi…**，选网络、输入密码、点连接 |
| 连隐藏网络 | **Wi-Fi…** → *隐藏网络*，输入网络名和密码 |
| 酒店 Wi-Fi 登录 | 直接连接，登录页会自动在浏览器里打开 |
| 把网卡还给 Windows | 托盘 → *联网方式* → **按需接管网卡** 或 **关闭 AirDrop** |
| 改设备名、保存位置、语言 | 托盘 → **设置…** |

### 联网方式

| 模式 | 上网 | AirDrop |
|---|---|---|
| **同时联网和 AirDrop**（默认） | 走 nekodrop 自己的 Wi-Fi 连接（“nekodrop”网卡） | 一直可用 |
| **按需接管网卡** | 正常的 Intel 驱动 | 附近 iPhone 打开 AirDrop 或你发送文件时才接管网卡，传输期间会断网 |
| **关闭 AirDrop** | 正常的 Intel 驱动 | 关闭 |

### 支持的 Wi-Fi 加密

| 网络类型 | 同时模式下 |
|---|---|
| 开放网络 | ✅ |
| WPA/WPA2 个人版 | ✅ |
| WPA2/WPA3 混合 | ✅（用 WPA2 连接） |
| 需要网页登录的网络 | ✅ 自动识别并打开登录页 |
| 隐藏网络 | ✅ |
| 纯 WPA3 个人版（SAE） | ❌ 暂不支持，会自动改用“按需”模式 |
| WPA2/WPA3 企业版（802.1X、PEAP 等） | ❌ 暂不支持，会自动改用“按需”模式 |
| WEP | ❌ |

## 进度

| 部分 | 状态 |
|---|---|
| 用户态 Intel 驱动：固件启动、监听、注入、扫描 | ✅ AX201 上可用 |
| iPhone → Windows | ✅ 可用（收到的照片完整，哈希一致） |
| Windows → iPhone | 🧪 能发现设备，传输还在实验阶段 |
| 联网同时 AirDrop | ✅ 可用（WPA2、DHCP、DNS 都走“nekodrop”网卡） |
| Wi-Fi 管理、已保存网络、网页登录识别 | ✅ |
| 服务、托盘程序、通知、右键菜单、安装包 | ✅ |
| 纯 WPA3、802.1X、WEP | ⏳ 计划中 |
| 其他 Intel 网卡（AX200/AX210/AX211/BE200） | ⏳ 计划中 |
| 内核 WiFiCx 驱动（`kdriver/`） | 🧊 已搁置：实验性，安装包**不会安装**它，见 [docs/KERNEL.md](docs/KERNEL.md) |

## 工作原理

```
 nekodrop.exe（托盘程序，用户会话）──JSON 管道──► NekoDropService（SYSTEM）
   │  AirDrop 引擎（Python）                        │ 接管 / 归还网卡
   │  通知 · 右键菜单 · 发送窗口                       │ Wi-Fi 管理、看门狗
   │                                                ▼
   └── awdl0（Wintun，IPv6）◄── awdld（AWDL，OWL）◄─管道─ nekodrop-radio（用户态 Intel 驱动）
                                                         ├ MAC 0：AWDL，信道 6/44/149
     “nekodrop” TAP 网卡 ◄── 以太网桥接 ◄─────────────────┤ MAC 1：站点模式，连你的路由器
     （Windows 上网用）                                   └ UMDF 寄存器代理 + 用户态 DMA → AX201
```

1. **寄存器代理**：一个很小的 UMDF 驱动（`driver/regproxy`）代替 Intel 驱动绑定 AX201。它只负责把网卡寄存器和中断映射给一个用户态进程，我们自己的代码不在内核里运行。
2. **用户态驱动**：`nekodrop-radio`（`iwxradio/`）是把 OpenBSD/itlwm 的 `iwx` 驱动移植到用户态。它通过用户态 DMA 加载 Intel 固件，然后同时跑两个 MAC：一个监听和注入 AWDL 帧，另一个是普通的 Wi-Fi 站点，支持 WPA2（四次握手、密钥交给固件、速率控制）。
3. **AWDL**：`awdld`（OWL，Open Wireless Link）通过 radio 管道实现苹果的 AWDL 协议，把链路做成 IPv6 网卡 `awdl0`。
4. **AirDrop**：Python 引擎在 `awdl0` 上实现 AirDrop：mDNS 发现，8771 端口上的 HTTPS（8772-8779 备用），`/Discover`、`/Ask`、`/Upload` 流程，以及发送时用的蓝牙信标。
5. **上网**：站点 MAC 收发的帧桥接到“nekodrop” TAP 网卡，Windows 把它当成以太网，用 DHCP 拿地址。

设计细节：[docs/DESIGN.md](docs/DESIGN.md)（驱动、DMA、风险）· [docs/CLIENT.md](docs/CLIENT.md)（服务、客户端、联网模式）。

## 常见问题

| 现象 | 试试 |
|---|---|
| iPhone 搜不到电脑 | 把 iPhone 的 AirDrop 设为“所有人（10 分钟）”，关掉再打开（清掉缓存的旧记录），手机解锁并放在附近 |
| iPhone 上出现两个电脑 | 之前运行留下的缓存记录，在 iPhone 上把 AirDrop 关掉再打开 |
| 没网 | 托盘 → **Wi-Fi…**，确认已连接。网络可能需要网页登录，或者是纯 WPA3 / 企业网（见上表） |
| Windows 的 Wi-Fi 图标没了 | 同时模式下是正常的：网卡归 nekodrop 管。用 **Wi-Fi…**，或者切到“按需” |
| 其他问题 | 日志在 `C:\ProgramData\nekodrop\service.log`（radio、AWDL、Wi-Fi）和 `%LocalAppData%\nekodrop\engine.log`（AirDrop），提 issue 时请附上 |

## 从源码构建

需要 Visual Studio 2022 Build Tools（C++、Windows SDK、CMake）、.NET 8 SDK 和 Git。Inno Setup 6 可选，没装就输出 zip。不用装 WDK，脚本会用它的 NuGet 包。

```powershell
git clone https://github.com/zheznanohana/nekodrop.git
cd nekodrop
.\scripts\fetch-deps.ps1        # 下载 winawdl、ForbiddenItlwm（固件）、MinGW、WDK NuGet 到 third_party\
.\scripts\build-installer.ps1   # 全部构建 → build\nekodrop-0.1.0-setup.exe
.\scripts\build-radio.ps1       # 只构建用户态驱动并跑离线自检
```

| 目录 | 内容 |
|---|---|
| `iwxradio/` | 用户态 Intel 驱动（C++）：固件、AWDL 监听/注入、站点模式、WPA2、UMAC 扫描 |
| `driver/regproxy/` | UMDF 寄存器代理驱动 |
| `app/service/` | NekoDropService（C#）：网卡接管、radio 监管、Wi-Fi 管理 |
| `app/client/` | 托盘程序（C# WPF）：界面、通知、运行 AirDrop 引擎 |
| `app/shellext/` | Windows 11 右键菜单 |
| `installer/` | Inno Setup 脚本、安装/卸载脚本、TAP 配置 |
| `kdriver/` | 已搁置的内核 WiFiCx 驱动（实验性，不随安装包发布） |

## 致谢

nekodrop 建立在这些项目之上：

- [retropineaple/ForbiddenItlwm](https://github.com/retropineaple/ForbiddenItlwm)：Intel 22000 系列驱动（itlwm `hal_iwx`），以及 AWDL 相关策略（信道跟随、国家码学习、Block Ack、速率阶梯、单播重发、信标提示）。GPL-2.0。
- [raph559/winawdl](https://github.com/raph559/winawdl)：在 Windows 上从用户态接管 PCIe 网卡的方法（UMDF 寄存器代理、用户态 DMA）、radio 管道、OWL 移植和 AirDrop 引擎。MIT。
- [seemoo-lab/owl](https://github.com/seemoo-lab/owl) 和 [seemoo-lab/opendrop](https://github.com/seemoo-lab/opendrop)：这里一切的源头，开源的 AWDL 和 AirDrop 实现。
- OpenBSD `iwx(4)` 和 Linux `iwlwifi`。
- [Wintun](https://www.wintun.net/)、[TAP-Windows](https://github.com/OpenVPN/tap-windows6)。

## 许可

nekodrop 以 **GPL-2.0** 发布（见 [LICENSE](LICENSE)），因为 `iwxradio/` 派生自 itlwm/ForbiddenItlwm。`if_iwxreg.h` 是 BSD/GPL-2.0 双许可。`driver/regproxy` 和 AirDrop 引擎沿用 winawdl 的 MIT 许可。iwlwifi 固件按 Intel 的再分发许可附带，Wintun 和 TAP-Windows 各按其许可。

## 免责声明

nekodrop 是独立项目，与 Apple、Intel 没有任何关系，也未获得它们的认可。AirDrop、iPhone、iPad、Mac 是 Apple Inc. 的商标，Intel 是 Intel Corporation 的商标。nekodrop 运行期间会替换你的 Wi-Fi 驱动。这是实验性软件，按原样提供，不附带任何担保。
