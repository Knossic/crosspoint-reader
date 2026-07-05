#include "TxtReaderActivity.h"

#include <BidiUtils.h>
#include <Epub/hyphenation/Hyphenator.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Serialization.h>
#include <Utf8.h>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "ProgressFile.h"
#include "ReaderUtils.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr size_t CHUNK_SIZE = 8 * 1024;  // 8KB chunk for reading
// Cache file magic and version
constexpr uint32_t CACHE_MAGIC = 0x54585449;  // "TXTI"
constexpr uint8_t CACHE_VERSION = 4;          // Increment when cache format changes
}  // namespace

void TxtReaderActivity::onEnter() {
  Activity::onEnter();

  if (!txt) {
    return;
  }

  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

  txt->setupCacheDir();

  // Save current txt as last opened file and add to recent books
  auto filePath = txt->getPath();
  auto fileName = filePath.substr(filePath.rfind('/') + 1);
  APP_STATE.openEpubPath = filePath;
  APP_STATE.saveToFile();
  RECENT_BOOKS.addBook(filePath, fileName, "", "");

  // Trigger first update
  requestUpdate();
}

void TxtReaderActivity::onExit() {
  Activity::onExit();

  // Reset orientation back to portrait for the rest of the UI
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  pageOffsets.clear();
  currentPageLines.clear();
  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();
  txt.reset();
}

void TxtReaderActivity::loop() {
  // While the page-index build runs on the render task, queuing a Pop here
  // would block the main task on the RenderLock until the build finishes
  // (minutes for large books) — the "dead buttons during indexing" freeze.
  // Instead, Back requests an abort; the build loop notices within one page.
  if (indexBuilding_.load(std::memory_order_relaxed)) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      indexAbortRequested_.store(true, std::memory_order_relaxed);
    }
    return;
  }
  // Build was aborted: leave the activity the same way a Back press would.
  if (indexAborted_.load(std::memory_order_relaxed)) {
    indexAborted_.store(false, std::memory_order_relaxed);
    onGoHome();
    return;
  }

  // Long press BACK (1s+) goes to file selection
  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= ReaderUtils::GO_HOME_MS) {
    activityManager.goToFileBrowser(txt ? txt->getPath() : "");
    return;
  }

  // Short press BACK goes directly to home
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() < ReaderUtils::GO_HOME_MS) {
    onGoHome();
    return;
  }

  const auto [prevTriggered, nextTriggered, fromTilt] = ReaderUtils::detectPageTurn(mappedInput);
  if (!prevTriggered && !nextTriggered) {
    return;
  }

  if (prevTriggered && currentPage > 0) {
    currentPage--;
    requestUpdate();
  } else if (nextTriggered) {
    if (currentPage < totalPages - 1) {
      currentPage++;
      requestUpdate();
    } else {
      onGoHome();
    }
  }
}

void TxtReaderActivity::initializeReader() {
  if (initialized) {
    return;
  }

  // Store current settings for cache validation
  cachedFontId = SETTINGS.getReaderFontId();
  cachedScreenMargin = SETTINGS.screenMargin;
  cachedParagraphAlignment = SETTINGS.paragraphAlignment;
  cachedHyphenationEnabled = SETTINGS.hyphenationEnabled;

  // Plain text has no language metadata, so use the UI language as the
  // hyphenation-pattern hint (unknown codes simply disable pattern breaks).
  Hyphenator::setPreferredLanguage(I18n::getLanguageCode(I18N.getLanguage()));

  // Calculate viewport dimensions
  renderer.getOrientedViewableTRBL(&cachedOrientedMarginTop, &cachedOrientedMarginRight, &cachedOrientedMarginBottom,
                                   &cachedOrientedMarginLeft);
  cachedOrientedMarginTop += cachedScreenMargin;
  cachedOrientedMarginLeft += cachedScreenMargin;
  cachedOrientedMarginRight += cachedScreenMargin;
  cachedOrientedMarginBottom +=
      std::max(cachedScreenMargin, static_cast<uint8_t>(UITheme::getInstance().getStatusBarHeight()));

  viewportWidth = renderer.getScreenWidth() - cachedOrientedMarginLeft - cachedOrientedMarginRight;
  const int viewportHeight = renderer.getScreenHeight() - cachedOrientedMarginTop - cachedOrientedMarginBottom;
  const int lineHeight = renderer.getLineHeight(cachedFontId);

  linesPerPage = viewportHeight / lineHeight;
  if (linesPerPage < 1) linesPerPage = 1;

  LOG_DBG("TRS", "Viewport: %dx%d, lines per page: %d", viewportWidth, viewportHeight, linesPerPage);

  // Try to load cached page index first
  if (!loadPageIndexCache()) {
    // Cache not found, build page index
    if (!buildPageIndex()) {
      // Aborted by the user: persist nothing (a partial index would be
      // indistinguishable from a complete one) and let loop() exit.
      pageOffsets.clear();
      totalPages = 0;
      initialized = true;  // don't re-trigger the build on a re-render
      indexAborted_.store(true, std::memory_order_relaxed);
      return;
    }
    // Save to cache for next time
    savePageIndexCache();
  }

  // Load saved progress
  loadProgress();

  initialized = true;
}

