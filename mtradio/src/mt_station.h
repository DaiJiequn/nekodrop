// SPDX-License-Identifier: GPL-2.0-only
/* The Wi-Fi station's radio side on the MT7902 (StaDevice, iwxradio's
 * sta_device.h), for WifiStation to run the join, WPA2/WPA3 and the TAP
 * bridge as it does on Intel cards.
 *
 * The card stays in sniffer mode for AWDL, so the station is done on the
 * host: a second own address (own MAC 1) makes the MAC acknowledge frames to
 * the station, frames go out through the same fixed-rate TX path as AWDL's,
 * and CCMP is sealed and opened here (ccmp.h). The card has one channel at a
 * time (mt7921: num_different_channels 1): the radio's scheduler moves it
 * between the access point and AWDL, and frames for the AP wait here while
 * it is away (on_channel). */
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "ccmp.h"
#include "mt7902.h"
#include "sta_device.h"

class MtStation final : public StaDevice {
public:
  using Logger = std::function<void(const std::string &)>;
  MtStation(Mt7902 &dev, Logger log) : _dev(dev), _log(std::move(log)) {}

  /* ---- StaDevice ---- */
  bool sta_join(const StaParams &p) override;
  bool sta_associated(uint16_t aid, uint8_t dtim_period) override;
  void sta_join_update(const StaParams &p) override { _params = p; }
  bool sta_set_key(bool group, unsigned key_index, const uint8_t key[16]) override;
  bool sta_set_mfp(bool on) override {
    _mfp = on;
    return true;
  }
  /* Broadcast management frames' integrity (BIP) is not checked on the host. */
  bool sta_set_igtk(unsigned, const uint8_t *, uint64_t) override { return true; }
  bool sta_rate_control() override { return false; }
  bool sta_tlc() const override { return false; }
  int sta_transmit(const uint8_t *frame, size_t len, const TxRate &rate, bool encrypt) override;
  bool sta_add_rx_ba(unsigned, uint16_t, uint16_t) override { return false; }
  bool sta_remove_rx_ba(unsigned) override { return true; }
  void sta_leave() override;
  const StaParams &sta_params() const override { return _params; }

  /* ---- The radio's side ---- */
  bool joined() const { return _active; }
  /* The AP's channel and the width to receive it at. */
  uint8_t channel() const { return _params.channel; }
  uint8_t width() const { return static_cast<uint8_t>(_associated ? _params.width : 20); }
  const uint8_t *ap() const { return _params.bssid; }
  bool associated() const { return _associated; }

  /* The card is on the AP's channel (true) or away. Queued frames go out
   * when it comes back. */
  void on_channel(bool here);
  bool here() const { return _here; }
  /* A null data frame with the power-management bit: true before leaving the
   * AP's channel (it holds our frames), false on coming back. Token or -1. */
  int send_null(bool sleep);
  bool null_pending() const { return _null_token >= 0; }

  /* A frame from the AP: false to drop it (a repeat, a replay, or one that
   * would not decrypt). Protected frames come back opened in `buf`, the CCMP
   * header in place and no MIC, with status kStaRxDecDone | kStaRxMicOk. */
  bool receive(const Mt7902::RxFrame &in, RxFrame &out, std::vector<uint8_t> &buf);
  /* TX status: true when the token was the station's. */
  bool on_tx_status(unsigned token, bool acked);

  struct Stats {
    uint64_t tx = 0, tx_acked = 0, tx_failed = 0, tx_held = 0, tx_dropped = 0;
    uint64_t rx = 0, rx_opened = 0, rx_bad_mic = 0, rx_no_key = 0, rx_replay = 0, rx_repeat = 0;
    uint64_t rx_retry = 0;                  /* the AP sent it again: it did not hear our acknowledgement */
    uint64_t sleep_sent = 0, sleep_acked = 0, wake_sent = 0, wake_acked = 0;
  };
  const Stats &stats() const { return _stats; }
  std::string rate_name() const;

private:
  struct Held {
    std::vector<uint8_t> frame;
    uint16_t rate;
    bool data;
  };
  bool send_now(const std::vector<uint8_t> &f, uint16_t rate, bool data);
  void flush();
  uint16_t data_rate() const;

  Mt7902 &_dev;
  Logger _log;
  StaParams _params{};
  bool _active = false, _associated = false, _here = false, _mfp = false;
  uint16_t _aid = 0;
  ccmp::Key _ptk, _gtk[4];
  uint64_t _tx_pn = 0;
  /* Replay counters: per TID for unicast (16 = management), per key ID for group. */
  uint64_t _rx_pn[17] = {}, _rx_group_pn[4] = {};
  uint16_t _tx_seq = 0;
  int32_t _last_rx_seq[2] = {-1, -1}; /* management, data: the last sequence number heard */
  std::deque<Held> _held;
  std::map<unsigned, bool> _inflight; /* token -> unicast data (the rate ladder counts it) */
  int _null_token = -1;
  bool _null_sleep = false;
  std::vector<uint8_t> _scratch;
  /* Fixed rates for data, slowest first; moved by the acknowledgements. */
  unsigned _level = 2, _sent = 0, _failed = 0;
  Stats _stats;
};
