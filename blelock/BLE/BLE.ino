#include <Arduino_GFX_Library.h>
#include <Wire.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>

// --- Color Palette ---
#define BLACK   0x0000
#define WHITE   0xFFFF
#define RED     0xF800
#define BLUE    0x001F  

// --- Hardware Pins ---
#define LCD_DC   15
#define LCD_CS   14
#define LCD_SCK  1   
#define LCD_DIN  2   
#define LCD_RST  22  
#define LCD_BL   23  

Arduino_DataBus *bus = new Arduino_HWSPI(LCD_DC, LCD_CS, LCD_SCK, LCD_DIN);
Arduino_GFX *gfx = new Arduino_ST7789(bus, LCD_RST, 0, true, 172, 320, 34, 0, 34, 0);

#define SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"

BLEServer *pServer = NULL;
bool deviceConnected = false;
bool lastDeviceConnected = false;
bool lockStateChanged = true; 
String lockStatusString = "LOCKED";
bool wrongCodeEntered = false;

unsigned long unlockTimeMarker = 0;       
const unsigned long lockDelayDuration = 5000; // 5 seconds

void drawUI(String status, uint16_t global_color) {
  gfx->fillScreen(BLACK);
  gfx->drawRect(0, 0, 320, 172, global_color); // Frame matches the system state color
  
  // Header - Now uses the clean uniform state color instead of yellow!
  gfx->setTextColor(global_color);
  gfx->setTextSize(1);
  gfx->setCursor(15, 12);
  gfx->println("OZLOCK SMART EMULATOR [BLE ACTIVE]");
  
  // Connection State Info
  gfx->setCursor(15, 30);
  if (deviceConnected) {
    gfx->print("App State: CONNECTED");
  } else {
    gfx->print("App State: ADVERTISING...");
  }

  // Giant Center Status Indicator Text Layout Alignment
  if (status.length() > 8) {
    gfx->setTextSize(3);
    gfx->setCursor(35, 85);
  } else {
    gfx->setTextSize(4);
    gfx->setCursor(60, 80);
  }
  gfx->println(status);
}

class MyServerCallbacks: public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) {
      Serial.println("[BLE Event] Connection handshake initiated!");
      deviceConnected = true;
    };
    void onDisconnect(BLEServer* pServer) {
      Serial.println("[BLE Event] Connection dropped.");
      deviceConnected = false;
    }
};

class LockControlCallbacks: public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pCharacteristic) {
      uint8_t* rawData = pCharacteristic->getData();
      size_t len = pCharacteristic->getLength();

      if (len > 0) {
        Serial.print("[GATT Write] Received raw Hex stream: ");
        for (size_t i = 0; i < len; i++) {
          Serial.print("0x");
          if (rawData[i] < 16) Serial.print("0");
          Serial.print(rawData[i], HEX);
          Serial.print(" ");
        }
        Serial.println();

        bool matched = false;

        if (len == 2 && rawData[0] == 0x12 && rawData[1] == 0x34) {
          matched = true;
        }
        else if (len == 1 && rawData[0] == 0x04) {
          matched = true;
        }
        else {
          String textVal = String((char*)rawData).substring(0, len);
          textVal.trim();
          if (textVal == "1234" || textVal == "OPEN" || textVal == "4") {
            matched = true;
          }
        }

        if (matched) {
          Serial.println("[Access Auth] Match successful! Unlocking...");
          lockStatusString = "UNLOCKED";
          lockStateChanged = true;
          wrongCodeEntered = false;
          unlockTimeMarker = millis(); 
        } else {
          Serial.println("[Access Auth] Match failed. Invalid key sequence.");
          wrongCodeEntered = true;
          lockStateChanged = true;
        }
      }
    }
};

void setup() {
  Serial.begin(115200);
  while (!Serial) { delay(10); }

  Serial.println("\n*** INITIALIZING RED/BLUE BLE SERVER: OZLOCK ***");

  pinMode(LCD_BL, OUTPUT);
  digitalWrite(LCD_BL, HIGH); 
  gfx->begin();
  gfx->setRotation(5); 
  drawUI("LOCKED", RED); // Boots up into 100% pure RED layout

  BLEDevice::init("OZLOCK"); 
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  BLEService *pService = pServer->createService(SERVICE_UUID);
  BLECharacteristic *pCharacteristic = pService->createCharacteristic(
                                         CHARACTERISTIC_UUID,
                                         BLECharacteristic::PROPERTY_READ |
                                         BLECharacteristic::PROPERTY_WRITE
                                       );

  pCharacteristic->setCallbacks(new LockControlCallbacks());
  pCharacteristic->setValue("LOCKED"); 
  pService->start();

  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  
  BLEDevice::startAdvertising();
  Serial.println("[BLE System] Device 'OZLOCK' is actively broadcasting profiles.");
}

void loop() {
  if (pServer != NULL) {
    uint32_t activeConnections = pServer->getConnectedCount();
    deviceConnected = (activeConnections > 0);
  }

  if (deviceConnected != lastDeviceConnected) {
    lockStateChanged = true; 
    if (!deviceConnected) {
      delay(500); 
      BLEDevice::startAdvertising();
    }
    lastDeviceConnected = deviceConnected;
  }

  // --- AUTOMATIC TIMED RELOCK ENGINE ---
  if (lockStatusString == "UNLOCKED") {
    if (millis() - unlockTimeMarker >= lockDelayDuration) {
      Serial.println("[Timer Event] 5 Seconds elapsed. Secure auto-relock engaging...");
      lockStatusString = "LOCKED";
      lockStateChanged = true;
    }
  }

  // Update screen interface canvas layouts dynamically
  if (lockStateChanged) {
    if (wrongCodeEntered) {
      drawUI("DENIED!", RED);
      delay(1500); 
      wrongCodeEntered = false;
      drawUI(lockStatusString, (lockStatusString == "UNLOCKED") ? BLUE : RED);
    } 
    else if (lockStatusString == "UNLOCKED") {
      drawUI("UNLOCKED", BLUE); // Everything turns BLUE
    } 
    else {
      drawUI("LOCKED", RED);   // Everything turns RED
    }
    lockStateChanged = false;
  }
  
  delay(20); 
}
