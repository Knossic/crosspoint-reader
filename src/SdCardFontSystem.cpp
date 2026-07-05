#include "SdCardFontSystem.h"

#include <Arduino.h>
#include <EpdFontFamily.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>

#include "CrossPointSettings.h"

namespace {

static uint8_t fontSizeEnumFromSettings() {
  uint8_t e = SETTINGS.fontSize;
  if (e >= CrossPointSettings::FONT_SIZE_COUNT) e = 1;  // default to MEDIUM
  return e;
}

}  // namespace

void SdCardFontSystem::begin(GfxRenderer& renderer) {
  registry_.discover();

  // Register this system as the SD font ID resolver in settings.
  // Uses a static trampoline since CrossPointSettings stores a plain function pointer.
  SETTINGS.sdFontIdResolver = [](void* ctx, const char* familyName, uint8_t fontSizeEnum) -> int {
    return static_cast<SdCardFontSystem*>(ctx)->resolveFontId(familyName, fontSizeEnum);
  };
  SETTINGS.sdFontResolverCtx = this;

  // If user has a saved SD font selection, load it
  if (SETTINGS.sdFontFamilyName[0] != '\0') {
    const auto* family = registry_.findFamily(SETTINGS.sdFontFamilyName);
    if (family) {
      if (manager_.loadFamily(*family, renderer, fontSizeEnumFromSettings())) {
        LOG_DBG("SDFS", "Loaded SD card font family: %s", SETTINGS.sdFontFamilyName);
      } else {
        LOG_ERR("SDFS", "Failed to load SD font family: %s (clearing)", SETTINGS.sdFontFamilyName);
        SETTINGS.sdFontFamilyName[0] = '\0';
        SETTINGS.saveToFile();
      }
    } else {
      LOG_DBG("SDFS", "SD font family not found on card: %s (clearing)", SETTINGS.sdFontFamilyName);
      SETTINGS.sdFontFamilyName[0] = '\0';
      SETTINGS.saveToFile();
    }
  }

  syncUiFallback(renderer);

  LOG_DBG("SDFS", "SD font system ready (%d families discovered)", registry_.getFamilyCount());
}

void SdCardFontSystem::ensureLoaded(GfxRenderer& renderer) {
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
    LOG_DBG("SDFS", "Registry dirty — re-discovering fonts");
    registry_.discover();
  }

  const char* wantedFamily = SETTINGS.sdFontFamilyName;
  const std::string& currentFamily = manager_.currentFamilyName();
  const uint8_t sizeEnum = fontSizeEnumFromSettings();

  if (wantedFamily[0] == '\0') {
    if (!currentFamily.empty()) {
      manager_.unloadAll(renderer);
    }
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
      SETTINGS.sdFontFamilyName[0] = '\0';
      SETTINGS.saveToFile();
      return;
    }
    const auto* selected = family->findClosestReaderSize(sizeEnum);
    const uint8_t wantedPt = selected ? selected->pointSize : 0;
    if (!registryWasDirty && wantedPt == manager_.currentPointSize()) return;
    LOG_DBG("SDFS", "Reloading %s: size %u -> %u (enum %u)%s", wantedFamily, manager_.currentPointSize(), wantedPt,
            sizeEnum, registryWasDirty ? " [registry dirty]" : "");
  }

  if (!currentFamily.empty()) {
    manager_.unloadAll(renderer);
  }

  const auto* family = registry_.findFamily(wantedFamily);
  if (family) {
    if (manager_.loadFamily(*family, renderer, sizeEnum)) {
      LOG_DBG("SDFS", "Loaded SD font family: %s", wantedFamily);
    } else {
      LOG_ERR("SDFS", "Failed to load SD font family: %s (clearing)", wantedFamily);
      SETTINGS.sdFontFamilyName[0] = '\0';
      SETTINGS.saveToFile();
    }
  } else {
    LOG_DBG("SDFS", "SD font family not found: %s (clearing)", wantedFamily);
    SETTINGS.sdFontFamilyName[0] = '\0';
    SETTINGS.saveToFile();
  }
}

int SdCardFontSystem::resolveFontId(const char* familyName, uint8_t /*fontSizeEnum*/) const {
  // The manager loads exactly one size (closest to SETTINGS.fontSize), so the
  // enum is implicit — always return the single loaded font ID for this family.
  // ensureLoaded() must have been called with the current settings before this.
  return manager_.getFontId(familyName);
}

void SdCardFontSystem::unloadUiFallback(GfxRenderer& renderer) {
  if (!uiFallbackFont_) return;
  if (renderer.getFallbackFontId() == uiFallbackFontId_) {
    renderer.setFallbackFontId(0);
  }
  renderer.removeFont(uiFallbackFontId_);
  uiFallbackFont_.reset();
  uiFallbackFontId_ = 0;
  uiFallbackFamilyName_.clear();
  uiFallbackPointSize_ = 0;
}

void SdCardFontSystem::syncUiFallback(GfxRenderer& renderer) {
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
      renderer.isSdCardFont(uiFallbackFontId_) && renderer.getFallbackFontId() == uiFallbackFontId_) {
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
  renderer.setFallbackFontId(fontId);

  uiFallbackFont_ = std::move(font);
  uiFallbackFontId_ = fontId;
  uiFallbackFamilyName_ = family->name;
  uiFallbackPointSize_ = selected->pointSize;
  LOG_DBG("SDFS", "UI fallback loaded: %s %upt id=%d heap %lu -> %lu", uiFallbackFamilyName_.c_str(),
          uiFallbackPointSize_, uiFallbackFontId_, static_cast<unsigned long>(heapBefore),
          static_cast<unsigned long>(ESP.getFreeHeap()));
}