bool TxtReaderActivity::buildPageIndex() {
  pageOffsets.clear();
  pageOffsets.push_back(0);  // First page starts at offset 0

  size_t offset = 0;
  const size_t fileSize = txt->getFileSize();

  LOG_DBG("TRS", "Building page index for %zu bytes...", fileSize);

  indexAbortRequested_.store(false, std::memory_order_relaxed);
  indexBuilding_.store(true, std::memory_order_relaxed);
  GUI.drawPopup(renderer, tr(STR_INDEXING));

  // Progress popup throttle: each drawPopup costs a display refresh
  // (~500ms), so repaint only after both 5 percentage points AND 2 seconds.
  int lastShownPct = 0;
  unsigned long lastPopupMs = millis();

  while (offset < fileSize) {
    if (indexAbortRequested_.load(std::memory_order_relaxed)) {
      indexBuilding_.store(false, std::memory_order_relaxed);
      LOG_DBG("TRS", "Page index build aborted at %zu/%zu bytes", offset, fileSize);
      return false;
    }

    std::vector<std::string> tempLines;
    size_t nextOffset = offset;

    if (!loadPageAtOffset(offset, tempLines, nextOffset)) {
      break;
    }

    if (nextOffset <= offset) {
      // No progress made, avoid infinite loop
      break;
    }

    offset = nextOffset;
    if (offset < fileSize) {
      pageOffsets.push_back(offset);
    }

    const int pct = static_cast<int>((static_cast<uint64_t>(offset) * 100) / fileSize);
    if (pct >= lastShownPct + 5 && millis() - lastPopupMs >= 2000) {
      char msg[32];
      snprintf(msg, sizeof(msg), tr(STR_INDEXING_PERCENT), pct);
      GUI.drawPopup(renderer, msg);
      lastShownPct = pct;
      lastPopupMs = millis();
    }

    // Yield to other tasks periodically
    if (pageOffsets.size() % 20 == 0) {
      vTaskDelay(1);
    }
  }

  totalPages = pageOffsets.size();
  indexBuilding_.store(false, std::memory_order_relaxed);
  LOG_DBG("TRS", "Built page index: %d pages", totalPages);
  return true;
}

