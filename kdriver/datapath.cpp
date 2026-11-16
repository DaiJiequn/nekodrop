// SPDX-License-Identifier: GPL-2.0-only
/*
 * The station's data path (NetAdapterCx). WiFiCx's adapter is Native 802.11
 * (*MediaType 16): packets carry an 802.11 data header. Transmit copies each
 * packet and queues it for the worker, which sends it on the station queue
 * (encrypted by the firmware once Windows installed the pairwise key; EAPOL
 * of the 4-way handshake goes in the clear). Receive takes the AP's data
 * frames the firmware decrypted, drops the CCMP header and MIC and the QoS
 * field, and hands them to the OS. DataFormat=1 in the driver's registry key
 * switches both directions to Ethernet frames instead, should a build of
 * WiFiCx want those.
 */
#include "precomp.h"

#include <net/virtualaddress.h>

struct NEKO_QUEUE {
  NEKO_DEVICE *Dev;
  NET_RING_COLLECTION const *Rings;
  NET_EXTENSION Va;
  volatile LONG Notify;
};
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(NEKO_QUEUE, NekoGetQueue);

static NEKO_DEVICE *g_dev; /* one card per driver instance */
static const UCHAR kLlc[6] = {0xaa, 0xaa, 0x03, 0x00, 0x00, 0x00};

static ULONG HeaderLength(const UCHAR *f) {
  ULONG h = 24;
  if ((f[1] & 0x03) == 0x03) h += 6;  /* four addresses */
  if ((f[0] & 0x80)) h += 2;          /* QoS data */
  return h;
}

static bool IsPairwiseEapol(const UCHAR *payload, ULONG len) {
  /* LLC/SNAP 888E, EAPOL-Key, key information "pairwise" bit */
  if (len < 8 + 4 + 3 || !RtlEqualMemory(payload, kLlc, 6) || payload[6] != 0x88 || payload[7] != 0x8e) return false;
  const UCHAR *e = payload + 8;
  return e[1] == 3 && (e[6] & 0x08);
}

/* ------------------------------------------------------------- transmit */

void NekoDataTransmit(NEKO_DEVICE *dev, NEKO_FRAME *f) {
  if (dev->Conn != ConnUp || f->Length < 14) return;
  UCHAR buf[2400];
  ULONG n = 0;
  if (dev->DataFormat == 1) {
    /* Ethernet: DA, SA, ethertype, payload -> 802.11 ToDS + LLC/SNAP */
    if (f->Length - 14 + 32 > sizeof(buf)) return;
    buf[n++] = 0x08;
    buf[n++] = 0x01;
    buf[n++] = 0;
    buf[n++] = 0;
    RtlCopyMemory(buf + n, dev->ConnBssid, 6);
    n += 6;
    RtlCopyMemory(buf + n, dev->Address, 6);
    n += 6;
    RtlCopyMemory(buf + n, f->Data, 6);
    n += 6;
    buf[n++] = 0;
    buf[n++] = 0;
    RtlCopyMemory(buf + n, kLlc, 6);
    n += 6;
    RtlCopyMemory(buf + n, f->Data + 12, f->Length - 12);
    n += f->Length - 12;
  } else {
    if (f->Length > sizeof(buf) || f->Length < 24 || (f->Data[0] & 0x0c) != 0x08) return;
    RtlCopyMemory(buf, f->Data, f->Length);
    n = f->Length;
    buf[1] = static_cast<UCHAR>((buf[1] & ~0x03) | 0x01); /* ToDS */
    RtlCopyMemory(buf + 10, dev->Address, 6);
  }
  const ULONG h = HeaderLength(buf);
  if (h > n) return;
  const bool eapol = n >= h + 8 && RtlEqualMemory(buf + h, kLlc, 6) && buf[h + 6] == 0x88 && buf[h + 7] == 0x8e;
  const bool encrypt = dev->PairwiseKey && !(eapol && IsPairwiseEapol(buf + h, n - h));
  if (encrypt) buf[1] |= 0x40;
  else buf[1] &= static_cast<UCHAR>(~0x40);
  khal_sta_transmit(buf, n, encrypt, eapol);
}

