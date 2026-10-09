#include "BatteryMonitor.h"

#include <Arduino.h>
#include <BoardConfig.h>
#include <Logging.h>
#include <M5Pm1.h>
#include <esp_idf_version.h>
#if ESP_IDF_VERSION_MAJOR < 5
#include <esp_adc_cal.h>
#endif

#include <algorithm>
#include <cmath>

#if FREEINK_BATTERY_I2C_GAUGE
#include <Wire.h>

// Minimal, dependency-free I2C fuel-gauge read for boards that carry one (e.g.
// LilyGo T5 S3: BQ27220 gauge + BQ25896 charger). Standard TI command registers;
// the gauge reports true battery state, so no ADC pin or divider is involved.
// Addresses/pins come from BoardConfig::ACTIVE.batteryGauge.
namespace {
constexpr uint8_t BQ27220_VOLTAGE = 0x08;          // battery voltage, mV (u16 LE)
constexpr uint8_t BQ27220_CURRENT = 0x0C;          // average current, signed mA (i16 LE)
constexpr uint8_t BQ27220_STATE_OF_CHARGE = 0x2C;  // SoC, percent (u16 LE)
constexpr uint8_t BQ25896_REG_STATUS = 0x0B;       // CHRG_STAT in bits [4:3]

// The gauge's I2C controller (Wire or Wire1) per BoardConfig. On single-bus SoCs
// (ESP32-C3, SOC_I2C_NUM == 1) Wire1 doesn't exist, so always use Wire there.
TwoWire& gaugeWire() {
#if SOC_I2C_NUM > 1
  if (BoardConfig::ACTIVE.batteryGauge.i2cBus == 1) return Wire1;
#endif
  return Wire;
}

// Standard I2C bus-recovery sequence, run directly on the raw GPIOs before
// Wire takes ownership of the pins. If a slave was left holding SDA low mid-
// transaction (e.g. a brownout during a previous session, or the slave seeing
// a transaction start without a matching stop), the bus looks permanently
// busy to every future transaction -- which matches a gauge that fails from
// its very first-ever read, not an occasional failure. Pulses SCL up to 9
// times to walk a stuck slave through releasing SDA, then issues a manual
// STOP condition. No-op if SDA is already idle-high.
void recoverI2cBus(int8_t sdaPin, int8_t sclPin) {
  if (sdaPin < 0 || sclPin < 0) return;
  pinMode(sdaPin, INPUT_PULLUP);
  pinMode(sclPin, OUTPUT_OPEN_DRAIN);
  digitalWrite(sclPin, HIGH);
  delayMicroseconds(5);
  if (digitalRead(sdaPin) == HIGH) return;  // already idle, nothing stuck
  LOG_DBG("BAT", "I2C bus recovery: SDA stuck low, pulsing SCL");
  for (int i = 0; i < 9 && digitalRead(sdaPin) == LOW; ++i) {
    digitalWrite(sclPin, LOW);
    delayMicroseconds(5);
    digitalWrite(sclPin, HIGH);
    delayMicroseconds(5);
  }
  // Manual STOP: SDA low-to-high while SCL is high.
  pinMode(sdaPin, OUTPUT_OPEN_DRAIN);
  digitalWrite(sdaPin, LOW);
  delayMicroseconds(5);
  digitalWrite(sclPin, HIGH);
  delayMicroseconds(5);
  digitalWrite(sdaPin, HIGH);
  delayMicroseconds(5);
  LOG_DBG("BAT", "I2C bus recovery: SDA now %s", digitalRead(sdaPin) == HIGH ? "released" : "still stuck");
}

bool g_wireReady = false;
void ensureWire() {
  if (g_wireReady) return;
  const auto& g = BoardConfig::ACTIVE.batteryGauge;
  recoverI2cBus(g.i2cSda, g.i2cScl);
  gaugeWire().begin(g.i2cSda, g.i2cScl, g.i2cHz);
  g_wireReady = true;
}

bool readReg16(uint8_t addr, uint8_t reg, uint16_t& out) {
  if (addr == 0) return false;
  ensureWire();
  TwoWire& w = gaugeWire();
  w.beginTransmission(addr);
  w.write(reg);
  if (w.endTransmission(false) != 0) return false;
  if (w.requestFrom(addr, static_cast<uint8_t>(2), static_cast<uint8_t>(true)) < 2) return false;
  const uint8_t lo = w.read();
  const uint8_t hi = w.read();
  out = static_cast<uint16_t>(lo) | (static_cast<uint16_t>(hi) << 8);
  return true;
}

bool readReg8(uint8_t addr, uint8_t reg, uint8_t& out) {
  if (addr == 0) return false;
  ensureWire();
  TwoWire& w = gaugeWire();
  w.beginTransmission(addr);
  w.write(reg);
  if (w.endTransmission(false) != 0) return false;
  if (w.requestFrom(addr, static_cast<uint8_t>(1), static_cast<uint8_t>(true)) < 1) return false;
  out = w.read();
  return true;
}

bool writeReg8(uint8_t addr, uint8_t reg, uint8_t val) {
  if (addr == 0) return false;
  ensureWire();
  TwoWire& w = gaugeWire();
  w.beginTransmission(addr);
  w.write(reg);
  w.write(val);
  return w.endTransmission(true) == 0;
}

// --- CW2017 (CellWise) fuel gauge -------------------------------------------
// Register map + init recovered from the Xteink X4 Pro OEM firmware (the
// XTEink::Cw2017PowerHal class in app1) via Ghidra. Unlike the BQ27220, the CW2017
// reports 0% until a matching 80-byte BATINFO battery profile is resident, so init
// must verify/upload one before SoC reads mean anything.
constexpr uint8_t CW2017_REG_VERSION = 0x00;  // running state in (ver & 0xFD) == 0x0D
constexpr uint8_t CW2017_REG_VCELL_H = 0x02;  // 14-bit VCELL, big-endian over 0x02/0x03
constexpr uint8_t CW2017_REG_SOC = 0x04;      // integer percent (0x05 = fraction, unused)
constexpr uint8_t CW2017_REG_MODE = 0x08;     // soft-reset / sleep control
constexpr uint8_t CW2017_REG_CONFIG = 0x0B;   // bit7 = profile-loaded / update-enable
constexpr uint8_t CW2017_REG_BATINFO = 0x10;  // 80-byte profile spans 0x10..0x5F

// The exact BATINFO profile the OEM uploads (app1 table @ DROM 0x3c5d8d00). This is
// battery-model-specific; it is the profile for the X4 Pro's cell.
constexpr uint8_t CW2017_BATINFO[80] = {
    0x50, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xaa, 0xbf, 0xb5, 0xb4, 0xa4, 0x9c, 0xeb, 0xe2,
    0xdf, 0xe5, 0xca, 0xa0, 0x8a, 0x62, 0x53, 0x48, 0x40, 0x3a, 0x32, 0xb1, 0xae, 0xda, 0xb5, 0xff,
    0xff, 0xff, 0xe8, 0xdb, 0xd9, 0xd6, 0xd4, 0xd2, 0xd0, 0xcb, 0xc3, 0xbc, 0x9e, 0x87, 0x7b, 0x71,
    0x72, 0x7c, 0x8c, 0xa3, 0xb7, 0xc8, 0xa5, 0x4f, 0x00, 0x00, 0xab, 0x02, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x64, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x23};

// Soft-reset: MODE 0xF0 -> 0x30 -> 0x00, 20 ms apart (OEM FUN_4215042c).
void cw2017Reset(uint8_t addr) {
  writeReg8(addr, CW2017_REG_MODE, 0xF0);
  delay(20);
  writeReg8(addr, CW2017_REG_MODE, 0x30);
  delay(20);
  writeReg8(addr, CW2017_REG_MODE, 0x00);
  delay(20);
}

// One-shot: make sure a valid BATINFO profile is loaded. Wakes/resets the gauge if it
// isn't running, then uploads+enables the profile only if the resident bytes don't
// already match (the OEM leaves it resident across warm boots, so this is usually a
// verify-only no-op). Bounded polling so a cold gauge can't stall boot.
void cw2017EnsureProfile(uint8_t addr) {
  uint8_t ver = 0;
  if (!readReg8(addr, CW2017_REG_VERSION, ver)) return;  // gauge absent; nothing to do
  if ((ver & 0xFD) != 0x0D) cw2017Reset(addr);

  uint8_t cfg = 0;
  if (readReg8(addr, CW2017_REG_CONFIG, cfg) && (cfg & 0x80)) {
    bool match = true;
    for (uint8_t i = 0; i < sizeof(CW2017_BATINFO); ++i) {
      uint8_t b = 0;
      if (!readReg8(addr, static_cast<uint8_t>(CW2017_REG_BATINFO + i), b) || b != CW2017_BATINFO[i]) {
        match = false;
        break;
      }
    }
    if (match) return;  // correct profile already loaded
  }

  for (uint8_t i = 0; i < sizeof(CW2017_BATINFO); ++i) {
    writeReg8(addr, static_cast<uint8_t>(CW2017_REG_BATINFO + i), CW2017_BATINFO[i]);
  }
  writeReg8(addr, CW2017_REG_CONFIG, 0x80);  // update-enable (bit7); alert threshold = 0
  delay(20);
  cw2017Reset(addr);
  for (int i = 0; i < 50; ++i) {  // ~1 s cap for the SoC to become valid
    uint8_t soc = 0;
    if (readReg8(addr, CW2017_REG_SOC, soc) && soc <= 100) break;
    delay(20);
  }
}

// --- LC709203F (ON Semi / Nisshinbo) fuel gauge -----------------------------
// Voltage-model gauge (no sense resistor); register map from Adafruit's
// open-source Adafruit_LC709203F library (BSD), the driver for the exact
// breakout this board uses. Every transaction carries an 8-bit CRC (SMBus
// PEC) computed over the address/command/data bytes -- the chip silently
// rejects reads/writes that don't include a correct one.
constexpr uint8_t LC709203F_CMD_INITRSOC = 0x07;
constexpr uint8_t LC709203F_CMD_CELLVOLTAGE = 0x09;  // battery voltage, mV (u16 LE)
constexpr uint8_t LC709203F_CMD_APA = 0x0B;          // pack-size compensation
constexpr uint8_t LC709203F_CMD_RSOC = 0x0D;         // state of charge, percent 0-100 (u16 LE)
constexpr uint8_t LC709203F_CMD_BATTPROF = 0x12;     // 0 = 3.7V profile, 1 = 4.2V profile
constexpr uint8_t LC709203F_CMD_POWERMODE = 0x15;    // 1 = operate, 2 = sleep
constexpr uint8_t LC709203F_CMD_STATUSBIT = 0x16;    // temperature source: 0 = I2C, 1 = thermistor
constexpr uint8_t LC709203F_APA_2000MAH = 0x2D;      // closest preset to the board's 2100mAh pack

uint8_t lc709203fCrc8(const uint8_t* data, int len) {
  constexpr uint8_t POLYNOMIAL = 0x07;
  uint8_t crc = 0x00;
  for (int j = len; j; --j) {
    crc ^= *data++;
    for (int i = 8; i; --i) {
      crc = (crc & 0x80) ? static_cast<uint8_t>((crc << 1) ^ POLYNOMIAL) : static_cast<uint8_t>(crc << 1);
    }
  }
  return crc;
}

bool lc709203fReadWord(uint8_t addr, uint8_t command, uint16_t& out) {
  if (addr == 0) return false;
  ensureWire();
  TwoWire& w = gaugeWire();
  w.beginTransmission(addr);
  w.write(command);
  // True repeated start (endTransmission(false) + requestFrom), per the
  // datasheet's read sequence: a stop in between was tried as a workaround for
  // an earlier ESP_ERR_INVALID_STATE, but that turned out to be a symptom of
  // SDA/SCL being swapped on the wiring, not a real driver incompatibility --
  // with that fixed, a stop here makes the chip lose its register-address
  // context and the read comes back as unclocked 0xFFFF. Repeated start keeps
  // the transaction atomic, as this chip expects.
  const uint8_t endResult = w.endTransmission(false);
  if (endResult != 0) {
    LOG_DBG("BAT", "LC709203F cmd 0x%02x: endTransmission failed (%u)", command, endResult);
    return false;
  }
  const size_t got = w.requestFrom(addr, static_cast<uint8_t>(3), static_cast<uint8_t>(true));
  if (got < 3) {
    LOG_DBG("BAT", "LC709203F cmd 0x%02x: requestFrom got %u/3 bytes", command, static_cast<unsigned>(got));
    return false;
  }
  uint8_t reply[6];
  reply[0] = static_cast<uint8_t>(addr << 1);          // write byte, as sent on the wire
  reply[1] = command;
  reply[2] = static_cast<uint8_t>(reply[0] | 0x1);      // read byte, as sent on the wire
  reply[3] = w.read();                                  // data low
  reply[4] = w.read();                                  // data high
  reply[5] = w.read();                                  // PEC the chip sent
  const uint8_t computedCrc = lc709203fCrc8(reply, 5);
  LOG_DBG("BAT", "LC709203F cmd 0x%02x: data=0x%02x%02x crc_recv=0x%02x crc_calc=0x%02x", command, reply[4], reply[3],
          reply[5], computedCrc);
  if (computedCrc != reply[5]) return false;
  out = static_cast<uint16_t>(reply[3]) | (static_cast<uint16_t>(reply[4]) << 8);
  return true;
}

bool lc709203fWriteWord(uint8_t addr, uint8_t command, uint16_t data) {
  if (addr == 0) return false;
  ensureWire();
  TwoWire& w = gaugeWire();
  uint8_t send[5];
  send[0] = static_cast<uint8_t>(addr << 1);
  send[1] = command;
  send[2] = data & 0xFF;
  send[3] = static_cast<uint8_t>(data >> 8);
  send[4] = lc709203fCrc8(send, 4);
  w.beginTransmission(addr);
  w.write(send + 1, 4);  // command + data + PEC, not the address byte (the I2C peripheral sends that)
  const uint8_t result = w.endTransmission(true);
  LOG_DBG("BAT", "LC709203F write cmd 0x%02x data 0x%04x: endTransmission=%u", command, data, result);
  return result == 0;
}

// One-shot init: operate mode, pack-size compensation, 4.2V profile (this is a
// standard 1S LiPo, matching BatteryMonitor's own discharge curve elsewhere in
// this file), and I2C temperature mode -- not thermistor mode, since this
// board's Therm pin is left unconnected (see BoardConfig.h's T5_47 profile
// comment); thermistor mode with nothing attached would feed the gauge a
// floating/invalid temperature instead of its internal default.
//
// Retries on failure rather than latching "done" either way: an I2C glitch
// during the very first attempt (e.g. the bus not fully settled yet right
// after boot) used to leave `inited` permanently true with some/all of these
// writes never having landed -- the gauge then free-runs on whatever its
// power-on-reset defaults are (wrong power mode, wrong pack-size compensation,
// wrong profile) for the rest of the session, no further attempt possible.
// Each readGaugeSoc()/readGaugeMillivolts() call is already throttled by
// HalPowerManager's cache (~1.5s), so a retry-every-call here on persistent
// failure is bounded, not a tight loop.
bool lc709203fEnsureInit(uint8_t addr) {
  static bool inited = false;
  if (inited) return true;
  bool ok = true;
  ok &= lc709203fWriteWord(addr, LC709203F_CMD_POWERMODE, 0x0001);
  ok &= lc709203fWriteWord(addr, LC709203F_CMD_APA, LC709203F_APA_2000MAH);
  ok &= lc709203fWriteWord(addr, LC709203F_CMD_BATTPROF, 0x0001);
  ok &= lc709203fWriteWord(addr, LC709203F_CMD_STATUSBIT, 0x0000);
  if (!ok) {
    LOG_ERR("BAT", "LC709203F init: one or more writes failed, will retry on next read");
    return false;
  }
  inited = true;
  return true;
}

// SoC (0..100) from the active gauge, dispatched by type. false on I2C failure.
bool readGaugeSoc(uint16_t& out) {
  const auto& g = BoardConfig::ACTIVE.batteryGauge;
  if (g.gaugeType == BoardConfig::GaugeType::Lc709203f) {
    if (!lc709203fEnsureInit(g.gaugeAddr)) return false;
    uint16_t rsoc = 0;
    if (!lc709203fReadWord(g.gaugeAddr, LC709203F_CMD_RSOC, rsoc)) return false;
    out = rsoc > 100 ? 100 : rsoc;
    return true;
  }
  if (g.gaugeType == BoardConfig::GaugeType::Cw2017) {
    static bool inited = false;
    if (!inited) {
      cw2017EnsureProfile(g.gaugeAddr);
      inited = true;
    }
    uint8_t soc = 0;
    if (!readReg8(g.gaugeAddr, CW2017_REG_SOC, soc)) return false;
    out = soc > 100 ? 100 : soc;
    return true;
  }
  uint16_t soc = 0;
  if (!readReg16(g.gaugeAddr, BQ27220_STATE_OF_CHARGE, soc)) return false;
  out = soc > 100 ? 100 : soc;
  return true;
}

// Battery voltage (mV) from the active gauge, dispatched by type. false on failure.
bool readGaugeMillivolts(uint16_t& out) {
  const auto& g = BoardConfig::ACTIVE.batteryGauge;
  if (g.gaugeType == BoardConfig::GaugeType::Lc709203f) {
    // Best-effort: cell voltage is a near-raw ADC reading on this chip, not
    // dependent on the RSOC-algorithm config init sets up, so still attempt
    // the read even if init hasn't succeeded yet (unlike readGaugeSoc, which
    // gates on it since RSOC accuracy does depend on that config).
    (void)lc709203fEnsureInit(g.gaugeAddr);
    uint16_t mv = 0;
    if (!lc709203fReadWord(g.gaugeAddr, LC709203F_CMD_CELLVOLTAGE, mv)) return false;
    out = mv;
    return true;
  }
  if (g.gaugeType == BoardConfig::GaugeType::Cw2017) {
    uint8_t hi = 0, lo = 0;
    if (!readReg8(g.gaugeAddr, CW2017_REG_VCELL_H, hi)) return false;
    if (!readReg8(g.gaugeAddr, static_cast<uint8_t>(CW2017_REG_VCELL_H + 1), lo)) return false;
    const uint16_t raw14 = static_cast<uint16_t>((hi & 0x3F) << 8) | lo;
    out = static_cast<uint16_t>((raw14 * 5 + 8) >> 4);  // OEM formula (~0.3125 mV/LSB, rounded)
    return true;
  }
  uint16_t mv = 0;
  if (!readReg16(g.gaugeAddr, BQ27220_VOLTAGE, mv)) return false;
  out = mv;
  return true;
}

// Charging state for an I2C-gauge board, from the active board's gauge config.
// Two sources, in order of preference:
//   1. A dedicated charger IC (BQ25896): CHRG_STAT in REG0B[4:3] — 01 pre-charge
//      or 10 fast-charge means charging. Used by LilyGo T5 S3.
//   2. Gauge-native fallback (BQ27220 Current(), signed mA): current flowing INTO
//      the battery (> 0) means charging. Lets boards with a gauge but NO charger
//      IC — e.g. Xteink X3 — still report charge status. Current() is used rather
//      than the BatteryStatus DSG bit because DSG also clears during rest, so it
//      can't tell "charging" from "idle"; the current sign can.
// `known` is set false only when neither source responds (transient I2C failure or
// a board with neither gaugeAddr nor chargerAddr).
bool readGaugeCharging(bool& known) {
  const auto& g = BoardConfig::ACTIVE.batteryGauge;
  // CW2017 and LC709203F both lack a current-direction register (voltage-model
  // gauges, no sense resistor) and this board has no separate charger IC, so
  // charging state isn't observable from either.
  if (g.gaugeType == BoardConfig::GaugeType::Cw2017 || g.gaugeType == BoardConfig::GaugeType::Lc709203f) {
    known = false;
    return false;
  }
  if (g.chargerAddr != 0) {
    uint8_t status = 0;
    if (readReg8(g.chargerAddr, BQ25896_REG_STATUS, status)) {
      known = true;
      const uint8_t chrg = (status >> 3) & 0x03;
      return chrg == 0x01 || chrg == 0x02;
    }
  }
  uint16_t raw = 0;
  if (readReg16(g.gaugeAddr, BQ27220_CURRENT, raw)) {
    known = true;
    return static_cast<int16_t>(raw) > 0;
  }
  known = false;
  return false;
}
}  // namespace
#endif  // FREEINK_BATTERY_I2C_GAUGE