bool TxtReaderActivity::loadPageAtOffset(size_t offset, std::vector<std::string>& outLines, size_t& nextOffset) {
  outLines.clear();
  const size_t fileSize = txt->getFileSize();

  if (offset >= fileSize) {
    return false;
  }

  // Read a chunk from file
  size_t chunkSize = std::min(CHUNK_SIZE, fileSize - offset);
  auto* buffer = static_cast<uint8_t*>(malloc(chunkSize + 1));
  if (!buffer) {
    LOG_ERR("TRS", "Failed to allocate %zu bytes", chunkSize);
    return false;
  }

  if (!txt->readContent(buffer, offset, chunkSize)) {
    free(buffer);
    return false;
  }
  buffer[chunkSize] = '\0';

  // Prime the SD card font's advance table with this chunk's codepoints.
  // Without this, every getTextAdvanceX() call in the wrap loop below triggers
  // on-demand glyph loads through the 8-slot overflow ring buffer, which
  // thrashes for any text with more than 8 unique chars (i.e. all English),
  // floods the heap with short-lived bitmap allocations, and eventually
  // corrupts FreeRTOS state. The advance table persists across calls per
  // font, so the cost amortizes to ~ASCII-size after the first chunk.
  if (renderer.isSdCardFont(cachedFontId)) {
    renderer.ensureSdCardFontReady(cachedFontId, reinterpret_cast<const char*>(buffer), /*styleMask=*/0x01);
    if (cachedHyphenationEnabled) {
      // Inserted hyphens are not necessarily present in the chunk text itself.
      renderer.ensureSdCardFontReady(cachedFontId, "-", /*styleMask=*/0x01);
    }
  }

  // Parse lines from buffer
  size_t pos = 0;

  while (pos < chunkSize && static_cast<int>(outLines.size()) < linesPerPage) {
    // Find end of line
    size_t lineEnd = pos;
    while (lineEnd < chunkSize && buffer[lineEnd] != '\n') {
      lineEnd++;
    }

    // Check if we have a complete line
    bool lineComplete = (lineEnd < chunkSize) || (offset + lineEnd >= fileSize);

    if (!lineComplete && static_cast<int>(outLines.size()) > 0) {
      // Incomplete line and we already have some lines, stop here
      break;
    }

    // Calculate the actual length of line content in the buffer (excluding newline)
    size_t lineContentLen = lineEnd - pos;

    // Check for carriage return
    bool hasCR = (lineContentLen > 0 && buffer[pos + lineContentLen - 1] == '\r');
    size_t displayLen = hasCR ? lineContentLen - 1 : lineContentLen;

    // Extract line content for display (without CR/LF)
    std::string line(reinterpret_cast<char*>(buffer + pos), displayLen);

    // Track position within this source line (in bytes from pos)
    size_t lineBytePos = 0;

    if (line.empty()) {
      outLines.emplace_back();
    } else {
      lineBytePos = wrapSourceLine(line, outLines);
    }

    // Determine how much of the source buffer we consumed
    if (lineBytePos >= displayLen) {
      // Fully consumed this source line, move past the newline
      pos = lineEnd + 1;
    } else {
      // Partially consumed - page is full mid-line
      // Move pos to where we stopped in the line (NOT past the line)
      pos = pos + lineBytePos;
      break;
    }
  }

  // Ensure we make progress even if calculations go wrong
  if (pos == 0 && !outLines.empty()) {
    // Fallback: at minimum, consume something to avoid infinite loop
    pos = 1;
  }

  nextOffset = offset + pos;

  // Make sure we don't go past the file
  if (nextOffset > fileSize) {
    nextOffset = fileSize;
  }

  free(buffer);

  return !outLines.empty();
}

