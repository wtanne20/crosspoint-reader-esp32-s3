#include "Ed047Tc1Driver.h"

#include <Arduino.h>
#include <BoardConfig.h>
#include <Ed047Tc1Battery.h>
#include <Ed047Tc1RefreshTuning.h>
#include <Logging.h>

#include <algorithm>
#include <cstring>

#if FREEINK_DRIVER_ED047_DIRECT
#include <esp_heap_caps.h>

#include "Ed047Tc1DiffWaveform.h"
#include "vendor/ed047tc1/ed047tc1.h"
#include "vendor/ed047tc1/epd_driver.h"
#endif

namespace freeink {

#if FREEINK_DRIVER_ED047_DIRECT
namespace {

uint8_t* g_packed = nullptr;   // scratch 4bpp buffer, sized for a full-screen draw
size_t g_packedCap = 0;
bool g_initialized = false;

// Full-screen 4bpp snapshot of the last frame actually drawn, for the
// differential fast path (see display()) -- always full-panel-sized and
// -strided (packedRowBytes(EPD_WIDTH)), never per-call-sized like g_packed
// can be for a displayWindow() call. g_prevValid is false until a real frame
// has been drawn (or after deepSleep(), since we have no code-level guarantee
// the panel's actual analog state after a power cycle still matches this
// buffer -- see Ed047Tc1Driver::deepSleep()).
uint8_t* g_prevPacked = nullptr;
size_t g_prevPackedCap = 0;
bool g_prevValid = false;

// Driver-level safety net, independent of what any activity requests: the
// diff path (MODE_DU) is fast but imperfect -- known real-world EPD "direct
// update" behavior, not a bug -- so faint residue can accumulate over many
// consecutive diff draws. The reader already forces its own periodic Full
// refresh (every 15 pages by default) to scrub this, but menu screens
// (Home, Browse Files, Settings) never request Full at all, so without this
// counter they'd accumulate residue forever with no cleanup. Forcing a deep
// clean here every g_maxConsecutiveDiffDraws draws applies regardless of
// which screen is asking, closing that gap without needing every screen to
// cooperate. Runtime-tunable via setMenuFullCleanInterval() -- see
// Ed047Tc1Driver.h -- default (8) matches this constant's original tuned
// value, changed only if the user opts in via Settings.
int g_maxConsecutiveDiffDraws = 8;
int g_diffDrawCount = 0;

// Clear-cycle depth for a Full refresh (see display()). Runtime-tunable via
// setFullRefreshClearCycles() -- default (4) is the value empirically tuned
// this project to fix under-erase ghosting (see display()'s comment); never
// clamped below 2 here, since 1 cycle already proved insufficient on this
// hardware.
int g_fullRefreshClearCycles = 4;

// Round up a row's packed byte count: 2 px/byte, odd widths pad one nibble
// (the vendored core requires a byte never straddle a row — see epd_driver.h's
// epd_draw_image doc).
inline size_t packedRowBytes(int32_t w) { return static_cast<size_t>(w + 1) / 2; }

// Convert a rect of FreeInk's 1-bpp frame (MSB-first, bit=1 -> white; see
// FreeInkDisplay.cpp's blit comment) into the vendored core's 4-bit packed
// format (nibble 0x0 = black, 0xF = white; low nibble = even column, high
// nibble = odd column -- matches epd_driver.c's epd_copy_to_framebuffer, the
// vendored reference for this packing).
//
// Applies BoardConfig::ACTIVE.orientation.mirrorX/mirrorY here (this raw
// panel has no controller RAM/gate-scan registers to flip in hardware, unlike
// the SSD1677/UC8253-class boards' DisplayOrientation consumers): reads from
// the mirrored source position while writing destination bytes in the same
// left-to-right, top-to-bottom order as before, so every caller gets a
// correctly-mirrored result with no changes on their end. Correct for the
// only real caller (display(), which always passes the full-screen rect,
// x=0 y=0 w=h=full) -- mirroring a sub-rect's *position* within the full
// screen (not just the pixels inside it) would need more than this for
// displayWindow(), but that path is unused in this build (see its own
// comment), so it's not implemented.
void convertRect(const uint8_t* fb1bpp, uint16_t fbWidthBytes, int32_t x, int32_t y, int32_t w, int32_t h,
                  uint8_t* out) {
  const bool mirrorX = BoardConfig::ACTIVE.orientation.mirrorX;
  const bool mirrorY = BoardConfig::ACTIVE.orientation.mirrorY;
  const size_t rowBytes = packedRowBytes(w);
  for (int32_t row = 0; row < h; ++row) {
    const int32_t srcRowIdx = mirrorY ? (h - 1 - row) : row;
    const uint8_t* srcRow = fb1bpp + static_cast<size_t>(y + srcRowIdx) * fbWidthBytes;
    uint8_t* dstRow = out + static_cast<size_t>(row) * rowBytes;
    for (size_t bp = 0; bp < rowBytes; ++bp) {
      const int32_t col0 = static_cast<int32_t>(bp) * 2;
      const int32_t col1 = col0 + 1;
      const int32_t srcCol0 = mirrorX ? (w - 1 - col0) : col0;
      const int32_t sx0 = x + srcCol0;
      const bool white0 = (srcRow[sx0 >> 3] >> (7 - (sx0 & 7))) & 1;
      uint8_t nib0 = white0 ? 0xF : 0x0;
      uint8_t nib1 = 0x0;
      if (col1 < w) {
        const int32_t srcCol1 = mirrorX ? (w - 1 - col1) : col1;
        const int32_t sx1 = x + srcCol1;
        const bool white1 = (srcRow[sx1 >> 3] >> (7 - (sx1 & 7))) & 1;
        nib1 = white1 ? 0xF : 0x0;
      }
      dstRow[bp] = static_cast<uint8_t>(nib0 | (nib1 << 4));
    }
  }
}

bool ensureScratch(size_t bytes) {
  if (g_packed && g_packedCap >= bytes) return true;
  if (g_packed) heap_caps_free(g_packed);
  g_packed = static_cast<uint8_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  g_packedCap = g_packed ? bytes : 0;
  return g_packed != nullptr;
}

bool ensurePrevScratch(size_t bytes) {
  if (g_prevPacked && g_prevPackedCap >= bytes) return true;
  if (g_prevPacked) heap_caps_free(g_prevPacked);
  g_prevPacked = static_cast<uint8_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  g_prevPackedCap = g_prevPacked ? bytes : 0;
  return g_prevPacked != nullptr;
}

// epd_clear_area() is hardcoded to 4 dark/white cycles (32 total passes) --
// thorough, but far more than routine navigation needs (see display()). This
// lets each call site pick its own depth via epd_clear_area_cycles().
constexpr int32_t kClearCycleTimeUs = 50;  // matches epd_clear_area()'s own default

void drawRect(const uint8_t* fb1bpp, uint16_t fbWidthBytes, int32_t x, int32_t y, int32_t w, int32_t h,
              int32_t clearCycles, bool turnOff) {
  if (w <= 0 || h <= 0) return;
  const size_t need = packedRowBytes(w) * static_cast<size_t>(h);
  if (!ensureScratch(need)) return;
  convertRect(fb1bpp, fbWidthBytes, x, y, w, h, g_packed);

  const Rect_t area = {x, y, w, h};
  epd_poweron();
  if (clearCycles > 0) epd_clear_area_cycles(area, clearCycles, kClearCycleTimeUs);
  epd_draw_image(area, g_packed, BLACK_ON_WHITE);
  if (turnOff) {
    epd_poweroff_all();
  } else {
    epd_poweroff();
  }
}

}  // namespace
#endif  // FREEINK_DRIVER_ED047_DIRECT

PanelGeometry Ed047Tc1Driver::geometry() const {
  const uint16_t w = BoardConfig::ACTIVE.displayWidth;
  const uint16_t h = BoardConfig::ACTIVE.displayHeight;
  const uint16_t wb = w / 8;
  return {w, h, wb, static_cast<uint32_t>(wb) * h};
}

void Ed047Tc1Driver::begin(EpdBus& bus) {
  (void)bus;
#if FREEINK_DRIVER_ED047_DIRECT
  if (g_initialized) return;
  // Release deepSleep()'s hold on the shift-register control lines first: a
  // gpio_hold_en() survives the wake reset and silently blocks any further
  // gpio_set_direction()/gpio_set_level() on that pin (including the
  // vendored epd_init() below) until gpio_hold_dis() runs. No-op on a cold
  // boot that never held these pins.
  for (gpio_num_t pin : {CFG_DATA, CFG_CLK, CFG_STR}) {
    gpio_hold_dis(pin);
  }
  epd_init();
  const auto& g = geometry();
  ensureScratch(packedRowBytes(g.width) * g.height);
  ensurePrevScratch(packedRowBytes(g.width) * g.height);
  g_prevValid = false;  // no frame drawn yet -- first display() call uses the legacy path
  g_initialized = true;
#endif
}

void Ed047Tc1Driver::display(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, RefreshMode mode, bool turnOff) {
  (void)bus;
  (void)prev;
#if FREEINK_DRIVER_ED047_DIRECT
  const auto& g = geometry();
  const size_t fullBytes = packedRowBytes(g.width) * static_cast<size_t>(g.height);
  if (!ensureScratch(fullBytes)) return;
  convertRect(fb, g.widthBytes, 0, 0, g.width, g.height, g_packed);

  const Rect_t area = {0, 0, g.width, g.height};
  // Differential fast path (vendor/epdiy/): a real (from,to)-aware waveform
  // that converges in 5 phases with no preceding clear, instead of
  // epd_draw_image()'s fixed 15-frame target-only waveform (which -- see
  // docs/lilygo-t5-47-support.md -- needs a clear first, since it assumes a
  // known-clean starting state and has no awareness of what's already on the
  // panel).
  //
  // Was temporarily disabled while chasing a "correct page, then dark and
  // unreadable within under a second" bug on every text page. Root-caused to
  // something unrelated to this path entirely: displayGray() (see this
  // class's header comment) was falling through to PanelDriver's default,
  // which fed a grayscale-plane buffer from the reader's anti-aliasing pass
  // (on by default) through this driver's 1bpp-only convertRect(), producing
  // exactly that corrupted second draw. Fixed by overriding displayGray() as
  // a no-op. Re-enabled now that the actual cause is fixed, not this path.
  //
  // Only for routine Half/Fast refreshes with a valid previous frame to diff
  // against: Full always uses the legacy deep-clean path below, and the
  // first draw ever (or the first after deep sleep, see deepSleep()) has no
  // previous frame to diff from yet. Also capped at g_maxConsecutiveDiffDraws
  // in a row -- see that variable's comment.
  const bool useDiff =
      mode != RefreshMode::Full && g_prevValid && g_prevPacked && g_diffDrawCount < g_maxConsecutiveDiffDraws;

  epd_poweron();
  const bool diffOk = useDiff && epdDrawImageDiff(area, g_packed, g_prevPacked, ed047Tc1DuPhases());
  if (diffOk) {
    ++g_diffDrawCount;
  } else {
    g_diffDrawCount = 0;
    // Full mode, no valid previous frame yet, the periodic safety-net cap
    // above was hit, or the diff draw itself failed (e.g. OOM building its
    // LUT) -- redraw unconditionally instead. Always the deep 4-cycle clean
    // here, not mode-gated: this branch now runs both for rare invalidation
    // events (first draw ever, first after deep sleep/battery-read/a
    // windowed draw) and for the routine periodic cleanup -- in both cases
    // we specifically don't trust the panel's starting state, so a light
    // clear would undermine establishing a solid baseline for every diff
    // draw that follows. A 1-cycle clear here (an earlier version of this
    // code) was cheap but let a faint ghost of a dense boot-logo-style
    // graphic bleed into the very next draw.
    epd_clear_area_cycles(area, g_fullRefreshClearCycles, kClearCycleTimeUs);
    epd_draw_image(area, g_packed, BLACK_ON_WHITE);
  }
  if (turnOff) {
    epd_poweroff_all();
  } else {
    epd_poweroff();
  }

  if (ensurePrevScratch(fullBytes)) {
    memcpy(g_prevPacked, g_packed, fullBytes);
    g_prevValid = true;
  }
#else
  (void)fb;
  (void)mode;
  (void)turnOff;
#endif
}

void Ed047Tc1Driver::displayWindow(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, uint16_t x, uint16_t y,
                                    uint16_t w, uint16_t h, bool turnOff) {
  (void)bus;
  (void)prev;
#if FREEINK_DRIVER_ED047_DIRECT
  const auto& g = geometry();
  // Same reasoning as display(): needs a clear too, or the same under-erase
  // artifact shows up in whatever region was last drawn there. Always the
  // light 1-cycle depth -- windowed calls are already small-area and cheap,
  // and nothing currently calls this expecting a Full-grade deep clean.
  //
  // Stays on this legacy path unconditionally, unlike display() -- it never
  // touches g_prevPacked, so a windowed draw leaves it stale for whatever
  // sub-rect it just changed. g_prevValid = false below forces the next
  // display() call back to its own legacy path rather than risk diffing
  // against that stale region.
  drawRect(fb, g.widthBytes, x, y, w, h, /*clearCycles=*/1, turnOff);
  g_prevValid = false;
  g_diffDrawCount = 0;
#else
  (void)fb;
  (void)x;
  (void)y;
  (void)w;
  (void)h;
  (void)turnOff;
#endif
}

void Ed047Tc1Driver::deepSleep(EpdBus& bus) {
  (void)bus;
#if FREEINK_DRIVER_ED047_DIRECT
  // Matches the vendored reference's own pre-deep-sleep sequence
  // (examples/sleep.ino): fully de-assert the shift-register power group
  // (panel boost + the board's status LED, which shares the rail) before the
  // SoC itself goes to deep sleep.
  epd_poweroff_all();

  // epd_poweroff_all() already clocked an all-zero word into the shift
  // register, so its *output* (panel power) is latched off regardless of
  // what the 3 control lines do afterward. But PowerManager::deepSleep()'s
  // esp_sleep_config_gpio_isolate() leaves every pin not explicitly held
  // floating through the night -- if CFG_CLK/CFG_STR pick up enough noise to
  // produce a spurious clock+strobe edge, whatever CFG_DATA is floating to
  // at that moment gets shifted in and re-latched, potentially re-enabling
  // the boost rail. Drive all 3 to a defined idle-low level and hold them so
  // the pads stay actively driven (immune to external noise, unlike a float)
  // through deep sleep. Same gpio_hold_en() pattern PowerManager::
  // powerDownRailsForSleep() uses for the other rail-enable pins; needs the
  // matching gpio_deep_sleep_hold_en() PowerManager::deepSleep() already
  // calls to make the holds persist.
  for (gpio_num_t pin : {CFG_DATA, CFG_CLK, CFG_STR}) {
    gpio_hold_dis(pin);  // a hold left over from a previous cycle would make this a no-op
    gpio_set_direction(pin, GPIO_MODE_OUTPUT);
    gpio_set_level(pin, 0);
    gpio_hold_en(pin);
  }

  // Deep sleep physically powers the panel down; nothing here can confirm
  // the analog state on the glass after wake still matches g_prevPacked, and
  // a wrong guess means a visibly corrupted screen. Force the next display()
  // call back to its legacy full-clean path instead of risking a diff
  // against a possibly-stale buffer.
  g_prevValid = false;
  g_diffDrawCount = 0;
#endif
}

PanelDriver& ed047Tc1Driver() {
  static Ed047Tc1Driver instance;
  return instance;
}

void setEd047Tc1MenuFullCleanInterval(int draws) {
#if FREEINK_DRIVER_ED047_DIRECT
  g_maxConsecutiveDiffDraws = std::clamp(draws, 4, 20);
#else
  (void)draws;
#endif
}

void setEd047Tc1FullRefreshClearCycles(int cycles) {
#if FREEINK_DRIVER_ED047_DIRECT
  g_fullRefreshClearCycles = std::clamp(cycles, 2, 4);
#else
  (void)cycles;
#endif
}

bool ed047Tc1ReadBatteryMillivolts(uint16_t& outMv) {
#if FREEINK_DRIVER_ED047_DIRECT
  const int8_t pin = BoardConfig::ACTIVE.batteryAdc;
  if (pin < 0) return false;
  epd_poweron();
  // The vendor reference (examples/demo.ino) uses 10ms; bumped to 30ms since a
  // bad low-ball reading was observed running on battery alone (no USB rail
  // to help this settle) -- give the boost rail more margin before sampling.
  delay(30);
  const uint16_t mv = analogReadMilliVolts(pin);
  epd_poweroff_all();
  // Same reasoning as Ed047Tc1Driver::deepSleep(): this power-cycles the
  // shared shift-register rail outside any display()/deepSleep() call, so
  // treat it the same way -- force the next display() call back to its
  // legacy path rather than risk a diff against a possibly-stale buffer.
  g_prevValid = false;
  g_diffDrawCount = 0;
  outMv = static_cast<uint16_t>(mv * BoardConfig::ACTIVE.batteryDividerMultiplier);
  LOG_DBG("BAT", "pin %d raw=%u mV divider=%.2f -> %u mV", pin, mv, BoardConfig::ACTIVE.batteryDividerMultiplier,
          outMv);
  return true;
#else
  (void)outMv;
  return false;
#endif
}

}  // namespace freeink
