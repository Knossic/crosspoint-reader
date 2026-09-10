#include "SdCardFontSystem.h"

#include <Arduino.h>
#include <EpdFontFamily.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <iterator>

#include "CrossPointSettings.h"
#include "ReaderFontSizes.h"
#include "fontIds.h"

namespace {

// Point the reader font size at a size the given family actually ships, and
// persist the change so the settings UI and the loaded font never disagree.
// Guarded by the value-change check: a no-op snap must not write SPIFFS.
void snapFontPointSizeTo(const uint8_t availablePointSize) {
  if (availablePointSize == 0 || availablePointSize == SETTINGS.fontPointSize) return;
  LOG_DBG("SDFS", "Font size %u unavailable, snapping to %u", SETTINGS.fontPointSize, availablePointSize);
  SETTINGS.fontPointSize = availablePointSize;
  SETTINGS.saveToFile();
}

// Built-in UI fonts and their physical point sizes (at 150 DPI, matching the
// SD-font converter). Prefer a same-size SD fallback; older font packs may
// only provide reader sizes. See SdCardFontSystem::setupUiFallbacks.
struct UiFontSize {
  int fontId;
  uint8_t pointSize;
};
constexpr UiFontSize kUiFontSizes[] = {
    {SMALL_FONT_ID, 8},
    {UI_10_FONT_ID, 10},
    {UI_12_FONT_ID, 12},
};

}  // namespace

void SdCardFontSystem::begin(GfxRenderer& renderer) {
  registry_.discover();

  // Register this system as the SD font ID resolver in settings.
  // Uses a static trampoline since CrossPointSettings stores a plain function pointer.
  SETTINGS.sdFontIdResolver = [](void* ctx, const char* familyName, uint8_t pointSize) -> int {
    return static_cast<SdCardFontSystem*>(ctx)->resolveFontId(familyName, pointSize);
  };
  SETTINGS.sdFontResolverCtx = this;

  // If user has a saved SD font selection, load it
  if (SETTINGS.sdFontFamilyName[0] != '\0') {
    const auto* family = registry_.findFamily(SETTINGS.sdFontFamilyName);
    if (family) {
      if (manager_.loadFamily(*family, renderer, SETTINGS.fontPointSize)) {
        snapFontPointSizeTo(manager_.currentPointSize());
        setupUiFallbacks(renderer);
        LOG_DBG("SDFS", "Loaded SD card font family: %s", SETTINGS.sdFontFamilyName);
      } else {
        LOG_ERR("SDFS", "Failed to load SD font family: %s (clearing)", SETTINGS.sdFontFamilyName);
        SETTINGS.clearSdFontFamily();
      }
    } else {
      LOG_DBG("SDFS", "SD font family not found on card: %s (clearing)", SETTINGS.sdFontFamilyName);
      SETTINGS.clearSdFontFamily();
    }
  }

  syncUiFallback(renderer);

  LOG_DBG("SDFS", "SD font system ready (%d families discovered)", registry_.getFamilyCount());
}

void SdCardFontSystem::ensureLoaded(GfxRenderer& renderer) {
  if (SETTINGS.sdFontFamilyName[0] != '\0') unloadUiFallback(renderer);
  ensureReaderFontLoaded(renderer);
  syncUiFallback(renderer);
}

