// SPDX-License-Identifier: GPL-2.0-only
/*
 * WDI_TASK_CONNECT and what follows it: open-system authentication and
 * association with the first preferred BSS on the station context (MAC 1,
 * its own PHY and binding beside the AWDL listener, as proven on hardware
 * 2026-11-17), then WDI_INDICATION_ASSOCIATION_RESULT and the connect
 * completion. For WPA2 Windows runs the 4-way handshake over the data path
 * and installs the keys with WDI_SET_ADD_CIPHER_KEYS; the firmware does the
 * CCMP work. Deauthentication from the AP, a disconnect request or a reset
 * tear the station down and report WDI_INDICATION_DISASSOCIATION.
 */
#include "precomp.h"

static ULONGLONG NowUs() { return KeQueryInterruptTime() / 10; }
static const ULONG kStepTimeoutUs = 300000, kStepTries = 4;

static ULONG Header(NEKO_DEVICE *dev, UCHAR *f, UCHAR subtype) {
  RtlZeroMemory(f, 24);
  f[0] = subtype;
  RtlCopyMemory(f + 4, dev->ConnBssid, 6);
  RtlCopyMemory(f + 10, dev->Address, 6);
  RtlCopyMemory(f + 16, dev->ConnBssid, 6);
  const USHORT seq = static_cast<USHORT>(++dev->MgmtSeq << 4);
  f[22] = static_cast<UCHAR>(seq);
  f[23] = static_cast<UCHAR>(seq >> 8);
  return 24;
}

static void SendAuth(NEKO_DEVICE *dev) {
  UCHAR f[30];
  ULONG n = Header(dev, f, 0xb0);
  f[n++] = 0; f[n++] = 0; /* open system */
  f[n++] = 1; f[n++] = 0; /* transaction 1 */
  f[n++] = 0; f[n++] = 0; /* status */
  khal_sta_transmit(f, n, false, true);
}

static void Put(UCHAR *b, ULONG &n, ULONG cap, const void *src, ULONG len) {
  if (n + len > cap) return;
  RtlCopyMemory(b + n, src, len);
  n += len;
}

static void SendAssoc(NEKO_DEVICE *dev) {
  UCHAR f[600];
  ULONG n = Header(dev, f, 0x00);
  UCHAR *body = f + n;
  ULONG b = 0;
  const ULONG cap = sizeof(f) - n;
  const USHORT capability = static_cast<USHORT>(dev->ConnCapability & 0x0431); /* ESS, privacy, short preamble/slot */
  body[b++] = static_cast<UCHAR>(capability);
  body[b++] = static_cast<UCHAR>(capability >> 8);
  body[b++] = 10; body[b++] = 0; /* listen interval */
  const UCHAR ssidIe[2] = {0, static_cast<UCHAR>(dev->ConnSsidLength)};
  Put(body, b, cap, ssidIe, 2);
  Put(body, b, cap, dev->ConnSsid, dev->ConnSsidLength);
  const UCHAR *ies = dev->ConnBeacon + 12;
  const ULONG iesLength = dev->ConnBeaconLength > 12 ? dev->ConnBeaconLength - 12u : 0u;
  UCHAR l = 0;
  static const UCHAR rateIds[] = {1, 50};
  for (UCHAR id : rateIds) { /* the AP's rates, as offered */
    if (const UCHAR *r = NekoFindIe(ies, iesLength, id, &l)) {
      const UCHAR h[2] = {id, l};
      Put(body, b, cap, h, 2);
      Put(body, b, cap, r, l);
    }
  }
  if (dev->ConnRsn) {
    /* WPA2: CCMP pairwise, the AP's group cipher, PSK or 802.1X as chosen. */
    UCHAR group[4] = {0x00, 0x0f, 0xac, 0x04};
    if (const UCHAR *rsn = NekoFindIe(ies, iesLength, 48, &l); rsn && l >= 6) RtlCopyMemory(group, rsn + 2, 4);
    const UCHAR akm = dev->ConnWmm & 0x80 ? 1 : 2; /* high bit borrowed below: 802.1X */
    const UCHAR ie[] = {48, 20, 1, 0, group[0], group[1], group[2], group[3], 1, 0, 0x00, 0x0f, 0xac, 0x04,
                        1,  0,  0x00, 0x0f, 0xac, akm, 0, 0};
    Put(body, b, cap, ie, sizeof(ie));
  }
  if (dev->ConnHt) {
    const UCHAR ht[] = {45, 26,
                        0x6e, 0x01,             /* 20/40, SM PS off, SGI 20/40, RX STBC 1 */
                        0x17,                   /* A-MPDU 64 KiB, 4 us spacing */
                        0xff, 0xff, 0, 0, 0, 0, 0, 0, 0, 0, /* MCS 0-15 */
                        0, 0, 0x01, 0, 0, 0,    /* TX MCS set defined */
                        0, 0, 0, 0, 0, 0, 0};
    Put(body, b, cap, ht, sizeof(ht));
  }
  if (dev->ConnVht) {
    const UCHAR vht[] = {191, 12, 0x20, 0x01, 0x80, 0x03, /* SGI 80, RX STBC 1, A-MPDU exp 7 */
                         0xfa, 0xff, 0, 0, 0xfa, 0xff, 0, 0}; /* 2SS MCS 0-9 */
    Put(body, b, cap, vht, sizeof(vht));
  }
  if (dev->ConnWmm & 1) {
    const UCHAR wmm[] = {0xdd, 7, 0x00, 0x50, 0xf2, 0x02, 0x00, 0x01, 0x00};
    Put(body, b, cap, wmm, sizeof(wmm));
  }
  dev->AssocReqLength = static_cast<USHORT>(b < sizeof(dev->AssocReq) ? b : sizeof(dev->AssocReq));
  RtlCopyMemory(dev->AssocReq, body, dev->AssocReqLength);
  khal_sta_transmit(f, n + b, false, true);
}

