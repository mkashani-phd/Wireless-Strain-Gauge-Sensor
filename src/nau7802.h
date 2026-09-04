// ---------------------------------------------------------------------------
// nau7802.h  -  minimal NAU7802 driver that exposes every register.
//
// Written from the Nuvoton NAU7802 datasheet register map so that the web GUI
// can poke arbitrary registers.  No external library needed.
// ---------------------------------------------------------------------------
#pragma once
#include <Arduino.h>
#include <Wire.h>

#define NAU_ADDR 0x2A

// ---- register map ---------------------------------------------------------
enum {
  NAU_PU_CTRL     = 0x00,
  NAU_CTRL1       = 0x01,
  NAU_CTRL2       = 0x02,
  NAU_OCAL1_B2    = 0x03,   // 24-bit channel-1 offset cal (0x03..0x05)
  NAU_GCAL1_B3    = 0x06,   // 32-bit channel-1 gain cal   (0x06..0x09)
  NAU_OCAL2_B2    = 0x0A,   // 24-bit channel-2 offset cal (0x0A..0x0C)
  NAU_GCAL2_B3    = 0x0D,   // 32-bit channel-2 gain cal   (0x0D..0x10)
  NAU_I2C_CTRL    = 0x11,
  NAU_ADCO_B2     = 0x12,   // 24-bit conversion result    (0x12..0x14)
  NAU_ADCO_B1     = 0x13,
  NAU_ADCO_B0     = 0x14,
  NAU_ADC_REG     = 0x15,   // "ADC" register (chopper control), shared w/ OTP
  NAU_OTP_B1      = 0x16,
  NAU_OTP_B0      = 0x17,
  NAU_PGA         = 0x1B,
  NAU_PGA_PWR     = 0x1C,
  NAU_DEVICE_REV  = 0x1F
};

// PU_CTRL (0x00) bits
#define PU_RR     0   // register reset
#define PU_PUD    1   // power up digital
#define PU_PUA    2   // power up analog
#define PU_PUR    3   // power up ready (RO)
#define PU_CS     4   // cycle start
#define PU_CR     5   // cycle ready / data ready (RO)
#define PU_OSCS   6   // clock source
#define PU_AVDDS  7   // 1 = use internal LDO for AVDD

// CTRL1 (0x01): [2:0] GAINS, [5:3] VLDO, 6 DRDY_SEL, 7 CRP
// CTRL2 (0x02): [1:0] CALMOD, 2 CALS, 3 CAL_ERR, [6:4] CRS, 7 CHS

// index -> human value tables (index is the raw bitfield value)
static const uint16_t NAU_GAIN_TABLE[8] = {1, 2, 4, 8, 16, 32, 64, 128};
static const uint16_t NAU_SPS_TABLE[8]  = {10, 20, 40, 80, 0, 0, 0, 320};
static const char*    NAU_LDO_TABLE[8]  = {"4.5", "4.2", "3.9", "3.6",
                                           "3.3", "3.0", "2.7", "2.4"};

class NAU7802 {
public:
  bool begin(TwoWire *w = &Wire) {
    _w = w;
    if (!ping()) return false;
    reset();
    if (!powerUp()) return false;
    writeReg(NAU_ADC_REG, 0x30);            // disable ADC chopper (datasheet rec.)
    setBit(NAU_PGA_PWR, 7, true);           // PGA 330 pF decoupling cap
    setBit(NAU_PU_CTRL, PU_AVDDS, true);    // AVDD from internal LDO
    return true;
  }

  bool ping() {
    _w->beginTransmission(NAU_ADDR);
    return _w->endTransmission() == 0;
  }

  // ---- raw register access (this is what the GUI drives) ------------------
  bool writeReg(uint8_t reg, uint8_t val) {
    _w->beginTransmission(NAU_ADDR);
    _w->write(reg);
    _w->write(val);
    return _w->endTransmission() == 0;
  }

  uint8_t readReg(uint8_t reg) {
    _w->beginTransmission(NAU_ADDR);
    _w->write(reg);
    if (_w->endTransmission(false) != 0) return 0;
    if (_w->requestFrom((uint8_t)NAU_ADDR, (uint8_t)1) != 1) return 0;
    return _w->read();
  }

