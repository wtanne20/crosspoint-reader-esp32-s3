#pragma once

#include <Arduino.h>
#include <BatteryMonitor.h>
#include <InputManager.h>
#include <Logging.h>
#include <freertos/semphr.h>

#include <cassert>

#include "HalGPIO.h"

class HalPowerManager;
extern HalPowerManager powerManager;  // Singleton

class HalPowerManager {
  int normalFreq = 0;  // MHz
  bool isLowPower = false;

  mutable int _batteryCachedPercent = 0;         // Last read battery percentage (0-100)
  mutable unsigned long _batteryLastPollMs = 0;  // Timestamp of last battery read in milliseconds
#if FREEINK_DEVICE_LILYGO_T5_47
  // Current Warning/Low/Good/High bucket (0-3) for the 4-state indicator --
  // see getBatteryPercentage()'s T5_47 branch. Kept across calls so the
  // hysteresis band has a previous state to compare against.
  mutable uint8_t _batteryBucket = 0;
#endif

  enum LockMode { None, NormalSpeed };
  LockMode currentLockMode = None;
  SemaphoreHandle_t modeMutex = nullptr;  // Protect access to currentLockMode

 public:
#if BOARD_HAS_PSRAM
  static constexpr int LOW_POWER_FREQ = 80;  // MHz
#else
  static constexpr int LOW_POWER_FREQ = 10;  // MHz
#endif
  static constexpr unsigned long IDLE_POWER_SAVING_MS = 3000;  // ms
  static constexpr unsigned long BATTERY_POLL_MS = 1500;       // ms

  void begin();

  // Control CPU frequency for power saving
  void setPowerSaving(bool enabled);

  // Setup wake up GPIO and enter deep sleep
  // Should be called inside main loop() to handle the currentLockMode
  void startDeepSleep(HalGPIO& gpio) const;

  // Pass-through to freeink::PowerManager::stuckReleaseCount() -- see that
  // method's doc comment. Diagnostic for "device stayed awake instead of
  // sleeping"; log at boot, don't gate behavior on it.
  static uint32_t stuckReleaseCount();

  // LilyGo T5 4.7 only (see .cpp): replaces the idle-branch delay(50) with a
  // real ESP32 light sleep, waking instantly on any front/power button or a
  // bounded timer. Returns false (caller should delay() as before) when any
  // safety gate fails -- every other board, WiFi active, or a serial monitor
  // attached. Distinct from, and doesn't touch, the deep-sleep button-hold
  // wake path (verifyPowerButtonWakeup()/armPowerButtonWakeup()) -- this only
  // covers pauses between actions while the device is already awake.
  bool lightSleepIfIdle();

  // Get battery percentage (range 0-100)
  uint16_t getBatteryPercentage() const;

  // T5_47's 4-state indicator (see getBatteryPercentage()'s T5_47 branch in
  // the .cpp for why a precise percentage isn't trustworthy on this board's
  // voltage-based gauge). Meaningful only after a call to
  // getBatteryPercentage() on the same board/config; Warning on every other
  // board (unused there -- they show a real percentage instead).
  enum class BatteryBucket : uint8_t { Warning, Low, Good, Full };
  BatteryBucket getBatteryBucket() const;

  // True once a call to getBatteryPercentage() has landed in the lowest
  // ("Warning") bucket. T5_47 only; every other board keeps using
  // getBatteryPercentage() <= threshold directly for this check, since they
  // still return a real percentage. Call getBatteryPercentage() first on the
  // same cache cycle -- this just reports the last bucket it computed, it
  // doesn't take its own reading.
  bool isBatteryWarningLevel() const;

  // Forced, uncached millivolt read, bypassing getBatteryPercentage()'s
  // cache -- for pinning an exact voltage to a precise moment (e.g. the
  // sleep/wake boundary) rather than whatever's in the cache. On a gauge-
  // equipped board this reads the gauge's own voltage register over I2C and
  // never touches the display; on an ADC board it takes a real ADC sample,
  // which on some boards means briefly powering a rail the divider rides.
  uint16_t readBatteryMillivoltsForced() const;

  // RAII helper class to manage power saving locks
  // Usage: create an instance of Lock in a scope to disable power saving, for example when running a task that needs
  // full performance. When the Lock instance is destroyed (goes out of scope), power saving will be re-enabled.
  class Lock {
    friend class HalPowerManager;
    bool valid = false;

   public:
    explicit Lock();
    ~Lock();

    // Non-copyable and non-movable
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;
    Lock(Lock&&) = delete;
    Lock& operator=(Lock&&) = delete;
  };
};
