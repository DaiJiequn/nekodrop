# nekodrop

让 Windows 用**板载的 Intel AX201 网卡**收发 AirDrop，并且**同时保持 Wi-Fi 上网**。不需要 USB 网卡，也不需要 Linux 虚拟机。

它把两个开源项目拼在一起：

| 来源 | 用到的部分 |
|---|---|
| [retropineaple/ForbiddenItlwm](https://github.com/retropineaple/ForbiddenItlwm) 0.6.0（macOS，GPL-2.0） | Intel 22000 系列驱动（itlwm `hal_iwx`），以及 AWDL 监听、信道跟随（44/149 的 min 规则、国家码学习）、接收 Block Ack、BA 期间关混杂、速率阶梯、单播重发、信标提示等策略 |
| [raph559/winawdl](https://github.com/raph559/winawdl)（Windows，MIT/部分 GPL） | 不写内核驱动就能接管 PCIe 网卡的框架（UMDF 寄存器代理 + 用户态 DMA）、radio 管道协议、AWDL 守护进程（OWL 移植）、AirDrop 收发引擎 |

本项目新写的部分：用户态 Intel 驱动（`iwxradio/`）、站点模式（WPA2 + TAP 桥接，联网同时 AirDrop 靠它）、Windows 服务和托盘客户端（`app/`）、安装包（`installer/`）。

## 状态（2026-11-16）

| 部分 | 状态 |
|---|---|
| 用户态驱动：固件启动、监听、收发 | **实机验证通过**（AX201 / 固件 68） |
| iPhone → Windows AirDrop | **实机验证通过**：869 KB 照片完整收到两次，哈希一致 |
| Windows → iPhone | 实机试过：能发现、对方能接受，但传输停滞。原因已定位（固件对注入帧只发一次，没收到 ACK 就报 LONG_LIMIT），已按上游移植主机侧重发，**待实机复测** |
| 上游 0.6.0 策略：信道跟随、国家码、Block Ack、BA 过滤、速率阶梯、重发、信标提示 | 已移植，离线单元测试 15/15 通过（含用实测 iPhone 帧判定信道 44），**待实机复测** |
| 联网同时 AirDrop（站点模式：认证、关联、WPA2 四次握手、固件密钥、TLC 速率控制、TAP 桥接） | 已实现，加密对 IEEE/RFC 官方向量 5/5 通过，**未上机** |
| Windows 服务、托盘客户端、系统通知、设置、中英双语 | 已实现；三个窗口（发送、主界面、设置）中英文离屏渲染检查通过；服务的网卡检测和 Wi-Fi 读取已在本机验证 |
| 右键“用 nekodrop 发送” | Windows 11 第一级菜单（IExplorerCommand + 稀疏包，安装时用本机证书签名）和经典菜单/发送到；点击后弹出发送窗口选设备。签名已验证，**注册待安装后实测** |
| **内核 Wi-Fi 驱动 `nekodrop-wlan.sys`**（WiFiCx：Windows 自带 Wi-Fi 开关照常可用，同时跑 AirDrop） | 已编译，静态检查通过（导入表只有 ntoskrnl/HAL/WDFLDR，INF 校验通过）；扫描、WPA2 连接、数据通路、AWDL 接口都已实现；**待开测试签名后实机加载**，见 [docs/KERNEL.md](docs/KERNEL.md) |
| 一键安装包 `nekodrop-0.1.0-setup.exe` | 自带 .NET 运行时、Python、驱动、TAP、服务，中英文安装界面，失败会弹出原因和日志位置 |

## 结构

```
nekodrop.exe（托盘客户端，用户会话）──JSON 管道──► NekoDropService（SYSTEM）
     │  AirDrop 引擎（Python，winawdl）                 │ 切换网卡驱动 / 读 Wi-Fi 配置
     │  系统通知 · 右键菜单 · 发送窗口                    ▼
     └──────── awdl0 (Wintun) ◄── awdld ◄──管道── nekodrop-radio（用户态 Intel 驱动）
                                                        ├ MAC 0：AWDL 监听（LISTENER）
                     TAP 以太网适配器 ◄── 站点桥接 ◄──────┤ MAC 1：BSS_STA 连路由器
                                                        └ UMDF 寄存器代理 → AX201
```

设计细节见 [docs/DESIGN.md](docs/DESIGN.md)（驱动、DMA、风险、Windows 驱动调研）和 [docs/CLIENT.md](docs/CLIENT.md)（服务、客户端、界面规范、联网模式）。

## 构建

需要：VS 2022 Build Tools（C++、Windows SDK、CMake）、.NET 8 SDK。WDK 用 NuGet 包（`third_party\wdk-nuget`），不需要管理员权限。

```powershell
.\scripts\build-installer.ps1      # 全部构建，出 build\nekodrop-0.1.0-setup.exe（装了 Inno Setup 6 时）或 zip
.\scripts\build-radio.ps1          # 只构建用户态驱动并跑离线自检
```

## 安装与卸载

解压 zip，以管理员身份运行 `install.ps1`（`-DryRun` 只列出会做的事）。卸载运行安装目录里的 `uninstall.ps1`：它会先把网卡还给 Intel 驱动，再删除服务、驱动包、证书信任和防火墙规则。

## 许可

`iwxradio/` 派生自 itlwm/ForbiddenItlwm，GPL-2.0；`if_iwxreg.h` 是 BSD/GPLv2 双许可；`driver/regproxy` 和 AirDrop 引擎沿用 winawdl 的 MIT；iwlwifi 固件按 Intel 的再分发许可附带；Wintun 和 TAP-Windows 各按其许可。
