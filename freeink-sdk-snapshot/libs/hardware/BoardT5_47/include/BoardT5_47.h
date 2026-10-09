#pragma once

#include <cstdint>

namespace BoardT5_47 {

// Throttled battery percentage (0-100) for the LilyGo T5 4.7" V2.3. The
// board's battery-sense divider rides the EPD boost rail (see
// Ed047Tc1Battery.h), so a real sample briefly cycles the panel's power
// group; this caches the result and only re-samples every kPollIntervalMs to
// keep that rare and keep it out of the way of an in-progress display
// refresh. Safe to call often (e.g. every status-bar redraw) — only the
// first call per interval touches hardware.
uint8_t readBatteryPercent();

// True once two consecutive real samples (so ~5-10 minutes apart, never the
// same cached value counted twice) have both read at or below
// kCriticalPercent. Requiring two keeps a single bad ADC sample -- this board
// has misread before -- from locking the user out with a false low-battery.
// Resets on any healthy sample and on reboot.
bool batteryCritical();

}  // namespace BoardT5_47