static void TxAdvance(NETPACKETQUEUE queue) {
  NEKO_QUEUE *q = NekoGetQueue(queue);
  NET_RING *pr = NetRingCollectionGetPacketRing(q->Rings);
  NET_RING *fr = NetRingCollectionGetFragmentRing(q->Rings);
  while (pr->BeginIndex != pr->EndIndex) {
    NET_PACKET *p = NetRingGetPacketAtIndex(pr, pr->BeginIndex);
    if (!p->Ignore && p->FragmentCount) {
      ULONG total = 0;
      UINT32 fi = p->FragmentIndex;
      for (UINT16 i = 0; i < p->FragmentCount; i++, fi = NetRingIncrementIndex(fr, fi))
        total += static_cast<ULONG>(NetRingGetFragmentAtIndex(fr, fi)->ValidLength);
      if (total && total <= 2400) {
        if (NEKO_FRAME *frame = NekoFrameAlloc(total)) {
          ULONG off = 0;
          fi = p->FragmentIndex;
          for (UINT16 i = 0; i < p->FragmentCount; i++, fi = NetRingIncrementIndex(fr, fi)) {
            NET_FRAGMENT *frag = NetRingGetFragmentAtIndex(fr, fi);
            const auto *va = NetExtensionGetFragmentVirtualAddress(&q->Va, fi);
            RtlCopyMemory(frame->Data + off, static_cast<UCHAR *>(va->VirtualAddress) + frag->Offset,
                          static_cast<SIZE_T>(frag->ValidLength));
            off += static_cast<ULONG>(frag->ValidLength);
          }
          NekoQueueTx(q->Dev, frame);
        }
      }
    }
    /* Copied: the packet and its fragments go back to the OS now. */
    UINT32 next = p->FragmentIndex;
    for (UINT16 i = 0; i < p->FragmentCount; i++) next = NetRingIncrementIndex(fr, next);
    if (p->FragmentCount) fr->BeginIndex = next;
    fr->NextIndex = fr->BeginIndex;
    pr->BeginIndex = NetRingIncrementIndex(pr, pr->BeginIndex);
    pr->NextIndex = pr->BeginIndex;
  }
}

static void TxNotify(NETPACKETQUEUE queue, BOOLEAN enabled) { InterlockedExchange(&NekoGetQueue(queue)->Notify, enabled); }
static void TxCancel(NETPACKETQUEUE queue) {
  NEKO_QUEUE *q = NekoGetQueue(queue);
  NET_RING *pr = NetRingCollectionGetPacketRing(q->Rings);
  NET_RING *fr = NetRingCollectionGetFragmentRing(q->Rings);
  while (pr->BeginIndex != pr->EndIndex) {
    NetRingGetPacketAtIndex(pr, pr->BeginIndex)->Ignore = 1;
    pr->BeginIndex = NetRingIncrementIndex(pr, pr->BeginIndex);
  }
  pr->NextIndex = pr->BeginIndex;
  fr->BeginIndex = fr->NextIndex = fr->EndIndex;
}

static NTSTATUS CreateTxQueue(NETADAPTER adapter, NETTXQUEUE_INIT *init) {
  UNREFERENCED_PARAMETER(adapter);
  NEKO_DEVICE *dev = g_dev;
  WDF_OBJECT_ATTRIBUTES attrs;
  WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attrs, NEKO_QUEUE);
  NET_PACKET_QUEUE_CONFIG cfg;
  NET_PACKET_QUEUE_CONFIG_INIT(&cfg, TxAdvance, TxNotify, TxCancel);
  NETPACKETQUEUE queue;
  NTSTATUS st = NetTxQueueCreate(init, &attrs, &cfg, &queue);
  if (!NT_SUCCESS(st)) return st;
  NEKO_QUEUE *q = NekoGetQueue(queue);
  q->Dev = dev;
  q->Rings = NetTxQueueGetRingCollection(queue);
  NET_EXTENSION_QUERY query;
  NET_EXTENSION_QUERY_INIT(&query, NET_FRAGMENT_EXTENSION_VIRTUAL_ADDRESS_NAME,
                           NET_FRAGMENT_EXTENSION_VIRTUAL_ADDRESS_VERSION_1, NetExtensionTypeFragment);
  NetTxQueueGetExtension(queue, &query, &q->Va);
  if (!q->Va.Enabled) return STATUS_NOT_SUPPORTED;
  dev->TxQueue = queue;
  return STATUS_SUCCESS;
}

/* -------------------------------------------------------------- receive */

