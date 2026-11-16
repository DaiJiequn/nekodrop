// SPDX-License-Identifier: GPL-2.0-only
/* AWE allocation follows devourer's PcieTransportWin.cpp (winawdl fork,
 * GPL-2.0): large-page frames, mapped as their 512 small frames into one
 * MEM_PHYSICAL view, since the large-page flags are refused for AWE views. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstring>

#include "dma.h"

namespace {

constexpr size_t kPageBytes = 4096;
constexpr size_t kPagesPerBlock = DmaPool::kBlockBytes / kPageBytes;

using AllocatePages2Fn = BOOL(WINAPI *)(HANDLE, PULONG_PTR, PULONG_PTR,
                                        PMEM_EXTENDED_PARAMETER, ULONG);

std::vector<ULONG_PTR> allocate_blocks(size_t count) {
  const auto allocate = reinterpret_cast<AllocatePages2Fn>(
      GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "AllocateUserPhysicalPages2"));
  std::vector<ULONG_PTR> frames(count);
  MEM_EXTENDED_PARAMETER large{};
  large.Type = MemExtendedParameterAttributeFlags;
  large.ULong64 = MEM_EXTENDED_PARAMETER_NONPAGED_LARGE;
  ULONG_PTR got = count;
  if (!allocate || !allocate(GetCurrentProcess(), &got, frames.data(), &large, 1)) got = 0;
  frames.resize(got);
  return frames;
}

void free_frames(std::vector<ULONG_PTR> &frames) {
  ULONG_PTR count = frames.size();
  if (count) FreeUserPhysicalPages(GetCurrentProcess(), &count, frames.data());
  frames.clear();
}

} // namespace

bool enable_lock_memory_privilege() {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
    return false;
  TOKEN_PRIVILEGES privileges{};
  privileges.PrivilegeCount = 1;
  privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
  const bool found =
      LookupPrivilegeValueW(nullptr, L"SeLockMemoryPrivilege", &privileges.Privileges[0].Luid);
  if (found) AdjustTokenPrivileges(token, FALSE, &privileges, 0, nullptr, nullptr);
  const bool enabled = found && GetLastError() == ERROR_SUCCESS;
  CloseHandle(token);
  return enabled;
}

DmaPool::~DmaPool() {
  if (_view) {
    MapUserPhysicalPages(_view, _frames.size() * kPagesPerBlock, nullptr);
    VirtualFree(_view, 0, MEM_RELEASE);
  }
  std::vector<ULONG_PTR> frames(_frames.begin(), _frames.end());
  free_frames(frames);
}

bool DmaPool::init(size_t blocks, std::string &error) {
  if (!enable_lock_memory_privilege()) {
    error = "SeLockMemoryPrivilege is not available: run from an elevated prompt "
            "(and grant 'Lock pages in memory' if Windows still refuses)";
    return false;
  }
  std::vector<ULONG_PTR> frames = allocate_blocks(blocks);
  if (frames.size() < blocks) {
    error = "only " + std::to_string(frames.size()) + " of " + std::to_string(blocks) +
            " locked 2 MiB blocks were free: memory is fragmented, try after a reboot";
    free_frames(frames);
    return false;
  }
  const size_t len = blocks * kBlockBytes;
  _view = static_cast<uint8_t *>(
      VirtualAlloc(nullptr, len, MEM_RESERVE | MEM_PHYSICAL, PAGE_READWRITE));
  if (!_view) {
    error = "reserving the DMA view failed (" + std::to_string(GetLastError()) + ")";
    free_frames(frames);
    return false;
  }
  std::vector<ULONG_PTR> pages(kPagesPerBlock);
  for (size_t i = 0; i < blocks; i++) {
    for (size_t p = 0; p < kPagesPerBlock; p++) pages[p] = frames[i] + p;
    if (!MapUserPhysicalPages(_view + i * kBlockBytes, kPagesPerBlock, pages.data())) {
      error = "mapping DMA block " + std::to_string(i) + " failed (" +
              std::to_string(GetLastError()) + ")";
      _frames.assign(frames.begin(), frames.end());
      return false;
    }
  }
  _frames.assign(frames.begin(), frames.end());
  std::memset(_view, 0, len);
  return true;
}

DmaBuf DmaPool::alloc(size_t size, size_t align) {
  DmaBuf buf;
  if (!size || size > kBlockBytes || (align & (align - 1))) return buf;
  for (; _block < _frames.size(); _block++, _used = 0) {
    const size_t start = (_used + align - 1) & ~(align - 1);
    if (start + size > kBlockBytes) continue;
    buf.va = _view + _block * kBlockBytes + start;
    buf.pa = static_cast<uint64_t>(_frames[_block]) * kPageBytes + start;
    buf.size = size;
    _used = start + size;
    std::memset(buf.va, 0, size);
    return buf;
  }
  return buf;
}

size_t DmaPool::bytes_free() const {
  if (_block >= _frames.size()) return 0;
  return (_frames.size() - _block) * kBlockBytes - _used;
}
