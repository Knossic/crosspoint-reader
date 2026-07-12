#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

// Codec for 1-bit grayscale plane bands: a vertical XOR predictor followed by
// Elias-gamma-coded bit run lengths (a fax-G3-flavored scheme).
//
// Predictor: each row is XORed with the row above (per band; the first row of
// a band is coded raw, keeping bands independently decodable). Text is
// vertically coherent, so solid ink and solid white both cancel to zeros and
// only the edges where ink starts or stops survive.
//
// Bitstream: one initial bit gives the color of the first run, then each run
// length r >= 1 is Elias-gamma coded (floor(log2 r) zero bits, then the
// binary digits of r MSB-first), colors alternating. Bits are consumed
// row-major, MSB-first within a byte, with runs crossing row boundaries.
// Typical text bands code 3-5x smaller than raw; the pathological worst case
// (alternating 2-bit runs) expands 1.5x, which the encoder's cap check turns
// into a caller-side fallback. Runs are capped at 2^16 - 1 bits by the
// format, so a band must be under 65536 bits (8KB); encode() rejects larger.
namespace PlaneRle {

namespace detail {

struct BitWriter {
  uint8_t* dst;
  size_t cap;
  size_t pos = 0;
  uint32_t acc = 0;
  int nbits = 0;
  bool overflow = false;

  void put(uint32_t value, int count) {  // count <= 17; value < 2^count
    acc = (acc << count) | value;
    nbits += count;
    while (nbits >= 8) {
      if (pos >= cap) {
        overflow = true;
        nbits = 0;
        return;
      }
      dst[pos++] = static_cast<uint8_t>(acc >> (nbits - 8));
      nbits -= 8;
    }
  }

  void putGamma(uint32_t r) {  // 1 <= r < 2^16
    int n = 0;
    while ((r >> (n + 1)) != 0) {
      n++;
    }
    put(0, n);
    put(r, n + 1);  // top bit of r is bit n, which is 1
  }

  size_t finish() {  // flush the final partial byte, zero-padded
    if (nbits > 0) {
      if (pos >= cap) {
        overflow = true;
        return 0;
      }
      dst[pos++] = static_cast<uint8_t>(acc << (8 - nbits));
      nbits = 0;
    }
    return pos;
  }
};

// Buffered reader: a 64-bit MSB-aligned window refilled bytewise, so a whole
// gamma code is consumed with shifts instead of per-bit calls — this runs in
// the visible post-refresh window, unlike the encoder. The window must be
// 64-bit: the longest code is 31 bits, and a 32-bit window can strand at 28
// available bits with no room to refill a whole byte.
struct BitReader {
  const uint8_t* src;
  size_t len;
  size_t pos = 0;
  uint64_t window = 0;  // valid bits MSB-aligned, low bits zero
  int avail = 0;
  bool fail = false;

  void refill() {
    while (avail <= 56 && pos < len) {
      window |= static_cast<uint64_t>(src[pos++]) << (56 - avail);
      avail += 8;
    }
  }

  int readBit() {
    refill();
    if (avail == 0) {
      fail = true;
      return 0;
    }
    const int b = static_cast<int>(window >> 63);
    window <<= 1;
    avail--;
    return b;
  }