void NekoDataOnFrame(NEKO_DEVICE *dev, const KhalRx *f) {
  if (dev->Conn != ConnUp || f->len < 24) return;
  const UCHAR *d = f->data;
  if ((d[1] & 0x03) != 0x02) return;                      /* FromDS */
  if (!RtlEqualMemory(d + 10, dev->ConnBssid, 6)) return; /* from our AP */
  if (d[0] & 0x40) return;                                /* null / QoS null */
  ULONG h = HeaderLength(d);
  if (f->len < h) return;
  const UCHAR *payload = d + h;
  ULONG len = static_cast<ULONG>(f->len - h);
  if (d[1] & 0x40) { /* protected: the firmware decrypted it, or it is dropped */
    const USHORT ok = 0x0002 | 0x0004; /* IWX_RX_MPDU_RES_STATUS_DEC_DONE | MIC_OK */
    if ((f->status & ok) != ok || len < 8) return;
    payload += 8;
    len -= 8;
    if (!f->mic_stripped) {
      if (len < 8) return;
      len -= 8;
    }
  }
  if (len < 8 || !RtlEqualMemory(payload, kLlc, 3)) return;
  NEKO_FRAME *out;
  if (dev->DataFormat == 1) {
    out = NekoFrameAlloc(14 + len - 8);
    if (!out) return;
    RtlCopyMemory(out->Data, d + 4, 6);      /* DA */
    RtlCopyMemory(out->Data + 6, d + 16, 6); /* SA */
    RtlCopyMemory(out->Data + 12, payload + 6, len - 6);
  } else {
    out = NekoFrameAlloc(24 + len);
    if (!out) return;
    RtlCopyMemory(out->Data, d, 24);
    out->Data[0] = 0x08;                                   /* plain data, no QoS field */
    out->Data[1] = static_cast<UCHAR>(d[1] & ~0x40);       /* no longer protected */
    RtlCopyMemory(out->Data + 24, payload, len);
  }
  NekoQueueRx(dev, out);
}

static void RxAdvance(NETPACKETQUEUE queue) {
  NEKO_QUEUE *q = NekoGetQueue(queue);
  NEKO_DEVICE *dev = q->Dev;
  NET_RING *pr = NetRingCollectionGetPacketRing(q->Rings);
  NET_RING *fr = NetRingCollectionGetFragmentRing(q->Rings);
  while (pr->BeginIndex != pr->EndIndex && fr->BeginIndex != fr->EndIndex) {
    KIRQL irql;
    KeAcquireSpinLock(&dev->Lock, &irql);
    LIST_ENTRY *e = IsListEmpty(&dev->RxFrames) ? nullptr : RemoveHeadList(&dev->RxFrames);
    if (e) dev->RxCount--;
    KeReleaseSpinLock(&dev->Lock, irql);
    if (!e) break;
    NEKO_FRAME *frame = CONTAINING_RECORD(e, NEKO_FRAME, Link);
    const UINT32 fi = fr->BeginIndex;
    NET_FRAGMENT *frag = NetRingGetFragmentAtIndex(fr, fi);
    const auto *va = NetExtensionGetFragmentVirtualAddress(&q->Va, fi);
    const ULONG len = frame->Length < frag->Capacity ? frame->Length : static_cast<ULONG>(frag->Capacity);
    RtlCopyMemory(static_cast<UCHAR *>(va->VirtualAddress), frame->Data, len);
    frag->ValidLength = len;
    frag->Offset = 0;
    NET_PACKET *p = NetRingGetPacketAtIndex(pr, pr->BeginIndex);
    p->FragmentIndex = fi;
    p->FragmentCount = 1;
    p->Layout = {};
    NekoFrameFree(frame);
    fr->BeginIndex = NetRingIncrementIndex(fr, fr->BeginIndex);
    fr->NextIndex = fr->BeginIndex;
    pr->BeginIndex = NetRingIncrementIndex(pr, pr->BeginIndex);
    pr->NextIndex = pr->BeginIndex;
  }
}

