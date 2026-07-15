#include <Arduino_GFX_Library.h>
#include <Wire.h>

// --- Color Palette ---
#define BLACK   0x0000
#define WHITE   0xFFFF
#define CYAN    0x07FF
#define YELLOW  0xFFE0
#define GREEN   0x07E0

// --- Official Waveshare 1.47" Factory Pin Layout ---
#define LCD_DC   15
#define LCD_CS   14
#define LCD_SCK  1    
#define LCD_DIN  2    
#define LCD_RST  22   
#define LCD_BL   23   

// --- TRUE Hardware Touch Connections ---
#define I2C_SDA_PIN   18  
#define I2C_SCL_PIN   19  
#define TOUCH_RST     20  
#define TOUCH_INT     21  
#define TOUCH_I2C_ADDR 0x63 // Your batch's verified address node

Arduino_DataBus *bus = new Arduino_HWSPI(LCD_DC, LCD_CS, LCD_SCK, LCD_DIN);
Arduino_GFX *gfx = new Arduino_ST7789(bus, LCD_RST, 0, true, 172, 320, 34, 0, 34, 0);

unsigned long logTimer = 0;

void setup() {
  Serial.begin(115200);
  while (!Serial) { delay(10); } 
  
  Serial.println("\n*** INITIALIZING FIXED CST816 CANVAS ***");

  // 1. Boot Display Panel
  pinMode(LCD_BL, OUTPUT);
  digitalWrite(LCD_BL, HIGH); 
  gfx->begin();
  gfx->setRotation(5); // Mirror-corrected landscape alignment
  gfx->fillScreen(BLACK);
  
  // Draw Canvas Frame
  gfx->drawRect(0, 0, 320, 172, WHITE);
  gfx->setTextColor(YELLOW);
  gfx->setTextSize(1);
  gfx->setCursor(10, 8);
  gfx->println("FIXED CANVAS ACTIVE - DRAW NOW");

  // 2. Hardware Power Reset Touch Processor
  pinMode(TOUCH_INT, INPUT_PULLUP);
  pinMode(TOUCH_RST, OUTPUT);
  digitalWrite(TOUCH_RST, LOW);  
  delay(100);
  digitalWrite(TOUCH_RST, HIGH); 
  delay(200);

  // 3. Bind I2C Lines
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  delay(50);
  
  Serial.println("[System] Touch array running safely.");
}

void loop() {
  // Read coordinate bytes from the CST816 memory registers
  Wire.beginTransmission(TOUCH_I2C_ADDR);
  Wire.write(0x00); // Point to start of tracking registers
  if (Wire.endTransmission() == 0) {
    Wire.requestFrom(TOUCH_I2C_ADDR, 7); // Pull standard 7-byte CST data array
    
    if (Wire.available() >= 7) {
      uint8_t buffer[7]; // FIXED: Allocated proper array memory slot size to prevent crashes!
      for (int i = 0; i < 7; i++) {
        buffer[i] = Wire.read();
      }

      // CST816 Map: Register 0x02 contains the active touch point count
      uint8_t touchCount = buffer[2]; 
      
      if (touchCount > 0 && touchCount <= 5) {
        // CST816 Map: Parse coordinate points safely from array registers
        int rawX = ((buffer[3] & 0x0F) << 8) | buffer[4];
        int rawY = ((buffer[5] & 0x0F) << 8) | buffer[6];

        // Map vertical matrix traces to horizontal wide 320x172 screen geometry
        int touchX = 320 - rawY;
        int touchY = rawX;

        // Print real-time drawing logs directly to your Mac screen
        Serial.print("🎯 TOUCH CAPTURED! -> X: ");
        Serial.print(touchX);
        Serial.print(" | Y: ");
        Serial.println(touchY);

        // Render your drawing trace! Draws a solid 4x4 box under your stylus tip
        if (touchX >= 0 && touchX < 320 && touchY >= 20 && touchY < 172) {
          gfx->fillRect(touchX - 2, touchY - 2, 4, 4, CYAN);
        }
      }
    }
  }

  // Regular heartbeat status printer
  if (millis() - logTimer > 3000) {
    Serial.print("[Active Log] Listening on 0x63. Interrupt line state: ");
    Serial.println(digitalRead(TOUCH_INT));
    logTimer = millis();
  }
  
  delay(10); // Safe background execution padding
}
