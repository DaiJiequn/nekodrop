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

## 实机测试

以下都是作者在自己的笔记本上实际测过的，不是纸上谈兵：

| | |
|---|---|
| 网卡 | **Intel Wi-Fi 6 AX201 160MHz**（CNVi，固件 `iwlwifi-QuZ-a0-hr-b0-68`） |
| 系统 | Windows 11 专业版 x64 |
| iPhone → Windows | ✅ 可用：照片和文件完整收到 |
| Windows → iPhone | ✅ 可用：右键 → 用 nekodrop 发送，iPhone 接受并收到 |
| 同时上网 | ✅ 可用：AirDrop 运行时电脑通过 nekodrop 自己的 WPA2 连接保持在线，Wi-Fi 和 AWDL 在同一个 5 GHz 信道上也没问题 |

其他 AX201 笔记本用的是同一颗芯片、同一份固件，应该表现一致，但目前只在这一台上测过。欢迎在 [Issues](../../issues) 反馈其他机器的结果。

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
| Windows → iPhone | ✅ 可用（AX201 实测） |
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

## 代码来源

nekodrop 是在其他开源项目的基础上做的。这里写清楚哪些是**直接用了源码**（复制、移植或打包），哪些只是**参考**。

### 用到的源码

| 项目 | 许可 | 用了什么 | 在 nekodrop 里的位置 |
|---|---|---|---|
| [retropineaple/ForbiddenItlwm](https://github.com/retropineaple/ForbiddenItlwm) 0.6.0（itlwm 分支，macOS） | GPL-2.0 | Intel 22000 系列驱动 `hal_iwx`：固件加载、TLV 解析、context info、收发环、固件命令、寄存器头文件 `if_iwxreg.h`。还有它的 AWDL 策略：信道跟随（44/149 的 min 规则、国家码学习）、接收 Block Ack、BA 期间关混杂、速率阶梯、主机侧单播重发、信标提示 | `iwxradio/`，从内核 C 移植成用户态 C++ 驱动 |
| ForbiddenItlwm 的固件目录 | Intel 再分发许可 | iwlwifi 固件 `iwlwifi-Qu*-hr-b0-68.ucode` | 随安装包放在 `radio\firmware\` |
| [raph559/winawdl](https://github.com/raph559/winawdl) | MIT | 在 Windows 上从用户态接管 PCIe 网卡的方法：UMDF 寄存器代理驱动（原本给 Realtek RTL8852CE 用）和用户态 DMA；radio 管道协议 | `driver/regproxy/`（改成 AX201 的 PCI ID）、`iwxradio/`（管道和 DMA） |
| winawdl 的 `awdl/`（`awdld`） | GPL-3.0 或更新（基于 OWL） | AWDL 守护进程：OWL 的协议核心加上 Windows I/O 层和 Wintun | 从源码编译，随安装包放在 `awdl\awdld.exe` |
| winawdl 的 `airdrop/` | MIT | Python 写的 AirDrop 引擎：mDNS、HTTPS 接收端、发送端、蓝牙信标 | 放在 `airdrop\`，打了两个小补丁（`scripts/patch-engine.py`：固定实例名、备用端口） |
| [seemoo-lab/owl](https://github.com/seemoo-lab/owl) | GPL-3.0 | `awdld` 里的 AWDL 实现 | 经由 winawdl |
| [Wintun](https://www.wintun.net/) | Wintun 许可 | 虚拟网卡 `awdl0` | `awdl\wintun.dll`（预编译） |
| [TAP-Windows6](https://github.com/OpenVPN/tap-windows6) | GPL-2.0 / OpenVPN | Windows 上网用的“nekodrop”虚拟以太网卡 | 安装时装上（OpenVPN 签名的安装程序） |

### 参考过的项目（没有复制代码）

| 项目 | 参考了什么 |
|---|---|
| OpenBSD `iwx(4)`、Linux `iwlwifi` | 固件、UMAC 扫描、站点模式、密钥的工作方式 |
| [seemoo-lab/opendrop](https://github.com/seemoo-lab/opendrop) | AirDrop 协议（`/Discover`、`/Ask`、`/Upload`、mDNS 记录） |
| [microsoft/Windows-driver-samples](https://github.com/microsoft/Windows-driver-samples) | 已搁置的内核驱动（`kdriver/`）的 WiFiCx 结构 |
| [t4t5/owly](https://github.com/t4t5/owly)、[JungyoKim/WirDrop](https://github.com/JungyoKim/WirDrop) | 其他平台上实现 AirDrop 的尝试，设计时对比参考 |

### nekodrop 自己写的部分

- `iwxradio/`：用户态移植本身，以及所有 Windows 特有的部分：两个 MAC 同时运行（AWDL 监听加上独立 PHY 上的 Wi-Fi 站点）、WPA2 站点模式（四次握手、密钥交给固件、TLC 速率控制）、TAP 桥接、固件 UMAC 扫描、信标解析和加密方式识别。
- `app/service/`：Windows 服务：网卡接管、radio 和 awdld 监管、Wi-Fi 管理（DPAPI 加密保存的网络、自动连接、隐藏网络、网页登录识别）。
- `app/client/`：托盘程序（WPF）、通知、发送窗口、Wi-Fi 窗口、中英文界面。
- `app/shellext/`：Windows 11 右键菜单。
- `installer/`：Inno Setup 安装包、本机驱动签名、TAP 配置和卸载。

## 许可

nekodrop 整体以 **GPL-2.0** 发布（见 [LICENSE](LICENSE)），因为 `iwxradio/` 派生自 itlwm/ForbiddenItlwm。

- `if_iwxreg.h`：BSD/GPL-2.0 双许可。
- `driver/regproxy` 和 AirDrop 引擎：沿用 winawdl 的 MIT 许可。
- `awdld`：GPL-3.0 或更新，来自 winawdl/OWL，是通过管道通信的独立程序。
- iwlwifi 固件：Intel 再分发许可（`licenses\iwlwifi-firmware.txt`）。
- Wintun 和 TAP-Windows：各按其许可。

安装包会把所有许可证复制到 `licenses\`。

## 免责声明

nekodrop 是独立项目，与 Apple、Intel 没有任何关系，也未获得它们的认可。AirDrop、iPhone、iPad、Mac 是 Apple Inc. 的商标，Intel 是 Intel Corporation 的商标。nekodrop 运行期间会替换你的 Wi-Fi 驱动。这是实验性软件，按原样提供，不附带任何担保。
