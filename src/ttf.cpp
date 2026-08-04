// ttf.cpp -- scalable AA text rasterizer (see ttf.h).

#include "ttf.h"
#include "panel.h"
#include <string.h>
#include <esp_heap_caps.h>
#include <esp_log.h>

// stb_truetype: route its scratch allocations to PSRAM (a 400 px hero's edge list is large,
// and internal heap is the scarce one on this board), and drop its asserts.
#define STBTT_malloc(sz, u)  ((void)(u), heap_caps_malloc((sz), MALLOC_CAP_SPIRAM))
#define STBTT_free(p, u)     ((void)(u), free(p))
#define STBTT_assert(x)      ((void)0)
#define STB_TRUETYPE_IMPLEMENTATION
#include "third_party/stb_truetype.h"

// The bundled CP1252 subset of DejaVuSans-Bold (tools/genttf.py). Mono is bundled the same
// way once the companion ships DejaVuSansMono-Bold (byte-identical, for the metrics contract);
// until then TTF_MONO falls back to sans so tabular apps still render, just not monospaced.
extern const unsigned char ttfSansBold[];
extern const unsigned int  ttfSansBold_len;
#ifdef TTF_HAVE_MONO
extern const unsigned char ttfMonoBold[];
extern const unsigned int  ttfMonoBold_len;
#endif

// CP1252 byte -> Unicode is charset.h's cp1252ToUnicode() (the same mapping the rest of the
// firmware uses), so the glyph a byte selects here matches everything else.

// ---- faces ------------------------------------------------------------------------------
struct Face { stbtt_fontinfo info; bool ready; int ascent, descent, lineGap; };
static Face gFace[TTF_FACES];
static uint8_t* gCustomTtf = nullptr;   // resident copy of the uploaded face (PSRAM)

static bool faceInit(Face& f, const unsigned char* data) {
  f.ready = false;
  if (!data) return false;
  const int off = stbtt_GetFontOffsetForIndex(data, 0);
  if (off < 0 || !stbtt_InitFont(&f.info, data, off)) return false;
  stbtt_GetFontVMetrics(&f.info, &f.ascent, &f.descent, &f.lineGap);
  f.ready = true;
  return true;
}

// ---- glyph cache ------------------------------------------------------------------------
// Open-addressed hash table with linear probing; LRU eviction bounded by a PSRAM budget.
// Key packs (face:2 | size:12 | codepoint:16) into 30 bits.
#define TTF_CACHE_SLOTS  512    // power of 2; a HUB75 panel shows at most a few dozen small glyphs
#define TTF_CACHE_BUDGET (512u * 1024)   // small panel, small sizes -- 512 KB is plenty

struct Glyph {
  uint32_t key;        // 0 = empty
  uint16_t w, h;
  int16_t  xoff, yoff; // bitmap origin relative to (pen, baseline)
  int16_t  advance;    // rounded pen advance in px
  uint32_t used;       // monotonic LRU stamp
  uint8_t* cov;        // w*h coverage (PSRAM), null for a zero-area glyph (e.g. space)
};
// The table lives in PSRAM, not internal .bss: at 2048 slots it is ~48 KB, and internal RAM
// is the scarce pool the WiFi-over-SDIO RX path draws from -- parking this table there once
// starved sdio_process_rx under connection churn (rapid app switching) into a TASK_WDT. PSRAM
// has 25 MB free and the table is only touched from the render path, never an ISR.
static Glyph*   gCache = nullptr;
static uint32_t gUseClock = 0, gCacheBytes = 0, gCacheCount = 0;

static inline uint32_t glyphKey(uint8_t face, int size, int cp) {
  return ((uint32_t)(face & 3) << 28) | ((uint32_t)(size & 0xFFF) << 16) | (uint32_t)(cp & 0xFFFF);
}
static inline uint32_t keyHash(uint32_t k) { k *= 2654435761u; return (k >> 15) & (TTF_CACHE_SLOTS - 1); }

static void cacheEvictOne() {
  if (!gCache) return;
  // Evict the least-recently-used occupied slot. Linear scan -- only runs when full/over budget.
  uint32_t best = 0xFFFFFFFF; int bi = -1;
  for (int i = 0; i < TTF_CACHE_SLOTS; i++)
    if (gCache[i].key && gCache[i].used < best) { best = gCache[i].used; bi = i; }
  if (bi < 0) return;
  if (gCache[bi].cov) { free(gCache[bi].cov); gCacheBytes -= (uint32_t)gCache[bi].w * gCache[bi].h; }
  gCache[bi] = Glyph{};
  gCacheCount--;
}

