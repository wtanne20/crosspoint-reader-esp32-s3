#pragma once

// Trimmed board-pin header for the vendored LilyGo-EPD47 driver core.
//
// Upstream's utilities.h carries pins for BOTH the classic ESP32 (WROVER) T5
// 4.7" board and its ESP32-S3 successor (T5 4.7" V2.3), selected by
// CONFIG_IDF_TARGET_*. FreeInk only targets the S3 board, so only that branch
// is kept. Values verified against Xinyuan-LilyGO/LilyGo-EPD47 (esp32s3
// branch) src/utilities.h and examples/button, examples/demo.
//
// None of these are referenced by epd_driver.c/ed047tc1.c/i2s_data_bus.c/
// rmt_pulse.c themselves (grepped upstream: zero hits) — this header exists
// only to satisfy epd_driver.h's #include "utilities.h" and to document the
// pins the board-support layer (BoardT5_47) reads.

#if !defined(CONFIG_IDF_TARGET_ESP32S3)
#error "vendor/ed047tc1 is only wired for CONFIG_IDF_TARGET_ESP32S3 (LilyGo T5 4.7\" V2.3)"
#endif

#define BUTTON_1 (21)  // single user/BOOT-adjacent button, RTC-capable (ext1 wake)

#define BATT_PIN (14)  // battery ADC, 2:1 divider. Requires EPD POWER_EN (epd_poweron())
                       // asserted first -- the sense divider rides the same boost rail
                       // as the panel (see LilyGo-EPD47 examples/demo.ino).

#define SD_MISO (16)
#define SD_MOSI (15)
#define SD_SCLK (11)
#define SD_CS (42)

#define BOARD_SCL (17)  // unpopulated on the non-touch base V2.3; broken out for the
#define BOARD_SDA (18)  // touch-overlay variant.
#define TOUCH_INT (47)