namespace {
constexpr uint8_t M5PM1_REG_PWR_SRC = 0x04;
constexpr uint8_t M5PM1_REG_VREF_L = 0x20;
constexpr uint8_t M5PM1_PWR_SRC_5VIN = 1u << 0;
constexpr uint8_t M5PM1_PWR_SRC_5VINOUT = 1u << 1;

uint16_t readLe16(const uint8_t* bytes) {
  return static_cast<uint16_t>(bytes[0]) | (static_cast<uint16_t>(bytes[1]) << 8);
}
}  // namespace

BatteryMonitor::BatteryMonitor()
    : BatteryMonitor(BoardConfig::ACTIVE.batteryAdc, BoardConfig::ACTIVE.batteryDividerMultiplier,
                     BoardConfig::ACTIVE.batteryChargeStatus) {}

BatteryMonitor::BatteryMonitor(int8_t adcPin, float dividerMultiplier, int8_t chargeStatusPin)
    : _adcPin(adcPin), _dividerMultiplier(dividerMultiplier), _chargeStatusPin(chargeStatusPin) {
  if (_chargeStatusPin >= 0) {
    pinMode(_chargeStatusPin, INPUT_PULLUP);
  }
}

bool BatteryMonitor::hasAdcBackend() const {
  return _adcPin >= 0;
}

