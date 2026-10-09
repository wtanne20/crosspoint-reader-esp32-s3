#pragma once

// Runtime-tunable refresh knobs for the LilyGo T5 4.7" raw-panel driver (see
// Ed047Tc1Driver.h). Exposed as a public header (like Ed047Tc1Battery.h) so
// firmware can wire user Settings into the driver without depending on the
// private driver/ tree or the vendored GPLv3 core it wraps -- and so the
// driver itself doesn't need to depend on CrossPointSettings, which would
// invert this SDK's layering (see docs/lilygo-t5-47-support.md).
//
// Both values start at the driver's existing tuned defaults (see
// Ed047Tc1Driver.cpp) and only change if a caller opts in, so firmware that
// never calls these keeps today's exact behavior.

namespace freeink {

// How many consecutive fast/diff menu redraws happen before the driver
// forces a full clean (Ed047Tc1Driver.cpp's g_maxConsecutiveDiffDraws).
// Clamped to [4, 20]: menu screens never request a real Full refresh on
// their own, so this is their only ghost cleanup -- an unbounded value risks
// visible residue accumulating for a very long stretch.
void setEd047Tc1MenuFullCleanInterval(int draws);

// Clear-cycle depth for a Full refresh (Ed047Tc1Driver.cpp's
// g_fullRefreshClearCycles). Clamped to [2, 4]: 1 cycle is deliberately
// excluded -- already proven on this hardware to leave visible ghost
// residue (see Ed047Tc1Driver.cpp's display() comment).
void setEd047Tc1FullRefreshClearCycles(int cycles);

}  // namespace freeink
