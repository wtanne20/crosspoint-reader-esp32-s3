# LilyGo T5 4.7" V2.3 (960x540 ED047TC1, direct drive)

The LilyGo T5-4.7-inch-e-paper-V2.3 is an ESP32-S3-WROOM-1 board (16MB flash,
8MB OPI PSRAM) carrying the same **ED047TC1: raw 960x540 16-gray parallel EPD
with no on-glass controller** as the LilyGo T5 S3 Pro/Lite and M5Stack
PaperS3 — but a completely different, simpler power/bus topology from either.

**This is NOT the same board as `BoardT5S3`/`LILYGO_T5S3`** (a different,
newer LilyGo product: T5S3-4.7-e-paper-PRO/Lite, with a TPS65185 PMIC,
PCA9535 IO-expander, GT911 touch, LoRa, and GPS). Confirmed against LilyGo's
own repo: `Xinyuan-LilyGO/LilyGo-EPD47`, `esp32s3` branch —
`src/ed047tc1.h`/`utilities.h` show a bit-banged 3-wire shift register for
panel power and a single BOOT-adjacent GPIO button, nothing else. There is no
PMIC and no expander on this board.

## Display

The MCU clocks every row itself:

- **Row data (D0-D7) + horizontal clock (CKH)**: the S3's I2S peripheral in
  LCD/parallel mode (`i2s_data_bus.c`), fed by a two-buffer DMA descriptor
  ring.
