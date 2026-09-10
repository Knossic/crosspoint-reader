#pragma once

#include "EpdFontFamily.h"

namespace fontFallback {
// Coverage queries never load glyphs or substitute the replacement character.
const EpdFontFamily& select(const EpdFontFamily& primary, const EpdFontFamily* fallback, uint32_t cp,
                            EpdFontFamily::Style style);

struct Measurement {
  int width;
  int advance;
};
// Input is already in visual order. Uses the same per-glyph rounding as drawing.
Measurement measure(const EpdFontFamily& primary, const EpdFontFamily& fallback, const char* text,
                    EpdFontFamily::Style style);
}  // namespace fontFallback
