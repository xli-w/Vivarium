#pragma once
#include <Arduino.h>
#include <SPI.h>

class TS_Point {
public:
  int16_t x, y, z;
  TS_Point(void) : x(0), y(0), z(0) {}
  TS_Point(int16_t x, int16_t y, int16_t z) : x(x), y(y), z(z) {}
  bool operator==(TS_Point p) { return ((p.x == x) && (p.y == y) && (p.z == z)); }
  bool operator!=(TS_Point p) { return ((p.x != x) || (p.y != y) || (p.z != z)); }
};

class XPT2046_Touchscreen {
public:
  constexpr XPT2046_Touchscreen(uint8_t cspin, uint8_t tirq = 255)
      : csPin(cspin), tirqPin(tirq), rotation(0), isrWake(true),
        xraw(0), yraw(0), zraw(0), msraw(0x80000000), mySPI(nullptr) {}

  bool begin(SPIClass &spi = SPI);
  TS_Point getPoint();
  bool touched();
  void read_raw(int16_t *vi);
  bool tirqTouched();
  void setRotation(uint8_t n) { rotation = n % 4; }

private:
  void update();
  uint8_t csPin, tirqPin, rotation;
  bool isrWake;
  int16_t xraw, yraw, zraw;
  uint32_t msraw;
  SPIClass *mySPI;
};
