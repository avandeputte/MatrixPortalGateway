// panel.cpp -- the HUB75 driver: ESP32-S3 LCD_CAM + GDMA, no ISR, no external library.
//
// WHY THIS EXISTS
// ---------------
// A common HUB75 approach DMAs only the six RGB data lines and drives LAT, OE and the
// address lines as ordinary GPIO from a timer ISR, once per row per bitplane. Any
// variance in when that ISR runs lands on the OE window, and dim pixels shimmer whenever
// WiFi, a flash write or a PSRAM cache miss delays it.
//
// Here, all thirteen signals live in the 16-bit LCD_CAM data word, and GDMA walks a
// CIRCULAR descriptor chain. Once started, the refresh runs entirely in hardware: no
// interrupt, no CPU, nothing for WiFi to disturb. The CPU only writes pixels. It also
// costs no colour levels to dim the wall, because brightness is the OE duty cycle
// rather than a multiply into every colour.
//
// THE DATA WORD (16-bit parallel output, one word per PCLK)
// ---------------------------------------------
//   bit  0..5   R1 G1 B1 R2 G2 B2   pixel data for the row pair
//   bit  6      LAT                 latch the shift register into the output register
//   bit  7      OE                  ACTIVE LOW: 0 = LEDs on, 1 = blanked
//   bit  8..12  A B C D E           which row pair is displayed
//
// THE FRAME
// ---------
// For each row pair r and bitplane p there is one BLOCK of (width + TAIL) words:
//
//   body[0 .. width-1]  shift row r's plane-p pixels.  ADDR = the previously latched
//                       row -- r-1 for each row's plane-0 block, r for the plane>=1
//                       repeats (see writeControlBits) -- because the panel is still
//                       DISPLAYING that row. OE is low for the first `onClocks` words
//                       -- that is the brightness, and it costs no colour levels.
//   tail[0 .. TAIL-1]   OE high (blanked), ADDR = r, and a LAT pulse in the middle so
//                       row r's data moves into the output register while it is dark.
//
// Binary code modulation: plane p must be displayed 2^p times as long as plane 0. We do
// not duplicate the pixel data -- we link the SAME block into the chain 2^p times. Each
// repetition re-shifts and re-latches identical data, so the row simply stays lit for
// another block. Total blocks per frame = rows * (2^depth - 1).
//
// TUNING
// ------
// Verified on a 128x32 chain (two 64x32 panels) at depth 4, ~157 Hz at the current
// 5 MHz LCD_CLK_HZ (the WiFi-coexistence cap below). If you change the
// panel, two constants below are where reality bites, in order:
//   1. TAIL_WORDS -- the blanking window, vs the panel's latch setup/hold.
//   2. LCD_CLK_HZ -- drop it if the far end of a long chain ghosts.
// PANEL_BOOT_TEST in common.h draws a border and a diagonal for four seconds at boot;
// it tells you which of the two is wrong far faster than a wall of glyphs.
//
// The one thing NOT to touch: LAT is strobed on the LAST PIXEL word, not on a tail word.
// LCD_CAM clocks every word it sends, tail words included, and the panel's shift register
// advances on every one of them. A LAT on a tail word is therefore an EXTRA clock past
// the data: it shifts a blank in and pushes column 0 out the far end, so the whole image
// lands one pixel left with a dead column on the right. Latching on the last pixel word
// is the clock that shifts that pixel in, so the register holds exactly columns 0..W-1
// at the latch and nothing moves.

#include "panel.h"
#include <math.h>          // sqrtf, for the ellipse scanlines

#include <Arduino.h>
#include <driver/gpio.h>
#include <esp_private/gdma.h>
#include <esp_private/gpio.h>   // gpio_iomux_output (the driver/gpio.h one is deprecated)
#include <esp_private/periph_ctrl.h>
#include <esp_rom_gpio.h>
#include <esp_rom_sys.h>
#include <hal/dma_types.h>
#include <hal/gpio_hal.h>
#include <soc/lcd_cam_reg.h>
#include <soc/lcd_cam_struct.h>
#include <soc/gpio_sig_map.h>
#include <esp_heap_caps.h>

// ---- tunables ---------------------------------------------------------------------
/* PCLK. Must divide the 160 MHz PLL by an integer (see lcdInit).

   5 MHz, not 10. This is a RADIO constraint, not a display one. The GDMA chain reads
   this framebuffer out of internal SRAM continuously and forever -- at 10 MHz that is
   ~20 MB/s plus ~76k descriptor fetches/sec of AHB traffic, on the same internal SRAM
   the WiFi MAC DMAs through. It starved the radio: association failed with
   4WAY_HANDSHAKE_TIMEOUT / ASSOC_EXPIRE at an RSSI of -46, outbound SYNs went
   unanswered, and MQTT could not connect. Building with -DPANEL_DISABLE=1 (panel never
   started, everything else identical) made WiFi associate instantly and MQTT connect on
   the first try -- which is what identified the panel as the cause.

   The display does not need 10 MHz. 5 MHz still yields ~157 Hz refresh on the default
   128x32 @ depth 4, which is far above the flicker threshold, and it halves both the
   data rate and the descriptor-fetch rate.

   If a long chain ghosts at the far end, lower this further (the far end sees the clock
   last) -- do not raise it. Raising it buys refresh nobody can see and takes it out of
   the radio. Refresh also scales with bit depth: cfg.panelBitDepth is the other lever.

   A/B'd at 10 MHz on the Waveshare board (2026-07-18): unlike the MatrixPortal,
   the radio SURVIVED -- instant association, 0% ping loss, normal HTTP latency.
   The clock is not what blocks depth 4 on a 256x64 panel here; the 144.6 KB
   double-buffered internal framebuffer is (it left 26 KB of heap and a 1.7 KB
   min -- unshippable). If the driver ever grows single-buffering or PSRAM
   bounce buffers, 10 MHz + depth 4 at ~80 Hz is on the table on this board. */
