// SPDX-License-Identifier: GPL-2.0-only
/* nekodrop-radio-mt: owns a MediaTek MT7902 through the register proxy and
 * relays frames for one client (awdld) over winawdl's radio pipe
 * (radio_link.h), like nekodrop-radio-rtl does for the RTL8852CE.
 *
 * Usage: nekodrop-radio-mt <channel> [--width 20|40|80] [--country CC]
 *        [--fw-dir DIR] [--data-rate auto|MCSn|OFDMn]
 *        nekodrop-radio-mt <channel> --test-rx SECONDS   (no pipe: what is heard)
 *        nekodrop-radio-mt <channel> --test-tx SECONDS   (probes an access point heard)
 *
 * One thread owns the card: it drains the RX rings and the client's queue.
 * Another reads the pipe. Lines starting "radio: " are for NekoDropService
 * (Supervisor.OnRadioLine): "ready on channel", "bring-up failed: ", "rx: ". */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <timeapi.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "awdl_policy.h"
#include "mt7902.h"
#include "pipe_server.h"
#include "radio_link.h"

namespace {

using Clock = std::chrono::steady_clock;
using Mac = std::array<uint8_t, 6>;

std::atomic<bool> g_stop{false};
/* The firmware asserted or the card stopped completing frames: the daemon
 * exits so NekoDropService starts a fresh radio (Supervisor's watchdog). */
std::atomic<bool> g_card_dead{false};
/* Peer stations exist: a firmware crash now is blamed on them. */
std::atomic<bool> g_stations_up{false};

/* After the firmware crashed with peer stations, the next radios run
 * without them for a while (fixed rates, no A-MPDU), so transfers finish. */
const char *crash_flag_path() { return R"(C:\ProgramData\nekodrop\mt7902-station-crash)"; }
bool recent_station_crash() {
  WIN32_FILE_ATTRIBUTE_DATA a{};
  if (!GetFileAttributesExA(crash_flag_path(), GetFileExInfoStandard, &a)) return false;
  FILETIME now;
  GetSystemTimeAsFileTime(&now);
  const auto t = [](FILETIME f) { return (static_cast<uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
  return t(now) - t(a.ftLastWriteTime) < 30ull * 60 * 10000000;
}


BOOL WINAPI on_console(DWORD) {
  g_stop = true;
  return TRUE;
}

void say(const char *f, ...) {
  char line[600];
  va_list a;
  va_start(a, f);
  std::vsnprintf(line, sizeof(line), f, a);
  va_end(a);
  std::printf("radio: %s\n", line);
  std::fflush(stdout);
}

std::string mac_str(const uint8_t *m) {
  char s[18];
  std::snprintf(s, sizeof(s), "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
  return s;
}

std::string find_firmware_dir() {
  namespace fs = std::filesystem;
  std::error_code ec;
  /* Beside the program first (an installed nekodrop ships it), then the
   * MediaTek driver's own copy in the driver store. */
  wchar_t self[MAX_PATH];
  GetModuleFileNameW(nullptr, self, MAX_PATH);
  const fs::path local = fs::path(self).parent_path() / "firmware";
  if (fs::exists(local / "WIFI_RAM_CODE_MT7902_1.bin", ec)) return local.string();
  fs::path best;
  fs::file_time_type best_time{};
  for (const auto &e : fs::directory_iterator(R"(C:\Windows\System32\DriverStore\FileRepository)", ec)) {
    if (!e.is_directory(ec) || e.path().filename().string().rfind("mtkwl", 0) != 0) continue;
    const fs::path ram = e.path() / "WIFI_RAM_CODE_MT7902_1.bin";
    if (!fs::exists(ram, ec) || !fs::exists(e.path() / "WIFI_MT7902_patch_mcu_1_1_hdr.bin", ec)) continue;
    const auto t = fs::last_write_time(ram, ec);
    if (best.empty() || t > best_time) {
      best = e.path();
      best_time = t;
    }
  }
  return best.string();
}

struct Options {
  uint8_t channel = 0;
  uint8_t width = 0;
  std::string country = "CN";
  std::string fw_dir;
  std::string data_rate = "auto";
  bool aggregate = true;
  unsigned test_rx = 0, test_tx = 0, test_kinds = 0, test_sta = 0;
  int only = -1;
};

std::optional<Options> parse(int argc, char **argv) {
  if (argc < 2) return std::nullopt;
  Options o;
  const int ch = std::atoi(argv[1]);
  if (ch < 1 || ch > 177) return std::nullopt;
  o.channel = static_cast<uint8_t>(ch);
  for (int i = 2; i < argc; i++) {
    const std::string a = argv[i];
    const bool more = i + 1 < argc;
    if (a == "--width" && more) o.width = static_cast<uint8_t>(std::atoi(argv[++i]));
    else if (a == "--country" && more) o.country = argv[++i];
    else if (a == "--fw-dir" && more) o.fw_dir = argv[++i];
    else if (a == "--data-rate" && more) o.data_rate = argv[++i];
    else if (a == "--no-agg") o.aggregate = false;
    else if (a == "--test-rx" && more) o.test_rx = static_cast<unsigned>(std::atoi(argv[++i]));
    else if (a == "--test-tx" && more) o.test_tx = static_cast<unsigned>(std::atoi(argv[++i]));
    else if (a == "--test-kinds" && more) o.test_kinds = static_cast<unsigned>(std::atoi(argv[++i]));
    else if (a == "--only" && more) o.only = std::atoi(argv[++i]);
    else if (a == "--test-sta" && more) o.test_sta = static_cast<unsigned>(std::atoi(argv[++i]));
    else return std::nullopt;
  }
  if (o.width == 0) o.width = o.channel > 14 ? 80 : 20;
  if (o.width != 20 && o.width != 40 && o.width != 80) return std::nullopt;
  if (o.channel <= 14) o.width = 20;
  if (o.country.size() != 2) return std::nullopt;
  return o;
}

/* ---- 802.11 helpers ---- */
constexpr uint8_t kAwdlBssid[6] = {0x00, 0x25, 0x00, 0xff, 0x94, 0x73};

uint16_t fc_of(const uint8_t *f) { return static_cast<uint16_t>(f[0] | f[1] << 8); }
bool is_mgmt(uint16_t fc) { return ((fc >> 2) & 3) == 0; }
bool is_data(uint16_t fc) { return ((fc >> 2) & 3) == 2; }
bool is_action(uint16_t fc) { return (fc & 0xfc) == 0xd0; }
bool is_beacon(uint16_t fc) { return (fc & 0xfc) == 0x80; }

/* AWDL action frames: category 127 (vendor), Apple OUI 00:17:f2, type 8. */
bool is_awdl(const uint8_t *f, size_t len) {
  return len >= 30 && is_action(fc_of(f)) && f[24] == 127 && f[25] == 0x00 && f[26] == 0x17 &&
         f[27] == 0xf2 && f[28] == 0x08;
}

/* Rates unicast data may go at, slowest first. */
struct RateLadder {
  std::vector<std::pair<std::string, uint16_t>> levels;
  size_t level = 0;
  unsigned sent = 0, failed = 0;
  uint64_t up = 0, down = 0;
  void result(bool acked) {
    sent++;
    failed += !acked;
    if (sent < 32) return;
    if (failed * 100 / sent > 30 && level > 0) {
      level--;
      down++;
    } else if (failed * 100 / sent < 8 && level + 1 < levels.size()) {
      level++;
      up++;
    }
    sent = failed = 0;
  }
};

RateLadder make_ladder(const std::string &spec) {
  RateLadder l;
  auto named = [](const std::string &s) -> std::optional<std::pair<std::string, uint16_t>> {
    if (s.rfind("MCS", 0) == 0) return std::make_pair(s, Mt7902::tx_rate_ht(std::atoi(s.c_str() + 3)));
    if (s.rfind("OFDM", 0) == 0) return std::make_pair(s, Mt7902::tx_rate_ofdm(std::atoi(s.c_str() + 4)));
    return std::nullopt;
  };
  if (spec != "auto") {
    if (auto r = named(spec)) l.levels.push_back(*r);
  }
  if (l.levels.empty()) {
    /* What iPhones were measured to receive on other backends (winawdl's ladder). */
    for (const char *s : {"OFDM24", "MCS2", "MCS4", "MCS5", "MCS7"}) l.levels.push_back(*named(s));
    l.level = 2;
  }
  return l;
}

/* ---- The relay ---- */
struct ClientMsg {
  uint16_t type;
  std::vector<uint8_t> payload;
};

class Relay {
public:
  Relay(Mt7902 &dev, PipeServer &pipe, RateLadder ladder) : _dev(dev), _pipe(pipe), _ladder(std::move(ladder)) {
    dev.set_rx_handler([this](const Mt7902::RxFrame &f) { on_rx(f); });
    dev.set_tx_status_handler([this](unsigned token, bool acked, unsigned tries) { on_tx_status(token, acked, tries); });
  }

  void push(ClientMsg m) {
    std::lock_guard<std::mutex> lock(_mutex);
    if (_queue.size() < 4096) _queue.push_back(std::move(m));
  }

  static constexpr unsigned kReservedSlots = 16;

  /* A TX message whose frame is data to one station. */
  static bool is_unicast_data(const ClientMsg &m) {
    if (m.type != RADIO_LINK_TX || m.payload.size() < 4) return false;
    const size_t rt = m.payload[2] | static_cast<size_t>(m.payload[3]) << 8;
    if (m.payload.size() < rt + 10) return false;
    const uint8_t *f = m.payload.data() + rt;
    return is_data(fc_of(f)) && !(f[4] & 1);
  }

  void client_gone() {
    std::lock_guard<std::mutex> lock(_mutex);
    _queue.clear();
  }

  /* The card's thread. */
  void run() {
    auto last_report = Clock::now();
    unsigned idle = 0;
    /* Two lanes: AWDL's own frames (action frames carrying the sync
     * timestamps, multicast, settings) never wait behind a burst of unicast
     * data, and data never takes the last kReservedSlots TX slots. Late
     * sync frames make an iPhone drop us from AWDL mid-transfer. */
    std::deque<ClientMsg> ctrl, data;
    while (!g_stop) {
      {
        std::lock_guard<std::mutex> lock(_mutex);
        while (!_queue.empty() && ctrl.size() + data.size() < 512) {
          ClientMsg &m = _queue.front();
          (is_unicast_data(m) ? data : ctrl).push_back(std::move(m));
          _queue.pop_front();
        }
      }
      unsigned did = 0;
      while (!ctrl.empty()) {
        if (!handle(ctrl.front())) break; /* no TX slot: try again after polling */
        ctrl.pop_front();
        did++;
      }
      while (!data.empty() && _dev.tx_slots_free() > kReservedSlots) {
        if (!handle(data.front())) break;
        data.pop_front();
        did++;
      }
      did += _dev.poll();
      if (_pipe.connected()) _pipe.flush_send();
      /* Every TX slot out for seconds: the card stopped reporting sent
       * frames, and nothing more can go out. */
      if (_dev.tx_slots_free() == 0) {
        if (_stalled_since == Clock::time_point{}) _stalled_since = Clock::now();
        else if (Clock::now() - _stalled_since > std::chrono::seconds(3) && !g_card_dead) {
          say("TX stalled: no frame reported sent for 3 s; restarting the radio");
          g_card_dead = true;
          g_stop = true;
        }
      } else {
        _stalled_since = Clock::time_point{};
      }
      if (did) idle = 0;
      else if (++idle > 200) Sleep(1);
      else SwitchToThread();
      if (Clock::now() - last_report > std::chrono::seconds(10)) {
        report();
        last_report = Clock::now();
      }
    }
  }

  void report() {
    say("rx: %llu frames, %llu awdl, %llu addressed to us (%llu%% repeats); tx: %llu sent, %llu acked, "
        "%llu failed, %llu refused; data rate %s (up %llu, down %llu); ADDBA declined %llu; "
        "stations %u, firmware-rate data %llu, A-MPDU rx sessions %llu tx sessions %llu; power-save bit on %llu",
        _rx, _rx_awdl, _rx_to_us, _rx_to_us ? _rx_to_us_retry * 100 / _rx_to_us : 0ull, _tx, _tx_acked,
        _tx_failed, _tx_refused, _ladder.levels[_ladder.level].first.c_str(), _ladder.up, _ladder.down,
        _addba_declined, static_cast<unsigned>(_peers.size()), _tx_sta, _rx_ba_sessions, _tx_ba_sessions,
        _rx_to_us_pm);
  }

  uint8_t channel = 0, width = 20;
  bool aggregate = true;

private:
  bool handle(const ClientMsg &m) {
    const bool six = m.payload.size() == 6;
    if (m.type == RADIO_LINK_TX) return send_client_frame(m.payload);
    if (m.type == RADIO_LINK_SET_ADDRESS && six) {
      std::memcpy(_own.data(), m.payload.data(), 6);
      _own_set = true;
      adopt_identity();
    } else if (m.type == RADIO_LINK_SET_BSSID_FILTER && six) {
      std::memcpy(_bssid.data(), m.payload.data(), 6);
      _bssid_set = _bssid != Mac{};
      adopt_identity();
    } else if (m.type == RADIO_LINK_SET_CHANNEL && m.payload.size() == 1 && m.payload[0] != channel) {
      const uint8_t to = m.payload[0];
      const uint8_t w = to > 14 ? width : 20;
      if (_dev.set_channel(to, w)) {
        say("channel %u -> %u", channel, to);
        channel = to;
      } else {
        say("channel change to %u failed; staying on %u", to, channel);
      }
    }
    return true;
  }

  void adopt_identity() {
    if (!_own_set) return;
    _peers.clear(); /* re-adding the interface drops its stations */
    const Mac bssid = _bssid_set ? _bssid : Mac{kAwdlBssid[0], kAwdlBssid[1], kAwdlBssid[2], kAwdlBssid[3], kAwdlBssid[4], kAwdlBssid[5]};
    if (_dev.set_identity(_own.data(), bssid.data()))
      say("acknowledging frames for %s (BSSID %s)", mac_str(_own.data()).c_str(), mac_str(bssid.data()).c_str());
    else
      say("the card will not acknowledge frames for the client; peers will repeat everything they send it");
  }

  bool send_client_frame(const std::vector<uint8_t> &msg) {
    if (msg.size() < 4) return true;
    const size_t rt = msg[2] | static_cast<size_t>(msg[3]) << 8;
    if (rt >= msg.size() || msg.size() - rt < 10) return true;
    return send_frame(msg.data() + rt, msg.size() - rt, false);
  }

  /* Returns false only when the card has no room: the caller retries. */
  bool send_frame(const uint8_t *f, size_t len, bool ours) {
    const uint16_t fc = fc_of(f);
    const bool group = (f[4] & 1) != 0;
    const bool unicast_data = is_data(fc) && !group;
    if (unicast_data && !ours && len >= 24) {
      Peer *p = station(f + 4);
      if (p && p->sta_ok) return send_to_station(*p, f, len);
    }
    const uint16_t rate = unicast_data ? _ladder.levels[_ladder.level].second
                                       : (channel > 14 ? Mt7902::tx_rate_ofdm(6) : Mt7902::tx_rate_ofdm(6));
    /* On 2.4 GHz the MT7902 never completes a frame that waits for an
     * acknowledgement (it stays queued and blocks the queue; 5 GHz is fine,
     * measured with --test-kinds): send there without one. The service moves
     * AWDL to the peer's 5 GHz channel as soon as it hears it. */
    _dev.force_no_ack = channel <= 14 ? 1 : -1;
    const int token = _dev.transmit(f, len, rate);
    if (token < 0) {
      if (_dev.tx_slots_free() == 0) return false;
      _tx_refused++;
      return true;
    }
    _tx++;
    _inflight[static_cast<unsigned>(token)] = unicast_data;
    (void)ours;
    return true;
  }

  void on_tx_status(unsigned token, bool acked, unsigned) {
    auto it = _inflight.find(token);
    const bool unicast_data = it != _inflight.end() && it->second;
    if (it != _inflight.end()) _inflight.erase(it);
    acked ? _tx_acked++ : _tx_failed++;
    if (unicast_data) _ladder.result(acked);
  }

  void on_rx(const Mt7902::RxFrame &f) {
    _rx++;
    if (f.len < 10) return;
    const uint16_t fc = fc_of(f.data);
    if (is_awdl(f.data, f.len)) {
      _rx_awdl++;
      /* NekoDropService moves the radio to the peer's 5 GHz channel, and
       * learns its country, from these (Supervisor.OnEvent), as with the
       * Intel radio. */
      if (!_peer_heard_here && f.channel == channel && channel > 14) {
        _peer_heard_here = true;
        say("AWDL peer heard on channel %u: transmitting allowed here", channel);
      }
      uint8_t seq[16];
      if (awdl::peer_sequence(f.data, f.len, seq) && std::memcmp(seq, _peer_seq, 16) != 0) {
        std::memcpy(_peer_seq, seq, 16);
        const unsigned ch = awdl::peer_channel(seq);
        if (ch && ch != _peer5g) {
          _peer5g = ch;
          say("event: peer-channel %u", ch);
        }
      }
      char c[3];
      if (awdl::peer_country(f.data, f.len, c) && _peer_country.note(c)) say("event: peer-country %s", _peer_country.confirmed);
    }
    const bool to_us = _own_set && std::memcmp(f.data + 4, _own.data(), 6) == 0;
    if (to_us && is_data(fc)) {
      _rx_to_us++;
      if (f.data[1] & 0x08) _rx_to_us_retry++;
      if (f.data[1] & 0x10) _rx_to_us_pm++; /* the peer says it is going to sleep */
    }
    if (to_us && is_action(fc) && f.len >= 24 + 4 && f.data[24] == 3) block_ack_action(f);
    if (is_awdl(f.data, f.len)) learn_caps(f);
    if (f.len < 24 || !_pipe.connected()) return;
    if (_bssid_set && std::memcmp(f.data + 16, _bssid.data(), 6) != 0) return;
    /* Radiotap: FLAGS and DBM_ANTSIGNAL only, as winawdl's radio. */
    _msg.assign(10, 0);
    _msg[2] = 10;
    const uint32_t present = (1u << 1) | (1u << 5);
    std::memcpy(&_msg[4], &present, 4);
    _msg[9] = static_cast<uint8_t>(f.dbm);
    _msg.insert(_msg.end(), f.data, f.data + f.len);
    if (_msg.size() <= RADIO_LINK_MAX_PAYLOAD)
      _pipe.queue_send(RADIO_LINK_RX, {_msg.data(), _msg.size()});
  }

  /* ---- Peer stations and Block Ack (A-MPDU) ----
   * A peer we exchange unicast data with on 5 GHz gets a STA record: the
   * firmware then picks the rate (VHT at 80 MHz included) and the MAC keeps
   * Block Ack sessions, set up from the ADDBA handshake: the peer's requests
   * are accepted once the card is told (iPhone to PC in A-MPDUs), and we ask
   * the peer ourselves for TID 0 (PC to iPhone). Data to a station goes as
   * QoS data, TID 0, with our own sequence numbers. Anything failing falls
   * back to the fixed-rate path. */
  struct Peer {
    Mac addr{};
    uint8_t wcid = 0;
    bool sta_ok = false;
    Mt7902::PeerCaps caps;
    uint16_t seq = 0; /* TID 0 sequence number, last used */
    uint64_t data = 0;
    bool tx_ba = false, addba_pending = false;
    unsigned addba_tries = 0;
    uint16_t addba_ssn = 0;
    uint8_t dialog = 0;
    Clock::time_point addba_at{};
  };

  Peer *find_peer(const uint8_t *mac) {
    for (Peer &p : _peers)
      if (std::memcmp(p.addr.data(), mac, 6) == 0) return &p;
    return nullptr;
  }

  /* The peer's station, set up on first use; nullptr where not possible. */
  Peer *station(const uint8_t *mac) {
    if (!aggregate || channel <= 14) return nullptr;
    if (Peer *p = find_peer(mac)) return p;
    if (_peers.size() >= Mt7902::kLastPeerWcid - Mt7902::kFirstPeerWcid + 1u) return nullptr;
    Peer p;
    std::memcpy(p.addr.data(), mac, 6);
    p.wcid = static_cast<uint8_t>(Mt7902::kFirstPeerWcid + _peers.size());
    auto caps = _caps.find(mac_str(mac));
    if (caps != _caps.end()) p.caps = caps->second;
    p.sta_ok = _dev.sta_add(p.wcid, mac, p.caps);
    if (p.sta_ok) g_stations_up = true;
    say("station %s: wcid %u, %s (HT cap %04x, A-MPDU params %02x)", mac_str(mac).c_str(), p.wcid,
        p.sta_ok ? "firmware rate control" : "not accepted, fixed rates", p.caps.ht_cap, p.caps.ampdu_params);
    _peers.push_back(p);
    return &_peers.back();
  }

  /* AWDL TLV 7 (enhanced data rate capabilities): the sender's HT
   * capabilities, A-MPDU parameters and MCS set (OWL's frame.h). */
  void learn_caps(const Mt7902::RxFrame &f) {
    for (size_t i = 40; i + 3 <= f.len;) {
      const size_t value = size_t(f.data[i + 1]) | size_t(f.data[i + 2]) << 8;
      if (i + 3 + value > f.len) return;
      if (f.data[i] == 7 && value >= 6) {
        Mt7902::PeerCaps c;
        c.ht_cap = static_cast<uint16_t>(f.data[i + 5] | f.data[i + 6] << 8);
        c.ampdu_params = f.data[i + 7];
        c.rx_mcs[0] = f.data[i + 8];
        _caps[mac_str(f.data + 10)] = c;
        return;
      }
      i += 3 + value;
    }
  }

  bool send_to_station(Peer &p, const uint8_t *f, size_t len) {
    const uint16_t fc = fc_of(f);
    const size_t hdr = 24;
    if ((fc & 0x0300) || len > 3800) return send_frame(f, len, true); /* 4-address or oversized */
    /* QoS data, TID 0: A-MPDUs carry QoS data only. */
    _qos.assign(f, f + hdr);
    _qos[0] = static_cast<uint8_t>(_qos[0] | 0x80);
    if (fc & 0x80) {
      _qos.insert(_qos.end(), f + hdr, f + len);
      _qos[hdr] = static_cast<uint8_t>(_qos[hdr] & 0xf0); /* TID 0, normal ack */
    } else {
      _qos.push_back(0);
      _qos.push_back(0);
      _qos.insert(_qos.end(), f + hdr, f + len);
    }
    const uint16_t seq = static_cast<uint16_t>((p.seq + 1) & 0xfff);
    _qos[22] = static_cast<uint8_t>(seq << 4);
    _qos[23] = static_cast<uint8_t>(seq >> 4);
    const int token = _dev.transmit_sta(_qos.data(), _qos.size(), p.wcid);
    if (token < 0) {
      if (_dev.tx_slots_free() == 0) return false;
      _tx_refused++;
      return true;
    }
    p.seq = seq;
    p.data++;
    _tx++;
    _tx_sta++;
    _inflight[static_cast<unsigned>(token)] = false;
    maybe_ask_aggregation(p);
    return true;
  }

  /* We ask the peer to take A-MPDUs on TID 0 once data flows. */
  void maybe_ask_aggregation(Peer &p) {
    if (p.tx_ba || p.data < 8 || p.addba_tries >= 5) return;
    const auto now = Clock::now();
    if (p.addba_pending && now - p.addba_at < std::chrono::seconds(2)) return;
    p.addba_pending = true;
    p.addba_tries++;
    p.addba_at = now;
    p.dialog = static_cast<uint8_t>(p.dialog == 255 ? 1 : p.dialog + 1);
    p.addba_ssn = static_cast<uint16_t>((p.seq + 1) & 0xfff);
    uint8_t r[24 + 9] = {};
    mgmt_header(r, p.addr.data());
    r[24] = 3; /* Block Ack */
    r[25] = 0; /* ADDBA request */
    r[26] = p.dialog;
    const uint16_t params = static_cast<uint16_t>(1u << 1 | 64u << 6); /* immediate, TID 0, 64 frames */
    r[27] = static_cast<uint8_t>(params);
    r[28] = static_cast<uint8_t>(params >> 8);
    r[31] = static_cast<uint8_t>(p.addba_ssn << 4);
    r[32] = static_cast<uint8_t>(p.addba_ssn >> 4);
    send_frame(r, sizeof(r), true);
  }

  void mgmt_header(uint8_t *r, const uint8_t *to) {
    r[0] = 0xd0;
    std::memcpy(r + 4, to, 6);
    std::memcpy(r + 10, _own.data(), 6);
    const Mac bssid = _bssid_set ? _bssid : Mac{kAwdlBssid[0], kAwdlBssid[1], kAwdlBssid[2], kAwdlBssid[3], kAwdlBssid[4], kAwdlBssid[5]};
    std::memcpy(r + 16, bssid.data(), 6);
    _seq = static_cast<uint16_t>((_seq + 1) & 0xfff);
    r[22] = static_cast<uint8_t>(_seq << 4);
    r[23] = static_cast<uint8_t>(_seq >> 4);
  }

  void block_ack_action(const Mt7902::RxFrame &f) {
    const uint8_t action = f.data[25];
    const uint8_t *from = f.data + 10;
    if (action == 0 && f.len >= 24 + 9) { /* ADDBA request */
      Peer *p = station(from);
      if (!p || !p->sta_ok) return decline_addba(f);
      const uint16_t params = static_cast<uint16_t>(f.data[27] | f.data[28] << 8);
      const uint8_t tid = static_cast<uint8_t>((params >> 2) & 0xf);
      uint16_t win = static_cast<uint16_t>(params >> 6);
      if (!win || win > 64) win = 64;
      const uint16_t ssn = static_cast<uint16_t>((f.data[31] | f.data[32] << 8) >> 4);
      if (!_dev.sta_ba(p->wcid, from, tid, ssn, win, false, true)) return decline_addba(f);
      uint8_t r[24 + 9] = {};
      mgmt_header(r, from);
      r[24] = 3;
      r[25] = 1;          /* ADDBA response */
      r[26] = f.data[26]; /* dialog token */
      const uint16_t out = static_cast<uint16_t>(1u << 1 | static_cast<unsigned>(tid) << 2 | win << 6);
      r[29] = static_cast<uint8_t>(out);
      r[30] = static_cast<uint8_t>(out >> 8);
      send_frame(r, sizeof(r), true);
      _rx_ba_sessions++;
      say("A-MPDU from %s accepted: TID %u, window %u", mac_str(from).c_str(), tid, win);
    } else if (action == 1 && f.len >= 24 + 9) { /* ADDBA response */
      Peer *p = find_peer(from);
      if (!p || !p->addba_pending || f.data[26] != p->dialog) return;
      p->addba_pending = false;
      const uint16_t status = static_cast<uint16_t>(f.data[27] | f.data[28] << 8);
      const uint16_t params = static_cast<uint16_t>(f.data[29] | f.data[30] << 8);
      uint16_t win = static_cast<uint16_t>(params >> 6);
      if (!win || win > 64) win = 64;
      if (status == 0 && _dev.sta_ba(p->wcid, from, 0, p->addba_ssn, win, true, true)) {
        p->tx_ba = true;
        _tx_ba_sessions++;
        say("A-MPDU to %s agreed: window %u", mac_str(from).c_str(), win);
      } else {
        say("A-MPDU to %s refused (status %u)", mac_str(from).c_str(), status);
      }
    } else if (action == 2 && f.len >= 24 + 6) { /* DELBA */
      Peer *p = find_peer(from);
      if (!p) return;
      const uint16_t params = static_cast<uint16_t>(f.data[26] | f.data[27] << 8);
      const bool peer_was_originator = (params & 0x0800) != 0;
      const uint8_t tid = static_cast<uint8_t>(params >> 12);
      _dev.sta_ba(p->wcid, from, tid, 0, 0, !peer_was_originator, false);
      if (!peer_was_originator && tid == 0) {
        p->tx_ba = false;
        p->addba_tries = 0;
      }
      say("A-MPDU with %s ended by the peer (TID %u)", mac_str(from).c_str(), tid);
    }
  }

  /* Without a station for the peer: decline, so it sends single frames,
   * which the card acknowledges. Status 37: request declined. */
  void decline_addba(const Mt7902::RxFrame &f) {
    uint8_t r[24 + 9] = {};
    r[0] = 0xd0;
    std::memcpy(r + 4, f.data + 10, 6); /* to the sender */
    std::memcpy(r + 10, _own.data(), 6);
    std::memcpy(r + 16, f.data + 16, 6);
    _seq = static_cast<uint16_t>((_seq + 1) & 0xfff);
    r[22] = static_cast<uint8_t>(_seq << 4);
    r[23] = static_cast<uint8_t>(_seq >> 4);
    r[24] = 3;              /* Block Ack */
    r[25] = 1;              /* ADDBA response */
    r[26] = f.data[26];     /* dialog token */
    r[27] = 37;             /* status: declined */
    r[28] = 0;
    r[29] = f.data[27];     /* the parameter set asked for */
    r[30] = f.data[28];
    send_frame(r, sizeof(r), true);
    _addba_declined++;
  }

  Mt7902 &_dev;
  PipeServer &_pipe;
  RateLadder _ladder;
  std::mutex _mutex;
  std::deque<ClientMsg> _queue;
  std::map<unsigned, bool> _inflight;
  std::vector<uint8_t> _msg;
  Mac _own{}, _bssid{};
  bool _own_set = false, _bssid_set = false;
  uint16_t _seq = 0;
  bool _peer_heard_here = false;
  uint8_t _peer_seq[16] = {};
  unsigned _peer5g = 0;
  awdl::PeerCountry _peer_country;
  uint64_t _rx = 0, _rx_awdl = 0, _rx_to_us = 0, _rx_to_us_retry = 0;
  uint64_t _tx = 0, _tx_acked = 0, _tx_failed = 0, _tx_refused = 0, _addba_declined = 0;
  std::vector<Peer> _peers;
  std::map<std::string, Mt7902::PeerCaps> _caps;
  std::vector<uint8_t> _qos;
  uint64_t _tx_sta = 0, _rx_ba_sessions = 0, _tx_ba_sessions = 0;
  Clock::time_point _stalled_since{};
  uint64_t _rx_to_us_pm = 0;
};

/* ---- Self tests on the air ---- */

int test_rx(Mt7902 &dev, unsigned seconds) {
  uint64_t total = 0, beacons = 0, awdl = 0, data = 0, mgmt = 0, ctrl = 0;
  std::map<std::string, std::pair<int, std::string>> aps; /* bssid -> (dbm, ssid) */
  std::set<std::string> awdl_peers;
  std::map<uint8_t, uint64_t> channels;
  dev.set_rx_handler([&](const Mt7902::RxFrame &f) {
    total++;
    channels[f.channel]++;
    const uint16_t fc = fc_of(f.data);
    if (is_data(fc)) data++;
    else if (is_mgmt(fc)) mgmt++;
    else ctrl++;
    if (f.len >= 38 && is_beacon(fc)) {
      beacons++;
      std::string ssid;
      if (f.data[36] == 0 && f.data[37] <= 32 && 38u + f.data[37] <= f.len)
        ssid.assign(reinterpret_cast<const char *>(f.data + 38), f.data[37]);
      aps[mac_str(f.data + 16)] = {f.dbm, ssid};
    }
    if (is_awdl(f.data, f.len)) {
      awdl++;
      awdl_peers.insert(mac_str(f.data + 10));
    }
  });
  const auto end = Clock::now() + std::chrono::seconds(seconds);
  while (Clock::now() < end && !g_stop) {
    if (!dev.poll()) Sleep(1);
  }
  say("test-rx: %llu frames in %u s on channel %u: %llu mgmt (%llu beacons, %llu AWDL), %llu data, %llu control",
      total, seconds, dev.channel(), mgmt, beacons, awdl, data, ctrl);
  for (const auto &c : channels) say("test-rx: %llu frames reported on channel %u", c.second, c.first);
  int shown = 0;
  for (const auto &ap : aps) {
    if (shown++ >= 12) break;
    say("test-rx: AP %s %d dBm \"%s\"", ap.first.c_str(), ap.second.first, ap.second.second.c_str());
  }
  for (const auto &p : awdl_peers) say("test-rx: AWDL peer %s", p.c_str());
  return total ? 0 : 3;
}

/* Unicast probe requests to an access point heard on the channel: it
 * acknowledges each one it hears, so the TX reports say whether the frames
 * left the antenna at all. */
int test_tx(Mt7902 &dev, unsigned seconds) {
  std::optional<Mac> ap;
  int best = -128;
  dev.set_rx_handler([&](const Mt7902::RxFrame &f) {
    if (f.len >= 24 && is_beacon(fc_of(f.data)) && f.dbm > best) {
      best = f.dbm;
      Mac m;
      std::memcpy(m.data(), f.data + 16, 6);
      ap = m;
    }
  });
  const auto listen_end = Clock::now() + std::chrono::seconds(3);
  while (Clock::now() < listen_end && !g_stop)
    if (!dev.poll()) Sleep(1);
  if (!ap) {
    say("test-tx: no access point heard on channel %u", dev.channel());
    return 4;
  }
  say("test-tx: probing %s (%d dBm)", mac_str(ap->data()).c_str(), best);
  uint64_t sent = 0, acked = 0, failed = 0;
  dev.set_tx_status_handler([&](unsigned, bool ok, unsigned) { ok ? acked++ : failed++; });
  const uint8_t *me = dev.mac_address();
  /* The access point's probe responses come to our address: if the card
   * acknowledges them, almost none is a retry. */
  uint64_t responses = 0, retried = 0;
  dev.set_rx_handler([&](const Mt7902::RxFrame &f) {
    if (f.len >= 24 && (fc_of(f.data) & 0xfc) == 0x50 && std::memcmp(f.data + 4, me, 6) == 0) {
      responses++;
      retried += (f.data[1] & 0x08) != 0;
    }
  });
  const auto end = Clock::now() + std::chrono::seconds(seconds);
  uint16_t seq = 0;
  auto next = Clock::now();
  while (Clock::now() < end && !g_stop) {
    if (Clock::now() >= next) {
      uint8_t f[24 + 2 + 8] = {0x40, 0x00};
      std::memcpy(f + 4, ap->data(), 6);
      std::memcpy(f + 10, me, 6);
      std::memcpy(f + 16, ap->data(), 6);
      seq = static_cast<uint16_t>((seq + 1) & 0xfff);
      f[22] = static_cast<uint8_t>(seq << 4);
      f[23] = static_cast<uint8_t>(seq >> 4);
      f[24] = 0; f[25] = 0;                     /* wildcard SSID */
      const uint8_t rates[] = {1, 6, 0x8c, 0x12, 0x98, 0x24, 0xb0, 0x48}; /* 6,9,12,18,24,36 */
      std::memcpy(f + 26, rates, sizeof(rates));
      if (dev.transmit(f, sizeof(f), Mt7902::tx_rate_ofdm(6)) >= 0) sent++;
      next = Clock::now() + std::chrono::milliseconds(20);
    }
    if (!dev.poll()) Sleep(1);
  }
  const auto drain = Clock::now() + std::chrono::milliseconds(500);
  while (Clock::now() < drain) if (!dev.poll()) Sleep(1);
  say("test-tx: %llu probe requests sent, %llu acknowledged, %llu not", sent, acked, failed);
  say("test-tx: %llu probe responses to us, %llu of them retries (%llu%%)", responses, retried,
      responses ? retried * 100 / responses : 0ull);
  say("test-tx: %s", dev.tx_ring_state().c_str());
  std::string types;
  for (unsigned t = 0; t < 32; t++)
    if (dev.rx_types[t]) types += " type" + std::to_string(t) + "=" + std::to_string(dev.rx_types[t]);
  say("test-tx: RX packets by type:%s", types.c_str());
  return acked ? 0 : 5;
}

/* Which kinds of frame the card completes: group action frames (as awdld
 * sends AWDL), with and without NO_ACK, and unicast data on the AC and ALTX
 * queues, to an access point heard here. */
int test_kinds(Mt7902 &dev, unsigned per_kind, int only) {
  std::optional<Mac> ap;
  int best = -128;
  dev.set_rx_handler([&](const Mt7902::RxFrame &f) {
    if (f.len >= 24 && is_beacon(fc_of(f.data)) && f.dbm > best) {
      best = f.dbm;
      Mac m;
      std::memcpy(m.data(), f.data + 16, 6);
      ap = m;
    }
  });
  const auto listen_end = Clock::now() + std::chrono::seconds(3);
  while (Clock::now() < listen_end && !g_stop)
    if (!dev.poll()) Sleep(1);
  if (!ap) {
    say("test-kinds: no access point heard");
    return 4;
  }
  dev.set_rx_handler(nullptr);
  struct Kind { const char *name; bool group; bool data; int no_ack; int altx; uint64_t sent = 0, ok = 0, bad = 0; };
  Kind kinds[] = {{"group action, NO_ACK", true, false, 1, 1},
                  {"group action, no NO_ACK", true, false, 0, 1},
                  {"unicast data, AC queue", false, true, 0, 0},
                  {"unicast data, ALTX queue", false, true, 0, 1},
                  {"unicast action, ALTX", false, false, 0, 1}};
  std::map<unsigned, int> owner;
  dev.set_tx_status_handler([&](unsigned token, bool ok, unsigned) {
    auto it = owner.find(token);
    if (it == owner.end()) return;
    ok ? kinds[it->second].ok++ : kinds[it->second].bad++;
    owner.erase(it);
  });
  const uint8_t *me = dev.mac_address();
  uint16_t seq = 0;
  for (int k = 0; k < 5; k++) {
    if (only >= 0 && k != only) continue;
    Kind &K = kinds[k];
    dev.force_no_ack = K.no_ack;
    dev.force_altx = K.altx;
    for (unsigned i = 0; i < per_kind && !g_stop; i++) {
      uint8_t f[24 + 8 + 16] = {};
      f[0] = K.data ? 0x08 : 0xd0;
      if (K.data) f[1] = 0x01; /* to DS */
      if (K.group) std::memcpy(f + 4, kAwdlBssid, 6), f[4] = 0x01; else std::memcpy(f + 4, ap->data(), 6);
      std::memcpy(f + 10, me, 6);
      std::memcpy(f + 16, ap->data(), 6);
      seq = static_cast<uint16_t>((seq + 1) & 0xfff);
      f[22] = static_cast<uint8_t>(seq << 4);
      f[23] = static_cast<uint8_t>(seq >> 4);
      if (K.data) { const uint8_t llc[8] = {0xaa, 0xaa, 0x03, 0, 0, 0, 0x88, 0x8e}; std::memcpy(f + 24, llc, 8); }
      else { f[24] = 127; f[25] = 0x00; f[26] = 0x17; f[27] = 0xf2; f[28] = 0x08; }
      const int t = dev.transmit(f, sizeof(f), Mt7902::tx_rate_ofdm(6));
      if (t >= 0) { owner[static_cast<unsigned>(t)] = k; K.sent++; }
      const auto gap = Clock::now() + std::chrono::milliseconds(15);
      while (Clock::now() < gap) if (!dev.poll()) Sleep(1);
    }
    const auto drain = Clock::now() + std::chrono::milliseconds(800);
    while (Clock::now() < drain) if (!dev.poll()) Sleep(1);
    say("test-kinds: %-26s sent %llu, completed ok %llu, failed %llu, never reported %llu", K.name, K.sent, K.ok,
        K.bad, K.sent - K.ok - K.bad);
  }
  dev.force_no_ack = dev.force_altx = -1;
  say("test-kinds: %s", dev.tx_ring_state().c_str());
  return 0;
}

/* The station path against an access point heard here: a STA record for
 * it, QoS data through it at the firmware's rate, and the Block Ack
 * commands accepted (the AP never agreed to one, so they are undone). */
int test_sta(Mt7902 &dev, unsigned frames) {
  std::optional<Mac> ap;
  int best = -128;
  dev.set_rx_handler([&](const Mt7902::RxFrame &f) {
    if (f.len >= 24 && is_beacon(fc_of(f.data)) && f.dbm > best) {
      best = f.dbm;
      Mac m;
      std::memcpy(m.data(), f.data + 16, 6);
      ap = m;
    }
  });
  const auto listen_end = Clock::now() + std::chrono::seconds(3);
  while (Clock::now() < listen_end && !g_stop)
    if (!dev.poll()) Sleep(1);
  dev.set_rx_handler(nullptr);
  if (!ap) {
    say("test-sta: no access point heard");
    return 4;
  }
  Mt7902::PeerCaps caps;
  const bool added = dev.sta_add(1, ap->data(), caps);
  say("test-sta: STA record for %s (%d dBm): %s", mac_str(ap->data()).c_str(), best, added ? "accepted" : "refused");
  if (!added) return 6;
  uint64_t sent = 0, ok = 0, bad = 0;
  dev.set_tx_status_handler([&](unsigned, bool acked, unsigned) { acked ? ok++ : bad++; });
  const uint8_t *me = dev.mac_address();
  for (unsigned i = 0; i < frames && !g_stop; i++) {
    uint8_t f[26 + 8 + 1200] = {};
    f[0] = 0x88; /* QoS data */
    f[1] = 0x01; /* to DS */
    std::memcpy(f + 4, ap->data(), 6);
    std::memcpy(f + 10, me, 6);
    std::memcpy(f + 16, ap->data(), 6);
    const uint16_t seq = static_cast<uint16_t>(i + 1);
    f[22] = static_cast<uint8_t>(seq << 4);
    f[23] = static_cast<uint8_t>(seq >> 4);
    const uint8_t llc[8] = {0xaa, 0xaa, 0x03, 0, 0, 0, 0x88, 0x8e};
    std::memcpy(f + 26, llc, 8);
    if (dev.transmit_sta(f, sizeof(f), 1) >= 0) sent++;
    const auto gap = Clock::now() + std::chrono::milliseconds(10);
    while (Clock::now() < gap) if (!dev.poll()) Sleep(1);
  }
  const auto drain = Clock::now() + std::chrono::milliseconds(800);
  while (Clock::now() < drain) if (!dev.poll()) Sleep(1);
  say("test-sta: %llu QoS data frames at the firmware's rate, %llu acknowledged, %llu not, %llu unreported", sent, ok,
      bad, sent - ok - bad);
  const bool rx_ba = dev.sta_ba(1, ap->data(), 0, 1, 64, false, true) && dev.sta_ba(1, ap->data(), 0, 0, 0, false, false);
  const bool tx_ba = dev.sta_ba(1, ap->data(), 0, static_cast<uint16_t>(frames + 1), 64, true, true) && dev.sta_ba(1, ap->data(), 0, 0, 0, true, false);
  say("test-sta: Block Ack commands: receive %s, send %s", rx_ba ? "accepted" : "refused", tx_ba ? "accepted" : "refused");
  say("test-sta: station removed: %s", dev.sta_remove(1, ap->data()) ? "yes" : "no");
  return ok ? 0 : 7;
}

} // namespace

int main(int argc, char **argv) {
  const auto opt = parse(argc, argv);
  if (!opt) {
    std::fprintf(stderr,
                 "usage: %s <channel> [--width 20|40|80] [--country CC] [--fw-dir DIR] "
                 "[--data-rate auto|MCSn|OFDMn] [--test-rx S] [--test-tx S]\n",
                 argv[0]);
    return 2;
  }
  SetConsoleCtrlHandler(on_console, TRUE);
  timeBeginPeriod(1);
  SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);

  Mt7902 dev;
  dev.set_logger([](const std::string &l) { std::printf("radio: %s\n", l.c_str()); std::fflush(stdout); });
  dev.set_event_handler([](const uint8_t *d, size_t len) {
    /* Unsolicited firmware events (and answers nobody waited for). */
    if (len < sizeof(mt::McuRxd)) return;
    const auto *r = reinterpret_cast<const mt::McuRxd *>(d);
    /* MCU_EVENT_COREDUMP (0xf0): the firmware asserted. It no longer answers
     * commands nor reports sent frames; only a fresh start brings it back. */
    if (r->eid == 0xf0 && !g_card_dead) {
      const std::string text(reinterpret_cast<const char *>(d) + sizeof(mt::McuRxd), len - sizeof(mt::McuRxd));
      if (text.find("ASSERT") != std::string::npos) {
        say("firmware crashed (%.100s): restarting the radio", text.c_str());
        if (g_stations_up) {
          if (FILE *flag = std::fopen(crash_flag_path(), "w")) std::fclose(flag);
          say("peer stations off for 30 minutes after this crash");
        }
        g_card_dead = true;
        g_stop = true;
      }
    }
    static unsigned shown = 0;
    if (shown++ < 60) {
      std::string text;
      for (size_t i = sizeof(mt::McuRxd); i < len && text.size() < 160; i++)
        text += (d[i] >= 32 && d[i] < 127) ? static_cast<char>(d[i]) : '.';
      say("fw-event: eid %02x ext %02x seq %u option %02x len %zu %s", r->eid, r->ext_eid, r->seq, r->option, len,
          text.c_str());
    }
  });
  const std::string fw = opt->fw_dir.empty() ? find_firmware_dir() : opt->fw_dir;
  std::string error;
  if (fw.empty()) error = "no MT7902 firmware found (WIFI_RAM_CODE_MT7902_1.bin)";
  else if (!dev.open(fw, error)) {}
  else if (!dev.radio_start(opt->channel, opt->width, opt->country, error)) {}
  if (!error.empty()) {
    say("bring-up failed: %s", error.c_str());
    dev.close();
    return 1;
  }
  say("MT7902 firmware %s, MAC %s", dev.firmware_version().c_str(), mac_str(dev.mac_address()).c_str());

  int rc = 0;
  if (opt->test_rx) rc = test_rx(dev, opt->test_rx);
  else if (opt->test_tx) rc = test_tx(dev, opt->test_tx);
  else if (opt->test_kinds) rc = test_kinds(dev, opt->test_kinds, opt->only);
  else if (opt->test_sta) rc = test_sta(dev, opt->test_sta);
  else {
    PipeServer pipe(RADIO_LINK_PIPE_NAME, [] { return g_stop.load(); });
    if (!pipe.valid()) {
      say("bring-up failed: cannot create %s (%lu): already running?", RADIO_LINK_PIPE_NAME, GetLastError());
      dev.close();
      return 1;
    }
    Relay relay(dev, pipe, make_ladder(opt->data_rate));
    relay.channel = opt->channel;
    relay.width = dev.width_mhz();
    relay.aggregate = opt->aggregate;
    if (relay.aggregate && recent_station_crash()) {
      relay.aggregate = false;
      say("peer stations off: the firmware crashed with one less than 30 minutes ago");
    }
    std::thread reader([&] {
      while (!g_stop && pipe.wait_for_client()) {
        radio_link_hello hello{RADIO_LINK_VERSION, relay.channel, relay.width, {}};
        pipe.send(RADIO_LINK_HELLO, {reinterpret_cast<const uint8_t *>(&hello), sizeof(hello)});
        say("client connected");
        radio_link_header h{};
        while (!g_stop && pipe.read_exact(&h, sizeof(h))) {
          if (h.length > RADIO_LINK_MAX_PAYLOAD) break;
          ClientMsg m{h.type, std::vector<uint8_t>(h.length)};
          if (h.length && !pipe.read_exact(m.payload.data(), h.length)) break;
          relay.push(std::move(m));
        }
        relay.client_gone();
        pipe.drop_client();
        say("client gone");
      }
    });
    say("ready on channel %u, waiting for a client", opt->channel);
    relay.run();
    relay.report();
    g_stop = true;
    reader.join();
  }
  dev.close();
  timeEndPeriod(1);
  return g_card_dead ? 3 : rc;
}
