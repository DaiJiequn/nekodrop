// SPDX-License-Identifier: GPL-2.0-only
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <winioctl.h>

#include <cstring>

#include "tap.h"

namespace {

/* tap-windows6's control codes (tap-windows.h). */
constexpr DWORD tap_ctl(DWORD n) { return CTL_CODE(FILE_DEVICE_UNKNOWN, n, METHOD_BUFFERED, FILE_ANY_ACCESS); }
const DWORD kTapGetMac = tap_ctl(1);
const DWORD kTapSetMediaStatus = tap_ctl(6);

const char kAdapterClass[] =
    "SYSTEM\\CurrentControlSet\\Control\\Class\\{4D36E972-E325-11CE-BFC1-08002BE10318}";
const char kConnections[] =
    "SYSTEM\\CurrentControlSet\\Control\\Network\\{4D36E972-E325-11CE-BFC1-08002BE10318}";

std::string reg_string(HKEY key, const char *value) {
  char buf[256] = {};
  DWORD size = sizeof(buf) - 1, type = 0;
  if (RegQueryValueExA(key, value, nullptr, &type, reinterpret_cast<BYTE *>(buf), &size) != ERROR_SUCCESS ||
      type != REG_SZ)
    return {};
  return buf;
}

std::string connection_name(const std::string &guid) {
  HKEY key;
  const std::string path = std::string(kConnections) + "\\" + guid + "\\Connection";
  if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) return {};
  std::string name = reg_string(key, "Name");
  RegCloseKey(key);
  return name;
}

} // namespace

TapAdapter::~TapAdapter() {
  if (_handle) {
    CancelIo(_handle);
    CloseHandle(_handle);
  }
  if (_read_event) CloseHandle(_read_event);
  if (_write_event) CloseHandle(_write_event);
}

bool TapAdapter::open(const std::string &wanted, std::string &error) {
  HKEY cls;
  if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, kAdapterClass, 0, KEY_READ, &cls) != ERROR_SUCCESS) {
    error = "cannot read the network adapter class";
    return false;
  }
  std::string guid;
  for (DWORD i = 0;; i++) {
    char sub[256];
    DWORD len = sizeof(sub);
    if (RegEnumKeyExA(cls, i, sub, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
    HKEY key;
    if (RegOpenKeyExA(cls, sub, 0, KEY_READ, &key) != ERROR_SUCCESS) continue;
    const std::string component = reg_string(key, "ComponentId");
    const std::string id = reg_string(key, "NetCfgInstanceId");
    RegCloseKey(key);
    if (component != "tap0901" || id.empty()) continue;
    const std::string name = connection_name(id);
    if (wanted.empty() || wanted == id || wanted == name) {
      guid = id;
      _name = name;
      break;
    }
  }
  RegCloseKey(cls);
  if (guid.empty()) {
    error = "no TAP-Windows (tap0901) adapter found: install tap-windows6";
    return false;
  }
  const std::string path = "\\\\.\\Global\\" + guid + ".tap";
  HANDLE h = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                         FILE_ATTRIBUTE_SYSTEM | FILE_FLAG_OVERLAPPED, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    error = "cannot open " + path + " (error " + std::to_string(GetLastError()) +
            "): is another program using the TAP adapter?";
    return false;
  }
  DWORD got = 0;
  if (!DeviceIoControl(h, kTapGetMac, _mac.data(), 6, _mac.data(), 6, &got, nullptr) || got != 6) {
    CloseHandle(h);
    error = "the TAP adapter did not give its MAC address";
    return false;
  }
  _handle = h;
  _read_event = CreateEventA(nullptr, TRUE, FALSE, nullptr);
  _write_event = CreateEventA(nullptr, TRUE, FALSE, nullptr);
  _read_buf.resize(2048);
  return true;
}

bool TapAdapter::set_connected(bool up) {
  ULONG status = up ? 1 : 0;
  DWORD got = 0;
  return _handle && DeviceIoControl(_handle, kTapSetMediaStatus, &status, sizeof(status), &status,
                                    sizeof(status), &got, nullptr);
}

bool TapAdapter::read(std::vector<uint8_t> &frame, unsigned timeout_ms) {
  frame.clear();
  if (!_handle) return false;
  auto *ov = reinterpret_cast<OVERLAPPED *>(_read_ov);
  static_assert(sizeof(OVERLAPPED) <= sizeof(_read_ov), "overlapped storage");
  if (!_read_pending) {
    std::memset(ov, 0, sizeof(*ov));
    ov->hEvent = _read_event;
    ResetEvent(_read_event);
    DWORD got = 0;
    if (ReadFile(_handle, _read_buf.data(), static_cast<DWORD>(_read_buf.size()), &got, ov)) {
      frame.assign(_read_buf.begin(), _read_buf.begin() + got);
      return true;
    }
    if (GetLastError() != ERROR_IO_PENDING) return false;
    _read_pending = true;
  }
  if (WaitForSingleObject(_read_event, timeout_ms) != WAIT_OBJECT_0) return true;
  DWORD got = 0;
  _read_pending = false;
  if (!GetOverlappedResult(_handle, ov, &got, FALSE)) return false;
  frame.assign(_read_buf.begin(), _read_buf.begin() + got);
  return true;
}

bool TapAdapter::write(const uint8_t *frame, size_t len) {
  if (!_handle) return false;
  OVERLAPPED ov{};
  ov.hEvent = _write_event;
  ResetEvent(_write_event);
  DWORD put = 0;
  if (WriteFile(_handle, frame, static_cast<DWORD>(len), &put, &ov)) return true;
  if (GetLastError() != ERROR_IO_PENDING) return false;
  return GetOverlappedResult(_handle, &ov, &put, TRUE) != 0;
}
