#pragma once
// Minimal CST810/CST816 touch reader on Wire, so the I2C bus can be shared with
// the audio codecs (LovyanGFX's own touch driver programs the I2C peripheral
// directly and would fight with Wire). Raw controller axes; the caller rotates.
#include <Wire.h>
#include "board_config.h"

static inline void cst810_init() {
  Wire.beginTransmission(I2C_ADDR_TOUCH);
  Wire.write(0xFE); Wire.write(0x01);          // DisAutoSleep: keep answering without an INT line
  Wire.endTransmission();
}

static inline bool cst810_read(uint16_t& x, uint16_t& y) {
  Wire.beginTransmission(I2C_ADDR_TOUCH);
  Wire.write(0x02);                            // FingerNum, XposH, XposL, YposH, YposL
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((uint8_t)I2C_ADDR_TOUCH, (uint8_t)5) != 5) return false;
  uint8_t n = Wire.read(), xh = Wire.read(), xl = Wire.read(), yh = Wire.read(), yl = Wire.read();
  if ((n & 0x0F) == 0) return false;
  x = ((xh & 0x0F) << 8) | xl;
  y = ((yh & 0x0F) << 8) | yl;
  return true;
}
