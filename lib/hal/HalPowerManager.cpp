#include "HalPowerManager.h"

#include <BoardConfig.h>
#include <Logging.h>
#include <PowerManager.h>
#include <WiFi.h>
#include <driver/gpio.h>
#include <esp_sleep.h>
#include <soc/soc_caps.h>

#include <cassert>

#include "HalGPIO.h"

#if FREEINK_DEVICE_PAPERMONO
#include <M5Pm1.h>
#endif
#if FREEINK_DEVICE_LILYGO_T5_47
#include <BoardT5_47.h>
#endif

HalPowerManager powerManager;  // Singleton instance

// GPIO13 controls the X4 battery latch and the X3 SD power rail on the C3
// Xteink boards. Other boards use it for unrelated signals, including the
// X4 Pro display chip select.
static constexpr gpio_num_t XTEINK_C3_GPIO13 = GPIO_NUM_13;

void HalPowerManager::begin() {
  if (BoardConfig::ACTIVE.batteryAdc >= 0) {
    pinMode(BoardConfig::ACTIVE.batteryAdc, INPUT);
  }
  normalFreq = getCpuFrequencyMhz();
  modeMutex = xSemaphoreCreateMutex();
  assert(modeMutex != nullptr);
}

void HalPowerManager::setPowerSaving(bool enabled) {
  if (normalFreq <= 0) {
    return;  // invalid state
  }

  auto wifiMode = WiFi.getMode();
  if (wifiMode != WIFI_MODE_NULL) {
    // Wifi is active, force disabling power saving
    enabled = false;
  }

  // Note: We don't use mutex here to avoid too much overhead,
  // it's not very important if we read a slightly stale value for currentLockMode
  const LockMode mode = currentLockMode;

  if (mode == None && enabled && !isLowPower) {
    LOG_DBG("PWR", "Going to low-power mode");
    if (!setCpuFrequencyMhz(LOW_POWER_FREQ)) {
      LOG_DBG("PWR", "Failed to set CPU frequency = %d MHz", LOW_POWER_FREQ);
      return;
    }
    isLowPower = true;

  } else if ((!enabled || mode != None) && isLowPower) {
    LOG_DBG("PWR", "Restoring normal CPU frequency");
    if (!setCpuFrequencyMhz(normalFreq)) {
      LOG_DBG("PWR", "Failed to set CPU frequency = %d MHz", normalFreq);
      return;
    }
    isLowPower = false;
  }

  // Otherwise, no change needed
}

void HalPowerManager::startDeepSleep(HalGPIO& gpio) const {
#if FREEINK_DEVICE_LILYGO_T5_47
  // Belt and braces with lightSleepIfIdle()'s own cleanup: only the power-button
  // ext1 source (armed below) may wake deep sleep.
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);
#endif
#ifdef ENABLE_SERIAL_LOG
  // Tear down HWCDC so the host sees a clean disconnect and the peripheral
  // doesn't hold power domains that interfere with USB-powered GPIO wake.
  // logSerial is the raw HWCDC reference; Serial is the MySerialImpl proxy
  // (which doesn't expose end()).
  logSerial.end();
#endif

#if !SOC_PM_SUPPORT_EXT1_WAKEUP
  if (gpio.isXteinkDevice()) {
    // GPIO13 gates the battery MOSFET on both Xteink C3 boards; driving it low
    // is the battery power-off (the SDK wake source still handles USB power).
    // Release any surviving pad hold first: hold_en survives deep sleep via
    // the SDK's deepSleep() (esp_sleep_config_gpio_isolate +
    // gpio_deep_sleep_hold_en), and a held pad silently ignores the drive.
    gpio_hold_dis(XTEINK_C3_GPIO13);
    gpio_set_direction(XTEINK_C3_GPIO13, GPIO_MODE_OUTPUT);
    gpio_set_level(XTEINK_C3_GPIO13, 0);
    gpio_hold_en(XTEINK_C3_GPIO13);
  }
#endif

  // Cut the gated peripheral rails (touch/SD/EPD on boards like the Sticky) and
  // hold the enables off through deep sleep — otherwise the GT911 and SD card
  // stay powered all through "off" and drain the battery. No-op on boards with
  // no switched rails (X4/X3). Trade-off: no touch-to-wake; wake is the power
  // button. Must run after display.deepSleep() so the panel controller gets its
  // deep-sleep command while its rail is still up (enterDeepSleep() in main.cpp
  // guarantees that ordering).
  freeink::PowerManager::powerDownRailsForSleep();