bool BatteryMonitor::hasGaugeBackend() const {
#if FREEINK_BATTERY_I2C_GAUGE
  return BoardConfig::ACTIVE.batteryGauge.gaugeAddr != 0;
#else
  return false;
#endif
}

bool BatteryMonitor::hasM5Pm1Backend() const {
  return BoardConfig::isM5StackPaperColor() || BoardConfig::isPaperMono();
}

uint16_t BatteryMonitor::readPercentage() const {
#if FREEINK_BATTERY_I2C_GAUGE
  // Runtime, per active profile: gauge boards (X3, LilyGo, X4 Pro) read SoC over I2C;
  // ADC boards (X4) in the same binary fall through to the divider path below.
  if (BoardConfig::ACTIVE.batteryGauge.gaugeAddr != 0) {
    uint16_t soc = 0;
    if (!readGaugeSoc(soc)) return 0;
    return soc;
  }
#endif
  if (hasM5Pm1Backend()) {
    Status status;
    if (readM5Pm1Status(status) && status.percentageKnown) return status.percentage;
    return 0;
  }
  if (!hasAdcBackend()) return 0;
  return percentageFromMillivolts(readMillivolts());
}

bool BatteryMonitor::readPercentageChecked(uint16_t& out) const {
#if FREEINK_BATTERY_I2C_GAUGE
  if (BoardConfig::ACTIVE.batteryGauge.gaugeAddr != 0) {
    uint16_t soc = 0;
    if (!readGaugeSoc(soc)) return false;
    out = soc;
    return true;
  }
#endif
  if (hasM5Pm1Backend()) {
    Status status;
    if (!readM5Pm1Status(status) || !status.percentageKnown) return false;
    out = status.percentage;
    return true;
  }
  if (!hasAdcBackend()) return false;
  out = percentageFromMillivolts(readMillivolts());
  return true;
}

