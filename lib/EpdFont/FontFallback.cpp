#include "FontFallback.h"

#include <BidiUtils.h>
#include <Utf8.h>

#include <algorithm>

namespace fontFallback {
const EpdFontFamily& select(const EpdFontFamily& primary, const EpdFontFamily* fallback, const uint32_t cp,
                            const EpdFontFamily::Style style) {
  if (fallback && !utf8IsCombiningMark(cp) && !BidiUtils::isTransparentMark(cp) && !primary.hasCodepoint(cp, style) &&
      fallback->hasCodepoint(cp, style)) {
    return *fallback;
  }
  return primary;
}

Measurement measure(const EpdFontFamily& primary, const EpdFontFamily& fallback, const char* text,
                    const EpdFontFamily::Style style) {
  int x = 0, minX = 0, maxX = 0;
  int lastLeft = 0, lastWidth = 0;
  int32_t advance = 0;
  uint32_t previous = 0;
  const EpdFontFamily* previousFont = nullptr;
  const bool scaled = (style & (EpdFontFamily::SUP | EpdFontFamily::SUB)) != 0;
  while (const uint32_t decoded = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&text))) {
    const auto& font = select(primary, &fallback, decoded, style);
    const bool mark = utf8IsCombiningMark(decoded) || BidiUtils::isTransparentMark(decoded);
    const uint32_t cp = mark ? decoded : font.applyLigatures(decoded, text, style);
    if (!mark && previous != 0) {
      const int kern = previousFont == &font ? font.getKerning(previous, cp, style) : 0;
      x += fp4::toPixel(advance + kern);
    }
    const auto* glyph = font.getGlyph(cp, style);
    if (glyph) {
      const int left = scaled && !mark ? glyph->left / 2 : glyph->left;
      const int width = scaled && !mark ? (glyph->width + 1) / 2 : glyph->width;
      const int base = mark ? combiningMark::anchorOver(combiningMark::anchorFor(cp), x, lastLeft, lastWidth,
                                                        glyph->left, glyph->width)
                            : x;
      minX = std::min(minX, base + left);
      maxX = std::max(maxX, base + left + width);
    }
    if (!mark) {
      lastLeft = glyph ? glyph->left : 0;
      lastWidth = glyph ? glyph->width : 0;
      advance = glyph ? glyph->advanceX : 0;
      if (scaled) advance = (advance + 1) / 2;
      previous = cp;
      previousFont = &font;
    }
  }
  return {maxX - minX, x + fp4::toPixel(advance)};
}
}  // namespace fontFallback