// Find or rasterize the glyph. Returns null only on a hard failure (never for a blank glyph,
// which returns a valid entry with cov==null and a real advance).
static Glyph* glyphGet(uint8_t face, int size, int cp) {
  if (!gFace[face].ready || !gCache) return nullptr;
  const uint32_t key = glyphKey(face, size, cp);
  uint32_t h = keyHash(key);
  int firstFree = -1;
  for (int probe = 0; probe < TTF_CACHE_SLOTS; probe++) {
    const uint32_t i = (h + probe) & (TTF_CACHE_SLOTS - 1);
    if (gCache[i].key == key) { gCache[i].used = ++gUseClock; return &gCache[i]; }
    if (!gCache[i].key) { firstFree = (int)i; break; }
  }

  // Miss: rasterize. Keep at least one free slot; evict LRU when full or over the byte budget.
  stbtt_fontinfo* fi = &gFace[face].info;
  const float scale = stbtt_ScaleForPixelHeight(fi, (float)size);
  const int gi = stbtt_FindGlyphIndex(fi, cp);
  int adv = 0, lsb = 0; stbtt_GetGlyphHMetrics(fi, gi, &adv, &lsb);
  const int16_t advPx = (int16_t)(adv * scale + 0.5f);

  int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  if (gi) stbtt_GetGlyphBitmapBox(fi, gi, scale, scale, &x0, &y0, &x1, &y1);
  const int w = x1 - x0, hh = y1 - y0;

  uint8_t* cov = nullptr;
  if (gi && w > 0 && hh > 0) {
    const uint32_t need = (uint32_t)w * hh;
    while ((gCacheCount + 1 >= TTF_CACHE_SLOTS || gCacheBytes + need > TTF_CACHE_BUDGET) && gCacheCount)
      cacheEvictOne();
    cov = (uint8_t*)heap_caps_malloc(need, MALLOC_CAP_SPIRAM);
    if (cov) {
      stbtt_MakeGlyphBitmap(fi, cov, w, hh, w, scale, scale, gi);
      gCacheBytes += need;
    }
  }

  // Re-find a free slot (eviction above may have opened one; re-probe from the hash).
  if (firstFree < 0 || gCache[firstFree].key) {
    firstFree = -1;
    for (int probe = 0; probe < TTF_CACHE_SLOTS; probe++) {
      const uint32_t i = (h + probe) & (TTF_CACHE_SLOTS - 1);
      if (!gCache[i].key) { firstFree = (int)i; break; }
    }
    if (firstFree < 0) { if (cov) free(cov); return nullptr; }   // table wedged: draw nothing
  }

  Glyph& g = gCache[firstFree];
  g.key = key; g.w = (uint16_t)(cov ? w : 0); g.h = (uint16_t)(cov ? hh : 0);
  g.xoff = (int16_t)x0; g.yoff = (int16_t)y0; g.advance = advPx;
  g.cov = cov; g.used = ++gUseClock;
  gCacheCount++;
  return &g;
}

// ---- public API -------------------------------------------------------------------------
bool ttfBegin() {
  if (!gCache) {   // the glyph-cache table in PSRAM, not internal .bss (see gCache above)
    gCache = (Glyph*)heap_caps_calloc(TTF_CACHE_SLOTS, sizeof(Glyph), MALLOC_CAP_SPIRAM);
    if (!gCache) { printf("[TTF] glyph cache alloc failed -- gtext disabled\n"); return false; }
  }
  const bool okSans = faceInit(gFace[TTF_SANS], ttfSansBold);
#ifdef TTF_HAVE_MONO
  if (!faceInit(gFace[TTF_MONO], ttfMonoBold)) gFace[TTF_MONO] = gFace[TTF_SANS];
#else
  gFace[TTF_MONO] = gFace[TTF_SANS];   // no mono bundled yet -> sans (companion-coordinated)
#endif
  gFace[TTF_CUSTOM].ready = false;
  if (okSans) printf("[TTF] faces ready: sans(%u bytes)%s\n", ttfSansBold_len,
#ifdef TTF_HAVE_MONO
                     " mono");
#else
                     " mono=sans");
#endif
  else printf("[TTF] sans face failed to parse -- gtext disabled\n");
  return okSans;
}

bool ttfFaceReady(uint8_t face) { return face < TTF_FACES && gFace[face].ready; }

static void cacheClearFace(uint8_t face) {
  if (!gCache) return;
  for (int i = 0; i < TTF_CACHE_SLOTS; i++)
    if (gCache[i].key && (uint8_t)((gCache[i].key >> 28) & 3) == face) {
      if (gCache[i].cov) { free(gCache[i].cov); gCacheBytes -= (uint32_t)gCache[i].w * gCache[i].h; }
      gCache[i] = Glyph{}; gCacheCount--;
    }
}