BatteryMonitor::Status BatteryMonitor::readStatus() const {
  Status status;

#if FREEINK_BATTERY_I2C_GAUGE
  if (BoardConfig::ACTIVE.batteryGauge.gaugeAddr != 0) {
    status.supported = true;
    uint16_t soc = 0;
    if (readGaugeSoc(soc)) {
      status.percentageKnown = true;
      status.percentage = soc;
    }
    uint16_t mv = 0;
    if (readGaugeMillivolts(mv)) {
      status.millivoltsKnown = true;
      status.millivolts = mv;
    }
    // Charging: from a dedicated charger IC when present, else the gauge's own
    // Current() sign — so gauge-only boards (X3) report it too.
    bool chargingKnown = false;
    const bool charging = readGaugeCharging(chargingKnown);
    status.chargingKnown = chargingKnown;
    status.charging = charging;
    return status;
  }
#endif

  if (hasM5Pm1Backend()) {
    readM5Pm1Status(status);
    return status;
  }

  if (hasAdcBackend()) {
    status.supported = true;
    status.millivolts = readMillivolts();
    status.millivoltsKnown = status.millivolts > 0;
    if (status.millivoltsKnown) {
      status.percentage = percentageFromMillivolts(status.millivolts);
      status.percentageKnown = true;
    }
    if (_chargeStatusPin >= 0) {
      status.chargingKnown = true;
      status.charging = digitalRead(_chargeStatusPin) == LOW;
    }
  }
  return status;
}

