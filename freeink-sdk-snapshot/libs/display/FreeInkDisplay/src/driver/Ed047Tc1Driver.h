#pragma once

// Direct-drive EPD panel driver for the LilyGo T5 4.7" V2.3 (ED047TC1, 960x540,
// 16-gray, ESP32-S3-WROOM-1). NO on-glass controller and NO PMIC: the MCU clocks
// every row itself over the S3's I2S/LCD peripheral (data bus + CKH row clock),
// pulses the gate driver via RMT (CKV), and sequences panel power/OE/scan-dir/
// latch through a bit-banged 3-wire shift register (CFG_DATA/CFG_CLK/CFG_STR) --
// there is no PMIC or IO-expander on this board (contrast with LgfxEpdDriver,
// written for the *different*, PMIC-equipped LilyGo T5 S3). See
// docs/lilygo-t5-47-support.md.
//
// The row-clocking/gate-pulse/shift-register core is vendored from
// Xinyuan-LilyGO/LilyGo-EPD47 (GPLv3 -- see driver/vendor/ed047tc1/NOTICE.md)
// because it is the only available reference implementation for this drive
// scheme; this file is the thin FreeInk-side adapter around it, not vendored.
//
// Like LgfxEpdDriver this owns the panel end to end (usesExternalBus() == true):
// the vendored core drives GPIO/I2S/RMT registers directly and manages its own
// pins (fixed at compile time for CONFIG_IDF_TARGET_ESP32S3 in the vendored
// ed047tc1.h -- this driver is single-board, unlike LgfxEpdDriver's injectable
// per-board config, because the vendored core has no config-injection seam).
//
// Grayscale: the vendored core's epd_draw_image() always runs a fixed multi-pass
// contrast waveform (15 passes; there's no separate fast/reduced-quality mode to
// select), so B/W and grayscale content both go through the same path here.
// FreeInk's LSB/MSB grayscale-plane streaming (supportsStripGrayscale) is NOT
// implemented -- text-only B/W is dramatically fewer waveform passes per screen
// than 16-gray on this hardware class, which is the right tradeoff for battery
// life and matches how most CrossPoint content (EPUB text) reads anyway.
//
// epd_draw_image() is not a true differential update: it has no awareness of
// a pixel's prior value, only its target, so it needs a preceding clear to
// establish a known starting state (LilyGo's own reference code confirms
// this -- every call site pairs it with a preceding clear, no exceptions).
// display() therefore has two paths:
//   - Full mode, the very first draw after begin()/deepSleep(), or any draw
//     immediately after a windowed displayWindow() call: the legacy
//     epd_draw_image() path, preceded by a clear (epd_clear_area_cycles()) --
//     4 cycles for Full's deep clean, 1 light cycle otherwise.
//   - Routine Half/Fast refreshes with a valid previous frame to diff
//     against: a real (from,to)-aware waveform vendored from epdiy
//     (driver/vendor/epdiy/, see its NOTICE.md) that converges in 5 phases
//     with NO clear at all, via Ed047Tc1DiffWaveform.h.
// displayWindow() always uses the legacy path (see its own comment in the
// .cpp for why). See display()'s comment in the .cpp for the full dispatch
// logic and why each invalidation point (begin(), deepSleep(),
// displayWindow(), the battery-read power cycle) exists.

#include "PanelDriver.h"

namespace freeink {

class Ed047Tc1Driver : public PanelDriver {
 public:
  uint32_t spiHz() const override { return 0; }  // no SPI bus; vendored core owns I2S/RMT/GPIO
  BusyPolarity busyPolarity() const override { return BusyPolarity::ActiveLow; }
  bool usesExternalBus() const override { return true; }
  PanelGeometry geometry() const override;

  void begin(EpdBus& bus) override;
  void deepSleep(EpdBus& bus) override;
  void display(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, RefreshMode mode, bool turnOff) override;
  void displayWindow(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, uint16_t x, uint16_t y, uint16_t w,
                      uint16_t h, bool turnOff) override;
  // supportsStripGrayscale() stays PanelDriver's default (false, see the
  // Grayscale paragraph above), but the reader's anti-aliasing pass doesn't
  // actually check that before calling displayGray() -- it's a plain
  // SETTINGS.textAntiAliasing toggle (on by default), so this override gets
  // called on every text page regardless. The base class's default
  // implementation forwards straight to display(), which would run our 1bpp
  // convertRect() over a grayscale-plane buffer -- a format it doesn't
  // understand, producing a corrupted second draw over the already-correct
  // B/W page a moment earlier (confirmed on real hardware: readable page,
  // then dark/unreadable within under a second, every text page, tracked
  // down to exactly this call). A true no-op is correct, not just a
  // workaround: the B/W base this follows is already the intended final
  // image on a board with no grayscale support.
  void displayGray(EpdBus& bus, const uint8_t* fb, bool turnOff, const unsigned char* lut,
                    bool factoryMode) override {
    (void)bus;
    (void)fb;
    (void)turnOff;
    (void)lut;
    (void)factoryMode;
  }
  // No requestResync()/skipInitialResync() override needed: begin() already
  // starts with no valid previous frame to diff against (see the .cpp), so
  // the first post-boot display() call always takes the legacy clear-first
  // path on its own -- there's no separate "next paint must clear" state
  // this driver needs the base class's hook for.
};

PanelDriver& ed047Tc1Driver();

}  // namespace freeink