static bool HasWmm(const UCHAR *ies, ULONG len) {
  for (ULONG i = 0; i + 2 <= len;) {
    const UCHAR id = ies[i], n = ies[i + 1];
    if (i + 2 + n > len) break;
    if (id == 0xdd && n >= 5 && ies[i + 2] == 0x00 && ies[i + 3] == 0x50 && ies[i + 4] == 0xf2 && ies[i + 5] == 2)
      return true;
    i += 2 + n;
  }
  return false;
}

static void Result(NEKO_DEVICE *dev, WDI_ASSOC_STATUS status, USHORT code) {
  WDI_ASSOCIATION_RESULT_CONTAINER r;
  RtlCopyMemory(r.BSSID.Address, dev->ConnBssid, 6);
  r.AssociationResultParameters.AssociationStatus = status;
  r.AssociationResultParameters.StatusCode = code;
  r.AssociationResultParameters.ReAssociation = FALSE;
  r.AssociationResultParameters.AuthAlgorithm = dev->ConnRsn ? ((dev->ConnWmm & 0x80) ? WDI_AUTH_ALGO_RSNA : WDI_AUTH_ALGO_RSNA_PSK)
                                                             : WDI_AUTH_ALGO_80211_OPEN;
  r.AssociationResultParameters.UnicastCipherAlgorithm = dev->ConnRsn ? WDI_CIPHER_ALGO_CCMP : WDI_CIPHER_ALGO_NONE;
  r.AssociationResultParameters.MulticastDataCipherAlgorithm = dev->ConnRsn ? WDI_CIPHER_ALGO_CCMP : WDI_CIPHER_ALGO_NONE;
  r.AssociationResultParameters.MulticastMgmtCipherAlgorithm = WDI_CIPHER_ALGO_NONE;
  r.AssociationResultParameters.PortAuthorized = !dev->ConnRsn;
  r.AssociationResultParameters.WMMQoSEnabled = (dev->ConnWmm & 1) != 0;
  r.AssociationResultParameters.DSInfo = WDI_DS_UNKNOWN;
  r.AssociationResultParameters.BandID = dev->ConnChannel <= 14 ? WDI_BAND_ID_2400 : WDI_BAND_ID_5000;
  r.Optional.AssociationRequestFrame_IsPresent = dev->AssocReqLength != 0;
  r.AssociationRequestFrame.ElementCount = dev->AssocReqLength;
  r.AssociationRequestFrame.pElements = dev->AssocReq;
  r.Optional.AssociationResponseFrame_IsPresent = dev->AssocRespLength != 0;
  r.AssociationResponseFrame.ElementCount = dev->AssocRespLength;
  r.AssociationResponseFrame.pElements = dev->AssocResp;
  r.Optional.BeaconProbeResponse_IsPresent = dev->ConnBeaconLength != 0;
  r.BeaconProbeResponse.ElementCount = dev->ConnBeaconLength;
  r.BeaconProbeResponse.pElements = dev->ConnBeacon;
  WDI_PHY_TYPE phy = dev->ConnVht ? WDI_PHY_TYPE_VHT : dev->ConnHt ? WDI_PHY_TYPE_HT
                     : dev->ConnChannel <= 14 ? WDI_PHY_TYPE_ERP : WDI_PHY_TYPE_OFDM;
  r.ActivePhyTypeList.ElementCount = 1;
  r.ActivePhyTypeList.pElements = &phy;

  WDI_INDICATION_ASSOCIATION_RESULT_LIST list;
  list.AssociationResults.ElementCount = 1;
  list.AssociationResults.pElements = &r;
  UINT8 *out = nullptr;
  ULONG outLength = 0;
  if (GenerateWdiIndicationAssociationResult(&list, 0, &dev->Tlv, &outLength, &out) == NDIS_STATUS_SUCCESS) {
    NekoIndicate(dev, &dev->ConnHeader, WDI_INDICATION_ASSOCIATION_RESULT, 0, STATUS_SUCCESS, out, outLength);
    FreeGenerated(out);
  }
  /* Nothing above was allocated by the generator's containers: detach. */
  list.AssociationResults.pElements = nullptr;
  list.AssociationResults.ElementCount = 0;
  r.AssociationRequestFrame.pElements = r.AssociationResponseFrame.pElements = r.BeaconProbeResponse.pElements = nullptr;
  r.AssociationRequestFrame.ElementCount = r.AssociationResponseFrame.ElementCount = r.BeaconProbeResponse.ElementCount = 0;
  r.ActivePhyTypeList.pElements = nullptr;
  r.ActivePhyTypeList.ElementCount = 0;
}

