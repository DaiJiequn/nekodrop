// SPDX-License-Identifier: GPL-2.0-only
/* A user-mode driver for Intel Wi-Fi 6 cards of the 22000 family (AX200,
 * AX201 and kin), reduced to what an AWDL radio needs: boot the firmware,
 * hold one listener MAC context on one channel, hand up every frame heard and
 * inject frames.
 *
 * It is a port of the bring-up and data paths of itlwm's hal_iwx (OpenBSD
 * if_iwx, as extended by ForbiddenItlwm for AWDL) from the macOS kernel to a
 * Windows process:
 *   - registers go through the UMDF register proxy (proxy.h),
 *   - DMA memory is AWE-locked physical memory (dma.h),
 *   - interrupts are not used; the receive ring's status word, which the card
 *     writes by DMA, is polled.
 *
 * Single-threaded: every method must be called from the same thread, which
 * also runs poll(). Received frames are delivered from inside poll() and from
 * inside synchronous firmware commands. */
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "dma.h"
#include "fw.h"
#include "proxy.h"

struct RxFrame {
  const uint8_t *data; /* 802.11 frame, header first, no FCS */
  size_t len;
  int8_t dbm;          /* -127 when unknown */
  uint8_t channel;
  uint32_t rate_n_flags;
};

struct TxRate {
  enum Kind { Legacy, Ht } kind = Legacy;
  unsigned mbps_x2 = 12; /* legacy rate in 500 kb/s units: 12 = 6 Mb/s */
  unsigned mcs = 0;      /* HT MCS 0..15 */
};

struct TxStats {
  uint64_t submitted = 0, completed = 0, acked = 0, failed = 0, retries = 0,
           dropped_full = 0;
};

class IwxDevice {
public:
  enum class MacType { Listener, Ibss, Sta };

  struct Options {
    std::string firmware_dir;  /* holds iwlwifi-*.ucode */
    std::string country = "ZZ"; /* MCC to ask for; ZZ keeps what the card has */
    MacType mac_type = MacType::Listener;
    bool verbose = false;
  };

  using Logger = std::function<void(const std::string &)>;
  using RxHandler = std::function<void(const RxFrame &)>;

  explicit IwxDevice(Logger log) : _log(std::move(log)) {}
  ~IwxDevice();

  /* Opens the card, boots its firmware and runs the init sequence. */
  bool open(const Options &options, std::string &error);
  /* Adds the PHY/MAC/binding/station/queue for monitoring on `channel`
   * (1-14 or a 5 GHz number) at `width_mhz` (20, 40 or 80). `address` is the
   * MAC the listener answers to; it can change later. */
  bool start_monitor(uint8_t channel, unsigned width_mhz, const uint8_t address[6]);
  bool set_channel(uint8_t channel, unsigned width_mhz);
  bool set_address(const uint8_t address[6]);
  /* Queues one frame (802.11 header first, no FCS). False if the ring is full
   * or the frame is malformed. */
  bool transmit(const uint8_t *frame, size_t len, const TxRate &rate);
  /* Handles whatever the card has delivered since the last call. Returns the
   * number of receive buffers processed. */
  int poll();
  /* Stops DMA and resets the card. Safe to call twice. */
  void stop();

  void set_rx_handler(RxHandler handler) { _on_rx = std::move(handler); }
  const TxStats &tx_stats() const { return _tx_stats; }
  uint8_t channel() const { return _channel; }
  unsigned width_mhz() const { return _width; }
  bool healthy() const { return _open && !_fw_error && !_proxy.failed(); }
  const std::array<uint8_t, 6> &hw_address() const { return _hw_addr; }

private:
  struct TxRing {
    DmaBuf desc, bc_tbl, cmd, payload;
    unsigned count = 0, qid = 0, cur = 0, tail = 0, queued = 0;
  };
  struct CmdSlot {
    bool pending = false, want_resp = false, narrow = false;
    std::vector<uint8_t> resp;
  };

