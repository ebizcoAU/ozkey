#include <Arduino_GFX_Library.h>

// --- Manual Color Definitions (RGB565 Hex Codes) ---
#define BLACK   0x0000
#define BLUE    0x001F
#define RED     0xF800
#define GREEN   0x07E0
#define CYAN    0x07FF
#define MAGENTA 0xF81F
#define YELLOW  0xFFE0
#define WHITE   0xFFFF

// Correct Waveshare 1.47" Touch Edition Hardware Pin Traces
#define LCD_DC   15
#define LCD_CS   14
#define LCD_SCK  1    // Real trace pin
#define LCD_DIN  2    // Real trace pin
#define LCD_RST  22   // Real trace pin
#define LCD_BL   23   // Real trace pin

// Initialize the 1.47" Bus and Driver with its exact 172x320 alignment offsets
Arduino_DataBus *bus = new Arduino_HWSPI(LCD_DC, LCD_CS, LCD_SCK, LCD_DIN);
Arduino_GFX *gfx = new Arduino_ST7789(
  bus, LCD_RST, 0 /* rotation */, false /* IPS */, 
  172 /* width */, 320 /* height */, 
  34 /* col offset 1 */, 0 /* row offset 1 */, 
  34 /* col offset 2 */, 0 /* row offset 2 */
);

void setup() {
  Serial.begin(115200);
  
  // Power up the backlight panel using the correct GPIO 23 trace
  pinMode(LCD_BL, OUTPUT);
  digitalWrite(LCD_BL, HIGH); 

  // Initialize the GFX engine
  if (!gfx->begin()) {
    Serial.println("Display Initialization Failed!");
    return;
  }

  // Draw Background and Text
  gfx->fillScreen(BLACK);
  gfx->setRotation(5); // Set to horizontal landscape mode
  
  gfx->setCursor(15, 40);
  gfx->setTextColor(CYAN);
  gfx->setTextSize(3);
  gfx->println("ESP32-C6");

  gfx->setCursor(15, 80);
  gfx->setTextColor(WHITE);
  gfx->setTextSize(2);
  gfx->println("1.47 INCH ALIVE!");
}

void loop() {
  // Static screen display loop
}
