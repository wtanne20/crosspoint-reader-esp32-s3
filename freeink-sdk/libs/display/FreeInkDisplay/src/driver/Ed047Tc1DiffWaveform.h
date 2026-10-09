#pragma once

#include <cstdint>

#include "vendor/ed047tc1/epd_driver.h"  // Rect_t (header self-wraps extern "C")

namespace freeink {

// Mirrors epdiy's EpdWaveformPhases shape (not vendored -- a 3-field POD, not
// a creative work worth copyleft attribution; avoids pulling in epdiy's own
// header chain). See vendor/epdiy/NOTICE.md.
struct Ed047Tc1WaveformPhases {
  int phases;
  const uint8_t* luts;       // phases * 16 * 4 bytes, (to,from)-packed
  const int* phaseTimesDus;  // per-phase epd_output_row() duration
};

// The vendored epdiy MODE_DU waveform for this panel.
const Ed047Tc1WaveformPhases& ed047Tc1DuPhases();

// Differential draw over `area`: `toPacked`/`fromPacked` must both be
// full-panel 4bpp-packed buffers (2px/byte, nibble 0x0=black/0xF=white, row
// stride EPD_WIDTH/2 bytes -- the format and stride Ed047Tc1Driver's
// g_packed/g_prevPacked use for a full-screen draw). Unlike epd_draw_image(),
// this does NOT need (and should not be preceded by) a clear -- the waveform
// already encodes a per-pixel (from,to) transition; see vendor/epdiy/NOTICE.md
// and docs/lilygo-t5-47-support.md.
//
// Scope: area.width must be a multiple of 4, and both buffers must use the
// full-panel stride -- this is only wired up for Ed047Tc1Driver::display()'s
// full-screen calls, not displayWindow() (see Ed047Tc1Driver.cpp). Returns
// false (does nothing) on invalid input or allocation failure; the caller
// falls back to the legacy epd_draw_image() path in that case.
bool epdDrawImageDiff(Rect_t area, const uint8_t* toPacked, const uint8_t* fromPacked,
                       const Ed047Tc1WaveformPhases& phases);

}  // namespace freeink
