// SPDX-License-Identifier: GPL-2.0-only
/*
 * Receive interrupts. The card runs in MSI mode (IWX_UREG_CHICK_MSI_ENABLE,
 * set by the hardware layer; the INF asks Windows for one message). Only the
 * receive causes are unmasked: the ISR acknowledges FH_RX/SW_RX and wakes the
 * worker, which drains the ring as it always did. Error causes stay masked
 * and unacknowledged so the hardware layer's health check still sees them.
 * Should no interrupt ever arrive, the worker's 1 ms poll carries on alone;
 * once interrupts flow it relaxes to 10 ms between polls.
 */
#include "precomp.h"

static const ULONG kCsrInt = 0x008, kCsrIntMask = 0x00c, kCsrFhIntStatus = 0x010;
static const ULONG kRxCauses = (1u << 31) /* FH_RX */ | (1u << 3) /* SW_RX */;
static const ULONG kFhRxMask = (1u << 30) | (1u << 17) | (1u << 16);

static BOOLEAN Isr(WDFINTERRUPT interrupt, ULONG) {
  NEKO_DEVICE *dev = NekoGetDevice(WdfInterruptGetDevice(interrupt));
  volatile UCHAR *bar = dev->Bar;
  if (!bar || !dev->IrqEnabled) return FALSE;
  const ULONG causes = READ_REGISTER_ULONG(reinterpret_cast<volatile ULONG *>(bar + kCsrInt));
  if (causes == 0xffffffffu || !(causes & kRxCauses)) return FALSE; /* gone, or not ours */
  WRITE_REGISTER_ULONG(reinterpret_cast<volatile ULONG *>(bar + kCsrInt), causes & kRxCauses);
  WRITE_REGISTER_ULONG(reinterpret_cast<volatile ULONG *>(bar + kCsrFhIntStatus), kFhRxMask);
  WdfInterruptQueueDpcForIsr(interrupt);
  return TRUE;
}

static void Dpc(WDFINTERRUPT interrupt, WDFOBJECT) {
  NEKO_DEVICE *dev = NekoGetDevice(WdfInterruptGetDevice(interrupt));
  InterlockedIncrement(&dev->IrqCount);
  KeSetEvent(&dev->Wake, IO_NO_INCREMENT, FALSE);
}

NTSTATUS NekoIrqCreate(NEKO_DEVICE *dev) {
  WDF_INTERRUPT_CONFIG cfg;
  WDF_INTERRUPT_CONFIG_INIT(&cfg, Isr, Dpc);
  return WdfInterruptCreate(dev->Device, &cfg, WDF_NO_OBJECT_ATTRIBUTES, &dev->Interrupt);
}

/* After the firmware is up (the bring-up polls CSR_INT for ALIVE itself). */
void NekoIrqEnable(NEKO_DEVICE *dev) {
  if (!dev->Bar || !dev->Interrupt) return;
  dev->IrqEnabled = TRUE;
  WRITE_REGISTER_ULONG(reinterpret_cast<volatile ULONG *>(dev->Bar + kCsrIntMask), kRxCauses);
}

void NekoIrqDisable(NEKO_DEVICE *dev) {
  if (!dev->Bar) return;
  dev->IrqEnabled = FALSE;
  WRITE_REGISTER_ULONG(reinterpret_cast<volatile ULONG *>(dev->Bar + kCsrIntMask), 0);
  if (dev->Interrupt) WdfInterruptSynchronize(dev->Interrupt, [](WDFINTERRUPT, WDFCONTEXT) -> BOOLEAN { return TRUE; }, nullptr);
}
