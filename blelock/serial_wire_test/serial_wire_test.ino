// serial_wire_test.ino — proves CP2102 <-> ESP32-C6 Serial1 link (Tuya bus)
// Wiring:  ESP32 GPIO16 (TX) -> CP2102 RXD
//          ESP32 GPIO17 (RX) -> CP2102 TXD
//          ESP32 GND         -> CP2102 GND   (no VCC)
// Observe on laptop:  screen /dev/cu.SLAB_USBtoUART 9600   (exit: Ctrl-A K y)
// Keep the Arduino Serial Monitor on the usbmodem port @115200 for the debug side.

#define TUYA_TX_PIN 16  // wired to CP2102 RXD
#define TUYA_RX_PIN 17  // wired to CP2102 TXD

void setup() {
  Serial.begin(115200);                                       // USB-JTAG console
  Serial1.begin(9600, SERIAL_8N1, TUYA_RX_PIN, TUYA_TX_PIN);  // Tuya bus (rx, tx)
  Serial.println("[TEST] Serial1 up @ 9600 8N1 on GPIO16(TX)/GPIO17(RX)");
}

uint32_t last = 0;
void loop() {
  if (millis() - last > 1000) {          // heartbeat OUT the wire
    last = millis();
    Serial1.println("HELLO-FROM-C6");
    Serial.println("[TX1->] HELLO-FROM-C6");
  }
  while (Serial1.available()) {           // echo anything coming IN
    char c = Serial1.read();
    Serial.printf("[RX1<-] 0x%02X %c\n", (uint8_t)c, c);
  }
}
