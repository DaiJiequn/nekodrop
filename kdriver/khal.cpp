// SPDX-License-Identifier: GPL-2.0-only
/* khal.h over IwxDevice. Built in the C++-library group (NEKO_KERNEL). */
#include "plat.h"
#include "iwx.h"
#include "khal.h"

#include <cstring>

namespace {
IwxDevice *g_dev;
KhalLogCallback g_log;
KhalRxCallback g_rx;
void *g_rx_ctx;

TxRate legacy(unsigned mbps_x2) {
  TxRate r;
  r.kind = TxRate::Legacy;
  r.mbps_x2 = mbps_x2;
  return r;
}

void copy_error(char *dst, size_t len, const std::string &src) {
  if (!dst || !len) return;
  const size_t n = src.size() < len - 1 ? src.size() : len - 1;
  std::memcpy(dst, src.data(), n);
  dst[n] = 0;
}
} // namespace

bool khal_open(uint8_t channel, const char *country, KhalLogCallback log, char *error, size_t error_len) {
  khal_close();
  g_log = log;
  g_dev = new IwxDevice([](const std::string &line) {
    if (g_log) g_log(line.c_str());
  });
  IwxDevice::Options opt;
  opt.firmware_dir = ""; /* plat_read_file resolves names under NEKO_FIRMWARE_DIR */
  opt.country = country && country[0] ? country : "ZZ";
  opt.mac_type = IwxDevice::MacType::Listener;
  std::string err;
  if (!g_dev->open(opt, err)) {
    copy_error(error, error_len, err);
    delete g_dev;
    g_dev = nullptr;
    return false;
  }
  g_dev->set_rx_handler([](const RxFrame &f) {
    if (!g_rx) return;
    KhalRx k{f.data, f.len, f.dbm, f.channel, f.status, f.mic_stripped};
    g_rx(g_rx_ctx, &k);
  });
  /* The listener answers to its own random locally administered address;
   * AWDL sets the real one later. */
  uint8_t addr[6];
  std::memcpy(addr, g_dev->hw_address().data(), 6);
  addr[0] = static_cast<uint8_t>((addr[0] | 0x02) & 0xfe);
  addr[5] ^= 0x5a;
  if (!g_dev->start_monitor(channel, channel > 14 ? 80 : 20, addr)) {
    copy_error(error, error_len, "the listener could not start");
    delete g_dev;
    g_dev = nullptr;
    return false;
  }
  return true;
}

void khal_close() {
  if (!g_dev) return;
  g_dev->stop();
  delete g_dev;
  g_dev = nullptr;
}

bool khal_is_open() { return g_dev != nullptr; }

void khal_info(KhalInfo *info) {
  std::memset(info, 0, sizeof(*info));
  if (!g_dev) return;
  std::memcpy(info->hw_address, g_dev->hw_address().data(), 6);
}

void khal_set_rx(KhalRxCallback cb, void *ctx) {
  g_rx = cb;
  g_rx_ctx = ctx;
}

int khal_poll() { return g_dev ? g_dev->poll() : 0; }
void khal_check_health() { if (g_dev) g_dev->check_health(); }
bool khal_healthy() { return g_dev && g_dev->healthy(); }

bool khal_set_channel(uint8_t channel, unsigned width_mhz) { return g_dev && g_dev->set_channel(channel, width_mhz); }
uint8_t khal_channel() { return g_dev ? g_dev->channel() : 0; }
bool khal_set_promiscuous(bool on) { return g_dev && g_dev->set_promiscuous(on); }
bool khal_set_listener_address(const uint8_t address[6]) { return g_dev && g_dev->set_address(address); }
bool khal_listener_transmit(const uint8_t *frame, size_t len, unsigned mbps_x2) {
  return g_dev && g_dev->transmit(frame, len, legacy(mbps_x2));
}
uint32_t khal_channel_flags(uint8_t channel) { return g_dev ? g_dev->channel_flags(channel) : 0; }
bool khal_set_country(const char alpha2[3]) { return g_dev && g_dev->set_country(std::string(alpha2, 2)); }

static StaParams to_params(const KhalSta *s) {
  StaParams p{};
  std::memcpy(p.bssid, s->bssid, 6);
  std::memcpy(p.addr, s->addr, 6);
  p.channel = s->channel;
  p.width = s->width ? s->width : 20;
  p.beacon_int = s->beacon_int ? s->beacon_int : 100;
  p.ht = s->ht;
  p.vht = s->vht;
  p.mimo = s->mimo;
  return p;
}

bool khal_sta_join(const KhalSta *sta) { return g_dev && g_dev->sta_join(to_params(sta)); }

bool khal_sta_associated(const KhalSta *agreed, uint16_t aid, uint8_t dtim) {
  if (!g_dev) return false;
  g_dev->sta_join_update(to_params(agreed));
  return g_dev->sta_associated(aid, dtim);
}

bool khal_sta_rate_control() { return g_dev && g_dev->sta_rate_control(); }
bool khal_sta_set_key(bool group, unsigned index, const uint8_t key[16]) {
  return g_dev && g_dev->sta_set_key(group, index, key);
}

bool khal_sta_transmit(const uint8_t *frame, size_t len, bool encrypt, bool mgmt) {
  if (!g_dev) return false;
  TxRate r;
  if (mgmt || !g_dev->sta_tlc()) {
    const bool g5 = g_dev->sta_params().channel > 14;
    r = legacy(g5 ? 12 : 2); /* 6 Mb/s OFDM, or 1 Mb/s CCK on 2.4 GHz */
  } else {
    r.kind = TxRate::Auto;
  }
  return g_dev->sta_transmit(frame, len, r, encrypt) >= 0;
}

bool khal_sta_add_rx_ba(unsigned tid, uint16_t ssn, uint16_t window) {
  return g_dev && g_dev->sta_add_rx_ba(tid, ssn, window);
}
void khal_sta_leave() { if (g_dev) g_dev->sta_leave(); }
bool khal_sta_active() { return g_dev && g_dev->sta_active(); }
unsigned khal_sta_tx_acked() { return g_dev ? g_dev->sta_tx_acked() : 0; }
unsigned khal_sta_tx_failed() { return g_dev ? g_dev->sta_tx_failed() : 0; }
