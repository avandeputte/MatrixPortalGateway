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
// THE DATA WORD (16-bit bus, one word per PCLK)
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
//   body[0 .. width-1]  shift row r's plane-p pixels.  ADDR = r-1, because the panel is
//                       still DISPLAYING the row latched at the end of the previous
//                       block. OE is low for the first `onClocks` words -- that is the
//                       brightness, and it costs no colour levels.
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
// Verified on a 128x32 chain (two 64x32 panels) at depth 4, 315 Hz. If you change the
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
   the radio. Refresh also scales with bit depth: cfg.panelBitDepth is the other lever. */
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
static uint8_t  bright  = 255;
static uint8_t  brightPending = 0;      // buffers still to receive the new OE duty
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
  LCD_CAM.lcd_user.lcd_2byte_en       = 1;   // 16-bit bus: all 13 signals per word
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

  W = width; H = height; DEPTH = depth;
  ROWS = (uint8_t)(height / 2);                       // row PAIRS
  ADDR_BITS = (height == 64) ? 5 : (height == 32 ? 4 : 3);
  // GDMA moves bytes, and in 16-bit LCD mode a descriptor's length must be a whole number
  // of 32-bit words. Every real panel width is even, but pad rather than trust that.
  BLOCK = (uint32_t)W + TAIL_WORDS;
  if (BLOCK & 1) BLOCK++;
  info.depth = DEPTH;

  const size_t fbBytes   = (size_t)ROWS * DEPTH * BLOCK * sizeof(word_t);
  const size_t descBytes = sizeof(dma_descriptor_t) * (size_t)ROWS * ((1u << DEPTH) - 1);
  const size_t want      = fbBytes * 2 + descBytes * 2;

  // dispInit() runs BEFORE WiFi.begin() so the panel gets first claim on internal
  // SRAM -- which means an over-large geometry silently starves the WiFi/lwIP pool
  // that is allocated from the same heap moments later. The symptom is not a panel
  // fault: it is TCP connects failing and loop()'s heap floor rebooting the board.
  // Refuse the config instead, and leave the caller to run headless with the RAM
  // intact. PANEL_RAM_BUDGET is what we are willing to spend before WiFi exists.
  const size_t freeNow = heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  if (want > PANEL_RAM_BUDGET || want + PANEL_RAM_RESERVE > freeNow) {
    printf("[PANEL] %ux%u depth %u needs %u B of internal DMA RAM "
           "(budget %u, free %u, reserve %u for WiFi) -- refusing\n",
           (unsigned)W, (unsigned)H, (unsigned)DEPTH, (unsigned)want,
           (unsigned)PANEL_RAM_BUDGET, (unsigned)freeNow, (unsigned)PANEL_RAM_RESERVE);
    return false;
  }

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
  // auto_update_desc: the engine follows desc->next by itself. That is the whole point --
  // it is what lets the chain run forever with no interrupt.
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

void panelPixel(int x, int y, uint8_t r, uint8_t g, uint8_t b) {
  if (!info.ok || x < 0 || y < 0 || x >= W || y >= H) return;
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
  for (int j = 0; j < h; j++) panelHLine(x, y + j, w, r, g, b);
}

// Swap buffers by re-pointing the LIVE chain's tail at the other chain's head. GDMA reads
// desc->next when it finishes a descriptor, so the switch happens at a block boundary and
// never mid-row. Then wait one frame, so the caller cannot start drawing into a buffer the
// engine is still reading.
void panelShow() {
  if (!info.ok) return;
  if (brightPending) { writeControlBits(drawBuf); brightPending--; }

  const uint8_t next = drawBuf;
  desc[liveBuf][descN - 1].next = &desc[next][0];
  desc[next][descN - 1].next    = &desc[next][0];   // stay on `next` until the swap after
  liveBuf = next;
  drawBuf = (uint8_t)(next ^ 1);

  // GDMA may still be reading the buffer we just handed back to the CPU, for up to one
  // pass of the chain. Yield rather than spin: this runs on taskDisplay, on the same
  // core as the network stack, and a 3 ms busy-wait 33 times a second is 10% of it.
  if (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING)
    vTaskDelay(pdMS_TO_TICKS(frameUs / 1000 + 1));
  else
    esp_rom_delay_us(frameUs);
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

void panelResume() {
  if (!info.ok) return;
  pinmux(HUB75_OE, (uint8_t)(LCD_DATA_OUT0_IDX + 7));
  gdma_start(dma_chan, (intptr_t)&desc[liveBuf][0]);
  esp_rom_delay_us(1);
  LCD_CAM.lcd_user.lcd_start = 1;
}