void SdCardFontSystem::ensureReaderFontLoaded(GfxRenderer& renderer) {
  // If the web server (or another task) installed/deleted fonts, re-discover.
  // Track whether we just re-discovered so we can force a reload below even
  // when the wanted family/size still maps to the same point size — the file
  // contents on disk may have changed (e.g. user re-uploaded a new build).
  const bool registryWasDirty = registryDirty_.exchange(false, std::memory_order_acquire);
  if (registryWasDirty) {
    unloadUiFallback(renderer);
    LOG_DBG("SDFS", "Registry dirty — re-discovering fonts");
    registry_.discover();
  }

  const char* wantedFamily = SETTINGS.sdFontFamilyName;
  const std::string& currentFamily = manager_.currentFamilyName();

  if (wantedFamily[0] == '\0') {
    if (!currentFamily.empty()) {
      manager_.unloadAll(renderer);
    }
    // Back on a built-in family, which exists only at BUILTIN_READER_POINT_SIZES:
    // a size inherited from an SD family has to come back into that set.
    snapFontPointSizeTo(snapToNearestPointSize(BUILTIN_READER_POINT_SIZES, std::size(BUILTIN_READER_POINT_SIZES),
                                               SETTINGS.fontPointSize));
    return;
  }

  // Reload if family changed OR if the user-selected size maps to a
  // different file than what's currently loaded OR if the registry was
  // just rediscovered (file may have been replaced on disk).
  bool familyMatches = (currentFamily == wantedFamily);
  if (familyMatches) {
    const auto* family = registry_.findFamily(wantedFamily);
    if (!family) {
      LOG_DBG("SDFS", "SD font family disappeared: %s (clearing)", wantedFamily);
      manager_.unloadAll(renderer);
      SETTINGS.clearSdFontFamily();
      return;
    }
    const auto* selected = family->findNearestSize(SETTINGS.fontPointSize);
    const uint8_t wantedPt = selected ? selected->pointSize : 0;
    // Snap before the early return: the wanted size can already be loaded while
    // the setting still names a size this family does not ship.
    snapFontPointSizeTo(wantedPt);
    if (!registryWasDirty && wantedPt == manager_.currentPointSize()) return;
    LOG_DBG("SDFS", "Reloading %s: size %u -> %u%s", wantedFamily, manager_.currentPointSize(), wantedPt,
            registryWasDirty ? " [registry dirty]" : "");
  }

  if (!currentFamily.empty()) {
    manager_.unloadAll(renderer);
  }

  const auto* family = registry_.findFamily(wantedFamily);
  if (family) {
    if (manager_.loadFamily(*family, renderer, SETTINGS.fontPointSize)) {
      snapFontPointSizeTo(manager_.currentPointSize());
      setupUiFallbacks(renderer);
      LOG_DBG("SDFS", "Loaded SD font family: %s", wantedFamily);
    } else {
      LOG_ERR("SDFS", "Failed to load SD font family: %s (clearing)", wantedFamily);
      SETTINGS.clearSdFontFamily();
    }
  } else {
    LOG_DBG("SDFS", "SD font family not found: %s (clearing)", wantedFamily);
    SETTINGS.clearSdFontFamily();
  }
}

void SdCardFontSystem::setupUiFallbacks(GfxRenderer& renderer) {
  const std::string& familyName = manager_.currentFamilyName();
  if (familyName.empty()) return;  // no SD family loaded — nothing to fall back to

  const auto* family = registry_.findFamily(familyName);
  if (!family) return;

  // Probe the already-loaded reader-size font before paying for the UI sizes:
  // resolveTextFontId only redirects on CJK codepoints, so a Latin-only family
  // can never act as a fallback and its UI sizes would be dead weight in RAM.
  const auto readerIt = renderer.getFontMap().find(manager_.getFontId(familyName));
  if (readerIt == renderer.getFontMap().end()) return;
  // One representative codepoint per script: Han, Hiragana, Katakana, Hangul.
  static constexpr uint32_t kCjkProbes[] = {0x4E00, 0x3042, 0x30A2, 0xAC00};
  bool hasCjk = false;
  for (const uint32_t cp : kCjkProbes) {
    if (readerIt->second.hasCodepoint(cp)) {
      hasCjk = true;
      break;
    }
  }
  if (!hasCjk) {
    LOG_DBG("SDFS", "%s has no CJK coverage - skipping UI fallback sizes", familyName.c_str());
    return;
  }

  for (const auto& ui : kUiFontSizes) {
    const auto* file = family->findNearestSize(ui.pointSize);
    if (!file) continue;
    const int sdFontId = manager_.loadFamilyExtraSize(*family, renderer, file->pointSize);
    if (sdFontId != 0) {
      renderer.setFallbackFont(ui.fontId, sdFontId);
    } else {
      LOG_DBG("SDFS", "No %u pt SD glyphs for UI fallback in %s", ui.pointSize, familyName.c_str());
    }
  }
}