static void Complete(NEKO_DEVICE *dev, NTSTATUS status) {
  if (!dev->ConnPending) return;
  dev->ConnPending = FALSE;
  NekoIndicate(dev, &dev->ConnHeader, WDI_INDICATION_CONNECT_COMPLETE, dev->ConnHeader.TransactionId, status, nullptr, 0);
}

static void LinkState(NEKO_DEVICE *dev) {
  WDI_INDICATION_LINK_STATE_CHANGE_PARAMETERS p;
  RtlCopyMemory(p.LinkStateChangeParameters.PeerMACAddress.Address, dev->ConnBssid, 6);
  const UINT32 kbps = dev->ConnVht ? 866700 : dev->ConnHt ? 300000 : 54000;
  p.LinkStateChangeParameters.TxLinkSpeed = kbps;
  p.LinkStateChangeParameters.RxLinkSpeed = kbps;
  const int q = (dev->LastRssi + 90) * 100 / 60;
  p.LinkStateChangeParameters.LinkQuality = static_cast<UINT8>(q < 0 ? 0 : q > 100 ? 100 : q);
  UINT8 *out = nullptr;
  ULONG outLength = 0;
  if (GenerateWdiIndicationLinkStateChange(&p, 0, &dev->Tlv, &outLength, &out) == NDIS_STATUS_SUCCESS) {
    NekoIndicate(dev, &dev->ConnHeader, WDI_INDICATION_LINK_STATE_CHANGE, 0, STATUS_SUCCESS, out, outLength);
    FreeGenerated(out);
  }
}

static void Fail(NEKO_DEVICE *dev, WDI_ASSOC_STATUS status, USHORT code, const char *why) {
  NEKO_LOG("connect failed: %s", why);
  khal_sta_leave();
  dev->Conn = ConnIdle;
  Result(dev, status, code);
  Complete(dev, STATUS_SUCCESS); /* the task ran; the result carries the failure */
}

