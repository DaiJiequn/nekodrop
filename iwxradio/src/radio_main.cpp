// SPDX-License-Identifier: GPL-2.0-only
/*
 * iwx_radio: the radio daemon for Intel AX201-class cards.
 *
 * It speaks winawdl's radio link (radio_link.h) on \\.\pipe\winawdl-radio,
 * so winawdl's AWDL daemon (awdld, OWL on Windows), its AirDrop engine and its
 * app run on top unchanged; only the Realtek radio (devourer) is replaced by
 * the user-mode Intel driver in iwx.cpp. Built as winawdl_radio.exe it drops
 * into winawdl's layout.
 *
 * Usage: winawdl_radio <channel> [--width 20|40|80] [--country XX]
 *          [--mac-type listener|ibss|sta] [--firmware DIR]
 *          [--data-rate 6|12|24|36|48|54|MCS0..MCS15] [--verbose]
 * winawdl's own options (--retries, --capture, --no-block-ack,
 * --no-aggregate, --no-quick-submit) are accepted and ignored.
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <timeapi.h>

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "awdl_policy.h"
#include "iwx.h"
#include "wifi_sta.h"
#include "pipe_server.h"
#include "radio_link.h"

namespace {

std::atomic<bool> g_stop{false};

bool stopping() { return g_stop.load(); }

BOOL WINAPI on_console(DWORD) {
  g_stop = true;
  return TRUE;
}

void log_line(const std::string &line) {
  SYSTEMTIME t;
  GetLocalTime(&t);
  std::fprintf(stderr, "%02u:%02u:%02u.%03u radio: %s\n", t.wHour, t.wMinute, t.wSecond,
               t.wMilliseconds, line.c_str());
  std::fflush(stderr);
}

struct Options {
  uint8_t channel = 0;
  unsigned width = 0; /* 0: 80 on 5 GHz, 20 on 2.4 GHz, as winawdl */
  IwxDevice::Options dev;
  TxRate data_rate{TxRate::Legacy, 48, 0}; /* 24 Mb/s */
  bool fixed_rate = false;  /* --data-rate given: no rate ladder */
  bool no_block_ack = false;
  std::string sta_config; /* --sta-config: join this network beside AWDL */
  /* Test mode: serve the pipe without touching the card, and report what the
   * client sends. Checks the radio link against awdld with no hardware. */
  bool no_card = false;
};

bool parse_rate(const std::string &s, TxRate &rate) {
  if (s.size() > 3 && (s[0] == 'M' || s[0] == 'm') && (s[1] == 'C' || s[1] == 'c')) {
    rate.kind = TxRate::Ht;
    rate.mcs = static_cast<unsigned>(std::atoi(s.c_str() + 3));
    return rate.mcs <= 15;
  }
  const double mbps = std::atof(s.c_str());
  if (mbps <= 0) return false; /* "auto", "client": keep the default */
  rate.kind = TxRate::Legacy;
  rate.mbps_x2 = static_cast<unsigned>(mbps * 2 + 0.5);
  return true;
}

std::string exe_dir() {
  char path[MAX_PATH] = {};
  GetModuleFileNameA(nullptr, path, MAX_PATH);
  std::string s = path;
  const size_t slash = s.find_last_of("\\/");
  return slash == std::string::npos ? "." : s.substr(0, slash);
}

bool parse_options(int argc, char **argv, Options &o) {
  o.dev.firmware_dir = exe_dir() + "\\firmware";
  for (int i = 1; i < argc; i++) {
    const std::string a = argv[i];
    const auto value = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
    if (a == "--width") o.width = static_cast<unsigned>(std::atoi(value().c_str()));
    else if (a == "--country") o.dev.country = value();
    else if (a == "--firmware") o.dev.firmware_dir = value();
    else if (a == "--mac-type") {
      const std::string t = value();
      if (t == "listener") o.dev.mac_type = IwxDevice::MacType::Listener;
      else if (t == "ibss") o.dev.mac_type = IwxDevice::MacType::Ibss;
      else if (t == "sta") o.dev.mac_type = IwxDevice::MacType::Sta;
      else return false;
    } else if (a == "--data-rate") o.fixed_rate = parse_rate(value(), o.data_rate);
    else if (a == "--verbose") o.dev.verbose = true;
    else if (a == "--no-card") o.no_card = true;
    else if (a == "--sta-config") o.sta_config = value();
    else if (a == "--retries" || a == "--capture") value();
    else if (a == "--no-block-ack") o.no_block_ack = true;
    else if (a == "--no-aggregate" || a == "--no-quick-submit") {}
    else if (!a.empty() && std::isdigit(static_cast<unsigned char>(a[0])) && !o.channel)
      o.channel = static_cast<uint8_t>(std::atoi(a.c_str()));
    else return false;
  }
  if (!o.channel) return false;
  if (!o.width) o.width = o.channel <= 14 ? 20 : 80;
  if (o.width != 20 && o.width != 40 && o.width != 80) return false;
  if (o.dev.country.size() != 2) return false;
  for (auto &c : o.dev.country) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return true;
}

