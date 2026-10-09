# Vendored: epdiy MODE_DU waveform data for ED047TC1

`ed047tc1_du_waveform.h` in this directory vendors **only** the `MODE_DU`
(type=1, 5-phase) waveform byte tables for the ED047TC1 panel from:

    vroland/epdiy
    https://github.com/vroland/epdiy
    commit at time of vendoring: 42c16127dc5d4bca15e830624f22666ad914575f (2026-08-23)
    source file: src/waveforms/epdiy_ED047TC1.h

**License: GNU LGPLv3** (see `LICENSE` in this directory) — a different
license from both the MIT freeink-sdk tree this lives in and the GPLv3
`vendor/ed047tc1/` sibling directory next to this one. Do not assume either
of those notices covers this directory.

**Why this is vendored:** the `ed047tc1/` sibling directory's `epd_driver.c`
(LilyGo's own reference driver) only implements a fixed 15-frame,
target-value-only waveform — every draw redrives every pixel through the
full contrast range regardless of its prior state, which is why that path
needs a preceding clear pass to avoid under-erasing prior content (see
`docs/lilygo-t5-47-support.md`). epdiy ships a genuine differential (from,to)
transition waveform for this exact panel that converges in 5 phases instead
of 15, with no preceding clear needed — but epdiy's own consumption engine is
a much larger, general-purpose, multi-panel rendering framework not suited to
grafting onto this SDK's simpler pipeline (see the design notes in this
project's history for the full comparison). Only the waveform **data** is
vendored here; the code that consumes it (`Ed047Tc1DiffWaveform.h/.cpp`,
alongside this vendor directory) is original, MIT-licensed CrossPoint code
written against this data's shape, not copied from epdiy's engine.

**Why LGPL code lives in an MIT tree:** same rationale as the GPLv3
`vendor/ed047tc1/` sibling directory — this is a personal-use build (flashing
one's own device), not a distributed binary or an upstream contribution.
**If you plan to distribute a built firmware image, or upstream this board's
support, address LGPL compliance for this subtree first** (source
availability for this vendored file, license propagation for the combined
work) — do not just copy this directory into an all-MIT release without
doing that.

**What was NOT vendored:** epdiy's GC16/GL16 (full grayscale, 30-phase)
waveform tables, its `WHITE_TO_GL16`/`BLACK_TO_GL16` fast-path tables, its
LUT-building/lookup engine (`output_common/lut.c`), its render task
orchestration, board files, or its `EpdiyHighlevelState` framebuffer/dirty-
tracking system — none of that is used here. `RefreshMode::Full` on this
board still uses the unmodified `ed047tc1/` GPLv3 15-frame path.
