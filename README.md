<p align="center">
  <img src="app/assets/nekodrop.png" width="128" alt="nekodrop">
</p>

<h1 align="center">nekodrop</h1>

<p align="center">
  <b>AirDrop on Windows, with the Intel Wi-Fi card already in your laptop. Wi-Fi keeps working.</b><br>
  No USB dongle, no Linux VM, no Mac.
</p>

<p align="center">
  English · <a href="README.zh-CN.md">简体中文</a>
</p>

<p align="center">
  <img alt="platform" src="https://img.shields.io/badge/platform-Windows%2010%20%7C%2011%20x64-0078d4">
  <img alt="card" src="https://img.shields.io/badge/card-Intel%20Wi--Fi%206%20AX201-6c5ce7">
  <img alt="license" src="https://img.shields.io/badge/license-GPL--2.0-blue">
  <img alt="status" src="https://img.shields.io/badge/status-experimental-orange">
</p>

---

## What it does

- **Receive from iPhone, iPad and Mac.** nekodrop shows up in the AirDrop sheet like any Apple device. Files land in a folder you choose (default `Downloads\nekodrop`), and you can accept or decline each transfer from a notification.
- **Send to Apple devices.** Right-click a file in Explorer and choose **Send with nekodrop**. This is on the Windows 11 first-level menu, the classic menu and *Send to*. Then pick a nearby device.
- **Stay online.** In *Wi-Fi and AirDrop together* mode nekodrop drives the card itself. It joins your Wi-Fi on one virtual interface and runs AWDL (Apple Wireless Direct Link) on the other. Windows sees your internet connection as a wired adapter called **nekodrop**.
- **Manage Wi-Fi.** Scanning, connecting, hidden networks and saved networks with auto-connect all work. Saved Windows Wi-Fi profiles are imported, and captive portals (hotel, airport and campus sign-in pages) are detected and opened in your browser.
- **Recover by itself.** A service watches the radio, the AWDL daemon and the AirDrop engine and restarts whatever stops. It starts with Windows.
- **Tray app in Chinese and English**, with a light UI, system notifications and a settings window.

## Supported hardware

nekodrop talks to the Wi-Fi chip directly, so it only works on chips it has a driver for.

| Card | Status |
|---|---|
| **Intel Wi-Fi 6 AX201** (160 MHz, CNVi): PCI IDs `8086:02F0`, `06F0`, `34F0`, `3DF0`, `4DF0`, `43F0`, `A0F0` | ✅ Supported, tested on real hardware (firmware `Qu/QuZ-hr-68`) |
| Intel Wireless-AC 9560 / 9461 / 9462 (same PCI IDs, older radio) | ❌ Not supported (different firmware family) |
| Intel AX200, AX210, AX211, AX411, BE200 | ❌ Not yet. The upstream driver supports them, so ports are possible |
| Realtek, MediaTek, Qualcomm, Broadcom | ❌ No |

To check your card: *Device Manager → Network adapters*. It should read **Intel(R) Wi-Fi 6 AX201 160MHz**. The installer refuses to run on any other card.

AX201 ships in most Intel 10th and 11th gen laptops (Comet Lake, Ice Lake, Tiger Lake, Jasper Lake).

## Requirements

- Windows 10 2004 (build 19041) or later, x64. Windows 11 recommended.
- An Intel AX201 Wi-Fi card (see above).
- Administrator rights to install.
- Bluetooth, for sending: AirDrop wakes nearby Apple devices with a Bluetooth LE beacon.
- On the Apple side, AirDrop set to **Everyone for 10 Minutes**. *Contacts Only* needs an Apple ID certificate nekodrop doesn't have.

## Install

1. Download `nekodrop-<version>-setup.exe` from [Releases](../../releases) and run it.
2. The installer:
   - copies the files to `C:\Program Files\nekodrop`;
   - creates a code-signing certificate **on your PC**, signs the small register-proxy driver with it, trusts it, and **deletes the private key**, so nothing else can ever be signed with it;
   - creates the TAP adapter **nekodrop** (TAP-Windows, signed by OpenVPN);
   - adds two inbound firewall rules, link-local addresses only (TCP 8771-8779, UDP 5353);
   - installs and starts the **NekoDropService** service.
3. The tray icon (the purple cat) appears. Open it and turn on **Allow receiving**.

On first start nekodrop takes the Wi-Fi card away from the Intel driver and reconnects you to the network Windows was using. The Windows Wi-Fi icon disappears; use nekodrop's **Wi-Fi…** window instead.

