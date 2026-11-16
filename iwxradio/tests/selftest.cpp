// SPDX-License-Identifier: GPL-2.0-only
/* Checks that need no card: the firmware ABI struct sizes the driver depends
 * on, and that each bundled Qu/QuZ firmware parses into the section layout
 * and command versions the bring-up expects.
 * Usage: iwx_selftest <firmware dir> */
#include <cstdio>
#include <string>

#include "../src/iwxreg.h"
#include "../src/fw.h"
#include "../src/awdl_policy.h"
#include <cstring>
#include <vector>

static int failures = 0;

#define EXPECT_SIZE(type, n)                                                            \
  do {                                                                                  \
    if (sizeof(type) != (n)) {                                                          \
      std::printf("FAIL sizeof(%s) = %zu, expected %d\n", #type, sizeof(type), (n));    \
      failures++;                                                                       \
    } else {                                                                            \
      std::printf("ok   sizeof(%s) = %d\n", #type, (n));                                \
    }                                                                                   \
  } while (0)

int main(int argc, char **argv) {
  /* Sizes from Linux iwlwifi's fw/api headers for the same API versions. */
  EXPECT_SIZE(iwx_tfh_tb, 10);
  EXPECT_SIZE(iwx_tfh_tfd, 256);
  EXPECT_SIZE(iwx_cmd_header, 4);
  EXPECT_SIZE(iwx_cmd_header_wide, 8);
  EXPECT_SIZE(iwx_device_cmd, 324);
  EXPECT_SIZE(iwx_tx_cmd_gen2, 20);
  EXPECT_SIZE(iwx_rb_status, 12);
  EXPECT_SIZE(iwx_tx_queue_cfg_cmd, 24);
  EXPECT_SIZE(iwx_tx_queue_cfg_rsp, 8);
  EXPECT_SIZE(iwx_binding_cmd, 28);
  EXPECT_SIZE(iwx_phy_context_cmd, 32);
  EXPECT_SIZE(iwx_add_sta_cmd, 48); /* ADD_STA_CMD_API_S_VER_10 */
  EXPECT_SIZE(iwx_mcc_update_cmd, 28);
  EXPECT_SIZE(iwx_alive_resp_v4, 0x74);
  EXPECT_SIZE(iwx_tx_resp, 44);
  std::printf("info IWX_RX_DESC_SIZE_V1 = %zu, sizeof(iwx_context_info) = %zu, "
              "sizeof(iwx_mac_ctx_cmd) = %zu\n",
              (size_t)IWX_RX_DESC_SIZE_V1, sizeof(iwx_context_info), sizeof(iwx_mac_ctx_cmd));

  if (argc > 1) {
    const char *names[] = {"iwlwifi-Qu-b0-hr-b0-68.ucode", "iwlwifi-Qu-c0-hr-b0-68.ucode",
                           "iwlwifi-QuZ-a0-hr-b0-68.ucode"};
    for (const char *name : names) {
      Firmware fw;
      std::string error;
      const std::string path = std::string(argv[1]) + "\\" + name;
      if (!fw.load(path, error)) {
        std::printf("FAIL %s: %s\n", name, error.c_str());
        failures++;
        continue;
      }
      size_t lmac = 0, umac = 0, paging = 0, seps = 0;
      for (const auto &s : fw.regular) {
        if (s.devoff == IWX_CPU1_CPU2_SEPARATOR_SECTION || s.devoff == IWX_PAGING_SEPARATOR_SECTION)
          seps++;
        else if (seps == 0) lmac++;
        else if (seps == 1) umac++;
        else paging++;
      }
      size_t largest = 0;
      for (const auto &s : fw.regular) largest = s.len > largest ? s.len : largest;
      std::printf("ok   %s: version %s, %zu LMAC / %zu UMAC / %zu paging sections, largest %zu "
                  "bytes, phy_config 0x%x\n",
                  name, fw.version.c_str(), lmac, umac, paging, largest, fw.phy_config);
      std::printf("     cmd versions: TX_CMD %u, PHY_CONTEXT %u, ADD_STA %u, SCAN_REQ_UMAC %u, "
                  "MAC_CONTEXT %u; DQA %d, LAR %d, CDB %d, UHB %d, NVM_INFO_v4 %d\n",
                  fw.cmd_version(IWX_LONG_GROUP, IWX_TX_CMD),
                  fw.cmd_version(IWX_LONG_GROUP, IWX_PHY_CONTEXT_CMD),
                  fw.cmd_version(IWX_LONG_GROUP, IWX_ADD_STA),
                  fw.cmd_version(IWX_LONG_GROUP, IWX_SCAN_REQ_UMAC),
                  fw.cmd_version(IWX_LONG_GROUP, IWX_MAC_CONTEXT_CMD),
                  fw.has_capa(IWX_UCODE_TLV_CAPA_DQA_SUPPORT), fw.has_capa(IWX_UCODE_TLV_CAPA_LAR_SUPPORT),
                  fw.has_capa(IWX_UCODE_TLV_CAPA_CDB_SUPPORT),
                  fw.has_capa(IWX_UCODE_TLV_CAPA_ULTRA_HB_CHANNELS),
                  fw.has_api(IWX_UCODE_TLV_API_REGULATORY_NVM_INFO));
      if (largest > (2u << 20)) {
        std::printf("FAIL a section is larger than one 2 MiB DMA block\n");
        failures++;
      }
    }
  }

  /* AWDL policy (ported from ForbiddenItlwm), on a captured iPhone frame. */
  {
#define CHECK(cond, what)                                                                   \
  do {                                                                                      \
    if (cond) std::printf("ok   %s\n", what);                                               \
    else {                                                                                  \
      std::printf("FAIL %s\n", what);                                                       \
      failures++;                                                                           \
    }                                                                                       \
  } while (0)
    /* TLV 18 as an iPhone in Japan sent it on 2026-11-16, and TLV 12 "JP". */
    const uint8_t tlv18[] = {0x0f, 0x03, 0x00, 0x03, 0xff, 0xff, 0x2c, 0x80, 0x2c, 0x80, 0x2c, 0x80,
                             0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2c, 0x80, 0x06, 0x51,
                             0x2c, 0x80, 0x2c, 0x80, 0x2c, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                             0x2c, 0x80, 0x00, 0x00, 0x00};
    std::vector<uint8_t> f(40, 0);
    f[0] = 0xd0; f[24] = 0x7f; f[25] = 0x00; f[26] = 0x17; f[27] = 0xf2; f[28] = 0x08;
    f.push_back(18); f.push_back(sizeof(tlv18)); f.push_back(0);
    f.insert(f.end(), tlv18, tlv18 + sizeof(tlv18));
    const uint8_t tlv12[] = {12, 4, 0, 0, 0, 'J', 'P'};
    f.insert(f.end(), tlv12, tlv12 + sizeof(tlv12));
    CHECK(awdl::is_awdl_action(f.data(), f.size()), "AWDL action frame recognised");
    uint8_t seq[16];
    CHECK(awdl::peer_sequence(f.data(), f.size(), seq), "channel sequence parsed");
    CHECK(awdl::peer_channel(seq) == 44, "iPhone in Japan: peer 5 GHz channel is 44");
    char c[3];
    CHECK(awdl::peer_country(f.data(), f.size(), c) && std::string(c) == "JP", "peer country JP");
    awdl::PeerCountry pc;
    bool changed = false;
    for (int k = 0; k < 5; k++) changed |= pc.note(c);
    CHECK(changed && std::string(pc.confirmed) == "JP", "country confirmed after 5 frames");
    uint8_t s149[16] = {149, 149, 149, 0, 0, 0, 0, 0, 6, 149, 149, 149, 0, 0, 0, 0};
    CHECK(awdl::peer_channel(s149) == 149, "sequence naming 149 gives 149");
    uint8_t wifi44[16] = {149, 149, 0, 0, 0, 0, 44, 44, 149, 149, 0, 0, 0, 0, 44, 44};
    CHECK(awdl::peer_channel(wifi44) == 149, "44 only in Wi-Fi slots, 149 named: 149");

    /* ADDBA request -> response round trip. */
    const uint8_t own[6] = {0x02, 1, 2, 3, 4, 5}, peer[6] = {0x66, 0x71, 0xa5, 0xbe, 0xeb, 0xb3};
    uint8_t req[33] = {};
    req[0] = 0xd0;
    std::memcpy(req + 4, own, 6);
    std::memcpy(req + 10, peer, 6);
    req[24] = 3; req[25] = 0; req[26] = 7;
    const uint16_t params = uint16_t(2 | (5 << 2) | (64 << 6));
    req[27] = uint8_t(params); req[28] = uint8_t(params >> 8);
    req[31] = uint8_t(123 << 4); req[32] = uint8_t(123 >> 4);
    awdl::AddbaRequest r;
    CHECK(awdl::parse_addba_request(req, sizeof(req), own, &r) && r.tid == 5 && r.window == 64 &&
              r.ssn == 123 && r.token == 7 && r.immediate,
          "ADDBA request parsed (tid 5, window 64, ssn 123)");
    uint8_t ans[33];
    awdl::build_addba_response(ans, r, own, 9);
    CHECK(ans[25] == 1 && ans[26] == 7 && std::memcmp(ans + 4, peer, 6) == 0 &&
              (((ans[29] | ans[30] << 8) >> 2) & 15) == 5,
          "ADDBA response built for the same token and tid");

    awdl::BaFilter bf;
    bf.note(1000000, 0);
    CHECK(bf.note(2000001, 40) && bf.off, "busy data turns promiscuous off");
    bf.note(3000002, 41); bf.note(4000003, 42);
    CHECK(bf.note(5000004, 43) && !bf.off, "three quiet seconds turn it back on");

    awdl::RateLadder rl;
    for (int k = 0; k < 64; k++) rl.note(true, false);
    CHECK(rl.step == 1, "64 clean frames step the rate up");
    for (int k = 0; k < 64; k++) rl.note(k % 3 != 0, false);
    CHECK(rl.step == 0, "a third lost steps it back down");
    for (int k = 0; k < 64 * 8; k++) rl.note(true, false);
    CHECK(rl.step == 3, "20 MHz tops out at HT MCS 7");
    for (int k = 0; k < 64 * 8; k++) rl.note(true, true);
    CHECK(rl.step == 5, "80 MHz reaches VHT 2x2 MCS 7");
  }
  std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
  return failures ? 1 : 0;
}
