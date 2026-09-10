#pragma once

#include <LovyanGFX.hpp>

// Board: ESP32-2432S028R, single micro-USB, ILI9341 + XPT2046.
// This is a board-specific configuration rather than relying on generic
// Adafruit_ILI9341 geometry. The CYD panel uses the 240 x 320 ILI9341
// memory map, with an offset rotation of 2 on this hardware family.

class TERRA_LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ILI9341 _panel;
  lgfx::Bus_SPI _bus;
  lgfx::Light_PWM _light;
  lgfx::Touch_XPT2046 _touch;

public:
  TERRA_LGFX() {
    {
      auto cfg = _bus.config();
      cfg.spi_host = HSPI_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = 27000000; // 27MHz is rock-solid for CYD ILI9341 panels (prevents SPI bit jitter)
      cfg.freq_read = 16000000;
      cfg.spi_3wire = false;
      cfg.use_lock = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = 14;
      cfg.pin_mosi = 13;
      cfg.pin_miso = 12;
      cfg.pin_dc   = 2;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }

    {
      auto cfg = _panel.config();
      cfg.pin_cs = 15;
      cfg.pin_rst = -1;       // CYD reset is tied to the ESP32 reset
      cfg.pin_busy = -1;

      cfg.panel_width = 240;
      cfg.panel_height = 320;
      cfg.memory_width = 240;
      cfg.memory_height = 320;

      cfg.offset_x = 0;
      cfg.offset_y = 0;
      cfg.offset_rotation = 0;

      cfg.dummy_read_pixel = 8;
      cfg.dummy_read_bits = 1;
      cfg.readable = false;
      cfg.invert = false;
      cfg.rgb_order = false;
      cfg.dlen_16bit = false;
      cfg.bus_shared = false;

      _panel.config(cfg);
    }

    {
      auto cfg = _light.config();
      cfg.pin_bl = 21;
      cfg.invert = false;
      cfg.freq = 12000;
      cfg.pwm_channel = 7;
      _light.config(cfg);
      _panel.setLight(&_light);
    }

    {
      auto cfg = _touch.config();
      cfg.x_min = 300;
      cfg.x_max = 3900;
      cfg.y_min = 3700;
      cfg.y_max = 200;
      cfg.pin_int = 36;
      cfg.bus_shared = false;
      cfg.offset_rotation = 0;
      cfg.spi_host = -1;      // XPT2046 manages its own SPI host here
      cfg.freq = 1000000;
      cfg.pin_sclk = 25;
      cfg.pin_mosi = 32;
      cfg.pin_miso = 39;
      cfg.pin_cs = 33;
      _touch.config(cfg);
      _panel.setTouch(&_touch);
    }

    setPanel(&_panel);
  }
};
