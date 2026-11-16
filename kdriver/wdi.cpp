// SPDX-License-Identifier: GPL-2.0-only
/*
 * WDI command dispatch. WiFiCx hands every WDI task and property request here
 * as a TLV message; each is answered with WifiRequestComplete. Unhandled ones
 * report STATUS_NOT_SUPPORTED until their part is written (docs/KERNEL.md).
 */
#include "precomp.h"

void NekoWdiSendCommand(_In_ WDFDEVICE device, _In_ WIFIREQUEST request) {
  UNREFERENCED_PARAMETER(device);
  const UINT16 id = WifiRequestGetMessageId(request);
  UINT in = 0, out = 0;
  PVOID buffer = WifiRequestGetInOutBuffer(request, &in, &out);
  UNREFERENCED_PARAMETER(buffer);
  NEKO_LOG("WDI message %u (in %u, out %u): not handled yet", id, in, out);
  WifiRequestComplete(request, STATUS_NOT_SUPPORTED, 0);
}
