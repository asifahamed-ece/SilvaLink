#include <Arduino.h>
#include <SPI.h>
#include <LoRa.h>
#include <DHT.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

// ============================================================
//  SilvaLink Node A — Optimized Firmware
//  - Unique NODE_ID in every packet
//  - Structured protocol: NODE_ID|TYPE|PAYLOAD
//  - DHT22 read retry with fallback to last known good
//  - Post-SOS ACK listen window
//  - BLE notification on ACK receipt
//  - Periodic heartbeat
// ============================================================

// ---------------- Node Identity ----------------
#define NODE_ID "A"

// ---------------- LoRa Pins --------------------
#define LORA_MISO 19
#define LORA_MOSI 23
#define LORA_SCK  18
#define LORA_CS   5
#define LORA_RST  14
#define LORA_DIO0 2
#define LORA_FREQ 433E6

// ---------------- DHT22 Sensor -----------------
#define DHTPIN  4
#define DHTTYPE DHT22
DHT dht(DHTPIN, DHTTYPE);

#define DHT_RETRY_COUNT    3
#define DHT_RETRY_DELAY_MS 500

// ---------------- BLE Configuration ------------
#define SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"

BLECharacteristic *pCharacteristic = nullptr;
bool deviceConnected   = false;
bool prevConnected     = false;

// ---- SOS Queue (from BLE callback → main loop) ----
volatile bool pendingSOS       = false;
int           pendingEmergencyCode = 0;
String        pendingCustomMsg = "";

// ---- Last Known Good Readings ----
float lastGoodTemp = 0.0f;
float lastGoodHum  = 0.0f;
bool  hasGoodReading = false;

// ---- Timing ----
unsigned long lastEnvSend  = 0;
unsigned long lastHeartbeat = 0;
const unsigned long envInterval = 5000;   // Send env data every 5 seconds
const unsigned long hbInterval  = 30000;  // Heartbeat every 30 seconds

// ============================================================
//  BLE Callbacks
// ============================================================

class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) override {
    deviceConnected = true;
    Serial.println("[BLE] Phone connected");
  }

  void onDisconnect(BLEServer* pServer) override {
    deviceConnected = false;
    Serial.println("[BLE] Phone disconnected — restarting advertising");
  }
};

class CharacteristicCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pChar) override {
    std::string rxValue = pChar->getValue();
    if (rxValue.length() == 0) return;

    // Byte 0 = emergency hex code, rest = custom UTF-8 message
    pendingEmergencyCode = rxValue[0];

    if (rxValue.length() > 1) {
      pendingCustomMsg = String(rxValue.substr(1).c_str());
    } else {
      pendingCustomMsg = "";
    }

    // Flag for main loop to send safely (BLE callback runs on BLE task)
    pendingSOS = true;
  }
};

// ============================================================
//  LoRa Packet Builders
//  Protocol: NODE_ID|TYPE|PAYLOAD
// ============================================================

void sendEnvPacket(float temp, float hum) {
  // Format: A|ENV|25.3|60.1
  String packet = String(NODE_ID) + "|ENV|" + String(temp, 1) + "|" + String(hum, 1);
  LoRa.beginPacket();
  LoRa.print(packet);
  LoRa.endPacket();
  Serial.println("[LoRa TX] " + packet);
}

void sendSOSPacket(int emergencyCode, const String& customMsg) {
  // Format: A|SOS|01|Help needed at ridge
  char hexCode[3];
  snprintf(hexCode, sizeof(hexCode), "%02X", emergencyCode);

  String packet = String(NODE_ID) + "|SOS|" + String(hexCode);
  if (customMsg.length() > 0) {
    packet += "|" + customMsg;
  }

  LoRa.beginPacket();
  LoRa.print(packet);
  LoRa.endPacket();
  Serial.println("[LoRa TX] SOS: " + packet);
}

void sendHeartbeat() {
  // Format: A|HB
  String packet = String(NODE_ID) + "|HB";
  LoRa.beginPacket();
  LoRa.print(packet);
  LoRa.endPacket();
  Serial.println("[LoRa TX] Heartbeat");
}

// ============================================================
//  ACK Listener — Waits for admin acknowledgment after SOS
// ============================================================

bool listenForAck(unsigned long timeoutMs) {
  unsigned long start = millis();
  while (millis() - start < timeoutMs) {
    int packetSize = LoRa.parsePacket();
    if (packetSize) {
      String incoming = "";
      while (LoRa.available()) incoming += (char)LoRa.read();

      // Expected: ACK|A|SOS_RECEIVED
      if (incoming.startsWith("ACK|" + String(NODE_ID))) {
        Serial.println("[LoRa RX] ACK received from Admin!");
        return true;
      }
    }
    delay(10);
  }
  return false;
}

// ============================================================
//  DHT22 Read with Retry
// ============================================================

