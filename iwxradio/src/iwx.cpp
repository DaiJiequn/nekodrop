// SPDX-License-Identifier: GPL-2.0-only
/*
 * User-mode Intel 22000-family driver. Derived from itlwm's hal_iwx
 * (ItlIwx.cpp, GPL-2.0, by zxystd, extended by retropineaple's ForbiddenItlwm),
 * which is itself a port of OpenBSD's if_iwx (ISC, Stefan Sperling et al.),
 * which follows Linux iwlwifi (dual BSD/GPL). Function names keep the iwx_
 * vocabulary where a step corresponds one to one, so the two can be read side
 * by side.
 */
#include "iwx.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#include "iwxreg.h"

namespace {

/* struct ieee80211_frame is only used as a flexible array in the header. */
/* The 22000 firmware walks every TFD queue as 256 entries whatever size it is
 * told: a 64-entry ring made it transmit the 192 "frames" of memory behind
 * the ring at every wrap, and assert on one of them (0x4216). */
constexpr unsigned kTxQueueSize = IWX_TFD_QUEUE_SIZE_MAX;
constexpr unsigned kCmdPayloadInline =
    sizeof(((iwx_device_cmd *)nullptr)->data_wide);
constexpr unsigned kTxPayloadSlot = 4096;
/* Pages locked while carving: enough that runs of 8 adjacent frames turn up
 * for the firmware's 32 KiB sections; the rest are returned once carved. */
constexpr size_t kDmaScanPages = 262144; /* 1 GiB, locked for well under a second */
constexpr size_t kDmaMinPages = 8192;

const uint8_t kNvmChannels[] = {
    1,   2,   3,   4,   5,   6,   7,   8,   9,   10,  11,  12,  13,  14,  36,  40,
    44,  48,  52,  56,  60,  64,  68,  72,  76,  80,  84,  88,  92,  96,  100, 104,
    108, 112, 116, 120, 124, 128, 132, 136, 140, 144, 149, 153, 157, 161, 165, 169,
    173, 177, 181};

uint64_t now_us() {
  static LARGE_INTEGER freq = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return f;
  }();
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return static_cast<uint64_t>(t.QuadPart) * 1000000ull / static_cast<uint64_t>(freq.QuadPart);
}

void delay_us(unsigned us) {
  if (us >= 2000) {
    Sleep(us / 1000);
    return;
  }
  const uint64_t end = now_us() + us;
  while (now_us() < end) YieldProcessor();
}

std::string hex(uint32_t v) {
  char b[16];
  std::snprintf(b, sizeof(b), "0x%08x", v);
  return b;
}

/* Header length of an 802.11 frame, as net80211's ieee80211_get_hdrlen. */
size_t hdrlen_80211(const uint8_t *f, size_t len) {
  if (len < 10) return 0;
  const uint8_t type = f[0] & 0x0c;
  if (type == 0x04) { /* control */
    const uint8_t sub = f[0] & 0xf0;
    return (sub == 0xc0 || sub == 0xd0) ? 10 : 16; /* CTS/ACK : RTS etc. */
  }
  size_t h = 24;
  if ((f[1] & 0x03) == 0x03) h += 6;       /* four addresses */
  if (type == 0x08 && (f[0] & 0x80)) h += 2; /* QoS data */
  if ((f[1] & 0x80) && type == 0x08 && (f[0] & 0x80)) h += 4; /* +HTC */
  return h <= len ? h : 0;
}

uint16_t offload_header(unsigned hdrlen) {
  return static_cast<uint16_t>(((hdrlen / 2) << 8) | ((hdrlen % 4) ? (1u << 13) : 0));
}

uint8_t ofdm_plcp(unsigned mbps_x2) {
  switch (mbps_x2) {
  case 12: return 0xd;
  case 18: return 0xf;
  case 24: return 0x5;
  case 36: return 0x7;
  case 48: return 0x9;
  case 72: return 0xb;
  case 96: return 0x1;
  case 108: return 0x3;
  default: return 0;
  }
}

int ofdm_index(unsigned mbps_x2) {
  static const unsigned rates[] = {12, 18, 24, 36, 48, 72, 96, 108};
  for (int i = 0; i < 8; i++)
    if (rates[i] == mbps_x2) return i;
  return -1;
}

int cck_index(unsigned mbps_x2) {
  static const unsigned rates[] = {2, 4, 11, 22};
  for (int i = 0; i < 4; i++)
    if (rates[i] == mbps_x2) return i;
  return -1;
}

uint8_t cck_plcp(unsigned mbps_x2) {
  switch (mbps_x2) {
  case 2: return 10;
  case 4: return 20;
  case 11: return 55;
  case 22: return 110;
  default: return 0;
  }
}

} // namespace

IwxDevice::~IwxDevice() { stop(); }

/* ---------------------------------------------------------------- registers */

bool IwxDevice::poll_bit(uint32_t reg, uint32_t bits, uint32_t mask, unsigned timeout_us) {
  const uint64_t end = now_us() + timeout_us;
  for (;;) {
    const uint32_t v = rd(reg);
    if (v != 0xffffffffu && (v & mask) == (bits & mask)) return true;
    if (now_us() >= end || _proxy.failed()) return false;
    delay_us(10);
  }
}

bool IwxDevice::nic_lock() {
  if (_nic_locks > 0) {
    _nic_locks++;
    return true;
  }
  setb(IWX_CSR_GP_CNTRL, IWX_CSR_GP_CNTRL_REG_FLAG_MAC_ACCESS_REQ);
  delay_us(2);
  if (poll_bit(IWX_CSR_GP_CNTRL, IWX_CSR_GP_CNTRL_REG_VAL_MAC_ACCESS_EN,
               IWX_CSR_GP_CNTRL_REG_FLAG_MAC_CLOCK_READY |
                   IWX_CSR_GP_CNTRL_REG_FLAG_GOING_TO_SLEEP,
               150000)) {
    _nic_locks++;
    return true;
  }
  _log("acquiring the device (MAC access) failed");
  return false;
}

void IwxDevice::nic_unlock() {
  if (_nic_locks > 0 && --_nic_locks == 0)
    clrb(IWX_CSR_GP_CNTRL, IWX_CSR_GP_CNTRL_REG_FLAG_MAC_ACCESS_REQ);
}

/* The two accesses of a peripheral read go in one proxy call. */
uint32_t IwxDevice::read_prph(uint32_t addr) {
  std::vector<RegOp> ops{
      {IWX_HBUS_TARG_PRPH_RADDR, (addr & 0x000fffff) | (3u << 24), 1},
      {IWX_HBUS_TARG_PRPH_RDAT, 0, 0}};
  return _proxy.batch(ops) ? ops[1].value : 0xffffffffu;
}

void IwxDevice::write_prph(uint32_t addr, uint32_t value) {
  std::vector<RegOp> ops{
      {IWX_HBUS_TARG_PRPH_WADDR, (addr & 0x000fffff) | (3u << 24), 1},
      {IWX_HBUS_TARG_PRPH_WDAT, value, 1}};
  _proxy.batch(ops);
}

void IwxDevice::set_bits_prph(uint32_t addr, uint32_t bits) {
  if (nic_lock()) {
    write_prph(addr, read_prph(addr) | bits);
    nic_unlock();
  }
}

void IwxDevice::clear_bits_prph(uint32_t addr, uint32_t bits) {
  if (nic_lock()) {
    write_prph(addr, read_prph(addr) & ~bits);
    nic_unlock();
  }
}

/* ----------------------------------------------------------------- bring-up */

bool IwxDevice::identify(std::string &error) {
  if (_proxy.vendor() != 0x8086) {
    error = "the card bound to the proxy is not an Intel device";
    return false;
  }
  _hw_rev_raw = rd(IWX_CSR_HW_REV);
  if (_hw_rev_raw == 0xffffffffu) {
    error = "the card does not answer (CSR_HW_REV reads all ones)";
    return false;
  }
  /* 8000 family and later: the step is in bits 0-1; keep the old layout. */
  _hw_rev = (_hw_rev_raw & 0xfff0) | (IWX_CSR_HW_REV_STEP(_hw_rev_raw << 2) << 2);
  _hw_rf_id = rd(IWX_CSR_HW_RF_ID);
  const uint32_t mac_type = CSR_HW_REV_TYPE(_hw_rev);
  /* 0x33 = Qu, 0x35 = QuZ: the AX201 (CNVi) and AX200-class Qu parts. */
  if (mac_type != 0x33 && mac_type != 0x35) {
    error = "unsupported Intel MAC type " + hex(mac_type) +
            " (this build knows Qu and QuZ, the AX201 family)";
    return false;
  }
  if (_hw_rev == CSR_HW_REV_TYPE_QUZ)
    _fw_name = "iwlwifi-QuZ-a0-hr-b0-68.ucode";
  else if (_hw_rev == CSR_HW_REV_TYPE_QU_C0)
    _fw_name = "iwlwifi-Qu-c0-hr-b0-68.ucode";
  else
    _fw_name = "iwlwifi-Qu-b0-hr-b0-68.ucode";
  /* The AX201 is a CNVi: the MAC half sits in the chipset. 0x2723 (AX200)
   * would be discrete, but it is a "cc" part this build does not load. */
  _integrated = true;
  _log("card " + hex(_proxy.vendor()) + ":" + hex(_proxy.device()) + " subsystem " +
       hex(_proxy.subsystem()) + ", hw_rev " + hex(_hw_rev_raw) + " rf_id " + hex(_hw_rf_id) +
       ", firmware " + _fw_name);
  return true;
}

bool IwxDevice::alloc_rings(std::string &error) {
  if (!_dma.init(kDmaScanPages, kDmaMinPages, error)) return false;
  _log("DMA: " + std::to_string(_dma.pages_locked()) + " pages locked, longest physical run " +
       std::to_string(_dma.longest_run()) + " pages, " + std::to_string(_dma.runs_of(8)) +
       " runs of 8+ (the firmware needs about 20)");

  _ctxt_info = _dma.alloc(sizeof(iwx_context_info), 4096);
  /* Receive: free list (64-bit addresses with the buffer id in the low 12
   * bits), used list, status word, 512 buffers of 4 KiB. */
  _rx_free = _dma.alloc(IWX_RX_MQ_RING_COUNT * sizeof(uint64_t), 256);
  _rx_used = _dma.alloc(IWX_RX_MQ_RING_COUNT * sizeof(uint32_t), 256);
  _rx_stat = _dma.alloc(sizeof(iwx_rb_status), 16);
  if (!_ctxt_info || !_rx_free || !_rx_used || !_rx_stat) {
    error = "out of DMA memory for the rings";
    return false;
  }
  _rx_bufs.resize(IWX_RX_MQ_RING_COUNT);
  auto *free_list = reinterpret_cast<uint64_t *>(_rx_free.va);
  for (unsigned i = 0; i < IWX_RX_MQ_RING_COUNT; i++) {
    _rx_bufs[i] = _dma.alloc_page();
    if (!_rx_bufs[i]) {
      error = "out of DMA memory for receive buffers";
      return false;
    }
    free_list[i] = htole64(_rx_bufs[i].pa | (i & 0x0fff));
  }

  const auto make_ring = [&](TxRing &ring, unsigned count, unsigned qid, size_t payload) {
    ring.count = count;
    ring.qid = qid;
    ring.desc = _dma.alloc(count * sizeof(iwx_tfh_tfd), 256);
    ring.bc_tbl = _dma.alloc(sizeof(iwx_agn_scd_bc_tbl), 1024);
    bool ok = ring.desc && ring.bc_tbl;
    if (payload) {
      /* Data ring: a page per slot for the TX command and one for the
       * payload, so only the 64 KiB of descriptors need adjacent frames. */
      ring.payload.resize(count);
      ring.cmd_slot.resize(count);
      for (auto &slot : ring.payload) ok = ok && (slot = _dma.alloc_page());
      for (auto &slot : ring.cmd_slot) ok = ok && (slot = _dma.alloc_page());
    } else {
      ring.cmd = _dma.alloc(count * sizeof(iwx_device_cmd), IWX_FIRST_TB_SIZE_ALIGN);
      ok = ok && ring.cmd;
    }
    return ok;
  };
  if (!make_ring(_cmdq, IWX_CMD_QUEUE_SIZE, IWX_DQA_CMD_QUEUE, 0) ||
      !make_ring(_dataq, kTxQueueSize, 0, kTxPayloadSlot) ||
      !make_ring(_staq, kTxQueueSize, 0, kTxPayloadSlot)) {
    error = "out of DMA memory for the transmit rings";
    return false;
  }
  _cmd_slots.resize(IWX_CMD_QUEUE_SIZE);
  _cmd_big.resize(IWX_CMD_QUEUE_SIZE);
  for (auto &big : _cmd_big) {
    big = _dma.alloc_page();
    if (!big) {
      error = "out of DMA memory for command buffers";
      return false;
    }
  }
  return true;
}

