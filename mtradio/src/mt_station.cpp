// SPDX-License-Identifier: GPL-2.0-only
#include "mt_station.h"

#include <cstring>

namespace {

/* Fixed data rates for the AP, slowest first (as the AWDL ladder). */
struct Level {
  const char *name;
  uint16_t rate;
};
const Level kLevels[] = {{"OFDM24", Mt7902::tx_rate_ofdm(24)}, {"MCS2", Mt7902::tx_rate_ht(2)},
                         {"MCS4", Mt7902::tx_rate_ht(4)},      {"MCS5", Mt7902::tx_rate_ht(5)},
                         {"MCS7", Mt7902::tx_rate_ht(7)}};
constexpr unsigned kLevelCount = sizeof(kLevels) / sizeof(kLevels[0]);
constexpr size_t kHeldMax = 4096; /* a quarter second away at full speed */

uint16_t seq_of(const uint8_t *f) { return static_cast<uint16_t>((f[22] | f[23] << 8) >> 4); }

} // namespace

bool MtStation::sta_join(const StaParams &p) {
  _params = p;
  _active = true;
  _associated = false;
  _aid = 0;
  _ptk.clear();
  for (auto &k : _gtk) k.clear();
  _tx_pn = 0;
  std::memset(_rx_pn, 0, sizeof(_rx_pn));
  std::memset(_rx_group_pn, 0, sizeof(_rx_group_pn));
  _last_rx_seq[0] = _last_rx_seq[1] = -1;
  _held.clear();
  if (!_dev.set_station_identity(p.addr, p.bssid)) {
    _log("station: the card refused the station's own address");
    return false;
  }
  return true;
}

bool MtStation::sta_associated(uint16_t aid, uint8_t) {
  _aid = aid;
  _associated = true;
  return true;
}

bool MtStation::sta_set_key(bool group, unsigned key_index, const uint8_t key[16]) {
  if (group) {
    const unsigned id = key_index & 3;
    _rx_group_pn[id] = 0;
    return _gtk[id].set(key);
  }
  _tx_pn = 0;
  std::memset(_rx_pn, 0, sizeof(_rx_pn));
  return _ptk.set(key);
}

void MtStation::sta_leave() {
  if (_active) _dev.clear_station_identity();
  _active = _associated = false;
  _ptk.clear();
  for (auto &k : _gtk) k.clear();
  _held.clear();
  _inflight.clear();
  _null_token = -1;
}

uint16_t MtStation::data_rate() const { return kLevels[_level].rate; }

std::string MtStation::rate_name() const { return kLevels[_level].name; }

int MtStation::sta_transmit(const uint8_t *frame, size_t len, const TxRate &rate, bool encrypt) {
  if (!_active || len < 24) return -1;
  std::vector<uint8_t> f(frame, frame + len);
  /* One sequence number space for everything the station sends: WifiStation
   * leaves data frames at 0 (Intel's firmware numbers them). */
  _tx_seq = static_cast<uint16_t>((_tx_seq + 1) & 0xfff);
  f[22] = static_cast<uint8_t>((f[22] & 0x0f) | (_tx_seq << 4));
  f[23] = static_cast<uint8_t>(_tx_seq >> 4);
  const bool unicast_data = (f[0] & 0x0c) == 0x08 && !(f[4] & 1);
  uint16_t r;
  if (rate.kind == TxRate::Legacy) {
    const unsigned x2 = rate.mbps_x2;
    const bool cck = _params.channel <= 14 && (x2 == 2 || x2 == 4 || x2 == 11 || x2 == 22);
    r = cck ? Mt7902::tx_rate_cck(x2) : Mt7902::tx_rate_ofdm(x2 / 2);
  } else {
    r = data_rate();
  }
  if (encrypt) {
    if (!_ptk.valid()) return -1;
    std::vector<uint8_t> sealed;
    f[1] &= static_cast<uint8_t>(~0x40);
    if (!_ptk.seal(f.data(), f.size(), ++_tx_pn, 0, sealed)) return -1;
    f.swap(sealed);
  }
  if (!_here) {
    if (_held.size() >= kHeldMax) {
      _held.pop_front();
      _stats.tx_dropped++;
    }
    _held.push_back({std::move(f), r, unicast_data});
    _stats.tx_held++;
    return 0;
  }
  return send_now(f, r, unicast_data) ? 0 : -1;
}

bool MtStation::send_now(const std::vector<uint8_t> &f, uint16_t rate, bool data) {
  /* The station is on 5 GHz only, where the card completes frames that wait
   * for an acknowledgement. */
  _dev.force_no_ack = -1;
  _dev.force_altx = -1;
  const int token = _dev.transmit(f.data(), f.size(), rate, Mt7902::kStationOmac);
  if (token < 0) return false;
  _inflight[static_cast<unsigned>(token)] = data;
  _stats.tx++;
  return true;
}

