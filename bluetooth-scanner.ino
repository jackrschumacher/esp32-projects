#include <Arduino.h>
#include <WiFi.h>
#include <LittleFS.h>
#include "esp_wifi.h"
#include <set>

// ================= CONFIGURATION =================
const unsigned long SAVE_INTERVAL_MS = 15 * 60 * 1000;  // Save to storage every 15 Mins
const unsigned long STATUS_INTERVAL_MS = 0.5 * 60 * 1000; // Print status every 2 Mins
const char* DATA_PATH = "/readings.txt";

// ================= GLOBALS =================
std::set<String> macsInRam;       
unsigned long lastSaveTime = 0;
unsigned long lastStatusTime = 0;
unsigned long lastChannelHop = 0;

// ================= HELPERS =================

void initFS() {
  if (!LittleFS.begin(true)) {
    Serial.println("Error mounting LittleFS");
  } else {
    Serial.println("Storage Mounted.");
  }
}

String formatTime(unsigned long m) {
  unsigned long seconds = m / 1000;
  unsigned long minutes = seconds / 60;
  unsigned long hours = minutes / 60;
  char buf[20];
  snprintf(buf, sizeof(buf), "%02lu:%02lu:%02lu", hours, minutes % 60, seconds % 60);
  return String(buf);
}

void printStatus() {
   float tempC = temperatureRead();
   uint32_t freeRam = ESP.getFreeHeap();
   unsigned long uptime = millis() / 1000;
   
   Serial.println("\n--- SYSTEM STATUS ---");
   Serial.printf("Time           : %s\n", formatTime(millis()).c_str());
   Serial.printf("Current Buffer : %d unique devices\n", macsInRam.size());
   Serial.printf("Chip Temp      : %.1f C\n", tempC);
   Serial.printf("Free RAM       : %u bytes\n", freeRam);
   Serial.println("---------------------");
}

void saveToStorage(unsigned long timestamp, int count) {
  File file = LittleFS.open(DATA_PATH, FILE_APPEND);
  if (!file) {
    Serial.println("Error: Could not open file to save.");
    return;
  }
  file.printf("%lu,%d\n", timestamp, count);
  file.close();

  // Print specific 'Saved' message separate from standard status
  float tempC = temperatureRead();
  uint32_t freeRam = ESP.getFreeHeap();
  Serial.printf(">> STORAGE SAVED: %s | Devices: %d | Temp: %.1fC | Free RAM: %u\n", 
                formatTime(timestamp).c_str(), count, tempC, freeRam);
}

// ================= SERIAL COMMANDS =================

void printGraph() {
  if (!LittleFS.exists(DATA_PATH)) {
    Serial.println("No data found.");
    return;
  }

  File file = LittleFS.open(DATA_PATH, FILE_READ);
  Serial.println("\n=== 15-MINUTE OCCUPANCY LOG ===");
  Serial.println("Time (Boot) | Count | Graph");
  Serial.println("-----------------------------------");

  while (file.available()) {
    String line = file.readStringUntil('\n');
    int commaIndex = line.indexOf(',');
    if (commaIndex > 0) {
      unsigned long ts = line.substring(0, commaIndex).toInt();
      int count = line.substring(commaIndex + 1).toInt();
      
      String bar = "";
      int visualCount = (count > 60) ? 60 : count; 
      for (int i = 0; i < visualCount; i++) bar += "=";
      
      Serial.printf("%s    | %-5d | %s\n", formatTime(ts).c_str(), count, bar.c_str());
    }
  }
  Serial.println("===================================\n");
  file.close();
}

void dumpCSV() {
  if (!LittleFS.exists(DATA_PATH)) return;
  File file = LittleFS.open(DATA_PATH, FILE_READ);
  Serial.println("--- CSV START ---");
  while(file.available()) Serial.write(file.read());
  Serial.println("\n--- CSV END ---");
  file.close();
}

void clearData() {
  LittleFS.remove(DATA_PATH);
  Serial.println("Storage cleared.");
}

// ================= WIFI SNIFFER =================

void promiscuous_rx_cb(void* buf, wifi_promiscuous_pkt_type_t type) {
  if (ESP.getFreeHeap() < 10000) return; // Safety cutoff

  wifi_promiscuous_pkt_t* pkt = (wifi_promiscuous_pkt_t*)buf;
  uint8_t* payload = pkt->payload;
  uint8_t frame_ctrl = payload[0];

  // Management Frame check (Probe Request is Subtype 4)
  if ((frame_ctrl & 0x0C) == 0x00) { 
    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02x:%02x:%02x:%02x:%02x:%02x",
             payload[10], payload[11], payload[12],
             payload[13], payload[14], payload[15]);
    macsInRam.insert(String(macStr));
  }
}

// ================= SETUP & LOOP =================

void setup() {
  Serial.begin(115200);
  initFS();

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_promiscuous_rx_cb(&promiscuous_rx_cb);

  Serial.println("System Started.");
  printStatus(); // Print initial status
  Serial.println("Commands: 'graph', 'csv', 'clear', 'status'");
  
  lastSaveTime = millis();
  lastStatusTime = millis();
}

void loop() {
  unsigned long currentMillis = millis();

  // 1. Channel Hopping (Every 250ms)
  if (currentMillis - lastChannelHop > 250) {
    lastChannelHop = currentMillis;
    int ch = (currentMillis / 250) % 13 + 1;
    esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
  }

  // 2. Scheduled Status Print (Every 2 Mins)
  if (currentMillis - lastStatusTime >= STATUS_INTERVAL_MS) {
    printStatus();
    lastStatusTime = currentMillis;
  }

  // 3. Storage Save (Every 15 Mins)
  if (currentMillis - lastSaveTime >= SAVE_INTERVAL_MS) {
    int count = macsInRam.size();
    saveToStorage(currentMillis, count);
    macsInRam.clear(); // Reset buffer
    lastSaveTime = currentMillis;
  }

  // 4. Serial Inputs
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if (cmd == "graph") printGraph();
    else if (cmd == "csv") dumpCSV();
    else if (cmd == "clear") clearData();
    else if (cmd == "status") printStatus();
  }
}
