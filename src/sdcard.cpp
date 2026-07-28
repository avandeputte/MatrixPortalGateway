#include "gateway.h"
#include "sdcard.h"
#include "SD_MMC.h"

// sdcard.cpp -- see sdcard.h. SD_MMC 1-bit mode on the Waveshare-documented pins.

#define SD_PIN_CLK  1
#define SD_PIN_CMD  44
#define SD_PIN_D0   17

static bool        gSdReady = false;
static const char* gSdType  = "none";

static const char* cardTypeName(uint8_t t) {
  switch (t) {
    case CARD_MMC:  return "MMC";
    case CARD_SD:   return "SDSC";
    case CARD_SDHC: return "SDHC";
    default:        return "unknown";
  }
}

void sdInit() {
  // setPins must precede begin; both can fail benignly (no card, bad card, wiring).
  if (!SD_MMC.setPins(SD_PIN_CLK, SD_PIN_CMD, SD_PIN_D0)) {
    printf("[SD] setPins failed -- microSD disabled\n");
    return;
  }
  // "/sdcard" mount, 1-bit mode (true), default freq. A missing card returns false here.
  if (!SD_MMC.begin("/sdcard", true)) {
    printf("[SD] no card / mount failed -- microSD disabled\n");
    return;
  }
  const uint8_t ct = SD_MMC.cardType();
  if (ct == CARD_NONE) {
    printf("[SD] card slot empty -- microSD disabled\n");
    SD_MMC.end();
    return;
  }
  gSdType  = cardTypeName(ct);
  gSdReady = true;
  printf("[SD] %s mounted: %llu MB total, %llu MB used\n", gSdType,
         SD_MMC.cardSize() / (1024ULL * 1024ULL),
         SD_MMC.usedBytes() / (1024ULL * 1024ULL));
}

bool sdReady() { return gSdReady; }

bool sdInfo(uint64_t& sizeMB, uint64_t& usedMB, const char*& type) {
  if (!gSdReady) return false;
  sizeMB = SD_MMC.cardSize()  / (1024ULL * 1024ULL);
  usedMB = SD_MMC.usedBytes() / (1024ULL * 1024ULL);
  type   = gSdType;
  return true;
}