#define LCD_CLK_HZ      5000000u
#define TAIL_WORDS      4           // blanked settle words after the latch: address hold
#define MIN_ON_CLOCKS   1           // never fully dark while brightness > 0

// ---- data-word bit assignments (we choose these; the pinmux below must agree) ------
#define BIT_R1  (1u << 0)
#define BIT_G1  (1u << 1)
#define BIT_B1  (1u << 2)
#define BIT_R2  (1u << 3)
#define BIT_G2  (1u << 4)
#define BIT_B2  (1u << 5)
#define BIT_LAT (1u << 6)
#define BIT_OE  (1u << 7)           // active low
#define ADDR_SHIFT 8
#define RGB_MASK  (BIT_R1|BIT_G1|BIT_B1|BIT_R2|BIT_G2|BIT_B2)

typedef uint16_t word_t;

static PanelInfo info = {false, 0, 0, 0, 0, 0};

static word_t*           fb[2]    = {nullptr, nullptr};   // framebuffers
static dma_descriptor_t* desc[2]  = {nullptr, nullptr};   // one circular chain each
static int               descN    = 0;                    // descriptors per chain
static gdma_channel_handle_t dma_chan = nullptr;

static uint16_t W = 0, H = 0;
static uint8_t  DEPTH = 0, ROWS = 0, ADDR_BITS = 0;
static uint32_t BLOCK = 0;              // words per (row, plane) block
static uint8_t  drawBuf = 1;            // buffer the CPU draws into
static uint8_t  liveBuf = 0;            // buffer GDMA is walking
static volatile uint8_t bright        = 255;   // written from taskWeb (settings) + taskDisplay
static volatile uint8_t brightPending = 0;      // buffers still to receive the new OE duty
static uint32_t frameUs = 0;            // one full pass over the chain

static inline word_t* blockPtr(uint8_t buf, int row, int plane) {
  return fb[buf] + ((uint32_t)row * DEPTH + plane) * BLOCK;
}

// Quantize an 8-bit channel to DEPTH bits, rounding rather than truncating. Brightness
// is NOT applied here -- it is the OE duty cycle, so a dim wall keeps all its levels.
static inline uint8_t quant(uint8_t c) {
  const uint16_t maxv = (1u << DEPTH) - 1;
  return (uint8_t)(((uint32_t)c * maxv + 127) / 255);
}

// ---- control-bit skeleton ----------------------------------------------------------
// Written once at begin() and after every brightness change. Pixel writes only ever
// touch RGB_MASK, so the skeleton survives them.
static void writeControlBits(uint8_t buf) {
  const uint32_t bodyLen  = W;
  const uint32_t addrMask = (uint32_t)((1u << ADDR_BITS) - 1) << ADDR_SHIFT;
  uint32_t onClocks = ((uint32_t)bright * bodyLen) / 255;
  if (bright && onClocks < MIN_ON_CLOCKS) onClocks = MIN_ON_CLOCKS;
  if (onClocks > bodyLen) onClocks = bodyLen;

  for (int r = 0; r < ROWS; r++) {
    uint32_t prev = (uint32_t)((r + ROWS - 1) % ROWS) << ADDR_SHIFT;
    uint32_t self = (uint32_t)r << ADDR_SHIFT;
    for (int p = 0; p < DEPTH; p++) {
      // A block's body displays whatever the PREVIOUS block's tail latched -- so the
      // address lines must name that row, not the row being shifted in.
      //
      // Row r's plane-0 block is the first of the row: the block before it was row r-1's
      // last repeat, so row r-1 is on screen and ADDR = r-1. But every plane >= 1 block
      // follows another block OF ROW r (plane p-1's last repeat, or an earlier repeat of
      // plane p itself), so row r is already latched and ADDR = r.
      //
      // Getting this wrong is not subtle and it is not survivable: with ADDR = r-1
      // everywhere, 28672 of a frame's 30720 lit clocks land on the wrong row and the
      // BCM weights collapse from 1:2:4:8 to 0:0:0:1.
      uint32_t addr = (p == 0) ? prev : self;
      word_t* blk = blockPtr(buf, r, p);
      for (uint32_t x = 0; x < bodyLen; x++) {
        word_t w = (word_t)(blk[x] & RGB_MASK);        // keep pixels
        w |= (word_t)(addr & addrMask);
        if (x >= onClocks) w |= BIT_OE;                // blank the rest of the block
        if (x == bodyLen - 1) {
          // Latch on the clock that shifts the last pixel in: no extra clock, no shift.
          // Force OE high here too -- OE gates the row currently on screen (the one this
          // block is still displaying), so latching while it is lit would tear it. One
          // dark clock in W is invisible.
          w |= BIT_LAT | BIT_OE;
        }
        blk[x] = w;
      }
      // Tail: blanked, address already switched to our row. Pure hold time so the panel
      // settles after the latch before the next block's data starts shifting.
      for (uint32_t t = 0; t < TAIL_WORDS; t++) {
        blk[bodyLen + t] = BIT_OE | (word_t)(self & addrMask);
      }
    }
  }
}

// ---- LCD_CAM + GDMA ----------------------------------------------------------------
static void pinmux(int8_t pin, uint8_t signal) {
  esp_rom_gpio_connect_out_signal(pin, signal, false, false);
  gpio_iomux_output((gpio_num_t)pin, PIN_FUNC_GPIO);
  gpio_set_drive_capability((gpio_num_t)pin, GPIO_DRIVE_CAP_3);
}

