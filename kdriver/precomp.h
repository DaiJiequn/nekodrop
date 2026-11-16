// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <ntddk.h>
#include <wdf.h>
#include <netadaptercx.h>
#include <wificx.h>

#define NEKO_TAG 'okeN'
#define NEKO_LOG(fmt, ...) \
  DbgPrintEx(DPFLTR_IHVNETWORK_ID, DPFLTR_INFO_LEVEL, "nekodrop: " fmt "\n", __VA_ARGS__)

/* Per-device state. */
struct NEKO_DEVICE {
  WDFDEVICE Device;
  NETADAPTER Adapter;
  /* BAR0 */
  volatile UCHAR *Bar;
  SIZE_T BarLength;
  /* firmware state */
  BOOLEAN Open;
};
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(NEKO_DEVICE, NekoGetDevice);

/* hw.cpp */
NTSTATUS NekoHwPrepare(NEKO_DEVICE *dev, WDFCMRESLIST translated);
void NekoHwRelease(NEKO_DEVICE *dev);
ULONG NekoRead32(NEKO_DEVICE *dev, ULONG offset);
void NekoWrite32(NEKO_DEVICE *dev, ULONG offset, ULONG value);

/* wdi.cpp */
EVT_WIFI_DEVICE_SEND_COMMAND NekoWdiSendCommand;

/* adapter.cpp */
NTSTATUS NekoAdapterCreate(NEKO_DEVICE *dev, NETADAPTER_INIT *init);
