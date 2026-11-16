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
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "iwx.h"
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
    } else if (a == "--data-rate") parse_rate(value(), o.data_rate);
    else if (a == "--verbose") o.dev.verbose = true;
    else if (a == "--retries" || a == "--capture") value();
    else if (a == "--no-block-ack" || a == "--no-aggregate" || a == "--no-quick-submit") {}
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
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    IwxDevice dev(log_line);
    std::string error;
    if (!dev.open(opt.dev, error)) {
      log_line("bring-up failed: " + error);
      failed = true;
      g_stop = true;
      return;
    }
    std::vector<uint8_t> message;
    dev.set_rx_handler([&](const RxFrame &f) {
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
    ready = true;
    log_line("ready on channel " + std::to_string(opt.channel) + ", waiting for a client");

    uint64_t idle_rounds = 0, last_report = GetTickCount64();
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
          TxRate rate{TxRate::Legacy, 12, 0};
          const bool unicast = len >= 10 && !(frame[4] & 1);
          const bool data = (frame[0] & 0x0c) == 0x08;
          if (rt.has_mcs) rate = {TxRate::Ht, 0, rt.mcs};
          else if (rt.has_rate && rt.rate_x2) rate = {TxRate::Legacy, rt.rate_x2, 0};
          else if (unicast && data) rate = opt.data_rate;
          dev.transmit(frame, len, rate);
        } else if (req.type == RADIO_LINK_SET_ADDRESS && req.payload.size() == 6) {
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
      if (busy) {
        idle_rounds = 0;
      } else if (++idle_rounds > 200) {
        Sleep(1); /* quiet: 1 ms ticks (timeBeginPeriod) */
      } else {
        SwitchToThread();
      }
      const uint64_t now = GetTickCount64();
      if (now - last_report >= 10000) {
        last_report = now;
        const TxStats &s = dev.tx_stats();
        log_line("tx: " + std::to_string(s.submitted) + " submitted, " +
                 std::to_string(s.acked) + " done, " + std::to_string(s.failed) + " failed, " +
                 std::to_string(s.retries) + " retries, " + std::to_string(s.dropped_full) +
                 " dropped (ring full)");
      }
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
