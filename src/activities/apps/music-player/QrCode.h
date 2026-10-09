#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Minimal QR encoder for the account-binding flow. Scope is intentionally
// narrow: byte mode, ECC LOW, versions 1..10, fixed data mask 0 (any mask is
// spec-legal; readers must decode all eight). Input is one short HTTPS URL.
struct QrCode {
  int version = 0;
  int size = 0;                      // modules per side
  std::vector<uint8_t> modules;      // size*size row-major, 1 = dark

  bool get(int x, int y) const { return modules[static_cast<size_t>(y) * size + x] != 0; }
};

// Encode text (UTF-8 bytes). False only when it exceeds version-10 LOW
// capacity, which cannot happen for the ~80-byte login URL.
bool qrEncodeLow(const std::string& text, QrCode& out);
