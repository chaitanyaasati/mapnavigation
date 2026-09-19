#pragma once
// =============================================================================
//  Cheeko Gotchi V2  ("CGOTCHI ESP32-S3 AI Development Kit")  - pin map
// =============================================================================
//  Recovered 2026-09-19 from the stock firmware (GPIO-matrix dump over the
//  built-in USB-JTAG) and confirmed with hw_probe/. Not published anywhere.
//
//  Module: ESP32-S3 rev 0.2, 16 MB quad flash, 8 MB octal PSRAM, native USB.
//  Build:  esp32:esp32:esp32s3  PSRAM=opi FlashSize=16M USBMode=hwcdc CDCOnBoot=cdc
// -----------------------------------------------------------------------------

// --- LCD: JD9853, 240x296 portrait, SPI2 (FSPI), no MISO -------------------
#define LCD_W          240
#define LCD_H          296
#define PIN_LCD_SCLK   9
#define PIN_LCD_MOSI   10
#define PIN_LCD_CS     14
#define PIN_LCD_DC     8
#define PIN_LCD_RST    17
#define PIN_LCD_BL     13     // PWM backlight (LEDC in stock fw)

// --- I2C0: touch, codec, IMU -------------------------------------------------
#define PIN_I2C_SDA    12
#define PIN_I2C_SCL    11
#define I2C_ADDR_TOUCH 0x15   // CST810 (CST8xx protocol), single touch, no INT pin found -> poll
#define I2C_ADDR_ES8311 0x18  // DAC / speaker
#define I2C_ADDR_IMU   0x19   // LIS2DH-class accelerometer (WHO_AM_I 0x33)
#define I2C_ADDR_ES7210 0x40  // 4-mic ADC
#define I2C_ADDR_GAUGE 0x55   // BQ27220, only answers with a battery fitted

// --- I2S0: ES8311 out + ES7210 in, shared clocks ----------------------------
#define PIN_I2S_MCLK   5
#define PIN_I2S_BCLK   15
#define PIN_I2S_WS     16
#define PIN_I2S_DOUT   6      // ESP32 -> ES8311
#define PIN_I2S_DIN    7      // ES7210 -> ESP32

// --- Power control (partly understood) ----------------------------------------
// GPIO2: external pull-up; stock drives it LOW. Left floating (HIGH) the whole
//        board powered itself off after ~30 min; driven HIGH for ~3 s it powered
//        off at once. Keep it LOW like stock.
// GPIO4: external pull-down; stock drives it LOW. Pulsing/holding it HIGH had no
//        observable effect. Left as input.
// Known hardware quirk (also with the ORIGINAL firmware): the display backlight
// switches off after a while and only a power-cycle brings it back. The LEDC
// PWM on GPIO13 keeps running when that happens; nothing on the ESP side
// (LCD re-init, PWM cycling, GPIO2/4 games) restores it.
// Speaker: ES8311 output is configured and I2S clocks are proven by the mic
// path, but no sound comes out - the amp enable was not found on GPIO
// 1/2/3/4/18/21/38/41/42/47/48 (each tried HIGH during a tone). TODO.
#define PIN_POWER_HOLD 2      // keep LOW (stock does)
#define PIN_GPIO4      4      // purpose unknown; leave as input

// --- Buttons (active low) ----------------------------------------------------
#define PIN_KEY_VOL_UP   40   // also wired to GPIO0 (BOOT) - holding it at reset enters download mode
#define PIN_KEY_VOL_DOWN 39
// Power key is hardware-only (latch); not visible on a GPIO we found.

// --- Reserved: 19/20 USB, 26-32 flash, 33-37 PSRAM, 43/44 UART0, 45/46 strap
