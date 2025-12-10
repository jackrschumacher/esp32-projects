#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <Adafruit_NeoPixel.h>
#include <FS.h>
#include <LittleFS.h>

// --- HARDWARE CONFIG ---
#define RGB_PIN 48  
#define BUTTON_PIN 0  
#define NUMPIXELS 1
#define WIFI_CHANNEL 1

// --- TIMING CONFIG ---
#define LOG_INTERVAL_MS 900000       // Save to File every 15 Minutes
#define CALIBRATION_INTERVAL_MS 60000 // Learn Environment every 1 Minute

// --- OBJECTS ---
Adafruit_NeoPixel pixels(NUMPIXELS, RGB_PIN, NEO_GRB + NEO_KHZ800);

// --- VARIABLES ---
volatile int packetCount = 0;      

// Separate accumulators for Learning vs Logging
unsigned long minuteTotalPackets = 0;      
unsigned long fifteenMinuteTotalPackets = 0; 

// --- SMART HISTORY BUFFER (Last 15 Minutes) ---
long history[15];
int historyIndex = 0;
bool historyFull = false;

// --- DYNAMIC THRESHOLDS (Self-Learning) ---
long dynamicLow = 200;  
long dynamicHigh = 800; 

// --- FADE VARS ---
int currentR = 0, currentG = 0, currentB = 0;
int targetR = 0, targetG = 0, targetB = 0;
int fadeSpeed = 5; 

// --- TIMERS ---
unsigned long lastTrafficCheck = 0;
unsigned long lastFadeUpdate = 0;
unsigned long lastCalibrationTime = 0; // Timer 1 (Learning)
unsigned long lastLogTime = 0;         // Timer 2 (Logging)

// Sniffer Callback
void promiscuous_rx_cb(void* buf, wifi_promiscuous_pkt_type_t type) {
  packetCount++;
}

// --- SMART CALIBRATION FUNCTION ---
void calibrateThresholds() {
  if (historyIndex == 0 && !historyFull) return;

  long minVal = 999999;
  long maxVal = 0;
  int count = historyFull ? 15 : historyIndex;

  for (int i = 0; i < count; i++) {
    if (history[i] < minVal) minVal = history[i];
    if (history[i] > maxVal) maxVal = history[i];
  }

  if (maxVal - minVal < 50) maxVal = minVal + 50; 

  dynamicLow = minVal + ((maxVal - minVal) * 0.33);
  dynamicHigh = minVal + ((maxVal - minVal) * 0.66);
}

// --- REPORT GENERATOR ---
void generateReport() {
  if (!LittleFS.exists("/traffic_log.txt")) {
    Serial.println("No logs found yet.");
    return;
  }

  File file = LittleFS.open("/traffic_log.txt", "r");
  if (!file) return;

  // PASS 1: Find Max for scaling
  long maxPackets = 1; 
  while (file.available()) {
    String line = file.readStringUntil('\n');
    if (line.length() < 2) continue;
    int comma = line.indexOf(',');
    if (comma > 0) {
      String valStr = line.substring(comma + 1);
      long val = valStr.toInt();
      if (val > maxPackets) maxPackets = val;
    }
  }
  file.close(); 

  // PASS 2: Print Graph
  file = LittleFS.open("/traffic_log.txt", "r");
  Serial.println("\n\n===== TRAFFIC HISTORY GRAPH (15m Intervals) =====");
  Serial.printf("Scale: 100%% = %ld packets/sec (avg)\n", maxPackets);
  Serial.println("-------------------------------------------------");

  int lineCount = 1;
  while (file.available()) {
    String line = file.readStringUntil('\n');
    if (line.length() < 2) continue;
    
    int comma = line.indexOf(',');
    if (comma > 0) {
      String valStr = line.substring(comma + 1);
      long val = valStr.toInt();
      int barLength = (int)((val * 50) / maxPackets);
      
      Serial.printf("%03d |", lineCount++);
      for (int i = 0; i < barLength; i++) Serial.print("#");
      Serial.printf(" (%ld)\n", val);
    }
  }
  Serial.println("=================================================\n");
  file.close();
}

void appendFile(const char * path, String message){
  File file = LittleFS.open(path, FILE_APPEND);
  if(!file) return;
  file.print(message);
  file.close();
}