static void lcdInit() {
  periph_module_enable(PERIPH_LCD_CAM_MODULE);
  periph_module_reset(PERIPH_LCD_CAM_MODULE);

  LCD_CAM.lcd_user.lcd_reset = 1;
  esp_rom_delay_us(100);

  // 160 MHz PLL, integer prescale to LCD_CLK_HZ.
  uint32_t div = 160000000u / LCD_CLK_HZ;
  if (div < 2) div = 2;
  LCD_CAM.lcd_clock.clk_en             = 1;
  LCD_CAM.lcd_clock.lcd_clk_sel        = 3;        // PLL160M
  LCD_CAM.lcd_clock.lcd_clkm_div_a     = 1;
  LCD_CAM.lcd_clock.lcd_clkm_div_b     = 1;
  LCD_CAM.lcd_clock.lcd_clkm_div_num   = div - 1;
  LCD_CAM.lcd_clock.lcd_ck_out_edge    = 0;        // PCLK low in first half
  LCD_CAM.lcd_clock.lcd_ck_idle_edge   = 0;        // PCLK idles low
  LCD_CAM.lcd_clock.lcd_clk_equ_sysclk = 1;

  LCD_CAM.lcd_ctrl.lcd_rgb_mode_en    = 0;   // i8080 mode, not RGB
  LCD_CAM.lcd_rgb_yuv.lcd_conv_bypass = 0;
  LCD_CAM.lcd_misc.lcd_next_frame_en  = 0;
  LCD_CAM.lcd_data_dout_mode.val      = 0;   // no per-line data delays
  LCD_CAM.lcd_user.lcd_8bits_order    = 0;
  LCD_CAM.lcd_user.lcd_bit_order      = 0;
  LCD_CAM.lcd_user.lcd_2byte_en       = 1;   // 16-bit output: all 13 signals per word
  LCD_CAM.lcd_user.lcd_cmd            = 0;
  LCD_CAM.lcd_user.lcd_dummy          = 0;   // no dummy phases: the chain never restarts
  LCD_CAM.lcd_user.lcd_always_out_en  = 1;   // keep clocking as long as DMA feeds us
  LCD_CAM.lcd_user.lcd_dout           = 1;
  LCD_CAM.lcd_user.lcd_update         = 1;

  // Data lines. The order here IS the bit assignment above.
  const int8_t pins[13] = {
    HUB75_R1, HUB75_G1, HUB75_B1, HUB75_R2, HUB75_G2, HUB75_B2,
    HUB75_LAT, HUB75_OE,
    HUB75_ADDR_A, HUB75_ADDR_B, HUB75_ADDR_C, HUB75_ADDR_D, HUB75_ADDR_E
  };
  for (int i = 0; i < 13; i++) {
    if (i >= 8 + ADDR_BITS) break;           // a 1/16 panel never drives E
    pinmux(pins[i], (uint8_t)(LCD_DATA_OUT0_IDX + i));
  }
  pinmux(HUB75_CLK, LCD_PCLK_IDX);

  LCD_CAM.lc_dma_int_ena.val = 0;            // no interrupts: nothing to service
  LCD_CAM.lc_dma_int_clr.val = 0x03;
}

// Build one circular descriptor chain per buffer. Plane p is linked 2^p times, which is
// the BCM weighting -- the data is not duplicated, only the descriptor pointing at it.
// The order here is load-bearing: writeControlBits() assumes each row's blocks are
// contiguous and that plane 0 comes first. Reorder this and the address lag breaks.
static bool buildChains() {
  const int repeats = (1 << DEPTH) - 1;      // 1 + 2 + 4 + ... + 2^(depth-1)
  descN = ROWS * repeats;
  for (int b = 0; b < 2; b++) {
    desc[b] = (dma_descriptor_t*)heap_caps_aligned_alloc(
        64, sizeof(dma_descriptor_t) * descN, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!desc[b]) return false;
    int d = 0;
    for (int r = 0; r < ROWS; r++) {
      for (int p = 0; p < DEPTH; p++) {
        for (int rep = 0; rep < (1 << p); rep++, d++) {
          dma_descriptor_t& x = desc[b][d];
          x.dw0.owner    = DMA_DESCRIPTOR_BUFFER_OWNER_DMA;
          x.dw0.suc_eof  = 0;                // never end: this is a loop
          x.dw0.size     = BLOCK * sizeof(word_t);
          x.dw0.length   = BLOCK * sizeof(word_t);
          x.buffer       = blockPtr((uint8_t)b, r, p);
          x.next         = &desc[b][d + 1];  // patched below for the last one
        }
      }
    }
    desc[b][descN - 1].next = &desc[b][0];   // close the loop
  }
  return true;
}

// Release everything panelBegin() may have claimed. Every failure path below runs
// through here: bailing out with the framebuffers still held would strand tens of KB
// of internal DMA RAM for the rest of the boot -- and the firmware then runs headless
// (display.cpp), so nothing would ever reclaim it. That is strictly worse than not
// having tried, because WiFi draws from the same pool.
static void panelFreeAll() {
  for (int b = 0; b < 2; b++) {
    if (fb[b])   { heap_caps_free(fb[b]);   fb[b]   = NULL; }
    if (desc[b]) { heap_caps_free(desc[b]); desc[b] = NULL; }
  }
  descN = 0;
}