int SdCardFontSystem::resolveFontId(const char* familyName, uint8_t /*pointSize*/) const {
  // The manager holds exactly one reader-size font, already selected for
  // SETTINGS.fontPointSize, so the size argument is implicit — always return
  // that font's ID. ensureLoaded() must have run for the current settings first.
  return manager_.getFontId(familyName);
}

void SdCardFontSystem::unloadUiFallback(GfxRenderer& renderer) {
  if (!uiFallbackFont_) return;
  if (manager_.currentFamilyName().empty()) renderer.clearFallbackFonts();
  renderer.removeFont(uiFallbackFontId_);
  uiFallbackFont_.reset();
  uiFallbackFontId_ = 0;
  uiFallbackFamilyName_.clear();
  uiFallbackPointSize_ = 0;
}

void SdCardFontSystem::syncUiFallback(GfxRenderer& renderer) {
  if (!manager_.currentFamilyName().empty()) {
    unloadUiFallback(renderer);
    return;
  }
  // Family policy: the user's selected reader family when set, otherwise the
  // first discovered family. No SD families installed -> no fallback (UI
  // behaves exactly as before: missing glyphs render as the replacement box).
  const SdCardFontFamilyInfo* family = nullptr;
  if (SETTINGS.sdFontFamilyName[0] != '\0') {
    family = registry_.findFamily(SETTINGS.sdFontFamilyName);
  }
  if (!family && registry_.getFamilyCount() > 0) {
    family = &registry_.getFamilies().front();
  }
  if (!family || family->files.empty()) {
    unloadUiFallback(renderer);
    return;
  }

  // Smallest available size: UI chrome fonts are 10-12pt, and one size keeps
  // the resident interval tables + glyph caches to a single font's worth.
  const SdCardFontFileInfo* selected = &*std::min_element(
      family->files.begin(), family->files.end(),
      [](const SdCardFontFileInfo& a, const SdCardFontFileInfo& b) { return a.pointSize < b.pointSize; });

  // Still loaded and still registered? Reader font reloads wipe all SD font
  // registrations in the renderer (clearSdCardFonts), so re-check both sides.
  if (uiFallbackFont_ && uiFallbackFamilyName_ == family->name && uiFallbackPointSize_ == selected->pointSize &&
      renderer.isSdCardFont(uiFallbackFontId_)) {
    return;
  }

  unloadUiFallback(renderer);

  const uint32_t heapBefore = ESP.getFreeHeap();
  auto font = makeUniqueNoThrow<SdCardFont>();
  if (!font) {
    LOG_ERR("SDFS", "OOM: UI fallback SdCardFont");
    return;
  }
  if (!font->load(selected->path.c_str())) {
    LOG_ERR("SDFS", "UI fallback load failed: %s", selected->path.c_str());
    return;
  }

  // Decorated name gives an ID distinct from a reader instance of the same
  // file (\x01 cannot appear in a directory-derived family name).
  char decorated[80];
  snprintf(decorated, sizeof(decorated), "%s\x01ui", family->name.c_str());
  const int fontId = SdCardFontManager::computeFontId(font->contentHash(), decorated, selected->pointSize);
  if (renderer.getFontMap().count(fontId) != 0) {
    LOG_ERR("SDFS", "UI fallback font ID %d collides with existing font, skipping", fontId);
    return;
  }

  renderer.registerSdCardFont(fontId, font.get());
  renderer.insertFont(
      fontId, EpdFontFamily(font->getEpdFont(0), font->getEpdFont(1), font->getEpdFont(2), font->getEpdFont(3)));
  for (const auto& ui : kUiFontSizes) renderer.setFallbackFont(ui.fontId, fontId);

  uiFallbackFont_ = std::move(font);
  uiFallbackFontId_ = fontId;
  uiFallbackFamilyName_ = family->name;
  uiFallbackPointSize_ = selected->pointSize;
  LOG_DBG("SDFS", "UI fallback loaded: %s %upt id=%d heap %lu -> %lu", uiFallbackFamilyName_.c_str(),
          uiFallbackPointSize_, uiFallbackFontId_, static_cast<unsigned long>(heapBefore),
          static_cast<unsigned long>(ESP.getFreeHeap()));
}