uint16_t BatteryMonitor::readMillivolts() const {
#if FREEINK_BATTERY_I2C_GAUGE
  if (BoardConfig::ACTIVE.batteryGauge.gaugeAddr != 0) {
    uint16_t gaugeMv = 0;
    readGaugeMillivolts(gaugeMv);
    return gaugeMv;  // gauge reports true battery mV (no divider)
  }
#endif
  if (hasM5Pm1Backend()) {
    Status status;
    if (readM5Pm1Status(status) && status.millivoltsKnown) return status.millivolts;
    return 0;
  }
  if (!hasAdcBackend()) return 0;
#if ESP_IDF_VERSION_MAJOR < 5
  // ESP-IDF 4.x doesn't have analogReadMilliVolts, so calibrate manually.
  const uint16_t raw = analogRead(_adcPin);
  esp_adc_cal_characteristics_t adc_chars;
  esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_12, ADC_WIDTH_BIT_12, 1100, &adc_chars);
  const uint16_t mv = esp_adc_cal_raw_to_voltage(raw, &adc_chars);
#else
  // ESP-IDF 5.x has analogReadMilliVolts
  const uint16_t mv = analogReadMilliVolts(_adcPin);
#endif

  return static_cast<uint16_t>(mv * _dividerMultiplier);
}

