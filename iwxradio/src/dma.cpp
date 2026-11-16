// SPDX-License-Identifier: GPL-2.0-only
/* AWE use follows devourer's PcieTransportWin.cpp (winawdl fork, GPL-2.0);
 * the sorted small-page pool is this project's, for PCs with no free large
 * pages. */
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
    for (size_t i = 0; i < _frames.size(); i++)
      if (_frames[i]) MapUserPhysicalPages(_view + i * kPageBytes, 1, nullptr);
    VirtualFree(_view, 0, MEM_RELEASE);
  }
  std::vector<ULONG_PTR> held;
  for (uintptr_t f : _frames)
    if (f) held.push_back(f);
  ULONG_PTR n = held.size();
  if (n) FreeUserPhysicalPages(GetCurrentProcess(), &n, held.data());
}

bool DmaPool::init(size_t pages, size_t min_pages, std::string &error) {
  if (!enable_lock_memory_privilege()) {
    error = "SeLockMemoryPrivilege is not available: run as SYSTEM, or elevated with "
            "'Lock pages in memory' granted";
    return false;
  }
  std::vector<ULONG_PTR> frames(pages);
  ULONG_PTR got = pages;
  if (!AllocateUserPhysicalPages(GetCurrentProcess(), &got, frames.data())) got = 0;
  frames.resize(got);
  if (got < min_pages) {
    error = "only " + std::to_string(got) + " lockable 4 KiB pages (" +
            std::to_string(min_pages) + " needed)";
    ULONG_PTR n = frames.size();
    if (n) FreeUserPhysicalPages(GetCurrentProcess(), &n, frames.data());
    return false;
  }
  std::sort(frames.begin(), frames.end());
  _frames.assign(frames.begin(), frames.end());
  _taken.assign(_frames.size(), false);
  _back = _frames.size();
  _view = static_cast<uint8_t *>(
      VirtualAlloc(nullptr, _frames.size() * kPageBytes, MEM_RESERVE | MEM_PHYSICAL, PAGE_READWRITE));
  if (!_view || !MapUserPhysicalPages(_view, frames.size(), frames.data())) {
    error = "mapping the DMA pages failed (" + std::to_string(GetLastError()) + ")";
    return false;
  }
  return true;
}

DmaBuf DmaPool::page_at(size_t index) {
  _taken[index] = true;
  _used_pages++;
  DmaBuf b;
  b.va = _view + index * kPageBytes;
  b.pa = static_cast<uint64_t>(_frames[index]) * kPageBytes;
  b.size = kPageBytes;
  std::memset(b.va, 0, kPageBytes);
  return b;
}

DmaBuf DmaPool::alloc_page() {
  if (!_spare.empty()) {
    const size_t i = _spare.back();
    _spare.pop_back();
    return page_at(i);
  }
  /* Single pages come from the far end, so runs of adjacent frames near the
   * front stay whole for the buffers that need them. */
  while (_back > _cursor && _taken[_back - 1]) _back--;
  if (_back <= _cursor) return {};
  return page_at(--_back);
}

DmaBuf DmaPool::alloc(size_t size, size_t align) {
  if (!size || align > kPageBytes || (align & (align - 1))) return {};
  if (size <= kPageBytes) {
    /* Pack small buffers into one page. */
    const size_t start = (_part_used + align - 1) & ~(align - 1);
    if (_part == SIZE_MAX || start + size > kPageBytes) {
      DmaBuf p = alloc_page();
      if (!p) return {};
      _part = static_cast<size_t>((p.va - _view) / kPageBytes);
      _part_used = 0;
    }
    const size_t at = (_part_used + align - 1) & ~(align - 1);
    _part_used = at + size;
    DmaBuf b;
    b.va = _view + _part * kPageBytes + at;
    b.pa = static_cast<uint64_t>(_frames[_part]) * kPageBytes + at;
    b.size = size;
    return b;
  }
  /* A run of adjacent frames; frames skipped on the way are kept as spares. */
  const size_t n = (size + kPageBytes - 1) / kPageBytes;
  size_t i = _cursor;
  while (i + n <= _back) {
    size_t k = 0;
    while (k < n && !_taken[i + k] && (k == 0 || _frames[i + k] == _frames[i] + k)) k++;
    if (k == n) {
      for (size_t j = _cursor; j < i; j++)
        if (!_taken[j]) _spare.push_back(j);
      DmaBuf b;
      b.va = _view + i * kPageBytes;
      b.pa = static_cast<uint64_t>(_frames[i]) * kPageBytes;
      b.size = size;
      for (size_t j = 0; j < n; j++) {
        _taken[i + j] = true;
        _used_pages++;
      }
      std::memset(b.va, 0, n * kPageBytes);
      if (i == _cursor) _cursor = i + n;
      else _cursor = std::max(_cursor, i + n);
      return b;
    }
    i += k ? k : 1;
  }
  return {};
}

void DmaPool::trim() {
  std::vector<ULONG_PTR> unused;
  for (size_t i = 0; i < _frames.size(); i++) {
    if (_taken[i] || !_frames[i]) continue;
    MapUserPhysicalPages(_view + i * kPageBytes, 1, nullptr);
    unused.push_back(_frames[i]);
    _frames[i] = 0;
    _taken[i] = true; /* never hand it out again */
  }
  _spare.clear();
  ULONG_PTR n = unused.size();
  if (n) FreeUserPhysicalPages(GetCurrentProcess(), &n, unused.data());
}

size_t DmaPool::longest_run() const {
  size_t best = 0, run = 0;
  for (size_t i = 0; i < _frames.size(); i++) {
    run = (i && _frames[i] && _frames[i] == _frames[i - 1] + 1) ? run + 1 : 1;
    best = std::max(best, run);
  }
  return best;
}

size_t DmaPool::runs_of(size_t n) const {
  size_t count = 0, run = 0;
  for (size_t i = 0; i < _frames.size(); i++) {
    run = (i && _frames[i] && _frames[i] == _frames[i - 1] + 1) ? run + 1 : 1;
    if (run == n) {
      count++;
      run = 0;
    }
  }
  return count;
}