NTSTATUS NekoConnectStart(NEKO_DEVICE *dev, const WDI_MESSAGE_HEADER *hdr, const WDI_TASK_CONNECT_PARAMETERS *p) {
  if (dev->Conn != ConnIdle) NekoDisconnect(dev, nullptr, TRUE, 3);
  if (!p->PreferredBSSEntryList.ElementCount) return STATUS_INVALID_PARAMETER;
  const WDI_CONNECT_BSS_ENTRY_CONTAINER &e = p->PreferredBSSEntryList.pElements[0];
  RtlCopyMemory(dev->ConnBssid, e.BSSID.Address, 6);
  dev->ConnChannel = static_cast<UCHAR>(e.ChannelInfo.ChannelNumber);
  const WDI_BYTE_BLOB *frame = e.Optional.ProbeResponseFrame_IsPresent ? &e.ProbeResponseFrame
                               : e.Optional.BeaconFrame_IsPresent       ? &e.BeaconFrame
                                                                        : nullptr;
  dev->ConnBeaconLength = 0;
  if (frame && frame->ElementCount >= 12) {
    dev->ConnBeaconLength = static_cast<USHORT>(frame->ElementCount < sizeof(dev->ConnBeacon) ? frame->ElementCount
                                                                                               : sizeof(dev->ConnBeacon));
    RtlCopyMemory(dev->ConnBeacon, frame->pElements, dev->ConnBeaconLength);
  } else if (NEKO_BSS *b = NekoFindBss(dev, dev->ConnBssid)) {
    dev->ConnBeaconLength = b->BodyLength;
    RtlCopyMemory(dev->ConnBeacon, b->Body, b->BodyLength);
  }
  if (dev->ConnBeaconLength < 12) return STATUS_INVALID_PARAMETER;
  dev->ConnBeaconInt = static_cast<USHORT>(dev->ConnBeacon[8] | dev->ConnBeacon[9] << 8);
  dev->ConnCapability = static_cast<USHORT>(dev->ConnBeacon[10] | dev->ConnBeacon[11] << 8);
  const UCHAR *ies = dev->ConnBeacon + 12;
  const ULONG iesLength = dev->ConnBeaconLength - 12u;
  UCHAR l = 0;
  dev->ConnSsidLength = 0;
  if (p->ConnectParameters.SSIDList.ElementCount && p->ConnectParameters.SSIDList.pElements[0].ElementCount <= 32) {
    dev->ConnSsidLength = p->ConnectParameters.SSIDList.pElements[0].ElementCount;
    RtlCopyMemory(dev->ConnSsid, p->ConnectParameters.SSIDList.pElements[0].pElements, dev->ConnSsidLength);
  } else if (const UCHAR *s = NekoFindIe(ies, iesLength, 0, &l); s && l <= 32) {
    dev->ConnSsidLength = l;
    RtlCopyMemory(dev->ConnSsid, s, l);
  }
  dev->ConnRsn = NekoFindIe(ies, iesLength, 48, &l) != nullptr;
  dev->ConnHt = NekoFindIe(ies, iesLength, 45, &l) != nullptr;
  dev->ConnVht = dev->ConnChannel > 14 && NekoFindIe(ies, iesLength, 191, &l) != nullptr;
  dev->ConnWmm = HasWmm(ies, iesLength) ? 1 : 0;
  /* 802.1X when Windows asked for RSNA (enterprise): remembered in bit 7. */
  for (UINT32 i = 0; i < p->ConnectParameters.AuthenticationAlgorithms.ElementCount; i++)
    if (p->ConnectParameters.AuthenticationAlgorithms.pElements[i] == WDI_AUTH_ALGO_RSNA) dev->ConnWmm |= 0x80;

  KhalSta sta = {};
  RtlCopyMemory(sta.bssid, dev->ConnBssid, 6);
  RtlCopyMemory(sta.addr, dev->Address, 6);
  sta.channel = dev->ConnChannel;
  sta.width = 20;
  sta.beacon_int = dev->ConnBeaconInt;
  if (!khal_sta_join(&sta)) return STATUS_DEVICE_NOT_READY;
  dev->ConnHeader = *hdr;
  dev->ConnPending = TRUE;
  dev->PairwiseKey = FALSE;
  dev->AssocReqLength = dev->AssocRespLength = 0;
  dev->Conn = ConnAuth;
  dev->ConnTries = 1;
  dev->ConnDeadlineUs = NowUs() + kStepTimeoutUs;
  NEKO_LOG("connect: %02x:%02x:%02x:%02x:%02x:%02x channel %u%s%s%s", dev->ConnBssid[0], dev->ConnBssid[1],
           dev->ConnBssid[2], dev->ConnBssid[3], dev->ConnBssid[4], dev->ConnBssid[5], dev->ConnChannel,
           dev->ConnRsn ? " RSN" : "", dev->ConnHt ? " HT" : "", dev->ConnVht ? " VHT" : "");
  SendAuth(dev);
  return STATUS_SUCCESS;
}

