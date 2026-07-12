#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

// Byte-wise RLE tuned for 1-bit grayscale text planes, which are dominated by
// runs of 0x00 (blank rows, margins, word gaps). Control byte c:
//   c in [0, 127]   -> run of (c + 1) zero bytes      (1..128)
//   c in [128, 255] -> (c - 127) literal bytes follow (1..128)
// Worst case (no zero runs) expands len by ceil(len / 128) control bytes.
namespace PlaneRle {

// Encodes len bytes from src into dst (capacity cap). Returns the encoded
// size, or 0 if the output would exceed cap.
inline size_t encode(const uint8_t* src, size_t len, uint8_t* dst, size_t cap) {
  size_t si = 0, di = 0;
  while (si < len) {
    if (src[si] == 0) {
      size_t run = 1;
      while (si + run < len && src[si + run] == 0 && run < 128) {
        run++;
      }
      if (di + 1 > cap) {
        return 0;
      }
      dst[di++] = static_cast<uint8_t>(run - 1);
      si += run;
    } else {
      // Extend the literal across isolated zeros: a single embedded zero
      // costs one literal byte either way, but breaking the literal costs an
      // extra control byte. Stop only when >= 2 consecutive zeros follow (or
      // a single trailing zero, where a run costs the same).
      size_t lit = 1;
      while (si + lit < len && lit < 128) {
        if (src[si + lit] == 0 && (si + lit + 1 == len || src[si + lit + 1] == 0)) {
          break;
        }
        lit++;
      }
      if (di + 1 + lit > cap) {
        return 0;
      }
      dst[di++] = static_cast<uint8_t>(127 + lit);
      memcpy(dst + di, src + si, lit);
      di += lit;
      si += lit;
    }
  }
  return di;
}

// Decodes srcLen encoded bytes into exactly dstLen output bytes. Returns
// false on malformed input (overrun or length mismatch); dst contents are
// unspecified on failure.
inline bool decode(const uint8_t* src, size_t srcLen, uint8_t* dst, size_t dstLen) {
  size_t si = 0, di = 0;
  while (si < srcLen) {
    const uint8_t c = src[si++];
    if (c < 128) {
      const size_t run = static_cast<size_t>(c) + 1;
      if (di + run > dstLen) {
        return false;
      }
      memset(dst + di, 0, run);
      di += run;
    } else {
      const size_t lit = static_cast<size_t>(c) - 127;
      if (si + lit > srcLen || di + lit > dstLen) {
        return false;
      }
      memcpy(dst + di, src + si, lit);
      si += lit;
      di += lit;
    }
  }
  return di == dstLen;
}

}  // namespace PlaneRle
