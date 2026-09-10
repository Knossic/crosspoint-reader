#include <FontFallback.h>
#include <gtest/gtest.h>

namespace {
constexpr EpdGlyph glyph(int width, int advance, int left = 0) {
  return {static_cast<uint8_t>(width), 8, static_cast<uint16_t>(advance), static_cast<int16_t>(left), 8, 0, 0};
}
constexpr EpdGlyph primaryGlyphs[] = {glyph(5, 96), glyph(5, 96), glyph(4, 80), glyph(4, 80),
                                      glyph(2, 0),  glyph(3, 48), glyph(4, 64)};
constexpr EpdUnicodeInterval primaryCoverage[] = {{0x41, 0x41, 0},    {0x56, 0x56, 1},   {0x65, 0x65, 2},
                                                  {0xE9, 0xE9, 3},    {0x301, 0x301, 4}, {0x2026, 0x2026, 5},
                                                  {0xFFFD, 0xFFFD, 6}};
constexpr EpdGlyph subsetGlyphs[] = {glyph(20, 320), glyph(10, 160), glyph(10, 160)};
constexpr EpdUnicodeInterval subsetCoverage[] = {{0x41, 0x41, 0}, {0x4E09, 0x4E09, 1}, {0x4F53, 0x4F53, 2}};

class FontFallbackTest : public ::testing::Test {
 protected:
  EpdFontData primaryData{};
  EpdFontData subsetData{};
  EpdFont primaryFont{&primaryData};
  EpdFont subsetFont{&subsetData};
  EpdFontFamily primary{&primaryFont};
  EpdFontFamily subset{&subsetFont};
  void SetUp() override {
    primaryData.glyph = primaryGlyphs;
    primaryData.intervals = primaryCoverage;
    primaryData.intervalCount = 7;
    subsetData.glyph = subsetGlyphs;
    subsetData.intervals = subsetCoverage;
    subsetData.intervalCount = 3;
  }
};

TEST_F(FontFallbackTest, RetainsLatinAccentsAndEllipsisAlongsideCjk) {
  const auto measured = fontFallback::measure(primary, subset, "三体Aé…", EpdFontFamily::REGULAR);
  EXPECT_EQ(measured.advance, 34);  // 10 + 10 + 6 + 5 + 3
  EXPECT_EQ(measured.width, 34);
  EXPECT_EQ(&fontFallback::select(primary, &subset, 0xE9, EpdFontFamily::REGULAR), &primary);
  EXPECT_EQ(&fontFallback::select(primary, &subset, 0x4E09, EpdFontFamily::REGULAR), &subset);
}

TEST_F(FontFallbackTest, PrefersPrimaryEvenWhenFallbackAlsoCoversGlyph) {
  EXPECT_EQ(&fontFallback::select(primary, &subset, 'A', EpdFontFamily::REGULAR), &primary);
  EXPECT_EQ(fontFallback::measure(primary, subset, "A三A", EpdFontFamily::REGULAR).advance, 22);
}

TEST_F(FontFallbackTest, MissingFromBothUsesPrimaryReplacement) {
  EXPECT_EQ(&fontFallback::select(primary, &subset, 0x4E2D, EpdFontFamily::REGULAR), &primary);
  EXPECT_EQ(fontFallback::measure(primary, subset, "三中A", EpdFontFamily::REGULAR).advance, 20);
}

TEST_F(FontFallbackTest, KeepsCombiningMarkAtZeroAdvance) {
  const auto composed = fontFallback::measure(primary, subset, "三é", EpdFontFamily::REGULAR);
  const auto decomposed = fontFallback::measure(primary, subset, "三e\xCC\x81", EpdFontFamily::REGULAR);
  EXPECT_EQ(decomposed.advance, composed.advance);
  EXPECT_EQ(decomposed.width, composed.width);
}

TEST_F(FontFallbackTest, UsesPrimaryKerningAfterReturningFromFallback) {
  static constexpr EpdKernClassEntry left[] = {{'A', 1}};
  static constexpr EpdKernClassEntry right[] = {{'V', 1}};
  static constexpr int8_t kern[] = {-16};
  primaryData.kernLeftClasses = left;
  primaryData.kernRightClasses = right;
  primaryData.kernLeftEntryCount = primaryData.kernRightEntryCount = 1;
  primaryData.kernLeftClassCount = primaryData.kernRightClassCount = 1;
  primaryData.kernMatrix = kern;
  EXPECT_EQ(fontFallback::measure(primary, subset, "AV三AV", EpdFontFamily::REGULAR).advance, 32);
}

TEST_F(FontFallbackTest, PreservesFractionalRoundingAtFontBoundaries) {
  EpdGlyph fractional[] = {glyph(5, 89), glyph(5, 89), glyph(4, 80), glyph(4, 80),
                           glyph(2, 0),  glyph(3, 48), glyph(4, 64)};
  primaryData.glyph = fractional;
  EXPECT_EQ(fontFallback::measure(primary, subset, "A三A", EpdFontFamily::REGULAR).advance, 22);
}

TEST_F(FontFallbackTest, HandlesStyleFallbackAndSuperscriptAdvances) {
  const auto style = static_cast<EpdFontFamily::Style>(EpdFontFamily::BOLD | EpdFontFamily::SUP);
  EXPECT_EQ(fontFallback::measure(primary, subset, "A三A", style).advance, 11);
}

TEST_F(FontFallbackTest, PreservesPrimaryBitmapBounds) {
  EpdGlyph overhang[] = {glyph(9, 96, -2), glyph(5, 96), glyph(4, 80), glyph(4, 80),
                         glyph(2, 0),      glyph(3, 48), glyph(4, 64)};
  primaryData.glyph = overhang;
  const auto measured = fontFallback::measure(primary, subset, "A三A", EpdFontFamily::REGULAR);
  EXPECT_EQ(measured.advance, 22);
  EXPECT_EQ(measured.width, 25);
}

TEST_F(FontFallbackTest, AbsentFallbackKeepsPrimary) {
  EXPECT_EQ(&fontFallback::select(primary, nullptr, 0x4E09, EpdFontFamily::REGULAR), &primary);
}
}  // namespace
