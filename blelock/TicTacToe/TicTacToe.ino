#include <Arduino_GFX_Library.h>
#include <Wire.h>

// --- Color Palette ---
#define BLACK   0x0000
#define WHITE   0xFFFF
#define CYAN    0x07FF
#define MAGENTA 0xF81F
#define YELLOW  0xFFE0

// --- Official Waveshare 1.47" Display Pins ---
#define LCD_DC   15
#define LCD_CS   14
#define LCD_SCK  1   
#define LCD_DIN  2   
#define LCD_RST  22  
#define LCD_BL   23  

// --- Verified Hardware Touch Traces ---
#define I2C_SDA_PIN   18  
#define I2C_SCL_PIN   19  
#define TOUCH_RST     20  
#define TOUCH_INT     21  
#define TOUCH_I2C_ADDR 0x63 

// Initialize Screen with Landscape Orientation 5 (Mirror Corrected)
Arduino_DataBus *bus = new Arduino_HWSPI(LCD_DC, LCD_CS, LCD_SCK, LCD_DIN);
Arduino_GFX *gfx = new Arduino_ST7789(bus, LCD_RST, 0, true, 172, 320, 34, 0, 34, 0);

// --- Game Logic Engine Matrices ---
int board[3][3] = { {0,0,0}, {0,0,0}, {0,0,0} }; // 0=Empty, 1=Human (X), 2=AI (O)
bool playerTurn = true;
bool gameOver = false;

// 1.47" Screen Geometry grid boundaries in landscape bounds
const int gridW = 320 / 3;
const int gridH = 172 / 3;

void drawGrid() {
  gfx->fillScreen(BLACK);
  // Draw Vertical Grids
  gfx->drawFastVLine(gridW, 0, 172, WHITE);
  gfx->drawFastVLine(gridW * 2, 0, 172, WHITE);
  // Draw Horizontal Grids
  gfx->drawFastHLine(0, gridH, 320, WHITE);
  gfx->drawFastHLine(0, gridH * 2, 320, WHITE);
}

void drawPieces() {
  for (int r = 0; r < 3; r++) {
    for (int c = 0; c < 3; c++) {
      int centerX = (c * gridW) + (gridW / 2);
      int centerY = (r * gridH) + (gridH / 2);
      
      if (board[r][c] == 1) { // Draw Cross X (Cyan)
        gfx->drawLine(centerX - 15, centerY - 15, centerX + 15, centerY + 15, CYAN);
        gfx->drawLine(centerX + 15, centerY - 15, centerX - 15, centerY + 15, CYAN);
      } else if (board[r][c] == 2) { // Draw Ring O (Magenta)
        gfx->drawCircle(centerX, centerY, 15, MAGENTA);
      }
    }
  }
}

int checkWin() {
  // Check Rows and Columns matching combinations
  for (int i = 0; i < 3; i++) {
    if (board[i][0] != 0 && board[i][0] == board[i][1] && board[i][0] == board[i][2]) return board[i][0];
    if (board[0][i] != 0 && board[0][i] == board[1][i] && board[0][i] == board[2][i]) return board[0][i];
  }
  // Check Diagonal sets
  if (board[0][0] != 0 && board[0][0] == board[1][1] && board[0][0] == board[2][2]) return board[0][0];
  if (board[0][2] != 0 && board[0][2] == board[1][1] && board[0][2] == board[2][0]) return board[0][2];
  
  // Verify remaining empty slots for potential Draw game
  bool openSpaces = false;
  for (int r = 0; r < 3; r++) {
    for (int c = 0; c < 3; c++) {
      if (board[r][c] == 0) openSpaces = true;
    }
  }
  if (!openSpaces) return 3; // 3 acts as the draw code signature
  return 0; // Game continues running
}