bool readDHT(float &outTemp, float &outHum) {
  for (int attempt = 0; attempt < DHT_RETRY_COUNT; attempt++) {
    float h = dht.readHumidity();
    float t = dht.readTemperature();

    if (!isnan(h) && !isnan(t) && t > -40.0f && t < 85.0f && h >= 0.0f && h <= 100.0f) {
      outTemp = t;
      outHum  = h;

      // Cache as last known good
      lastGoodTemp   = t;
      lastGoodHum    = h;
      hasGoodReading = true;
      return true;
    }

    if (attempt < DHT_RETRY_COUNT - 1) {
      Serial.printf("[DHT] Read failed, retry %d/%d...\n", attempt + 1, DHT_RETRY_COUNT);
      delay(DHT_RETRY_DELAY_MS);
    }
  }

  // All retries failed — use last known good if available
  if (hasGoodReading) {
    outTemp = lastGoodTemp;
    outHum  = lastGoodHum;
    Serial.println("[DHT] Using last known good reading");
    return true;
  }

  Serial.println("[DHT] All reads failed, no cached data");
  return false;
}

// ============================================================
//  Setup
// ============================================================

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n========================================");
  Serial.printf("  SilvaLink Node %s — Trekker Sensor v2.0\n", NODE_ID);
  Serial.println("========================================\n");

  // Initialize DHT22
  dht.begin();
  delay(1000);  // DHT22 needs warm-up time

  // Initialize LoRa
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_CS);
  LoRa.setPins(LORA_CS, LORA_RST, LORA_DIO0);
  if (!LoRa.begin(LORA_FREQ)) {
    Serial.println("[LoRa] Init FAILED! Check wiring.");
    while (1) { delay(1000); }
  }
  LoRa.setSpreadingFactor(7);
  Serial.println("[LoRa] Ready at 433MHz, SF7");

  // Initialize BLE
  BLEDevice::init("SilvaLink_Trekker_" NODE_ID);
  BLEServer* pServer = BLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());

  BLEService* pService = pServer->createService(SERVICE_UUID);
  pCharacteristic = pService->createCharacteristic(
    CHARACTERISTIC_UUID,
    BLECharacteristic::PROPERTY_READ   |
    BLECharacteristic::PROPERTY_WRITE  |
    BLECharacteristic::PROPERTY_NOTIFY
  );

  pCharacteristic->setCallbacks(new CharacteristicCallbacks());
  pCharacteristic->addDescriptor(new BLE2902());
  pService->start();

  BLEAdvertising* pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06);  // iOS connection parameter hint
  pAdvertising->setMinPreferred(0x12);
  BLEDevice::startAdvertising();

  Serial.printf("[BLE] Advertising as 'SilvaLink_Trekker_%s'\n", NODE_ID);
  Serial.println("[READY] Waiting for connections...\n");
}

// ============================================================
//  Main Loop
// ============================================================

void loop() {

  // ---- Handle BLE connection state changes ----
  if (!deviceConnected && prevConnected) {
    // Device just disconnected — restart advertising after short delay
    delay(500);
    BLEDevice::startAdvertising();
  }
  prevConnected = deviceConnected;

  // ---- SOS Transmission (queued from BLE callback) ----
  if (pendingSOS) {
    pendingSOS = false;  // Clear flag immediately

    String sosType = "UNKNOWN";
    if (pendingEmergencyCode == 0x01)      sosType = "MEDICAL";
    else if (pendingEmergencyCode == 0x02) sosType = "DISASTER";
    else if (pendingEmergencyCode == 0x03) sosType = "WILDLIFE";
    else if (pendingEmergencyCode == 0x04) sosType = "LOST";

    Serial.printf("\n[SOS] Sending %s alert...\n", sosType.c_str());

    // Send the SOS packet via LoRa
    sendSOSPacket(pendingEmergencyCode, pendingCustomMsg);

    // Listen for ACK from Admin (3 second window)
    Serial.println("[SOS] Waiting for ACK...");
    bool ackReceived = listenForAck(3000);

    // Notify the Flutter app via BLE
    if (deviceConnected && pCharacteristic) {
      String bleMsg;
      if (ackReceived) {
        bleMsg = "SOS ACK ✓";
        Serial.println("[SOS] ACK confirmed → notifying app");
      } else {
        bleMsg = "SOS SENT (no ACK)";
        Serial.println("[SOS] No ACK received (may still be delivered)");
      }
      pCharacteristic->setValue(bleMsg.c_str());
      pCharacteristic->notify();
    }

    Serial.println();
  }

  // ---- Periodic: Send Environmental Data ----
  if (millis() - lastEnvSend >= envInterval) {
    lastEnvSend = millis();

    float temperature, humidity;
    if (readDHT(temperature, humidity)) {
      // Send to Admin via LoRa
      sendEnvPacket(temperature, humidity);

      // Send to Flutter app via BLE
      if (deviceConnected && pCharacteristic) {
        String bleTempStr = String(temperature, 1) + " °C";
        pCharacteristic->setValue(bleTempStr.c_str());
        pCharacteristic->notify();
      }
    }
  }

  // ---- Periodic: Heartbeat (keeps node alive on Admin dashboard) ----
  if (millis() - lastHeartbeat >= hbInterval) {
    lastHeartbeat = millis();
    sendHeartbeat();
  }
}
