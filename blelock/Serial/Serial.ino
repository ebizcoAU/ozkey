void setup() {
  Serial.begin(115200);
  
  // Wait indefinitely until the Serial Monitor is actually opened on your laptop
  while (!Serial) { 
    delay(10); 
  }
  
  Serial.println("\n=================================");
  Serial.println("  SERIAL PORT IS FINALLY ALIVE!  ");
  Serial.println("=================================");
}

void loop() {
  Serial.println("[Heartbeat] ESP32-C6 is communicating fine...");
  delay(1000);
}