- **Gate pulse (CKV)**: an RMT channel (`rmt_pulse.c`) pulses the row strobe.
- **Panel power, output-enable, scan direction, mode, and per-row latch**: an
  8-bit value bit-banged out over 3 plain GPIOs (CFG_DATA/CFG_CLK/CFG_STR —
  `ed047tc1.c`'s `push_cfg()`), latched into a discrete shift register on the
  board that gates the boost converter driving the panel's high-voltage
  rails. This is why `LgfxEpdDriver`/LovyanGFX's `Bus_EPD` — written for
  boards where OE/latch/STV are real GPIOs Bus_EPD drives directly — cannot
  drive this board: those signals aren't GPIOs here, they're bits in a
  register that must be re-serialized on every latch pulse.

`Ed047Tc1Driver` (`FREEINK_DRIVER_ED047_DIRECT`, `DisplayController::Ed047Direct`)
vendors LilyGo's own `ed047tc1.c/.h`, `epd_driver.c/.h`, `i2s_data_bus.c/.h`,
and `rmt_pulse.c/.h` near-verbatim (see
`libs/display/FreeInkDisplay/src/driver/vendor/ed047tc1/NOTICE.md` —
**this vendored code is GPLv3**, not MIT like the rest of the SDK) and wraps
it in a thin FreeInk `PanelDriver` adapter. Like `LgfxEpdDriver` it owns the
whole panel (`usesExternalBus() == true`).

**No graduated refresh quality, and no flash-free update.** The vendored
core's `epd_draw_image()` always runs a fixed 15-pass contrast waveform over
the given area — there's no separate "fast" mode to select the way
SSD1677/UC8253 LUTs offer. This panel also has no on-glass controller/RAM,
so that waveform is not a true differential update: it assumes the panel
starts from a known-clean state. LilyGo's own reference code confirms this —
every `epd_draw_image()` call site in their demo is immediately preceded by
a clear, no exceptions, down to a 400×50px live text region that's cleared
and redrawn on every touch event. An earlier version of this driver only
cleared on `Full` mode (skipping it for `Half`/`Fast`, matching how
controller-based drivers here treat "fast" refreshes); on real hardware that
left prior screen content visibly under-erased under new content on every
normal navigation.

`Ed047Tc1Driver`'s `displayWindow()` always clears before drawing, at a light
1-cycle depth (`epd_clear_area()` is hardcoded to a thorough 4-cycle sweep,
32 passes; `epd_clear_area_cycles()` lets each call site pick its own depth)
-- enough for its small-area updates without the full visible flash. `display()`
clears at the full 4-cycle depth instead, whenever it isn't using the
differential path below: `Full` mode, or the rare cases where there's no
trustworthy previous frame to diff against (first draw after boot/deep
sleep/a windowed draw/a battery read -- see below). An earlier version used
the light depth there too, on the theory that any non-`Full` refresh was
routine and cheap; on real hardware that let a faint ghost of a dense
boot-logo-style graphic bleed into the very next screen, because a light
clear isn't a strong enough baseline for the one-time, unusually large
transitions this branch now mostly handles.

**Routine `Half`/`Fast` refreshes skip the clear entirely**, via a real
differential waveform vendored from
[epdiy](https://github.com/vroland/epdiy) (`driver/vendor/epdiy/`, LGPLv3 —
see that directory's own NOTICE.md; a *different* license from the GPLv3
`ed047tc1/` vendor tree next to it). Unlike LilyGo's `epd_draw_image()`,
epdiy ships a genuine `(from,to)`-aware waveform for this exact panel
(`MODE_DU`) that converges in 5 phases instead of 15, and — because it knows
the pixel's prior value, not just its target — needs no preceding clear.
`Ed047Tc1Driver` keeps a full-screen previous-frame buffer (`g_prevPacked`)
to feed it. Only epdiy's waveform *data* is vendored; the code that consumes
it (`Ed047Tc1DiffWaveform.h/.cpp`) is original CrossPoint code written
against that data's shape, modeled on (not copied from) epdiy's own
`output_common/lut.c`, and reuses this project's own already-validated
`ed047tc1.c`/`i2s_data_bus.c`/`rmt_pulse.c` bus/power code unchanged —
epdiy's much larger general-purpose rendering engine was deliberately not
adopted (see the design history for the full comparison).

This only applies to `display()`'s full-screen calls, not `displayWindow()`.
The previous-frame buffer is invalidated (forcing the next `display()` call
back to the legacy clear-first path) whenever something could have changed
the panel's actual state without going through this tracking: `deepSleep()`
(the panel's supply is physically powered down), a `displayWindow()` call
(it never updates the previous-frame buffer, so it would be stale for that
region), and the battery-read power cycle (`ed047Tc1ReadBatteryMillivolts()`
also power-cycles the shared shift-register rail to sample the ADC).

**MODE_DU is fast but not perfect** — a real, known "direct update" EPD
characteristic, not a bug: it can leave faint residue after repeated use,
which is why real e-readers (and CrossPoint's own reader activity, via
`refreshFrequency`) periodically force a full/deep refresh to scrub it. That
existing cadence only covers the reader's own page turns, though — menu
screens (`UiListActivity` and friends: Home, Browse Files, Settings) never
request `Full` at all, so without a separate mechanism they'd accumulate
residue indefinitely. `Ed047Tc1Driver` closes that gap itself: `display()`
caps consecutive diff draws at `kMaxConsecutiveDiffDraws` (8, chosen as a
conservative starting point pending real on-device feedback) and forces the
deep-clean legacy path once that's hit, regardless of which screen is
asking. This is a driver-level safety net independent of any app-level
refresh-frequency setting or cadence tuning done for the controller-based
boards elsewhere in this SDK.

16-gray (`supportsStripGrayscale`) is **not implemented**: B/W-only keeps
refreshes to one waveform pass over the changed area instead of a much
larger multi-plane sequence, which is both simpler and better for battery
life — the right tradeoff for EPUB text, which is most of what CrossPoint
renders.

`displayGray()` is explicitly overridden as a no-op, not left at
`PanelDriver`'s default. The reader's text anti-aliasing pass
(`SETTINGS.textAntiAliasing`, **on by default**) doesn't check
`supportsStripGrayscale()` before running — it always renders an AA pass and
calls `displayGray()` with a grayscale-plane-formatted buffer, straight after
the page's normal B/W draw already displayed correctly. The base class's
default `displayGray()` forwards to `display()`, which runs this driver's
`convertRect()` (built for plain 1bpp input) over that grayscale buffer —
confirmed on real hardware to produce a corrupted second draw (readable page,
then dark and unreadable, well under a second later) on every text page.
The no-op is the correct fix, not a workaround: the preceding B/W draw is
already the intended final image on a board with no grayscale support, so
`displayGray()` has nothing correct to do here. Leaving text anti-aliasing
enabled in Settings still costs the wasted LSB/MSB render passes (CPU/battery)
even with this fix — recommend turning it off for this board.

## Board configuration

`BoardConfig::LILYGO_T5_47` carries geometry, `DisplayController::Ed047Direct`,
the button set, ADC battery, and plain-SPI SD pins. Unlike
`LgfxEpdConfig`-based boards, there is **no injectable per-board pin config**:
the vendored `ed047tc1.h` hardcodes the S3 pin set directly
(`#elif defined(CONFIG_IDF_TARGET_ESP32S3)`), so `Ed047Tc1Driver` needs no
config object at all.

**Mount orientation ships as `{true, true}` (180°)**, not `NO_FLIP`, because
this DIY build's panel is physically mounted rotated relative to the
vendored driver's native scan direction. Applied in `convertRect()`
(`Ed047Tc1Driver.cpp`) since this raw panel has no controller RAM/gate-scan
registers to flip in hardware the way the SSD1677/UC8253-class boards'
`DisplayOrientation` consumers do (`Ssd1677Driver.cpp`, `PaperMonoDriver.cpp`)
— reads from the mirrored source row/column while writing destination bytes
in normal order, so every caller gets a correctly-mirrored result for free.
This is deliberately separate from `SETTINGS.orientation`: that setting only
ever affects reading content (every non-reader screen forces `Portrait`
regardless of it — see every theme's `renderer.setOrientation(Portrait)`
before drawing chrome), so it can't answer "the whole physical mount is
rotated." `BoardConfig::ACTIVE.orientation` operates one layer below that,
so it correctly rotates everything — boot splash, menus, reading, all of it
— uniformly. Only correct for full-screen draws (`display()`, which always
passes the full-screen rect) — `displayWindow()`'s general sub-rect case
would need the rect's *position* mirrored within the full screen too, not
just the pixels inside it; not implemented, since that path is unused in
this build anyway (see its own comment in `Ed047Tc1Driver.cpp`).

If a future build of this board is mounted the other way up, swap `ROTATE_180`
back to `NO_FLIP` in `BoardConfig.h`'s `LILYGO_T5_47` profile (both are
pre-defined `DisplayOrientation` constants there).

## Peripherals

- **Buttons** — the bare board has exactly ONE built-in GPIO button (GPIO21,
  active-low, RTC-capable — confirmed as the ext1 deep-sleep wake source in
  LilyGo's own `examples/button.ino`); there is no touch controller and no
  other GPIO button on the base V2.3 board. This profile assumes 4 more
  buttons added by the user, wired to the exposed GPIO header: GPIO21 is
  `input.power` only, and GPIO45/10/48/39 are `input.back`/`confirm`/`left`/
  `right` respectively. These 4 are per LilyGo's own published GPIO usage
  table for this board, which lists them as the *only* free pins — every
  other unused-looking pin (including 17/18, the touch controller's I2C
  pins, and 47, its IRQ) is marked used/reserved there despite the touch
  chip being unpopulated on the non-touch base V2.3. An earlier version of
  this profile used 9/17/18 instead, on the wrong assumption that an
  unpopulated touch chip means its pins are free; corrected once LilyGo's
  own table was available. GPIO45 is one of the S3's 4 strapping pins
  (VDD_SPI flash-voltage select, sampled only at reset) — fine as a normal
  button input after boot, just don't leave that one held down through a
  cold power-on. No `input.up`/`down`: those are fixed, non-remappable
  roles (used for page-turning) that this profile doesn't wire to anything.
  Between real page-turn buttons and a BLE HID page-turner remote
  (`FREEINK_CAP_BLE_HID_HOST`, already in the SDK), the latter is the
  lower-effort path if page-turning-by-button matters; wiring 2 more buttons
  to `input.up`/`down` is the alternative and needs a BoardConfig change, not
  just new wires (see `MappedInputManager`'s "Physical Fixed" vs. "User
  Remappable" distinction).
- **Battery** — plain ADC on GPIO14, 2:1 divider (confirmed from LilyGo's
  `examples/demo.ino`: `((float)v/4095.0) * 2.0 * 3.3 * vref`). The sense
  divider rides the **same shift-register-gated boost rail as the panel** —
  a reading is only valid with `epd_poweron()` asserted first (see that same
  example: "When reading the battery voltage, POWER_EN must be turned on").
  `Ed047Tc1Battery.h`/`ed047Tc1ReadBatteryMillivolts()` does the
  poweron/sample/poweroff_all sequence; `BoardT5_47::readBatteryPercent()`
  throttles it to once per 5 minutes (`libs/hardware/BoardT5_47`) so routine
  status-bar reads don't cycle the panel's boost converter or risk racing an
  in-progress display refresh (the vendored core's power state is a single
  global, not reentrant). **Not** wired into `BatteryMonitor`'s generic ADC
  path, which has no per-board power-up hook.
- **No charge-status pin, no USB-present pin, no RTC, no frontlight, no
  PMIC** — none were found in LilyGo's own board source; `NO_GAUGE`,
  `NO_FRONTLIGHT`, `NO_SENSORS` accordingly. A "blue LED" mentioned in
  LilyGo's demo is a fixed indicator on the EPD power rail, not an
  addressable/controllable GPIO.
- **SD card** — plain SPI/SdFat (SCLK11 MISO16 MOSI15 CS42), no power gate.

## Power / battery-life notes

This panel has no PMIC standby state: the boost converter is either fully on
(mid-refresh) or fully off (`epd_poweroff_all()`, called by
`Ed047Tc1Driver::deepSleep()` before every MCU deep sleep, matching LilyGo's
own `examples/sleep.ino` sequence) — so there's no intermediate power state to
tune beyond keeping refreshes infrequent and the MCU in deep sleep between
them, which is the same policy every other FreeInk board already follows via
`PowerManager`. The device-specific pieces are: correctly powering the panel
off between refreshes (handled in `Ed047Tc1Driver::display()`'s `turnOff`
path), arming GPIO21 as the deep-sleep wake source (generic
`PowerManager::armPowerButtonWakeup()`, already RTC-capable), and not
polling the battery ADC more than necessary (see above).
