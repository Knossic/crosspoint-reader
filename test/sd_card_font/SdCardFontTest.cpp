#include <HalStorage.h>
#include <SdCardFont.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <array>
#include <cstdlib>
#include <cstring>
#include <new>

// Fail only the requested bitmap allocation; metadata and the fallback ring
// still have space, as on a fragmented device heap.
static size_t rejectedArraySize = 0;
void* operator new[](size_t size, const std::nothrow_t&) noexcept {
  return size == rejectedArraySize ? nullptr : std::malloc(size);
}
void operator delete[](void* ptr) noexcept { std::free(ptr); }
void operator delete[](void* ptr, size_t) noexcept { std::free(ptr); }

class SdCardFontTest : public testing::Test {
 protected:
  char path[64] = "/tmp/crosspoint-font-test-XXXXXX";
  SdCardFont font;
  std::string text;

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
    header[36] = 1;   // interval count
    header[40] = 32;  // glyph count
    header[44] = 24;  // advance Y
    header[45] = 20;  // ascender
    header[56] = 64;  // style data offset
    fwrite(header.data(), 1, header.size(), file);
    const EpdUnicodeInterval interval{32, 63, 0};
    fwrite(&interval, sizeof(interval), 1, file);
    for (uint32_t i = 0; i < 32; ++i) {
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
    for (int i = 0; i < 32; ++i) {
      bitmap.fill(static_cast<uint8_t>(i));
      fwrite(bitmap.data(), 1, bitmap.size(), file);
    }
    fclose(file);
    ASSERT_TRUE(font.load(path));
    sdReadCount = 0;
  }

  void TearDown() override {
    rejectedArraySize = 0;
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