/* The fields of a radiotap header this daemon reads: total length, the rate
 * and the MCS index if given. Walks only the standard fields up to MCS. */
struct RadiotapInfo {
  size_t length = 0;
  bool has_rate = false, has_mcs = false;
  unsigned rate_x2 = 0, mcs = 0;
};

bool parse_radiotap(const uint8_t *p, size_t len, RadiotapInfo &info) {
  if (len < 8 || p[0] != 0) return false;
  info.length = p[2] | size_t(p[3]) << 8;
  if (info.length < 8 || info.length > len) return false;
  uint32_t present;
  std::memcpy(&present, p + 4, 4);
  size_t off = 8;
  uint32_t word = present;
  while (word & 0x80000000u) { /* extended bitmaps: skip them */
    if (off + 4 > info.length) return false;
    std::memcpy(&word, p + off, 4);
    off += 4;
  }
  static const struct { uint8_t align, size; } fields[] = {
      {8, 8}, {1, 1}, {1, 1}, {2, 4}, {2, 2}, {1, 1}, {1, 1}, {2, 2}, {2, 2}, {2, 2},
      {1, 1}, {1, 1}, {1, 1}, {1, 1}, {2, 2}, {2, 2}, {1, 1}, {1, 1}, {4, 8}, {1, 3}};
  for (unsigned bit = 0; bit < 20; bit++) {
    if (!(present & (1u << bit))) continue;
    off = (off + fields[bit].align - 1) & ~size_t(fields[bit].align - 1);
    if (off + fields[bit].size > info.length) return true;
    if (bit == 2) {
      info.has_rate = true;
      info.rate_x2 = p[off];
    } else if (bit == 19) {
      info.has_mcs = (p[off] & 0x02) != 0; /* MCS index known */
      info.mcs = p[off + 2];
    }
    off += fields[bit].size;
  }
  return true;
}

constexpr uint16_t kStaEthernet = 0x100; /* inbox: a frame from the TAP adapter */

struct Request {
  uint16_t type;
  std::vector<uint8_t> payload;
};

class Mailbox {
public:
  void push(Request r) {
    std::lock_guard<std::mutex> l(_m);
    _q.push_back(std::move(r));
  }
  bool pop(Request &r) {
    std::lock_guard<std::mutex> l(_m);
    if (_q.empty()) return false;
    r = std::move(_q.front());
    _q.pop_front();
    return true;
  }
  size_t size() {
    std::lock_guard<std::mutex> l(_m);
    return _q.size();
  }

private:
  std::mutex _m;
  std::deque<Request> _q;
};

} // namespace

