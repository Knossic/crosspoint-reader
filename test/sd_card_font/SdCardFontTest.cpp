#include <HalStorage.h>
#include <SdCardFont.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <array>
#include <cstdlib>
#include <cstring>
#include <new>

// Fail only the requested bitmap allocation; metadata and the fallback ring
// still have space, as on a fragmented device heap. rejectedArrayMinSize
// models a bounded largest free block: every array at or above it fails.
static size_t rejectedArraySize = 0;
static size_t rejectedArrayMinSize = SIZE_MAX;
void* operator new[](size_t size, const std::nothrow_t&) noexcept {
  if (size == rejectedArraySize || size >= rejectedArrayMinSize) return nullptr;
  return std::malloc(size);
}
void operator delete[](void* ptr) noexcept { std::free(ptr); }
void operator delete[](void* ptr, size_t) noexcept { std::free(ptr); }

class SdCardFontTest : public testing::Test {
 protected:
  char path[64] = "/tmp/crosspoint-font-test-XXXXXX";
  SdCardFont font;
  std::string text;
  // Glyphs cover codepoints 32..32+glyphCount-1; every bitmap is 128 bytes
  // filled with the glyph's index. Derived fixtures raise the count.
  uint32_t glyphCount = 32;

  void SetUp() override {
    const int fd = mkstemp(path);
    ASSERT_GE(fd, 0);
    FILE* file = fdopen(fd, "wb");
    ASSERT_NE(file, nullptr);
    std::array<uint8_t, 64> header{};
    memcpy(header.data(), "CPFONT", 6);
    header[8] = CPFONT_VERSION;
    header[10] = 1;
    header[12] = 1;
    header[36] = 1;  // interval count
    header[40] = static_cast<uint8_t>(glyphCount);
    header[44] = 24;  // advance Y
    header[45] = 20;  // ascender
    header[56] = 64;  // style data offset
    fwrite(header.data(), 1, header.size(), file);
    const EpdUnicodeInterval interval{32, 32 + glyphCount - 1, 0};
    fwrite(&interval, sizeof(interval), 1, file);
    for (uint32_t i = 0; i < glyphCount; ++i) {
      EpdGlyph glyph{};
      glyph.width = 16;
      glyph.height = 16;
      glyph.advanceX = 16 * 16;
      glyph.top = 16;
      glyph.dataLength = 128;
      glyph.dataOffset = i * 128;
      fwrite(&glyph, sizeof(glyph), 1, file);
      text += static_cast<char>(32 + i);
    }
    std::array<uint8_t, 128> bitmap{};
    for (uint32_t i = 0; i < glyphCount; ++i) {
      bitmap.fill(static_cast<uint8_t>(i));
      fwrite(bitmap.data(), 1, bitmap.size(), file);
    }
    fclose(file);
    ASSERT_TRUE(font.load(path));
    sdReadCount = 0;
  }

  void TearDown() override {
    rejectedArraySize = 0;
    rejectedArrayMinSize = SIZE_MAX;
    ESP.maxAlloc = 1024 * 1024;
    unlink(path);
  }

  void prewarmFragmented(bool partial) {
    rejectedArraySize = 4096;
    ESP.maxAlloc = partial ? 5120 : 4096;
    font.prewarm(text.c_str(), 1, false, false);
    rejectedArraySize = 0;
  }

  const uint8_t* bitmap(const EpdGlyph* glyph) {
    const auto* data = font.getEpdFont()->data;
    if (!data->bitmap || glyph->dataOffset == SdCardFont::DEFERRED_BITMAP_OFFSET) {
      return font.getDeferredBitmap(data, glyph);
    }
    return data->bitmap + glyph->dataOffset;
  }
};

TEST_F(SdCardFontTest, PartialBitmapCacheKeepsAllMetricsWithoutReads) {
  prewarmFragmented(true);
  const auto before = sdReadCount;
  for (int pass = 0; pass < 14; ++pass) {
    for (uint32_t cp = 32; cp < 64; ++cp) {
      const auto* glyph = font.getEpdFont()->getGlyph(cp);
      ASSERT_NE(glyph, nullptr);
      EXPECT_EQ(glyph->advanceX, 256);
      EXPECT_FALSE(font.isOverflowGlyph(glyph));
    }
  }
  EXPECT_EQ(sdReadCount, before);
}