double BatteryMonitor::readVolts() const {
  return static_cast<double>(readMillivolts()) / 1000.0;
}

bool BatteryMonitor::isCharging() const {
#if FREEINK_BATTERY_I2C_GAUGE
  // Gauge boards: prefer a charger IC's status (BQ25896), else fall back to the
  // gauge's own Current() sign, so a board with a gauge but no charger IC (e.g.
  // X3) still reports charging. Unknown/failed reads report false.
  if (BoardConfig::ACTIVE.batteryGauge.gaugeAddr != 0) {
    bool known = false;
    const bool charging = readGaugeCharging(known);
    return known && charging;
  }
#endif
  if (hasM5Pm1Backend()) {
    Status status;
    return readM5Pm1Status(status) && status.chargingKnown && status.charging;
  }
  if (_chargeStatusPin < 0) {
    return false;
  }
  // MCP73832-style /STAT: LOW while charging.
  return digitalRead(_chargeStatusPin) == LOW;
}

bool BatteryMonitor::readM5Pm1Status(Status& status) const {
  status.supported = true;
  if (!hasM5Pm1Backend()) return false;

  freeink::m5pm1::beginBus();

  // VREF/VBAT/VIN/5VOUT are four complete 16-bit little-endian millivolt
  // registers (0x20..0x27). Only ADC_RES at 0x28 is 12-bit. Read the whole
  // block atomically so paired bytes and the three rails share one sample.
  uint8_t rails[8] = {};
  const bool railsKnown = freeink::m5pm1::readBytes(M5PM1_REG_VREF_L, rails, sizeof(rails));
  if (railsKnown) {
    const uint16_t batMv = readLe16(rails + 2);
    const uint16_t vinMv = readLe16(rails + 4);
    const uint16_t vinOutMv = readLe16(rails + 6);
    status.millivoltsKnown = true;
    status.millivolts = batMv;
    status.percentageKnown = true;
    status.percentage = percentageFromMillivolts(batMv);
    status.pm1VinMv = vinMv;
    status.pm1VinOutMv = vinOutMv;
  }

  uint8_t powerSource = 0;
  const bool pwrSrcKnown = freeink::m5pm1::readReg(M5PM1_REG_PWR_SRC, &powerSource);
  if (pwrSrcKnown) {
    const uint8_t sources = powerSource & 0x07;
    status.pm1PowerSource = sources;
    status.externalPowerKnown = true;
    // PM1 manual: PWR_SRC is a bitmap, not the enum used by older M5PM1
    // wrappers. Multiple bits may be set at once (the connected Paper Mono
    // reports 0x05 = BAT | 5VIN). Unlike the ADC rail samples, these validity
    // bits drop when the cable is removed, so they are the authoritative
    // source for the charging badge.
    status.externalPower = (sources & (M5PM1_PWR_SRC_5VIN | M5PM1_PWR_SRC_5VINOUT)) != 0;
  }

  // Product semantics for Paper Mono: external supply present means charging.
  status.chargingKnown = status.externalPowerKnown;
  status.charging = status.externalPower;
  return status.percentageKnown || status.millivoltsKnown || status.externalPowerKnown;
}

