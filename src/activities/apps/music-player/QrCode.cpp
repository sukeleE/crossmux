#include "QrCode.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

// ECC LOW tables for versions 1..10 (index 0 unused).
constexpr int kTotalCodewords[11] = {0, 26, 44, 70, 100, 134, 172, 196, 242, 292, 346};
constexpr int kEcPerBlock[11] = {0, 7, 10, 15, 20, 26, 18, 20, 24, 30, 18};
constexpr int kBlocks[11] = {0, 1, 1, 1, 1, 1, 2, 2, 2, 2, 4};
constexpr int kByteCapacityLow[11] = {0, 17, 32, 53, 78, 106, 134, 154, 192, 230, 271};

// Alignment pattern centre coordinates (6 is added implicitly as the grid
// origin; combinations landing inside finder zones are skipped at draw time).
const int kAlignCenters[11][3] = {
    {0, 0, 0}, {0, 0, 0}, {18, 0, 0}, {22, 0, 0}, {26, 0, 0}, {30, 0, 0},
    {34, 0, 0}, {22, 38, 0}, {24, 42, 0}, {26, 46, 0}, {28, 50, 0},
};
constexpr int kAlignCount[11] = {0, 0, 1, 1, 1, 1, 1, 2, 2, 2, 2};

// Reed-Solomon GF(256), primitive polynomial 0x11D.
struct GfTables {
  int exp[512];
  int log[256];
  GfTables() {
    int x = 1;
    for (int i = 0; i < 255; ++i) {
      exp[i] = x;
      log[x] = i;
      x <<= 1;
      if (x & 0x100) x ^= 0x11D;
    }
    for (int i = 255; i < 512; ++i) exp[i] = exp[i - 255];
  }
};
const GfTables kGf;

int gfMul(int a, int b) {
  if (a == 0 || b == 0) return 0;
  return kGf.exp[kGf.log[a] + kGf.log[b]];
}

// RS generator polynomial coefficients for degree ecCount (cached).
const std::vector<int>& rsGenerator(int ecCount) {
  static std::vector<int> cache[31];
  auto& g = cache[ecCount];
  if (!g.empty()) return g;
  g.push_back(1);
  for (int i = 0; i < ecCount; ++i) {
    const int a = kGf.exp[i];
    std::vector<int> next(g.size() + 1, 0);
    for (size_t j = 0; j < g.size(); ++j) {
      next[j] ^= gfMul(g[j], a);
      next[j + 1] ^= g[j];
    }
    g.swap(next);
  }
  return g;
}

std::vector<uint8_t> rsRemainder(const std::vector<uint8_t>& data, int ecCount) {
  const std::vector<int>& gen = rsGenerator(ecCount);
  std::vector<uint8_t> rem(ecCount, 0);
  for (uint8_t byte : data) {
    const int factor = byte ^ rem[0];
    for (int i = 0; i < ecCount - 1; ++i) {
      rem[i] = rem[i + 1] ^ gfMul(gen[ecCount - 1 - i], factor);
    }
    rem[ecCount - 1] = gfMul(gen[0], factor);
  }
  return rem;
}

struct BitBuffer {
  std::vector<uint8_t> bytes;
  int bitCount = 0;
  void append(uint32_t value, int count) {
    for (int i = count - 1; i >= 0; --i) {
      if ((bitCount & 7) == 0) bytes.push_back(0);
      if ((value >> i) & 1) bytes[bitCount / 8] |= 1 << (7 - (bitCount & 7));
      ++bitCount;
    }
  }
};

}  // namespace