void NekoConnectTick(NEKO_DEVICE *dev) {
  const ULONGLONG now = NowUs();
  if (dev->Conn == ConnUp) {
    if (now - dev->LinkReportUs > 10000000) {
      dev->LinkReportUs = now;
      LinkState(dev);
    }
    return;
  }
  if ((dev->Conn != ConnAuth && dev->Conn != ConnAssoc) || now < dev->ConnDeadlineUs) return;
  if (dev->ConnTries >= kStepTries) {
    Fail(dev, WDI_ASSOC_STATUS_FAILURE, 0, dev->Conn == ConnAuth ? "no answer to authentication" : "no answer to association");
    return;
  }
  dev->ConnTries++;
  dev->ConnDeadlineUs = now + kStepTimeoutUs;
  if (dev->Conn == ConnAuth) SendAuth(dev);
  else SendAssoc(dev);
}

void NekoConnectOnFrame(NEKO_DEVICE *dev, const KhalRx *f) {
  if (dev->Conn == ConnIdle || f->len < 24) return;
  if (!RtlEqualMemory(f->data + 4, dev->Address, 6) || !RtlEqualMemory(f->data + 10, dev->ConnBssid, 6)) return;
  const UCHAR sub = f->data[0] & 0xf0;
  const UCHAR *body = f->data + 24;
  const size_t bodyLength = f->len - 24;
  if (sub == 0xb0 && dev->Conn == ConnAuth && bodyLength >= 6) {
    const USHORT seq = static_cast<USHORT>(body[2] | body[3] << 8), status = static_cast<USHORT>(body[4] | body[5] << 8);
    if (seq != 2) return;
    if (status) {
      Fail(dev, WDI_ASSOC_STATUS_FAILURE, status, "authentication refused");
      return;
    }
    dev->Conn = ConnAssoc;
    dev->ConnTries = 1;
    dev->ConnDeadlineUs = NowUs() + kStepTimeoutUs;
    SendAssoc(dev);
  } else if ((sub == 0x10 || sub == 0x30) && dev->Conn == ConnAssoc && bodyLength >= 6) {
    const USHORT status = static_cast<USHORT>(body[2] | body[3] << 8);
    const USHORT aid = static_cast<USHORT>((body[4] | body[5] << 8) & 0x3fff);
    dev->AssocRespLength = static_cast<USHORT>(bodyLength < sizeof(dev->AssocResp) ? bodyLength : sizeof(dev->AssocResp));
    RtlCopyMemory(dev->AssocResp, body, dev->AssocRespLength);
    if (status) {
      Fail(dev, WDI_ASSOC_STATUS_FAILURE, status, "association refused");
      return;
    }
    /* What both sides support: the response's HT/VHT elements. */
    UCHAR l = 0;
    const UCHAR *ies = body + 6;
    const ULONG iesLength = static_cast<ULONG>(bodyLength - 6);
    KhalSta agreed = {};
    RtlCopyMemory(agreed.bssid, dev->ConnBssid, 6);
    RtlCopyMemory(agreed.addr, dev->Address, 6);
    agreed.channel = dev->ConnChannel;
    agreed.beacon_int = dev->ConnBeaconInt;
    agreed.ht = dev->ConnHt && NekoFindIe(ies, iesLength, 45, &l);
    agreed.vht = dev->ConnVht && NekoFindIe(ies, iesLength, 191, &l);
    const UCHAR *htop = NekoFindIe(ies, iesLength, 61, &l);
    const bool wide40 = htop && l >= 2 && (htop[1] & 0x04);
    const UCHAR *vhtop = NekoFindIe(ies, iesLength, 192, &l);
    const bool wide80 = agreed.vht && vhtop && l >= 1 && vhtop[0] >= 1;
    agreed.width = wide80 ? 80 : wide40 ? 40 : 20;
    agreed.mimo = true;
    const UCHAR *tim = NekoFindIe(dev->ConnBeacon + 12, dev->ConnBeaconLength - 12u, 5, &l);
    const UCHAR dtim = tim && l >= 2 ? tim[1] : 1;
    if (!khal_sta_associated(&agreed, aid, dtim)) {
      Fail(dev, WDI_ASSOC_STATUS_FAILURE, 0, "the firmware refused the association");
      return;
    }
    khal_sta_rate_control();
    dev->ConnHt = agreed.ht;
    dev->ConnVht = agreed.vht;
    dev->Conn = ConnUp;
    dev->LinkReportUs = NowUs();
    if (NEKO_BSS *b = NekoFindBss(dev, dev->ConnBssid)) dev->LastRssi = b->Rssi;
    NEKO_LOG("associated, AID %u, %u MHz%s%s", aid, agreed.width, agreed.ht ? " HT" : "", agreed.vht ? " VHT" : "");
    Result(dev, WDI_ASSOC_STATUS_SUCCESS, 0);
    Complete(dev, STATUS_SUCCESS);
    LinkState(dev);
  } else if (sub == 0xc0 || sub == 0xa0) { /* deauthentication, disassociation */
    const USHORT reason = bodyLength >= 2 ? static_cast<USHORT>(body[0] | body[1] << 8) : 0;
    NEKO_LOG("the access point disconnected us, reason %u", reason);
    if (dev->Conn != ConnUp) {
      Fail(dev, WDI_ASSOC_STATUS_FAILURE, reason, "refused during association");
      return;
    }
    NekoDisconnect(dev, nullptr, FALSE, reason);
  }
}