int main(int argc, char **argv) {
  Options opt;
  if (!parse_options(argc, argv, opt)) {
    std::fprintf(stderr,
                 "usage: %s <channel> [--width 20|40|80] [--country XX]\n"
                 "          [--mac-type listener|ibss|sta] [--firmware DIR]\n"
                 "          [--data-rate 6|12|24|36|48|54|MCS0..MCS15] [--verbose]\n",
                 argv[0]);
    return 2;
  }
  SetConsoleCtrlHandler(on_console, TRUE);
  timeBeginPeriod(1);

  PipeServer pipe(RADIO_LINK_PIPE_NAME, stopping);
  if (!pipe.valid()) {
    log_line("cannot create " + std::string(RADIO_LINK_PIPE_NAME) + " (error " +
             std::to_string(GetLastError()) + "): is another radio running?");
    return 1;
  }

  Mailbox inbox;
  std::atomic<bool> ready{false}, failed{false};
  std::atomic<bool> client_connected{false};
  uint8_t bssid_filter[6] = {};
  std::atomic<bool> have_filter{false};
  std::mutex filter_mutex;

  /* The device thread owns the card: bring-up, receive polling, sending. */
  std::thread device_thread([&] {
    if (opt.no_card) {
      ready = true;
      log_line("test mode: no card; counting what the client sends");
      uint64_t tx = 0, action = 0, data = 0, other = 0, last = GetTickCount64();
      Request req;
      while (!stopping()) {
        while (inbox.pop(req)) {
          if (req.type == RADIO_LINK_TX) {
            RadiotapInfo rt;
            if (!parse_radiotap(req.payload.data(), req.payload.size(), rt)) {
              log_line("TX with a malformed radiotap header");
              continue;
            }
            const uint8_t fc = req.payload.size() > rt.length ? req.payload[rt.length] : 0;
            tx++;
            if (fc == 0xd0) action++;
            else if ((fc & 0x0c) == 0x08) data++;
            else other++;
          } else if (req.payload.size() == 6) {
            char b[64];
            std::snprintf(b, sizeof(b), "%s %02x:%02x:%02x:%02x:%02x:%02x",
                          req.type == RADIO_LINK_SET_ADDRESS ? "address" : "bssid filter",
                          req.payload[0], req.payload[1], req.payload[2], req.payload[3],
                          req.payload[4], req.payload[5]);
            log_line(b);
          } else if (req.type == RADIO_LINK_SET_CHANNEL && req.payload.size() == 1) {
            log_line("channel " + std::to_string(req.payload[0]));
          }
        }
        if (GetTickCount64() - last >= 2000) {
          last = GetTickCount64();
          log_line("client sent " + std::to_string(tx) + " frames: " + std::to_string(action) +
                   " action, " + std::to_string(data) + " data, " + std::to_string(other) +
                   " other");
        }
        Sleep(5);
      }
      return;
    }
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    IwxDevice dev(log_line);
    std::string error;
    if (!dev.open(opt.dev, error)) {
      log_line("bring-up failed: " + error);
      failed = true;
      g_stop = true;
      return;
    }
    std::unique_ptr<WifiStation> sta;
    StaConfig sta_cfg;
    if (!opt.sta_config.empty()) {
      std::string err;
      if (sta_cfg.load(opt.sta_config, err)) sta = std::make_unique<WifiStation>(dev, log_line);
      else log_line("station config: " + err);
      /* The file holds the Wi-Fi password: gone once read. */
      DeleteFileA(opt.sta_config.c_str());
    }
    std::vector<uint8_t> message;
    uint8_t dev_address[6] = {};
    bool beacon_heard = false;
    uint64_t tx_held_passive = 0;
    uint8_t peer_seq[16] = {};
    unsigned peer5g = 0;
    awdl::PeerCountry peer_country;
    std::string want_country, have_country = opt.dev.country;
    uint64_t rx_all = 0, rx_awdl = 0, rx_to_us = 0;
    uint32_t rx_data_to_us = 0;
    int8_t awdl_dbm = -127;
    std::deque<awdl::AddbaRequest> addba_pending;
    std::deque<uint8_t> delba_pending;
    const auto now_us = [] {
      static LARGE_INTEGER f = [] {
        LARGE_INTEGER v;
        QueryPerformanceFrequency(&v);
        return v;
      }();
      LARGE_INTEGER t;
      QueryPerformanceCounter(&t);
      return static_cast<uint64_t>(t.QuadPart) * 1000000ull / static_cast<uint64_t>(f.QuadPart);
    };
    dev.set_rx_handler([&](const RxFrame &f) {
      rx_all++;
      if (sta && sta->owns(f)) {
        sta->on_frame(f);
        return;
      }
      /* Beacon hint: a beacon heard in our 80 MHz block lifts "passive". */
      if (f.len >= 36 && (f.data[0] == 0x80 || f.data[0] == 0x50) && !beacon_heard) {
        const unsigned a = awdl::block80_first(f.channel), b = awdl::block80_first(dev.channel());
        if ((a && a == b) || f.channel == dev.channel()) {
          beacon_heard = true;
          log_line("beacon heard on channel " + std::to_string(f.channel) + ": transmitting allowed here");
        }
      }
      if (awdl::is_awdl_action(f.data, f.len)) {
        rx_awdl++;
        awdl_dbm = f.dbm;
        uint8_t seq[16];
        if (awdl::peer_sequence(f.data, f.len, seq) && std::memcmp(seq, peer_seq, 16) != 0) {
          std::memcpy(peer_seq, seq, 16);
          const unsigned ch = awdl::peer_channel(seq);
          if (ch && ch != peer5g) {
            peer5g = ch;
            log_line("event: peer-channel " + std::to_string(ch));
          }
        }
        char c[3];
        if (awdl::peer_country(f.data, f.len, c) && peer_country.note(c)) {
          log_line("event: peer-country " + std::string(peer_country.confirmed));
          want_country = peer_country.confirmed; /* applied from the main loop */
        }
      }
      if (f.len >= 10 && std::memcmp(f.data + 4, dev_address, 6) == 0) {
        rx_to_us++;
        if ((f.data[0] & 0x0c) == 0x08) rx_data_to_us++;
        awdl::AddbaRequest req;
        bool sender = false;
        uint8_t tid = 0;
        if (awdl::parse_addba_request(f.data, f.len, dev_address, &req)) {
          if (addba_pending.size() < 16) addba_pending.push_back(req);
        } else if (awdl::parse_delba(f.data, f.len, dev_address, &sender, &tid) && sender) {
          delba_pending.push_back(tid);
        }
      }
      if (!client_connected || f.len < 24) return;
      if (have_filter) {
        std::lock_guard<std::mutex> l(filter_mutex);
        if (std::memcmp(f.data + 16, bssid_filter, 6) != 0) return;
      }
      /* The radiotap header winawdl's radio sends: flags and signal. */
      constexpr size_t kRadiotapBytes = 10;
      message.assign(kRadiotapBytes, 0);
      message[2] = kRadiotapBytes;
      const uint32_t present = (1u << 1) | (1u << 5);
      std::memcpy(&message[4], &present, 4);
      message[8] = 0;
      message[9] = static_cast<uint8_t>(f.dbm);
      message.insert(message.end(), f.data, f.data + f.len);
      if (message.size() <= RADIO_LINK_MAX_PAYLOAD) pipe.send(RADIO_LINK_RX, message);
    });
    if (!dev.start_monitor(opt.channel, opt.width, dev.hw_address().data())) {
      log_line("could not start listening");
      failed = true;
      g_stop = true;
      dev.stop();
      return;
    }
    std::thread tap_thread;
    if (sta) {
      std::string err;
      if (!sta->start(sta_cfg, err)) {
        log_line("station: " + err);
        log_line("event: wifi failed");
        sta.reset();
      } else {
        tap_thread = std::thread([&] {
          std::vector<uint8_t> eth;
          while (!stopping()) {
            if (!sta->tap().read(eth, 200)) {
              Sleep(50);
              continue;
            }
            if (!eth.empty() && inbox.size() < 4096) inbox.push({kStaEthernet, eth});
          }
        });
      }
    }
    {
      const uint32_t cf = dev.channel_flags(opt.channel);
      log_line(std::string("event: channel ") + std::to_string(opt.channel) + " " +
               (opt.channel <= 14 || (cf & (1u << 3)) ? "active" : "passive"));
    }

    /* Host-side resend (ForbiddenItlwm's ResendState): frames to one station
     * go out one at a time; a failed one is sent again, with the retry flag,
     * for up to 400 ms, since the peer may be on another channel. */
    struct Unicast {
      std::vector<uint8_t> frame;
      TxRate rate;
      bool data;
    };
    std::deque<Unicast> uq;
    int inflight = -1;
    unsigned attempts = 0;
    uint64_t first_us = 0, not_before_us = 0, sent_us = 0;
    uint64_t resent = 0, gave_up = 0, uq_dropped = 0;
    awdl::RateLadder ladder;
    const bool wide80 = opt.channel > 14 && opt.width >= 80;
    const auto data_rate = [&]() -> TxRate {
      if (opt.fixed_rate) return opt.data_rate;
      const unsigned st = ladder.step;
      TxRate r;
      if (st == 0) {
        r.kind = TxRate::Legacy;
        r.mbps_x2 = 48;
      } else if (st <= 3) {
        r.kind = TxRate::Ht;
        r.mcs = awdl::kRateMcs[st];
      } else {
        r.kind = TxRate::Vht;
        r.mcs = awdl::kRateMcs[st];
        r.nss = 2;
        r.wide80 = true;
      }
      return r;
    };
    dev.set_tx_status_handler([&](unsigned slot, bool acked) {
      if (inflight < 0 || slot != static_cast<unsigned>(inflight) || uq.empty()) return;
      inflight = -1;
      const uint64_t now = now_us();
      if (uq.front().data && attempts == 1) ladder.note(acked, wide80);
      if (acked) {
        uq.pop_front();
        attempts = 0;
        return;
      }
      if (now - first_us >= awdl::kResendWindowUS || attempts >= awdl::kResendMaxAttempts) {
        uq.pop_front();
        attempts = 0;
        gave_up++;
        return;
      }
      uq.front().frame[1] |= 0x08; /* retry */
      not_before_us = now + awdl::resend_backoff_us(attempts);
      resent++;
    });
    const auto pump = [&] {
      const uint64_t now = now_us();
      if (inflight >= 0) {
        if (now - sent_us < 1000000) return false;
        inflight = -1; /* no status in a second: do not stall behind it */
      }
      if (uq.empty() || now < not_before_us) return false;
      if (!attempts) {
        first_us = now;
        if (uq.front().data) uq.front().rate = data_rate();
      }
      const int slot = dev.transmit_slot(uq.front().frame.data(), uq.front().frame.size(),
                                         uq.front().rate);
      if (slot < 0) return false;
      inflight = slot;
      attempts++;
      sent_us = now;
      return true;
    };
    const auto may_send = [&] {
      const uint32_t cf = dev.channel_flags(dev.channel());
      return dev.channel() <= 14 || (cf & (1u << 3)) || beacon_heard;
    };
    awdl::BaFilter ba_filter;
    uint16_t mgmt_seq = 0;
    uint64_t ba_answers = 0;
    ready = true;
    log_line("ready on channel " + std::to_string(opt.channel) + ", waiting for a client");

    uint64_t idle_rounds = 0, last_report = GetTickCount64(), last_health = 0;
    Request req;
    while (!stopping() && dev.healthy()) {
      bool busy = dev.poll() > 0;
      for (int n = 0; n < 64 && inbox.pop(req); n++) {
        busy = true;
        if (req.type == RADIO_LINK_TX) {
          RadiotapInfo rt;
          if (!parse_radiotap(req.payload.data(), req.payload.size(), rt)) continue;
          const uint8_t *frame = req.payload.data() + rt.length;
          const size_t len = req.payload.size() - rt.length;
          if (len < 10) continue;
          TxRate rate;
          const bool unicast = !(frame[4] & 1);
          const bool data = (frame[0] & 0x0c) == 0x08;
          if (rt.has_mcs) {
            rate.kind = TxRate::Ht;
            rate.mcs = rt.mcs;
          } else if (rt.has_rate && rt.rate_x2) {
            rate.mbps_x2 = rt.rate_x2;
          }
          if (!may_send()) {
            tx_held_passive++; /* no initiating on a passive channel */
          } else if (unicast) {
            if (uq.size() >= 1024) uq_dropped++;
            else uq.push_back({std::vector<uint8_t>(frame, frame + len), rate, data});
          } else {
            dev.transmit(frame, len, rate);
          }
        } else if (req.type == kStaEthernet) {
          if (sta) sta->on_ethernet(req.payload.data(), req.payload.size());
        } else if (req.type == RADIO_LINK_SET_ADDRESS && req.payload.size() == 6) {
          std::memcpy(dev_address, req.payload.data(), 6);
          dev.set_address(req.payload.data());
        } else if (req.type == RADIO_LINK_SET_BSSID_FILTER && req.payload.size() == 6) {
          std::lock_guard<std::mutex> l(filter_mutex);
          std::memcpy(bssid_filter, req.payload.data(), 6);
          static const uint8_t zero[6] = {};
          have_filter = std::memcmp(bssid_filter, zero, 6) != 0;
        } else if (req.type == RADIO_LINK_SET_CHANNEL && req.payload.size() == 1) {
          const uint8_t ch = req.payload[0];
          dev.set_channel(ch, ch <= 14 ? 20 : opt.width);
        }
      }
      /* Block Ack: open the firmware session, then answer from our queue. The
       * peer repeats its request until it hears an answer. */
      while (!addba_pending.empty()) {
        const awdl::AddbaRequest r = addba_pending.front();
        addba_pending.pop_front();
        if (!r.immediate || opt.no_block_ack) continue;
        if (!dev.add_rx_ba(r.peer, r.tid, r.ssn, r.window, r.token)) continue;
        uint8_t answer[awdl::kAddbaResponseLength];
        awdl::build_addba_response(answer, r, dev_address, ++mgmt_seq);
        if (may_send()) {
          uq.push_front({std::vector<uint8_t>(answer, answer + sizeof(answer)), TxRate{}, false});
          ba_answers++;
        }
      }
      while (!delba_pending.empty()) {
        dev.remove_rx_ba(delba_pending.front());
        delba_pending.pop_front();
      }
      /* Promiscuous receive off while a session's data is busy. */
      bool any_ba = false;
      for (unsigned t = 0; t < 8; t++) any_ba |= dev.rx_ba_open(t);
      if (any_ba && ba_filter.note(now_us(), rx_data_to_us)) dev.set_promiscuous(!ba_filter.off);
      if (!any_ba && !dev.promiscuous()) {
        ba_filter = awdl::BaFilter{};
        dev.set_promiscuous(true);
      }
      if (!want_country.empty() && want_country != have_country) {
        if (dev.set_country(want_country)) {
          have_country = want_country;
          log_line("event: country " + have_country);
        }
        want_country.clear();
      }
      if (sta) {
        const auto before = sta->state();
        sta->tick(now_us());
        if (sta->state() != before) log_line(std::string("event: wifi ") + sta->state_name());
        std::vector<uint8_t> ans;
        while (sta->take_addba(ans)) {
          TxRate r;
          dev.sta_transmit(ans.data(), ans.size(), r, sta->state() == WifiStation::State::Running);
        }
        if (sta->state() != WifiStation::State::Running) busy = true;
      }
      if (pump()) busy = true;
      if (inflight >= 0 || !uq.empty()) busy = true;
      if (busy) {
        idle_rounds = 0;
      } else if (++idle_rounds > 200) {
        Sleep(1); /* quiet: 1 ms ticks (timeBeginPeriod) */
      } else {
        SwitchToThread();
      }
      const uint64_t now = GetTickCount64();
      if (now - last_health >= 1000) {
        last_health = now;
        dev.check_health();
      }
      if (now - last_report >= 5000) {
        last_report = now;
        const TxStats &s = dev.tx_stats();
        log_line("rx: " + std::to_string(rx_all) + " frames, " + std::to_string(rx_awdl) +
                 " AWDL action frames (last " + std::to_string(awdl_dbm) + " dBm), " +
                 std::to_string(rx_to_us) + " addressed to us");
        log_line("tx: " + std::to_string(s.submitted) + " submitted, " + std::to_string(s.acked) +
                 " done, " + std::to_string(s.failed) + " failed; unicast: " + std::to_string(resent) +
                 " resent, " + std::to_string(gave_up) + " given up, " + std::to_string(uq.size()) +
                 " waiting, " + std::to_string(uq_dropped) + " dropped; rate step " +
                 std::to_string(ladder.step) + " (" + std::to_string(ladder.ups) + " up, " +
                 std::to_string(ladder.downs) + " down); BA answers " + std::to_string(ba_answers) +
                 (dev.promiscuous() ? "" : ", promiscuous off") +
                 (tx_held_passive ? "; " + std::to_string(tx_held_passive) + " held (passive channel)" : ""));
        if (sta)
          log_line(std::string("wifi: ") + sta->state_name() + ", " + std::to_string(sta->rx_packets()) +
                   " packets in, " + std::to_string(sta->tx_packets()) + " out");
      }
    }
    if (sta) sta->stop();
    if (tap_thread.joinable()) {
      g_stop = true;
      tap_thread.join();
    }
    if (!dev.healthy()) log_line("the card stopped answering or the firmware failed");
    dev.stop();
    g_stop = true;
  });

  /* Wait for the card before taking clients, so HELLO tells the truth. */
  while (!ready && !failed && !stopping()) Sleep(10);

  std::vector<uint8_t> payload;
  while (!stopping() && pipe.wait_for_client()) {
    radio_link_hello hello{RADIO_LINK_VERSION, opt.channel,
                           static_cast<uint8_t>(opt.channel <= 14 ? 20 : opt.width), {}};
    client_connected = true;
    pipe.send(RADIO_LINK_HELLO, {reinterpret_cast<const uint8_t *>(&hello), sizeof(hello)});
    log_line("client connected");
    radio_link_header header{};
    while (!stopping() && pipe.read_exact(&header, sizeof(header))) {
      if (header.length > RADIO_LINK_MAX_PAYLOAD) {
        log_line("client sent an oversized message");
        break;
      }
      payload.resize(header.length);
      if (header.length && !pipe.read_exact(payload.data(), header.length)) break;
      if (inbox.size() > 4096) continue; /* the card is not keeping up: drop */
      inbox.push({header.type, payload});
    }
    client_connected = false;
    {
      std::lock_guard<std::mutex> l(filter_mutex);
      have_filter = false;
    }
    log_line("client gone");
    pipe.drop_client();
  }
  g_stop = true;
  device_thread.join();
  timeEndPeriod(1);
  return failed ? 1 : 0;
}