bool panelBegin(uint16_t width, uint16_t height, uint8_t depth) {
  info = {false, width, height, depth, 0, 0};
  if (depth < 1 || depth > 8) depth = DEFAULT_BIT_DEPTH;

  W = width; H = height;
  ROWS = (uint8_t)(height / 2);                       // row PAIRS
  ADDR_BITS = (height == 64) ? 5 : (height == 32 ? 4 : 3);
  // GDMA moves bytes, and in 16-bit LCD mode a descriptor's length must be a whole number
  // of 32-bit words. Every real panel width is even, but pad rather than trust that.
  BLOCK = (uint32_t)W + TAIL_WORDS;
  if (BLOCK & 1) BLOCK++;

  // Internal DMA RAM the two framebuffers + two descriptor chains cost at a given bit depth. The
  // framebuffer MUST be internal (PSRAM is far too slow to feed the panel), and dispInit() runs
  // BEFORE WiFi.begin() -- so an over-deep panel would starve the WiFi/lwIP pool allocated moments
  // later, and the symptom is not a panel fault but TCP connects failing and loop()'s heap floor
  // rebooting the board. PANEL_RAM_BUDGET caps what we spend before WiFi exists; PANEL_RAM_RESERVE
  // is what must stay free for WiFi afterwards.
  auto wantFor = [&](uint8_t d) -> size_t {
    size_t fb   = (size_t)ROWS * d * BLOCK * sizeof(word_t);
    size_t desc = sizeof(dma_descriptor_t) * (size_t)ROWS * ((1u << d) - 1);
    return fb * 2 + desc * 2;
  };
  // Rather than refuse an over-deep panel outright and run headless -- a silent blank screen, the
  // exact trap a 256x64 @ depth 4 falls into (144 KB > budget) -- step the depth DOWN to the
  // deepest that fits both the budget and the live free-RAM reserve. Fewer bitplanes is fewer
  // brightness levels, but a lit panel beats a dark one. Give up only if even one plane won't fit.
  const size_t freeNow   = heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  const uint8_t reqDepth = depth;
  while (depth > 1 && (wantFor(depth) > PANEL_RAM_BUDGET ||
                       wantFor(depth) + PANEL_RAM_RESERVE > freeNow))
    depth--;
  const size_t want = wantFor(depth);
  if (want > PANEL_RAM_BUDGET || want + PANEL_RAM_RESERVE > freeNow) {
    printf("[PANEL] %ux%u even at depth 1 needs %u B of internal DMA RAM "
           "(budget %u, free %u, reserve %u for WiFi) -- refusing\n",
           (unsigned)W, (unsigned)H, (unsigned)want,
           (unsigned)PANEL_RAM_BUDGET, (unsigned)freeNow, (unsigned)PANEL_RAM_RESERVE);
    return false;
  }
  if (depth != reqDepth)
    printf("[PANEL] %ux%u depth %u wants %u B of internal DMA RAM (budget %u, free %u) -- "
           "clamped to depth %u so the panel still lights\n",
           (unsigned)W, (unsigned)H, (unsigned)reqDepth, (unsigned)wantFor(reqDepth),
           (unsigned)PANEL_RAM_BUDGET, (unsigned)freeNow, (unsigned)depth);
  DEPTH = depth;
  info.depth = DEPTH;
  const size_t fbBytes = (size_t)ROWS * DEPTH * BLOCK * sizeof(word_t);

  for (int b = 0; b < 2; b++) {
    // MALLOC_CAP_DMA is internal by definition -- GDMA cannot read this board's quad
    // PSRAM anywhere near fast enough, and the compiler will not warn you about it.
    fb[b] = (word_t*)heap_caps_aligned_alloc(64, fbBytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!fb[b]) {
      printf("[PANEL] native: %u bytes of DMA RAM unavailable\n", (unsigned)fbBytes);
      panelFreeAll(); return false;
    }
    memset(fb[b], 0, fbBytes);
  }

  bright = 255;
  writeControlBits(0);
  writeControlBits(1);

  if (!buildChains()) {
    printf("[PANEL] native: descriptor alloc failed\n");
    panelFreeAll(); return false;
  }
  info.bytes = fbBytes * 2 + sizeof(dma_descriptor_t) * descN * 2;

  lcdInit();

  gdma_channel_alloc_config_t cc = {};
  cc.direction = GDMA_CHANNEL_DIRECTION_TX;
  if (gdma_new_ahb_channel(&cc, &dma_chan) != ESP_OK) {
    printf("[PANEL] native: no GDMA channel\n");
    panelFreeAll(); return false;
  }
  gdma_connect(dma_chan, GDMA_MAKE_TRIGGER(GDMA_TRIG_PERIPH_LCD, 0));
  // Link-following is inherent to GDMA linked-list mode -- that is what lets the
  // chain run forever with no interrupt. These two flags are something else:
  // owner_check off (the chain never hands ownership back) and auto_update_desc off
  // (the engine must not write into descriptors we are about to re-point on a swap).
  gdma_strategy_config_t sc = {.owner_check = false, .auto_update_desc = false};
  gdma_apply_strategy(dma_chan, &sc);

  liveBuf = 0; drawBuf = 1;
  gdma_start(dma_chan, (intptr_t)&desc[0][0]);
  esp_rom_delay_us(1);
  LCD_CAM.lcd_user.lcd_start = 1;

  // Words per frame -> refresh. No measurement needed: nothing can steal these clocks.
  uint32_t wordsPerFrame = (uint32_t)descN * BLOCK;
  frameUs = (wordsPerFrame * 1000000ull) / LCD_CLK_HZ;
  info.refreshHz = frameUs ? (1000000u / frameUs) : 0;
  info.ok = true;
  return true;
}

const PanelInfo& panelInfo() { return info; }

void panelSetBrightness(uint8_t b) {
  if (b == bright) return;
  bright = b;
  // BOTH buffers carry their own OE duty in their control bits. Writing only the one
  // we are about to draw into would leave the other at the old brightness, and the
  // wall would strobe between the two on alternate frames. Apply it over two shows.
  brightPending = 2;
}

// ---- drawing (back buffer) ----------------------------------------------------------
void panelClear() {
  if (!info.ok) return;
  const uint32_t bodyLen = W;
  for (int r = 0; r < ROWS; r++)
    for (int p = 0; p < DEPTH; p++) {
      word_t* blk = blockPtr(drawBuf, r, p);
      for (uint32_t x = 0; x < bodyLen; x++) blk[x] &= (word_t)~RGB_MASK;
    }
}

