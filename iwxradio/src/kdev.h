// SPDX-License-Identifier: GPL-2.0-only
/*
 * KernelDevice: the listener of nekodrop-wlan.sys (the kernel Wi-Fi driver)
 * behind the same calls radio_main makes on IwxDevice, so the AWDL engine
 * and its upstream 0.6.0 policies run unchanged while Windows keeps its Wi-Fi
 * on the same card. Talks to \\.\NekodropAwdl (kdriver/nekoioctl.h). The
 * station calls exist only to satisfy radio_main; Windows owns the station.
 */
#pragma once

#include "plat.h"
#include "iwx.h"

#include <atomic>
#include <deque>
#include <mutex>
#include <thread>

class KernelDevice {
public:
  using Logger = IwxDevice::Logger;
  using RxHandler = IwxDevice::RxHandler;
  using TxStatusHandler = IwxDevice::TxStatusHandler;

  explicit KernelDevice(Logger log) : _log(std::move(log)) {}
  ~KernelDevice() { stop(); }

  bool open(const IwxDevice::Options &options, std::string &error);
  bool start_monitor(uint8_t channel, unsigned width_mhz, const uint8_t address[6]);
  bool set_channel(uint8_t channel, unsigned width_mhz);
  bool set_address(const uint8_t address[6]);
  bool add_rx_ba(const uint8_t peer[6], unsigned tid, uint16_t ssn, uint16_t window, uint8_t token);
  bool remove_rx_ba(unsigned tid);
  bool rx_ba_open(unsigned tid) const { return tid < 8 && (_ba_tids >> tid) & 1; }
  bool set_promiscuous(bool on);
  bool promiscuous() const { return _promisc; }
  bool set_country(const std::string &alpha2);
  uint32_t channel_flags(uint8_t channel) const;
  /* The kernel driver answers the firmware's country notice itself. */
  bool answer_country_notice(std::string *) { return false; }
  /* Experimental A-MPDU sending is a user-mode feature: never available here. */
  struct AggStats {
    uint64_t frames = 0, notifs = 0, txed = 0, acked = 0, singles = 0;
    uint32_t last_rate = 0, peak_frames = 0;
  };
  bool agg_available() const { return false; }
  bool agg_ready() const { return false; }
  bool agg_setup(const uint8_t *, bool, bool) { return false; }
  bool agg_enable(bool) { return false; }
  bool agg_pause(bool) { return false; }
  int transmit_agg(const uint8_t *, size_t) { return -1; }
  unsigned agg_queued() const { return 0; }
  uint16_t agg_seq() const { return 0; }
  uint32_t ba_epoch() const { return 0; }
  const uint8_t *ba_peer() const { return nullptr; }
  const AggStats &agg_stats() const { return _agg_stats; }
  bool transmit(const uint8_t *frame, size_t len, const TxRate &rate) { return transmit_slot(frame, len, rate) >= 0; }
  /* The driver rings the doorbell itself: `kick` and kick_data() are for the user-mode path. */
  int transmit_slot(const uint8_t *frame, size_t len, const TxRate &rate, bool kick = true);
  void kick_data() {}
  void set_tx_status_handler(TxStatusHandler handler) { _on_tx = std::move(handler); }
  void set_rx_handler(RxHandler handler) { _on_rx = std::move(handler); }
  int poll();
  void check_health();
  bool healthy() const { return _handle != INVALID_HANDLE_VALUE && _healthy; }
  AggStats _agg_stats;
  const TxStats &tx_stats() const { return _stats; }
  uint8_t channel() const { return _channel; }
  unsigned width_mhz() const { return _width; }
  const std::array<uint8_t, 6> &hw_address() const { return _hw; }
  void stop();

  /* Windows runs the station on this card. */
  int sta_transmit(const uint8_t *, size_t, const TxRate &, bool) { return -1; }
  unsigned sta_tx_acked() const { return 0; }
  unsigned sta_tx_failed() const { return 0; }

private:
  bool ioctl(DWORD code, const void *in, DWORD inLength, void *out, DWORD outLength, DWORD *got = nullptr) const;
  void reader();

  Logger _log;
  RxHandler _on_rx;
  TxStatusHandler _on_tx;
  HANDLE _handle = INVALID_HANDLE_VALUE;
  std::thread _reader;
  std::atomic<bool> _stop{false};
  std::mutex _lock;
  std::deque<std::vector<uint8_t>> _batches; /* IOCTL_NEKO_READ results */
  std::array<uint8_t, 6> _hw{};
  uint8_t _channel = 0;
  unsigned _width = 20;
  bool _promisc = true, _healthy = true;
  uint8_t _ba_tids = 0;
  uint32_t _flags[4] = {};
  TxStats _stats;
};
