# freeink-sdk snapshot — LC709203F battery gauge work

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
  reads (RSOC, cell voltage), CRC8 (SMBus PEC), init sequence, and an I2C
  bus-recovery routine.
- `libs/hardware/BoardConfig/include/BoardConfig.h` — `GaugeType::Lc709203f`,
  the `FREEINK_BATTERY_I2C_GAUGE` gate, and the T5_47 profile's `batteryGauge`
  field (GPIO17 SCL / GPIO18 SDA, address 0x0B).

## To actually apply this to the submodule

The submodule checkout under `freeink-sdk/` already has these exact changes
(plus the detached-HEAD issue above). To get them onto a real branch there:

```sh
cd freeink-sdk
git checkout -b feature/lc709203f-battery-gauge
git add libs/hardware/BatteryMonitor/include/BatteryMonitor.h \
        libs/hardware/BatteryMonitor/src/BatteryMonitor.cpp \
        libs/hardware/BoardConfig/include/BoardConfig.h
git commit -m "feat: add LC709203F I2C fuel gauge support for LilyGo T5 4.7"
# then push to your own fork of Free-Ink/freeink-sdk, not origin
```
