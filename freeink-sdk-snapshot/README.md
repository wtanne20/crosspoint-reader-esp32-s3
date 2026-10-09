# freeink-sdk snapshot — LilyGo T5 4.7 board support (full backup)

The actual build uses these files from the `freeink-sdk` git submodule
(`origin` = `https://github.com/Free-Ink/freeink-sdk.git`, a shared upstream
project repo this fork doesn't have push access to). That submodule checkout
has never had any of this work committed inside it — it's all sitting as
uncommitted local changes (modified tracked files plus brand-new untracked
files) relative to the upstream-pinned commit it's checked out at.

This directory is a plain-file **copy** of that submodule's current state,
covering the entire LilyGo T5 4.7 board-support effort (display driver,
battery gauge, power management, board profile) so none of it only exists in
an uncommitted local submodule checkout that could be lost if that checkout
is ever reset. It is not wired into the build (nothing references
`freeink-sdk-snapshot/` from `platformio.ini`'s `lib_deps`) — the real source
of truth remains the submodule at `freeink-sdk/`.

**Note on scope:** several files here (`BoardConfig.h`, `BatteryMonitor.h/.cpp`,
`Ed047Tc1Driver.cpp`) are full-file copies, not minimal diffs — this board's
support was built up across several sessions and the pieces can't be cleanly
separated from each other.

## Files

- `libs/hardware/BatteryMonitor/include/BatteryMonitor.h` /
  `src/BatteryMonitor.cpp` — LC709203F register reads (RSOC, cell voltage),
  CRC8 (SMBus PEC), retry-on-failure init, I2C bus-recovery routine.
- `libs/hardware/BoardConfig/include/BoardConfig.h` — the `LILYGO_T5_47`
  board profile: geometry, `DisplayController::Ed047Direct`, buttons, ADC
  battery, SD pins, `GaugeType::Lc709203f` / `FREEINK_BATTERY_I2C_GAUGE`.
- `libs/hardware/PowerManager/include/PowerManager.h` / `src/PowerManager.cpp`
  — bounded `waitForPowerButtonRelease()` (10s timeout) plus a persistent
  `stuckReleaseCount()` diagnostic that survives deep sleep/restart.
- `libs/hardware/SDCardManager/src/SDCardManager.cpp` — releases a CS-pin
  hold on init (paired with the HalPowerManager.cpp change in the main repo
  that holds SD CS high through sleep on boards with no SD power-enable pin).
- `libs/hardware/BoardT5_47/` — `BoardT5_47` HAL glue (battery percent
  throttling, board-specific helpers); `library.json` for the PlatformIO lib.
- `libs/ui/FreeInkUI/include/FreeInkUIGfxRenderer.h` — renderer interface
  additions this board's driver needs.
- `libs/display/FreeInkDisplay/src/FreeInkDisplay.cpp` — wiring for the
  Ed047Direct controller path.
- `libs/display/FreeInkDisplay/include/Ed047Tc1Battery.h` — battery-voltage
  read via the shared shift-register-gated boost rail.
- `libs/display/FreeInkDisplay/include/Ed047Tc1RefreshTuning.h` — runtime
  tunables (`setEd047Tc1MenuFullCleanInterval`, `setFullRefreshClearCycles`)
  exposed to Settings.
- `libs/display/FreeInkDisplay/src/driver/Ed047Tc1Driver.{h,cpp}` — the main
  panel driver: I2S/RMT row output, differential MODE_DU fast path, legacy
  15-phase clear-first fallback, consecutive-diff-draw safety net, shift-
  register pin holds through deep sleep.
- `libs/display/FreeInkDisplay/src/driver/Ed047Tc1DiffWaveform.{h,cpp}` —
  the differential-draw implementation consuming the vendored MODE_DU LUT.
  Currently uses a **locally-owned 9-phase repeat** of the vendored 5-phase
  waveform (same proven-safe 1000 dus/phase duration, just repeated more
  times) to reduce "text stacking" ghosting on quick page turns — see the
  comment above `kSettlePhaseCount` in `Ed047Tc1DiffWaveform.cpp` for why
  duration itself (not repeat count) is the fragile parameter here: an
  earlier attempt to lengthen one phase's duration corrupted the display by
  desyncing the RMT-driven CKV pulse from the I2S DMA row cadence.
- `libs/display/FreeInkDisplay/src/driver/vendor/` — vendored third-party
  waveform/driver data: `ed047tc1/` (LilyGo's own driver code, GPLv3) and
  `epdiy/` (the MODE_DU waveform LUT data, LGPLv3). See each directory's own
  `NOTICE.md`/`LICENSE` for provenance; these must stay byte-for-byte copies
  of upstream, any local tuning happens in `Ed047Tc1DiffWaveform.cpp` instead.
- `docs/lilygo-t5-47-support.md` — the board's own design-history doc.

## To actually apply this to the submodule

The submodule checkout under `freeink-sdk/` already has these exact changes
(plus the detached-HEAD issue above). To get them onto a real branch there:

```sh
cd freeink-sdk
git checkout -b feature/t5-47-board-support
git add libs/hardware/BatteryMonitor libs/hardware/BoardConfig \
        libs/hardware/PowerManager libs/hardware/SDCardManager \
        libs/hardware/BoardT5_47 libs/ui/FreeInkUI/include/FreeInkUIGfxRenderer.h \
        libs/display/FreeInkDisplay/src/FreeInkDisplay.cpp \
        libs/display/FreeInkDisplay/include/Ed047Tc1Battery.h \
        libs/display/FreeInkDisplay/include/Ed047Tc1RefreshTuning.h \
        libs/display/FreeInkDisplay/src/driver/Ed047Tc1Driver.cpp \
        libs/display/FreeInkDisplay/src/driver/Ed047Tc1Driver.h \
        libs/display/FreeInkDisplay/src/driver/Ed047Tc1DiffWaveform.cpp \
        libs/display/FreeInkDisplay/src/driver/Ed047Tc1DiffWaveform.h \
        libs/display/FreeInkDisplay/src/driver/vendor \
        docs/lilygo-t5-47-support.md
git commit -m "feat: LilyGo T5 4.7 board support"
# then push to your own fork of Free-Ink/freeink-sdk, not origin
```