// Wraps one source line (no CR/LF) into visual lines, appending to outLines until
// the line is fully consumed or the page is full. Returns bytes of `line` consumed.
//
// Break priority at the overflow point: hyphenation of the straddling word (when
// enabled), else the rightmost space or CJK inter-character opportunity (kinsoku
// respected), else a hard break at the last fitting codepoint. All width checks
// use getTextAdvanceX() on the exact string that will be drawn, so the fit
// decision matches drawText() to the pixel.
size_t TxtReaderActivity::wrapSourceLine(const std::string& line, std::vector<std::string>& outLines) const {
  // Scratch vectors are bounded by the codepoints scanned per visual line
  // (roughly 2x what fits, thanks to the galloping probe), not by line length.
  std::vector<uint32_t> cps;     // decoded codepoints from the current scan start
  std::vector<uint32_t> cpEnds;  // byte offset into `line` just past cps[i]
  cps.reserve(96);
  cpEnds.reserve(96);

  const auto stripSoftHyphens = [](std::string& s) {
    size_t p = 0;
    while ((p = s.find("\xC2\xAD", p)) != std::string::npos) s.erase(p, 2);
  };

  // Measures line[startByte, endByte) as it would be drawn (soft hyphens stripped,
  // optional visible hyphen appended).
  const auto measure = [&](const size_t startByte, const size_t endByte, const bool appendHyphen) {
    std::string s = line.substr(startByte, endByte - startByte);
    stripSoftHyphens(s);
    if (appendHyphen) s.push_back('-');
    return renderer.getTextAdvanceX(cachedFontId, s.c_str(), EpdFontFamily::REGULAR);
  };

  const auto isWordSeparator = [](const uint32_t cp) { return cp == ' ' || utf8IsCjkBreakable(cp); };

  size_t lineStart = 0;
  while (lineStart < line.size() && static_cast<int>(outLines.size()) < linesPerPage) {
    cps.clear();
    cpEnds.clear();
    const auto* base = reinterpret_cast<const unsigned char*>(line.c_str());
    const unsigned char* decodePtr = base + lineStart;

    // Decode until `count` codepoints are available (or the line ends).
    const auto decodeTo = [&](const size_t count) {
      while (cps.size() < count && *decodePtr) {
        const uint32_t cp = utf8NextCodepoint(&decodePtr);
        if (cp == 0) break;
        cps.push_back(cp);
        cpEnds.push_back(static_cast<uint32_t>(decodePtr - base));
      }
      return cps.size();
    };

    // Gallop then bisect for the largest codepoint-prefix that fits the viewport.
    size_t fitCount = 0;  // codepoints known to fit
    bool wholeLineFits = false;
    for (size_t probe = 16;; probe *= 2) {
      const size_t available = decodeTo(probe);
      if (available == 0) {
        // Malformed trailing UTF-8: emit the raw remainder to guarantee progress.
        outLines.push_back(line.substr(lineStart));
        return line.size();
      }
      if (measure(lineStart, cpEnds[available - 1], false) <= viewportWidth) {
        fitCount = available;
        if (available < probe) {  // line exhausted before overflow
          wholeLineFits = true;
          break;
        }
        continue;
      }
      size_t bad = available;  // smallest count known to overflow
      while (bad - fitCount > 1) {
        const size_t mid = fitCount + (bad - fitCount) / 2;
        if (measure(lineStart, cpEnds[mid - 1], false) <= viewportWidth) {
          fitCount = mid;
        } else {
          bad = mid;
        }
      }
      break;
    }

    if (wholeLineFits) {
      std::string rest = line.substr(lineStart);
      stripSoftHyphens(rest);
      outLines.push_back(std::move(rest));
      return line.size();
    }

    if (fitCount == 0) {
      // Even a single codepoint overflows: emit it alone to guarantee progress.
      outLines.push_back(line.substr(lineStart, cpEnds[0] - lineStart));
      lineStart = cpEnds[0];
      continue;
    }

    // Rightmost legal break opportunity within the fitting prefix. A break
    // before cps[i] emits [lineStart, emitEnd) and resumes at `resume`.
    size_t emitEnd = 0;
    size_t resume = 0;
    bool hasBreak = false;
    for (size_t i = 1; i <= fitCount; ++i) {  // cps[fitCount] exists: it overflowed
      if (cps[i - 1] == ' ') {
        // Trim the whole run of spaces from the line end; skip exactly one when resuming.
        size_t j = i - 1;
        while (j > 0 && cps[j - 1] == ' ') --j;
        const size_t end = (j >= 1) ? cpEnds[j - 1] : lineStart;
        if (end > lineStart) {
          emitEnd = end;
          resume = cpEnds[i - 1];
          hasBreak = true;
        }
      } else if (utf8HasCjkBreakOpportunityBetween(cps[i - 1], cps[i])) {
        emitEnd = cpEnds[i - 1];
        resume = cpEnds[i - 1];
        hasBreak = true;
      }
    }
    if (cps[fitCount] == ' ') {
      // The overflow codepoint is itself a space: the entire fitting prefix ends a
      // word, so break right there and resume past the space.
      size_t j = fitCount;
      while (j > 0 && cps[j - 1] == ' ') --j;
      if (j >= 1) {
        emitEnd = cpEnds[j - 1];
        resume = cpEnds[fitCount];
        hasBreak = true;
      }
    }

    bool emitted = false;
    if (cachedHyphenationEnabled) {
      // Word straddling the fit boundary: cps[fitCount] is the first codepoint
      // that no longer fits; hyphenating its word can reclaim the slack that a
      // break at the last separator would leave.
      size_t wordStartIdx = fitCount;
      while (wordStartIdx > 0 && !isWordSeparator(cps[wordStartIdx - 1])) --wordStartIdx;
      if (wordStartIdx < fitCount && !isWordSeparator(cps[fitCount])) {
        // Bounded forward scan for the word end (it may extend past the overflow point).
        constexpr size_t MAX_WORD_SCAN_CPS = 64;
        size_t wordEndIdx = fitCount;
        while (wordEndIdx < wordStartIdx + MAX_WORD_SCAN_CPS && decodeTo(wordEndIdx + 1) > wordEndIdx &&
               !isWordSeparator(cps[wordEndIdx])) {
          ++wordEndIdx;
        }
        const size_t wordStartByte = (wordStartIdx > 0) ? cpEnds[wordStartIdx - 1] : lineStart;
        const size_t wordEndByte = cpEnds[wordEndIdx - 1];
        const std::string word = line.substr(wordStartByte, wordEndByte - wordStartByte);
        // Fallback every-N splitting only when there is no other way to break the line.
        const auto breaks = Hyphenator::breakOffsets(word, /*includeFallback=*/!hasBreak);
        for (auto it = breaks.rbegin(); it != breaks.rend() && !emitted; ++it) {
          const size_t candidateEnd = wordStartByte + it->byteOffset;
          // Prefixes past the overflow codepoint cannot fit once the hyphen is added.
          if (candidateEnd <= lineStart || candidateEnd > cpEnds[fitCount]) continue;
          if (measure(lineStart, candidateEnd, it->requiresInsertedHyphen) <= viewportWidth) {
            std::string cand = line.substr(lineStart, candidateEnd - lineStart);
            stripSoftHyphens(cand);
            if (it->requiresInsertedHyphen) cand.push_back('-');
            outLines.push_back(std::move(cand));
            lineStart = candidateEnd;
            emitted = true;
          }
        }
      }
    }

    if (!emitted) {
      if (hasBreak) {
        std::string out = line.substr(lineStart, emitEnd - lineStart);
        stripSoftHyphens(out);
        outLines.push_back(std::move(out));
        lineStart = resume;
      } else {
        // No separator and no hyphenation point: hard break after the last fitting codepoint.
        std::string out = line.substr(lineStart, cpEnds[fitCount - 1] - lineStart);
        stripSoftHyphens(out);
        outLines.push_back(std::move(out));
        lineStart = cpEnds[fitCount - 1];
      }
    }
  }
  return lineStart;
}

