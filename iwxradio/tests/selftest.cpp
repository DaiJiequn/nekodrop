// SPDX-License-Identifier: GPL-2.0-only
/* Checks that need no card: the firmware ABI struct sizes the driver depends
 * on, and that each bundled Qu/QuZ firmware parses into the section layout
 * and command versions the bring-up expects.
 * Usage: iwx_selftest <firmware dir> */
#include <cstdio>
#include <string>

#include "../src/iwxreg.h"
#include "../src/cards.h"
#include "../src/fw.h"
#include "../src/awdl_policy.h"
#include "../src/crypto.h"
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
  /* AX210 family */
  EXPECT_SIZE(iwx_tx_cmd_gen3, 28);          /* TX_CMD_API_S_VER_8 */
  EXPECT_SIZE(iwx_rx_transfer_desc, 16);
  EXPECT_SIZE(iwx_rx_completion_desc, 32);
  EXPECT_SIZE(iwx_gen3_bc_tbl, 2048);
  EXPECT_SIZE(iwx_context_info_gen3, 104);   /* IPC_CONTEXT_INFO_S */
  EXPECT_SIZE(iwx_alive_resp_v5, 0x74 + 12); /* v4 + sku_id */

  /* Card selection (cards.h), with register values as iwx keeps them. */
  struct CardCase { uint16_t dev; uint32_t hw_rev, rf_id; const char *fw; bool ax210, integrated; };
  const CardCase cases[] = {
      {0x43F0, 0x354, 0x0010a100, "iwlwifi-QuZ-a0-hr-b0-68.ucode", false, true}, /* this project's AX201 */
      {0xA0F0, 0x334, 0x0010a100, "iwlwifi-Qu-b0-hr-b0-68.ucode", false, true},
      {0x34F0, 0x338, 0x0010a100, "iwlwifi-Qu-c0-hr-b0-68.ucode", false, true},
      {0x02F0, 0x354, 0x00108000, "iwlwifi-QuZ-a0-jf-b0-68.ucode", false, true},  /* 9560 on QuZ */
      {0x2723, 0x340, 0x0010a100, "iwlwifi-cc-a0-68.ucode", false, false},        /* AX200 */
      {0x2725, 0x420, 0x0010d000, "iwlwifi-ty-a0-gf-a0-68.ucode", true, false},    /* AX210: discrete */
      {0x7AF0, 0x370, 0x0010d000, "iwlwifi-so-a0-gf-a0-68.ucode", true, true},     /* AX211 */
      {0x51F0, 0x370, 0x1010d000, "iwlwifi-so-a0-gf4-a0-68.ucode", true, true},    /* AX411 */
      {0x2726, 0x420, 0x0010d000, nullptr, true, true},                             /* SnJ: no firmware */
      {0x1234, 0x354, 0x0010a100, nullptr, false, true},
  };
  for (const auto &k : cases) {
    const CardInfo c = cards::select(k.dev, k.hw_rev, k.rf_id);
    const bool ok = k.fw ? (c.known && c.firmware == k.fw && c.ax210 == k.ax210 && c.integrated == k.integrated)
                         : !c.known;
    std::printf("%s card %04x rev %03x rf %08x -> %s%s\n", ok ? "ok  " : "FAIL", k.dev, k.hw_rev, k.rf_id,
                c.known ? c.firmware.c_str() : "unsupported: ", c.known ? "" : c.why.c_str());
    if (!ok) failures++;
  }
  {
    const CardInfo c = cards::select(0x43F0, 0x354, 0x0010a100);
    if (c.xtal_latency != 12000 || c.ltr_delay != IWX_SOC_FLAGS_LTR_APPLY_DELAY_2500 || !c.low_latency_xtal || !c.tested) {
      std::printf("FAIL the tested AX201 lost its transport parameters\n");
      failures++;
    }
  }
  {
    /* The Raptor Lake AX211 (issue #2): Linux's so_long_latency_imr parameters. */
    const CardInfo c = cards::select(0x7A70, 0x370, 0x0010d000);
    if (!c.known || !c.integrated || !c.imr || !c.low_latency_xtal || c.xtal_latency != 12000) {
      std::printf("FAIL the 7A70 AX211 lost its IMR / long-latency parameters\n");
      failures++;
    }
  }
  std::printf("info IWX_RX_DESC_SIZE_V1 = %zu, sizeof(iwx_context_info) = %zu, "
              "sizeof(iwx_mac_ctx_cmd) = %zu\n",
              (size_t)IWX_RX_DESC_SIZE_V1, sizeof(iwx_context_info), sizeof(iwx_mac_ctx_cmd));

  if (argc > 1) {
    const char *names[] = {"iwlwifi-Qu-b0-hr-b0-68.ucode", "iwlwifi-Qu-c0-hr-b0-68.ucode",
                           "iwlwifi-QuZ-a0-hr-b0-68.ucode", "iwlwifi-Qu-b0-jf-b0-68.ucode",
                           "iwlwifi-Qu-c0-jf-b0-68.ucode", "iwlwifi-QuZ-a0-jf-b0-68.ucode",
                           "iwlwifi-cc-a0-68.ucode", "iwlwifi-so-a0-gf-a0-68.ucode",
                           "iwlwifi-so-a0-gf4-a0-68.ucode", "iwlwifi-so-a0-hr-b0-68.ucode",
                           "iwlwifi-so-a0-jf-b0-68.ucode", "iwlwifi-ty-a0-gf-a0-68.ucode"};
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
      const bool ax210 = std::strncmp(name, "iwlwifi-so-", 11) == 0 || std::strncmp(name, "iwlwifi-ty-", 11) == 0;
      if (ax210 && !fw.iml_len) {
        std::printf("FAIL %s: an AX210-family firmware without an image loader\n", name);
        failures++;
      }
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

  /* WPA2 crypto against published vectors. */
  {
    const auto hex_eq = [](const uint8_t *p, const char *hex) {
      for (size_t i = 0; hex[2 * i]; i++) {
        unsigned v;
        std::sscanf(hex + 2 * i, "%2x", &v);
        if (p[i] != v) return false;
      }
      return true;
    };
    uint8_t pmk[32];
    CHECK(crypto::pmk_from_passphrase("password", "IEEE", pmk) &&
              hex_eq(pmk, "f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e"),
          "PBKDF2 PMK matches IEEE 802.11i H.4 (\"password\"/\"IEEE\")");
    uint8_t key20[20];
    std::memset(key20, 0x0b, 20);
    uint8_t prf[64];
    const char *hi = "Hi There";
    CHECK(crypto::prf_sha1(key20, 20, "prefix", reinterpret_cast<const uint8_t *>(hi), 8, prf, 64) &&
              hex_eq(prf, "bcd4c650b30b9684951829e0d75f9d54b862175ed9f00606e17d8da35402ffee"
                          "75df78c3d31e0f889f012120c0862beb67753e7439ae242edb8373698356cf5a"),
          "PRF-512 matches IEEE 802.11i H.3 test 1");
    uint8_t kek[16], plain[16];
    for (int i = 0; i < 16; i++) {
      kek[i] = static_cast<uint8_t>(i);
      plain[i] = static_cast<uint8_t>(0x11 * i);
    }
    std::vector<uint8_t> wrapped, back;
    CHECK(crypto::aes_wrap(kek, 16, plain, 16, wrapped) &&
              hex_eq(wrapped.data(), "1fa68b0a8112b447aef34bd8fb5a7b829d3e862371d2cfe5"),
          "AES key wrap matches RFC 3394 4.1");
    CHECK(crypto::aes_unwrap(kek, 16, wrapped.data(), wrapped.size(), back) && back.size() == 16 &&
              std::memcmp(back.data(), plain, 16) == 0,
          "AES key unwrap round-trips");
    wrapped[3] ^= 1;
    CHECK(!crypto::aes_unwrap(kek, 16, wrapped.data(), wrapped.size(), back),
          "AES key unwrap rejects a corrupted key");
  }
  std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
  return failures ? 1 : 0;
}
