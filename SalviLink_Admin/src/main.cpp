#include <Arduino.h>
#include <esp_task_wdt.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <LoRa.h>
#include <ArduinoJson.h>

// ============================================================
//  SilvaLink Admin Gateway — Optimized Firmware
//  - Multi-node tracking (up to 8 nodes)
//  - Structured LoRa packet parsing: NODE_ID|TYPE|PAYLOAD
//  - WebSocket push for env, SOS, ACK, and system status
//  - SOS acknowledgment relay via LoRa
//  - Stack-allocated JSON (no heap fragmentation)
//  - Watchdog timer for crash recovery
// ============================================================

// ---------------- LoRa Pin Configuration ----------------
#define LORA_MISO 19
#define LORA_MOSI 23
#define LORA_SCK  18
#define LORA_CS   5
#define LORA_RST  14
#define LORA_DIO0 2
#define LORA_FREQ 433E6

// ---------------- WiFi AP Configuration -----------------
const char* ssid     = "SilvaLink Admin";
const char* password = "123456789";

// ---------------- Server & WebSocket --------------------
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

// ---------------- Node Tracking -------------------------
#define MAX_NODES 8

struct NodeInfo {
  char     id[4];            // e.g. "A", "B"
  float    temp;
  float    hum;
  int      rssi;
  unsigned long lastSeen;    // millis() timestamp
  uint8_t  status;           // 0=offline, 1=online, 2=sos
};

NodeInfo nodes[MAX_NODES];
uint8_t  nodeCount = 0;

// ---------------- System Stats --------------------------
unsigned long bootTime      = 0;
uint32_t      packetCount   = 0;
uint32_t      errorCount    = 0;

// ============================================================
//  Node Management
// ============================================================

NodeInfo* findNode(const char* id) {
  for (uint8_t i = 0; i < nodeCount; i++) {
    if (strcmp(nodes[i].id, id) == 0) return &nodes[i];
  }
  return nullptr;
}

NodeInfo* getOrCreateNode(const char* id) {
  NodeInfo* node = findNode(id);
  if (node) return node;

  if (nodeCount >= MAX_NODES) {
    Serial.println("[WARN] Max nodes reached, ignoring new node");
    return nullptr;
  }

  node = &nodes[nodeCount++];
  strncpy(node->id, id, sizeof(node->id) - 1);
  node->id[sizeof(node->id) - 1] = '\0';
  node->temp     = 0.0f;
  node->hum      = 0.0f;
  node->rssi     = 0;
  node->lastSeen = 0;
  node->status   = 1;

  Serial.printf("[NODE] Registered new node: %s (total: %d)\n", id, nodeCount);
  return node;
}

// ============================================================
//  WebSocket Notification Functions
// ============================================================

void notifyEnv(NodeInfo* node) {
  StaticJsonDocument<256> doc;
  doc["type"] = "env";
  doc["node"] = node->id;
  doc["temp"] = serialized(String(node->temp, 1));
  doc["hum"]  = serialized(String(node->hum, 1));
  doc["rssi"] = node->rssi;

  char json[256];
  serializeJson(doc, json, sizeof(json));
  ws.textAll(json);
}

void notifySOS(const char* nodeId, const char* sosType, const char* customMsg, int rssi) {
  StaticJsonDocument<512> doc;
  doc["type"]      = "sos";
  doc["node"]      = nodeId;
  doc["sosType"]   = sosType;
  doc["customMsg"] = customMsg;
  doc["rssi"]      = rssi;

  char json[512];
  serializeJson(doc, json, sizeof(json));
  ws.textAll(json);
}

void notifyAck(const char* nodeId) {
  StaticJsonDocument<128> doc;
  doc["type"] = "ack";
  doc["node"] = nodeId;

  char json[128];
  serializeJson(doc, json, sizeof(json));
  ws.textAll(json);
}

// ============================================================
//  LoRa Packet Processing
//  Protocol: NODE_ID|TYPE|PAYLOAD
//  ENV:  A|ENV|25.3|60.1
//  SOS:  A|SOS|01|Help needed at ridge
//  HB:   A|HB
// ============================================================

