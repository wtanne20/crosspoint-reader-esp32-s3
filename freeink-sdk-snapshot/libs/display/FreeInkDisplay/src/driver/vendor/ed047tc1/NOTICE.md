# Vendored: LilyGo-EPD47 display core

`ed047tc1.c/.h`, `epd_driver.c/.h`, `i2s_data_bus.c/.h`, and `rmt_pulse.c/.h` in
this directory are vendored, near-verbatim, from:

    Xinyuan-LilyGO/LilyGo-EPD47, esp32s3 branch
    https://github.com/Xinyuan-LilyGO/LilyGo-EPD47
    commit at time of vendoring: HEAD of esp32s3, August 2026

**License: GNU GPLv3** (see `LICENSE` in this directory) — NOT the MIT license
that covers the rest of freeink-sdk and the CrossPoint fork it's vendored
into. This is the only available reference implementation for the ED047TC1's
raw-parallel drive scheme (I2S/LCD-peripheral row clocking + RMT gate pulse +
a bit-banged 3-wire shift register for panel power/OE/mode/latch — see
docs/lilygo-t5-47-support.md for why this is a structurally different display
class from the SSD1677/UC8253/ED2208 controller-chip drivers elsewhere in this
SDK, and from the PMIC-based LgfxEpdDriver written for a *different* LilyGo
board).

**Why GPL code lives in an MIT tree:** there is no permissively-licensed
driver for this exact hardware. epdiy (the more general community driver for
this panel family) is LGPLv3 — still copyleft. This vendoring was a deliberate,
informed choice for a personal-use build (flashing one's own device, not
distributing binaries or upstreaming this driver into the MIT freeink-sdk).
**If you plan to distribute a built firmware image, or upstream this board's
support, address GPL compliance for this subtree first** (source offer,
license propagation for the combined work) — do not just copy this directory
into an all-MIT release without doing that.

Trimmed from upstream: the font/text-drawing, primitive-shape (line/circle/
triangle), and zlib font-decompression code in `epd_driver.c` is unused by
FreeInk (which has its own renderer/font stack) but was left in place rather
than hand-edited out of timing-adjacent code, to keep this subtree a faithful,
diffable copy of the upstream file. Only `utilities.h` was rewritten (trimmed
to the ESP32-S3 pin set this board actually needs; see that file's own
comment) since it's a per-board pin header upstream expects each consumer to
supply.
