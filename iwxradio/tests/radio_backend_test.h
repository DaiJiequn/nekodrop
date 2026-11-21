// SPDX-License-Identifier: GPL-2.0-only
/* Offline checks of the radio backend contract (src/radio_backend.h): the
 * capability matrix of the two backends radio_main runs (iwx, kernel), the
 * policies radio_main applies per capability, and that a backend with no
 * Wi-Fi station, no firmware scan and no Intel A-MPDU queue (a stand-in for
 * the planned Realtek "devourer" backend, docs/HARDWARE.md) satisfies the
 * contract with the core listener calls alone, without stubs.
 *
 * Portable (no Windows headers), so it also builds outside iwx_selftest.
 * Returns the number of failures. */
#pragma once

#include <array>
#include <cctype>
#include <cstdio>
#include <string>

#include "../src/radio_backend.h"

namespace radio_backend_test {

/* The stand-in: what a user-mode Realtek backend would declare today. */
constexpr RadioCapabilities devourer_capabilities() {
  RadioCapabilities c{};
  c.backend = "devourer";
  c.max_tx_window = radio::kDefaultTxWindow; /* no faster sending until measured */
  return c;
}

/* The core calls only: no sta_*, umac_scan, add_rx_ba, set_promiscuous,
 * agg_*, kick_data, answer_country_notice, set_country or channel_flags. */
struct MockDevourerBackend {
  static constexpr RadioCapabilities kCapabilities = devourer_capabilities();
  bool open(const RadioOpenOptions &, std::string &) { return true; }
  bool start_monitor(uint8_t, unsigned, const uint8_t *) { return true; }
  bool set_channel(uint8_t, unsigned) { return true; }
  bool set_address(const uint8_t *) { return true; }
  bool transmit(const uint8_t *, size_t, const TxRate &) { return true; }
  int transmit_slot(const uint8_t *, size_t, const TxRate &, bool = true) { return 0; }
  void set_rx_handler(RadioRxHandler h) { _rx = h; }
  void set_tx_status_handler(RadioTxStatusHandler h) { _tx = h; }
  int poll() { return 0; }
  void check_health() {}
  bool healthy() const { return true; }
  const TxStats &tx_stats() const { return _stats; }
  uint8_t channel() const { return 44; }
  unsigned width_mhz() const { return 80; }
  const std::array<uint8_t, 6> &hw_address() const { return _hw; }
  void stop() {}

private:
  RadioRxHandler _rx;
  RadioTxStatusHandler _tx;
  TxStats _stats;
  std::array<uint8_t, 6> _hw{};
};

/* Compiling these is the check: a stub-free backend meets the contract, and
 * the detectors see the optional groups it lacks. */
static_assert(radio::check_backend_contract<MockDevourerBackend>(), "devourer stand-in meets the contract");
static_assert(radio::detail::has_core<MockDevourerBackend>::value, "core calls detected");
static_assert(!radio::detail::has_station<MockDevourerBackend>::value, "no station calls");
static_assert(!radio::detail::has_umac_scan<MockDevourerBackend>::value, "no firmware scan");
static_assert(!radio::detail::has_aggregate<MockDevourerBackend>::value, "no A-MPDU calls");
static_assert(!radio::detail::has_batched_kick<MockDevourerBackend>::value, "no kick_data");
static_assert(!radio::detail::has_channel_flags<MockDevourerBackend>::value, "no channel flags");
/* A type missing the core calls is seen as such. */
struct NotABackend {};
static_assert(!radio::detail::has_core<NotABackend>::value, "an empty type is not a backend");
static_assert(!radio::detail::has_capabilities<NotABackend>::value, "an empty type declares nothing");

inline bool contains_ci(const std::string &hay, const char *needle) {
  std::string h, n(needle);
  for (char c : hay) h += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  for (auto &c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return h.find(n) != std::string::npos;
}

inline int run() {
  int failures = 0;
  const auto check = [&](bool ok, const char *what) {
    std::printf("%s backend contract: %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failures++;
  };
  constexpr RadioCapabilities iwx = radio::iwx_capabilities();
  constexpr RadioCapabilities kern = radio::kernel_capabilities();
  constexpr RadioCapabilities dev = devourer_capabilities();

  /* The matrix (docs/HARDWARE.md, "The radio's backend contract"). */
  check(radio::validate_capabilities(iwx) && radio::validate_capabilities(kern) && radio::validate_capabilities(dev),
        "iwx, kernel and devourer capability sets are consistent");
  check(std::string(iwx.backend) == "iwx" && iwx.supports_station && !iwx.windows_owns_station &&
            iwx.supports_umac_scan && iwx.supports_rx_ba && iwx.supports_promiscuous_toggle &&
            iwx.supports_aggregate && iwx.supports_batched_kick && iwx.supports_country_notice &&
            iwx.supports_set_country && iwx.enforces_passive_channel && iwx.max_tx_window == 64,
        "iwx: everything, TX window up to 64");
  check(std::string(kern.backend) == "kernel" && !kern.supports_station && kern.windows_owns_station &&
            !kern.supports_umac_scan && kern.supports_rx_ba && kern.supports_promiscuous_toggle &&
            !kern.supports_aggregate && !kern.supports_batched_kick && !kern.supports_country_notice &&
            kern.supports_set_country && kern.enforces_passive_channel && kern.max_tx_window == 64,
        "kernel: Windows owns the station; Block Ack, promiscuous, country, passive rule; no A-MPDU");
  check(!dev.supports_station && !dev.windows_owns_station && !dev.supports_umac_scan && !dev.supports_rx_ba &&
            !dev.supports_aggregate && !dev.supports_batched_kick && !dev.enforces_passive_channel,
        "devourer stand-in: listener and injector only");

  /* Inconsistent sets are refused. */
  {
    RadioCapabilities c = iwx;
    c.windows_owns_station = true;
    check(!radio::validate_capabilities(c), "refused: station on the backend and on Windows");
    c = iwx;
    c.supports_rx_ba = false;
    check(!radio::validate_capabilities(c), "refused: aggregate without receive Block Ack");
    c = iwx;
    c.supports_promiscuous_toggle = false;
    check(!radio::validate_capabilities(c), "refused: aggregate without promiscuous control");
    c = iwx;
    c.max_tx_window = 65;
    check(!radio::validate_capabilities(c), "refused: TX window above 64");
    c = iwx;
    c.max_tx_window = 4;
    check(!radio::validate_capabilities(c), "refused: TX window below the default 8");
    c = kern;
    c.enforces_passive_channel = false;
    c.supports_country_notice = true;
    check(!radio::validate_capabilities(c), "refused: country notice without channel flags");
    c = dev;
    c.backend = "";
    check(!radio::validate_capabilities(c), "refused: no backend name");
  }

  /* TX window: what radio_main did before the contract, for iwx and kernel. */
  {
    const radio::TxConfig i8 = radio::resolve_tx_config(8, iwx), i48 = radio::resolve_tx_config(48, iwx);
    check(i8.window == 8 && !i8.batch_kick && i8.initial_rate_step == 0 && !i8.window_clamped,
          "iwx default: window 8, no batch doorbell, ladder from step 0");
    check(i48.window == 48 && i48.batch_kick && i48.initial_rate_step == 1 && !i48.window_clamped,
          "iwx faster sending (48): window 48, batch doorbell, ladder from step 1");
    const radio::TxConfig k8 = radio::resolve_tx_config(8, kern), k48 = radio::resolve_tx_config(48, kern);
    check(k8.window == 8 && !k8.batch_kick && k8.initial_rate_step == 0, "kernel default: window 8, step 0");
    check(k48.window == 48 && !k48.batch_kick && k48.initial_rate_step == 1 && !k48.window_clamped,
          "kernel --tx-window 48: window 48 and step 1 as before, the driver rings its own doorbell");
    const radio::TxConfig d48 = radio::resolve_tx_config(48, dev);
    check(d48.window == 8 && d48.window_clamped && !d48.batch_kick && d48.initial_rate_step == 0,
          "devourer --tx-window 48: clamped to its maximum 8");
  }

  /* Passive channels (#11). */
  {
    const uint32_t passive = 0, active = radio::kChannelActiveFlag;
    check(radio::channel_allows_tx(6, passive, false, iwx), "2.4 GHz always allows sending");
    check(!radio::channel_allows_tx(44, passive, false, iwx) && !radio::channel_allows_tx(44, passive, false, kern),
          "passive 44 holds frames on iwx and kernel");
    check(radio::channel_allows_tx(44, passive, true, iwx), "a beacon or AWDL peer heard unlocks 44");
    check(radio::channel_allows_tx(44, active, false, kern), "active 44 allows sending");
    check(radio::channel_allows_tx(44, passive, false, dev), "no channel flags: no passive rule");
  }

  /* Discovery scan and the Wi-Fi list scan. */
  {
    check(radio::should_start_passive_discovery_scan(false, false, false, iwx),
          "iwx on a passive channel: one discovery scan");
    check(!radio::should_start_passive_discovery_scan(true, false, false, iwx) &&
              !radio::should_start_passive_discovery_scan(false, true, false, iwx) &&
              !radio::should_start_passive_discovery_scan(false, false, true, iwx),
          "no discovery scan when sending is allowed or a scan is on its way");
    check(!radio::should_start_passive_discovery_scan(false, false, false, kern) &&
              !radio::should_start_passive_discovery_scan(false, false, false, dev),
          "no discovery scan without a firmware scan and country notice");
    check(!radio::is_scan_complete(false, false, 99999, iwx), "no scan, nothing completes");
    check(!radio::is_scan_complete(true, false, 300, iwx) && radio::is_scan_complete(true, false, 301, iwx),
          "firmware scan: done once the firmware says so, after 300 ms");
    check(!radio::is_scan_complete(true, true, 15000, iwx) && radio::is_scan_complete(true, true, 15001, iwx),
          "firmware scan still running: ends after 15 s");
    check(!radio::is_scan_complete(true, false, 2000, kern) && radio::is_scan_complete(true, false, 2001, kern),
          "listening scan (kernel): 2 s");
    check(radio::is_scan_complete(true, false, 2001, dev), "listening scan (devourer): 2 s");
  }

  /* Station policy wording. */
  check(radio::station_policy_message(iwx).empty(), "iwx runs the station");
  check(radio::station_policy_message(kern) ==
            "station config ignored: Windows runs the station on the kernel driver",
        "kernel: the line the radio always printed");
  check(radio::station_policy_message(dev) == "station config ignored: backend devourer has no Wi-Fi station",
        "devourer: names the backend");

  /* The start-up summary must not look like a stage, an error or an event
   * to installer/report.ps1 (its error filter is case-insensitive) or to
   * Supervisor.cs (prefixes it acts on). */
  {
    static const char *const kTriggers[] = {
        "error", "fail", "refus", "cannot", "could not", "timed out", "not supported", "exited", "restart",
        "giving the card", "taking the card", "ready on", "pnvm", "init_complete", "nvm:", "alive",
        "unsupported", "card 0x", "family", "rx: ", "tx: ", "aggregate: ", "event: ", "bring-up",
        "transmitting allowed", "moving there", "stayed passive", "starting:", "stopping:", "wi-fi: joining",
        "gateway", "lmac", "umac", "secboot", "wifi: "};
    const RadioCapabilities all[] = {iwx, kern, dev};
    for (const auto &c : all) {
      const std::string s = radio::format_capabilities_summary(c);
      const char *hit = nullptr;
      for (const char *t : kTriggers)
        if (contains_ci(s, t)) hit = t;
      std::printf("     %s\n", s.c_str());
      check(!hit && s.rfind(std::string("backend ") + c.backend + ": ", 0) == 0,
            hit ? hit : "summary line is plain information");
    }
    check(radio::format_capabilities_summary(iwx).find("tx-window up to 64") != std::string::npos &&
              radio::format_capabilities_summary(kern).find("station windows") != std::string::npos,
          "summary names the window and who owns the station");
  }
  return failures;
}

} // namespace radio_backend_test
