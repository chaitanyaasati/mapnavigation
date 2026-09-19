// =============================================================================
//  hw_probe - Cheeko Gotchi V2 (ESP32-S3) hardware discovery sketch
// =============================================================================
//  Throwaway. Finds what the GPIO-matrix dump could not:
//    * which of GPIO 2/4/8/17 are the LCD DC and RST lines (JD9853 over SPI2)
//    * which GPIOs the buttons / touch-INT / headphone-detect sit on
//  and re-confirms the I2C bus contents and PSRAM/flash sizes.
//
//  Known from the stock firmware (read over USB-JTAG, see plan):
//    LCD SPI: SCLK 9, MOSI 10, CS 14, backlight PWM 13
//    I2C:     SDA 12, SCL 11   (CST810 0x15, ES8311 0x18, IMU 0x19, ES7210 0x40, BQ27220 0x55)
//
//  LESSON FROM RUN 1: driving GPIO2 high powered the board OFF. The stock
//  firmware holds GPIO2 and GPIO4 low, so those two are the power-kill line
//  and (probably) the speaker-amp enable - NEVER drive them high blindly.
//  That leaves GPIO8 / GPIO17 (both idle high in stock) for LCD DC and RST.
//
//  HOW TO USE
//    Watch the screen and the serial log together. The sketch cycles through
//    the (DC, RST) pairings of GPIO8/17, re-initialising the panel each time
//    and painting a solid colour with N white bars, where N = combo number.
//    Whichever combo puts a picture on the screen is the right one; the bar
//    count identifies it even if colours are off. Press the buttons at any
//    time - GPIO changes are printed as they happen.
//    Press BOOT (GPIO0) to run the OPTIONAL amp/kill test: it pulses GPIO4
//    high for 200 ms. If the board dies, GPIO4 is the kill line and GPIO2 is
//    the amp enable; if it survives, the other way round.
// =============================================================================
#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>

static const int PIN_SCLK = 9, PIN_MOSI = 10, PIN_CS = 14, PIN_BL = 13;
static const int PIN_SDA = 12, PIN_SCL = 11;
static const int CANDS[] = {8, 17};   // GPIO2/4 excluded: power-kill + amp-enable
static const int NCAND = 2;
static const int INPUTS[] = {0, 1, 3, 18, 21, 38, 39, 40, 41, 42, 47, 48};
// only these had pull-ups in the stock config; leave the rest floating to avoid surprises
static const int PULLUP_OK[] = {0, 39, 40};

static const int W = 240, H = 296;

SPIClass spi(FSPI);
int g_dc = -1;

// --- minimal JD9853 driver (init list borrowed from LovyanGFX Panel_JD9853) ---
static const uint8_t INIT[] = {
  0xDF, 2, 0x98, 0x53,  0xDE, 1, 0x00,  0xB2, 1, 0x38,
  0xB7, 4, 0x00, 0x29, 0x00, 0x51,
  0xBB, 6, 0x28, 0x2F, 0x55, 0x73, 0x63, 0xF0,
  0xC0, 2, 0x22, 0xA2,  0xC1, 1, 0x12,
  0xC3, 8, 0x7D, 0x08, 0x08, 0x0C, 0xC2, 0x72, 0x22, 0x88,
  0xC4, 12, 0x00, 0x00, 0xA0, 0x79, 0x0A, 0x0B, 0x16, 0x79, 0x0A, 0x0B, 0x16, 0x82,
  0xC8, 32, 0x3F, 0x29, 0x23, 0x20, 0x27, 0x2D, 0x2A, 0x2B, 0x2A, 0x28, 0x25, 0x17, 0x12, 0x0D, 0x07, 0x02,
            0x3F, 0x29, 0x23, 0x20, 0x27, 0x2D, 0x2A, 0x2B, 0x2A, 0x28, 0x25, 0x17, 0x12, 0x0D, 0x07, 0x02,
  0xD0, 5, 0x04, 0x04, 0x6A, 0x1C, 0x03,  0xD7, 2, 0x00, 0x20,  0xE6, 1, 0x10,
  0xDE, 1, 0x01,  0xBB, 1, 0x04,  0xD7, 1, 0x12,
  0xB7, 5, 0x03, 0x13, 0xE5, 0x38, 0x38,
  0xC1, 3, 0x14, 0x15, 0xC0,  0xC2, 2, 0x06, 0x3A,  0xC4, 2, 0x72, 0x12,
  0xBE, 1, 0x00,  0xDE, 1, 0x00,
  0x35, 1, 0x00,  0x36, 1, 0x00,  0x3A, 1, 0x05,
  0xFF
};

static void cmd(uint8_t c) {
  digitalWrite(g_dc, LOW);  digitalWrite(PIN_CS, LOW);
  spi.transfer(c);
  digitalWrite(PIN_CS, HIGH);
}
static void dat(const uint8_t* d, size_t n) {
  digitalWrite(g_dc, HIGH); digitalWrite(PIN_CS, LOW);
  spi.writeBytes(d, n);
  digitalWrite(PIN_CS, HIGH);
}
static void dat1(uint8_t v) { dat(&v, 1); }

static void lcdInit(int dc, int rst) {
  g_dc = dc;
  pinMode(dc, OUTPUT);
  if (rst >= 0) {
    pinMode(rst, OUTPUT);
    digitalWrite(rst, HIGH); delay(10);
    digitalWrite(rst, LOW);  delay(20);
    digitalWrite(rst, HIGH); delay(120);
  }
  cmd(0x01); delay(20);             // software reset (always, in case RST is NC)
  for (const uint8_t* p = INIT; *p != 0xFF;) {
    uint8_t c = *p++, n = *p++;
    cmd(c); if (n) dat(p, n); p += n;
  }
  cmd(0x11); delay(120);            // sleep out
  cmd(0x21);                        // inversion on (most JD9853 IPS panels want it; harmless for the probe)
  cmd(0x29); delay(10);             // display on
}