void TxtReaderActivity::render(RenderLock&&) {
  if (!txt) {
    return;
  }

  // Initialize reader if not done
  if (!initialized) {
    initializeReader();
  }

  if (indexAborted_.load(std::memory_order_relaxed)) {
    return;  // build cancelled; loop() is about to exit the activity
  }

  if (pageOffsets.empty()) {
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_EMPTY_FILE), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  // Bounds check
  if (currentPage < 0) currentPage = 0;
  if (currentPage >= totalPages) currentPage = totalPages - 1;

  // Load current page content
  size_t offset = pageOffsets[currentPage];
  size_t nextOffset;
  currentPageLines.clear();
  loadPageAtOffset(offset, currentPageLines, nextOffset);

  renderer.clearScreen();
  renderPage();

  // Save progress
  saveProgress();
}

void TxtReaderActivity::renderPage() {
  const int lineHeight = renderer.getLineHeight(cachedFontId);
  const int contentWidth = viewportWidth;

  // Render text lines with alignment
  auto renderLines = [&]() {
    int y = cachedOrientedMarginTop;
    for (const auto& line : currentPageLines) {
      if (!line.empty()) {
        int x = cachedOrientedMarginLeft;
        const bool lineIsRtl = BidiUtils::startsWithRtl(line.c_str(), BidiUtils::RTL_PARAGRAPH_PROBE_DEPTH);
        uint8_t effectiveAlignment = cachedParagraphAlignment;
        if (lineIsRtl && (effectiveAlignment == CrossPointSettings::LEFT_ALIGN ||
                          effectiveAlignment == CrossPointSettings::JUSTIFIED)) {
          effectiveAlignment = CrossPointSettings::RIGHT_ALIGN;
        }
        const int textWidth = renderer.getTextAdvanceX(cachedFontId, line.c_str(), EpdFontFamily::REGULAR);

        // Apply text alignment
        switch (effectiveAlignment) {
          case CrossPointSettings::LEFT_ALIGN:
          default:
            // x already set to left margin
            break;
          case CrossPointSettings::CENTER_ALIGN: {
            x = cachedOrientedMarginLeft + (contentWidth - textWidth) / 2;
            break;
          }
          case CrossPointSettings::RIGHT_ALIGN: {
            x = cachedOrientedMarginLeft + contentWidth - textWidth;
            break;
          }
          case CrossPointSettings::JUSTIFIED:
            // For plain text, justified is treated as left-aligned
            // (true justification would require word spacing adjustments)
            break;
        }

        renderer.drawText(cachedFontId, x, y, line.c_str());
      }
      y += lineHeight;
    }
  };

  // Font prewarm: scan pass accumulates text, then prewarm, then real render
  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  renderLines();  // scan pass — text accumulated, no drawing
  scope.endScanAndPrewarm();

  // BW rendering
  renderLines();
  renderStatusBar();

  ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);

  if (SETTINGS.textAntiAliasing) {
    ReaderUtils::renderAntiAliased(renderer, [&renderLines]() { renderLines(); });
  }
  // scope destructor clears font cache via FontCacheManager
}

