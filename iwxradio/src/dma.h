// SPDX-License-Identifier: GPL-2.0-only
/* Memory the card reads and writes by DMA, from a user-mode process.
 *
 * UMDF gives a user-mode driver register access but no DMA. Following
 * winawdl's design, the process allocates locked 2 MiB physical blocks with
 * the documented AWE call AllocateUserPhysicalPages2 (SeLockMemoryPrivilege,
 * held by an elevated administrator) and uses their physical addresses as bus
 * addresses. winawdl measured that this works with Kernel DMA Protection on.
 *
 * Every buffer handed out lies inside one block, so it is physically
 * contiguous. The Intel 22000 family takes 64-bit addresses everywhere, so
 * unlike the Realtek case no block has to lie below 4 GiB. */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct DmaBuf {
  uint8_t *va = nullptr;
  uint64_t pa = 0;
  size_t size = 0;
  explicit operator bool() const { return va != nullptr; }
};

class DmaPool {
public:
  DmaPool() = default;
  ~DmaPool();
  DmaPool(const DmaPool &) = delete;
  DmaPool &operator=(const DmaPool &) = delete;

  static constexpr size_t kBlockBytes = size_t{2} << 20;

  /* Takes `blocks` locked 2 MiB blocks. */
  bool init(size_t blocks, std::string &error);
  /* A zeroed buffer of `size` bytes aligned to `align` (a power of two, at
   * most 4096 apart from whole blocks). Never freed individually: the pool
   * goes as a whole, after the card's DMA is stopped. */
  DmaBuf alloc(size_t size, size_t align = 256);
  size_t bytes_free() const;

private:
  uint8_t *_view = nullptr;
  std::vector<uintptr_t> _frames; /* first 4 KiB frame of each block */
  size_t _block = 0;              /* block being carved */
  size_t _used = 0;               /* bytes used in that block */
};

bool enable_lock_memory_privilege();
