#include <Epub/Page.h>
#include <GfxRenderer.h>
#include <gtest/gtest.h>

#include <memory>
#include <string>

#define class struct
#define private public
#include "Epub/parsers/ChapterHtmlSlimParser.h"
#undef private
#undef class

namespace {

class ChapterHtmlSlimParserTest : public ::testing::TestWithParam<const char*> {
 protected:
  std::string filepath = "unused.xhtml";
  GfxRenderer renderer;
  CssParser cssParser{"/tmp"};
  ChapterHtmlSlimParser parser{nullptr,
                               filepath,
                               renderer,
                               0,
                               1.0f,
                               false,
                               0,
                               static_cast<uint16_t>(renderer.getScreenWidth()),
                               static_cast<uint16_t>(renderer.getScreenHeight()),
                               false,
                               false,
                               {},
                               true,
                               "",
                               "",
                               0,
                               {},
                               nullptr,
                               &cssParser};

  void SetUp() override { parser.currentTextBlock = std::make_unique<ParsedText>(false); }
};

TEST_P(ChapterHtmlSlimParserTest, KeepsCssVerticalAlignAndInternalLinkMetadata) {
  const char* verticalAlign = GetParam();
  const char* expectedHref = "#note-target";
  const XML_Char* attributes[] = {"href", expectedHref, "style", verticalAlign, nullptr};

  ChapterHtmlSlimParser::startElement(&parser, "a", attributes);
  const uint8_t linkId = parser.currentFootnoteLinkId;
  ASSERT_NE(linkId, 0u);
  ChapterHtmlSlimParser::characterData(&parser, "1", 1);
  ChapterHtmlSlimParser::endElement(&parser, "a");

  ASSERT_EQ(parser.currentTextBlock->size(), 1u);
  const auto style = parser.currentTextBlock->getWordStyleAt(0);
  const auto expectedStyle =
      std::string(verticalAlign).find("super") != std::string::npos ? EpdFontFamily::SUP : EpdFontFamily::SUB;
  EXPECT_NE(static_cast<uint8_t>(style) & static_cast<uint8_t>(expectedStyle), 0u);

  ASSERT_EQ(parser.pendingFootnotes.size(), 1u);
  const FootnoteEntry& footnote = parser.pendingFootnotes.front().second;
  EXPECT_STREQ(footnote.href, expectedHref);
  ASSERT_EQ(parser.currentTextBlock->wordLinkIds.size(), 1u);
  EXPECT_EQ(parser.currentTextBlock->wordLinkIds.front(), linkId);
  EXPECT_TRUE(parser.currentTextBlock->linkTargetMatches(linkId, expectedHref));
}

INSTANTIATE_TEST_SUITE_P(CssVerticalAlign, ChapterHtmlSlimParserTest,
                         ::testing::Values("vertical-align: super", "vertical-align: sub"));

TEST(ParsedTextSpacingTest, DropsChineseWhitespaceAndPreservesVisibleOffsets) {
  ParsedText text(false);
  text.addWord("中", EpdFontFamily::REGULAR, false, false, 10);
  text.addWord("文", EpdFontFamily::BOLD, false, false, 12);
  ASSERT_EQ(text.size(), 2u);
  EXPECT_FALSE(text.wordContinues[1]);
  EXPECT_TRUE(text.wordNoSpaceBefore[1]);
  EXPECT_EQ(text.visibleOffsetAt(1), 12u);
  EXPECT_EQ(text.getWordStyleAt(1), EpdFontFamily::BOLD);
}

TEST(ParsedTextSpacingTest, KeepsKoreanWordSpaces) {
  ParsedText text(false);
  text.addWord("한", EpdFontFamily::REGULAR);
  text.addWord("글", EpdFontFamily::REGULAR);
  ASSERT_EQ(text.size(), 2u);
  EXPECT_FALSE(text.wordContinues[1]);
  EXPECT_FALSE(text.wordNoSpaceBefore[1]);
}

TEST(ParsedTextSpacingTest, KeepsLatinToChineseSpaces) {
  ParsedText text(false);
  text.addWord("hello", EpdFontFamily::REGULAR);
  text.addWord("中", EpdFontFamily::REGULAR);
  ASSERT_EQ(text.size(), 2u);
  EXPECT_FALSE(text.wordContinues[1]);
  EXPECT_FALSE(text.wordNoSpaceBefore[1]);
}

TEST(ParsedTextSpacingTest, KeepsClosingPunctuationAttached) {
  ParsedText text(false);
  text.addWord("中", EpdFontFamily::REGULAR);
  text.addWord("。", EpdFontFamily::REGULAR);
  ASSERT_EQ(text.size(), 2u);
  EXPECT_TRUE(text.wordContinues[1]);
  EXPECT_FALSE(text.wordNoSpaceBefore[1]);
}

}  // namespace