// Some HUB75 panels are wired BGR rather than RGB -- the panel's own colour order, not
// something the firmware can detect. On a BGR panel every colour comes out wrong in a very
// specific way: red draws blue, blue draws orange, yellow draws cyan, purple draws pink,
// while green and white look perfectly fine (green is its own channel, white is all three).
// That is exactly why it can go unnoticed for a long time -- text is white.
//
// Swap here, at the one choke point every pixel passes through (panelHLine, panelVLine and
// panelFillRect all funnel into this), rather than by re-mapping the pins: the pin map is
// correct and matches Adafruit's reference, and the next panel may well be RGB.
static bool bgrOrder = false;
void panelSetColourOrder(bool bgr) { bgrOrder = bgr; }

// Lazy tear-guard (v3.1): panelShow used to SLEEP ~one frame after every swap so the CPU
// could not draw into a buffer GDMA was still scanning. That parked the caller even when
// nothing would draw for a while (every HTTP frame push ate it inside the request). Now
// the swap only STAMPS the moment, and the wait happens at the FIRST buffer write after
// it -- usually already absorbed by network transfer or compose time.
static volatile bool     swapPending = false;
static volatile uint32_t swapAtUs    = 0;
static void panelWaitDrawable() {
  if (!swapPending) return;
  const uint32_t el = (uint32_t)(micros() - swapAtUs);
  if (el < frameUs) {
    const uint32_t rem = frameUs - el;
    if (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING)
      vTaskDelay(pdMS_TO_TICKS(rem / 1000 + 1));
    else
      esp_rom_delay_us(rem);
  }
  swapPending = false;
}

void panelPixel(int x, int y, uint8_t r, uint8_t g, uint8_t b) {
  if (!info.ok || x < 0 || y < 0 || x >= W || y >= H) return;
  panelWaitDrawable();
  if (bgrOrder) { uint8_t t = r; r = b; b = t; }
  const bool lower = (y >= ROWS);
  const int  row   = lower ? (y - ROWS) : y;
  const uint8_t qr = quant(r), qg = quant(g), qb = quant(b);
  const word_t rb = lower ? BIT_R2 : BIT_R1;
  const word_t gb = lower ? BIT_G2 : BIT_G1;
  const word_t bb = lower ? BIT_B2 : BIT_B1;
  for (int p = 0; p < DEPTH; p++) {
    word_t* w = blockPtr(drawBuf, row, p) + x;
    word_t v = *w & (word_t)~(rb | gb | bb);
    if ((qr >> p) & 1) v |= rb;
    if ((qg >> p) & 1) v |= gb;
    if ((qb >> p) & 1) v |= bb;
    *w = v;
  }
}

void panelHLine(int x, int y, int w, uint8_t r, uint8_t g, uint8_t b) {
  for (int i = 0; i < w; i++) panelPixel(x + i, y, r, g, b);
}
void panelVLine(int x, int y, int h, uint8_t r, uint8_t g, uint8_t b) {
  for (int i = 0; i < h; i++) panelPixel(x, y + i, r, g, b);
}
void panelFillRect(int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b) {
  // Fast path (v3.0.1): a fill is the hottest canvas op -- ops apps clear the whole
  // panel every frame -- and per-pixel panelPixel calls made a 256x64 clear cost
  // ~25 ms (50K branchy read-modify-writes). Hoist the per-(row,plane) masks and run
  // a tight word loop instead: same quant, same bit assembly, ~5x faster.
  if (!info.ok) return;
  int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
  int x1 = (x + w > W) ? W : x + w, y1 = (y + h > H) ? H : y + h;
  if (x0 >= x1 || y0 >= y1) return;
  panelWaitDrawable();
  if (bgrOrder) { uint8_t t = r; r = b; b = t; }
  const uint8_t qr = quant(r), qg = quant(g), qb = quant(b);
  for (int yy = y0; yy < y1; yy++) {
    const bool lower = (yy >= ROWS);
    const int  row   = lower ? (yy - ROWS) : yy;
    const word_t rb = lower ? BIT_R2 : BIT_R1;
    const word_t gb = lower ? BIT_G2 : BIT_G1;
    const word_t bb = lower ? BIT_B2 : BIT_B1;
    const word_t mask = (word_t)~(rb | gb | bb);
    for (int p = 0; p < DEPTH; p++) {
      word_t set = 0;
      if ((qr >> p) & 1) set |= rb;
      if ((qg >> p) & 1) set |= gb;
      if ((qb >> p) & 1) set |= bb;
      word_t* wp = blockPtr(drawBuf, row, p) + x0;
      for (int i = x1 - x0; i > 0; i--, wp++) *wp = (word_t)((*wp & mask) | set);
    }
  }
}

// Row blitters (v3.1): the frame-shaped paths (full-frame PUT, animation playback, QOI
// decode, transition tweens) draw whole horizontal runs -- per-pixel panelPixel calls
// cost ~20 ms a frame in call overhead, repeated row math and branchy bit assembly.
// Same recipe as the fill fast path: quantize the run once, then one tight
// read-modify-write pass per bitplane. ~4-6x faster than the per-pixel path.
static uint8_t blitQ[3][PANEL_MAX_W];      // per-channel quantized run (panel width max)

static void panelBlitRun(int x, int y, int n) {   // blitQ[]: n quantized pixels
  const bool lower = (y >= ROWS);
  const int  row   = lower ? (y - ROWS) : y;
  const word_t rb = lower ? BIT_R2 : BIT_R1;
  const word_t gb = lower ? BIT_G2 : BIT_G1;
  const word_t bb = lower ? BIT_B2 : BIT_B1;
  const word_t mask = (word_t)~(rb | gb | bb);
  for (int p = 0; p < DEPTH; p++) {
    word_t* wp = blockPtr(drawBuf, row, p) + x;
    for (int i = 0; i < n; i++) {
      word_t set = 0;
      if ((blitQ[0][i] >> p) & 1) set |= rb;
      if ((blitQ[1][i] >> p) & 1) set |= gb;
      if ((blitQ[2][i] >> p) & 1) set |= bb;
      wp[i] = (word_t)((wp[i] & mask) | set);
    }
  }
}

