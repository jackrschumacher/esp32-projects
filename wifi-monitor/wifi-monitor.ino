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

// --- LOGGING CONFIG ---
#define LOG_INTERVAL_MS 60000 // Log every 1 Minute to build history faster

// --- OBJECTS ---
Adafruit_NeoPixel pixels(NUMPIXELS, RGB_PIN, NEO_GRB + NEO_KHZ800);

// --- VARIABLES ---
volatile int packetCount = 0;      
unsigned long intervalTotalPackets = 0; 

// --- SMART HISTORY BUFFER (Last 15 Minutes) ---
long history[15]; // Stores the packet count for each of the last 15 minutes
int historyIndex = 0;
bool historyFull = false;

// --- DYNAMIC THRESHOLDS (Self-Learning) ---
// These will change automatically based on your environment!
long dynamicLow = 200;  // Default starting value
long dynamicHigh = 800; // Default starting value

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

// --- SMART CALIBRATION FUNCTION ---
void calibrateThresholds() {
  // If we don't have enough data yet, stick to defaults
  if (historyIndex == 0 && !historyFull) return;

  long minVal = 999999;
  long maxVal = 0;
  int count = historyFull ? 15 : historyIndex;

  // 1. Find the Min and Max in our history buffer
  for (int i = 0; i < count; i++) {
    if (history[i] < minVal) minVal = history[i];
    if (history[i] > maxVal) maxVal = history[i];
  }

  // Avoid divide by zero or ranges that are too tight
  if (maxVal - minVal < 50) {
    maxVal = minVal + 50; 
  }

  // 2. Set Limits based on relative range
  // Low (Green) is the bottom 33% of the range
  // High (Red) is the top 66% of the range
  dynamicLow = minVal + ((maxVal - minVal) * 0.33);
  dynamicHigh = minVal + ((maxVal - minVal) * 0.66);

  Serial.printf(">> RE-CALIBRATING: Min History: %ld | Max History: %ld\n", minVal, maxVal);
  Serial.printf(">> NEW THRESHOLDS: Green < %ld | Red > %ld\n", dynamicLow, dynamicHigh);
}

// --- FILE HELPERS ---
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

  Serial.println("--- Adaptive Traffic Monitor Started ---");
  Serial.println("Learning environment... thresholds will adjust every minute.");
  
  lastLogTime = millis();
}

void loop() {
  unsigned long currentMillis = millis();

  // --- PART 1: LOGGING & LEARNING (Every Minute) ---
  if (currentMillis - lastLogTime > LOG_INTERVAL_MS) {
    // 1. Calculate Per-Second Average for this minute
    // Since LOG_INTERVAL is 60000ms, we divide total by 60
    long avgPerSec = intervalTotalPackets / 60;

    // 2. Store in History Buffer
    history[historyIndex] = avgPerSec;
    historyIndex++;
    if (historyIndex >= 15) {
      historyIndex = 0; // Wrap around (Circular Buffer)
      historyFull = true;
    }

    // 3. Log to file
    String logEntry = String(millis()/1000) + "," + String(avgPerSec) + "\n";
    appendFile("/traffic_log.txt", logEntry);

    // 4. *** MAGIC HAPPENS HERE ***
    // Recalculate what "Loud" and "Quiet" mean based on new data
    calibrateThresholds();

    intervalTotalPackets = 0;
    lastLogTime = currentMillis;
  }

  // --- PART 2: LIVE PLOTTER (Every 1 Sec) ---
  if (currentMillis - lastTrafficCheck > 1000) {
    int currentPackets = packetCount; 
    packetCount = 0; 
    intervalTotalPackets += currentPackets; // Add to the minute-total

    // Stats
    uint32_t freeHeap = ESP.getFreeHeap();
    float cpuTemp = temperatureRead();
    
    Serial.printf("Traffic:%d, Temp:%.1f\n", 
                  currentPackets, cpuTemp);

    // --- ADAPTIVE LED LOGIC ---
    // Uses the DYNAMIC variables instead of hardcoded defines
    if (currentPackets == 0) {
      targetR = 0; targetG = 0; targetB = 0;
    } 
    else if (currentPackets < dynamicLow) {
      targetR = 0; targetG = 255; targetB = 0; // Green (Below 33%)
    } 
    else if (currentPackets < dynamicHigh) {
      targetR = 255; targetG = 180; targetB = 0; // Yellow (Middle 33%)
    } 
    else {
      targetR = 255; targetG = 0; targetB = 0; // Red (Top 33%)
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