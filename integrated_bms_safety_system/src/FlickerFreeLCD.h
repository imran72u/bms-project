#ifndef FLICKER_FREE_LCD_H
#define FLICKER_FREE_LCD_H
#include <Arduino.h>
#include <LiquidCrystal_I2C.h>

class FlickerFreeLCD {
public:
  FlickerFreeLCD(uint8_t addr=0x27, uint8_t cols=20, uint8_t rows=4) : lcd(addr, cols, rows), cols(cols), rows(rows) {}
  void begin() { lcd.init(); lcd.backlight(); clearCache(); }
  void clearCache() { for (uint8_t r=0;r<4;r++) cache[r][0]='\\0'; }
  void render(const char* lines[4]) {
    for (uint8_t r=0;r<rows && r<4;r++) updateLine(r, lines[r] ? lines[r] : "");
  }
  void updateLine(uint8_t row, const char* text) {
    if (row>=rows || row>=4) return;
    char next[21];
    snprintf(next, sizeof(next), "%-20s", text ? text : "");
    next[cols < 20 ? cols : 20] = '\\0';
    if (strncmp(cache[row], next, sizeof(cache[row])) == 0) return;
    lcd.setCursor(0,row);
    lcd.print(next);
    strncpy(cache[row], next, sizeof(cache[row])-1);
    cache[row][sizeof(cache[row])-1]='\\0';
  }
  LiquidCrystal_I2C& raw() { return lcd; }
private:
  LiquidCrystal_I2C lcd;
  uint8_t cols, rows;
  char cache[4][21]{};
};
#endif