void panelBlitRow888(int x, int y, int n, const uint8_t* rgb) {
  if (!info.ok || y < 0 || y >= H) return;
  if (x < 0) { rgb -= 3 * x; n += x; x = 0; }
  if (x + n > W) n = W - x;
  if (n <= 0) return;
  panelWaitDrawable();
  const int ri = bgrOrder ? 2 : 0, bi = bgrOrder ? 0 : 2;
  for (int i = 0; i < n; i++) {
    blitQ[0][i] = quant(rgb[i * 3 + ri]);
    blitQ[1][i] = quant(rgb[i * 3 + 1]);
    blitQ[2][i] = quant(rgb[i * 3 + bi]);
  }
  panelBlitRun(x, y, n);
}

void panelBlitRow565(int x, int y, int n, const uint8_t* be565) {
  if (!info.ok || y < 0 || y >= H) return;
  if (x < 0) { be565 -= 2 * x; n += x; x = 0; }
  if (x + n > W) n = W - x;
  if (n <= 0) return;
  panelWaitDrawable();
  for (int i = 0; i < n; i++) {
    const uint16_t v = ((uint16_t)be565[i * 2] << 8) | be565[i * 2 + 1];
    uint8_t r = (uint8_t)(((v >> 11) & 0x1F) << 3);
    uint8_t g = (uint8_t)(((v >> 5)  & 0x3F) << 2);
    uint8_t b = (uint8_t)((v & 0x1F) << 3);
    if (bgrOrder) { uint8_t t = r; r = b; b = t; }
    blitQ[0][i] = quant(r);
    blitQ[1][i] = quant(g);
    blitQ[2][i] = quant(b);
  }
  panelBlitRun(x, y, n);
}