void TxtReaderActivity::renderStatusBar() const {
  const float progress = totalPages > 0 ? (currentPage + 1) * 100.0f / totalPages : 0;
  std::string title;
  if (SETTINGS.statusBarTitle != CrossPointSettings::STATUS_BAR_TITLE::HIDE_TITLE) {
    title = txt->getTitle();
  }
  GUI.drawStatusBar(renderer, progress, currentPage + 1, totalPages, title);
}

void TxtReaderActivity::saveProgress() const {
  uint8_t data[4];
  data[0] = currentPage & 0xFF;
  data[1] = (currentPage >> 8) & 0xFF;
  data[2] = 0;
  data[3] = 0;
  if (!ProgressFile::writeAtomic(txt->getCachePath(), data, sizeof(data))) {
    LOG_ERR("TRS", "Failed to save progress: page %d", currentPage);
  }
}

void TxtReaderActivity::loadProgress() {
  HalFile f;
  if (Storage.openFileForRead("TRS", txt->getCachePath() + "/progress.bin", f)) {
    uint8_t data[4];
    if (f.read(data, 4) == 4) {
      currentPage = data[0] + (data[1] << 8);
      if (currentPage >= totalPages) {
        currentPage = totalPages - 1;
      }
      if (currentPage < 0) {
        currentPage = 0;
      }
      LOG_DBG("TRS", "Loaded progress: page %d/%d", currentPage, totalPages);
    }
  }
}

