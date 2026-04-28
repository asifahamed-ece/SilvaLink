#include <Arduino.h>
#include <SPI.h>
#include <LoRa.h>
#include <DHT.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

// ============================================================
//  SilvaLink Node A — Trekker Sensor v2.1
//  - Physical SOS Button + BLE SOS Trigger
//  - Structured protocol: NODE_ID|TYPE|PAYLOAD
// ============================================================
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

// ---------------- Physical SOS Button ----------
#define SOS_BUTTON_PIN 12            // ← Change to your button pin
#define SOS_COOLDOWN_MS 5000         // Prevent spam
int buttonState = HIGH;
int lastButtonState = HIGH;
unsigned long lastDebounceTime = 0;
const unsigned long debounceDelay = 50;
unsigned long lastSOSTriggerTime = 0;

// ---------------- BLE Configuration ------------
#define SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"
BLECharacteristic *pCharacteristic = nullptr;
bool deviceConnected = false;
bool prevConnected = false;

// ---- SOS Queue ----
volatile bool pendingSOS = false;
int pendingEmergencyCode = 0;
String pendingCustomMsg = "";

// ---- Last Known Good Readings ----
float lastGoodTemp = 0.0f;
float lastGoodHum = 0.0f;
bool hasGoodReading = false;

// ---- Timing ----
unsigned long lastEnvSend = 0;
unsigned long lastHeartbeat = 0;
const unsigned long envInterval = 5000;
const unsigned long hbInterval = 30000;

// ============================================================
//  BLE Callbacks
// ============================================================
class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) override { deviceConnected = true; Serial.println("[BLE] Connected"); }
  void onDisconnect(BLEServer* pServer) override { deviceConnected = false; Serial.println("[BLE] Disconnected"); }
};

class CharacteristicCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pChar) override {
    std::string rx = pChar->getValue();
    if (rx.length() == 0) return;
    pendingEmergencyCode = rx[0];
    pendingCustomMsg = (rx.length() > 1) ? String(rx.substr(1).c_str()) : "";
    if (!pendingSOS) pendingSOS = true;
  }
};

// ============================================================
//  LoRa Packet Builders
// ============================================================
void sendEnvPacket(float temp, float hum) {
  String pkt = String(NODE_ID) + "|ENV|" + String(temp, 1) + "|" + String(hum, 1);
  LoRa.beginPacket(); LoRa.print(pkt); LoRa.endPacket();
  Serial.println("[LoRa TX] " + pkt);
}

void sendSOSPacket(int code, const String& msg) {
  char hex[3]; snprintf(hex, sizeof(hex), "%02X", code);
  String pkt = String(NODE_ID) + "|SOS|" + String(hex);
  if (msg.length() > 0) pkt += "|" + msg;
  LoRa.beginPacket(); LoRa.print(pkt); LoRa.endPacket();
  Serial.println("[LoRa TX] SOS: " + pkt);
}

void sendHeartbeat() {
  String pkt = String(NODE_ID) + "|HB";
  LoRa.beginPacket(); LoRa.print(pkt); LoRa.endPacket();
}

// ============================================================
//  ACK Listener
// ============================================================
bool listenForAck(unsigned long timeoutMs) {
  unsigned long start = millis();
  while (millis() - start < timeoutMs) {
    int size = LoRa.parsePacket();
    if (size) {
      String inc = "";
      while (LoRa.available()) inc += (char)LoRa.read();
      if (inc.startsWith("ACK|" + String(NODE_ID))) {
        Serial.println("[LoRa RX] ACK!");
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
bool readDHT(float &t, float &h) {
  for (int i = 0; i < DHT_RETRY_COUNT; i++) {
    float hum = dht.readHumidity();
    float temp = dht.readTemperature();
    if (!isnan(hum) && !isnan(temp) && temp > -40 && temp < 85 && hum >= 0 && hum <= 100) {
      t = temp; h = hum;
      lastGoodTemp = temp; lastGoodHum = hum; hasGoodReading = true;
      return true;
    }
    if (i < DHT_RETRY_COUNT - 1) delay(DHT_RETRY_DELAY_MS);
  }
  if (hasGoodReading) { t = lastGoodTemp; h = lastGoodHum; return true; }
  return false;
}

// ============================================================
//  Setup
// ============================================================
void setup() {
  Serial.begin(115200); delay(500);
  Serial.println("\n[Node] SilvaLink Trekker v2.1 — " + String(NODE_ID));

  // Physical button setup
  pinMode(SOS_BUTTON_PIN, INPUT_PULLUP);

  dht.begin(); delay(1000);

  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_CS);
  LoRa.setPins(LORA_CS, LORA_RST, LORA_DIO0);
  if (!LoRa.begin(LORA_FREQ)) { Serial.println("[LoRa] FAILED!"); while(1) delay(1000); }
  LoRa.setSpreadingFactor(7); Serial.println("[LoRa] Ready 433MHz SF7");

  BLEDevice::init("SilvaLink_Trekker_" NODE_ID);
  BLEServer* pServer = BLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());
  BLEService* pService = pServer->createService(SERVICE_UUID);
  pCharacteristic = pService->createCharacteristic(CHARACTERISTIC_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_NOTIFY);
  pCharacteristic->setCallbacks(new CharacteristicCallbacks());
  pCharacteristic->addDescriptor(new BLE2902());
  pService->start();
  BLEAdvertising* pAdv = BLEDevice::getAdvertising();
  pAdv->addServiceUUID(SERVICE_UUID);
  pAdv->setScanResponse(true);
  BLEDevice::startAdvertising();
  Serial.println("[BLE] Advertising...");
}

// ============================================================
//  Loop
// ============================================================
void loop() {
  // BLE reconnect
  if (!deviceConnected && prevConnected) { delay(500); BLEDevice::startAdvertising(); }
  prevConnected = deviceConnected;

  // ---- Physical Button SOS ----
  int reading = digitalRead(SOS_BUTTON_PIN);
  if (reading != lastButtonState) lastDebounceTime = millis();
  if ((millis() - lastDebounceTime) > debounceDelay) {
    if (reading != buttonState) {
      buttonState = reading;
      if (buttonState == LOW && !pendingSOS && (millis() - lastSOSTriggerTime > SOS_COOLDOWN_MS)) {
        Serial.println("[BTN] SOS TRIGGERED!");
        pendingEmergencyCode = 0x04; // LOST/GENERAL
        pendingCustomMsg = "Physical Button Activated";
        pendingSOS = true;
        lastSOSTriggerTime = millis();
      }
    }
  }
  lastButtonState = reading;

  // ---- Process Queued SOS ----
  if (pendingSOS) {
    pendingSOS = false;
    sendSOSPacket(pendingEmergencyCode, pendingCustomMsg);
    bool ack = listenForAck(3000);
    if (deviceConnected && pCharacteristic) {
      pCharacteristic->setValue(ack ? "SOS ACK ✓" : "SOS SENT");
      pCharacteristic->notify();
    }
    pendingCustomMsg = "";
  }

  // ---- Periodic Env Data ----
  if (millis() - lastEnvSend >= envInterval) {
    lastEnvSend = millis();
    float t, h;
    if (readDHT(t, h)) {
      sendEnvPacket(t, h);
      if (deviceConnected && pCharacteristic) {
        pCharacteristic->setValue((String(t, 1) + " °C").c_str());
        pCharacteristic->notify();
      }
    }
  }

  // ---- Heartbeat ----
  if (millis() - lastHeartbeat >= hbInterval) {
    lastHeartbeat = millis();
    sendHeartbeat();
  }
}