TEST_F(SdCardFontTest, CachedAndDeferredBitmapsMatchFileThroughEviction) {
  prewarmFragmented(true);
  const auto before = sdReadCount;
  ASSERT_NE(font.getEpdFont()->data->bitmap, nullptr);
  for (uint32_t cp = 32; cp < 64; ++cp) {
    const auto* glyph = font.getEpdFont()->getGlyph(cp);
    const auto* bits = bitmap(glyph);
    ASSERT_NE(bits, nullptr);
    for (size_t i = 0; i < 128; ++i) EXPECT_EQ(bits[i], cp - 32);
    if (cp < 40) EXPECT_EQ(sdReadCount, before);
  }
  const auto* glyph = font.getEpdFont()->getGlyph(40);
  ASSERT_NE(bitmap(glyph), nullptr);
  EXPECT_EQ(bitmap(glyph)[127], 8);
  const auto after = sdReadCount;
  EXPECT_NE(bitmap(glyph), nullptr);
  EXPECT_EQ(sdReadCount, after);
}

TEST_F(SdCardFontTest, NoBitmapArenaStillKeepsMetricsAndLoadsVisibleGlyph) {
  prewarmFragmented(false);
  EXPECT_EQ(font.getEpdFont()->data->bitmap, nullptr);
  const auto before = sdReadCount;
  const auto* glyph = font.getEpdFont()->getGlyph(63);
  ASSERT_NE(glyph, nullptr);
  EXPECT_EQ(sdReadCount, before);
  ASSERT_NE(bitmap(glyph), nullptr);
  EXPECT_EQ(bitmap(glyph)[127], 31);
}

TEST_F(SdCardFontTest, FullCacheRendersWithoutAdditionalReads) {
  font.prewarm(text.c_str(), 1, false, false);
  const auto before = sdReadCount;
  for (uint32_t cp = 32; cp < 64; ++cp) {
    const auto* glyph = font.getEpdFont()->getGlyph(cp);
    ASSERT_NE(glyph, nullptr);
    ASSERT_NE(bitmap(glyph), nullptr);
    EXPECT_EQ(bitmap(glyph)[127], cp - 32);
  }
  EXPECT_EQ(sdReadCount, before);
}

TEST_F(SdCardFontTest, RepeatedPrewarmReusesPartialCache) {
  prewarmFragmented(true);
  const auto before = sdReadCount;
  font.clearCache();
  font.prewarm(text.c_str(), 1, false, false);
  EXPECT_EQ(sdReadCount, before);
  ASSERT_NE(bitmap(font.getEpdFont()->getGlyph(63)), nullptr);
}

TEST_F(SdCardFontTest, MetadataOnlyPrewarmDoesNotUseOldBitmapOffsets) {
  font.prewarm(text.c_str(), 1, false, false);
  font.releaseResidentCaches();
  font.prewarm("?", 1, true, false);
  EXPECT_EQ(font.getEpdFont()->data->bitmap, nullptr);
  const auto* glyph = font.getEpdFont()->getGlyph(63);
  ASSERT_NE(glyph, nullptr);
  ASSERT_NE(bitmap(glyph), nullptr);
  EXPECT_EQ(bitmap(glyph)[127], 31);
}

TEST_F(SdCardFontTest, AdvanceTableFallsBackToSmallerBufferWhenFragmented) {
  // Largest free block below the full 16KB codepoint buffer, as in a reader
  // holding its bitmap arena: layout must still batch instead of giving up.
  rejectedArrayMinSize = 4096 + 8;
  EXPECT_EQ(font.buildAdvanceTable(text.c_str(), 1), 0);
  rejectedArrayMinSize = SIZE_MAX;
  EXPECT_TRUE(font.hasAdvanceTable());
  for (uint32_t cp = 32; cp < 64; ++cp) EXPECT_EQ(font.getAdvance(cp, 0), 256) << "cp " << cp;
}

TEST_F(SdCardFontTest, FailedDeferredReadPreservesMetricsAndCachedBitmaps) {
  prewarmFragmented(true);
  unlink(path);
  const auto* glyph = font.getEpdFont()->getGlyph(63);
  ASSERT_NE(glyph, nullptr);
  EXPECT_EQ(bitmap(glyph), nullptr);
  EXPECT_EQ(glyph->advanceX, 256);
  const auto* cached = font.getEpdFont()->getGlyph(33);
  ASSERT_NE(cached, nullptr);
  ASSERT_NE(bitmap(cached), nullptr);
  EXPECT_EQ(bitmap(cached)[127], 1);
}

// A book's worth of page turns under a bitmap arena smaller than one page,
// as on a fragmented device heap. Pages are 24 consecutive glyphs sliding by
// 12, so each page shares half its glyphs with the previous one and
// introduces 12 new ones.
class SdCardFontPagesTest : public SdCardFontTest {
 protected:
  static constexpr uint32_t GLYPH_BYTES = 128;
  static constexpr uint32_t PAGE_GLYPHS = 24;
  static constexpr uint32_t PAGE_STEP = 12;

