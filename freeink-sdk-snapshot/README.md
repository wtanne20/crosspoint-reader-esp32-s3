# freeink-sdk snapshot — battery gauge + overnight-drain fixes

The actual build uses these files from the `freeink-sdk` git submodule
(`origin` = `https://github.com/Free-Ink/freeink-sdk.git`, a shared upstream
project repo this fork doesn't have push access to). That submodule checkout
is currently on a detached HEAD, so its local commits aren't reachable from
any branch and can't be pushed there directly.

This directory is a plain-file **copy** of that submodule's current state for
the three files touched by the LC709203F fuel-gauge integration, kept here so
the work is backed up to this repo instead of only existing in an uncommitted
local submodule checkout. It is not wired into the build (nothing references
`freeink-sdk-snapshot/` from `platformio.ini`'s `lib_deps`) — the real source
of truth remains the submodule at `freeink-sdk/`.

**Note on scope:** `BoardConfig.h` is a full-file copy, not a minimal diff —
the LC709203F gauge config is one field inside the much larger `LILYGO_T5_47`
board profile struct (added in an earlier session's T5_47 board bring-up
work), and the two can't be cleanly separated. `BatteryMonitor.h`/`.cpp` are
likewise full current copies, including some earlier `percentageFromMillivoltsStepped`
support that predates today's gauge work but is part of the same battery-
reading effort.

## Files

- `libs/hardware/BatteryMonitor/include/BatteryMonitor.h`
- `libs/hardware/BatteryMonitor/src/BatteryMonitor.cpp` — LC709203F register
  reads (RSOC, cell voltage), CRC8 (SMBus PEC), init sequence (now
  retry-on-failure instead of latching "done" after a failed write), and an
  I2C bus-recovery routine.
- `libs/hardware/BoardConfig/include/BoardConfig.h` — `GaugeType::Lc709203f`,
  the `FREEINK_BATTERY_I2C_GAUGE` gate, and the T5_47 profile's `batteryGauge`
  field (GPIO17 SCL / GPIO18 SDA, address 0x0B).
- `libs/hardware/PowerManager/include/PowerManager.h` / `src/PowerManager.cpp`
  — `waitForPowerButtonRelease()` now bounded (10s timeout instead of an
  unbounded spin that could keep the device awake all night at full CPU
  power), plus a persistent `stuckReleaseCount()` diagnostic that survives
  deep sleep/restart.
- `libs/hardware/SDCardManager/src/SDCardManager.cpp` — releases a CS-pin
  hold on init (paired with the HalPowerManager.cpp change in the main repo
  that holds SD CS high through sleep on boards with no SD power-enable pin).
- `libs/display/FreeInkDisplay/src/driver/Ed047Tc1Driver.cpp` — holds the
  ED047TC1 shift-register's 3 control pins (CFG_DATA/CFG_CLK/CFG_STR) through
  deep sleep so they can't pick up noise and accidentally re-latch the panel
  boost rail on; this file is a full copy (it's untracked/new in the
  submodule from an earlier session's T5_47 display-driver work, not
  something this fix can be cleanly separated from).

## To actually apply this to the submodule

The submodule checkout under `freeink-sdk/` already has these exact changes
(plus the detached-HEAD issue above). To get them onto a real branch there:

```sh
cd freeink-sdk
git checkout -b feature/t5-47-overnight-drain-fixes
git add libs/hardware/BatteryMonitor/include/BatteryMonitor.h \
        libs/hardware/BatteryMonitor/src/BatteryMonitor.cpp \
        libs/hardware/BoardConfig/include/BoardConfig.h \
        libs/hardware/PowerManager/include/PowerManager.h \
        libs/hardware/PowerManager/src/PowerManager.cpp \
        libs/hardware/SDCardManager/src/SDCardManager.cpp \
        libs/display/FreeInkDisplay/src/driver/Ed047Tc1Driver.cpp
git commit -m "fix: overnight battery drain on LilyGo T5 4.7"
# then push to your own fork of Free-Ink/freeink-sdk, not origin
```