void setup() {
  Serial.begin(115200);
  delay(1000); 

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  if(!LittleFS.begin(true)){
    Serial.println("LittleFS Mount Failed");
    return;
  }

  pixels.begin();
  pixels.setBrightness(20);
  pixels.show();

  // Initialize History Buffer with 0
  for(int i=0; i<15; i++) history[i] = 0;

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_promiscuous_rx_cb(promiscuous_rx_cb);
  esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);

  Serial.println("--- Wifi Packet monitor ---");
  Serial.println("Commands: 'graph', 'thresholds', 'system'");
  
  lastLogTime = millis();
  lastCalibrationTime = millis();
}

void loop() {
  unsigned long currentMillis = millis();

  // --- 1. SERIAL COMMAND LISTENER ---
  if (Serial.available()) {
    String command = Serial.readStringUntil('\n');
    command.trim(); 
    
    if (command.equalsIgnoreCase("graph")) {
      pixels.setPixelColor(0, pixels.Color(0, 0, 255)); 
      pixels.show();
      generateReport();
    }
    else if (command.equalsIgnoreCase("thresholds")) {
      Serial.printf("Low < %ld | High > %ld\n", dynamicLow, dynamicHigh );
    }
    else if (command.equalsIgnoreCase("system")) {
      uint32_t freeHeap = ESP.getFreeHeap();
      float cpuTemp = temperatureRead();
      uint32_t usedBytes = LittleFS.usedBytes();
      uint32_t totalBytes = LittleFS.totalBytes();
      float storagePercent = (float)usedBytes / totalBytes * 100;
      Serial.printf("Temp:%.1f C, FreeRAM:%.1f KB, Storage:%.1f%%\n", 
                    cpuTemp, freeHeap/ 1024.0, storagePercent);
    }
  }

  // --- 2. BUTTON LISTENER ---
  if (digitalRead(BUTTON_PIN) == LOW) {
    delay(50); 
    if (digitalRead(BUTTON_PIN) == LOW) {
      pixels.setPixelColor(0, pixels.Color(0, 0, 255)); 
      pixels.show();
      generateReport();
      while(digitalRead(BUTTON_PIN) == LOW) delay(10);
    }
  }

  // --- 3. FAST TIMER: CALIBRATION (Every 1 Minute) ---
  if (currentMillis - lastCalibrationTime > CALIBRATION_INTERVAL_MS) {
    long minuteAvg = minuteTotalPackets / 60; // Avg packets per second this minute

    // Update History
    history[historyIndex] = minuteAvg;
    historyIndex++;
    if (historyIndex >= 15) {
      historyIndex = 0; 
      historyFull = true;
    }

    // Recalculate Thresholds based on recent history
    calibrateThresholds();

    minuteTotalPackets = 0;
    lastCalibrationTime = currentMillis;
  }

  // --- 4. SLOW TIMER: LOGGING (Every 15 Minutes) ---
  if (currentMillis - lastLogTime > LOG_INTERVAL_MS) {
    long fifteenMinAvg = fifteenMinuteTotalPackets / (LOG_INTERVAL_MS / 1000);

    String logEntry = String(millis()/1000) + "," + String(fifteenMinAvg) + "\n";
    appendFile("/traffic_log.txt", logEntry);

    fifteenMinuteTotalPackets = 0;
    lastLogTime = currentMillis;
  }

  // --- 5. LIVE LOOP (Every 1 Sec) ---
  if (currentMillis - lastTrafficCheck > 1000) {
    int currentPackets = packetCount; 
    packetCount = 0; 
    
    // Add to BOTH accumulators
    minuteTotalPackets += currentPackets;
    fifteenMinuteTotalPackets += currentPackets;

    // Print with Newline for clean output
    Serial.printf("Traffic:%d\n", currentPackets);

    // Adaptive LED Logic using Dynamic Thresholds
    if (currentPackets == 0) {
      targetR = 0; targetG = 0; targetB = 0;
    } 
    else if (currentPackets < dynamicLow) {
      targetR = 0; targetG = 255; targetB = 0; 
    } 
    else if (currentPackets < dynamicHigh) {
      targetR = 255; targetG = 180; targetB = 0; 
    } 
    else {
      targetR = 255; targetG = 0; targetB = 0; 
    }

    lastTrafficCheck = currentMillis;
  }

  // --- 6. ANIMATION ---
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