bool IwxDevice::set_hw_ready() {
  setb(IWX_CSR_HW_IF_CONFIG_REG, IWX_CSR_HW_IF_CONFIG_REG_BIT_NIC_READY);
  const bool ready = poll_bit(IWX_CSR_HW_IF_CONFIG_REG, IWX_CSR_HW_IF_CONFIG_REG_BIT_NIC_READY,
                              IWX_CSR_HW_IF_CONFIG_REG_BIT_NIC_READY, 50);
  if (ready) setb(IWX_CSR_MBOX_SET_REG, IWX_CSR_MBOX_SET_REG_OS_ALIVE);
  return ready;
}

bool IwxDevice::prepare_card_hw() {
  if (set_hw_ready()) return true;
  setb(IWX_CSR_DBG_LINK_PWR_MGMT_REG, IWX_CSR_RESET_LINK_PWR_MGMT_DISABLED);
  delay_us(1000);
  for (int tries = 0; tries < 10; tries++) {
    setb(IWX_CSR_HW_IF_CONFIG_REG, IWX_CSR_HW_IF_CONFIG_REG_PREPARE);
    const uint64_t end = now_us() + 150000;
    do {
      if (rd(IWX_CSR_HW_IF_CONFIG_REG) == 0xffffffffu) return false;
      if (set_hw_ready()) return true;
      delay_us(200);
    } while (now_us() < end);
    delay_us(25000);
  }
  return false;
}

void IwxDevice::clear_persistence_bit() {
  const uint32_t hpm = read_prph(IWX_HPM_DEBUG);
  if (hpm != 0xa5a5a5a0 && (hpm & IWX_PERSISTENCE_BIT)) {
    if (read_prph(IWX_PREG_PRPH_WPROT_22000) & IWX_PREG_WFPM_ACCESS) {
      _log("cannot clear the persistence bit");
      return;
    }
    write_prph(IWX_HPM_DEBUG, hpm & ~IWX_PERSISTENCE_BIT);
  }
}

void IwxDevice::force_power_gating() {
  set_bits_prph(IWX_HPM_HIPM_GEN_CFG, IWX_HPM_HIPM_GEN_CFG_CR_FORCE_ACTIVE);
  delay_us(20);
  set_bits_prph(IWX_HPM_HIPM_GEN_CFG,
                IWX_HPM_HIPM_GEN_CFG_CR_PG_EN | IWX_HPM_HIPM_GEN_CFG_CR_SLP_EN);
  delay_us(20);
  clear_bits_prph(IWX_HPM_HIPM_GEN_CFG, IWX_HPM_HIPM_GEN_CFG_CR_FORCE_ACTIVE);
}

bool IwxDevice::apm_init() {
  setb(IWX_CSR_GIO_CHICKEN_BITS, IWX_CSR_GIO_CHICKEN_BITS_REG_BIT_L1A_NO_L0S_RX);
  setb(IWX_CSR_DBG_HPET_MEM_REG, IWX_CSR_DBG_HPET_MEM_REG_VAL);
  setb(IWX_CSR_HW_IF_CONFIG_REG, IWX_CSR_HW_IF_CONFIG_REG_BIT_HAP_WAKE_L1A);
  /* iwx_apm_config also clears ASPM in PCI config space, which a UMDF
   * driver cannot reach; the L0s bit here is the part within the BAR. */
  setb(IWX_CSR_GIO_REG, IWX_CSR_GIO_REG_VAL_L0S_DISABLED);
  setb(IWX_CSR_GP_CNTRL, IWX_CSR_GP_CNTRL_REG_FLAG_INIT_DONE);
  if (!poll_bit(IWX_CSR_GP_CNTRL, IWX_CSR_GP_CNTRL_REG_FLAG_MAC_CLOCK_READY,
                IWX_CSR_GP_CNTRL_REG_FLAG_MAC_CLOCK_READY, 25000)) {
    _log("timeout waiting for the clock to stabilise");
    return false;
  }
  return true;
}

void IwxDevice::apm_stop() {
  setb(IWX_CSR_DBG_LINK_PWR_MGMT_REG, IWX_CSR_RESET_LINK_PWR_MGMT_DISABLED);
  setb(IWX_CSR_HW_IF_CONFIG_REG,
       IWX_CSR_HW_IF_CONFIG_REG_PREPARE | IWX_CSR_HW_IF_CONFIG_REG_ENABLE_PME);
  delay_us(1000);
  clrb(IWX_CSR_DBG_LINK_PWR_MGMT_REG, IWX_CSR_RESET_LINK_PWR_MGMT_DISABLED);
  delay_us(5000);
  setb(IWX_CSR_RESET, IWX_CSR_RESET_REG_FLAG_STOP_MASTER);
  if (!poll_bit(IWX_CSR_RESET, IWX_CSR_RESET_REG_FLAG_MASTER_DISABLED,
                IWX_CSR_RESET_REG_FLAG_MASTER_DISABLED, 100))
    _log("timeout waiting for the bus master to stop");
  clrb(IWX_CSR_GP_CNTRL, IWX_CSR_GP_CNTRL_REG_FLAG_INIT_DONE);
}

/* Nothing here takes interrupts: every cause stays masked, so the card never
 * raises a line no handler listens to. CSR_INT still latches the causes. */
void IwxDevice::mask_interrupts() {
  wr(IWX_CSR_INT_MASK, 0);
  wr(IWX_CSR_INT, ~0u);
  wr(IWX_CSR_FH_INT_STATUS, ~0u);
  wr(IWX_CSR_MSIX_FH_INT_MASK_AD, ~0u);
  wr(IWX_CSR_MSIX_HW_INT_MASK_AD, ~0u);
}

bool IwxDevice::start_hw() {
  if (!prepare_card_hw()) {
    _log("the card did not become ready");
    return false;
  }
  clear_persistence_bit();
  setb(IWX_CSR_RESET, IWX_CSR_RESET_REG_FLAG_SW_RESET);
  delay_us(5000);
  if (_integrated) {
    setb(IWX_CSR_GP_CNTRL, IWX_CSR_GP_CNTRL_REG_FLAG_INIT_DONE);
    delay_us(20);
    if (!poll_bit(IWX_CSR_GP_CNTRL, IWX_CSR_GP_CNTRL_REG_FLAG_MAC_CLOCK_READY,
                  IWX_CSR_GP_CNTRL_REG_FLAG_MAC_CLOCK_READY, 25000)) {
      _log("timeout waiting for the clock before power gating");
      return false;
    }
    force_power_gating();
    setb(IWX_CSR_RESET, IWX_CSR_RESET_REG_FLAG_SW_RESET);
    delay_us(5000);
  }
  if (!apm_init()) return false;
  /* MSI rather than MSI-X causes, so CSR_INT reports them (iwx_conf_msix_hw
   * with sc_msix = 0). */
  if (nic_lock()) {
    write_prph(IWX_UREG_CHICK, IWX_UREG_CHICK_MSI_ENABLE);
    nic_unlock();
  }
  mask_interrupts();
  setb(IWX_CSR_GP_CNTRL, IWX_CSR_GP_CNTRL_REG_FLAG_RFKILL_WAKE_L1A_EN);
  const bool rfkill = (rd(IWX_CSR_GP_CNTRL) & IWX_CSR_GP_CNTRL_REG_FLAG_HW_RF_KILL_SW) == 0;
  if (rfkill) _log("warning: the hardware radio switch reports the radio off");
  return true;
}

void IwxDevice::nic_config() {
  const uint32_t cfg = _fw.phy_config;
  const uint32_t radio_type = (cfg & IWX_FW_PHY_CFG_RADIO_TYPE) >> IWX_FW_PHY_CFG_RADIO_TYPE_POS;
  const uint32_t radio_step = (cfg & IWX_FW_PHY_CFG_RADIO_STEP) >> IWX_FW_PHY_CFG_RADIO_STEP_POS;
  const uint32_t radio_dash = (cfg & IWX_FW_PHY_CFG_RADIO_DASH) >> IWX_FW_PHY_CFG_RADIO_DASH_POS;
  uint32_t reg = 0;
  reg |= IWX_CSR_HW_REV_STEP(_hw_rev) << IWX_CSR_HW_IF_CONFIG_REG_POS_MAC_STEP;
  reg |= IWX_CSR_HW_REV_DASH(_hw_rev) << IWX_CSR_HW_IF_CONFIG_REG_POS_MAC_DASH;
  reg |= radio_type << IWX_CSR_HW_IF_CONFIG_REG_POS_PHY_TYPE;
  reg |= radio_step << IWX_CSR_HW_IF_CONFIG_REG_POS_PHY_STEP;
  reg |= radio_dash << IWX_CSR_HW_IF_CONFIG_REG_POS_PHY_DASH;
  const uint32_t mask =
      IWX_CSR_HW_IF_CONFIG_REG_MSK_MAC_DASH | IWX_CSR_HW_IF_CONFIG_REG_MSK_MAC_STEP |
      IWX_CSR_HW_IF_CONFIG_REG_MSK_PHY_STEP | IWX_CSR_HW_IF_CONFIG_REG_MSK_PHY_DASH |
      IWX_CSR_HW_IF_CONFIG_REG_MSK_PHY_TYPE | IWX_CSR_HW_IF_CONFIG_REG_BIT_RADIO_SI |
      IWX_CSR_HW_IF_CONFIG_REG_BIT_MAC_SI;
  wr(IWX_CSR_HW_IF_CONFIG_REG, (rd(IWX_CSR_HW_IF_CONFIG_REG) & ~mask) | reg);
}

void IwxDevice::set_ltr() {
  const uint32_t ltr =
      IWX_CSR_LTR_LONG_VAL_AD_NO_SNOOP_REQ |
      (IWX_CSR_LTR_LONG_VAL_AD_SCALE_USEC << 26) /* NO_SNOOP_SCALE */ |
      (250u << 16) /* NO_SNOOP_VAL */ | IWX_CSR_LTR_LONG_VAL_AD_SNOOP_REQ |
      (IWX_CSR_LTR_LONG_VAL_AD_SCALE_USEC << 10) /* SNOOP_SCALE */ | 250u;
  if (_integrated) {
    if (nic_lock()) {
      write_prph(IWX_HPM_MAC_LTR_CSR, IWX_HPM_MAC_LRT_ENABLE_ALL);
      write_prph(IWX_HPM_UMAC_LTR, ltr);
      nic_unlock();
    }
  } else {
    wr(IWX_CSR_LTR_LONG_VAL_AD, ltr);
  }
}

/* iwx_start_fw + iwx_ctxt_info_init + iwx_load_firmware, waiting for ALIVE
 * by polling instead of sleeping on the interrupt. */