static void setWindow(int x0, int y0, int x1, int y1) {
  uint8_t a[4] = {(uint8_t)(x0 >> 8), (uint8_t)x0, (uint8_t)(x1 >> 8), (uint8_t)x1};
  uint8_t b[4] = {(uint8_t)(y0 >> 8), (uint8_t)y0, (uint8_t)(y1 >> 8), (uint8_t)y1};
  cmd(0x2A); dat(a, 4);
  cmd(0x2B); dat(b, 4);
  cmd(0x2C);
}

static void fillRect(int x0, int y0, int w, int h, uint16_t rgb565) {
  setWindow(x0, y0, x0 + w - 1, y0 + h - 1);
  static uint8_t line[W * 2];
  for (int i = 0; i < w; i++) { line[2 * i] = rgb565 >> 8; line[2 * i + 1] = rgb565; }
  digitalWrite(g_dc, HIGH); digitalWrite(PIN_CS, LOW);
  for (int y = 0; y < h; y++) spi.writeBytes(line, w * 2);
  digitalWrite(PIN_CS, HIGH);
}

// --- inputs ------------------------------------------------------------------
static int lastLevel[sizeof(INPUTS) / sizeof(INPUTS[0])];

static void pollInputs() {
  for (size_t i = 0; i < sizeof(INPUTS) / sizeof(INPUTS[0]); i++) {
    int v = digitalRead(INPUTS[i]);
    if (v != lastLevel[i]) {
      Serial.printf("  [input] GPIO%-2d -> %d\n", INPUTS[i], v);
      lastLevel[i] = v;
    }
  }
}

static void i2cScan() {
  Serial.println("I2C scan (SDA 12 / SCL 11):");
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) Serial.printf("  found 0x%02X\n", a);
  }
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("\n=== hw_probe: Cheeko Gotchi V2 ===");
  Serial.printf("chip: %s rev %d, %d cores @ %d MHz\n", ESP.getChipModel(), ESP.getChipRevision(), ESP.getChipCores(), ESP.getCpuFreqMHz());
  Serial.printf("flash: %u MB, psram: %u MB (free %u KB)\n", ESP.getFlashChipSize() >> 20, ESP.getPsramSize() >> 20, ESP.getFreePsram() >> 10);

  for (size_t i = 0; i < sizeof(INPUTS) / sizeof(INPUTS[0]); i++) {
    bool pu = false;
    for (int p : PULLUP_OK) if (p == INPUTS[i]) pu = true;
    pinMode(INPUTS[i], pu ? INPUT_PULLUP : INPUT);
    lastLevel[i] = digitalRead(INPUTS[i]);
    Serial.printf("  GPIO%-2d idle=%d\n", INPUTS[i], lastLevel[i]);
  }

  Wire.begin(PIN_SDA, PIN_SCL, 400000);
  i2cScan();

  pinMode(PIN_BL, OUTPUT);
  analogWrite(PIN_BL, 200);                       // backlight on
  pinMode(PIN_CS, OUTPUT); digitalWrite(PIN_CS, HIGH);
  spi.begin(PIN_SCLK, -1, PIN_MOSI, PIN_CS);
  spi.beginTransaction(SPISettings(40000000, MSBFIRST, SPI_MODE0));
  Serial.println("Cycling DC/RST candidates (GPIO8/17 only). Report the bar count you see on screen.");
}

// Optional, user-triggered: is GPIO4 the amp enable or the power kill?
static void ampOrKillTest() {
  Serial.println("BOOT pressed: pulsing GPIO4 HIGH for 200 ms. If the board dies, GPIO4 = power kill.");
  Serial.flush();
  delay(50);
  pinMode(4, OUTPUT); digitalWrite(4, HIGH); delay(200); digitalWrite(4, LOW); pinMode(4, INPUT);
  Serial.println("...still alive: GPIO4 is the amp enable, GPIO2 is the power kill.");
}

void loop() {
  static const uint16_t COLORS[] = {0xF800, 0x07E0, 0x001F, 0xFFE0, 0xF81F, 0x07FF, 0xFD20, 0x8010};
  int combo = 0;
  for (int di = 0; di < NCAND; di++) {
    for (int ri = -1; ri < NCAND; ri++) {
      if (ri == di) continue;
      int dc = CANDS[di], rst = ri < 0 ? -1 : CANDS[ri];
      combo++;
      // release the previous DC/RST lines so they don't fight the next combo
      for (int c : CANDS) pinMode(c, INPUT);
      Serial.printf("combo %2d: DC=GPIO%-2d RST=%s -> %d white bars\n", combo, dc, rst < 0 ? "none" : String("GPIO" + String(rst)).c_str(), combo);
      lcdInit(dc, rst);
      fillRect(0, 0, W, H, COLORS[combo % 8]);
      for (int b = 0; b < combo; b++) fillRect(20, 12 + b * 16, W - 40, 8, 0xFFFF);
      uint32_t t0 = millis();
      while (millis() - t0 < 4000) {
        pollInputs();
        if (digitalRead(0) == LOW) { ampOrKillTest(); while (digitalRead(0) == LOW) delay(10); }
        delay(10);
      }
    }
  }
}
