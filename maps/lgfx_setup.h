#pragma once
// LovyanGFX device for the Cheeko Gotchi V2: JD9853 240x296 over SPI2.
// Touch (CST810) is read via Wire in touch_cst810.h so the I2C bus can be shared with the codecs.
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include "board_config.h"

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_JD9853  _panel;
  lgfx::Bus_SPI       _bus;
  lgfx::Light_PWM     _light;

public:
  LGFX() {
    {
      auto cfg = _bus.config();
      cfg.spi_host    = SPI2_HOST;
      cfg.spi_mode    = 0;
      cfg.freq_write  = 40000000;
      cfg.freq_read   = 16000000;
      cfg.spi_3wire   = true;      // no MISO
      cfg.use_lock    = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk    = PIN_LCD_SCLK;
      cfg.pin_mosi    = PIN_LCD_MOSI;
      cfg.pin_miso    = -1;
      cfg.pin_dc      = PIN_LCD_DC;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    {
      auto cfg = _panel.config();
      cfg.pin_cs        = PIN_LCD_CS;
      cfg.pin_rst       = PIN_LCD_RST;
      cfg.pin_busy      = -1;
      cfg.memory_width  = 240;
      cfg.memory_height = LCD_H;   // 296: with 320 the 180-degree rotation lands outside the visible GRAM (black screen)
      cfg.panel_width   = LCD_W;
      cfg.panel_height  = LCD_H;
      cfg.offset_x      = 0;
      cfg.offset_y      = 0;
      cfg.offset_rotation = 0;
      cfg.readable      = false;
      cfg.invert        = false;   // panel is not inverted (with true: RED came out yellow etc.)
      cfg.rgb_order     = true;    // panel wants BGR
      cfg.dlen_16bit    = false;
      cfg.bus_shared    = false;
      _panel.config(cfg);
    }
    {
      auto cfg = _light.config();
      cfg.pin_bl      = PIN_LCD_BL;
      cfg.invert      = false;
      cfg.freq        = 12000;
      cfg.pwm_channel = 7;
      _light.config(cfg);
      _panel.setLight(&_light);
    }
    setPanel(&_panel);
  }
};