void processStructuredPacket(const String& packet, int rssi) {
  // Parse pipe-delimited fields
  int first  = packet.indexOf('|');
  if (first < 0) return;

  String nodeId = packet.substring(0, first);
  nodeId.trim();
  if (nodeId.length() == 0 || nodeId.length() > 3) return;

  int second = packet.indexOf('|', first + 1);
  String pType;
  String payload;

  if (second > 0) {
    pType   = packet.substring(first + 1, second);
    payload = packet.substring(second + 1);
  } else {
    pType = packet.substring(first + 1);
    payload = "";
  }

  pType.trim();
  pType.toUpperCase();

  NodeInfo* node = getOrCreateNode(nodeId.c_str());
  if (!node) return;

  node->rssi    = rssi;
  node->lastSeen = millis();

  if (pType == "ENV") {
    // Payload: temp|hum
    int sep = payload.indexOf('|');
    if (sep > 0) {
      float temp = payload.substring(0, sep).toFloat();
      float hum  = payload.substring(sep + 1).toFloat();

      // Sanity check
      if (temp > -40.0f && temp < 85.0f && hum >= 0.0f && hum <= 100.0f) {
        node->temp   = temp;
        node->hum    = hum;
        node->status = 1; // online
        Serial.printf("[ENV] Node %s: %.1f°C, %.1f%%, RSSI %d\n", node->id, temp, hum, rssi);
        notifyEnv(node);
      } else {
        Serial.printf("[WARN] Invalid env data from %s: T=%.1f H=%.1f\n", node->id, temp, hum);
        errorCount++;
      }
    }
  }
  else if (pType == "SOS") {
    // Payload: hexCode|customMessage
    node->status = 2; // SOS active

    String sosType = "UNKNOWN";
    String customMsg = "";
    int sep = payload.indexOf('|');

    if (sep > 0) {
      int code = (int)strtol(payload.substring(0, sep).c_str(), NULL, 16);
      customMsg = payload.substring(sep + 1);

      if (code == 0x01)      sosType = "MEDICAL";
      else if (code == 0x02) sosType = "DISASTER";
      else if (code == 0x03) sosType = "WILDLIFE";
      else if (code == 0x04) sosType = "LOST";
    } else {
      // No custom message, just code
      int code = (int)strtol(payload.c_str(), NULL, 16);
      if (code == 0x01)      sosType = "MEDICAL";
      else if (code == 0x02) sosType = "DISASTER";
      else if (code == 0x03) sosType = "WILDLIFE";
      else if (code == 0x04) sosType = "LOST";
    }

    Serial.printf("[SOS] Node %s: Type=%s Msg=%s RSSI=%d\n",
                  node->id, sosType.c_str(), customMsg.c_str(), rssi);
    notifySOS(node->id, sosType.c_str(), customMsg.c_str(), rssi);
  }
  else if (pType == "HB") {
    // Heartbeat — just update lastSeen
    if (node->status != 2) node->status = 1;
    Serial.printf("[HB] Node %s alive, RSSI %d\n", node->id, rssi);
  }
  else {
    Serial.printf("[WARN] Unknown packet type '%s' from node %s\n", pType.c_str(), nodeId.c_str());
    errorCount++;
  }
}

// Legacy packet support (backward compatibility)
void processLegacyPacket(const String& packet, int rssi) {
  if (packet.startsWith("!!! SOS")) {
    // Legacy SOS format: "!!! SOS ALERT !!! Type: MEDICAL | Msg: help"
    NodeInfo* node = getOrCreateNode("A");
    if (!node) return;

    node->rssi     = rssi;
    node->lastSeen = millis();
    node->status   = 2;

    String sosType = "UNKNOWN";
    String customMsg = "";

    int typeIdx = packet.indexOf("Type:");
    if (typeIdx >= 0) {
      int msgIdx = packet.indexOf("|", typeIdx);
      if (msgIdx > 0) {
        sosType = packet.substring(typeIdx + 5, msgIdx);
        sosType.trim();
        int msgStart = packet.indexOf("Msg:", msgIdx);
        if (msgStart > 0) {
          customMsg = packet.substring(msgStart + 4);
          customMsg.trim();
        }
      } else {
        sosType = packet.substring(typeIdx + 5);
        sosType.trim();
      }
    }

    Serial.printf("[SOS-LEGACY] Type=%s Msg=%s RSSI=%d\n", sosType.c_str(), customMsg.c_str(), rssi);
    notifySOS(node->id, sosType.c_str(), customMsg.c_str(), rssi);
  }
  else if (packet.startsWith("T:")) {
    // Legacy env format: "T:25.3 H:60.1"
    NodeInfo* node = getOrCreateNode("A");
    if (!node) return;

    node->rssi     = rssi;
    node->lastSeen = millis();

    int tIdx = packet.indexOf("T:");
    int hIdx = packet.indexOf("H:");
    if (tIdx >= 0 && hIdx > tIdx) {
      String tempStr = packet.substring(tIdx + 2, packet.indexOf(' ', tIdx));
      String humStr  = packet.substring(hIdx + 2);

      node->temp   = tempStr.toFloat();
      node->hum    = humStr.toFloat();
      node->status = 1;

      Serial.printf("[ENV-LEGACY] %.1f°C, %.1f%%, RSSI %d\n", node->temp, node->hum, rssi);
      notifyEnv(node);
    }
  }
  else {
    Serial.println("[UNKNOWN] " + packet);
  }
}

// ============================================================
//  LoRa: Send ACK back to a node
// ============================================================