### Uninstall

Use *Settings → Apps → nekodrop → Uninstall*, or run `uninstall.ps1` in the install folder. Uninstalling gives the card back to the Intel driver, then removes the service, the driver package, the certificate trust and the firewall rules.

## Usage

| I want to… | Do this |
|---|---|
| Receive | Keep **Allow receiving** on. On the iPhone, tap Share → AirDrop and pick your PC's name |
| Send | Right-click files → **Send with nekodrop** → pick a device. Or drag files onto the main window |
| Change Wi-Fi | Tray → **Wi-Fi…**: pick a network, enter the password, Connect |
| Hidden network | **Wi-Fi…** → *Hidden network*: type the SSID and password |
| Sign in to hotel Wi-Fi | Connect; the sign-in page opens in your browser by itself |
| Give the card back to Windows | Tray → *Wi-Fi and AirDrop* → **Take the card on demand** or **AirDrop off** |
| Change name, folder or language | Tray → **Settings…** |

### Modes

| Mode | Internet | AirDrop |
|---|---|---|
| **Wi-Fi and AirDrop together** (default) | Through nekodrop's own Wi-Fi connection (adapter "nekodrop") | Always available |
| **Take the card on demand** | The normal Intel driver | nekodrop takes the card when an iPhone nearby opens AirDrop or you send. Internet drops during the transfer |
| **AirDrop off** | The normal Intel driver | Off |

### Wi-Fi security

| Network | Together mode |
|---|---|
| Open | ✅ |
| WPA/WPA2-Personal | ✅ |
| WPA2/WPA3 transition | ✅ (joins with WPA2) |
| Captive portal | ✅ detected, sign-in page opened |
| Hidden SSID | ✅ |
| WPA3-Personal only (SAE) | ❌ not yet. nekodrop switches to *on demand* |
| WPA2/WPA3-Enterprise (802.1X, PEAP…) | ❌ not yet. nekodrop switches to *on demand* |
| WEP | ❌ |

## Status

| Part | State |
|---|---|
| User-mode Intel driver: firmware boot, monitor, inject, scan | ✅ works on AX201 |
| iPhone → Windows | ✅ works (photos received intact, hash-checked) |
| Windows → iPhone | 🧪 discovery works; transfer is experimental |
| Wi-Fi and AirDrop at the same time | ✅ works (WPA2, DHCP, DNS through the "nekodrop" adapter) |
| Wi-Fi manager, saved networks, captive portal | ✅ |
| Service, tray app, notifications, context menu, installer | ✅ |
| WPA3-only, 802.1X, WEP | ⏳ planned |
| Other Intel cards (AX200/AX210/AX211/BE200) | ⏳ planned |
| Kernel WiFiCx driver (`kdriver/`) | 🧊 shelved: experimental and **not installed** by the setup. See [docs/KERNEL.md](docs/KERNEL.md) |

## How it works

```
 nekodrop.exe (tray app, your session) ──JSON pipe──► NekoDropService (SYSTEM)
   │  AirDrop engine (Python)                         │ takes/gives back the card
   │  notifications · context menu · send window      │ Wi-Fi manager, watchdogs
   │                                                  ▼
   └── awdl0 (Wintun, IPv6) ◄── awdld (AWDL, OWL) ◄─pipe─ nekodrop-radio (user-mode Intel driver)
                                                          ├ MAC 0: AWDL on channel 6/44/149
     "nekodrop" TAP adapter ◄── Ethernet bridge ◄─────────┤ MAC 1: station, joined to your router
     (Windows' internet)                                  └ UMDF register proxy + user-mode DMA → AX201
```

1. **Register proxy.** A tiny UMDF driver (`driver/regproxy`) binds to the AX201 in place of Intel's driver. It does nothing but map the card's registers and interrupts into one user-mode process. No kernel code of ours runs.
2. **User-mode driver.** `nekodrop-radio` (`iwxradio/`) is a port of the OpenBSD/itlwm `iwx` driver to user mode. It loads Intel's firmware through user-mode DMA, then runs two MACs at once: one listens and injects AWDL frames, the other is a normal Wi-Fi station with WPA2 (4-way handshake, keys in firmware, rate control).
3. **AWDL.** `awdld` (OWL, the Open Wireless Link) speaks Apple's AWDL protocol over the radio pipe and exposes the link as the IPv6 interface `awdl0`.
4. **AirDrop.** A Python engine speaks AirDrop over `awdl0`: mDNS discovery, HTTPS on port 8771 (spare ports 8772-8779), the `/Discover`, `/Ask` and `/Upload` flow, and the BLE beacon used for sending.
5. **Internet.** Frames from the station MAC are bridged to the TAP adapter "nekodrop". Windows treats it as Ethernet and gets an address by DHCP.

