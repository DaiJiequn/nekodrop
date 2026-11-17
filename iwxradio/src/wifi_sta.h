// SPDX-License-Identifier: GPL-2.0-only
/*
 * The Wi-Fi station that runs beside AWDL, host side: join, WPA2-PSK, and the
 * bridge between the access point and Windows' TAP adapter.
 *
 * Supported: open networks and WPA2-Personal (AKM 2, CCMP), which is what
 * the firmware's key cache does in hardware. WPA3 (SAE), enterprise and
 * WPA1/TKIP are refused at start; the service then uses on-demand mode.
 */
#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

#include "iwx.h"
#include "tap.h"

struct StaConfig {
  std::string ssid;
  uint8_t bssid[6] = {};
  uint8_t channel = 0;
  std::string passphrase; /* 8-63 characters, or empty with `psk_hex` or `open` */
  std::string psk_hex;    /* 64 hex digits */
  bool open = false;
  std::string tap;        /* TAP adapter name or GUID; empty: the first */
  unsigned max_width = 80;
  /* Receive Block Ack sessions with the AP. Off by default until the host
   * reorders by BAID/NSSN as Linux does: requests are declined (status 37)
   * and the AP sends unaggregated (Codex review #24, agreed fix). */
  bool block_ack = false;

  /* key=value lines: ssid, bssid, channel, passphrase, psk, open, tap, width, ba. */
  bool load(const std::string &path, std::string &error);
};

class WifiStation {
public:
  using Logger = std::function<void(const std::string &)>;
  enum class State { Idle, Listening, Authenticating, Associating, Handshake, Running, Failed };

  WifiStation(IwxDevice &dev, Logger log) : _dev(dev), _log(std::move(log)) {}

  bool start(const StaConfig &cfg, std::string &error);
  /* Whether a received frame belongs to the station (our BSS). */
  bool owns(const RxFrame &f) const;
  void on_frame(const RxFrame &f);
  /* An Ethernet frame Windows sent on the TAP adapter. */
  void on_ethernet(const uint8_t *frame, size_t len);
  /* Timers: call every few milliseconds from the device thread. */
  void tick(uint64_t now_us);
  void stop();

  State state() const { return _state; }
  const char *state_name() const;
  TapAdapter &tap() { return _tap; }
  uint64_t rx_packets() const { return _rx_packets; }
  uint64_t tx_packets() const { return _tx_packets; }
  /* Unicast only: Windows' multicast (mDNS, SSDP, LLMNR) never gets answers. */
  uint64_t tx_unicast() const { return _tx_unicast; }
  uint64_t rx_unicast() const { return _rx.unicast; }
  /* Receive Block Ack requests from the AP, answered like AWDL's. */
  bool take_addba(std::vector<uint8_t> &answer);
  /* Data frames from the AP and why some never reached Windows. */
  struct RxStats {
    uint64_t data = 0, unicast = 0, undecrypted = 0, not_llc = 0, amsdu = 0, tap_failed = 0;
  };
  const RxStats &rx_stats() const { return _rx; }
  /* Leaves and joins again in 5 s (the radio's stall watchdog). */
  void rejoin(const std::string &why) {
    if (_state != State::Idle && _state != State::Failed) fail(why);
  }

private:
  void send_auth();
  void send_assoc();
  void handle_mgmt(const RxFrame &f);
  void handle_data(const RxFrame &f);
  void deliver(const uint8_t *da, const uint8_t *sa, const uint8_t *body, size_t len, bool encrypted);
  void handle_eapol(const uint8_t *body, size_t len, bool encrypted);
  bool send_eapol(const std::vector<uint8_t> &eapol, bool protect);
  void reset_join_state();
  bool install_gtk();
  void learn_beacon(const uint8_t *f, size_t len);
  void fail(const std::string &why);
  bool send_data(const uint8_t *da, uint16_t ethertype, const uint8_t *payload, size_t len, bool protect);
  bool derive_ptk(const uint8_t anonce[32], uint8_t out[48]);
  /* The KCK (and KEK at +16) of the handshake in progress: the pending PTK
   * during a rekey, else the one in use. */
  const uint8_t *kck() const { return _rekey ? _tptk : _ptk; }
  /* The GTK KDE from unwrapped key data: a 16-byte CCMP key, else false. */
  bool take_gtk(const std::vector<uint8_t> &plain);
  bool mic_ok(const uint8_t *eapol, size_t len, const uint8_t *key = nullptr);
  void add_mic(std::vector<uint8_t> &eapol, const uint8_t *key = nullptr);

  IwxDevice &_dev;
  Logger _log;
  TapAdapter _tap;
  StaConfig _cfg;
  std::array<uint8_t, 6> _addr{};
  State _state = State::Idle;
  uint64_t _now = 0, _state_since = 0, _last_try = 0;
  unsigned _tries = 0, _joins = 0;

  /* What the beacon told us. */
  bool _beacon_seen = false, _ap_rsn = false, _ap_ht = false, _ap_vht = false, _ap_2ss = false;
  uint16_t _ap_capab = 0, _beacon_int = 100;
  uint8_t _dtim = 1;
  unsigned _ap_width = 20;
  std::vector<uint8_t> _ap_rates;

  /* WPA2 */
  uint8_t _pmk[32] = {};
  uint8_t _ptk[48] = {}; /* KCK 16 | KEK 16 | TK 16 */
  uint8_t _tptk[48] = {}; /* a rekey's PTK until it is installed */
  bool _rekey = false, _ptk_installed = false;
  uint64_t _rekey_since = 0;
  uint8_t _gtk_in_fw[16] = {}, _gtk_in_fw_idx = 0;
  bool _gtk_in_fw_valid = false;
  uint8_t _ba_token[8] = {}, _ba_token_valid = 0;
  uint16_t _ba_ssn[8] = {}, _ba_window[8] = {};
  bool _m4_unsent = false; /* 4/4 could not be queued; its keys wait for the resend */
  /* The last EAPOL reply, sent again when the AP repeats its message. */
  std::vector<uint8_t> _last_reply;
  bool _last_reply_protect = false;
  uint8_t _last_reply_replay[8] = {};
  unsigned _declined_ba = 0;
  uint8_t _snonce[32] = {}, _anonce[32] = {};
  uint8_t _replay[8] = {};
  bool _replay_valid = false, _keys_installed = false, _ptk_ready = false;
  uint64_t _install_at = 0;
  uint8_t _gtk[32] = {};
  unsigned _gtk_len = 0, _gtk_index = 1;
  std::vector<uint8_t> _rsn_ie;

  std::deque<std::vector<uint8_t>> _addba_answers;
  uint16_t _mgmt_seq = 0;
  uint64_t _rx_packets = 0, _tx_packets = 0, _tx_unicast = 0;
  RxStats _rx;
};
