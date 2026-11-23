# mtradio — MediaTek MT7902 radio

`nekodrop-radio-mt.exe` drives a MediaTek MT7902 (`14C3:7902`) from user mode
through the register proxy (`driver/regproxy`), like the Intel and Realtek
radios. It is a port of the parts of Linux [mt76](https://github.com/openwrt/mt76)
(`mt7921/`, `mt76_connac*`, mt76 commit `41d5d1d`) that AirDrop needs:

- firmware boot (ROM patch + RAM code) and WFDMA rings in AWE pages below 4 GiB
- the firmware's sniffer on one channel for receiving, and cut-through TX with
  TX-free reports for sending
- one ad-hoc interface whose address the MAC acknowledges
- a STA record per AWDL peer: firmware rate control and Block Ack sessions,
  so A-MPDU works both ways
- the radio pipe protocol (`radio_link.h`) and the AWDL policy ported from
  ForbiddenItlwm, so the service and awdld treat it like the other radios

The card has no station mode here, so the service runs it on demand.

## Firmware

It needs linux-firmware's `WIFI_RAM_CODE_MT7902_1.bin` and
`WIFI_MT7902_patch_mcu_1_1_hdr.bin` (`scripts/fetch-deps.ps1` downloads them,
pinned and hash-checked). The copy in MediaTek's Windows driver boots, but it
ignores the unified (UNI) commands this port sends.

## Build and test

    scripts\fetch-deps.ps1
    scripts\build-radio-mt.ps1      # -> build\mtradio

Hardware tests take the card from Windows for a minute (the wired network
stays up) and always give it back to MediaTek's driver. Run them elevated:

- `mtradio\scripts\smoke-test.ps1` — binds the register proxy from a local
  `driver\regproxy` build, runs `mtprobe.exe` (chip ID, DMA, firmware boot)
- `mtradio\scripts\test-radio.ps1` — receive and send rounds on 2.4 and 5 GHz
- `mtradio\scripts\test-installed.ps1` — the same with nekodrop installed, using
  its register proxy; e.g. `-Runs '44 --test-sta 60'`

`mtprobe` and the daemon run as SYSTEM through a one-shot scheduled task:
AWE pages need SeLockMemoryPrivilege.

## Known limits

- On 2.4 GHz, frames that need an ACK never complete (cause unknown; maybe
  Bluetooth coexistence), so the daemon sends without ACK there. AirDrop
  itself moves to 5 GHz.
- Sending to an iPhone is slower than receiving (about 5 vs 15 MB/s measured).

## Licence

GPL-2.0-only like nekodrop. The parts taken from mt76 are BSD-3-Clause-Clear /
ISC (MediaTek, Lorenzo Bianconi and others): see `LICENSE.mt76`.
