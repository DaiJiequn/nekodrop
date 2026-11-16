# nekodrop kernel driver (WiFiCx)

Why: with the user-mode radio, Windows loses its Wi-Fi adapter the moment the
card leaves Intel's driver (no Wi-Fi switch, no network list; internet only
through a TAP "Ethernet"). Windows lists Wi-Fi only from a WLAN miniport. So
nekodrop becomes one: a KMDF + NetAdapterCx + **WiFiCx** driver for the AX201
that serves Windows' Wi-Fi (scan, connect, WPA2 through WlanSvc, data path) and,
beside it, the AWDL interface that awdld and the AirDrop engine already use.
This is ForbiddenItlwm's model on macOS: one driver does both.

## What is reused

| Source | Becomes |
|---|---|
| `iwxradio/` (hardware-verified on this AX201: firmware boot, monitor/listener MAC, AWDL TX/RX, station MAC 1, keys, TLC, Block Ack, 0.6.0 policies) | the hardware layer, with `RegProxy` replaced by mapped BAR0 (MmMapIoSpaceEx) and `DmaPool` by WDF common buffers |
| itlwm `hal_iwx` / OpenBSD net80211 | what iwxradio lacks for Windows: UMAC scan (`iwx_umac_scan`), scan result parsing, interrupts (MSI-X causes, `iwx_intr_msix`) |
| `wifi_sta.cpp` | MLME for WDI connect (auth, assoc, deauth); the 4-way handshake moves to Windows (EAPOL goes up the data path, keys come down as WDI_SET_ADD_CIPHER_KEYS) |
| awdld + AirDrop engine + client | unchanged; the radio pipe protocol is carried by a device IOCTL instead of a named pipe |

## Layout

```
kdriver/
  driver.cpp      DriverEntry, EvtDeviceAdd (WifiDeviceInitConfig/Initialize), PnP/power
  hw.cpp          BAR0, interrupts, DMA (kernel twin of iwxradio's proxy + dma)
  wdi.cpp         EvtWifiDeviceSendCommand: WDI task/property dispatch (TlvGeneratorParser)
  adapter.cpp     NetAdapter, TX/RX rings <-> firmware queues
  awdl_ioctl.cpp  private interface for awdld (frames, channel, events)
  nekodrop-wlan.inf
```

## WDI messages, in order of need

1. Open/init: `WDI_TASK_OPEN`, `WDI_GET_ADAPTER_CAPABILITIES` (interface, station, band, PHY), `WDI_TASK_CREATE_PORT`.
2. Scan: `WDI_TASK_SCAN` -> UMAC scan -> `WDI_INDICATION_BSS_ENTRY_LIST`, `WDI_INDICATION_SCAN_COMPLETE`.
3. Connect: `WDI_TASK_CONNECT` -> auth/assoc -> `WDI_INDICATION_ASSOCIATION_RESULT`, `WDI_INDICATION_CONNECT_COMPLETE`; EAPOL through the data path; `WDI_SET_ADD_CIPHER_KEYS`; `WDI_TASK_DISCONNECT`, `WDI_INDICATION_DISASSOCIATION`.
4. Data: NetAdapter TX/RX queues, 802.11 <-> Ethernet in the driver (WiFiCx data path is Ethernet frames).
5. Power, roaming, link quality: later.

## Concurrency with AWDL

Station on MAC 1 / its own PHY and binding (hardware-proven 2026-11-17), the
AWDL listener on MAC 0 following the peer channel (6 / 44 / 149) as upstream.

## Signing

Development builds are test-signed. Secure Boot is off on the target PC; the
user enables test signing once (`bcdedit /set testsigning on`, reboot). A
release needs attestation signing through the Microsoft partner portal.

## Status

- [ ] skeleton builds (KMDF + WiFiCx, binds DEV_43F0, loads)
- [ ] hardware layer in kernel: firmware boots
- [ ] WDI open + capabilities: Windows shows a Wi-Fi adapter
- [ ] scan
- [ ] connect WPA2-Personal, internet
- [ ] AWDL IOCTL + awdld beside the station