bool TxtReaderActivity::loadPageIndexCache() {
  // Cache file format (using serialization module):
  // - uint32_t: magic "TXTI"
  // - uint8_t: cache version
  // - uint32_t: file size (to validate cache)
  // - int32_t: viewport width
  // - int32_t: lines per page
  // - int32_t: font ID (to invalidate cache on font change)
  // - int32_t: screen margin (to invalidate cache on margin change)
  // - uint8_t: paragraph alignment (to invalidate cache on alignment change)
  // - uint8_t: hyphenation enabled (to invalidate cache on hyphenation toggle)
  // - uint32_t: total pages count
  // - N * uint32_t: page offsets

  std::string cachePath = txt->getCachePath() + "/index.bin";
  HalFile f;
  if (!Storage.openFileForRead("TRS", cachePath, f)) {
    LOG_DBG("TRS", "No page index cache found");
    return false;
  }

  // Read and validate header using serialization module
  uint32_t magic;
  serialization::readPod(f, magic);
  if (magic != CACHE_MAGIC) {
    LOG_DBG("TRS", "Cache magic mismatch, rebuilding");
    return false;
  }

  uint8_t version;
  serialization::readPod(f, version);
  if (version != CACHE_VERSION) {
    LOG_DBG("TRS", "Cache version mismatch (%d != %d), rebuilding", version, CACHE_VERSION);
    return false;
  }

  uint32_t fileSize;
  serialization::readPod(f, fileSize);
  if (fileSize != txt->getFileSize()) {
    LOG_DBG("TRS", "Cache file size mismatch, rebuilding");
    return false;
  }

  int32_t cachedWidth;
  serialization::readPod(f, cachedWidth);
  if (cachedWidth != viewportWidth) {
    LOG_DBG("TRS", "Cache viewport width mismatch, rebuilding");
    return false;
  }

  int32_t cachedLines;
  serialization::readPod(f, cachedLines);
  if (cachedLines != linesPerPage) {
    LOG_DBG("TRS", "Cache lines per page mismatch, rebuilding");
    return false;
  }

  int32_t fontId;
  serialization::readPod(f, fontId);
  if (fontId != cachedFontId) {
    LOG_DBG("TRS", "Cache font ID mismatch (%d != %d), rebuilding", fontId, cachedFontId);
    return false;
  }

  int32_t margin;
  serialization::readPod(f, margin);
  if (margin != cachedScreenMargin) {
    LOG_DBG("TRS", "Cache screen margin mismatch, rebuilding");
    return false;
  }

  uint8_t alignment;
  serialization::readPod(f, alignment);
  if (alignment != cachedParagraphAlignment) {
    LOG_DBG("TRS", "Cache paragraph alignment mismatch, rebuilding");
    return false;
  }

  uint8_t hyphenation;
  serialization::readPod(f, hyphenation);
  if (hyphenation != cachedHyphenationEnabled) {
    LOG_DBG("TRS", "Cache hyphenation mismatch, rebuilding");
    return false;
  }

  uint32_t numPages;
  serialization::readPod(f, numPages);

  // Read page offsets
  pageOffsets.clear();
  pageOffsets.reserve(numPages);

  for (uint32_t i = 0; i < numPages; i++) {
    uint32_t offset;
    serialization::readPod(f, offset);
    pageOffsets.push_back(offset);
  }

  totalPages = pageOffsets.size();
  LOG_DBG("TRS", "Loaded page index cache: %d pages", totalPages);
  return true;
}

void TxtReaderActivity::savePageIndexCache() const {
  std::string cachePath = txt->getCachePath() + "/index.bin";
  HalFile f;
  if (!Storage.openFileForWrite("TRS", cachePath, f)) {
    LOG_ERR("TRS", "Failed to save page index cache");
    return;
  }

  // Write header using serialization module
  serialization::writePod(f, CACHE_MAGIC);
  serialization::writePod(f, CACHE_VERSION);
  serialization::writePod(f, static_cast<uint32_t>(txt->getFileSize()));
  serialization::writePod(f, static_cast<int32_t>(viewportWidth));
  serialization::writePod(f, static_cast<int32_t>(linesPerPage));
  serialization::writePod(f, static_cast<int32_t>(cachedFontId));
  serialization::writePod(f, static_cast<int32_t>(cachedScreenMargin));
  serialization::writePod(f, cachedParagraphAlignment);
  serialization::writePod(f, cachedHyphenationEnabled);
  serialization::writePod(f, static_cast<uint32_t>(pageOffsets.size()));

  // Write page offsets
  for (size_t offset : pageOffsets) {
    serialization::writePod(f, static_cast<uint32_t>(offset));
  }

  LOG_DBG("TRS", "Saved page index cache: %d pages", totalPages);
}

ScreenshotInfo TxtReaderActivity::getScreenshotInfo() const {
  ScreenshotInfo info;
  info.readerType = ScreenshotInfo::ReaderType::Txt;
  if (txt) {
    const std::string t = txt->getTitle();
    snprintf(info.title, sizeof(info.title), "%s", t.c_str());
  }
  info.currentPage = currentPage + 1;
  info.totalPages = totalPages;
  info.progressPercent = totalPages > 0 ? static_cast<int>((currentPage + 1) * 100.0f / totalPages + 0.5f) : 0;
  if (info.progressPercent > 100) info.progressPercent = 100;
  return info;
}