  void SetUp() override {
    glyphCount = 96;
    SdCardFontTest::SetUp();
  }

  // Bound the arena to `arenaGlyphs` bitmaps: the full-page allocation
  // fails, and the largest free block leaves exactly that much after the
  // allocator's headroom.
  void limitArena(uint32_t arenaGlyphs) {
    rejectedArrayMinSize = arenaGlyphs * GLYPH_BYTES + 1;
    ESP.maxAlloc = 4096 + arenaGlyphs * GLYPH_BYTES;
  }

  static std::string pageText(uint32_t page) {
    std::string s;
    for (uint32_t i = 0; i < PAGE_GLYPHS; ++i) s += static_cast<char>(32 + page * PAGE_STEP + i);
    return s;
  }

  void prewarmPage(uint32_t page) {
    font.clearCache();
    font.prewarm(pageText(page).c_str(), 1, false, false);
  }

  // Draw every glyph of the page once (one grayscale strip pass) and return
  // the SD reads it cost. Verifies every bitmap against the file.
  size_t renderPage(uint32_t page) {
    const auto before = sdReadCount;
    for (uint32_t i = 0; i < PAGE_GLYPHS; ++i) {
      const uint32_t cp = 32 + page * PAGE_STEP + i;
      const auto* glyph = font.getEpdFont()->getGlyph(cp);
      EXPECT_NE(glyph, nullptr) << "cp " << cp;
      if (!glyph) continue;
      EXPECT_FALSE(font.isOverflowGlyph(glyph)) << "metrics for cp " << cp << " fell out of the mini table";
      const auto* bits = bitmap(glyph);
      EXPECT_NE(bits, nullptr) << "cp " << cp;
      if (bits) {
        EXPECT_EQ(bits[127], cp - 32);
      }
    }
    return sdReadCount - before;
  }

  uint32_t residentCount() {
    const auto* data = font.getEpdFont()->data;
    uint32_t n = 0;
    for (uint32_t i = 0; i < data->intervalCount; ++i) n += data->intervals[i].last - data->intervals[i].first + 1;
    return n;
  }
};

TEST_F(SdCardFontPagesTest, ArenaSmallerThanPagePrioritizesCurrentPageAcrossTurns) {
  constexpr uint32_t ARENA_GLYPHS = 17;  // 7 of each page's 24 glyphs stay deferred
  limitArena(ARENA_GLYPHS);
  for (uint32_t page = 0; page < 7; ++page) {
    prewarmPage(page);
    // Stale glyphs never fit alongside the page, so none are retained.
    EXPECT_EQ(residentCount(), PAGE_GLYPHS) << "page " << page;
    // Exactly the glyphs the arena cannot hold load on demand (two reads
    // each), whatever came before; the 8-slot ring then serves repeat passes.
    EXPECT_EQ(renderPage(page), (PAGE_GLYPHS - ARENA_GLYPHS) * 2) << "page " << page;
    EXPECT_EQ(renderPage(page), 0u) << "page " << page;
  }
}

TEST_F(SdCardFontPagesTest, ArenaLargerThanPageKeepsPagesFullyCachedAndBoundsStaleGlyphs) {
  constexpr uint32_t ARENA_GLYPHS = 40;
  limitArena(ARENA_GLYPHS);
  for (uint32_t page = 0; page < 7; ++page) {
    prewarmPage(page);
    EXPECT_LE(residentCount(), ARENA_GLYPHS) << "page " << page;
    EXPECT_GE(residentCount(), PAGE_GLYPHS) << "page " << page;
    EXPECT_EQ(renderPage(page), 0u) << "page " << page;
  }
}

TEST_F(SdCardFontPagesTest, SeparateStringsOnOneScreenAccumulateWhenTheyFit) {
  limitArena(40);
  prewarmPage(0);
  // A status-bar string prewarmed after the page (no clearCache in between)
  // joins the resident set instead of evicting the page.
  const std::string statusBar = pageText(6).substr(0, 4);
  font.prewarm(statusBar.c_str(), 1, false, false);
  EXPECT_EQ(residentCount(), PAGE_GLYPHS + 4);
  EXPECT_EQ(renderPage(0), 0u);
  const auto before = sdReadCount;
  for (char c : statusBar) {
    const auto* glyph = font.getEpdFont()->getGlyph(static_cast<uint32_t>(c));
    ASSERT_NE(glyph, nullptr);
    ASSERT_NE(bitmap(glyph), nullptr);
  }
  EXPECT_EQ(sdReadCount, before);
}