// Standard 1S Li-ion / LiPo (4.20 V) rest-voltage discharge curve, one entry per
// 10% notch. The curve is deliberately not resampled any finer: between 20% and
// 60% the whole span is ~130 mV, so a tenth of a volt-step is already below the
// noise of any of the three backends. A caller that needs real resolution should
// read millivolts and show those instead.
//
// The 0% anchor is 3.45 V rather than the cell's protection cut-off. Below that
// the pack falls off a cliff and the remaining runtime is minutes, so reporting
// it as empty is honest; it also leaves headroom for the sag under an e-ink
// refresh, which is the heaviest load this device draws.
constexpr uint16_t LIION_NOTCH_MV[11] = {
    3450,  //   0%
    3680,  //  10%
    3740,  //  20%
    3770,  //  30%
    3790,  //  40%
    3820,  //  50%
    3870,  //  60%
    3920,  //  70%
    3980,  //  80%
    4060,  //  90%
    4200,  // 100%
};

// A notch change has to clear the boundary by this much before it is accepted.
// The 20-40% band is only 50 mV wide, so without a deadband a few millivolts of
// sampler noise would swap the icon back and forth between page turns. Kept
// well under the narrowest half-segment (10 mV) so no notch can become a trap.
constexpr uint16_t NOTCH_HYSTERESIS_MV = 8;