// Bresenham line from (x0,y0) to (x1,y1). panelPixel clamps, so off-panel endpoints are fine.
void panelLine(int x0, int y0, int x1, int y1, uint8_t r, uint8_t g, uint8_t b) {
  if (!info.ok) return;
  int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
  int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;
  for (;;) {
    panelPixel(x0, y0, r, g, b);
    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
}

// Midpoint circle centred (cx,cy), radius rad -- an outline, or a filled disc.
void panelCircle(int cx, int cy, int rad, bool fill, uint8_t r, uint8_t g, uint8_t b) {
  if (!info.ok || rad < 0) return;
  int x = rad, y = 0, err = 1 - rad;
  while (x >= y) {
    if (fill) {
      panelHLine(cx - x, cy + y, 2 * x + 1, r, g, b);
      panelHLine(cx - x, cy - y, 2 * x + 1, r, g, b);
      panelHLine(cx - y, cy + x, 2 * y + 1, r, g, b);
      panelHLine(cx - y, cy - x, 2 * y + 1, r, g, b);
    } else {
      panelPixel(cx + x, cy + y, r, g, b); panelPixel(cx - x, cy + y, r, g, b);
      panelPixel(cx + x, cy - y, r, g, b); panelPixel(cx - x, cy - y, r, g, b);
      panelPixel(cx + y, cy + x, r, g, b); panelPixel(cx - y, cy + x, r, g, b);
      panelPixel(cx + y, cy - x, r, g, b); panelPixel(cx - y, cy - x, r, g, b);
    }
    y++;
    if (err < 0) err += 2 * y + 1;
    else { x--; err += 2 * (y - x) + 1; }
  }
}

static inline void iswap(int& a, int& b) { int t = a; a = b; b = t; }

// Quarter-arc helpers (Adafruit-GFX corner bitmask: 1=TL 2=TR 4=BR 8=BL) -- used by round rects.
static void drawCircleHelper(int cx, int cy, int rad, uint8_t corner, uint8_t r, uint8_t g, uint8_t b) {
  int f = 1 - rad, ddx = 1, ddy = -2 * rad, x = 0, y = rad;
  while (x < y) {
    if (f >= 0) { y--; ddy += 2; f += ddy; }
    x++; ddx += 2; f += ddx;
    if (corner & 0x4) { panelPixel(cx + x, cy + y, r, g, b); panelPixel(cx + y, cy + x, r, g, b); }
    if (corner & 0x2) { panelPixel(cx + x, cy - y, r, g, b); panelPixel(cx + y, cy - x, r, g, b); }
    if (corner & 0x8) { panelPixel(cx - y, cy + x, r, g, b); panelPixel(cx - x, cy + y, r, g, b); }
    if (corner & 0x1) { panelPixel(cx - y, cy - x, r, g, b); panelPixel(cx - x, cy - y, r, g, b); }
  }
}
static void fillCircleHelper(int cx, int cy, int rad, uint8_t corner, int delta, uint8_t r, uint8_t g, uint8_t b) {
  int f = 1 - rad, ddx = 1, ddy = -2 * rad, x = 0, y = rad, px = 0, py = rad;
  delta++;
  while (x < y) {
    if (f >= 0) { y--; ddy += 2; f += ddy; }
    x++; ddx += 2; f += ddx;
    if (x < y + 1) {
      if (corner & 1) panelVLine(cx + x, cy - y, 2 * y + delta, r, g, b);
      if (corner & 2) panelVLine(cx - x, cy - y, 2 * y + delta, r, g, b);
    }
    if (y != py) {
      if (corner & 1) panelVLine(cx + py, cy - px, 2 * px + delta, r, g, b);
      if (corner & 2) panelVLine(cx - py, cy - px, 2 * px + delta, r, g, b);
      py = y;
    }
    px = x;
  }
}

// Triangle (x0,y0)-(x1,y1)-(x2,y2); outline (three lines) or filled (Adafruit scanline algorithm).
void panelTriangle(int x0, int y0, int x1, int y1, int x2, int y2, bool fill, uint8_t r, uint8_t g, uint8_t b) {
  if (!info.ok) return;
  if (!fill) {
    panelLine(x0, y0, x1, y1, r, g, b); panelLine(x1, y1, x2, y2, r, g, b); panelLine(x2, y2, x0, y0, r, g, b);
    return;
  }
  if (y0 > y1) { iswap(y0, y1); iswap(x0, x1); }
  if (y1 > y2) { iswap(y2, y1); iswap(x2, x1); }
  if (y0 > y1) { iswap(y0, y1); iswap(x0, x1); }
  if (y0 == y2) {                                  // degenerate: a flat line
    int a = x0, bb = x0;
    if (x1 < a) a = x1; else if (x1 > bb) bb = x1;
    if (x2 < a) a = x2; else if (x2 > bb) bb = x2;
    panelHLine(a, y0, bb - a + 1, r, g, b); return;
  }
  int dx01 = x1 - x0, dy01 = y1 - y0, dx02 = x2 - x0, dy02 = y2 - y0, dx12 = x2 - x1, dy12 = y2 - y1;
  int sa = 0, sb = 0, y, last = (y1 == y2) ? y1 : y1 - 1;
  for (y = y0; y <= last; y++) {
    int a = x0 + sa / dy01, bb = x0 + sb / dy02;
    sa += dx01; sb += dx02;
    if (a > bb) iswap(a, bb);
    panelHLine(a, y, bb - a + 1, r, g, b);
  }
  sa = dx12 * (y - y1); sb = dx02 * (y - y0);
  for (; y <= y2; y++) {
    int a = x1 + sa / dy12, bb = x0 + sb / dy02;
    sa += dx12; sb += dx02;
    if (a > bb) iswap(a, bb);
    panelHLine(a, y, bb - a + 1, r, g, b);
  }
}

// Rounded rectangle, outline or filled (straight edges + four quarter-circle corners).
void panelRoundRect(int x, int y, int w, int h, int rad, bool fill, uint8_t r, uint8_t g, uint8_t b) {
  if (!info.ok || w <= 0 || h <= 0) return;
  int maxr = ((w < h) ? w : h) / 2;
  if (rad > maxr) rad = maxr;
  if (rad < 0) rad = 0;
  if (fill) {
    panelFillRect(x + rad, y, w - 2 * rad, h, r, g, b);
    fillCircleHelper(x + w - rad - 1, y + rad, rad, 1, h - 2 * rad - 1, r, g, b);
    fillCircleHelper(x + rad,         y + rad, rad, 2, h - 2 * rad - 1, r, g, b);
  } else {
    panelHLine(x + rad, y,         w - 2 * rad, r, g, b);
    panelHLine(x + rad, y + h - 1, w - 2 * rad, r, g, b);
    panelVLine(x,         y + rad, h - 2 * rad, r, g, b);
    panelVLine(x + w - 1, y + rad, h - 2 * rad, r, g, b);
    drawCircleHelper(x + rad,         y + rad,         rad, 1, r, g, b);
    drawCircleHelper(x + w - rad - 1, y + rad,         rad, 2, r, g, b);
    drawCircleHelper(x + w - rad - 1, y + h - rad - 1, rad, 4, r, g, b);
    drawCircleHelper(x + rad,         y + h - rad - 1, rad, 8, r, g, b);
  }
}

// Axis-aligned ellipse centred (cx,cy) with semi-axes (a,b). Scanline fill; outline sampled both
// by-row and by-column so the flat top/bottom and sides have no gaps.
void panelEllipse(int cx, int cy, int a, int b, bool fill, uint8_t r, uint8_t g, uint8_t bc) {
  if (!info.ok || a < 0 || b < 0) return;
  if (a == 0) { panelVLine(cx, cy - b, 2 * b + 1, r, g, bc); return; }
  if (b == 0) { panelHLine(cx - a, cy, 2 * a + 1, r, g, bc); return; }
  if (fill) {
    for (int dy = -b; dy <= b; dy++) {
      float t = 1.0f - (float)(dy * dy) / (float)(b * b);
      if (t < 0) continue;
      int dx = (int)(a * sqrtf(t) + 0.5f);
      panelHLine(cx - dx, cy + dy, 2 * dx + 1, r, g, bc);
    }
  } else {
    for (int dy = -b; dy <= b; dy++) {
      float t = 1.0f - (float)(dy * dy) / (float)(b * b);
      if (t < 0) continue;
      int dx = (int)(a * sqrtf(t) + 0.5f);
      panelPixel(cx + dx, cy + dy, r, g, bc); panelPixel(cx - dx, cy + dy, r, g, bc);
    }
    for (int dx = -a; dx <= a; dx++) {
      float t = 1.0f - (float)(dx * dx) / (float)(a * a);
      if (t < 0) continue;
      int dy = (int)(b * sqrtf(t) + 0.5f);
      panelPixel(cx + dx, cy + dy, r, g, bc); panelPixel(cx + dx, cy - dy, r, g, bc);
    }
  }
}

// Copy the live buffer into the back buffer, so a PARTIAL draw lands on top of what is currently on
// screen rather than on the frame from two shows ago (the two buffers otherwise diverge). Both
// buffers carry the same control-bit skeleton, and pixel writes only touch RGB, so copying whole
// words is safe. Reading the live buffer while GDMA also reads it is fine -- both are readers.
void panelCloneToBack() {
  if (!info.ok) return;
  memcpy(fb[drawBuf], fb[liveBuf], (size_t)ROWS * DEPTH * BLOCK * sizeof(word_t));
}

// Reconstruct the LIVE frame (what is on screen right now, in any mode) into `out` as raw pixels,
// row-major, top-left origin: W*H*3 rgb888, or W*H*2 big-endian rgb565 if rgb565. This reverses the
// bitplane packing, so the colours are quantised to the panel's actual bit depth -- a true
// screenshot, not the intended image. Brightness is the OE duty, not in the framebuffer, so it is
// not reflected (a dim wall reads back at full value). Read-only: it never parks or swaps, so a
// frame swap mid-read can tear a live effect slightly -- fine for a preview. Snapshot fast, then the
// caller streams `out` at leisure. out must hold W*H*(rgb565?2:3) bytes.
// One pixel of `buf` reconstructed from the bitplanes into 8-bit RGB: quantised to the panel depth,
// with the BGR swap undone. The quant() round-trip is exact (a read then a panelPixel write is
// idempotent), which is what lets panelScroll shift a frame repeatedly without the colours drifting.
static inline void readPixelRGB(uint8_t buf, int x, int y, uint8_t& r, uint8_t& g, uint8_t& b) {
  const uint16_t maxv = (uint16_t)((1u << DEPTH) - 1);
  const bool lower = (y >= ROWS);
  const int  row   = lower ? (y - ROWS) : y;
  const word_t rmask = lower ? BIT_R2 : BIT_R1;
  const word_t gmask = lower ? BIT_G2 : BIT_G1;
  const word_t bmask = lower ? BIT_B2 : BIT_B1;
  uint16_t qr = 0, qg = 0, qb = 0;
  for (int p = 0; p < DEPTH; p++) {
    word_t w = blockPtr(buf, row, p)[x];
    if (w & rmask) qr |= (uint16_t)(1u << p);
    if (w & gmask) qg |= (uint16_t)(1u << p);
    if (w & bmask) qb |= (uint16_t)(1u << p);
  }
  r = (uint8_t)((qr * 255 + maxv / 2) / maxv);
  g = (uint8_t)((qg * 255 + maxv / 2) / maxv);
  b = (uint8_t)((qb * 255 + maxv / 2) / maxv);
  if (bgrOrder) { uint8_t t = r; r = b; b = t; }
}

void panelReadback(uint8_t* out, bool rgb565) {
  if (!info.ok || !out) return;
  const uint8_t buf = liveBuf;                     // the displayed buffer (single-byte, atomic read)
  size_t o = 0;
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++) {
      uint8_t r, g, b; readPixelRGB(buf, x, y, r, g, b);
      if (rgb565) {
        uint16_t v = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
        out[o++] = (uint8_t)(v >> 8); out[o++] = (uint8_t)(v & 0xFF);
      } else { out[o++] = r; out[o++] = g; out[o++] = b; }
    }
}