void sendLoRaAck(const char* nodeId) {
  String ackPacket = "ACK|";
  ackPacket += nodeId;
  ackPacket += "|SOS_RECEIVED";

  LoRa.beginPacket();
  LoRa.print(ackPacket);
  LoRa.endPacket();

  Serial.printf("[ACK] Sent to Node %s: %s\n", nodeId, ackPacket.c_str());
}

// ============================================================
//  WebSocket Event Handler
// ============================================================

void onWsEvent(AsyncWebSocket* server, AsyncWebSocketClient* client,
               AwsEventType type, void* arg, uint8_t* data, size_t len) {

  if (type == WS_EVT_CONNECT) {
    Serial.printf("[WS] Client #%u connected\n", client->id());

    // Send current state of all nodes
    for (uint8_t i = 0; i < nodeCount; i++) {
      if (nodes[i].lastSeen > 0) {
        notifyEnv(&nodes[i]);
      }
    }
  }
  else if (type == WS_EVT_DISCONNECT) {
    Serial.printf("[WS] Client #%u disconnected\n", client->id());
  }
  else if (type == WS_EVT_DATA) {
    // Handle incoming messages from dashboard (e.g., ACK button)
    AwsFrameInfo* info = (AwsFrameInfo*)arg;
    if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
      data[len] = 0; // Null-terminate

      StaticJsonDocument<256> doc;
      DeserializationError err = deserializeJson(doc, (char*)data);
      if (err) return;

      const char* msgType = doc["type"];
      if (msgType && strcmp(msgType, "ack") == 0) {
        const char* nodeId = doc["node"];
        if (nodeId) {
          // Relay ACK via LoRa
          sendLoRaAck(nodeId);

          // Update node status
          NodeInfo* node = findNode(nodeId);
          if (node) node->status = 1;

          // Notify all dashboard clients
          notifyAck(nodeId);
        }
      }
    }
  }
}

// ============================================================
//  Setup
// ============================================================

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n========================================");
  Serial.println("  SilvaLink Admin Gateway v2.0");
  Serial.println("========================================\n");

  bootTime = millis();

  // Mount LittleFS
  if (!LittleFS.begin(true)) {
    Serial.println("[FS] LittleFS mount failed!");
  } else {
    Serial.println("[FS] LittleFS mounted");
  }

  // Start WiFi Access Point
  WiFi.softAP(ssid, password);
  Serial.printf("[WiFi] AP started — SSID: %s\n", ssid);
  Serial.printf("[WiFi] IP: %s\n", WiFi.softAPIP().toString().c_str());

  // Initialize LoRa
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_CS);
  LoRa.setPins(LORA_CS, LORA_RST, LORA_DIO0);
  if (!LoRa.begin(LORA_FREQ)) {
    Serial.println("[LoRa] Init FAILED! Check wiring.");
    while (1) { delay(1000); }
  }
  LoRa.setSpreadingFactor(7);
  Serial.println("[LoRa] Ready at 433MHz, SF7");

  // Setup WebSocket
  ws.onEvent(onWsEvent);
  server.addHandler(&ws);

  // Serve dashboard from LittleFS
  server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

  // Start HTTP server
  server.begin();
  Serial.println("[HTTP] Server started");
  Serial.println("[READY] Open http://192.168.4.1\n");

  // Enable ESP32 watchdog (8 second timeout)
  esp_task_wdt_init(8, true);
  esp_task_wdt_add(NULL);
}

// ============================================================
//  Main Loop
// ============================================================

void loop() {
  // Feed the watchdog
  esp_task_wdt_reset();

  // Cleanup stale WebSocket connections
  ws.cleanupClients();

  // ---- Process incoming LoRa packets ----
  int packetSize = LoRa.parsePacket();
  if (packetSize) {
    String incoming = "";
    incoming.reserve(packetSize + 1);
    while (LoRa.available()) {
      incoming += (char)LoRa.read();
    }

    int rssi = LoRa.packetRssi();
    packetCount++;

    // Route to appropriate parser
    if (incoming.indexOf('|') > 0) {
      // New structured protocol
      processStructuredPacket(incoming, rssi);
    } else {
      // Legacy format (backward compatible)
      processLegacyPacket(incoming, rssi);
    }
  }

  // ---- Periodic: Mark stale nodes as offline (every 10s) ----
  static unsigned long lastStaleCheck = 0;
  if (millis() - lastStaleCheck >= 10000) {
    lastStaleCheck = millis();
    unsigned long now = millis();
    for (uint8_t i = 0; i < nodeCount; i++) {
      if (nodes[i].status == 1 && (now - nodes[i].lastSeen) > 30000) {
        nodes[i].status = 0; // Mark offline
        Serial.printf("[NODE] Node %s went offline\n", nodes[i].id);
      }
    }
  }
}
