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

// --- LOGGING CONFIG (15 MINUTES) ---
#define LOG_INTERVAL_MS 900000 
// #define LOG_INTERVAL_MS 60000 // Uncomment for testing (1 min)

// --- TRAFFIC LIMITS ---
#define LOW_TRAFFIC 400
#define HIGH_TRAFFIC 800

// --- OBJECTS ---
Adafruit_NeoPixel pixels(NUMPIXELS, RGB_PIN, NEO_GRB + NEO_KHZ800);

// --- VARIABLES ---
volatile int packetCount = 0;      
unsigned long intervalTotalPackets = 0; 
int intervalCounter = 1;                  

// --- FADE VARS ---
int currentR = 0, currentG = 0, currentB = 0;
int targetR = 0, targetG = 0, targetB = 0;
int fadeSpeed = 5; 

// --- TIMERS ---
unsigned long lastTrafficCheck = 0;
unsigned long lastFadeUpdate = 0;
unsigned long lastLogTime = 0;

// Sniffer Callback
void promiscuous_rx_cb(void* buf, wifi_promiscuous_pkt_type_t type) {
  packetCount++;
}

// --- REPORT GENERATOR ---
void generateReport() {
  if (!LittleFS.exists("/traffic_log.txt")) return;

  File file = LittleFS.open("/traffic_log.txt", "r");
  if (!file) return;

  long maxPackets = 1; 
  while (file.available()) {
    String line = file.readStringUntil('\n');
    if (line.length() < 2) continue;
    int firstComma = line.indexOf(',');
    int secondComma = line.indexOf(',', firstComma + 1);
    if (firstComma > 0 && secondComma > 0) {
      String totalStr = line.substring(firstComma + 1, secondComma);
      long total = totalStr.toInt();
      if (total > maxPackets) maxPackets = total;
    }
  }
  file.close(); 

  file = LittleFS.open("/traffic_log.txt", "r");
  // Note: These print statements will look like "noise" on the Plotter
  // but are readable in the Serial Monitor.
  Serial.println("\n\n===== 15-MINUTE INTERVAL REPORT =====");
  Serial.printf("Scale: 100%% = %ld packets\n", maxPackets);
  Serial.println("-------------------------------------");

  while (file.available()) {
    String line = file.readStringUntil('\n');
    if (line.length() < 2) continue;
    int firstComma = line.indexOf(',');
    int secondComma = line.indexOf(',', firstComma + 1);
    if (firstComma > 0) {
      String idStr = line.substring(0, firstComma);
      String totalStr = line.substring(firstComma + 1, secondComma);
      long total = totalStr.toInt();
      int barLength = (int)((total * 50) / maxPackets);
      Serial.printf("Int %-3s |", idStr.c_str());
      for (int i = 0; i < barLength; i++) Serial.print("#");
      Serial.printf(" (%ld)\n", total);
    }
  }
  Serial.println("=====================================\n");
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

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_promiscuous_rx_cb(promiscuous_rx_cb);
  esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);

  // Short delay before starting to avoid plotting startup text
  delay(500);
  
  lastLogTime = millis();
}

void loop() {
  unsigned long currentMillis = millis();

  // --- BUTTON CHECK ---
  if (digitalRead(BUTTON_PIN) == LOW) {
    delay(50); 
    if (digitalRead(BUTTON_PIN) == LOW) {
      pixels.setPixelColor(0, pixels.Color(0, 0, 255));
      pixels.show();
      generateReport();
      while(digitalRead(BUTTON_PIN) == LOW) delay(10);
      lastFadeUpdate = 0; 
    }
  }

  // --- PART 1: LOGGING ---
  if (currentMillis - lastLogTime > LOG_INTERVAL_MS) {
    float avgPerSec = intervalTotalPackets / (LOG_INTERVAL_MS / 1000.0);
    String logEntry = String(intervalCounter) + ", " + 
                      String(intervalTotalPackets) + ", " + 
                      String(avgPerSec, 2) + "\n";
    
    // Minimal print to avoid messing up graph too much
    // Serial.print("SAVING"); 
    appendFile("/traffic_log.txt", logEntry);
    
    // Auto-generate report (will create a gap in the graph)
    generateReport(); 
    
    intervalTotalPackets = 0;
    intervalCounter++;
    lastLogTime = currentMillis;
  }

  // --- PART 2: LIVE PLOTTER OUTPUT ---
  if (currentMillis - lastTrafficCheck > 1000) {
    int currentPackets = packetCount; 
    packetCount = 0; 
    intervalTotalPackets += currentPackets;

    // --- SYSTEM STATS ---
    uint32_t freeHeap = ESP.getFreeHeap();
    float cpuTemp = temperatureRead();
    
    // --- PLOTTER FORMAT ---
    // Syntax: Label:Value, Label:Value
    // FreeRAM is divided by 1024 to convert Bytes to KB (Scaling for graph)
    Serial.printf("Traffic:%d, Temp:%.1f, FreeRAM_KB:%.1f\n", 
                  currentPackets, cpuTemp, freeHeap / 1024.0);

    // Determine Color
    if (currentPackets == 0) {
      targetR = 0; targetG = 0; targetB = 0;
    } else if (currentPackets < LOW_TRAFFIC) {
      targetR = 0; targetG = 255; targetB = 0; 
    } else if (currentPackets < HIGH_TRAFFIC) {
      targetR = 255; targetG = 180; targetB = 0; 
    } else {
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