bool IwxDevice::load_firmware(std::string &error) {
  if (!prepare_card_hw()) {
    error = "card not ready before firmware load";
    return false;
  }
  wr(IWX_CSR_INT, 0xffffffffu);
  mask_interrupts();
  wr(IWX_CSR_UCODE_DRV_GP1_CLR, IWX_CSR_UCODE_SW_BIT_RFKILL);
  wr(IWX_CSR_UCODE_DRV_GP1_CLR, IWX_CSR_UCODE_DRV_GP1_BIT_CMD_BLOCKED);
  wr(IWX_CSR_INT, ~0u);

  /* iwx_nic_init */
  if (!apm_init()) {
    error = "APM init failed";
    return false;
  }
  nic_config();
  _proxy.write8(IWX_CSR_INT_COALESCING, IWX_HOST_INT_TIMEOUT_DEF);
  setb(IWX_CSR_MAC_SHADOW_REG_CTRL, 0x800fffff);

  /* Firmware sections into DMA memory: LMAC, separator, UMAC, separator,
   * paging (iwx_init_fw_sec). */
  auto *ci = reinterpret_cast<iwx_context_info *>(_ctxt_info.va);
  std::memset(ci, 0, sizeof(*ci));
  const auto &secs = _fw.regular;
  const auto count_until_sep = [&](size_t start) {
    size_t n = 0;
    while (start < secs.size() && secs[start].devoff != IWX_CPU1_CPU2_SEPARATOR_SECTION &&
           secs[start].devoff != IWX_PAGING_SEPARATOR_SECTION) {
      start++;
      n++;
    }
    return n;
  };
  const size_t lmac = count_until_sep(0);
  const size_t umac = count_until_sep(lmac + 1);
  const size_t paging = count_until_sep(lmac + umac + 2);
  if (lmac > nitems(ci->dram.lmac_img) || umac > nitems(ci->dram.umac_img) ||
      paging > nitems(ci->dram.virtual_img)) {
    error = "firmware has more sections than the context info holds";
    return false;
  }
  const auto place = [&](const FwSection &sec, uint64_t &slot) {
    DmaBuf b = _dma.alloc(sec.len, 4096);
    if (!b) return false;
    std::memcpy(b.va, sec.data, sec.len);
    slot = htole64(b.pa);
    _fw_sections.push_back(b);
    return true;
  };
  for (size_t i = 0; i < lmac; i++)
    if (!place(secs[i], ci->dram.lmac_img[i])) goto nomem;
  for (size_t i = 0; i < umac; i++)
    if (!place(secs[lmac + 1 + i], ci->dram.umac_img[i])) goto nomem;
  for (size_t i = 0; i < paging; i++)
    if (!place(secs[lmac + umac + 2 + i], ci->dram.virtual_img[i])) goto nomem;
  _dma.trim();
  _log("DMA: " + std::to_string(_dma.pages_used()) + " pages in use, the rest returned");
  _log("firmware " + _fw.version + ": " + std::to_string(lmac) + " LMAC, " +
       std::to_string(umac) + " UMAC, " + std::to_string(paging) + " paging sections");

  ci->version.version = 0;
  ci->version.mac_id = htole16(static_cast<uint16_t>(rd(IWX_CSR_HW_REV)));
  ci->version.size = htole16(sizeof(*ci) / 4);
  ci->control.control_flags =
      htole32(IWX_CTXT_INFO_TFD_FORMAT_LONG |
              (IWX_RX_QUEUE_CB_SIZE(IWX_MQ_RX_TABLE_SIZE) << IWX_CTXT_INFO_RB_CB_SIZE_POS) |
              (IWX_CTXT_INFO_RB_SIZE_4K << IWX_CTXT_INFO_RB_SIZE_POS));
  ci->rbd_cfg.free_rbd_addr = htole64(_rx_free.pa);
  ci->rbd_cfg.used_rbd_addr = htole64(_rx_used.pa);
  ci->rbd_cfg.status_wr_ptr = htole64(_rx_stat.pa);
  ci->hcmd_cfg.cmd_queue_addr = htole64(_cmdq.desc.pa);
  ci->hcmd_cfg.cmd_queue_size = IWX_TFD_QUEUE_CB_SIZE(IWX_CMD_QUEUE_SIZE);

  /* From here the card may write into our memory: arm the stop the proxy
   * replays if this process dies (iwx_disable_rx_dma + stop master). */
  {
    std::vector<RegOp> close_ops{
        {IWX_CSR_RESET, IWX_CSR_RESET_REG_FLAG_STOP_MASTER, 2},
        {IWX_CSR_RESET, IWX_CSR_RESET_REG_FLAG_SW_RESET, 2},
    };
    if (!_proxy.set_close_ops(close_ops)) {
      error = "could not arm the DMA stop the driver replays on exit; refusing to start DMA";
      return false;
    }
  }

  _alive_seen = _alive_ok = false;
  wr(IWX_CSR_CTXT_INFO_BA, static_cast<uint32_t>(_ctxt_info.pa));
  wr(IWX_CSR_CTXT_INFO_BA + 4, static_cast<uint32_t>(_ctxt_info.pa >> 32));
  set_ltr();
  if (!nic_lock()) {
    error = "MAC access failed before the firmware kick";
    return false;
  }
  write_prph(IWX_UREG_CPU_INIT_RUN, 1);
  nic_unlock();

  /* The ALIVE cause latches in CSR_INT; the firmware has then set up the
   * receive engine, so hand it the free buffers (iwx_intr's ALIVE branch). */
  {
    const uint64_t end = now_us() + 1000000;
    bool alive_cause = false;
    while (now_us() < end) {
      const uint32_t cause = rd(IWX_CSR_INT);
      if (cause != 0xffffffffu && (cause & IWX_CSR_INT_BIT_ALIVE)) {
        wr(IWX_CSR_INT, cause);
        alive_cause = true;
        break;
      }
      if (cause != 0xffffffffu && (cause & (IWX_CSR_INT_BIT_SW_ERR | IWX_CSR_INT_BIT_HW_ERR))) {
        wr(IWX_CSR_INT, cause);
        break;
      }
      Sleep(1);
    }
    if (!alive_cause) {
      if (nic_lock()) {
        _log("SecBoot CPU1 " + hex(read_prph(IWX_UMAG_SB_CPU_1_STATUS)) + " CPU2 " +
             hex(read_prph(IWX_UMAG_SB_CPU_2_STATUS)) + " UMAC PC " +
             hex(read_prph(IWX_UREG_UMAC_CURRENT_PC)) + " LMAC PC " +
             hex(read_prph(IWX_UREG_LMAC1_CURRENT_PC)));
        nic_unlock();
      }
      error = "the firmware did not come alive";
      return false;
    }
    auto *free_list = reinterpret_cast<uint64_t *>(_rx_free.va);
    for (unsigned i = 0; i < IWX_RX_MQ_RING_COUNT; i++)
      free_list[i] = htole64(_rx_bufs[i].pa | (i & 0x0fff));
    wr(IWX_RFH_Q0_FRBDCB_WIDX_TRG, 8);
  }
  if (!wait_for([&] { return _alive_seen; }, 1000)) {
    error = "ALIVE interrupt seen but no ALIVE notification arrived";
    return false;
  }
  if (!_alive_ok) {
    error = "the firmware reported a failed ALIVE";
    return false;
  }
  _log("firmware alive");
  return true;

nomem:
  error = "out of DMA memory for the firmware image";
  return false;
}

/* iwx_run_init_mvm_ucode(readnvm = 1) followed by the parts of iwx_init_hw
 * a monitor needs. */
bool IwxDevice::run_init_sequence(std::string &error) {
  iwx_init_extended_cfg_cmd init_cfg{};
  init_cfg.init_flags = htole32(IWX_INIT_NVM);
  _init_complete = false;
  if (send_cmd(IWX_WIDE_ID(IWX_SYSTEM_GROUP, IWX_INIT_EXTENDED_CFG_CMD), &init_cfg,
               sizeof(init_cfg), false, nullptr)) {
    error = "INIT_EXTENDED_CFG failed";
    return false;
  }
  iwx_nvm_access_complete_cmd nvm_complete{};
  if (send_cmd(IWX_WIDE_ID(IWX_REGULATORY_AND_NVM_GROUP, IWX_NVM_ACCESS_COMPLETE),
               &nvm_complete, sizeof(nvm_complete), false, nullptr)) {
    error = "NVM_ACCESS_COMPLETE failed";
    return false;
  }
  if (!wait_for([&] { return _init_complete; }, 2000)) {
    error = "no INIT_COMPLETE from the firmware";
    return false;
  }
  if (int err = nvm_get()) {
    error = "reading the NVM failed (" + std::to_string(err) + ")";
    return false;
  }

  if (!nic_lock()) {
    error = "MAC access failed after init";
    return false;
  }
  int err = 0;
  {
    iwx_tx_ant_cfg_cmd ant{};
    ant.valid = htole32(valid_tx_ant());
    err = send_cmd(IWX_TX_ANT_CONFIGURATION_CMD, &ant, sizeof(ant), false, nullptr);
  }
  if (!err) {
    /* WiFi-only arbitration with Bluetooth, as itlwm does by default. */
    iwx_bt_coex_cmd bt{};
    bt.mode = htole32(IWX_BT_COEX_WIFI);
    err = send_cmd(IWX_BT_CONFIG, &bt, sizeof(bt), false, nullptr);
  }
  if (!err) {
    /* iwl_qu_long_latency_trans_cfg: integrated, xtal latency 12000,
     * low-latency xtal, LTR apply delay 2500. */
    iwx_soc_configuration_cmd soc{};
    uint32_t flags = IWX_SOC_FLAGS_LTR_APPLY_DELAY_2500 & IWX_SOC_FLAGS_LTR_APPLY_DELAY_MASK;
    const uint8_t scan_ver = cmd_version(IWX_LONG_GROUP, IWX_SCAN_REQ_UMAC);
    if (scan_ver != IWX_FW_CMD_VER_UNKNOWN && scan_ver >= 2)
      flags |= IWX_SOC_CONFIG_CMD_FLAGS_LOW_LATENCY;
    soc.flags = htole32(flags);
    soc.latency = htole32(12000);
    err = send_cmd(iwx_cmd_id(IWX_SOC_CONFIGURATION_CMD, IWX_SYSTEM_GROUP, 0), &soc,
                   sizeof(soc), false, nullptr);
  }
  if (!err && _fw.has_capa(IWX_UCODE_TLV_CAPA_DQA_SUPPORT)) {
    iwx_dqa_enable_cmd dqa{};
    dqa.cmd_queue = htole32(IWX_DQA_CMD_QUEUE);
    err = send_cmd(iwx_cmd_id(IWX_DQA_ENABLE_CMD, IWX_DATA_PATH_GROUP, 0), &dqa, sizeof(dqa),
                   false, nullptr);
  }
  if (!err) {
    /* Monitor: no power save (iwx_power_update_device). */
    iwx_device_power_cmd power{};
    err = send_cmd(IWX_POWER_TABLE_CMD, &power, sizeof(power), false, nullptr);
  }
  if (!err && _lar_enabled) err = update_mcc("ZZ", -1, nullptr);
  nic_unlock();
  if (!err && _opt.country != "ZZ" && !set_country(_opt.country))
    _log("the firmware did not take country " + _opt.country);
  if (err) {
    error = "firmware init commands failed (" + std::to_string(err) + ")";
    return false;
  }
  return true;
}

bool IwxDevice::open(const Options &options, std::string &error) {
  _opt = options;
  if (!_proxy.open(error)) return false;
  if (!identify(error)) return false;
  std::string path = _opt.firmware_dir;
  if (!path.empty() && path.back() != '\\' && path.back() != '/') path += '\\';
  if (!_fw.load(path + _fw_name, error)) return false;
  if (!alloc_rings(error)) return false;
  if (!start_hw()) {
    error = "hardware start failed";
    return false;
  }
  if (!load_firmware(error)) {
    dump_error_tables();
    stop();
    return false;
  }
  _open = true;
  if (!run_init_sequence(error)) {
    dump_error_tables();
    stop();
    return false;
  }
  return true;
}

