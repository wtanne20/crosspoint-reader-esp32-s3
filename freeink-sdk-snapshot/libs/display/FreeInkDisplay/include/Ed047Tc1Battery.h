#pragma once

// Battery ADC helper for the LilyGo T5 4.7" V2.3 (see Ed047Tc1Driver.h). On
// this board the battery-sense divider rides the SAME shift-register-gated
// boost rail as the EPD panel (confirmed from LilyGo-EPD47's own
// examples/demo.ino, which briefly calls epd_poweron() before every ADC
// sample) — there is no PMIC or always-on sense rail. A plain analogRead()
// on BoardConfig::ACTIVE.batteryAdc floats/reads garbage with the rail off.
//
// This function is deliberately NOT wired into BatteryMonitor's generic ADC
// path (BatteryMonitor.cpp), because that path has no per-board power-up hook
// and, more importantly, calling epd_poweron()/epd_poweroff_all() from an
// arbitrary, unthrottled call site risks racing the display driver's own
// power sequencing mid-refresh (the vendored core's power state is a single
// global, not reentrant). Callers (BoardT5_47's throttled wrapper) must poll
// this rarely and never from a context that might overlap a display refresh.
//
// Declared as a public header (not the private driver/ tree, which also keeps
// the GPLv3-licensed vendor code it wraps out of other libraries' include
// path) so board-support code can call it without depending on the vendored
// core directly. Implemented in src/driver/Ed047Tc1Driver.cpp.

#include <cstdint>

namespace freeink {

// Returns true and fills outMv (millivolts, already scaled by the board's
// divider) on success. Returns false (leaves outMv unchanged) when built
// without FREEINK_DRIVER_ED047_DIRECT.
bool ed047Tc1ReadBatteryMillivolts(uint16_t& outMv);

}  // namespace freeink
