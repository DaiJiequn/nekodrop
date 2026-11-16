// SPDX-License-Identifier: GPL-2.0-only
/*
 * Hardware access: BAR0 mapping. The kernel twin of iwxradio's RegProxy; the
 * DMA pool (WDF common buffers) and interrupts come next.
 */
#include "precomp.h"

NTSTATUS NekoHwPrepare(NEKO_DEVICE *dev, WDFCMRESLIST translated) {
  const ULONG count = WdfCmResourceListGetCount(translated);
  for (ULONG i = 0; i < count; i++) {
    PCM_PARTIAL_RESOURCE_DESCRIPTOR d = WdfCmResourceListGetDescriptor(translated, i);
    if (d->Type != CmResourceTypeMemory || dev->Bar) continue;
    /* The first memory BAR: CSRs (iwx: 8 KiB minimum). */
    if (d->u.Memory.Length < 0x2000) continue;
    dev->Bar = static_cast<volatile UCHAR *>(
        MmMapIoSpaceEx(d->u.Memory.Start, d->u.Memory.Length, PAGE_READWRITE | PAGE_NOCACHE));
    if (!dev->Bar) return STATUS_INSUFFICIENT_RESOURCES;
    dev->BarLength = d->u.Memory.Length;
  }
  if (!dev->Bar) return STATUS_DEVICE_CONFIGURATION_ERROR;
  /* CSR_HW_REV (0x028): proves the mapping reaches the card. */
  NEKO_LOG("BAR0 mapped, %Iu bytes, CSR_HW_REV 0x%08x", dev->BarLength, NekoRead32(dev, 0x028));
  return STATUS_SUCCESS;
}

void NekoHwRelease(NEKO_DEVICE *dev) {
  if (dev->Bar) {
    MmUnmapIoSpace(const_cast<UCHAR *>(dev->Bar), dev->BarLength);
    dev->Bar = nullptr;
  }
}

ULONG NekoRead32(NEKO_DEVICE *dev, ULONG offset) {
  return READ_REGISTER_ULONG(reinterpret_cast<volatile ULONG *>(dev->Bar + offset));
}

void NekoWrite32(NEKO_DEVICE *dev, ULONG offset, ULONG value) {
  WRITE_REGISTER_ULONG(reinterpret_cast<volatile ULONG *>(dev->Bar + offset), value);
}