  uint32_t gamma() {
    refill();
    if (avail == 0) {
      fail = true;
      return 0;
    }
    int n = 0;
    uint64_t w = window;
    while ((w >> 63) == 0) {
      if (++n > 15) {  // format caps runs below 2^16
        fail = true;
        return 0;
      }
      w <<= 1;
    }
    const int total = 2 * n + 1;  // <= 31 <= avail whenever the stream has the bits
    if (total > avail) {
      fail = true;
      return 0;
    }
    const uint32_t r = static_cast<uint32_t>(window >> (64 - total));  // leading zeros contribute nothing
    window <<= total;
    avail -= total;
    return r;
  }
};

// cur ^= prev, word-wise when alignment allows (panel rows are 100 or 60
// bytes from a malloc'd band, so the fast path is the normal one; the byte
// path keeps arbitrary shapes correct — RISC-V faults on unaligned loads).
inline void xorRow(uint8_t* cur, const uint8_t* prev, size_t n) {
  if (((reinterpret_cast<uintptr_t>(cur) | reinterpret_cast<uintptr_t>(prev)) & 3) == 0 && (n & 3) == 0) {
    uint32_t* c = reinterpret_cast<uint32_t*>(cur);
    const uint32_t* p = reinterpret_cast<const uint32_t*>(prev);
    for (size_t i = 0; i < n / 4; i++) {
      c[i] ^= p[i];
    }
  } else {
    for (size_t i = 0; i < n; i++) {
      cur[i] ^= prev[i];
    }
  }
}

// Sets bits [start, start + count) in p, MSB-first indexing. count >= 1.
// Whole middle bytes go through memset, so long ink runs fill fast.
inline void setBits(uint8_t* p, size_t start, size_t count) {
  const size_t endBit = start + count - 1;  // inclusive
  const size_t firstByte = start >> 3;
  const size_t lastByte = endBit >> 3;
  const uint8_t headMask = static_cast<uint8_t>(0xFF >> (start & 7));
  const uint8_t tailMask = static_cast<uint8_t>(0xFF << (7 - (endBit & 7)));
  if (firstByte == lastByte) {
    p[firstByte] |= headMask & tailMask;
    return;
  }
  p[firstByte] |= headMask;
  if (lastByte > firstByte + 1) {
    memset(p + firstByte + 1, 0xFF, lastByte - firstByte - 1);
  }
  p[lastByte] |= tailMask;
}

}  // namespace detail

// Encodes a band of rows x rowBytes. MUTATES band (in-place XOR predictor);
// callers discard the scratch after encoding. Returns the encoded size, or 0
// if the output would exceed cap (or the band exceeds the 65535-bit format
// limit — split the band before widening a panel past that).
inline size_t encode(uint8_t* band, size_t rowBytes, size_t rows, uint8_t* dst, size_t cap) {
  const size_t nBytes = rowBytes * rows;
  const size_t nBits = nBytes * 8;
  if (nBits == 0 || nBits >= (1u << 16)) {
    return 0;
  }

  // Vertical XOR predictor, bottom-up so each row deltas against a raw row.
  for (size_t r = rows - 1; r >= 1; r--) {
    uint8_t* cur = band + r * rowBytes;
    detail::xorRow(cur, cur - rowBytes, rowBytes);
  }

  detail::BitWriter w{dst, cap};
  int color = (band[0] >> 7) & 1;
  w.put(static_cast<uint32_t>(color), 1);
  size_t pos = 0;
  while (pos < nBits && !w.overflow) {
    // Scan the current run: bit-by-bit to a byte boundary, then whole bytes
    // (post-XOR bands are mostly 0x00, so zero runs skip 8 bits at a time),
    // then bit-by-bit through the byte containing the run's end.
    const uint8_t want = color ? 0xFF : 0x00;
    size_t p = pos;
    while (p < nBits && (p & 7) != 0 && ((band[p >> 3] >> (7 - (p & 7))) & 1) == color) {
      p++;
    }
    if ((p & 7) == 0) {
      while (p + 8 <= nBits && band[p >> 3] == want) {
        p += 8;
      }
      while (p < nBits && ((band[p >> 3] >> (7 - (p & 7))) & 1) == color) {
        p++;
      }
    }
    w.putGamma(static_cast<uint32_t>(p - pos));
    pos = p;
    color ^= 1;
  }
  const size_t out = w.finish();
  return w.overflow ? 0 : out;
}

// Decodes srcLen encoded bytes into a band of rows x rowBytes. Returns false
// on malformed input; band contents are unspecified on failure.
inline bool decode(const uint8_t* src, size_t srcLen, uint8_t* band, size_t rowBytes, size_t rows) {
  const size_t nBytes = rowBytes * rows;
  const size_t nBits = nBytes * 8;
  if (nBits == 0 || nBits >= (1u << 16)) {
    return false;
  }
  memset(band, 0, nBytes);

  detail::BitReader rd{src, srcLen};
  int color = rd.readBit();
  size_t pos = 0;
  while (pos < nBits) {
    const uint32_t run = rd.gamma();
    if (rd.fail || run == 0 || pos + run > nBits) {
      return false;
    }
    if (color) {
      detail::setBits(band, pos, run);
    }
    pos += run;
    color ^= 1;
  }

  // Undo the predictor, top-down so each row adds back a reconstructed row.
  for (size_t r = 1; r < rows; r++) {
    uint8_t* cur = band + r * rowBytes;
    detail::xorRow(cur, cur - rowBytes, rowBytes);
  }
  return true;
}

}  // namespace PlaneRle
