# winairdrop

让 Windows 用**板载的 Intel AX201 网卡**直接收发 AirDrop：不用 USB 网卡，也不用 Linux 虚拟机。

这个项目把两个开源项目拼到了一起（“二合一”）：

| 来源 | 拿来的是什么 |
|---|---|
| [retropineaple/ForbiddenItlwm](https://github.com/retropineaple/ForbiddenItlwm)（macOS 内核驱动，GPL-2.0） | 怎么驱动 Intel 22000 系列网卡（itlwm 的 `hal_iwx`），以及它摸索出的 AWDL 监听方式：用 `LISTENER` 类型的 MAC 上下文，以 AWDL 地址作为本机地址，打开混杂过滤，让固件替我们给 iPhone 回 ACK |
| [raph559/winawdl](https://github.com/raph559/winawdl)（Windows，MIT，部分 GPL） | 在 Windows 上不写内核驱动就能接管 PCIe 网卡的整套框架：UMDF 用户态寄存器代理驱动、AWE 锁页内存做 DMA、命名管道 radio 协议，以及跑在上面的 AWDL 守护进程（OWL 移植）、AirDrop 收发引擎和界面 |

winawdl 只支持 Realtek RTL8852CE，它的芯片驱动层（devourer）只认 Realtek。本项目用一个**新写的用户态 Intel 驱动**替换掉那一层，对上仍说 winawdl 的 radio 管道协议，所以 winawdl 上层的东西不用改就能接上。

```
iPhone ──AWDL──> AX201 ──PCIe──> winairdrop_regproxy (UMDF 寄存器代理, driver/regproxy)
                                     │ 寄存器读写 (DeviceIoControl)
                                     ▼
                      winawdl_radio.exe  ← 本项目新写 (iwxradio/)
                      · 加载 iwlwifi 固件 (context info, gen2)
                      · AWE 物理内存做 RX/TX 环
                      · LISTENER MAC 上下文 + monitor 站点 + 发送队列
                                     │ \\.\pipe\winawdl-radio
                                     ▼
                      awdld (winawdl/OWL)  →  Wintun awdl0  →  AirDrop 引擎 (winawdl)
```

## 当前状态

**能编译，但还没在网卡上跑过。** 下面这些代码写好了，但都还没上机验证：

| 部分 | 状态 |
|---|---|
| `driver/regproxy`：匹配 Intel AX201 PCI ID、BAR 下限改成 8 KiB | 已改好，待编译签名 |
| `iwxradio/`：用户态 Intel 驱动（上电、固件加载、ALIVE、NVM、MCC、PHY/MAC/binding/station/队列、收包、发包） | 已写完，待上机 |
| `iwxradio/tests/selftest`：固件 ABI 结构体大小、三份 Qu/QuZ 固件解析 | 离线可跑 |
| winawdl 的 awdld / AirDrop 引擎 / App | 直接沿用；App 里写死了 Realtek 的硬件 ID，现阶段用脚本启动，不走 App |

最大的未知数是**固件能不能在用户态 DMA 下启动**。winawdl 在 Realtek 上验证过这条路（开着内核 DMA 保护也行）；Intel 的固件加载方式不同（卡自己按 context info 去拉固件分段），要上机才知道。

## 上机测试前必须知道的

- **接管网卡期间 Windows 没有 Wi-Fi。** 你这台电脑现在只靠 AX201 上网（有线网卡没插线），所以测试前要先插网线或用手机 USB 共享网络，否则会断网。`take-card.ps1` 检测到没有别的网络时会拒绝执行。
- 驱动用本机生成的证书签名，证书会被加进本机的受信任根证书和受信任发布者里（winawdl 也是这么做的）。`give-card.ps1` 会撤销这些。
- 用户态 DMA：网卡可以写进程锁定的物理内存。进程异常退出时，代理驱动会执行预先登记好的“停止 DMA + 复位”寄存器写入（沿用 winawdl 的三层防护）。
- 蓝牙不受影响：AX201 的蓝牙是单独的 USB 设备。

## 构建

需要：VS 2022 Build Tools（C++、Windows 11 SDK、**C++ Clang tools**）、WDK 10.0.26100、CMake/Ninja（随 Build Tools 一起装）。

```powershell
.\driver\regproxy\build.ps1     # 编译寄存器代理驱动
.\scripts\package-driver.ps1    # 打包并用本机证书签名
.\scripts\build-radio.ps1       # 编译 winawdl_radio.exe 并跑离线自检
```

## 上机步骤（插好网线之后）

```powershell
# 管理员 PowerShell
.\scripts\take-card.ps1                                   # 把 AX201 交给代理驱动
.\build\iwxradio\winawdl_radio.exe 6 --country CN         # 先在 2.4 GHz 信道 6 上起监听
# 另开窗口：winawdl 的 AWDL 守护进程和 AirDrop 引擎，见 docs/DESIGN.md
.\scripts\give-card.ps1                                   # 测完把网卡还给 Intel 驱动
```

## 许可

用户态 Intel 驱动派生自 itlwm/ForbiddenItlwm（GPL-2.0），因此 `iwxradio/` 采用 GPL-2.0；寄存器定义头文件 `if_iwxreg.h` 为 BSD/GPLv2 双许可；`driver/regproxy` 沿用 winawdl 的 MIT；iwlwifi 固件按 Intel 的再分发许可附带。
