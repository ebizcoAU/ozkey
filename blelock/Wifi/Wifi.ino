#include <Arduino_GFX.h>
#include <display/Arduino_ST7789.h>
#include <databus/Arduino_HWSPI.h>
#include <Wire.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include <WiFi.h> 

// --- BGR-Corrected Hardware Color Definitions ---
#define REAL_BLACK   0x0000
#define REAL_WHITE   0xFFFF
#define REAL_RED     0x001F  // Sends bits to the Red channel on a BGR panel
#define REAL_BLUE    0xF800  // Sends bits to the Blue channel on a BGR panel

// --- Hardware Pins ---
#define LCD_DC   15
#define LCD_CS   14
#define LCD_SCK  1   
#define LCD_DIN  2   
#define LCD_RST  22  
#define LCD_BL   23  

// Corrected display initializer (IPS set to false)
Arduino_DataBus *bus = new Arduino_HWSPI(LCD_DC, LCD_CS, LCD_SCK, LCD_DIN);
Arduino_GFX *gfx = new Arduino_ST7789(bus, LCD_RST, 0, false, 172, 320, 34, 0, 34, 0);

// --- Wi-Fi Credentials ---
const char* ssid = "PHAN";
const char* password = "fatfamily55";

// --- BLE GATT UUID Identifiers ---
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
  gfx->fillScreen(REAL_BLACK);
  gfx->drawRect(0, 0, 320, 172, global_color); 
  
  // 1. Header Line
  gfx->setTextColor(global_color, REAL_BLACK);
  gfx->setTextSize(1);
  gfx->setCursor(15, 12);
  gfx->println("OZLOCK SMART EMULATOR [ACTIVE]");
  
  // 2. BLE Status Line
  gfx->setCursor(15, 26);
  if (deviceConnected) {
    gfx->print("BLE: CONNECTED");
  } else {
    gfx->print("BLE: ADVERTISING...");
  }

  // 3. WiFi Status Line
  gfx->setCursor(15, 40);
  if (WiFi.status() == WL_CONNECTED) {
    gfx->print("WiFi: CONNECTED | IP: ");
    gfx->print(WiFi.localIP()); 
  } else {
    gfx->print("WiFi: CONNECTING...");
  }

  // 4. Status Indicator Word
  if (status.length() > 8) {
    gfx->setTextSize(3);
    gfx->setCursor(35, 95);
  } else {
    gfx->setTextSize(4);
    gfx->setCursor(60, 90);
  }
  gfx->println(status); 
}

class MyServerCallbacks: public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) {
      deviceConnected = true;
    };
    void onDisconnect(BLEServer* pServer) {
      deviceConnected = false;
    }
};

class LockControlCallbacks: public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pCharacteristic) {
      uint8_t* rawData = pCharacteristic->getData();
      size_t len = pCharacteristic->getLength();

      if (len > 0) {
        bool matched = false;

        // Check for 2 bytes: 0x12 0x34
        if (len == 2 && rawData[0] == 0x12 && rawData[1] == 0x34) {
          matched = true;
        }
        // Check for 1 byte: 0x04
        else if (len == 1 && rawData[0] == 0x04) {
          matched = true;
        }

        if (matched) {
          lockStatusString = "UNLOCKED";
          lockStateChanged = true;
          wrongCodeEntered = false;
          unlockTimeMarker = millis(); 
        } else {
          wrongCodeEntered = true;
          lockStateChanged = true;
        }
      }
    }
};

void setup() {
  Serial.begin(115200);
  
  pinMode(LCD_BL, OUTPUT);
  digitalWrite(LCD_BL, HIGH); 
  gfx->begin();
  gfx->setRotation(5); 
  drawUI("LOCKED", REAL_RED); // Strictly RED at startup

  WiFi.begin(ssid, password);

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

  static wl_status_t lastWiFiStatus = WL_IDLE_STATUS;
  wl_status_t currentWiFiStatus = WiFi.status();
  if (currentWiFiStatus != lastWiFiStatus) {
    lockStateChanged = true; 
    lastWiFiStatus = currentWiFiStatus;
  }

  if (lockStatusString == "UNLOCKED") {
    if (millis() - unlockTimeMarker >= lockDelayDuration) {
      lockStatusString = "LOCKED";
      lockStateChanged = true;
    }
  }

  if (lockStateChanged) {
    if (wrongCodeEntered) {
      drawUI("DENIED!", REAL_RED);
      delay(1500); 
      wrongCodeEntered = false;
      drawUI(lockStatusString, (lockStatusString == "UNLOCKED") ? REAL_BLUE : REAL_RED);
    } 
    else if (lockStatusString == "UNLOCKED") {
      drawUI("UNLOCKED", REAL_BLUE); // STRICTLY BLUE text
    } 
    else {
      drawUI("LOCKED", REAL_RED);   // STRICTLY RED text
    }
    lockStateChanged = false;
  }
  
  delay(20); 
}