#if FREEINK_DEVICE_LILYGO_T5_47
  // This board's SD card has no power-enable pin at all (see BoardConfig.h's
  // T5_47 profile comment, "no power gate") -- powerDownRailsForSleep()
  // above is a no-op for it, so the card stays powered through deep sleep
  // regardless. The one thing still controllable is CS: left floating under
  // esp_sleep_config_gpio_isolate(), an SD card's active-low CS could settle
  // anywhere, including appearing selected, which can keep the card's SPI
  // logic from reaching its lowest-current standby state. Hold it explicitly
  // deselected (HIGH) through sleep; cheap, and the only lever available
  // without hardware rework to add a real power switch.
  if (BoardConfig::isLilyGoT5_47() && BoardConfig::ACTIVE.sd.cs >= 0) {
    const auto cs = static_cast<gpio_num_t>(BoardConfig::ACTIVE.sd.cs);
    gpio_hold_dis(cs);
    gpio_set_direction(cs, GPIO_MODE_OUTPUT);
    gpio_set_level(cs, HIGH);
    gpio_hold_en(cs);
  }
#endif

#if FREEINK_DEVICE_PAPERMONO
  // Its power button is behind the M5PM1 PMIC rather than an ESP GPIO, so
  // normal GPIO deep sleep would have no wake source. Ask the PMIC to shut the
  // device down; a button click then restarts it through a cold boot.
  if (freeink::m5pm1::requestShutdown()) {
    delay(1000);  // allow the PMIC firmware time to drop power
  }
#endif

  // Waits for the power button to be physically released (so holding it doesn't
  // immediately wake the device again), then arms the wake source and sleeps.
  freeink::PowerManager::deepSleepUntilPowerButton();
}

uint32_t HalPowerManager::stuckReleaseCount() { return freeink::PowerManager::stuckReleaseCount(); }

bool HalPowerManager::lightSleepIfIdle() {
#if FREEINK_DEVICE_LILYGO_T5_47
  // Scoped to this board only: it has no WiFi-adjacent background tasks, no
  // touch/tilt/gauge peripheral that needs periodic servicing, and no PMIC --
  // every button (back/confirm/left/right/power) is a plain active-low GPIO
  // (see BoardConfig.h's LILYGO_T5_47 profile), which is all light-sleep GPIO
  // wakeup needs. Other boards would each need their own review before this
  // is safe for them, so this deliberately isn't a generic PowerManager path.
  if (!BoardConfig::isLilyGoT5_47()) return false;
  if (WiFi.getMode() != WIFI_MODE_NULL) return false;  // radio needs the CPU awake to service it
  if (Serial) return false;  // a monitor is attached -- don't delay serial command handling

  static bool wakeSourcesArmed = false;
  if (!wakeSourcesArmed) {
    const auto& in = BoardConfig::ACTIVE.input;
    const int8_t pins[] = {in.back, in.confirm, in.left, in.right, in.power};
    for (int8_t pin : pins) {
      if (pin == BoardConfig::PIN_UNASSIGNED) continue;
      // All active-low with an internal pull-up on this board (see
      // InputManager::begin()'s pinMode calls) -- pressed reads LOW.
      gpio_wakeup_enable(static_cast<gpio_num_t>(pin), GPIO_INTR_LOW_LEVEL);
    }
    esp_sleep_enable_gpio_wakeup();
    wakeSourcesArmed = true;
  }

  // Bounded even with no button press, so loop() still services the
  // auto-sleep timeout and periodic heap logging while otherwise idle.
  esp_sleep_enable_gpio_wakeup();
  esp_sleep_enable_timer_wakeup(200000);  // 200ms, in microseconds
  esp_light_sleep_start();
  // Disarm both: wake sources persist until cleared, and a leftover 200ms timer
  // would carry into the next deep sleep and reboot the device every 200ms.
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);
  return true;
#else
  return false;
#endif
}

