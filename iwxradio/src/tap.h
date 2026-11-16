// SPDX-License-Identifier: GPL-2.0-only
/* A TAP-Windows (tap0901) Ethernet adapter: how the station's traffic reaches
 * Windows. Windows runs DHCP, ARP and IPv6 on it as on any wired adapter; the
 * station uses the adapter's MAC address as its own on the air, so nothing in
 * the frames needs rewriting. */
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

class TapAdapter {
public:
  ~TapAdapter();
  /* Opens the first tap0901 adapter, or the one whose name or GUID is given. */
  bool open(const std::string &wanted, std::string &error);
  bool is_open() const { return _handle != nullptr; }
  const std::array<uint8_t, 6> &mac() const { return _mac; }
  const std::string &name() const { return _name; }
  /* Marks the cable plugged in or out, which is what starts Windows' DHCP. */
  bool set_connected(bool up);
  /* Blocks up to `timeout_ms` for one Ethernet frame. Empty on timeout. */
  bool read(std::vector<uint8_t> &frame, unsigned timeout_ms);
  bool write(const uint8_t *frame, size_t len);

private:
  void *_handle = nullptr;
  void *_read_event = nullptr, *_write_event = nullptr;
  bool _read_pending = false;
  std::vector<uint8_t> _read_buf;
  std::array<uint8_t, 6> _mac{};
  std::string _name;
  alignas(8) unsigned char _read_ov[64] = {};
};
