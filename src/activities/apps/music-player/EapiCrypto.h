#pragma once

#include <string>

// NetEase "eapi" request encryption, the channel used by the official desktop
// client. Unlike weapi (the web channel), the login QR endpoints over eapi are
// not risk-controlled with code 8821 when the device sits on a carrier-egress
// IP (a phone hotspot) — real desktop clients log in from such networks all
// the time.
//
//   digest = md5("nobody" + url + "use" + json + "md5forencrypt")      (hex)
//   blob   = url + "-36cd479b6b5-" + json + "-36cd479b6b5-" + digest
//   params = AES-128-ECB(blob, fixed key, PKCS7), uppercase hex
//
// `url` is the ORIGINAL /api/... path (it is part of the signed payload),
// while the request itself is POSTed to /eapi/<same suffix>. The form
// "params=<hex>" is the body. mbedTLS only; no external libraries.
namespace EapiCrypto {

bool encrypt(const std::string& apiPath, const std::string& jsonBody,
             std::string& paramsHex);

}  // namespace EapiCrypto
