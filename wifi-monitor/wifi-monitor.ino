#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <Adafruit_NeoPixel.h>
#include <FS.h>
#include <LittleFS.h>
#include <WebServer.h> // NEW: Required for the Web Interface

// --- HARDWARE CONFIG ---
#define RGB_PIN 48  
#define BUTTON_PIN 0  
#define NUMPIXELS 1
#define WIFI_CHANNEL 1

// --- TIMING CONFIG ---
#define LOG_INTERVAL_MS 900000       // Save to File every 15 Minutes
#define CALIBRATION_INTERVAL_MS 60000 // Learn Environment every 1 Minute

// --- AP CONFIG (NEW) ---
const char* ap_ssid = "jsESP32-Wifi";
const char* ap_pass = "StarshipFalcon9#";

// --- OBJECTS ---
Adafruit_NeoPixel pixels(NUMPIXELS, RGB_PIN, NEO_GRB + NEO_KHZ800);
WebServer server(80); // NEW: Web Server on port 80

// --- VARIABLES ---
volatile int packetCount = 0;       
bool inAPMode = false; // NEW: Tracks system state

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
unsigned long lastCalibrationTime = 0; 
unsigned long lastLogTime = 0;         

// Sniffer Callback
void promiscuous_rx_cb(void* buf, wifi_promiscuous_pkt_type_t type) {
  packetCount++;
}

// --- WEB HANDLERS (NEW) ---
void handleRoot() {
  File file = LittleFS.open("/traffic_log.txt", "r");
  String html = "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>body{font-family:sans-serif; text-align:center;} table{margin:auto; border-collapse: collapse;} td, th {border: 1px solid #ddd; padding: 8px;}</style>";
  html += "</head><body><h1>Traffic Logs</h1>";
  
  if (!file || !file.available()) {
    html += "<p>No logs found.</p>";
  } else {
    html += "<table><tr><th>Time (sec)</th><th>Avg Packets</th></tr>";
    while (file.available()) {
      String line = file.readStringUntil('\n');
      if (line.length() > 0) {
        int comma = line.indexOf(',');
        if (comma > 0) {
           html += "<tr><td>" + line.substring(0, comma) + "</td><td>" + line.substring(comma+1) + "</td></tr>";
        }
      }
    }
    html += "</table>";
    file.close();
  }
  
  html += "<br><br><form action='/clear' method='get'><button style='background:red;color:white;padding:10px;'>CLEAR LOGS</button></form>";
  html += "</body></html>";
  server.send(200, "text/html", html);
}

void handleClear() {
  LittleFS.remove("/traffic_log.txt");
  server.send(200, "text/html", "Logs cleared! <a href='/'>Go Back</a>");
}

// --- HELPER FUNCTIONS ---
void appendFile(const char * path, String message){
  File file = LittleFS.open(path, FILE_APPEND);
  if(!file) return;
  file.print(message);
  file.close();
}

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

// --- STATE MANAGEMENT (NEW) ---
void enableAPMode() {
  Serial.println("Switching to AP Mode...");
  
  // 1. Stop Sniffer
  esp_wifi_set_promiscuous(false);
  
  // 2. Start AP
  WiFi.mode(WIFI_AP);
  WiFi.softAP(ap_ssid, ap_pass);
  Serial.print("AP IP Address: ");
  Serial.println(WiFi.softAPIP());

  // 3. Start Server
  server.on("/", handleRoot);
  server.on("/clear", handleClear);
  server.begin();
  
  // 4. Visual Indicator (Cyan)
  pixels.setPixelColor(0, pixels.Color(0, 255, 255));
  pixels.show();
  
  inAPMode = true;
}

void disableAPMode() {
  Serial.println("Switching back to Sniffer Mode...");
  
  // 1. Stop Server & AP
  server.stop();
  WiFi.softAPdisconnect(true);
  
  // 2. Restart Sniffer
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_promiscuous_rx_cb(promiscuous_rx_cb);
  esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
  
  inAPMode = false;
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

  // Initialize History Buffer
  for(int i=0; i<15; i++) history[i] = 0;

  // Start in Sniffer Mode
  disableAPMode(); 
  
  lastLogTime = millis();
  lastCalibrationTime = millis();
}

void loop() {
  unsigned long currentMillis = millis();

  // --- BUTTON HANDLING (Toggle Mode) ---
  // Simple debounce logic
  if (digitalRead(BUTTON_PIN) == LOW) {
    delay(50); 
    if (digitalRead(BUTTON_PIN) == LOW) {
      if (inAPMode) {
        disableAPMode();
      } else {
        enableAPMode();
      }
      // Wait for release to prevent cycling
      while(digitalRead(BUTTON_PIN) == LOW) delay(10);
    }
  }

  // --- MODE 1: ACCESS POINT (VIEW DATA) ---
  if (inAPMode) {
    server.handleClient();
    // In AP mode, we skip all sniffing and logging logic
    // Just pulse the LED gently to show we are alive
    int b = (millis() / 10) % 255; 
    if (b>127) b = 255 - b; // Triangle wave for breathing effect
    pixels.setPixelColor(0, pixels.Color(0, b*2, b*2)); // Cyan Breathe
    pixels.show();
    return; 
  }

  // --- MODE 2: SNIFFER (COLLECT DATA) ---

  // ... (Standard Sniffer Logic) ...
  
  // 1. FAST TIMER: CALIBRATION (Every 1 Minute)
  if (currentMillis - lastCalibrationTime > CALIBRATION_INTERVAL_MS) {
    long minuteAvg = minuteTotalPackets / 60; 
    history[historyIndex] = minuteAvg;
    historyIndex++;
    if (historyIndex >= 15) {
      historyIndex = 0; 
      historyFull = true;
    }
    calibrateThresholds();
    minuteTotalPackets = 0;
    lastCalibrationTime = currentMillis;
  }

  // 2. SLOW TIMER: LOGGING (Every 15 Minutes)
  if (currentMillis - lastLogTime > LOG_INTERVAL_MS) {
    long fifteenMinAvg = fifteenMinuteTotalPackets / (LOG_INTERVAL_MS / 1000);
    String logEntry = String(millis()/1000) + "," + String(fifteenMinAvg) + "\n";
    appendFile("/traffic_log.txt", logEntry);
    fifteenMinuteTotalPackets = 0;
    lastLogTime = currentMillis;
  }

  // 3. LIVE LOOP (Every 1 Sec)
  if (currentMillis - lastTrafficCheck > 1000) {
    int currentPackets = packetCount; 
    packetCount = 0; 
    
    minuteTotalPackets += currentPackets;
    fifteenMinuteTotalPackets += currentPackets;

    Serial.printf("Traffic:%d pckts/sec \n", currentPackets);

    // Adaptive LED Logic
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

  // 4. ANIMATION
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