  /* Registers */
  uint32_t rd(uint32_t off) { return _proxy.read32(off); }
  void wr(uint32_t off, uint32_t v) { _proxy.write32(off, v); }
  void setb(uint32_t off, uint32_t bits) { _proxy.set_bits(off, bits); }
  void clrb(uint32_t off, uint32_t bits) { _proxy.clear_bits(off, bits); }
  bool poll_bit(uint32_t reg, uint32_t bits, uint32_t mask, unsigned timeout_us);
  bool nic_lock();
  void nic_unlock();
  uint32_t read_prph(uint32_t addr);
  void write_prph(uint32_t addr, uint32_t value);
  void set_bits_prph(uint32_t addr, uint32_t bits);
  void clear_bits_prph(uint32_t addr, uint32_t bits);

  /* Bring-up */
  bool identify(std::string &error);
  bool alloc_rings(std::string &error);
  bool set_hw_ready();
  bool prepare_card_hw();
  void clear_persistence_bit();
  void force_power_gating();
  bool apm_init();
  void apm_stop();
  bool start_hw();
  void mask_interrupts();
  void nic_config();
  bool load_firmware(std::string &error);
  bool run_init_sequence(std::string &error);
  void set_ltr();

  /* Firmware commands */
  int send_cmd(uint32_t id, const void *data, size_t len, bool want_resp,
               std::vector<uint8_t> *resp, unsigned timeout_ms = 2000);
  int send_cmd_status(uint32_t id, const void *data, size_t len, uint32_t *status);
  bool wait_for(const std::function<bool()> &done, unsigned timeout_ms);
  uint8_t cmd_version(uint8_t group, uint8_t cmd) const {
    return _fw.cmd_version(group, cmd);
  }
  uint8_t valid_tx_ant() const;
  uint8_t valid_rx_ant() const;
  int nvm_get();
  int update_mcc(const std::string &alpha2);
  int phy_ctxt_cmd(uint32_t action);
  int mac_ctxt_cmd(uint32_t action);
  int binding_cmd(uint32_t action);
  int add_monitor_sta();
  int enable_monitor_queue();

  /* Receive */
  void handle_rb(unsigned index);
  void handle_packet(const uint8_t *pkt, size_t len);
  void handle_mpdu(const uint8_t *payload, size_t len);
  void handle_tx_resp(const uint8_t *payload, size_t len);
  void dump_error_tables();

  Logger _log;
  RxHandler _on_rx;
  Options _opt;
  RegProxy _proxy;
  DmaPool _dma;
  Firmware _fw;

  uint32_t _hw_rev = 0, _hw_rev_raw = 0, _hw_rf_id = 0;
  bool _integrated = true;
  std::string _fw_name;
  int _nic_locks = 0;
  bool _open = false, _fw_error = false;

  DmaBuf _ctxt_info, _rx_free, _rx_used, _rx_stat;
  std::vector<DmaBuf> _rx_bufs;
  std::vector<DmaBuf> _fw_sections; /* kept until the card is reset */
  unsigned _rx_cur = 0;
  TxRing _cmdq, _dataq;
  std::vector<CmdSlot> _cmd_slots;
  std::vector<DmaBuf> _cmd_big; /* per slot, for commands over 316 bytes */

  /* Notifications the bring-up waits for */
  bool _alive_seen = false, _alive_ok = false, _init_complete = false;
  uint32_t _lmac_error_table = 0, _umac_error_table = 0;

  /* NVM */
  uint8_t _nvm_tx_ant = 0, _nvm_rx_ant = 0;
  bool _lar_enabled = false;
  std::array<uint8_t, 6> _hw_addr{};

  /* Monitor state */
  bool _phy_added = false, _mac_added = false, _binding_added = false,
       _sta_added = false, _queue_enabled = false;
  uint8_t _channel = 0;
  unsigned _width = 20;
  std::array<uint8_t, 6> _address{};
  TxStats _tx_stats;
};
