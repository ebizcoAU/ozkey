#include <Arduino_GFX.h>
#include <display/Arduino_ST7789.h>
#include <databus/Arduino_HWSPI.h>

// --- Hardware Pins ---
#define LCD_DC   15
#define LCD_CS   14
#define LCD_SCK  1   
#define LCD_DIN  2   
#define LCD_RST  22  
#define LCD_BL   23  

// --- BGR-Corrected Hardware Color Definitions ---
#define REAL_BLACK   0x0000
#define REAL_WHITE   0xFFFF
#define REAL_RED     0x001F  // Sends bits to the Red channel on a BGR panel
#define REAL_BLUE    0xF800  // Sends bits to the Blue channel on a BGR panel
#define REAL_GREEN   0x07E0  

Arduino_DataBus *bus = new Arduino_HWSPI(LCD_DC, LCD_CS, LCD_SCK, LCD_DIN);

// FIX: Changed the 4th parameter from 'true' to 'false' to disable color inversion
Arduino_GFX *gfx = new Arduino_ST7789(
  bus, LCD_RST, 0 /* rotation */, false /* IPS set to FALSE */, 
  172 /* width */, 320 /* height */, 
  34 /* col offset 1 */, 0 /* row offset 1 */, 
  34 /* col offset 2 */, 0 /* row offset 2 */
);

void setup() {
  Serial.begin(115200);
  while (!Serial) { delay(10); }

  Serial.println("\n=== UN-INVERTED BGR COLOR CORRECTION TEST ===");

  pinMode(LCD_BL, OUTPUT);
  digitalWrite(LCD_BL, HIGH); 
  gfx->begin();
  gfx->setRotation(5); 
}

void loop() {
  // Paint Background Black, Draw a border box, and render un-inverted text
  gfx->fillScreen(REAL_BLACK);
  gfx->drawRect(0, 0, 320, 172, REAL_WHITE);

  // 1. Test Red Channel
  gfx->setTextColor(REAL_RED, REAL_BLACK);
  gfx->setTextSize(3);
  gfx->setCursor(20, 20);
  gfx->println("TRUE RED TEXT");

  // 2. Test Blue Channel
  gfx->setTextColor(REAL_BLUE, REAL_BLACK);
  gfx->setTextSize(3);
  gfx->setCursor(20, 70);
  gfx->println("TRUE BLUE TEXT");

  // 3. Test White Channel
  gfx->setTextColor(REAL_WHITE, REAL_BLACK);
  gfx->setTextSize(3);
  gfx->setCursor(20, 120);
  gfx->println("TRUE WHITE TEXT");

  delay(5000); 
}