Design notes: [docs/DESIGN.md](docs/DESIGN.md) (driver, DMA, risks) · [docs/CLIENT.md](docs/CLIENT.md) (service, client, modes).

## Troubleshooting

| Symptom | Try |
|---|---|
| iPhone doesn't see the PC | Set the iPhone's AirDrop to *Everyone for 10 Minutes*, turn it off and on (this clears a cached record), and keep the phone unlocked nearby |
| The PC shows up twice on the iPhone | A cached record from an earlier run. Toggle AirDrop on the iPhone |
| No internet | Tray → **Wi-Fi…**: check you're connected. The network may need a sign-in page or be WPA3-only/802.1X (see above) |
| The Windows Wi-Fi icon disappeared | Expected in *together* mode: nekodrop owns the card. Use **Wi-Fi…**, or switch to *on demand* |
| Something else | Logs: `C:\ProgramData\nekodrop\service.log` (radio, AWDL, Wi-Fi) and `%LocalAppData%\nekodrop\engine.log` (AirDrop). Please attach them to an issue |

## Building from source

Needs Visual Studio 2022 Build Tools (C++, Windows SDK, CMake), the .NET 8 SDK and Git. Inno Setup 6 is optional (without it you get a zip). No WDK install is needed: the NuGet package is used.

```powershell
git clone https://github.com/zheznanohana/nekodrop.git
cd nekodrop
.\scripts\fetch-deps.ps1        # winawdl, ForbiddenItlwm (firmware), MinGW, WDK NuGet → third_party\
.\scripts\build-installer.ps1   # everything → build\nekodrop-0.1.0-setup.exe
.\scripts\build-radio.ps1       # just the user-mode driver, plus its offline self-tests
```

| Folder | What |
|---|---|
| `iwxradio/` | User-mode Intel driver (C++): firmware, AWDL monitor/inject, station, WPA2, UMAC scan |
| `driver/regproxy/` | UMDF register-proxy driver |
| `app/service/` | NekoDropService (C#): card ownership, radio supervision, Wi-Fi manager |
| `app/client/` | Tray app (C# WPF): UI, notifications, AirDrop engine host |
| `app/shellext/` | Windows 11 context-menu handler |
| `installer/` | Inno Setup script, install/uninstall scripts, TAP setup |
| `kdriver/` | Shelved kernel WiFiCx driver (experimental, not shipped) |

## Credits

nekodrop stands on these projects:

- [retropineaple/ForbiddenItlwm](https://github.com/retropineaple/ForbiddenItlwm): Intel 22000-series driver (itlwm `hal_iwx`) and AWDL strategies (channel following, country learning, Block Ack, rate stepping, unicast resend, beacon hints). GPL-2.0.
- [raph559/winawdl](https://github.com/raph559/winawdl): the user-mode takeover of a PCIe Wi-Fi card on Windows (UMDF register proxy and user-mode DMA), the radio pipe, the OWL port and the AirDrop engine. MIT.
- [seemoo-lab/owl](https://github.com/seemoo-lab/owl) and [seemoo-lab/opendrop](https://github.com/seemoo-lab/opendrop): the open AWDL and AirDrop implementations everything here descends from.
- OpenBSD `iwx(4)` and Linux `iwlwifi`.
- [Wintun](https://www.wintun.net/), [TAP-Windows](https://github.com/OpenVPN/tap-windows6).

## License

nekodrop is **GPL-2.0** (see [LICENSE](LICENSE)), because `iwxradio/` derives from itlwm/ForbiddenItlwm. `if_iwxreg.h` is dual BSD/GPL-2.0. `driver/regproxy` and the AirDrop engine keep winawdl's MIT license. The iwlwifi firmware ships under Intel's redistribution license. Wintun and TAP-Windows ship under their own licenses.

## Disclaimer

nekodrop is an independent project, not affiliated with or endorsed by Apple or Intel. AirDrop, iPhone, iPad and Mac are trademarks of Apple Inc.; Intel is a trademark of Intel Corporation. nekodrop replaces your Wi-Fi driver while it runs. It's experimental software, provided as is, without warranty.