bool qrEncodeLow(const std::string& text, QrCode& out) {
  int version = 0;
  for (int v = 1; v <= 10; ++v) {
    if (static_cast<int>(text.size()) <= kByteCapacityLow[v]) {
      version = v;
      break;
    }
  }
  if (version == 0) return false;

  const int n = 17 + 4 * version;
  const int totalCodewords = kTotalCodewords[version];
  const int ecPerBlock = kEcPerBlock[version];
  const int blocks = kBlocks[version];
  const int dataTotal = totalCodewords - ecPerBlock * blocks;

  // --- Codeword stream --------------------------------------------------------
  BitBuffer bits;
  bits.append(0b0100, 4);  // byte-mode indicator
  bits.append(text.size(), version >= 10 ? 16 : 8);
  for (unsigned char c : text) bits.append(c, 8);

  const int freeBits = dataTotal * 8 - bits.bitCount;
  for (int i = 0; i < 4 && i < freeBits; ++i) bits.append(0, 1);  // terminator
  while ((bits.bitCount & 7) != 0) bits.append(0, 1);               // pad to byte
  bool padHigh = true;
  while (static_cast<int>(bits.bytes.size()) < dataTotal) {
    bits.bytes.push_back(padHigh ? 0xEC : 0x11);
    padHigh = !padHigh;
  }

  // --- Blocks + ECC -----------------------------------------------------------
  const int shortLen = dataTotal / blocks;
  const int extra = dataTotal % blocks;  // last `extra` blocks get one more byte

  std::vector<std::vector<uint8_t>> dataBlocks(blocks);
  std::vector<std::vector<uint8_t>> ecBlocks(blocks);
  int pos = 0;
  for (int b = 0; b < blocks; ++b) {
    const int len = shortLen + (b >= blocks - extra ? 1 : 0);
    dataBlocks[b].assign(bits.bytes.begin() + pos, bits.bytes.begin() + pos + len);
    pos += len;
    ecBlocks[b] = rsRemainder(dataBlocks[b], ecPerBlock);
  }

  std::vector<uint8_t> codewords;
  codewords.reserve(totalCodewords);
  for (int i = 0; i < shortLen + 1; ++i) {
    for (int b = 0; b < blocks; ++b) {
      if (i < static_cast<int>(dataBlocks[b].size())) codewords.push_back(dataBlocks[b][i]);
    }
  }
  for (int i = 0; i < ecPerBlock; ++i) {
    for (int b = 0; b < blocks; ++b) codewords.push_back(ecBlocks[b][i]);
  }

  // --- Matrix ------------------------------------------------------------------
  std::vector<uint8_t> function(static_cast<size_t>(n) * n, 0);
  std::vector<uint8_t> module(static_cast<size_t>(n) * n, 0);

  auto isFunction = [&](int x, int y) -> uint8_t& {
    return function[static_cast<size_t>(y) * n + x];
  };

  // Finder zones (incl. one-module separator).
  for (int y = 0; y < 8; ++y) {
    for (int x = 0; x < 8; ++x) {
      isFunction(x, y) = 1;
      isFunction(n - 1 - x, y) = 1;
      isFunction(x, n - 1 - y) = 1;
    }
  }
  // Timing row/column.
  for (int i = 0; i < n; ++i) {
    isFunction(6, i) = 1;
    isFunction(i, 6) = 1;
  }
  // Alignment pattern footprints.
  const int* centers = kAlignCenters[version];
  const int gridCount = kAlignCount[version] + 1;  // coordinates: 6 + centers
  for (int ai = 0; ai < gridCount; ++ai) {
    for (int bi = 0; bi < gridCount; ++bi) {
      const int gx = ai == 0 ? 6 : centers[ai - 1];
      const int gy = bi == 0 ? 6 : centers[bi - 1];
      // Skip footprints overlapping a finder zone.
      if ((gx < 8 && gy < 8) || (gx >= n - 8 && gy < 8) || (gx < 8 && gy >= n - 8)) continue;
      for (int dy = -2; dy <= 2; ++dy) {
        for (int dx = -2; dx <= 2; ++dx) isFunction(gx + dx, gy + dy) = 1;
      }
    }
  }
  // Format footprints + dark module.
  for (int i = 0; i <= 8; ++i) {
    isFunction(8, i) = 1;
    isFunction(i, 8) = 1;
  }
  for (int i = 0; i < 8; ++i) isFunction(8, n - 1 - i) = 1;  // Copy B y20..13
  for (int i = 0; i < 7; ++i) isFunction(n - 1 - i, 8) = 1;  // Copy B x20..14
  isFunction(n - 8, 8) = 1;                                   // dark module
  // Version info footprints (v >= 7).
  if (version >= 7) {
    for (int i = 0; i < 18; ++i) {
      isFunction(n - 11 + i % 3, i / 3) = 1;
      isFunction(i / 3, n - 11 + i % 3) = 1;
    }
  }

  // --- Place data bits in column pairs in a zigzag -----------------------------
  // Each column pair is scanned in the opposite direction to its predecessor,
  // starting upward at the rightmost pair. Function cells are skipped without
  // consuming a bit; right column of each pair precedes the left. The pair
  // containing timing column 6 is jumped over.
  int bitIndex = 0;
  for (int right = n - 1; right >= 1; right -= 2) {
    if (right == 6) --right;  // jump the timing column; pair becomes (5,4)
    const bool upward = ((right + 1) & 2) == 0;
    for (int vert = 0; vert < n; ++vert) {
      const int y = upward ? n - 1 - vert : vert;
      for (int c = 0; c < 2; ++c) {
        const int xx = right - c;
        uint8_t& f = function[static_cast<size_t>(y) * n + xx];
        if (f) continue;
        bool dark = false;
        if (bitIndex < static_cast<int>(codewords.size()) * 8) {
          dark = (codewords[bitIndex / 8] >> (7 - (bitIndex & 7))) & 1;
        }
        module[static_cast<size_t>(y) * n + xx] = dark;
        ++bitIndex;
#ifdef QR_DEBUG
        fprintf(stderr, "bit %d -> (%d,%d)\n", bitIndex - 1, xx, y);
#endif
      }
    }
  }

  // Data mask 0: invert when (x+y) even.
  for (int y = 0; y < n; ++y) {
    for (int xx = 0; xx < n; ++xx) {
      if (!function[static_cast<size_t>(y) * n + xx] && ((xx + y) & 1) == 0) {
        module[static_cast<size_t>(y) * n + xx] ^= 1;
      }
    }
  }

  // --- Function patterns -------------------------------------------------------
  auto setModule = [&](int xx, int yy, int dark) {
    module[static_cast<size_t>(yy) * n + xx] = dark;
  };

  // Finders.
  const int finderOrigins[3][2] = {{0, 0}, {n - 7, 0}, {0, n - 7}};
  for (const auto& o : finderOrigins) {
    for (int dy = 0; dy < 7; ++dy) {
      for (int dx = 0; dx < 7; ++dx) {
        const bool border = dx == 0 || dx == 6 || dy == 0 || dy == 6;
        const bool inner = dx >= 2 && dx <= 4 && dy >= 2 && dy <= 4;
        setModule(o[0] + dx, o[1] + dy, border || inner);
      }
    }
  }

  // Timing patterns.
  for (int i = 8; i <= n - 9; ++i) {
    setModule(i, 6, (i & 1) == 0);
    setModule(6, i, (i & 1) == 0);
  }

  // Alignment patterns: dark outer ring and centre, white middle ring.
  for (int ai = 0; ai < gridCount; ++ai) {
    for (int bi = 0; bi < gridCount; ++bi) {
      const int gx = ai == 0 ? 6 : centers[ai - 1];
      const int gy = bi == 0 ? 6 : centers[bi - 1];
      if ((gx < 8 && gy < 8) || (gx >= n - 8 && gy < 8) || (gx < 8 && gy >= n - 8)) continue;
      for (int dy = -2; dy <= 2; ++dy) {
        for (int dx = -2; dx <= 2; ++dx) {
          const int d = std::max(std::abs(dx), std::abs(dy));
          setModule(gx + dx, gy + dy, d == 2 || d == 0);
        }
      }
    }
  }

  // Format information: BCH(15,5), generator 0x537, xor mask 0x5412.
  // ECC LOW = 01, mask 0 = 000.
  int formatData = (0b01 << 3) | 0;
  int rem = formatData << 10;
  for (int i = 14; i >= 10; --i) {
    if (rem & (1 << i)) rem ^= 0x537 << (i - 10);
  }
  const int formatVal = ((formatData << 10) | rem) ^ 0x5412;

  for (int i = 0; i < 15; ++i) {
    const int bit = (formatVal >> i) & 1;
    if (i < 6) {
      setModule(8, i, bit);
    } else if (i == 6) {
      setModule(8, 7, bit);
    } else if (i == 7) {
      setModule(8, 8, bit);
    } else if (i == 8) {
      setModule(7, 8, bit);
    } else {
      setModule(14 - i, 8, bit);
    }
    if (i < 7) {
      setModule(n - 1 - i, 8, bit);        // Copy B horizontal: x20..14
    } else {
      setModule(8, n - 8 + (i - 7), bit);  // Copy B vertical: y14..20
    }
  }
  setModule(n - 8, 8, 1);  // dark module

  // Version information: BCH(18,6), generator 0x1F25.
  if (version >= 7) {
    rem = version << 12;
    for (int i = 17; i >= 12; --i) {
      if (rem & (1 << i)) rem ^= 0x1F25 << (i - 12);
    }
    const int versionVal = (version << 12) | rem;
    for (int i = 0; i < 18; ++i) {
      const int bit = (versionVal >> i) & 1;
      setModule(n - 11 + i % 3, i / 3, bit);
      setModule(i / 3, n - 11 + i % 3, bit);
    }
  }

  out.version = version;
  out.size = n;
  out.modules = std::move(module);
  return true;
}
