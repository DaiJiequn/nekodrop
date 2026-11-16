// SPDX-License-Identifier: GPL-2.0-only
/*
 * WDI commands. WiFiCx calls NekoWdiSendCommand (up to DISPATCH_LEVEL); the
 * request is queued for the worker thread, which owns the hardware. Each task
 * follows WDI's message pattern: M1 (the command) is parsed, M3
 * (WifiRequestComplete) acknowledges it, and the task's completion indication
 * (M4, WifiDeviceReceiveIndication with the command's transaction ID) ends it,
 * now for quick tasks or later for scan and connect. Properties complete with
 * M3 alone. Messages not handled here answer STATUS_NOT_SUPPORTED, as WiFiCx
 * expects from drivers that do not implement them.
 */
#include "precomp.h"

void NekoWdiSendCommand(_In_ WDFDEVICE device, _In_ WIFIREQUEST request) {
  NEKO_DEVICE *dev = NekoGetDevice(device);
  auto *job = static_cast<NEKO_JOB *>(ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(NEKO_JOB), NEKO_TAG));
  if (!job) {
    WifiRequestComplete(request, STATUS_INSUFFICIENT_RESOURCES, 0);
    return;
  }
  job->Request = request;
  job->MessageId = WifiRequestGetMessageId(request);
  job->Buffer = WifiRequestGetInOutBuffer(request, &job->InLength, &job->OutLength);
  KIRQL irql;
  KeAcquireSpinLock(&dev->Lock, &irql);
  InsertTailList(&dev->Jobs, &job->Link);
  KeReleaseSpinLock(&dev->Lock, irql);
  NekoKick(dev);
}

void NekoIndicate(NEKO_DEVICE *dev, const WDI_MESSAGE_HEADER *orig, UINT16 id, UINT32 transaction, NTSTATUS status,
                  const UCHAR *tlv, ULONG tlvLength) {
  WDF_OBJECT_ATTRIBUTES attrs;
  WDF_OBJECT_ATTRIBUTES_INIT(&attrs);
  attrs.ParentObject = dev->Device;
  WDFMEMORY memory;
  PUCHAR buffer;
  const SIZE_T size = sizeof(WDI_MESSAGE_HEADER) + tlvLength;
  if (!NT_SUCCESS(WdfMemoryCreate(&attrs, NonPagedPoolNx, NEKO_TAG, size, &memory, reinterpret_cast<PVOID *>(&buffer))))
    return;
  RtlZeroMemory(buffer, size);
  auto *h = reinterpret_cast<WDI_MESSAGE_HEADER *>(buffer);
  if (orig) {
    h->PortId = orig->PortId;
    h->IhvSpecificId = orig->IhvSpecificId;
  }
  h->Status = status;
  h->TransactionId = transaction;
  if (tlv && tlvLength) RtlCopyMemory(buffer + sizeof(WDI_MESSAGE_HEADER), tlv, tlvLength);
  WifiDeviceReceiveIndication(dev->Device, id, memory);
  WdfObjectDelete(memory);
}

static void M3(NEKO_JOB *job, NTSTATUS status) {
  WifiRequestComplete(job->Request, status, sizeof(WDI_MESSAGE_HEADER));
}

static void M4(NEKO_DEVICE *dev, const WDI_MESSAGE_HEADER &hdr, UINT16 completeId, NTSTATUS status) {
  NekoIndicate(dev, &hdr, completeId, hdr.TransactionId, status, nullptr, 0);
}

/* The TLVs after the message header. */
static ULONG TlvLength(const NEKO_JOB *job) {
  return job->InLength > sizeof(WDI_MESSAGE_HEADER) ? job->InLength - sizeof(WDI_MESSAGE_HEADER) : 0;
}
static const UINT8 *Tlv(const NEKO_JOB *job) { return static_cast<const UINT8 *>(job->Buffer) + sizeof(WDI_MESSAGE_HEADER); }

static void RadioStatus(NEKO_DEVICE *dev, const WDI_MESSAGE_HEADER &hdr) {
  WDI_INDICATION_RADIO_STATUS_PARAMETERS p;
  p.RadioState.HardwareState = TRUE;
  p.RadioState.SoftwareState = dev->RadioOn;
  UINT8 *out = nullptr;
  ULONG outLength = 0;
  if (GenerateWdiIndicationRadioStatus(&p, 0, &dev->Tlv, &outLength, &out) == NDIS_STATUS_SUCCESS) {
    NekoIndicate(dev, &hdr, WDI_INDICATION_RADIO_STATUS, 0, STATUS_SUCCESS, out, outLength);
    FreeGenerated(out);
  }
}

