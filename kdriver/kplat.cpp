// SPDX-License-Identifier: GPL-2.0-only
/*
 * plat.h in the kernel, plus what the C++ library needs there.
 *
 * The hardware layer is shared with the user-mode radio and uses the MSVC
 * standard library (vector, string, deque, function). In the kernel it is
 * built without exceptions (_HAS_EXCEPTIONS=0, /EHs-c-): allocation goes to
 * non-paged pool, and the library's "throw" paths (length_error and the
 * like, which this code never triggers by design) stop the system with a
 * bugcheck naming the reason rather than continuing with broken state.
 */
#include "plat.h"

#include <ntstrsafe.h>
#include <stdarg.h>

NekoCard g_neko_card;

uint64_t now_us() {
  LARGE_INTEGER freq;
  const LARGE_INTEGER t = KeQueryPerformanceCounter(&freq);
  return static_cast<uint64_t>(t.QuadPart) / static_cast<uint64_t>(freq.QuadPart) * 1000000ull +
         (static_cast<uint64_t>(t.QuadPart) % static_cast<uint64_t>(freq.QuadPart)) * 1000000ull /
             static_cast<uint64_t>(freq.QuadPart);
}

void delay_us(unsigned us) {
  if (us >= 1000 && KeGetCurrentIrql() == PASSIVE_LEVEL) {
    LARGE_INTEGER interval;
    interval.QuadPart = -static_cast<LONGLONG>(us) * 10; /* relative, 100 ns units */
    KeDelayExecutionThread(KernelMode, FALSE, &interval);
    return;
  }
  while (us) {
    const unsigned step = us > 50 ? 50 : us; /* KeStallExecutionProcessor: keep it short */
    KeStallExecutionProcessor(step);
    us -= step;
  }
}

void plat_fence() { KeMemoryBarrier(); }

bool plat_read_file(const std::string &path, std::vector<uint8_t> &out, std::string &error) {
  /* `path` is a firmware file name; the files live in NEKO_FIRMWARE_DIR. */
  std::string full = NEKO_FIRMWARE_DIR "\\";
  const size_t slash = path.find_last_of("/\\");
  full += slash == std::string::npos ? path : path.substr(slash + 1);
  std::vector<WCHAR> wide(full.begin(), full.end());
  wide.push_back(0);
  UNICODE_STRING name;
  RtlInitUnicodeString(&name, wide.data());
  OBJECT_ATTRIBUTES oa;
  InitializeObjectAttributes(&oa, &name, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, nullptr, nullptr);
  HANDLE file;
  IO_STATUS_BLOCK io;
  NTSTATUS st = ZwCreateFile(&file, GENERIC_READ | SYNCHRONIZE, &oa, &io, nullptr, FILE_ATTRIBUTE_NORMAL,
                             FILE_SHARE_READ, FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE,
                             nullptr, 0);
  if (!NT_SUCCESS(st)) {
    error = "cannot open " + full;
    return false;
  }
  FILE_STANDARD_INFORMATION info;
  st = ZwQueryInformationFile(file, &io, &info, sizeof(info), FileStandardInformation);
  if (NT_SUCCESS(st) && info.EndOfFile.QuadPart > 0 && info.EndOfFile.QuadPart < 16 * 1024 * 1024) {
    out.resize(static_cast<size_t>(info.EndOfFile.QuadPart));
    LARGE_INTEGER offset{};
    st = ZwReadFile(file, nullptr, nullptr, nullptr, &io, out.data(), static_cast<ULONG>(out.size()), &offset,
                    nullptr);
    if (NT_SUCCESS(st) && io.Information != out.size()) st = STATUS_END_OF_FILE;
  } else if (NT_SUCCESS(st)) {
    st = STATUS_FILE_INVALID;
  }
  ZwClose(file);
  if (!NT_SUCCESS(st)) {
    error = "cannot read " + full;
    return false;
  }
  return true;
}

/* ---------------------------------------------------------- C++ runtime */