void handleEndGame(int outcome) {
  gameOver = true;
  delay(300); // Small pause to show final winning piece
  
  // Render notification overlay dialog container
  gfx->fillRect(40, 50, 240, 70, BLACK);
  gfx->drawRect(40, 50, 240, 70, WHITE);
  gfx->setTextSize(3);
  gfx->setCursor(60, 70);
  
  if (outcome == 1) {
    gfx->setTextColor(CYAN);
    gfx->println("YOU WIN!");
  } else if (outcome == 2) {
    gfx->setTextColor(MAGENTA);
    gfx->println("AI WINS!");
  } else {
    gfx->setTextColor(YELLOW);
    gfx->println("IT'S A DRAW");
  }
  
  delay(3000); // Keep message on screen for 3 seconds
  
  // Wipe internal backend game array structure clean
  for (int r = 0; r < 3; r++) {
    for (int c = 0; c < 3; c++) board[r][c] = 0;
  }
  gameOver = false;
  playerTurn = true;
  drawGrid();
}

void makeAIMove() {
  if (gameOver) return;
  // AI Logic Strategy: Scan rows and pick first available open box index
  for (int r = 0; r < 3; r++) {
    for (int c = 0; c < 3; c++) {
      if (board[r][c] == 0) {
        board[r][c] = 2; // AI claims the coordinate slot
        drawPieces();
        playerTurn = true;
        return;
      }
    }
  }
}

void setup() {
  Serial.begin(115200);
  while (!Serial) { delay(10); }
  
  Serial.println("\n*** CST816 CALIBRATED TIC-TAC-TOE BOOTUP ***");

  // 1. Kickstart display frame panel systems
  pinMode(LCD_BL, OUTPUT);
  digitalWrite(LCD_BL, HIGH); 
  gfx->begin();
  gfx->setRotation(5); 
  drawGrid();

  // 2. Hardware Power Reset local Touch Controller Chip
  pinMode(TOUCH_INT, INPUT_PULLUP);
  pinMode(TOUCH_RST, OUTPUT);
  digitalWrite(TOUCH_RST, LOW);  delay(100);
  digitalWrite(TOUCH_RST, HIGH); delay(200);

  // 3. Open True I2C Wire Communication channels
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  delay(50);
  Serial.println("[System] Play loop initialized successfully.");
}

void loop() {
  if (gameOver) return;

  // Process User Interaction touch actions if active turn sequence is valid
  if (playerTurn) {
    Wire.beginTransmission(TOUCH_I2C_ADDR);
    Wire.write(0x00);
    if (Wire.endTransmission() == 0) {
      Wire.requestFrom(TOUCH_I2C_ADDR, 7);
      
      if (Wire.available() >= 7) {
        uint8_t buffer[7];
        for (int i = 0; i < 7; i++) {
          buffer[i] = Wire.read();
        }

        uint8_t touchCount = buffer[2]; // CST816 touch log identifier mapping register index
        
        if (touchCount > 0 && touchCount <= 5) {
          int rawX = ((buffer[3] & 0x0F) << 8) | buffer[4];
          int rawY = ((buffer[5] & 0x0F) << 8) | buffer[6];

          // FIX: Mathematically invert the Y dimension to line up accurately with the LCD panel orientation
          int touchX = 320 - rawY;
          int touchY = 172 - rawX; // Swapped from touchY = rawX

          // Map the coordinate math dimensions directly to board box matrix spaces
          int clickedCol = touchX / gridW;
          int clickedRow = touchY / gridH;

          // Prevent boundaries exceptions overflow checks
          if (clickedRow >= 0 && clickedRow < 3 && clickedCol >= 0 && clickedCol < 3) {
            if (board[clickedRow][clickedCol] == 0) {
              Serial.print("Touch Corrected -> Row: "); Serial.print(clickedRow);
              Serial.print(" | Col: "); Serial.println(clickedCol);
              
              board[clickedRow][clickedCol] = 1; // Log user entry
              drawPieces();
              
              int checkState = checkWin();
              if (checkState != 0) {
                handleEndGame(checkState);
              } else {
                playerTurn = false; 
                delay(500); // Input bounce protection delay layout configuration
              }
            }
          }
        }
      }
    }
  }

  // Handle AI turn state shifts
  if (!playerTurn && !gameOver) {
    delay(600); // Small pause so the AI feels like it's taking time to "think"
    makeAIMove();
    int checkState = checkWin();
    if (checkState != 0) {
      handleEndGame(checkState);
    }
  }
  
  delay(10); // Standard thread scanning speed pad
}