void IwxDevice::stop() {
  if (!_proxy.is_open()) return;
  if (_open || !_fw_sections.empty()) {
    /* iwx_stop_device: stop receive DMA, then the bus master, then reset. */
    if (nic_lock()) {
      write_prph(IWX_RFH_RXF_DMA_CFG, 0);
      for (int i = 0; i < 100; i++) {
        const uint32_t v = read_prph(IWX_RFH_GEN_STATUS);
        if (v == 0xffffffffu || (v & IWX_RXF_DMA_IDLE)) break;
        delay_us(10);
      }
      nic_unlock();
    }
    clrb(IWX_CSR_GP_CNTRL, IWX_CSR_GP_CNTRL_REG_FLAG_MAC_ACCESS_REQ);
    _nic_locks = 0;
    apm_stop();
    setb(IWX_CSR_RESET, IWX_CSR_RESET_REG_FLAG_SW_RESET);
    delay_us(5000);
    mask_interrupts();
    prepare_card_hw();
  }
  _proxy.set_close_ops({});
  _open = false;
  _phy_added = _mac_added = _binding_added = _sta_added = _queue_enabled = false;
}

/* ---------------------------------------------------------------- commands */

bool IwxDevice::wait_for(const std::function<bool()> &done, unsigned timeout_ms) {
  const uint64_t end = now_us() + uint64_t(timeout_ms) * 1000;
  while (!done()) {
    if (_fw_error || _proxy.failed() || now_us() >= end) return done();
    if (poll() == 0) delay_us(100);
  }
  return true;
}

/* iwx_send_cmd, synchronous only. Returns 0, or an errno-style code. */
int IwxDevice::send_cmd(uint32_t id, const void *data, size_t len, bool want_resp,
                        std::vector<uint8_t> *resp, unsigned timeout_ms) {
  if (_fw_error || _proxy.failed()) return ENXIO;
  TxRing &ring = _cmdq;
  const unsigned idx = ring.cur & (ring.count - 1);
  CmdSlot &slot = _cmd_slots[idx];
  if (slot.pending || ring.queued >= ring.count - 1) return ENOSPC;
  if (len > IWX_MAX_CMD_PAYLOAD_SIZE) return EINVAL;

  uint32_t code = id;
  slot.narrow = iwx_cmd_groupid(code) == 0;
  if (slot.narrow) code = IWX_WIDE_ID(IWX_LONG_GROUP, code);

  iwx_device_cmd *cmd;
  uint64_t paddr;
  if (len > kCmdPayloadInline) {
    cmd = reinterpret_cast<iwx_device_cmd *>(_cmd_big[idx].va);
    paddr = _cmd_big[idx].pa;
  } else {
    cmd = reinterpret_cast<iwx_device_cmd *>(ring.cmd.va) + idx;
    paddr = ring.cmd.pa + uint64_t(idx) * sizeof(iwx_device_cmd);
  }
  const size_t hdrlen = sizeof(cmd->hdr_wide);
  cmd->hdr_wide.cmd = iwx_cmd_opcode(code);
  cmd->hdr_wide.group_id = iwx_cmd_groupid(code);
  cmd->hdr_wide.qid = static_cast<uint8_t>(ring.qid);
  cmd->hdr_wide.idx = static_cast<uint8_t>(idx);
  cmd->hdr_wide.length = htole16(static_cast<uint16_t>(len));
  cmd->hdr_wide.reserved = 0;
  cmd->hdr_wide.version = iwx_cmd_version(code);
  if (len) std::memcpy(cmd->data_wide, data, len);

  auto *desc = reinterpret_cast<iwx_tfh_tfd *>(ring.desc.va) + idx;
  std::memset(desc, 0, sizeof(*desc));
  const size_t total = hdrlen + len;
  desc->tbs[0].tb_len = htole16(static_cast<uint16_t>(std::min<size_t>(total, IWX_FIRST_TB_SIZE)));
  uint64_t a = htole64(paddr);
  std::memcpy(&desc->tbs[0].addr, &a, sizeof(a));
  if (total > IWX_FIRST_TB_SIZE) {
    desc->tbs[1].tb_len = htole16(static_cast<uint16_t>(total - IWX_FIRST_TB_SIZE));
    a = htole64(paddr + IWX_FIRST_TB_SIZE);
    std::memcpy(&desc->tbs[1].addr, &a, sizeof(a));
    desc->num_tbs = htole16(2);
  } else {
    desc->num_tbs = htole16(1);
  }
  slot.pending = true;
  slot.want_resp = want_resp;
  slot.resp.clear();
  ring.queued++;
  ring.cur = (ring.cur + 1) % IWX_TFD_QUEUE_SIZE_MAX; /* the pointer wraps at 256, not at the ring */
  std::atomic_thread_fence(std::memory_order_seq_cst);
  wr(IWX_HBUS_TARG_WRPTR, (ring.qid << 16) | ring.cur);

  if (!wait_for([&] { return !slot.pending; }, timeout_ms)) {
    _log("firmware command " + hex(id) + " timed out");
    slot.pending = false;
    return _fw_error ? EIO : ETIMEDOUT;
  }
  if (want_resp) {
    if (slot.resp.empty()) return EIO; /* failed, or the response was too long */
    if (resp) *resp = std::move(slot.resp);
  }
  return 0;
}

int IwxDevice::send_cmd_status(uint32_t id, const void *data, size_t len, uint32_t *status) {
  std::vector<uint8_t> resp;
  const int err = send_cmd(id, data, len, true, &resp);
  if (err) return err;
  const auto *pkt = reinterpret_cast<const iwx_rx_packet *>(resp.data());
  if (resp.size() < sizeof(*pkt) + sizeof(iwx_cmd_response) ||
      (pkt->hdr.group_id & IWX_CMD_FAILED_MSK) ||
      iwx_rx_packet_payload_len(pkt) != sizeof(iwx_cmd_response))
    return EIO;
  *status = le32toh(reinterpret_cast<const iwx_cmd_response *>(pkt->data)->status);
  return 0;
}

uint8_t IwxDevice::valid_tx_ant() const {
  uint8_t ant = (_fw.phy_config & IWX_FW_PHY_CFG_TX_CHAIN) >> IWX_FW_PHY_CFG_TX_CHAIN_POS;
  if (_nvm_tx_ant) ant &= _nvm_tx_ant;
  return ant;
}

uint8_t IwxDevice::valid_rx_ant() const {
  uint8_t ant = (_fw.phy_config & IWX_FW_PHY_CFG_RX_CHAIN) >> IWX_FW_PHY_CFG_RX_CHAIN_POS;
  if (_nvm_rx_ant) ant &= _nvm_rx_ant;
  return ant;
}

int IwxDevice::nvm_get() {
  iwx_nvm_get_info cmd{};
  std::vector<uint8_t> resp;
  const bool v4 = _fw.has_api(IWX_UCODE_TLV_API_REGULATORY_NVM_INFO);
  const int err = send_cmd(IWX_WIDE_ID(IWX_REGULATORY_AND_NVM_GROUP, IWX_NVM_GET_INFO), &cmd,
                           sizeof(cmd), true, &resp);
  if (err) return err;
  const auto *pkt = reinterpret_cast<const iwx_rx_packet *>(resp.data());
  const size_t want = v4 ? sizeof(iwx_nvm_get_info_rsp) : sizeof(iwx_nvm_get_info_rsp_v3);
  if (iwx_rx_packet_payload_len(pkt) != want) return EIO;
  const auto *rsp = reinterpret_cast<const iwx_nvm_get_info_rsp *>(pkt->data);
  _nvm_tx_ant = static_cast<uint8_t>(le32toh(rsp->phy_sku.tx_chains));
  _nvm_rx_ant = static_cast<uint8_t>(le32toh(rsp->phy_sku.rx_chains));
  _lar_enabled = le32toh(rsp->regulatory.lar_enabled) && _fw.has_capa(IWX_UCODE_TLV_CAPA_LAR_SUPPORT);

  /* The MAC address strapped by the platform (iwx_set_mac_addr_from_csr). */
  const uint32_t a0 = rd(IWX_CSR_MAC_ADDR0_STRAP), a1 = rd(IWX_CSR_MAC_ADDR1_STRAP);
  _hw_addr = {uint8_t(a0 >> 24), uint8_t(a0 >> 16), uint8_t(a0 >> 8), uint8_t(a0),
              uint8_t(a1 >> 24), uint8_t(a1 >> 16)};
  if ((_hw_addr[0] & 1) || std::all_of(_hw_addr.begin(), _hw_addr.end(), [](uint8_t b) { return b == 0; })) {
    const uint32_t b0 = rd(IWX_CSR_MAC_ADDR0_OTP), b1 = rd(IWX_CSR_MAC_ADDR1_OTP);
    _hw_addr = {uint8_t(b0 >> 24), uint8_t(b0 >> 16), uint8_t(b0 >> 8), uint8_t(b0),
                uint8_t(b1 >> 24), uint8_t(b1 >> 16)};
  }
  char line[160];
  std::snprintf(line, sizeof(line),
                "NVM: tx antennas 0x%x, rx antennas 0x%x, LAR %s, address %02x:%02x:%02x:%02x:%02x:%02x",
                valid_tx_ant(), valid_rx_ant(), _lar_enabled ? "on" : "off", _hw_addr[0],
                _hw_addr[1], _hw_addr[2], _hw_addr[3], _hw_addr[4], _hw_addr[5]);
  _log(line);
  return 0;
}

int IwxDevice::update_mcc(const std::string &alpha2, int source, std::string *got) {
  iwx_mcc_update_cmd cmd{};
  const char c0 = alpha2.size() > 0 ? alpha2[0] : 'Z', c1 = alpha2.size() > 1 ? alpha2[1] : 'Z';
  cmd.mcc = htole16(static_cast<uint16_t>(c0 << 8 | c1));
  if (source >= 0)
    cmd.source_id = static_cast<uint8_t>(source);
  else if (_fw.has_api(IWX_UCODE_TLV_API_WIFI_MCC_UPDATE) || _fw.has_capa(IWX_UCODE_TLV_CAPA_LAR_MULTI_MCC))
    cmd.source_id = (c0 == 'Z' && c1 == 'Z') ? IWX_MCC_SOURCE_GET_CURRENT : IWX_MCC_SOURCE_ME;
  else
    cmd.source_id = IWX_MCC_SOURCE_OLD_FW;
  std::vector<uint8_t> resp;
  const int err = send_cmd(IWX_MCC_UPDATE_CMD, &cmd, sizeof(cmd), true, &resp);
  if (err) return err;
  const auto *pkt = reinterpret_cast<const iwx_rx_packet *>(resp.data());
  if (pkt->hdr.group_id & IWX_CMD_FAILED_MSK) return EIO;
  const size_t plen = iwx_rx_packet_payload_len(pkt);
  if (plen < sizeof(iwx_mcc_update_resp)) return EIO;
  const auto *r = reinterpret_cast<const iwx_mcc_update_resp *>(pkt->data);
  const uint32_t n = le32toh(r->n_channels);
  if (plen != sizeof(*r) + n * sizeof(uint32_t)) return EIO;
  const uint16_t mcc = le16toh(r->mcc);
  const std::string domain{char(mcc >> 8), char(mcc & 0xff)};
  if (got) *got = domain;
  std::string summary = "regulatory: asked " + alpha2 + " (source " + std::to_string(cmd.source_id) +
                        "), firmware says " + domain + " (source " + std::to_string(r->source_id) +
                        ", status " + std::to_string(le32toh(r->status)) + "):";
  for (uint32_t i = 0; i < n && i < nitems(kNvmChannels); i++) {
    const uint8_t ch = kNvmChannels[i];
    if (ch != 6 && ch != 36 && ch != 44 && ch != 149) continue;
    const uint32_t flags = le32toh(r->channels[i]);
    _chan_flags[ch] = flags;
    summary += " ch" + std::to_string(ch) + "=" +
               (!(flags & IWX_NVM_CHANNEL_VALID) ? "off"
                : (flags & IWX_NVM_CHANNEL_ACTIVE) ? "active"
                                                    : "passive") +
               ((flags & IWX_NVM_CHANNEL_80MHZ) ? "/80" : "");
  }
  _log(summary);
  return 0;
}

bool IwxDevice::set_promiscuous(bool on) {
  if (on == _promisc) return true;
  _promisc = on;
  if (!_mac_added) return true;
  if (!nic_lock()) return false;
  const int err = mac_ctxt_cmd(IWX_FW_CTXT_ACTION_MODIFY);
  nic_unlock();
  if (err) _log("promiscuous change failed (" + std::to_string(err) + ")");
  return err == 0;
}

