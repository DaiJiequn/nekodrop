// SPDX-License-Identifier: GPL-2.0-only
/* What the OpenBSD/itlwm register header (third_party/iwx/if_iwxreg.h) expects
 * from its kernel environment, for a user-mode Windows build (MSVC or clang-cl).
 * x86-64 is little-endian, so the byte-order helpers are identities. */
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#if defined(__clang__) || defined(__GNUC__)
#define __packed __attribute__((packed))
#define __aligned(x) __attribute__((aligned(x)))
#else
/* MSVC: iwxreg.h wraps the register header in #pragma pack(1). */
#define __packed
#define __aligned(x)
#endif

/* Only used as a flexible array type in the register header. */
struct ieee80211_frame {
  uint8_t i_fc[2];
  uint8_t i_dur[2];
  uint8_t i_addr1[6];
  uint8_t i_addr2[6];
  uint8_t i_addr3[6];
  uint8_t i_seq[2];
} __packed;

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef uint16_t __le16;
typedef uint32_t __le32;
typedef uint64_t __le64;
typedef uint16_t __be16;
typedef uint32_t __be32;
typedef uint8_t u_int8_t;
typedef uint16_t u_int16_t;
typedef uint32_t u_int32_t;
typedef uint64_t u_int64_t;

#define htole16(x) ((uint16_t)(x))
#define htole32(x) ((uint32_t)(x))
#define htole64(x) ((uint64_t)(x))
#define le16toh(x) ((uint16_t)(x))
#define le32toh(x) ((uint32_t)(x))
#define le64toh(x) ((uint64_t)(x))
#define letoh16 le16toh
#define letoh32 le32toh
#define letoh64 le64toh
#define le16_to_cpu(x) le16toh(x)
#define le32_to_cpu(x) le32toh(x)
static inline uint16_t le16_to_cpup(const uint16_t *p) { return *p; }
static inline uint32_t le32_to_cpup(const uint32_t *p) { return *p; }

#ifndef BIT
#define BIT(n) (1U << (n))
#endif
#ifndef BIT_ULL
#define BIT_ULL(n) (1ULL << (n))
#endif
#define ETHER_ADDR_LEN 6
#define IEEE80211_ADDR_LEN 6
#define IEEE80211_NWID_LEN 32
#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif

#define offsetofend(type, member)                                              \
  (offsetof(type, member) + sizeof(((type *)0)->member))
#define nitems(a) (sizeof(a) / sizeof((a)[0]))
#define howmany(x, y) (((x) + ((y)-1)) / (y))
#define roundup(x, y) ((((x) + ((y)-1)) / (y)) * (y))
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

/* BSD find-last-set: 1-based index of the highest set bit, 0 for none. */
static inline int _fls(uint32_t x) {
  int n = 0;
  while (x) { n++; x >>= 1; }
  return n;
}
static inline int flsl(uint64_t x) {
  int n = 0;
  while (x) { n++; x >>= 1; }
  return n;
}

static inline void setbit_u8(uint8_t *a, unsigned i) { a[i / 8] |= 1u << (i % 8); }
static inline bool isset_u8(const uint8_t *a, unsigned i) {
  return (a[i / 8] >> (i % 8)) & 1u;
}

/* Values from itlwm's if_iwxvar.h that this driver uses; that header itself
 * pulls in net80211 and IOKit, so the few constants are repeated here. */
#define IWX_RBUF_SIZE 4096
#define IWX_RX_MQ_RING_COUNT 512
#define IWX_CMD_RESP_MAX PAGE_SIZE
#define IWX_STATION_ID 0
#define IWX_AUX_STA_ID 1
#define IWX_MONITOR_STA_ID 2
#define IWX_DEVICE_FAMILY_22000 1
#define IWX_DEVICE_FAMILY_AX210 2
#define CSR_HW_REV_TYPE(_val) (((_val)&0x000FFF0) >> 4)
#define CSR_HW_REV_TYPE_QU_B0 (0x0000334)
#define CSR_HW_REV_TYPE_QU_C0 (0x0000338)
#define CSR_HW_REV_TYPE_QUZ (0x0000354)
enum iwx_ucode_type {
  IWX_UCODE_TYPE_REGULAR,
  IWX_UCODE_TYPE_INIT,
  IWX_UCODE_TYPE_WOW,
  IWX_UCODE_TYPE_REGULAR_USNIFFER,
  IWX_UCODE_TYPE_MAX
};
#define IWX_INIT_COMPLETE 0x01
#define IWX_CALIB_COMPLETE 0x02
#define IWX_PNVM_COMPLETE 0x04
