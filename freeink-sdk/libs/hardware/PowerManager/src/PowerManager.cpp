#include "PowerManager.h"

#include <Arduino.h>
#include <BoardConfig.h>
#include <Logging.h>
#include <esp_sleep.h>
#include <soc/soc_caps.h>

#if SOC_PM_SUPPORT_EXT1_WAKEUP && SOC_RTCIO_INPUT_OUTPUT_SUPPORTED
#include <driver/rtc_io.h>
#endif

namespace freeink {
namespace {
int8_t powerPin() { return BoardConfig::ACTIVE.input.power; }
bool powerActiveHigh() { return BoardConfig::ACTIVE.input.powerActiveHigh; }

// Diagnostic counter for waitForPowerButtonRelease()'s timeout path. Same
// RTC_NOINIT_ATTR + magic-number pattern as main.cpp's silentReboot* globals:
// survives deep sleep and ESP.restart(), reads as garbage (caught by the
// magic check) after real power loss or a fresh flash.
RTC_NOINIT_ATTR uint32_t g_stuckReleaseMagic;
RTC_NOINIT_ATTR uint32_t g_stuckReleaseCount;
constexpr uint32_t kStuckReleaseMagic = 0x57554B30;  // "WUK0"
}  // namespace

void PowerManager::armWakeOnPins(uint64_t gpioMask, bool wakeLow) {
#if SOC_PM_SUPPORT_EXT1_WAKEUP
  // Xtensa (S3/S2, classic ESP32): RTC ext1. Pins must be RTC GPIOs.
  //
  // The classic ESP32 RTC has no "any low" mode — only ESP_EXT1_WAKEUP_ALL_LOW
  // ("wake when ALL selected pins are low"). For a single wake pin (the common
  // power-button case) ALL_LOW and ANY_LOW are identical; a multi-pin low wake on
  // classic ESP32 fires only when every pin is low. S2/S3 expose ANY_LOW directly.
#if defined(CONFIG_IDF_TARGET_ESP32)
  const esp_sleep_ext1_wakeup_mode_t lowMode = ESP_EXT1_WAKEUP_ALL_LOW;
#else
  const esp_sleep_ext1_wakeup_mode_t lowMode = ESP_EXT1_WAKEUP_ANY_LOW;
#endif
  esp_sleep_enable_ext1_wakeup(gpioMask, wakeLow ? lowMode : ESP_EXT1_WAKEUP_ANY_HIGH);

#if SOC_RTCIO_INPUT_OUTPUT_SUPPORTED
  // ext1 samples the pad through the RTC IO block, which does not inherit the
  // digital pull that pinMode() set while awake. Without its own pull the line
  // floats in deep sleep, can drift to the active level, and re-wakes the chip
  // immediately (observed: a wake every ~1.7s, ext1 cause, no button touched).
  // Tie each wake line to its inactive level with the RTC pull; that needs the
  // RTC_PERIPH domain kept powered (a few uA).
  bool tiedAny = false;
  for (int pin = 0; pin < 64; ++pin) {
    if (!(gpioMask & (1ULL << pin))) continue;
    const auto gpio = static_cast<gpio_num_t>(pin);
    if (!rtc_gpio_is_valid_gpio(gpio)) continue;
    if (wakeLow) {
      rtc_gpio_pulldown_dis(gpio);
      rtc_gpio_pullup_en(gpio);
    } else {
      rtc_gpio_pullup_dis(gpio);
      rtc_gpio_pulldown_en(gpio);
    }
    tiedAny = true;
  }
  if (tiedAny) esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
#endif
#elif SOC_GPIO_SUPPORT_DEEPSLEEP_WAKEUP
  // RISC-V (C3/C6/H2): the deep-sleep "gpio" wakeup source.
  esp_deep_sleep_enable_gpio_wakeup(gpioMask, wakeLow ? ESP_GPIO_WAKEUP_GPIO_LOW : ESP_GPIO_WAKEUP_GPIO_HIGH);
#else
#error "FreeInk PowerManager: target has no supported deep-sleep GPIO wakeup source"
#endif
}

bool PowerManager::armPowerButtonWakeup() {
  const int8_t pin = powerPin();
  if (pin < 0) return false;
  const bool activeHigh = powerActiveHigh();

  // Hold the idle level with the opposite pull so the line is defined in sleep.
  pinMode(pin, activeHigh ? INPUT_PULLDOWN : INPUT_PULLUP);
  armWakeOnPins(1ULL << pin, /*wakeLow=*/!activeHigh);
  return true;
}

void PowerManager::waitForPowerButtonRelease() {
  const int8_t pin = powerPin();
  if (pin < 0) return;
  const bool activeHigh = powerActiveHigh();

  pinMode(pin, activeHigh ? INPUT_PULLDOWN : INPUT_PULLUP);
  const int pressedLevel = activeHigh ? HIGH : LOW;
  // Bounded: an unbounded wait here means a stuck-low/noisy/glitched read on
  // this pin (not necessarily a held button) keeps the device fully awake at
  // full CPU power indefinitely, since the device never reaches deepSleep()
  // below it -- a much larger drain than any sleep-current issue. 10s is far
  // longer than any real press-then-release, so this only ever fires on a
  // genuinely stuck line. Proceeding anyway (rather than refusing to sleep)
  // is deliberate: arming the wake source and sleeping regardless means a
  // transient glitch costs one extra wake-sleep cycle instead of never
  // sleeping again.
  constexpr unsigned long kMaxWaitMs = 10000;
  const unsigned long start = millis();
  while (digitalRead(pin) == pressedLevel) {
    if (millis() - start >= kMaxWaitMs) {
      if (g_stuckReleaseMagic != kStuckReleaseMagic) {
        g_stuckReleaseMagic = kStuckReleaseMagic;
        g_stuckReleaseCount = 0;
      }
      ++g_stuckReleaseCount;
      LOG_ERR("PWR", "Power pin %d still reads pressed after %lu ms, proceeding to sleep anyway (count=%lu)", pin,
              kMaxWaitMs, static_cast<unsigned long>(g_stuckReleaseCount));
      break;
    }
    delay(50);
  }
}

uint32_t PowerManager::stuckReleaseCount() { return g_stuckReleaseMagic == kStuckReleaseMagic ? g_stuckReleaseCount : 0; }

namespace {
// Drive a rail-enable pin to `offLevel` and latch it so the level survives deep
// sleep (requires gpio_deep_sleep_hold_en(), done in deepSleep()). gpio_hold_dis
// first: a hold left over from a previous cycle would make the writes no-ops.
void holdRailOff(int8_t pin, uint8_t offLevel) {
  if (pin < 0) return;
  const auto g = static_cast<gpio_num_t>(pin);
  gpio_hold_dis(g);
  pinMode(pin, OUTPUT);
  digitalWrite(pin, offLevel);
  gpio_hold_en(g);
}
}  // namespace

void PowerManager::powerDownRailsForSleep() {
  const auto& b = BoardConfig::ACTIVE;
  // Keep RESET defined through deep sleep, but never drive an unpowered panel's
  // input HIGH: on boards with a gated EPD rail (Sticky), that can back-power the
  // controller through its RESET protection diode and turn sleep into a
  // milliamp-level drain. Hold RESET LOW alongside a switched-off rail. Boards
  // whose panel rail remains powered (X4 Pro) keep RESET HIGH so a UC8179 cannot
  // drift out of DSLP and restart its analog booster. EpdBus and XteinkDetect
  // release the hold before issuing a reset pulse on wake.
  const uint8_t resetSleepLevel = b.display.powerEnable >= 0 ? LOW : HIGH;
  holdRailOff(b.display.rst, resetSleepLevel);
  holdRailOff(b.display.powerEnable, LOW);
  // SD enable OFF = the inactive level: LOW for active-high enables, HIGH for the
  // active-low ones (e.g. X4 Pro's GPIO5, which powers the card while held LOW).
  holdRailOff(b.sd.powerEnable, b.sd.powerActiveHigh ? LOW : HIGH);
  holdRailOff(b.touch.powerEnable, b.touch.powerEnableActiveHigh ? LOW : HIGH);
  // The mic enable also carries a polarity flag; OFF is the inactive level.
  holdRailOff(b.mic.enable, b.mic.enableActiveHigh ? LOW : HIGH);
}

void PowerManager::deepSleep() {
  esp_sleep_config_gpio_isolate();
  gpio_deep_sleep_hold_en();
  esp_deep_sleep_start();
  while (true) {
  }  // esp_deep_sleep_start() does not return; satisfy [[noreturn]]
}

void PowerManager::deepSleepUntilPowerButton() {
  waitForPowerButtonRelease();
  armPowerButtonWakeup();
  deepSleep();
}

}  // namespace freeink
