#include <Arduino.h>
#include <esp_task_wdt.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <LoRa.h>
#include <ArduinoJson.h>

#define LORA_MISO 19
#define LORA_MOSI 23
#define LORA_SCK  18
#define LORA_CS   5
#define LORA_RST  14
#define LORA_DIO0 2
#define LORA_FREQ 433E6

const char* ssid     = "SilvaLink Admin";
const char* password = "123456789";

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

#define MAX_NODES 8
struct NodeInfo {
  char     id[4];
  float    temp;
  float    hum;
  int      rssi;
  unsigned long lastSeen;
  uint8_t  status;
};
NodeInfo nodes[MAX_NODES];
uint8_t  nodeCount = 0;

unsigned long bootTime = 0;
uint32_t packetCount = 0;
uint32_t errorCount = 0;

NodeInfo* findNode(const char* id) {
  for (uint8_t i = 0; i < nodeCount; i++) if (strcmp(nodes[i].id, id) == 0) return &nodes[i];
  return nullptr;
}

NodeInfo* getOrCreateNode(const char* id) {
  NodeInfo* node = findNode(id);
  if (node) return node;
  if (nodeCount >= MAX_NODES) return nullptr;
  node = &nodes[nodeCount++];
  strncpy(node->id, id, sizeof(node->id) - 1);
  node->id[sizeof(node->id) - 1] = '\0';
  node->temp = 0.0f; node->hum = 0.0f; node->rssi = 0;
  node->lastSeen = 0; node->status = 1;
  Serial.printf("[NODE] Registered: %s (total: %d)\n", id, nodeCount);
  return node;
}

void notifyEnv(NodeInfo* node) {
  StaticJsonDocument<256> doc;
  doc["type"] = "env";
  doc["node"].set(node->id); // Explicit set() ensures string copy in ArduinoJson v7
  doc["temp"].set(String(node->temp, 1));
  doc["hum"].set(String(node->hum, 1));
  doc["rssi"] = node->rssi;
  
  char json[256];
  size_t len = serializeJson(doc, json, sizeof(json));
  ws.textAll(json);
  Serial.printf("[WS TX] %s\n", json);
}

void notifySOS(const char* nodeId, const char* sosType, const char* customMsg, int rssi) {
  StaticJsonDocument<512> doc;
  doc["type"] = "sos"; doc["node"].set(nodeId);
  doc["sosType"].set(sosType); doc["customMsg"].set(customMsg);
  doc["rssi"] = rssi;
  char json[512]; serializeJson(doc, json, sizeof(json));
  ws.textAll(json);
}

void notifyAck(const char* nodeId) {
  StaticJsonDocument<128> doc;
  doc["type"] = "ack"; doc["node"].set(nodeId);
  char json[128]; serializeJson(doc, json, sizeof(json));
  ws.textAll(json);
}

void processStructuredPacket(const String& packet, int rssi) {
  int first = packet.indexOf('|');
  if (first < 0) return;
  String nodeId = packet.substring(0, first);
  nodeId.trim();
  if (nodeId.length() == 0 || nodeId.length() > 3) return;

  int second = packet.indexOf('|', first + 1);
  String pType, payload;
  if (second > 0) {
    pType = packet.substring(first + 1, second);
    payload = packet.substring(second + 1);
  } else {
    pType = packet.substring(first + 1);
    payload = "";
  }
  pType.trim(); pType.toUpperCase();

  NodeInfo* node = getOrCreateNode(nodeId.c_str());
  if (!node) return;
  node->rssi = rssi; node->lastSeen = millis();

  if (pType == "ENV") {
    int sep = payload.indexOf('|');
    if (sep > 0) {
      float temp = payload.substring(0, sep).toFloat();
      float hum  = payload.substring(sep + 1).toFloat();
      if (temp > -40.0f && temp < 85.0f && hum >= 0.0f && hum <= 100.0f) {
        node->temp = temp; node->hum = hum; node->status = 1;
        Serial.printf("[ENV] %s: %.1fC %.1f%% RSSI %d\n", node->id, temp, hum, rssi);
        notifyEnv(node);
      } else errorCount++;
    }
  } else if (pType == "SOS") {
    node->status = 2;
    String sosType = "LOST", customMsg = "";
    int sep = payload.indexOf('|');
    if (sep > 0) {
      int code = strtol(payload.substring(0, sep).c_str(), NULL, 16);
      customMsg = payload.substring(sep + 1);
      if (code == 0x01) sosType = "MEDICAL"; else if (code == 0x02) sosType = "DISASTER";
      else if (code == 0x03) sosType = "WILDLIFE";
    }
    notifySOS(node->id, sosType.c_str(), customMsg.c_str(), rssi);
  } else if (pType == "HB") {
    if (node->status != 2) node->status = 1;
  }
}

void sendLoRaAck(const char* nodeId) {
  String ack = "ACK|" + String(nodeId) + "|SOS_RECEIVED";
  LoRa.beginPacket(); LoRa.print(ack); LoRa.endPacket();
}

void onWsEvent(AsyncWebSocket* server, AsyncWebSocketClient* client, AwsEventType type, void* arg, uint8_t* data, size_t len) {
  if (type == WS_EVT_DATA) {
    AwsFrameInfo* info = (AwsFrameInfo*)arg;
    if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
      data[len] = 0;
      StaticJsonDocument<256> doc;
      if (!deserializeJson(doc, (char*)data)) {
        if (strcmp(doc["type"], "ack") == 0) {
          const char* nid = doc["node"];
          if (nid) { sendLoRaAck(nid); NodeInfo* n = findNode(nid); if (n) n->status = 1; notifyAck(nid); }
        }
      }
    }
  }
}

void setup() {
  Serial.begin(115200); delay(500);
  Serial.println("\n[Admin] SilvaLink Gateway v2.1 Ready");
  bootTime = millis();
  LittleFS.begin(true);
  WiFi.softAP(ssid, password);
  Serial.printf("[WiFi] %s | %s\n", ssid, WiFi.softAPIP().toString().c_str());

  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_CS);
  LoRa.setPins(LORA_CS, LORA_RST, LORA_DIO0);
  if (!LoRa.begin(LORA_FREQ)) { while(1) delay(1000); }
  LoRa.setSpreadingFactor(7);

  ws.onEvent(onWsEvent); server.addHandler(&ws);
  server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");
  server.begin();
  esp_task_wdt_init(8, true); esp_task_wdt_add(NULL);
}

void loop() {
  esp_task_wdt_reset(); ws.cleanupClients();
  int pSize = LoRa.parsePacket();
  if (pSize) {
    String pkt; pkt.reserve(pSize + 1);
    while (LoRa.available()) pkt += (char)LoRa.read();
    packetCount++;
    if (pkt.indexOf('|') > 0) processStructuredPacket(pkt, LoRa.packetRssi());
  }
  static unsigned long lastCheck = 0;
  if (millis() - lastCheck > 10000) {
    lastCheck = millis();
    for (uint8_t i = 0; i < nodeCount; i++) {
      if (nodes[i].status == 1 && (millis() - nodes[i].lastSeen > 30000)) nodes[i].status = 0;
    }
  }
}