// SPDX-License-Identifier: GPL-2.0-only
/* Checks that need no card: the firmware ABI struct sizes the driver depends
 * on, and that each bundled Qu/QuZ firmware parses into the section layout
 * and command versions the bring-up expects.
 * Usage: iwx_selftest <firmware dir> */
#include <cstdio>
#include <string>

#include "../src/iwxreg.h"
#include "../src/fw.h"

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
  std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
  return failures ? 1 : 0;
}
