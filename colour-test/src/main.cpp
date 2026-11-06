// CYD colour correction: tap the buttons until every tile matches its label.
#include <Arduino.h>
#include <SPI.h>
#include <XPT2046_Touchscreen.h>
#include <TFT_eSPI.h>

#define TS_X0 200
#define TS_X1 3700
#define TS_Y0 240
#define TS_Y1 3800

TFT_eSPI tft;
SPIClass touchSPI(HSPI);
XPT2046_Touchscreen ts(33, 36);
bool bgr = false, inv = false;

static void applyMode() {
  tft.writecommand(0x36);                       // MADCTL: landscape (MV) + colour order bit
  tft.writedata(0x20 | (bgr ? 0x08 : 0x00));
  tft.invertDisplay(inv);
}

struct Tile { const char *name; uint8_t r, g, b; uint16_t text; };
static const Tile TILES[6] = {
  {"RED", 255, 0, 0, TFT_WHITE},      {"GREEN", 0, 255, 0, TFT_BLACK},   {"BLUE", 0, 0, 255, TFT_WHITE},
  {"YELLOW", 255, 255, 0, TFT_BLACK}, {"ORANGE", 255, 128, 0, TFT_BLACK}, {"WHITE", 255, 255, 255, TFT_BLACK}};

static void draw() {
  applyMode();
  tft.fillScreen(TFT_BLACK);
  tft.setTextSize(2);
  for (int i = 0; i < 6; i++) {
    int x = (i % 3) * 107, y = (i / 3) * 58;
    uint16_t c = tft.color565(TILES[i].r, TILES[i].g, TILES[i].b);
    tft.fillRect(x, y, 106, 57, c);
    tft.setTextColor(TILES[i].text, c);
    tft.setCursor(x + (106 - (int)strlen(TILES[i].name) * 12) / 2, y + 21);
    tft.print(TILES[i].name);
  }
  for (int i = 0; i < 16; i++) {                       // grey ramp: should run smoothly black -> white
    uint8_t g = i * 17;
    tft.fillRect(i * 20, 118, 20, 14, tft.color565(g, g, g));
  }
  tft.setTextSize(1); tft.setTextColor(TFT_WHITE, TFT_BLACK);
  String flags = String(bgr ? "-DTFT_RGB_ORDER=TFT_BGR " : "") + (inv ? "-DTFT_INVERSION_ON" : "");
  if (!flags.length()) flags = "(no extra flags needed)";
  tft.setCursor(4, 138); tft.print("Match every tile to its name. Flags to use:");
  tft.setCursor(4, 150); tft.setTextColor(TFT_YELLOW, TFT_BLACK); tft.print(flags);
  Serial.println("build_flags: " + flags);
  const char *lab[2] = {bgr ? "R/B swap: ON" : "R/B swap: off", inv ? "Invert: ON" : "Invert: off"};
  tft.setTextSize(2);
  for (int i = 0; i < 2; i++) {
    int x = 6 + i * 160;
    tft.fillRoundRect(x, 176, 148, 58, 6, 0x31A6);
    tft.setTextColor(TFT_WHITE, 0x31A6);
    tft.setCursor(x + (148 - (int)strlen(lab[i]) * 12) / 2, 197);
    tft.print(lab[i]);
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(TFT_BL, OUTPUT); digitalWrite(TFT_BL, HIGH);
  tft.init(); tft.setRotation(1);
  touchSPI.begin(25, 39, 32, 33);
  ts.begin(touchSPI); ts.setRotation(1);
  draw();
}

void loop() {
  static bool was = false; static uint32_t last = 0;
  bool down = ts.tirqTouched() && ts.touched();
  if (down && !was && millis() - last > 250) {
    last = millis();
    TS_Point p = ts.getPoint();
    int tx = constrain(map(p.x, TS_X0, TS_X1, 0, 320), 0, 319);
    int ty = constrain(map(p.y, TS_Y0, TS_Y1, 0, 240), 0, 239);
    if (ty >= 176) { if (tx < 160) bgr = !bgr; else inv = !inv; draw(); }
  }
  was = down;
}
