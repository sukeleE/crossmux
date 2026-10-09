#pragma once

#include <string>

// NetEase "weapi" request encryption, the channel used by the web client.
// As of 2026-10 the plaintext /api/login/* endpoints answer 401 "无权限访问.
// ENC"; login requests therefore have to be wrapped like this:
//
//   1. JSON body -> AES-128-CBC(preset key, fixed IV, PKCS7) -> base64
//   2. that base64 -> AES-128-CBC(random 16-char secret key) -> base64 = params
//   3. reversed secret key -> textbook RSA (e=65537, fixed modulus) -> hex
//      = encSecKey (256 hex chars)
//
// The form "params=<...>&encSecKey=<...>" is POSTed to /weapi/<endpoint>.
// Implemented on the mbedTLS shipped with the ESP32 framework; no external
// libraries. Host reference: ~/weapitest/weapi.py.
namespace WeapiCrypto {

bool encrypt(const std::string& jsonBody, std::string& params,
             std::string& encSecKey);

}  // namespace WeapiCrypto
