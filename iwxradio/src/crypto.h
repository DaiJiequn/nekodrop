// SPDX-License-Identifier: GPL-2.0-only
/* The WPA2-PSK primitives the station needs, on Windows CNG (bcrypt). */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace crypto {

/* PMK = PBKDF2-HMAC-SHA1(passphrase, ssid, 4096, 32) (IEEE 802.11i). */
bool pmk_from_passphrase(const std::string &passphrase, const std::string &ssid, uint8_t pmk[32]);
bool hmac_sha1(const uint8_t *key, size_t key_len, const uint8_t *data, size_t len, uint8_t out[20]);
bool hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *data, size_t len, uint8_t out[32]);
/* IEEE 802.11 PRF-n with HMAC-SHA1: `out_len` bytes. */
bool prf_sha1(const uint8_t *key, size_t key_len, const char *label, const uint8_t *data,
              size_t data_len, uint8_t *out, size_t out_len);
/* RFC 3394 AES key unwrap. `in_len` is a multiple of 8, at least 24. */
bool aes_unwrap(const uint8_t *kek, size_t kek_len, const uint8_t *in, size_t in_len,
                std::vector<uint8_t> &out);
/* RFC 3394 AES key wrap (used by the self-test against the RFC vectors). */
bool aes_wrap(const uint8_t *kek, size_t kek_len, const uint8_t *in, size_t in_len,
              std::vector<uint8_t> &out);
bool random_bytes(uint8_t *out, size_t len);

} // namespace crypto
