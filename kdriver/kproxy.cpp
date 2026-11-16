// SPDX-License-Identifier: GPL-2.0-only
/* RegProxy (proxy.h) in the kernel: the BAR is mapped by the driver, so every
 * access is a direct MMIO read or write. Close ops run when the hardware is
 * released (NekoHwRelease) to stop DMA, as the UMDF proxy did on handle close. */
#include "plat.h"
#include "proxy.h"

static std::vector<RegOp> *g_close_ops;

RegProxy::~RegProxy() {}

bool RegProxy::open(std::string &error) {
  if (!g_neko_card.bar) {
    error = "BAR0 is not mapped";
    return false;
  }
  _handle = const_cast<UCHAR *>(g_neko_card.bar);
  _vendor = g_neko_card.vendor;
  _device = g_neko_card.device;
  _subsystem = g_neko_card.subsystem;
  _bar_len = g_neko_card.bar_len;
  return true;
}

static inline volatile ULONG *reg(void *base, uint32_t off) {
  return reinterpret_cast<volatile ULONG *>(static_cast<UCHAR *>(base) + off);
}

uint32_t RegProxy::read32(uint32_t off) {
  if (!_handle || off + 4 > _bar_len) { _failed = true; return 0xffffffffu; }
  return READ_REGISTER_ULONG(reg(_handle, off));
}

void RegProxy::write32(uint32_t off, uint32_t value) {
  if (!_handle || off + 4 > _bar_len) { _failed = true; return; }
  WRITE_REGISTER_ULONG(reg(_handle, off), value);
}

void RegProxy::write8(uint32_t off, uint8_t value) {
  if (!_handle || off + 1 > _bar_len) { _failed = true; return; }
  WRITE_REGISTER_UCHAR(static_cast<UCHAR *>(_handle) + off, value);
}

void RegProxy::set_bits(uint32_t off, uint32_t bits) { write32(off, read32(off) | bits); }
void RegProxy::clear_bits(uint32_t off, uint32_t bits) { write32(off, read32(off) & ~bits); }

bool RegProxy::batch(std::vector<RegOp> &ops) {
  for (auto &op : ops) {
    switch (op.kind) {
    case 0: op.value = read32(op.offset); break;
    case 1: write32(op.offset, op.value); break;
    case 2: set_bits(op.offset, op.value); break;
    case 3: clear_bits(op.offset, op.value); break;
    default: return false;
    }
  }
  return !_failed;
}

bool RegProxy::set_close_ops(const std::vector<RegOp> &ops) {
  delete g_close_ops;
  g_close_ops = new std::vector<RegOp>(ops);
  return true;
}

/* Called by NekoHwRelease before the BAR is unmapped. */
void neko_run_close_ops() {
  if (!g_close_ops || !g_neko_card.bar) return;
  RegProxy p;
  std::string e;
  if (p.open(e)) p.batch(*g_close_ops);
  delete g_close_ops;
  g_close_ops = nullptr;
}