void MtStation::flush() {
  while (_here && !_held.empty()) {
    if (!send_now(_held.front().frame, _held.front().rate, _held.front().data)) break;
    _held.pop_front();
  }
}

void MtStation::on_channel(bool here) {
  _here = here && _active;
  flush();
}

int MtStation::send_null(bool sleep) {
  if (!_active || !_associated) return -1;
  uint8_t f[24] = {0x48, static_cast<uint8_t>(0x01 | (sleep ? 0x10 : 0))}; /* null data, to DS */
  std::memcpy(f + 4, _params.bssid, 6);
  std::memcpy(f + 10, _params.addr, 6);
  std::memcpy(f + 16, _params.bssid, 6);
  _tx_seq = static_cast<uint16_t>((_tx_seq + 1) & 0xfff);
  f[22] = static_cast<uint8_t>(_tx_seq << 4);
  f[23] = static_cast<uint8_t>(_tx_seq >> 4);
  _dev.force_no_ack = -1;
  _dev.force_altx = 1;
  const int token = _dev.transmit(f, sizeof(f), Mt7902::tx_rate_ofdm(6), Mt7902::kStationOmac);
  _dev.force_altx = -1;
  if (token >= 0) {
    _inflight[static_cast<unsigned>(token)] = false;
    _null_token = token;
    _null_sleep = sleep;
    sleep ? _stats.sleep_sent++ : _stats.wake_sent++;
  }
  return token;
}

bool MtStation::on_tx_status(unsigned token, bool acked) {
  auto it = _inflight.find(token);
  if (it == _inflight.end()) return false;
  const bool data = it->second;
  _inflight.erase(it);
  if (static_cast<int>(token) == _null_token) {
    _null_token = -1;
    if (acked) _null_sleep ? _stats.sleep_acked++ : _stats.wake_acked++;
  }
  acked ? _stats.tx_acked++ : _stats.tx_failed++;
  if (data) {
    _sent++;
    _failed += !acked;
    if (_sent >= 32) {
      if (_failed * 100 / _sent > 30 && _level > 0) _level--;
      else if (_failed * 100 / _sent < 8 && _level + 1 < kLevelCount) _level++;
      _sent = _failed = 0;
    }
  }
  return true;
}

bool MtStation::receive(const Mt7902::RxFrame &in, RxFrame &out, std::vector<uint8_t> &buf) {
  _stats.rx++;
  const uint8_t *f = in.data;
  out = RxFrame{in.data, in.len, in.dbm, in.channel, 0, 0, false};
  if (in.len < 24) return false;
  const bool data = (f[0] & 0x0c) == 0x08;
  const bool group = (f[4] & 1) != 0;
  if (f[1] & 0x08) _stats.rx_retry++;
  /* Repeats: the card acknowledges, but a lost acknowledgement still brings
   * the frame again with the retry bit. */
  if (!group) {
    const int32_t seq = seq_of(f);
    int32_t &last = _last_rx_seq[data ? 1 : 0];
    if ((f[1] & 0x08) && seq == last) {
      _stats.rx_repeat++;
      return false;
    }
    last = seq;
  }
  if (!(f[1] & 0x40)) return true;
  const size_t h = ccmp::header_len(f, in.len);
  if (!h || in.len < h + ccmp::kHeaderLen + ccmp::kMicLen) return false;
  const unsigned key_id = f[h + 3] >> 6;
  const ccmp::Key &key = group ? _gtk[key_id] : _ptk;
  if (!key.valid()) {
    _stats.rx_no_key++;
    return true; /* WifiStation counts and drops it as undecrypted */
  }
  if (!key.open(f, in.len, buf)) {
    _stats.rx_bad_mic++;
    return false;
  }
  const uint64_t pn = ccmp::packet_number(f + h);
  uint64_t *last;
  if (group) last = &_rx_group_pn[key_id];
  else if (!data) last = &_rx_pn[16];
  else last = &_rx_pn[(f[0] & 0x80) ? (f[(f[1] & 0x03) == 0x03 ? 30 : 24] & 0x0f) : 0];
  if (pn <= *last) {
    _stats.rx_replay++;
    return false;
  }
  *last = pn;
  _stats.rx_opened++;
  out.data = buf.data();
  out.len = buf.size();
  out.status = kStaRxDecDone | kStaRxMicOk;
  out.mic_stripped = true;
  return true;
}