bool IwxDevice::add_rx_ba(const uint8_t peer[6], unsigned tid, uint16_t ssn, uint16_t window,
                          uint8_t token) {
  if (tid >= 8 || !_mac_added) return false;
  if (!nic_lock()) return false;
  constexpr uint8_t kBaStation = 3; /* ForbiddenItlwm's kAwdlBaStationId */
  int err = 0;
  uint32_t status = 0;
  if (_ba_station && std::memcmp(_ba_peer, peer, 6) != 0) {
    /* A new peer: the old station and its sessions go. */
    iwx_rm_sta_cmd rm{};
    rm.sta_id = kBaStation;
    send_cmd(IWX_REMOVE_STA, &rm, sizeof(rm), false, nullptr);
    _ba_station = false;
    _ba_tids = 0;
  }
  if (!_ba_station) {
    iwx_add_sta_cmd cmd{};
    cmd.sta_id = kBaStation;
    cmd.station_type = IWX_STA_LINK;
    cmd.mac_id_n_color = htole32(IWX_FW_CMD_ID_AND_COLOR(0, 0));
    std::memcpy(cmd.addr, peer, 6);
    cmd.tid_disable_tx = htole16(0xffff);
    cmd.station_flags = htole32((_width >= 80 && _channel > 14 ? IWX_STA_FLG_FAT_EN_80MHZ
                                                               : IWX_STA_FLG_FAT_EN_20MHZ) |
                                IWX_STA_FLG_MIMO_EN_MIMO2);
    cmd.station_flags_msk = htole32(IWX_STA_FLG_FAT_EN_MSK | IWX_STA_FLG_MIMO_EN_MSK);
    status = IWX_ADD_STA_SUCCESS;
    err = send_cmd_status(IWX_ADD_STA, &cmd, sizeof(cmd), &status);
    if (err || (status & IWX_ADD_STA_STATUS_MASK) != IWX_ADD_STA_SUCCESS) {
      nic_unlock();
      _log("Block Ack: peer station refused (err " + std::to_string(err) + ", status " +
           hex(status) + ")");
      return false;
    }
    _ba_station = true;
    std::memcpy(_ba_peer, peer, 6);
  }
  /* A new token is a new session with its own start: drop the old one first. */
  if (((_ba_tids >> tid) & 1) && _ba_token[tid] != token) {
    iwx_add_sta_cmd cmd{};
    cmd.sta_id = kBaStation;
    cmd.mac_id_n_color = htole32(IWX_FW_CMD_ID_AND_COLOR(0, 0));
    cmd.add_modify = IWX_STA_MODE_MODIFY;
    cmd.remove_immediate_ba_tid = static_cast<uint8_t>(tid);
    cmd.modify_mask = IWX_STA_MODIFY_REMOVE_BA_TID;
    status = IWX_ADD_STA_SUCCESS;
    send_cmd_status(IWX_ADD_STA, &cmd, sizeof(cmd), &status);
    _ba_tids &= static_cast<uint8_t>(~(1u << tid));
  }
  if (!((_ba_tids >> tid) & 1)) {
    iwx_add_sta_cmd cmd{};
    cmd.sta_id = kBaStation;
    cmd.mac_id_n_color = htole32(IWX_FW_CMD_ID_AND_COLOR(0, 0));
    cmd.add_modify = IWX_STA_MODE_MODIFY;
    cmd.add_immediate_ba_tid = static_cast<uint8_t>(tid);
    cmd.add_immediate_ba_ssn = htole16(ssn);
    cmd.rx_ba_window = htole16(window);
    cmd.modify_mask = IWX_STA_MODIFY_ADD_BA_TID;
    status = IWX_ADD_STA_SUCCESS;
    err = send_cmd_status(IWX_ADD_STA, &cmd, sizeof(cmd), &status);
    if (err || (status & IWX_ADD_STA_STATUS_MASK) != IWX_ADD_STA_SUCCESS ||
        !(status & IWX_ADD_STA_BAID_VALID_MASK)) {
      nic_unlock();
      _log("Block Ack: session refused (err " + std::to_string(err) + ", status " + hex(status) + ")");
      return false;
    }
    _ba_tids |= static_cast<uint8_t>(1u << tid);
    _ba_token[tid] = token;
    _log("Block Ack: receive session open, tid " + std::to_string(tid) + ", window " +
         std::to_string(window));
  }
  nic_unlock();
  return true;
}

bool IwxDevice::remove_rx_ba(unsigned tid) {
  if (tid >= 8 || !((_ba_tids >> tid) & 1)) return true;
  if (!nic_lock()) return false;
  iwx_add_sta_cmd cmd{};
  cmd.sta_id = 3;
  cmd.mac_id_n_color = htole32(IWX_FW_CMD_ID_AND_COLOR(0, 0));
  cmd.add_modify = IWX_STA_MODE_MODIFY;
  cmd.remove_immediate_ba_tid = static_cast<uint8_t>(tid);
  cmd.modify_mask = IWX_STA_MODIFY_REMOVE_BA_TID;
  uint32_t status = IWX_ADD_STA_SUCCESS;
  const int err = send_cmd_status(IWX_ADD_STA, &cmd, sizeof(cmd), &status);
  nic_unlock();
  _ba_tids &= static_cast<uint8_t>(~(1u << tid));
  return err == 0;
}

bool IwxDevice::set_country(const std::string &alpha2) {
  if (!_lar_enabled || alpha2.size() != 2) return false;
  if (!nic_lock()) return false;
  /* As ForbiddenItlwm answers a country learned from Wi-Fi: with the Wi-Fi
   * source. Other sources are tried after it. */
  bool ok = false;
  /* Measured on an AX201 (firmware 68): only the BIOS source is taken; the
   * Wi-Fi, ME and MCC-API sources answer status 4 and change nothing. */
  for (int source : {int(IWX_MCC_SOURCE_BIOS), int(IWX_MCC_SOURCE_WIFI)}) {
    std::string got;
    if (update_mcc(alpha2, source, &got) == 0 && got == alpha2) {
      ok = true;
      break;
    }
  }
  nic_unlock();
  return ok;
}

int IwxDevice::phy_ctxt_cmd(uint32_t action) { return phy_cmd(0, _channel, _width, action); }

int IwxDevice::phy_cmd(unsigned phy_id, uint8_t channel, unsigned width_mhz, uint32_t action) {
  const bool is_2ghz = channel <= 14;
  const uint8_t width = (width_mhz >= 80 && !is_2ghz) ? IWX_PHY_VHT_CHANNEL_MODE80
                        : (width_mhz >= 40)            ? IWX_PHY_VHT_CHANNEL_MODE40
                                                    : IWX_PHY_VHT_CHANNEL_MODE20;
  /* The control channel's place inside the 40/80 MHz block, as Apple
   * devices use 149-161 and 36-48 (ForbiddenItlwm's iwx_get_ctrl_pos). */
  uint8_t pos = IWX_PHY_VHT_CTRL_POS_1_BELOW;
  if (width == IWX_PHY_VHT_CHANNEL_MODE80) {
    switch (channel) {
    case 36: case 149: pos = IWX_PHY_VHT_CTRL_POS_2_BELOW; break;
    case 40: case 153: pos = IWX_PHY_VHT_CTRL_POS_1_BELOW; break;
    case 44: case 157: pos = IWX_PHY_VHT_CTRL_POS_1_ABOVE; break;
    case 48: case 161: pos = IWX_PHY_VHT_CTRL_POS_2_ABOVE; break;
    default: pos = IWX_PHY_VHT_CTRL_POS_1_BELOW; break;
    }
  } else if (width == IWX_PHY_VHT_CHANNEL_MODE40) {
    pos = (channel % 8 == 4 || channel == 149 || channel == 157 || channel <= 7)
              ? IWX_PHY_VHT_CTRL_POS_1_BELOW
              : IWX_PHY_VHT_CTRL_POS_1_ABOVE;
  }
  const uint8_t rx_ant = valid_rx_ant();
  const uint8_t chains = (rx_ant & (rx_ant - 1)) ? 2 : 1;
  const uint32_t rxchain = (uint32_t(rx_ant) << IWX_PHY_RX_CHAIN_VALID_POS) |
                           (uint32_t(chains) << IWX_PHY_RX_CHAIN_CNT_POS) |
                           (uint32_t(chains) << IWX_PHY_RX_CHAIN_MIMO_CNT_POS);
  const uint32_t id = IWX_FW_CMD_ID_AND_COLOR(phy_id, 0);
  const uint32_t lmac = (is_2ghz || !_fw.has_capa(IWX_UCODE_TLV_CAPA_CDB_SUPPORT))
                            ? IWX_LMAC_24G_INDEX
                            : IWX_LMAC_5G_INDEX;
  if (cmd_version(IWX_LONG_GROUP, IWX_PHY_CONTEXT_CMD) == 3) {
    iwx_phy_context_cmd cmd{};
    cmd.id_and_color = htole32(id);
    cmd.action = htole32(action);
    cmd.lmac_id = htole32(lmac);
    cmd.ci.band = is_2ghz ? IWX_PHY_BAND_24 : IWX_PHY_BAND_5;
    cmd.ci.channel = htole32(channel);
    cmd.ci.width = width;
    cmd.ci.ctrl_pos = pos;
    cmd.rxchain_info = htole32(rxchain);
    return send_cmd(IWX_PHY_CONTEXT_CMD, &cmd, sizeof(cmd), false, nullptr);
  }
  if (_fw.has_capa(IWX_UCODE_TLV_CAPA_ULTRA_HB_CHANNELS)) {
    iwx_phy_context_cmd_uhb cmd{};
    cmd.id_and_color = htole32(id);
    cmd.action = htole32(action);
    cmd.ci.band = is_2ghz ? IWX_PHY_BAND_24 : IWX_PHY_BAND_5;
    cmd.ci.channel = htole32(channel);
    cmd.ci.width = width;
    cmd.ci.ctrl_pos = pos;
    cmd.rxchain_info = htole32(rxchain);
    cmd.txchain_info = htole32(valid_tx_ant());
    return send_cmd(IWX_PHY_CONTEXT_CMD, &cmd, sizeof(cmd), false, nullptr);
  }
  iwx_phy_context_cmd_v1 cmd{};
  cmd.id_and_color = htole32(id);
  cmd.action = htole32(action);
  cmd.ci.band = is_2ghz ? IWX_PHY_BAND_24 : IWX_PHY_BAND_5;
  cmd.ci.channel = channel;
  cmd.ci.width = width;
  cmd.ci.ctrl_pos = pos;
  cmd.rxchain_info = htole32(rxchain);
  cmd.txchain_info = htole32(valid_tx_ant());
  return send_cmd(IWX_PHY_CONTEXT_CMD, &cmd, sizeof(cmd), false, nullptr);
}

/* The AWDL listener: ForbiddenItlwm's monitor-mode MAC context. Its node
 * address is the AWDL address, which is what makes the firmware acknowledge
 * frames sent to us; the filters let everything else through as well. */