bool ttfSetCustom(const uint8_t* ttf, size_t len) {
  ttfClearCustom();
  gCustomTtf = (uint8_t*)heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
  if (!gCustomTtf) return false;
  memcpy(gCustomTtf, ttf, len);
  if (!faceInit(gFace[TTF_CUSTOM], gCustomTtf)) { ttfClearCustom(); return false; }
  return true;
}
void ttfClearCustom() {
  cacheClearFace(TTF_CUSTOM);
  gFace[TTF_CUSTOM].ready = false;
  if (gCustomTtf) { free(gCustomTtf); gCustomTtf = nullptr; }
}

int ttfTextWidth(uint8_t face, int size, const char* s, int tracking) {
  if (face >= TTF_FACES || !gFace[face].ready || !s) return 0;
  if (size < TTF_MIN_SIZE) size = TTF_MIN_SIZE; else if (size > TTF_MAX_SIZE) size = TTF_MAX_SIZE;
  int w = 0;
  for (const uint8_t* p = (const uint8_t*)s; *p; p++) {
    const Glyph* g = glyphGet(face, size, cp1252ToUnicode(*p));
    if (g) w += g->advance + tracking;
  }
  if (w && tracking) w -= tracking;   // no trailing gap
  return w;
}

// Blit one run of glyphs in a single colour at (startX, baselineY). aa=false hard-thresholds.
static void ttfBlitRun(uint8_t face, int size, int startX, int baselineY, const char* s,
                       int tracking, uint8_t r, uint8_t g, uint8_t b, bool aa) {
  static uint8_t rowBuf[TTF_MAX_SIZE + 8];   // thresholded row scratch (single-threaded ops path)
  int penX = startX;
  for (const uint8_t* p = (const uint8_t*)s; *p; p++) {
    Glyph* gl = glyphGet(face, size, cp1252ToUnicode(*p));
    if (!gl) continue;
    if (gl->cov && gl->w > 0) {
      const int gx = penX + gl->xoff, gy = baselineY + gl->yoff;
      const int w = gl->w > (int)sizeof(rowBuf) ? (int)sizeof(rowBuf) : gl->w;
      for (int row = 0; row < gl->h; row++) {
        const uint8_t* src = gl->cov + (size_t)row * gl->w;
        if (aa) {
          panelBlitCoverRow(gx, gy + row, w, src, r, g, b);
        } else {
          for (int i = 0; i < w; i++) rowBuf[i] = src[i] >= 0x80 ? 255 : 0;
          panelBlitCoverRow(gx, gy + row, w, rowBuf, r, g, b);
        }
      }
    }
    penX += gl->advance + tracking;
  }
}

void ttfDrawText(int x, int y, int size, uint8_t face, const char* s, int align,
                 uint8_t r, uint8_t g, uint8_t b, bool aa, int tracking,
                 bool hasOutline, uint8_t orr, uint8_t org, uint8_t orb,
                 bool hasShadow, uint8_t shr, uint8_t shg, uint8_t shb) {
  if (face >= TTF_FACES) face = TTF_SANS;
  if (!gFace[face].ready || !s || !*s) return;
  if (size < TTF_MIN_SIZE) size = TTF_MIN_SIZE; else if (size > TTF_MAX_SIZE) size = TTF_MAX_SIZE;

  const float scale = stbtt_ScaleForPixelHeight(&gFace[face].info, (float)size);
  const int baselineY = y + (int)(gFace[face].ascent * scale + 0.5f);   // y = top of ascent box

  int startX = x;
  if (align == 1 || align == 2) {
    const int w = ttfTextWidth(face, size, s, tracking);
    startX = (align == 1) ? x - w / 2 : x - w;
  }

  // Outline (8-neighbour ring) or shadow (+1,+1) first, then the fill -- same as the bitmap op.
  if (hasOutline) {
    for (int dy = -1; dy <= 1; dy++)
      for (int dx = -1; dx <= 1; dx++)
        if (dx || dy) ttfBlitRun(face, size, startX + dx, baselineY + dy, s, tracking, orr, org, orb, aa);
  } else if (hasShadow) {
    ttfBlitRun(face, size, startX + 1, baselineY + 1, s, tracking, shr, shg, shb, aa);
  }
  ttfBlitRun(face, size, startX, baselineY, s, tracking, r, g, b, aa);
}

void ttfCacheStats(uint32_t* entries, uint32_t* bytes) {
  if (entries) *entries = gCacheCount;
  if (bytes) *bytes = gCacheBytes;
}