void NekoDisconnect(NEKO_DEVICE *dev, const WDI_MESSAGE_HEADER *hdr, BOOLEAN byHost, USHORT reason) {
  const BOOLEAN wasUp = dev->Conn == ConnUp;
  if (byHost && dev->Conn != ConnIdle) {
    UCHAR f[26];
    ULONG n = Header(dev, f, 0xc0);
    f[n++] = static_cast<UCHAR>(reason);
    f[n++] = static_cast<UCHAR>(reason >> 8);
    khal_sta_transmit(f, n, false, true);
  }
  khal_sta_leave();
  dev->Conn = ConnIdle;
  dev->PairwiseKey = FALSE;
  if (dev->ConnPending) {
    Result(dev, WDI_ASSOC_STATUS_FAILURE, reason);
    Complete(dev, STATUS_SUCCESS);
  }
  if (!wasUp) return;
  WDI_INDICATION_DISASSOCIATION_PARAMETERS p;
  RtlCopyMemory(p.DisconnectIndicationParameters.MacAddress.Address, dev->ConnBssid, 6);
  p.DisconnectIndicationParameters.DisassociationWABIReason =
      byHost ? WDI_ASSOC_STATUS_DISASSOCIATED_BY_HOST : WDI_ASSOC_STATUS_PEER_DEAUTHENTICATED;
  UINT8 *out = nullptr;
  ULONG outLength = 0;
  if (GenerateWdiIndicationDisassociation(&p, 0, &dev->Tlv, &outLength, &out) == NDIS_STATUS_SUCCESS) {
    NekoIndicate(dev, hdr ? hdr : &dev->ConnHeader, WDI_INDICATION_DISASSOCIATION, 0, STATUS_SUCCESS, out, outLength);
    FreeGenerated(out);
  }
}

NTSTATUS NekoAddKeys(NEKO_DEVICE *dev, const WDI_SET_ADD_CIPHER_KEYS_PARAMETERS *p) {
  for (UINT32 i = 0; i < p->SetCipherKey.ElementCount; i++) {
    const WDI_SET_ADD_CIPHER_KEYS_CONTAINER &k = p->SetCipherKey.pElements[i];
    if (k.CipherKeyTypeInfo.CipherAlgorithm != WDI_CIPHER_ALGO_CCMP || !k.Optional.CCMPKey_IsPresent ||
        k.CCMPKey.ElementCount != 16) {
      NEKO_LOG("key: cipher %u not supported", k.CipherKeyTypeInfo.CipherAlgorithm);
      return STATUS_NOT_SUPPORTED;
    }
    const bool group = k.CipherKeyTypeInfo.KeyType == WDI_CIPHER_KEY_TYPE_GROUP_KEY;
    const unsigned index = k.Optional.CipherKeyID_IsPresent ? k.CipherKeyID.CipherKeyID : 0;
    if (!khal_sta_set_key(group, index, k.CCMPKey.pElements)) return STATUS_UNSUCCESSFUL;
    if (!group) dev->PairwiseKey = TRUE;
    NEKO_LOG("key: %s key %u installed", group ? "group" : "pairwise", index);
  }
  return STATUS_SUCCESS;
}