int IwxDevice::mac_ctxt_cmd(uint32_t action) {
  static const uint8_t kAwdlBssid[6] = {0x00, 0x25, 0x00, 0xff, 0x94, 0x73};
  static const uint8_t kBroadcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
  iwx_mac_ctx_cmd cmd{};
  cmd.id_and_color = htole32(IWX_FW_CMD_ID_AND_COLOR(0, 0));
  cmd.action = htole32(action);
  if (action == IWX_FW_CTXT_ACTION_REMOVE)
    return send_cmd(IWX_MAC_CONTEXT_CMD, &cmd, sizeof(cmd), false, nullptr);
  uint32_t type = IWX_FW_MAC_TYPE_LISTENER;
  if (_opt.mac_type == MacType::Ibss) type = IWX_FW_MAC_TYPE_IBSS;
  if (_opt.mac_type == MacType::Sta) type = IWX_FW_MAC_TYPE_BSS_STA;
  cmd.mac_type = htole32(type);
  cmd.tsf_id = htole32(IWX_TSF_ID_A);
  std::memcpy(cmd.node_addr, _address.data(), 6);
  std::memcpy(cmd.bssid_addr, _opt.mac_type == MacType::Listener ? kBroadcast : kAwdlBssid, 6);
  if (_opt.mac_type != MacType::Listener) {
    /* ACK rates: the OFDM basics on 5 GHz, CCK and OFDM on 2.4 GHz. */
    cmd.cck_rates = htole32(_channel <= 14 ? 0x0f : 0);
    cmd.ofdm_rates = htole32(0x15); /* 6, 12, 24 */
    cmd.qos_flags = htole32(IWX_MAC_QOS_FLG_TGN);
  }
  cmd.filter_flags = htole32((_promisc ? IWX_MAC_FILTER_IN_PROMISC : 0u) |
                             IWX_MAC_FILTER_IN_CONTROL_AND_MGMT | IWX_MAC_FILTER_ACCEPT_GRP |
                             IWX_MAC_FILTER_IN_BEACON | IWX_MAC_FILTER_IN_PROBE_REQUEST |
                             IWX_MAC_FILTER_IN_CRC32);
  if (_opt.mac_type == MacType::Ibss) {
    cmd.ibss.bi = htole32(100);
    cmd.ibss.bi_reciprocal = htole32(iwx_reciprocal(100));
  } else if (_opt.mac_type == MacType::Sta) {
    cmd.sta.is_assoc = htole32(action != IWX_FW_CTXT_ACTION_ADD);
    cmd.sta.bi = htole32(100);
    cmd.sta.bi_reciprocal = htole32(iwx_reciprocal(100));
    cmd.sta.dtim_interval = htole32(100);
    cmd.sta.dtim_reciprocal = htole32(iwx_reciprocal(100));
    cmd.sta.listen_interval = htole32(10);
    cmd.sta.assoc_id = htole32(1);
  }
  return send_cmd(IWX_MAC_CONTEXT_CMD, &cmd, sizeof(cmd), false, nullptr);
}

int IwxDevice::binding_cmd(uint32_t action) {
  /* With the station on the listener's channel, both MACs share binding 0. */
  uint32_t macs[2] = {IWX_FW_CMD_ID_AND_COLOR(0, 0), IWX_FW_CMD_ID_AND_COLOR(1, 0)};
  const unsigned n = (_sta_mac_added && _sta_shared_binding) ? 2 : 1;
  return binding_for(0, 0, macs, n, _channel, action);
}

int IwxDevice::binding_for(unsigned id, unsigned phy, const uint32_t *macs, unsigned nmacs,
                           uint8_t channel, uint32_t action) {
  iwx_binding_cmd cmd{};
  cmd.id_and_color = htole32(IWX_FW_CMD_ID_AND_COLOR(id, 0));
  cmd.action = htole32(action);
  cmd.phy = htole32(IWX_FW_CMD_ID_AND_COLOR(phy, 0));
  for (unsigned i = 0; i < IWX_MAX_MACS_IN_BINDING; i++)
    cmd.macs[i] = htole32(i < nmacs ? macs[i] : IWX_FW_CTXT_INVALID);
  cmd.lmac_id = htole32((channel <= 14 || !_fw.has_capa(IWX_UCODE_TLV_CAPA_CDB_SUPPORT))
                            ? IWX_LMAC_24G_INDEX
                            : IWX_LMAC_5G_INDEX);
  uint32_t status = 0;
  const int err = send_cmd_status(IWX_BINDING_CONTEXT_CMD, &cmd, sizeof(cmd), &status);
  return err ? err : (status ? EIO : 0);
}

int IwxDevice::add_monitor_sta() {
  iwx_add_sta_cmd cmd{};
  cmd.sta_id = IWX_MONITOR_STA_ID;
  cmd.station_type = IWX_STA_GENERAL_PURPOSE;
  cmd.mac_id_n_color = htole32(IWX_FW_CMD_ID_AND_COLOR(0, 0));
  std::memset(cmd.addr, 0, sizeof(cmd.addr)); /* etheranyaddr */
  cmd.add_modify = 0;
  cmd.station_flags_msk = htole32(IWX_STA_FLG_FAT_EN_MSK | IWX_STA_FLG_MIMO_EN_MSK);
  cmd.tid_disable_tx = htole16(0xffff);
  uint32_t status = IWX_ADD_STA_SUCCESS;
  const int err = send_cmd_status(IWX_ADD_STA, &cmd, sizeof(cmd), &status);
  if (err) return err;
  return (status & IWX_ADD_STA_STATUS_MASK) == IWX_ADD_STA_SUCCESS ? 0 : EIO;
}

/* The firmware picks the queue number (TVQM); iwx_tvqm_enable_txq. */
int IwxDevice::enable_monitor_queue() { return enable_queue(_dataq, IWX_MONITOR_STA_ID, IWX_MGMT_TID); }

int IwxDevice::enable_queue(TxRing &ring, uint8_t sta_id, uint8_t tid) {
  iwx_tx_queue_cfg_cmd cmd{};
  cmd.sta_id = sta_id;
  cmd.tid = tid;
  cmd.flags = htole16(IWX_TX_QUEUE_CFG_ENABLE_QUEUE);
  cmd.cb_size = htole32(IWX_TFD_QUEUE_CB_SIZE(ring.count));
  cmd.byte_cnt_addr = htole64(ring.bc_tbl.pa);
  cmd.tfdq_addr = htole64(ring.desc.pa);
  std::vector<uint8_t> resp;
  const int err = send_cmd(IWX_SCD_QUEUE_CFG, &cmd, sizeof(cmd), true, &resp);
  if (err) return err;
  const auto *pkt = reinterpret_cast<const iwx_rx_packet *>(resp.data());
  if ((pkt->hdr.group_id & IWX_CMD_FAILED_MSK) ||
      iwx_rx_packet_payload_len(pkt) != sizeof(iwx_tx_queue_cfg_rsp))
    return EIO;
  const auto *r = reinterpret_cast<const iwx_tx_queue_cfg_rsp *>(pkt->data);
  ring.qid = le16toh(r->queue_number);
  ring.cur = ring.tail = le16toh(r->write_pointer) & (ring.count - 1);
  ring.queued = 0;
  _log("transmit queue " + std::to_string(ring.qid) + " assigned for station " +
       std::to_string(sta_id) + ", write pointer " + std::to_string(ring.cur));
  return 0;
}

bool IwxDevice::start_monitor(uint8_t channel, unsigned width_mhz, const uint8_t address[6]) {
  _channel = channel;
  _width = width_mhz;
  std::memcpy(_address.data(), address, 6);
  if (!nic_lock()) return false;
  int err = phy_ctxt_cmd(IWX_FW_CTXT_ACTION_ADD);
  if (err) _log("PHY context add failed (" + std::to_string(err) + ")");
  else _phy_added = true;
  if (!err) {
    err = mac_ctxt_cmd(IWX_FW_CTXT_ACTION_ADD);
    if (err) _log("MAC context add failed (" + std::to_string(err) + ")");
    else _mac_added = true;
  }
  if (!err) {
    err = binding_cmd(IWX_FW_CTXT_ACTION_ADD);
    if (err) _log("binding failed (" + std::to_string(err) + ")");
    else _binding_added = true;
  }
  if (!err) {
    err = add_monitor_sta();
    if (err) _log("monitor station add failed (" + std::to_string(err) + ")");
    else _sta_added = true;
  }
  if (!err) {
    err = enable_monitor_queue();
    if (err) _log("transmit queue setup failed (" + std::to_string(err) + ")");
    else _queue_enabled = true;
  }
  /* The rest of iwx_run for a monitor: the MAC context again, now "running",
   * smart FIFO fully on, every multicast let through, no MAC power save, and
   * the whole air time for this binding where the firmware wants quotas. */
  if (!err) {
    err = mac_ctxt_cmd(IWX_FW_CTXT_ACTION_MODIFY);
    if (err) _log("MAC context modify failed (" + std::to_string(err) + ")");
  }
  if (!err) {
    iwx_sf_cfg_cmd sf{};
    sf.state = htole32(IWX_SF_FULL_ON);
    sf.watermark[IWX_SF_LONG_DELAY_ON] = htole32(IWX_SF_W_MARK_SCAN);
    sf.watermark[IWX_SF_FULL_ON] = htole32(IWX_SF_W_MARK_LEGACY);
    for (unsigned i = 0; i < IWX_SF_NUM_SCENARIO; i++)
      for (unsigned j = 0; j < IWX_SF_NUM_TIMEOUT_TYPES; j++)
        sf.long_delay_timeouts[i][j] = htole32(IWX_SF_LONG_DELAY_AGING_TIMER);
    static const uint32_t full_on[IWX_SF_NUM_SCENARIO][IWX_SF_NUM_TIMEOUT_TYPES] = {
        {IWX_SF_SINGLE_UNICAST_AGING_TIMER, IWX_SF_SINGLE_UNICAST_IDLE_TIMER},
        {IWX_SF_AGG_UNICAST_AGING_TIMER, IWX_SF_AGG_UNICAST_IDLE_TIMER},
        {IWX_SF_MCAST_AGING_TIMER, IWX_SF_MCAST_IDLE_TIMER},
        {IWX_SF_BA_AGING_TIMER, IWX_SF_BA_IDLE_TIMER},
        {IWX_SF_TX_RE_AGING_TIMER, IWX_SF_TX_RE_IDLE_TIMER}};
    std::memcpy(sf.full_on_timeouts, full_on, sizeof(full_on));
    err = send_cmd(IWX_REPLY_SF_CFG_CMD, &sf, sizeof(sf), false, nullptr);
    if (err) _log("smart FIFO config failed (" + std::to_string(err) + ")");
  }
  if (!err) {
    /* A byte buffer: the struct ends in a zero-size array. */
    alignas(4) uint8_t buf[roundup(sizeof(iwx_mcast_filter_cmd), 4)] = {};
    auto *mc = reinterpret_cast<iwx_mcast_filter_cmd *>(buf);
    mc->filter_own = 1;
    mc->pass_all = 1;
    std::memset(mc->bssid, 0xff, sizeof(mc->bssid));
    err = send_cmd(IWX_MCAST_FILTER_CMD, buf, sizeof(buf), false, nullptr);
    if (err) _log("multicast filter failed (" + std::to_string(err) + ")");
  }
  if (!err) {
    iwx_mac_power_cmd pm{};
    pm.id_and_color = htole32(IWX_FW_CMD_ID_AND_COLOR(0, 0));
    pm.keep_alive_seconds = htole16(25);
    err = send_cmd(IWX_MAC_PM_POWER_TABLE, &pm, sizeof(pm), false, nullptr);
    if (err) _log("MAC power table failed (" + std::to_string(err) + ")");
  }
  if (!err && !_fw.has_capa(IWX_UCODE_TLV_CAPA_DYNAMIC_QUOTA)) {
    iwx_time_quota_cmd q{};
    for (unsigned i = 0; i < IWX_MAX_BINDINGS; i++)
      q.quotas[i].id_and_color = htole32(IWX_FW_CTXT_INVALID);
    q.quotas[0].id_and_color = htole32(IWX_FW_CMD_ID_AND_COLOR(0, 0));
    q.quotas[0].quota = htole32(IWX_MAX_QUOTA);
    err = send_cmd(IWX_TIME_QUOTA_CMD, &q, sizeof(q), false, nullptr);
    if (err) _log("time quota failed (" + std::to_string(err) + ")");
  }
  nic_unlock();
  if (!err)
    _log("listening on channel " + std::to_string(_channel) + " at " + std::to_string(_width) +
         " MHz");
  return err == 0;
}

