// SSD1306 driver (128x64 RAM, 72x40 window) for the 0.42" ESP32-C3 board. I2C: SDA=GPIO5, SCL=GPIO6.
#pragma once
#include "canvas.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"

#ifndef OLED_SDA
#define OLED_SDA 5   // override with -DOLED_SDA=8 -DOLED_SCL=9 if your board variant differs
#endif
#ifndef OLED_SCL
#define OLED_SCL 6
#endif

class Oled : public Canvas {
 public:
  bool begin(int sda = OLED_SDA, int scl = OLED_SCL) {
    i2c_master_bus_config_t bc = {};
    bc.i2c_port = I2C_NUM_0;
    bc.sda_io_num = (gpio_num_t)sda;
    bc.scl_io_num = (gpio_num_t)scl;
    bc.clk_source = I2C_CLK_SRC_DEFAULT;
    bc.glitch_ignore_cnt = 7;
    bc.flags.enable_internal_pullup = true;
    if (i2c_new_master_bus(&bc, &bus_) != ESP_OK) return false;
    i2c_device_config_t dc = {};
    dc.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dc.device_address = 0x3C;
    dc.scl_speed_hz = 400000;
    if (i2c_master_bus_add_device(bus_, &dc, &dev_) != ESP_OK) return false;
    static const uint8_t init[] = {0xAE, 0xD5, 0x80, 0xA8, 0x3F, 0xD3, 0x00, 0x40, 0x8D, 0x14, 0x20, 0x00,
                                   0xDA, 0x12, 0x81, 0xCF, 0xD9, 0xF1, 0xDB, 0x40, 0xA4, 0xA6};
    for (uint8_t c : init) cmd(c);
    set_flip(false);
    cmd(0xAF);
    clear();
    flush();
    return true;
  }
  // rotate the picture 180 degrees (segment remap + COM scan direction)
  void set_flip(bool f) {
    cmd(f ? 0xA0 : 0xA1);
    cmd(f ? 0xC0 : 0xC8);
  }
  // send only the part of the RAM that the 72x40 window covers
  void flush() {
    if (!dev_) return;
    const int c0 = xoff < 0 ? 0 : xoff, c1 = xoff + OLED_W - 1 > 127 ? 127 : xoff + OLED_W - 1;
    const int p0 = yoff >> 3, p1 = (yoff + OLED_H - 1) >> 3;
    const uint8_t addr[] = {0x00, 0x21, (uint8_t)c0, (uint8_t)c1, 0x22, (uint8_t)p0, (uint8_t)p1};
    i2c_master_transmit(dev_, addr, sizeof addr, 100);
    uint8_t line[130];
    line[0] = 0x40;
    for (int p = p0; p <= p1; p++) {
      memcpy(line + 1, fb_ + p * 128 + c0, c1 - c0 + 1);
      i2c_master_transmit(dev_, line, c1 - c0 + 2, 100);
    }
  }

 private:
  void cmd(uint8_t c) {
    if (!dev_) return;
    uint8_t b[2] = {0x00, c};
    i2c_master_transmit(dev_, b, 2, 100);
  }
  i2c_master_bus_handle_t bus_ = nullptr;
  i2c_master_dev_handle_t dev_ = nullptr;
};