void *operator new(size_t size) {
  void *p = ExAllocatePool2(POOL_FLAG_NON_PAGED, size ? size : 1, 'okeN');
  if (!p) KeBugCheckEx(0xE2 /* MANUALLY_INITIATED_CRASH-like marker */, 'NEKO', 'NEW', size, 0);
  return p;
}
void *operator new[](size_t size) { return operator new(size); }
void operator delete(void *p) noexcept { if (p) ExFreePool(p) /* also frees TLV allocations */; }
void operator delete[](void *p) noexcept { operator delete(p); }
void operator delete(void *p, size_t) noexcept { operator delete(p); } /* also the TLV library's (ULONG_PTR) delete */
void operator delete[](void *p, size_t) noexcept { operator delete(p); }

namespace std {
[[noreturn]] void __cdecl _Xbad_alloc() { KeBugCheckEx(0xE2, 'NEKO', 1, 0, 0); }
[[noreturn]] void __cdecl _Xinvalid_argument(const char *) { KeBugCheckEx(0xE2, 'NEKO', 2, 0, 0); }
[[noreturn]] void __cdecl _Xlength_error(const char *) { KeBugCheckEx(0xE2, 'NEKO', 3, 0, 0); }
[[noreturn]] void __cdecl _Xout_of_range(const char *) { KeBugCheckEx(0xE2, 'NEKO', 4, 0, 0); }
[[noreturn]] void __cdecl _Xoverflow_error(const char *) { KeBugCheckEx(0xE2, 'NEKO', 5, 0, 0); }
[[noreturn]] void __cdecl _Xruntime_error(const char *) { KeBugCheckEx(0xE2, 'NEKO', 6, 0, 0); }
[[noreturn]] void __cdecl _Xbad_function_call() { KeBugCheckEx(0xE2, 'NEKO', 7, 0, 0); }
} // namespace std

extern "C" [[noreturn]] void __cdecl _invalid_parameter_noinfo_noreturn() { KeBugCheckEx(0xE2, 'NEKO', 8, 0, 0); }
extern "C" void __cdecl _invalid_parameter_noinfo() {}

/* plat_snprintf */
int plat_snprintf(char *buf, size_t size, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  size_t remaining = 0;
  const NTSTATUS st = RtlStringCbVPrintfExA(buf, size, nullptr, &remaining, STRSAFE_IGNORE_NULLS, fmt, ap);
  va_end(ap);
  if (!NT_SUCCESS(st) && st != STATUS_BUFFER_OVERFLOW) return -1;
  return static_cast<int>(size - remaining);
}

/* More of what the library references with exceptions off. */
extern "C" [[noreturn]] void __cdecl _invoke_watson(const wchar_t *, const wchar_t *, const wchar_t *, unsigned int,
                                                    uintptr_t) {
  KeBugCheckEx(0xE2, 'NEKO', 9, 0, 0);
}
extern "C" [[noreturn]] void __cdecl abort() { KeBugCheckEx(0xE2, 'NEKO', 10, 0, 0); }
namespace stdext { class exception; }
namespace std { void(__cdecl *_Raise_handler)(const stdext::exception &) = nullptr; }

/* The WiFiCx TLV generator/parser allocates with an allocation context
 * (always 0 here): plain non-paged pool, NULL on failure. */
void *__cdecl operator new(size_t size, ULONG_PTR) noexcept {
  return ExAllocatePool2(POOL_FLAG_NON_PAGED, size ? size : 1, 'vlTN');
}
void *__cdecl operator new[](size_t size, ULONG_PTR ctx) noexcept { return operator new(size, ctx); }

#include "kplat_glue.h"
void NekoGlueSetCard(volatile UCHAR *bar, ULONG barLength, USHORT vendor, USHORT device, USHORT subsystem,
                     WDFDMAENABLER dma) {
  g_neko_card.bar = bar;
  g_neko_card.bar_len = barLength;
  g_neko_card.vendor = vendor;
  g_neko_card.device = device;
  g_neko_card.subsystem = subsystem;
  g_neko_card.dma = dma;
}
void neko_run_close_ops();
void NekoGlueRunCloseOps() { neko_run_close_ops(); }
