#include "WeapiCrypto.h"

#include <mbedtls/aes.h>
#include <mbedtls/base64.h>
#include <mbedtls/bignum.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#include "esp_random.h"

namespace WeapiCrypto {

namespace {

constexpr const char* PRESET_KEY = "0CoJUm6Qyw8W8jud";
constexpr const char* IV = "0102030405060708";
constexpr const char* BASE62 =
    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";

// NetEase web-client RSA modulus (hex).
constexpr const char* MODULUS =
    "00e0b509f6259df8642dbc35662901477df22677ec152b5ff68ace615bb7b725152b3a"
    "b17a876aea8a5aa76d2e417629ec4ee341f56135fccf695280104e0312ecbda92557c93"
    "870114af6c9d05c4f7f0c3685b7a46bee255932575cce10b424d813cfe4875d3e82047"
    "b97ddef52741d546b8e289dc6935b3ece0462db0a22b8e7";

std::string makeSecret() {
  std::string secret;
  secret.reserve(16);
  for (int i = 0; i < 16; ++i) secret += BASE62[esp_random() % 62];
  return secret;
}

// AES-128-CBC with PKCS7 padding, then standard base64 (no trailing newline).
bool aesCbcBase64(const std::string& input, const uint8_t* key,
                  std::string& out) {
  // PKCS7 pad to a full block (always pads, so an exact multiple gains a block).
  const size_t pad = 16 - input.size() % 16;
  std::string padded = input;
  padded.append(pad, static_cast<char>(pad));

  std::vector<uint8_t> cipher(padded.size());

  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  int ret = mbedtls_aes_setkey_enc(&aes, key, 128);
  if (ret == 0) {
    uint8_t iv[16];
    std::memcpy(iv, IV, sizeof(iv));
    ret = mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_ENCRYPT, padded.size(), iv,
                               reinterpret_cast<const uint8_t*>(padded.data()),
                               cipher.data());
  }
  mbedtls_aes_free(&aes);
  if (ret != 0) return false;

  size_t needed = 0;
  ret = mbedtls_base64_encode(nullptr, 0, &needed, cipher.data(), cipher.size());
  if (ret != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL || needed == 0) return false;

  out.resize(needed);
  size_t written = 0;
  ret = mbedtls_base64_encode(reinterpret_cast<uint8_t*>(out.data()),
                              out.size(), &written, cipher.data(),
                              cipher.size());
  if (ret != 0) return false;
  // mbedTLS writes a terminating NUL counted in `written`; drop it.
  out.resize(strlen(out.c_str()));
  return true;
}

// Textbook RSA over the reversed secret key; 256 lowercase hex chars.
std::string rsaEncryptSecret(const std::string& secret) {
  std::string reversed = secret;
  std::reverse(reversed.begin(), reversed.end());

  // M = the 16 reversed bytes as an integer.
  std::string mHex;
  mHex.reserve(32);
  char byte[3];
  for (char c : reversed) {
    std::snprintf(byte, sizeof(byte), "%02x", static_cast<unsigned char>(c));
    mHex += byte;
  }

  mbedtls_mpi n, m, e, r;
  mbedtls_mpi_init(&n);
  mbedtls_mpi_init(&m);
  mbedtls_mpi_init(&e);
  mbedtls_mpi_init(&r);

  std::string enc;
  if (mbedtls_mpi_read_string(&n, 16, MODULUS) == 0 &&
      mbedtls_mpi_read_string(&m, 16, mHex.c_str()) == 0 &&
      mbedtls_mpi_lset(&e, 65537) == 0 &&
      mbedtls_mpi_exp_mod(&r, &m, &e, &n, nullptr) == 0) {
    char buffer[300];
    size_t length = 0;
    if (mbedtls_mpi_write_string(&r, 16, buffer, sizeof(buffer), &length) == 0 &&
        length > 0) {
      enc.assign(buffer, length - 1);  // drop the NUL terminator
      if (enc.size() < 256) enc = std::string(256 - enc.size(), '0') + enc;
    }
  }

  mbedtls_mpi_free(&n);
  mbedtls_mpi_free(&m);
  mbedtls_mpi_free(&e);
  mbedtls_mpi_free(&r);
  return enc;
}

}  // namespace

bool encrypt(const std::string& jsonBody, std::string& params,
             std::string& encSecKey) {
  const std::string secret = makeSecret();

  std::string first;
  if (!aesCbcBase64(jsonBody, reinterpret_cast<const uint8_t*>(PRESET_KEY),
                    first)) {
    return false;
  }
  // Second-layer key is the raw 16-char secret (NUL-free base62 alphabet).
  if (!aesCbcBase64(first, reinterpret_cast<const uint8_t*>(secret.data()),
                    params)) {
    return false;
  }

  encSecKey = rsaEncryptSecret(secret);
  return encSecKey.size() == 256;
}

}  // namespace WeapiCrypto