#if FREEINK_DEVICE_LILYGO_T5_47
namespace {
// 4-bucket battery indicator for T5_47: a voltage-based gauge on this board
// can't support a trustworthy single-percent reading (post-charge relaxation,
// load-dependent voltage sag, a generic discharge-curve model not tuned to
// this exact cell -- see the investigation this was built from), so showing
// a precise number implies accuracy the sensor doesn't have and makes normal
// voltage noise look like active drain. Reusing the existing icon-fill +
// percent-text UI (BaseTheme::fillBatteryIcon/drawBatteryLeft etc.) with a
// coarse, rarely-changing value instead is a smaller, lower-risk change than
// adding new UI code paths, and solves the same problem: a stable
// Warning/Low/Good/High read is actually more honest than a twitchy 73%.
//
// Boundaries (mV) roughly match the existing LIION_NOTCH_MV discharge curve
// (BatteryMonitor.cpp): Warning below the curve's own 0% anchor (3450mV),
// Low ~0-10%, Good ~20-80% (this chemistry's famously flat middle -- most of
// the pack's real usable capacity lives in this one wide band, so losing
// resolution here reflects the cell's own curve, not a shortcut), High
// ~90-100%. Representative percents (10/40/70/100) are chosen for a visibly
// distinct icon-fill level per bucket, not meant to be read as precise.
constexpr uint16_t kBatteryBucketThresholdsMv[3] = {3500, 3700, 4000};
constexpr uint16_t kBatteryBucketHysteresisMv = 20;
constexpr uint16_t kBatteryBucketRepresentativePercent[4] = {10, 40, 70, 100};

uint16_t batteryBucketPercent(uint16_t millivolts, uint8_t& previousBucket) {
  uint8_t bucket = previousBucket <= 3 ? previousBucket : 0;
  // Move up a bucket once clearly past the next boundary, or down once
  // clearly below the current one -- the hysteresis band prevents a reading
  // sitting right on a line from flapping between two buckets.
  while (bucket < 3 && millivolts >= kBatteryBucketThresholdsMv[bucket] + kBatteryBucketHysteresisMv) ++bucket;
  while (bucket > 0 && millivolts < kBatteryBucketThresholdsMv[bucket - 1] - kBatteryBucketHysteresisMv) --bucket;
  previousBucket = bucket;
  return kBatteryBucketRepresentativePercent[bucket];
}
}  // namespace
#endif

uint16_t HalPowerManager::getBatteryPercentage() const {
#if FREEINK_DEVICE_LILYGO_T5_47
  // This board's battery-sense divider rides the EPD boost rail (see
  // Ed047Tc1Battery.h) — a plain, unthrottled ADC poll would cycle panel
  // power on every call. BoardT5_47::readBatteryPercent() throttles and
  // caches internally; skip the generic ADC/EMA path below entirely.
  // Gated on batteryGauge.gaugeAddr == 0 (not just the board check) so this
  // automatically steps aside the moment BoardConfig.h's LC709203F gauge
  // config is live, instead of silently shadowing it like it did the last
  // time both were toggled independently.
  if (BoardConfig::isLilyGoT5_47() && BoardConfig::ACTIVE.batteryGauge.gaugeAddr == 0) {
    return BoardT5_47::readBatteryPercent();
  }
#endif
  static const BatteryMonitor battery;
  if (BoardConfig::ACTIVE.batteryGauge.gaugeAddr != 0) {
    const unsigned long now = millis();
    if (_batteryLastPollMs != 0 && (now - _batteryLastPollMs) < BATTERY_POLL_MS) {
      return _batteryCachedPercent;
    }

    _batteryLastPollMs = now;
#if FREEINK_DEVICE_LILYGO_T5_47
    if (BoardConfig::isLilyGoT5_47()) {
      const uint16_t mv = battery.readMillivolts();
      if (mv == 0) return _batteryCachedPercent;  // failed read, keep showing the last good bucket
      _batteryCachedPercent = batteryBucketPercent(mv, _batteryBucket);
      return _batteryCachedPercent;
    }
#endif
    uint16_t percent = 0;
    if (!battery.readPercentageChecked(percent)) {
      return _batteryCachedPercent;
    }
    _batteryCachedPercent = percent;
    return _batteryCachedPercent;
  }

  // smooth the battery %.
  if (_batteryCachedPercent == 0) {
    _batteryCachedPercent = 10 * battery.readPercentage();
  } else {
    _batteryCachedPercent = (_batteryCachedPercent * 9 + battery.readPercentage() * 10) / 10;
  }
  return _batteryCachedPercent / 10;
}

uint16_t HalPowerManager::readBatteryMillivoltsForced() const {
  static const BatteryMonitor battery;
  return battery.readMillivolts();
}

bool HalPowerManager::isBatteryWarningLevel() const {
#if FREEINK_DEVICE_LILYGO_T5_47
  if (BoardConfig::isLilyGoT5_47() && BoardConfig::ACTIVE.batteryGauge.gaugeAddr != 0) {
    return _batteryBucket == 0;
  }
#endif
  return false;
}

HalPowerManager::Lock::Lock() {
  xSemaphoreTake(powerManager.modeMutex, portMAX_DELAY);
  // Current limitation: only one lock at a time
  if (powerManager.currentLockMode != None) {
    LOG_ERR("PWR", "Lock already held, ignore");
    valid = false;
  } else {
    powerManager.currentLockMode = NormalSpeed;
    valid = true;
  }
  xSemaphoreGive(powerManager.modeMutex);
  if (valid) {
    // Immediately restore normal CPU frequency if currently in low-power mode
    powerManager.setPowerSaving(false);
  }
}

HalPowerManager::Lock::~Lock() {
  xSemaphoreTake(powerManager.modeMutex, portMAX_DELAY);
  if (valid) {
    powerManager.currentLockMode = None;
  }
  xSemaphoreGive(powerManager.modeMutex);
}
