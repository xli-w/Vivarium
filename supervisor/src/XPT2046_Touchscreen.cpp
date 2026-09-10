#include "XPT2046_Touchscreen.h"

#define Z_THRESHOLD     350
#define SPI_SETTING     SPISettings(2000000, MSBFIRST, SPI_MODE0)

bool XPT2046_Touchscreen::begin(SPIClass &spi) {
  mySPI = &spi;
  pinMode(csPin, OUTPUT);
  digitalWrite(csPin, HIGH);
  if (255 != tirqPin) {
    pinMode(tirqPin, INPUT);
  }
  return true;
}

bool XPT2046_Touchscreen::tirqTouched() {
  if (255 != tirqPin) {
    return (digitalRead(tirqPin) == LOW);
  }
  return true;
}

bool XPT2046_Touchscreen::touched() {
  update();
  return (zraw >= Z_THRESHOLD);
}

TS_Point XPT2046_Touchscreen::getPoint() {
  update();
  int16_t x = xraw;
  int16_t y = yraw;

  switch (rotation) {
    case 1:
      x = 4095 - yraw;
      y = xraw;
      break;
    case 2:
      x = 4095 - xraw;
      y = 4095 - yraw;
      break;
    case 3:
      x = yraw;
      y = 4095 - xraw;
      break;
    default:
      break;
  }

  return TS_Point(x, y, zraw);
}

void XPT2046_Touchscreen::read_raw(int16_t *vi) {
  update();
  vi[0] = xraw;
  vi[1] = yraw;
  vi[2] = zraw;
}

static int16_t besttwoofthree(int16_t a, int16_t b, int16_t c) {
  int16_t da = a > b ? a - b : b - a;
  int16_t db = a > c ? a - c : c - a;
  int16_t dc = b > c ? b - c : c - b;

  if (da <= db && da <= dc) return (a + b) / 2;
  if (db <= da && db <= dc) return (a + c) / 2;
  return (b + c) / 2;
}

void XPT2046_Touchscreen::update() {
  uint32_t now = millis();
  if (now - msraw < 4) return; // 250Hz max sample rate
  msraw = now;

  mySPI->beginTransaction(SPI_SETTING);
  digitalWrite(csPin, LOW);

  // Measure Z1
  mySPI->transfer(0xB1);
  int16_t z1 = mySPI->transfer16(0xC1) >> 3; // Read Z1, request Z2
  int16_t z2 = mySPI->transfer16(0x91) >> 3; // Read Z2, request Y
  int16_t z = z1 + 4095 - z2;
  if (z < 0) z = 0;

  if (z >= Z_THRESHOLD) {
    int16_t y1 = mySPI->transfer16(0xD1) >> 3; // Read Y1, request X
    int16_t x1 = mySPI->transfer16(0x91) >> 3; // Read X1, request Y
    int16_t y2 = mySPI->transfer16(0xD1) >> 3; // Read Y2, request X
    int16_t x2 = mySPI->transfer16(0x91) >> 3; // Read X2, request Y
    int16_t y3 = mySPI->transfer16(0xD0) >> 3; // Read Y3, request X (power down)
    int16_t x3 = mySPI->transfer16(0x00) >> 3; // Read X3

    digitalWrite(csPin, HIGH);
    mySPI->endTransaction();

    xraw = besttwoofthree(x1, x2, x3);
    yraw = besttwoofthree(y1, y2, y3);
    zraw = z;
  } else {
    digitalWrite(csPin, HIGH);
    mySPI->endTransaction();
    zraw = 0;
  }
}