bool IwxDevice::set_channel(uint8_t channel, unsigned width_mhz) {
  if (!_phy_added) return false;
  if (channel == _channel && width_mhz == _width) return true;
  const uint8_t old_channel = _channel;
  const unsigned old_width = _width;
  _channel = channel;
  _width = width_mhz;
  /* Changing band under a live binding is what itlwm refuses (it asserts the
   * firmware on CDB parts); a modify within one band is safe. */
  if ((old_channel <= 14) != (channel <= 14)) {
    _log("channel change across bands is not supported live");
    _channel = old_channel;
    _width = old_width;
    return false;
  }
  if (!nic_lock()) return false;
  const int err = phy_ctxt_cmd(IWX_FW_CTXT_ACTION_MODIFY);
  nic_unlock();
  if (err) {
    _log("channel change failed (" + std::to_string(err) + ")");
    _channel = old_channel;
    _width = old_width;
    return false;
  }
  _log("now on channel " + std::to_string(channel));
  return true;
}

bool IwxDevice::set_address(const uint8_t address[6]) {
  if (std::memcmp(address, _address.data(), 6) == 0) return true;
  std::memcpy(_address.data(), address, 6);
  if (!_mac_added) return true;
  if (!nic_lock()) return false;
  const int err = mac_ctxt_cmd(IWX_FW_CTXT_ACTION_MODIFY);
  nic_unlock();
  if (err) _log("address change failed (" + std::to_string(err) + ")");
  return err == 0;
}

/* ---------------------------------------------------------------- transmit */

int IwxDevice::transmit_slot(const uint8_t *frame, size_t len, const TxRate &rate) {
  if (!_queue_enabled || _fw_error) return -1;
  return transmit_on(_dataq, frame, len, rate, false);
}

int IwxDevice::sta_transmit(const uint8_t *frame, size_t len, const TxRate &rate, bool encrypt) {
  if (!_sta_queue_enabled || _fw_error) return -1;
  return transmit_on(_staq, frame, len, rate, encrypt);
}

int IwxDevice::transmit_on(TxRing &ring, const uint8_t *frame, size_t len, const TxRate &rate,
                           bool encrypt) {
  const size_t hdrlen = hdrlen_80211(frame, len);
  if (!hdrlen || len - hdrlen > kTxPayloadSlot || hdrlen > 64) return -1;
  if (ring.queued >= ring.count - 2) {
    _tx_stats.dropped_full++;
    return -1;
  }
  const unsigned idx = ring.cur & (ring.count - 1);
  auto *cmd = reinterpret_cast<iwx_device_cmd *>(ring.cmd_slot[idx].va);
  const uint64_t cmd_pa = ring.cmd_slot[idx].pa;
  cmd->hdr.cmd = IWX_TX_CMD;
  cmd->hdr.group_id = 0;
  cmd->hdr.qid = static_cast<uint8_t>(ring.qid);
  cmd->hdr.idx = static_cast<uint8_t>(idx);

  /* Rate: always fixed by us (IWX_TX_FLAGS_CMD_RATE), on the first valid
   * antenna, in the encoding the TX command version expects. */
  const uint8_t tx_ant = valid_tx_ant();
  const unsigned ant_idx = (tx_ant & 1) ? 0 : (tx_ant & 2) ? 1 : 0;
  uint32_t rnf = (1u << ant_idx) << IWX_RATE_MCS_ANT_POS;
  const uint8_t tx_ver = cmd_version(IWX_LONG_GROUP, IWX_TX_CMD);
  const bool new_format = tx_ver > 8 && tx_ver != IWX_FW_CMD_VER_UNKNOWN;
  if (rate.kind == TxRate::Vht && new_format) {
    const unsigned mcs = std::min(rate.mcs, 9u);
    rnf = (rate.nss > 1 ? IWX_RATE_MCS_ANT_AB_MSK : rnf) | IWX_RATE_MCS_VHT_MSK |
          (rate.wide80 && _width >= 80 ? IWX_RATE_MCS_CHAN_WIDTH_80 : 0u) |
          (rate.nss > 1 ? IWX_RATE_MCS_NSS_MSK : 0u) | mcs;
  } else if (rate.kind == TxRate::Ht || rate.kind == TxRate::Vht) {
    const unsigned mcs = std::min(rate.mcs, 15u);
    if (mcs > 7) rnf = IWX_RATE_MCS_ANT_AB_MSK;
    if (new_format)
      rnf |= IWX_RATE_MCS_HT_MSK | (mcs > 7 ? IWX_RATE_MCS_NSS_MSK : 0u) | (mcs & 7u);
    else
      rnf |= IWX_RATE_MCS_HT_MSK_V1 | mcs;
  } else {
    unsigned r = rate.mbps_x2;
    const bool cck = cck_index(r) >= 0;
    if (cck && _channel > 14) r = 12; /* no CCK on 5 GHz */
    if (cck_index(r) >= 0) {
      rnf |= new_format ? (IWX_RATE_MCS_CCK_MSK | uint32_t(cck_index(r)))
                        : (IWX_RATE_MCS_CCK_MSK_V1 | cck_plcp(r));
    } else {
      if (ofdm_index(r) < 0) r = 12;
      rnf |= new_format ? (IWX_RATE_MCS_LEGACY_OFDM_MSK | uint32_t(ofdm_index(r))) : ofdm_plcp(r);
    }
  }

  auto *tx = reinterpret_cast<iwx_tx_cmd_gen2 *>(cmd->data);
  std::memset(tx, 0, sizeof(*tx));
  tx->len = htole16(static_cast<uint16_t>(len));
  tx->offload_assist = htole16(offload_header(static_cast<unsigned>(hdrlen)));
  const bool fixed = rate.kind != TxRate::Auto;
  tx->flags = htole32((fixed ? IWX_TX_FLAGS_CMD_RATE : 0u) | (encrypt ? 0u : IWX_TX_FLAGS_ENCRYPT_DIS));
  tx->rate_n_flags = htole32(fixed ? rnf : 0u);
  std::memcpy(reinterpret_cast<uint8_t *>(tx) + sizeof(*tx), frame, hdrlen);

  const size_t payload_len = len - hdrlen;
  uint8_t *payload = ring.payload[idx].va;
  const uint64_t payload_pa = ring.payload[idx].pa;
  if (payload_len) std::memcpy(payload, frame + hdrlen, payload_len);

  auto *desc = reinterpret_cast<iwx_tfh_tfd *>(ring.desc.va) + idx;
  std::memset(desc, 0, sizeof(*desc));
  const unsigned num_tbs = payload_len ? 3 : 2;
  desc->num_tbs = htole16(static_cast<uint16_t>(num_tbs));
  desc->tbs[0].tb_len = htole16(IWX_FIRST_TB_SIZE);
  uint64_t a = htole64(cmd_pa);
  std::memcpy(&desc->tbs[0].addr, &a, sizeof(a));
  const size_t tb1 = sizeof(iwx_cmd_header) + sizeof(*tx) + hdrlen - IWX_FIRST_TB_SIZE;
  desc->tbs[1].tb_len = htole16(static_cast<uint16_t>((tb1 + 3) & ~size_t(3)));
  a = htole64(cmd_pa + IWX_FIRST_TB_SIZE);
  std::memcpy(&desc->tbs[1].addr, &a, sizeof(a));
  if (payload_len) {
    desc->tbs[2].tb_len = htole16(static_cast<uint16_t>(payload_len));
    a = htole64(payload_pa);
    std::memcpy(&desc->tbs[2].addr, &a, sizeof(a));
  }

  /* Byte count table: dwords, fetch chunks in the top bits (pre-AX210). */
  const unsigned filled = offsetof(iwx_tfh_tfd, tbs) + num_tbs * sizeof(iwx_tfh_tb);
  const unsigned chunks = howmany(filled, 64) - 1;
  auto *bc = reinterpret_cast<iwx_agn_scd_bc_tbl *>(ring.bc_tbl.va);
  bc->tfd_offset[idx] = htole16(static_cast<uint16_t>(howmany(len, 4) | (chunks << 12)));

  _tx_hist[_tx_hist_at++ % 8] = {static_cast<uint16_t>(len), static_cast<uint8_t>(hdrlen),
                                 frame[0], frame[1], frame[4], rnf, static_cast<uint8_t>(idx)};
  ring.cur = (ring.cur + 1) % ring.count;
  ring.queued++;
  std::atomic_thread_fence(std::memory_order_seq_cst);
  wr(IWX_HBUS_TARG_WRPTR, (ring.qid << 16) | ring.cur);
  _tx_stats.submitted++;
  return static_cast<int>(idx);
}

/* ------------------------------------------------------------------ receive */

int IwxDevice::poll() {
  if (!_rx_stat) return 0;
  const auto *stat = reinterpret_cast<volatile iwx_rb_status *>(_rx_stat.va);
  uint16_t hw = le16toh(stat->closed_rb_num) & 0xfff;
  hw &= IWX_RX_MQ_RING_COUNT - 1;
  int handled = 0;
  while (_rx_cur != hw) {
    handle_rb(_rx_cur);
    _rx_cur = (_rx_cur + 1) % IWX_RX_MQ_RING_COUNT;
    handled++;
  }
  if (handled) {
    /* Every buffer behind the card's position is free again; the card wants
     * the index in multiples of 8 (iwx_notif_intr). */
    const unsigned w = hw == 0 ? IWX_RX_MQ_RING_COUNT - 1 : hw - 1u;
    wr(IWX_RFH_Q0_FRBDCB_WIDX_TRG, w & ~7u);
  }
  return handled;
}

void IwxDevice::handle_rb(unsigned index) {
  /* The card fills the free buffers in order and closes them in order, so
   * slot `index` holds buffer `index`, as itlwm's iwx_notif_intr assumes. */
  const uint8_t *buf = _rx_bufs[index].va;
  const size_t minsz = sizeof(uint32_t) + sizeof(iwx_cmd_header);
  size_t offset = 0;
  while (offset + minsz < IWX_RBUF_SIZE) {
    const auto *pkt = reinterpret_cast<const iwx_rx_packet *>(buf + offset);
    const int qid = pkt->hdr.qid & ~0x80;
    if ((qid == 0 && pkt->hdr.idx == 0 && pkt->hdr.cmd == 0 && pkt->hdr.group_id == 0) ||
        pkt->len_n_flags == htole32(IWX_FH_RSCSR_FRAME_INVALID))
      break;
    const size_t len = sizeof(pkt->len_n_flags) + iwx_rx_packet_len(pkt);
    if (len < minsz || len > IWX_RBUF_SIZE - offset) break;
    handle_packet(buf + offset, len);
    offset += roundup(len, IWX_FH_RSCSR_FRAME_ALIGN);
  }
  /* Clear the first packet header so a stale buffer never parses twice, and
   * hand the same buffer back at the same slot of the free list. */
  std::memset(_rx_bufs[index].va, 0, IWX_RBUF_SIZE); /* stale packets must never parse again */
  auto *free_list = reinterpret_cast<uint64_t *>(_rx_free.va);
  free_list[index] = htole64(_rx_bufs[index].pa | (index & 0x0fff));
}

