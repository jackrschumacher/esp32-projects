#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <Adafruit_NeoPixel.h>
#include <FS.h>
#include <LittleFS.h>

// --- IMPORTANT: PIN CONFIGURATION ---
#define RGB_PIN 48  
#define NUMPIXELS 1
#define WIFI_CHANNEL 1

// --- LOGGING CONFIGURATION ---
#define LOG_INTERVAL_MS 900000 // 15 minutes
// #define LOG_INTERVAL_MS 60000 // Uncomment for testing (1 Minute)

// Setup the NeoPixel library
Adafruit_NeoPixel pixels(NUMPIXELS, RGB_PIN, NEO_GRB + NEO_KHZ800);

// Traffic limits
#define LOW_TRAFFIC 200
#define HIGH_TRAFFIC 400

// --- FADE VARIABLES ---
int currentR = 0, currentG = 0, currentB = 0;
int targetR = 0, targetG = 0, targetB = 0;
int fadeSpeed = 5; 

// --- COUNTERS ---
volatile int packetCount = 0;      
unsigned long hourTotalPackets = 0;
int hourCounter = 1;               

// --- TIMERS ---
unsigned long lastTrafficCheck = 0;
unsigned long lastFadeUpdate = 0;
unsigned long lastLogTime = 0;

int trafficInterval = 1000; 

// Sniffer Callback
void promiscuous_rx_cb(void* buf, wifi_promiscuous_pkt_type_t type) {
  packetCount++;
}

// --- FILE SYSTEM HELPERS ---
void appendFile(const char * path, String message){
  File file = LittleFS.open(path, FILE_APPEND);
  if(!file){
    Serial.println("- failed to open file for appending");
    return;
  }
  file.print(message);
  file.close();
}

void readFile(const char * path){
  Serial.printf("Reading file: %s\r\n", path);
  File file = LittleFS.open(path);
  if(!file || file.isDirectory()){
    Serial.println("- failed to open file for reading");
    return;
  }
  while(file.available()){
    Serial.write(file.read());
  }
  file.close();
}

void setup() {
  Serial.begin(115200);
  delay(1000); 

  // 1. Initialize File System
  if(!LittleFS.begin(true)){
    Serial.println("LittleFS Mount Failed");
    return;
  }

  // 2. Print existing history to Serial on boot
  Serial.println("\n--- HISTORY LOG (Copy to CSV) ---");
  Serial.println("Hour, Total_Packets, Avg_Packets_Per_Sec");
  readFile("/traffic_log.txt");
  Serial.println("--- END HISTORY ---");

  // 3. Initialize LED
  pixels.begin();
  pixels.setBrightness(20);
  pixels.clear();
  pixels.show();

  // 4. Initialize WiFi Sniffer
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_promiscuous_rx_cb(promiscuous_rx_cb);
  esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);

  Serial.println("--- RGB Traffic Monitor & Logger Started ---");
  lastLogTime = millis();
}

void loop() {
  unsigned long currentMillis = millis();

  // --- PART 1: HOURLY LOGGING ---
  if (currentMillis - lastLogTime > LOG_INTERVAL_MS) {
    float avgPerSec = hourTotalPackets / (LOG_INTERVAL_MS / 1000.0);
    String logEntry = String(hourCounter) + ", " + 
                      String(hourTotalPackets) + ", " + 
                      String(avgPerSec, 2) + "\n";
    
    Serial.print("SAVING LOG: "); Serial.print(logEntry);
    appendFile("/traffic_log.txt", logEntry);

    hourTotalPackets = 0;
    hourCounter++;
    lastLogTime = currentMillis;
  }

  // --- PART 2: LED LOGIC & SERIAL STATS (Every 1 Sec) ---
  if (currentMillis - lastTrafficCheck > trafficInterval) {
    
    // Capture counters
    int currentPackets = packetCount; 
    packetCount = 0; 
    hourTotalPackets += currentPackets;

    // --- SYSTEM STATS ---
    // RAM
    uint32_t freeHeap = ESP.getFreeHeap();
    uint32_t totalHeap = ESP.getHeapSize();
    float ramPercent = (float)freeHeap / totalHeap * 100;

    // STORAGE
    uint32_t usedBytes = LittleFS.usedBytes();
    uint32_t totalBytes = LittleFS.totalBytes();
    float storagePercent = (float)usedBytes / totalBytes * 100;

    // CPU TEMP
    float cpuTemp = temperatureRead();

    // Print Dashboard
    Serial.println("------------------------------------------------");
    Serial.printf("TRAFFIC: %d pkts/sec | Hour Total: %lu\n", currentPackets, hourTotalPackets);
    Serial.printf("SYSTEM : CPU: %.1f°C | RAM Free: %d B (%.1f%%)\n", cpuTemp, freeHeap, ramPercent);
    Serial.printf("STORAGE: Used: %d / %d B (%.1f%%)\n", usedBytes, totalBytes, storagePercent);
    Serial.println("------------------------------------------------");

    // Determine LED Color
    if (currentPackets == 0) {
      targetR = 0; targetG = 0; targetB = 0;
    } 
    else if (currentPackets < LOW_TRAFFIC) {
      targetR = 0; targetG = 255; targetB = 0;
    } 
    else if (currentPackets < HIGH_TRAFFIC) {
      targetR = 255; targetG = 180; targetB = 0;
    } 
    else {
      targetR = 255; targetG = 0; targetB = 0;
    }

    lastTrafficCheck = currentMillis;
  }

  // --- PART 3: ANIMATION ---
  if (currentMillis - lastFadeUpdate > 10) {
    if (currentR < targetR) currentR += fadeSpeed;
    if (currentR > targetR) currentR -= fadeSpeed;
    if (abs(currentR - targetR) < fadeSpeed) currentR = targetR; 

    if (currentG < targetG) currentG += fadeSpeed;
    if (currentG > targetG) currentG -= fadeSpeed;
    if (abs(currentG - targetG) < fadeSpeed) currentG = targetG;

    if (currentB < targetB) currentB += fadeSpeed;
    if (currentB > targetB) currentB -= fadeSpeed;
    if (abs(currentB - targetB) < fadeSpeed) currentB = targetB;

    pixels.setPixelColor(0, pixels.Color(currentR, currentG, currentB));
    pixels.show();
    
    lastFadeUpdate = currentMillis;
  }
}