void NekoWdiRun(NEKO_DEVICE *dev, NEKO_JOB *job) {
  if (job->IoRequest) {
    NekoAwdlRun(dev, job);
    return;
  }
  if (job->InLength < sizeof(WDI_MESSAGE_HEADER)) {
    WifiRequestComplete(job->Request, STATUS_INVALID_PARAMETER, 0);
    return;
  }
  const WDI_MESSAGE_HEADER hdr = *static_cast<const WDI_MESSAGE_HEADER *>(job->Buffer);
  switch (job->MessageId) {
  case WDI_TASK_SET_RADIO_STATE: {
    WDI_SET_RADIO_STATE_PARAMETERS p;
    if (ParseWdiTaskSetRadioState(TlvLength(job), Tlv(job), &dev->Tlv, &p) != NDIS_STATUS_SUCCESS) {
      M3(job, STATUS_INVALID_PARAMETER);
      return;
    }
    M3(job, STATUS_SUCCESS);
    const BOOLEAN on = p.SoftwareRadioState != 0;
    if (!on && dev->RadioOn) {
      NekoScanAbort(dev);
      if (dev->Conn != ConnIdle) NekoDisconnect(dev, &hdr, TRUE, 3);
    }
    dev->RadioOn = on;
    NEKO_LOG("radio %s", on ? "on" : "off");
    RadioStatus(dev, hdr);
    M4(dev, hdr, WDI_INDICATION_SET_RADIO_STATE_COMPLETE, STATUS_SUCCESS);
    return;
  }
  case WDI_TASK_DOT11_RESET:
    M3(job, STATUS_SUCCESS);
    NekoScanAbort(dev);
    if (dev->Conn != ConnIdle) NekoDisconnect(dev, nullptr, TRUE, 3);
    M4(dev, hdr, WDI_INDICATION_DOT11_RESET_COMPLETE, STATUS_SUCCESS);
    return;
  case WDI_TASK_SCAN: {
    WDI_SCAN_PARAMETERS p;
    if (ParseWdiTaskScan(TlvLength(job), Tlv(job), &dev->Tlv, &p) != NDIS_STATUS_SUCCESS) {
      M3(job, STATUS_INVALID_PARAMETER);
      return;
    }
    if (!dev->RadioOn || dev->Scanning) {
      M3(job, dev->RadioOn ? STATUS_DEVICE_BUSY : STATUS_POWER_STATE_INVALID);
      CleanupParsedWdiTaskScan(&p);
      return;
    }
    NekoScanStart(dev, &hdr, &p); /* copies what it needs */
    CleanupParsedWdiTaskScan(&p);
    M3(job, STATUS_SUCCESS);
    return; /* M4 when the last channel is done */
  }
  case WDI_TASK_CONNECT: {
    WDI_TASK_CONNECT_PARAMETERS p;
    if (ParseWdiTaskConnect(TlvLength(job), Tlv(job), &dev->Tlv, &p) != NDIS_STATUS_SUCCESS) {
      M3(job, STATUS_INVALID_PARAMETER);
      return;
    }
    const NTSTATUS st = dev->RadioOn ? NekoConnectStart(dev, &hdr, &p) : STATUS_POWER_STATE_INVALID;
    CleanupParsedWdiTaskConnect(&p);
    M3(job, st);
    return; /* M4 from the connect state machine */
  }
  case WDI_TASK_DISCONNECT:
    M3(job, STATUS_SUCCESS);
    if (dev->Conn != ConnIdle) NekoDisconnect(dev, &hdr, TRUE, 3);
    M4(dev, hdr, WDI_INDICATION_DISCONNECT_COMPLETE, STATUS_SUCCESS);
    return;
  case WDI_SET_ADD_CIPHER_KEYS: {
    WDI_SET_ADD_CIPHER_KEYS_PARAMETERS p;
    if (ParseWdiSetAddCipherKeys(TlvLength(job), Tlv(job), &dev->Tlv, &p) != NDIS_STATUS_SUCCESS) {
      M3(job, STATUS_INVALID_PARAMETER);
      return;
    }
    const NTSTATUS st = NekoAddKeys(dev, &p);
    CleanupParsedWdiSetAddCipherKeys(&p);
    M3(job, st);
    return;
  }
  case WDI_SET_SAE_AUTH_PARAMS: {
    WDI_SET_SAE_AUTH_PARAMS_COMMAND p;
    if (ParseWdiSetSaeAuthParams(TlvLength(job), Tlv(job), &dev->Tlv, &p) != NDIS_STATUS_SUCCESS) {
      M3(job, STATUS_INVALID_PARAMETER);
      return;
    }
    const NTSTATUS st = NekoSaeParams(dev, &hdr, &p);
    CleanupParsedWdiSetSaeAuthParams(&p);
    M3(job, st);
    return;
  }
  case WDI_SET_DELETE_CIPHER_KEYS:
    M3(job, STATUS_SUCCESS); /* keys go with the station context on disconnect */
    return;
  default:
    NEKO_LOG("WDI message %u not supported", job->MessageId);
    M3(job, STATUS_NOT_SUPPORTED);
    return;
  }
}