uint16_t BatteryMonitor::percentageFromMillivolts(uint16_t millivolts) {
  // A failed read reports 0 mV, which lands on 0% here. That is deliberate: the
  // cubic this table replaced evaluated to +7501 at 0 V and clamped to a
  // confident 100%, so an I2C or ADC failure showed a full battery.
  if (millivolts >= LIION_NOTCH_MV[10]) return 100;
  // Round at the midpoint of each segment instead of flooring, so a cell resting
  // just below 4.20 V straight off the charger still reads 100%.
  for (uint8_t i = 10; i > 0; --i) {
    const uint16_t boundary = static_cast<uint16_t>((LIION_NOTCH_MV[i - 1] + LIION_NOTCH_MV[i]) / 2);
    if (millivolts >= boundary) return static_cast<uint16_t>(i * 10);
  }
  return 0;
}

uint16_t BatteryMonitor::percentageFromMillivoltsStepped(uint16_t millivolts, uint16_t previousPercent, uint8_t step) {
  if (step == 0) step = 1;
  if (millivolts <= LIION_NOTCH_MV[0]) return 0;
  if (millivolts >= LIION_NOTCH_MV[10]) return 100;

  uint8_t segment = 0;
  while (segment < 9 && millivolts >= LIION_NOTCH_MV[segment + 1]) ++segment;
  const float spanMv = static_cast<float>(LIION_NOTCH_MV[segment + 1] - LIION_NOTCH_MV[segment]);
  const float exact = segment * 10.0f + 10.0f * static_cast<float>(millivolts - LIION_NOTCH_MV[segment]) / spanMv;

  if (previousPercent <= 100) {
    const float held = static_cast<float>(((previousPercent + step / 2) / step) * step);
    if (std::fabs(exact - held) < step * 0.5f + 1.0f) return static_cast<uint16_t>(held);
  }
  const auto snapped = static_cast<uint16_t>(std::lround(exact / step) * step);
  return std::min<uint16_t>(snapped, 100);
}

uint16_t BatteryMonitor::percentageFromMillivolts(uint16_t millivolts, uint16_t previousPercent) {
  const uint16_t notch = percentageFromMillivolts(millivolts);
  if (previousPercent > 100) return notch;  // no usable history
  const uint16_t previousNotch = static_cast<uint16_t>((previousPercent / 10) * 10);
  if (notch == previousNotch) return notch;

  // Re-run the lookup with the sample pushed back toward the notch we are
  // leaving. If it still crosses, the move is real; if not, hold.
  const int32_t bias = notch > previousNotch ? -NOTCH_HYSTERESIS_MV : NOTCH_HYSTERESIS_MV;
  const int32_t biased = std::clamp<int32_t>(static_cast<int32_t>(millivolts) + bias, 0, UINT16_MAX);
  return percentageFromMillivolts(static_cast<uint16_t>(biased)) == notch ? notch : previousNotch;
}