void IwxDevice::handle_packet(const uint8_t *raw, size_t len) {
  const auto *pkt = reinterpret_cast<const iwx_rx_packet *>(raw);
  const int qid = pkt->hdr.qid;
  const int idx = pkt->hdr.idx;
  uint32_t code = IWX_WIDE_ID(pkt->hdr.group_id, pkt->hdr.cmd);
  const size_t plen = len - sizeof(pkt->len_n_flags) - sizeof(pkt->hdr);
  const uint8_t *payload = pkt->data;

  /* Narrow commands were sent in the long group; answers may come back in
   * either. */
  if (iwx_cmd_groupid(code) == IWX_LONG_GROUP && qid == IWX_DQA_CMD_QUEUE &&
      idx < int(_cmd_slots.size()) && _cmd_slots[idx].narrow)
    code = iwx_cmd_opcode(code);

  switch (code) {
  case IWX_REPLY_RX_MPDU_CMD:
    handle_mpdu(payload, plen, (le32toh(pkt->len_n_flags) & IWX_FH_RSCSR_RADA_EN) != 0);
    break;
  case IWX_TX_CMD:
    handle_tx_resp(payload, plen, static_cast<unsigned>(idx));
    break;
  case IWX_ALIVE: {
    if (plen == sizeof(iwx_alive_resp_v4)) {
      const auto *r = reinterpret_cast<const iwx_alive_resp_v4 *>(payload);
      _alive_ok = le16toh(r->status) == IWX_ALIVE_STATUS_OK;
      _lmac_error_table = le32toh(r->lmac_data[0].dbg_ptrs.error_event_table_ptr);
      _umac_error_table = le32toh(r->umac_data.dbg_ptrs.error_info_addr);
    } else if (plen >= sizeof(iwx_alive_resp_v5)) {
      const auto *r = reinterpret_cast<const iwx_alive_resp_v5 *>(payload);
      _alive_ok = le16toh(r->status) == IWX_ALIVE_STATUS_OK;
      _lmac_error_table = le32toh(r->lmac_data[0].dbg_ptrs.error_event_table_ptr);
      _umac_error_table = le32toh(r->umac_data.dbg_ptrs.error_info_addr);
    }
    _alive_seen = true;
    break;
  }
  case IWX_INIT_COMPLETE_NOTIF:
    _init_complete = true;
    break;
  case IWX_REPLY_ERROR: {
    if (plen >= sizeof(iwx_error_resp)) {
      const auto *r = reinterpret_cast<const iwx_error_resp *>(payload);
      _log("firmware error " + hex(le32toh(r->error_type)) + " for command " +
           hex(r->cmd_id));
    }
    break;
  }
  default:
    break;
  }

  /* A command's answer completes its slot; any answer of the right size is
   * kept for a caller that wants one. */
  if (!(qid & 0x80) && qid == IWX_DQA_CMD_QUEUE && idx < int(_cmd_slots.size())) {
    CmdSlot &slot = _cmd_slots[idx];
    if (slot.pending) {
      if (slot.want_resp && !(pkt->hdr.group_id & IWX_CMD_FAILED_MSK) && len <= IWX_CMD_RESP_MAX)
        slot.resp.assign(raw, raw + len);
      slot.pending = false;
      if (_cmdq.queued) _cmdq.queued--;
    }
  }
}

void IwxDevice::handle_mpdu(const uint8_t *payload, size_t len, bool rada) {
  const size_t desc_size = IWX_RX_DESC_SIZE_V1;
  if (len < desc_size) return;
  const auto *desc = reinterpret_cast<const iwx_rx_mpdu_desc *>(payload);
  if (!(le16toh(desc->status) & IWX_RX_MPDU_RES_STATUS_CRC_OK) ||
      !(le16toh(desc->status) & IWX_RX_MPDU_RES_STATUS_OVERRUN_OK))
    return;
  size_t mpdu_len = le16toh(desc->mpdu_len);
  if (mpdu_len < 10 || mpdu_len > len - desc_size) return;
  const uint8_t *frame = payload + desc_size;

  /* The firmware pads two bytes after the header of some frames. */
  thread_local std::vector<uint8_t> unpadded;
  if (desc->mac_flags2 & IWX_RX_MPDU_MFLG2_PAD) {
    const size_t h = hdrlen_80211(frame, mpdu_len);
    if (!h || h + 2 > mpdu_len) return;
    unpadded.assign(frame, frame + h);
    unpadded.insert(unpadded.end(), frame + h + 2, frame + mpdu_len);
    frame = unpadded.data();
    mpdu_len -= 2;
  }
  /* Energy is reported as a positive number of dB below zero; 0 = none. */
  const int ea = desc->v1.energy_a ? -int(desc->v1.energy_a) : -256;
  const int eb = desc->v1.energy_b ? -int(desc->v1.energy_b) : -256;
  const int energy = std::max(ea, eb);
  RxFrame f{frame, mpdu_len, static_cast<int8_t>(energy < -127 ? -127 : energy), desc->v1.channel,
            le32toh(desc->v1.rate_n_flags), le16toh(desc->status), rada};
  if (_on_rx) _on_rx(f);
}

void IwxDevice::handle_tx_resp(const uint8_t *payload, size_t len, unsigned slot) {
  if (len < sizeof(iwx_tx_resp)) return;
  const auto *r = reinterpret_cast<const iwx_tx_resp *>(payload);
  if (r->frame_count < 1 ||
      len < sizeof(*r) + size_t(r->frame_count - 1) * sizeof(r->status) + sizeof(uint32_t))
    return;
  const unsigned queue = le16toh(r->tx_queue);
  if (_sta_queue_enabled && queue == _staq.qid) {
    /* The station's frames: the firmware retries them itself; only the ring
     * is reclaimed. */
    uint32_t ssn;
    std::memcpy(&ssn, reinterpret_cast<const uint8_t *>(&r->status) + r->frame_count * sizeof(r->status),
                sizeof(ssn));
    const unsigned idx = (le32toh(ssn) & 0xfff) & (_staq.count - 1);
    const unsigned freed = (idx + _staq.count - _staq.tail) % _staq.count;
    if (freed <= _staq.queued) {
      _staq.tail = idx;
      _staq.queued -= freed;
    }
    return;
  }
  if (queue != _dataq.qid || !_queue_enabled) return;
  if (r->frame_count == 1) {
    const unsigned status = le16toh(r->status.status) & IWX_TX_STATUS_MSK;
    if (status == IWX_TX_STATUS_SUCCESS || status == IWX_TX_STATUS_DIRECT_DONE)
      _tx_stats.acked++;
    else {
      _tx_stats.failed++;
      if (_opt.verbose && _tx_fail_logged++ < 40) {
        char b[160];
        std::snprintf(b, sizeof(b),
                      "tx failed: status 0x%04x, retries %u, rts fails %u, bt kills %u, rate 0x%08x, "
                      "frame_ctrl 0x%04x, bytes %u",
                      le16toh(r->status.status), r->failure_frame, r->failure_rts, r->bt_kill_count,
                      le32toh(r->initial_rate), le16toh(r->frame_ctrl), le16toh(r->byte_cnt));
        _log(b);
      }
    }
    _tx_stats.retries += r->failure_frame;
    if (_on_tx)
      _on_tx(slot, status == IWX_TX_STATUS_SUCCESS || status == IWX_TX_STATUS_DIRECT_DONE);
  }
  uint32_t ssn;
  std::memcpy(&ssn, reinterpret_cast<const uint8_t *>(&r->status) + r->frame_count * sizeof(r->status),
              sizeof(ssn));
  const unsigned idx = (le32toh(ssn) & 0xfff) & (_dataq.count - 1);
  /* Reclaim everything from the tail up to the firmware's next index. */
  const unsigned n = _dataq.count;
  const unsigned freed = (idx + n - _dataq.tail) % n;
  if (freed <= _dataq.queued) {
    _dataq.tail = idx;
    _dataq.queued -= freed;
    _tx_stats.completed += freed;
  }
}

bool IwxDevice::read_mem(uint32_t addr, uint32_t *out, unsigned dwords) {
  if (!nic_lock()) return false;
  std::vector<RegOp> ops;
  ops.push_back({IWX_HBUS_TARG_MEM_RADDR, addr, 1});
  for (unsigned i = 0; i < dwords; i++) ops.push_back({IWX_HBUS_TARG_MEM_RDAT, 0, 0});
  const bool ok = _proxy.batch(ops);
  nic_unlock();
  if (!ok) return false;
  for (unsigned i = 0; i < dwords; i++) out[i] = ops[i + 1].value;
  return true;
}

namespace {
const char *error_name(uint32_t id) {
  static const struct { uint32_t num; const char *name; } names[] = {
      {0x34, "NMI_INTERRUPT_WDG"}, {0x35, "SYSASSERT"}, {0x37, "UCODE_VERSION_MISMATCH"},
      {0x38, "BAD_COMMAND"}, {0x39, "BAD_COMMAND"}, {0x3C, "NMI_INTERRUPT_DATA_ACTION_PT"},
      {0x3D, "FATAL_ERROR"}, {0x46, "NMI_TRM_HW_ERR"}, {0x4C, "NMI_INTERRUPT_TRM"},
      {0x54, "NMI_INTERRUPT_BREAK_POINT"}, {0x5C, "NMI_INTERRUPT_WDG_RXF_FULL"},
      {0x64, "NMI_INTERRUPT_WDG_NO_RBD_RXF_FULL"}, {0x66, "NMI_INTERRUPT_HOST"},
      {0x70, "NMI_INTERRUPT_LMAC_FATAL"}, {0x71, "NMI_INTERRUPT_UMAC_FATAL"},
      {0x73, "NMI_INTERRUPT_OTHER_LMAC_FATAL"}, {0x7C, "NMI_INTERRUPT_ACTION_PT"},
      {0x84, "NMI_INTERRUPT_UNKNOWN"}, {0x86, "NMI_INTERRUPT_INST_ACTION_PT"}};
  for (const auto &n : names)
    if (n.num == (id & ~0xf0000000u)) return n.name;
  return "ADVANCED_SYSASSERT";
}
} // namespace

void IwxDevice::check_health() {
  if (!_open || _fw_error) return;
  const uint32_t cause = rd(IWX_CSR_INT);
  if (cause == 0xffffffffu) {
    _log("the card stopped answering (CSR_INT reads all ones)");
    _fw_error = true;
    return;
  }
  if (!(cause & (IWX_CSR_INT_BIT_SW_ERR | IWX_CSR_INT_BIT_HW_ERR))) return;
  _log("firmware/DMA error: CSR_INT " + hex(cause) + ", FH_INT_STATUS " +
       hex(rd(IWX_CSR_FH_INT_STATUS)));
  _fw_error = true;
  dump_error_tables();
}

void IwxDevice::dump_error_tables() {
  if (!_proxy.is_open() || _proxy.failed()) return;
  const auto *stat = reinterpret_cast<volatile iwx_rb_status *>(_rx_stat.va);
  if (stat)
    _log("rx ring: card closed " + std::to_string(le16toh(stat->closed_rb_num) & 0xfff) +
         ", host at " + std::to_string(_rx_cur) + "; tx queue " + std::to_string(_dataq.qid) +
         " cur " + std::to_string(_dataq.cur) + " tail " + std::to_string(_dataq.tail) +
         " queued " + std::to_string(_dataq.queued));
  for (unsigned i = 0; i < 8; i++) {
    const TxHist &h = _tx_hist[(_tx_hist_at + i) % 8];
    if (!h.len) continue;
    char b[128];
    std::snprintf(b, sizeof(b), "tx history: slot %u len %u hdr %u fc %02x %02x addr1[0] %02x rate 0x%08x",
                  h.slot, h.len, h.hdrlen, h.fc0, h.fc1, h.a1, h.rnf);
    _log(b);
  }
  if (_lmac_error_table >= 0x400000) {
    uint32_t t[38] = {};
    if (read_mem(_lmac_error_table, t, 38) && t[0]) {
      _log("LMAC error " + hex(t[1]) + " " + error_name(t[1]) + ": blink2 " + hex(t[4]) +
           " ilink1 " + hex(t[5]) + " ilink2 " + hex(t[6]) + " data1 " + hex(t[7]) +
           " data2 " + hex(t[8]) + " data3 " + hex(t[9]) + " hcmd " + hex(t[23]) +
           " last_cmd " + hex(t[29]));
    } else {
      _log("LMAC error log empty or unreadable (at " + hex(_lmac_error_table) + ")");
    }
  }
  if (_umac_error_table >= 0x400000) {
    uint32_t t[15] = {};
    if (read_mem(_umac_error_table, t, 15) && t[0]) {
      _log("UMAC error " + hex(t[1]) + " " + error_name(t[1]) + ": blink1 " + hex(t[2]) +
           " blink2 " + hex(t[3]) + " ilink1 " + hex(t[4]) + " ilink2 " + hex(t[5]) +
           " data1 " + hex(t[6]) + " data2 " + hex(t[7]) + " data3 " + hex(t[8]) +
           " cmd_header " + hex(t[13]));
    } else {
      _log("UMAC error log empty or unreadable (at " + hex(_umac_error_table) + ")");
    }
  }
  if (nic_lock()) {
    _log("UMAC PC " + hex(read_prph(IWX_UREG_UMAC_CURRENT_PC)) + ", LMAC PC " +
         hex(read_prph(IWX_UREG_LMAC1_CURRENT_PC)));
    nic_unlock();
  }
}