  bool setBit(uint8_t reg, uint8_t bit, bool on) {
    uint8_t v = readReg(reg);
    v = on ? (v | (1 << bit)) : (v & ~(1 << bit));
    return writeReg(reg, v);
  }

  bool getBit(uint8_t reg, uint8_t bit) { return (readReg(reg) >> bit) & 1; }

  // write a bitfield without disturbing the rest of the register
  bool setField(uint8_t reg, uint8_t lsb, uint8_t width, uint8_t val) {
    uint8_t mask = ((1 << width) - 1) << lsb;
    uint8_t v = readReg(reg);
    v = (v & ~mask) | ((val << lsb) & mask);
    return writeReg(reg, v);
  }

  // ---- lifecycle ----------------------------------------------------------
  void reset() {
    setBit(NAU_PU_CTRL, PU_RR, true);
    delay(2);
    setBit(NAU_PU_CTRL, PU_RR, false);
    delay(2);
  }

  bool powerUp() {
    setBit(NAU_PU_CTRL, PU_PUD, true);
    setBit(NAU_PU_CTRL, PU_PUA, true);
    uint32_t t0 = millis();
    while (!getBit(NAU_PU_CTRL, PU_PUR)) {
      if (millis() - t0 > 200) return false;
      delay(1);
    }
    setBit(NAU_PU_CTRL, PU_CS, true);       // start continuous conversions
    return true;
  }

  // ---- configuration ------------------------------------------------------
  void setGainIdx(uint8_t idx)  { setField(NAU_CTRL1, 0, 3, idx & 7); }
  uint8_t getGainIdx()          { return readReg(NAU_CTRL1) & 0x07; }

  void setLdoIdx(uint8_t idx)   { setField(NAU_CTRL1, 3, 3, idx & 7);
                                  setBit(NAU_PU_CTRL, PU_AVDDS, true); }
  uint8_t getLdoIdx()           { return (readReg(NAU_CTRL1) >> 3) & 0x07; }

  void setSpsIdx(uint8_t idx)   { setField(NAU_CTRL2, 4, 3, idx & 7); }
  uint8_t getSpsIdx()           { return (readReg(NAU_CTRL2) >> 4) & 0x07; }

  void setChannel(uint8_t ch)   { setBit(NAU_CTRL2, 7, ch == 2); }  // 1 or 2
  uint8_t getChannel()          { return getBit(NAU_CTRL2, 7) ? 2 : 1; }

  // CALMOD: 0 = internal offset, 2 = external offset, 3 = external gain
  // Returns true on success, false on CAL_ERR or timeout.
  bool calibrateAFE(uint8_t calmod = 0) {
    setField(NAU_CTRL2, 0, 2, calmod);
    setBit(NAU_CTRL2, 2, true);             // CALS = start
    uint32_t t0 = millis();
    while (getBit(NAU_CTRL2, 2)) {          // hardware clears CALS when done
      if (millis() - t0 > 1500) return false;
      delay(2);
    }
    return !getBit(NAU_CTRL2, 3);           // CAL_ERR
  }

  // ---- data ---------------------------------------------------------------
  bool available() { return getBit(NAU_PU_CTRL, PU_CR); }

  int32_t readRaw() {
    _w->beginTransmission(NAU_ADDR);
    _w->write(NAU_ADCO_B2);
    if (_w->endTransmission(false) != 0) return _last;
    if (_w->requestFrom((uint8_t)NAU_ADDR, (uint8_t)3) != 3) return _last;
    int32_t v = ((int32_t)_w->read() << 16) |
                ((int32_t)_w->read() << 8)  |
                 (int32_t)_w->read();
    if (v & 0x800000) v |= 0xFF000000;      // sign-extend 24 -> 32 bit
    _last = v;
    return v;
  }

  uint8_t revision() { return readReg(NAU_DEVICE_REV) & 0x0F; }

private:
  TwoWire *_w = &Wire;
  int32_t  _last = 0;
};
