#include "EapiCrypto.h"

#include <mbedtls/aes.h>
#include <mbedtls/md5.h>

#include <cstdio>
#include <cstring>

namespace EapiCrypto {

namespace {

constexpr const char* EAPI_KEY = "e82ckenh8dichen8";  // 16 bytes

// AES-128-ECB with PKCS7 padding; uppercase hex.
bool aesEcbHex(const std::string& input, std::string& out) {
  const size_t pad = 16 - input.size() % 16;  // always pads
  std::string padded = input;
  padded.append(pad, static_cast<char>(pad));

  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  int ret = mbedtls_aes_setkey_enc(
      &aes, reinterpret_cast<const uint8_t*>(EAPI_KEY), 128);

  const size_t blocks = padded.size() / 16;
  std::string cipher;
  cipher.resize(padded.size());
  if (ret == 0) {
    for (size_t b = 0; b < blocks; ++b) {
      ret = mbedtls_aes_crypt_ecb(
          &aes, MBEDTLS_AES_ENCRYPT,
          reinterpret_cast<const uint8_t*>(padded.data()) + b * 16,
          reinterpret_cast<uint8_t*>(&cipher[0]) + b * 16);
      if (ret != 0) break;
    }
  }
  mbedtls_aes_free(&aes);
  if (ret != 0) return false;

  static const char* kHex = "0123456789ABCDEF";
  out.resize(cipher.size() * 2);
  for (size_t i = 0; i < cipher.size(); ++i) {
    const uint8_t c = static_cast<uint8_t>(cipher[i]);
    out[i * 2] = kHex[c >> 4];
    out[i * 2 + 1] = kHex[c & 0x0f];
  }
  return true;
}

}  // namespace

bool encrypt(const std::string& apiPath, const std::string& jsonBody,
             std::string& paramsHex) {
  const std::string message =
      "nobody" + apiPath + "use" + jsonBody + "md5forencrypt";

  uint8_t digestBytes[16];
  if (mbedtls_md5(reinterpret_cast<const uint8_t*>(message.data()),
                  message.size(), digestBytes) != 0) {
    return false;
  }
  char digestHex[33];
  for (int i = 0; i < 16; ++i) {
    std::snprintf(digestHex + i * 2, 3, "%02x", digestBytes[i]);
  }

  const std::string blob = apiPath + "-36cd479b6b5-" + jsonBody +
                           "-36cd479b6b5-" + digestHex;
  return aesEcbHex(blob, paramsHex);
}

}  // namespace EapiCrypto