// Shift the live frame into the back buffer by (dx,dy); pixels shifted in from off-frame get the
// (fr,fg,fb) fill. Reads liveBuf, writes drawBuf -- separate buffers, so no temp is needed. Because
// the read/write round-trips exactly, a client can scroll repeatedly (a marquee) with no drift.
void panelScroll(int dx, int dy, uint8_t fr, uint8_t fg, uint8_t fb) {
  if (!info.ok) return;
  const uint8_t src = liveBuf;
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++) {
      int sx = x - dx, sy = y - dy;
      if (sx >= 0 && sx < W && sy >= 0 && sy < H) {
        uint8_t r, g, b; readPixelRGB(src, sx, sy, r, g, b);
        panelPixel(x, y, r, g, b);
      } else panelPixel(x, y, fr, fg, fb);
    }
}

// Swap buffers by re-pointing the LIVE chain's tail at the other chain's head. GDMA reads
// desc->next when it finishes a descriptor, so the switch happens at a block boundary and
// never mid-row. Then wait one frame, so the caller cannot start drawing into a buffer the
// engine is still reading.
static void (*sOverlay)(void) = nullptr;
void panelSetOverlay(void (*fn)(void)) { sOverlay = fn; }

void panelShow() {
  if (sOverlay) sOverlay();   // draw the overlay into the outgoing frame (v2.1)
  if (!info.ok) return;
  if (brightPending) { writeControlBits(drawBuf); brightPending = (uint8_t)(brightPending - 1); }

  const uint8_t next = drawBuf;
  desc[liveBuf][descN - 1].next = &desc[next][0];
  desc[next][descN - 1].next    = &desc[next][0];   // stay on `next` until the swap after
  liveBuf = next;
  drawBuf = (uint8_t)(next ^ 1);

  // GDMA may still be reading the buffer we just handed back to the CPU, for up to one
  // pass of the chain -- but instead of sleeping HERE (which parked every caller for a
  // frame whether or not it would draw again soon), just stamp the swap; the first
  // buffer write afterwards waits out whatever remains (panelWaitDrawable above).
  swapPending = true;
  swapAtUs    = micros();
}

void panelStop() {
  if (!info.ok) return;
  LCD_CAM.lcd_user.lcd_start = 0;
  gdma_stop(dma_chan);
  // OE is still muxed to LCD_DATA_OUT7, so driving it as GPIO would do nothing. Detach
  // the peripheral signal first, then park it high: dark, nothing latched, no clock.
  esp_rom_gpio_connect_out_signal(HUB75_OE, SIG_GPIO_OUT_IDX, false, false);
  gpio_set_direction((gpio_num_t)HUB75_OE, GPIO_MODE_OUTPUT);
  gpio_set_level((gpio_num_t)HUB75_OE, 1);
}

void panelRelease() {
  if (info.ok) panelStop();
  panelFreeAll();
  info.ok = false;
}

void panelResume() {
  if (!info.ok) return;
  pinmux(HUB75_OE, (uint8_t)(LCD_DATA_OUT0_IDX + 7));
  gdma_start(dma_chan, (intptr_t)&desc[liveBuf][0]);
  esp_rom_delay_us(1);
  LCD_CAM.lcd_user.lcd_start = 1;
}