static void RxNotify(NETPACKETQUEUE queue, BOOLEAN enabled) { InterlockedExchange(&NekoGetQueue(queue)->Notify, enabled); }
static void RxCancel(NETPACKETQUEUE queue) {
  NEKO_QUEUE *q = NekoGetQueue(queue);
  NET_RING *pr = NetRingCollectionGetPacketRing(q->Rings);
  NET_RING *fr = NetRingCollectionGetFragmentRing(q->Rings);
  while (pr->BeginIndex != pr->EndIndex) {
    NetRingGetPacketAtIndex(pr, pr->BeginIndex)->Ignore = 1;
    pr->BeginIndex = NetRingIncrementIndex(pr, pr->BeginIndex);
  }
  pr->NextIndex = pr->BeginIndex;
  fr->BeginIndex = fr->NextIndex = fr->EndIndex;
}

static NTSTATUS CreateRxQueue(NETADAPTER adapter, NETRXQUEUE_INIT *init) {
  UNREFERENCED_PARAMETER(adapter);
  NEKO_DEVICE *dev = g_dev;
  WDF_OBJECT_ATTRIBUTES attrs;
  WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attrs, NEKO_QUEUE);
  NET_PACKET_QUEUE_CONFIG cfg;
  NET_PACKET_QUEUE_CONFIG_INIT(&cfg, RxAdvance, RxNotify, RxCancel);
  NETPACKETQUEUE queue;
  NTSTATUS st = NetRxQueueCreate(init, &attrs, &cfg, &queue);
  if (!NT_SUCCESS(st)) return st;
  NEKO_QUEUE *q = NekoGetQueue(queue);
  q->Dev = dev;
  q->Rings = NetRxQueueGetRingCollection(queue);
  NET_EXTENSION_QUERY query;
  NET_EXTENSION_QUERY_INIT(&query, NET_FRAGMENT_EXTENSION_VIRTUAL_ADDRESS_NAME,
                           NET_FRAGMENT_EXTENSION_VIRTUAL_ADDRESS_VERSION_1, NetExtensionTypeFragment);
  NetRxQueueGetExtension(queue, &query, &q->Va);
  if (!q->Va.Enabled) return STATUS_NOT_SUPPORTED;
  dev->RxQueue = queue;
  return STATUS_SUCCESS;
}

void NekoRxNotify(NEKO_DEVICE *dev) {
  if (dev->RxQueue && NekoGetQueue(dev->RxQueue)->Notify) {
    InterlockedExchange(&NekoGetQueue(dev->RxQueue)->Notify, FALSE);
    NetRxQueueNotifyMoreReceivedPacketsAvailable(dev->RxQueue);
  }
}

/* -------------------------------------------------------------- adapter */

NTSTATUS NekoAdapterCreate(NEKO_DEVICE *dev, NETADAPTER_INIT *init) {
  NET_ADAPTER_DATAPATH_CALLBACKS datapath;
  NET_ADAPTER_DATAPATH_CALLBACKS_INIT(&datapath, CreateTxQueue, CreateRxQueue);
  NetAdapterInitSetDatapathCallbacks(init, &datapath);
  NETADAPTER adapter;
  NTSTATUS st = NetAdapterCreate(init, WDF_NO_OBJECT_ATTRIBUTES, &adapter);
  if (!NT_SUCCESS(st)) return st;
  st = WifiAdapterInitialize(adapter);
  if (!NT_SUCCESS(st)) return st;
  dev->Adapter = adapter;
  g_dev = dev;

  NET_ADAPTER_LINK_LAYER_CAPABILITIES ll;
  NET_ADAPTER_LINK_LAYER_CAPABILITIES_INIT(&ll, 866700000ull, 866700000ull);
  NetAdapterSetLinkLayerCapabilities(adapter, &ll);
  NetAdapterSetLinkLayerMtuSize(adapter, 1500);
  NET_ADAPTER_LINK_LAYER_ADDRESS address;
  NET_ADAPTER_LINK_LAYER_ADDRESS_INIT(&address, 6, dev->Address);
  NetAdapterSetPermanentLinkLayerAddress(adapter, &address);
  NetAdapterSetCurrentLinkLayerAddress(adapter, &address);

  NET_ADAPTER_TX_CAPABILITIES tx;
  NET_ADAPTER_TX_CAPABILITIES_INIT(&tx, 1);
  NET_ADAPTER_RX_CAPABILITIES rx;
  NET_ADAPTER_RX_CAPABILITIES_INIT_SYSTEM_MANAGED(&rx, 2400, 1);
  NetAdapterSetDataPathCapabilities(adapter, &tx, &rx);

  st = NetAdapterStart(adapter);
  NEKO_LOG("adapter started: 0x%08x", st);
  return st;
}
