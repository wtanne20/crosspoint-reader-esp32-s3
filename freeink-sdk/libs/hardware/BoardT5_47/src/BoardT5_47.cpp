#include "BoardT5_47.h"

#include <Arduino.h>
#include <BatteryMonitor.h>
#include <Ed047Tc1Battery.h>
#include <Logging.h>

#include <algorithm>

namespace BoardT5_47 {
namespace {

// Cycling the EPD boost rail just to sample the battery divider (see
// Ed047Tc1Battery.h) is cheap per-call but wasteful and racy if done often;
// a few minutes of staleness on a battery percentage is imperceptible. It
// also invalidates Ed047Tc1Driver's diff-refresh cache (see
// Ed047Tc1ReadBatteryMillivolts()'s comment), forcing whatever screen redraw
// happens right after a poll onto the slow full-clean path. That's the only
// real cost of polling more often -- the poll itself is a few tens of ms of
// extra current, negligible next to active reading time -- so this favors a
// fresher reading over minimizing how often that slow-path redraw fires.
constexpr unsigned long kPollIntervalMs = 5UL * 60UL * 1000UL;  // 5 minutes

unsigned long g_lastPollMs = 0;
uint16_t g_cachedPercent = 0xFFFF;  // sentinel: not yet sampled

// Reported in steps of 5%: this board samples rarely and caches, so finer steps
// don't flicker between page turns.
constexpr uint8_t kPercentStep = 5;
constexpr uint16_t kCriticalPercent = 5;
uint8_t g_consecutiveLowSamples = 0;

// A real single-cell Li-ion/LiPo sits at 2.5-3.0V even fully depleted past its
// protection cutoff; a reading meaningfully below that isn't a dead battery,
// it's a bad sample (e.g. the shared boost rail this divider rides not fully
// settled yet -- more likely running on battery alone than on USB, since USB
// gives that rail extra headroom). Discard it and keep showing the last good
// reading rather than flashing a false "0%".
constexpr uint16_t kMinPlausibleMv = 2500;

}  // namespace

uint8_t readBatteryPercent() {
  const unsigned long now = millis();
  if (g_cachedPercent != 0xFFFF && (now - g_lastPollMs) < kPollIntervalMs) {
    return static_cast<uint8_t>(g_cachedPercent);
  }

  uint16_t mv = 0;
  if (freeink::ed047Tc1ReadBatteryMillivolts(mv)) {
    // Still throttle the next attempt to the normal interval even when the
    // sample is rejected, so a persistently bad reading can't turn into a
    // tight retry loop hammering the boost rail every status-bar redraw.
    g_lastPollMs = now;
    if (mv >= kMinPlausibleMv) {
      const uint16_t previous = g_cachedPercent == 0xFFFF ? 101 : g_cachedPercent;
      g_cachedPercent = BatteryMonitor::percentageFromMillivoltsStepped(mv, previous, kPercentStep);
      g_consecutiveLowSamples = g_cachedPercent <= kCriticalPercent
                                    ? static_cast<uint8_t>(std::min(g_consecutiveLowSamples + 1, 255))
                                    : 0;
    } else {
      LOG_DBG("BAT", "Discarding implausible reading: %u mV", mv);
    }
  }
  return static_cast<uint8_t>(g_cachedPercent == 0xFFFF ? 0 : g_cachedPercent);
}

bool batteryCritical() { return g_consecutiveLowSamples >= 2; }

}  // namespace BoardT5_47
