// SPDX-License-Identifier: GPL-2.0-only
/*
 * The NetAdapter WiFiCx asks for when Windows creates the station port. Data
 * queues (TX/RX rings to firmware queues) are added with the data path.
 */
#include "precomp.h"

NTSTATUS NekoAdapterCreate(NEKO_DEVICE *dev, NETADAPTER_INIT *init) {
  NETADAPTER adapter;
  NTSTATUS status = NetAdapterCreate(init, WDF_NO_OBJECT_ATTRIBUTES, &adapter);
  if (!NT_SUCCESS(status)) return status;
  status = WifiAdapterInitialize(adapter);
  if (!NT_SUCCESS(status)) return status;
  dev->Adapter = adapter;
  NEKO_LOG("adapter created");
  return STATUS_SUCCESS;
}
