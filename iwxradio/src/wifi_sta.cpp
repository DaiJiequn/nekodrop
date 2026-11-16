// SPDX-License-Identifier: GPL-2.0-only
/*
 * Station state machine. Frame layouts are IEEE 802.11-2016 (9.3.3, 9.4.2,
 * 12.7); the WPA2 handshake follows 12.7.6 with key descriptor version 2
 * (HMAC-SHA1 MIC, AES key wrap), the one AKM 2 uses.
 */
#include "wifi_sta.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>

#include "awdl_policy.h"
#include "crypto.h"
#include "iwxreg.h"

namespace {

constexpr uint16_t kEtherEapol = 0x888e;
constexpr uint8_t kRsnOui[3] = {0x00, 0x0f, 0xac};
constexpr uint64_t kStepTimeoutUS = 400000;
constexpr uint64_t kHandshakeTimeoutUS = 5000000;
constexpr uint64_t kRejoinDelayUS = 5000000;

/* Key information bits (12.7.2). */
constexpr uint16_t kKeyVersionMask = 0x0007, kKeyPairwise = 0x0008, kKeyInstall = 0x0040,
                   kKeyAck = 0x0080, kKeyMic = 0x0100, kKeySecure = 0x0200,
                   kKeyEncryptedData = 0x1000;

bool parse_mac(const std::string &s, uint8_t out[6]) {
  unsigned v[6];
  if (std::sscanf(s.c_str(), "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6 &&
      std::sscanf(s.c_str(), "%x-%x-%x-%x-%x-%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6)
    return false;
  for (int i = 0; i < 6; i++) out[i] = static_cast<uint8_t>(v[i]);
  return true;
}

bool parse_hex(const std::string &s, uint8_t *out, size_t n) {
  if (s.size() != n * 2) return false;
  for (size_t i = 0; i < n; i++) {
    unsigned v;
    if (std::sscanf(s.c_str() + 2 * i, "%2x", &v) != 1) return false;
    out[i] = static_cast<uint8_t>(v);
  }
  return true;
}

size_t hdrlen(const uint8_t *f, size_t len) {
  if (len < 24) return 0;
  size_t h = 24;
  if ((f[1] & 0x03) == 0x03) h += 6;
  if ((f[0] & 0x0c) == 0x08 && (f[0] & 0x80)) h += 2;
  return h <= len ? h : 0;
}

void put_le16(std::vector<uint8_t> &v, uint16_t x) {
  v.push_back(static_cast<uint8_t>(x));
  v.push_back(static_cast<uint8_t>(x >> 8));
}

void put_be16(std::vector<uint8_t> &v, uint16_t x) {
  v.push_back(static_cast<uint8_t>(x >> 8));
  v.push_back(static_cast<uint8_t>(x));
}

uint16_t be16(const uint8_t *p) { return static_cast<uint16_t>(p[0] << 8 | p[1]); }
uint16_t le16(const uint8_t *p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }

/* WPA2-PSK, CCMP pairwise and group: what we ask for and accept. */
std::vector<uint8_t> our_rsn_ie() {
  return {0x30, 0x14, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x04, 0x01, 0x00, 0x00, 0x0f,
          0xac, 0x04, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x02, 0x00, 0x00};
}

TxRate mgmt_rate() {
  TxRate r;
  r.kind = TxRate::Legacy;
  r.mbps_x2 = 12; /* 6 Mb/s OFDM: valid in both bands */
  return r;
}

} // namespace

/* ------------------------------------------------------------- config */

bool StaConfig::load(const std::string &path, std::string &error) {
  std::ifstream in(path);
  if (!in) {
    error = "cannot read " + path;
    return false;
  }
  std::string line;
  bool have_bssid = false;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const size_t eq = line.find('=');
    if (eq == std::string::npos || line[0] == '#') continue;
    const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
    if (k == "ssid") ssid = v;
    else if (k == "bssid") have_bssid = parse_mac(v, bssid);
    else if (k == "channel") channel = static_cast<uint8_t>(std::atoi(v.c_str()));
    else if (k == "passphrase") passphrase = v;
    else if (k == "psk") psk_hex = v;
    else if (k == "open") open = v == "1" || v == "true";
    else if (k == "tap") tap = v;
    else if (k == "width") max_width = static_cast<unsigned>(std::atoi(v.c_str()));
  }
  if (ssid.empty() || ssid.size() > 32) error = "ssid missing or longer than 32 bytes";
  else if (!have_bssid) error = "bssid missing";
  else if (!awdl::fixed_schedule_channel(channel) && !(channel >= 52 && channel <= 144) && channel != 165)
    error = "unsupported channel " + std::to_string(channel);
  else if (!open && psk_hex.empty() && (passphrase.size() < 8 || passphrase.size() > 63))
    error = "need a passphrase of 8-63 characters, a 64-digit psk, or open=1";
  if (!error.empty()) return false;
  return true;
}

/* -------------------------------------------------------------- start */

const char *WifiStation::state_name() const {
  switch (_state) {
  case State::Idle: return "idle";
  case State::Listening: return "waiting for a beacon";
  case State::Authenticating: return "authenticating";
  case State::Associating: return "associating";
  case State::Handshake: return "4-way handshake";
  case State::Running: return "connected";
  case State::Failed: return "failed";
  }
  return "?";
}

bool WifiStation::start(const StaConfig &cfg, std::string &error) {
  _cfg = cfg;
  if (!_cfg.open) {
    if (!_cfg.psk_hex.empty()) {
      if (!parse_hex(_cfg.psk_hex, _pmk, 32)) {
        error = "psk is not 64 hex digits";
        return false;
      }
    } else if (!crypto::pmk_from_passphrase(_cfg.passphrase, _cfg.ssid, _pmk)) {
      error = "PBKDF2 failed";
      return false;
    }
  }
  if (!_tap.is_open() && !_tap.open(_cfg.tap, error)) return false;
  _addr = _tap.mac();
  _tap.set_connected(false);
  _rsn_ie = our_rsn_ie();
  StaParams p{};
  std::memcpy(p.bssid, _cfg.bssid, 6);
  std::memcpy(p.addr, _addr.data(), 6);
  p.channel = _cfg.channel;
  p.width = 20;
  if (!_dev.sta_join(p)) {
    error = "the firmware refused the station context";
    return false;
  }
  _joins++;
  _state = State::Listening;
  _state_since = _now;
  _tries = 0;
  char b[96];
  std::snprintf(b, sizeof(b), "station: using TAP adapter \"%s\", address %02x:%02x:%02x:%02x:%02x:%02x",
                _tap.name().c_str(), _addr[0], _addr[1], _addr[2], _addr[3], _addr[4], _addr[5]);
  _log(b);
  return true;
}

void WifiStation::stop() {
  if (_state == State::Idle) return;
  _tap.set_connected(false);
  _dev.sta_leave();
  _state = State::Idle;
}

void WifiStation::fail(const std::string &why) {
  _log("station: " + why + "; retrying in 5 s");
  _tap.set_connected(false);
  _dev.sta_leave();
  _keys_installed = _ptk_ready = _replay_valid = false;
  _state = State::Failed;
  _state_since = _now;
}

/* -------------------------------------------------------------- demux */

bool WifiStation::owns(const RxFrame &f) const {
  if (_state == State::Idle || f.len < 24) return false;
  const uint8_t type = f.data[0] & 0x0c;
  if (type == 0x04) return false; /* control: leave to the firmware */
  const uint8_t *a1 = f.data + 4, *a2 = f.data + 10, *a3 = f.data + 16;
  if (std::memcmp(a2, _cfg.bssid, 6) == 0) return true;        /* from the AP */
  if (std::memcmp(a3, _cfg.bssid, 6) == 0 && (a1[0] & 1)) return true; /* our BSS's broadcasts */
  return false;
}

void WifiStation::on_frame(const RxFrame &f) {
  const uint8_t type = f.data[0] & 0x0c;
  if (type == 0x00) handle_mgmt(f);
  else if (type == 0x08) handle_data(f);
}

/* ---------------------------------------------------------- management */

void WifiStation::learn_beacon(const uint8_t *f, size_t len) {
  if (len < 36) return;
  _beacon_int = le16(f + 32);
  _ap_capab = le16(f + 34);
  _ap_rsn = _ap_ht = _ap_vht = _ap_2ss = false;
  _ap_width = 20;
  _ap_rates.clear();
  for (size_t i = 36; i + 2 <= len;) {
    const uint8_t id = f[i], n = f[i + 1];
    if (i + 2 + n > len) break;
    const uint8_t *v = f + i + 2;
    if (id == 1 || id == 50) _ap_rates.insert(_ap_rates.end(), v, v + n);
    else if (id == 5 && n >= 2) _dtim = v[1] ? v[1] : 1;
    else if (id == 48 && n >= 18) {
      /* Group cipher and the first pairwise/AKM must be CCMP / PSK. */
      const bool ccmp = std::memcmp(v + 2, kRsnOui, 3) == 0 && v[5] == 4;
      _ap_rsn = ccmp;
    } else if (id == 45 && n >= 26) {
      _ap_ht = true;
      _ap_2ss = v[4] != 0; /* MCS 8-15 supported */
    } else if (id == 61 && n >= 2) {
      if (v[1] & 0x03) _ap_width = 40; /* secondary channel offset */
    } else if (id == 191 && n >= 12) {
      _ap_vht = true;
    } else if (id == 192 && n >= 1) {
      if (v[0] >= 1) _ap_width = 80;
    }
    i += 2 + n;
  }
  if (!_beacon_seen) {
    _beacon_seen = true;
    _log(std::string("station: beacon heard: ") + (_ap_rsn ? "WPA2" : (_ap_capab & 0x10) ? "protected (not WPA2-CCMP)" : "open") +
         (_ap_ht ? ", HT" : "") + (_ap_vht ? ", VHT" : "") + ", " + std::to_string(_ap_width) + " MHz");
  }
}

void WifiStation::send_auth() {
  std::vector<uint8_t> f(24, 0);
  f[0] = 0xb0;
  std::memcpy(&f[4], _cfg.bssid, 6);
  std::memcpy(&f[10], _addr.data(), 6);
  std::memcpy(&f[16], _cfg.bssid, 6);
  f[22] = static_cast<uint8_t>(++_mgmt_seq << 4);
  f[23] = static_cast<uint8_t>(_mgmt_seq >> 4);
  put_le16(f, 0); /* open system */
  put_le16(f, 1); /* sequence 1 */
  put_le16(f, 0); /* status */
  _dev.sta_transmit(f.data(), f.size(), mgmt_rate(), false);
}

void WifiStation::send_assoc() {
  std::vector<uint8_t> f(24, 0);
  f[0] = 0x00;
  std::memcpy(&f[4], _cfg.bssid, 6);
  std::memcpy(&f[10], _addr.data(), 6);
  std::memcpy(&f[16], _cfg.bssid, 6);
  f[22] = static_cast<uint8_t>(++_mgmt_seq << 4);
  f[23] = static_cast<uint8_t>(_mgmt_seq >> 4);
  uint16_t cap = 0x0001 | 0x0400; /* ESS, short slot time */
  if (_ap_capab & 0x0020) cap |= 0x0020; /* short preamble */
  put_le16(f, cap);
  put_le16(f, 10); /* listen interval */
  f.push_back(0);
  f.push_back(static_cast<uint8_t>(_cfg.ssid.size()));
  f.insert(f.end(), _cfg.ssid.begin(), _cfg.ssid.end());
  std::vector<uint8_t> rates = _ap_rates;
  if (rates.empty()) {
    if (_cfg.channel <= 14) rates = {0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24, 0x30, 0x48, 0x60, 0x6c};
    else rates = {0x8c, 0x12, 0x98, 0x24, 0xb0, 0x48, 0x60, 0x6c};
  }
  const size_t first = std::min<size_t>(8, rates.size());
  f.push_back(1);
  f.push_back(static_cast<uint8_t>(first));
  f.insert(f.end(), rates.begin(), rates.begin() + first);
  if (rates.size() > first) {
    f.push_back(50);
    f.push_back(static_cast<uint8_t>(rates.size() - first));
    f.insert(f.end(), rates.begin() + first, rates.end());
  }
  if (!_cfg.open) f.insert(f.end(), _rsn_ie.begin(), _rsn_ie.end());
  if (_ap_ht) {
    /* HT capabilities: 20/40 (if the AP is wide), SM PS off, short GI 20/40,
     * 1 RX STBC stream; 64 KiB A-MPDU, 8 us spacing; MCS 0-15 (two chains). */
    const uint16_t htcap = static_cast<uint16_t>((_ap_width >= 40 ? 0x0002 : 0) | 0x000c | 0x0020 |
                                                 (_ap_width >= 40 ? 0x0040 : 0) | 0x0100);
    const uint8_t ht[28] = {45,
                            26,
                            static_cast<uint8_t>(htcap),
                            static_cast<uint8_t>(htcap >> 8),
                            0x1b,
                            0xff,
                            0xff,
                            0,
                            0,
                            0,
                            0,
                            0,
                            0,
                            0,
                            0,
                            0,
                            0,
                            0x01,
                            0,
                            0,
                            0};
    f.insert(f.end(), ht, ht + sizeof(ht));
  }
  if (_ap_vht && _cfg.channel > 14) {
    /* VHT capabilities: short GI 80, 1 RX STBC stream, 1 MiB A-MPDU;
     * MCS 0-9 on two streams. */
    const uint8_t vht[14] = {191, 12, 0x20, 0x01, 0x80, 0x03, 0xfa, 0xff, 0, 0, 0xfa, 0xff, 0, 0};
    f.insert(f.end(), vht, vht + sizeof(vht));
  }
  _dev.sta_transmit(f.data(), f.size(), mgmt_rate(), false);
}

void WifiStation::handle_mgmt(const RxFrame &f) {
  const uint8_t sub = f.data[0] & 0xf0;
  if (sub == 0x80 || sub == 0x50) { /* beacon, probe response */
    learn_beacon(f.data, f.len);
    return;
  }
  if (std::memcmp(f.data + 4, _addr.data(), 6) != 0) return;
  if (sub == 0xb0 && _state == State::Authenticating && f.len >= 30) {
    const uint16_t alg = le16(f.data + 24), seq = le16(f.data + 26), status = le16(f.data + 28);
    if (alg == 0 && seq == 2) {
      if (status != 0) return fail("authentication refused, status " + std::to_string(status));
      _state = State::Associating;
      _state_since = _now;
      _tries = 0;
      _last_try = 0;
    }
  } else if ((sub == 0x10 || sub == 0x30) && _state == State::Associating && f.len >= 30) {
    const uint16_t status = le16(f.data + 26), aid = le16(f.data + 28);
    if (status != 0) return fail("association refused, status " + std::to_string(status));
    unsigned width = 20;
    if (_ap_ht && _ap_width >= 40) width = std::min(_ap_width, _cfg.max_width);
    StaParams p = _dev.sta_params();
    p.ht = _ap_ht;
    p.vht = _ap_vht && _cfg.channel > 14;
    p.mimo = _ap_2ss;
    p.width = width;
    p.beacon_int = _beacon_int;
    _dev.sta_join_update(p);
    if (!_dev.sta_associated(aid, _dtim)) return fail("the firmware refused the association");
    _dev.sta_rate_control();
    if (_cfg.open) {
      _state = State::Running;
      _keys_installed = true;
      _tap.set_connected(true);
      _log("station: connected (open network)");
    } else {
      _state = State::Handshake;
      _replay_valid = false;
    }
    _state_since = _now;
  } else if (sub == 0xc0 || sub == 0xa0) { /* deauthentication, disassociation */
    const uint16_t reason = f.len >= 26 ? le16(f.data + 24) : 0;
    if (_state != State::Failed) fail("the access point disconnected us, reason " + std::to_string(reason));
  } else if (sub == 0xd0 && f.len >= 33) {
    /* Block Ack request from the AP: firmware session + answer. */
    awdl::AddbaRequest r;
    if (awdl::parse_addba_request(f.data, f.len, _addr.data(), &r) && r.immediate &&
        _dev.sta_add_rx_ba(r.tid, r.ssn, r.window)) {
      std::vector<uint8_t> a(awdl::kAddbaResponseLength);
      awdl::build_addba_response(a.data(), r, _addr.data(), ++_mgmt_seq);
      if (_addba_answers.size() < 8) _addba_answers.push_back(std::move(a));
    }
  }
}

bool WifiStation::take_addba(std::vector<uint8_t> &answer) {
  if (_addba_answers.empty()) return false;
  answer = std::move(_addba_answers.front());
  _addba_answers.pop_front();
  return true;
}

/* ---------------------------------------------------------------- data */

void WifiStation::handle_data(const RxFrame &f) {
  const uint8_t flags = f.data[1];
  if ((flags & 0x03) != 0x02) return;        /* FromDS only */
  if (f.data[0] & 0x40) return;               /* null / QoS null */
  const size_t h = hdrlen(f.data, f.len);
  if (!h) return;
  const uint8_t *da = f.data + 4, *sa = f.data + 16;
  if (std::memcmp(sa, _addr.data(), 6) == 0) return; /* our own broadcast, echoed */
  const uint8_t *body = f.data + h;
  size_t len = f.len - h;
  if (flags & 0x40) {
    /* Protected: the firmware decrypted it, or it is dropped. */
    if ((f.status & (IWX_RX_MPDU_RES_STATUS_DEC_DONE | IWX_RX_MPDU_RES_STATUS_MIC_OK)) !=
        (IWX_RX_MPDU_RES_STATUS_DEC_DONE | IWX_RX_MPDU_RES_STATUS_MIC_OK))
      return;
    if (len < 8) return;
    body += 8; /* CCMP header stays in place */
    len -= 8;
    if (!f.mic_stripped) {
      if (len < 8) return;
      len -= 8;
    }
  }
  if (len < 8 || body[0] != 0xaa || body[1] != 0xaa || body[2] != 0x03) return;
  const uint16_t ethertype = be16(body + 6);
  body += 8;
  len -= 8;
  if (ethertype == kEtherEapol) {
    handle_eapol(body, len);
    return;
  }
  if (_state != State::Running) return;
  std::vector<uint8_t> eth;
  eth.reserve(14 + len);
  eth.insert(eth.end(), da, da + 6);
  eth.insert(eth.end(), sa, sa + 6);
  put_be16(eth, ethertype);
  eth.insert(eth.end(), body, body + len);
  if (_tap.write(eth.data(), eth.size())) _rx_packets++;
}

void WifiStation::send_data(const uint8_t *da, uint16_t ethertype, const uint8_t *payload, size_t len,
                            bool protect) {
  std::vector<uint8_t> f(24, 0);
  f[0] = 0x08;
  f[1] = static_cast<uint8_t>(0x01 | (protect ? 0x40 : 0)); /* ToDS */
  std::memcpy(&f[4], _cfg.bssid, 6);
  std::memcpy(&f[10], _addr.data(), 6);
  std::memcpy(&f[16], da, 6);
  const uint8_t llc[6] = {0xaa, 0xaa, 0x03, 0, 0, 0};
  f.insert(f.end(), llc, llc + 6);
  put_be16(f, ethertype);
  f.insert(f.end(), payload, payload + len);
  TxRate rate;
  if (ethertype == kEtherEapol) {
    rate = mgmt_rate();
  } else if (_dev.sta_tlc()) {
    rate.kind = TxRate::Auto;
  } else if (_ap_ht) {
    rate.kind = TxRate::Ht;
    rate.mcs = _ap_2ss ? 12 : 7; /* 2x2 MCS 12 or 1x1 MCS 7 */
  } else {
    rate.kind = TxRate::Legacy;
    rate.mbps_x2 = 108; /* 54 Mb/s */
  }
  if (_dev.sta_transmit(f.data(), f.size(), rate, protect) >= 0) _tx_packets++;
}

void WifiStation::on_ethernet(const uint8_t *frame, size_t len) {
  if (_state != State::Running || len < 14) return;
  /* The adapter's own address is ours; anything else would be bridged
   * traffic, which a station may not send. */
  if (std::memcmp(frame + 6, _addr.data(), 6) != 0) return;
  const uint16_t ethertype = be16(frame + 12);
  send_data(frame, ethertype, frame + 14, len - 14, _keys_installed && !_cfg.open);
}

/* ---------------------------------------------------------------- WPA2 */

bool WifiStation::derive_ptk(const uint8_t anonce[32]) {
  uint8_t data[6 + 6 + 32 + 32];
  const bool ap_low = std::memcmp(_cfg.bssid, _addr.data(), 6) < 0;
  std::memcpy(data, ap_low ? _cfg.bssid : _addr.data(), 6);
  std::memcpy(data + 6, ap_low ? _addr.data() : _cfg.bssid, 6);
  const bool a_low = std::memcmp(anonce, _snonce, 32) < 0;
  std::memcpy(data + 12, a_low ? anonce : _snonce, 32);
  std::memcpy(data + 44, a_low ? _snonce : anonce, 32);
  return crypto::prf_sha1(_pmk, 32, "Pairwise key expansion", data, sizeof(data), _ptk, 48);
}

/* EAPOL-Key layout (12.7.2): version, type 3, length; descriptor 2; key info
 * at 5; key length 7; replay 9; nonce 17; IV 49; RSC 65; reserved 73; MIC 81;
 * data length 97; data 99. */
bool WifiStation::mic_ok(const uint8_t *eapol, size_t len) {
  if (len < 99) return false;
  std::vector<uint8_t> copy(eapol, eapol + len);
  std::memset(copy.data() + 81, 0, 16);
  uint8_t mac[20];
  if (!crypto::hmac_sha1(_ptk, 16, copy.data(), copy.size(), mac)) return false;
  return std::memcmp(mac, eapol + 81, 16) == 0;
}

void WifiStation::add_mic(std::vector<uint8_t> &eapol) {
  std::memset(eapol.data() + 81, 0, 16);
  uint8_t mac[20];
  crypto::hmac_sha1(_ptk, 16, eapol.data(), eapol.size(), mac);
  std::memcpy(eapol.data() + 81, mac, 16);
}

void WifiStation::send_eapol(const std::vector<uint8_t> &eapol, bool protect) {
  static const uint8_t kAp[6] = {};
  (void)kAp;
  send_data(_cfg.bssid, kEtherEapol, eapol.data(), eapol.size(), protect);
}

void WifiStation::handle_eapol(const uint8_t *body, size_t len) {
  if (_cfg.open || len < 99 || body[1] != 3 || body[4] != 2) return;
  const size_t total = 4 + be16(body + 2);
  if (total > len || total < 99) return;
  const uint16_t info = be16(body + 5);
  if ((info & kKeyVersionMask) != 2) {
    _log("station: unsupported EAPOL key version " + std::to_string(info & kKeyVersionMask) +
         " (WPA3 or PMF-SHA256 network?)");
    return;
  }
  const uint8_t *replay = body + 9;
  if (_replay_valid && std::memcmp(replay, _replay, 8) <= 0) return; /* replayed */
  const uint16_t data_len = be16(body + 97);
  if (99u + data_len > total) return;
  const uint8_t *key_data = body + 99;

  auto make_reply = [&](uint16_t reply_info, const std::vector<uint8_t> &kd) {
    std::vector<uint8_t> e(99, 0);
    e[0] = body[0]; /* the AP's EAPOL version */
    e[1] = 3;
    const uint16_t blen = static_cast<uint16_t>(95 + kd.size());
    e[2] = static_cast<uint8_t>(blen >> 8);
    e[3] = static_cast<uint8_t>(blen);
    e[4] = 2;
    e[5] = static_cast<uint8_t>(reply_info >> 8);
    e[6] = static_cast<uint8_t>(reply_info);
    std::memcpy(&e[9], replay, 8);
    e[97] = static_cast<uint8_t>(kd.size() >> 8);
    e[98] = static_cast<uint8_t>(kd.size());
    e.insert(e.end(), kd.begin(), kd.end());
    return e;
  };

  if ((info & kKeyPairwise) && (info & kKeyAck) && !(info & kKeyMic)) {
    /* Message 1: ANonce. */
    std::memcpy(_anonce, body + 17, 32);
    crypto::random_bytes(_snonce, 32);
    if (!derive_ptk(_anonce)) return;
    _ptk_ready = true;
    std::memcpy(_replay, replay, 8);
    _replay_valid = true;
    std::vector<uint8_t> m2 = make_reply(2 | kKeyPairwise | kKeyMic, _rsn_ie);
    std::memcpy(&m2[17], _snonce, 32);
    add_mic(m2);
    send_eapol(m2, false);
    _log("station: 4-way handshake 1/4 received, 2/4 sent");
  } else if ((info & kKeyPairwise) && (info & kKeyAck) && (info & kKeyMic) && (info & kKeyInstall)) {
    /* Message 3: verify, unwrap the GTK, answer, install. */
    if (!_ptk_ready || std::memcmp(body + 17, _anonce, 32) != 0) return;
    if (!mic_ok(body, total)) {
      _log("station: message 3/4 has a bad MIC (wrong password?)");
      return fail("4-way handshake failed: check the Wi-Fi password");
    }
    std::memcpy(_replay, replay, 8);
    if ((info & kKeyEncryptedData) && data_len >= 24) {
      std::vector<uint8_t> plain;
      if (!crypto::aes_unwrap(_ptk + 16, 16, key_data, data_len, plain))
        return fail("could not unwrap the group key");
      for (size_t i = 0; i + 2 <= plain.size();) {
        const uint8_t id = plain[i], n = plain[i + 1];
        if (id == 0xdd && n == 0 ) break; /* padding */
        if (i + 2 + n > plain.size()) break;
        if (id == 0xdd && n >= 6 && std::memcmp(&plain[i + 2], kRsnOui, 3) == 0 && plain[i + 5] == 1) {
          _gtk_index = plain[i + 6] & 3;
          _gtk_len = std::min<unsigned>(n - 6, sizeof(_gtk));
          std::memcpy(_gtk, &plain[i + 8], _gtk_len);
        }
        i += 2 + n;
      }
    }
    std::vector<uint8_t> m4 = make_reply(2 | kKeyPairwise | kKeyMic | kKeySecure, {});
    add_mic(m4);
    send_eapol(m4, false);
    /* Keys go in once message 4 has left: installing first would make the
     * firmware encrypt it. */
    _install_at = _now + 20000;
    _log("station: 4-way handshake 3/4 received, 4/4 sent");
  } else if (!(info & kKeyPairwise) && (info & kKeyAck) && (info & kKeyMic)) {
    /* Group key handshake (rekey). */
    if (!_keys_installed || !mic_ok(body, total)) return;
    std::memcpy(_replay, replay, 8);
    std::vector<uint8_t> plain;
    if ((info & kKeyEncryptedData) && crypto::aes_unwrap(_ptk + 16, 16, key_data, data_len, plain)) {
      for (size_t i = 0; i + 2 <= plain.size();) {
        const uint8_t id = plain[i], n = plain[i + 1];
        if (i + 2 + n > plain.size()) break;
        if (id == 0xdd && n >= 6 && std::memcmp(&plain[i + 2], kRsnOui, 3) == 0 && plain[i + 5] == 1) {
          _gtk_index = plain[i + 6] & 3;
          _gtk_len = std::min<unsigned>(n - 6, sizeof(_gtk));
          std::memcpy(_gtk, &plain[i + 8], _gtk_len);
          _dev.sta_set_key(true, _gtk_index, _gtk);
        }
        i += 2 + n;
      }
    }
    std::vector<uint8_t> g2 = make_reply(2 | kKeyMic | kKeySecure, {});
    add_mic(g2);
    send_eapol(g2, true);
    _log("station: group key renewed");
  }
}

/* --------------------------------------------------------------- timers */

void WifiStation::tick(uint64_t now_us) {
  _now = now_us;
  switch (_state) {
  case State::Listening:
    /* A beacon tells us the AP's capabilities; after 1.5 s go anyway. */
    if (_beacon_seen || now_us - _state_since > 1500000) {
      if (!_cfg.open && _beacon_seen && !_ap_rsn && (_ap_capab & 0x10)) {
        fail("the network is not WPA2-Personal with CCMP (WPA3/TKIP are not supported)");
        return;
      }
      _state = State::Authenticating;
      _state_since = now_us;
      _tries = 0;
      _last_try = 0;
    }
    break;
  case State::Authenticating:
  case State::Associating:
    if (!_last_try || now_us - _last_try > kStepTimeoutUS) {
      if (++_tries > 6) {
        fail(_state == State::Authenticating ? "no answer to authentication" : "no answer to association");
        return;
      }
      _last_try = now_us;
      if (_state == State::Authenticating) send_auth();
      else send_assoc();
    }
    break;
  case State::Handshake:
    if (_install_at && now_us >= _install_at) {
      _install_at = 0;
      if (!_dev.sta_set_key(false, 0, _ptk + 32)) return fail("could not install the pairwise key");
      if (_gtk_len) _dev.sta_set_key(true, _gtk_index, _gtk);
      _keys_installed = true;
      _state = State::Running;
      _state_since = now_us;
      _tap.set_connected(true);
      _log("station: connected to \"" + _cfg.ssid + "\" (WPA2)");
    } else if (now_us - _state_since > kHandshakeTimeoutUS) {
      fail("no 4-way handshake from the access point");
    }
    break;
  case State::Failed:
    if (now_us - _state_since > kRejoinDelayUS) {
      std::string error;
      _beacon_seen = false;
      if (!start(_cfg, error)) {
        _state = State::Failed;
        _state_since = now_us;
      }
    }
    break;
  default:
    break;
  }
}
